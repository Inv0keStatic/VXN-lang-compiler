#if defined(__linux__) && !defined(VX_PLATFORM_LINUX_H)
#define VX_PLATFORM_LINUX_H
#ifndef VX_GRAPHICS_CORE_H
#include "graphics_core.h"
#endif
#include <errno.h>
#include <time.h>

static bool vx_platform_now(double *seconds) {
    struct timespec value;
    if(clock_gettime(CLOCK_MONOTONIC,&value)) return false;
    *seconds=(double)value.tv_sec+(double)value.tv_nsec/1000000000.0;
    return true;
}
static bool vx_platform_sleep(unsigned milliseconds) {
    struct timespec remaining={(time_t)(milliseconds/1000),(long)(milliseconds%1000)*1000000L};
    while(nanosleep(&remaining,&remaining)) if(errno!=EINTR) return false;
    return true;
}
static int vx_platform_open(VxSurface *s,const uint64_t *title,size_t length,bool visible) {
    (void)s; (void)title; (void)length; (void)visible; return VX_GFX_UNSUPPORTED;
}
static void vx_platform_poll(VxSurface *s) { (void)s; }
static bool vx_platform_present(VxSurface *s) { (void)s; return false; }
static bool vx_platform_close(VxSurface *s) { (void)s; return true; }
static void vx_platform_finish(void) {}
#endif
