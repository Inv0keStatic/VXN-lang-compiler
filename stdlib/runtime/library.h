#ifndef VX_LIBRARY_H
#define VX_LIBRARY_H
#ifndef VX_GRAPHICS_CORE_H
#include "graphics_core.h"
#endif
#if defined(_WIN32)
#ifndef VX_PLATFORM_WIN32_H
#include "platform_win32.h"
#endif
#elif defined(__linux__)
#ifndef VX_PLATFORM_LINUX_H
#include "platform_linux.h"
#endif
#else
#ifndef VX_PLATFORM_STUB_H
#include "platform_stub.h"
#endif
#endif

static void vx_lib_init(VxLibrary *library) {
    memset(library,0,sizeof *library);
    for(int i=0;i<VX_GFX_MAX_SURFACES;++i) library->surfaces[i].generation=1;
}
static VxSurface *vx_lib_get(VxLibrary *library,uint64_t handle) {
    unsigned slot=(unsigned)(handle&255);
    if(!slot||slot>VX_GFX_MAX_SURFACES) return NULL;
    VxSurface *s=&library->surfaces[slot-1];
    return s->pixels&&s->generation==(handle>>8)?s:NULL;
}
static int vx_lib_create(VxLibrary *library,uint64_t width,uint64_t height,uint64_t *handle) {
    if(!width||!height||width>VX_GFX_MAX_DIMENSION||height>VX_GFX_MAX_DIMENSION) return VX_GFX_INVALID_SIZE;
    size_t pixels=(size_t)width*(size_t)height;
    if(pixels>VX_GFX_MAX_PIXELS-library->pixels) return VX_GFX_RESOURCE;
    int slot=0;
    while(slot<VX_GFX_MAX_SURFACES&&(library->surfaces[slot].pixels||!library->surfaces[slot].generation)) ++slot;
    if(slot==VX_GFX_MAX_SURFACES) return VX_GFX_RESOURCE;
    VxSurface *s=&library->surfaces[slot]; uint64_t generation=s->generation;
    memset(s,0,sizeof *s); s->generation=generation;
    s->pixels=calloc(pixels,sizeof *s->pixels); if(!s->pixels) return VX_GFX_RESOURCE;
    s->width=(int)width; s->height=(int)height;
    s->client_width=s->width; s->client_height=s->height;
    library->pixels+=pixels; *handle=(generation<<8)|(unsigned)(slot+1);
    return -1;
}
static bool vx_lib_release(VxLibrary *library,VxSurface *s) {
    bool ok=vx_platform_close(s);
    library->pixels-=(size_t)s->width*(size_t)s->height;
    free(s->pixels); s->pixels=NULL; s->window=false;
    s->generation=s->generation==(UINT64_MAX>>8)?0:s->generation+1;
    return ok;
}
static bool vx_lib_finish(VxLibrary *library) {
    bool ok=true;
    for(int i=0;i<VX_GFX_MAX_SURFACES;++i)
        if(library->surfaces[i].pixels&&!vx_lib_release(library,&library->surfaces[i])) ok=false;
    vx_platform_finish(); return ok;
}
static uint64_t vx_lib_double(double value) {
    uint64_t bits; memcpy(&bits,&value,sizeof bits); return bits;
}
static int vx_lib_signed(uint64_t value) {
    /* Source is canonical sint32; avoid implementation-defined unsigned casts. */
    return value&UINT64_C(0x80000000)?(int)(-(int64_t)(UINT64_C(0x100000000)-value)):(int)value;
}
static void vx_lib_error(uint64_t *result,int error) { result[0]=1; result[1]=(uint64_t)error; }
static void vx_lib_exec(VxLibrary *library,int op,const uint64_t *args,size_t length,uint64_t *result) {
    result[0]=result[1]=0;
    if(op==VX_TIME_NOW) {
        double seconds;
        if(vx_platform_now(&seconds)) result[1]=vx_lib_double(seconds);
        else vx_lib_error(result,
#if defined(_WIN32) || defined(__linux__)
            VX_TIME_UNAVAILABLE
#else
            VX_TIME_UNSUPPORTED
#endif
        );
        return;
    }
    if(op==VX_TIME_SLEEP) {
        if(args[0]>60000) vx_lib_error(result,VX_TIME_INVALID_DURATION);
        else if(!vx_platform_sleep((unsigned)args[0])) vx_lib_error(result,
#if defined(_WIN32) || defined(__linux__)
            VX_TIME_UNAVAILABLE
#else
            VX_TIME_UNSUPPORTED
#endif
        );
        return;
    }
    if(op==VX_WINDOW_OPEN||op==VX_CANVAS_CREATE) {
        uint64_t handle=0; int error=vx_lib_create(library,args[0],args[1],&handle);
        if(error<0&&op==VX_WINDOW_OPEN) {
            VxSurface *s=vx_lib_get(library,handle);
            error=vx_platform_open(s,args+3,length,args[2]==0);
            if(error>=0) (void)vx_lib_release(library,s);
        }
        if(error<0) result[1]=handle; else vx_lib_error(result,error);
        return;
    }
    VxSurface *s=vx_lib_get(library,args[0]);
    if(!s) { vx_lib_error(result,VX_GFX_INVALID_HANDLE); return; }
    if(op>=VX_WINDOW_CANVAS&&op<=VX_WINDOW_CLOSE&&!s->window) {
        vx_lib_error(result,VX_GFX_WRONG_KIND); return;
    }
    if(op==VX_WINDOW_CANVAS) { result[1]=args[0]; return; }
    if(op==VX_WINDOW_CLOSE||op==VX_CANVAS_CLOSE) {
        if(op==VX_CANVAS_CLOSE&&s->window) { vx_lib_error(result,VX_GFX_WRONG_KIND); return; }
        if(!vx_lib_release(library,s)) vx_lib_error(result,VX_GFX_CLOSE_FAILED);
        return;
    }
    if(op==VX_WINDOW_PRESENT) {
        if(!vx_platform_present(s)) vx_lib_error(result,VX_GFX_PRESENT_FAILED);
        return;
    }
    if(op==VX_WINDOW_POLL) {
        vx_platform_poll(s);
        uint64_t *frame=result+1; memset(frame,0,VX_FRAME_CELLS*sizeof *frame);
        frame[0]=s->closed; frame[1]=(unsigned)s->width; frame[2]=(unsigned)s->height;
        frame[3]=(unsigned)s->client_width; frame[4]=(unsigned)s->client_height;
        int64_t x=s->client_width?(int64_t)s->mouse_x*s->width/s->client_width:0;
        int64_t y=s->client_height?(int64_t)s->mouse_y*s->height/s->client_height:0;
        frame[5]=(uint32_t)x; frame[6]=(uint32_t)y;
        frame[7]=vx_lib_double((double)s->wheel/120.0);
        double now=s->last_poll; (void)vx_platform_now(&now);
        frame[8]=vx_lib_double(now>=s->last_poll?now-s->last_poll:0); s->last_poll=now;
        for(int i=0;i<VX_GFX_KEYS;++i) { frame[9+i]=s->keys[i]; frame[9+VX_GFX_KEYS+i]=s->pressed[i]; }
        for(int i=0;i<VX_GFX_BUTTONS;++i) {
            frame[9+2*VX_GFX_KEYS+i]=s->buttons[i];
            frame[9+2*VX_GFX_KEYS+VX_GFX_BUTTONS+i]=s->clicked[i];
        }
        memset(s->pressed,0,sizeof s->pressed); memset(s->clicked,0,sizeof s->clicked); s->wheel=0;
        return;
    }
    if(op==VX_CANVAS_READ_PIXEL) {
        int x=vx_lib_signed(args[1]),y=vx_lib_signed(args[2]);
        if(x<0||y<0||x>=s->width||y>=s->height) { vx_lib_error(result,VX_GFX_OUT_OF_BOUNDS); return; }
        uint32_t color=s->pixels[(size_t)y*(size_t)s->width+(size_t)x];
        result[1]=(color>>16)&255; result[2]=(color>>8)&255; result[3]=color&255; result[4]=255; return;
    }
    if(op==VX_CANVAS_COPY) {
        VxSurface *source=vx_lib_get(library,args[1]);
        if(!source) { vx_lib_error(result,VX_GFX_INVALID_HANDLE); return; }
        vx_gfx_copy(s,source,vx_lib_signed(args[2]),vx_lib_signed(args[3]));
        return;
    }
    int offset=op==VX_DRAW_CLEAR?1:op==VX_DRAW_PIXEL?3:op==VX_DRAW_LINE||op==VX_DRAW_RECT?5:4;
    const uint64_t *color=args+offset; uint32_t rgb=vx_gfx_rgb(color); unsigned alpha=(unsigned)color[3];
    if(op==VX_DRAW_CLEAR) { vx_gfx_clear(s,rgb); return; }
    int x=vx_lib_signed(args[1]),y=vx_lib_signed(args[2]);
    switch(op) {
    case VX_DRAW_PIXEL:vx_gfx_pixel(s,x,y,rgb,alpha);break;
    case VX_DRAW_LINE:vx_gfx_line(s,x,y,vx_lib_signed(args[3]),vx_lib_signed(args[4]),rgb,alpha);break;
    case VX_DRAW_RECT:vx_gfx_rect(s,x,y,args[3],args[4],rgb,alpha);break;
    case VX_DRAW_CIRCLE:
        if(args[3]>VX_GFX_MAX_DIMENSION) vx_lib_error(result,VX_GFX_INVALID_ARGUMENT);
        else vx_gfx_circle(s,x,y,(unsigned)args[3],rgb,alpha);
        break;
    case VX_DRAW_TEXT:
        if(!args[3]||args[3]>16) { vx_lib_error(result,VX_GFX_INVALID_ARGUMENT); break; }
        if(length>1024) { vx_lib_error(result,VX_GFX_INVALID_TEXT); break; }
        for(size_t i=0;i<length;++i) if(args[8+i]!='\n'&&(args[8+i]<32||args[8+i]>126)) {
            vx_lib_error(result,VX_GFX_INVALID_TEXT); return;
        }
        vx_gfx_text(s,x,y,(unsigned)args[3],args+8,length,rgb,alpha);
        break;
    default:vx_lib_error(result,VX_GFX_INVALID_ARGUMENT);break;
    }
}
#endif
