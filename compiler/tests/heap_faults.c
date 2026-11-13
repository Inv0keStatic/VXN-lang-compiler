#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
static size_t allocations,frees;
static int fail;
static void *allocate(size_t bytes) { if(fail) return NULL; void *p=malloc(bytes); if(p) ++allocations; return p; }
static void release(void *p) { if(p) ++frees; free(p); }
#define VX_HEAP_ALLOC allocate
#define VX_HEAP_FREE release
#include "../runtime/heap.h"
static unsigned checks,failures;
static void expect(int ok,const char *name) { ++checks; if(!ok) { ++failures; fprintf(stderr,"FAIL: %s\n",name); } }
#include "text_runtime.h"
#include "vector_insert_runtime.h"
#include "line_runtime.h"
#include "memory_runtime.h"
int main(void) {
    VxHeap h={0}; uint64_t value[]={42,7},box_map[]={1,0,0};
    vx_heap_enter(&h);
    fail=1; expect(vx_heap_new(&h,value,2)==0,"allocation failure is recoverable");
    expect(!h.live&&!allocations,"failed allocation creates no owner"); fail=0;
    uint64_t b=vx_heap_new(&h,value,2); expect(b!=0,"box allocates");
    expect(((uint64_t *)(uintptr_t)b)[0]==42&&((uint64_t *)(uintptr_t)b)[1]==7,"payload copied");
    vx_heap_adopt(&h,&b,box_map,0,1); vx_heap_drop(&h,0,0); expect(h.live==1,"binding survives temporary cleanup");
    vx_heap_enter(&h); vx_heap_adopt(&h,&b,box_map,0,1); expect(h.frames[1]==NULL,"parameter transfer detaches caller owner");
    vx_heap_adopt(&h,&b,box_map,1,0); vx_heap_leave(&h); expect(h.live==1,"returned owner survives callee cleanup");
    vx_heap_drop(&h,0,0); expect(!h.live&&frees==1,"discarded return drops once");
    for(int i=0;i<100;++i) {
        b=vx_heap_new(&h,value,2); vx_heap_adopt(&h,&b,box_map,0,3); vx_heap_drop(&h,3,0);
    }
    expect(h.live==0&&allocations==frees,"repeated scope cleanup has no accumulation");
    b=vx_heap_new(&h,value,2); uint64_t choice[]={1,b},choice_map[]={2,0,0,2,1,1,2,0,1,2,1,1,0};
    vx_heap_adopt(&h,choice,choice_map,0,7); vx_heap_drop(&h,0,0); expect(h.live==1,"selected choice payload is traced once");
    uint64_t inactive[]={0,UINT64_C(0xdeadbeef)},inactive_map[]={2,0,1,2,1,1,0};
    vx_heap_adopt(&h,inactive,inactive_map,0,9); expect(h.live==1,"inactive choice padding is not traced");
    vx_heap_drop(&h,7,0); expect(!h.live,"selected payload cleanup");
    b=vx_heap_new(&h,value,2); vx_heap_adopt(&h,&b,box_map,0,1);
    uint64_t c=vx_heap_new(&h,value,2); vx_heap_adopt(&h,&c,box_map,0,2);
    vx_heap_drop(&h,1,0); expect(h.live==1&&((uint64_t *)(uintptr_t)c)[0]==42,"drop preserves other bindings");
    vx_heap_leave(&h); expect(h.depth==0&&!h.live&&allocations==frees,"normal frame cleanup");
    vx_heap_enter(&h); (void)vx_heap_new(&h,value,2); vx_heap_enter(&h); (void)vx_heap_new(&h,value,2);
    vx_heap_finish(&h); expect(!h.depth&&!h.live&&allocations==frees,"interpreter teardown releases host storage");
    vx_heap_enter(&h);
    b=vx_heap_box(&h,value,2,NULL);
    uint64_t outer=vx_heap_box(&h,&b,1,box_map);
    expect(outer&&h.live==2&&vx_heap_node(outer)->children==vx_heap_node(b),"nested allocation attaches payload ownership");
    vx_heap_adopt(&h,&outer,box_map,0,1); vx_heap_drop(&h,0,0);
    expect(h.live==2&&((uint64_t *)(uintptr_t)b)[0]==42,"nested payload survives temporary cleanup");
    vx_heap_enter(&h); vx_heap_adopt(&h,&outer,box_map,0,1);
    vx_heap_adopt(&h,&outer,box_map,1,0); vx_heap_leave(&h);
    expect(h.live==2,"moving outer owner transfers children without freeing them");
    vx_heap_drop(&h,0,0); expect(!h.live&&allocations==frees,"nested drop frees every node exactly once");
    b=vx_heap_box(&h,value,2,NULL); fail=1;
    expect(!vx_heap_box(&h,&b,1,box_map)&&h.live==1,"failed nesting leaves input in temporary ownership");
    fail=0; vx_heap_drop(&h,0,0); expect(!h.live&&allocations==frees,"failed nesting input is cleaned up");
    b=vx_heap_box(&h,value,2,NULL);
    for(unsigned i=0;i<10000&&b;++i) b=vx_heap_box(&h,&b,1,box_map);
    expect(b&&h.live==10001,"deep ownership chain constructed");
    vx_heap_leave(&h); expect(!h.live&&allocations==frees,"deep ownership cleanup uses no recursive C stack");
    vx_heap_enter(&h);
    uint64_t args[4]={0},result[4]={0}; fail=1;
    vx_vec_exec(&h,VX_VEC_NEW,args,1,NULL,result);
    expect(result[0]==1&&result[1]==0&&!h.live,"vector construction failure returns allocation error");
    fail=0; vx_vec_exec(&h,VX_VEC_NEW,args,1,NULL,result); uint64_t vector=result[1];
    vx_heap_adopt(&h,&vector,box_map,0,1); args[0]=(uint64_t)(uintptr_t)&vector;
    vx_vec_exec(&h,VX_VEC_SLICE,args,1,NULL,result);
    expect(result[0]!=0&&!result[1],"empty vector view has a valid one-past pointer");
    args[1]=7; fail=1; vx_vec_exec(&h,VX_VEC_RESERVE,args,1,NULL,result);
    uint64_t *header=(uint64_t *)(uintptr_t)vector;
    expect(result[0]==1&&!header[0]&&!header[1]&&!header[2]&&h.live==1,"failed initial reserve leaves header unchanged");
    fail=0;
    for(unsigned i=0;i<1500;++i) { args[1]=i; vx_vec_exec(&h,VX_VEC_PUSH,args,1,NULL,result); }
    expect(result[0]==0&&header[1]==1500&&header[2]>=1500&&h.live==2,"vector grows beyond fixed-array limit");
    int contents_ok=1;
    for(unsigned i=0;i<1500;++i) if(((uint64_t *)(uintptr_t)header[0])[i]!=i) contents_ok=0;
    expect(contents_ok,"repeated vector growth preserves all initialized elements");
    uint64_t old_buffer=header[0],old_capacity=header[2]; fail=1; args[1]=3000;
    vx_vec_exec(&h,VX_VEC_RESERVE,args,1,NULL,result);
    expect(result[0]==1&&header[0]==old_buffer&&header[1]==1500&&header[2]==old_capacity,"failed growth preserves data length and capacity");
    fail=0; args[1]=UINT32_MAX;
    vx_vec_exec(&h,VX_VEC_RESERVE,args,1,NULL,result);
    expect(result[0]==1&&header[0]==old_buffer,"capacity overflow fails without truncation");
    vx_vec_exec(&h,VX_VEC_CLEAR,args,1,NULL,result);
    expect(!header[1]&&header[2]==old_capacity&&header[0]==old_buffer,"clear retains storage capacity");
    vx_heap_drop(&h,1,0); expect(!h.live&&allocations==frees,"scalar vector cleanup frees header and buffer");

    vx_vec_exec(&h,VX_VEC_NEW,args,1,NULL,result); vector=result[1]; vx_heap_adopt(&h,&vector,box_map,0,1);
    args[0]=(uint64_t)(uintptr_t)&vector;
    for(unsigned i=0;i<4;++i) { args[1]=vx_heap_box(&h,value,2,NULL); vx_vec_exec(&h,VX_VEC_PUSH,args,1,box_map,result); }
    expect(h.live==6,"vector owns four boxed elements and two storage allocations");
    args[1]=vx_heap_box(&h,value,2,NULL); fail=1;
    vx_vec_exec(&h,VX_VEC_PUSH,args,1,box_map,result);
    expect(result[0]==1&&h.live==7&&((uint64_t *)(uintptr_t)vector)[1]==4,"failed owned push keeps incoming value temporary and vector unchanged");
    fail=0; vx_heap_drop(&h,0,0); expect(h.live==6,"discarded input after failed push is released once");
    args[1]=vx_heap_box(&h,value,2,NULL);
    vx_vec_exec(&h,VX_VEC_PUSH,args,1,box_map,result);
    expect(result[0]==0&&h.live==7,"owned growth reparents existing and incoming children");
    vx_vec_exec(&h,VX_VEC_POP,args,1,box_map,result); b=result[1];
    vx_heap_adopt(&h,&b,box_map,0,2);
    vx_vec_exec(&h,VX_VEC_CLEAR,args,1,box_map,result);
    expect(h.live==3&&((uint64_t *)(uintptr_t)b)[0]==42,"popped owner survives clearing remaining elements");
    vx_heap_drop(&h,1,0);
    expect(h.live==1&&((uint64_t *)(uintptr_t)b)[0]==42,"popped owner survives dropping vector storage");
    vx_heap_leave(&h); expect(!h.live&&allocations==frees,"owned vector allocations freed exactly once");
    vx_heap_enter(&h);
    uint64_t pair_map[]={1,1,1,2,0},pair[3],removed[3];
    uint64_t many_args[4],many_result[4];
    vx_vec_exec(&h,VX_VEC_NEW,many_args,3,pair_map,many_result);
    vector=many_result[1]; vx_heap_adopt(&h,&vector,box_map,0,1);
    many_args[0]=(uint64_t)(uintptr_t)&vector;
    for(unsigned i=0;i<6;++i) {
        many_args[1]=i;
        many_args[2]=vx_heap_box(&h,value,2,NULL);
        many_args[3]=vx_heap_box(&h,value,2,NULL);
        vx_vec_exec(&h,VX_VEC_PUSH,many_args,3,pair_map,many_result);
    }
    header=(uint64_t *)(uintptr_t)vector; old_buffer=header[0]; old_capacity=header[2];
    expect(h.live==14&&header[1]==6,"multi-owner vector initializes all payloads");
    fail=1; size_t before_allocations=allocations;
    many_args[1]=2; vx_vec_exec(&h,VX_VEC_REMOVE,many_args,3,pair_map,many_result);
    memcpy(removed,many_result+1,sizeof removed); vx_heap_adopt(&h,removed,pair_map,0,2);
    uint64_t *data=(uint64_t *)(uintptr_t)header[0];
    expect(many_result[0]==0&&removed[0]==2&&header[1]==5&&data[6]==3&&data[12]==5,"ordered remove preserves the remaining order and stride");
    many_args[1]=0; vx_vec_exec(&h,VX_VEC_SWAP_REMOVE,many_args,3,pair_map,many_result);
    expect(many_result[0]==0&&many_result[1]==0&&header[1]==4&&data[0]==5,"swap removal replaces the hole with the last element");
    vx_heap_drop(&h,0,0); expect(h.live==12,"discarded removal drops only the returned owners");
    many_args[1]=UINT32_MAX; vx_vec_exec(&h,VX_VEC_REMOVE,many_args,3,pair_map,many_result);
    expect(many_result[0]==1&&header[1]==4&&h.live==12,"out-of-range removal is non-failing and leaves storage unchanged");
    vx_vec_exec(&h,VX_VEC_TRUNCATE,many_args,3,pair_map,many_result);
    expect(header[1]==4&&h.live==12,"truncating above length is a no-op");
    many_args[1]=1; vx_vec_exec(&h,VX_VEC_TRUNCATE,many_args,3,pair_map,many_result);
    expect(header[1]==1&&h.live==6&&data[0]==5,"truncation destroys exactly the removed tail owners");
    expect(header[0]==old_buffer&&header[2]==old_capacity,"removal and truncation retain capacity");
    expect(((uint64_t *)(uintptr_t)removed[1])[0]==42&&((uint64_t *)(uintptr_t)removed[2])[1]==7,"removed aggregate survives vector truncation");
    many_args[1]=0; vx_vec_exec(&h,VX_VEC_REMOVE,many_args,3,pair_map,many_result);
    vx_heap_drop(&h,0,0); expect(!header[1]&&h.live==4,"last removal does not trace stale spare capacity");
    vx_vec_exec(&h,VX_VEC_SWAP_REMOVE,many_args,3,pair_map,many_result);
    expect(many_result[0]==1&&!header[1],"empty swap removal returns Empty");
    expect(allocations==before_allocations,"removal and truncation never allocate");
    fail=0; pair[0]=42; pair[1]=vx_heap_box(&h,value,2,NULL); pair[2]=vx_heap_box(&h,value,2,NULL);
    outer=vx_heap_box(&h,pair,3,pair_map); size_t before_live=h.live;
    fail=1; before_allocations=allocations;
    vx_heap_unbox(&h,outer,3,pair_map,pair);
    expect(h.live==before_live-1&&pair[0]==42,"unboxing releases only the containing allocation");
    expect(allocations==before_allocations&&((uint64_t *)(uintptr_t)pair[1])[0]==42,"unboxing transfers nested owners without allocating");
    vx_heap_drop(&h,0,0); expect(h.live==4,"discarded unboxed payload drops children exactly once");
    fail=0; vx_heap_leave(&h);
    expect(!h.live&&allocations==frees,"mixed remove truncate and unbox cleanup balances every allocation");
    vx_heap_enter(&h);
    uint64_t storage[]={0,0},region[4],owner_choice_map[]={2,0,1,2,1,1,0};
    vx_heap_region_bind(&h,region,storage,2,1);
    vx_heap_enter(&h);
    uint64_t replacement[]={1,vx_heap_box(&h,value,2,NULL)};
    size_t replacement_allocations=allocations;
    expect(vx_heap_replace(&h,storage,replacement,2,owner_choice_map),"empty owner-bearing local receives a replacement");
    expect(allocations==replacement_allocations,"replacement bookkeeping does not allocate");
    vx_heap_leave(&h);
    expect(h.live==1&&((uint64_t *)(uintptr_t)storage[1])[0]==42,"replacement survives the callee frame");
    vx_heap_enter(&h); uint64_t empty[]={0,0};
    expect(vx_heap_replace(&h,storage,empty,2,owner_choice_map)&&!h.live,"replacement with an empty choice releases the old value");
    replacement[1]=vx_heap_box(&h,value,2,NULL);
    expect(vx_heap_replace(&h,storage,replacement,2,owner_choice_map),"empty-to-owning replacement can repeat");
    vx_heap_leave(&h); vx_heap_drop(&h,1,0); vx_heap_region_end(&h,1);
    expect(!h.live&&!h.regions[h.depth],"region and owner cleanup remain independent");
    uint64_t container=vx_heap_box(&h,empty,2,owner_choice_map);
    vx_heap_adopt(&h,&container,box_map,0,2); vx_heap_enter(&h);
    replacement[1]=vx_heap_box(&h,value,2,NULL);
    expect(vx_heap_replace(&h,(uint64_t *)(uintptr_t)container,replacement,2,owner_choice_map),"replacement finds containing heap storage");
    vx_heap_leave(&h);
    expect(h.live==2&&vx_heap_node(container)->children!=NULL,"new child remains owned by its heap container");
    vx_heap_drop(&h,2,0); vx_heap_leave(&h);
    expect(!h.live&&allocations==frees,"replacement paths free every allocation exactly once");
    text_runtime_tests();
    /* Exhaust cuts and strides against the arithmetic partition, with the
       allocator disabled: splitting cannot inspect, copy or own elements. */
    uint64_t split_storage[64],split_before[64],split_args[3],split_out[5];
    for(unsigned i=0;i<64;++i) split_storage[i]=i+17;
    memcpy(split_before,split_storage,sizeof split_storage);
    size_t split_allocations=allocations; bool split_ok=true; fail=1;
    for(unsigned stride=1;stride<=4;++stride) for(unsigned length=0;length<=16;++length)
        for(unsigned cut=0;cut<=17;++cut) {
            split_args[0]=(uint64_t)(uintptr_t)split_storage; split_args[1]=length; split_args[2]=cut;
            vx_slice_split(split_args,stride,split_out);
            if(cut>length) split_ok&=split_out[0]==1&&split_out[1]==split_args[0]&&split_out[2]==length&&!split_out[3]&&!split_out[4];
            else split_ok&=!split_out[0]&&split_out[1]==split_args[0]&&split_out[2]==cut&&split_out[3]==(uint64_t)(uintptr_t)(split_storage+cut*stride)&&split_out[4]==length-cut;
        }
    expect(split_ok&&!memcmp(split_storage,split_before,sizeof split_storage)&&allocations==split_allocations,"1224 checked slice partitions preserve elements and allocate nothing");
    split_args[0]=split_args[1]=split_args[2]=0; vx_slice_split(split_args,1,split_out);
    expect(!split_out[0]&&!split_out[1]&&!split_out[2]&&!split_out[3]&&!split_out[4],"empty null-backed split avoids pointer arithmetic");
    fail=0;
    vector_insert_runtime_tests();
    memory_runtime_tests();
    line_runtime_tests();
    printf("%u heap checks, %u failures\n",checks,failures); return failures?1:0;
}
