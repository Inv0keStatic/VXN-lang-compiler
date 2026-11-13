/* Stable, scalar-only C ABI for directly emitted LLVM modules. */
#include "scalar.h"
#include "io_api.h"
#include "io.h"
#include "heap.h"
#include "line.h"
#include "parse.h"
#include "utf8.h"
#include "text.h"
#include "../../stdlib/runtime/library.h"
#include <inttypes.h>
#include <stdlib.h>

static VxIo io;
static VxLibrary library;
static VxHeap heap;
static uint64_t call_depth, stack_cells;

void vxn_rt_parse_int(const uint64_t *bytes,uint64_t length,uint64_t width,uint64_t sign,uint64_t result[3]) {
    vx_parse_int(bytes,length,(unsigned)width,(int)sign,result);
}
void vxn_rt_parse_float(const uint64_t *bytes,uint64_t length,uint64_t width,uint64_t result[3]) {
    vx_parse_float(bytes,length,(unsigned)width,result);
}
void vxn_rt_utf8(uint64_t next,const uint64_t *bytes,uint64_t length,uint64_t offset,uint64_t result[3]) {
    vx_utf8_exec((int)next,bytes,length,offset,result);
}
void vxn_rt_string_from_utf8(uint64_t address,uint64_t result[4]) { vx_string_from_utf8(address,result); }

void vxn_rt_init(void) {
    call_depth = stack_cells = 0;
    vx_io_init(&io,stdin,stdout,stderr);
    vx_lib_init(&library);
}
uint64_t vxn_rt_finish(void) {
    vx_heap_finish(&heap);
    uint64_t status=0;
    if(!vx_lib_finish(&library)) { fputs("graphics cleanup failed\n",stderr); status=2; }
    if(vx_io_finish(&io)) { fputs("I/O cleanup failed\n",stderr); status=2; }
    return status;
}
_Noreturn void vxn_rt_panic(void) {
    if(!vx_lib_finish(&library)) exit(2);
    if (vx_io_finish(&io)) exit(2);
    if (fputs("panic\n",stderr) == EOF || fflush(stderr)) exit(2);
    exit(1);
}
_Noreturn void vxn_rt_host_error(void) {
    (void)vx_lib_finish(&library);
    (void)vx_io_finish(&io);
    fputs("native resource limit exceeded\n",stderr); exit(2);
}
void vxn_rt_enter(uint64_t cells) {
    if (call_depth == 96 || cells > 32768 - stack_cells) vxn_rt_host_error();
    ++call_depth; stack_cells += cells; vx_heap_enter(&heap);
}
void vxn_rt_leave(uint64_t cells) { vx_heap_leave(&heap); --call_depth; stack_cells -= cells; }
void vxn_rt_text(uint64_t op,const uint64_t *args,uint64_t *result) {
    vx_text_exec(&heap,(int)op,args,result);
}
void vxn_rt_split(const uint64_t *args,uint64_t stride,uint64_t *result) {
    vx_slice_split(args,(size_t)stride,result);
}
void vxn_rt_vec(uint64_t op,const uint64_t *args,uint64_t stride,const uint64_t *map,uint64_t *result) {
    vx_vec_exec(&heap,(int)op,args,(size_t)stride,map,result);
}
uint64_t vxn_rt_box_new(const uint64_t *value,uint64_t cells,const uint64_t *map) { return vx_heap_box(&heap,value,(size_t)cells,map); }
void vxn_rt_box_into_inner(uint64_t address,uint64_t cells,const uint64_t *map,uint64_t *result) {
    vx_heap_unbox(&heap,address,(size_t)cells,map,result);
}
void vxn_rt_adopt(const uint64_t *value,const uint64_t *map,uint64_t parent,uint64_t slot) { vx_heap_adopt(&heap,value,map,(int)parent,slot); }
void vxn_rt_drop(uint64_t slot) { vx_heap_drop(&heap,slot,0); }
void vxn_rt_destroy_value(const uint64_t *value,const uint64_t *map) { vx_heap_destroy_value(&heap,value,map); }
void vxn_rt_region_bind(uint64_t *record,const uint64_t *storage,uint64_t cells,uint64_t slot) { vx_heap_region_bind(&heap,record,storage,(size_t)cells,slot); }
void vxn_rt_region_end(uint64_t slot) { vx_heap_region_end(&heap,slot); }
void vxn_rt_region_pop(uint64_t *record) { vx_heap_region_pop(&heap,record); }
void vxn_rt_replace(uint64_t *destination,const uint64_t *value,uint64_t cells,const uint64_t *map) {
    if(!vx_heap_replace(&heap,destination,value,(size_t)cells,map)) vxn_rt_host_error();
}
void vxn_rt_memory(uint64_t swap,const uint64_t *args,uint64_t cells,const uint64_t *map,uint64_t *out) {
    if(!vx_memory_exec(&heap,(int)swap,args,(size_t)cells,map,out)) vxn_rt_host_error();
}
uint64_t vxn_rt_calc(uint64_t op,uint64_t a,uint64_t b,uint64_t width,uint64_t sign,uint64_t mode) {
    bool ok = true;
    uint64_t value = vx_integer_op((int)op,a,b,(unsigned)width,sign != 0,(unsigned)mode,&ok);
    if (!ok) vxn_rt_panic();
    return value;
}
uint64_t vxn_rt_float(uint64_t op,uint64_t a,uint64_t b,uint64_t width) {
    return vx_float_op((int)op,a,b,(unsigned)width);
}
uint64_t vxn_rt_convert(uint64_t a,uint64_t fw,uint64_t fk,uint64_t tw,uint64_t tk) {
    return vx_convert(a,(unsigned)fw,(unsigned)fk,(unsigned)tw,(unsigned)tk);
}
uint64_t vxn_rt_bound(uint64_t index,uint64_t length,uint64_t width,uint64_t sign) {
    if ((sign && vx_signed_value(index,(unsigned)width) < 0) || index >= length) vxn_rt_panic();
    return index;
}
uint64_t vxn_rt_slice_range(uint64_t start,uint64_t count,uint64_t length,uint64_t sw,uint64_t ss,uint64_t cw,uint64_t cs) {
    if(!vx_slice_range_valid(start,count,length,(unsigned)sw,ss!=0,(unsigned)cw,cs!=0)) vxn_rt_panic();
    return start;
}
void vxn_rt_output_extended(const uint64_t *value,uint64_t width) {
    char text[64]; vxn_ext_format(value,width,text);
    if(printf("%s\n",text)<0||fflush(stdout)) { (void)vx_lib_finish(&library); (void)vx_io_finish(&io); exit(2); }
}
void vxn_rt_output(uint64_t value,uint64_t width,uint64_t kind) {
    int result;
    if (kind == 3) result = fputs(value ? "true\n" : "false\n",stdout);
    else if (kind == 2) result = printf(width == 32 ? "%.9g\n" : "%.17g\n",vx_float_value(value,(unsigned)width));
    else if (kind == 1) result = printf("%" PRId64 "\n",vx_signed_value(value,(unsigned)width));
    else result = printf("%" PRIu64 "\n",value);
    if (result < 0 || fflush(stdout)) { (void)vx_lib_finish(&library); (void)vx_io_finish(&io); exit(2); }
}
void vxn_rt_io_read_into(uint64_t handle,uint64_t *bytes,uint64_t length,uint64_t result[3]) {
    vx_io_read_into(&io,handle,bytes,length,result);
}
void vxn_rt_io_line(uint64_t handle,uint64_t vector,uint64_t limit,uint64_t result[4]) {
    vx_io_line(&io,&heap,handle,(uint64_t *)(uintptr_t)vector,limit,result);
}
void vxn_rt_io(uint64_t op,uint64_t handle,const uint64_t *bytes,uint64_t length,
               uint64_t value,uint64_t format,uint64_t *result) {
    vx_io_exec(&io,(int)op,handle,bytes,(size_t)length,value,(unsigned)format,result);
}
void vxn_rt_library(uint64_t op,const uint64_t *args,uint64_t length,uint64_t *result) {
    vx_lib_exec(&library,(int)op,args,(size_t)length,result);
}
