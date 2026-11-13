#include "../runtime/utf8.h"

static void utf8_runtime_tests(void) {
    bool ok=true; unsigned tested=0;
    for(unsigned a=0;a<256;++a) for(unsigned b=0;b<256;++b) {
        uint64_t bytes[2]={a,b},result[3]; vx_utf8_exec(0,bytes,2,0,result);
        bool ascii=a<128&&b<128,two=a>=0xc2&&a<=0xdf&&b>=0x80&&b<=0xbf;
        ok&=(result[0]==0)==(ascii||two);
        if(ascii||two) ok&=result[1]==(ascii?2u:1u)&&result[2]==0;
        vx_utf8_next(bytes,2,0,result);
        if(a<128) ok&=result[0]==0&&result[1]==a&&result[2]==1;
        else if(two) ok&=result[0]==0&&result[1]==(a-0xc0)*64+(b-0x80)&&result[2]==2;
        else ok&=result[0]==2;
        ++tested;
    }
    expect(ok,"exhaustive two-byte UTF-8 validity and first scalar"); ok=true;
    for(uint32_t scalar=0;scalar<=UINT32_C(0x10ffff);++scalar) {
        if(scalar>=0xd800&&scalar<=0xdfff) continue;
        uint64_t bytes[4],result[3]; unsigned length;
        /* Independent arithmetic encoder, including unassigned/noncharacter values. */
        if(scalar<128) { length=1; bytes[0]=scalar; }
        else if(scalar<2048) { length=2; bytes[0]=192+scalar/64; bytes[1]=128+scalar%64; }
        else if(scalar<65536) {
            length=3; bytes[0]=224+scalar/4096; bytes[1]=128+(scalar/64)%64; bytes[2]=128+scalar%64;
        } else {
            length=4; bytes[0]=240+scalar/262144; bytes[1]=128+(scalar/4096)%64;
            bytes[2]=128+(scalar/64)%64; bytes[3]=128+scalar%64;
        }
        vx_utf8_next(bytes,length,0,result);
        ok&=result[0]==0&&result[1]==scalar&&result[2]==length;
        vx_utf8_exec(0,bytes,length,0,result);
        ok&=result[0]==0&&result[1]==1&&result[2]==0; ++tested;
        for(unsigned cut=1;cut<length;++cut) {
            vx_utf8_next(bytes,cut,0,result);
            ok&=result[0]==2&&result[1]==1&&result[2]==0; ++tested;
        }
        for(unsigned i=1;i<length;++i) {
            uint64_t previous=bytes[i]; bytes[i]=0;
            vx_utf8_next(bytes,length,0,result);
            ok&=result[0]==2&&result[1]==0&&result[2]==i;
            bytes[i]=previous; ++tested;
        }
    }
    expect(ok,"every Unicode scalar roundtrips; truncated and damaged continuations fail");
    uint64_t result[3]; vx_utf8_exec(0,NULL,0,0,result);
    expect(result[0]==0&&result[1]==0&&result[2]==0,"empty UTF-8 validation does not read storage");
    vx_utf8_next(NULL,0,0,result);
    expect(result[0]==1&&result[1]==0&&result[2]==0,"empty UTF-8 decoding returns End");
    vx_utf8_next(NULL,0,UINT32_MAX,result);
    expect(result[0]==2&&result[1]==2&&result[2]==UINT32_MAX,"UTF-8 offset failure precedes memory access");
    printf("%u UTF-8 scalar and byte-pattern oracle cases\n",tested);
    uint64_t data[2]={0xc3,0xa9},vector[3]={(uint64_t)(uintptr_t)data,2,2},converted[4];
    uint64_t address=(uint64_t)(uintptr_t)vector;
    vx_string_from_utf8(address,converted);
    expect(converted[0]==0&&converted[1]==address&&converted[2]==0&&converted[3]==0,"UTF-8 owner conversion preserves successful owner address");
    data[1]=0;
    vx_string_from_utf8(address,converted);
    expect(converted[0]==1&&converted[1]==0&&converted[2]==1&&converted[3]==address,"UTF-8 owner conversion preserves failed owner address and error");
}
