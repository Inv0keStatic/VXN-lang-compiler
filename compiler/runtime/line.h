#ifndef VX_LINE_H
#define VX_LINE_H
/* Append a binary line including LF to an exclusively borrowed byte vector.
   Reserve before reading: a failed growth never consumes an unreported byte.
   limit bounds bytes consumed in this call, not the vector's prior prefix. */
static inline void vx_io_line(VxIo *io,VxHeap *heap,uint64_t handle,
                              uint64_t *vector,uint64_t limit,uint64_t result[4]) {
    memset(result,0,4*sizeof(uint64_t)); result[0]=2;
    VxIoFile *file=vx_io_find(io,handle);
    if(!file) { result[2]=VX_IO_INVALID_HANDLE; return; }
    if(!(file->mode&1)) { result[2]=VX_IO_WRONG_MODE; return; }
    if(limit>UINT32_MAX) { result[1]=2; return; }
    if(!limit) { result[1]=2; return; }
    if(!vx_io_binary(io,file)) { result[2]=VX_IO_READ_FAILED; return; }
    uint64_t *v=(uint64_t *)(uintptr_t)*vector,total=0;
    for(;;) {
        if(total==limit) { result[1]=2; result[3]=total; return; }
        if(!vx_vec_reserve(heap,v,1,1)) { result[1]=1; result[3]=total; return; }
        int byte=fgetc(file->file);
        if(byte!=EOF) { ((uint64_t *)(uintptr_t)v[0])[v[1]++]=(unsigned char)byte; ++total; }
        if(ferror(file->file)) { result[2]=VX_IO_READ_FAILED; result[3]=total; return; }
        if(byte==EOF||byte=='\n') { result[0]=total?0:1; result[1]=total; return; }
    }
}
#endif
