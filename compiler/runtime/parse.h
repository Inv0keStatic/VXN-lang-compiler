#ifndef VX_PARSE_H
#define VX_PARSE_H
#include <stdint.h>

/* Input is a bounded array of Vxn byte cells, never a NUL-terminated C string.
   Result cells: Ok(value) = {0,value,0}; Err(kind,index) = {1,kind,index}.
   Error tags match stdlib/parse.vxn: Empty, InvalidDigit, Overflow. */
static void vx_parse_int(const uint64_t *bytes,uint64_t length,unsigned width,int sign,uint64_t result[3]) {
    result[0]=1; result[1]=0; result[2]=0;
    if(!length) return;
    uint64_t index=0; int negative=0;
    if(bytes[0]=='+') index=1;
    else if(sign&&bytes[0]=='-') { index=1; negative=1; }
    if(index==length) return;
    uint64_t mask=width==64?UINT64_MAX:(UINT64_C(1)<<width)-1;
    uint64_t limit=sign?(mask>>1)+(unsigned)negative:mask,value=0;
    uint64_t quotient=limit/10,remainder=limit%10;
    for(;index<length;++index) {
        uint64_t byte=bytes[index];
        if(byte<'0'||byte>'9') { result[1]=1; result[2]=index; return; }
        uint64_t digit=byte-'0';
        if(value>quotient||(value==quotient&&digit>remainder)) {
            result[1]=2; result[2]=index; return;
        }
        value=value*10+digit;
    }
    result[0]=0; result[1]=(negative?UINT64_C(0)-value:value)&mask;
}

/* Exact decimal comparison against binary32/64 rounding midpoints.
   No host floating arithmetic, locale, heap allocation or truncated input.
   A midpoint is n * 2^e with n < 2^54 and -1075 <= e <= 970.
   For e < 0 its decimal integer is n * 5^-e with scale e; otherwise
   it is n * 2^e. Starting with at most 17 digits and adding at most one
   digit per multiplication bounds storage by 1092 digits (122 base-1e9
   limbs). The 128 limbs / 1152 digits below have headroom for both formats. */
typedef struct {
    const uint64_t *bytes;
    uint64_t first, significant;
    int64_t order;
} VxDecimal;

static int vx_decimal_midpoint(const VxDecimal *input,uint64_t bits,unsigned width) {
    unsigned fraction=width==32?23:52;
    unsigned exponent=(unsigned)(bits>>fraction);
    uint64_t mantissa=bits&((UINT64_C(1)<<fraction)-1);
    if(exponent) mantissa|=UINT64_C(1)<<fraction;
    uint64_t n=mantissa*2+1;
    int power=(int)(exponent?exponent:1)-(width==32?127:1023)-(int)fraction-1;
    int scale=power<0?power:0;
    uint32_t limbs[128]; unsigned used=0;
    do { limbs[used++]=(uint32_t)(n%1000000000); n/=1000000000; } while(n);
    unsigned multiplier=power<0?5:2,steps=(unsigned)(power<0?-power:power);
    for(unsigned step=0;step<steps;++step) {
        uint64_t carry=0;
        for(unsigned i=0;i<used;++i) {
            uint64_t v=(uint64_t)limbs[i]*multiplier+carry;
            limbs[i]=(uint32_t)(v%1000000000); carry=v/1000000000;
        }
        if(carry) limbs[used++]=(uint32_t)carry;
    }
    unsigned char digits[1152]; unsigned count=0;
    uint32_t top=limbs[used-1],divisor=1;
    while(top/divisor>=10) divisor*=10;
    do { digits[count++]=(unsigned char)(top/divisor); top%=divisor; divisor/=10; } while(divisor);
    for(unsigned i=used-1;i>0;--i) {
        uint32_t v=limbs[i-1];
        for(divisor=100000000;divisor;divisor/=10) {
            digits[count++]=(unsigned char)(v/divisor); v%=divisor;
        }
    }
    int64_t order=(int64_t)count+scale-1;
    if(input->order!=order) return input->order<order?-1:1;
    uint64_t cursor=input->first;
    for(unsigned i=0;i<count;++i) {
        unsigned digit=0;
        if(i<input->significant) {
            if(input->bytes[cursor]=='.') ++cursor;
            digit=(unsigned)(input->bytes[cursor++]-'0');
        }
        if(digit!=digits[i]) return digit<digits[i]?-1:1;
    }
    /* Any remaining nonzero input digit makes the exact value larger. */
    return input->significant>count?1:0;
}

static void vx_parse_float(const uint64_t *bytes,uint64_t length,unsigned width,uint64_t result[3]) {
    result[0]=1; result[1]=0; result[2]=0;
    if(!length) return;
    uint64_t cursor=0,negative=0;
    if(bytes[0]=='+'||bytes[0]=='-') { negative=bytes[0]=='-'; ++cursor; }
    if(cursor==length) return;
    uint64_t start=cursor,digits=0,fraction=0,first_digit=0,last_nonzero=0;
    VxDecimal input={bytes,0,0,0}; int point=0;
    while(cursor<length) {
        uint64_t byte=bytes[cursor];
        if(byte>='0'&&byte<='9') {
            ++digits; fraction+=(unsigned)point;
            if(byte!='0') {
                if(!first_digit) { first_digit=digits; input.first=cursor; }
                last_nonzero=digits;
            }
        } else if(byte=='.'&&!point) point=1;
        else break;
        ++cursor;
    }
    result[1]=1;
    if(!digits) { result[2]=start; return; }
    int64_t exponent=0; int exponent_negative=0;
    if(cursor<length&&(bytes[cursor]=='e'||bytes[cursor]=='E')) {
        ++cursor;
        if(cursor<length&&(bytes[cursor]=='+'||bytes[cursor]=='-')) {
            exponent_negative=bytes[cursor]=='-'; ++cursor;
        }
        if(cursor==length) { result[2]=cursor; return; }
        for(;cursor<length;++cursor) {
            uint64_t byte=bytes[cursor];
            if(byte<'0'||byte>'9') { result[2]=cursor; return; }
            /* Input length is at most UINT32_MAX; this saturation is far
               beyond any cancellation by mantissa/fraction digit counts. */
            if(exponent<INT64_C(1099511627776)) {
                exponent=exponent*10+(int64_t)(byte-'0');
                if(exponent>INT64_C(1099511627776)) exponent=INT64_C(1099511627776);
            }
        }
    }
    if(cursor!=length) { result[2]=cursor; return; }
    uint64_t sign=negative<<(width-1),maximum=width==32?UINT64_C(0x7f7fffff):UINT64_C(0x7fefffffffffffff);
    if(!first_digit) { result[0]=0; result[1]=sign; return; }
    input.significant=last_nonzero-first_digit+1;
    input.order=(exponent_negative?-exponent:exponent)-(int64_t)fraction+(int64_t)(digits-first_digit);
    uint64_t low=0,high=maximum+1;
    if(input.order<(width==32?-46:-324)) high=0;
    else if(input.order>(width==32?38:308)) low=high;
    while(low<high) {
        uint64_t middle=low+(high-low)/2;
        int compare=vx_decimal_midpoint(&input,middle,width);
        if(compare<0||(compare==0&&!(middle&1))) high=middle;
        else low=middle+1;
    }
    if(low>maximum) { result[1]=2; result[2]=length; return; }
    result[0]=0; result[1]=low|sign; result[2]=0;
}
#endif
