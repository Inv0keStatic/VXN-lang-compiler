#ifndef VX_UTF8_H
#define VX_UTF8_H
#include <stdint.h>

/* Unicode 16.0, section 3.9.3, Tables 3-6 and 3-7. Input uses byte cells.
   Utf8Read: Scalar(value,next)={0,value,next}; End={1,0,0};
   Err(InvalidByte/Truncated/OutOfBounds(offset))={2,kind,offset}. */
static void vx_utf8_next(const uint64_t *bytes,uint64_t length,uint64_t offset,uint64_t result[3]) {
    result[0]=2; result[1]=0; result[2]=offset;
    if(offset>length) { result[1]=2; return; }
    if(offset==length) { result[0]=1; result[2]=0; return; }
    uint64_t lead=bytes[offset],value;
    unsigned count,low=0x80,high=0xbf;
    if(lead<=0x7f) { result[0]=0; result[1]=lead; result[2]=offset+1; return; }
    if(lead>=0xc2&&lead<=0xdf) { count=2; value=lead&0x1f; }
    else if(lead>=0xe0&&lead<=0xef) {
        count=3; value=lead&0x0f;
        if(lead==0xe0) low=0xa0;
        if(lead==0xed) high=0x9f;
    } else if(lead>=0xf0&&lead<=0xf4) {
        count=4; value=lead&7;
        if(lead==0xf0) low=0x90;
        if(lead==0xf4) high=0x8f;
    } else return;
    for(unsigned i=1;i<count;++i) {
        if(i>=length-offset) { result[1]=1; return; }
        uint64_t byte=bytes[offset+i];
        if(byte<(i==1?low:0x80)||byte>(i==1?high:0xbf)) { result[2]=offset+i; return; }
        value=(value<<6)|(byte&0x3f);
    }
    result[0]=0; result[1]=value; result[2]=offset+count;
}
static void vx_utf8_exec(int next,const uint64_t *bytes,uint64_t length,uint64_t offset,uint64_t result[3]) {
    if(next) { vx_utf8_next(bytes,length,offset,result); return; }
    uint64_t position=0,count=0,decoded[3];
    while(position<length) {
        vx_utf8_next(bytes,length,position,decoded);
        if(decoded[0]==2) { result[0]=1; result[1]=decoded[1]; result[2]=decoded[2]; return; }
        position=decoded[2]; ++count;
    }
    result[0]=0; result[1]=count; result[2]=0;
}
/* Vec/String share a header; validation moves ownership into Ok or Err without copying. */
static inline void vx_string_from_utf8(uint64_t address,uint64_t result[4]) {
    const uint64_t *vector=(const uint64_t *)(uintptr_t)address;
    uint64_t checked[3];
    vx_utf8_exec(0,(const uint64_t *)(uintptr_t)vector[0],vector[1],0,checked);
    result[0]=checked[0]; result[1]=result[2]=result[3]=0;
    if(!checked[0]) result[1]=address;
    else { result[1]=checked[1]; result[2]=checked[2]; result[3]=address; }
}
#endif
