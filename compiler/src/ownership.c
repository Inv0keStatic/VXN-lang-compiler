#include "internal.h"
#include <string.h>

static bool owned(const VxnProgram *p,Type t,unsigned char *known) {
    if(t<TY_AGGREGATE) return false;
    const TypeDef *d=p->types[t];
    if(d->lazy) return true;
    if(d->kind!=TD_PARAMETER&&vx_invocable(p,t)&&d->signature->call_mode==2) return true;
    if(vx_opaque(p,t)) return owned(p,d->element,known);
    if(vx_exclusive_captures(p,t)) return true;
    if(vx_heap_type(p,t)||(d->kind==TD_PARAMETER&&d->ownership==2)) return true;
    if(known[t]) return known[t]==2;
    known[t]=1;
    bool result=false;
    if(d->kind==TD_ARRAY) result=owned(p,d->element,known);
    if((d->kind==TD_STRUCT||d->kind==TD_CLOSURE)) {
        for(int i=0;i<d->count&&!result;++i) result=owned(p,d->fields[i].type,known);
    } else if(d->kind==TD_CHOICE) {
        for(int i=0;i<d->count&&!result;++i) for(int j=0;j<d->variants[i].count&&!result;++j)
            result=owned(p,d->variants[i].fields[j],known);
    }
    known[t]=(unsigned char)(result?2:3); return result;
}
bool vx_owned(const VxnProgram *p,Type t) {
    unsigned char known[VX_MAX_TYPES]={0}; return owned(p,t,known);
}
/* Abstract Move permits all non-panic cleanup effects; builtin owners use alloc/block. */
static unsigned cleanup(const VxnProgram *p,Type t,unsigned char *seen) {
    if(t<TY_AGGREGATE||seen[t]) return 0;
    seen[t]=1; const TypeDef *d=p->types[t]; unsigned effects=0;
    if(vx_dyn_fn(p,t)) return EF_ALL&~EF_PANIC;
    if(vx_opaque(p,t)) return cleanup(p,d->element,seen);
    if(d->kind==TD_PARAMETER) return d->ownership==2?(EF_ALL&~EF_PANIC):0;
    if(vx_heap_type(p,t)) effects=EF_ALLOC|EF_BLOCK;
    if(vx_heap_type(p,t)||d->kind==TD_ARRAY) effects|=cleanup(p,d->element,seen);
    if((d->kind==TD_STRUCT||d->kind==TD_CLOSURE)) for(int i=0;i<d->count;++i) effects|=cleanup(p,d->fields[i].type,seen);
    if(d->kind==TD_CHOICE) for(int i=0;i<d->count;++i) for(int j=0;j<d->variants[i].count;++j)
        effects|=cleanup(p,d->variants[i].fields[j],seen);
    return effects;
}
unsigned vx_cleanup_effects(const VxnProgram *p,Type t) {
    unsigned char seen[VX_MAX_TYPES]={0}; return cleanup(p,t,seen);
}
/* Trace owner(offset) or conditional(tag, value, words).
   Visit only the active choice payload; ignore padding. */
typedef struct { VxnProgram *p; uint64_t *data; size_t count; } Map;
static void word(Map *m,uint64_t x) {
    if(m->count==VX_MAX_NODES*8u) vx_fail(m->p,(Token){0},"owner layout tracing budget exceeded");
    if(m->data) m->data[m->count]=x;
    ++m->count;
}
static void trace(Map *m,Type t,int offset) {
    if(!vx_owned(m->p,t)) return;
    const TypeDef *d=m->p->types[t];
    if(vx_opaque(m->p,t)) { trace(m,d->element,offset); return; }
    if(vx_heap_type(m->p,t)) { word(m,1); word(m,(uint64_t)offset); }
    else if(d->kind==TD_ARRAY) for(int i=0;i<d->count;++i) trace(m,d->element,offset+i*vx_slots(m->p,d->element));
    else if((d->kind==TD_STRUCT||d->kind==TD_CLOSURE)) for(int i=0;i<d->count;++i) trace(m,d->fields[i].type,offset+d->fields[i].offset);
    else for(int i=0;i<d->count;++i) {
        word(m,2); word(m,(uint64_t)offset); word(m,(uint64_t)i); size_t length=m->count; word(m,0);
        int at=offset+1;
        for(int j=0;j<d->variants[i].count;++j) { Type f=d->variants[i].fields[j]; trace(m,f,at); at+=vx_slots(m->p,f); }
        if(m->data) m->data[length]=m->count-length-1;
    }
}
/* Track initialized cells per projection across joins and loop back-edges.
   Frames are capped at 16384 cells. */
#define OWN_WORDS (16384/64)
typedef struct { uint64_t live[OWN_WORDS]; } State;
typedef struct Loop Loop;
struct Loop { Loop *parent; State breaks,continues; bool has_break,has_continue; };
typedef struct { VxnProgram *p; Function *f; State state; Loop *loop; size_t work; bool guard; } Own;
static void merge(State *a,const State *b) { for(int i=0;i<OWN_WORDS;++i) a->live[i]&=b->live[i]; }
static void join(State *a,bool *present,const State *b) { if(*present) merge(a,b); else { *a=*b; *present=true; } }
static void expression(Own *o,Expr *e,bool consume);
static bool statements(Own *o,Stmt *s);
static void budget(Own *o,Token at) { if(!o->work--) vx_fail(o->p,at,"ownership analysis work limit exceeded"); }
static bool live(const State *s,int cell) { return (s->live[cell/64]&(UINT64_C(1)<<(cell%64)))!=0; }
static void set_range(State *s,int start,int count,bool value) {
    for(int i=start;i<start+count;++i) {
        uint64_t mask=UINT64_C(1)<<(i%64);
        if(value) s->live[i/64]|=mask; else s->live[i/64]&=~mask;
    }
}
static void set_local(Own *o,int id,bool value) {
    set_range(&o->state,o->f->offsets[id],vx_slots(o->p,o->f->local_types[id]),value);
}
static void available_range(Own *o,int id,int start,int count,Token at) {
    if(!vx_owned(o->p,o->f->local_types[id])&&!vx_exclusive(o->p,o->f->local_types[id])) return;
    for(int i=start;i<start+count;++i) if(!live(&o->state,i))
        vx_fail(o->p,at,"owner or exclusive handle may have been moved or left uninitialized (including a moved field)");
}
static void available(Own *o,int id,Token at) {
    available_range(o,id,o->f->offsets[id],vx_slots(o->p,o->f->local_types[id]),at);
}
static bool location(Own *o,const Expr *e,int *id,int *start) {
    if(e->kind==EX_VAR) { *id=e->id; *start=o->f->offsets[e->id]; return true; }
    if(e->kind!=EX_FIELD&&e->kind!=EX_INDEX) return false;
    if(!location(o,e->left,id,start)) return false;
    const TypeDef *d=o->p->types[e->left->type];
    if(e->kind==EX_FIELD) { *start+=d->fields[e->id].offset; return true; }
    Type t; uint64_t index;
    if(d->kind!=TD_ARRAY||!vx_constant(e->right,&t,&index)||index>=(uint64_t)d->count) return false;
    *start+=(int)index*vx_slots(o->p,d->element); return true;
}
static void consume_local(Own *o,Expr *e) {
    if(o->guard) vx_fail(o->p,e->token,"match guards cannot move owned values or exclusive handles");
    if(vx_capture(o->f,e->id)>=0) o->f->call_mode=2;
    set_local(o,e->id,false); e->move=true;
}
static void capture_access(Own *o,const Expr *e,int mode) {
    if(!e) return;
    if(e->kind==EX_VAR) {
        if(vx_capture(o->f,e->id)>=0&&o->f->call_mode<mode) o->f->call_mode=mode;
    } else if(e->kind==EX_FIELD||e->kind==EX_INDEX||e->kind==EX_DEREF) capture_access(o,e->left,e->kind==EX_VIEW_CALL&&vx_uniq_fn_view(o->p,e->operand_type)&&mode<1?1:mode);
}
static void bindings(Own *o,Arm *a) { for(int i=0;i<a->count;++i) set_local(o,a->locals[i],true); }
static void expression(Own *o,Expr *e,bool consume) {
    if(!e) return;
    budget(o,e->token);
    switch(e->kind) {
    case EX_VAR:
        available(o,e->id,e->token);
        if(consume&&(vx_owned(o->p,e->type)||vx_exclusive(o->p,e->type))) consume_local(o,e);
        return;
    case EX_CLOSURE_CALL:case EX_INVOKE:case EX_VIEW_CALL:case EX_BOX_CALL: {
        int mode=e->op;
        if(o->guard&&mode) vx_fail(o->p,e->token,"match guards require shared callable invocation");
        capture_access(o,e->left,e->kind==EX_VIEW_CALL&&vx_uniq_fn_view(o->p,e->operand_type)&&mode<1?1:mode);
        expression(o,e->left,mode==2);
        for(Arg *a=e->args;a;a=a->next) expression(o,a->expr,true);
        return;
    }
    case EX_SLICE_BORROW:case EX_VIEW_BORROW:case EX_REF:
        if(vx_exclusive(o->p,e->type)) capture_access(o,e->left,1);
        expression(o,e->left,false); return;
    case EX_DEREF:
        if(consume&&vx_owned(o->p,e->type)) vx_fail(o->p,e->token,"cannot move an owner out of a shared reference");
        if(consume&&vx_exclusive(o->p,e->type)) vx_fail(o->p,e->token,"cannot move an exclusive handle out of a shared reference");
        expression(o,e->left,false); return;
    case EX_FIELD:case EX_INDEX: {
        int id,start;
        if(location(o,e,&id,&start)) {
            int count=vx_slots(o->p,e->type); available_range(o,id,start,count,e->token);
            if(consume&&vx_owned(o->p,e->type)) {
                if(o->guard) vx_fail(o->p,e->token,"match guards cannot move owned payloads");
                capture_access(o,e,2); set_range(&o->state,start,count,false); e->move=true; }
            return;
        }
        if(consume&&vx_owned(o->p,e->type)) vx_fail(o->p,e->token,"partial owner moves require a local field or a constant in-bounds array index; borrowed storage cannot be moved");
        expression(o,e->left,false); expression(o,e->right,true); return;
    }
    case EX_SLICE_LENGTH:case EX_LENGTH:expression(o,e->args->expr,false); return;
    case EX_MATCH: {
        expression(o,e->left,true); State before=o->state,merged={0}; bool has=false;
        for(Arm *a=e->arms;a;a=a->next) {
            o->state=before; bindings(o,a); bool prior=o->guard; o->guard=true; expression(o,a->guard,true); o->guard=prior; expression(o,a->expr,true); join(&merged,&has,&o->state);
        }
        o->state=merged; return;
    }
    case EX_BINARY:
        expression(o,e->left,true);
        if(e->op==VX_LAND||e->op==VX_LOR) { State before=o->state; expression(o,e->right,true); merge(&o->state,&before); }
        else expression(o,e->right,true);
        return;
    default:break;
    }
    expression(o,e->left,true); expression(o,e->right,true);
    for(Arg *a=e->args;a;a=a->next) {
        expression(o,a->expr,true);
    }
}
static bool statements(Own *o,Stmt *s) {
    for(;s;s=s->next) {
        budget(o,s->token);
        switch(s->kind) {
        case ST_BIND:expression(o,s->expr,true); set_local(o,s->local,s->expr!=NULL); break;
        case ST_ASSIGN: {
            capture_access(o,s->target,1);
            int id,start; bool local=location(o,s->target,&id,&start);
            if(!local) {
                expression(o,s->target,false);
            } else if(s->op>=0) available_range(o,id,start,vx_slots(o->p,s->type),s->token);
            expression(o,s->expr,true);
            if(local) set_range(&o->state,start,vx_slots(o->p,s->type),true);
            break;
        }
        case ST_EXPR:expression(o,s->expr,true); break;
        case ST_RETURN:expression(o,s->expr,true); return false;
        case ST_BLOCK:if(!statements(o,s->body)) return false; break;
        case ST_IF: {
            expression(o,s->expr,true); State before=o->state;
            bool yes=statements(o,s->body); State after=o->state; o->state=before;
            bool no=statements(o,s->otherwise);
            if(yes&&no) merge(&o->state,&after); else if(yes) o->state=after;
            if(!yes&&!no) return false;
            break;
        }
        case ST_MATCH: {
            expression(o,s->expr,true); State before=o->state,merged={0}; bool has=false;
            for(Arm *a=s->arms;a;a=a->next) {
                o->state=before; bindings(o,a); bool prior=o->guard; o->guard=true; expression(o,a->guard,true); o->guard=prior; bool next=true;
                if(a->expr) expression(o,a->expr,true); else next=statements(o,a->body);
                if(next) join(&merged,&has,&o->state);
            }
            o->state=merged; if(!has) return false; break;
        }
        case ST_LOOP: {
            (void)statements(o,s->initial); State head=o->state;
            expression(o,s->expr,true); State exits=o->state;
            Loop *loop=vx_alloc(o->p,sizeof *loop); loop->parent=o->loop; o->loop=loop;
            bool back=statements(o,s->body);
            if(loop->has_continue) { if(back) merge(&o->state,&loop->continues); else o->state=loop->continues; back=true; }
            if(back) {
                (void)statements(o,s->step);
                for(int i=0;i<o->f->local_count;++i) if(vx_owned(o->p,o->f->local_types[i])||vx_exclusive(o->p,o->f->local_types[i])) {
                    int start=o->f->offsets[i],count=vx_slots(o->p,o->f->local_types[i]);
                    for(int j=start;j<start+count;++j) if(live(&head,j)&&!live(&o->state,j))
                        vx_fail(o->p,s->token,"owner is moved on a loop back-edge; reinitialize it or exit the loop");
                }
            }
            if(loop->has_break) merge(&exits,&loop->breaks);
            o->state=exits; o->loop=loop->parent; break;
        }
        case ST_BREAK:join(&o->loop->breaks,&o->loop->has_break,&o->state); return false;
        case ST_CONTINUE:join(&o->loop->continues,&o->loop->has_continue,&o->state); return false;
        }
    }
    return true;
}
void vx_check_function_ownership(VxnProgram *p,Function *f) {
    Own *o=vx_alloc(p,sizeof *o); o->p=p; o->f=f; o->work=16u*1024u*1024u;
    for(int j=0;j<f->param_count;++j) set_local(o,j,true);
    for(int j=0;j<f->capture_count;++j) set_local(o,f->captures[j].local,true);
    (void)statements(o,f->body);
}
void vx_check_ownership(VxnProgram *p) {
    bool needed=false;
    for(int i=TY_AGGREGATE;i<p->type_count;++i) if(vx_owned(p,i)) {
        TypeDef *d=p->types[i]; Map m={.p=p}; trace(&m,i,0); word(&m,0);
        d->owner_map_count=m.count; d->owner_map=vx_alloc(p,m.count*sizeof(uint64_t));
        m.data=d->owner_map; m.count=0; trace(&m,i,0); word(&m,0); needed=true;
    }
    for(int i=0;i<p->function_count&&!needed;++i)
        for(int j=0;j<p->functions[i]->local_count;++j)
            if(vx_exclusive(p,p->functions[i]->local_types[j])) needed=true;
    if(!needed) return;
    for(int i=0;i<p->function_count;++i) {
        vx_check_function_ownership(p,p->functions[i]);
    }
}
