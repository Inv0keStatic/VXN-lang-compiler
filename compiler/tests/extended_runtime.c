#include "../runtime/extended.h"
#include "../runtime/scalar.h"
#include <stdio.h>
#include <stdlib.h>
#include "extended_vectors.h"
static unsigned checks,failures;
static void expect(int yes,const char *label) { ++checks; if(!yes) { ++failures; fprintf(stderr,"FAIL %s\n",label); } }
static void parse(const char *s,unsigned width,uint64_t *out) {
    uint64_t bytes[20000]; size_t n=strlen(s);
    if(n>sizeof bytes/sizeof *bytes) abort();
    for(size_t i=0;i<n;++i) bytes[i]=(unsigned char)s[i];
    vxn_ext_parse(bytes,n,width,out);
}
int main(void) {
    for(size_t i=0;i<sizeof extended_vectors/sizeof *extended_vectors;++i) {
        uint64_t actual[3]; char text[20000]; size_t length=0;
        for(size_t j=0;extended_vectors[i].chunks[j];++j) {
            size_t n=strlen(extended_vectors[i].chunks[j]);
            if(n>=sizeof text-length) abort();
            memcpy(text+length,extended_vectors[i].chunks[j],n); length+=n;
        }
        text[length]=0; parse(text,extended_vectors[i].width,actual);
        expect(!memcmp(actual,extended_vectors[i].expected,sizeof actual),text);
    }
    for(size_t i=0;i<sizeof extended_operations/sizeof *extended_operations;++i) {
        uint64_t actual[2];
        vxn_ext_op(extended_operations[i].width,extended_operations[i].op,
            extended_operations[i].a,extended_operations[i].b,actual);
        expect(!memcmp(actual,extended_operations[i].result,sizeof actual),"rational arithmetic oracle");
    }
    uint64_t seed=42;
    for(unsigned width=80;width<=128;width+=48) {
        uint64_t one[3],two[3],three[3],a[2],b[2];
        parse("1",width,one); parse("2",width,two); parse("3",width,three);
        vxn_ext_op(width,VX_ADD,one+1,two+1,a); expect(!memcmp(a,three+1,sizeof a),"add");
        vxn_ext_op(width,VX_DIV,one+1,three+1,a); vxn_ext_op(width,VX_MUL,a,three+1,b);
        expect(!memcmp(b,one+1,sizeof b),"one third times three rounds to one");
        for(unsigned i=0;i<400;++i) {
            uint64_t cells[2],result[3]; char text[64];
            seed=seed*UINT64_C(6364136223846793005)+1; cells[0]=seed;
            seed=seed*UINT64_C(6364136223846793005)+1; cells[1]=seed;
            if(width==80) { cells[1]&=65535; if((cells[1]&32767)==32767) --cells[1]; if(cells[1]&32767) cells[0]|=UINT64_C(1)<<63; else cells[0]&=UINT64_MAX>>1; }
            else if(((cells[1]>>48)&32767)==32767) cells[1]-=UINT64_C(1)<<48;
            vxn_ext_format(cells,width,text); parse(text,width,result);
            expect(!result[0]&&!memcmp(cells,result+1,sizeof cells),text);
        }
    }
    printf("%u extended runtime checks, %u failures\n",checks,failures); return failures?1:0;
}
