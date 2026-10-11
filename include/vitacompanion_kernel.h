#pragma once

#include <stdint.h>

#define VITACOMPANION_KERNEL_ABI_VERSION 3
#define VITACOMPANION_KERNEL_MODULE_NAME "vitacompanion_kernel"
/* This syscall lost its fifth, stack-passed argument on device. */
#define VITACOMPANION_TOUCH_ACTIVE_FLAG 0x100

int vitaCompanionKernelGetApiVersion(void);
int vitaCompanionKernelSetButtons(uint32_t buttons, int pressed);
int vitaCompanionKernelSetAnalog(int stick, int x, int y, int active);
int vitaCompanionKernelSetTouch(int port, int slot_and_active, int x, int y);
int vitaCompanionKernelReset(void);

/*
 * Screen capture. Copies a rectangle of the framebuffer on screen, shrunk by
 * 1 << shift (nearest neighbour), into dst as a VcFrameHeader followed by
 * out_w * out_h RGB565 pixels. Four arguments only (see the touch note):
 *   xy        = x << 16 | y          (source pixels)
 *   wh_shift  = w << 20 | h << 8 | shift; w or h 0 = to the right/bottom edge
 * Returns the number of bytes written, or < 0.
 */
#define VITACOMPANION_FRAME_MAGIC 0x31464356 /* "VCF1" */
typedef struct VcFrameHeader {
    uint32_t magic;
    uint16_t src_w, src_h;   /* framebuffer size */
    uint16_t x, y;           /* rectangle origin, source pixels */
    uint16_t out_w, out_h;   /* pixels that follow */
    uint32_t pixelformat;    /* SCE_DISPLAY_PIXELFORMAT_*; only A8B8G8R8 is converted */
    int32_t pid;             /* process that owns the framebuffer */
} VcFrameHeader;

int vitaCompanionKernelGrabFrame(void *dst, uint32_t dst_size, uint32_t xy, uint32_t wh_shift);
