#include <fenv.h>
#include <math.h>
#include "float_vectors.h"

static bool float_parse_bits(const char *text,unsigned width,uint64_t expected) {
    uint64_t bytes[2200],result[3]; size_t length=strlen(text);
    if(length>sizeof bytes/sizeof bytes[0]) return false;
    for(size_t i=0;i<length;++i) bytes[i]=(unsigned char)text[i];
    vx_parse_float(bytes,length,width,result);
    uint64_t infinity=width==32?UINT64_C(0x7f800000):UINT64_C(0x7ff0000000000000);
    uint64_t magnitude=expected&~(UINT64_C(1)<<(width-1));
    bool ok=magnitude==infinity?result[0]==1&&result[1]==2&&result[2]==length:
        !result[0]&&result[1]==expected&&!result[2];
    if(!ok) fprintf(stderr,"float%u parse mismatch: %.60s got %llu/%llx/%llu expected %llx\n",width,text,
        (unsigned long long)result[0],(unsigned long long)result[1],(unsigned long long)result[2],(unsigned long long)expected);
    return ok;
}

static void float_runtime_tests(void) {
    bool ok=true; unsigned tested=0;
    for(size_t i=0;i<sizeof float_vectors/sizeof float_vectors[0];++i) {
        ok&=float_parse_bits(float_vectors[i].text,float_vectors[i].width,float_vectors[i].bits); ++tested;
    }
    expect(ok,"exact rational midpoint vectors, both neighbors, subnormal and overflow boundaries");
    int saved=fegetround(); int modes[]={FE_TONEAREST,FE_DOWNWARD,FE_UPWARD,FE_TOWARDZERO};
    ok=true;
    for(size_t i=0;i<sizeof modes/sizeof modes[0];++i) {
        if(fesetround(modes[i])) { ok=false; continue; }
        ok&=float_parse_bits("0.1",32,UINT64_C(0x3dcccccd));
        ok&=float_parse_bits("0.1",64,UINT64_C(0x3fb999999999999a));
        ok&=float_parse_bits("1.0000000596046447753906250000001",32,UINT64_C(0x3f800001));
        ok&=float_parse_bits("-1e-999999999999999999999",64,UINT64_C(0x8000000000000000));
        tested+=4;
    }
    expect(ok,"decimal rounding independent of host rounding mode, including double-rounding adversary");
    fesetround(FE_TONEAREST); ok=true;
    uint64_t state=UINT64_C(0x7be31a55f2c9d601);
    for(unsigned width=32;width<=64;width*=2) for(unsigned i=0;i<512;++i) {
        state^=state<<13; state^=state>>7; state^=state<<17;
        char text[96]; int exponent=(int)(state%750)-380;
        snprintf(text,sizeof text,"%s%" PRIu64 ".%09uE%+d",i&1?"-":"+",state,(unsigned)(state>>32)%1000000000,exponent);
        uint64_t bits=0;
        if(width==32) { float value=strtof(text,NULL); uint32_t word; memcpy(&word,&value,sizeof word); bits=word; }
        else { double value=strtod(text,NULL); memcpy(&bits,&value,sizeof bits); }
        ok&=float_parse_bits(text,width,bits); ++tested;
    }
    expect(ok,"random decimal exponents against independent host strtof/strtod nearest oracle");
    if(saved!=-1) fesetround(saved);
    /* A decisive digit beyond the runtime's entire bounded midpoint buffer. */
    char long_text[2100]; const char *mid="1.00000000000000011102230246251565404236316680908203125";
    size_t prefix=strlen(mid); memcpy(long_text,mid,prefix); memset(long_text+prefix,'0',2000-prefix);
    long_text[2000]=0;
    expect(float_parse_bits(long_text,64,UINT64_C(0x3ff0000000000000)),"long exact midpoint with trailing zeros rounds even");
    long_text[1999]='1';
    expect(float_parse_bits(long_text,64,UINT64_C(0x3ff0000000000001)),"late nonzero decimal tail is not discarded");
    long_text[0]='1'; memset(long_text+1,'0',2000); memcpy(long_text+2001,"e-2000",7);
    expect(float_parse_bits(long_text,64,UINT64_C(0x3ff0000000000000)),"large decimal exponent cancels long mantissa exactly");
    printf("%u floating parsing oracle cases\n",tested+3);
}
