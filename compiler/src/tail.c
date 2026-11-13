#include "internal.h"

int vx_parameter_slots(const VxnProgram *p,const Function *f) {
    int cells=0;
    for(int i=0;i<f->param_count;++i) cells+=vx_slots(p,f->params[i].type);
    return cells;
}
/* 0: borrowed storage outside the departing frame, 1: owned storage,
   2: a capture whose storage follows the incoming environment ownership. */
int vx_tail_environment_mode(const VxnProgram *p,const Function *f,const Expr *e) {
    if(e->op==2||!vx_stable_place(p,e->left)) return 1;
    const Expr *root=e->left;
    while(root->kind==EX_FIELD||(root->kind==EX_INDEX&&!vx_slice(p,root->left->type))) root=root->left;
    if(root->kind!=EX_VAR) return 0;
    if(vx_capture(f,root->id)<0) return 1;
    return f->call_mode==2?1:2;
}
static void statements(VxnProgram *p,Function *f,const Stmt *s);
static void expression(VxnProgram *p,Function *f,const Expr *e);
static void arms(VxnProgram *p,Function *f,const Arm *a) {
    for(;a;a=a->next) { expression(p,f,a->guard); expression(p,f,a->expr); statements(p,f,a->body); }
}
static void expression(VxnProgram *p,Function *f,const Expr *e) {
    if(!e) return;
    expression(p,f,e->left); expression(p,f,e->right);
    for(const Arg *a=e->args;a;a=a->next) expression(p,f,a->expr);
    arms(p,f,e->arms);
}
static void statements(VxnProgram *p,Function *f,const Stmt *s) {
    for(;s;s=s->next) {
        if(s->tail) {
            const Expr *e=s->expr;
            bool view=e->kind==EX_VIEW_CALL||e->kind==EX_BOX_CALL;
            bool indirect=e->kind==EX_INVOKE||view;
            const Function *target=vx_call_contract(p,e);
            if(target->result!=f->result) vx_fail(p,s->token,"tail target must have the same result type");
            if(!f->comptime) {
                f->tail_member=true;
                if(indirect) {
                    /* Use the same target contract filter as normal dispatch. */
                    for(int i=0;i<p->function_count;++i)
                        if(!p->functions[i]->generic_count&&(view?vx_callable_contract(p,p->functions[i],target):vx_callable_target(p,p->functions[i],target)))
                            p->functions[i]->tail_member=true;
                } else p->functions[e->id]->tail_member=true;
            }
        }
        expression(p,f,s->expr); expression(p,f,s->target);
        statements(p,f,s->body); statements(p,f,s->otherwise);
        statements(p,f,s->initial); statements(p,f,s->step); arms(p,f,s->arms);
    }
}
void vx_prepare_tails(VxnProgram *p) {
    for(int i=0;i<p->function_count;++i) if(!p->functions[i]->generic_count)
        statements(p,p->functions[i],p->functions[i]->body);
    /* Caller-owned arguments survive each step; never dispatch pointers into its frame. */
    for(int i=0;i<p->function_count;++i) if(p->functions[i]->tail_member) {
        int cells=vx_parameter_slots(p,p->functions[i]);
        if(cells>p->tail_slots) p->tail_slots=cells;
        const Function *f=p->functions[i];
        int environment=f->capture_count?vx_slots(p,f->closure_type):0;
        if(environment>p->tail_environment_slots) p->tail_environment_slots=environment;
    }
    for(int i=0;i<p->function_count;++i) if(p->functions[i]->tail_member) {
        Function *f=p->functions[i]; f->stack_cost+=p->tail_slots+2*p->tail_environment_slots+2*vx_slots(p,f->result)+20;
        if(f->stack_cost>16384) vx_fail(p,f->name,"tail dispatch storage exceeds the conservative native frame budget (16384 scalar slots)");
    }
}
