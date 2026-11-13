#include "internal.h"
const char *const vx_library_type_names[LT_COUNT]={
    "Color","WindowFrame","GfxResult","CanvasOpen","WindowOpen","FrameRead",
    "PixelRead","TimeRead","TimeResult","WindowMode"
};
#define OS_EFFECTS (EF_IO|EF_ALLOC|EF_BLOCK)
const LibraryCall vx_library_calls[VX_LIBRARY_COUNT]={
    {"window_open",4,{LA_U32,LA_U32,LT_WINDOW_MODE,LA_BYTES},LT_WINDOW_OPEN,OS_EFFECTS},
    {"window_canvas",1,{LA_WINDOW},LT_CANVAS_OPEN,EF_IO},
    {"window_poll",1,{LA_WINDOW},LT_FRAME_READ,OS_EFFECTS},
    {"window_present",1,{LA_WINDOW},LT_GFX_RESULT,OS_EFFECTS},
    {"window_close",1,{LA_WINDOW},LT_GFX_RESULT,OS_EFFECTS},
    {"canvas_create",2,{LA_U32,LA_U32},LT_CANVAS_OPEN,EF_IO|EF_ALLOC},
    {"canvas_close",1,{LA_CANVAS},LT_GFX_RESULT,EF_IO|EF_ALLOC},
    {"draw_clear",2,{LA_CANVAS,LT_COLOR},LT_GFX_RESULT,EF_IO},
    {"draw_pixel",4,{LA_CANVAS,LA_I32,LA_I32,LT_COLOR},LT_GFX_RESULT,EF_IO},
    {"draw_line",6,{LA_CANVAS,LA_I32,LA_I32,LA_I32,LA_I32,LT_COLOR},LT_GFX_RESULT,EF_IO},
    {"draw_rectangle",6,{LA_CANVAS,LA_I32,LA_I32,LA_U32,LA_U32,LT_COLOR},LT_GFX_RESULT,EF_IO},
    {"draw_circle",5,{LA_CANVAS,LA_I32,LA_I32,LA_U32,LT_COLOR},LT_GFX_RESULT,EF_IO},
    {"draw_text",6,{LA_CANVAS,LA_I32,LA_I32,LA_U32,LT_COLOR,LA_BYTES},LT_GFX_RESULT,EF_IO},
    {"canvas_read_pixel",3,{LA_CANVAS,LA_I32,LA_I32},LT_PIXEL_READ,EF_IO},
    {"time_now",0,{0},LT_TIME_READ,EF_IO},
    {"time_sleep",1,{LA_U32},LT_TIME_RESULT,EF_IO|EF_BLOCK},
    {"canvas_copy",4,{LA_CANVAS,LA_CANVAS,LA_I32,LA_I32},LT_GFX_RESULT,EF_IO}
};
const char *const vx_library_parameter_names[VX_LIBRARY_COUNT][6]={
    {"width","height","mode","title"}, {"window"}, {"window"}, {"window"}, {"window"},
    {"width","height"}, {"canvas"}, {"canvas","color"}, {"canvas","x","y","color"},
    {"canvas","x0","y0","x1","y1","color"}, {"canvas","x","y","width","height","color"},
    {"canvas","x","y","radius","color"}, {"canvas","x","y","scale","color","text"},
    {"canvas","x","y"}, {NULL}, {"milliseconds"}, {"destination","source","x","y"}
};
int vx_library_builtin(const VxnProgram *p,Token name) {
    for(int i=0;i<VX_LIBRARY_COUNT;++i) if(vx_word(p,name,vx_library_calls[i].name)) return i;
    return -1;
}
Type vx_library_type(const VxnProgram *p,int type) {
    if(type<LT_COUNT) return p->library_types[type];
    switch(type) {
    case LA_U32:return TY_WORD; case LA_I32:return TY_INT_BASE+6;
    case LA_CANVAS:return TY_CANVAS; case LA_WINDOW:return TY_WINDOW;
    default:return TY_INFER;
    }
}
