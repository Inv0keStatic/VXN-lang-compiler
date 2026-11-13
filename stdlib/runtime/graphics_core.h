#ifndef VX_GRAPHICS_CORE_H
#define VX_GRAPHICS_CORE_H
#ifndef VX_LIBRARY_API_H
#include "library_api.h"
#endif
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint32_t *pixels; int width,height;
    uint64_t generation;
    bool window,closed;
    void *native;
    int client_width,client_height,mouse_x,mouse_y;
    unsigned char keys[VX_GFX_KEYS],pressed[VX_GFX_KEYS];
    unsigned char buttons[VX_GFX_BUTTONS],clicked[VX_GFX_BUTTONS];
    int64_t wheel;
    double last_poll;
} VxSurface;
typedef struct {
    VxSurface surfaces[VX_GFX_MAX_SURFACES];
    size_t pixels;
} VxLibrary;

static uint32_t vx_gfx_rgb(const uint64_t *color) {
    return ((uint32_t)color[0]<<16)|((uint32_t)color[1]<<8)|(uint32_t)color[2];
}
static uint32_t vx_gfx_blend(uint32_t destination,uint32_t source,unsigned alpha) {
    if(alpha==255) return source;
    unsigned inverse=255-alpha;
    unsigned r=(((source>>16)&255)*alpha+((destination>>16)&255)*inverse+127)/255;
    unsigned g=(((source>>8)&255)*alpha+((destination>>8)&255)*inverse+127)/255;
    unsigned b=((source&255)*alpha+(destination&255)*inverse+127)/255;
    return (r<<16)|(g<<8)|b;
}
static void vx_gfx_pixel(VxSurface *s,int x,int y,uint32_t color,unsigned alpha) {
    if(x<0||y<0||x>=s->width||y>=s->height) return;
    size_t at=(size_t)y*(size_t)s->width+(size_t)x;
    s->pixels[at]=vx_gfx_blend(s->pixels[at],color,alpha);
}
static void vx_gfx_clear(VxSurface *s,uint32_t color) {
    size_t size=(size_t)s->width*(size_t)s->height;
    for(size_t i=0;i<size;++i) s->pixels[i]=color;
}
/* Opaque RGB copy, clipped before accessing either image. Row direction plus
 * memmove preserve the original source even for overlapping self-copies. */
static void vx_gfx_copy(VxSurface *destination,const VxSurface *source,int64_t x,int64_t y) {
    int64_t sx=x<0?-x:0,sy=y<0?-y:0,dx=x>0?x:0,dy=y>0?y:0;
    int64_t width=source->width-sx,height=source->height-sy;
    if(width>destination->width-dx) width=destination->width-dx;
    if(height>destination->height-dy) height=destination->height-dy;
    if(width<=0||height<=0) return;
    bool reverse=source==destination&&dy>sy;
    for(int64_t row=0;row<height;++row) {
        int64_t at=reverse?height-1-row:row;
        memmove(destination->pixels+(size_t)(dy+at)*(size_t)destination->width+(size_t)dx,
                source->pixels+(size_t)(sy+at)*(size_t)source->width+(size_t)sx,
                (size_t)width*sizeof *source->pixels);
    }
}
static void vx_gfx_rect(VxSurface *s,int64_t x,int64_t y,uint64_t width,uint64_t height,
                        uint32_t color,unsigned alpha) {
    int64_t right=x+(int64_t)width,bottom=y+(int64_t)height;
    if(x<0) x=0;
    if(y<0) y=0;
    if(right>s->width) right=s->width;
    if(bottom>s->height) bottom=s->height;
    for(int64_t row=y;row<bottom;++row)
        for(int64_t col=x;col<right;++col) vx_gfx_pixel(s,(int)col,(int)row,color,alpha);
}
/* Clip before rasterizing, so even extreme source coordinates do bounded work. */
static bool vx_gfx_clip(double p,double q,double *start,double *end) {
    if(p==0) return q>=0;
    double ratio=q/p;
    if(p<0) { if(ratio>*end) return false; if(ratio>*start) *start=ratio; }
    else { if(ratio<*start) return false; if(ratio<*end) *end=ratio; }
    return true;
}
static int vx_gfx_coordinate(double value,int limit) {
    if(value<=0) return 0;
    if(value>=limit-1) return limit-1;
    return (int)(value+0.5);
}
static void vx_gfx_line(VxSurface *s,int x0,int y0,int x1,int y1,uint32_t color,unsigned alpha) {
    double x=x0,y=y0,dx=(double)x1-x0,dy=(double)y1-y0,start=0,end=1;
    if(!vx_gfx_clip(-dx,x,&start,&end)||!vx_gfx_clip(dx,s->width-1-x,&start,&end)||
       !vx_gfx_clip(-dy,y,&start,&end)||!vx_gfx_clip(dy,s->height-1-y,&start,&end)) return;
    x0=vx_gfx_coordinate(x+start*dx,s->width); y0=vx_gfx_coordinate(y+start*dy,s->height);
    x1=vx_gfx_coordinate(x+end*dx,s->width); y1=vx_gfx_coordinate(y+end*dy,s->height);
    int ax=abs(x1-x0),ay=-abs(y1-y0),sx=x0<x1?1:-1,sy=y0<y1?1:-1,error=ax+ay;
    for(;;) {
        vx_gfx_pixel(s,x0,y0,color,alpha);
        if(x0==x1&&y0==y1) break;
        int twice=2*error;
        if(twice>=ay) { error+=ay; x0+=sx; }
        if(twice<=ax) { error+=ax; y0+=sy; }
    }
}
static void vx_gfx_circle(VxSurface *s,int64_t x,int64_t y,unsigned radius,uint32_t color,unsigned alpha) {
    int64_t left=x-radius,right=x+radius,top=y-radius,bottom=y+radius;
    if(left<0) left=0;
    if(top<0) top=0;
    if(right>=s->width) right=s->width-1;
    if(bottom>=s->height) bottom=s->height-1;
    int64_t squared=(int64_t)radius*radius;
    for(int64_t row=top;row<=bottom;++row) for(int64_t col=left;col<=right;++col) {
        int64_t dx=col-x,dy=row-y;
        if(dx*dx+dy*dy<=squared) vx_gfx_pixel(s,(int)col,(int)row,color,alpha);
    }
}
/* Small original 5x7 block alphabet. Lowercase uses the uppercase glyph.
 * Rows are five-bit masks; other printable punctuation gets a box glyph. */
static const unsigned char vx_gfx_font[36][7]={
    {14,17,17,31,17,17,17},{30,17,17,30,17,17,30},{14,17,16,16,16,17,14},
    {30,17,17,17,17,17,30},{31,16,16,30,16,16,31},{31,16,16,30,16,16,16},
    {14,17,16,23,17,17,15},{17,17,17,31,17,17,17},{31,4,4,4,4,4,31},
    {7,2,2,2,18,18,12},{17,18,20,24,20,18,17},{16,16,16,16,16,16,31},
    {17,27,21,21,17,17,17},{17,25,25,21,19,19,17},{14,17,17,17,17,17,14},
    {30,17,17,30,16,16,16},{14,17,17,17,21,18,13},{30,17,17,30,20,18,17},
    {15,16,16,14,1,1,30},{31,4,4,4,4,4,4},{17,17,17,17,17,17,14},
    {17,17,17,17,17,10,4},{17,17,17,21,21,21,10},{17,17,10,4,10,17,17},
    {17,17,10,4,4,4,4},{31,1,2,4,8,16,31},
    {14,17,19,21,25,17,14},{4,12,4,4,4,4,14},{14,17,1,2,4,8,31},
    {30,1,1,14,1,1,30},{2,6,10,18,31,2,2},{31,16,16,30,1,1,30},
    {14,16,16,30,17,17,14},{31,1,2,4,8,8,8},{14,17,17,14,17,17,14},
    {14,17,17,15,1,1,14}
};
static unsigned vx_gfx_glyph(unsigned ch,unsigned row) {
    if(ch>='a'&&ch<='z') ch-=32;
    if(ch>='A'&&ch<='Z') return vx_gfx_font[ch-'A'][row];
    if(ch>='0'&&ch<='9') return vx_gfx_font[26+ch-'0'][row];
    if(ch==' ') return 0;
    if(ch=='.') return row==6?4:0;
    if(ch==':') return row==2||row==5?4:0;
    if(ch=='-') return row==3?14:0;
    if(ch=='/') return 1u<<(row*4/6);
    return row==0||row==6?31:17;
}
static void vx_gfx_text(VxSurface *s,int64_t x,int64_t y,unsigned scale,const uint64_t *bytes,
                       size_t length,uint32_t color,unsigned alpha) {
    int64_t initial=x;
    for(size_t i=0;i<length;++i) {
        unsigned ch=(unsigned)bytes[i];
        if(ch=='\n') { x=initial; y+=8*scale; continue; }
        for(unsigned row=0;row<7;++row) {
            unsigned bits=vx_gfx_glyph(ch,row);
            for(unsigned col=0;col<5;++col) if(bits&(16u>>col))
                vx_gfx_rect(s,x+col*scale,y+row*scale,scale,scale,color,alpha);
        }
        x+=6*scale;
    }
}
static void vx_gfx_button(unsigned char *down,unsigned char *pressed,int button,bool state) {
    if(state&&!down[button]) pressed[button]=1;
    down[button]=(unsigned char)state;
}
#endif
