#include <stdbool.h>
#include "../runtime/utf8.h"
#include "../runtime/text.h"

static void text_runtime_tests(void) {
    VxHeap h={0}; uint64_t args[4]={0},result[4]={0},map[]={1,0,0};
    vx_heap_enter(&h); fail=1;
    vx_text_exec(&h,VX_STRING_NEW,args,result);
    expect(result[0]==1&&!h.live,"String header failure is recoverable"); fail=0;
    vx_text_exec(&h,VX_STRING_NEW,args,result); uint64_t string=result[1];
    vx_heap_adopt(&h,&string,map,0,1); args[0]=(uint64_t)(uintptr_t)&string;
    uint64_t *header=(uint64_t *)(uintptr_t)string;
    const uint64_t prefix[]={65,0xc3,0xa9,0}; args[1]=(uint64_t)(uintptr_t)prefix; args[2]=4;
    fail=1; vx_text_exec(&h,VX_STRING_APPEND,args,result);
    expect(result[0]==1&&!header[0]&&!header[1]&&!header[2],"failed first append leaves empty String unchanged");
    fail=0; vx_text_exec(&h,VX_STRING_APPEND,args,result);
    expect(!result[0]&&header[1]==4&&!memcmp((void *)(uintptr_t)header[0],prefix,sizeof prefix),"append copies embedded NUL and UTF-8 exactly");
    uint64_t prior[3]; memcpy(prior,header,sizeof prior);
    fail=1; vx_text_exec(&h,VX_STRING_APPEND,args,result);
    expect(result[0]==1&&!memcmp(prior,header,sizeof prior)&&!memcmp((void *)(uintptr_t)header[0],prefix,sizeof prefix),"failed growing append preserves bytes pointer length and capacity");
    args[1]=0x1f30d; vx_text_exec(&h,VX_STRING_PUSH_SCALAR,args,result);
    expect(result[0]==1&&result[1]==1&&!memcmp(prior,header,sizeof prior),"scalar allocation error preserves existing String");
    args[1]=0xd800; vx_text_exec(&h,VX_STRING_PUSH_SCALAR,args,result);
    expect(result[0]==1&&!result[1]&&result[2]==0xd800&&!memcmp(prior,header,sizeof prior),"invalid scalar takes precedence over unavailable allocation");
    size_t before=allocations; args[1]=UINT32_MAX; vx_text_exec(&h,VX_STRING_RESERVE,args,result);
    expect(result[0]==1&&!memcmp(prior,header,sizeof prior)&&before==allocations,"String length overflow does not allocate or mutate");
    vx_text_exec(&h,VX_STRING_CLEAR,args,result);
    expect(!header[1]&&header[0]==prior[0]&&header[2]==prior[2],"String clear retains buffer without allocation");
    args[1]=0x1f30d; vx_text_exec(&h,VX_STRING_PUSH_SCALAR,args,result);
    uint64_t decoded[3]; vx_utf8_exec(1,(const uint64_t *)(uintptr_t)header[0],header[1],0,decoded);
    expect(!result[0]&&!decoded[0]&&decoded[1]==0x1f30d&&decoded[2]==4,"reserved capacity allows scalar append when allocator fails");
    vx_text_exec(&h,VX_STRING_CLEAR,args,result);
    args[1]=0; args[2]=0; vx_text_exec(&h,VX_STRING_APPEND,args,result);
    expect(!result[0]&&!header[1],"zero-length append does not dereference its source");
    fail=0;
    /* Independent decoding oracle for every Unicode scalar, plus invalid
       surrogate values. Four reserved cells suffice; this loop never grows. */
    bool correct=true; unsigned checked=0;
    for(uint64_t scalar=0;scalar<=0x10ffff;++scalar) {
        vx_text_exec(&h,VX_STRING_CLEAR,args,result); args[1]=scalar;
        vx_text_exec(&h,VX_STRING_PUSH_SCALAR,args,result); ++checked;
        if(scalar>=0xd800&&scalar<=0xdfff) {
            if(result[0]!=1||result[1]!=0||result[2]!=scalar||header[1]) correct=false;
        } else {
            vx_utf8_exec(1,(const uint64_t *)(uintptr_t)header[0],header[1],0,decoded);
            if(result[0]||decoded[0]||decoded[1]!=scalar||decoded[2]!=header[1]) correct=false;
        }
    }
    expect(correct,"every Unicode scalar encodes canonically and surrogates fail atomically");
    printf("%u scalar-encoding oracle cases\n",checked);
    args[1]=UINT32_MAX; memcpy(prior,header,sizeof prior); vx_text_exec(&h,VX_STRING_PUSH_SCALAR,args,result);
    expect(result[0]==1&&!result[1]&&result[2]==UINT32_MAX&&!memcmp(prior,header,sizeof prior),"out-of-range scalar preserves String");
    fail=1; before=allocations; args[1]=1; memcpy(prior,header,sizeof prior);
    vx_text_exec(&h,VX_STRING_TRUNCATE,args,result);
    expect(result[0]==1&&result[1]==1&&result[2]==1&&!memcmp(prior,header,sizeof prior),"truncate rejects continuation byte atomically");
    args[1]=UINT32_MAX; vx_text_exec(&h,VX_STRING_TRUNCATE,args,result);
    expect(result[0]==1&&!result[1]&&result[2]==UINT32_MAX&&!memcmp(prior,header,sizeof prior),"truncate rejects excessive offsets atomically");
    args[1]=4; vx_text_exec(&h,VX_STRING_TRUNCATE,args,result);
    expect(!result[0]&&!memcmp(prior,header,sizeof prior),"truncate at end keeps text unchanged");
    args[1]=0; vx_text_exec(&h,VX_STRING_TRUNCATE,args,result);
    expect(!result[0]&&!header[1]&&header[0]==prior[0]&&header[2]==prior[2]&&before==allocations,"truncate succeeds without allocator access and retains capacity");
    /* Exhaust all short strings over ASCII including embedded NUL against a
       separate straightforward first/last-match oracle. */
    correct=true; unsigned searches=0; uint64_t hay[6],needle[4];
    for(unsigned n=0;n<=6;++n) for(unsigned bits=0;bits<(1u<<n);++bits) {
        for(unsigned i=0;i<n;++i) hay[i]=(bits>>i)&1u;
        for(unsigned m=0;m<=4;++m) for(unsigned sub=0;sub<(1u<<m);++sub) {
            for(unsigned i=0;i<m;++i) needle[i]=(sub>>i)&1u;
            int first=-1,last=-1;
            if(m<=n) for(unsigned i=0;i<=n-m;++i) if(!memcmp(hay+i,needle,m*sizeof(uint64_t))) { if(first<0) first=(int)i; last=(int)i; }
            args[0]=(uint64_t)(uintptr_t)hay; args[1]=n; args[2]=(uint64_t)(uintptr_t)needle; args[3]=m;
            vx_text_exec(&h,VX_STR_FIND,args,result);
            if(result[0]!=(uint64_t)(first<0)||(first>=0&&result[1]!=(uint64_t)first)) correct=false;
            vx_text_exec(&h,VX_STR_RFIND,args,result);
            if(result[0]!=(uint64_t)(last<0)||(last>=0&&result[1]!=(uint64_t)last)) correct=false;
            vx_text_exec(&h,VX_STR_CONTAINS,args,result); if(result[0]!=(uint64_t)(first>=0)) correct=false;
            vx_text_exec(&h,VX_STR_STARTS_WITH,args,result); if(result[0]!=(uint64_t)(first==0)) correct=false;
            vx_text_exec(&h,VX_STR_ENDS_WITH,args,result); if(result[0]!=(uint64_t)(last>=0&&(unsigned)last+m==n)) correct=false;
            ++searches;
        }
    }
    expect(correct&&before==allocations,"search queries agree with independent oracle without allocating");
    printf("%u text-search oracle cases\n",searches);
    const uint64_t utf8[]={0x61,0xc3,0xa9,0xe2,0x82,0xac,0xf0,0x9f,0x98,0x80,0,0x62};
    const unsigned boundaries[]={0,1,3,6,10,11,12}; correct=true;
    for(unsigned start=0;start<=13;++start) for(unsigned count=0;count<=13;++count) {
        bool first=false,last=false;
        for(unsigned i=0;i<sizeof boundaries/sizeof *boundaries;++i) {
            first|=start==boundaries[i]; last|=start+count==boundaries[i];
        }
        args[0]=(uint64_t)(uintptr_t)utf8; args[1]=12; args[2]=start; args[3]=count;
        vx_text_exec(&h,VX_STR_SLICE,args,result);
        if(start>12||count>12-start) correct&=result[0]==1&&result[1]==0;
        else if(!first||!last) correct&=result[0]==1&&result[1]==1&&result[2]==(first?start+count:start);
        else correct&=!result[0]&&result[1]==(uint64_t)(uintptr_t)(utf8+start)&&result[2]==count;
    }
    expect(correct&&before==allocations,"all UTF-8 substring endpoints preserve boundaries and origins without allocation");
    args[0]=(uint64_t)(uintptr_t)utf8; args[1]=12;
    vx_text_exec(&h,VX_STR_FROM_UTF8,args,result);
    expect(!result[0]&&result[1]==args[0]&&result[2]==12,"validated text borrows original byte storage");
    args[0]=args[1]=args[2]=args[3]=0; vx_text_exec(&h,VX_STR_SLICE,args,result);
    expect(!result[0]&&!result[1]&&!result[2],"empty substring avoids null-pointer arithmetic");
    vx_text_exec(&h,VX_STR_FROM_UTF8,args,result);
    expect(!result[0]&&!result[1]&&!result[2]&&before==allocations,"empty UTF-8 view requires no allocation");
    fail=0;
    vx_heap_leave(&h); expect(!h.live&&allocations==frees,"String builder cleanup frees every allocation");
}
