#include <psp2/kernel/modulemgr.h>
#include <psp2/kernel/threadmgr.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#define TAIPOOL_AS_STDLIB
#include "cmd.h"
#include "input.h"
#include "main.h"
#include "net.h"
#include "nosleep.h"

#include <taipool.h>

extern SceUID net_thid;
extern volatile int all_is_up;
extern volatile int net_connected;

volatile int run;

void __unused _start() __attribute__((weak, alias("module_start")));
int __unused module_start(SceSize argc, const void* args)
{
    int result;

    (void)argc;
    (void)args;

    /* Without the kernel module (missing, or a different ABI) input is off,
       but FTP and the command port still start so the device can be fixed
       remotely. */
    input_start();

    // Required for ftpvita
    result = taipool_init(1 * 1024 * 1024);
    if (result < 0)
    {
        input_end();
        return result;
    }

#if ENABLE_LOGGING == 1
    SceUID fd = sceIoOpen("ux0:dump/vitacompanion_log.txt", SCE_O_TRUNC | SCE_O_CREAT | SCE_O_WRONLY, 0666);
    sceIoClose(fd);
#endif
    run = 1;
    result = nosleep_start();
    if (result < 0)
    {
        run = 0;
        taipool_term();
        return result;
    }
    result = net_start();
    if (result < 0)
    {
        run = 0;
        nosleep_end();
        taipool_term();
        input_end();
        return result;
    }

    return SCE_KERNEL_START_SUCCESS;
}

int __unused module_stop(SceSize argc, const void* args)
{
    (void)argc;
    (void)args;

    run = 0;
    sceKernelWaitThreadEnd(net_thid, NULL, NULL);

    net_end();
    nosleep_end();
    input_end();

    taipool_term();

    return SCE_KERNEL_STOP_SUCCESS;
}
