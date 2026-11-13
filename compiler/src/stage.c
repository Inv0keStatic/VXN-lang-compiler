#include "internal.h"
#include <string.h>

static bool data_type(const VxnProgram *p,Type t,unsigned char *known,bool abstract_data) {
    if(t>=TY_AGGREGATE&&p->types[t]->kind==TD_PARAMETER)
        return abstract_data&&p->types[t]->ownership==1&&!p->types[t]->callable_bound;
    if(vx_dyn_fn(p,t)||vx_slice(p,t)||vx_str(p,t)||vx_heap_type(p,t)||vx_invocable(p,t)) return false;
    if(t<TY_AGGREGATE) return t==TY_VOID||t==TY_BOOL||vx_integer(t)||vx_float(t);
    if(known[t]) return known[t]==2;
    known[t]=1;
    const TypeDef *d=p->types[t];
    if(d->kind==TD_ARRAY) { bool ok=data_type(p,d->element,known,abstract_data); known[t]=(unsigned char)(ok?2:3); return ok; }
    for(int i=0;i<d->count;++i) {
        if(d->kind==TD_STRUCT) { if(!data_type(p,d->fields[i].type,known,abstract_data)) { known[t]=3; return false; } }
        else for(int j=0;j<d->variants[i].count;++j) if(!data_type(p,d->variants[i].fields[j],known,abstract_data)) { known[t]=3; return false; }
    }
    known[t]=2; return true;
}
bool vx_stage_type(const VxnProgram *p,Type t) {
    unsigned char known[VX_MAX_TYPES]={0}; return data_type(p,t,known,false);
}
bool vx_stage_template_type(const VxnProgram *p,Type t) {
    unsigned char known[VX_MAX_TYPES]={0}; return data_type(p,t,known,true);
}
typedef struct { VxnProgram *p; bool all_locals,abstract_data; unsigned char locals[VX_MAX_LOCALS]; } Stage;
static void expression(Stage *s,const Expr *e);
static void statements(Stage *s,const Stmt *b);
static void prepare(VxnProgram *p,Function *f) {
    if(f->stage_state) return;
    if(++p->stage_depth>VX_MAX_DEPTH) vx_fail(p,f->name,"comptime call graph depth limit exceeded");
    vx_check_function(p,f); f->stage_state=1;
    for(int i=0;i<f->generic_count;++i) if(!vx_stage_template_type(p,f->generic_types[i]))
        vx_fail(p,f->name,"generic comptime evaluation requires Copy data parameters, not Move or callable bounds");
    if(!(f->generic_count?vx_stage_template_type(p,f->result):vx_stage_type(p,f->result))) vx_fail(p,f->name,"comptime cannot return references or resource handles");
    for(int i=0;i<f->local_count;++i) if(!(f->generic_count?vx_stage_template_type(p,f->local_types[i]):vx_stage_type(p,f->local_types[i])))
        vx_fail(p,f->local_names[i],"comptime evaluation of references and resource handles is not supported");
    Stage s={.p=p,.all_locals=true,.abstract_data=f->generic_count!=0}; statements(&s,f->body);
    f->stage_state=2; --p->stage_depth;
}
static void arms(Stage *s,const Arm *a) {
    for(;a;a=a->next) {
        for(int i=0;i<a->count;++i) s->locals[a->locals[i]]=1;
        expression(s,a->guard); expression(s,a->expr); statements(s,a->body);
        for(int i=0;i<a->count;++i) s->locals[a->locals[i]]=0;
    }
}
static void expression(Stage *s,const Expr *e) {
    if(!e) return;
    if(e->kind==EX_TRY&&!s->all_locals) vx_fail(s->p,e->token,"? cannot return from a standalone comptime expression; use a helper function");
    if(e->kind==EX_GENERIC_CALL||e->kind==EX_OPERATOR||e->kind==EX_METHOD) {
        if(!s->abstract_data) vx_fail(s->p,e->token,"comptime evaluation requires fully specialized call targets");
        for(int i=0;i<e->type_arg_count;++i) if(!vx_stage_template_type(s->p,e->type_args[i]))
            vx_fail(s->p,e->token,"comptime generic calls require reifiable data type arguments");
        if(e->kind==EX_GENERIC_CALL) prepare(s->p,s->p->functions[e->id]);
        else {
            const Function *contract=e->kind==EX_METHOD?vx_call_contract(s->p,e):s->p->constraints[e->value].methods[e->id];
            if((EF_ALL&~vx_forbidden(contract->caps))&(EF_IO|EF_ALLOC|EF_BLOCK|EF_WRITE))
                vx_fail(s->p,e->token,"comptime abstract operations require a contract excluding host effects");
        }
    }
    if(++s->p->stage_depth>256) vx_fail(s->p,e->token,"comptime validation depth limit exceeded");
    if(!(s->abstract_data?vx_stage_template_type(s->p,e->type):vx_stage_type(s->p,e->type))||e->kind==EX_REF||e->kind==EX_DEREF)
        vx_fail(s->p,e->token,"comptime evaluation of references and resource handles is not supported");
    if(e->kind==EX_VAR&&!s->all_locals&&!s->locals[e->id])
        vx_fail(s->p,e->token,"comptime expression depends on a runtime binding; use a comptime constant");
    if(e->kind==EX_MEMORY||e->kind==EX_OUTPUT||e->kind==EX_IO||e->kind==EX_LIBRARY||e->kind==EX_BOX_TAKE||e->kind==EX_BOX_NEW||e->kind==EX_BOX_REF||e->kind==EX_VEC||e->kind==EX_STRING)
        vx_fail(s->p,e->token,"comptime evaluation cannot perform I/O, allocation, timing or native library operations");
    if(e->kind==EX_CALL) prepare(s->p,s->p->functions[e->id]);
    expression(s,e->left); expression(s,e->right);
    for(Arg *a=e->args;a;a=a->next) expression(s,a->expr);
    arms(s,e->arms);
    --s->p->stage_depth;
}
static void statements(Stage *s,const Stmt *b) {
    if(!b) return;
    if(++s->p->stage_depth>256) vx_fail(s->p,b->token,"comptime validation depth limit exceeded");
    for(;b;b=b->next) {
        expression(s,b->expr); expression(s,b->target);
        statements(s,b->body); statements(s,b->otherwise); statements(s,b->initial); statements(s,b->step);
        arms(s,b->arms);
    }
    --s->p->stage_depth;
}
void vx_stage_value(VxnProgram *p,Function *f,Expr *e) {
    if(!vx_stage_type(p,e->type)||e->type==TY_VOID)
        vx_fail(p,e->token,"comptime constants require owned non-void scalar or aggregate data");
    Stage s={.p=p}; expression(&s,e);
    uint64_t *cells=vx_alloc(p,(size_t)vx_slots(p,e->type)*sizeof *cells);
    int status=vx_eval_static(p,f,e,cells);
    if(status) vx_fail(p,e->token,status==1?"comptime evaluation panicked":status==3?
        "comptime evaluation attempted a forbidden host operation":"comptime evaluation exceeded its resource budget");
    Token token=e->token; Type type=e->type;
    *e=(Expr){.token=token,.type=type,.value=cells[0],.cells=cells};
    e->kind=vx_indirect(type)?EX_STATIC:type==TY_BOOL?EX_BOOL:vx_float(type)?EX_REAL:EX_WORD;
    if(vx_float(type)&&!vx_extended(type)) e->real=vx_float_value(cells[0],vx_width(type));
}
void vx_check_stages(VxnProgram *p) {
    for(int i=0;i<p->function_count;++i) if(p->functions[i]->comptime) prepare(p,p->functions[i]);
}
