#include "internal.h"
#include <string.h>

/* Statement CFG with reaching definitions and backward loan liveness.
   Temporary loans follow evaluation order; stored loans are statement-conservative.
   No runtime tracking or backend alias promises. */
#define LIVE_WORDS ((VX_MAX_LOCALS+63)/64)
typedef struct { uint64_t bits[LIVE_WORDS]; } Live;
typedef struct Via Via;
struct Via { int id,depth; bool exclusive; Via *parent; };
typedef struct Path Path;
typedef struct Partition Partition;
struct Partition { int id,side; Partition *parent; };
struct Path { int root,count,actor,part[VX_MAX_DEPTH],pending_split; bool dependency; Via *via; Partition *partition; Path *next; };
typedef struct Loan Loan;
struct Loan { Path *paths,*reader; bool exclusive; Loan *next; };
typedef struct Node Node;
typedef struct Edge Edge;
typedef struct Definition Definition;
struct Edge { Node *to; Edge *next; };
struct Definition { int id,local,split_side,split_loan; Expr *expr; Node *node; Path *paths; Definition *next,*same,*generated; bool pattern; };
struct Node {
    const Stmt *stmt; Expr *expr; Edge *edges; Definition *definition;
    Live uses,live; uint64_t *reaches; bool reachable; Node *next;
};
typedef struct {
    VxnProgram *p; Function *f; Loan *loans; size_t work,paths;
    Node *nodes,*node; Definition *definitions,*locals[VX_MAX_LOCALS];
    int node_count,definition_count,loan_count,words; bool after;
    Live live_after,pattern_live;
} Permission;
enum { READ, WRITE, EXCLUSIVE, MOVE };
static void tick(Permission *c,Token at) {
    if(!c->work--) vx_fail(c->p,at,"exclusive analysis work limit exceeded");
}
static bool bit(const uint64_t *a,int id) { return (a[id/64]&(UINT64_C(1)<<(id%64)))!=0; }
static void set(uint64_t *a,int id) { a[id/64]|=UINT64_C(1)<<(id%64); }
static void clear(uint64_t *a,int id) { a[id/64]&=~(UINT64_C(1)<<(id%64)); }
static Path *path(Permission *c,int root,Token at) {
    tick(c,at);
    if(++c->paths>32768) vx_fail(c->p,at,"exclusive place-path limit exceeded");
    Path *r=vx_alloc(c->p,sizeof *r); r->root=root; r->actor=-1; return r;
}
static Path *copy(Permission *c,const Path *a,Token at) {
    Path *first=NULL,**tail=&first;
    for(;a;a=a->next) { Path *r=path(c,a->root,at); *r=*a; r->next=NULL; *tail=r; tail=&r->next; }
    return first;
}
static Path *append(Path *a,Path *b) {
    if(!a) return b;
    Path *last=a; while(last->next) last=last->next; last->next=b; return a;
}
static Path *project(Permission *c,Path *paths,int part,Token at) {
    for(Path *r=paths;r;r=r->next) {
        tick(c,at);
        if(r->dependency) continue;
        /* Unknown offsets/projections overlap the entire remaining suffix. */
        if(r->count&&r->part[r->count-1]==-1) continue;
        if(r->count==VX_MAX_DEPTH) vx_fail(c->p,at,"exclusive place projection limit exceeded");
        r->part[r->count++]=part;
    }
    return paths;
}
static bool through(const Via *v,int id) {
    for(;v;v=v->parent) if(v->id==id) return true;
    return false;
}
static Path *borrow(Permission *c,Path *paths,int id,bool exclusive,Token at) {
    for(Path *r=paths;r;r=r->next) {
        if(r->dependency) continue;
        if(through(r->via,id)) vx_fail(c->p,at,"cyclic stored reborrows require fresh loans from the original storage on each iteration");
        Via *v=vx_alloc(c->p,sizeof *v); v->id=id; v->exclusive=exclusive; v->parent=r->via;
        v->depth=r->via?r->via->depth+1:1;
        if(v->depth>VX_MAX_DEPTH) vx_fail(c->p,at,"stored reborrow depth limit exceeded");
        r->via=v;
    }
    return paths;
}
static bool same_path(const Path *a,const Path *b) {
    if(a->pending_split!=b->pending_split||a->dependency!=b->dependency||a->root!=b->root||a->count!=b->count||memcmp(a->part,b->part,(size_t)a->count*sizeof(int))) return false;
    const Partition *ap=a->partition,*bp=b->partition;
    for(;ap&&bp;ap=ap->parent,bp=bp->parent) if(ap->id!=bp->id||ap->side!=bp->side) return false;
    if(ap||bp) return false;
    const Via *x=a->via,*y=b->via;
    for(;x&&y;x=x->parent,y=y->parent) if(x->id!=y->id) return false;
    return !x&&!y;
}
static bool merge_paths(Path **into,Path *paths) {
    bool changed=false;
    while(paths) {
        Path *next=paths->next; bool present=false;
        for(Path *r=*into;r;r=r->next) if(same_path(r,paths)) { present=true; break; }
        if(!present) { paths->next=*into; *into=paths; changed=true; }
        paths=next;
    }
    return changed;
}
static Path *reference(Permission *c,const Expr *e);
static Path *place(Permission *c,const Expr *e) {
    tick(c,e->token);
    if(e->kind==EX_VAR) return path(c,e->id,e->token);
    if(e->kind==EX_DEREF) {
        Path *result=reference(c,e->left);
        if(e->type>=TY_AGGREGATE&&c->p->types[e->type]->lazy) {
            Path **at=&result;
            while(*at) { if((*at)->dependency) *at=(*at)->next; else at=&(*at)->next; }
        }
        return result;
    }
    if(e->kind==EX_FIELD) return project(c,place(c,e->left),e->id+1,e->token);
    if(e->kind==EX_INDEX) {
        Path *base=vx_slice(c->p,e->left->type)?reference(c,e->left):place(c,e->left);
        Type t; uint64_t index;
        int part=vx_constant(e->right,&t,&index)&&index<1024?(int)index+1024:-1;
        if(vx_slice(c->p,e->left->type)) part=-1;
        return project(c,base,part,e->token);
    }
    vx_fail(c->p,e->token,"internal error: unsupported exclusive place");
}
static Path *reference(Permission *c,const Expr *e) {
    tick(c,e->token);
    if(e->kind==EX_REF) {
        Path *storage=borrow(c,place(c,e->left),e->loan_id,vx_exclusive(c->p,e->type),e->token);
        return (vx_borrowed_closure(c->p,e->left->type)||vx_shared_aggregate(c->p,e->left->type))?append(storage,reference(c,e->left)):storage;
    }
    if(e->kind==EX_DEREF) {
        Path *result=reference(c,e->left);
        if(e->type>=TY_AGGREGATE&&c->p->types[e->type]->lazy) {
            Path **at=&result;
            while(*at) { if(!(*at)->dependency) *at=(*at)->next; else at=&(*at)->next; }
        } else if(vx_shared_aggregate(c->p,e->type)) {
            for(Path *r=result;r;r=r->next) r->dependency=true;
        } else if(vx_borrowed_closure(c->p,e->type)) {
            Path **at=&result;
            while(*at) { if(!(*at)->dependency) *at=(*at)->next; else at=&(*at)->next; }
        }
        return result;
    }
    if(e->kind==EX_MEMORY) {
        Path *result=reference(c,vx_argument(e,0)->expr),**at=&result;
        while(*at) { if(!(*at)->dependency) *at=(*at)->next; else at=&(*at)->next; }
        return result;
    }
    if(e->kind==EX_CLOSURE||e->kind==EX_RECORD||e->kind==EX_CHOICE) {
        Path *result=NULL;
        for(const Arg *a=e->args;a;a=a->next) if(vx_dependent(c->p,a->expr->type))
            result=append(result,reference(c,a->expr));
        for(Path *r=result;r;r=r->next) r->dependency=true;
        return result;
    }
    if(e->kind==EX_FIELD||e->kind==EX_TRY) {
        Path *result=reference(c,e->left);
        for(Path *r=result;r;r=r->next) r->dependency=vx_shared_aggregate(c->p,e->type);
        return result;
    }
    if(e->kind==EX_STRING&&vx_dependent(c->p,e->type)) {
        Path *result=reference(c,vx_argument(e,0)->expr);
        for(Path *r=result;r;r=r->next) r->dependency=true;
        return result;
    }
    if(e->kind==EX_VEC&&vx_dependent(c->p,e->type)) return project(c,reference(c,vx_argument(e,0)->expr),-2,e->token);
    if(e->kind==EX_BOX_REF) return project(c,reference(c,e->args->expr),-2,e->token);
    if(e->kind==EX_FN_VIEW) return reference(c,e->left);
    if(e->kind==EX_SLICE_BORROW||e->kind==EX_VIEW_BORROW) return borrow(c,reference(c,e->left),e->loan_id,vx_exclusive(c->p,e->type),e->token);
    if(e->kind==EX_SLICE) return reference(c,vx_argument(e,0)->expr);
    if(e->kind==EX_SLICE_SPLIT) {
        Path *result=borrow(c,reference(c,vx_argument(e,0)->expr),e->loan_id,true,e->token);
        for(Path *r=result;r;r=r->next) r->pending_split=e->loan_id;
        return result;
    }
    if(e->kind==EX_VAR) {
        Path *result=NULL;
        for(Definition *d=c->locals[e->id];d;d=d->same) if(bit(c->node->reaches,d->id)||(d->pattern&&d->node==c->node))
            result=append(result,copy(c,d->paths,e->token));
        for(Path *r=result;r;r=r->next) r->actor=e->id;
        return result;
    }
    if(e->kind==EX_CALL||e->kind==EX_GENERIC_CALL||e->kind==EX_METHOD||e->kind==EX_INVOKE||e->kind==EX_CLOSURE_CALL||e->kind==EX_VIEW_CALL||e->kind==EX_BOX_CALL) {
        Path *result=NULL; const Function *f=vx_call_contract(c->p,e); bool exclusive_source=false;
        for(const Arg *a=e->args;a;a=a->next) if(f->from_mask&(UINT32_C(1)<<a->field)) {
            result=append(result,project(c,reference(c,a->expr),-1,e->token));
            exclusive_source|=vx_exclusive(c->p,a->expr->type);
        }
        if(exclusive_source) result=borrow(c,result,e->loan_id,vx_exclusive(c->p,e->type),e->token);
        if(vx_split_result(c->p,e->type)) for(Path *r=result;r;r=r->next) r->pending_split=e->loan_id;
        if(vx_shared_aggregate(c->p,e->type)) for(Path *r=result;r;r=r->next) r->dependency=true;
        return result;
    }
    if(e->kind==EX_MATCH) {
        Path *result=NULL;
        for(const Arm *a=e->arms;a;a=a->next) result=append(result,reference(c,a->expr));
        return result;
    }
    if(e->kind==EX_TEXT||e->kind==EX_PANIC) return NULL;
    vx_fail(c->p,e->token,"internal error: unsupported exclusive origin");
}
static Definition *define(Permission *c,int local,Expr *e,Node *n) {
    Definition *d=vx_alloc(c->p,sizeof *d); d->id=c->definition_count++; d->local=local; d->expr=e; d->node=n;
    d->split_side=-1;
    d->same=c->locals[local]; c->locals[local]=d;
    d->next=c->definitions; c->definitions=d;
    if(n) { d->generated=n->definition; n->definition=d; }
    return d;
}
static Definition *pattern_definition(Permission *c,int i,const Arm *a,Expr *source,Node *n) {
    Definition *d=define(c,a->locals[i],source,n);
    if(vx_split_result(c->p,source->type)&&vx_uniq_slice(c->p,a->bindings[i].type)) {
        d->split_loan=++c->loan_count;
        if(a->pattern->kind==PAT_CONSTRUCTOR&&a->pattern->variant==0)
            d->split_side=a->offsets[i]==1?0:1;
    }
    return d;
}
static Path *pattern_paths(Permission *c,const Definition *d,Path *paths) {
    if(!d->split_loan) return paths;
    paths=borrow(c,paths,d->split_loan,true,d->expr->token);
    for(Path *r=paths;r;r=r->next) {
        if(d->split_side>=0&&r->pending_split) {
            Partition *p=vx_alloc(c->p,sizeof *p); p->id=r->pending_split;
            p->side=d->split_side; p->parent=r->partition; r->partition=p;
        }
        r->pending_split=0;
    }
    return paths;
}
static void uses(Permission *c,Node *n,Expr *e,bool introduce) {
    if(!e) return;
    tick(c,e->token);
    if(introduce&&(e->kind==EX_REF||e->kind==EX_SLICE_BORROW||e->kind==EX_VIEW_BORROW||e->kind==EX_SLICE_SPLIT)) e->loan_id=++c->loan_count;
    if(introduce&&vx_returned_borrow(c->p,e->type)&&(e->kind==EX_CALL||e->kind==EX_GENERIC_CALL||e->kind==EX_METHOD||e->kind==EX_INVOKE||e->kind==EX_CLOSURE_CALL||e->kind==EX_VIEW_CALL||e->kind==EX_BOX_CALL)) e->loan_id=++c->loan_count;
    if(e->kind==EX_VAR&&vx_dependent(c->p,e->type)) set(n->uses.bits,e->id);
    uses(c,n,e->left,introduce); uses(c,n,e->right,introduce);
    for(Arg *a=e->args;a;a=a->next) uses(c,n,a->expr,introduce);
    for(Arm *a=e->arms;a;a=a->next) {
        if(introduce) for(int i=0;i<a->count;++i) if(vx_dependent(c->p,a->bindings[i].type))
            pattern_definition(c,i,a,e->left,n)->pattern=true;
        uses(c,n,a->guard,introduce); uses(c,n,a->expr,introduce);
        for(int i=0;i<a->count;++i) clear(n->uses.bits,a->locals[i]);
    }
}
static Node *node(Permission *c,const Stmt *s,Expr *e) {
    if(++c->node_count>VX_MAX_NODES) vx_fail(c->p,c->f->name,"loan control-flow graph limit exceeded");
    Node *n=vx_alloc(c->p,sizeof *n); n->stmt=s; n->expr=e; n->next=c->nodes; c->nodes=n;
    uses(c,n,e,true);
    if(s&&s->kind==ST_ASSIGN&&s->target->kind!=EX_VAR) uses(c,n,s->target,true);
    if(s&&(s->kind==ST_BIND||(s->kind==ST_ASSIGN&&s->target->kind==EX_VAR))&&vx_dependent(c->p,s->type))
        n->definition=define(c,s->local,e,n);
    return n;
}
static void edge(Permission *c,Node *from,Node *to) {
    if(!to) return;
    Edge *e=vx_alloc(c->p,sizeof *e); e->to=to; e->next=from->edges; from->edges=e;
}
static Node *graph(Permission *c,const Stmt *s,Node *next,Node *on_break,Node *on_continue) {
    int count=0; for(const Stmt *at=s;at;at=at->next) ++count;
    const Stmt **list=vx_alloc(c->p,(size_t)count*sizeof *list);
    int i=0; for(const Stmt *at=s;at;at=at->next) list[i++]=at;
    while(count) {
        s=list[--count]; tick(c,s->token);
        if(s->kind==ST_BLOCK) { next=graph(c,s->body,next,on_break,on_continue); continue; }
        Node *n=node(c,s,s->expr);
        switch(s->kind) {
        case ST_RETURN:break;
        case ST_BREAK:edge(c,n,on_break);break;
        case ST_CONTINUE:edge(c,n,on_continue);break;
        case ST_IF:
            edge(c,n,graph(c,s->body,next,on_break,on_continue));
            edge(c,n,graph(c,s->otherwise,next,on_break,on_continue));break;
        case ST_LOOP: {
            Node *step=graph(c,s->step,n,next,n);
            edge(c,n,graph(c,s->body,step,next,step)); edge(c,n,next);
            next=graph(c,s->initial,n,on_break,on_continue); continue;
        }
        case ST_MATCH: {
            const Arm *arms[VX_MAX_FIELDS]; int arm_count=0;
            for(const Arm *a=s->arms;a;a=a->next) arms[arm_count++]=a;
            Node *following=NULL;
            while(arm_count) {
                const Arm *a=arms[--arm_count]; Node *arm;
                if(a->expr) { arm=node(c,NULL,a->expr); edge(c,arm,next); }
                else arm=graph(c,a->body,next,on_break,on_continue);
                if(a->guard) {
                    Node *guard=node(c,NULL,a->guard); edge(c,guard,arm); edge(c,guard,following); arm=guard;
                }
                Node *bindings=node(c,NULL,NULL);
                uses(c,bindings,s->expr,false);
                for(int i=0;i<a->count;++i) if(vx_dependent(c->p,a->bindings[i].type))
                    pattern_definition(c,i,a,s->expr,bindings);
                edge(c,bindings,arm); arm=bindings;
                edge(c,n,arm);
                /* Pattern failure can reach any later arm. */
                if(following) { Node *test=node(c,NULL,NULL); edge(c,test,arm); edge(c,test,following); following=test; }
                else following=arm;
            }
            break;
        }
        default:edge(c,n,next);break;
        }
        next=n;
    }
    return next;
}
static void dataflow(Permission *c,Node *entry) {
    c->words=(c->definition_count+63)/64;
    size_t bytes=(size_t)c->node_count*(size_t)c->words*sizeof(uint64_t);
    if(bytes>16u*1024u*1024u) vx_fail(c->p,c->f->name,"loan reaching-definition memory limit exceeded");
    uint64_t *reaches=vx_alloc(c->p,bytes),*out=vx_alloc(c->p,(size_t)c->words*sizeof *out);
    for(Node *n=c->nodes;n;n=n->next) { n->reaches=reaches; reaches+=c->words; }
    for(Definition *d=c->definitions;d;d=d->next) if(!d->node) set(entry->reaches,d->id);
    entry->reachable=true;
    bool changed;
    do {
        changed=false;
        for(Node *n=c->nodes;n;n=n->next) if(n->reachable) {
            memcpy(out,n->reaches,(size_t)c->words*sizeof *out);
            for(Definition *generated=n->definition;generated;generated=generated->generated) {
                for(Definition *d=c->locals[generated->local];d;d=d->same) clear(out,d->id);
                set(out,generated->id);
            }
            for(Edge *e=n->edges;e;e=e->next) {
                if(!e->to->reachable) { e->to->reachable=true; changed=true; }
                for(int w=0;w<c->words;++w) {
                    tick(c,c->f->name); uint64_t old=e->to->reaches[w]; e->to->reaches[w]|=out[w]; changed|=old!=e->to->reaches[w];
                }
            }
        }
    } while(changed);
    do {
        changed=false;
        for(Node *n=c->nodes;n;n=n->next) {
            Live live={0};
            for(Edge *e=n->edges;e;e=e->next) for(int w=0;w<LIVE_WORDS;++w) live.bits[w]|=e->to->live.bits[w];
            for(Definition *d=n->definition;d;d=d->generated) clear(live.bits,d->local);
            for(int w=0;w<LIVE_WORDS;++w) {
                tick(c,c->f->name); live.bits[w]|=n->uses.bits[w]; changed|=live.bits[w]!=n->live.bits[w];
            }
            n->live=live;
        }
    } while(changed);
    do {
        changed=false;
        for(Definition *d=c->definitions;d;d=d->next) if(d->node&&d->node->reachable&&d->expr) {
            c->node=d->node; changed|=merge_paths(&d->paths,pattern_paths(c,d,reference(c,d->expr)));
        }
    } while(changed);
}
static bool overlaps(const Path *a,const Path *b) {
    if(a->root!=b->root) return false;
    for(const Partition *x=a->partition;x;x=x->parent)
        for(const Partition *y=b->partition;y;y=y->parent)
            if(x->id==y->id&&x->side!=y->side) return false;
    int count=a->count<b->count?a->count:b->count;
    for(int i=0;i<count;++i) {
        if(a->part[i]==-1||b->part[i]==-1) return true;
        if(a->part[i]!=b->part[i]) return false;
    }
    return true;
}
static void lifetime(Permission *c,Path *paths,Token at) {
    for(Path *r=paths;r;r=r->next) {
        int id=r->root;
        if(id<c->f->param_count||vx_capture(c->f,id)>=0) continue;
        if(at.start<c->f->local_names[id].start||at.start>=c->f->local_ends[id])
            vx_fail(c->p,at,"borrowed storage does not outlive this reference use");
    }
}
static bool reader(const Path *r,const Path *allowed) {
    for(;allowed;allowed=allowed->next)
        if(allowed->via&&through(r->via,allowed->via->id)) return true;
    return false;
}
static void access(Permission *c,Path *paths,int requested,Token at) {
    const Live *live=c->after?&c->live_after:&c->node->live;
    for(Path *r=paths;r;r=r->next) {
        tick(c,at);
        int mode=r->dependency?(r->via->exclusive?EXCLUSIVE:READ):requested;
        for(Definition *d=c->definitions;d;d=d->next) if((bit(live->bits,d->local)&&bit(c->node->reaches,d->id))||(d->pattern&&d->node==c->node&&bit(c->pattern_live.bits,d->local))) {
            /* Reaching definitions are alternative permissions, not concurrent loans. */
            if(d->local==r->actor) continue;
            for(Path *other=d->paths;other;other=other->next) {
                tick(c,at);
                if(!overlaps(r,other)||through(r->via,other->via->id)) continue;
                if(mode!=READ||other->via->exclusive||(r->via&&r->via->exclusive))
                    vx_fail(c->p,at,"cannot mutate or access storage while a shared reference or exclusive handle keeps it borrowed");
            }
        }
        for(Loan *l=c->loans;l;l=l->next)
            for(Path *other=l->paths;other;other=other->next) {
                tick(c,at);
                bool exclusive=other->dependency?other->via->exclusive:l->exclusive;
                if(mode==READ&&!exclusive) continue;
                if(overlaps(r,other)&&!(mode==READ&&reader(r,l->reader)))
                    vx_fail(c->p,at,"access conflicts with an active call-scoped borrow from an earlier argument or assignment target");
            }
    }
}
static void retain(Permission *c,Path *paths,bool exclusive) {
    Loan *l=vx_alloc(c->p,sizeof *l); l->paths=paths; l->exclusive=exclusive; l->next=c->loans; c->loans=l;
}
static void retain_place(Permission *c,const Expr *e,Path *paths) {
    const Expr *base=e;
    while(base->kind==EX_FIELD||base->kind==EX_INDEX) {
        if(base->kind==EX_INDEX&&vx_slice(c->p,base->left->type)) {
            retain(c,paths,vx_uniq_slice(c->p,base->left->type));
            if(base->left->kind==EX_VAR) c->loans->reader=reference(c,base->left);
            return;
        }
        base=base->left;
    }
    bool exclusive=base->kind==EX_DEREF&&vx_uniq(base->left->type);
    retain(c,paths,exclusive);
    if(exclusive&&base->left->kind==EX_VAR) c->loans->reader=reference(c,base->left);
}
static void value(Permission *c,const Expr *e);
static void pointer(Permission *c,const Expr *e) {
    /* Locating a projection does not access its whole parent. */
    if(e->kind==EX_VAR&&vx_exclusive(c->p,e->type)) { lifetime(c,reference(c,e),e->token); return; }
    value(c,e);
}
static void address(Permission *c,const Expr *e) {
    tick(c,e->token);
    switch(e->kind) {
    case EX_VAR:return;
    case EX_DEREF:pointer(c,e->left);return;
    case EX_FIELD:if(vx_shared_aggregate(c->p,e->left->type)) lifetime(c,reference(c,e->left),e->token); address(c,e->left);return;
    case EX_INDEX: {
        if(vx_slice(c->p,e->left->type)) pointer(c,e->left); else address(c,e->left);
        Loan *before=c->loans;
        if(vx_slice(c->p,e->left->type)) {
            retain(c,reference(c,e->left),vx_uniq_slice(c->p,e->left->type));
            if(e->left->kind==EX_VAR) c->loans->reader=reference(c,e->left);
        }
        else retain_place(c,e->left,place(c,e->left));
        value(c,e->right); c->loans=before; return;
    }
    default:vx_fail(c->p,e->token,"internal error: unsupported exclusive address");
    }
}
static bool stable(const VxnProgram *p,const Expr *e) {
    if(e->kind==EX_VAR||e->kind==EX_DEREF) return true;
    if(e->kind==EX_INDEX&&vx_slice(p,e->left->type)) return true;
    return (e->kind==EX_FIELD||e->kind==EX_INDEX)&&stable(p,e->left);
}
static void value(Permission *c,const Expr *e) {
    if(!e) return;
    tick(c,e->token);
    if(vx_dependent(c->p,e->type)) lifetime(c,reference(c,e),e->token);
    switch(e->kind) {
    case EX_MATCH: {
        value(c,e->left);
        for(const Arm *a=e->arms;a;a=a->next) {
            Live prior=c->pattern_live;
            for(int i=0;i<a->count;++i) if(vx_dependent(c->p,a->bindings[i].type)) set(c->pattern_live.bits,a->locals[i]);
            value(c,a->guard); value(c,a->expr); c->pattern_live=prior;
        }
        return;
    }
    case EX_VIEW_CALL: {
        Loan *before=c->loans; pointer(c,e->left);
        Path *paths=reference(c,e->left); bool exclusive=vx_uniq_fn_view(c->p,e->operand_type);
        access(c,paths,exclusive?EXCLUSIVE:READ,e->token); retain(c,paths,exclusive);
        for(const Arg *a=e->args;a;a=a->next) {
            value(c,a->expr);
            if(vx_dependent(c->p,a->expr->type)) {
                Path *r=reference(c,a->expr); bool argument_exclusive=vx_exclusive(c->p,a->expr->type);
                access(c,r,argument_exclusive?EXCLUSIVE:READ,a->expr->token); retain(c,r,argument_exclusive);
            }
        }
        c->loans=before; return;
    }
    case EX_CLOSURE_CALL:case EX_BOX_CALL:case EX_CALL:case EX_INVOKE: {
        if(e->kind!=EX_CLOSURE_CALL&&e->kind!=EX_BOX_CALL&&(!e->left||(!e->op&&!vx_abstract(c->p,e->operand_type)))) break;
        Loan *before=c->loans;
        int mode=e->op;
        if(stable(c->p,e->left)) {
            address(c,e->left); Path *paths=place(c,e->left);
            access(c,paths,mode==2?MOVE:mode==1?EXCLUSIVE:READ,e->token);
            retain_place(c,e->left,paths); c->loans->exclusive=mode!=0;
        } else value(c,e->left);
        if(vx_borrowed_closure(c->p,e->left->type)||vx_shared_aggregate(c->p,e->left->type)) {
            Path *dependencies=reference(c,e->left); lifetime(c,dependencies,e->token);
            access(c,dependencies,READ,e->token); retain(c,dependencies,false);
        }
        for(const Arg *a=e->args;a;a=a->next) {
            value(c,a->expr);
            if(vx_dependent(c->p,a->expr->type)) {
                Path *r=reference(c,a->expr); bool exclusive=vx_exclusive(c->p,a->expr->type);
                access(c,r,exclusive?EXCLUSIVE:READ,a->expr->token); retain(c,r,exclusive);
            }
        }
        c->loans=before; return;
    }
    case EX_SLICE_LENGTH:
        pointer(c,e->args->expr); return;
    case EX_SLICE_BORROW:case EX_VIEW_BORROW:
        pointer(c,e->left); access(c,reference(c,e->left),vx_exclusive(c->p,e->type)?EXCLUSIVE:READ,e->token); return;
    case EX_REF:
        address(c,e->left); access(c,place(c,e->left),vx_exclusive(c->p,e->type)?EXCLUSIVE:READ,e->token);
        if(vx_borrowed_closure(c->p,e->left->type)||vx_shared_aggregate(c->p,e->left->type)) {
            Path *dependencies=reference(c,e->left); lifetime(c,dependencies,e->token); access(c,dependencies,READ,e->token);
        }
        return;
    case EX_VAR:
        if(vx_dependent(c->p,e->type)) {
            Path *r=reference(c,e); lifetime(c,r,e->token);
            if(vx_exclusive(c->p,e->type)) access(c,r,EXCLUSIVE,e->token);
            if(vx_borrowed_closure(c->p,e->type)||vx_shared_aggregate(c->p,e->type)) access(c,place(c,e),vx_owned(c->p,e->type)?MOVE:READ,e->token);
        } else access(c,place(c,e),vx_owned(c->p,e->type)?MOVE:READ,e->token);
        return;
    case EX_DEREF:case EX_FIELD:case EX_INDEX:
        if(stable(c->p,e)) { address(c,e); access(c,place(c,e),vx_owned(c->p,e->type)?MOVE:READ,e->token); return; }
        break;
    case EX_LENGTH:
        if(stable(c->p,e->args->expr)) {
            address(c,e->args->expr); access(c,place(c,e->args->expr),READ,e->token); return;
        }
        break;
    default:break;
    }
    value(c,e->left); value(c,e->right);
    Loan *before=c->loans;
    for(const Arg *a=e->args;a;a=a->next) {
        value(c,a->expr);
        if(vx_dependent(c->p,a->expr->type)) {
            Path *r=reference(c,a->expr); bool exclusive=vx_exclusive(c->p,a->expr->type);
            access(c,r,exclusive?EXCLUSIVE:READ,a->expr->token); retain(c,r,exclusive);
        }
    }
    c->loans=before;
    for(const Arm *a=e->arms;a;a=a->next) { value(c,a->guard); value(c,a->expr); }
}
static void check_node(Permission *c,Node *n) {
    c->node=n; c->after=false; c->live_after=(Live){0};
    for(Edge *e=n->edges;e;e=e->next) for(int w=0;w<LIVE_WORDS;++w) c->live_after.bits[w]|=e->to->live.bits[w];
    const Stmt *s=n->stmt;
    if(s&&s->kind==ST_ASSIGN) {
        if(s->target->kind==EX_VAR&&vx_borrowed(c->p,s->type)) { value(c,s->expr); return; }
        address(c,s->target); Path *target=place(c,s->target);
        Loan *before=c->loans;
        if(s->local<0) {
            access(c,target,WRITE,s->token); retain_place(c,s->target,target);
            c->loans->exclusive=true;
        } else if(s->op>=0) access(c,target,READ,s->token);
        value(c,s->expr); c->loans=before;
        /* The store follows the last shared read in its RHS. */
        c->after=true; access(c,target,WRITE,s->token); c->after=false;
    } else value(c,n->expr);
}
void vx_check_exclusive(VxnProgram *p,Function *f) {
    Permission *c=vx_alloc(p,sizeof *c); c->p=p; c->f=f; c->work=16u*1024u*1024u;
    for(int i=0;i<f->param_count;++i) if(vx_dependent(p,f->params[i].type)) {
        Definition *d=define(c,i,NULL,NULL);
        d->paths=borrow(c,path(c,-i-1,f->params[i].name),++c->loan_count,vx_exclusive(p,f->params[i].type),f->params[i].name);
        if(vx_split_result(p,f->params[i].type)) d->paths->pending_split=c->loan_count;
        d->paths->dependency=vx_shared_aggregate(p,f->params[i].type);
        Type type=f->params[i].type;
        if(f->lazy_helper&&vx_ref(type)&&vx_shared_aggregate(p,vx_referent(type))) {
            /* Cache storage and captured backing have distinct permissions. */
            Path *backing=borrow(c,path(c,-VX_MAX_LOCALS-i-1,f->params[i].name),++c->loan_count,false,f->params[i].name);
            backing->dependency=true; d->paths=append(d->paths,backing);
        }
    }
    for(int i=0;i<f->capture_count;++i) if(vx_dependent(p,f->captures[i].type)) {
        const Capture *capture=&f->captures[i]; Definition *d=define(c,capture->local,NULL,NULL);
        /* Captures reference external roots; construction checked alias compatibility. */
        d->paths=borrow(c,path(c,-capture->local-1,capture->name),++c->loan_count,vx_exclusive(p,capture->type)||vx_exclusive_captures(p,capture->type),capture->name);
        if(vx_split_result(p,capture->type)) d->paths->pending_split=c->loan_count;
        d->paths->dependency=vx_borrowed_closure(p,capture->type)||vx_shared_aggregate(p,capture->type);
    }
    Node *entry=node(c,NULL,NULL); edge(c,entry,graph(c,f->body,NULL,NULL,NULL));
    dataflow(c,entry);
    for(Node *n=c->nodes;n;n=n->next) if(n->reachable) check_node(c,n);
}
