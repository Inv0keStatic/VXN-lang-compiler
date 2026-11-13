#ifndef VX_IO_RUNTIME_H
#define VX_IO_RUNTIME_H
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#ifndef VX_IO_API_H
#include "io_api.h"
#endif
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#ifndef VXN_EXTENDED_H
#include "extended.h"
#endif
#include <string.h>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

/* Handles store slot/generation, not pointers. Retire exhausted generations permanently. */
#define VX_IO_FILES 67
typedef struct {
    FILE *file;
    uint64_t generation;
    unsigned mode;
    int original_mode;
    int touched;
} VxIoFile;
typedef struct { VxIoFile files[VX_IO_FILES]; } VxIo;

static inline void vx_io_init(VxIo *io, FILE *in, FILE *out, FILE *err) {
    memset(io,0,sizeof *io);
    io->files[0].file=in; io->files[0].mode=1;
    io->files[1].file=out; io->files[1].mode=2;
    io->files[2].file=err; io->files[2].mode=2;
    for(unsigned i=0;i<VX_IO_FILES;++i) io->files[i].original_mode=-1;
}
static inline VxIoFile *vx_io_find(VxIo *io, uint64_t handle) {
    unsigned slot=(unsigned)(handle & 255);
    if(!slot || slot>VX_IO_FILES) return NULL;
    VxIoFile *f=&io->files[slot-1];
    return f->file && f->generation==(handle>>8) ? f : NULL;
}
static inline int vx_io_binary(VxIo *io, VxIoFile *f) {
    if(f->touched) return 1;
#ifdef _WIN32
    if(f<io->files+3) {
        if((f->mode&2) && fflush(f->file)) return 0;
        int descriptor=_fileno(f->file);
        if(descriptor<0) return 0;
        f->original_mode=_setmode(descriptor,_O_BINARY);
        if(f->original_mode==-1) return 0;
    }
#else
    (void)io;
#endif
    f->touched=1;
    return 1;
}
static inline unsigned vx_io_open_error(int code) {
    if(code==ENOENT) return VX_IO_NOT_FOUND;
    if(code==EACCES || code==EPERM) return VX_IO_PERMISSION;
    if(code==EMFILE || code==ENOMEM) return VX_IO_RESOURCE;
#ifdef ENFILE
    if(code==ENFILE) return VX_IO_RESOURCE;
#endif
    return VX_IO_OPEN_FAILED;
}
static inline void vx_io_failure(uint64_t *result, unsigned tag, unsigned error) {
    result[0]=tag; result[1]=error;
}
/* Widen bytes read; preserve the tail and report partial reads on failure.
   Empty reads validate handle/mode without touching stdio. */
static inline void vx_io_read_into(VxIo *io,uint64_t handle,uint64_t *bytes,uint64_t length,uint64_t result[3]) {
    result[0]=result[1]=result[2]=0;
    VxIoFile *f=vx_io_find(io,handle);
    if(!f) { vx_io_failure(result,2,VX_IO_INVALID_HANDLE); return; }
    if(!(f->mode&1)) { vx_io_failure(result,2,VX_IO_WRONG_MODE); return; }
    if(length>UINT32_MAX) { vx_io_failure(result,2,VX_IO_READ_FAILED); return; }
    if(!length) return;
    if(!vx_io_binary(io,f)) { vx_io_failure(result,2,VX_IO_READ_FAILED); return; }
    unsigned char buffer[256]; uint64_t total=0;
    while(total<length) {
        size_t count=length-total<sizeof buffer?(size_t)(length-total):sizeof buffer;
        size_t read=fread(buffer,1,count,f->file);
        for(size_t i=0;i<read;++i) bytes[total+i]=buffer[i];
        total+=read;
        if(ferror(f->file)) { vx_io_failure(result,2,VX_IO_READ_FAILED); result[2]=total; return; }
        if(read<count) break;
    }
    if(total) result[1]=total;
    else result[0]=1;
}
/* Other calls use a two-cell result. For stream getters only cell zero is used.
   Byte arrays are value snapshots represented by uint64_t cells. */
static inline void vx_io_exec(VxIo *io, int op, uint64_t handle,
        const uint64_t *bytes, size_t length, uint64_t value, unsigned format,
        uint64_t result[2]) {
    result[0]=result[1]=0;
    if(op<=VX_IO_STDERR) { result[0]=(uint64_t)op+1; return; }
    if(op>=VX_IO_OPEN_READ && op<=VX_IO_OPEN_APPEND) {
        char path[1025];
        if(!length || length>1024) { vx_io_failure(result,1,VX_IO_INVALID_PATH); return; }
        for(size_t i=0;i<length;++i) {
            if(!bytes[i] || bytes[i]>255) { vx_io_failure(result,1,VX_IO_INVALID_PATH); return; }
            path[i]=(char)(unsigned char)bytes[i];
        }
        path[length]=0;
        unsigned slot=3;
        while(slot<VX_IO_FILES && (io->files[slot].file || io->files[slot].generation==(UINT64_MAX>>8))) ++slot;
        if(slot==VX_IO_FILES) { vx_io_failure(result,1,VX_IO_RESOURCE); return; }
        FILE *file=fopen(path,op==VX_IO_OPEN_READ?"rb":op==VX_IO_CREATE?"wb":"ab");
        if(!file) { vx_io_failure(result,1,vx_io_open_error(errno)); return; }
        VxIoFile *f=&io->files[slot];
        f->file=file; ++f->generation; f->mode=op==VX_IO_OPEN_READ?1:2;
        f->touched=0; f->original_mode=-1;
        result[1]=(f->generation<<8)|(slot+1);
        return;
    }
    unsigned error_tag=op==VX_IO_READ_BYTE?2:1;
    VxIoFile *f=vx_io_find(io,handle);
    if(!f) { vx_io_failure(result,error_tag,VX_IO_INVALID_HANDLE); return; }
    if(op==VX_IO_CLOSE) {
        if(f<io->files+3) { vx_io_failure(result,1,VX_IO_WRONG_MODE); return; }
        FILE *file=f->file; f->file=NULL;
        if(fclose(file)) vx_io_failure(result,1,VX_IO_CLOSE_FAILED);
        return;
    }
    unsigned required=op==VX_IO_READ_BYTE?1:2;
    if(!(f->mode&required)) { vx_io_failure(result,error_tag,VX_IO_WRONG_MODE); return; }
    if(!vx_io_binary(io,f)) {
        vx_io_failure(result,error_tag,op==VX_IO_READ_BYTE?VX_IO_READ_FAILED:VX_IO_WRITE_FAILED); return;
    }
    if(op==VX_IO_READ_BYTE) {
        int c=fgetc(f->file);
        if(c!=EOF) result[1]=(unsigned char)c;
        else if(ferror(f->file)) vx_io_failure(result,2,VX_IO_READ_FAILED);
        else result[0]=1;
        return;
    }
    if(op==VX_IO_FLUSH) {
        if(fflush(f->file) || ferror(f->file)) vx_io_failure(result,1,VX_IO_FLUSH_FAILED);
        return;
    }
    unsigned char buffer[1024]; size_t count=0;
    if(op==VX_IO_WRITE_BYTE) { buffer[0]=(unsigned char)value; count=1; }
    else if(op==VX_IO_WRITE) {
        size_t offset=0;
        while(offset<length) {
            count=length-offset<sizeof buffer?length-offset:sizeof buffer;
            for(size_t i=0;i<count;++i) buffer[i]=(unsigned char)bytes[offset+i];
            if(fwrite(buffer,1,count,f->file)!=count||ferror(f->file)) {
                vx_io_failure(result,1,VX_IO_WRITE_FAILED); return;
            }
            offset+=count;
        }
        if(ferror(f->file)) vx_io_failure(result,1,VX_IO_WRITE_FAILED);
        return;
    } else if(op==VX_IO_PRINT || op==VX_IO_PRINTLN) {
        unsigned width=format&255, kind=format>>8; int n;
        if(kind==3) n=snprintf((char *)buffer,sizeof buffer,"%s",value?"true":"false");
        else if(kind==2&&width>64) n=vxn_ext_format(bytes,width,(char *)buffer);
        else if(kind==2) n=snprintf((char *)buffer,sizeof buffer,width==32?"%.9g":"%.17g",vx_float_value(value,width));
        else if(kind==1) n=snprintf((char *)buffer,sizeof buffer,"%" PRId64,vx_signed_value(value,width));
        else n=snprintf((char *)buffer,sizeof buffer,"%" PRIu64,value);
        if(n<0 || (size_t)n>=sizeof buffer-1) { vx_io_failure(result,1,VX_IO_WRITE_FAILED); return; }
        count=(size_t)n;
        if(op==VX_IO_PRINTLN) buffer[count++]='\n';
    }
    if((count && fwrite(buffer,1,count,f->file)!=count) || ferror(f->file))
        vx_io_failure(result,1,VX_IO_WRITE_FAILED);
}
/* Close remaining owned files, flush used output streams, and restore any
   Windows standard-stream modes changed by this execution. Never close the
   caller's standard streams. Cleanup errors are host exit status 2. */
static inline int vx_io_finish(VxIo *io) {
    int failed=0;
    for(unsigned i=3;i<VX_IO_FILES;++i) if(io->files[i].file) {
        if(fclose(io->files[i].file)) failed=1;
        io->files[i].file=NULL;
    }
    for(int i=2;i>=0;--i) {
        VxIoFile *f=&io->files[i];
        if(f->touched && (f->mode&2) && fflush(f->file)) failed=1;
#ifdef _WIN32
        if(f->original_mode!=-1 && _setmode(_fileno(f->file),f->original_mode)==-1) failed=1;
#endif
        f->original_mode=-1; f->touched=0;
    }
    return failed;
}
#endif
