#include "../runtime/scalar.h"
#include "../runtime/io.h"
static int line_after=-1,line_failed;
static int line_getc(FILE *file) {
    if(line_after==0) { line_failed=1; return EOF; }
    if(line_after>0) --line_after;
    return fgetc(file);
}
static int line_error(FILE *file) { return line_failed||ferror(file); }
#define fgetc line_getc
#define ferror line_error
#include "../runtime/line.h"
#undef fgetc
#undef ferror

static void line_runtime_tests(void) {
    FILE *file=tmpfile(); expect(file!=NULL,"line input stream fixture"); if(!file) return;
    const unsigned char input[]={65,13,10,10,66,0,255,90};
    fwrite(input,1,sizeof input,file); rewind(file);
    VxIo io; vx_io_init(&io,file,stdout,stderr); io.files[0].touched=1;
    VxHeap heap={0}; vx_heap_enter(&heap); uint64_t args[2]={0},result[4],map[]={1,0,0};
    vx_vec_exec(&heap,VX_VEC_NEW,args,1,NULL,result); uint64_t vector=result[1];
    vx_heap_adopt(&heap,&vector,map,0,1); uint64_t *v=(uint64_t *)(uintptr_t)vector;
    args[0]=(uint64_t)(uintptr_t)&vector;
    fail=1; vx_io_line(&io,&heap,1,&vector,20,result);
    expect(result[0]==2&&result[1]==1&&!result[3]&&!v[1]&&!v[0]&&ftell(file)==0,"failed line reservation consumes no byte");
    fail=0; vx_io_line(&io,&heap,1,&vector,2,result);
    expect(result[0]==2&&result[1]==2&&result[3]==2&&v[1]==2&&ftell(file)==2,"line limit reports partial prefix without reading ahead");
    vx_io_line(&io,&heap,1,&vector,1,result); uint64_t *bytes=(uint64_t *)(uintptr_t)v[0];
    expect(!result[0]&&result[1]==1&&v[1]==3&&bytes[0]==65&&bytes[1]==13&&bytes[2]==10,"line retry appends LF and preserves CRLF");
    vx_vec_exec(&heap,VX_VEC_CLEAR,args,1,NULL,result); vx_io_line(&io,&heap,1,&vector,1,result);
    expect(!result[0]&&result[1]==1&&v[1]==1&&((uint64_t *)(uintptr_t)v[0])[0]==10,"empty line is Read not End");
    vx_vec_exec(&heap,VX_VEC_CLEAR,args,1,NULL,result); vx_io_line(&io,&heap,1,&vector,20,result); bytes=(uint64_t *)(uintptr_t)v[0];
    expect(!result[0]&&result[1]==4&&v[1]==4&&bytes[0]==66&&!bytes[1]&&bytes[2]==255&&bytes[3]==90,"unterminated final line preserves arbitrary bytes including NUL");
    vx_io_line(&io,&heap,1,&vector,20,result);
    expect(result[0]==1&&!result[1]&&v[1]==4,"EOF appends nothing and leaves existing prefix");
    rewind(file); size_t before=allocations; fail=1;
    vx_io_line(&io,&heap,1,&vector,0,result);
    expect(result[0]==2&&result[1]==2&&!result[3]&&ftell(file)==0&&before==allocations,"zero byte budget does not consume or allocate");
    vx_io_line(&io,&heap,0,&vector,0,result);
    expect(result[0]==2&&!result[1]&&result[2]==VX_IO_INVALID_HANDLE&&!result[3],"line validates handle before limit");
    vx_io_line(&io,&heap,2,&vector,0,result);
    expect(result[0]==2&&!result[1]&&result[2]==VX_IO_WRONG_MODE&&!result[3],"line validates read mode before limit");
    vx_vec_exec(&heap,VX_VEC_CLEAR,args,1,NULL,result); line_after=2;
    vx_io_line(&io,&heap,1,&vector,20,result);
    expect(result[0]==2&&!result[1]&&result[2]==VX_IO_READ_FAILED&&result[3]==2&&v[1]==2&&ftell(file)==2,"partial host read error retains exact prefix count");
    line_after=-1; line_failed=0; vx_io_line(&io,&heap,1,&vector,20,result);
    expect(!result[0]&&result[1]==1&&v[1]==3,"host read retry resumes after partial prefix");
    vx_vec_exec(&heap,VX_VEC_CLEAR,args,1,NULL,result); rewind(file); fwrite("abcdefghij\n",1,11,file); rewind(file);
    uint64_t capacity=v[2],address=v[0]; vx_io_line(&io,&heap,1,&vector,20,result);
    expect(result[0]==2&&result[1]==1&&result[3]==capacity&&v[1]==capacity&&v[0]==address&&v[2]==capacity&&ftell(file)==(long)capacity,"mid-line growth failure consumes only retained bytes");
    fail=0; vx_io_line(&io,&heap,1,&vector,20,result); bytes=(uint64_t *)(uintptr_t)v[0];
    bool correct=v[1]==11; for(unsigned i=0;i<10&&correct;++i) correct=bytes[i]=='a'+i;
    expect(!result[0]&&result[1]==11-capacity&&correct&&bytes[10]==10,"allocation retry preserves complete line without lost bytes");
    vx_heap_leave(&heap); expect(!heap.live&&allocations==frees,"line vector cleanup releases all allocations");
    expect(!vx_io_finish(&io),"line stream state cleanup"); fclose(file);
}
