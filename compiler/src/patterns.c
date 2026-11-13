#include "internal.h"

/* Constructor-matrix usefulness: bindings act as wildcards; guards add no coverage.
   Scratch rewinds on return; work/depth budgets bound recursion. */
typedef struct Cell Cell;
struct Cell { const Pattern *pattern; Type type; const Cell *next; };
#define PATTERN_CELLS 65536
typedef struct { Cell cells[PATTERN_CELLS]; size_t used,work; VxnProgram *p; Token at; } Coverage;
static bool any(const Pattern *p) { return !p||p->kind==PAT_WILD||p->kind==PAT_BIND; }
static const Cell *cell(Coverage *c,const Pattern *p,Type t,const Cell *next) {
    if(c->used==PATTERN_CELLS) vx_fail(c->p,c->at,"pattern coverage storage limit exceeded");
    Cell *n=&c->cells[c->used++]; *n=(Cell){p,t,next}; return n;
}
/* constructor: 0 scalar, 1 choice, 2 product (one constructor). */
static bool accepts(const Pattern *p,int constructor,uint64_t value) {
    return any(p)||(constructor==2?(p->kind==PAT_STRUCT||p->kind==PAT_ARRAY):
                   constructor==1?p->kind==PAT_CONSTRUCTOR&&(uint64_t)p->variant==value:
                               (p->kind==PAT_LITERAL||p->kind==PAT_TEXT)&&p->value==value);
}
static const Cell *expand(Coverage *c,const Cell *row,int constructor,uint64_t value) {
    if(!constructor) return row->next;
    const TypeDef *d=c->p->types[row->type];
    const Variant *v=constructor==1?&d->variants[value]:NULL;
    int count=v?v->count:d->count;
    const Pattern *children[VX_MAX_FIELDS]={0}; int i=0;
    if(!any(row->pattern)) for(const Pattern *p=row->pattern->children;p;p=p->next) children[i++]=p;
    const Cell *tail=row->next;
    for(i=count-1;i>=0;--i) tail=cell(c,children[i],v?v->fields[i]:d->kind==TD_STRUCT?d->fields[i].type:d->element,tail);
    return tail;
}
static bool useful(Coverage *c,const Cell *const *rows,int count,const Cell *query,int depth);
static void interval(const Pattern *p,Type t,uint64_t *lo,uint64_t *hi) {
    uint64_t sign=vx_signed_type(t)?UINT64_C(1)<<(vx_width(t)-1):0;
    if(any(p)) { *lo=0; *hi=vx_mask(vx_width(t)); }
    else { *lo=p->value^sign; *hi=(p->kind==PAT_RANGE?p->upper:p->value)^sign; }
}
/* Membership is constant between endpoints. Sample each segment without max+1 overflow. */
static bool integers(Coverage *c,const Cell *const *rows,int count,const Cell *query,int depth) {
    uint64_t lo,hi,starts[2*VX_MAX_FIELDS+1],low[VX_MAX_FIELDS],high[VX_MAX_FIELDS];
    interval(query->pattern,query->type,&lo,&hi); starts[0]=lo; int segments=1;
    for(int i=0;i<count;++i) {
        interval(rows[i]->pattern,query->type,&low[i],&high[i]);
        if(low[i]>lo&&low[i]<=hi) starts[segments++]=low[i];
        if(high[i]>=lo&&high[i]<hi) starts[segments++]=high[i]+1;
    }
    for(int s=0;s<segments;++s) {
        bool duplicate=false; for(int k=0;k<s;++k) if(starts[k]==starts[s]) duplicate=true;
        if(duplicate) continue;
        const Cell *selected[VX_MAX_FIELDS]; int n=0;
        for(int i=0;i<count;++i) if(low[i]<=starts[s]&&starts[s]<=high[i]) selected[n++]=rows[i]->next;
        if(useful(c,selected,n,query->next,depth+1)) return true;
    }
    return false;
}
static bool specialize(Coverage *c,const Cell *const *rows,int count,const Cell *query,
                       int constructor,uint64_t value,int depth) {
    size_t mark=c->used; const Cell *selected[VX_MAX_FIELDS]; int n=0;
    for(int i=0;i<count;++i) if(accepts(rows[i]->pattern,constructor,value))
        selected[n++]=expand(c,rows[i],constructor,value);
    bool result=useful(c,selected,n,expand(c,query,constructor,value),depth+1);
    c->used=mark; return result;
}
static bool useful(Coverage *c,const Cell *const *rows,int count,const Cell *query,int depth) {
    if(!c->work--||depth>256) vx_fail(c->p,c->at,"pattern coverage work/depth limit exceeded");
    if(!count) return true;
    if(!query) return false;
    const Pattern *q=query->pattern;
    if(q&&q->kind==PAT_OR) {
        for(const Pattern *branch=q->children;branch;branch=branch->next) {
            Cell alternative={branch,query->type,query->next};
            if(useful(c,rows,count,&alternative,depth+1)) return true;
        }
        return false;
    }
    for(int i=0;i<count;++i) if(rows[i]->pattern&&rows[i]->pattern->kind==PAT_OR) {
        size_t mark=c->used; const Cell *expanded[VX_MAX_FIELDS]; int total=0;
        for(int j=0;j<count;++j) {
            const Pattern *r=rows[j]->pattern;
            if(r&&r->kind==PAT_OR) {
                for(const Pattern *branch=r->children;branch;branch=branch->next) {
                    if(total==VX_MAX_FIELDS) vx_fail(c->p,c->at,"alternative coverage row limit exceeded");
                    expanded[total++]=cell(c,branch,rows[j]->type,rows[j]->next);
                }
            } else {
                if(total==VX_MAX_FIELDS) vx_fail(c->p,c->at,"alternative coverage row limit exceeded");
                expanded[total++]=rows[j];
            }
        }
        bool result=useful(c,expanded,total,query,depth+1); c->used=mark; return result;
    }
    if(any(q)) {
        const Cell *tails[VX_MAX_FIELDS]; int i=0;
        for(;i<count&&any(rows[i]->pattern);++i) tails[i]=rows[i]->next;
        if(i==count) return useful(c,tails,count,query->next,depth+1);
    }
    if(vx_integer(query->type)) return integers(c,rows,count,query,depth);
    if(!any(q)) return specialize(c,rows,count,query,q->kind==PAT_CONSTRUCTOR?1:q->kind==PAT_STRUCT||q->kind==PAT_ARRAY?2:0,
                                  q->kind==PAT_CONSTRUCTOR?(uint64_t)q->variant:q->value,depth);
    Type t=query->type;
    if(t>=TY_AGGREGATE&&(c->p->types[t]->kind==TD_STRUCT||
       (c->p->types[t]->kind==TD_ARRAY&&c->p->types[t]->count<=VX_MAX_FIELDS)))
        return specialize(c,rows,count,query,2,0,depth);
    bool choice=t>=TY_AGGREGATE&&c->p->types[t]->kind==TD_CHOICE;
    if(choice||t==TY_BOOL) {
        int variants=choice?c->p->types[t]->count:2;
        for(int i=0;i<variants;++i) if(specialize(c,rows,count,query,choice,(uint64_t)i,depth)) return true;
        return false;
    }
    /* Text has an unbounded domain. Opaque payloads can only be bound/ignored. */
    const Cell *selected[VX_MAX_FIELDS]; int n=0;
    for(int i=0;i<count;++i) if(any(rows[i]->pattern)) selected[n++]=rows[i]->next;
    return useful(c,selected,n,query->next,depth+1);
}
void vx_check_patterns(VxnProgram *p,Type t,const Arm *arms,Token at) {
    if(!p->pattern_workspace) p->pattern_workspace=vx_alloc(p,sizeof(Coverage));
    Coverage *c=p->pattern_workspace; c->p=p; c->at=at; c->used=0; c->work=1000000;
    const Cell *rows[VX_MAX_FIELDS]; int count=0;
    for(const Arm *a=arms;a;a=a->next) {
        const Cell *query=cell(c,a->pattern,t,NULL);
        if(!useful(c,rows,count,query,0)) vx_fail(p,a->token,"duplicate match arm or unreachable pattern");
        if(!a->guard) rows[count++]=query;
    }
    if(useful(c,rows,count,cell(c,NULL,t,NULL),0))
        vx_fail(p,at,"non-exhaustive match: missing cases; add unguarded patterns or a wildcard '_'");
}
void vx_pattern_layout(VxnProgram *p,Arm *a,Pattern *n,int offset) {
    n->offset=offset;
    if(n->kind==PAT_BIND) { a->bindings[n->binding].type=n->type; a->offsets[n->binding]=offset; }
    else if(n->kind==PAT_OR) {
        for(Pattern *child=n->children;child;child=child->next) vx_pattern_layout(p,a,child,offset);
    }
    else if(n->kind==PAT_CONSTRUCTOR||n->kind==PAT_STRUCT||n->kind==PAT_ARRAY) {
        const TypeDef *d=p->types[n->type];
        const Variant *v=n->kind==PAT_CONSTRUCTOR?&d->variants[n->variant]:NULL; int i=0;
        if(v) ++offset;
        for(Pattern *child=n->children;child;child=child->next) {
            vx_pattern_layout(p,a,child,offset);
            offset+=vx_slots(p,v?v->fields[i]:d->kind==TD_STRUCT?d->fields[i].type:d->element); ++i;
        }
    }
}
