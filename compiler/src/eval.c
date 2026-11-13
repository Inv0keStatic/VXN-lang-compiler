#include "internal.h"
#include <inttypes.h>
#include <stdlib.h>
#include "../runtime/io.h"
#include "../runtime/heap.h"
#include "../runtime/line.h"
#include "../runtime/parse.h"
#include "../runtime/utf8.h"
#include "../runtime/text.h"
#include "../../stdlib/runtime/library.h"
_Static_assert(sizeof(uintptr_t)<=sizeof(uint64_t),"reference cells must hold pointers");
typedef struct { uint64_t scalar; uint64_t *cells; } Value;
typedef struct {
    const VxnProgram *p; FILE *out,*err; unsigned steps;
    int status,depth,calls,cells; size_t memory,used;
    uint64_t *scratch;
    bool comptime;
    unsigned *stage_steps;
    VxIo io;
    VxLibrary library;
    VxHeap heap;
} Runtime;
typedef struct { Function *function; uint64_t *locals,*result,*environment,*regions,*tail_environment; int signal,tail_target; bool owned_environment,tail_owned; Value tail_args[VX_MAX_PARAMS]; jmp_buf return_jump; } Frame;
enum { SIGNAL_NONE, SIGNAL_RETURN, SIGNAL_BREAK, SIGNAL_CONTINUE };
#define SCRATCH_SLOTS (1024u*1024u)
static Value eval(Runtime *r,Frame *f,const Expr *e);
static void statements(Runtime *r,Frame *f,const Stmt *s);
static Value zero(void) { return (Value){0}; }
static void resource_error(Runtime *r) {
    if(!r->status&&!r->comptime) fputs("interpreter resource limit exceeded\n",r->err);
    r->status=2;
}
static void panic(Runtime *r) {
    if(r->comptime) { if(!r->status) r->status=1; return; }
    if(!r->status && vx_io_finish(&r->io)) { r->status=2; return; }
    if(!r->status) r->status=fputs("panic\n",r->err)==EOF||fflush(r->err)?2:1;
}
static bool step(Runtime *r) {
    if(r->status) return false;
    if(++r->steps>1000000u) { resource_error(r); return false; }
    if(r->stage_steps&&++*r->stage_steps>16000000u) { resource_error(r); return false; }
    return true;
}
static uint64_t *scratch(Runtime *r,size_t count) {
    if(count>SCRATCH_SLOTS-r->used) { resource_error(r); return NULL; }
    uint64_t *result=r->scratch+r->used; r->used+=count;
    memset(result,0,count*sizeof *result); return result;
}
static void save(const VxnProgram *p,Type t,uint64_t *destination,Value value) {
    if(vx_indirect(t)) memcpy(destination,value.cells,(size_t)vx_slots(p,t)*sizeof *destination);
    else *destination=value.scalar;
}
static Value read_value(Runtime *r,Type t,const uint64_t *source) {
    if(!vx_indirect(t)) return (Value){.scalar=*source};
    uint64_t *copy=scratch(r,(size_t)vx_slots(r->p,t));
    if(copy) memcpy(copy,source,(size_t)vx_slots(r->p,t)*sizeof *copy);
    return (Value){.cells=copy};
}
static void adopt(Runtime *r,Type t,Value value,int parent,int slot) {
    if(vx_owned(r->p,t)) vx_heap_adopt(&r->heap,value.cells,r->p->types[t]->owner_map,parent,(uint64_t)slot);
}
static void region(Runtime *r,Frame *f,int id) {
    Type t=f->function->local_types[id]; if(!vx_owned(r->p,t)) return;
    int capture=vx_capture(f->function,id);
    uint64_t *storage=capture>=0?f->environment+f->function->captures[capture].offset:f->locals+f->function->offsets[id];
    vx_heap_region_bind(&r->heap,f->regions+4*id,storage,(size_t)vx_slots(r->p,t),(uint64_t)id+1);
}
static void end_scope(Runtime *r,Frame *f,size_t end,size_t begin) {
    if(!end) return;
    for(int i=f->function->local_count-1;i>=0;--i) if(f->function->local_ends[i]==end&&f->function->local_names[i].start>=begin&&vx_owned(r->p,f->function->local_types[i]))
        { vx_heap_drop(&r->heap,(uint64_t)i+1,0); vx_heap_region_end(&r->heap,(uint64_t)i+1); }
}
static int execute_frame(Runtime *r,Frame *f) {
    switch(setjmp(f->return_jump)) {
    case 0:statements(r,f,f->function->body); return 0;
    case 1:return 1;
    case 2:return 2;
    default:resource_error(r); return 1;
    }
}
static void prepare_frame(Runtime *r,Frame *f,const Value *args) {
    const Function *fn=f->function;
    for(int i=0;i<fn->param_count;++i) {
        save(r->p,fn->params[i].type,f->locals+fn->offsets[i],args[i]);
        adopt(r,fn->params[i].type,args[i],0,i+1); region(r,f,i);
    }
    if(fn->call_mode==2||f->owned_environment) for(int i=0;i<fn->capture_count;++i) {
        const Capture *c=&fn->captures[i];
        adopt(r,c->type,(Value){.cells=f->environment+c->offset},0,c->local+1);
        region(r,f,c->local);
    }
}
static Value call(Runtime *r,int id,const Value *args,uint64_t *environment) {
    Value *incoming=NULL;
    size_t incoming_bytes=0;
    bool owned_environment=false; int bank=0;
    size_t mark=r->used; int depth=r->depth;
    for(;;) {
        if(r->calls==VX_MAX_DEPTH) { resource_error(r); break; }
        Function *fn=r->p->functions[id];
        if(fn->stack_cost>32768-r->cells) { resource_error(r); break; }
        size_t slots=(size_t)fn->local_slots+(size_t)vx_slots(r->p,fn->result)+1+(size_t)fn->local_count*4;
        size_t bytes=slots*sizeof(uint64_t)+sizeof(Frame);
        if(bytes>VX_MAX_MEMORY-r->memory) { resource_error(r); break; }
        Frame *f=calloc(1,sizeof *f); if(!f) { resource_error(r); break; }
        f->function=fn; f->environment=environment; f->owned_environment=owned_environment; f->locals=calloc(slots,sizeof *f->locals);
        if(!f->locals) { free(f); resource_error(r); break; }
        f->result=f->locals+fn->local_slots; f->regions=f->result+vx_slots(r->p,fn->result)+1;
        r->memory+=bytes; ++r->calls; r->cells+=fn->stack_cost;
        vx_heap_enter(&r->heap);
        prepare_frame(r,f,args);
        r->used=mark;
        int transfer=execute_frame(r,f);
        r->depth=depth;
        Value result=zero();
        if(transfer==2&&!r->status&&!incoming) {
            size_t count=VX_MAX_PARAMS*sizeof *incoming+2*(size_t)r->p->tail_environment_slots*sizeof(uint64_t);
            if(count>VX_MAX_MEMORY-r->memory||!(incoming=malloc(count))) resource_error(r);
            else { incoming_bytes=count; r->memory+=count; }
        }
        if(transfer==2&&!r->status) {
            id=f->tail_target; environment=f->tail_environment;
            const Function *target=r->p->functions[id];
            owned_environment=f->tail_owned;
            if(owned_environment&&target->capture_count) {
                uint64_t *storage=(uint64_t *)(incoming+VX_MAX_PARAMS)+(size_t)bank*r->p->tail_environment_slots;
                bank^=1;
                memcpy(storage,environment,(size_t)vx_slots(r->p,target->closure_type)*sizeof *storage);
                environment=storage;
                for(int i=0;i<target->capture_count;++i) {
                    const Capture *c=&target->captures[i];
                    adopt(r,c->type,(Value){.cells=environment+c->offset},1,0);
                }
            }
            for(int i=0;i<target->param_count;++i) {
                incoming[i]=f->tail_args[i]; adopt(r,target->params[i].type,incoming[i],1,0);
            }
            args=incoming;
        } else {
            r->used=mark;
            if(!r->status) { result=read_value(r,fn->result,f->result); adopt(r,fn->result,result,1,0); }
        }
        vx_heap_leave(&r->heap);
        --r->calls; r->memory-=bytes; r->cells-=fn->stack_cost; free(f->locals); free(f);
        if(transfer!=2||r->status) { r->memory-=incoming_bytes; free(incoming); return result; }
    }
    r->memory-=incoming_bytes; free(incoming); return zero();
}
static uint64_t operation(Runtime *r,int op,Type t,uint64_t a,uint64_t b) {
    if(vx_float(t)) return vx_float_op(op,a,b,vx_width(t));
    bool ok=true;
    uint64_t result=vx_integer_op(op,a,b,t==TY_BOOL?1:vx_width(t),vx_signed_type(t),vx_integer(t)?vx_mode(t):0,&ok);
    if(!ok) panic(r);
    return result;
}
static Value wide_operation(Runtime *r,int op,Type t,Value a,Value b) {
    uint64_t *result=scratch(r,2); if(!result) return zero();
    vxn_ext_op(vx_width(t),(unsigned)op,a.cells,b.cells?b.cells:a.cells,result);
    return op>=VX_EQ&&op<=VX_GE?(Value){.scalar=result[0]}:(Value){.cells=result};
}
static uint64_t *place(Runtime *r,Frame *f,const Expr *e) {
    if(e->kind==EX_VAR) {
        int capture=vx_capture(f->function,e->id);
        return capture>=0?f->environment+f->function->captures[capture].offset:f->locals+f->function->offsets[e->id];
    }
    if(e->kind==EX_DEREF) {
        Value reference=eval(r,f,e->left);
        return r->status?NULL:(uint64_t *)(uintptr_t)reference.scalar;
    }
    if(e->kind==EX_INDEX && vx_slice(r->p,e->left->type)) {
        Value view=eval(r,f,e->left); if(r->status) return NULL;
        Value index=eval(r,f,e->right); if(r->status) return NULL;
        Type it=e->right->type;
        if((vx_signed_type(it)&&vx_signed_value(index.scalar,vx_width(it))<0)||index.scalar>=view.cells[1]) { panic(r); return NULL; }
        return (uint64_t *)(uintptr_t)view.cells[0]+index.scalar*(uint64_t)vx_slots(r->p,e->type);
    }
    uint64_t *base=place(r,f,e->left); if(r->status) return NULL;
    TypeDef *d=r->p->types[e->left->type];
    if(e->kind==EX_FIELD) return base+d->fields[e->id].offset;
    Value index=eval(r,f,e->right); if(r->status) return NULL;
    Type it=e->right->type;
    if((vx_signed_type(it)&&vx_signed_value(index.scalar,vx_width(it))<0)||index.scalar>=(uint64_t)d->count) { panic(r); return NULL; }
    return base+index.scalar*(uint64_t)vx_slots(r->p,d->element);
}
static bool pattern_matches(Runtime *r,Frame *f,const Arm *a,const Pattern *p,const uint64_t *cells) {
    if(p->kind==PAT_OR) {
        for(const Pattern *child=p->children;child;child=child->next) if(pattern_matches(r,f,a,child,cells)) return true;
        return false;
    }
    if(p->kind==PAT_BIND) memcpy(f->locals+f->function->offsets[a->locals[p->binding]],cells+p->offset,(size_t)vx_slots(r->p,p->type)*sizeof(uint64_t));
    if(p->kind==PAT_RANGE) {
        uint64_t sign=vx_signed_type(p->type)?UINT64_C(1)<<(vx_width(p->type)-1):0,value=cells[p->offset]^sign;
        if(value<(p->value^sign)||value>(p->upper^sign)) return false;
    }
    if(p->kind==PAT_TEXT&&(cells[p->offset+1]!=(uint64_t)p->literal->argc||
       !vx_text_equal((const uint64_t *)(uintptr_t)cells[p->offset],p->literal->cells,(uint64_t)p->literal->argc))) return false;
    if(p->kind==PAT_LITERAL&&cells[p->offset]!=p->value) return false;
    if(p->kind==PAT_CONSTRUCTOR) {
        if(cells[p->offset]!=(uint64_t)p->variant) return false;
    }
    for(const Pattern *child=p->children;child;child=child->next) if(!pattern_matches(r,f,a,child,cells)) return false;
    return true;
}
static Arm *select_arm(Runtime *r,Frame *f,Type type,Value value,Arm *arms) {
    const uint64_t *cells=vx_indirect(type)?value.cells:&value.scalar;
    for(Arm *a=arms;a;a=a->next) {
        if(!pattern_matches(r,f,a,a->pattern,cells)) continue;
        if(a->guard) {
            Value guard=eval(r,f,a->guard); if(r->status) return NULL;
            if(!guard.scalar) continue;
        }
        /* Transfer ownership only after selection commits. */
        for(int i=0;i<a->count;++i) {
            adopt(r,a->bindings[i].type,(Value){.cells=f->locals+f->function->offsets[a->locals[i]]},0,a->locals[i]+1);
            region(r,f,a->locals[i]);
        }
        return a;
    }
    resource_error(r); return NULL;
}
/* Isolate scratch locals so debug/ASan recursion reaches the checked depth limit. */
static Value eval_vec(Runtime *r,Frame *f,const Expr *e) {
    if(r->comptime) { r->status=3; return zero(); }
    Value values[3]={{0}}; int count=0;
    for(const Arg *a=e->args;a;a=a->next) { values[a->field]=eval(r,f,a->expr); if(r->status) return zero(); count+=vx_slots(r->p,a->expr->type); }
    uint64_t *args=scratch(r,(size_t)(count?count:1)); if(!args) return zero();
    for(const Arg *a=e->args;a;a=a->next) save(r->p,a->expr->type,args+vx_argument_offset(r->p,e,a->field),values[a->field]);
    uint64_t *result=scratch(r,(size_t)vx_slots(r->p,e->kind==EX_MEMORY?e->operand_type:e->type)); if(!result) return zero();
    if(e->kind==EX_MEMORY) {
        if(!vx_memory_exec(&r->heap,e->op,args,(size_t)vx_slots(r->p,e->operand_type),vx_owned(r->p,e->operand_type)?r->p->types[e->operand_type]->owner_map:NULL,result)) resource_error(r);
    } else if(e->kind==EX_SLICE_SPLIT) vx_slice_split(args,(size_t)vx_slots(r->p,e->operand_type),result);
    else if(e->kind==EX_STRING) vx_text_exec(&r->heap,e->op,args,result);
    else vx_vec_exec(&r->heap,e->op,args,(size_t)vx_slots(r->p,e->operand_type),vx_owned(r->p,e->operand_type)?r->p->types[e->operand_type]->owner_map:NULL,result);
    return read_value(r,e->type,result);
}
static Value eval_unbox(Runtime *r,Frame *f,const Expr *e) {
    Value value=eval(r,f,e->args->expr); if(r->status) return zero();
    uint64_t *result=scratch(r,(size_t)vx_slots(r->p,e->type)); if(!result) return zero();
    vx_heap_unbox(&r->heap,value.cells[0],(size_t)vx_slots(r->p,e->type),vx_owned(r->p,e->type)?r->p->types[e->type]->owner_map:NULL,result);
    return read_value(r,e->type,result);
}
static Value eval_io(Runtime *r,Frame *f,const Expr *e) {
        if(r->comptime) { r->status=3; return zero(); }
        Value args[3]={{0}};
        for(Arg *a=e->args;a;a=a->next) { args[a->field]=eval(r,f,a->expr); if(r->status) return zero(); }
        if(e->op==VX_IO_READ_LINE) {
            uint64_t result[4]; vx_io_line(&r->io,&r->heap,args[0].scalar,(uint64_t *)(uintptr_t)args[1].scalar,args[2].scalar,result);
            return read_value(r,e->type,result);
        }
        if(e->op==VX_IO_READ_BUFFER) {
            uint64_t result[3]; vx_io_read_into(&r->io,args[0].scalar,(uint64_t *)(uintptr_t)args[1].cells[0],args[1].cells[1],result);
            return read_value(r,e->type,result);
        }
        bool path=e->op>=VX_IO_OPEN_READ&&e->op<=VX_IO_OPEN_APPEND;
        bool bytes=path||e->op==VX_IO_WRITE;
        const Arg *data=vx_argument(e,path?0:1);
        bool view=bytes&&vx_slice(r->p,data->expr->type);
        Value buffer=args[path?0:1];
        uint64_t result[2];
        vx_io_exec(&r->io,e->op,args[0].scalar,bytes?(view?(const uint64_t *)(uintptr_t)buffer.cells[0]:buffer.cells):data&&vx_extended(data->expr->type)?args[1].cells:NULL,
            bytes?(view?(size_t)buffer.cells[1]:(size_t)r->p->types[data->expr->type]->count):0,args[1].scalar,(unsigned)e->value,result);
        return read_value(r,e->type,result);
}
static Value eval_library(Runtime *r,Frame *f,const Expr *e) {
        if(r->comptime) { r->status=3; return zero(); }
        Value values[6]; int cells=0;
        for(Arg *a=e->args;a;a=a->next) {
            values[a->field]=eval(r,f,a->expr); if(r->status) return zero();
            cells+=vx_slots(r->p,a->expr->type);
        }
        uint64_t *args=scratch(r,(size_t)(cells?cells:1)); if(!args) return zero();
        for(Arg *a=e->args;a;a=a->next) {
            save(r->p,a->expr->type,args+vx_argument_offset(r->p,e,a->field),values[a->field]);
        }
        uint64_t *result=scratch(r,(size_t)vx_slots(r->p,e->type)); if(!result) return zero();
        vx_lib_exec(&r->library,e->op,args,(size_t)e->value,result);
        return (Value){.cells=result};
}
static Value eval_call(Runtime *r,Frame *f,const Expr *e) {
        int target=e->id; uint64_t *environment=NULL,*temporary_region=NULL; Value boxed=zero();
        if(e->kind==EX_BOX_CALL) {
            boxed=e->op!=2&&vx_stable_place(r->p,e->left)?read_value(r,e->operand_type,place(r,f,e->left)):eval(r,f,e->left);
            if(r->status) return zero();
            uint64_t *payload=(uint64_t *)(uintptr_t)boxed.cells[0];
            if(!payload||!payload[0]||payload[0]>(uint64_t)r->p->function_count) { resource_error(r); return zero(); }
            target=(int)payload[0]-1; environment=payload+1;
        } else if(e->kind==EX_INVOKE||e->kind==EX_VIEW_CALL) {
            Value callable=eval(r,f,e->left); if(r->status) return zero();
            uint64_t id=callable.cells[0];
            if(!id||id>(uint64_t)r->p->function_count) { resource_error(r); return zero(); }
            target=(int)id-1;
            if(e->kind==EX_VIEW_CALL) environment=(uint64_t *)(uintptr_t)callable.cells[1];
        } else if(e->kind==EX_CLOSURE_CALL) {
            environment=e->op!=2&&vx_stable_place(r->p,e->left)?place(r,f,e->left):eval(r,f,e->left).cells;
            if(r->status) return zero();
            if(vx_owned(r->p,e->operand_type)&&(e->op==2||!vx_stable_place(r->p,e->left))) {
                temporary_region=scratch(r,4); if(!temporary_region) return zero();
                vx_heap_region_bind(&r->heap,temporary_region,environment,(size_t)vx_slots(r->p,e->operand_type),0);
            }
        } else if(e->left) { (void)eval(r,f,e->left); if(r->status) return zero(); }
        /* Stage arguments in the arena to bound recursive C stack use. */
        size_t count=(size_t)r->p->functions[target]->param_count;
        size_t bytes=(count?count:1)*sizeof(Value);
        Value *args=(Value *)scratch(r,(bytes+sizeof(uint64_t)-1)/sizeof(uint64_t));
        if(!args) return zero();
        memset(args,0,bytes);
        for(Arg *a=e->args;a;a=a->next) { args[a->field]=eval(r,f,a->expr); if(r->status) return zero(); }
        Value result=call(r,target,args,environment);
        if(temporary_region) vx_heap_region_pop(&r->heap,temporary_region);
        if(e->kind==EX_BOX_CALL&&(e->op==2||!vx_stable_place(r->p,e->left)))
            vx_heap_destroy_value(&r->heap,boxed.cells,r->p->types[e->operand_type]->owner_map);
        return result;
    }
static Value eval_slice(Runtime *r,Frame *f,const Expr *e) {
        Value args[3]={{0}};
        for(const Arg *a=e->args;a;a=a->next) { args[a->field]=eval(r,f,a->expr); if(r->status) return zero(); }
        bool borrowed=vx_ref(e->operand_type); Type base=borrowed?vx_referent(e->operand_type):e->operand_type;
        uint64_t address=borrowed?args[0].scalar:args[0].cells[0];
        uint64_t length=borrowed?(uint64_t)r->p->types[base]->count:args[0].cells[1],start=0,count=length;
        if(e->argc==3) {
            Type st=vx_argument(e,1)->expr->type,ct=vx_argument(e,2)->expr->type;
            start=args[1].scalar; count=args[2].scalar;
            if(!vx_slice_range_valid(start,count,length,vx_width(st),vx_signed_type(st),vx_width(ct),vx_signed_type(ct))) { panic(r); return zero(); }
        }
        uint64_t *cells=scratch(r,2); if(!cells) return zero();
        cells[0]=(uint64_t)(uintptr_t)((uint64_t *)(uintptr_t)address+start*(uint64_t)vx_slots(r->p,r->p->types[base]->element));
        cells[1]=count; return (Value){.cells=cells};
    }
static Value eval_view(Runtime *r,Frame *f,const Expr *e) {
        Value source=eval(r,f,e->left); if(r->status) return zero();
        uint64_t *cells=scratch(r,2); if(!cells) return zero();
        if(vx_box_fn(r->p,e->operand_type)) {
            uint64_t *payload=(uint64_t *)(uintptr_t)*(uint64_t *)(uintptr_t)source.scalar;
            cells[0]=payload[0]; cells[1]=(uint64_t)(uintptr_t)(payload+1); return (Value){.cells=cells};
        }
        cells[0]=e->id>=0?(uint64_t)e->id+1:*(uint64_t *)(uintptr_t)source.scalar;
        cells[1]=e->id>=0&&r->p->functions[e->id]->capture_count?source.scalar:0;
        return (Value){.cells=cells};
    }
static Value eval_construct(Runtime *r,Frame *f,const Expr *e) {
        TypeDef *d=r->p->types[e->type]; uint64_t *cells=scratch(r,(size_t)d->slots);
        if(!cells) return zero();
        int offset=0,index=0;
        if(e->kind==EX_CHOICE) { cells[0]=(uint64_t)e->id; offset=1; }
        for(Arg *a=e->args;a;a=a->next) {
            Type t=(e->kind==EX_RECORD||e->kind==EX_CLOSURE)?d->fields[a->field].type:e->kind==EX_ARRAY?d->element:d->variants[e->id].fields[index++];
            if(e->kind==EX_RECORD||e->kind==EX_CLOSURE) offset=d->fields[a->field].offset;
            Value value=eval(r,f,a->expr); if(r->status) return zero();
            save(r->p,t,cells+offset,value); offset+=vx_slots(r->p,t);
        }
        return (Value){.cells=cells};
    }
static Value eval_box(Runtime *r,Frame *f,const Expr *e) {
        Value value=eval(r,f,e->args->expr); if(r->status) return zero();
        uint64_t address=vx_heap_box(&r->heap,!vx_indirect(e->operand_type)?&value.scalar:value.cells,(size_t)vx_slots(r->p,e->operand_type),vx_owned(r->p,e->operand_type)?r->p->types[e->operand_type]->owner_map:NULL);
        uint64_t cells[2]={address?0:1,address}; return read_value(r,e->type,cells);
    }
static Value eval_box_fn(Runtime *r,Frame *f,const Expr *e) {
    Value value=eval(r,f,e->left); if(r->status) return zero();
    if(vx_box_fn(r->p,e->operand_type)) {
        uint64_t cells[2]={0,value.cells[0]}; return read_value(r,e->type,cells);
    }
    uint64_t *payload=scratch(r,(size_t)vx_slots(r->p,e->id)); if(!payload) return zero();
    payload[0]=vx_callable(r->p,e->operand_type)?value.cells[0]:(uint64_t)r->p->types[e->operand_type]->closure_target+1;
    save(r->p,e->operand_type,payload+1,value);
    uint64_t address=vx_heap_box(&r->heap,payload,(size_t)vx_slots(r->p,e->id),r->p->types[e->id]->owner_map);
    uint64_t cells[2]={address?0:1,address}; return read_value(r,e->type,cells);
}
static Value eval_text(Runtime *r,const Expr *e) {
        /* Arena storage avoids enlarging recursive ASan frames. */
        uint64_t *view=scratch(r,2); if(!view) return zero();
        view[0]=(uint64_t)(uintptr_t)e->cells; view[1]=(uint64_t)e->argc;
        return (Value){.cells=view};
    }
static Value eval_string_from(Runtime *r,Frame *f,const Expr *e) {
        Value source=eval(r,f,e->args->expr); if(r->status) return zero();
        uint64_t *result=scratch(r,4); if(!result) return zero();
        vx_string_from_utf8(source.cells[0],result); return read_value(r,e->type,result);
    }
static Value eval_utf8(Runtime *r,Frame *f,const Expr *e) {
        Value values[2]={{0}};
        for(const Arg *a=e->args;a;a=a->next) { values[a->field]=eval(r,f,a->expr); if(r->status) return zero(); }
        const Expr *input=vx_argument(e,0)->expr; bool view=vx_shared_slice(r->p,input->type);
        const uint64_t *bytes=view?(const uint64_t *)(uintptr_t)values[0].cells[0]:values[0].cells;
        uint64_t length=view?values[0].cells[1]:(uint64_t)r->p->types[input->type]->count;
        uint64_t *result=scratch(r,3); if(!result) return zero();
        vx_utf8_exec(e->op,bytes,length,values[1].scalar,result); return read_value(r,e->type,result);
    }
static Value eval_parse_number(Runtime *r,Frame *f,const Expr *e) {
        const Expr *input=e->args->expr; Value data=eval(r,f,input); if(r->status) return zero();
        uint64_t *result=scratch(r,3); if(!result) return zero();
        bool view=vx_shared_slice(r->p,input->type);
        const uint64_t *bytes=view?(const uint64_t *)(uintptr_t)data.cells[0]:data.cells;
        uint64_t length=view?data.cells[1]:(uint64_t)r->p->types[input->type]->count;
        if(vx_extended(e->operand_type)) vxn_ext_parse(bytes,length,vx_width(e->operand_type),result);
        else if(vx_float(e->operand_type)) vx_parse_float(bytes,length,vx_width(e->operand_type),result);
        else vx_parse_int(bytes,length,vx_width(e->operand_type),vx_signed_type(e->operand_type),result);
        return read_value(r,e->type,result);
    }
static Value eval_output(Runtime *r,Frame *f,const Expr *e) {
        if(r->comptime) { r->status=3; return zero(); }
        Value value=eval(r,f,e->args->expr); if(r->status) return zero();
        Type t=e->args->expr->type; int written;
        if(t==TY_BOOL) written=fputs(value.scalar?"true\n":"false\n",r->out);
        else if(vx_extended(t)) { char text[64]; vxn_ext_format(value.cells,vx_width(t),text); written=fprintf(r->out,"%s\n",text); }
        else if(vx_float(t)) written=fprintf(r->out,t==TY_F32?"%.9g\n":"%.17g\n",vx_float_value(value.scalar,vx_width(t)));
        else if(vx_signed_type(t)) written=fprintf(r->out,"%" PRId64 "\n",vx_signed_value(value.scalar,vx_width(t)));
        else written=fprintf(r->out,"%" PRIu64 "\n",value.scalar);
        if(written<0||fflush(r->out)) r->status=2;
        return zero();
    }
static Value eval_cast(Runtime *r,Frame *f,const Expr *e) {
        Value a=eval(r,f,e->left); Type from=e->operand_type,to=e->type;
        if(!r->status&&(vx_extended(from)||vx_extended(to))) {
            uint64_t *result=scratch(r,2); if(!result) return zero();
            vxn_ext_convert(vx_extended(from)?a.cells:&a.scalar,vx_width(from),vx_float(from)?2:vx_signed_type(from),vx_width(to),vx_float(to)?2:vx_signed_type(to),result);
            return vx_extended(to)?(Value){.cells=result}:(Value){.scalar=result[0]};
        }
        return r->status?zero():(Value){.scalar=vx_convert(a.scalar,vx_width(from),vx_float(from)?2:vx_signed_type(from),vx_width(to),vx_float(to)?2:vx_signed_type(to))};
    }
static Value eval_inner(Runtime *r,Frame *f,const Expr *e) {
    switch(e->kind) {
    case EX_HOLE:case EX_GENERIC_CALL:case EX_OPERATOR:case EX_METHOD:resource_error(r); return zero();
    case EX_VEC:case EX_STRING:case EX_SLICE_SPLIT:case EX_MEMORY:return eval_vec(r,f,e);
    case EX_BOX_TAKE:return eval_unbox(r,f,e);
    case EX_BOX_FN:return eval_box_fn(r,f,e);
    case EX_BOX_NEW:return eval_box(r,f,e);
    case EX_BOX_REF: {
        Value value=eval(r,f,e->args->expr);
        return r->status?zero():(Value){.scalar=*(uint64_t *)(uintptr_t)value.scalar};
    }
    case EX_TRY: {
        Value input=eval(r,f,e->left); if(r->status) return zero();
        if(input.cells[0]==(uint64_t)e->id) return read_value(r,e->type,input.cells+1);
        Type error=r->p->types[e->left->type]->variants[1-e->id].fields[0];
        memset(f->result,0,(size_t)vx_slots(r->p,f->function->result)*sizeof *f->result);
        f->result[0]=(uint64_t)e->op;
        memcpy(f->result+1,input.cells+1,(size_t)vx_slots(r->p,error)*sizeof *f->result);
        longjmp(f->return_jump,1);
    }
    case EX_REAL:if(vx_extended(e->type)) return read_value(r,e->type,e->cells);
        /* fall through */
    case EX_WORD:case EX_BOOL:return (Value){.scalar=e->value};
    case EX_FUNCTION: { uint64_t id=(uint64_t)e->id+1; return read_value(r,e->type,&id); }
    case EX_FN_COERCE:return eval(r,f,e->left);
    case EX_STATIC:return read_value(r,e->type,e->cells);
    case EX_TEXT:return eval_text(r,e);
    case EX_REF: {
        uint64_t *pointer=place(r,f,e->left);
        return r->status?zero():(Value){.scalar=(uint64_t)(uintptr_t)pointer};
    }
    case EX_DEREF: {
        Value reference=eval(r,f,e->left);
        return r->status?zero():read_value(r,e->type,(const uint64_t *)(uintptr_t)reference.scalar);
    }
    case EX_BYTES: {
        uint64_t *cells=scratch(r,(size_t)vx_slots(r->p,e->type));
        if(!cells) return zero();
        for(int i=0;i<e->argc;++i) cells[i]=e->bytes[i];
        return (Value){.cells=cells};
    }
    case EX_IO:return eval_io(r,f,e);
    case EX_STRING_INTO:return eval(r,f,e->args->expr);
    case EX_STRING_FROM:return eval_string_from(r,f,e);
    case EX_UTF8:return eval_utf8(r,f,e);
    case EX_PARSE_NUMBER:return eval_parse_number(r,f,e);
    case EX_LIBRARY:return eval_library(r,f,e);
    case EX_VAR:return read_value(r,e->type,place(r,f,e));
    case EX_UNARY: {
        Value a=eval(r,f,e->left);
        return r->status?zero():vx_extended(e->operand_type)?wide_operation(r,e->op,e->operand_type,a,a):(Value){.scalar=operation(r,e->op,e->operand_type,a.scalar,0)};
    }
    case EX_BINARY: {
        Value a=eval(r,f,e->left); if(r->status) return zero();
        if(e->op==VX_LAND&&!a.scalar) return zero();
        if(e->op==VX_LOR&&a.scalar) return (Value){.scalar=1};
        Value b=eval(r,f,e->right);
        return r->status?zero():vx_extended(e->operand_type)?wide_operation(r,e->op,e->operand_type,a,b):(Value){.scalar=operation(r,e->op,e->operand_type,a.scalar,b.scalar)};
    }
    case EX_CALL:case EX_INVOKE:case EX_CLOSURE_CALL:case EX_VIEW_CALL:case EX_BOX_CALL:return eval_call(r,f,e);
    case EX_OUTPUT:return eval_output(r,f,e);
    case EX_PANIC:panic(r);return zero();
    case EX_LENGTH:
        (void)eval(r,f,e->args->expr); return (Value){.scalar=e->value};
    case EX_FN_VIEW:return eval_view(r,f,e);
    case EX_SLICE_BORROW:case EX_VIEW_BORROW:return eval(r,f,e->left);
    case EX_SLICE_LENGTH: {
        Value view=eval(r,f,e->args->expr); return r->status?zero():(Value){.scalar=view.cells[1]};
    }
    case EX_SLICE:return eval_slice(r,f,e);
    case EX_CAST:return eval_cast(r,f,e);
    case EX_CLOSURE:case EX_RECORD:case EX_ARRAY:case EX_CHOICE:return eval_construct(r,f,e);
    case EX_FIELD:case EX_INDEX: {
        if(e->kind==EX_INDEX && vx_slice(r->p,e->left->type)) {
            uint64_t *pointer=place(r,f,e); return r->status?zero():read_value(r,e->type,pointer);
        }
        Value base=eval(r,f,e->left); if(r->status) return zero();
        TypeDef *d=r->p->types[e->left->type]; size_t offset;
        if(e->kind==EX_FIELD) offset=(size_t)d->fields[e->id].offset;
        else {
            Value index=eval(r,f,e->right); if(r->status) return zero();
            Type it=e->right->type;
            if((vx_signed_type(it)&&vx_signed_value(index.scalar,vx_width(it))<0)||index.scalar>=(uint64_t)d->count) { panic(r); return zero(); }
            offset=(size_t)index.scalar*(size_t)vx_slots(r->p,e->type);
        }
        return read_value(r,e->type,base.cells+offset);
    }
    case EX_MATCH: {
        Value value=eval(r,f,e->left); if(r->status) return zero();
        Arm *arm=select_arm(r,f,e->left->type,value,e->arms);
        Value result=arm?eval(r,f,arm->expr):zero();
        if(arm) end_scope(r,f,arm->end,arm->token.start);
        return result;
    }
    }
    resource_error(r); return zero();
}
static Value eval(Runtime *r,Frame *f,const Expr *e) {
    if(!step(r)) return zero();
    if(r->depth==256) { resource_error(r); return zero(); }
    ++r->depth; Value value=eval_inner(r,f,e); --r->depth;
    if(!r->status&&e->move) {
        adopt(r,e->type,value,0,0);
        if(vx_owned(r->p,e->type)) {
            /* Movable projections have only field/constant-index addressing. */
            uint64_t *source=place(r,f,e);
            if(!r->status) memset(source,0,(size_t)vx_slots(r->p,e->type)*sizeof(uint64_t));
        }
    }
    return value;
}
static void tail_arguments(Runtime *r,Frame *f,const Expr *e) {
    int target=e->id;
    if(e->kind==EX_INVOKE||e->kind==EX_VIEW_CALL) {
        Value value=eval(r,f,e->left); if(r->status) return;
        uint64_t id=value.cells[0];
        if(!id||id>(uint64_t)r->p->function_count) { resource_error(r); return; }
        target=(int)id-1;
        if(e->kind==EX_VIEW_CALL) f->tail_environment=(uint64_t *)(uintptr_t)value.cells[1];
    } else if(e->kind==EX_CLOSURE_CALL||e->kind==EX_BOX_CALL) {
        uint64_t *storage=e->op!=2&&vx_stable_place(r->p,e->left)?place(r,f,e->left):eval(r,f,e->left).cells;
        if(r->status) return;
        if(e->kind==EX_BOX_CALL) {
            uint64_t *payload=(uint64_t *)(uintptr_t)storage[0];
            uint64_t id=payload[0];
            if(!id||id>(uint64_t)r->p->function_count) { resource_error(r); return; }
            target=(int)id-1; storage=payload+1;
        }
        f->tail_environment=storage;
        int mode=vx_tail_environment_mode(r->p,f->function,e);
        f->tail_owned=mode==1||(mode==2&&f->owned_environment);
    } else if(e->left) (void)eval(r,f,e->left);
    for(const Arg *a=e->args;a&&!r->status;a=a->next) f->tail_args[a->field]=eval(r,f,a->expr);
    if(!r->status) { f->tail_target=target; longjmp(f->return_jump,2); }
}
static void statements_inner(Runtime *r,Frame *f,const Stmt *s) {
    for(;s&&!r->status&&!f->signal;s=s->next) {
        if(!step(r)) return;
        size_t mark=r->used;
        switch(s->kind) {
        case ST_BIND:
            if(s->expr) {
                Value value=eval(r,f,s->expr);
                if(!r->status) { save(r->p,s->type,f->locals+f->function->offsets[s->local],value); adopt(r,s->type,value,0,s->local+1); }
            }
            if(!r->status) region(r,f,s->local);
            break;
        case ST_ASSIGN: {
            uint64_t *target=place(r,f,s->target); if(r->status) break;
            Value prior=s->op>=0?read_value(r,s->type,target):zero();
            Value value=eval(r,f,s->expr); if(r->status) break;
            if(s->op>=0) { if(vx_extended(s->type)) value=wide_operation(r,s->op,s->type,prior,value); else value.scalar=operation(r,s->op,s->type,prior.scalar,value.scalar); }
            if(!r->status) {
                if(vx_owned(r->p,s->type)&&(s->target->kind!=EX_VAR||vx_capture(f->function,s->local)>=0)) {
                    if(!vx_heap_replace(&r->heap,target,value.cells,(size_t)vx_slots(r->p,s->type),r->p->types[s->type]->owner_map)) resource_error(r);
                    break;
                }
                if(vx_owned(r->p,s->type)) {
                    if(s->target->kind==EX_VAR) vx_heap_drop(&r->heap,(uint64_t)s->local+1,0);
                    else vx_heap_destroy_value(&r->heap,target,r->p->types[s->type]->owner_map);
                }
                save(r->p,s->type,target,value); adopt(r,s->type,value,0,s->local+1);
            }
            break;
        }
        case ST_EXPR:(void)eval(r,f,s->expr);break;
        case ST_RETURN: {
            if(s->tail) {
                tail_arguments(r,f,s->expr);
                break;
            }
            if(s->expr) {
                Value value=eval(r,f,s->expr);
                if(!r->status) save(r->p,f->function->result,f->result,value);
            }
            f->signal=SIGNAL_RETURN; break;
        }
        case ST_IF: {
            Value condition=eval(r,f,s->expr); vx_heap_drop(&r->heap,0,0);
            if(!r->status) statements(r,f,condition.scalar?s->body:s->otherwise);
            break;
        }
        case ST_BLOCK:statements(r,f,s->body);break;
        case ST_LOOP:
            if(s->initial) statements(r,f,s->initial);
            while(!r->status&&!f->signal) {
                r->used=mark; Value condition=eval(r,f,s->expr); vx_heap_drop(&r->heap,0,0); if(r->status||!condition.scalar) break;
                r->used=mark; statements(r,f,s->body);
                if(f->signal==SIGNAL_RETURN) break;
                if(f->signal==SIGNAL_BREAK) { f->signal=SIGNAL_NONE; break; }
                if(f->signal==SIGNAL_CONTINUE) f->signal=SIGNAL_NONE;
                if(!r->status&&s->step) statements(r,f,s->step);
            }
            end_scope(r,f,s->end,s->token.start); break;
        case ST_BREAK:f->signal=SIGNAL_BREAK;break;
        case ST_CONTINUE:f->signal=SIGNAL_CONTINUE;break;
        case ST_MATCH: {
            Value value=eval(r,f,s->expr); if(r->status) break;
            Arm *arm=select_arm(r,f,s->expr->type,value,s->arms);
            if(arm) { if(arm->expr) (void)eval(r,f,arm->expr); else statements(r,f,arm->body); }
            if(arm) end_scope(r,f,arm->end,arm->token.start);
            break;
        }
        }
        if(!f->signal) vx_heap_drop(&r->heap,0,0);
        r->used=mark;
    }
}
static void statements(Runtime *r,Frame *f,const Stmt *s) {
    if(r->status) return;
    if(r->depth==256) { resource_error(r); return; }
    ++r->depth; statements_inner(r,f,s); --r->depth;
    if(s) end_scope(r,f,s->scope_end,s->token.start);
}
int vxn_run_io(const VxnProgram *p,FILE *in,FILE *out,FILE *err) {
    if(!vxn_ok(p)||!in||!out||!err) return 2;
    if(p->main_id<0) { fputs("execution requires a main function\n",err); return 2; }
    Runtime r={.p=p,.out=out,.err=err};
    vx_io_init(&r.io,in,out,err);
    vx_lib_init(&r.library);
    r.scratch=calloc(SCRATCH_SLOTS,sizeof *r.scratch);
    if(!r.scratch) { resource_error(&r); return 2; }
    (void)call(&r,p->main_id,NULL,NULL);
    vx_heap_finish(&r.heap);
    if(!vx_lib_finish(&r.library)) { fputs("graphics cleanup failed\n",err); r.status=2; }
    if(vx_io_finish(&r.io)) { fputs("I/O cleanup failed\n",err); r.status=2; }
    free(r.scratch); return r.status;
}
int vxn_run(const VxnProgram *p,FILE *out,FILE *err) { return vxn_run_io(p,stdin,out,err); }
int vx_eval_static(VxnProgram *p,Function *f,const Expr *e,uint64_t *out) {
    Runtime r={.p=p,.comptime=true,.stage_steps=&p->stage_steps};
    r.scratch=calloc(SCRATCH_SLOTS,sizeof *r.scratch);
    Frame frame={.function=f}; frame.locals=calloc((size_t)f->local_slots+1,sizeof *frame.locals);
    if(!r.scratch||!frame.locals) { free(r.scratch); free(frame.locals); return 2; }
    Value value=eval(&r,&frame,e);
    if(!r.status) save(p,e->type,out,value);
    free(frame.locals); free(r.scratch); return r.status;
}
