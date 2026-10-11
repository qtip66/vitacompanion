/*
 * Screen server (SCREEN_PORT, default 1339): the live framebuffer for remote
 * control, so a client sees the screen without taking screenshots.
 *
 * One client at a time, one request line per reply:
 *   GRAB x y w h shift   -> u32 length, then VcFrameHeader + RGB565 pixels
 *                           (w/h 0 = to the edge, shift 0..3 = 1/1..1/8 size)
 *   DELTA shift [y h]    -> u32 length, then VcFrameHeader, a bitmap of
 *                           changed rows ((out_h + 7) / 8 bytes, LSB first)
 *                           and those rows' pixels, top to bottom. Full width;
 *                           y/h pick a band of source rows (h 0 = to the bottom)
 *                           so a full-size frame fits the buffer in pieces.
 *                           Rows are compared with the last DELTA of the same
 *                           shift on this connection; the first sends every row.
 *   INFO                 -> one text line: api, buffer size, proto=2
 * An error reply is u32 0xFFFFFFFF followed by the i32 result code.
 * The buffer is allocated per connection, so SceShell holds no memory
 * while nobody is watching.
 */
#include "screen.h"

#include <vitacompanion_kernel.h>
#include <stdio.h>
#include <string.h>
#include <vitasdk.h>

#ifndef SCREEN_PORT
#define SCREEN_PORT 1339
#endif
#define SCREEN_MAX_ROWS 544
#define SCREEN_IO_TIMEOUT_US (30 * 1000 * 1000)

extern volatile int run;

static SceUID screen_thid = -1;
static int screen_sockfd = -1;
static volatile int screen_client_sockfd = -1;
static uint32_t row_hash[4][SCREEN_MAX_ROWS];   /* per shift, so a half-size stream and full-size shots can mix */

static int send_all(int s, const void *data, unsigned int len)
{
    const uint8_t *p = data;
    while (len > 0)
    {
        int n = sceNetSend(s, p, len, 0);
        if (n <= 0)
            return -1;
        p += n;
        len -= (unsigned int)n;
    }
    return 0;
}

static int send_error(int s, int code)
{
    uint32_t msg[2] = {0xFFFFFFFFu, (uint32_t)code};
    return send_all(s, msg, sizeof(msg));
}

static int recv_line(int s, char *buf, unsigned int cap)
{
    unsigned int used = 0;
    while (used + 1 < cap)
    {
        int n = sceNetRecv(s, buf + used, 1, 0);
        if (n <= 0)
            return -1;
        if (buf[used] == '\n')
            break;
        used++;
    }
    buf[used] = '\0';
    return (int)used;
}

static uint32_t fnv1a(const uint8_t *p, unsigned int len)
{
    uint32_t h = 2166136261u;
    while (len--)
        h = (h ^ *p++) * 16777619u;
    return h;
}

static int parse_uints(const char *s, unsigned int *out, int max)
{
    int count = 0;
    while (*s && count < max)
    {
        while (*s == ' ')
            s++;
        if (*s < '0' || *s > '9')
            break;
        out[count] = 0;
        while (*s >= '0' && *s <= '9')
            out[count] = out[count] * 10 + (unsigned int)(*s++ - '0');
        count++;
    }
    return count;
}

static int handle_grab(int s, uint8_t *buf, unsigned int cap, const char *args)
{
    unsigned int v[5] = {0};
    int n;
    uint32_t len;

    if (parse_uints(args, v, 5) != 5)
        return send_error(s, -100);
    n = vitaCompanionKernelGrabFrame(buf + 4, cap - 4, v[0] << 16 | (v[1] & 0xffff),
        (v[2] & 0xfff) << 20 | (v[3] & 0xfff) << 8 | (v[4] & 0xff));
    if (n < 0)
        return send_error(s, n);
    len = (uint32_t)n;
    memcpy(buf, &len, 4);
    return send_all(s, buf, len + 4);
}

static int handle_delta(int s, uint8_t *buf, unsigned int cap, const char *args)
{
    unsigned int v[3] = {1, 0, 0};   /* shift, first source row, source rows (0 = to the bottom) */
    unsigned int shift, first;
    VcFrameHeader hdr;
    uint8_t head[4 + sizeof(VcFrameHeader)];
    uint8_t bits[(SCREEN_MAX_ROWS + 7) / 8];
    uint8_t *pixels;
    unsigned int row, row_bytes, bitmap_bytes, changed = 0;
    int n;
    uint32_t len;

    parse_uints(args, v, 3);
    shift = v[0];
    if (shift > 3 || v[1] > 0xfff || v[2] > 0xfff)
        return send_error(s, -100);
    n = vitaCompanionKernelGrabFrame(buf, cap, v[1] & 0xffff, (v[2] << 8) | shift);
    if (n < 0)
        return send_error(s, n);
    memcpy(&hdr, buf, sizeof(hdr));
    first = (unsigned int)hdr.y >> shift;
    if (first + hdr.out_h > SCREEN_MAX_ROWS)
        return send_error(s, -101);

    /* Changed rows are packed towards the start (destination never after the
       source, so increasing order is safe), then sent after the bitmap. */
    row_bytes = hdr.out_w * 2u;
    bitmap_bytes = (hdr.out_h + 7u) / 8u;
    pixels = buf + sizeof(hdr);
    memset(bits, 0, sizeof(bits));
    for (row = 0; row < hdr.out_h; ++row)
    {
        uint8_t *src = pixels + row * row_bytes;
        uint32_t h = fnv1a(src, row_bytes) | 1u;  /* never 0, the "unknown" value */
        if (h != row_hash[shift][first + row])
        {
            row_hash[shift][first + row] = h;
            bits[row >> 3] |= (uint8_t)(1u << (row & 7));
            if (changed != row)
                memmove(pixels + changed * row_bytes, src, row_bytes);
            changed++;
        }
    }

    len = (uint32_t)(sizeof(hdr) + bitmap_bytes + changed * row_bytes);
    memcpy(head, &len, 4);
    memcpy(head + 4, &hdr, sizeof(hdr));
    if (send_all(s, head, sizeof(head)) < 0 || send_all(s, bits, bitmap_bytes) < 0)
        return -1;
    return changed ? send_all(s, pixels, changed * row_bytes) : 0;
}

static void serve_client(int s)
{
    SceUID block = -1;
    uint8_t *buf = NULL;
    unsigned int cap = 0;
    static const unsigned int sizes[] = {0x100000, 0x80000, 0x40000};
    unsigned int i;
    char line[96];

    for (i = 0; i < sizeof(sizes) / sizeof(sizes[0]) && block < 0; ++i)
    {
        block = sceKernelAllocMemBlock("vitacompanion_screen", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, sizes[i], NULL);
        cap = sizes[i];
    }
    if (block < 0 || sceKernelGetMemBlockBase(block, (void **)&buf) < 0)
    {
        send_error(s, block < 0 ? block : -103);
        if (block >= 0)
            sceKernelFreeMemBlock(block);
        return;
    }
    memset(row_hash, 0, sizeof(row_hash));

    while (run && recv_line(s, line, sizeof(line)) >= 0)
    {
        int r;
        if (strncmp(line, "GRAB", 4) == 0)
            r = handle_grab(s, buf, cap, line + 4);
        else if (strncmp(line, "DELTA", 5) == 0)
            r = handle_delta(s, buf, cap, line + 5);
        else if (strncmp(line, "INFO", 4) == 0)
        {
            char msg[64];
            snprintf(msg, sizeof(msg), "vitacompanion screen api=%d buf=%u proto=2\n",
                vitaCompanionKernelGetApiVersion(), cap);
            r = send_all(s, msg, strlen(msg));
        }
        else
            r = send_error(s, -104);
        if (r < 0)
            break;
    }
    sceKernelFreeMemBlock(block);
}

static int screen_thread(unsigned int args, void *argp)
{
    SceNetSockaddrIn addr;
    int timeout = SCREEN_IO_TIMEOUT_US;
    int nodelay = 1;

    (void)args;
    (void)argp;

    screen_sockfd = sceNetSocket("vitacompanion_screen_sock", SCE_NET_AF_INET, SCE_NET_SOCK_STREAM, 0);
    if (screen_sockfd < 0)
        return screen_sockfd;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = SCE_NET_AF_INET;
    addr.sin_addr.s_addr = sceNetHtonl(SCE_NET_INADDR_ANY);
    addr.sin_port = sceNetHtons(SCREEN_PORT);
    if (sceNetBind(screen_sockfd, (SceNetSockaddr *)&addr, sizeof(addr)) < 0 ||
        sceNetListen(screen_sockfd, 1) < 0)
    {
        sceNetSocketClose(screen_sockfd);
        screen_sockfd = -1;
        return -1;
    }

    while (run)
    {
        SceNetSockaddrIn client;
        unsigned int client_len = sizeof(client);
        int c = sceNetAccept(screen_sockfd, (SceNetSockaddr *)&client, &client_len);
        if (c < 0)
        {
            if (!run || screen_sockfd < 0)
                break;
            sceKernelDelayThread(100 * 1000);
            continue;
        }
        sceNetSetsockopt(c, SCE_NET_SOL_SOCKET, SCE_NET_SO_RCVTIMEO, &timeout, sizeof(timeout));
        sceNetSetsockopt(c, SCE_NET_SOL_SOCKET, SCE_NET_SO_SNDTIMEO, &timeout, sizeof(timeout));
        sceNetSetsockopt(c, SCE_NET_IPPROTO_TCP, SCE_NET_TCP_NODELAY, &nodelay, sizeof(nodelay));
        screen_client_sockfd = c;
        serve_client(c);
        screen_client_sockfd = -1;
        sceNetSocketClose(c);
    }
    return 0;
}

int screen_start(void)
{
    screen_thid = sceKernelCreateThread("vitacompanion_screen_thread", screen_thread, 0x40, 0x4000, 0, 0, NULL);
    if (screen_thid < 0)
        return screen_thid;
    return sceKernelStartThread(screen_thid, 0, NULL);
}

void screen_end(void)
{
    int s = screen_sockfd;
    int c = screen_client_sockfd;

    screen_sockfd = -1;
    if (c >= 0)
        sceNetSocketAbort(c, 0);
    if (s >= 0)
    {
        sceNetSocketAbort(s, 0);
        sceNetSocketClose(s);
    }
    if (screen_thid >= 0)
    {
        sceKernelWaitThreadEnd(screen_thid, NULL, NULL);
        sceKernelDeleteThread(screen_thid);
        screen_thid = -1;
    }
}
