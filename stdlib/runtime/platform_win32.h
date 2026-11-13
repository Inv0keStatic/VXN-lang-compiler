#if defined(_WIN32) && !defined(VX_PLATFORM_WIN32_H)
#define VX_PLATFORM_WIN32_H
/*
 * Vxn Win32 platform adaptation. Altered from ideas and message-handling code
 * in raylib 6.2, src/platforms/rcore_desktop_win32.c; NOT the original raylib
 * backend. Vxn owns its state, software canvas, handles and cell ABI.
 *
 * Copyright (c) 2013-2026 Ramon Santamaria (@raysan5) and contributors
 *
 * This software is provided "as-is", without any express or implied warranty. In no event
 * will the authors be held liable for any damages arising from the use of this software.
 *
 * Permission is granted to anyone to use this software for any purpose, including commercial
 * applications, and to alter it and redistribute it freely, subject to the following restrictions:
 *
 *   1. The origin of this software must not be misrepresented; you must not claim that you
 *   wrote the original software. If you use this software in a product, an acknowledgment
 *   in the product documentation would be appreciated but is not required.
 *
 *   2. Altered source versions must be plainly marked as such, and must not be misrepresented
 *   as being the original software.
 *
 *   3. This notice may not be removed or altered from any source distribution.
 */
#ifndef VX_GRAPHICS_CORE_H
#include "graphics_core.h"
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <windowsx.h>

static bool vx_platform_now(double *seconds) {
    LARGE_INTEGER frequency,counter;
    if(!QueryPerformanceFrequency(&frequency)||!QueryPerformanceCounter(&counter)||frequency.QuadPart<=0) return false;
    *seconds=(double)counter.QuadPart/(double)frequency.QuadPart; return true;
}
static bool vx_platform_sleep(unsigned milliseconds) { Sleep(milliseconds); return true; }
static bool vx_win_blit(VxSurface *s,HWND window,HDC dc) {
    RECT bounds;
    if(!GetClientRect(window,&bounds)) return false;
    if(bounds.right==0||bounds.bottom==0) return true;
    BITMAPINFO info; memset(&info,0,sizeof info);
    info.bmiHeader.biSize=sizeof info.bmiHeader;
    info.bmiHeader.biWidth=s->width; info.bmiHeader.biHeight=-s->height;
    info.bmiHeader.biPlanes=1; info.bmiHeader.biBitCount=32; info.bmiHeader.biCompression=BI_RGB;
    SetStretchBltMode(dc,COLORONCOLOR);
    return StretchDIBits(dc,0,0,bounds.right,bounds.bottom,0,0,s->width,s->height,
                        s->pixels,&info,DIB_RGB_COLORS,SRCCOPY)>0;
}
static int vx_win_key(WPARAM key) {
    if(key>='A'&&key<='Z') return 6+(int)(key-'A');
    switch(key) {
    case VK_LEFT:return 0; case VK_RIGHT:return 1; case VK_UP:return 2;
    case VK_DOWN:return 3; case VK_ESCAPE:return 4; case VK_SPACE:return 5;
    default:return -1;
    }
}
static LRESULT CALLBACK vx_win_proc(HWND window,UINT message,WPARAM wp,LPARAM lp) {
    VxSurface *s=(VxSurface *)(uintptr_t)GetWindowLongPtrW(window,GWLP_USERDATA);
    if(message==WM_NCCREATE) {
        s=(VxSurface *)((CREATESTRUCTW *)lp)->lpCreateParams;
        SetWindowLongPtrW(window,GWLP_USERDATA,(LONG_PTR)s);
    }
    if(!s) return DefWindowProcW(window,message,wp,lp);
    switch(message) {
    case WM_CLOSE:s->closed=true; return 0;
    case WM_NCDESTROY:
        s->closed=true; s->native=NULL; SetWindowLongPtrW(window,GWLP_USERDATA,0);
        return DefWindowProcW(window,message,wp,lp);
    case WM_KILLFOCUS:
        memset(s->keys,0,sizeof s->keys); memset(s->pressed,0,sizeof s->pressed);
        memset(s->buttons,0,sizeof s->buttons); memset(s->clicked,0,sizeof s->clicked);
        if(GetCapture()==window) ReleaseCapture();
        return 0;
    case WM_CAPTURECHANGED:memset(s->buttons,0,sizeof s->buttons); return 0;
    case WM_SIZE: {
        RECT bounds;
        if(GetClientRect(window,&bounds)) { s->client_width=bounds.right; s->client_height=bounds.bottom; }
        return 0;
    }
    case WM_ERASEBKGND:return 1;
    case WM_PRINTCLIENT:
        if(s->pixels) (void)vx_win_blit(s,window,(HDC)wp);
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT paint; HDC dc=BeginPaint(window,&paint);
        if(dc&&s->pixels) (void)vx_win_blit(s,window,dc);
        EndPaint(window,&paint); return 0;
    }
    case WM_MOUSEMOVE:s->mouse_x=GET_X_LPARAM(lp); s->mouse_y=GET_Y_LPARAM(lp); return 0;
    case WM_MOUSEWHEEL: {
        int64_t delta=GET_WHEEL_DELTA_WPARAM(wp);
        if(delta>0&&s->wheel>INT32_MAX-delta) s->wheel=INT32_MAX;
        else if(delta<0&&s->wheel<INT32_MIN-delta) s->wheel=INT32_MIN;
        else s->wheel+=delta;
        return 0;
    }
    case WM_KEYDOWN:case WM_KEYUP:case WM_SYSKEYDOWN:case WM_SYSKEYUP: {
        int key=vx_win_key(wp); bool down=message==WM_KEYDOWN||message==WM_SYSKEYDOWN;
        if(key>=0) vx_gfx_button(s->keys,s->pressed,key,down);
        if(wp==VK_ESCAPE&&down) s->closed=true;
        if(message==WM_SYSKEYDOWN||message==WM_SYSKEYUP) return DefWindowProcW(window,message,wp,lp);
        return 0;
    }
    case WM_LBUTTONDOWN:case WM_LBUTTONUP:case WM_RBUTTONDOWN:case WM_RBUTTONUP:
    case WM_MBUTTONDOWN:case WM_MBUTTONUP: {
        int button=message==WM_LBUTTONDOWN||message==WM_LBUTTONUP?0:
                   message==WM_RBUTTONDOWN||message==WM_RBUTTONUP?1:2;
        bool down=message==WM_LBUTTONDOWN||message==WM_RBUTTONDOWN||message==WM_MBUTTONDOWN;
        s->mouse_x=GET_X_LPARAM(lp); s->mouse_y=GET_Y_LPARAM(lp);
        vx_gfx_button(s->buttons,s->clicked,button,down);
        if(down) SetCapture(window);
        else if(!s->buttons[0]&&!s->buttons[1]&&!s->buttons[2]&&GetCapture()==window) ReleaseCapture();
        return 0;
    }
    case WM_SYSCOMMAND:
        if((wp&0xfff0)==SC_KEYMENU) return 0;
        break;
    default:break;
    }
    return DefWindowProcW(window,message,wp,lp);
}
static int vx_platform_open(VxSurface *s,const uint64_t *title,size_t length,bool visible) {
    char utf8[1025]; WCHAR wide[1025];
    if(length>1024) return VX_GFX_INVALID_TEXT;
    for(size_t i=0;i<length;++i) { if(!title[i]||title[i]>255) return VX_GFX_INVALID_TEXT; utf8[i]=(char)title[i]; }
    int count=length?MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,utf8,(int)length,wide,1024):0;
    if(length&&!count) return VX_GFX_INVALID_TEXT;
    wide[count]=0;
    WNDCLASSW wc; memset(&wc,0,sizeof wc);
    wc.style=CS_HREDRAW|CS_VREDRAW; wc.lpfnWndProc=vx_win_proc;
    wc.hInstance=GetModuleHandleW(NULL); wc.hCursor=LoadCursorW(NULL,MAKEINTRESOURCEW(32512));
    wc.lpszClassName=L"VxnStdWindow";
    if(!RegisterClassW(&wc)) {
        WNDCLASSW existing;
        if(GetLastError()!=ERROR_CLASS_ALREADY_EXISTS||!GetClassInfoW(wc.hInstance,wc.lpszClassName,&existing)||
           existing.lpfnWndProc!=vx_win_proc) return VX_GFX_OPEN_FAILED;
    }
    RECT bounds={0,0,s->width,s->height}; DWORD style=WS_OVERLAPPEDWINDOW;
    if(!AdjustWindowRectEx(&bounds,style,FALSE,0)) return VX_GFX_OPEN_FAILED;
    HWND window=CreateWindowExW(0,wc.lpszClassName,wide,style,CW_USEDEFAULT,CW_USEDEFAULT,
        bounds.right-bounds.left,bounds.bottom-bounds.top,NULL,NULL,wc.hInstance,s);
    if(!window) return VX_GFX_OPEN_FAILED;
    s->native=window; s->window=true;
    (void)vx_platform_now(&s->last_poll);
    if(visible) ShowWindow(window,SW_SHOW);
    return -1;
}
static void vx_platform_poll(VxSurface *s) {
    if(!s->native) { s->closed=true; return; }
    MSG message;
    /* Only pump this window; do not consume an embedder's unrelated events.
     * Keep WM_QUIT available to its owner after reporting close to Vxn. */
    for(unsigned work=0;work<4096&&PeekMessageW(&message,(HWND)s->native,0,0,PM_REMOVE);++work) {
        if(message.message==WM_QUIT) { s->closed=true; PostQuitMessage((int)message.wParam); break; }
        TranslateMessage(&message); DispatchMessageW(&message);
        if(!s->native) break;
    }
}
static bool vx_platform_present(VxSurface *s) {
    if(!s->native) return false;
    HWND window=(HWND)s->native; HDC dc=GetDC(window); if(!dc) return false;
    bool ok=vx_win_blit(s,window,dc);
    if(!ReleaseDC(window,dc)) ok=false;
    return ok;
}
static bool vx_platform_close(VxSurface *s) {
    if(!s->native) return true;
    HWND window=(HWND)s->native;
    if(DestroyWindow(window)) return true;
    /* Never leave a callback pointing into a released interpreter context. */
    SetWindowLongPtrW(window,GWLP_USERDATA,0); s->native=NULL; return false;
}
static void vx_platform_finish(void) {
    (void)UnregisterClassW(L"VxnStdWindow",GetModuleHandleW(NULL));
}
#endif
