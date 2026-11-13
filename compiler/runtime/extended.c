#include "platform.h"
#include "softfloat.h"
#include "extended.h"
#include "scalar.h"
#include <stdio.h>

static float128_t quad(const uint64_t *v) {
    float128_t r;
#ifdef LITTLEENDIAN
    r.v[0]=v[0]; r.v[1]=v[1];
#else
    r.v[1]=v[0]; r.v[0]=v[1];
#endif
    return r;
}
static void put_quad(float128_t v,uint64_t *out) {
#ifdef LITTLEENDIAN
    out[0]=v.v[0]; out[1]=v.v[1];
#else
    out[0]=v.v[1]; out[1]=v.v[0];
#endif
}
static extFloat80_t extended(const uint64_t *v) {
    extFloat80_t r; r.signif=v[0]; r.signExp=(uint16_t)v[1]; return r;
}
typedef struct { uint_fast8_t rounding,precision,flags,tininess; } State;
static State enter(void) {
    State s={softfloat_roundingMode,extF80_roundingPrecision,softfloat_exceptionFlags,softfloat_detectTininess};
    softfloat_roundingMode=softfloat_round_near_even; extF80_roundingPrecision=80;
    softfloat_exceptionFlags=0; softfloat_detectTininess=softfloat_tininess_afterRounding; return s;
}
static void leave(State s) {
    softfloat_roundingMode=s.rounding; extF80_roundingPrecision=s.precision;
    softfloat_exceptionFlags=s.flags; softfloat_detectTininess=s.tininess;
}
void vxn_ext_op(uint64_t width,uint64_t op,const uint64_t *aa,const uint64_t *bb,uint64_t *out) {
    State s=enter(); uint64_t bits[2]={aa[0],aa[1]}; bool truth=false;
    if(op==VX_NEG) { bits[1]^=width==80?UINT64_C(0x8000):UINT64_C(0x8000000000000000); }
    else if(op!=VX_POS&&width==80) {
        extFloat80_t a=extended(aa),b=extended(bb),r=a;
        switch(op) {
        case VX_ADD:r=extF80_add(a,b);break; case VX_SUB:r=extF80_sub(a,b);break;
        case VX_MUL:r=extF80_mul(a,b);break; case VX_DIV:r=extF80_div(a,b);break;
        case VX_EQ:truth=extF80_eq(a,b);break; case VX_NE:truth=!extF80_eq(a,b);break;
        case VX_LT:truth=extF80_lt_quiet(a,b);break; case VX_LE:truth=extF80_le_quiet(a,b);break;
        case VX_GT:truth=extF80_lt_quiet(b,a);break; case VX_GE:truth=extF80_le_quiet(b,a);break;
        }
        bits[0]=r.signif; bits[1]=r.signExp;
    } else if(op!=VX_POS) {
        float128_t a=quad(aa),b=quad(bb),r=a;
        switch(op) {
        case VX_ADD:r=f128_add(a,b);break; case VX_SUB:r=f128_sub(a,b);break;
        case VX_MUL:r=f128_mul(a,b);break; case VX_DIV:r=f128_div(a,b);break;
        case VX_EQ:truth=f128_eq(a,b);break; case VX_NE:truth=!f128_eq(a,b);break;
        case VX_LT:truth=f128_lt_quiet(a,b);break; case VX_LE:truth=f128_le_quiet(a,b);break;
        case VX_GT:truth=f128_lt_quiet(b,a);break; case VX_GE:truth=f128_le_quiet(b,a);break;
        }
        put_quad(r,bits);
    }
    if(op>=VX_EQ&&op<=VX_GE) out[0]=truth;
    else { out[0]=bits[0]; out[1]=bits[1]; }
    leave(s);
}
void vxn_ext_convert(const uint64_t *a,uint64_t fw,uint64_t fk,uint64_t tw,uint64_t tk,uint64_t *out) {
    State s=enter(); float128_t v;
    if(fk==2) {
        if(fw==128) v=quad(a);
        else if(fw==80) v=extF80_to_f128(extended(a));
        else if(fw==32) v=f32_to_f128((float32_t){(uint32_t)*a});
        else v=f64_to_f128((float64_t){*a});
    } else v=fk==1?i64_to_f128(vx_signed_value(*a,(unsigned)fw)):ui64_to_f128(*a);
    if(tk==2) {
        if(tw==128) put_quad(v,out);
        else if(tw==80) { extFloat80_t e=f128_to_extF80(v); out[0]=e.signif; out[1]=e.signExp; }
        else if(tw==32) out[0]=f128_to_f32(v).v;
        else out[0]=f128_to_f64(v).v;
    } else {
        /* Endpoint comparisons implement VXN's truncate/saturate/NaN->0
           contract independently of SoftFloat's invalid-conversion result. */
        uint64_t bits[2]; put_quad(v,bits);
        bool nan=(bits[1]&UINT64_C(0x7fff000000000000))==UINT64_C(0x7fff000000000000)&&
            ((bits[1]&UINT64_C(0xffffffffffff))||bits[0]);
        uint64_t top=UINT64_C(1)<<(tw-1),mask=vx_mask((unsigned)tw);
        if(nan) out[0]=0;
        else if(tk==1) {
            int64_t minimum=tw==64?INT64_MIN:-(int64_t)top;
            if(f128_le(v,i64_to_f128(minimum))) out[0]=top;
            else if(f128_le(ui64_to_f128(top-1),v)) out[0]=top-1;
            else out[0]=(uint64_t)f128_to_i64_r_minMag(v,false)&mask;
        } else if(f128_le(v,ui64_to_f128(0))) out[0]=0;
        else if(f128_le(ui64_to_f128(mask),v)) out[0]=mask;
        else out[0]=f128_to_ui64_r_minMag(v,false);
    }
    leave(s);
}

/* Decimal conversion uses bounded integer arithmetic, with no host floating
   arithmetic or heap allocation. A binary128 midpoint has fewer than 11585
   significant decimal digits (114 binary bits, power >= -16495). Retaining
   11600 digits and a nonzero-tail flag therefore preserves its side of every
   rounding boundary. Both formats have decimal orders in [-4966,4932]. */
#define DIGITS 11600
#define LIMBS 2048
typedef struct { uint32_t v[LIMBS]; unsigned n; } Big;
static void normalize(Big *a) { while(a->n&&!a->v[a->n-1]) --a->n; }
static void small(Big *a,uint32_t n) { a->n=n?1:0; a->v[0]=n; }
static void multiply(Big *a,uint32_t factor,uint32_t add) {
    uint64_t carry=add;
    for(unsigned i=0;i<a->n;++i) { uint64_t v=(uint64_t)a->v[i]*factor+carry; a->v[i]=(uint32_t)v; carry=v>>32; }
    if(carry) a->v[a->n++]=(uint32_t)carry;
}
static unsigned bits(const Big *a) {
    if(!a->n) return 0;
    unsigned count=(a->n-1)*32; uint32_t top=a->v[a->n-1];
    while(top) { ++count; top>>=1; } return count;
}
static void shift(Big *a,unsigned count) {
    if(!a->n||!count) return;
    unsigned words=count/32,part=count%32,old=a->n; uint32_t carry=0;
    for(unsigned i=0;i<old;++i) {
        uint32_t v=a->v[i]; a->v[i]=(v<<part)|carry; carry=part?v>>(32-part):0;
    }
    if(carry) a->v[a->n++]=carry;
    if(words) { memmove(a->v+words,a->v,a->n*sizeof *a->v); memset(a->v,0,words*sizeof *a->v); a->n+=words; }
}
static void halve(Big *a) {
    uint32_t carry=0;
    for(unsigned i=a->n;i>0;--i) { uint32_t v=a->v[i-1]; a->v[i-1]=(v>>1)|carry; carry=v<<31; }
    normalize(a);
}
static int compare(const Big *a,const Big *b) {
    if(a->n!=b->n) return a->n<b->n?-1:1;
    for(unsigned i=a->n;i>0;--i) if(a->v[i-1]!=b->v[i-1]) return a->v[i-1]<b->v[i-1]?-1:1;
    return 0;
}
static void subtract(Big *a,const Big *b) {
    uint64_t borrow=0;
    for(unsigned i=0;i<a->n;++i) {
        uint64_t sub=(i<b->n?b->v[i]:0)+borrow,v=a->v[i]; a->v[i]=(uint32_t)(v-sub); borrow=v<sub;
    }
    normalize(a);
}
static uint32_t divide_small(Big *a,uint32_t d) {
    uint64_t remainder=0;
    for(unsigned i=a->n;i>0;--i) { uint64_t v=(remainder<<32)|a->v[i-1]; a->v[i-1]=(uint32_t)(v/d); remainder=v%d; }
    normalize(a); return (uint32_t)remainder;
}
void vxn_ext_parse(const uint64_t *bytes,uint64_t length,uint64_t width,uint64_t out[3]) {
    out[0]=1; out[1]=0; out[2]=0;
    if(!length) return;
    uint64_t cursor=0,negative=0;
    if(bytes[0]=='+'||bytes[0]=='-') { negative=bytes[0]=='-'; ++cursor; }
    if(cursor==length) return;
    uint64_t start=cursor,digits=0,fraction=0,first=0,first_at=0,last=0; bool point=false;
    while(cursor<length) {
        uint64_t byte=bytes[cursor];
        if(byte>='0'&&byte<='9') {
            ++digits; fraction+=point;
            if(byte!='0') { if(!first) { first=digits; first_at=cursor; } last=digits; }
        } else if(byte=='.'&&!point) point=true;
        else break;
        ++cursor;
    }
    out[1]=1;
    if(!digits) { out[2]=start; return; }
    int64_t exponent=0; bool exponent_negative=false;
    if(cursor<length&&(bytes[cursor]=='e'||bytes[cursor]=='E')) {
        ++cursor;
        if(cursor<length&&(bytes[cursor]=='+'||bytes[cursor]=='-')) { exponent_negative=bytes[cursor]=='-'; ++cursor; }
        if(cursor==length) { out[2]=cursor; return; }
        for(;cursor<length;++cursor) {
            uint64_t byte=bytes[cursor];
            if(byte<'0'||byte>'9') { out[2]=cursor; return; }
            if(exponent<INT64_C(1099511627776)) {
                exponent=exponent*10+(int64_t)(byte-'0');
                if(exponent>INT64_C(1099511627776)) exponent=INT64_C(1099511627776);
            }
        }
    }
    if(cursor!=length) { out[2]=cursor; return; }
    uint64_t sign=negative<<(width==80?15:width==32?31:63);
    int64_t order=(exponent_negative?-exponent:exponent)-(int64_t)fraction+(int64_t)(digits-first);
    if(!first||order < -4966) { out[0]=0; out[1]=width<=64?sign:0; out[2]=width<=64?0:sign; return; }
    if(order>4932) { out[1]=2; out[2]=length; return; }
    uint64_t significant=last-first+1,keep=significant<DIGITS?significant:DIGITS;
    Big numerator,denominator,trial; small(&numerator,0); small(&denominator,1);
    cursor=first_at;
    for(uint64_t i=0;i<keep;++i) {
        if(bytes[cursor]=='.') ++cursor;
        multiply(&numerator,10,(uint32_t)(bytes[cursor++]-'0'));
    }
    int power=(int)(order-(int64_t)keep+1);
    for(int i=0;i<(power<0?-power:power);++i) multiply(power<0?&denominator:&numerator,5,0);
    int binary=(int)bits(&numerator)-(int)bits(&denominator);
    if(binary>=0) { trial=denominator; shift(&trial,(unsigned)binary); if(compare(&numerator,&trial)<0) --binary; }
    else { trial=numerator; shift(&trial,(unsigned)-binary); if(compare(&trial,&denominator)<0) --binary; }
    binary+=power;
    unsigned precision=width==32?24:width==64?53:width==80?64:113;
    int bias=width==32?127:width==64?1023:16383,minimum=1-bias;
    int quantum=(binary < minimum?minimum:binary)-(int)precision+1;
    int scaling=power-quantum;
    if(scaling>=0) shift(&numerator,(unsigned)scaling); else shift(&denominator,(unsigned)-scaling);
    uint64_t low=0,high=0; trial=denominator; shift(&trial,precision);
    for(int i=(int)precision;i>=0;--i) {
        if(compare(&numerator,&trial)>=0) {
            subtract(&numerator,&trial);
            if(i<64) low|=UINT64_C(1)<<i; else high|=UINT64_C(1)<<(i-64);
        }
        halve(&trial);
    }
    shift(&numerator,1); int cmp=compare(&numerator,&denominator);
    if(cmp>0||(cmp==0&&((low&1)||significant>keep))) { if(!++low) ++high; }
    if(precision<64?(low>>precision)!=0:precision==64?high!=0:(high&(UINT64_C(1)<<49))!=0) {
        low=(low>>1)|(high<<63); high>>=1; ++quantum;
    }
    unsigned encoded=(precision<=64?(low>>(precision-1)):(high>>48))?(unsigned)(quantum+(int)precision-1+bias):0;
    if(encoded>=(unsigned)(bias*2+1)) { out[1]=2; out[2]=length; return; }
    out[0]=0; out[1]=low;
    if(width<=64) { out[1]=sign|((uint64_t)encoded<<(precision-1))|(low&((UINT64_C(1)<<(precision-1))-1)); out[2]=0; return; }
    out[2]=sign|(width==80?encoded:((uint64_t)encoded<<48)|(high&UINT64_C(0xffffffffffff)));
}
int vxn_ext_format(const uint64_t *value,uint64_t width,char *out) {
    bool negative=(value[1]>>(width==80?15:63))!=0;
    unsigned exponent=(unsigned)(width==80?value[1]&32767:(value[1]>>48)&32767);
    uint64_t low=value[0],high=width==80?0:value[1]&UINT64_C(0xffffffffffff);
    if(exponent==32767) {
        bool nan=width==80?(low&UINT64_C(0x7fffffffffffffff))!=0:high||low;
        return sprintf(out,nan?"nan":negative?"-inf":"inf");
    }
    if(width==128&&exponent) high|=UINT64_C(1)<<48;
    if(!low&&!high) return sprintf(out,negative?"-0":"0");
    Big number; number.n=4;
    number.v[0]=(uint32_t)low; number.v[1]=(uint32_t)(low>>32);
    number.v[2]=(uint32_t)high; number.v[3]=(uint32_t)(high>>32); normalize(&number);
    int power=(int)(exponent?exponent:1)-16383-(width==80?63:112),scale=power<0?power:0;
    if(power>=0) shift(&number,(unsigned)power);
    else for(int i=0;i<-power;++i) multiply(&number,5,0);
    char reversed[DIGITS+1]; unsigned count=0;
    while(number.n) reversed[count++]=(char)('0'+divide_small(&number,10));
    int order=(int)count+scale-1; unsigned precision=width==80?21:36;
    char digits[37]; unsigned n=count<precision?count:precision;
    for(unsigned i=0;i<n;++i) digits[i]=reversed[count-1-i];
    if(count>n) {
        unsigned next=(unsigned)(reversed[count-1-n]-'0'); bool rest=false;
        for(unsigned i=0;i<count-1-n;++i) rest|=reversed[i]!='0';
        if(next>5||(next==5&&(rest||((digits[n-1]-'0')&1)))) {
            unsigned i=n; while(i&&digits[i-1]=='9') digits[--i]='0';
            if(i) ++digits[i-1]; else { digits[0]='1'; ++order; }
        }
    }
    while(n>1&&digits[n-1]=='0') --n;
    char *p=out; if(negative) *p++='-';
    if(order < -4||order >= (int)precision) {
        *p++=digits[0]; if(n>1) { *p++='.'; memcpy(p,digits+1,n-1); p+=n-1; }
        p+=sprintf(p,"e%+03d",order);
    } else if(order<0) {
        *p++='0'; *p++='.'; for(int i=-1;i>order;--i) *p++='0'; memcpy(p,digits,n); p+=n;
    } else {
        for(int i=0;i<=order;++i) *p++=i<(int)n?digits[i]:'0';
        if(n>(unsigned)order+1) { *p++='.'; unsigned rest=n-(unsigned)order-1; memcpy(p,digits+order+1,rest); p+=rest; }
    }
    *p=0; return (int)(p-out);
}
