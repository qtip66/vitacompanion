#include <vitacompanion_input.h>
#include <vitacompanion_kernel.h>

#include <psp2/touch.h>
#include <psp2kern/ctrl.h>
#include <psp2kern/display.h>
#include <psp2kern/kernel/cpu.h>
#include <psp2kern/kernel/sysmem.h>
#include <psp2kern/kernel/modulemgr.h>
#include <psp2kern/kernel/threadmgr.h>
#include <psp2kern/kernel/suspend.h>
#include <psp2kern/kernel/sysmem/data_transfers.h>
#include <stdint.h>
#include <string.h>
#include <taihen.h>

#define CTRL_REFRESH_US (8 * 1000)
#define CTRL_EMULATION_SAMPLES 32
#define FRONT_TOUCH_REPORTS 6
#define REAR_TOUCH_REPORTS 4
#define SYNTHETIC_TOUCH_ID_BASE 0x01
#define USER_BUTTON_MASK 0x0000FFFF

typedef struct {
    int active;
    uint8_t x;
    uint8_t y;
} analog_state;

typedef struct {
    int active;
    uint16_t x;
    uint16_t y;
} touch_state;

static SceUID state_mutex = -1;
static SceUID ctrl_thread_id = -1;
static volatile int ctrl_thread_running;
static uint32_t button_state;
static analog_state analog_states[2];
static touch_state touch_states[2][VITACOMPANION_TOUCH_SLOTS];

static SceUID touch_hook_ids[4] = {-1, -1, -1, -1};
static tai_hook_ref_t touch_peek_ref;
static tai_hook_ref_t touch_peek_region_ref;
static tai_hook_ref_t touch_read_ref;
static tai_hook_ref_t touch_read_region_ref;

/*
 * Common dialogs (e.g. the PSN sign-up prompt inside a game) read touch
 * through the user-only sceTouchPeekRegionExt/sceTouchReadRegionExt syscalls,
 * which do not go through the ksceTouch* exports hooked above.
 */
#define SCE_TOUCH_USER_LIBRARY_NID 0x3E4F4A81
static SceUID touch_ext_hook_ids[2] = {-1, -1};
static tai_hook_ref_t touch_peek_region_ext_ref;
static tai_hook_ref_t touch_read_region_ext_ref;

static void state_lock(void)
{
    if (state_mutex >= 0)
        ksceKernelLockMutex(state_mutex, 1, NULL);
}

static void state_unlock(void)
{
    if (state_mutex >= 0)
        ksceKernelUnlockMutex(state_mutex, 1);
}

static void clear_state(void)
{
    int port;
    int slot;

    button_state = 0;
    for (port = 0; port < 2; ++port)
    {
        analog_states[port].active = 0;
        analog_states[port].x = VITACOMPANION_ANALOG_CENTER;
        analog_states[port].y = VITACOMPANION_ANALOG_CENTER;
    }

    for (port = 0; port < 2; ++port)
    {
        for (slot = 0; slot < VITACOMPANION_TOUCH_SLOTS; ++slot)
        {
            touch_states[port][slot].active = 0;
            touch_states[port][slot].x = 0;
            touch_states[port][slot].y = 0;
        }
    }
}

static void reset_ctrl_emulation(void)
{
    ksceCtrlSetButtonEmulation(0, 0, 0, 0, CTRL_EMULATION_SAMPLES);
    ksceCtrlSetAnalogEmulation(0, 0,
        VITACOMPANION_ANALOG_CENTER, VITACOMPANION_ANALOG_CENTER,
        VITACOMPANION_ANALOG_CENTER, VITACOMPANION_ANALOG_CENTER,
        VITACOMPANION_ANALOG_CENTER, VITACOMPANION_ANALOG_CENTER,
        VITACOMPANION_ANALOG_CENTER, VITACOMPANION_ANALOG_CENTER, 0);
}

static int ctrl_thread(unsigned int args, void *argp)
{
    uint32_t last_buttons = 0;
    int analog_was_active = 0;

    (void)args;
    (void)argp;

    while (ctrl_thread_running)
    {
        uint32_t buttons;
        analog_state left;
        analog_state right;
        int analog_active;

        state_lock();
        buttons = button_state;
        left = analog_states[VITACOMPANION_STICK_LEFT];
        right = analog_states[VITACOMPANION_STICK_RIGHT];
        state_unlock();

        if (buttons != 0 || last_buttons != 0)
        {
            ksceCtrlSetButtonEmulation(
                0, 0, buttons & USER_BUTTON_MASK, buttons,
                CTRL_EMULATION_SAMPLES);
        }
        last_buttons = buttons;

        analog_active = left.active || right.active;
        if (analog_active)
        {
            uint8_t lx = left.active ? left.x : VITACOMPANION_ANALOG_CENTER;
            uint8_t ly = left.active ? left.y : VITACOMPANION_ANALOG_CENTER;
            uint8_t rx = right.active ? right.x : VITACOMPANION_ANALOG_CENTER;
            uint8_t ry = right.active ? right.y : VITACOMPANION_ANALOG_CENTER;

            ksceCtrlSetAnalogEmulation(
                0, 0, lx, ly, rx, ry, lx, ly, rx, ry,
                CTRL_EMULATION_SAMPLES);
        }
        else if (analog_was_active)
        {
            ksceCtrlSetAnalogEmulation(0, 0,
                VITACOMPANION_ANALOG_CENTER,
                VITACOMPANION_ANALOG_CENTER,
                VITACOMPANION_ANALOG_CENTER,
                VITACOMPANION_ANALOG_CENTER,
                VITACOMPANION_ANALOG_CENTER,
                VITACOMPANION_ANALOG_CENTER,
                VITACOMPANION_ANALOG_CENTER,
                VITACOMPANION_ANALOG_CENTER, 0);
        }
        analog_was_active = analog_active;

        ksceKernelDelayThread(CTRL_REFRESH_US);
    }

    reset_ctrl_emulation();
    ksceKernelExitDeleteThread(0);
    return 0;
}

static void patch_touch_data(unsigned int port, SceTouchData *data,
    unsigned int count)
{
    touch_state points[VITACOMPANION_TOUCH_SLOTS];
    unsigned int maximum_reports;
    unsigned int buffer_index;
    int slot;
    int injected = 0;

    if (port > VITACOMPANION_TOUCH_REAR || !data)
        return;

    state_lock();
    for (slot = 0; slot < VITACOMPANION_TOUCH_SLOTS; ++slot)
        points[slot] = touch_states[port][slot];
    state_unlock();

    maximum_reports = port == VITACOMPANION_TOUCH_FRONT
        ? FRONT_TOUCH_REPORTS : REAR_TOUCH_REPORTS;

    for (buffer_index = 0; buffer_index < count; ++buffer_index)
    {
        SceTouchData *current = &data[buffer_index];

        for (slot = 0; slot < VITACOMPANION_TOUCH_SLOTS; ++slot)
        {
            SceTouchReport *report;

            if (!points[slot].active ||
                current->reportNum >= maximum_reports)
                continue;

            report = &current->report[current->reportNum++];
            report->id = (uint8_t)(SYNTHETIC_TOUCH_ID_BASE + slot);
            report->force = 0;
            report->x = (int16_t)points[slot].x;
            report->y = (int16_t)points[slot].y;
            report->reserved[0] = 0;
            report->reserved[1] = 0;
            report->reserved[2] = 0;
            report->reserved[3] = 0;
            report->reserved[4] = 0;
            report->reserved[5] = 0;
            report->reserved[6] = 0;
            report->reserved[7] = 0;
            report->info = 0;
            injected = 1;
        }
    }

    /* Real panel input resets the idle timer; SceShell ignores touches otherwise. */
    if (injected)
        ksceKernelPowerTick(SCE_KERNEL_POWER_TICK_DEFAULT);
}

static int touch_peek_hook(unsigned int port, SceTouchData *data,
    unsigned int count)
{
    int result = TAI_CONTINUE(int, touch_peek_ref, port, data, count);

    if (result > 0)
        patch_touch_data(port, data, (unsigned int)result);
    return result;
}

static int touch_peek_region_hook(unsigned int port, SceTouchData *data,
    unsigned int count, int region)
{
    int result = TAI_CONTINUE(int, touch_peek_region_ref,
        port, data, count, region);

    if (result > 0)
        patch_touch_data(port, data, (unsigned int)result);
    return result;
}

static int touch_read_hook(unsigned int port, SceTouchData *data,
    unsigned int count)
{
    int result = TAI_CONTINUE(int, touch_read_ref, port, data, count);

    if (result > 0)
        patch_touch_data(port, data, (unsigned int)result);
    return result;
}

static int touch_read_region_hook(unsigned int port, SceTouchData *data,
    unsigned int count, int region)
{
    int result = TAI_CONTINUE(int, touch_read_region_ref,
        port, data, count, region);

    if (result > 0)
        patch_touch_data(port, data, (unsigned int)result);
    return result;
}

static int any_touch_active(unsigned int port)
{
    int slot;
    int active = 0;

    if (port > VITACOMPANION_TOUCH_REAR)
        return 0;
    state_lock();
    for (slot = 0; slot < VITACOMPANION_TOUCH_SLOTS; ++slot)
        active |= touch_states[port][slot].active;
    state_unlock();
    return active;
}

static void patch_user_touch_data(unsigned int port, void *user_data,
    int count)
{
    SceTouchData sample;
    SceUInt64 previous = 0;
    int i;

    if (count <= 0 || count > 64 || !user_data || !any_touch_active(port))
        return;

    for (i = 0; i < count; ++i)
    {
        void *user_sample = (char *)user_data + i * sizeof(SceTouchData);

        if (ksceKernelCopyFromUser(&sample, user_sample, sizeof(sample)) < 0)
            return;
        /* Bail out unless this really is a SceTouchData history. */
        if (sample.reportNum > SCE_TOUCH_MAX_REPORT ||
            sample.timeStamp < previous)
            return;
        previous = sample.timeStamp;

        patch_touch_data(port, &sample, 1);
        if (ksceKernelCopyToUser(user_sample, &sample, sizeof(sample)) < 0)
            return;
    }
}

static int touch_peek_region_ext_hook(unsigned int port, void *data,
    unsigned int count, int region)
{
    int result = TAI_CONTINUE(int, touch_peek_region_ext_ref,
        port, data, count, region);

    if (result > 0)
        patch_user_touch_data(port, data, result);
    return result;
}

static int touch_read_region_ext_hook(unsigned int port, void *data,
    unsigned int count, int region)
{
    int result = TAI_CONTINUE(int, touch_read_region_ext_ref,
        port, data, count, region);

    if (result > 0)
        patch_user_touch_data(port, data, result);
    return result;
}

static void install_touch_ext_hooks(const char *module_name)
{
    touch_ext_hook_ids[0] = taiHookFunctionExportForKernel(KERNEL_PID,
        &touch_peek_region_ext_ref, module_name, SCE_TOUCH_USER_LIBRARY_NID,
        0x2CF6D7E2, touch_peek_region_ext_hook);
    touch_ext_hook_ids[1] = taiHookFunctionExportForKernel(KERNEL_PID,
        &touch_read_region_ext_ref, module_name, SCE_TOUCH_USER_LIBRARY_NID,
        0x9F0ACAF9, touch_read_region_ext_hook);
}

static void release_touch_hooks(void)
{
    if (touch_ext_hook_ids[1] >= 0)
        taiHookReleaseForKernel(touch_ext_hook_ids[1], touch_read_region_ext_ref);
    if (touch_ext_hook_ids[0] >= 0)
        taiHookReleaseForKernel(touch_ext_hook_ids[0], touch_peek_region_ext_ref);
    touch_ext_hook_ids[0] = touch_ext_hook_ids[1] = -1;

    tai_hook_ref_t *refs[] = {
        &touch_peek_ref,
        &touch_peek_region_ref,
        &touch_read_ref,
        &touch_read_region_ref
    };
    int i;

    for (i = 3; i >= 0; --i)
    {
        if (touch_hook_ids[i] >= 0)
        {
            taiHookReleaseForKernel(touch_hook_ids[i], *refs[i]);
            touch_hook_ids[i] = -1;
        }
    }
}

static int install_touch_hooks(void)
{
    static const uint32_t nids[4] = {
        0xBAD1960B,
        0x9B3F7207,
        0x70C8AACE,
        0x9A91F624
    };
    static const void *functions[4] = {
        touch_peek_hook,
        touch_peek_region_hook,
        touch_read_hook,
        touch_read_region_hook
    };
    tai_hook_ref_t *refs[4] = {
        &touch_peek_ref,
        &touch_peek_region_ref,
        &touch_read_ref,
        &touch_read_region_ref
    };
    tai_module_info_t info;
    const char *module_name = "SceTouch";
    int i;

    info.size = sizeof(info);
    if (taiGetModuleInfoForKernel(KERNEL_PID, module_name, &info) < 0)
    {
        module_name = "SceTouchDummy";
        info.size = sizeof(info);
        if (taiGetModuleInfoForKernel(KERNEL_PID, module_name, &info) < 0)
            return -1;
    }

    for (i = 0; i < 4; ++i)
    {
        touch_hook_ids[i] = taiHookFunctionExportForKernel(
            KERNEL_PID, refs[i], module_name, TAI_ANY_LIBRARY,
            nids[i], functions[i]);
        if (touch_hook_ids[i] < 0)
        {
            int result = touch_hook_ids[i];
            release_touch_hooks();
            return result;
        }
    }

    install_touch_ext_hooks(module_name);
    return 0;
}

int vitaCompanionKernelGetApiVersion(void)
{
    return VITACOMPANION_KERNEL_ABI_VERSION;
}

int vitaCompanionKernelSetButtons(uint32_t buttons, int pressed)
{
    state_lock();
    if (pressed)
        button_state |= buttons;
    else
        button_state &= ~buttons;
    state_unlock();
    return 0;
}

int vitaCompanionKernelSetAnalog(int stick, int x, int y, int active)
{
    if (stick < VITACOMPANION_STICK_LEFT ||
        stick > VITACOMPANION_STICK_RIGHT ||
        x < 0 || x > 255 || y < 0 || y > 255)
        return -1;

    state_lock();
    analog_states[stick].active = active != 0;
    analog_states[stick].x = active
        ? (uint8_t)x : VITACOMPANION_ANALOG_CENTER;
    analog_states[stick].y = active
        ? (uint8_t)y : VITACOMPANION_ANALOG_CENTER;
    state_unlock();
    return 0;
}

int vitaCompanionKernelSetTouch(int port, int slot_and_active, int x, int y)
{
    int slot = slot_and_active & 0xff;
    int active = (slot_and_active & VITACOMPANION_TOUCH_ACTIVE_FLAG) != 0;

    if (port < VITACOMPANION_TOUCH_FRONT ||
        port > VITACOMPANION_TOUCH_REAR ||
        (slot_and_active & ~0x103) != 0 ||
        slot < 0 ||
        slot >= VITACOMPANION_TOUCH_SLOTS ||
        x < 0 || x > VITACOMPANION_TOUCH_MAX_X ||
        y < 0 || y > VITACOMPANION_TOUCH_MAX_Y)
        return -1;

    state_lock();
    touch_states[port][slot].active = active != 0;
    touch_states[port][slot].x = (uint16_t)x;
    touch_states[port][slot].y = (uint16_t)y;
    state_unlock();
    return 0;
}

int vitaCompanionKernelReset(void)
{
    state_lock();
    clear_state();
    state_unlock();
    return 0;
}

/* ---- screen capture -------------------------------------------------------------------- */
#define FB_ROW_BUF_SIZE 0x4000   /* one source row (<= 1024 px * 4) + one output row */
static SceUID fb_mutex = -1;
static SceUID fb_buf_uid = -1;
static uint8_t *fb_buf;

static int framebuffer_info(SceDisplayFrameBufInfo *info)
{
    int head = ksceDisplayGetPrimaryHead();
    int index;

    /* index 0 is normally the foreground application; some states only fill index 1 */
    for (index = 0; index < 2; ++index)
    {
        memset(info, 0, sizeof(*info));
        info->size = sizeof(*info);
        if (ksceDisplayGetProcFrameBufInternal(-1, head, index, info) >= 0 &&
            info->framebuf.base != NULL && info->framebuf.width && info->framebuf.height)
            return 0;
    }
    return -1;
}

static int grab_frame(void *dst, uint32_t dst_size, uint32_t xy, uint32_t wh_shift)
{
    SceDisplayFrameBufInfo info;
    VcFrameHeader hdr;
    uint32_t x0 = xy >> 16, y0 = xy & 0xffff;
    uint32_t w = wh_shift >> 20, h = (wh_shift >> 8) & 0xfff, shift = wh_shift & 0xff;
    uint32_t step, out_w, out_h, row, col, pitch_bytes;
    uint32_t *src_row;
    uint16_t *out_row;
    uint8_t *out;

    if (shift > 3)
        return -2;
    if (framebuffer_info(&info) < 0)
        return -3;
    if (x0 >= info.framebuf.width || y0 >= info.framebuf.height)
        return -4;
    if (w == 0 || x0 + w > info.framebuf.width)
        w = info.framebuf.width - x0;
    if (h == 0 || y0 + h > info.framebuf.height)
        h = info.framebuf.height - y0;
    step = 1u << shift;
    out_w = w >> shift;
    out_h = h >> shift;
    if (out_w == 0 || out_h == 0 || w * 4 + out_w * 2 > FB_ROW_BUF_SIZE)
        return -5;
    if (dst_size < sizeof(hdr) + out_w * out_h * 2)
        return -6;

    memset(&hdr, 0, sizeof(hdr));
    hdr.magic = VITACOMPANION_FRAME_MAGIC;
    hdr.src_w = info.framebuf.width;
    hdr.src_h = info.framebuf.height;
    hdr.x = x0;
    hdr.y = y0;
    hdr.out_w = out_w;
    hdr.out_h = out_h;
    hdr.pixelformat = info.framebuf.pixelformat;
    hdr.pid = info.pid;
    if (ksceKernelCopyToUser(dst, &hdr, sizeof(hdr)) < 0)
        return -7;

    if (fb_buf == NULL)
    {
        fb_buf_uid = ksceKernelAllocMemBlock("vitacompanion_fb",
            SCE_KERNEL_MEMBLOCK_TYPE_KERNEL_RW, FB_ROW_BUF_SIZE, NULL);
        if (fb_buf_uid < 0 || ksceKernelGetMemBlockBase(fb_buf_uid, (void **)&fb_buf) < 0)
            return -8;
    }
    src_row = (uint32_t *)fb_buf;
    out_row = (uint16_t *)(fb_buf + w * 4);
    out = (uint8_t *)dst + sizeof(hdr);
    pitch_bytes = info.framebuf.pitch * 4;

    for (row = 0; row < out_h; ++row)
    {
        const uint8_t *src = (const uint8_t *)info.framebuf.base +
            (y0 + row * step) * pitch_bytes + x0 * 4;
        if (ksceKernelCopyFromUserProc(info.pid, src_row, src, w * 4) < 0)
            return -9;
        for (col = 0; col < out_w; ++col)
        {
            uint32_t p = src_row[col * step];    /* A8B8G8R8: R in the low byte */
            out_row[col] = (uint16_t)(((p & 0xf8) << 8) | ((p >> 5) & 0x7e0) | ((p >> 19) & 0x1f));
        }
        if (ksceKernelCopyToUser(out + row * out_w * 2, out_row, out_w * 2) < 0)
            return -10;
    }
    return (int)(sizeof(hdr) + out_w * out_h * 2);
}

int vitaCompanionKernelGrabFrame(void *dst, uint32_t dst_size, uint32_t xy, uint32_t wh_shift)
{
    uint32_t state;
    int result;

    ENTER_SYSCALL(state);
    if (fb_mutex >= 0)
        ksceKernelLockMutex(fb_mutex, 1, NULL);
    result = grab_frame(dst, dst_size, xy, wh_shift);
    if (fb_mutex >= 0)
        ksceKernelUnlockMutex(fb_mutex, 1);
    EXIT_SYSCALL(state);
    return result;
}

int module_start(SceSize argc, const void *args)
{
    int result;

    (void)argc;
    (void)args;

    clear_state();
    state_mutex = ksceKernelCreateMutex(
        "vitacompanion_input_mutex", 0, 0, NULL);
    if (state_mutex < 0)
        return SCE_KERNEL_START_FAILED;
    fb_mutex = ksceKernelCreateMutex("vitacompanion_fb_mutex", 0, 0, NULL);

    result = install_touch_hooks();
    if (result < 0)
    {
        ksceKernelDeleteMutex(state_mutex);
        state_mutex = -1;
        return SCE_KERNEL_START_FAILED;
    }

    ctrl_thread_id = ksceKernelCreateThread(
        "vitacompanion_input_thread", ctrl_thread,
        0x40, 0x1000, 0, 0, NULL);
    if (ctrl_thread_id < 0)
    {
        release_touch_hooks();
        ksceKernelDeleteMutex(state_mutex);
        state_mutex = -1;
        return SCE_KERNEL_START_FAILED;
    }

    ctrl_thread_running = 1;
    result = ksceKernelStartThread(ctrl_thread_id, 0, NULL);
    if (result < 0)
    {
        ctrl_thread_running = 0;
        ksceKernelDeleteThread(ctrl_thread_id);
        ctrl_thread_id = -1;
        release_touch_hooks();
        ksceKernelDeleteMutex(state_mutex);
        state_mutex = -1;
        return SCE_KERNEL_START_FAILED;
    }

    return SCE_KERNEL_START_SUCCESS;
}

int module_stop(SceSize argc, const void *args)
{
    (void)argc;
    (void)args;

    state_lock();
    clear_state();
    state_unlock();

    ctrl_thread_running = 0;
    if (ctrl_thread_id >= 0)
    {
        ksceKernelWaitThreadEnd(ctrl_thread_id, NULL, NULL);
        ctrl_thread_id = -1;
    }

    release_touch_hooks();
    reset_ctrl_emulation();

    if (state_mutex >= 0)
    {
        ksceKernelDeleteMutex(state_mutex);
        state_mutex = -1;
    }
    if (fb_buf_uid >= 0)
    {
        ksceKernelFreeMemBlock(fb_buf_uid);
        fb_buf_uid = -1;
        fb_buf = NULL;
    }
    if (fb_mutex >= 0)
    {
        ksceKernelDeleteMutex(fb_mutex);
        fb_mutex = -1;
    }

    return SCE_KERNEL_STOP_SUCCESS;
}

int _start(SceSize argc, const void *args)
    __attribute__((weak, alias("module_start")));
