#include "internal.h"
#include <string.h>

/* Lower suspensions through existing choice, call, exchange and borrow checks. */
typedef struct { VxnProgram *p; Token at; size_t end; Type type; } Builder;
static Expr *expr(Builder *b,ExprKind kind,const char *name) {
    Expr *e=vx_alloc(b->p,sizeof *e); e->kind=kind;
    e->token=name?vx_synthetic(b->at,name):b->at; return e;
}
static Stmt *stmt(Builder *b,StmtKind kind) {
    Stmt *s=vx_alloc(b->p,sizeof *s); s->kind=kind; s->token=b->at;
    s->end=s->scope_end=b->end; return s;
}
static void argument(Builder *b,Expr *e,Expr *value) {
    Arg **tail=&e->args; while(*tail) tail=&(*tail)->next;
    *tail=vx_alloc(b->p,sizeof **tail); (*tail)->expr=value; (*tail)->field=e->argc++;
}
static Expr *variant(Builder *b,int id,Expr *value) {
    Expr *e=expr(b,EX_CHOICE,NULL); e->type=b->type;
    e->member=b->p->types[b->type]->variants[id].name;
    if(value) argument(b,e,value);
    return e;
}
static Arm *arm(Builder *b,int id,const char *name) {
    Arm *a=vx_alloc(b->p,sizeof *a); a->token=b->at; a->end=b->end;
    Pattern *p=vx_alloc(b->p,sizeof *p); a->pattern=p;
    p->kind=PAT_CONSTRUCTOR; p->type=b->type; p->token=b->at;
    p->constructor=b->p->types[b->type]->variants[id].name;
    if(name) {
        a->count=1; a->bindings[0].name=vx_synthetic(b->at,name); a->bindings[0].mutable_binding=id==0;
        p->count=1; p->children=vx_alloc(b->p,sizeof *p);
        p->children->kind=PAT_BIND; p->children->token=a->bindings[0].name;
    }
    return a;
}
int vx_lazy_helper(VxnProgram *p,Type type,bool borrow,const Function *context,Token at) {
    TypeDef *d=p->types[type]; int *cached=borrow?&d->lazy_force:&d->lazy_take;
    if(*cached) return *cached-1;
    if(p->function_count==VX_MAX_FUNCTIONS) vx_fail(p,at,"lazy forcing exceeds function budget (256)");
    Type result=d->variants[1].fields[0];
    Builder b={p,at,at.start+at.length+1,type};
    Function *f=vx_alloc(p,sizeof *f); f->name=f->cap_token=vx_synthetic(at,borrow?"$lazy_force":"$lazy_take");
    f->operator_code=-1; f->lazy_helper=true; f->end=b.end;
    f->result=borrow?vx_reference_type(p,result,at):result;
    f->param_count=1; f->params[0].name=vx_synthetic(at,"$pending");
    f->params[0].type=borrow?vx_exclusive_type(p,type,true,at):type;
    if(vx_abstract(p,type)) {
        f->generic_count=context->generic_count;
        memcpy(f->generic_types,context->generic_types,(size_t)f->generic_count*sizeof(Type));
    }
    if(borrow) { f->from_explicit=true; f->from_count=1; f->from_names[0]=f->params[0].name; }
    int id=p->function_count++; p->functions[id]=f; *cached=id+1;
    Stmt *match=stmt(&b,ST_MATCH);
    if(borrow) {
        Stmt *old=stmt(&b,ST_BIND); old->name=vx_synthetic(at,"$old"); old->type=type;
        old->expr=expr(&b,EX_CALL,"mem_replace");
        Expr *loan=expr(&b,EX_REF,NULL); loan->op=1;
        loan->left=expr(&b,EX_DEREF,NULL); loan->left->left=expr(&b,EX_VAR,"$pending");
        argument(&b,old->expr,loan);
        argument(&b,old->expr,variant(&b,2,NULL));
        old->next=match; f->body=old; match->expr=expr(&b,EX_VAR,"$old");
    } else { f->body=match; match->expr=expr(&b,EX_VAR,"$pending"); }
    Arm *pending=arm(&b,0,"$thunk"),*ready=arm(&b,1,"$value"),*empty=arm(&b,2,NULL);
    match->arms=pending; pending->next=ready; ready->next=empty;
    Expr *call=expr(&b,EX_CALL,"$thunk");
    for(int which=0;which<2;++which) {
        Arm *a=which?ready:pending; Expr *value=which?expr(&b,EX_VAR,"$value"):call;
        Stmt *s=stmt(&b,borrow?ST_ASSIGN:ST_RETURN); a->body=s;
        if(borrow) {
            s->op=-1; s->target=expr(&b,EX_DEREF,NULL); s->target->left=expr(&b,EX_VAR,"$pending");
            s->expr=variant(&b,1,value);
        } else s->expr=value;
    }
    /* Empty exists only under an exclusive loan; source cannot access or reenter it.
       Diverge defensively without a panic effect. */
    empty->body=stmt(&b,ST_LOOP); empty->body->expr=expr(&b,EX_BOOL,NULL); empty->body->expr->value=1;
    if(borrow) {
        Stmt *ret=stmt(&b,ST_RETURN); match->next=ret;
        ret->expr=expr(&b,EX_REF,NULL);
        Expr *field=expr(&b,EX_FIELD,NULL); field->member=vx_synthetic(at,"$ready_value");
        field->left=expr(&b,EX_DEREF,NULL); field->left->left=expr(&b,EX_VAR,"$pending");
        ret->expr->left=field;
    }
    vx_check_function(p,f); return id;
}
