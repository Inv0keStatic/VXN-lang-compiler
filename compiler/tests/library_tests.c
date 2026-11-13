#include "../../stdlib/runtime/library.h"
#include <stdio.h>
static unsigned checks,failures;
static void expect(bool ok,const char *what) {
    ++checks; if(!ok) { ++failures; fprintf(stderr,"FAIL: %s\n",what); }
}
static void surfaces(void) {
    VxLibrary library; vx_lib_init(&library); uint64_t result[VX_LIBRARY_RESULT_CELLS],args[16]={8,8};
    vx_lib_exec(&library,VX_CANVAS_CREATE,args,0,result);
    expect(result[0]==0,"allocate offscreen canvas"); uint64_t first=result[1];
    VxSurface *s=vx_lib_get(&library,first);
    if(!s) { ++failures; return; }
    vx_gfx_circle(s,4,4,3,0xffffff,255);
    for(int y=0;y<8;++y) for(int x=0;x<8;++x)
        expect(s->pixels[y*8+x]==((x-4)*(x-4)+(y-4)*(y-4)<=9?0xffffffu:0),"circle coverage");
    vx_gfx_clear(s,0);
    vx_gfx_line(s,INT32_MIN,4,INT32_MAX,4,0x123456,255);
    for(int y=0;y<8;++y) for(int x=0;x<8;++x)
        expect(s->pixels[y*8+x]==(y==4?0x123456u:0),"bounded clipping across extreme line");
    vx_gfx_rect(s,INT32_MIN,INT32_MIN,UINT32_MAX,UINT32_MAX,0xabcdef,255);
    for(int i=0;i<64;++i) expect(s->pixels[i]==0xabcdef,"large rectangle clipped to canvas");
    args[0]=first; args[1]=0; args[2]=0; args[3]=4097; args[4]=255; args[5]=0; args[6]=0; args[7]=255;
    vx_lib_exec(&library,VX_DRAW_CIRCLE,args,0,result);
    expect(result[0]==1&&result[1]==VX_GFX_INVALID_ARGUMENT,"radius limit is checked");
    vx_lib_exec(&library,VX_CANVAS_CLOSE,args,0,result);
    expect(!result[0]&&!vx_lib_get(&library,first)&&library.pixels==0,"release invalidates all aliases");
    args[0]=1; args[1]=1; vx_lib_exec(&library,VX_CANVAS_CREATE,args,0,result);
    expect(!result[0]&&result[1]!=first&&!vx_lib_get(&library,first),"generation prevents stale reuse");
    expect(vx_lib_finish(&library),"cleanup remaining canvas");
    expect(library.pixels==0,"cleanup releases pixel budget");
    vx_lib_init(&library);
    for(int i=0;i<VX_GFX_MAX_SURFACES;++i) {
        uint64_t handle; expect(vx_lib_create(&library,1,1,&handle)<0,"surface slot allocation");
    }
    uint64_t handle;
    expect(vx_lib_create(&library,1,1,&handle)==VX_GFX_RESOURCE,"surface count bounded");
    expect(vx_lib_finish(&library),"cleanup surface table");
    vx_lib_init(&library);
    library.surfaces[0].generation=UINT64_MAX>>8;
    expect(vx_lib_create(&library,1,1,&handle)<0,"last generation allocation");
    uint64_t retired=handle;
    expect(vx_lib_release(&library,vx_lib_get(&library,handle)),"last generation release");
    expect(vx_lib_create(&library,1,1,&handle)<0&&(handle&255)==2&&!vx_lib_get(&library,retired),"exhausted slot retired");
    expect(vx_lib_finish(&library),"retirement cleanup");
}
#ifdef _WIN32
static void windows(void) {
    VxLibrary library; vx_lib_init(&library);
    uint64_t result[VX_LIBRARY_RESULT_CELLS],args[20]={32,24,1,'V','x','n',' ','t','e','s','t'};
    vx_lib_exec(&library,VX_WINDOW_OPEN,args,8,result);
    expect(!result[0],"create hidden Win32 window");
    if(result[0]) { (void)vx_lib_finish(&library); return; }
    uint64_t handle=result[1]; VxSurface *s=vx_lib_get(&library,handle); HWND native=(HWND)s->native;
    expect(IsWindow(native)&&!IsWindowVisible(native),"native window stays hidden");
    args[0]=handle; vx_lib_exec(&library,VX_CANVAS_CLOSE,args,0,result);
    expect(result[0]==1&&result[1]==VX_GFX_WRONG_KIND,"window owns canvas lifetime");
    PostMessageW(native,WM_KEYDOWN,VK_LEFT,0); PostMessageW(native,WM_KEYUP,VK_LEFT,0);
    PostMessageW(native,WM_MOUSEMOVE,0,MAKELPARAM(16,12));
    PostMessageW(native,WM_MOUSEWHEEL,MAKEWPARAM(0,120),0);
    PostMessageW(native,WM_MOUSEWHEEL,MAKEWPARAM(0,240),0);
    vx_lib_exec(&library,VX_WINDOW_POLL,args,0,result);
    expect(!result[0]&&!result[1+9]&&result[1+9+VX_GFX_KEYS],"press and release within one poll retained");
    expect(result[1+5]==16&&result[1+6]==12,"client mouse coordinates");
    double wheel; memcpy(&wheel,result+1+7,sizeof wheel);
    expect(wheel==3.0,"wheel events accumulate");
    vx_lib_exec(&library,VX_WINDOW_POLL,args,0,result);
    expect(!result[1+9+VX_GFX_KEYS]&&!result[1+7],"transient input consumed once");
    PostMessageW(native,WM_KEYDOWN,'W',0);
    vx_lib_exec(&library,VX_WINDOW_POLL,args,0,result);
    expect(result[1+9+6+'W'-'A']&&result[1+9+VX_GFX_KEYS+6+'W'-'A'],"letter key mapping");
    PostMessageW(native,WM_KEYDOWN,'W',1L<<30);
    vx_lib_exec(&library,VX_WINDOW_POLL,args,0,result);
    expect(result[1+9+6+'W'-'A']&&!result[1+9+VX_GFX_KEYS+6+'W'-'A'],"autorepeat is not a new press");
    PostMessageW(native,WM_KILLFOCUS,0,0);
    vx_lib_exec(&library,VX_WINDOW_POLL,args,0,result);
    expect(!result[1+9+6+'W'-'A'],"focus loss clears held keys");
    RECT bounds={0,0,320,240}; AdjustWindowRectEx(&bounds,WS_OVERLAPPEDWINDOW,FALSE,0);
    SetWindowPos(native,NULL,0,0,bounds.right-bounds.left,bounds.bottom-bounds.top,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
    PostMessageW(native,WM_MOUSEMOVE,0,MAKELPARAM(160,120));
    vx_lib_exec(&library,VX_WINDOW_POLL,args,0,result);
    expect(result[2]==32&&result[3]==24&&result[4]==320&&result[5]==240,"resize preserves logical canvas");
    expect(result[6]==16&&result[7]==12,"mouse maps to logical canvas after resize");
    vx_lib_exec(&library,VX_WINDOW_PRESENT,args,0,result);
    expect(!result[0],"GDI presentation");
    PostMessageW(native,WM_CLOSE,0,0); vx_lib_exec(&library,VX_WINDOW_POLL,args,0,result);
    expect(!result[0]&&result[1],"close request reported without invalidating canvas");
    expect(vx_lib_finish(&library)&&!IsWindow(native),"execution cleanup destroys native window");
    args[0]=32; args[1]=24; args[2]=1; args[3]=0xc0; args[4]=0x80;
    vx_lib_exec(&library,VX_WINDOW_OPEN,args,2,result);
    expect(result[0]==1&&result[1]==VX_GFX_INVALID_TEXT&&library.pixels==0,"invalid UTF-8 creation rolls back allocation");
    expect(vx_lib_finish(&library),"failed-open cleanup");
}
#endif
static void copies(void) {
    uint32_t original[16],pixels[16],other[16];
    for(unsigned i=0;i<16;++i) original[i]=i+1;
    VxSurface source={0},destination={0};
    source.width=source.height=destination.width=destination.height=4;
    source.pixels=pixels; destination.pixels=other;
    for(int alias=0;alias<2;++alias) for(int y=-5;y<=5;++y) for(int x=-5;x<=5;++x) {
        memcpy(pixels,original,sizeof pixels); memset(other,0,sizeof other);
        VxSurface *target=alias?&source:&destination;
        vx_gfx_copy(target,&source,x,y);
        for(int row=0;row<4;++row) for(int col=0;col<4;++col) {
            int sx=col-x,sy=row-y;
            uint32_t wanted=sx>=0&&sx<4&&sy>=0&&sy<4?original[sy*4+sx]:alias?original[row*4+col]:0;
            expect(target->pixels[row*4+col]==wanted,"canvas copy matches original source with clipping/overlap");
        }
    }
    memcpy(pixels,original,sizeof pixels);
    vx_gfx_copy(&source,&source,INT32_MIN,INT32_MAX);
    expect(!memcmp(pixels,original,sizeof pixels),"extreme copy offsets are no-ops");
    uint32_t wide[35]={0};
    destination.width=7; destination.height=5; destination.pixels=wide;
    vx_gfx_copy(&destination,&source,5,3);
    for(int row=0;row<5;++row) for(int col=0;col<7;++col) {
        uint32_t wanted=row>=3&&col>=5?original[(row-3)*4+col-5]:0;
        expect(wide[row*7+col]==wanted,"copy respects different source/destination row strides");
    }
}
int main(void) {
    copies();
    surfaces();
#if defined(_WIN32) || defined(__linux__)
    double before=0,after=0;
    expect(vx_platform_now(&before),"read monotonic clock");
    expect(vx_platform_sleep(1),"bounded platform sleep");
    expect(vx_platform_now(&after)&&after>=before,"clock does not go backwards");
#endif
#if defined(__linux__)
    VxLibrary library; vx_lib_init(&library);
    uint64_t result[VX_LIBRARY_RESULT_CELLS],args[4]={32,24,1,0};
    vx_lib_exec(&library,VX_WINDOW_OPEN,args,0,result);
    expect(result[0]==1&&result[1]==VX_GFX_UNSUPPORTED,"Linux window backend is explicitly unsupported");
    expect(!library.pixels&&vx_lib_finish(&library),"unsupported window releases its canvas");
#endif
#ifdef _WIN32
    windows();
#endif
    printf("%u standard-library checks, %u failures\n",checks,failures);
    if(fflush(stdout)) { fputs("cannot flush test report\n",stderr); return 1; }
    return failures?1:0;
}
