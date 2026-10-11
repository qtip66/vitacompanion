#include "net.h"

#include "cmd.h"
#include "log.h"
#include "screen.h"

#include <ftpvita.h>
#include <vitasdk.h>

#define NET_CTL_ERROR_NOT_TERMINATED ((int)0x80412102)

extern volatile int run;

volatile int all_is_up;
volatile int net_connected;

SceUID net_thid = -1;
static int netctl_cb_id = -1;
static int netctl_initialized;

int net_start()
{
    int result;

    net_thid = sceKernelCreateThread("vitacompanion_net_thread", net_thread, 0x40, 0x10000, 0, 0, NULL);
    if (net_thid < 0)
        return net_thid;

    result = sceKernelStartThread(net_thid, 0, NULL);
    if (result < 0)
    {
        sceKernelDeleteThread(net_thid);
        net_thid = -1;
        return result;
    }

    return 0;
}

void net_end()
{
    if (netctl_cb_id >= 0)
    {
        sceNetCtlInetUnregisterCallback(netctl_cb_id);
        netctl_cb_id = -1;
    }

    if (all_is_up)
    {
        screen_end();
        cmd_end();
        ftpvita_fini();
        all_is_up = 0;
    }

    if (netctl_initialized)
    {
        sceNetCtlTerm();
        netctl_initialized = 0;
    }
}

static void do_net_connected()
{
    char vita_ip[16];
    unsigned short int vita_port;

    LOG("do_net_connected\n");

#if ENABLE_LOGGING == 1
    ftpvita_set_info_log_cb(LOG);
    ftpvita_set_debug_log_cb(LOG);
#endif

    ftpvita_set_file_buf_size(512 * 1024);

    if (ftpvita_init(vita_ip, &vita_port) >= 0)
    {
        ftpvita_add_device("ux0:");
        ftpvita_add_device("ur0:");
        ftpvita_add_device("uma0:");
        ftpvita_add_device("imc0:");
        ftpvita_add_device("xmc0:");
        ftpvita_add_device("grw0:");

        if (cmd_start() >= 0)
        {
            screen_start();   /* optional: FTP and commands work without it */
            all_is_up = 1;
        }
        else
        {
            ftpvita_fini();
        }
    }
}

static void netctl_cb(int event_type, void* arg)
{
    int state;
    int result;

    LOG("netctl cb: %d\n", event_type);
    (void)arg;

    result = sceNetCtlInetGetState(&state);
    if (result < 0)
    {
        LOG("sceNetCtlInetGetState: 0x%08X\n", result);
        return;
    }

    if (state != 3 && all_is_up)
    {
        net_connected = 0;
        screen_end();
        cmd_end();
        ftpvita_fini();
        all_is_up = 0;
    }
    else if (state == 3 && !all_is_up)
    { /* IP obtained */
        net_connected = 1;
        do_net_connected();
    }
}

int net_thread(unsigned int args, void* argp)
{
    int ret;

    (void)args;
    (void)argp;

    sceKernelDelayThread(3 * 1000 * 1000);
    if (!run)
        return 0;

    ret = sceNetCtlInit();
    LOG("sceNetCtlInit: 0x%08X\n", ret);
    if (ret < 0 && ret != NET_CTL_ERROR_NOT_TERMINATED)
        return ret;
    netctl_initialized = ret == 0;

    ret = sceNetCtlInetRegisterCallback(netctl_cb, NULL, &netctl_cb_id);
    LOG("sceNetCtlInetRegisterCallback: 0x%08X\n", ret);
    if (ret < 0)
        return ret;

    netctl_cb(0, NULL);
    int tick = 0;
    while (run)
    {
        sceNetCtlCheckCallback();
        /* A Wi-Fi blip tears the servers down; if bringing them back fails (seen after RetroArch crashes: ping
           answers, every port refuses) netctl sends no further event. Retry every 5 s while they are down. */
        if (!all_is_up && ++tick >= 5)
        {
            tick = 0;
            netctl_cb(0, NULL);
        }
        sceKernelDelayThread(1000 * 1000);
    }

    return 0;
}
