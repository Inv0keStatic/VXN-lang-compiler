static void memory_runtime_tests(void) {
    VxHeap h={0}; const uint64_t map[]={1,0,0},choice_map[]={2,0,1,2,1,1,0};
    uint64_t seven=7,forty_two=42,previous[2],region_a[4],region_b[4];
    vx_heap_enter(&h);
    uint64_t a=vx_heap_box(&h,&seven,1,NULL);
    vx_heap_adopt(&h,&a,map,0,1); vx_heap_region_bind(&h,region_a,&a,1,1);
    vx_heap_enter(&h);
    uint64_t b=vx_heap_box(&h,&forty_two,1,NULL);
    size_t before_allocations=allocations,before_frees=frees; fail=1;
    expect(vx_heap_exchange(&h,&a,&b,1,map,previous),"exchange locates destination in caller frame");
    expect(previous[0]!=a&&*(uint64_t *)(uintptr_t)previous[0]==7&&*(uint64_t *)(uintptr_t)a==42,"exchange returns old live owner and installs new one");
    expect(allocations==before_allocations&&frees==before_frees,"exchange neither allocates nor destroys");
    vx_heap_leave(&h);
    expect(h.live==1&&*(uint64_t *)(uintptr_t)a==42,"discarded old owner dies in callee while replacement survives");
    fail=0;
    vx_heap_enter(&h);
    b=vx_heap_box(&h,&seven,1,NULL); vx_heap_adopt(&h,&b,map,0,1);
    vx_heap_region_bind(&h,region_b,&b,1,1);
    before_allocations=allocations; before_frees=frees; fail=1;
    expect(vx_heap_swap(&h,&a,&b,1,map,previous),"swap finds independent frame destinations");
    expect(*(uint64_t *)(uintptr_t)a==7&&*(uint64_t *)(uintptr_t)b==42,"swap exchanges both owners");
    expect(allocations==before_allocations&&frees==before_frees,"swap neither allocates nor destroys");
    vx_heap_leave(&h);
    expect(h.live==1&&*(uint64_t *)(uintptr_t)a==7,"swap preserves each destination cleanup scope");
    fail=0;
    uint64_t empty[2]={0,0},filled[2]={1,a};
    uint64_t container=vx_heap_box(&h,filled,2,choice_map);
    a=0; vx_heap_adopt(&h,&container,map,0,2);
    vx_heap_region_bind(&h,region_b,empty,2,3);
    before_allocations=allocations; before_frees=frees; fail=1;
    expect(vx_heap_swap(&h,(uint64_t *)(uintptr_t)container,empty,2,choice_map,previous),"swap heap child and empty local choice");
    expect(!vx_heap_node(container)->children&&empty[0]==1&&vx_heap_node(empty[1])->slot==3,"empty choice receives old child in its own cleanup slot");
    expect(vx_heap_exchange(&h,(uint64_t *)(uintptr_t)container,empty,2,choice_map,previous),"exchange into empty heap choice finds container");
    empty[0]=empty[1]=0;
    expect(!previous[0]&&vx_heap_node(container)->children&&vx_heap_node(container)->children->owner==vx_heap_node(container),"exchange attaches incoming child to heap owner");
    expect(allocations==before_allocations&&frees==before_frees,"choice exchanges need no allocator even across empty tags");
    uint64_t invalid=0,unchanged[2]; memcpy(unchanged,(void *)(uintptr_t)container,sizeof unchanged);
    expect(!vx_heap_swap(&h,(uint64_t *)(uintptr_t)container,&invalid,2,choice_map,previous)&&!memcmp(unchanged,(void *)(uintptr_t)container,sizeof unchanged),"unregistered second destination fails before changing first");
    vx_heap_leave(&h);
    expect(!h.live&&allocations==frees,"memory exchanges release every allocation once");
    /* Copy-only data needs no owner regions. Exhaust 4096 deterministic swaps
       and replacements against a separate scalar state with allocation off. */
    uint64_t data[8],oracle[8];
    for(unsigned i=0;i<8;++i) data[i]=oracle[i]=i;
    uint32_t rng=17; bool same=true;
    for(unsigned i=0;i<4096;++i) {
        rng=rng*1664525u+1013904223u; unsigned x=(rng>>8)&7,y=(x+1+((rng>>16)%7))&7;
        if(rng&1) {
            uint64_t value=rng,old=oracle[x]; oracle[x]=value;
            same&=vx_heap_exchange(&h,&data[x],&value,1,NULL,previous)&&previous[0]==old;
        } else {
            uint64_t old=oracle[x]; oracle[x]=oracle[y]; oracle[y]=old;
            same&=vx_heap_swap(&h,&data[x],&data[y],1,NULL,previous)!=0;
        }
        same&=!memcmp(data,oracle,sizeof data);
    }
    expect(same,"4096 copy memory operations match independent state oracle"); fail=0;
}
