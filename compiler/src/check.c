#include "internal.h"
#include <string.h>
typedef struct { Token name; Type type; int id, scope; bool mutable_binding; Expr *constant; int target; RecursiveGroup *recursive_group; int recursive_index; } Binding;
typedef struct Checker Checker;
struct Checker {
    VxnProgram *p; Function *f; Binding bindings[VX_MAX_LOCALS];
    Checker *parent; Binding captures[VX_MAX_FIELDS];
    unsigned char initialized[VX_MAX_LOCALS];
    int count, scope, depth, loops, static_context, guard;
};
enum { FLOW_NEXT=1, FLOW_RETURN=2, FLOW_BREAK=4, FLOW_CONTINUE=8 };
unsigned vx_builtin_effects(ExprKind kind) {
    return kind==EX_OUTPUT ? EF_IO|EF_ALLOC|EF_BLOCK : kind==EX_PANIC ? EF_ABORT : 0;
}
static void use(Checker *c,Token t,int callee,unsigned effects) {
    Use *u=vx_alloc(c->p,sizeof *u); u->token=t; u->callee=callee; u->effects=effects;
    u->comptime=c->static_context>0; u->guard=c->guard>0;
    if(c->f->last_use) c->f->last_use->next=u; else c->f->uses=u;
    c->f->last_use=u;
}
static int layout(VxnProgram *p,Type t,int depth) {
    if(vx_ref(t)) { if(vx_uniq(t)) vx_fail(p,(Token){0},"exclusive references inside aggregates require exclusive aggregate contracts"); return 1; }
    if(t==TY_VOID) vx_fail(p,(Token){0},"void cannot be an aggregate element");
    if(t<TY_AGGREGATE) return vx_slots(p,t);
    vx_expand_type(p,t);
    TypeDef *d=p->types[t];
    d->split_result|=d->kind==TD_CHOICE&&!d->name.module&&vx_word(p,d->name,"SplitResult");
    if(d->state==2) return d->slots;
    if(d->kind==TD_OPAQUE) {
        if(!d->definition) vx_fail(p,d->name,"opaque callable types are only supported as named function results");
        if(d->definition->check_state==1) vx_fail(p,d->name,"recursive opaque return inference requires a known concrete environment");
        vx_check_function(p,d->definition);
        if(!d->element) vx_fail(p,d->name,"opaque callable return requires a concrete returned closure");
        return d->slots;
    }
    if(d->state==1) vx_fail(p,d->name,"recursive value type is unsupported; direct value cycles have infinite size");
    if(depth>VX_MAX_DEPTH) vx_fail(p,d->name,"type nesting limit exceeded");
    if(d->kind==TD_UNRESOLVED) vx_fail(p,d->name,"unknown type");
    d->state=1; int slots=0;
    if(vx_fn_view(p,t)||vx_dyn_fn(p,t)) {
        d->element=vx_resolve_type(p,d->element); d->signature=p->types[d->element]->signature; slots=vx_dyn_fn(p,t)?1:2;
    }
    else if(vx_callable(p,t)) slots=1;
    else if(vx_heap_type(p,t)) {
        /* Descriptor size is payload-independent; defer payload layout. */
        if(vx_dependent(p,d->element)||d->element==TY_VOID)
            vx_fail(p,d->name,"heap container elements require owned non-void values");
        slots=1;
    }
    else if(vx_slice(p,t)||vx_str(p,t)) {
        d->element=vx_resolve_type(p,d->element);
        if(vx_dependent(p,d->element)) vx_fail(p,d->name,"slice elements must be owned values");
        (void)layout(p,d->element,depth+1); slots=2;
    }
    else if(d->kind==TD_ARRAY) {
        if(d->length) {
            uint64_t n=vx_check_length(p,d->length);
            if(n>1024) vx_fail(p,d->length->token,"array length must be in 0..1024");
            d->count=(int)n;
        }
        d->element=vx_resolve_type(p,d->element);
        if(vx_dyn_fn(p,d->element)) vx_fail(p,d->name,"DynFn requires Box storage");
        if(vx_dependent(p,d->element)) vx_fail(p,d->name,"borrowed array elements require borrowed-array contracts");
        slots=d->count*layout(p,d->element,depth+1);
    }
    else for(int i=0;i<d->count;++i) {
        Token name=d->kind==TD_STRUCT?d->fields[i].name:d->variants[i].name;
        for(int j=0;j<i;++j)
            if(vx_same(p,name,d->kind==TD_STRUCT?d->fields[j].name:d->variants[j].name))
                vx_fail(p,name,"duplicate field or constructor");
        if(d->kind==TD_STRUCT) {
            d->fields[i].type=vx_resolve_type(p,d->fields[i].type);
            if(vx_dyn_fn(p,d->fields[i].type)) vx_fail(p,d->fields[i].name,"DynFn requires Box storage");
            if(vx_exclusive(p,d->fields[i].type)||vx_fn_view(p,d->fields[i].type)||vx_borrowed_closure(p,d->fields[i].type)) vx_fail(p,d->fields[i].name,"borrowed fields support shared ref, Slice, str and shared aggregates only");
            d->borrowed_fields|=vx_shared_result(p,d->fields[i].type);
            d->fields[i].offset=slots; slots+=layout(p,d->fields[i].type,depth+1);
        } else {
            Variant *v=&d->variants[i]; int payload=0;
            for(int j=0;j<v->count;++j) {
                v->fields[j]=vx_resolve_type(p,v->fields[j]);
                if(vx_dyn_fn(p,v->fields[j])) vx_fail(p,v->name,"DynFn requires Box storage");
                if((vx_exclusive(p,v->fields[j])&&!d->split_result)||(!d->lazy&&(vx_fn_view(p,v->fields[j])||vx_borrowed_closure(p,v->fields[j])))) vx_fail(p,v->name,"borrowed fields support shared ref, Slice, str and shared aggregates only");
                d->borrowed_fields|=vx_shared_result(p,v->fields[j])||(d->lazy&&vx_dependent(p,v->fields[j]));
                payload+=layout(p,v->fields[j],depth+1);
            }
            v->slots=payload; if(payload>slots) slots=payload;
        }
        if(slots>VX_MAX_SLOTS) vx_fail(p,d->name,"aggregate exceeds 4096 scalar slots");
    }
    if(d->kind==TD_CHOICE) ++slots;
    if(d->lazy) {
        /* Internal projection; the forcing helper has installed Ready. */
        d->fields[0]=(Field){.name=vx_synthetic(d->name,"$ready_value"),.type=d->variants[1].fields[0],.offset=1};
        if(!d->generic_count) {
            Type thunk=d->variants[0].fields[0],result=d->variants[1].fields[0];
            if(vx_dependent(p,result)||vx_exclusive(p,thunk)||vx_exclusive_captures(p,thunk)) vx_fail(p,d->name,"lazy values require owned results and shared-only borrowed captures");
            if(!vx_invocable(p,thunk)||p->types[thunk]->signature->param_count||p->types[thunk]->signature->result!=result)
                vx_fail(p,d->name,"Lazy[T, F] requires a zero-argument callable returning T");
        }
    }
    if(!slots) slots=1;
    if(slots>VX_MAX_SLOTS) vx_fail(p,d->name,"aggregate exceeds 4096 scalar slots");
    d->slots=slots; d->state=2; d->resolved=t;
    if(d->borrowed_fields&&!d->lazy&&vx_owned(p,t)) vx_fail(p,d->name,"shared borrowed aggregates cannot also contain owning or move-only fields yet");
    return slots;
}
static Type canonical_type(VxnProgram *p,Type t) {
    if(vx_ref(t)) return vx_retarget(t,canonical_type(p,vx_referent(t)));
    while(t>=TY_AGGREGATE&&p->types[t]->state==2&&p->types[t]->resolved!=t) t=p->types[t]->resolved;
    return t;
}
static void finish_types(VxnProgram *p) {
    unsigned char checked[VX_MAX_TYPES]={0}; bool more;
    p->resolving_types=true;
    do {
        more=false;
        for(int t=TY_AGGREGATE;t<p->type_count;++t) {
            TypeDef *d=p->types[t];
            if(d->state!=2||checked[t]) continue;
            checked[t]=1; more=true;
            if(d->callable_bound) {
                d->callable_bound=vx_resolve_type(p,d->callable_bound);
                d->signature=p->types[d->callable_bound]->signature;
            }
            if(vx_callable(p,t)||vx_opaque(p,t)) {
                Function *f=d->signature; f->result=vx_resolve_type(p,f->result);
                if(vx_dyn_fn(p,f->result)) vx_fail(p,d->name,"DynFn requires Box storage");
                if(vx_fn_view(p,f->result)) vx_fail(p,d->name,"callables cannot return callable views yet");
                for(int i=0;i<f->param_count;++i) {
                    Param *a=&f->params[i]; a->type=vx_resolve_type(p,a->type);
                    if(vx_dyn_fn(p,a->type)) vx_fail(p,d->name,"DynFn requires Box storage");
                    if(a->type==TY_VOID) vx_fail(p,d->name,"callable parameter cannot be void");
                    vx_borrow_parameter(p,f,a);
                    for(int j=0;j<i;++j) if(a->name.length&&vx_same(p,a->name,f->params[j].name))
                        vx_fail(p,a->name,"duplicate callable parameter name");
                }
                vx_borrow_signature(p,f);
                if(vx_ref(f->result)&&f->from_inferred)
                    vx_fail(p,d->name,"callable borrowed results require an unambiguous origin or explicit from(...)");
            }
            if(vx_heap_type(p,t)) {
                d->element=vx_resolve_type(p,d->element);
                if(vx_box_fn(p,t)) d->signature=p->types[d->element]->signature;
                if(vx_dependent(p,d->element)||d->element==TY_VOID)
                    vx_fail(p,d->name,"heap container elements require owned non-void values");
            }
            for(int i=0;i<d->type_arg_count;++i) if(!vx_kind_arity(p,d->type_args[i])) d->type_args[i]=vx_resolve_type(p,d->type_args[i]);
        }
    } while(more);
    /* Merge normalized type identities, including laid-out parents.
       Aliases decrease IDs, ensuring convergence. */
    do {
        more=false;
        for(int t=TY_AGGREGATE;t<p->type_count;++t) {
            TypeDef *d=p->types[t]; if(d->state!=2) continue;
            d->element=canonical_type(p,d->element);
            for(int i=0;i<d->type_arg_count;++i) d->type_args[i]=canonical_type(p,d->type_args[i]);
            if(d->kind==TD_STRUCT) for(int i=0;i<d->count;++i) d->fields[i].type=canonical_type(p,d->fields[i].type);
            if(d->kind==TD_CHOICE) for(int i=0;i<d->count;++i)
                for(int j=0;j<d->variants[i].count;++j) d->variants[i].fields[j]=canonical_type(p,d->variants[i].fields[j]);
            if(vx_callable(p,t)) {
                Function *f=d->signature; f->result=canonical_type(p,f->result);
                for(int i=0;i<f->param_count;++i) f->params[i].type=canonical_type(p,f->params[i].type);
                if(d->resolved==t) {
                    Type same=vx_callable_type(p,f,d->name);
                    if(same<t&&p->types[same]->state==2) { d->resolved=canonical_type(p,same); more=true; }
                }
            }
            if(d->resolved!=t) continue;
            if(vx_fn_view(p,t)||vx_dyn_fn(p,t)||vx_box_fn(p,t)) d->signature=p->types[d->element]->signature;
            bool container=d->kind==TD_ARRAY||vx_slice(p,t)||vx_heap_type(p,t)||vx_fn_view(p,t)||vx_dyn_fn(p,t);
            if(!container&&!d->template_type) continue;
            for(int i=TY_AGGREGATE;i<t;++i) {
                TypeDef *other=p->types[i]; if(other->state!=2||other->kind!=d->kind) continue;
                bool same=container ? other->element==d->element&&other->count==d->count :
                    other->template_type==d->template_type&&other->type_arg_count==d->type_arg_count&&
                    !memcmp(other->type_args,d->type_args,(size_t)d->type_arg_count*sizeof(Type));
                if(same) { d->resolved=canonical_type(p,i); more=true; break; }
            }
        }
    } while(more);
    if(p->generics_ready) for(int t=TY_AGGREGATE;t<p->type_count;++t)
        if(p->types[t]->state==2) vx_check_type_application(p,t);
    p->resolving_types=false;
}
Type vx_resolve_type(VxnProgram *p,Type t) {
    if(vx_ref(t)) {
        Type referent=vx_resolve_type(p,vx_referent(t));
        if(vx_split_result(p,referent)) vx_fail(p,p->types[referent]->name,"consume SplitResult with match instead of borrowing its descriptor");
        return vx_retarget(t,referent);
    }
    if(t<TY_AGGREGATE) return t;
    if(vx_kind_arity(p,t)) vx_fail(p,p->types[t]->name,"generic value type requires explicit type arguments");
    if(++p->check_depth>VX_MAX_DEPTH) vx_fail(p,p->types[t]->name,"type/stage dependency depth limit exceeded");
    layout(p,t,0); --p->check_depth;
    if(!p->check_depth&&!p->resolving_types) finish_types(p);
    return canonical_type(p,t);
}
static int global_id(VxnProgram *p,Token name) {
    int owner=vx_symbol_module(p,name);
    for(int i=0;i<p->global_count;++i) if(p->globals[i].name.module==owner && vx_same(p,name,p->globals[i].name)) {
        vx_visible(p,name,p->globals[i].name,p->globals[i].public_api); return i;
    }
    return -1;
}
static Binding *lookup(Checker *c,Token name) {
    if(name.qualifier) return NULL;
    for(int i=c->count-1;i>=0;--i) if(vx_same(c->p,name,c->bindings[i].name)) return &c->bindings[i];
    for(int i=0;i<c->f->capture_count;++i) if(vx_same(c->p,name,c->captures[i].name)) return &c->captures[i];
    Binding *source=c->parent?lookup(c->parent,name):NULL;
    if(source) {
        if(source->constant) return source;
        if(!c->parent->initialized[source->id]&&vx_capture(c->parent->f,source->id)<0)
            vx_fail(c->p,name,"cannot capture an uninitialized binding");
        if(c->f->capture_count==VX_MAX_FIELDS||c->f->local_count==VX_MAX_LOCALS)
            vx_fail(c->p,name,"closure capture/local limit exceeded");
        int index=c->f->capture_count++,id=c->f->local_count++;
        c->f->captures[index]=(Capture){.name=source->name,.type=source->type,.local=id,.source=source->id,.mutable_binding=source->mutable_binding};
        c->f->local_types[id]=source->type; c->f->local_names[id]=source->name;
        c->f->local_starts[id]=c->f->name.start; c->initialized[id]=1;
        c->captures[index]=*source; c->captures[index].id=id;
        return &c->captures[index];
    }
    return NULL;
}
static int function_id(VxnProgram *p,Token name) {
    int owner=vx_symbol_module(p,name);
    for(int i=0;i<p->function_count;++i) {
        Function *f=p->functions[i];
        if(f->operator_impl||f->instance||f->anonymous) continue;
        if(vx_same(p,name,f->name) && (f->name.module==owner || (!name.qualifier && !f->name.module))) {
            vx_visible(p,name,f->name,f->public_api); return i;
        }
    }
    return -1;
}
static int bind(Checker *c,Token name,Type type,bool mut,bool initialized) {
    if(vx_dyn_fn(c->p,type)) vx_fail(c->p,name,"DynFn requires Box storage");
    if(type==TY_VOID) vx_fail(c->p,name,"a binding cannot have type void");
    for(int i=c->count-1;i>=0 && c->bindings[i].scope==c->scope;--i)
        if(vx_same(c->p,name,c->bindings[i].name)) vx_fail(c->p,name,"duplicate binding in this scope");
    if(c->f->local_count==VX_MAX_LOCALS) vx_fail(c->p,name,"local binding limit exceeded");
    int id=c->f->local_count++,size=vx_slots(c->p,type);
    if(c->f->local_slots+size>65536) vx_fail(c->p,name,"function storage exceeds 65536 scalar slots");
    c->f->offsets[id]=c->f->local_slots; c->f->local_slots+=size; c->f->local_types[id]=type;
    c->f->local_names[id]=name; c->f->local_scopes[id]=c->scope;
    c->f->local_starts[id]=name.start;
    c->bindings[c->count++]=(Binding){.name=name,.type=type,.id=id,.scope=c->scope,.mutable_binding=mut,.target=-1}; c->initialized[id]=(unsigned char)initialized;
    if(vx_owned(c->p,type)) { use(c,name,-1,vx_cleanup_effects(c->p,type)); c->f->last_use->cleanup=true; }
    return id;
}
static void end_scope(Checker *c,int saved,size_t end) {
    for(int i=saved;i<c->count;++i) c->f->local_ends[c->bindings[i].id]=end;
}
static int field_id(VxnProgram *p,Type t,Token name) {
    if(t<TY_AGGREGATE || p->types[t]->kind!=TD_STRUCT) vx_fail(p,name,"field access requires a struct");
    TypeDef *d=p->types[t];
    for(int i=0;i<d->count;++i) if(vx_same(p,name,d->fields[i].name)) return i;
    vx_fail(p,name,"unknown field");
}
static int result_ok(VxnProgram *p,Type t,Token at) {
    t=vx_resolve_type(p,t);
    if(t<TY_AGGREGATE||p->types[t]->kind!=TD_CHOICE||p->types[t]->count!=2)
        vx_fail(p,at,"? requires a choice with exactly Ok(T) and Err(E) variants");
    const TypeDef *d=p->types[t]; int ok=-1,err=-1;
    for(int i=0;i<2;++i) if(d->variants[i].count==1) {
        if(vx_word(p,d->variants[i].name,"Ok")) ok=i;
        if(vx_word(p,d->variants[i].name,"Err")) err=i;
    }
    if(ok<0||err<0) vx_fail(p,at,"? requires a choice with exactly Ok(T) and Err(E) variants");
    return ok;
}
static Type hint(Checker *c,const Expr *e,int depth);
static Type string_result_type(VxnProgram *p,int op) {
    if(op==VX_STRING_NEW||op==VX_STRING_FROM_STR) return p->string_alloc;
    if(op==VX_STRING_PUSH_SCALAR) return p->string_edit;
    if(op==VX_STRING_CLEAR) return TY_VOID;
    if(op==VX_STRING_CAPACITY||op==VX_STR_SCALAR_LEN) return TY_WORD;
    if(op==VX_STR_FIND||op==VX_STR_RFIND) return p->text_search;
    if(op==VX_STR_COMPARE) return p->text_order;
    if(op==VX_STRING_TRUNCATE) return p->string_index;
    if(op==VX_STR_SLICE) return p->text_slice;
    if(op==VX_STR_FROM_UTF8) return p->str_result;
    if(op>=VX_STR_EQUAL&&op<=VX_STR_IS_BOUNDARY) return TY_BOOL;
    return p->alloc_result;
}
static Type hint_call(Checker *c,const Expr *e,int depth) {
        if((vx_word(c->p,e->token,"force")||vx_word(c->p,e->token,"force_take"))&&e->args) {
            Type t=hint(c,e->args->expr,depth+1); if(vx_ref(t)) t=vx_referent(t);
            if(t>=TY_AGGREGATE&&c->p->types[t]->lazy) {
                Type result=c->p->types[t]->variants[1].fields[0];
                return vx_word(c->p,e->token,"force")?vx_reference_type(c->p,result,e->token):result;
            }
        }
        Binding *binding=lookup(c,e->token);
        if(binding) {
            Type type=binding->type; if(vx_ref(type)) type=vx_referent(type);
            if(vx_invocable(c->p,type)) return c->p->types[type]->signature->result;
        }
        int string=vx_string_builtin(c->p,e->token);
        if(string>=0) return string_result_type(c->p,string);
        if(vx_word(c->p,e->token,"len")) return TY_WORD;
        if(vx_word(c->p,e->token,"string_view")) return vx_resolve_type(c->p,vx_str_type(c->p,e->token));
        if(vx_word(c->p,e->token,"str_bytes")) return vx_resolve_type(c->p,vx_slice_type(c->p,TY_INT_BASE,e->token));
        if(vx_word(c->p,e->token,"str_byte_len")) return TY_WORD;
        if(vx_word(c->p,e->token,"string_from_utf8")) return c->p->string_result;
        if(vx_word(c->p,e->token,"string_into_bytes")) return vx_resolve_type(c->p,vx_vec_type(c->p,TY_INT_BASE,e->token));
        if(vx_word(c->p,e->token,"string_bytes")) return vx_resolve_type(c->p,vx_slice_type(c->p,TY_INT_BASE,e->token));
        if(vx_word(c->p,e->token,"string_byte_len")) return TY_WORD;
        if(vx_word(c->p,e->token,"box_fn")&&e->type_arg_count==1&&e->argc==1) {
            Type marker=vx_dyn_fn_type(c->p,vx_resolve_type(c->p,e->type_args[0]),e->token);
            return vx_resolve_type(c->p,vx_apply_type(c->p,c->p->box_result,&marker,1,e->token));
        }
        if(vx_word(c->p,e->token,"box_new")&&e->type_arg_count==1&&e->argc==1) {
            Type t=vx_resolve_type(c->p,e->type_args[0]);
            return vx_resolve_type(c->p,vx_apply_type(c->p,c->p->box_result,&t,1,e->token));
        }
        if(vx_word(c->p,e->token,"box_into_inner")&&!e->type_arg_count&&e->argc==1) {
            Type t=hint(c,e->args->expr,depth+1);
            return t>=TY_AGGREGATE&&c->p->types[t]->kind==TD_BOX?c->p->types[t]->element:TY_INFER;
        }
        if((vx_word(c->p,e->token,"box_ref")||vx_word(c->p,e->token,"box_uniq"))&&!e->type_arg_count&&e->argc==1) {
            Type t=hint(c,e->args->expr,depth+1);
            if(vx_ref(t)&&vx_referent(t)>=TY_AGGREGATE&&c->p->types[vx_referent(t)]->kind==TD_BOX)
                return vx_word(c->p,e->token,"box_uniq")?vx_retarget(t,c->p->types[vx_referent(t)]->element):vx_reference_type(c->p,c->p->types[vx_referent(t)]->element,e->token);
            return TY_INFER;
        }
        if(vx_word(c->p,e->token,"slice")||vx_word(c->p,e->token,"uniq_slice")) {
            const Arg *data=NULL;
            for(const Arg *a=e->args;a;a=a->next) if(vx_word(c->p,a->name,"data")||(!a->name.length&&a==e->args)) { data=a; break; }
            Type t=data?hint(c,data->expr,depth+1):TY_INFER;
            if(vx_slice(c->p,t)) return t;
            if(vx_ref(t)) {
                t=vx_referent(t);
                if(t>=TY_AGGREGATE&&c->p->types[t]->kind==TD_ARRAY) return vx_word(c->p,e->token,"uniq_slice")?vx_uniq_slice_type(c->p,c->p->types[t]->element,e->token):vx_slice_type(c->p,c->p->types[t]->element,e->token);
            }
            return TY_INFER;
        }
        if(vx_word(c->p,e->token,"utf8_validate")) return c->p->utf8_result;
        if(vx_word(c->p,e->token,"utf8_next")) return c->p->utf8_read;
        if((vx_word(c->p,e->token,"parse_int")||vx_word(c->p,e->token,"parse_float"))&&e->type_arg_count==1) {
            Type t=vx_resolve_type(c->p,e->type_args[0]);
            return vx_resolve_type(c->p,vx_apply_type(c->p,c->p->parse_result,&t,1,e->token));
        }
        int vec=vx_vec_builtin(c->p,e->token);
        if(vec>=0) {
            if(vec==VX_VEC_NEW&&e->type_arg_count==1) {
                Type t=vx_resolve_type(c->p,e->type_args[0]);
                return vx_resolve_type(c->p,vx_apply_type(c->p,c->p->vec_result,&t,1,e->token));
            }
            if(vec==VX_VEC_LEN||vec==VX_VEC_CAPACITY) return TY_WORD;
            if(vec==VX_VEC_RESERVE||vec==VX_VEC_PUSH) return c->p->alloc_result;
            if(vec==VX_VEC_CLEAR||vec==VX_VEC_TRUNCATE) return TY_VOID;
            const Arg *owner=NULL;
            for(const Arg *a=e->args;a;a=a->next) if(vx_word(c->p,a->name,"vector")||(!a->name.length&&a==e->args)) { owner=a; break; }
            Type t=owner?hint(c,owner->expr,depth+1):TY_INFER;
            if(vx_ref(t)&&vx_vec(c->p,vx_referent(t))) {
                Type element=c->p->types[vx_referent(t)]->element;
                if(vec==VX_VEC_POP||vec==VX_VEC_REMOVE||vec==VX_VEC_SWAP_REMOVE) return vx_resolve_type(c->p,vx_apply_type(c->p,c->p->vec_pop,&element,1,e->token));
                if(vec==VX_VEC_INSERT) return vx_resolve_type(c->p,vx_apply_type(c->p,c->p->vec_insert,&element,1,e->token));
                return vec==VX_VEC_UNIQ?vx_uniq_slice_type(c->p,element,e->token):vx_slice_type(c->p,element,e->token);
            }
            return TY_INFER;
        }
        int io=vx_io_builtin(c->p,e->token);
        if(io>=0) return vx_io_result(c->p,io);
        int lib=vx_library_builtin(c->p,e->token);
        if(lib>=0) return vx_library_type(c->p,vx_library_calls[lib].result);
        int i=function_id(c->p,e->token); if(i<0) return TY_INFER;
        Function *f=c->p->functions[i]; Type result=vx_resolve_type(c->p,f->result);
        if(f->generic_count&&!e->type_arg_count) return vx_abstract(c->p,result)?TY_INFER:result;
        if(f->generic_count==e->type_arg_count && f->generic_count) {
            Type types[VX_MAX_GENERICS];
            for(int j=0;j<e->type_arg_count;++j) types[j]=vx_kind_arity(c->p,e->type_args[j])?e->type_args[j]:vx_resolve_type(c->p,e->type_args[j]);
            result=vx_substitute_type(c->p,result,f->generic_types,types,f->generic_count,e->token);
        }
        return result;
    }
static Type hint(Checker *c,const Expr *e,int depth) {
    if(!e) return TY_INFER;
    if(depth>VX_MAX_DEPTH) vx_fail(c->p,e->token,"expression tree exceeds prototype depth limit");
    switch(e->kind) {
    case EX_VAR: {
        Binding *b=lookup(c,e->token); if(b) return b->type;
        int id=global_id(c->p,e->token); if(id<0) return TY_INFER;
        vx_check_global(c->p,id); return c->p->globals[id].type;
    }
    case EX_BOOL:return TY_BOOL;
    case EX_TRY: {
        Type t=hint(c,e->left,depth+1); if(!t) return TY_INFER;
        int ok=result_ok(c->p,t,e->token); return c->p->types[vx_resolve_type(c->p,t)]->variants[ok].fields[0];
    }
    case EX_METHOD: {
        Function *f=vx_method_contract(c->p,(Expr *)e); Constraint *bound=&c->p->constraints[e->op];
        return vx_substitute_type(c->p,f->result,&bound->self,&e->operand_type,1,e->token);
    }
    case EX_CALL:return hint_call(c,e,depth);
    case EX_INVOKE: {
        Type t=hint(c,e->left,depth+1); if(vx_ref(t)) t=vx_referent(t);
        return vx_invocable(c->p,t)?c->p->types[t]->signature->result:TY_INFER;
    }
    case EX_FUNCTION:return e->type;
    case EX_CAST:case EX_RECORD:case EX_CHOICE:case EX_BYTES:case EX_TEXT:return e->type;
    case EX_FIELD: {
        Type t=hint(c,e->left,depth+1);
        if(vx_ref(t)) t=vx_referent(t);
        return t>=TY_AGGREGATE?c->p->types[t]->fields[field_id(c->p,t,e->member)].type:TY_INFER;
    }
    case EX_INDEX: {
        Type t=hint(c,e->left,depth+1);
        if(vx_ref(t)) t=vx_referent(t);
        return t>=TY_AGGREGATE && (c->p->types[t]->kind==TD_ARRAY||vx_slice(c->p,t))?c->p->types[t]->element:TY_INFER;
    }
    case EX_REF: { Type t=hint(c,e->left,depth+1); if(vx_fn_view(c->p,t)) return vx_fn_view_type(c->p,c->p->types[t]->element,e->op!=0,e->token); if(vx_slice(c->p,t)) return e->op?vx_uniq_slice_type(c->p,c->p->types[t]->element,e->token):vx_slice_type(c->p,c->p->types[t]->element,e->token); return t?(e->op?vx_exclusive_type(c->p,t,e->op==1,e->token):vx_reference_type(c->p,t,e->token)):TY_INFER; }
    case EX_DEREF: {
        Type t=hint(c,e->left,depth+1); return vx_ref(t)?vx_referent(t):TY_INFER;
    }
    case EX_UNARY:
        if(e->op==VX_NOT) return TY_BOOL;
        /* Negative literals take context from a typed peer. */
        return hint(c,e->left,depth+1);
    case EX_BINARY:
        if((e->op>=VX_EQ&&e->op<=VX_GE)||e->op==VX_LAND||e->op==VX_LOR) return TY_BOOL;
        { Type t=hint(c,e->left,depth+1); return t?t:hint(c,e->right,depth+1); }
    default:return TY_INFER;
    }
}
static bool has_real(const Expr *e,int depth) {
    if(!e||depth>VX_MAX_DEPTH) return false;
    return e->kind==EX_REAL || has_real(e->left,depth+1) || has_real(e->right,depth+1);
}
static bool has_negative(const Expr *e,int depth) {
    if(!e||depth>VX_MAX_DEPTH) return false;
    return (e->kind==EX_UNARY&&e->op==VX_NEG) || has_negative(e->left,depth+1) || has_negative(e->right,depth+1);
}
static Type numeric_hint(Checker *c,Expr *e,Type expected) {
    if(vx_integer(expected)||vx_float(expected)) return expected;
    Type t=hint(c,e->left,0); if(!t) t=hint(c,e->right,0);
    if(t) return t;
    if(has_real(e,0)) return TY_F64;
    return has_negative(e,0)?TY_DEFAULT_SINT:TY_DEFAULT_UINT;
}
static Type check_expr(Checker *c,Expr *e,Type expected);
static _Noreturn void type_mismatch(VxnProgram *p,Token at,Type expected,Type actual) {
    char want[256],got[256]; vx_describe_type(p,expected,want,sizeof want); vx_describe_type(p,actual,got,sizeof got);
    vx_fail(p,at,"expected %s, got %s",want,got);
}
static void check_default(VxnProgram *p,Function *f,int index) {
    Param *a=&f->params[index];
    if(!a->default_value || a->default_state==2) return;
    if(a->default_state==1) vx_fail(p,a->name,"cyclic default argument dependency");
    if(++p->check_depth>VX_MAX_DEPTH) vx_fail(p,a->name,"default argument dependency depth limit exceeded");
    a->default_state=1; a->type=vx_resolve_type(p,a->type);
    if(!vx_stage_type(p,a->type)) vx_fail(p,a->name,"default arguments require owned compile-time data; no references or resource handles");
    Function *context=vx_alloc(p,sizeof *context); context->name=a->name; context->result=a->type;
    Checker *c=vx_alloc(p,sizeof *c); c->p=p; c->f=context; c->static_context=1;
    check_expr(c,a->default_value,a->type); vx_stage_value(p,context,a->default_value);
    a->default_state=2; --p->check_depth;
}
static uint32_t map_arguments(VxnProgram *p,Expr *e,int count,const Param *params,const char *const *names) {
    if(e->argc>count) vx_fail(p,e->token,"expected %d arguments, got %d",count,e->argc);
    uint32_t seen=0; bool named=false; int position=0;
    for(Arg *a=e->args;a;a=a->next) {
        int id=position++;
        if(a->name.length) {
            named=true;
            for(id=0;id<count;++id) if(params?vx_same(p,a->name,params[id].name):vx_word(p,a->name,names[id])) break;
            if(id==count) vx_fail(p,a->name,"unknown named argument");
        } else if(named) vx_fail(p,a->expr->token,"positional arguments must precede named arguments");
        uint32_t bit=UINT32_C(1)<<id;
        if(seen&bit) vx_fail(p,a->name.length?a->name:a->expr->token,"duplicate argument for one parameter");
        seen|=bit; a->field=id;
    }
    return seen;
}
static Type auto_deref(Checker *c,Expr **e,Type t) {
    if(!vx_ref(t)) return t;
    Expr *d=vx_alloc(c->p,sizeof *d);
    d->kind=EX_DEREF; d->left=*e; d->token=(*e)->token; d->type=vx_referent(t);
    *e=d; return d->type;
}
static bool stable_place(const VxnProgram *p,const Expr *e) {
    if(e->kind==EX_VAR||e->kind==EX_DEREF) return true;
    if(e->kind==EX_INDEX&&vx_slice(p,e->left->type)) return true;
    return (e->kind==EX_FIELD||e->kind==EX_INDEX)&&stable_place(p,e->left);
}
/* Checked places only. A const exclusive handle may still write its pointee. */
static int exclusive_place(Checker *c,const Expr *e,bool writable) {
    if(e->kind==EX_DEREF) {
        if(!vx_uniq(e->left->type)) vx_fail(c->p,e->token,"cannot mutate through a shared reference or create an exclusive reborrow");
        if(writable&&!vx_writable(e->left->type)) vx_fail(c->p,e->token,"uniq const does not permit writable access");
        return -1;
    }
    if(e->kind==EX_VAR) {
        Binding *b=lookup(c,e->token);
        if(!b) vx_fail(c->p,e->token,"exclusive access requires a local place");
        if(writable&&!b->mutable_binding) vx_fail(c->p,e->token,"cannot assign or mutably borrow through a const binding");
        return b->id;
    }
    if(e->kind==EX_FIELD||e->kind==EX_INDEX) {
        if(vx_uniq_slice(c->p,e->left->type)) return -1;
        if(vx_slice(c->p,e->left->type)) vx_fail(c->p,e->token,"cannot mutate through a shared Slice or create an exclusive reborrow");
        return exclusive_place(c,e->left,writable);
    }
    vx_fail(c->p,e->token,"assignment target or exclusive borrow requires a stable local place");
}
static unsigned block(Checker *c,Stmt *s,bool nested);
static bool never(const Expr *e) {
    if(!e) return false;
    if(e->kind==EX_PANIC) return true;
    if(e->left&&never(e->left)) return true;
    if(e->right && !(e->kind==EX_BINARY&&(e->op==VX_LAND||e->op==VX_LOR)) && never(e->right)) return true;
    for(Arg *a=e->args;a;a=a->next) if(never(a->expr)) return true;
    return false;
}
static bool arithmetic_fault(Expr *e) {
    Type type; uint64_t value;
    if(vx_constant(e,&type,&value)) return false;
    Type t=e->operand_type;
    if(!vx_integer(t)) return false;
    if(e->op==VX_DIV||e->op==VX_MOD) {
        if(!e->right||!vx_constant(e->right,&type,&value)||!value) return true;
        return e->op==VX_DIV && vx_mode(t)==1 && vx_signed_type(t) && value==vx_mask(vx_width(t));
    }
    if(e->op==VX_SHL||e->op==VX_SHR)
        return !e->right||!vx_constant(e->right,&type,&value)||value>=vx_width(t);
    return vx_mode(t)==1 && (e->op==VX_ADD||e->op==VX_SUB||e->op==VX_MUL||e->op==VX_NEG);
}
static void check_pattern(Checker *c,Type t,Arm *a,Pattern *n) {
    if(n->kind==PAT_OR) {
        for(Pattern *child=n->children;child;child=child->next) check_pattern(c,t,a,child);
    } else if(n->kind==PAT_STRUCT||n->kind==PAT_ARRAY) {
        bool record=n->kind==PAT_STRUCT;
        if(t<TY_AGGREGATE||c->p->types[t]->kind!=(record?TD_STRUCT:TD_ARRAY))
            vx_fail(c->p,n->token,"%s pattern requires a %s value",record?"struct":"array",record?"struct":"fixed-array");
        if(record&&vx_resolve_type(c->p,n->type)!=t) vx_fail(c->p,n->token,"pattern belongs to a different struct");
        TypeDef *d=c->p->types[t]; Pattern *fields[VX_MAX_FIELDS]={0}; int index=0;
        for(Pattern *child=n->children;child;child=child->next) {
            int id=index++;
            if(record) {
                id=0; while(id<d->count&&!vx_same(c->p,child->field,d->fields[id].name)) ++id;
                if(id==d->count) vx_fail(c->p,child->field,"unknown struct pattern field");
                if(fields[id]) vx_fail(c->p,child->field,"duplicate struct pattern field");
                fields[id]=child;
            }
            check_pattern(c,record?d->fields[id].type:d->element,a,child);
        }
        if(n->count!=d->count) vx_fail(c->p,n->token,"%s pattern must match every %s; use _ to ignore values",record?"struct":"array",record?"field":"element");
        if(record&&d->count) {
            n->children=fields[0];
            for(int i=0;i<d->count;++i) fields[i]->next=i+1<d->count?fields[i+1]:NULL;
        }
    } else if(n->kind==PAT_CONSTRUCTOR) {
        if(t<TY_AGGREGATE||c->p->types[t]->kind!=TD_CHOICE)
            vx_fail(c->p,n->token,"constructor pattern requires a choice value");
        if(vx_resolve_type(c->p,n->type)!=t) vx_fail(c->p,n->token,"pattern belongs to a different choice");
        TypeDef *d=c->p->types[t]; int id=0;
        if(d->recursive_cell&&!c->f->recursive_group) vx_fail(c->p,n->token,"recursive lazy state is sealed");
        if(d->lazy&&!c->f->lazy_helper) vx_fail(c->p,n->token,"Lazy state is sealed; use force or force_take");
        while(id<d->count&&!vx_same(c->p,d->variants[id].name,n->constructor)) ++id;
        if(id==d->count) vx_fail(c->p,n->constructor,"unknown choice constructor");
        n->variant=id; Variant *v=&d->variants[id];
        if(n->count!=v->count) vx_fail(c->p,n->token,"pattern has wrong number of payload bindings or patterns");
        int i=0; for(Pattern *child=n->children;child;child=child->next) check_pattern(c,v->fields[i++],a,child);
    } else if(n->kind==PAT_TEXT) {
        if(!vx_str(c->p,t)) vx_fail(c->p,n->token,"text pattern requires a str view");
        n->value=(uint64_t)n->literal->id;
        /* Coverage uses decoded text, not escape spelling; a catchall is still required. */
        for(const TextLiteral *l=c->p->text_literals;l;l=l->next)
            if(l->count==n->literal->argc&&!memcmp(l->cells,n->literal->cells,(size_t)l->count*sizeof(uint64_t))&&(uint64_t)l->id<n->value)
                n->value=(uint64_t)l->id;
    } else if(n->kind==PAT_LITERAL||n->kind==PAT_RANGE) {
        if(t!=TY_BOOL&&!vx_integer(t)) vx_fail(c->p,n->token,"literal patterns require an integer or bool value");
        Type literal=check_expr(c,n->literal,t);
        if(!vx_constant(n->literal,&literal,&n->value)) vx_fail(c->p,n->token,"pattern literal is out of range");
        if(n->kind==PAT_RANGE) {
            if(!vx_integer(t)) vx_fail(c->p,n->token,"range patterns require integer values");
            literal=check_expr(c,n->limit,t);
            if(!vx_constant(n->limit,&literal,&n->upper)) vx_fail(c->p,n->token,"pattern endpoint is out of range");
            uint64_t sign=vx_signed_type(t)?UINT64_C(1)<<(vx_width(t)-1):0;
            if((n->value^sign)>(n->upper^sign)) vx_fail(c->p,n->token,"range pattern lower endpoint exceeds upper endpoint");
        }
    } else if(n->kind==PAT_BIND) {
        Param *b=&a->bindings[n->binding];
        if(b->type) {
            if(b->type!=t) vx_fail(c->p,n->token,"pattern alternative binding types must agree");
        } else { b->type=t; a->locals[n->binding]=bind(c,b->name,t,b->mutable_binding,true); }
    }
    n->type=t;
}
static void arm_bindings(Checker *c,Type t,Arm *a) {
    check_pattern(c,t,a,a->pattern); vx_pattern_layout(c->p,a,a->pattern,0);
    if(a->guard) { ++c->guard; check_expr(c,a->guard,TY_BOOL); --c->guard; }
}
static Type match_type(Checker *c,Expr *e) {
    Type t=check_expr(c,e,TY_INFER);
    if(t!=TY_BOOL&&!vx_integer(t)&&!vx_str(c->p,t)&&(t<TY_AGGREGATE||
       (c->p->types[t]->kind!=TD_CHOICE&&c->p->types[t]->kind!=TD_STRUCT&&c->p->types[t]->kind!=TD_ARRAY)))
        vx_fail(c->p,e->token,"match requires a choice, integer or bool value, struct, fixed array or str");
    return t;
}
static Type check_io(Checker *c,Expr *e,int op) {
    int count=op==VX_IO_READ_LINE?3:op<=VX_IO_STDERR?0:(op>=VX_IO_WRITE_BYTE&&op<=VX_IO_PRINTLN)||op==VX_IO_READ_BUFFER?2:1;
    if(e->argc!=count) vx_fail(c->p,e->token,"I/O operation expects %d arguments, got %d",count,e->argc);
    bool path=op>=VX_IO_OPEN_READ&&op<=VX_IO_OPEN_APPEND;
    const char *names[]={path?"path":"file",op==VX_IO_WRITE_BYTE?"byte":op==VX_IO_WRITE?"bytes":op==VX_IO_READ_BUFFER||op==VX_IO_READ_LINE?"buffer":"value","limit"};
    (void)map_arguments(c->p,e,count,NULL,names);
    for(Arg *a=e->args;a;a=a->next) {
        if(!path && !a->field) check_expr(c,a->expr,TY_FILE);
        else if(op==VX_IO_READ_LINE) {
            Type expected=a->field==2?TY_WORD:vx_exclusive_type(c->p,vx_resolve_type(c->p,vx_vec_type(c->p,TY_INT_BASE,a->expr->token)),true,a->expr->token);
            check_expr(c,a->expr,expected);
        }
        else if(path || op==VX_IO_WRITE) {
            Type t=check_expr(c,a->expr,TY_INFER);
            if(t<TY_AGGREGATE || (c->p->types[t]->kind!=TD_ARRAY&&!vx_slice(c->p,t)) || c->p->types[t]->element!=TY_INT_BASE)
                vx_fail(c->p,a->expr->token,"I/O bytes require a fixed array of wrap uint8 or Slice[wrap uint8] (or a byte literal)");
        } else if(op==VX_IO_READ_BUFFER) {
            Type t=check_expr(c,a->expr,TY_INFER);
            if(!vx_uniq_slice(c->p,t)||c->p->types[t]->element!=TY_INT_BASE)
                vx_fail(c->p,a->expr->token,"io_read requires UniqSlice[wrap uint8]");
        } else if(op==VX_IO_WRITE_BYTE) check_expr(c,a->expr,TY_INT_BASE);
        else if(op==VX_IO_PRINT || op==VX_IO_PRINTLN) {
            Type t=check_expr(c,a->expr,TY_INFER);
            if(t!=TY_BOOL && !vx_integer(t) && !vx_float(t)) vx_fail(c->p,a->expr->token,"I/O print requires a scalar; use io_write for bytes");
            e->value=(t==TY_BOOL?1:vx_width(t)) | ((unsigned)(t==TY_BOOL?3:vx_float(t)?2:vx_signed_type(t))<<8);
        }
    }
    e->kind=EX_IO; e->op=op;
    if(op>VX_IO_STDERR) use(c,e->token,-1,EF_IO|EF_ALLOC|EF_BLOCK|(op==VX_IO_READ_BUFFER||op==VX_IO_READ_LINE?EF_WRITE:0));
    return vx_io_result(c->p,op);
}
static Type check_library(Checker *c,Expr *e,int op) {
    const LibraryCall *spec=&vx_library_calls[op];
    if(e->argc!=spec->count) vx_fail(c->p,e->token,"%s expects %d arguments, got %d",spec->name,spec->count,e->argc);
    (void)map_arguments(c->p,e,spec->count,NULL,vx_library_parameter_names[op]);
    for(Arg *a=e->args;a;a=a->next) {
        int i=a->field;
        if(spec->arguments[i]==LA_BYTES) {
            Type t=check_expr(c,a->expr,TY_INFER);
            if(t<TY_AGGREGATE||c->p->types[t]->kind!=TD_ARRAY||c->p->types[t]->element!=TY_INT_BASE)
                vx_fail(c->p,a->expr->token,"library text requires a fixed array of wrap uint8");
            e->value=(uint64_t)c->p->types[t]->count;
        } else check_expr(c,a->expr,vx_library_type(c->p,spec->arguments[i]));
    }
    e->kind=EX_LIBRARY; e->op=op; use(c,e->token,-1,spec->effects);
    return vx_library_type(c->p,spec->result);
}
static Type check_vec(Checker *c,Expr *e,int op) {
    Type element;
    if(op==VX_VEC_NEW) {
        if(e->type_arg_count!=1||e->argc) vx_fail(c->p,e->token,"vec_new[T] expects one owned type argument and no values");
        element=vx_resolve_type(c->p,e->type_args[0]);
        (void)vx_resolve_type(c->p,vx_vec_type(c->p,element,e->token));
    } else {
        int count=op==VX_VEC_INSERT?3:op==VX_VEC_RESERVE||op==VX_VEC_PUSH||op==VX_VEC_REMOVE||op==VX_VEC_SWAP_REMOVE||op==VX_VEC_TRUNCATE?2:1;
        if(e->type_arg_count||e->argc!=count) vx_fail(c->p,e->token,"vector operation expects %d arguments and no type arguments",count);
        const char *names[]={"vector",op==VX_VEC_RESERVE?"additional":op==VX_VEC_TRUNCATE?"length":op==VX_VEC_PUSH?"value":"index","value"};
        (void)map_arguments(c->p,e,count,NULL,names);
        Type t=check_expr(c,vx_argument(e,0)->expr,TY_INFER);
        if(!vx_ref(t)||!vx_vec(c->p,vx_referent(t))) vx_fail(c->p,e->token,"vector operation requires ref Vec[T] or uniq mut Vec[T]");
        bool writable=op==VX_VEC_RESERVE||op==VX_VEC_PUSH||op==VX_VEC_POP||op==VX_VEC_CLEAR||op==VX_VEC_UNIQ||op==VX_VEC_REMOVE||op==VX_VEC_SWAP_REMOVE||op==VX_VEC_TRUNCATE||op==VX_VEC_INSERT;
        if(writable?!vx_writable(t):vx_uniq(t))
            vx_fail(c->p,e->token,writable?"vector mutation requires uniq mut Vec[T]":"vector observation requires a shared ref reborrow");
        element=c->p->types[vx_referent(t)]->element;
        if(count>=2) check_expr(c,vx_argument(e,1)->expr,op==VX_VEC_PUSH?element:TY_WORD);
        if(count==3) check_expr(c,vx_argument(e,2)->expr,element);
    }
    e->kind=EX_VEC; e->op=op; e->operand_type=element;
    if(op==VX_VEC_NEW) {
        use(c,e->token,-1,EF_ALLOC|EF_BLOCK);
        return vx_resolve_type(c->p,vx_apply_type(c->p,c->p->vec_result,&element,1,e->token));
    }
    if(op==VX_VEC_RESERVE||op==VX_VEC_PUSH||op==VX_VEC_CLEAR)
        use(c,e->token,-1,EF_ALLOC|EF_BLOCK|EF_WRITE|vx_cleanup_effects(c->p,element));
    if(op==VX_VEC_INSERT) {
        use(c,e->token,-1,EF_ALLOC|EF_BLOCK|EF_WRITE);
        return vx_resolve_type(c->p,vx_apply_type(c->p,c->p->vec_insert,&element,1,e->token));
    }
    if(op==VX_VEC_POP||op==VX_VEC_REMOVE||op==VX_VEC_SWAP_REMOVE||op==VX_VEC_TRUNCATE) {
        use(c,e->token,-1,EF_WRITE|vx_cleanup_effects(c->p,element));
        return op==VX_VEC_TRUNCATE?TY_VOID:vx_resolve_type(c->p,vx_apply_type(c->p,c->p->vec_pop,&element,1,e->token));
    }
    if(op==VX_VEC_SLICE||op==VX_VEC_UNIQ)
        return vx_resolve_type(c->p,op==VX_VEC_UNIQ?vx_uniq_slice_type(c->p,element,e->token):vx_slice_type(c->p,element,e->token));
    if(op==VX_VEC_CLEAR) return TY_VOID;
    return op==VX_VEC_LEN||op==VX_VEC_CAPACITY?TY_WORD:c->p->alloc_result;
}
static Type check_string(Checker *c,Expr *e,int op) {
    bool query=(op>=VX_STR_EQUAL&&op<=VX_STR_SCALAR_LEN)||op==VX_STR_SLICE||op==VX_STR_FROM_UTF8;
    bool pair=op>=VX_STR_EQUAL&&op<=VX_STR_COMPARE;
    int count=op==VX_STR_SLICE?3:op==VX_STR_FROM_UTF8?1:op==VX_STRING_NEW?0:op==VX_STRING_FROM_STR||op==VX_STRING_CLEAR||op==VX_STRING_CAPACITY||op==VX_STR_SCALAR_LEN?1:2;
    if(e->type_arg_count||e->argc!=count) vx_fail(c->p,e->token,"string operation expects %d arguments and no type arguments",count);
    const char *names[]={op==VX_STR_FROM_UTF8?"bytes":op==VX_STR_EQUAL||op==VX_STR_COMPARE?"left":"text",
        op==VX_STRING_APPEND||op==VX_STR_ENDS_WITH?"suffix":op==VX_STR_STARTS_WITH?"prefix":
        op==VX_STRING_PUSH_SCALAR?"scalar":op==VX_STR_EQUAL||op==VX_STR_COMPARE?"right":
        op==VX_STR_SLICE?"start":op==VX_STR_IS_BOUNDARY||op==VX_STRING_TRUNCATE?"offset":pair?"needle":"additional","count"};
    (void)map_arguments(c->p,e,count,NULL,names);
    Type text=vx_resolve_type(c->p,c->p->string_type),view=vx_resolve_type(c->p,vx_str_type(c->p,e->token));
    for(Arg *a=e->args;a;a=a->next) {
        Type expected;
        if(op==VX_STR_FROM_UTF8) expected=vx_slice_type(c->p,TY_INT_BASE,e->token);
        else if(pair||(query&&a->field==0)||op==VX_STRING_FROM_STR||(a->field==1&&op==VX_STRING_APPEND)) expected=view;
        else if(a->field>=1) expected=TY_WORD;
        else expected=op==VX_STRING_CAPACITY?vx_reference_type(c->p,text,e->token):vx_exclusive_type(c->p,text,true,e->token);
        check_expr(c,a->expr,expected);
    }
    unsigned effects=0;
    if(op==VX_STRING_NEW||op==VX_STRING_FROM_STR) effects=EF_ALLOC|EF_BLOCK;
    else if(op==VX_STRING_CLEAR||op==VX_STRING_TRUNCATE) effects=EF_WRITE;
    else if(op!=VX_STRING_CAPACITY&&!query) effects=EF_ALLOC|EF_BLOCK|EF_WRITE;
    if(effects) use(c,e->token,-1,effects);
    e->kind=EX_STRING; e->op=op; return string_result_type(c->p,op);
}
static int callable_target(const Expr *e) {
    if(!e) return -1;
    if(e->kind==EX_FUNCTION) return e->id;
    if(e->kind==EX_VAR&&e->value) return (int)e->value-1;
    if(e->kind==EX_FN_COERCE) return callable_target(e->left);
    return -1;
}
static Type check_invoke(Checker *c,Expr *e) {
    Type t=check_expr(c,e->left,TY_INFER);
    t=auto_deref(c,&e->left,t);
    if(!vx_invocable(c->p,t)) vx_fail(c->p,e->token,"value is not callable; invocation requires a callable value");
    Function *f=c->p->types[t]->signature;
    if(e->type_arg_count) vx_fail(c->p,e->token,"callable values do not accept type arguments");
    if(e->argc!=f->param_count) vx_fail(c->p,e->token,"callable expects %d arguments, got %d",f->param_count,e->argc);
    (void)map_arguments(c->p,e,f->param_count,f->params,NULL);
    for(Arg *a=e->args;a;a=a->next) check_expr(c,a->expr,f->params[a->field].type);
    e->operand_type=t;
    e->op=f->call_mode;
    if(c->p->types[t]->callable_bound) {
        e->call_contract=c->p->types[t]->callable_bound;
        c->f->uses_references=true;
    }
    if(f->call_mode) c->f->uses_references=true;
    if(vx_fn_view(c->p,t)) {
        e->kind=EX_VIEW_CALL; c->p->types[t]->invoked=true; c->f->uses_references=true;
        use(c,e->token,-1,EF_ALL&~vx_forbidden(f->caps)); c->f->last_use->indirect=true;
        return f->result;
    }
    if(f->call_mode==1&&stable_place(c->p,e->left)) (void)exclusive_place(c,e->left,true);
    if(f->call_mode==2&&e->left->kind==EX_DEREF) vx_fail(c->p,e->token,"consuming closure invocation requires ownership of the environment");
    if(vx_box_fn(c->p,t)) {
        e->kind=EX_BOX_CALL; c->p->types[t]->invoked=true; c->f->uses_references=true;
        use(c,e->token,-1,EF_ALL&~vx_forbidden(f->caps)); c->f->last_use->indirect=true;
        if(e->op==2||!stable_place(c->p,e->left)) { use(c,e->token,-1,vx_cleanup_effects(c->p,t)); c->f->last_use->cleanup=true; }
        return f->result;
    }
    if(vx_closure(c->p,t)) {
        e->id=c->p->types[t]->closure_target; e->kind=EX_CLOSURE_CALL; c->f->uses_references=true;
        if(vx_opaque(c->p,t)) use(c,e->token,-1,EF_ALL&~vx_forbidden(f->caps));
        else use(c,e->token,e->id,0);
        return f->result;
    }
    int target=vx_function_item(c->p,t)?c->p->types[t]->closure_target:callable_target(e->left);
    if(target>=0) {
        e->id=target; e->kind=EX_CALL; use(c,e->token,e->id,0);
    } else {
        e->kind=EX_INVOKE; c->p->types[t]->invoked=true;
        use(c,e->token,-1,EF_ALL&~vx_forbidden(f->caps)); c->f->last_use->indirect=true;
    }
    return f->result;
}
static Type check_box_fn(Checker *c,Expr *e) {
    if(e->type_arg_count!=1||e->argc!=1) vx_fail(c->p,e->token,"box_fn[contract] expects one callable value");
    const char *names[]={"value"}; (void)map_arguments(c->p,e,1,NULL,names);
    Type contract=vx_resolve_type(c->p,e->type_args[0]);
    Type marker=vx_resolve_type(c->p,vx_dyn_fn_type(c->p,contract,e->token));
    Type source=check_expr(c,e->args->expr,TY_INFER);
    if(vx_dependent(c->p,source)) vx_fail(c->p,e->token,"boxed callable erasure requires owned captures; borrowed environments cannot escape");
    if(!vx_invocable(c->p,source))
        vx_fail(c->p,e->token,"box_fn requires an owned callable value or a declared callable bound");
    vx_check_view_contract(c->p,source,contract,e->token,false);
    e->kind=EX_BOX_FN; e->left=e->args->expr; e->args=NULL; e->argc=0;
    e->operand_type=source; e->call_contract=contract;
    e->id=vx_box_fn(c->p,source)?-1:vx_box_fn_payload(c->p,source,e->token);
    use(c,e->token,-1,EF_ALLOC|EF_BLOCK|vx_cleanup_effects(c->p,source));
    return vx_resolve_type(c->p,vx_apply_type(c->p,c->p->box_result,&marker,1,e->token));
}
static Type check_recursive_force(Checker *c,Expr *e,Type expected);
static Type check_call(Checker *c,Expr *e,Type expected) {
    if(vx_word(c->p,e->token,"try_force")) return check_recursive_force(c,e,expected);
    if(lookup(c,e->token)) {
        Expr *callee=vx_alloc(c->p,sizeof *callee); callee->kind=EX_VAR; callee->token=e->token; e->left=callee;
        return check_invoke(c,e);
    }
    Type actual=TY_VOID;
    int string=vx_string_builtin(c->p,e->token); if(string>=0) return check_string(c,e,string);
    if(vx_word(c->p,e->token,"box_fn")) return check_box_fn(c,e);
    if(vx_word(c->p,e->token,"delay")) {
        if(e->type_arg_count||e->argc!=1) vx_fail(c->p,e->token,"delay expects one thunk argument and no type arguments");
        const char *names[]={"thunk"}; (void)map_arguments(c->p,e,1,NULL,names);
        Type thunk=check_expr(c,e->args->expr,TY_INFER);
        if(!vx_invocable(c->p,thunk)||c->p->types[thunk]->signature->param_count)
            vx_fail(c->p,e->token,"delay requires a zero-argument callable");
        Type result=c->p->types[thunk]->signature->result;
        if(result==TY_VOID||vx_dependent(c->p,result)||vx_dyn_fn(c->p,result))
            vx_fail(c->p,e->token,"lazy result must be an owned non-void value");
        if(vx_exclusive(c->p,thunk)||vx_exclusive_captures(c->p,thunk)) vx_fail(c->p,e->token,"lazy values support shared borrowed captures; exclusive captures require further loan contracts");
        Type args[]={result,thunk};
        Type lazy=vx_resolve_type(c->p,vx_apply_type(c->p,c->p->lazy_type,args,2,e->token));
        e->kind=EX_CHOICE; e->member=c->p->types[lazy]->variants[0].name;
        e->args->name=(Token){0}; e->args->field=0;
        return lazy;
    }
    if(vx_word(c->p,e->token,"force")||vx_word(c->p,e->token,"force_take")) {
        bool borrow=vx_word(c->p,e->token,"force");
        if(e->type_arg_count||e->argc!=1) vx_fail(c->p,e->token,"forcing expects one argument and no type arguments");
        const char *names[]={"pending"}; (void)map_arguments(c->p,e,1,NULL,names);
        Type input=check_expr(c,e->args->expr,TY_INFER),t=borrow&&vx_ref(input)?vx_referent(input):input;
        if((borrow&&!vx_writable(input))||t<TY_AGGREGATE||!c->p->types[t]->lazy)
            vx_fail(c->p,e->token,borrow?"force requires uniq mut Lazy[T, F]":"force_take consumes a Lazy[T, F] value");
        e->id=vx_lazy_helper(c->p,t,borrow,c->f,e->token);
        Function *helper=c->p->functions[e->id];
        e->kind=helper->generic_count?EX_GENERIC_CALL:EX_CALL;
        e->type_arg_count=helper->generic_count;
        memcpy(e->type_args,helper->generic_types,(size_t)helper->generic_count*sizeof(Type));
        use(c,e->token,e->id,0); return helper->result;
    }
    if(vx_word(c->p,e->token,"str_bytes")||vx_word(c->p,e->token,"str_byte_len")) {
        bool bytes=vx_word(c->p,e->token,"str_bytes");
        if(e->type_arg_count||e->argc!=1) vx_fail(c->p,e->token,"str operation expects one argument and no type arguments");
        const char *names[]={"text"}; (void)map_arguments(c->p,e,1,NULL,names);
        check_expr(c,e->args->expr,vx_resolve_type(c->p,vx_str_type(c->p,e->token)));
        if(bytes) {
            e->kind=EX_SLICE_BORROW; e->left=e->args->expr; e->args=NULL; e->argc=0;
            return vx_resolve_type(c->p,vx_slice_type(c->p,TY_INT_BASE,e->token));
        }
        e->kind=EX_SLICE_LENGTH; return TY_WORD;
    }
    if(vx_word(c->p,e->token,"string_from_utf8")||vx_word(c->p,e->token,"string_into_bytes")||
       vx_word(c->p,e->token,"string_bytes")||vx_word(c->p,e->token,"string_byte_len")||vx_word(c->p,e->token,"string_view")) {
        bool from=vx_word(c->p,e->token,"string_from_utf8"),into=vx_word(c->p,e->token,"string_into_bytes");
        bool view=vx_word(c->p,e->token,"string_view");
        bool bytes=view||vx_word(c->p,e->token,"string_bytes");
        if(e->type_arg_count||e->argc!=1) vx_fail(c->p,e->token,"string operation expects one argument and no type arguments");
        const char *names[]={from?"bytes":"text"}; (void)map_arguments(c->p,e,1,NULL,names);
        Type vec=vx_resolve_type(c->p,vx_vec_type(c->p,TY_INT_BASE,e->token)),text=vx_resolve_type(c->p,c->p->string_type);
        check_expr(c,e->args->expr,from?vec:into?text:vx_reference_type(c->p,text,e->token));
        if(from||into) {
            use(c,e->token,-1,vx_cleanup_effects(c->p,text)); c->f->last_use->cleanup=true;
            e->kind=from?EX_STRING_FROM:EX_STRING_INTO;
            return from?c->p->string_result:vec;
        }
        e->kind=EX_VEC; e->op=bytes?VX_VEC_SLICE:VX_VEC_LEN; e->operand_type=TY_INT_BASE;
        return view?vx_resolve_type(c->p,vx_str_type(c->p,e->token)):bytes?vx_resolve_type(c->p,vx_slice_type(c->p,TY_INT_BASE,e->token)):TY_WORD;
    }
    if(vx_word(c->p,e->token,"utf8_validate")||vx_word(c->p,e->token,"utf8_next")) {
        bool next=vx_word(c->p,e->token,"utf8_next"); int count=next?2:1;
        if(e->type_arg_count||e->argc!=count) vx_fail(c->p,e->token,"UTF-8 operation expects %d arguments and no type arguments",count);
        const char *names[]={"bytes","offset"}; (void)map_arguments(c->p,e,count,NULL,names);
        for(Arg *a=e->args;a;a=a->next) {
            if(a->field) { check_expr(c,a->expr,TY_WORD); continue; }
            Type input=check_expr(c,a->expr,TY_INFER);
            if(input<TY_AGGREGATE||(c->p->types[input]->kind!=TD_ARRAY&&!vx_shared_slice(c->p,input))||c->p->types[input]->element!=TY_INT_BASE)
                vx_fail(c->p,a->expr->token,"UTF-8 bytes require wrap uint8 array or shared Slice; reborrow an exclusive view with ref");
        }
        e->kind=EX_UTF8; e->op=next; return next?c->p->utf8_read:c->p->utf8_result;
    }
    if(vx_word(c->p,e->token,"parse_int")||vx_word(c->p,e->token,"parse_float")) {
        bool floating=vx_word(c->p,e->token,"parse_float");
        if(e->type_arg_count!=1||e->argc!=1) vx_fail(c->p,e->token,floating?"parse_float[T] expects one float type and one byte input":"parse_int[T] expects one integer type and one byte input");
        Type t=vx_resolve_type(c->p,e->type_args[0]);
        if(floating?!vx_float(t):!vx_integer(t)) vx_fail(c->p,e->token,floating?"parse_float requires float32 or float64":"parse_int requires a concrete integer result type");
        const char *names[]={"bytes"}; (void)map_arguments(c->p,e,1,NULL,names);
        Type input=check_expr(c,e->args->expr,TY_INFER);
        if(input<TY_AGGREGATE||(c->p->types[input]->kind!=TD_ARRAY&&!vx_shared_slice(c->p,input))||c->p->types[input]->element!=TY_INT_BASE)
            vx_fail(c->p,e->token,"numeric parsing bytes require wrap uint8 array or shared Slice; reborrow an exclusive view with ref");
        e->kind=EX_PARSE_NUMBER; e->operand_type=t; e->type_arg_count=0;
        return vx_resolve_type(c->p,vx_apply_type(c->p,c->p->parse_result,&t,1,e->token));
    }
    if(vx_word(c->p,e->token,"fn_ref")||vx_word(c->p,e->token,"fn_uniq")) {
        bool exclusive=vx_word(c->p,e->token,"fn_uniq");
        if(e->type_arg_count!=1||e->argc!=1) vx_fail(c->p,e->token,"fn_ref/fn_uniq expects one callable contract and one borrowed callable");
        const char *names[]={"callable"}; (void)map_arguments(c->p,e,1,NULL,names);
        Type contract=vx_resolve_type(c->p,e->type_args[0]);
        Type view=vx_resolve_type(c->p,vx_fn_view_type(c->p,contract,exclusive,e->token));
        Type source=check_expr(c,e->args->expr,TY_INFER);
        if(!vx_ref(source)||(exclusive?!vx_writable(source):vx_uniq(source)))
            vx_fail(c->p,e->token,exclusive?"fn_uniq requires uniq mut callable":"fn_ref requires ref callable");
        Type base=vx_referent(source);
        if(!vx_invocable(c->p,base)) vx_fail(c->p,e->token,"callable view requires a callable value");
        vx_check_view_contract(c->p,base,view,e->token,false);
        e->kind=EX_FN_VIEW; e->operand_type=base; e->left=e->args->expr; e->args=NULL; e->argc=0;
        e->id=(vx_closure(c->p,base)||vx_function_item(c->p,base))?c->p->types[base]->closure_target:-1;
        e->type_arg_count=0; return view;
    }
    int vec=vx_vec_builtin(c->p,e->token); if(vec>=0) return check_vec(c,e,vec);
        if(vx_word(c->p,e->token,"box_into_inner")) {
            if(e->type_arg_count||e->argc!=1) vx_fail(c->p,e->token,"box_into_inner expects one Box[T] value and no type arguments");
            const char *names[]={"owner"}; (void)map_arguments(c->p,e,1,NULL,names);
            Type t=check_expr(c,e->args->expr,TY_INFER);
            if(t<TY_AGGREGATE||c->p->types[t]->kind!=TD_BOX) vx_fail(c->p,e->token,"box_into_inner requires an owned Box[T] value");
            if(vx_box_fn(c->p,t)) vx_fail(c->p,e->token,"erased callable payloads cannot be extracted; invoke or move the Box");
            e->kind=EX_BOX_TAKE; e->operand_type=c->p->types[t]->element;
            use(c,e->token,-1,EF_ALLOC|EF_BLOCK);
            return e->operand_type;
        }
        if(vx_word(c->p,e->token,"box_new")) {
            if(e->type_arg_count!=1||e->argc!=1) vx_fail(c->p,e->token,"box_new[T] expects one type argument and one value");
            const char *names[]={"value"}; (void)map_arguments(c->p,e,1,NULL,names);
            Type t=vx_resolve_type(c->p,e->type_args[0]);
            if(vx_dyn_fn(c->p,t)) vx_fail(c->p,e->token,"use box_fn to allocate an erased callable");
            (void)vx_resolve_type(c->p,vx_box_type(c->p,t,e->token));
            check_expr(c,e->args->expr,t); e->kind=EX_BOX_NEW; e->operand_type=t;
            use(c,e->token,-1,EF_ALLOC|EF_BLOCK|vx_cleanup_effects(c->p,t));
            return vx_resolve_type(c->p,vx_apply_type(c->p,c->p->box_result,&t,1,e->token));
        }
        if(vx_word(c->p,e->token,"box_ref")||vx_word(c->p,e->token,"box_uniq")) {
            bool exclusive=vx_word(c->p,e->token,"box_uniq");
            if(e->type_arg_count||e->argc!=1) vx_fail(c->p,e->token,"box_ref expects one shared reference to a Box");
            const char *names[]={"owner"}; (void)map_arguments(c->p,e,1,NULL,names);
            Type t=check_expr(c,e->args->expr,TY_INFER);
            if(!vx_ref(t)||vx_referent(t)<TY_AGGREGATE||c->p->types[vx_referent(t)]->kind!=TD_BOX)
                vx_fail(c->p,e->token,"box_ref expects ref Box[T]");
            if(exclusive!=vx_uniq(t)) vx_fail(c->p,e->token,exclusive?"box_uniq expects uniq Box[T]":"box_ref expects shared ref Box[T]; explicitly reborrow the exclusive handle");
            if(vx_box_fn(c->p,vx_referent(t))) vx_fail(c->p,e->token,"borrow the boxed callable handle with ref or uniq mut");
            e->kind=EX_BOX_REF;
            return vx_retarget(t,c->p->types[vx_referent(t)]->element);
        }
        if(e->type_arg_count && function_id(c->p,e->token)<0) vx_fail(c->p,e->token,"type arguments require a declared generic function");
        if(lookup(c,e->token)||global_id(c->p,e->token)>=0) vx_fail(c->p,e->token,"this name refers to a value, which is not callable");
        int io=vx_io_builtin(c->p,e->token),lib=vx_library_builtin(c->p,e->token);
        if(io>=0) actual=check_io(c,e,io);
        else if(lib>=0) actual=check_library(c,e,lib);
        else if(vx_word(c->p,e->token,"output")) {
            if(e->argc!=1) vx_fail(c->p,e->token,"output expects one scalar argument");
            const char *names[]={"value"}; (void)map_arguments(c->p,e,1,NULL,names);
            Type h=hint(c,e->args->expr,0);
            if(!h && e->args->expr->kind==EX_CALL && vx_word(c->p,e->args->expr->token,"panic")) h=TY_DEFAULT_UINT;
            Type t=check_expr(c,e->args->expr,h);
            if(t!=TY_BOOL&&!vx_integer(t)&&!vx_float(t)) vx_fail(c->p,e->token,"output requires a scalar");
            e->kind=EX_OUTPUT; use(c,e->token,-1,vx_builtin_effects(EX_OUTPUT));
        } else if(vx_word(c->p,e->token,"panic")) {
            if(e->argc) vx_fail(c->p,e->token,"panic expects no arguments");
            e->kind=EX_PANIC; actual=expected==TY_INFER?TY_VOID:expected;
            use(c,e->token,-1,vx_builtin_effects(EX_PANIC));
        } else if(vx_word(c->p,e->token,"mem_replace")||vx_word(c->p,e->token,"mem_swap")) {
            bool swap=vx_word(c->p,e->token,"mem_swap");
            if(e->type_arg_count||e->argc!=2) vx_fail(c->p,e->token,"memory operation expects two arguments and no type arguments");
            const char *names[]={swap?"left":"target",swap?"right":"value"};
            (void)map_arguments(c->p,e,2,NULL,names);
            Type target=check_expr(c,vx_argument(e,0)->expr,TY_INFER);
            if(!vx_writable(target)) vx_fail(c->p,e->token,"memory operation requires uniq mut T");
            Type element=vx_referent(target);
            if(element==TY_VOID||(vx_dependent(c->p,element)&&!c->f->lazy_helper)||vx_dyn_fn(c->p,element))
                vx_fail(c->p,e->token,"memory operation requires owned non-void values; borrowed dependencies cannot be exchanged");
            check_expr(c,vx_argument(e,1)->expr,swap?target:element);
            e->kind=EX_MEMORY; e->op=swap; e->operand_type=element;
            use(c,e->token,-1,EF_WRITE);
            if(!swap&&vx_owned(c->p,element)) {
                use(c,e->token,-1,vx_cleanup_effects(c->p,element)); c->f->last_use->cleanup=true;
            }
            actual=swap?TY_VOID:element;
        } else if(vx_word(c->p,e->token,"uniq_split")) {
            if(e->argc!=2) vx_fail(c->p,e->token,"uniq_split expects data and index");
            const char *names[]={"data","index"}; (void)map_arguments(c->p,e,2,NULL,names);
            Type source=TY_INFER;
            for(Arg *a=e->args;a;a=a->next) {
                Type t=check_expr(c,a->expr,a->field?TY_WORD:TY_INFER);
                if(!a->field) source=t;
            }
            if(!vx_uniq_slice(c->p,source)) vx_fail(c->p,e->token,"uniq_split requires UniqSlice; explicitly borrow an array with uniq_slice");
            Type element=c->p->types[source]->element;
            e->kind=EX_SLICE_SPLIT; e->operand_type=element;
            actual=vx_resolve_type(c->p,vx_apply_type(c->p,c->p->split_result,&element,1,e->token));
        } else if(vx_word(c->p,e->token,"slice")||vx_word(c->p,e->token,"uniq_slice")) {
            if(e->argc!=1 && e->argc!=3) vx_fail(c->p,e->token,"slice expects data, or data/start/count");
            const char *names[]={"data","start","count"}; (void)map_arguments(c->p,e,e->argc,NULL,names);
            Type source=TY_INFER; Expr *start=NULL,*count=NULL;
            for(Arg *a=e->args;a;a=a->next) {
                Type t=check_expr(c,a->expr,TY_INFER);
                if(!a->field) source=t;
                else {
                    if(!vx_integer(t)) vx_fail(c->p,a->expr->token,"slice start/count must be integers");
                    if(a->field==1) start=a->expr; else count=a->expr;
                }
            }
            bool borrowed=vx_ref(source); Type base=borrowed?vx_referent(source):source;
            bool exclusive=vx_word(c->p,e->token,"uniq_slice");
            if(exclusive) {
                if(!(borrowed&&vx_writable(source)&&base>=TY_AGGREGATE&&c->p->types[base]->kind==TD_ARRAY)&&!vx_uniq_slice(c->p,source))
                    vx_fail(c->p,e->token,"uniq_slice requires uniq mut array or an existing UniqSlice");
            } else {
                if(vx_exclusive(c->p,source)) vx_fail(c->p,e->token,"slice requires a shared ref reborrow, not an exclusive handle");
                if(!(borrowed&&base>=TY_AGGREGATE&&c->p->types[base]->kind==TD_ARRAY) && !vx_slice(c->p,base))
                    vx_fail(c->p,e->token,"slice requires a shared reference to a fixed array or an existing Slice");
            }
            Type element=c->p->types[base]->element;
            actual=vx_resolve_type(c->p,exclusive?vx_uniq_slice_type(c->p,element,e->token):vx_slice_type(c->p,element,e->token));
            e->kind=EX_SLICE; e->operand_type=source;
            if(start) {
                Type st,ct; uint64_t s,n;
                bool sc=vx_constant(start,&st,&s),nc=vx_constant(count,&ct,&n);
                if((sc&&vx_signed_type(st)&&vx_signed_value(s,vx_width(st))<0)||(nc&&vx_signed_type(ct)&&vx_signed_value(n,vx_width(ct))<0))
                    vx_fail(c->p,e->token,"constant slice range is out of bounds");
                if(borrowed&&sc&&nc) {
                    uint64_t length=(uint64_t)c->p->types[base]->count;
                    if(s>length||n>length-s) vx_fail(c->p,e->token,"constant slice range is out of bounds");
                } else use(c,e->token,-1,EF_ABORT);
            }
        } else if(vx_word(c->p,e->token,"len")) {
            if(e->argc!=1) vx_fail(c->p,e->token,"len expects one fixed array or Slice");
            const char *names[]={"array"}; (void)map_arguments(c->p,e,1,NULL,names);
            Type t=check_expr(c,e->args->expr,TY_INFER);
            if(vx_slice(c->p,t)) e->kind=EX_SLICE_LENGTH;
            else if(vx_vec(c->p,t)) {
                if(!stable_place(c->p,e->args->expr)) vx_fail(c->p,e->token,"len of Vec requires a stable place");
                Expr *borrow=vx_alloc(c->p,sizeof *borrow); borrow->kind=EX_REF; borrow->token=e->args->expr->token;
                borrow->left=e->args->expr; borrow->type=vx_reference_type(c->p,t,e->token); e->args->expr=borrow;
                e->kind=EX_VEC; e->op=VX_VEC_LEN; e->operand_type=c->p->types[t]->element;
            } else {
                if(t<TY_AGGREGATE||c->p->types[t]->kind!=TD_ARRAY) vx_fail(c->p,e->token,"len requires a fixed array or Slice");
                e->kind=EX_LENGTH; e->value=(uint64_t)c->p->types[t]->count;
            }
            actual=TY_WORD;
        } else {
            int id=function_id(c->p,e->token);
            if(id<0) vx_fail(c->p,e->token,"unknown function");
            Function *f=c->p->functions[id]; e->id=id;
            for(int i=0;i<e->type_arg_count;++i) if(!vx_kind_arity(c->p,e->type_args[i])) e->type_args[i]=vx_resolve_type(c->p,e->type_args[i]);
            bool infer=f->generic_count&&!e->type_arg_count;
            if(!infer) vx_check_type_arguments(c->p,f,e->type_args,e->type_arg_count,e->token);
            if(f->generic_count) {
                if(!c->p->generics_ready) vx_fail(c->p,e->token,"generic staging requires completed constraint/implementation signatures; cyclic staged layout dependency");
                e->kind=EX_GENERIC_CALL;
            }
            uint32_t seen=map_arguments(c->p,e,f->param_count,f->params,NULL);
            if(f->comptime) ++c->static_context;
            Arg **tail=&e->args;
            for(Arg *a=e->args;a;a=a->next) {
                Type t=vx_resolve_type(c->p,f->params[a->field].type);
                Type expected_type=infer?vx_argument_hint(c->p,f,t,e->type_args,e->token):
                    vx_substitute_type(c->p,t,f->generic_types,e->type_args,f->generic_count,e->token);
                Type argument=check_expr(c,a->expr,expected_type);
                if(infer) vx_infer_type_arguments(c->p,f,t,argument,e->type_args,a->expr->token);
                tail=&a->next;
            }
            if(infer) {
                for(int i=0;i<f->generic_count;++i) if(!e->type_args[i])
                    vx_fail(c->p,e->token,"cannot infer all type arguments from explicit arguments; supply explicit type arguments");
                e->type_arg_count=f->generic_count;
                vx_check_type_arguments(c->p,f,e->type_args,e->type_arg_count,e->token);
            }
            for(int i=0;i<f->param_count;++i) if(!(seen&(UINT32_C(1)<<i))) {
                if(!f->params[i].default_value) {
                    char name[81]; vx_name(c->p,f->params[i].name,name,sizeof name);
                    vx_fail(c->p,e->token,"missing required argument '%s' (expected %d arguments, got %d)",name,f->param_count,e->argc);
                }
                check_default(c->p,f,i); Arg *a=vx_alloc(c->p,sizeof *a);
                a->field=i; a->expr=f->params[i].default_value; *tail=a; tail=&a->next; ++e->argc;
            }
            actual=vx_substitute_type(c->p,vx_resolve_type(c->p,f->result),f->generic_types,e->type_args,f->generic_count,e->token);
            use(c,e->token,id,0);
            if(e->kind==EX_GENERIC_CALL && (c->static_context||c->f->comptime)) {
                bool abstract=false;
                for(int i=0;i<e->type_arg_count;++i) abstract|=vx_abstract(c->p,e->type_args[i]);
                if(abstract&&!(c->f->comptime&&c->f->generic_count))
                    vx_fail(c->p,e->token,"comptime generic calls require concrete type arguments outside a comptime template");
                if(!abstract&&!(c->f->comptime&&c->f->generic_count)) vx_specialize_call(c->p,e);
            }
            if(f->comptime) {
                --c->static_context;
                if(!c->f->comptime && !c->static_context) { e->type=actual; vx_stage_value(c->p,c->f,e); }
            }
        }
    return actual;
}
static Type check_method(Checker *c,Expr *e) {
    Function *f=vx_method_contract(c->p,e); Constraint *bound=&c->p->constraints[e->op];
    if((c->static_context||c->f->comptime)&&vx_abstract(c->p,e->operand_type)&&!(c->f->comptime&&c->f->generic_count))
        vx_fail(c->p,e->token,"comptime constraint calls require a concrete implementing type");
    if(e->argc!=f->param_count) vx_fail(c->p,e->token,"constraint operation expects %d arguments, got %d",f->param_count,e->argc);
    (void)map_arguments(c->p,e,f->param_count,f->params,NULL);
    for(Arg *a=e->args;a;a=a->next) {
        Type t=vx_substitute_type(c->p,f->params[a->field].type,&bound->self,&e->operand_type,1,e->token);
        check_expr(c,a->expr,t);
    }
    Type result=vx_substitute_type(c->p,f->result,&bound->self,&e->operand_type,1,e->token);
    /* Use the declared contract, even for a stronger witness. */
    use(c,e->token,-1,EF_ALL&~vx_forbidden(f->caps));
    if(!vx_abstract(c->p,e->operand_type)) {
        e->id=vx_method_implementation(c->p,e->op,e->operand_type,e->id,e->token);
        e->kind=EX_CALL; e->type_arg_count=0;
    }
    return result;
}
static Type check_overload(Checker *c,Expr *e,Type t) {
    int method=0,constraint=0; Function *contract=vx_operator_contract(c->p,t,e->op,e->token,&method,&constraint);
    Type result;
    if(contract) {
        result=contract->result==TY_BOOL?TY_BOOL:t; e->kind=EX_OPERATOR; e->id=method; e->value=(uint64_t)constraint;
        use(c,e->token,-1,EF_ALL&~vx_forbidden(contract->caps));
    } else {
        e->id=vx_operator_implementation(c->p,t,e->op,-1,e->token); e->kind=EX_CALL;
        result=c->p->functions[e->id]->result; use(c,e->token,e->id,0);
    }
    check_expr(c,e->left,t); if(e->right) check_expr(c,e->right,t);
    Arg *a=vx_alloc(c->p,sizeof *a); a->expr=e->left; e->args=a; e->argc=1;
    if(e->right) { a->next=vx_alloc(c->p,sizeof *a); a->next->expr=e->right; a->next->field=1; ++e->argc; }
    e->left=e->right=NULL; e->operand_type=t; return result;
}
static Type check_function_value(Checker *c,Expr *e,Type expected) {
    Type actual;
        Function *f=c->p->functions[e->id];
        if((f->generic_count&&!f->anonymous)||f->comptime) vx_fail(c->p,e->token,"function values require a concrete runtime function; use a wrapper");
        f->result=vx_resolve_type(c->p,f->result);
        for(int i=0;i<f->param_count;++i) {
            f->params[i].type=vx_resolve_type(c->p,f->params[i].type);
            vx_borrow_parameter(c->p,f,&f->params[i]);
        }
        if(vx_callable(c->p,expected)) {
            const Function *contract=c->p->types[expected]->signature;
            if(!vx_callable_shape(c->p,f,contract)) vx_fail(c->p,e->token,"function value does not match callable parameters, names or result");
            if(f->anonymous&&!f->caps) f->caps=contract->caps;
            actual=expected;
        } else actual=TY_INFER;
        if(f->anonymous) {
            f->capture_context=c; vx_check_function(c->p,f); f->capture_context=NULL;
            if(f->capture_count) {
                if(vx_callable(c->p,expected)) vx_fail(c->p,e->token,"captured closures require their concrete inferred type; callable erasure must be explicit");
                (void)vx_closure_type(c->p,e->id,e->token);
                actual=f->closure_type; e->kind=EX_CLOSURE;
                Arg **tail=&e->args;
                for(int i=0;i<f->capture_count;++i) {
                    const Capture *capture=&f->captures[i]; Arg *a=vx_alloc(c->p,sizeof *a);
                    a->field=i; a->expr=vx_alloc(c->p,sizeof *a->expr);
                    a->expr->kind=EX_VAR; a->expr->type=capture->type; a->expr->id=capture->source; a->expr->token=e->token;
                    *tail=a; tail=&a->next;
                }
                use(c,e->token,-1,vx_cleanup_effects(c->p,actual)); c->f->last_use->cleanup=true;
            }
        }
        if(!actual) actual=vx_function_item_type(c->p,e->id,e->token);
    return actual;
}
static bool has_holes(const Expr *e) {
    for(const Arg *a=e->args;a;a=a->next) if(a->expr->kind==EX_HOLE) return true;
    return false;
}
static Token partial_name(VxnProgram *p,Token at,int index) {
    char text[40]; snprintf(text,sizeof text,"$partial_%d",index);
    char *stored=vx_alloc(p,strlen(text)+1); strcpy(stored,text);
    return vx_synthetic(at,stored);
}
static Expr *partial_var(VxnProgram *p,Token name) {
    Expr *e=vx_alloc(p,sizeof *e); e->kind=EX_VAR; e->token=name; return e;
}
static Type check_partial(Checker *c,Expr *e,Type expected) {
    VxnProgram *p=c->p; Function *target=NULL; Expr *callee=e->left;
    bool direct=e->kind==EX_CALL&&!lookup(c,e->token),method=e->kind==EX_METHOD;
    if(method) vx_fail(p,e->token,"partial constraint calls require a named wrapper");
    if(direct) {
        int id=function_id(p,e->token);
        if(id<0) vx_fail(p,e->token,"partial application requires a named function or callable value");
        target=p->functions[id];
        if(target->comptime) vx_fail(p,e->token,"partial application requires a runtime function");
    } else {
        if(!callee) callee=partial_var(p,e->token);
        Type t=check_expr(c,callee,TY_INFER),base=vx_ref(t)?vx_referent(t):t;
        if(!vx_invocable(p,base)) vx_fail(p,e->token,"partial application requires a callable");
        target=p->types[base]->signature;
        if(e->type_arg_count) vx_fail(p,e->token,"callable values do not accept type arguments");
    }
    vx_borrow_signature(p,target);
    bool infer=direct&&target->generic_count&&!e->type_arg_count;
    for(int i=0;i<e->type_arg_count;++i) if(!vx_kind_arity(p,e->type_args[i])) e->type_args[i]=vx_resolve_type(p,e->type_args[i]);
    if(direct&&!infer) vx_check_type_arguments(p,target,e->type_args,e->type_arg_count,e->token);
    uint32_t seen=map_arguments(p,e,target->param_count,target->params,NULL);
    Function *context=vx_alloc(p,sizeof *context); Checker *captures=vx_alloc(p,sizeof *captures);
    captures->p=p; captures->f=context;
    Expr *values[VX_MAX_FIELDS]; int value_count=0;
    Token callee_name={0};
    if(callee) {
        callee_name=partial_name(p,e->token,value_count);
        bind(captures,callee_name,callee->type,true,true); values[value_count++]=callee;
    }
    int hole_count=0;
    const Function *hinted=vx_callable(p,expected)?p->types[expected]->signature:NULL;
    for(Arg *a=e->args;a;a=a->next) {
        Type pattern=vx_resolve_type(p,target->params[a->field].type);
        if(a->expr->kind==EX_HOLE) {
            if(infer&&hinted&&hole_count<hinted->param_count)
                vx_infer_type_arguments(p,target,pattern,hinted->params[hole_count].type,e->type_args,a->expr->token);
            ++hole_count; continue;
        }
        Type hint=infer?vx_argument_hint(p,target,pattern,e->type_args,e->token):
            vx_substitute_type(p,pattern,target->generic_types,e->type_args,direct?target->generic_count:0,e->token);
        Type actual=check_expr(c,a->expr,hint);
        if(infer) vx_infer_type_arguments(p,target,pattern,actual,e->type_args,a->expr->token);
    }
    if(infer) {
        for(int i=0;i<target->generic_count;++i) if(!e->type_args[i])
            vx_fail(p,e->token,"cannot infer partial application types from bound arguments; supply type arguments or a callable context");
        e->type_arg_count=target->generic_count;
        vx_check_type_arguments(p,target,e->type_args,e->type_arg_count,e->token);
    }
    if(p->function_count==VX_MAX_FUNCTIONS) vx_fail(p,e->token,"partial application exceeds function budget");
    Function *f=vx_alloc(p,sizeof *f); f->name=f->cap_token=e->token; f->anonymous=true; f->operator_code=-1;
    f->end=c->f->end; f->generic_count=c->f->generic_count;
    memcpy(f->generic_types,c->f->generic_types,(size_t)f->generic_count*sizeof(Type));
    f->result=vx_substitute_type(p,vx_resolve_type(p,target->result),target->generic_types,e->type_args,direct?target->generic_count:0,e->token);
    Stmt *ret=f->body=vx_alloc(p,sizeof *ret); ret->kind=ST_RETURN; ret->token=e->token; ret->end=ret->scope_end=f->end;
    Expr *call=ret->expr=vx_alloc(p,sizeof *call); call->kind=direct?EX_CALL:EX_INVOKE; call->token=e->token;
    if(callee) call->left=partial_var(p,callee_name);
    if(direct) { call->type_arg_count=e->type_arg_count; memcpy(call->type_args,e->type_args,sizeof call->type_args); }
    Arg **tail=&call->args;
    for(Arg *a=e->args;a;a=a->next) {
        int i=a->field; Arg *out=vx_alloc(p,sizeof *out); out->name=target->params[i].name;
        if(a->expr->kind==EX_HOLE) {
            Param *param=&f->params[f->param_count++]; *param=target->params[i]; param->default_value=NULL;
            param->type=vx_substitute_type(p,vx_resolve_type(p,param->type),target->generic_types,e->type_args,direct?target->generic_count:0,e->token);
            if(!param->name.length) param->name=partial_name(p,e->token,VX_MAX_FIELDS+f->param_count);
            out->expr=partial_var(p,param->name);
            if(target->from_mask&(UINT32_C(1)<<i)) f->from_names[f->from_count++]=param->name;
        } else {
            if(value_count==VX_MAX_FIELDS) vx_fail(p,e->token,"partial application capture limit exceeded");
            Token name=partial_name(p,e->token,value_count);
            bind(captures,name,a->expr->type,false,true); values[value_count++]=a->expr;
            out->expr=partial_var(p,name);
        }
        *tail=out; tail=&out->next; ++call->argc;
    }
    for(int i=0;i<target->param_count;++i) if(!(seen&(UINT32_C(1)<<i))) {
        if(!direct||!target->params[i].default_value) vx_fail(p,e->token,"missing argument in partial application; use _ for an unbound parameter");
        check_default(p,target,i); Arg *a=vx_alloc(p,sizeof *a); a->name=target->params[i].name;
        a->expr=target->params[i].default_value; *tail=a; tail=&a->next; ++call->argc;
    }
    f->from_explicit=target->from_explicit||vx_dependent(p,f->result);
    int id=p->function_count++; p->functions[id]=f;
    Token at=e->token; memset(e,0,sizeof *e); e->kind=EX_FUNCTION; e->id=id; e->token=at;
    Type actual=check_function_value(captures,e,expected);
    /* Capture expressions retain call-site order, independent of body lookup order. */
    if(e->kind==EX_CLOSURE) {
        Arg *ordered=NULL,**next=&ordered;
        for(int i=0;i<value_count;++i) for(Arg *a=e->args;a;a=a->next)
            if(f->captures[a->field].source==i) {
                Arg *out=vx_alloc(p,sizeof *out); *out=*a; out->expr=values[i]; out->next=NULL;
                *next=out; next=&out->next; break;
            }
        e->args=ordered; e->argc=f->capture_count;
        use(c,at,-1,vx_cleanup_effects(p,actual)); c->f->last_use->cleanup=true;
    }
    return actual;
}
static Type check_recursive_force(Checker *c,Expr *e,Type expected) {
    if(e->type_arg_count||e->argc!=1||e->args->expr->kind!=EX_VAR)
        vx_fail(c->p,e->token,"try_force expects one recursive binding name");
    const char *names[]={"value"}; (void)map_arguments(c->p,e,1,NULL,names);
    Token name=e->args->expr->token; RecursiveGroup *group=c->f->recursive_group; int index=-1;
    bool local=false;
    for(int i=c->count-1;i>=0;--i) if(vx_same(c->p,name,c->bindings[i].name)) {
        group=c->bindings[i].recursive_group; index=c->bindings[i].recursive_index; local=true; break;
    }
    if(!local&&group) for(int i=0;i<group->count;++i) if(vx_same(c->p,name,group->names[i])) index=i;
    if(!group||index<0||name.qualifier) vx_fail(c->p,name,"try_force requires a binding in the current recursive group or local scope");
    Function *helper=c->p->functions[group->helpers[index]];
    e->token=helper->name; e->args->name=(Token){0};
    Expr *loan=vx_alloc(c->p,sizeof *loan); loan->kind=EX_REF; loan->token=name; loan->op=1;
    loan->left=partial_var(c->p,group->storage);
    if(c->f->recursive_group==group) {
        Expr *deref=vx_alloc(c->p,sizeof *deref); deref->kind=EX_DEREF; deref->token=name;
        deref->left=loan->left; loan->left=deref;
    }
    e->args->expr=loan; e->type_arg_count=helper->generic_count;
    memcpy(e->type_args,helper->generic_types,(size_t)helper->generic_count*sizeof(Type));
    return check_call(c,e,expected);
}
static Expr *recursive_choice(VxnProgram *p,Token at,Type type,int variant,Expr *value) {
    Expr *e=vx_alloc(p,sizeof *e); e->kind=EX_CHOICE; e->token=at; e->type=type;
    e->id=variant; e->member=p->types[type]->variants[variant].name;
    if(value) { e->argc=1; e->args=vx_alloc(p,sizeof *e->args); e->args->expr=value; }
    return e;
}
static Stmt *recursive_stmt(VxnProgram *p,Token at,size_t end,StmtKind kind) {
    Stmt *s=vx_alloc(p,sizeof *s); s->kind=kind; s->token=at; s->end=s->scope_end=end; return s;
}
static Expr *recursive_field(VxnProgram *p,RecursiveGroup *group,int index) {
    Expr *e=vx_alloc(p,sizeof *e); e->kind=EX_FIELD; e->token=e->member=group->names[index];
    e->left=partial_var(p,group->storage); return e;
}
static void check_recursive_group(Checker *c,Stmt *s) {
    VxnProgram *p=c->p;
    if(c->static_context||c->f->comptime) vx_fail(p,s->token,"recursive lazy groups require runtime storage");
    RecursiveGroup *group=vx_alloc(p,sizeof *group); Stmt *bindings[VX_MAX_FIELDS];
    for(Stmt *b=s->body;b;b=b->next) {
        if(b->kind!=ST_BIND||b->comptime||b->mutable_binding||!b->expr||!b->type)
            vx_fail(p,b->token,"rec groups contain initialized const lazy/eager typed bindings");
        if(group->count==VX_MAX_FIELDS) vx_fail(p,b->name,"recursive group binding limit exceeded (32)");
        for(int i=0;i<group->count;++i) if(vx_same(p,b->name,group->names[i])) vx_fail(p,b->name,"duplicate recursive binding");
        Type value=vx_resolve_type(p,b->type);
        if(value==TY_VOID||vx_dependent(p,value)||vx_owned(p,value)||vx_dyn_fn(p,value))
            vx_fail(p,b->name,"recursive lazy groups currently require owned Copy results");
        int i=group->count++; group->names[i]=b->name; group->values[i]=value; bindings[i]=b;
    }
    if(!group->count) vx_fail(p,s->token,"recursive lazy group cannot be empty");
    if(p->type_count==VX_MAX_TYPES) vx_fail(p,s->token,"recursive group exceeds type budget");
    Type base=p->type_count++; TypeDef *d=p->types[base]=vx_alloc(p,sizeof *d);
    d->kind=TD_STRUCT; d->name=partial_name(p,s->token,base+VX_MAX_FIELDS*2); d->count=group->count;
    d->generic_count=c->f->generic_count; memcpy(d->generic_types,c->f->generic_types,sizeof d->generic_types);
    for(int i=0;i<group->count;++i) {
        Type cell=vx_resolve_type(p,vx_apply_type(p,p->recursive_cell,&group->values[i],1,group->names[i]));
        d->fields[i]=(Field){.name=group->names[i],.type=cell};
    }
    if(d->generic_count) (void)layout(p,base,0);
    group->type=vx_resolve_type(p,d->generic_count?vx_apply_type(p,base,d->generic_types,d->generic_count,s->token):base);
    group->storage=d->name;
    Expr *initial=vx_alloc(p,sizeof *initial); initial->kind=EX_RECORD; initial->token=s->token; initial->type=group->type;
    Arg **tail=&initial->args;
    for(int i=0;i<group->count;++i) {
        Stmt *b=bindings[i]; Function *f; Expr *body=NULL;
        if(b->lazy) { group->helpers[i]=b->expr->id; f=p->functions[b->expr->id]; body=f->body->expr; }
        else {
            if(p->function_count==VX_MAX_FUNCTIONS) vx_fail(p,b->name,"recursive helper function limit exceeded");
            group->helpers[i]=p->function_count++; f=p->functions[group->helpers[i]]=vx_alloc(p,sizeof *f);
        }
        f->name=f->cap_token=partial_name(p,b->name,group->helpers[i]+VX_MAX_TYPES);
        f->anonymous=false; f->operator_code=-1; f->recursive_group=group; f->end=s->end;
        f->generic_count=c->f->generic_count; memcpy(f->generic_types,c->f->generic_types,sizeof f->generic_types);
        f->result=vx_resolve_type(p,vx_apply_type(p,p->lazy_result,&group->values[i],1,b->name));
        f->param_count=1; f->params[0]=(Param){.name=group->storage,.type=vx_exclusive_type(p,group->type,true,b->name)};
        Type cell=p->types[group->type]->fields[i].type;
        Stmt *match=f->body=recursive_stmt(p,b->name,s->end,ST_MATCH); match->expr=recursive_field(p,group,i);
        Arm **arm=&match->arms;
        for(int tag=0;tag<3;++tag) {
            Arm *a=vx_alloc(p,sizeof *a); a->token=b->name; a->end=s->end; *arm=a; arm=&a->next;
            Pattern *pattern=a->pattern=vx_alloc(p,sizeof *pattern); pattern->kind=PAT_CONSTRUCTOR;
            pattern->token=b->name; pattern->type=cell; pattern->constructor=p->types[cell]->variants[tag].name;
            Stmt *ret=recursive_stmt(p,b->name,s->end,ST_RETURN);
            if(tag==2) {
                a->count=1; a->bindings[0].name=partial_name(p,b->name,-1);
                pattern->count=1; pattern->children=vx_alloc(p,sizeof *pattern);
                pattern->children->kind=PAT_BIND; pattern->children->token=a->bindings[0].name;
                ret->expr=recursive_choice(p,b->name,f->result,0,partial_var(p,a->bindings[0].name)); a->body=ret;
            } else if(tag==1||!b->lazy) {
                ret->expr=recursive_choice(p,b->name,f->result,1,recursive_choice(p,b->name,p->lazy_error,0,NULL)); a->body=ret;
            } else {
                Stmt *busy=a->body=recursive_stmt(p,b->name,s->end,ST_ASSIGN); busy->op=-1;
                busy->target=recursive_field(p,group,i); busy->expr=recursive_choice(p,b->name,cell,1,NULL);
                Stmt *value=busy->next=recursive_stmt(p,b->name,s->end,ST_BIND); value->name=partial_name(p,b->name,-2);
                value->type=group->values[i]; value->expr=body;
                Stmt *ready=value->next=recursive_stmt(p,b->name,s->end,ST_ASSIGN); ready->op=-1;
                ready->target=recursive_field(p,group,i); ready->expr=recursive_choice(p,b->name,cell,2,partial_var(p,value->name));
                ready->next=ret; ret->expr=recursive_choice(p,b->name,f->result,0,partial_var(p,value->name));
            }
        }
        if(!b->lazy) check_expr(c,b->expr,group->values[i]);
        Arg *arg=vx_alloc(p,sizeof *arg); arg->field=i; arg->name=b->name;
        arg->expr=recursive_choice(p,b->name,cell,b->lazy?0:2,b->lazy?NULL:b->expr);
        *tail=arg; tail=&arg->next; ++initial->argc;
    }
    s->kind=ST_BIND; s->recursive=false; s->body=NULL; s->name=group->storage;
    s->type=group->type; s->mutable_binding=true; s->expr=initial;
    s->local=bind(c,s->name,s->type,true,true);
    for(int i=0;i<group->count;++i) {
        bind(c,group->names[i],TY_WORD,false,true);
        c->bindings[c->count-1].recursive_group=group; c->bindings[c->count-1].recursive_index=i;
    }
    for(int i=0;i<group->count;++i) vx_check_function(p,p->functions[group->helpers[i]]);
}
static Type coerce_callable(Checker *c,Expr *e,Type actual,Type expected) {
    if(expected!=actual&&vx_callable(c->p,expected)&&(vx_callable(c->p,actual)||vx_function_item(c->p,actual))) {
        const Function *source=c->p->types[actual]->signature,*target=c->p->types[expected]->signature;
        if(e->kind==EX_FUNCTION) {
            if(!vx_callable_shape(c->p,c->p->functions[e->id],target))
                vx_fail(c->p,e->token,"function value does not match callable parameters, names or result");
            actual=e->type=expected;
        } else if(vx_function_item(c->p,actual)) {
            if(!vx_callable_shape(c->p,source,target)) vx_fail(c->p,e->token,"function item does not match callable parameters, names or result");
            Expr *value=vx_alloc(c->p,sizeof *value); *value=*e; value->type=actual;
            memset(e,0,sizeof *e); e->kind=EX_FN_COERCE; e->token=value->token;
            e->left=value; e->type=expected; actual=expected;
        } else {
            if(!vx_callable_shape(c->p,source,target)||
               source->call_mode>target->call_mode||
               ((EF_ALL&~vx_forbidden(source->caps))&vx_forbidden(target->caps))||
               ((target->caps&CP_HOT)&&!(source->caps&CP_HOT))||
               (source->from_mask&~target->from_mask))
                vx_fail(c->p,e->token,"callable conversion would strengthen an unproved contract");
            Expr *value=vx_alloc(c->p,sizeof *value); *value=*e;
            memset(e,0,sizeof *e); e->kind=EX_FN_COERCE; e->token=value->token;
            e->left=value; e->type=expected; actual=expected;
        }
    }
    return actual;
}
static Type check_borrow(Checker *c,Expr *e) {
    Type actual;
        Type t=check_expr(c,e->left,TY_INFER);
        if(!stable_place(c->p,e->left)) vx_fail(c->p,e->token,"cannot borrow a temporary; ref requires a stable place");
        if(vx_split_result(c->p,t)) vx_fail(c->p,e->token,"consume SplitResult with match instead of borrowing its descriptor");
        if(vx_fn_view(c->p,t)) {
            if(e->op==2) vx_fail(c->p,e->token,"use ref view for a shared callable reborrow");
            if(e->op&&!vx_uniq_fn_view(c->p,t)) vx_fail(c->p,e->token,"cannot create an exclusive callable reborrow from FnRef");
            actual=vx_resolve_type(c->p,vx_fn_view_type(c->p,c->p->types[t]->element,e->op!=0,e->token));
            e->kind=EX_VIEW_BORROW;
        } else if(vx_str(c->p,t)) {
            if(e->op) vx_fail(c->p,e->token,"str is a shared text view; exclusive reborrowing is unsupported");
            actual=t; e->kind=EX_SLICE_BORROW;
        } else if(vx_slice(c->p,t)) {
            if(e->op==2) vx_fail(c->p,e->token,"use ref view for a read-only slice reborrow");
            if(e->op&&!vx_uniq_slice(c->p,t)) vx_fail(c->p,e->token,"cannot create an exclusive reborrow from a shared Slice");
            actual=vx_resolve_type(c->p,e->op?vx_uniq_slice_type(c->p,c->p->types[t]->element,e->token):vx_slice_type(c->p,c->p->types[t]->element,e->token));
            e->kind=EX_SLICE_BORROW;
        } else {
            actual=e->op?vx_exclusive_type(c->p,t,e->op==1,e->token):vx_reference_type(c->p,t,e->token);
            if(e->op) (void)exclusive_place(c,e->left,e->op==1);
        }
        return actual;
    }
static Type check_real(Checker *c,Expr *e,Type expected) {
    Type actual=vx_float(expected)?expected:TY_F64;
    uint64_t bytes[128],result[3]; size_t length=0;
    for(size_t i=0;i<e->token.length;++i) {
        unsigned char ch=(unsigned char)c->p->source[e->token.start+i];
        if(ch!='_') bytes[length++]=ch;
    }
    vxn_ext_parse(bytes,length,vx_width(actual),result);
    if(result[0]) vx_fail(c->p,e->token,"floating literal must fit %s and be well formed",vx_type(actual));
    e->value=result[1];
    if(vx_extended(actual)) { e->cells=vx_alloc(c->p,2*sizeof(uint64_t)); memcpy(e->cells,result+1,2*sizeof(uint64_t)); }
    return actual;
}
static Type check_binary(Checker *c,Expr *e,Type expected) {
    Type actual=TY_VOID;
    bool logical=e->op==VX_LAND||e->op==VX_LOR, comparison=e->op>=VX_EQ&&e->op<=VX_GE;
    Type t=logical?TY_BOOL:numeric_hint(c,e,comparison?TY_INFER:expected);
    if(!logical && t>=TY_AGGREGATE) { actual=check_overload(c,e,vx_resolve_type(c->p,t)); return actual; }
    if(t==TY_BOOL && !(logical||e->op==VX_EQ||e->op==VX_NE)) vx_fail(c->p,e->token,"operator requires numeric operands");
    if(t!=TY_BOOL&&!vx_integer(t)&&!vx_float(t)) vx_fail(c->p,e->token,"operator requires scalar operands");
    if(vx_float(t)&&(e->op==VX_MOD||(e->op>=VX_AND&&e->op<=VX_SHR))) vx_fail(c->p,e->token,"operator requires integer operands");
    check_expr(c,e->left,t);
    if(e->op==VX_SHL||e->op==VX_SHR) {
        Type right=check_expr(c,e->right,TY_INFER);
        if(!vx_integer(right)) vx_fail(c->p,e->right->token,"shift count must be an integer");
    } else check_expr(c,e->right,t);
    e->operand_type=t; actual=logical||comparison?TY_BOOL:t; return actual;
}
static Type check_cast(Checker *c,Expr *e) {
    Type actual=TY_VOID;
    actual=e->type; Type from=check_expr(c,e->left,TY_INFER); e->operand_type=from;
    if((!vx_integer(from)&&!vx_float(from))||(!vx_integer(actual)&&!vx_float(actual)))
        vx_fail(c->p,e->token,"cast and narrow currently require numeric types");
    bool safe=false;
    if(vx_integer(from)&&vx_integer(actual))
        safe=vx_signed_type(from)==vx_signed_type(actual)?vx_width(actual)>=vx_width(from):
            !vx_signed_type(from)&&vx_signed_type(actual)&&vx_width(actual)>vx_width(from);
    else if(vx_float(from)&&vx_float(actual)) safe=vx_width(actual)>=vx_width(from);
    else if(vx_integer(from)&&vx_float(actual))
        safe=vx_width(from)-(vx_signed_type(from)?1u:0u)<=(actual==TY_F32?24u:actual==TY_F64?53u:actual==TY_F80?64u:113u);
    if(!e->op&&!safe) vx_fail(c->p,e->token,"cast must preserve every source value; use narrow for a lossy conversion");
    return actual;
}
static Type check_record(Checker *c,Expr *e) {
    Type actual=TY_VOID;
    actual=e->type;
    if(actual<TY_AGGREGATE) vx_fail(c->p,e->token,"%s is opaque; obtain a handle through its library",vx_type(actual));
    TypeDef *d=c->p->types[actual];
    if(d->kind!=TD_STRUCT) vx_fail(c->p,e->token,"record initializer requires a struct type");
    bool seen[VX_MAX_FIELDS]={0};
    for(Arg *a=e->args;a;a=a->next) {
        int id=field_id(c->p,actual,a->name);
        if(seen[id]) vx_fail(c->p,a->name,"duplicate field initializer");
        seen[id]=true; a->field=id; check_expr(c,a->expr,d->fields[id].type);
    }
    for(int i=0;i<d->count;++i) if(!seen[i]) vx_fail(c->p,e->token,"record initializer must provide every field");
    return actual;
}
static Type check_index(Checker *c,Expr *e) {
    Type actual=TY_VOID;
    Type t=check_expr(c,e->left,TY_INFER);
    t=auto_deref(c,&e->left,t);
    if(vx_str(c->p,t)) vx_fail(c->p,e->token,"str has no integer indexing; use str_bytes(text) for bytes or utf8_next for scalars");
    if(t<TY_AGGREGATE||(c->p->types[t]->kind!=TD_ARRAY&&!vx_slice(c->p,t))) vx_fail(c->p,e->token,"indexing requires a fixed array or Slice");
    Type index=check_expr(c,e->right,TY_INFER);
    if(!vx_integer(index)) vx_fail(c->p,e->right->token,"array index must be an integer");
    Type ct; uint64_t value;
    if(vx_slice(c->p,t)) { use(c,e->token,-1,EF_ABORT); c->f->last_use->bounds_check=e; }
    else if(vx_constant(e->right,&ct,&value)) {
        if((vx_signed_type(ct)&&vx_signed_value(value,vx_width(ct))<0)||value>=(uint64_t)c->p->types[t]->count)
            vx_fail(c->p,e->right->token,"constant array index is out of bounds");
    } else { use(c,e->token,-1,EF_ABORT); c->f->last_use->bounds_check=e; }
    actual=c->p->types[t]->element; return actual;
}
static Type check_choice(Checker *c,Expr *e) {
    Type actual=TY_VOID;
    actual=e->type;
    if(vx_split_result(c->p,actual)) vx_fail(c->p,e->token,"SplitResult is constructed only by checked uniq_split");
    if(actual<TY_AGGREGATE) vx_fail(c->p,e->token,"%s is opaque; obtain a handle through its library",vx_type(actual));
    TypeDef *d=c->p->types[actual];
    if(d->recursive_cell&&!c->f->recursive_group) vx_fail(c->p,e->token,"recursive lazy state is sealed");
    if(d->lazy&&!c->f->lazy_helper) vx_fail(c->p,e->token,"Lazy state is sealed; initialize a lazy binding instead");
    if(d->kind!=TD_CHOICE) vx_fail(c->p,e->token,"qualified constructor requires a choice");
    int id=0; while(id<d->count&&!vx_same(c->p,e->member,d->variants[id].name)) ++id;
    if(id==d->count) vx_fail(c->p,e->member,"unknown choice constructor");
    Variant *v=&d->variants[id]; e->id=id;
    if(e->argc!=v->count) vx_fail(c->p,e->token,"constructor has wrong number of payload arguments");
    int i=0; for(Arg *a=e->args;a;a=a->next) check_expr(c,a->expr,v->fields[i++]);
    return actual;
}
static Type check_expr(Checker *c,Expr *e,Type expected) {
    if(c->guard&&(e->kind==EX_TRY||(e->kind==EX_REF&&e->op)))
        vx_fail(c->p,e->token,"match guards cannot propagate errors or take exclusive borrows");
    if(++c->depth>VX_MAX_DEPTH) vx_fail(c->p,e->token,"expression tree exceeds prototype depth limit");
    expected=vx_resolve_type(c->p,expected);
    e->type=vx_resolve_type(c->p,e->type);
    Type actual=TY_VOID;
    switch(e->kind) {
    case EX_FUNCTION:actual=check_function_value(c,e,expected);break;
    case EX_WORD: {
        actual=vx_integer(expected)?expected:TY_DEFAULT_UINT;
        uint64_t max=vx_mask(vx_width(actual));
        if(vx_signed_type(actual)) max>>=1;
        if(e->value>max) vx_fail(c->p,e->token,"integer literal must fit %s",vx_type(actual));
        break;
    }
    case EX_REAL:actual=check_real(c,e,expected);break;
    case EX_BOOL:actual=TY_BOOL;break;
    case EX_TRY: {
        if(!c->f->body) vx_fail(c->p,e->token,"? is only valid inside a result-returning function");
        Type input=check_expr(c,e->left,TY_INFER),output=vx_resolve_type(c->p,c->f->result);
        int ok=result_ok(c->p,input,e->token),out_ok=result_ok(c->p,output,e->token);
        Type error=c->p->types[input]->variants[1-ok].fields[0];
        Type wanted=c->p->types[output]->variants[1-out_ok].fields[0];
        if(error!=wanted) vx_fail(c->p,e->token,"? requires exactly matching error types; implicit error conversion is not supported");
        e->id=ok; e->op=1-out_ok; e->operand_type=output;
        actual=c->p->types[input]->variants[ok].fields[0]; break;
    }
    case EX_BYTES:case EX_TEXT:actual=e->type;break;
    case EX_REF:actual=check_borrow(c,e);break;
    case EX_DEREF: {
        Type t=check_expr(c,e->left,TY_INFER);
        if(!vx_ref(t)) vx_fail(c->p,e->token,"dereference requires a reference");
        actual=vx_referent(t); break;
    }
    case EX_VAR: {
        Binding *b=lookup(c,e->token);
        if(!b) {
            int id=global_id(c->p,e->token);
            if(id<0) {
                id=function_id(c->p,e->token);
                if(id<0) vx_fail(c->p,e->token,c->f->anonymous?"unknown value in anonymous function; captured environments are not implemented yet":"unknown value");
                e->kind=EX_FUNCTION; e->id=id; --c->depth; return check_expr(c,e,expected);
            }
            vx_check_global(c->p,id); Token at=e->token; *e=*c->p->globals[id].expr; e->token=at;
            actual=e->type; break;
        }
        if(b->recursive_group) vx_fail(c->p,e->token,"recursive lazy bindings are accessed through try_force(name)");
        if(!b->constant&&!c->initialized[b->id]&&vx_capture(c->f,b->id)<0) vx_fail(c->p,e->token,"value may be uninitialized");
        actual=b->type; e->id=b->id;
        if(b->target>=0) e->value=(uint64_t)b->target+1;
        if(b->constant) { Token at=e->token; *e=*b->constant; e->token=at; }
        break;
    }
    case EX_UNARY: {
        Type operand=hint(c,e->left,0);
        if(operand>=TY_AGGREGATE) { actual=check_overload(c,e,vx_resolve_type(c->p,operand)); break; }
        actual=e->op==VX_NOT?TY_BOOL:numeric_hint(c,e,expected);
        if(e->op!=VX_NOT && !vx_integer(actual)&&!vx_float(actual)) vx_fail(c->p,e->token,"numeric unary operator requires a number");
        if(e->op==VX_INV&&!vx_integer(actual)) vx_fail(c->p,e->token,"bitwise complement requires an integer");
        if(e->op==VX_NEG && e->left->kind==EX_WORD && vx_signed_type(actual)) {
            uint64_t limit=UINT64_C(1)<<(vx_width(actual)-1);
            if(e->left->value>limit) vx_fail(c->p,e->token,"negative integer literal is outside its type");
            e->value=(UINT64_C(0)-e->left->value)&vx_mask(vx_width(actual)); e->kind=EX_WORD; e->left=NULL;
        } else check_expr(c,e->left,actual);
        e->operand_type=actual; break;
    }
    case EX_BINARY:actual=check_binary(c,e,expected);break;
    case EX_CALL:actual=has_holes(e)?check_partial(c,e,expected):check_call(c,e,expected);break;
    case EX_INVOKE:actual=has_holes(e)?check_partial(c,e,expected):check_invoke(c,e);break;
    case EX_METHOD:actual=has_holes(e)?check_partial(c,e,expected):check_method(c,e);break;
    case EX_CAST:actual=check_cast(c,e);break;
    case EX_RECORD:actual=check_record(c,e);break;
    case EX_ARRAY: {
        Type element=TY_INFER;
        if(expected!=TY_INFER) {
            if(expected<TY_AGGREGATE||c->p->types[expected]->kind!=TD_ARRAY) vx_fail(c->p,e->token,"array initializer requires an array type");
            element=c->p->types[expected]->element;
            if(c->p->types[expected]->count!=e->argc) vx_fail(c->p,e->token,"array initializer has the wrong length");
        }
        if(!e->argc && element==TY_INFER) vx_fail(c->p,e->token,"empty array initializer requires an explicit array type");
        for(Arg *a=e->args;a;a=a->next) {
            Type t=check_expr(c,a->expr,element);
            if(t==TY_VOID) vx_fail(c->p,a->expr->token,"array element cannot be void");
            if(element==TY_INFER) element=t;
        }
        actual=expected?expected:vx_resolve_type(c->p,vx_array_type(c->p,element,e->argc,e->token)); break;
    }
    case EX_FIELD: {
        Type t=check_expr(c,e->left,TY_INFER); t=auto_deref(c,&e->left,t);
        if(t>=TY_AGGREGATE&&c->p->types[t]->lazy&&c->f->lazy_helper&&vx_word(c->p,e->member,"$ready_value")) e->id=0;
        else e->id=field_id(c->p,t,e->member);
        actual=c->p->types[t]->fields[e->id].type; break;
    }
    case EX_INDEX:actual=check_index(c,e);break;
    case EX_CHOICE:actual=check_choice(c,e);break;
    case EX_MATCH: {
        Type t=match_type(c,e->left); actual=expected;
        for(Arm *a=e->arms;a;a=a->next) {
            int saved=c->count; ++c->scope; arm_bindings(c,t,a);
            if(!a->expr) vx_fail(c->p,a->token,"a value match requires expression arms; use a statement match for blocks");
            Type branch=check_expr(c,a->expr,actual);
            if(actual==TY_INFER) actual=branch;
            end_scope(c,saved,a->end); c->count=saved; --c->scope;
        }
        vx_check_patterns(c->p,t,e->arms,e->token); break;
    }
    default:vx_fail(c->p,e->token,"internal error: expression checked twice");
    }
    e->type=actual;
    if(expected!=actual&&vx_callable(c->p,expected)&&(vx_callable(c->p,actual)||vx_function_item(c->p,actual))) actual=coerce_callable(c,e,actual,expected);
    if(vx_dependent(c->p,actual)) c->f->uses_references=true;
    if(expected!=TY_INFER&&actual!=expected) type_mismatch(c->p,e->token,expected,actual);
    if((e->kind==EX_UNARY||e->kind==EX_BINARY)&&arithmetic_fault(e)) {
        use(c,e->token,-1,EF_ABORT); c->f->last_use->arithmetic_check=e;
    }
    --c->depth; return actual;
}
static int place(Checker *c,Expr *e,bool read) {
    if(e->kind==EX_VAR) {
        Binding *b=lookup(c,e->token);
        if(!b) vx_fail(c->p,e->token,"unknown assignment target");
        if(read&&!c->initialized[b->id]) vx_fail(c->p,e->token,"value may be uninitialized");
        e->id=b->id; e->type=b->type;
    } else check_expr(c,e,TY_INFER);
    int root=exclusive_place(c,e,true);
    if(root<0) use(c,e->token,-1,EF_WRITE);
    return root;
}
static unsigned char *snapshot(Checker *c) {
    unsigned char *copy=vx_alloc(c->p,VX_MAX_LOCALS); memcpy(copy,c->initialized,VX_MAX_LOCALS); return copy;
}
static void check_return(Checker *c,Expr *e) {
    Type hidden=vx_opaque_result(c->p,c->f->result);
    if(!hidden) { check_expr(c,e,c->f->result); return; }
    Type actual=check_expr(c,e,TY_INFER);
    if(actual==c->f->result) return;
    if(hidden!=c->f->result) {
        TypeDef *result=c->p->types[c->f->result];
        if(actual<TY_AGGREGATE||!c->p->types[actual]->lazy||c->p->types[actual]->type_args[0]!=result->type_args[0])
            vx_fail(c->p,e->token,"opaque Lazy return requires a suspension of its declared value type");
        actual=c->p->types[actual]->type_args[1];
    }
    if(vx_borrowed_closure(c->p,actual)) vx_fail(c->p,e->token,"borrowed captures cannot escape through an opaque result");
    if(!vx_closure(c->p,actual)&&!vx_function_item(c->p,actual)) vx_fail(c->p,e->token,"opaque callable return requires a concrete closure or function item");
    TypeDef *d=c->p->types[hidden],*source=c->p->types[actual];
    Function *contract=d->signature; contract->result=vx_resolve_type(c->p,contract->result);
    for(int i=0;i<contract->param_count;++i) contract->params[i].type=vx_resolve_type(c->p,contract->params[i].type);
    vx_borrow_signature(c->p,contract);
    if(source->signature->call_mode>contract->call_mode||!vx_callable_shape(c->p,source->signature,contract))
        vx_fail(c->p,e->token,"returned closure does not satisfy the opaque invocation mode or signature");
    if(d->element&&d->element!=actual) vx_fail(c->p,e->token,"all opaque returns must have the same concrete closure type");
    d->element=actual; d->slots=source->slots; d->closure_target=source->closure_target;
    d->resolved=hidden; d->state=2;
    if(hidden!=c->f->result) c->f->result=vx_resolve_type(c->p,c->f->result);
    Expr *value=vx_alloc(c->p,sizeof *value); *value=*e;
    memset(e,0,sizeof *e); e->kind=EX_FN_COERCE; e->token=value->token; e->left=value; e->type=c->f->result;
}
static unsigned block(Checker *c,Stmt *s,bool nested) {
    size_t scope_end=s?s->scope_end:0;
    int saved=c->count; if(nested) ++c->scope; unsigned flow=FLOW_NEXT;
    for(;s;s=s->next) {
        if(!(flow&FLOW_NEXT)) vx_fail(c->p,s->token,"unreachable statement after return, panic, break, or continue");
        unsigned current=FLOW_NEXT;
        switch(s->kind) {
        case ST_BIND:
            s->type=vx_resolve_type(c->p,s->type);
            if(s->comptime) ++c->static_context;
            if(s->lazy) {
                Type result=s->type;
                if(result==TY_VOID||vx_dependent(c->p,result)||vx_dyn_fn(c->p,result)) vx_fail(c->p,s->name,"lazy result must be an owned non-void value");
                Type thunk=check_expr(c,s->expr,TY_INFER);
                if(vx_exclusive(c->p,thunk)||vx_exclusive_captures(c->p,thunk)) vx_fail(c->p,s->name,"lazy values support shared borrowed captures; exclusive captures require further loan contracts");
                Type args[]={result,thunk};
                s->type=vx_resolve_type(c->p,vx_apply_type(c->p,c->p->lazy_type,args,2,s->name));
                Expr *wrapped=vx_alloc(c->p,sizeof *wrapped); wrapped->kind=EX_CHOICE; wrapped->type=s->type; wrapped->token=s->name;
                wrapped->member=c->p->types[s->type]->variants[0].name; wrapped->argc=1;
                wrapped->args=vx_alloc(c->p,sizeof *wrapped->args); wrapped->args->expr=s->expr; s->expr=wrapped;
            } else if(s->expr) s->type=check_expr(c,s->expr,s->type);
            if(s->comptime) {
                --c->static_context;
                if(!(c->f->comptime&&c->f->generic_count?vx_stage_template_type(c->p,s->type):vx_stage_type(c->p,s->type))) vx_fail(c->p,s->name,"comptime values require owned scalar or aggregate data; references and handles cannot cross stages");
                if(!c->f->comptime) vx_stage_value(c->p,c->f,s->expr);
            }
            s->local=bind(c,s->name,s->type,s->mutable_binding,s->expr!=NULL);
            if(!s->mutable_binding) c->bindings[c->count-1].target=callable_target(s->expr);
            c->f->local_starts[s->local]=s->end;
            if(s->comptime&&!c->f->comptime) c->bindings[c->count-1].constant=s->expr;
            if(never(s->expr)) current=FLOW_RETURN;
            break;
        case ST_ASSIGN: {
            s->local=place(c,s->target,s->op>=0); s->type=s->target->type;
            if(s->target->kind==EX_VAR&&vx_capture(c->f,s->target->id)>=0&&vx_dependent(c->p,s->type))
                vx_fail(c->p,s->token,"borrowed capture origins cannot be rebound");
            if((vx_borrowed_closure(c->p,s->type)||vx_shared_aggregate(c->p,s->type))&&s->target->kind!=EX_VAR&&!c->f->lazy_helper)
                vx_fail(c->p,s->token,"borrowed closure dependencies cannot be replaced through a reference");
            for(const Expr *part=s->target;part&&(part->kind==EX_FIELD||part->kind==EX_INDEX);part=part->left)
                if(vx_shared_aggregate(c->p,part->left->type)) vx_fail(c->p,s->token,"replace a shared borrowed aggregate as a whole; field origin updates are not supported yet");
            unsigned cleanup=vx_cleanup_effects(c->p,s->type);
            if(cleanup) { use(c,s->token,-1,cleanup); c->f->last_use->cleanup=true; }
            Type rhs=(s->op==VX_SHL||s->op==VX_SHR)?TY_INFER:s->type;
            Type rt=check_expr(c,s->expr,rhs);
            if(s->op>=0) {
                if((!vx_integer(s->type)&&!vx_float(s->type)) ||
                    ((s->op>=VX_AND||s->op==VX_MOD)&&!vx_integer(s->type)))
                    vx_fail(c->p,s->token,"compound assignment requires an appropriate numeric type");
                if(rhs==TY_INFER&&!vx_integer(rt)) vx_fail(c->p,s->expr->token,"shift count must be an integer");
                Expr operation={.kind=EX_BINARY,.type=s->type,.operand_type=s->type,.op=s->op,.left=s->target,.right=s->expr};
                if(arithmetic_fault(&operation)) { use(c,s->token,-1,EF_ABORT); c->f->last_use->arithmetic_assignment=s; }
            }
            if(s->local>=0) c->initialized[s->local]=1;
            if(never(s->expr)) current=FLOW_RETURN;
            break;
        }
        case ST_EXPR:check_expr(c,s->expr,TY_INFER);if(never(s->expr)) current=FLOW_RETURN;break;
        case ST_RETURN:
            if(s->expr) check_return(c,s->expr);
            else if(c->f->result!=TY_VOID) vx_fail(c->p,s->token,"return requires a value of type %s",vx_type(c->f->result));
            if(s->tail) {
                Expr *e=s->expr;
                if(e->kind!=EX_CALL&&e->kind!=EX_GENERIC_CALL&&e->kind!=EX_METHOD&&e->kind!=EX_INVOKE&&e->kind!=EX_CLOSURE_CALL&&e->kind!=EX_VIEW_CALL&&e->kind!=EX_BOX_CALL)
                    vx_fail(c->p,e->token,"return tail requires a function call with no pending result operation");
            }
            current=FLOW_RETURN;break;
        case ST_IF: {
            check_expr(c,s->expr,TY_BOOL); unsigned char *before=snapshot(c);
            unsigned yes=block(c,s->body,true); unsigned char *after_yes=snapshot(c);
            memcpy(c->initialized,before,VX_MAX_LOCALS); unsigned no=block(c,s->otherwise,true);
            for(int i=0;i<VX_MAX_LOCALS;++i) {
                if((yes&FLOW_NEXT)&&(no&FLOW_NEXT)) c->initialized[i]&=after_yes[i];
                else if(yes&FLOW_NEXT) c->initialized[i]=after_yes[i];
            }
            current=never(s->expr)?FLOW_RETURN:yes|no;break;
        }
        case ST_BLOCK:if(s->recursive) check_recursive_group(c,s); else current=block(c,s->body,true);break;
        case ST_LOOP: {
            int count=c->count; ++c->scope;
            if(s->initial) block(c,s->initial,false);
            check_expr(c,s->expr,TY_BOOL); unsigned char *before=snapshot(c);
            ++c->loops; unsigned body=block(c,s->body,true); --c->loops;
            memcpy(c->initialized,before,VX_MAX_LOCALS);
            if(s->step) block(c,s->step,false);
            memcpy(c->initialized,before,VX_MAX_LOCALS);
            end_scope(c,count,s->end); c->count=count; --c->scope;
            current=FLOW_NEXT|(body&FLOW_RETURN);
            Type t; uint64_t value;
            if(vx_constant(s->expr,&t,&value)&&value && !(body&FLOW_BREAK)) current=body&FLOW_RETURN;
            if(never(s->expr)) current=FLOW_RETURN;
            break;
        }
        case ST_BREAK:case ST_CONTINUE:
            if(!c->loops) vx_fail(c->p,s->token,"break/continue requires an enclosing loop");
            current=s->kind==ST_BREAK?FLOW_BREAK:FLOW_CONTINUE;break;
        case ST_MATCH: {
            Type t=match_type(c,s->expr);
            unsigned char *before=snapshot(c), *merged=vx_alloc(c->p,VX_MAX_LOCALS);
            memset(merged,1,VX_MAX_LOCALS); current=0;
            for(Arm *a=s->arms;a;a=a->next) {
                memcpy(c->initialized,before,VX_MAX_LOCALS);
                int count=c->count; ++c->scope; arm_bindings(c,t,a);
                unsigned arm;
                if(a->expr) { check_expr(c,a->expr,TY_INFER); arm=never(a->expr)?FLOW_RETURN:FLOW_NEXT; }
                else arm=block(c,a->body,false);
                if(arm&FLOW_NEXT) for(int i=0;i<VX_MAX_LOCALS;++i) merged[i]&=c->initialized[i];
                current|=arm; end_scope(c,count,a->end); c->count=count; --c->scope;
            }
            vx_check_patterns(c->p,t,s->arms,s->token); memcpy(c->initialized,merged,VX_MAX_LOCALS);
            if(never(s->expr)) current=FLOW_RETURN;
            break;
        }
        }
        flow=(flow&~FLOW_NEXT)|current;
    }
    if(nested) { end_scope(c,saved,scope_end); c->count=saved; --c->scope; }
    return flow;
}
unsigned vx_forbidden(unsigned caps) {
    if (caps & CP_PURE) return EF_ALL;
    unsigned mask = 0;
    if (caps & (CP_HOT | CP_NOALLOC)) mask |= EF_ALLOC;
    if (caps & (CP_HOT | CP_NOPANIC)) mask |= EF_PANIC;
    if (caps & (CP_HOT | CP_NOBLOCK)) mask |= EF_BLOCK;
    return mask;
}
static const char *effect_name(unsigned bit) {
    return bit == EF_IO ? "io" : bit == EF_ALLOC ? "alloc" : bit == EF_PANIC ? "panic" : bit == EF_BLOCK ? "block" : "write";
}
static void explain(VxnProgram *p, int root, unsigned bit) {
    int queue[VX_MAX_FUNCTIONS], parent[VX_MAX_FUNCTIONS], first = 0, last = 0, found = root;
    Use *origin = NULL;
    for (int i = 0; i < p->function_count; ++i) parent[i] = -2;
    queue[last++] = root; parent[root] = -1;
    while (first < last && !origin) {
        int id = queue[first++];
        for (Use *u = p->functions[id]->uses; u; u = u->next) {
            if(u->comptime) continue;
            if (u->effects & bit) { origin = u; found = id; break; }
            if (u->callee >= 0 && parent[u->callee] == -2 && (p->functions[u->callee]->effects & bit)) {
                parent[u->callee] = id; queue[last++] = u->callee;
            }
        }
    }
    int chain[VX_MAX_FUNCTIONS], n = 0;
    for (int id = found; id >= 0; id = parent[id]) chain[n++] = id;
    char path[768] = "", name[81]; size_t used = 0;
    for (int i = n - 1; i >= 0; --i) {
        vx_name(p, p->functions[chain[i]]->name, name, sizeof name);
        int wrote = snprintf(path + used, sizeof path - used, "%s%s", used ? " -> " : "", name);
        if (wrote < 0 || (size_t)wrote >= sizeof path - used) { used = sizeof path - 1; break; }
        used += (size_t)wrote;
    }
    if (origin) {
        vx_name(p, origin->token, name, sizeof name);
        snprintf(path + used, sizeof path - used, " -> %s", name);
    }
    unsigned caps = p->functions[root]->caps;
    const char *cap = (caps & CP_PURE) ? "pure" : (caps & CP_HOT) ? "hot" :
        bit == EF_ALLOC ? "noalloc" : bit == EF_PANIC ? "nopanic" : "noblock";
    vx_fail(p, p->functions[root]->cap_token, "effect `%s` violates `%s`: %s%s", effect_name(bit), cap, path,
        origin&&origin->cleanup?" (automatic owner cleanup)":"");
}

static size_t statement_cost(const VxnProgram *p,const Stmt *s);
static size_t expression_cost(const VxnProgram *p,const Expr *e) {
    if(!e) return 0;
    size_t cost=8+(size_t)vx_slots(p,e->type)*2;
    if(e->kind==EX_TRY) cost+=(size_t)vx_slots(p,e->operand_type)*2;
    cost+=expression_cost(p,e->left)+expression_cost(p,e->right);
    for(Arg *a=e->args;a;a=a->next) cost+=expression_cost(p,a->expr);
    for(Arm *a=e->arms;a;a=a->next) cost+=expression_cost(p,a->guard)+expression_cost(p,a->expr)+statement_cost(p,a->body);
    return cost;
}
static size_t statement_cost(const VxnProgram *p,const Stmt *s) {
    size_t cost=0;
    for(;s;s=s->next) {
        cost+=expression_cost(p,s->expr)+expression_cost(p,s->target)+statement_cost(p,s->body)+
            statement_cost(p,s->otherwise)+statement_cost(p,s->initial)+statement_cost(p,s->step);
        for(Arm *a=s->arms;a;a=a->next) cost+=expression_cost(p,a->guard)+expression_cost(p,a->expr)+statement_cost(p,a->body);
    }
    return cost;
}
void vx_check_function(VxnProgram *p,Function *f) {
    if(f->check_state==2) return;
    if(f->check_state==1) vx_fail(p,f->name,"cyclic compile-time dependency while checking a function");
    if(++p->check_depth>VX_MAX_DEPTH) vx_fail(p,f->name,"type/stage dependency depth limit exceeded");
    f->check_state=1;
    if(!vx_opaque_result(p,f->result)) f->result=vx_resolve_type(p,f->result);
    if(vx_dyn_fn(p,f->result)) vx_fail(p,f->name,"DynFn requires Box storage");
    Checker *c=vx_alloc(p,sizeof *c); c->p=p; c->f=f; c->parent=f->capture_context;
    for(int j=0;j<f->param_count;++j) {
        Param *param=&f->params[j]; param->type=vx_resolve_type(p,param->type);
        vx_borrow_parameter(p,f,param);
        bind(c,param->name,param->type,param->mutable_binding,true);
    }
    if((block(c,f->body,false)&FLOW_NEXT)&&f->result!=TY_VOID) vx_fail(p,f->name,"not every path returns a value");
    for(int j=0;j<f->local_count;++j) if(!f->local_ends[j]) f->local_ends[j]=f->end;
    vx_frame_layout(p,f);
    if(f->capture_count) {
        vx_check_function_ownership(p,f);
        if(f->call_mode==2) for(int i=0;i<f->capture_count;++i) {
            unsigned effects=vx_cleanup_effects(p,f->captures[i].type);
            if(effects) { use(c,f->captures[i].name,-1,effects); f->last_use->cleanup=true; }
        }
        if(f->call_mode==1) use(c,f->name,-1,EF_WRITE);
    }
    f->check_state=2; --p->check_depth;
}
void vx_frame_layout(VxnProgram *p,Function *f) {
    f->local_slots=0;
    for(int i=0;i<f->local_count;++i) {
        f->offsets[i]=f->local_slots; f->local_slots+=vx_slots(p,f->local_types[i]);
    }
    size_t cost=128+(size_t)f->local_slots*2+statement_cost(p,f->body);
    for(int i=0;i<f->local_count;++i) if(vx_owned(p,f->local_types[i])) cost+=4;
    if(cost>16384) vx_fail(p,f->name,"function exceeds conservative native frame budget (16384 scalar slots)");
    f->stack_cost=(int)cost;
}
void vx_check_global(VxnProgram *p,int id) {
    Global *g=&p->globals[id];
    if(g->state==2) return;
    if(g->state==1) vx_fail(p,g->name,"cyclic comptime constant dependency");
    if(++p->check_depth>VX_MAX_DEPTH) vx_fail(p,g->name,"type/stage dependency depth limit exceeded");
    g->state=1; g->type=vx_resolve_type(p,g->type);
    Checker *c=vx_alloc(p,sizeof *c); c->p=p; c->f=g->context; c->static_context=1;
    c->f->result=g->type;
    check_expr(c,g->expr,g->type); vx_stage_value(p,c->f,g->expr);
    g->state=2; --p->check_depth;
}
uint64_t vx_check_length(VxnProgram *p,Expr *e) {
    Function *f=vx_alloc(p,sizeof *f); f->name=e->token;
    Checker *c=vx_alloc(p,sizeof *c); c->p=p; c->f=f; c->static_context=1;
    Type t=check_expr(c,e,TY_INFER);
    if(!vx_integer(t)) vx_fail(p,e->token,"array length must be an integer compile-time value");
    vx_stage_value(p,f,e);
    if(vx_signed_type(t)&&vx_signed_value(e->value,vx_width(t))<0) vx_fail(p,e->token,"array length must be in 0..1024");
    return e->value;
}
void vx_check(VxnProgram *p) {
    for(int i=TY_AGGREGATE;i<p->type_count;++i)
        if(!p->types[i]->name.module) {
            if(vx_word(p,p->types[i]->name,"BoxResult")) p->box_result=i;
            if(vx_word(p,p->types[i]->name,"ParseResult")) p->parse_result=i;
            if(vx_word(p,p->types[i]->name,"Utf8Result")) p->utf8_result=i;
            if(vx_word(p,p->types[i]->name,"Utf8Read")) p->utf8_read=i;
            if(vx_word(p,p->types[i]->name,"StringResult")) p->string_result=i;
            if(vx_word(p,p->types[i]->name,"StringAlloc")) p->string_alloc=i;
            if(vx_word(p,p->types[i]->name,"StringEditResult")) p->string_edit=i;
            if(vx_word(p,p->types[i]->name,"TextSearch")) p->text_search=i;
            if(vx_word(p,p->types[i]->name,"TextOrder")) p->text_order=i;
            if(vx_word(p,p->types[i]->name,"StringIndexResult")) p->string_index=i;
            if(vx_word(p,p->types[i]->name,"TextSliceResult")) p->text_slice=i;
            if(vx_word(p,p->types[i]->name,"StrResult")) p->str_result=i;
            if(vx_word(p,p->types[i]->name,"SplitResult")) p->split_result=i;
            if(vx_word(p,p->types[i]->name,"LazyError")) p->lazy_error=i;
            if(vx_word(p,p->types[i]->name,"LazyResult")) p->lazy_result=i;
            if(vx_word(p,p->types[i]->name,"RecursiveCell")) { p->recursive_cell=i; p->types[i]->recursive_cell=true; }
            if(vx_word(p,p->types[i]->name,"Lazy")) { p->lazy_type=i; p->types[i]->lazy=true; }
            if(vx_word(p,p->types[i]->name,"VecResult")) p->vec_result=i;
            if(vx_word(p,p->types[i]->name,"VecPop")) p->vec_pop=i;
            if(vx_word(p,p->types[i]->name,"VecInsert")) p->vec_insert=i;
            if(vx_word(p,p->types[i]->name,"AllocResult")) p->alloc_result=i;
        }
    for(int i=0;i<p->function_count;++i) {
        Function *f=p->functions[i];
        if(vx_fn_view(p,f->result)) vx_fail(p,f->name,"call-scoped callable views cannot be returned");
        if(f->operator_impl||f->anonymous) continue;
        if(vx_word(p,f->name,"try_force")||vx_word(p,f->name,"delay")||vx_word(p,f->name,"force")||vx_word(p,f->name,"force_take")) vx_fail(p,f->name,"cannot redefine a builtin");
        for(int j=0;j<i;++j) if(!p->functions[j]->operator_impl&&!p->functions[j]->anonymous && vx_same(p,f->name,p->functions[j]->name) &&
            (f->name.module==p->functions[j]->name.module || !p->functions[j]->name.module)) vx_fail(p,f->name,"duplicate function or builtin");
        if(vx_word(p,f->name,"mem_replace")||vx_word(p,f->name,"mem_swap")||vx_word(p,f->name,"uniq_split")||vx_word(p,f->name,"string_view")||vx_word(p,f->name,"str_bytes")||vx_word(p,f->name,"str_byte_len")||vx_word(p,f->name,"string_from_utf8")||vx_word(p,f->name,"string_into_bytes")||vx_word(p,f->name,"string_bytes")||vx_word(p,f->name,"string_byte_len")||vx_word(p,f->name,"utf8_validate")||vx_word(p,f->name,"utf8_next")||vx_word(p,f->name,"parse_int")||vx_word(p,f->name,"parse_float")||vx_word(p,f->name,"box_fn")||vx_word(p,f->name,"fn_ref")||vx_word(p,f->name,"fn_uniq")||vx_word(p,f->name,"box_into_inner")||vx_word(p,f->name,"box_new")||vx_word(p,f->name,"box_ref")||vx_word(p,f->name,"box_uniq")||vx_word(p,f->name,"output")||vx_word(p,f->name,"panic")||vx_word(p,f->name,"len")||vx_word(p,f->name,"slice")||vx_word(p,f->name,"uniq_slice")||vx_string_builtin(p,f->name)>=0||vx_vec_builtin(p,f->name)>=0||vx_io_builtin(p,f->name)>=0||vx_library_builtin(p,f->name)>=0) vx_fail(p,f->name,"cannot redefine a builtin");
        bool main=f->name.module==1 && vx_word(p,f->name,"main");
        if(main) {
            p->main_id=i;
            if(f->comptime) vx_fail(p,f->name,"main must execute at runtime");
            if(f->result!=TY_VOID||f->param_count||f->generic_count) vx_fail(p,f->name,"main must return void and take no parameters");
        }
        if((main||f->public_api)&&!f->caps) vx_fail(p,f->name,"main and public functions require explicit effect annotations");
        if((f->caps&(CP_IO|CP_PURE))==(CP_IO|CP_PURE)) vx_fail(p,f->cap_token,"pure and io are conflicting effect annotations");
    }
    for(int i=0;i<p->global_count;++i) {
        Global *g=&p->globals[i];
        if(vx_word(p,g->name,"try_force")||vx_word(p,g->name,"delay")||vx_word(p,g->name,"force")||vx_word(p,g->name,"force_take")) vx_fail(p,g->name,"comptime constant conflicts with a builtin");
        for(int j=0;j<i;++j) if(g->name.module==p->globals[j].name.module && vx_same(p,g->name,p->globals[j].name)) vx_fail(p,g->name,"duplicate comptime constant");
        if(function_id(p,g->name)>=0||vx_word(p,g->name,"mem_replace")||vx_word(p,g->name,"mem_swap")||vx_word(p,g->name,"uniq_split")||vx_word(p,g->name,"string_view")||vx_word(p,g->name,"str_bytes")||vx_word(p,g->name,"str_byte_len")||vx_word(p,g->name,"string_from_utf8")||vx_word(p,g->name,"string_into_bytes")||vx_word(p,g->name,"string_bytes")||vx_word(p,g->name,"string_byte_len")||vx_word(p,g->name,"utf8_validate")||vx_word(p,g->name,"utf8_next")||vx_word(p,g->name,"parse_int")||vx_word(p,g->name,"parse_float")||vx_word(p,g->name,"box_fn")||vx_word(p,g->name,"fn_ref")||vx_word(p,g->name,"fn_uniq")||vx_word(p,g->name,"box_into_inner")||vx_word(p,g->name,"box_new")||vx_word(p,g->name,"box_ref")||vx_word(p,g->name,"box_uniq")||vx_word(p,g->name,"output")||vx_word(p,g->name,"panic")||vx_word(p,g->name,"len")||vx_word(p,g->name,"slice")||vx_word(p,g->name,"uniq_slice")||vx_string_builtin(p,g->name)>=0||vx_vec_builtin(p,g->name)>=0||vx_io_builtin(p,g->name)>=0||vx_library_builtin(p,g->name)>=0)
            vx_fail(p,g->name,"comptime constant conflicts with a function or builtin");
    }
    vx_prepare_generics(p);
    for(int i=TY_AGGREGATE;i<p->type_count;++i) {
        if(p->types[i]->constructor_arity) continue;
        if(p->types[i]->generic_count) {
            ++p->check_depth; (void)layout(p,i,0); --p->check_depth;
            finish_types(p);
        }
        else { vx_resolve_type(p,i); vx_check_type_application(p,i); }
    }
    vx_check_interfaces(p);
    for(int i=0;i<p->function_count;++i) for(int j=0;j<p->functions[i]->param_count;++j) check_default(p,p->functions[i],j);
    for(int i=0;i<p->global_count;++i) vx_check_global(p,i);
    for(int i=0;i<p->function_count;++i) vx_check_function(p,p->functions[i]);
    vx_check_stages(p);
    vx_prove_ranges(p);
    bool changed;
    do {
        changed=false;
        for(int i=0;i<p->function_count;++i) {
            Function *f=p->functions[i]; unsigned effects=0;
            for(Use *u=f->uses;u;u=u->next) if(!u->comptime) effects|=u->callee<0?u->effects:p->functions[u->callee]->effects;
            if(effects!=f->effects) { f->effects=effects; changed=true; }
        }
    } while(changed);
    for(int i=0;i<p->function_count;++i) {
        for(Use *u=p->functions[i]->uses;u;u=u->next)
            if(u->guard&&!u->comptime&&(u->callee<0?u->effects:p->functions[u->callee]->effects))
                vx_fail(p,u->token,"match guards must be pure (no mutation, allocation, I/O, blocking or panic)");
        unsigned violation=p->functions[i]->effects&vx_forbidden(p->functions[i]->caps);
        for(unsigned bit=1;bit<=EF_WRITE;bit<<=1) if(violation&bit) explain(p,i,bit);
    }
    vx_check_implementations(p);
    vx_specialize(p);
    vx_prepare_tails(p);
    vx_check_borrows(p);
    vx_check_callables(p);
    vx_check_ownership(p);
}
void vxn_report(const VxnProgram *p, FILE *out) {
    if (!vxn_ok(p)) return;
    for (int i = p->builtin_functions; i < p->function_count; ++i) {
        Function *f = p->functions[i]; char name[81],type_name[512]; vx_name(p, f->name, name, sizeof name);
        if(p->module_count>2) fprintf(out,"%s::",p->modules[f->name.module].name);
        if(f->anonymous) fprintf(out,"fn@%zu",f->name.start-p->modules[f->name.module].start);
        else fputs(name,out);
        int generic_count=f->instance?p->functions[f->instance_of]->generic_count:f->generic_count;
        if(generic_count) {
            fputc('[',out);
            for(int j=0;j<generic_count;++j) {
                vx_describe_type(p,f->instance?f->instance_types[j]:f->generic_types[j],type_name,sizeof type_name);
                fprintf(out,"%s%s",j?", ":"",type_name);
            }
            fputc(']',out);
        }
        vx_describe_type(p,f->result,type_name,sizeof type_name);
        fprintf(out, ": %s%s; effects {", f->comptime?"comptime ":"", type_name); bool comma = false;
        for (unsigned bit = 1; bit <= EF_WRITE; bit <<= 1) if (f->effects & bit) {
            fprintf(out, "%s%s", comma ? ", " : "", effect_name(bit)); comma = true;
        }
        fputs("}", out);
        if(f->dynamic_calls) fputs("; dynamic call targets",out);
        if(f->tail_member) fputs("; constant-stack tail dispatch",out);
        if(f->capture_count) fprintf(out,"; %s invocation; %d inline captures (%d cells)",
            f->call_mode==2?"consuming":f->call_mode==1?"exclusive":"shared",f->capture_count,vx_slots(p,f->closure_type));
        if(vx_returned_borrow(p,f->result)) {
            fputs("; from(",out); bool first=true;
            for(int j=0;j<f->param_count;++j) if(f->from_mask&(UINT32_C(1)<<j)) {
                vx_name(p,f->params[j].name,name,sizeof name);
                fprintf(out,"%s%s",first?"":", ",name); first=false;
            }
            fputc(')',out);
        }
        fputc('\n',out);
    }
    fputs("Checked by the Vxn frontend; the native backends and runtime are not verified.\n", out);
}
