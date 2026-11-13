#include "internal.h"
#include <string.h>

#define ORIGIN_WORDS ((2*VX_MAX_LOCALS+63)/64)
/* Storage roots and the backing carried by a value are distinct. */
#define BACKING VX_MAX_LOCALS
typedef struct { uint64_t bits[ORIGIN_WORDS]; } Origins;
typedef struct { Origins *locals; } BorrowState;
/* Flow-insensitive from/noescape and escape checks. Local loan liveness and
   conflicts are handled in exclusive.c. */
typedef struct {
    VxnProgram *p; Function *f; BorrowState *state;
    size_t *work; bool changed; Origins returned;
} Borrow;

static void work(Borrow *b,Token at) {
    if(!*b->work) vx_fail(b->p,at,"borrow analysis work limit exceeded");
    --*b->work;
}
static bool has(Origins a,int id) { return (a.bits[id/64]&(UINT64_C(1)<<(id%64)))!=0; }
static Origins singleton(int id) {
    Origins r={0}; r.bits[id/64]=UINT64_C(1)<<(id%64); return r;
}
static bool merge(Origins *a,Origins b) {
    bool changed=false;
    for(int i=0;i<ORIGIN_WORDS;++i) {
        uint64_t old=a->bits[i]; a->bits[i]|=b.bits[i]; changed|=old!=a->bits[i];
    }
    return changed;
}
static Origins expression(Borrow *b,const Expr *e);
static Origins place_origins(Borrow *b,const Expr *e) {
    work(b,e->token);
    if(e->kind==EX_VAR) return singleton(e->id);
    if(e->kind==EX_DEREF) return expression(b,e->left);
    if(e->kind==EX_INDEX&&vx_slice(b->p,e->left->type)) return expression(b,e->left);
    return place_origins(b,e->left);
}
static Origins expression(Borrow *b,const Expr *e) {
    Origins result={0};
    if(!e) return result;
    work(b,e->token);
    if(!vx_dependent(b->p,e->type)) return result;
    switch(e->kind) {
    case EX_VAR:return b->state->locals[e->id];
    case EX_REF:
        result=place_origins(b,e->left);
        if(vx_borrowed_closure(b->p,e->left->type)||vx_shared_aggregate(b->p,e->left->type)) (void)merge(&result,expression(b,e->left));
        return result;
    case EX_DEREF:case EX_FIELD:case EX_TRY:return expression(b,e->left);
    case EX_CLOSURE:case EX_RECORD:case EX_CHOICE:
        for(const Arg *a=e->args;a;a=a->next) (void)merge(&result,expression(b,a->expr));
        return result;
    case EX_SLICE_BORROW:case EX_VIEW_BORROW:case EX_FN_VIEW:return expression(b,e->left);
    case EX_SLICE:case EX_SLICE_SPLIT:return expression(b,vx_argument(e,0)->expr);
    case EX_VEC:case EX_STRING:return expression(b,vx_argument(e,0)->expr);
    case EX_BOX_REF:return expression(b,e->args->expr);
    case EX_MEMORY:return expression(b,vx_argument(e,0)->expr);
    case EX_CALL:case EX_GENERIC_CALL:case EX_METHOD:case EX_INVOKE:case EX_CLOSURE_CALL:case EX_VIEW_CALL:case EX_BOX_CALL: {
        const Function *callee=vx_call_contract(b->p,e);
        for(const Arg *a=e->args;a;a=a->next)
            if(callee->from_mask&(UINT32_C(1)<<a->field)) (void)merge(&result,expression(b,a->expr));
        break;
    }
    case EX_MATCH:
        for(const Arm *a=e->arms;a;a=a->next) (void)merge(&result,expression(b,a->expr));
        break;
    case EX_TEXT:case EX_PANIC:break;
    default:vx_fail(b->p,e->token,"internal error: unsupported reference expression");
    }
    return result;
}
static bool parameter_origin(const VxnProgram *p,const Function *f,int id) {
    return id<f->param_count&&vx_returned_borrow(p,f->params[id].type);
}
static bool tail_origin(const VxnProgram *p,const Function *f,int id) {
    if(id<f->param_count) return vx_dependent(p,f->params[id].type);
    int capture=vx_capture(f,id);
    return capture>=0&&(vx_borrowed(p,f->captures[capture].type)||vx_shared_aggregate(p,f->captures[capture].type));
}
static void collect_expression(Borrow *b,const Expr *e) {
    if(!e) return;
    work(b,e->token);
    collect_expression(b,e->left); collect_expression(b,e->right);
    for(const Arg *a=e->args;a;a=a->next) collect_expression(b,a->expr);
    if(e->kind==EX_TRY) {
        Type error=b->p->types[e->left->type]->variants[1-e->id].fields[0];
        if(vx_dependent(b->p,error)) (void)merge(&b->returned,expression(b,e->left));
    }
    for(const Arm *a=e->arms;a;a=a->next) {
        for(int i=0;i<a->count;++i) if(vx_dependent(b->p,a->bindings[i].type))
            b->changed|=merge(&b->state->locals[a->locals[i]],expression(b,e->left));
        collect_expression(b,a->guard); collect_expression(b,a->expr);
    }
}
static void collect(Borrow *b,const Stmt *s) {
    for(;s;s=s->next) {
        work(b,s->token);
        collect_expression(b,s->expr); collect_expression(b,s->target);
        if((s->kind==ST_BIND||s->kind==ST_ASSIGN)&&s->local>=0&&vx_dependent(b->p,s->type)&&s->expr)
            b->changed|=merge(&b->state->locals[s->local],expression(b,s->expr));
        if(s->kind==ST_RETURN&&vx_returned_borrow(b->p,b->f->result))
            (void)merge(&b->returned,expression(b,s->expr));
        collect(b,s->body); collect(b,s->otherwise); collect(b,s->initial); collect(b,s->step);
        for(const Arm *a=s->arms;a;a=a->next) {
            for(int i=0;i<a->count;++i) if(vx_dependent(b->p,a->bindings[i].type))
                b->changed|=merge(&b->state->locals[a->locals[i]],expression(b,s->expr));
            collect_expression(b,a->guard); collect_expression(b,a->expr); collect(b,a->body);
        }
    }
}
static void expression_lifetimes(Borrow *b,const Expr *e) {
    if(!e) return;
    work(b,e->token);
    expression_lifetimes(b,e->left); expression_lifetimes(b,e->right);
    for(const Arg *a=e->args;a;a=a->next) expression_lifetimes(b,a->expr);
    for(const Arm *a=e->arms;a;a=a->next) {
        expression_lifetimes(b,a->guard); expression_lifetimes(b,a->expr);
        if(!vx_dependent(b->p,e->type)) continue;
        Origins returned=expression(b,a->expr);
        for(int i=0;i<b->f->local_count;++i)
            if(has(returned,i)&&b->f->local_ends[i]<=a->end)
                vx_fail(b->p,a->token,"borrowed match result escapes an arm-local value");
    }
}
static void validate_body(Borrow *b,const Stmt *s) {
    for(;s;s=s->next) {
        work(b,s->token);
        expression_lifetimes(b,s->expr); expression_lifetimes(b,s->target);
        if(s->tail) {
            if(s->expr->kind==EX_VIEW_CALL||s->expr->kind==EX_CLOSURE_CALL||s->expr->kind==EX_BOX_CALL) {
                Origins origins=expression(b,s->expr->left);
                if(s->expr->kind!=EX_VIEW_CALL&&vx_tail_environment_mode(b->p,b->f,s->expr)==0)
                    (void)merge(&origins,place_origins(b,s->expr->left));
                for(int i=0;i<b->f->local_count;++i) if((has(origins,i)||(has(origins,BACKING+i)&&!tail_origin(b->p,b->f,i))))
                    vx_fail(b->p,s->expr->token,"tail callable environment borrows storage owned by the frame being replaced");
            }
            for(const Arg *a=s->expr->args;a;a=a->next) {
                Origins origins=expression(b,a->expr);
                for(int i=0;i<b->f->local_count;++i) if((has(origins,i)||(has(origins,BACKING+i)&&!tail_origin(b->p,b->f,i))))
                    vx_fail(b->p,a->expr->token,"tail argument borrows storage owned by the frame being replaced");
            }
        }
        validate_body(b,s->body); validate_body(b,s->otherwise); validate_body(b,s->initial); validate_body(b,s->step);
        for(const Arm *a=s->arms;a;a=a->next) {
            expression_lifetimes(b,a->guard); expression_lifetimes(b,a->expr); validate_body(b,a->body);
        }
    }
}
void vx_borrow_parameter(VxnProgram *p,Function *f,Param *a) {
    if(vx_fn_view(p,a->type)) a->noescape=true;
    if(vx_uniq(a->type)||vx_uniq_slice(p,a->type)||vx_split_result(p,a->type)) {
        bool returned=false;
        for(int i=0;i<f->from_count;++i) returned|=vx_same(p,a->name,f->from_names[i]);
        if(!returned) a->noescape=true;
    }
}
void vx_borrow_signature(VxnProgram *p,Function *f) {
    f->from_mask=0; f->from_inferred=false;
    if((vx_uniq(f->result)||vx_uniq_slice(p,f->result)||vx_split_result(p,f->result))&&!f->from_explicit)
        vx_fail(p,f->name,"exclusive returns require explicit from(...) returned-loan contracts");
    if((vx_shared_view(p,f->result)||vx_shared_aggregate(p,f->result))&&!f->from_explicit)
        vx_fail(p,f->name,vx_shared_aggregate(p,f->result)?"shared borrowed aggregate returns require explicit from(...)":vx_str(p,f->result)?"str returns require explicit from(...)":"shared Slice returns require explicit from(...)");
    uint32_t eligible=0;
    for(int i=0;i<f->param_count;++i) {
        Param *a=&f->params[i];
        vx_borrow_parameter(p,f,a);
        if(a->noescape&&!vx_dependent(p,a->type)) vx_fail(p,a->name,"noescape requires a reference parameter or Slice");
        if(vx_shared_result(p,a->type)&&!a->noescape) eligible|=UINT32_C(1)<<i;
    }
    if(f->from_explicit) {
        if(!vx_returned_borrow(p,f->result)) vx_fail(p,f->name,"from(...) requires a shared reference, exclusive borrow, Slice, str or borrowed aggregate result");
        for(int i=0;i<f->from_count;++i) {
            int id=0; while(id<f->param_count&&!vx_same(p,f->from_names[i],f->params[id].name)) ++id;
            if(id==f->param_count) vx_fail(p,f->from_names[i],"unknown from parameter");
            if(!parameter_origin(p,f,id)) vx_fail(p,f->from_names[i],"from origin must be a reference parameter, shared Slice, str or borrowed aggregate");
            if(f->params[id].noescape) vx_fail(p,f->from_names[i],"noescape parameter cannot appear in from(...)");
            if((vx_uniq(f->result)||vx_uniq_slice(p,f->result)||vx_split_result(p,f->result))&&!vx_exclusive(p,f->params[id].type))
                vx_fail(p,f->from_names[i],"exclusive result origins must be exclusive parameters");
            uint32_t bit=UINT32_C(1)<<id;
            if(f->from_mask&bit) vx_fail(p,f->from_names[i],"duplicate from origin");
            f->from_mask|=bit;
        }
    } else if(vx_ref(f->result)&&!vx_uniq(f->result)) {
        if(eligible && !(eligible&(eligible-1))) f->from_mask=eligible;
        else if(eligible&&f->public_api)
            vx_fail(p,f->name,"public borrowed result with multiple origins requires explicit from(...)");
        else f->from_inferred=true;
    }
}
void vx_check_borrows(VxnProgram *p) {
    bool needed=false;
    for(int i=0;i<p->function_count;++i) {
        Function *f=p->functions[i]; vx_borrow_signature(p,f);
        if(vx_returned_borrow(p,f->result)||f->uses_references) needed=true;
        for(int j=0;j<f->local_count;++j) if(vx_dependent(p,f->local_types[j])) needed=true;
    }
    if(!needed) return;
    for(int i=0;i<p->function_count;++i) {
        Function *f=p->functions[i]; BorrowState *s=vx_alloc(p,sizeof *s);
        f->borrow_state=s; s->locals=vx_alloc(p,(size_t)f->local_count*sizeof *s->locals);
        for(int j=0;j<f->param_count;++j) if(vx_dependent(p,f->params[j].type)) s->locals[j]=singleton(BACKING+j);
        for(int j=0;j<f->capture_count;++j) if(vx_dependent(p,f->captures[j].type))
            s->locals[f->captures[j].local]=singleton(BACKING+f->captures[j].local);
    }
    size_t budget=16u*1024u*1024u;
    bool changed;
    do {
        changed=false;
        for(int i=0;i<p->function_count;++i) {
            Function *f=p->functions[i];
            Borrow b={.p=p,.f=f,.state=f->borrow_state,.work=&budget}; collect(&b,f->body);
            changed|=b.changed;
            if(f->from_inferred) {
                uint32_t mask=f->from_mask;
                for(int j=0;j<f->param_count;++j) if(has(b.returned,BACKING+j)&&parameter_origin(p,f,j))
                    f->from_mask|=UINT32_C(1)<<j;
                changed|=mask!=f->from_mask;
            }
        }
    } while(changed);
    for(int i=0;i<p->function_count;++i) {
        Function *f=p->functions[i];
        Borrow b={.p=p,.f=f,.state=f->borrow_state,.work=&budget}; collect(&b,f->body);
        for(int j=0;j<f->local_count;++j) {
            if(has(b.returned,j)) vx_fail(p,f->name,"borrowed return escapes a local or by-value parameter");
            if(has(b.returned,BACKING+j)) {
                if(!parameter_origin(p,f,j)) vx_fail(p,f->name,"borrowed return escapes a local or by-value parameter");
                if(f->params[j].noescape) vx_fail(p,f->name,"borrowed return escapes a noescape parameter");
                if(!(f->from_mask&(UINT32_C(1)<<j))) vx_fail(p,f->name,"borrowed return origin is not permitted by from(...)");
            }
        }
        validate_body(&b,f->body);
        vx_check_exclusive(p,f);
    }
}
