static void vector_insert_runtime_tests(void) {
    VxHeap h={0}; uint64_t args[3]={0},result[3],map[]={1,0,0},value=42;
    vx_heap_enter(&h); vx_vec_exec(&h,VX_VEC_NEW,args,1,map,result); uint64_t vector=result[1];
    vx_heap_adopt(&h,&vector,map,0,1); args[0]=(uint64_t)(uintptr_t)&vector;
    uint64_t *header=(uint64_t *)(uintptr_t)vector;
    args[1]=1; args[2]=vx_heap_box(&h,&value,1,NULL); uint64_t input=args[2];
    size_t before=allocations; fail=1; vx_vec_exec(&h,VX_VEC_INSERT,args,1,map,result);
    expect(result[0]==1&&result[1]==input&&!header[0]&&!header[1]&&!header[2]&&before==allocations,"invalid insertion preserves vector and returns owner before allocation");
    args[1]=0; vx_vec_exec(&h,VX_VEC_INSERT,args,1,map,result);
    expect(result[0]==2&&!result[1]&&result[2]==input&&!header[0]&&!header[1],"first insertion allocation failure returns exact input owner");
    fail=0; vx_vec_exec(&h,VX_VEC_INSERT,args,1,map,result); vx_heap_drop(&h,0,0);
    expect(!result[0]&&header[1]==1&&h.live==3,"successful insertion rehomes owner into vector storage");
    for(uint64_t i=1;i<4;++i) { args[1]=i; args[2]=vx_heap_box(&h,&i,1,NULL); vx_vec_exec(&h,VX_VEC_INSERT,args,1,map,result); }
    uint64_t prior[3],contents[4]; memcpy(prior,header,sizeof prior); memcpy(contents,(void *)(uintptr_t)header[0],sizeof contents);
    args[1]=2; args[2]=vx_heap_box(&h,&value,1,NULL); input=args[2]; fail=1;
    vx_vec_exec(&h,VX_VEC_INSERT,args,1,map,result);
    expect(result[0]==2&&result[2]==input&&!memcmp(prior,header,sizeof prior)&&!memcmp(contents,(void *)(uintptr_t)header[0],sizeof contents),"growing insertion failure preserves metadata and all initialized elements");
    vx_heap_drop(&h,0,0); expect(h.live==6,"discarded insertion error destroys only returned owner");
    args[1]=1; vx_vec_exec(&h,VX_VEC_REMOVE,args,1,map,result); vx_heap_drop(&h,0,0);
    fail=0; value=9; args[2]=vx_heap_box(&h,&value,1,NULL); fail=1; before=allocations;
    vx_vec_exec(&h,VX_VEC_INSERT,args,1,map,result); uint64_t *data=(uint64_t *)(uintptr_t)header[0];
    expect(!result[0]&&header[1]==4&&before==allocations&&((uint64_t *)(uintptr_t)data[0])[0]==42&&((uint64_t *)(uintptr_t)data[1])[0]==9&&((uint64_t *)(uintptr_t)data[2])[0]==2&&((uint64_t *)(uintptr_t)data[3])[0]==3,"reserved insertion shifts complete owner descriptors without allocating");
    fail=0; vx_heap_leave(&h); expect(!h.live&&allocations==frees,"all inserted and returned owners are freed exactly once");
}
