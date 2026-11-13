#if !defined(_WIN32) && !defined(__linux__) && !defined(VX_PLATFORM_STUB_H)
#define VX_PLATFORM_STUB_H
#ifndef VX_GRAPHICS_CORE_H
#include "graphics_core.h"
#endif
static bool vx_platform_now(double *seconds) { (void)seconds; return false; }
static bool vx_platform_sleep(unsigned milliseconds) { (void)milliseconds; return false; }
static int vx_platform_open(VxSurface *s,const uint64_t *title,size_t length,bool visible) {
    (void)s; (void)title; (void)length; (void)visible; return VX_GFX_UNSUPPORTED;
}
static void vx_platform_poll(VxSurface *s) { (void)s; }
static bool vx_platform_present(VxSurface *s) { (void)s; return false; }
static bool vx_platform_close(VxSurface *s) { (void)s; return true; }
static void vx_platform_finish(void) {}
#endif
