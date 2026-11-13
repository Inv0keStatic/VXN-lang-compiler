#include "../runtime/parse.h"

static void parse_runtime_tests(void) {
    bool ok=true; unsigned tested=0;
    for(int sign=0;sign<2;++sign) for(unsigned a=0;a<256;++a) for(unsigned b=0;b<256;++b) {
        uint64_t bytes[2]={a,b},result[3]; vx_parse_int(bytes,2,64,sign,result);
        bool first=a>='0'&&a<='9',second=b>='0'&&b<='9';
        bool prefix=a=='+'||(sign&&a=='-'),valid=second&&(first||prefix);
        uint64_t value=first?(a-'0')*10+(b-'0'):b-'0';
        if(a=='-') value=UINT64_C(0)-value;
        ok&=valid?result[0]==0&&result[1]==value&&result[2]==0:
            result[0]==1&&result[1]==1&&result[2]==(first||prefix?1u:0u);
        ++tested;
    }
    expect(ok,"exhaustive two-byte decimal syntax including NUL/non-ASCII");
    ok=true;
    for(unsigned n=0;n<10000;++n) for(int mode=0;mode<3;++mode) {
        char text[32]; snprintf(text,sizeof text,"%s%u",mode==2?"-":"",n);
        size_t length=strlen(text); uint64_t bytes[32],result[3];
        for(size_t i=0;i<length;++i) bytes[i]=(unsigned char)text[i];
        vx_parse_int(bytes,(uint64_t)length,8,mode!=0,result);
        unsigned limit=mode==0?255:mode==1?127:128;
        if(n<=limit) ok&=result[0]==0&&result[1]==((mode==2?UINT64_C(0)-n:n)&255)&&result[2]==0;
        else {
            size_t overflow=0;
            for(size_t end=1;end<=length;++end) {
                char prefix[32]; memcpy(prefix,text,end); prefix[end]=0;
                if(strtoull(prefix+(mode==2),NULL,10)>limit) { overflow=end-1; break; }
            }
            ok&=result[0]==1&&result[1]==2&&result[2]==overflow;
        }
        ++tested;
    }
    expect(ok,"eight-bit decimal bounds and earliest overflow against prefix oracle");
    uint64_t state=UINT64_C(0x725a981b03342fd1); ok=true;
    for(unsigned width=8;width<=64;width*=2) for(int sign=0;sign<2;++sign) for(int i=0;i<2048;++i) {
        state^=state<<13; state^=state>>7; state^=state<<17;
        uint64_t mask=width==64?UINT64_MAX:(UINT64_C(1)<<width)-1,bits=state&mask;
        bool negative=sign&&(bits&(UINT64_C(1)<<(width-1)));
        uint64_t magnitude=negative?((~bits)&mask)+1:bits;
        char text[40]; snprintf(text,sizeof text,"%s000%" PRIu64,negative?"-":i%2?"+":"",magnitude);
        size_t length=strlen(text); uint64_t bytes[40],result[3];
        for(size_t j=0;j<length;++j) bytes[j]=(unsigned char)text[j];
        vx_parse_int(bytes,(uint64_t)length,width,sign,result);
        ok&=result[0]==0&&result[1]==bits&&result[2]==0; ++tested;
    }
    expect(ok,"decimal roundtrip bit patterns at every integer width");
    uint64_t empty[3]; vx_parse_int(NULL,0,64,0,empty);
    expect(empty[0]==1&&empty[1]==0&&empty[2]==0,"empty decimal input never reads backing storage");
    printf("%u decimal parsing oracle cases\n",tested);
}
