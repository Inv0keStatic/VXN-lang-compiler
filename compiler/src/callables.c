#include "internal.h"
#include <string.h>

static bool same_signature(const VxnProgram *p,const Function *a,const Function *b) {
    if(a->result!=b->result||a->param_count!=b->param_count||a->caps!=b->caps||a->call_mode!=b->call_mode||
       a->from_explicit!=b->from_explicit||a->from_count!=b->from_count) return false;
    for(int i=0;i<a->param_count;++i)
        if(a->params[i].type!=b->params[i].type||a->params[i].noescape!=b->params[i].noescape||
           !vx_same(p,a->params[i].name,b->params[i].name)) return false;
    for(int i=0;i<a->from_count;++i) if(!vx_same(p,a->from_names[i],b->from_names[i])) return false;
    return true;
}
Type vx_callable_type(VxnProgram *p,const Function *signature,Token at) {
    for(int i=TY_AGGREGATE;i<p->type_count;++i)
        if(vx_callable(p,i)&&same_signature(p,p->types[i]->signature,signature)) return i;
    if(p->type_count==VX_MAX_TYPES) vx_fail(p,at,"callable type limit exceeded");
    TypeDef *d=vx_alloc(p,sizeof *d); d->kind=TD_CALLABLE; d->name=at; d->slots=1;
    d->signature=vx_alloc(p,sizeof *d->signature);
    Function *f=d->signature; f->name=f->cap_token=at; f->caps=signature->caps;
    f->call_mode=signature->call_mode; f->result=signature->result; f->param_count=signature->param_count;
    memcpy(f->params,signature->params,(size_t)f->param_count*sizeof *f->params);
    for(int i=0;i<f->param_count;++i) { f->params[i].default_value=NULL; f->params[i].mutable_binding=false; }
    f->from_explicit=signature->from_explicit; f->from_count=signature->from_count;
    memcpy(f->from_names,signature->from_names,sizeof f->from_names);
    p->types[p->type_count]=d; return p->type_count++;
}
Type vx_fn_view_type(VxnProgram *p,Type contract,bool exclusive,Token at) {
    if(!vx_callable(p,contract)) vx_fail(p,at,"callable view requires an explicit callable contract");
    Function *f=p->types[contract]->signature;
    if(f->call_mode>(exclusive?1:0)) vx_fail(p,at,"borrowed callable view cannot provide the requested invocation mode");
    TypeKind kind=exclusive?TD_UNIQ_FN_VIEW:TD_FN_VIEW;
    for(int i=TY_AGGREGATE;i<p->type_count;++i)
        if(p->types[i]->kind==kind&&p->types[i]->element==contract) return i;
    if(p->type_count==VX_MAX_TYPES) vx_fail(p,at,"callable view type limit exceeded");
    TypeDef *d=vx_alloc(p,sizeof *d); d->kind=kind; d->name=at;
    d->element=contract; d->signature=f; d->slots=2;
    p->types[p->type_count]=d; return p->type_count++;
}
Type vx_dyn_fn_type(VxnProgram *p,Type contract,Token at) {
    if(!vx_callable(p,contract)) vx_fail(p,at,"DynFn requires an explicit callable contract");
    for(int t=TY_AGGREGATE;t<p->type_count;++t)
        if(vx_dyn_fn(p,t)&&p->types[t]->element==contract) return t;
    if(p->type_count==VX_MAX_TYPES) vx_fail(p,at,"dynamic callable type limit exceeded");
    TypeDef *d=vx_alloc(p,sizeof *d); d->kind=TD_DYN_FN; d->name=at;
    d->element=contract; d->signature=p->types[contract]->signature; d->slots=1;
    p->types[p->type_count]=d; return p->type_count++;
}
Type vx_box_fn_payload(VxnProgram *p,Type source,Token at) {
    /* Private layout; DynFn is only a contract marker. */
    for(int t=TY_AGGREGATE;t<p->type_count;++t) {
        TypeDef *d=p->types[t];
        if(d->kind==TD_STRUCT&&d->boxed_payload&&d->element==source) return t;
    }
    if(p->type_count==VX_MAX_TYPES) vx_fail(p,at,"boxed environment type limit exceeded");
    TypeDef *d=vx_alloc(p,sizeof *d); d->kind=TD_STRUCT; d->name=at;
    d->boxed_payload=true; d->element=source; d->count=2;
    d->fields[0]=(Field){.name=at,.type=TY_WORD,.offset=0};
    d->fields[1]=(Field){.name=at,.type=source,.offset=1};
    d->slots=1+vx_slots(p,source); d->state=2; d->resolved=p->type_count;
    if(d->slots>VX_MAX_SLOTS) vx_fail(p,at,"boxed environment exceeds 4096 scalar slots");
    p->types[p->type_count]=d; return p->type_count++;
}
void vx_check_view_contract(VxnProgram *p,Type source,Type view,Token at,bool complete) {
    const Function *a=p->types[source]->signature,*b=p->types[view]->signature;
    bool concrete=vx_function_item(p,source)||(vx_closure(p,source)&&!vx_opaque(p,source));
    if(!vx_callable_shape(p,a,b)||a->call_mode>b->call_mode)
        vx_fail(p,at,"callable does not satisfy the view's parameters, result or invocation mode");
    if(concrete) {
        if(complete&&!vx_callable_contract(p,a,b))
            vx_fail(p,at,"callable violates the borrowed view's effect or borrow-origin contract");
    } else if(((EF_ALL&~vx_forbidden(a->caps))&vx_forbidden(b->caps))||
              ((b->caps&CP_HOT)&&!(a->caps&CP_HOT))||(a->from_mask&~b->from_mask))
        vx_fail(p,at,"callable view would strengthen an unproved contract");
}
Type vx_closure_type(VxnProgram *p,int id,Token at) {
    Function *f=p->functions[id];
    if(f->closure_type) return f->closure_type;
    if(p->type_count==VX_MAX_TYPES) vx_fail(p,at,"closure type limit exceeded");
    TypeDef *d=vx_alloc(p,sizeof *d); d->kind=TD_CLOSURE; d->name=at;
    d->signature=f; d->closure_target=id; d->count=f->capture_count;
    for(int i=0;i<f->capture_count;++i) {
        Capture *capture=&f->captures[i]; capture->offset=d->slots;
        d->fields[i]=(Field){.name=capture->name,.type=capture->type,.offset=d->slots};
        d->slots+=vx_slots(p,capture->type);
        d->borrowed_captures|=vx_dependent(p,capture->type);
        d->exclusive_captures|=vx_exclusive(p,capture->type)||vx_exclusive_captures(p,capture->type);
    }
    if(d->slots>VX_MAX_SLOTS) vx_fail(p,at,"closure environment exceeds 4096 scalar slots");
    f->closure_type=p->type_count; d->resolved=f->closure_type; d->state=2;
    p->types[p->type_count++]=d; return f->closure_type;
}
Type vx_function_item_type(VxnProgram *p,int id,Token at) {
    Function *f=p->functions[id];
    if(f->item_type) return f->item_type;
    if(p->type_count==VX_MAX_TYPES) vx_fail(p,at,"function item type limit exceeded");
    TypeDef *d=vx_alloc(p,sizeof *d); d->kind=TD_FUNCTION_ITEM; d->name=f->name;
    d->signature=f; d->closure_target=id; d->slots=1; d->state=2;
    d->resolved=f->item_type=p->type_count; p->types[p->type_count++]=d;
    return f->item_type;
}
bool vx_callable_shape(const VxnProgram *p,const Function *source,const Function *target) {
    if(source->result!=target->result||source->param_count!=target->param_count) return false;
    for(int i=0;i<source->param_count;++i) {
        const Param *a=&source->params[i],*b=&target->params[i];
        if(a->type!=b->type) return false;
        if(b->name.length&&!vx_same(p,a->name,b->name)) return false;
        if(b->noescape&&!a->noescape) return false;
    }
    return true;
}
bool vx_callable_contract(const VxnProgram *p,const Function *source,const Function *target) {
    return (!source->generic_count||source->anonymous)&&!source->comptime&&source->call_mode<=target->call_mode&&vx_callable_shape(p,source,target)&&
        !(source->effects&vx_forbidden(target->caps))&&
        (!(target->caps&CP_HOT)||!source->dynamic_calls)&&
        !(source->from_mask&~target->from_mask);
}
bool vx_callable_target(const VxnProgram *p,const Function *source,const Function *target) {
    return !source->capture_count&&vx_callable_contract(p,source,target);
}
static void statements(VxnProgram *p,const Stmt *s);
static bool dynamic_statements(const VxnProgram *p,const Stmt *s);
static bool dynamic_expression(const VxnProgram *p,const Expr *e) {
    if(!e) return false;
    if(e->kind==EX_VIEW_CALL||e->kind==EX_BOX_CALL) return true;
    if(e->kind==EX_INVOKE) return !vx_abstract(p,e->operand_type)||!(p->types[e->operand_type]->signature->caps&CP_HOT);
    if((e->kind==EX_CALL||e->kind==EX_GENERIC_CALL||e->kind==EX_CLOSURE_CALL)&&p->functions[e->id]->dynamic_calls) return true;
    if(dynamic_expression(p,e->left)||dynamic_expression(p,e->right)) return true;
    for(const Arg *a=e->args;a;a=a->next) if(dynamic_expression(p,a->expr)) return true;
    for(const Arm *a=e->arms;a;a=a->next)
        if(dynamic_expression(p,a->guard)||dynamic_expression(p,a->expr)||dynamic_statements(p,a->body)) return true;
    return false;
}
static bool dynamic_statements(const VxnProgram *p,const Stmt *s) {
    for(;s;s=s->next) {
        if(dynamic_expression(p,s->expr)||dynamic_expression(p,s->target)||
           dynamic_statements(p,s->body)||dynamic_statements(p,s->otherwise)||
           dynamic_statements(p,s->initial)||dynamic_statements(p,s->step)) return true;
        for(const Arm *a=s->arms;a;a=a->next)
            if(dynamic_expression(p,a->guard)||dynamic_expression(p,a->expr)||dynamic_statements(p,a->body)) return true;
    }
    return false;
}
static void expression(VxnProgram *p,const Expr *e) {
    if(!e) return;
    if(e->kind==EX_BOX_FN) vx_check_view_contract(p,e->operand_type,e->call_contract,e->token,true);
    if(e->kind==EX_FN_VIEW) vx_check_view_contract(p,e->operand_type,e->type,e->token,true);
    if(e->kind==EX_FUNCTION) {
        const Function *source=p->functions[e->id],*target=p->types[e->type]->signature;
        if(!vx_callable_target(p,source,target))
            vx_fail(p,e->token,"function value violates its callable effect, parameter or borrow-origin contract");
    }
    if(e->kind==EX_FN_COERCE&&vx_function_item(p,e->left->type)&&vx_callable(p,e->type)) {
        if(!vx_callable_target(p,p->types[e->left->type]->signature,p->types[e->type]->signature))
            vx_fail(p,e->token,"function item violates its callable effect, parameter or borrow-origin contract");
    }
    if(e->kind==EX_GENERIC_CALL) vx_check_callable_arguments(p,p->functions[e->id],e->type_args,e->token,true);
    if(e->kind==EX_CALL&&p->functions[e->id]->instance) {
        const Function *instance=p->functions[e->id];
        vx_check_callable_arguments(p,p->functions[instance->instance_of],instance->instance_types,e->token,true);
    }
    expression(p,e->left); expression(p,e->right);
    for(const Arg *a=e->args;a;a=a->next) expression(p,a->expr);
    for(const Arm *a=e->arms;a;a=a->next) { expression(p,a->guard); expression(p,a->expr); statements(p,a->body); }
}
static void statements(VxnProgram *p,const Stmt *s) {
    for(;s;s=s->next) {
        expression(p,s->expr); expression(p,s->target);
        statements(p,s->body); statements(p,s->otherwise); statements(p,s->initial); statements(p,s->step);
        for(const Arm *a=s->arms;a;a=a->next) { expression(p,a->guard); expression(p,a->expr); statements(p,a->body); }
    }
}
void vx_check_callables(VxnProgram *p) {
    bool changed;
    do {
        changed=false;
        for(int i=0;i<p->function_count;++i) {
            Function *f=p->functions[i];
            if(!f->dynamic_calls&&dynamic_statements(p,f->body)) { f->dynamic_calls=true; changed=true; }
        }
    } while(changed);
    for(int i=0;i<p->function_count;++i)
        if((p->functions[i]->caps&CP_HOT)&&p->functions[i]->dynamic_calls)
            vx_fail(p,p->functions[i]->cap_token,"hot requires statically resolved call targets, including calls through helpers");
    for(int i=0;i<p->implementation_count;++i) {
        const Implementation *impl=&p->implementations[i]; const Constraint *contract=&p->constraints[impl->constraint];
        for(int j=0;j<impl->method_count;++j)
            if((contract->methods[j]->caps&CP_HOT)&&p->functions[impl->methods[j]]->dynamic_calls)
                vx_fail(p,p->functions[impl->methods[j]]->name,"hot constraint operation requires statically resolved call targets");
    }
    for(int i=TY_AGGREGATE;i<p->type_count;++i) if(vx_opaque(p,i)) {
        TypeDef *d=p->types[i];
        /* Keep the template contract; resolve hidden layout at monomorphization. */
        if(d->template_type&&vx_abstract(p,i)) continue;
        const Function *source=p->types[d->element]->signature,*target=d->signature;
        bool valid;
        if(vx_opaque(p,d->element))
            valid=vx_callable_shape(p,source,target)&&source->call_mode<=target->call_mode&&
                !((EF_ALL&~vx_forbidden(source->caps))&vx_forbidden(target->caps))&&
                (!(target->caps&CP_HOT)||(source->caps&CP_HOT))&&!(source->from_mask&~target->from_mask);
        else valid=vx_callable_contract(p,source,target);
        if(!valid)
            vx_fail(p,d->name,"opaque closure implementation violates its invocation, effect or borrow-origin contract");
    }
    for(int i=0;i<p->function_count;++i) statements(p,p->functions[i]->body);
}
