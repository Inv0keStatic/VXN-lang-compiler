/* Inject host I/O failures without a full disk, invalid FILE pointers, or
   platform-specific devices. Successful operations still use real stdio. */
#include "../runtime/scalar.h"
#include <errno.h>
#include <stdio.h>
static int open_failure,read_failure,short_write,flush_failure,close_failure;
static unsigned closes,checks,failures;
static size_t partial_read;
static FILE *fake_open(const char *path,const char *mode) {
    (void)path; (void)mode;
    if(open_failure) { errno=open_failure; return NULL; }
    return tmpfile();
}
static int fake_getc(FILE *file) { return read_failure?EOF:fgetc(file); }
static size_t fake_read(void *p,size_t size,size_t count,FILE *file) {
    if(read_failure) return fread(p,size,partial_read<count?partial_read:count,file);
    return fread(p,size,count,file);
}
static int fake_error(FILE *file) { return read_failure?1:ferror(file); }
static size_t fake_write(const void *p,size_t size,size_t count,FILE *file) {
    return fwrite(p,size,short_write&&count?count-1:count,file);
}
static int fake_flush(FILE *file) { return flush_failure?EOF:fflush(file); }
static int fake_close(FILE *file) {
    ++closes; int result=fclose(file); return close_failure?EOF:result;
}
#define fopen fake_open
#define fgetc fake_getc
#define fread fake_read
#define ferror fake_error
#define fwrite fake_write
#define fflush fake_flush
#define fclose fake_close
#include "../runtime/io.h"
#undef fopen
#undef fgetc
#undef fread
#undef ferror
#undef fwrite
#undef fflush
#undef fclose

static void expect(int ok,const char *message) {
    ++checks;
    if(!ok) { ++failures; fprintf(stderr,"FAIL: %s\n",message); }
}
static void result_error(const uint64_t result[2],unsigned tag,unsigned code,const char *message) {
    expect(result[0]==tag&&result[1]==code,message);
}
int main(void) {
    VxIo io; vx_io_init(&io,stdin,stdout,stderr);
    uint64_t result[2],path[]={120},data[]={65,66,67,68};
    const int host_errors[]={ENOENT,EACCES,EMFILE,ENOMEM,EBUSY};
    const unsigned language_errors[]={VX_IO_NOT_FOUND,VX_IO_PERMISSION,VX_IO_RESOURCE,VX_IO_RESOURCE,VX_IO_OPEN_FAILED};
    for(unsigned i=0;i<sizeof host_errors/sizeof host_errors[0];++i) {
        open_failure=host_errors[i];
        vx_io_exec(&io,VX_IO_OPEN_READ,0,path,1,0,0,result);
        result_error(result,1,language_errors[i],"open error mapping");
    }
    open_failure=0;
    vx_io_exec(&io,VX_IO_CREATE,0,path,1,0,0,result);
    expect(result[0]==0,"open output fixture");
    if(result[0]) return 1;
    uint64_t handle=result[1];
    VxIoFile *file=vx_io_find(&io,handle);
    short_write=1;
    vx_io_exec(&io,VX_IO_WRITE,handle,data,4,0,0,result);
    result_error(result,1,VX_IO_WRITE_FAILED,"partial write is an explicit error");
    expect(ftell(file->file)==3,"partial write preserves its actual prefix");
    short_write=0;
    uint64_t many[2050]; for(size_t i=0;i<2050;++i) many[i]=i&255;
    vx_io_exec(&io,VX_IO_WRITE,handle,many,2050,0,0,result);
    expect(result[0]==0&&ftell(file->file)==2053,"large byte view writes across bounded chunks");
    rewind(file->file); unsigned char actual[2053]; size_t actual_count=fread(actual,1,sizeof actual,file->file);
    int exact=actual_count==2053;
    for(size_t i=0;i<2050&&exact;++i) if(actual[i+3]!=(i&255)) exact=0;
    expect(exact,"large byte output verified independently");
    flush_failure=1;
    vx_io_exec(&io,VX_IO_FLUSH,handle,NULL,0,0,0,result);
    result_error(result,1,VX_IO_FLUSH_FAILED,"flush failure is recoverable");
    flush_failure=0; close_failure=1;
    vx_io_exec(&io,VX_IO_CLOSE,handle,NULL,0,0,0,result);
    result_error(result,1,VX_IO_CLOSE_FAILED,"close failure is recoverable");
    expect(vx_io_find(&io,handle)==NULL,"failed close still invalidates the handle");
    close_failure=0;
    vx_io_exec(&io,VX_IO_WRITE,handle,data,4,0,0,result);
    result_error(result,1,VX_IO_INVALID_HANDLE,"failed close cannot leave a live alias");

    vx_io_exec(&io,VX_IO_OPEN_READ,0,path,1,0,0,result);
    expect(result[0]==0,"open input fixture");
    if(result[0]) return 1;
    handle=result[1]; read_failure=1;
    vx_io_exec(&io,VX_IO_READ_BYTE,handle,NULL,0,0,0,result);
    result_error(result,2,VX_IO_READ_FAILED,"read error is distinct from EOF");
    read_failure=0;
    vx_io_exec(&io,VX_IO_READ_BYTE,handle,NULL,0,0,0,result);
    expect(result[0]==1,"empty stream is EOF");
    uint64_t buffer[600],read_result[3];
    for(size_t i=0;i<600;++i) buffer[i]=999;
    file=vx_io_find(&io,handle);
    rewind(file->file);
    for(unsigned i=0;i<520;++i) fputc((int)(i&255),file->file);
    rewind(file->file);
    vx_io_read_into(&io,handle,buffer,0,read_result);
    expect(read_result[0]==0&&read_result[1]==0&&ftell(file->file)==0,"zero-length read does not consume input");
    vx_io_read_into(&io,handle,buffer,600,read_result);
    expect(read_result[0]==0&&read_result[1]==520,"buffered read spans chunks and returns short prefix");
    bool correct=true; for(unsigned i=0;i<600;++i) if(buffer[i]!=(i<520?(i&255):999)) correct=false;
    expect(correct,"buffered read widens raw bytes and preserves untouched tail");
    vx_io_read_into(&io,handle,buffer,600,read_result);
    expect(read_result[0]==1&&buffer[0]==0&&buffer[599]==999,"buffered EOF leaves buffer unchanged");
    rewind(file->file); read_failure=1; partial_read=3;
    for(unsigned i=0;i<600;++i) buffer[i]=999;
    vx_io_read_into(&io,handle,buffer,8,read_result);
    expect(read_result[0]==2&&read_result[1]==VX_IO_READ_FAILED&&read_result[2]==3,"partial read error reports valid prefix count");
    expect(buffer[0]==0&&buffer[2]==2&&buffer[3]==999,"partial read error preserves written prefix and untouched tail");
    partial_read=0;
    vx_io_read_into(&io,handle,buffer,8,read_result);
    expect(read_result[0]==2&&read_result[2]==0&&buffer[3]==999,"zero-progress read error");
    read_failure=0;
    vx_io_read_into(&io,2,buffer,0,read_result);
    expect(read_result[0]==2&&read_result[1]==VX_IO_WRONG_MODE&&read_result[2]==0,"zero-length read still validates mode");
    vx_io_read_into(&io,0,buffer,0,read_result);
    expect(read_result[0]==2&&read_result[1]==VX_IO_INVALID_HANDLE&&read_result[2]==0,"zero-length read still validates handle");
    vx_io_read_into(&io,handle,buffer,UINT64_MAX,read_result);
    expect(read_result[0]==2&&read_result[1]==VX_IO_READ_FAILED&&read_result[2]==0,"unrepresentable read count rejected before buffer access");
    close_failure=1;
    expect(vx_io_finish(&io)==1,"shutdown reports a close failure");
    unsigned after_cleanup=closes; close_failure=0;
    expect(vx_io_finish(&io)==0&&closes==after_cleanup,"shutdown does not close handles twice");

    io.files[3].generation=UINT64_MAX>>8;
    uint64_t retired=(io.files[3].generation<<8)|4;
    vx_io_exec(&io,VX_IO_CREATE,0,path,1,0,0,result);
    expect(result[0]==0 && (result[1]&255)==5,"exhausted generation retires its slot");
    expect(vx_io_find(&io,retired)==NULL,"retired token is never valid again");
    expect(vx_io_finish(&io)==0,"retired-slot fixture cleanup");

    FILE *external=tmpfile();
    expect(external!=NULL,"external stream fixture");
    if(external) {
        vx_io_init(&io,stdin,external,stderr);
        io.files[1].touched=1; flush_failure=1;
        expect(vx_io_finish(&io)==1,"shutdown reports output flush failure");
        flush_failure=0;
        expect(fputc('x',external)!=EOF,"shutdown leaves caller's output stream open");
        fclose(external);
    }
    printf("%u I/O fault checks, %u failures\n",checks,failures);
    return failures?1:0;
}
