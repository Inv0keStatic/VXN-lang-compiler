#include "internal.h"
#include <inttypes.h>
#include "vxn_runtime_text.h"
#include "vxn_extended_text.h"
#include "vxn_io_api_text.h"
#include "vxn_io_text.h"
#include "vxn_heap_api_text.h"
#include "vxn_heap_text.h"
#include "vxn_line_text.h"
#include "vxn_parse_text.h"
#include "vxn_utf8_text.h"
#include "vxn_text_text.h"
#include "vxn_library_api_text.h"
#include "vxn_graphics_core_text.h"
#include "vxn_platform_win32_text.h"
#include "vxn_platform_linux_text.h"
#include "vxn_platform_stub_text.h"
#include "vxn_library_text.h"
typedef struct {
    const VxnProgram *p; FILE *out; int next,indent,label,loops;
    int loop_labels[VX_MAX_DEPTH],frame_cost; bool optimize;
    const Function *fn; const Stmt *loop_stmts[VX_MAX_DEPTH];
} Emitter;
static void ctype(FILE *out,Type t) {
    if(t==TY_VOID) fputs("void",out);
    else if(vx_extended(t)) fputs("vx_extended",out);
    else if(t>=TY_AGGREGATE) fprintf(out,"vx_type_%d",t);
    else fputs("uint64_t",out);
}
static void indent(Emitter *g) { for(int i=0;i<g->indent;++i) fputs("    ",g->out); }
static int temporary(Emitter *g,Type type) {
    int id=g->next++; indent(g); ctype(g->out,type); fprintf(g->out," t_%d = ",id); return id;
}
static void ignore_temp(Emitter *g,int id) {
    if(id>=0) { indent(g); fprintf(g->out,"(void)t_%d;\n",id); }
}
static int initialized_temp(Emitter *g,Type type) {
    int id=temporary(g,type); fputs(vx_indirect(type)?"{0};\n":"0;\n",g->out); ignore_temp(g,id); return id;
}
static void adopt(Emitter *g,Type t,int value,int parent,int slot) {
    if(!vx_owned(g->p,t)) return;
    indent(g); fprintf(g->out,"vx_heap_adopt(&vx_heap, t_%d.cells, vx_owner_%d, %d, %d);\n",value,t,parent,slot);
}
static void drop(Emitter *g,int slot) { indent(g); fprintf(g->out,"vx_heap_drop(&vx_heap, %d, 0);\n",slot); }
static void region(Emitter *g,int id) {
    Type t=g->fn->local_types[id]; if(!vx_owned(g->p,t)) return;
    int capture=vx_capture(g->fn,id); indent(g);
    fprintf(g->out,"uint64_t vx_region_%d[4]; vx_heap_region_bind(&vx_heap, vx_region_%d, ",id,id);
    if(capture>=0) fprintf(g->out,"environment + %d",g->fn->captures[capture].offset);
    else fprintf(g->out,"v_%d.cells",id);
    fprintf(g->out,", %d, %d);\n",vx_slots(g->p,t),id+1);
}
static void end_local(Emitter *g,int id) {
    drop(g,id+1); indent(g); fprintf(g->out,"vx_heap_region_end(&vx_heap, %d);\n",id+1);
}
static void end_scope(Emitter *g,size_t end,size_t begin) {
    if(!end) return;
    for(int i=g->fn->local_count-1;i>=0;--i) if(g->fn->local_ends[i]==end&&g->fn->local_names[i].start>=begin&&vx_owned(g->p,g->fn->local_types[i])) end_local(g,i);
}
static void loop_cleanup(Emitter *g) {
    const Stmt *s=g->loop_stmts[g->loops-1]; size_t begin=s->body?s->body->token.start:s->end;
    for(int i=g->fn->local_count-1;i>=0;--i)
        if(g->fn->local_names[i].start>=begin&&g->fn->local_ends[i]<=s->end&&vx_owned(g->p,g->fn->local_types[i])) end_local(g,i);
    drop(g,0);
}
static int emit_expr(Emitter *g,const Expr *e);
static void emit_block(Emitter *g,const Stmt *s);
static int load(Emitter *g,Type t,const char *base,int offset) {
    int id;
    if(!vx_indirect(t)) { id=temporary(g,t); fprintf(g->out,"(%s)[%d];\n",base,offset); }
    else {
        id=initialized_temp(g,t); indent(g);
        fprintf(g->out,"memcpy(t_%d.cells, (%s) + %d, %d * sizeof(uint64_t));\n",id,base,offset,vx_slots(g->p,t));
    }
    ignore_temp(g,id); return id;
}
static void store(Emitter *g,Type t,const char *base,int offset,int value) {
    indent(g);
    if(!vx_indirect(t)) fprintf(g->out,"(%s)[%d] = t_%d;\n",base,offset,value);
    else fprintf(g->out,"memcpy((%s) + %d, t_%d.cells, %d * sizeof(uint64_t));\n",base,offset,value,vx_slots(g->p,t));
}
static void return_value(Emitter *g,int value) {
    if(g->fn->tail_member&&value>=0) store(g,g->fn->result,"vx_result",0,value);
    indent(g); fprintf(g->out,"vx_leave(%d);\n",g->frame_cost); indent(g);
    if(g->fn->tail_member) fputs("return -1;\n",g->out);
    else if(value<0) fputs("return;\n",g->out);
    else fprintf(g->out,"return t_%d;\n",value);
}
static int calc(Emitter *g,int op,Type operand,Type result,int a,int b) {
    if(vx_extended(operand)) {
        int id=initialized_temp(g,result); indent(g);
        fprintf(g->out,"vxn_ext_op(%u, %d, t_%d.cells, t_%d.cells, ",vx_width(operand),op,a,b<0?a:b);
        fprintf(g->out,vx_extended(result)?"t_%d.cells);\n":"&t_%d);\n",id); return id;
    }
    int id=temporary(g,result);
    if(vx_float(operand)) fprintf(g->out,"vx_float_op(%d, t_%d, ",op,a);
    else fprintf(g->out,"vx_calc(%d, t_%d, ",op,a);
    if(b<0) fputs("0",g->out); else fprintf(g->out,"t_%d",b);
    if(vx_float(operand)) fprintf(g->out,", %u);\n",vx_width(operand));
    else fprintf(g->out,", %u, %d, %u);\n",operand==TY_BOOL?1:vx_width(operand),vx_signed_type(operand),vx_integer(operand)?vx_mode(operand):0);
    ignore_temp(g,id); return id;
}
static int emit_place(Emitter *g,const Expr *e,bool readonly) {
    int id;
    const char *qualifier=readonly?"const ":"";
    if(e->kind==EX_VAR) {
        id=g->next++; indent(g);
        int capture=vx_capture(g->fn,e->id);
        if(capture>=0) fprintf(g->out,"%suint64_t *p_%d = environment + %d;\n",qualifier,id,g->fn->captures[capture].offset);
        else if(vx_indirect(e->type)) fprintf(g->out,"%suint64_t *p_%d = v_%d.cells;\n",qualifier,id,e->id);
        else fprintf(g->out,"%suint64_t *p_%d = &v_%d;\n",qualifier,id,e->id);
    } else if(e->kind==EX_DEREF) {
        int reference=emit_expr(g,e->left); id=g->next++; indent(g);
        fprintf(g->out,"%suint64_t *p_%d = (%suint64_t *)(uintptr_t)t_%d;\n",qualifier,id,qualifier,reference);
    } else if(e->kind==EX_INDEX&&vx_slice(g->p,e->left->type)) {
        int view=emit_expr(g,e->left),index=emit_expr(g,e->right); id=g->next++; indent(g);
        fprintf(g->out,"%suint64_t *p_%d = (%suint64_t *)(uintptr_t)t_%d.cells[0] + vx_bound(t_%d, t_%d.cells[1], %u, %d) * %d;\n",
            qualifier,id,qualifier,view,index,view,vx_width(e->right->type),vx_signed_type(e->right->type),vx_slots(g->p,e->type));
    } else {
        int base=emit_place(g,e->left,readonly); TypeDef *d=g->p->types[e->left->type];
        if(e->kind==EX_FIELD) {
            id=g->next++; indent(g); fprintf(g->out,"%suint64_t *p_%d = p_%d + %d;\n",qualifier,id,base,d->fields[e->id].offset);
        } else {
            int index=emit_expr(g,e->right); id=g->next++; indent(g);
            fprintf(g->out,"%suint64_t *p_%d = p_%d + vx_bound(t_%d, %d, %u, %d) * %d;\n",qualifier,id,base,index,d->count,vx_width(e->right->type),vx_signed_type(e->right->type),vx_slots(g->p,d->element));
        }
    }
    return id;
}
static void pattern_test(Emitter *g,const Arm *a,const Pattern *p,const char *base) {
    if(p->kind==PAT_OR) {
        fputs(" && (",g->out);
        for(const Pattern *child=p->children;child;child=child->next) {
            if(child!=p->children) fputs(" || ",g->out);
            fputs("(1",g->out); pattern_test(g,a,child,base); fputs(")",g->out);
        }
        fputs(")",g->out); return;
    }
    if(p->kind==PAT_BIND) fprintf(g->out," && (memcpy(&v_%d, (%s) + %d, %d * sizeof(uint64_t)), 1)",a->locals[p->binding],base,p->offset,vx_slots(g->p,p->type));
    if(p->kind==PAT_RANGE) {
        uint64_t sign=vx_signed_type(p->type)?UINT64_C(1)<<(vx_width(p->type)-1):0;
        if((p->value^sign)>0) fprintf(g->out," && ((%s)[%d] ^ UINT64_C(%" PRIu64 ")) >= UINT64_C(%" PRIu64 ")",base,p->offset,sign,p->value^sign);
        if((p->upper^sign)<UINT64_MAX) fprintf(g->out," && ((%s)[%d] ^ UINT64_C(%" PRIu64 ")) <= UINT64_C(%" PRIu64 ")",base,p->offset,sign,p->upper^sign);
    }
    if(p->kind==PAT_TEXT) {
        fprintf(g->out," && (%s)[%d] == %d && vx_text_equal((const uint64_t *)(uintptr_t)(%s)[%d], (const uint64_t[]){",base,p->offset+1,p->literal->argc,base,p->offset);
        if(!p->literal->argc) fputs("0",g->out);
        for(int i=0;i<p->literal->argc;++i) fprintf(g->out,"%s%u",i?",":"",(unsigned)p->literal->cells[i]);
        fprintf(g->out,"}, %d)",p->literal->argc);
    }
    if(p->kind==PAT_LITERAL||p->kind==PAT_CONSTRUCTOR)
        fprintf(g->out," && (%s)[%d] == UINT64_C(%" PRIu64 ")",base,p->offset,
                p->kind==PAT_LITERAL?p->value:(uint64_t)p->variant);
    for(const Pattern *child=p->children;child;child=child->next) pattern_test(g,a,child,base);
}
static int emit_match(Emitter *g,const Expr *scrutinee,Arm *arms,Type result) {
    int source=emit_expr(g,scrutinee),id=result==TY_VOID?-1:initialized_temp(g,result),matched=g->next++;
    char base[64]; snprintf(base,sizeof base,vx_indirect(scrutinee->type)?"t_%d.cells":"&t_%d",source);
    indent(g); fprintf(g->out,"int matched_%d = 0;\n",matched);
    for(Arm *a=arms;a;a=a->next) {
        for(int i=0;i<a->count;++i) {
            indent(g); ctype(g->out,a->bindings[i].type); fprintf(g->out," v_%d = %s;\n",a->locals[i],vx_indirect(a->bindings[i].type)?"{{0}}":"0");
            indent(g); fprintf(g->out,"(void)v_%d;\n",a->locals[i]);
        }
        indent(g); fprintf(g->out,"if (!matched_%d",matched); pattern_test(g,a,a->pattern,base); fputs(") {\n",g->out); ++g->indent;
        if(a->guard) {
            int guard=emit_expr(g,a->guard); indent(g); fprintf(g->out,"if (t_%d) {\n",guard); ++g->indent;
        }
        indent(g); fprintf(g->out,"matched_%d = 1;\n",matched);
        for(int i=0;i<a->count;++i) {
            Type t=a->bindings[i].type;
            if(vx_owned(g->p,t)) { int value=temporary(g,t); fprintf(g->out,"v_%d;\n",a->locals[i]); adopt(g,t,value,0,a->locals[i]+1); }
            region(g,a->locals[i]);
        }
        if(a->expr) {
            int value=emit_expr(g,a->expr);
            if(id>=0) { indent(g); fprintf(g->out,"t_%d = t_%d;\n",id,value); }
        } else emit_block(g,a->body);
        end_scope(g,a->end,a->token.start);
        if(a->guard) { --g->indent; indent(g); fputs("}\n",g->out); }
        --g->indent; indent(g); fputs("}\n",g->out);
    }
    indent(g); fprintf(g->out,"if (!matched_%d) vx_host_error();\n",matched);
    return id;
}
static int emit_expr(Emitter *g,const Expr *e) {
    uint64_t value; Type type;
    if(e->kind==EX_REAL&&vx_extended(e->type)) {
        int id=temporary(g,e->type); fprintf(g->out,"{{UINT64_C(%" PRIu64 "),UINT64_C(%" PRIu64 ")}};\n",e->cells[0],e->cells[1]); ignore_temp(g,id); return id;
    }
    if((g->optimize&&vx_constant(e,&type,&value))||e->kind==EX_WORD||e->kind==EX_REAL||e->kind==EX_BOOL) {
        if(!g->optimize) value=e->value;
        int id=temporary(g,e->type); fprintf(g->out,"UINT64_C(%" PRIu64 ");\n",value); ignore_temp(g,id); return id;
    }
    switch(e->kind) {
    case EX_FUNCTION: {
        int id=temporary(g,e->type); fprintf(g->out,"{{UINT64_C(%d)}};\n",e->id+1); ignore_temp(g,id); return id;
    }
    case EX_FN_COERCE: {
        int source=emit_expr(g,e->left); char base[64]; snprintf(base,sizeof base,"t_%d.cells",source);
        return load(g,e->type,base,0);
    }
    case EX_VEC:case EX_STRING:case EX_SLICE_SPLIT:case EX_MEMORY: {
        int values[3]={0},count=0;
        for(const Arg *a=e->args;a;a=a->next) { values[a->field]=emit_expr(g,a->expr); count+=vx_slots(g->p,a->expr->type); }
        int args=g->next++; indent(g); fprintf(g->out,"uint64_t vec_args_%d[%d] = {0};\n",args,count?count:1);
        char base[64]; snprintf(base,sizeof base,"vec_args_%d",args);
        for(const Arg *a=e->args;a;a=a->next) store(g,a->expr->type,base,vx_argument_offset(g->p,e,a->field),values[a->field]);
        int result=g->next++; indent(g); fprintf(g->out,"uint64_t vec_result_%d[%d] = {0};\n",result,vx_slots(g->p,e->kind==EX_MEMORY?e->operand_type:e->type));
        if(e->kind==EX_MEMORY) {
            indent(g); fprintf(g->out,"if (!vx_memory_exec(&vx_heap, %d, vec_args_%d, %d, ",e->op,args,vx_slots(g->p,e->operand_type));
            if(vx_owned(g->p,e->operand_type)) fprintf(g->out,"vx_owner_%d",e->operand_type); else fputs("NULL",g->out);
            fprintf(g->out,", vec_result_%d)) vx_host_error();\n",result);
        } else if(e->kind==EX_SLICE_SPLIT) {
            indent(g); fprintf(g->out,"vx_slice_split(vec_args_%d, %d, vec_result_%d);\n",args,vx_slots(g->p,e->operand_type),result);
        } else if(e->kind==EX_STRING) {
            indent(g); fprintf(g->out,"vx_text_exec(&vx_heap, %d, vec_args_%d, vec_result_%d);\n",e->op,args,result);
        } else {
            indent(g); fprintf(g->out,"vx_vec_exec(&vx_heap, %d, vec_args_%d, %d, ",e->op,args,vx_slots(g->p,e->operand_type));
            if(vx_owned(g->p,e->operand_type)) fprintf(g->out,"vx_owner_%d",e->operand_type); else fputs("NULL",g->out);
            fprintf(g->out,", vec_result_%d);\n",result);
        }
        if(e->type==TY_VOID) return -1;
        snprintf(base,sizeof base,"vec_result_%d",result); return load(g,e->type,base,0);
    }
    case EX_BOX_FN: {
        int value=emit_expr(g,e->left),result=initialized_temp(g,e->type);
        if(vx_box_fn(g->p,e->operand_type)) {
            indent(g); fprintf(g->out,"t_%d.cells[1] = t_%d.cells[0];\n",result,value); return result;
        }
        int payload=initialized_temp(g,e->id);
        indent(g);
        if(vx_callable(g->p,e->operand_type)) fprintf(g->out,"t_%d.cells[0] = t_%d.cells[0];\n",payload,value);
        else fprintf(g->out,"t_%d.cells[0] = %d;\n",payload,g->p->types[e->operand_type]->closure_target+1);
        indent(g); fprintf(g->out,"memcpy(t_%d.cells + 1, t_%d.cells, %d * sizeof(uint64_t));\n",payload,value,vx_slots(g->p,e->operand_type));
        indent(g); fprintf(g->out,"t_%d.cells[1] = vx_heap_box(&vx_heap, t_%d.cells, %d, ",result,payload,vx_slots(g->p,e->id));
        if(vx_owned(g->p,e->id)) fprintf(g->out,"vx_owner_%d",e->id); else fputs("NULL",g->out);
        fputs(");\n",g->out); indent(g); fprintf(g->out,"t_%d.cells[0] = t_%d.cells[1] ? 0 : 1;\n",result,result);
        return result;
    }
    case EX_BOX_NEW: {
        int value_id=emit_expr(g,e->args->expr),result=initialized_temp(g,e->type),address=g->next++;
        indent(g); fprintf(g->out,"uint64_t address_%d = vx_heap_box(&vx_heap, ",address);
        if(!vx_indirect(e->operand_type)) fprintf(g->out,"&t_%d",value_id); else fprintf(g->out,"t_%d.cells",value_id);
        fprintf(g->out,", %d, ",vx_slots(g->p,e->operand_type));
        if(vx_owned(g->p,e->operand_type)) fprintf(g->out,"vx_owner_%d",e->operand_type); else fputs("NULL",g->out);
        fputs(");\n",g->out);
        indent(g); fprintf(g->out,"t_%d.cells[0] = address_%d ? 0 : 1; t_%d.cells[1] = address_%d;\n",result,address,result,address);
        return result;
    }
    case EX_BOX_TAKE: {
        int owner=emit_expr(g,e->args->expr),result=g->next++;
        indent(g); fprintf(g->out,"uint64_t box_result_%d[%d];\n",result,vx_slots(g->p,e->type));
        indent(g); fprintf(g->out,"vx_heap_unbox(&vx_heap, t_%d.cells[0], %d, ",owner,vx_slots(g->p,e->type));
        if(vx_owned(g->p,e->type)) fprintf(g->out,"vx_owner_%d",e->type); else fputs("NULL",g->out);
        fprintf(g->out,", box_result_%d);\n",result);
        char base[64]; snprintf(base,sizeof base,"box_result_%d",result); return load(g,e->type,base,0);
    }
    case EX_BOX_REF: {
        int ref=emit_expr(g,e->args->expr),result=temporary(g,e->type);
        fprintf(g->out,"*(const uint64_t *)(uintptr_t)t_%d;\n",ref); ignore_temp(g,result); return result;
    }
    case EX_TRY: {
        int input=emit_expr(g,e->left); indent(g); fprintf(g->out,"if (t_%d.cells[0] != %d) {\n",input,e->id); ++g->indent;
        int result=initialized_temp(g,e->operand_type);
        Type error=g->p->types[e->left->type]->variants[1-e->id].fields[0];
        indent(g); fprintf(g->out,"t_%d.cells[0] = %d;\n",result,e->op);
        indent(g); fprintf(g->out,"memcpy(t_%d.cells + 1, t_%d.cells + 1, %d * sizeof(uint64_t));\n",result,input,vx_slots(g->p,error));
        adopt(g,e->operand_type,result,1,0);
        return_value(g,result); --g->indent; indent(g); fputs("}\n",g->out);
        char base[64]; snprintf(base,sizeof base,"t_%d.cells",input); return load(g,e->type,base,1);
    }
    case EX_STATIC: {
        int id=temporary(g,e->type); fputs("{{",g->out);
        for(int i=0;i<vx_slots(g->p,e->type);++i) fprintf(g->out,"%sUINT64_C(%" PRIu64 ")",i?",":"",e->cells[i]);
        fputs("}};\n",g->out); ignore_temp(g,id); return id;
    }
    case EX_REF: {
        int pointer=emit_place(g,e->left,true),id=temporary(g,e->type);
        fprintf(g->out,"(uint64_t)(uintptr_t)p_%d;\n",pointer); ignore_temp(g,id); return id;
    }
    case EX_DEREF: {
        int pointer=emit_place(g,e,true); char base[64];
        snprintf(base,sizeof base,"p_%d",pointer); return load(g,e->type,base,0);
    }
    case EX_BYTES: {
        int id=temporary(g,e->type); fputs("{{",g->out);
        if(!e->argc) fputs("0",g->out);
        for(int i=0;i<e->argc;++i) fprintf(g->out,"%s%u",i?",":"",(unsigned)e->bytes[i]);
        fputs("}};\n",g->out); ignore_temp(g,id); return id;
    }
    case EX_TEXT: {
        int data=g->next++; indent(g);
        fprintf(g->out,"static const uint64_t text_%d[%d] = {",data,e->argc?e->argc:1);
        if(!e->argc) fputs("0",g->out);
        for(int i=0;i<e->argc;++i) fprintf(g->out,"%s%u",i?",":"",(unsigned)e->cells[i]);
        fputs("};\n",g->out);
        int id=temporary(g,e->type);
        fprintf(g->out,"{{(uint64_t)(uintptr_t)text_%d, %d}};\n",data,e->argc);
        ignore_temp(g,id); return id;
    }
    case EX_STRING_INTO: {
        int source=emit_expr(g,e->args->expr); char base[64]; snprintf(base,sizeof base,"t_%d.cells",source);
        return load(g,e->type,base,0);
    }
    case EX_STRING_FROM: {
        int source=emit_expr(g,e->args->expr),result=initialized_temp(g,e->type);
        indent(g); fprintf(g->out,"vx_string_from_utf8(t_%d.cells[0], t_%d.cells);\n",source,result); return result;
    }
    case EX_UTF8: {
        int args[2]={0};
        for(const Arg *a=e->args;a;a=a->next) args[a->field]=emit_expr(g,a->expr);
        const Expr *input=vx_argument(e,0)->expr; int result=initialized_temp(g,e->type);
        indent(g); fprintf(g->out,"vx_utf8_exec(%d, ",e->op);
        if(vx_shared_slice(g->p,input->type)) fprintf(g->out,"(const uint64_t *)(uintptr_t)t_%d.cells[0], t_%d.cells[1]",args[0],args[0]);
        else fprintf(g->out,"t_%d.cells, %d",args[0],g->p->types[input->type]->count);
        if(e->op) fprintf(g->out,", t_%d",args[1]); else fputs(", 0",g->out);
        fprintf(g->out,", t_%d.cells);\n",result); return result;
    }
    case EX_PARSE_NUMBER: {
        const Expr *input=e->args->expr; int data=emit_expr(g,input),result=initialized_temp(g,e->type);
        indent(g); fputs(vx_extended(e->operand_type)?"vxn_ext_parse(":vx_float(e->operand_type)?"vx_parse_float(":"vx_parse_int(",g->out);
        if(vx_shared_slice(g->p,input->type)) fprintf(g->out,"(const uint64_t *)(uintptr_t)t_%d.cells[0], t_%d.cells[1]",data,data);
        else fprintf(g->out,"t_%d.cells, %d",data,g->p->types[input->type]->count);
        if(vx_float(e->operand_type)) fprintf(g->out,", %u, t_%d.cells);\n",vx_width(e->operand_type),result);
        else fprintf(g->out,", %u, %d, t_%d.cells);\n",vx_width(e->operand_type),vx_signed_type(e->operand_type),result);
        return result;
    }
    case EX_IO: {
        int args[3]={-1,-1,-1},n=0;
        for(Arg *a=e->args;a;a=a->next) { args[a->field]=emit_expr(g,a->expr); ++n; }
        if(e->op==VX_IO_READ_LINE) {
            int result=initialized_temp(g,e->type); indent(g);
            fprintf(g->out,"vx_io_line(&vx_io, &vx_heap, t_%d, (uint64_t *)(uintptr_t)t_%d, t_%d, t_%d.cells);\n",args[0],args[1],args[2],result);
            return result;
        }
        if(e->op==VX_IO_READ_BUFFER) {
            int result=initialized_temp(g,e->type); indent(g);
            fprintf(g->out,"vx_io_read_into(&vx_io, t_%d, (uint64_t *)(uintptr_t)t_%d.cells[0], t_%d.cells[1], t_%d.cells);\n",args[0],args[1],args[1],result);
            return result;
        }
        int result=g->next++; indent(g); fprintf(g->out,"uint64_t io_%d[2];\n",result);
        indent(g); fprintf(g->out,"vx_io_exec(&vx_io, %d, ",e->op);
        bool path=e->op>=VX_IO_OPEN_READ&&e->op<=VX_IO_OPEN_APPEND;
        if(n && !path) fprintf(g->out,"t_%d",args[0]); else fputs("0",g->out);
        if(path || e->op==VX_IO_WRITE) {
            const Arg *data=vx_argument(e,path?0:1);
            if(vx_slice(g->p,data->expr->type)) fprintf(g->out,", (const uint64_t *)(uintptr_t)t_%d.cells[0], (size_t)t_%d.cells[1], 0",args[path?0:1],args[path?0:1]);
            else fprintf(g->out,", t_%d.cells, %d, 0",args[path?0:1],g->p->types[data->expr->type]->count);
        } else if(n==2&&vx_extended(vx_argument(e,1)->expr->type)) {
            fprintf(g->out,", t_%d.cells, 0, 0",args[1]);
        } else {
            fputs(", NULL, 0, ",g->out);
            if(n==2) fprintf(g->out,"t_%d",args[1]); else fputs("0",g->out);
        }
        fprintf(g->out,", %u, io_%d);\n",(unsigned)e->value,result);
        char base[64]; snprintf(base,sizeof base,"io_%d",result);
        return load(g,e->type,base,0);
    }
    case EX_LIBRARY: {
        int values[6],cells=0;
        for(const Arg *a=e->args;a;a=a->next) {
            values[a->field]=emit_expr(g,a->expr); cells+=vx_slots(g->p,a->expr->type);
        }
        int args=g->next++; indent(g);
        fprintf(g->out,"uint64_t library_args_%d[%d] = {0};\n",args,cells?cells:1);
        char base[64]; snprintf(base,sizeof base,"library_args_%d",args);
        for(const Arg *a=e->args;a;a=a->next) {
            store(g,a->expr->type,base,vx_argument_offset(g->p,e,a->field),values[a->field]);
        }
        int result=initialized_temp(g,e->type); indent(g);
        fprintf(g->out,"vx_lib_exec(&vx_library, %d, library_args_%d, %" PRIu64 ", t_%d.cells);\n",e->op,args,e->value,result);
        return result;
    }
    case EX_VAR: {
        int capture=vx_capture(g->fn,e->id);
        if(capture>=0) {
            const Capture *c=&g->fn->captures[capture]; int id=load(g,e->type,"environment",c->offset);
            if(e->move) {
                adopt(g,e->type,id,0,0);
                if(vx_owned(g->p,e->type)) { indent(g); fprintf(g->out,"memset(environment + %d, 0, %d * sizeof(uint64_t));\n",c->offset,vx_slots(g->p,e->type)); }
            }
            return id;
        }
        int id=temporary(g,e->type); fprintf(g->out,"v_%d;\n",e->id); ignore_temp(g,id);
        if(e->move) {
            adopt(g,e->type,id,0,0);
            if(vx_owned(g->p,e->type)) { indent(g); fprintf(g->out,"memset(v_%d.cells, 0, sizeof v_%d.cells);\n",e->id,e->id); }
        }
        return id;
    }
    case EX_UNARY: { int a=emit_expr(g,e->left); return calc(g,e->op,e->operand_type,e->type,a,-1); }
    case EX_BINARY: {
        int a=emit_expr(g,e->left);
        if(e->op==VX_LAND||e->op==VX_LOR) {
            int id=temporary(g,TY_BOOL); fprintf(g->out,"t_%d;\n",a);
            indent(g); fprintf(g->out,"if (%st_%d) {\n",e->op==VX_LOR?"!":"",a); ++g->indent;
            int b=emit_expr(g,e->right); indent(g); fprintf(g->out,"t_%d = t_%d;\n",id,b);
            --g->indent; indent(g); fputs("}\n",g->out); ignore_temp(g,id); return id;
        }
        int b=emit_expr(g,e->right); return calc(g,e->op,e->operand_type,e->type,a,b);
    }
    case EX_CALL:case EX_INVOKE:case EX_CLOSURE_CALL:case EX_VIEW_CALL:case EX_BOX_CALL: {
        int target=(e->kind==EX_INVOKE||e->kind==EX_VIEW_CALL)?emit_expr(g,e->left):-1;
        int environment=-1,temporary_region=-1,boxed=-1; bool borrowed_environment=false;
        if(e->kind==EX_BOX_CALL) {
            boxed=e->op!=2&&vx_stable_place(g->p,e->left)?-1:emit_expr(g,e->left);
            int descriptor=boxed<0?emit_place(g,e->left,e->op==0):-1;
            target=g->next++; indent(g);
            if(boxed>=0) fprintf(g->out,"uint64_t *vx_boxcall_%d = (uint64_t *)(uintptr_t)t_%d.cells[0];\n",target,boxed);
            else fprintf(g->out,"uint64_t *vx_boxcall_%d = (uint64_t *)(uintptr_t)*p_%d;\n",target,descriptor);
        }
        if(e->kind==EX_CLOSURE_CALL) {
            borrowed_environment=e->op!=2&&vx_stable_place(g->p,e->left);
            environment=borrowed_environment?emit_place(g,e->left,e->op==0):emit_expr(g,e->left);
            if(!borrowed_environment&&vx_owned(g->p,e->operand_type)) {
                temporary_region=g->next++; indent(g);
                fprintf(g->out,"uint64_t vx_tregion_%d[4]; vx_heap_region_bind(&vx_heap, vx_tregion_%d, t_%d.cells, %d, 0);\n",temporary_region,temporary_region,environment,vx_slots(g->p,e->operand_type));
            }
            if(!g->p->functions[e->id]->capture_count) {
                if(borrowed_environment) { indent(g); fprintf(g->out,"(void)p_%d;\n",environment); }
                environment=-1;
            }
        }
        if(e->kind==EX_CALL&&e->left) (void)emit_expr(g,e->left);
        int args[VX_MAX_PARAMS],n=0;
        for(Arg *a=e->args;a;a=a->next) { args[a->field]=emit_expr(g,a->expr); ++n; }
        int id=-1;
        if(e->type!=TY_VOID) id=temporary(g,e->type); else indent(g);
        if(e->kind==EX_BOX_CALL) fprintf(g->out,"vx_dispatch_%d(vx_boxcall_%d[0], vx_boxcall_%d + 1",e->operand_type,target,target);
        else if(e->kind==EX_VIEW_CALL) fprintf(g->out,"vx_dispatch_%d(t_%d.cells[0], (uint64_t *)(uintptr_t)t_%d.cells[1]",e->operand_type,target,target);
        else if(e->kind==EX_INVOKE) fprintf(g->out,"vx_dispatch_%d(t_%d.cells[0]",e->operand_type,target);
        else fprintf(g->out,"vxfn_%d(",e->id);
        if(environment>=0) fprintf(g->out,borrowed_environment?"p_%d":"t_%d.cells",environment);
        for(int i=0;i<n;++i) fprintf(g->out,"%st_%d",i||target>=0||environment>=0?", ":"",args[i]);
        fputs(");\n",g->out); ignore_temp(g,id);
        if(temporary_region>=0) { indent(g); fprintf(g->out,"vx_heap_region_pop(&vx_heap, vx_tregion_%d);\n",temporary_region); }
        if(boxed>=0) { indent(g); fprintf(g->out,"vx_heap_destroy_value(&vx_heap, t_%d.cells, vx_owner_%d);\n",boxed,e->operand_type); }
        return id;
    }
    case EX_OUTPUT: {
        int a=emit_expr(g,e->args->expr); Type t=e->args->expr->type;
        indent(g); if(vx_extended(t)) fprintf(g->out,"vx_output_extended(t_%d.cells, %u);\n",a,vx_width(t));
        else fprintf(g->out,"vx_output(t_%d, %u, %d);\n",a,t==TY_BOOL?1:vx_width(t),t==TY_BOOL?3:vx_float(t)?2:vx_signed_type(t));
        return -1;
    }
    case EX_LENGTH: {
        (void)emit_expr(g,e->args->expr); int id=temporary(g,TY_WORD);
        fprintf(g->out,"UINT64_C(%" PRIu64 ");\n",e->value); ignore_temp(g,id); return id;
    }
    case EX_FN_VIEW: {
        int source=emit_expr(g,e->left),id=initialized_temp(g,e->type); indent(g);
        if(vx_box_fn(g->p,e->operand_type)) {
            fprintf(g->out,"uint64_t *vx_boxview_%d = (uint64_t *)(uintptr_t)*(uint64_t *)(uintptr_t)t_%d;\n",id,source);
            indent(g); fprintf(g->out,"t_%d.cells[0] = vx_boxview_%d[0]; t_%d.cells[1] = (uint64_t)(uintptr_t)(vx_boxview_%d + 1);\n",id,id,id,id);
            return id;
        }
        if(e->id>=0) fprintf(g->out,"t_%d.cells[0] = %d;\n",id,e->id+1);
        else fprintf(g->out,"t_%d.cells[0] = *(uint64_t *)(uintptr_t)t_%d;\n",id,source);
        if(e->id>=0&&g->p->functions[e->id]->capture_count) { indent(g); fprintf(g->out,"t_%d.cells[1] = t_%d;\n",id,source); }
        return id;
    }
    case EX_SLICE_BORROW:case EX_VIEW_BORROW: {
        int view=emit_expr(g,e->left); char base[64]; snprintf(base,sizeof base,"t_%d.cells",view); return load(g,e->type,base,0);
    }
    case EX_SLICE_LENGTH: {
        int view=emit_expr(g,e->args->expr),id=temporary(g,TY_WORD);
        fprintf(g->out,"t_%d.cells[1];\n",view); ignore_temp(g,id); return id;
    }
    case EX_SLICE: {
        int args[3]={0}; for(const Arg *a=e->args;a;a=a->next) args[a->field]=emit_expr(g,a->expr);
        bool borrowed=vx_ref(e->operand_type); Type base=borrowed?vx_referent(e->operand_type):e->operand_type;
        int length=temporary(g,TY_WORD);
        if(borrowed) fprintf(g->out,"%d;\n",g->p->types[base]->count); else fprintf(g->out,"t_%d.cells[1];\n",args[0]);
        int start=temporary(g,TY_WORD);
        if(e->argc==3) {
            Type st=vx_argument(e,1)->expr->type,ct=vx_argument(e,2)->expr->type;
            fprintf(g->out,"vx_range(t_%d, t_%d, t_%d, %u, %d, %u, %d);\n",args[1],args[2],length,vx_width(st),vx_signed_type(st),vx_width(ct),vx_signed_type(ct));
        } else fputs("0;\n",g->out);
        ignore_temp(g,length); ignore_temp(g,start);
        int id=initialized_temp(g,e->type); indent(g);
        fprintf(g->out,"t_%d.cells[0] = (uint64_t)(uintptr_t)((const uint64_t *)(uintptr_t)t_%d%s + t_%d * %d);\n",
            id,args[0],borrowed?"":".cells[0]",start,vx_slots(g->p,g->p->types[base]->element));
        indent(g); fprintf(g->out,"t_%d.cells[1] = t_%d;\n",id,e->argc==3?args[2]:length); return id;
    }
    case EX_PANIC:
        indent(g); fputs("vx_panic();\n",g->out);
        return e->type==TY_VOID?-1:initialized_temp(g,e->type);
    case EX_CAST: {
        int a=emit_expr(g,e->left); Type from=e->operand_type,to=e->type;
        if(vx_extended(from)||vx_extended(to)) {
            int result=initialized_temp(g,to); indent(g); fputs("vxn_ext_convert(",g->out);
            fprintf(g->out,vx_extended(from)?"t_%d.cells":"&t_%d",a);
            fprintf(g->out,", %u, %d, %u, %d, ",vx_width(from),vx_float(from)?2:vx_signed_type(from),vx_width(to),vx_float(to)?2:vx_signed_type(to));
            fprintf(g->out,vx_extended(to)?"t_%d.cells);\n":"&t_%d);\n",result); return result;
        }
        int id=temporary(g,e->type);
        fprintf(g->out,"vx_convert(t_%d, %u, %d, %u, %d);\n",a,vx_width(from),vx_float(from)?2:vx_signed_type(from),vx_width(to),vx_float(to)?2:vx_signed_type(to));
        ignore_temp(g,id); return id;
    }
    case EX_CLOSURE:case EX_RECORD:case EX_ARRAY:case EX_CHOICE: {
        int id=initialized_temp(g,e->type),offset=0,index=0; TypeDef *d=g->p->types[e->type];
        if(e->kind==EX_CHOICE) { indent(g); fprintf(g->out,"t_%d.cells[0] = %d;\n",id,e->id); offset=1; }
        char base[64]; snprintf(base,sizeof base,"t_%d.cells",id);
        for(Arg *a=e->args;a;a=a->next) {
            Type t=(e->kind==EX_RECORD||e->kind==EX_CLOSURE)?d->fields[a->field].type:e->kind==EX_ARRAY?d->element:d->variants[e->id].fields[index++];
            if(e->kind==EX_RECORD||e->kind==EX_CLOSURE) offset=d->fields[a->field].offset;
            int item=emit_expr(g,a->expr); store(g,t,base,offset,item); offset+=vx_slots(g->p,t);
        }
        return id;
    }
    case EX_FIELD:case EX_INDEX: {
        if(e->move) {
            int pointer=emit_place(g,e,false); char base[64]; snprintf(base,sizeof base,"p_%d",pointer);
            int id=load(g,e->type,base,0); adopt(g,e->type,id,0,0); indent(g);
            fprintf(g->out,"memset(p_%d, 0, %d * sizeof(uint64_t));\n",pointer,vx_slots(g->p,e->type));
            return id;
        }
        if(e->kind==EX_INDEX&&vx_slice(g->p,e->left->type)) {
            int pointer=emit_place(g,e,true); char base[64]; snprintf(base,sizeof base,"p_%d",pointer); return load(g,e->type,base,0);
        }
        int source=emit_expr(g,e->left); TypeDef *d=g->p->types[e->left->type]; char base[96]; int offset=0;
        if(e->kind==EX_FIELD) { snprintf(base,sizeof base,"t_%d.cells",source); offset=d->fields[e->id].offset; }
        else {
            int index=emit_expr(g,e->right),pointer=g->next++; indent(g);
            fprintf(g->out,"const uint64_t *p_%d = t_%d.cells + vx_bound(t_%d, %d, %u, %d) * %d;\n",pointer,source,index,d->count,vx_width(e->right->type),vx_signed_type(e->right->type),vx_slots(g->p,e->type));
            snprintf(base,sizeof base,"p_%d",pointer);
        }
        return load(g,e->type,base,offset);
    }
    case EX_MATCH:return emit_match(g,e->left,e->arms,e->type);
    default:return -1;
    }
}
static void emit_block(Emitter *g,const Stmt *s) {
    size_t end=s?s->scope_end:0,begin=s?s->token.start:0;
    for(;s;s=s->next) { switch(s->kind) {
    case ST_BIND: {
        int id=s->expr?emit_expr(g,s->expr):-1; indent(g);
        if(!s->mutable_binding&&!vx_owned(g->p,s->type)) fputs("const ",g->out);
        ctype(g->out,s->type); fprintf(g->out," v_%d = ",s->local);
        if(id>=0) fprintf(g->out,"t_%d;\n",id); else fputs(vx_indirect(s->type)?"{0};\n":"0;\n",g->out);
        indent(g); fprintf(g->out,"(void)v_%d;\n",s->local);
        if(id>=0) adopt(g,s->type,id,0,s->local+1);
        region(g,s->local);
        break;
    }
    case ST_ASSIGN: {
        int p=emit_place(g,s->target,false),prior=-1;
        if(s->op>=0) { char address[64]; snprintf(address,sizeof address,"p_%d",p); prior=load(g,s->type,address,0); }
        int value=emit_expr(g,s->expr);
        if(s->op>=0) value=calc(g,s->op,s->type,s->type,prior,value);
        if(vx_owned(g->p,s->type)&&(s->target->kind!=EX_VAR||vx_capture(g->fn,s->local)>=0)) {
            indent(g); fprintf(g->out,"if (!vx_heap_replace(&vx_heap, p_%d, t_%d.cells, %d, vx_owner_%d)) vx_host_error();\n",p,value,vx_slots(g->p,s->type),s->type);
            break;
        }
        if(vx_owned(g->p,s->type)) {
            if(s->target->kind==EX_VAR) drop(g,s->local+1);
            else { indent(g); fprintf(g->out,"vx_heap_destroy_value(&vx_heap, p_%d, vx_owner_%d);\n",p,s->type); }
        }
        adopt(g,s->type,value,0,s->local+1);
        char base[64]; snprintf(base,sizeof base,"p_%d",p); store(g,s->type,base,0,value); break;
    }
    case ST_EXPR:(void)emit_expr(g,s->expr);break;
    case ST_RETURN: {
        if(s->tail) {
            const Expr *e=s->expr; const Function *target=vx_call_contract(g->p,e);
            int args[VX_MAX_PARAMS],pos=0,dynamic=-1,environment=-1;
            bool closure=e->kind==EX_CLOSURE_CALL||e->kind==EX_BOX_CALL;
            if(closure) {
                bool stable=e->op!=2&&vx_stable_place(g->p,e->left);
                int value=stable?emit_place(g,e->left,e->op==0):emit_expr(g,e->left);
                environment=g->next++; indent(g);
                fprintf(g->out,"const uint64_t *vx_tail_environment_%d = ",environment);
                if(e->kind==EX_BOX_CALL) fprintf(g->out,stable?"(uint64_t *)(uintptr_t)*p_%d;\n":"(uint64_t *)(uintptr_t)t_%d.cells[0];\n",value);
                else fprintf(g->out,stable?"p_%d;\n":"t_%d.cells;\n",value);
                if(e->kind==EX_BOX_CALL) {
                    dynamic=g->next++; indent(g);
                    fprintf(g->out,"uint64_t vx_tail_target_%d = *vx_tail_environment_%d++;\n",dynamic,environment);
                }
            } else if(e->left) dynamic=emit_expr(g,e->left);
            for(const Arg *a=e->args;a;a=a->next) args[a->field]=emit_expr(g,a->expr);
            for(int i=0;i<target->param_count;++i) {
                Type t=target->params[i].type;
                store(g,t,"vx_args",pos,args[i]); pos+=vx_slots(g->p,t); adopt(g,t,args[i],1,0);
            }
            indent(g);
            if(closure) {
                int mode=vx_tail_environment_mode(g->p,g->fn,e);
                fprintf(g->out,"vx_context[0] = (uint64_t)(uintptr_t)vx_tail_environment_%d; vx_context[1] = %s;\n",environment,mode==1?"1":mode==2?"vx_owned_environment":"0");
                indent(g); fputs("if (vx_context[1]) vx_tail_capture(",g->out);
                if(e->kind==EX_BOX_CALL) fprintf(g->out,"(int)vx_tail_target_%d - 1",dynamic);
                else fprintf(g->out,"%d",e->id);
                fputs(", vx_context);\n",g->out);
            } else if(e->kind==EX_VIEW_CALL) fprintf(g->out,"vx_context[0] = t_%d.cells[1]; vx_context[1] = 0;\n",dynamic);
            else fputs("vx_context[0] = 0; vx_context[1] = 0;\n",g->out);
            indent(g); fprintf(g->out,"vx_leave(%d); return ",g->frame_cost);
            if(e->kind==EX_INVOKE||e->kind==EX_VIEW_CALL) fprintf(g->out,"(int)t_%d.cells[0] - 1;\n",dynamic);
            else if(e->kind==EX_BOX_CALL) fprintf(g->out,"(int)vx_tail_target_%d - 1;\n",dynamic);
            else fprintf(g->out,"%d;\n",e->id);
            break;
        }
        int id=s->expr?emit_expr(g,s->expr):-1;
        if(id>=0) adopt(g,g->fn->result,id,1,0);
        return_value(g,id);
        break;
    }
    case ST_IF: {
        Type t; uint64_t value;
        if(g->optimize&&vx_constant(s->expr,&t,&value)) {
            indent(g); fputs("{\n",g->out); ++g->indent; emit_block(g,value?s->body:s->otherwise);
            --g->indent; indent(g); fputs("}\n",g->out);
        } else {
            int id=emit_expr(g,s->expr); drop(g,0); indent(g); fprintf(g->out,"if (t_%d) {\n",id);
            ++g->indent; emit_block(g,s->body); --g->indent; indent(g); fputs("} else {\n",g->out);
            ++g->indent; emit_block(g,s->otherwise); --g->indent; indent(g); fputs("}\n",g->out);
        }
        break;
    }
    case ST_BLOCK:
        indent(g); fputs("{\n",g->out); ++g->indent; emit_block(g,s->body);
        --g->indent; indent(g); fputs("}\n",g->out); break;
    case ST_LOOP: {
        int label=g->label++; g->loop_stmts[g->loops]=s; g->loop_labels[g->loops++]=label;
        indent(g); fputs("{\n",g->out); ++g->indent;
        if(s->initial) emit_block(g,s->initial);
        indent(g); fputs("for (;;) {\n",g->out); ++g->indent;
        int condition=emit_expr(g,s->expr); drop(g,0); indent(g); fprintf(g->out,"if (!t_%d) break;\n",condition);
        emit_block(g,s->body); indent(g); fprintf(g->out,"goto vx_next_%d;\nvx_next_%d: ;\n",label,label);
        if(s->step) emit_block(g,s->step);
        --g->indent; indent(g); fputs("}\n",g->out); --g->indent; indent(g); fputs("}\n",g->out); --g->loops;
        end_scope(g,s->end,s->token.start); break;
    }
    case ST_BREAK:loop_cleanup(g);indent(g);fputs("break;\n",g->out);break;
    case ST_CONTINUE:loop_cleanup(g);indent(g);fprintf(g->out,"goto vx_next_%d;\n",g->loop_labels[g->loops-1]);break;
    case ST_MATCH:(void)emit_match(g,s->expr,s->arms,TY_VOID);break;
    }
    if(s->kind!=ST_RETURN&&s->kind!=ST_BREAK&&s->kind!=ST_CONTINUE) drop(g,0);
    }
    end_scope(g,end,begin);
}
static void signature(FILE *out,const Function *f,int id) {
    ctype(out,f->result); fprintf(out," vxfn_%d(",id);
    if(f->capture_count) fprintf(out,"%suint64_t *environment",f->call_mode==0?"const ":"");
    else if(!f->param_count) fputs("void",out);
    for(int i=0;i<f->param_count;++i) {
        if(i||f->capture_count) fputs(", ",out);
        ctype(out,f->params[i].type); fprintf(out," v_%d",i);
    }
    fputc(')',out);
}
int vxn_emit_c(const VxnProgram *p,FILE *out,int optimize) {
    if(!vxn_ok(p)) return 2;
    fputs("#if defined(__linux__) && !defined(_POSIX_C_SOURCE)\n#define _POSIX_C_SOURCE 200809L\n#endif\n",out);
    fputs("#ifndef _CRT_SECURE_NO_WARNINGS\n#define _CRT_SECURE_NO_WARNINGS\n#endif\n",out);
    fputs(vx_runtime_text,out); fputs(vx_extended_text,out);
    fputs("\ntypedef struct { uint64_t cells[2]; } vx_extended;\n",out);
    fputs("\n_Static_assert(sizeof(uintptr_t)<=sizeof(uint64_t), \"reference cells must hold pointers\");\n",out);
    fputs(vx_io_api_text,out); fputs(vx_io_text,out);
    fputs(vx_parse_text,out);
    fputs(vx_utf8_text,out);
    fputs(vx_heap_api_text,out); fputs(vx_heap_text,out); fputs(vx_text_text,out);
    fputs(vx_line_text,out);
    fputs(vx_library_api_text,out); fputs(vx_graphics_core_text,out);
    fputs(vx_platform_win32_text,out); fputs(vx_platform_linux_text,out);
    fputs(vx_platform_stub_text,out); fputs(vx_library_text,out);
    fputs("\n#include <inttypes.h>\n#include <stdio.h>\n#include <stdlib.h>\n"
        "VxIo vx_io;\n"
        "VxLibrary vx_library;\n"
        "VxHeap vx_heap;\n"
        "int vx_library_cleanup(void) { if(vx_lib_finish(&vx_library)) return 0; fputs(\"graphics cleanup failed\\n\",stderr); return 2; }\n"
        "_Noreturn void vx_panic(void) {\n"
        "    if (vx_library_cleanup()) exit(2);\n"
        "    if (vx_io_finish(&vx_io)) exit(2);\n"
        "    if (fputs(\"panic\\n\", stderr) == EOF || fflush(stderr)) exit(2);\n    exit(1);\n}\n"
        "_Noreturn void vx_host_error(void) {\n"
        "    (void)vx_library_cleanup();\n"
        "    (void)vx_io_finish(&vx_io); fputs(\"native resource limit exceeded\\n\", stderr); exit(2);\n}\n"
        "unsigned vx_call_depth, vx_stack_cells;\n"
        "void vx_enter(unsigned cells) { (void)vx_string_from_utf8; (void)vx_utf8_exec; (void)vx_parse_int; (void)vx_parse_float; (void)vx_heap_new; (void)vx_heap_box; (void)vx_vec_exec; (void)vx_heap_adopt; (void)vx_heap_finish; (void)vx_float_op; (void)vx_convert; (void)vx_io_init; (void)vx_io_exec; (void)vx_lib_init; (void)vx_lib_exec; if (vx_call_depth == 96 || cells > 32768 - vx_stack_cells) vx_host_error(); ++vx_call_depth; vx_stack_cells += cells; vx_heap_enter(&vx_heap); }\n"
        "void vx_leave(unsigned cells) { vx_heap_leave(&vx_heap); --vx_call_depth; vx_stack_cells -= cells; }\n"
        "uint64_t vx_calc(int op, uint64_t a, uint64_t b, unsigned width, bool sign, unsigned mode) {\n"
        "    bool ok = true; uint64_t value = vx_integer_op(op,a,b,width,sign,mode,&ok);\n"
        "    if (!ok) vx_panic();\n    return value;\n}\n"
        "uint64_t vx_range(uint64_t start, uint64_t count, uint64_t length, unsigned sw, bool ss, unsigned cw, bool cs) { if (!vx_slice_range_valid(start,count,length,sw,ss,cw,cs)) vx_panic(); return start; }\n"
        "uint64_t vx_bound(uint64_t index, uint64_t length, unsigned width, bool sign) {\n"
        "    if ((sign && vx_signed_value(index,width) < 0) || index >= length) vx_panic();\n    return index;\n}\n"
        "void vx_output_extended(const uint64_t *value, unsigned width) { char text[64]; vxn_ext_format(value,width,text); if(printf(\"%s\\n\",text)<0 || fflush(stdout)) exit(2); }\n"
        "void vx_output(uint64_t value, unsigned width, unsigned kind) {\n"
        "    int result;\n"
        "    if (kind == 3) result = fputs(value ? \"true\\n\" : \"false\\n\", stdout);\n"
        "    else if (kind == 2) result = printf(width == 32 ? \"%.9g\\n\" : \"%.17g\\n\", vx_float_value(value,width));\n"
        "    else if (kind == 1) result = printf(\"%\" PRId64 \"\\n\", vx_signed_value(value,width));\n"
        "    else result = printf(\"%\" PRIu64 \"\\n\", value);\n"
        "    if (result < 0 || fflush(stdout)) { (void)vx_library_cleanup(); (void)vx_io_finish(&vx_io); exit(2); }\n}\n\n",out);
    for(int i=TY_AGGREGATE;i<p->type_count;++i)
        fprintf(out,"typedef struct { uint64_t cells[%d]; } vx_type_%d;\n",p->types[i]->slots,i);
    for(int i=TY_AGGREGATE;i<p->type_count;++i) if(p->types[i]->owner_map) {
        fprintf(out,"const uint64_t vx_owner_%d[] = {",i);
        for(size_t j=0;j<p->types[i]->owner_map_count;++j) fprintf(out,"%s%" PRIu64,j?",":"",p->types[i]->owner_map[j]);
        fputs("};\n",out);
    }
    for(int i=0;i<p->function_count;++i) if(!p->functions[i]->comptime&&!p->functions[i]->generic_count) { signature(out,p->functions[i],i); fputs(";\n",out); }
    bool tails=false;
    for(int i=0;i<p->function_count;++i) if(p->functions[i]->tail_member) {
        tails=true; fprintf(out,"int vx_step_%d(uint64_t *vx_result, uint64_t *vx_args, uint64_t *vx_context);\n",i);
    }
    if(tails) {
        fprintf(out,"void vx_tail_capture(int target, uint64_t *context) {\n    uint64_t *source = (uint64_t *)(uintptr_t)context[0];\n    uint64_t *storage = context + 3 + context[2] * %d;\n    context[2] ^= 1;\n    (void)source; (void)storage;\n    switch (target) {\n",p->tail_environment_slots);
        for(int i=0;i<p->function_count;++i) if(p->functions[i]->tail_member) {
            const Function *f=p->functions[i];
            fprintf(out,"    case %d:\n",i);
            if(f->capture_count) {
                fprintf(out,"        memcpy(storage, source, %d * sizeof(uint64_t));\n",vx_slots(p,f->closure_type));
                if(vx_owned(p,f->closure_type)) fprintf(out,"        vx_heap_adopt(&vx_heap, storage, vx_owner_%d, 1, 0);\n",f->closure_type);
            }
            fputs("        break;\n",out);
        }
        fputs("    default: vx_host_error();\n    }\n    context[0] = (uint64_t)(uintptr_t)storage;\n}\n",out);
        fputs("void vx_tail_run(int target, uint64_t *result, uint64_t *args, uint64_t *context) {\n    while (target >= 0) {\n        switch (target) {\n",out);
        for(int i=0;i<p->function_count;++i) if(p->functions[i]->tail_member)
            fprintf(out,"        case %d: target = vx_step_%d(result, args, context); break;\n",i,i);
        fputs("        default: vx_host_error();\n        }\n    }\n}\n",out);
    }
    for(int t=TY_AGGREGATE;t<p->type_count;++t) if((vx_callable(p,t)||vx_fn_view(p,t)||vx_box_fn(p,t))&&p->types[t]->invoked&&!vx_abstract(p,t)) {
        const Function *contract=p->types[t]->signature; bool view=vx_fn_view(p,t)||vx_box_fn(p,t);
        ctype(out,contract->result); fprintf(out," vx_dispatch_%d(uint64_t target",t);
        if(view) fputs(", uint64_t *environment",out);
        for(int j=0;j<contract->param_count;++j) { fputs(", ",out); ctype(out,contract->params[j].type); fprintf(out," a_%d",j); }
        fputs(") {\n",out);
        if(view) fputs("    (void)environment;\n",out);
        for(int j=0;j<contract->param_count;++j) fprintf(out,"    (void)a_%d;\n",j);
        fputs("    switch (target) {\n",out);
        for(int i=0;i<p->function_count;++i) if(!p->functions[i]->generic_count&&(view?vx_callable_contract(p,p->functions[i],contract):vx_callable_target(p,p->functions[i],contract))) {
            fprintf(out,"    case %d: %svxfn_%d(",i+1,contract->result==TY_VOID?"":"return ",i);
            bool environment=view&&p->functions[i]->capture_count;
            if(environment) fputs("environment",out);
            for(int j=0;j<contract->param_count;++j) fprintf(out,"%sa_%d",j||environment?", ":"",j);
            fputs(contract->result==TY_VOID?"); return;\n":");\n",out);
        }
        fputs("    default: vx_host_error();\n    }\n}\n",out);
    }
    Emitter g={.p=p,.out=out,.indent=1,.optimize=optimize!=0};
    for(int i=0;i<p->function_count;++i) {
        Function *f=p->functions[i]; g.next=g.label=g.loops=0; g.frame_cost=f->stack_cost; g.fn=f;
        if(f->comptime||f->generic_count) continue;
        fputc('\n',out);
        if(f->tail_member) {
            signature(out,f,i);
            fprintf(out," {\n    uint64_t vx_args[%d], vx_result[%d] = {0};\n",p->tail_slots?p->tail_slots:1,vx_slots(p,f->result)?vx_slots(p,f->result):1);
            int pos=0;
            for(int j=0;j<f->param_count;++j) {
                Type t=f->params[j].type;
                if(vx_indirect(t)) fprintf(out,"    memcpy(vx_args + %d, v_%d.cells, %d * sizeof(uint64_t));\n",pos,j,vx_slots(p,t));
                else fprintf(out,"    vx_args[%d] = v_%d;\n",pos,j);
                pos+=vx_slots(p,t);
            }
            fprintf(out,"    uint64_t vx_context[%d] = {0};\n    vx_context[0] = (uint64_t)(uintptr_t)%s;\n    vx_tail_run(%d, vx_result, vx_args, vx_context);\n",3+2*p->tail_environment_slots,f->capture_count?"environment":"NULL",i);
            if(f->result!=TY_VOID) {
                fputs("    ",out); ctype(out,f->result); fputs(" result;\n",out);
                if(vx_indirect(f->result)) fprintf(out,"    memcpy(result.cells, vx_result, %d * sizeof(uint64_t));\n",vx_slots(p,f->result));
                else fputs("    result = vx_result[0];\n",out);
                fputs("    return result;\n",out);
            }
            fprintf(out,"}\nint vx_step_%d(uint64_t *vx_result, uint64_t *vx_args, uint64_t *vx_context) {\n    uint64_t *environment = (uint64_t *)(uintptr_t)vx_context[0];\n    int vx_owned_environment = (int)vx_context[1];\n    (void)environment; (void)vx_result; (void)vx_args; (void)vx_owned_environment;\n",i);
            pos=0;
            for(int j=0;j<f->param_count;++j) {
                Type t=f->params[j].type;
                fputs("    ",out); ctype(out,t); fprintf(out," v_%d;\n",j);
                if(vx_indirect(t)) fprintf(out,"    memcpy(v_%d.cells, vx_args + %d, %d * sizeof(uint64_t));\n",j,pos,vx_slots(p,t));
                else fprintf(out,"    v_%d = vx_args[%d];\n",j,pos);
                pos+=vx_slots(p,t);
            }
        } else { signature(out,f,i); fputs(" {\n",out); }
        fprintf(out,"    vx_enter(%d);\n",f->stack_cost);
        for(int j=0;j<f->param_count;++j) fprintf(out,"    (void)v_%d;\n",j);
        for(int j=0;j<f->param_count;++j) if(vx_owned(p,f->params[j].type))
            fprintf(out,"    vx_heap_adopt(&vx_heap, v_%d.cells, vx_owner_%d, 0, %d);\n",j,f->params[j].type,j+1);
        for(int j=0;j<f->param_count;++j) region(&g,j);
        if(f->call_mode==2) for(int j=0;j<f->capture_count;++j) {
            const Capture *capture=&f->captures[j];
            if(vx_owned(p,capture->type)) fprintf(out,"    vx_heap_adopt(&vx_heap, environment + %d, vx_owner_%d, 0, %d);\n",capture->offset,capture->type,capture->local+1);
            region(&g,capture->local);
        }
        if(f->tail_member&&f->call_mode!=2) for(int j=0;j<f->capture_count;++j) {
            const Capture *capture=&f->captures[j];
            if(!vx_owned(p,capture->type)) continue;
            int id=capture->local;
            fprintf(out,"    uint64_t vx_region_%d[4];\n    if (vx_owned_environment) {\n        vx_heap_adopt(&vx_heap, environment + %d, vx_owner_%d, 0, %d);\n        vx_heap_region_bind(&vx_heap, vx_region_%d, environment + %d, %d, %d);\n    }\n",id,capture->offset,capture->type,id+1,id,capture->offset,vx_slots(p,capture->type),id+1);
        }
        emit_block(&g,f->body);
        if(f->result==TY_VOID) return_value(&g,-1);
        else fputs("    vx_host_error();\n",out);
        fputs("}\n",out);
    }
    if(p->main_id>=0) fprintf(out,"\nint main(void) { (void)vx_io_exec; vx_io_init(&vx_io,stdin,stdout,stderr); vx_lib_init(&vx_library); vxfn_%d(); int status=vx_library_cleanup(); if (vx_io_finish(&vx_io)) { fputs(\"I/O cleanup failed\\n\",stderr); return 2; } return status; }\n",p->main_id);
    return ferror(out)?2:0;
}
