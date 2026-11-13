#ifndef VX_TEXT_H
#define VX_TEXT_H

static inline int vx_text_equal(const uint64_t *a,const uint64_t *b,uint64_t length) {
    for(uint64_t i=0;i<length;++i) if(a[i]!=b[i]) return 0;
    return 1;
}
static inline int vx_text_boundary(const uint64_t *a,uint64_t length,uint64_t offset) {
    return offset<=length&&(offset==length||(a[offset]&0xc0)!=0x80);
}
/* Exact UTF-8 byte queries. Search is allocation-free O(n*m), comparison and
   scalar counting are O(n). Empty needles match either end as appropriate. */
static inline void vx_text_query(int op,const uint64_t *args,uint64_t *result) {
    const uint64_t *a=(const uint64_t *)(uintptr_t)args[0]; uint64_t n=args[1];
    if(op==VX_STR_IS_BOUNDARY) { result[0]=(uint64_t)vx_text_boundary(a,n,args[2]); return; }
    if(op==VX_STR_SCALAR_LEN) {
        result[0]=0; for(uint64_t i=0;i<n;++i) if((a[i]&0xc0)!=0x80) ++result[0]; return;
    }
    const uint64_t *b=(const uint64_t *)(uintptr_t)args[2]; uint64_t m=args[3];
    if(op==VX_STR_COMPARE) {
        for(uint64_t i=0;i<n&&i<m;++i) if(a[i]!=b[i]) { result[0]=a[i]<b[i]?0:2; return; }
        result[0]=n<m?0:n>m?2:1; return;
    }
    if(op==VX_STR_EQUAL) { result[0]=(uint64_t)(n==m&&vx_text_equal(a,b,n)); return; }
    if(op==VX_STR_STARTS_WITH||op==VX_STR_ENDS_WITH) {
        result[0]=0; if(m<=n) result[0]=(uint64_t)(!m||vx_text_equal(a+(op==VX_STR_ENDS_WITH?n-m:0),b,m)); return;
    }
    uint64_t found=0; int present=0;
    if(!m) { present=1; found=op==VX_STR_RFIND?n:0; }
    else if(m<=n) {
        for(uint64_t k=0;k<=n-m;++k) {
            uint64_t i=op==VX_STR_RFIND?n-m-k:k;
            if(vx_text_equal(a+i,b,m)) { present=1; found=i; break; }
        }
    }
    if(op==VX_STR_CONTAINS) result[0]=(uint64_t)present;
    else { result[0]=present?0:1; result[1]=found; }
}

/* Preconditions: checked str inputs, a valid String header for mutations,
   and an exclusive target loan disjoint from any borrowed source. All lengths
   count byte cells. Fallible operations leave existing strings unchanged. */
static inline void vx_text_exec(VxHeap *h,int op,const uint64_t *args,uint64_t *result) {
    if(op==VX_STR_SLICE) {
        const uint64_t *bytes=(const uint64_t *)(uintptr_t)args[0];
        uint64_t length=args[1],start=args[2],count=args[3];
        result[0]=1; result[1]=result[2]=0;
        if(start>length||count>length-start) return;
        if(!vx_text_boundary(bytes,length,start)) { result[1]=1; result[2]=start; return; }
        if(!vx_text_boundary(bytes,length,start+count)) { result[1]=1; result[2]=start+count; return; }
        result[0]=0; result[1]=start?(uint64_t)(uintptr_t)(bytes+start):args[0]; result[2]=count; return;
    }
    if(op==VX_STR_FROM_UTF8) {
        vx_utf8_exec(0,(const uint64_t *)(uintptr_t)args[0],args[1],0,result);
        if(!result[0]) { result[1]=args[0]; result[2]=args[1]; }
        return;
    }
    if(op>=VX_STR_EQUAL&&op<=VX_STR_SCALAR_LEN) { vx_text_query(op,args,result); return; }
    if(op==VX_STRING_NEW||op==VX_STRING_FROM_STR) {
        result[0]=1; result[1]=0;
        const uint64_t empty[3]={0}; uint64_t address=vx_heap_new(h,empty,3);
        if(!address) return;
        uint64_t *v=(uint64_t *)(uintptr_t)address;
        if(op==VX_STRING_FROM_STR&&args[1]) {
            if(!vx_vec_reserve(h,v,args[1],1)) { vx_heap_destroy(h,vx_heap_node(address)); return; }
            memcpy((uint64_t *)(uintptr_t)v[0],(const uint64_t *)(uintptr_t)args[0],(size_t)args[1]*sizeof(uint64_t));
            v[1]=args[1];
        }
        result[0]=0; result[1]=address; return;
    }
    uint64_t *v=(uint64_t *)(uintptr_t)*(const uint64_t *)(uintptr_t)args[0];
    if(op==VX_STRING_CAPACITY) { result[0]=v[2]; return; }
    if(op==VX_STRING_CLEAR) { v[1]=0; return; }
    if(op==VX_STRING_TRUNCATE) {
        uint64_t offset=args[1]; result[0]=1; result[1]=0; result[2]=offset;
        if(offset>v[1]) return;
        if(!vx_text_boundary((const uint64_t *)(uintptr_t)v[0],v[1],offset)) { result[1]=1; return; }
        v[1]=offset; result[0]=result[1]=result[2]=0; return;
    }
    result[0]=result[1]=0;
    if(op==VX_STRING_RESERVE) { result[0]=!vx_vec_reserve(h,v,args[1],1); return; }
    const uint64_t *bytes=NULL; uint64_t count=0,encoded[4];
    if(op==VX_STRING_PUSH_SCALAR) {
        uint64_t scalar=args[1]; result[2]=0;
        if(scalar>0x10ffff||(scalar>=0xd800&&scalar<=0xdfff)) {
            result[0]=1; result[1]=0; result[2]=scalar; return;
        }
        if(scalar<0x80) { encoded[0]=scalar; count=1; }
        else if(scalar<0x800) { encoded[0]=0xc0|(scalar>>6); encoded[1]=0x80|(scalar&63); count=2; }
        else if(scalar<0x10000) {
            encoded[0]=0xe0|(scalar>>12); encoded[1]=0x80|((scalar>>6)&63); encoded[2]=0x80|(scalar&63); count=3;
        } else {
            encoded[0]=0xf0|(scalar>>18); encoded[1]=0x80|((scalar>>12)&63);
            encoded[2]=0x80|((scalar>>6)&63); encoded[3]=0x80|(scalar&63); count=4;
        }
        bytes=encoded;
    } else { bytes=(const uint64_t *)(uintptr_t)args[1]; count=args[2]; }
    if(!vx_vec_reserve(h,v,count,1)) {
        result[0]=1; if(op==VX_STRING_PUSH_SCALAR) result[1]=1; return;
    }
    if(count) memcpy((uint64_t *)(uintptr_t)v[0]+v[1],bytes,(size_t)count*sizeof(uint64_t));
    v[1]+=count;
}
#endif
