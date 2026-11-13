#include "internal.h"
#include <string.h>
void vx_describe_type(const VxnProgram *p,Type t,char *out,size_t capacity) {
    if(!capacity) return;
    if(vx_ref(t)) {
        int n=snprintf(out,capacity,"%s",vx_uniq(t)?(vx_writable(t)?"uniq mut ":"uniq const "):"ref ");
        if(n>=0&&(size_t)n<capacity) vx_describe_type(p,vx_referent(t),out+n,capacity-(size_t)n);
    } else if(t<TY_AGGREGATE) snprintf(out,capacity,"%s",vx_type(t));
    else if(vx_function_item(p,t)) {
        const Function *f=p->types[t]->signature;
        if(f->anonymous) snprintf(out,capacity,"function-item@%zu",f->name.start-p->modules[f->name.module].start);
        else snprintf(out,capacity,"function-item(%s::%.*s)",p->modules[f->name.module].name,(int)f->name.length,p->source+f->name.start);
    }
    else if(vx_closure(p,t)&&!vx_opaque(p,t)) {
        const Function *f=p->types[t]->signature;
        snprintf(out,capacity,"closure@%zu (%s)",f->name.start-p->modules[f->name.module].start,f->call_mode==2?"consuming":f->call_mode==1?"exclusive":"shared");
    }
    else if(vx_callable(p,t)||vx_opaque(p,t)) {
        const Function *f=p->types[t]->signature; static const char *const caps[]={"pure","io","noalloc","nopanic","noblock","hot"};
        snprintf(out,capacity,vx_opaque(p,t)?"opaque safe ":"safe ");
        for(int i=0;i<6;++i) if(f->caps&(1u<<i)) { size_t n=strlen(out); snprintf(out+n,capacity-n,"%s ",caps[i]); }
        size_t n=strlen(out); snprintf(out+n,capacity-n,f->call_mode==2?"fn once(":f->call_mode==1?"fn mut(":"fn(");
        for(int i=0;i<f->param_count;++i) {
            n=strlen(out); snprintf(out+n,capacity-n,i?", const ":"const ");
            n=strlen(out); vx_describe_type(p,f->params[i].type,out+n,capacity-n);
        }
        n=strlen(out); snprintf(out+n,capacity-n,") -> "); n=strlen(out); vx_describe_type(p,f->result,out+n,capacity-n);
    }
    else if(p->types[t]->kind==TD_PARAMETER&&!p->types[t]->template_type) vx_name(p,p->types[t]->name,out,capacity);
    else if(p->types[t]->template_type) {
        const TypeDef *d=p->types[t]; vx_describe_type(p,d->template_type-1,out,capacity);
        size_t n=strlen(out); snprintf(out+n,capacity-n,"[");
        for(int i=0;i<d->type_arg_count;++i) {
            n=strlen(out); if(i) snprintf(out+n,capacity-n,",");
            n=strlen(out); vx_describe_type(p,d->type_args[i],out+n,capacity-n);
        }
        n=strlen(out); snprintf(out+n,capacity-n,"]");
    }
    else if(p->types[t]->kind==TD_STRING) snprintf(out,capacity,"String");
    else if(vx_str(p,t)) snprintf(out,capacity,"str");
    else if(vx_slice(p,t)||vx_heap_type(p,t)||vx_fn_view(p,t)||vx_dyn_fn(p,t)) {
        int n=snprintf(out,capacity,vx_dyn_fn(p,t)?"DynFn[":vx_fn_view(p,t)?(vx_uniq_fn_view(p,t)?"FnUniq[":"FnRef["):p->types[t]->kind==TD_BOX?"Box[":vx_vec(p,t)?"Vec[":vx_uniq_slice(p,t)?"UniqSlice[":"Slice[");
        if(n>=0&&(size_t)n<capacity) vx_describe_type(p,p->types[t]->element,out+n,capacity-(size_t)n);
        size_t used=strlen(out); snprintf(out+used,capacity-used,"]");
    }
    else if(p->types[t]->kind==TD_ARRAY) {
        Type base=t; while(base>=TY_AGGREGATE&&p->types[base]->kind==TD_ARRAY) base=p->types[base]->element;
        vx_describe_type(p,base,out,capacity);
        while(t>=TY_AGGREGATE&&p->types[t]->kind==TD_ARRAY) {
            size_t n=strlen(out); snprintf(out+n,capacity-n,"[%d]",p->types[t]->count); t=p->types[t]->element;
        }
    } else {
        const Token name=p->types[t]->name;
        snprintf(out,capacity,"%s::%.*s",p->modules[name.module].name,(int)name.length,p->source+name.start);
    }
}
const Arg *vx_argument(const Expr *e,int parameter) {
    for(const Arg *a=e->args;a;a=a->next) if(a->field==parameter) return a;
    return NULL;
}
int vx_argument_offset(const VxnProgram *p,const Expr *e,int parameter) {
    int offset=0;
    for(const Arg *a=e->args;a;a=a->next) if(a->field<parameter) offset+=vx_slots(p,a->expr->type);
    return offset;
}
int vx_integer(Type t) { return t >= TY_INT_BASE && t < TY_INT_END; }
int vx_float(Type t) { return t == TY_F32 || t == TY_F64 || vx_extended(t); }
unsigned vx_width(Type t) { return vx_float(t) ? (t == TY_F32 ? 32u : t == TY_F64 ? 64u : t == TY_F80 ? 80u : 128u) : 8u << ((t - TY_INT_BASE) & 3); }
bool vx_signed_type(Type t) { return vx_integer(t) && ((t - TY_INT_BASE) & 4); }
unsigned vx_mode(Type t) { return (unsigned)(t - TY_INT_BASE) / 8; }
const char *vx_type(Type t) {
    static const char *names[] = {
        "wrap uint8","wrap uint16","wrap uint32","wrap uint64","wrap sint8","wrap sint16","wrap sint32","wrap sint64",
        "checked uint8","checked uint16","checked uint32","checked uint64","checked sint8","checked sint16","checked sint32","checked sint64",
        "saturating uint8","saturating uint16","saturating uint32","saturating uint64","saturating sint8","saturating sint16","saturating sint32","saturating sint64" };
    if (vx_ref(t)) return vx_uniq(t)?"exclusive reference":"shared reference";
    if (vx_integer(t)) return names[t-TY_INT_BASE];
    if (t==TY_CANVAS) return "Canvas";
    if (t==TY_WINDOW) return "Window";
    return t == TY_BOOL ? "bool" : t == TY_VOID ? "void" : t == TY_F32 ? "float32" : t == TY_F64 ? "float64" : t == TY_F80 ? "float80" : t == TY_F128 ? "float128" : t == TY_FILE ? "IoFile" : t >= TY_AGGREGATE ? "aggregate" : "<inferred>";
}
int vx_slots(const VxnProgram *p, Type t) { return vx_extended(t) ? 2 : t < TY_AGGREGATE ? 1 : p->types[t]->slots; }
Type vx_array_type(VxnProgram *p, Type element, int count, Token at) {
    if(vx_shared_aggregate(p,element)) vx_fail(p,at,"borrowed array elements require borrowed-array contracts");
    if(vx_str(p,element)) vx_fail(p,at,"str inside aggregates requires borrowed-aggregate contracts");
    if(vx_fn_view(p,element)) vx_fail(p,at,"callable views inside aggregates require borrowed-aggregate contracts");
    if(vx_ref(element)) vx_fail(p,at,"references inside aggregates require borrowed-aggregate contracts");
    if(vx_slice(p,element)) vx_fail(p,at,"slices inside aggregates require borrowed-aggregate contracts");
    if (count < 0 || count > 1024) vx_fail(p,at,"array length must be in 0..1024");
    for (int i = TY_AGGREGATE; i < p->type_count; ++i) {
        TypeDef *d = p->types[i];
        if (d->kind == TD_ARRAY && d->element == element && d->count == count) return i;
    }
    if (p->type_count == VX_MAX_TYPES) vx_fail(p,at,"type limit exceeded");
    TypeDef *d = vx_alloc(p,sizeof *d); d->kind = TD_ARRAY; d->element = element; d->count = count; d->name = at;
    if (element < TY_AGGREGATE || p->types[element]->slots) {
        d->slots = vx_slots(p,element) * count;
        if (!d->slots) d->slots = 1;
        if (d->slots > VX_MAX_SLOTS) vx_fail(p,at,"aggregate exceeds 4096 scalar slots");
    }
    p->types[p->type_count] = d; return p->type_count++;
}
Type vx_reference_type(VxnProgram *p,Type t,Token at) {
    if(vx_split_result(p,t)) vx_fail(p,at,"consume SplitResult with match instead of borrowing its descriptor");
    if(vx_dyn_fn(p,t)) vx_fail(p,at,"DynFn requires Box storage; borrow the Box handle instead");
    if(t==TY_VOID||t==TY_INFER||vx_borrowed(p,t))
        vx_fail(p,at,"ref requires an owned non-void value type; nested references are not supported");
    return -t-1;
}
Type vx_exclusive_type(VxnProgram *p,Type t,bool writable,Token at) {
    return vx_reference_type(p,t,at)-(writable?1:2)*VX_MAX_TYPES;
}
static Type slice_type(VxnProgram *p,Type element,Token at,TypeKind kind) {
    if(element==TY_VOID || vx_dependent(p,element)||vx_dyn_fn(p,element)) vx_fail(p,at,"slice elements must be owned non-void values");
    for(int i=TY_AGGREGATE;i<p->type_count;++i) if(p->types[i]->kind==kind&&p->types[i]->element==element) return i;
    if(p->type_count==VX_MAX_TYPES) vx_fail(p,at,"type limit exceeded");
    TypeDef *d=vx_alloc(p,sizeof *d); d->kind=kind; d->element=element; d->slots=2; d->name=at;
    p->types[p->type_count]=d; return p->type_count++;
}
Type vx_slice_type(VxnProgram *p,Type element,Token at) { return slice_type(p,element,at,TD_SLICE); }
Type vx_uniq_slice_type(VxnProgram *p,Type element,Token at) { return slice_type(p,element,at,TD_UNIQ_SLICE); }
static Type heap_type(VxnProgram *p,Type element,Token at,TypeKind kind) {
    if(vx_dyn_fn(p,element)&&kind!=TD_BOX) vx_fail(p,at,"DynFn requires Box storage");
    if(element==TY_VOID||vx_dependent(p,element)) vx_fail(p,at,"%s requires an owned non-void payload",kind==TD_VEC?"Vec":"Box");
    for(int i=TY_AGGREGATE;i<p->type_count;++i) if(p->types[i]->kind==kind&&p->types[i]->element==element) return i;
    if(p->type_count==VX_MAX_TYPES) vx_fail(p,at,"type limit exceeded");
    TypeDef *d=vx_alloc(p,sizeof *d); d->kind=kind; d->element=element; d->name=at;
    if(kind==TD_BOX&&vx_dyn_fn(p,element)) d->signature=p->types[element]->signature;
    p->types[p->type_count]=d; return p->type_count++;
}
Type vx_box_type(VxnProgram *p,Type element,Token at) { return heap_type(p,element,at,TD_BOX); }
Type vx_vec_type(VxnProgram *p,Type element,Token at) { return heap_type(p,element,at,TD_VEC); }
Type vx_string_type(VxnProgram *p,Token at) {
    if(!p->string_type) p->string_type=heap_type(p,TY_INT_BASE,at,TD_STRING);
    return p->string_type;
}
Type vx_str_type(VxnProgram *p,Token at) {
    if(!p->str_type) p->str_type=slice_type(p,TY_INT_BASE,at,TD_STR);
    return p->str_type;
}
int vx_string_builtin(const VxnProgram *p,Token name) {
    static const char *names[VX_STRING_COUNT]={"string_new","string_from_str","string_append","string_push_scalar","string_reserve","string_clear","string_capacity","str_equal",
        "str_starts_with","str_ends_with","str_contains","str_find","str_rfind","str_compare","str_is_boundary","str_scalar_len","string_truncate","str_slice","str_from_utf8"};
    for(int i=0;i<VX_STRING_COUNT;++i) if(vx_word(p,name,names[i])) return i;
    return -1;
}
int vx_vec_builtin(const VxnProgram *p,Token name) {
    static const char *names[VX_VEC_COUNT]={"vec_new","vec_reserve","vec_push","vec_pop","vec_clear","vec_len","vec_capacity","vec_slice","vec_uniq","vec_remove","vec_swap_remove","vec_truncate","vec_insert"};
    for(int i=0;i<VX_VEC_COUNT;++i) if(vx_word(p,name,names[i])) return i;
    return -1;
}
const char *vx_op_name(int op) {
    static const char *names[] = {"+","-","*","/","%","==","!=","<","<=",">",">=","&","|","^","<<",">>","&&","||","!","-","~","+"};
    return op >= 0 && op <= VX_POS ? names[op] : "?";
}
int vx_io_builtin(const VxnProgram *p, Token name) {
    static const char *names[VX_IO_COUNT]={"io_stdin","io_stdout","io_stderr",
        "io_open_read","io_create","io_open_append","io_read_byte","io_write_byte",
        "io_write","io_print","io_println","io_flush","io_close","io_read","io_read_line"};
    for(int i=0;i<VX_IO_COUNT;++i) if(vx_word(p,name,names[i])) return i;
    return -1;
}
Type vx_io_result(const VxnProgram *p, int op) {
    if(op<=VX_IO_STDERR) return TY_FILE;
    if(op<=VX_IO_OPEN_APPEND) return p->io_open;
    if(op==VX_IO_READ_LINE) return p->io_line;
    return op==VX_IO_READ_BYTE?p->io_read:op==VX_IO_READ_BUFFER?p->io_read_buffer:p->io_result;
}
