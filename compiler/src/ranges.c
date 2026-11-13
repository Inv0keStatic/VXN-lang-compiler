#include "internal.h"
#include "range_math.h"

/* Must-analysis: interval hulls and index/view relations valid on every path.
   Alias writes/unknown callees clear facts. Loops restart from conditions;
   fallthrough/continue join before updates. Capture operands in evaluation order.
   Retain runtime checks. */
#define RANGE_FACTS 64
typedef struct { int index,view; } Fact;
typedef struct { int local; Interval value; } Bound;
typedef struct { Fact facts[RANGE_FACTS]; Bound bounds[RANGE_FACTS]; int count,bound_count; } State;
typedef struct Loop Loop;
struct Loop { Loop *parent; State *continues; bool has_continue; };
typedef struct { VxnProgram *p; size_t work; State *pool[VX_MAX_DEPTH*4]; int used; Loop *loop; } Analysis;

static void tick(Analysis *a,Token at) {
    if(!a->work--) vx_fail(a->p,at,"range analysis work limit exceeded");
}
static State *state_copy(Analysis *a,const State *source) {
    if(a->used==VX_MAX_DEPTH*4) vx_fail(a->p,(Token){0},"range analysis state depth exceeded");
    State *result=a->pool[a->used];
    if(!result) result=a->pool[a->used]=vx_alloc(a->p,sizeof *result);
    ++a->used;
    if(source) *result=*source; else { result->count=0; result->bound_count=0; }
    return result;
}
static void clear(State *s) { s->count=s->bound_count=0; }
static Interval local_range(const State *s,int local,Type type) {
    for(int i=0;i<s->bound_count;++i) if(s->bounds[i].local==local) return s->bounds[i].value;
    return range_full(type);
}
static void remember(State *s,int local,Interval value) {
    if(!value.type) return;
    for(int i=0;i<s->bound_count;++i) if(s->bounds[i].local==local) { s->bounds[i].value=value; return; }
    if(s->bound_count<RANGE_FACTS) s->bounds[s->bound_count++]=(Bound){local,value};
}
static bool same(Fact a,Fact b) { return a.index==b.index&&a.view==b.view; }
static void add(State *s,Fact fact) {
    for(int i=0;i<s->count;++i) if(same(s->facts[i],fact)) return;
    if(s->count<RANGE_FACTS) s->facts[s->count++]=fact;
}
static void kill(State *s,int local) {
    for(int i=0;i<s->bound_count;) {
        if(s->bounds[i].local==local) s->bounds[i]=s->bounds[--s->bound_count]; else ++i;
    }
    for(int i=0;i<s->count;) {
        if(s->facts[i].index==local||s->facts[i].view==local) s->facts[i]=s->facts[--s->count];
        else ++i;
    }
}
static void intersect(State *s,const State *other) {
    for(int i=0;i<s->bound_count;) {
        Interval *value=&s->bounds[i].value; int j=0;
        while(j<other->bound_count&&other->bounds[j].local!=s->bounds[i].local) ++j;
        if(j==other->bound_count) { s->bounds[i]=s->bounds[--s->bound_count]; continue; }
        Interval incoming=other->bounds[j].value;
        if(incoming.lo<value->lo) value->lo=incoming.lo;
        if(incoming.hi>value->hi) value->hi=incoming.hi;
        value->nonzero&=incoming.nonzero; ++i;
    }
    for(int i=0;i<s->count;) {
        bool found=false;
        for(int j=0;j<other->count;++j) if(same(s->facts[i],other->facts[j])) { found=true; break; }
        if(found) ++i; else s->facts[i]=s->facts[--s->count];
    }
}
static bool pure_call(Analysis *a,const Expr *e);
static bool stable_guard(Analysis *a,const Expr *e) {
    if(!e) return true;
    tick(a,e->token);
    switch(e->kind) {
    case EX_CALL:case EX_GENERIC_CALL:case EX_METHOD:case EX_INVOKE:case EX_CLOSURE_CALL:case EX_VIEW_CALL:case EX_BOX_CALL:
        if(!pure_call(a,e)) return false;
        break;
    case EX_MEMORY:case EX_IO:case EX_LIBRARY:case EX_VEC:case EX_STRING:case EX_BOX_TAKE:case EX_STRING_INTO:
        return false;
    case EX_REF:case EX_SLICE_BORROW:case EX_VIEW_BORROW:
        if(vx_exclusive(a->p,e->type)) return false;
        break;
    default:break;
    }
    if(!stable_guard(a,e->left)||!stable_guard(a,e->right)) return false;
    for(const Arg *arg=e->args;arg;arg=arg->next) if(!stable_guard(a,arg->expr)) return false;
    for(const Arm *arm=e->arms;arm;arm=arm->next)
        if(!stable_guard(a,arm->guard)||!stable_guard(a,arm->expr)) return false;
    return true;
}
static Interval cast_range(const Expr *e,Interval source) {
    Interval result=range_full(e->type);
    if(!source.type||!result.type||e->op) return result;
    uint64_t low=vx_integer_cast(range_bits(source,source.lo),vx_width(source.type),vx_signed_type(source.type),vx_width(e->type));
    uint64_t high=vx_integer_cast(range_bits(source,source.hi),vx_width(source.type),vx_signed_type(source.type),vx_width(e->type));
    result.lo=range_rank(e->type,low); result.hi=range_rank(e->type,high); result.nonzero=source.nonzero;
    return range_finish(result);
}
/* Used only to refine guards already established as nonmutating. */
static Interval interval(Analysis *a,const State *s,const Expr *e) {
    if(!e) return (Interval){0};
    tick(a,e->token);
    Type t; uint64_t value;
    if(vx_constant(e,&t,&value)) return range_single(t,value);
    if(e->kind==EX_VAR) return local_range(s,e->id,e->type);
    if(e->kind==EX_LENGTH) return range_single(e->type,e->value);
    if(e->kind==EX_CAST) return cast_range(e,interval(a,s,e->left));
    if(e->kind==EX_BINARY||e->kind==EX_UNARY) {
        bool safe; return range_calculate(e->op,e->operand_type,interval(a,s,e->left),interval(a,s,e->right),&safe);
    }
    return range_full(e->type);
}
static int length_view(const Expr *e) {
    while(e->kind==EX_CAST&&!e->op&&vx_integer(e->type)&&vx_integer(e->operand_type)) e=e->left;
    return e->kind==EX_SLICE_LENGTH&&e->args->expr->kind==EX_VAR?e->args->expr->id:-1;
}
static void refine(Analysis *a,State *s,const Expr *e,bool truth) {
    if(!e||!stable_guard(a,e)) return;
    if(e->kind==EX_UNARY&&e->op==VX_NOT) { refine(a,s,e->left,!truth); return; }
    if(e->kind!=EX_BINARY) return;
    if((e->op==VX_LAND&&truth)||(e->op==VX_LOR&&!truth)) {
        refine(a,s,e->left,truth); refine(a,s,e->right,truth); return;
    }
    if(e->op<VX_EQ||e->op>VX_GE) return;
    const Expr *index=e->left,*upper=e->right; int op=e->op;
    if(index->kind!=EX_VAR&&upper->kind==EX_VAR) {
        index=e->right; upper=e->left;
        if(op==VX_LT) op=VX_GT; else if(op==VX_GT) op=VX_LT;
        else if(op==VX_LE) op=VX_GE; else if(op==VX_GE) op=VX_LE;
    }
    if(index->kind!=EX_VAR||!vx_integer(index->type)) return;
    if(!truth) {
        if(op==VX_EQ) op=VX_NE; else if(op==VX_NE) op=VX_EQ;
        else if(op==VX_LT) op=VX_GE; else if(op==VX_GE) op=VX_LT;
        else if(op==VX_LE) op=VX_GT; else op=VX_LE;
    }
    Interval current=local_range(s,index->id,index->type),other=interval(a,s,upper),next=current;
    if(other.type!=current.type) return;
    if(op==VX_LT) {
        if(!other.hi) return;
        if(next.hi>=other.hi) next.hi=other.hi-1;
        int view=length_view(upper); if(view>=0) add(s,(Fact){index->id,view});
    } else if(op==VX_LE) { if(next.hi>other.hi) next.hi=other.hi; }
    else if(op==VX_GT) {
        if(other.lo==vx_mask(vx_width(other.type))) return;
        if(next.lo<=other.lo) next.lo=other.lo+1;
    } else if(op==VX_GE) { if(next.lo<other.lo) next.lo=other.lo; }
    else if(op==VX_EQ) {
        if(next.lo<other.lo) next.lo=other.lo;
        if(next.hi>other.hi) next.hi=other.hi;
        next.nonzero|=other.nonzero;
    } else if(other.lo==other.hi) {
        if(other.lo==range_zero(other.type)) next.nonzero=true;
        if(next.lo==other.lo&&next.lo<next.hi) ++next.lo;
        else if(next.hi==other.lo&&next.lo<next.hi) --next.hi;
    }
    if(next.lo<=next.hi) remember(s,index->id,range_finish(next));
}
static bool proven(Analysis *a,const State *s,const Expr *e,Interval index) {
    if(!index.type||index.lo<range_zero(index.type)) return false;
    Type t=e->left->type;
    if(!vx_slice(a->p,t)) return range_bits(index,index.hi)<(uint64_t)a->p->types[t]->count;
    if(e->right->kind!=EX_VAR||e->left->kind!=EX_VAR) return false;
    for(int i=0;i<s->count;++i)
        if(s->facts[i].index==e->right->id&&s->facts[i].view==e->left->id) return true;
    return false;
}
static Interval expression(Analysis *a,State *s,Expr *e);
static bool statements(Analysis *a,State *s,Stmt *stmt);
static bool pure_call(Analysis *a,const Expr *e) {
    switch(e->kind) {
    case EX_CALL:case EX_GENERIC_CALL:case EX_METHOD:case EX_INVOKE:case EX_CLOSURE_CALL:case EX_VIEW_CALL:case EX_BOX_CALL:
        return (vx_call_contract(a->p,e)->caps&CP_PURE)!=0;
    default:return false;
    }
}
static Interval expression(Analysis *a,State *s,Expr *e) {
    if(!e) return (Interval){0};
    tick(a,e->token);
    Interval result=range_full(e->type),left=expression(a,s,e->left);
    if(e->kind==EX_BINARY&&(e->op==VX_LAND||e->op==VX_LOR)) {
        State *right=state_copy(a,s); refine(a,right,e->left,e->op==VX_LAND);
        (void)expression(a,right,e->right); intersect(s,right); --a->used; return result;
    }
    Interval right=expression(a,s,e->right);
    for(Arg *arg=e->args;arg;arg=arg->next) (void)expression(a,s,arg->expr);
    if(e->kind==EX_MATCH) {
        State *merged=state_copy(a,NULL),*branch=state_copy(a,NULL); bool first=true;
        for(Arm *arm=e->arms;arm;arm=arm->next) {
            *branch=*s;
            for(int i=0;i<arm->count;++i) kill(branch,arm->locals[i]);
            (void)expression(a,branch,arm->guard); refine(a,branch,arm->guard,true);
            (void)expression(a,branch,arm->expr);
            if(first) { *merged=*branch; first=false; } else intersect(merged,branch);
        }
        *s=*merged; a->used-=2;
    }
    if(e->kind==EX_INDEX&&proven(a,s,e,right)) e->bounds_proven=true;
    if(e->kind==EX_VAR) result=local_range(s,e->id,e->type);
    else if(e->kind==EX_WORD) result=range_single(e->type,e->value);
    else if(e->kind==EX_LENGTH) result=range_single(e->type,e->value);
    else if(e->kind==EX_CAST) result=cast_range(e,left);
    else if(e->kind==EX_BINARY||e->kind==EX_UNARY) {
        bool safe;
        result=range_calculate(e->op,e->operand_type,left,right,&safe);
        if(safe) e->arithmetic_proven=true;
        if(!vx_integer(e->type)) result=(Interval){0};
    }
    switch(e->kind) {
    case EX_CALL:case EX_GENERIC_CALL:case EX_METHOD:case EX_INVOKE:case EX_CLOSURE_CALL:case EX_VIEW_CALL:case EX_BOX_CALL:
        if(!pure_call(a,e)) clear(s);
        break;
    case EX_REF:case EX_SLICE_BORROW:case EX_VIEW_BORROW:
        if(vx_exclusive(a->p,e->type)) clear(s);
        break;
    case EX_MEMORY:case EX_IO:case EX_LIBRARY:case EX_VEC:case EX_STRING:case EX_BOX_TAKE:case EX_STRING_INTO:
        clear(s); break;
    default:break;
    }
    return result;
}
static bool statements(Analysis *a,State *s,Stmt *stmt) {
    for(;stmt;stmt=stmt->next) {
        tick(a,stmt->token);
        switch(stmt->kind) {
        case ST_IF: {
            (void)expression(a,s,stmt->expr);
            State *yes=state_copy(a,s),*no=state_copy(a,s);
            refine(a,yes,stmt->expr,true); refine(a,no,stmt->expr,false);
            bool left=statements(a,yes,stmt->body),right=statements(a,no,stmt->otherwise);
            if(left&&right) { *s=*yes; intersect(s,no); }
            else if(left) *s=*yes; else if(right) *s=*no;
            a->used-=2;
            if(!left&&!right) return false;
            break;
        }
        case ST_LOOP: {
            if(!statements(a,s,stmt->initial)) return false;
            State *body=state_copy(a,NULL),*continues=state_copy(a,NULL);
            (void)expression(a,body,stmt->expr); refine(a,body,stmt->expr,true);
            Loop loop={a->loop,continues,false}; a->loop=&loop;
            bool fall=statements(a,body,stmt->body);
            a->loop=loop.parent;
            if(loop.has_continue) { if(fall) intersect(body,continues); else *body=*continues; }
            if(!fall&&!loop.has_continue) clear(body);
            (void)statements(a,body,stmt->step);
            a->used-=2; clear(s); break;
        }
        case ST_BLOCK:if(!statements(a,s,stmt->body)) return false;break;
        case ST_MATCH: {
            (void)expression(a,s,stmt->expr);
            State *merged=state_copy(a,NULL),*branch=state_copy(a,NULL); bool first=true;
            for(Arm *arm=stmt->arms;arm;arm=arm->next) {
                *branch=*s;
                for(int i=0;i<arm->count;++i) kill(branch,arm->locals[i]);
                (void)expression(a,branch,arm->guard); refine(a,branch,arm->guard,true);
                if(!statements(a,branch,arm->body)) continue;
                if(first) { *merged=*branch; first=false; } else intersect(merged,branch);
            }
            if(!first) *s=*merged;
            a->used-=2;
            if(first) return false;
            break;
        }
        case ST_ASSIGN: {
            Interval prior=expression(a,s,stmt->target),value=expression(a,s,stmt->expr);
            if(stmt->op>=0) {
                bool safe; value=range_calculate(stmt->op,stmt->type,prior,value,&safe);
                if(safe) stmt->arithmetic_proven=true;
            }
            if(stmt->target->kind==EX_VAR) { kill(s,stmt->local); remember(s,stmt->local,value); }
            else clear(s);
            break;
        }
        case ST_BIND: {
            Interval value=expression(a,s,stmt->expr); kill(s,stmt->local); remember(s,stmt->local,value); break;
        }
        case ST_RETURN:(void)expression(a,s,stmt->expr);return false;
        case ST_CONTINUE:
            if(a->loop) {
                if(a->loop->has_continue) intersect(a->loop->continues,s);
                else { *a->loop->continues=*s; a->loop->has_continue=true; }
            }
            return false;
        case ST_BREAK:return false;
        case ST_EXPR:(void)expression(a,s,stmt->expr);break;
        }
    }
    return true;
}
void vx_prove_ranges(VxnProgram *p) {
    Analysis analysis={.p=p,.work=16u*1024u*1024u};
    State *state=state_copy(&analysis,NULL);
    for(int i=0;i<p->function_count;++i) if(!p->functions[i]->instance) {
        clear(state); (void)statements(&analysis,state,p->functions[i]->body);
    }
    /* Instances share their checked template's effect obligations. */
    for(int i=0;i<p->function_count;++i) for(Use *u=p->functions[i]->uses;u;u=u->next)
        if((u->bounds_check&&u->bounds_check->bounds_proven)||
           (u->arithmetic_check&&u->arithmetic_check->arithmetic_proven)||
           (u->arithmetic_assignment&&u->arithmetic_assignment->arithmetic_proven)) u->effects=0;
}
