#include "internal.h"
#include <string.h>

Type vx_parameter_type(VxnProgram *p,Token name,Token bound) {
    if(p->type_count==VX_MAX_TYPES) vx_fail(p,name,"type limit exceeded");
    Type t=p->type_count++; TypeDef *d=vx_alloc(p,sizeof *d);
    d->kind=TD_PARAMETER; d->name=name;
    if(bound.length) { d->bound_names[0]=bound; d->bound_count=1; }
    d->slots=1; d->state=2; d->resolved=t; p->types[t]=d; return t;
}
Type vx_builtin_constructor(VxnProgram *p,TypeKind kind,Token at) {
    for(int i=TY_AGGREGATE;i<p->type_count;++i) if(p->types[i]->constructor_builtin==(int)kind) return i;
    Type t=vx_parameter_type(p,vx_synthetic(at,kind==TD_BOX?"Box":"Vec"),(Token){0});
    p->types[t]->constructor_arity=1; p->types[t]->constructor_builtin=(int)kind;
    p->types[t]->ownership=2; return t;
}
static int specialize_function(VxnProgram *p,int id,const Type *types,Token at);
bool vx_abstract(const VxnProgram *p,Type t) {
    if(vx_ref(t)) return vx_abstract(p,vx_referent(t));
    if(t<TY_AGGREGATE) return false;
    const TypeDef *d=p->types[t];
    if(d->constructor_builtin) return false;
    if(vx_function_item(p,t)||p->types[t]->kind==TD_CLOSURE) return p->types[t]->signature->generic_count!=0;
    if(vx_callable(p,t)) {
        if(vx_abstract(p,d->signature->result)) return true;
        for(int i=0;i<d->signature->param_count;++i) if(vx_abstract(p,d->signature->params[i].type)) return true;
        return false;
    }
    if(d->generic_count) return d->kind!=TD_STRUCT&&d->kind!=TD_CHOICE;
    if(d->template_type) {
        if(p->types[d->template_type-1]->constructor_arity) return true;
        for(int i=0;i<d->type_arg_count;++i) if(vx_abstract(p,d->type_args[i])) return true;
        return false;
    }
    return d->kind==TD_PARAMETER || ((d->kind==TD_ARRAY||d->kind==TD_SLICE||d->kind==TD_UNIQ_SLICE||d->kind==TD_BOX||d->kind==TD_VEC||vx_fn_view(p,t)||vx_dyn_fn(p,t))&&vx_abstract(p,d->element));
}
static Type substitute_type(VxnProgram *p,Type t,const Type *from,const Type *to,int count,Token at,bool resolve) {
    if(!count) return t;
    for(int i=0;i<count;++i) if(t==from[i]) return to[i];
    if(vx_ref(t)) return vx_retarget(t,substitute_type(p,vx_referent(t),from,to,count,at,resolve));
    if(t<TY_AGGREGATE) return t;
    TypeDef *d=p->types[t];
    if((vx_function_item(p,t)||d->kind==TD_CLOSURE)&&d->signature->generic_count) {
        const Function *f=d->signature; Type types[VX_MAX_GENERICS];
        for(int i=0;i<f->generic_count;++i) {
            types[i]=substitute_type(p,f->generic_types[i],from,to,count,at,resolve);
            if(vx_abstract(p,types[i])) return t;
        }
        int id=specialize_function(p,d->closure_target,types,at);
        return d->kind==TD_CLOSURE?vx_closure_type(p,id,at):vx_function_item_type(p,id,at);
    }
    if(vx_callable(p,t)) {
        Type params[VX_MAX_PARAMS],result=substitute_type(p,d->signature->result,from,to,count,at,resolve);
        bool changed=result!=d->signature->result;
        for(int i=0;i<d->signature->param_count;++i) {
            params[i]=substitute_type(p,d->signature->params[i].type,from,to,count,at,resolve);
            changed|=params[i]!=d->signature->params[i].type;
        }
        if(!changed) return t;
        Function *f=vx_alloc(p,sizeof *f); *f=*d->signature; f->result=result;
        for(int i=0;i<f->param_count;++i) f->params[i].type=params[i];
        Type applied=vx_callable_type(p,f,at); return resolve?vx_resolve_type(p,applied):applied;
    }
    if(vx_opaque(p,t)&&d->generic_count) {
        Type args[VX_MAX_GENERICS]; bool changed=false;
        for(int i=0;i<d->generic_count;++i) {
            args[i]=substitute_type(p,d->generic_types[i],from,to,count,at,resolve);
            changed|=args[i]!=d->generic_types[i];
        }
        if(!changed) return t;
        Type applied=vx_apply_type(p,t,args,d->generic_count,at);
        return resolve?vx_resolve_type(p,applied):applied;
    }
    if(d->template_type) {
        Type base=substitute_type(p,d->template_type-1,from,to,count,at,false);
        Type args[VX_MAX_GENERICS]; bool changed=base!=d->template_type-1;
        for(int i=0;i<d->type_arg_count;++i) { args[i]=substitute_type(p,d->type_args[i],from,to,count,at,resolve); changed|=args[i]!=d->type_args[i]; }
        if(!changed) return t;
        Type applied=vx_apply_type(p,base,args,d->type_arg_count,at);
        return resolve?vx_resolve_type(p,applied):applied;
    }
    if(d->kind!=TD_ARRAY && d->kind!=TD_SLICE && d->kind!=TD_UNIQ_SLICE && d->kind!=TD_BOX && d->kind!=TD_VEC && !vx_fn_view(p,t) && !vx_dyn_fn(p,t)) return t;
    Type element=substitute_type(p,d->element,from,to,count,at,resolve);
    if(element==d->element) return t;
    if(d->kind==TD_ARRAY && d->length) (void)vx_resolve_type(p,t);
    Type applied=vx_dyn_fn(p,t)?vx_dyn_fn_type(p,element,at):vx_fn_view(p,t)?vx_fn_view_type(p,element,vx_uniq_fn_view(p,t),at):d->kind==TD_ARRAY?vx_array_type(p,element,d->count,at):d->kind==TD_BOX?vx_box_type(p,element,at):d->kind==TD_VEC?vx_vec_type(p,element,at):d->kind==TD_UNIQ_SLICE?vx_uniq_slice_type(p,element,at):vx_slice_type(p,element,at);
    return resolve?vx_resolve_type(p,applied):applied;
}
Type vx_substitute_type(VxnProgram *p,Type t,const Type *from,const Type *to,int count,Token at) {
    return substitute_type(p,t,from,to,count,at,true);
}
Type vx_apply_type(VxnProgram *p,Type base,const Type *args,int count,Token at) {
    if(base<TY_AGGREGATE) vx_fail(p,at,"type arguments require a generic struct or choice");
    if(p->types[base]->constructor_builtin) {
        if(count!=1) vx_fail(p,at,"builtin constructor requires one type argument");
        return p->types[base]->constructor_builtin==TD_BOX?vx_box_type(p,args[0],at):vx_vec_type(p,args[0],at);
    }
    for(int i=TY_AGGREGATE;i<p->type_count;++i) {
        TypeDef *d=p->types[i];
        if(d->template_type==base+1 && d->type_arg_count==count && !memcmp(d->type_args,args,(size_t)count*sizeof(Type))) return i;
    }
    if(p->type_count==VX_MAX_TYPES) vx_fail(p,at,"type specialization limit exceeded (512)");
    Type t=p->type_count++; TypeDef *d=vx_alloc(p,sizeof *d); p->types[t]=d;
    d->kind=TD_APPLICATION; d->name=at; d->template_type=base+1; d->type_arg_count=count;
    memcpy(d->type_args,args,(size_t)count*sizeof(Type)); return t;
}
static void validate_arguments(VxnProgram *p,const Type *params,int expected,const Type *types,int count,Token at,bool marker,bool lazy);
void vx_check_type_application(VxnProgram *p,Type t) {
    TypeDef *d=p->types[t]; if(!d->template_type) return;
    TypeDef *base=p->types[d->template_type-1];
    if(base->constructor_arity) {
        if(base->constructor_arity!=d->type_arg_count) vx_fail(p,d->name,"constructor application has incorrect arity");
        for(int i=0;i<d->type_arg_count;++i) {
            Type arg=d->type_args[i];
            if(arg==TY_VOID||vx_dependent(p,arg)||vx_kind_arity(p,arg)) vx_fail(p,d->name,"constructor slots require owned value types");
        }
        return;
    }
    validate_arguments(p,base->generic_types,base->generic_count,d->type_args,d->type_arg_count,d->name,d->template_type-1==p->box_result,base->lazy);
}
void vx_expand_type(VxnProgram *p,Type t) {
    TypeDef *d=p->types[t]; if(d->kind!=TD_APPLICATION) return;
    if(d->state==1) vx_fail(p,d->name,"recursive generic type has infinite size");
    Type base_id=d->template_type-1; TypeDef *base=p->types[base_id];
    if(base->constructor_arity&&!base->constructor_builtin) {
        vx_check_type_application(p,t);
        d->kind=TD_PARAMETER; d->slots=1; d->ownership=2; d->state=2; d->resolved=t;
        return;
    }
    if((base->kind!=TD_STRUCT && base->kind!=TD_CHOICE && base->kind!=TD_OPAQUE)||!base->generic_count)
        vx_fail(p,d->name,"type arguments require a declared generic struct or choice");
    if(base->generic_count!=d->type_arg_count) vx_fail(p,d->name,"generic type has incorrect number of type arguments");
    if(base->kind==TD_OPAQUE) (void)vx_resolve_type(p,base_id);
    if(base->lazy&&d->type_arg_count==2&&vx_opaque(p,d->type_args[1]))
        (void)vx_resolve_type(p,d->type_args[1]);
    if(d->kind!=TD_APPLICATION) return;
    d->state=1;
    /* Defer type-argument layout to avoid false cycles through phantom parameters. */
    for(int i=0;i<d->type_arg_count;++i) {
        Type arg=d->type_args[i];
        if(vx_dyn_fn(p,arg)&&base_id!=p->box_result) vx_fail(p,d->name,"DynFn requires Box storage");
        if(arg==TY_VOID||(vx_dependent(p,arg)&&!(base->lazy&&i==1)))
            vx_fail(p,d->name,"type arguments must be owned non-void value types; pass borrows through ref T or Slice[T] parameters");
        if(arg>=TY_AGGREGATE&&p->types[arg]->generic_count&&!vx_opaque(p,arg)&&!p->types[base->generic_types[i]]->constructor_arity)
            vx_fail(p,d->name,"generic value type requires explicit type arguments");
    }
    for(int i=TY_AGGREGATE;i<t;++i) {
        TypeDef *other=p->types[i];
        if(other->state==2 && other->template_type==base_id+1 && other->type_arg_count==d->type_arg_count &&
           !memcmp(other->type_args,d->type_args,(size_t)d->type_arg_count*sizeof(Type))) {
            *d=*other; return;
        }
    }
    Type args[VX_MAX_GENERICS]; memcpy(args,d->type_args,sizeof args);
    Token at=d->name; *d=*base; d->generic_count=0; d->template_type=base_id+1;
    d->type_arg_count=base->generic_count; memcpy(d->type_args,args,sizeof args);
    d->state=1; d->slots=0; d->resolved=t;
    if(d->kind==TD_OPAQUE) {
        Type contract=vx_callable_type(p,base->signature,at);
        contract=substitute_type(p,contract,base->generic_types,args,base->generic_count,at,true);
        d->signature=p->types[contract]->signature;
        d->element=substitute_type(p,base->element,base->generic_types,args,base->generic_count,at,true);
        d->closure_target=p->types[d->element]->closure_target;
        d->slots=vx_slots(p,d->element); d->state=2; return;
    }
    for(int i=0;i<d->count;++i) {
        if(d->kind==TD_STRUCT) d->fields[i].type=substitute_type(p,d->fields[i].type,base->generic_types,args,base->generic_count,at,false);
        else for(int j=0;j<d->variants[i].count;++j)
            d->variants[i].fields[j]=substitute_type(p,d->variants[i].fields[j],base->generic_types,args,base->generic_count,at,false);
    }
    d->state=0;
}
int vx_constraint_id(VxnProgram *p,Token name) {
    int owner=vx_symbol_module(p,name);
    for(int i=0;i<p->constraint_count;++i) {
        Constraint *c=&p->constraints[i];
        if(c->name.module==owner && vx_same(p,name,c->name)) {
            vx_visible(p,name,c->name,c->public_api); return i;
        }
    }
    vx_fail(p,name,"unknown constraint");
}
static bool boolean_operator(int op) { return (op>=VX_EQ&&op<=VX_GE)||op==VX_NOT; }
static int arity(int op) { return op>=VX_NOT?1:2; }
static void operation_shape(VxnProgram *p,Function *f,Type self) {
    if(!f || f->comptime || (f->generic_count&&!f->operator_impl) || f->public_api)
        vx_fail(p,f?f->name:(Token){0},"operators require ordinary safe declarations");
    if(!f->caps) vx_fail(p,f->name,"operator declarations require explicit effect annotations");
    if((f->caps&(CP_PURE|CP_IO))==(CP_PURE|CP_IO)) vx_fail(p,f->cap_token,"pure and io are conflicting effect annotations");
    if(f->operator_code<0) {
        f->result=vx_resolve_type(p,f->result);
        if(vx_fn_view(p,f->result)) vx_fail(p,f->name,"constraint operations cannot return callable views yet");
        if(vx_returned_borrow(p,f->result)&&!f->from_explicit) vx_fail(p,f->name,"borrowed constraint results require explicit from(...)");
        for(int i=0;i<f->param_count;++i) {
            Param *a=&f->params[i]; a->type=vx_resolve_type(p,a->type);
            if(a->type==TY_VOID) vx_fail(p,a->name,"parameter cannot be void");
            if(a->default_value) vx_fail(p,a->name,"constraint operation defaults are not supported");
            vx_borrow_parameter(p,f,a);
            for(int j=0;j<i;++j) if(vx_same(p,a->name,f->params[j].name)) vx_fail(p,a->name,"duplicate parameter");
        }
        vx_borrow_signature(p,f);
        return;
    }
    if(f->param_count!=arity(f->operator_code)) vx_fail(p,f->name,"operator has incorrect arity");
    if(f->from_explicit) vx_fail(p,f->name,"operator signatures currently require owned values");
    f->result=vx_resolve_type(p,f->result);
    if(f->result!=(boolean_operator(f->operator_code)?TY_BOOL:self))
        vx_fail(p,f->name,"operator result must be bool for comparisons/!, otherwise the implementing type");
    for(int i=0;i<f->param_count;++i) {
        Param *a=&f->params[i]; a->type=vx_resolve_type(p,a->type);
        if(a->type!=self || a->default_value || a->noescape)
            vx_fail(p,a->name,"operator parameters must be implementing-type values without defaults or borrow qualifiers");
        for(int j=0;j<i;++j) if(vx_same(p,a->name,f->params[j].name)) vx_fail(p,a->name,"duplicate parameter");
    }
}
static bool same_operation(VxnProgram *p,const Function *a,const Function *b) {
    return a->operator_code==b->operator_code && (a->operator_code>=0||vx_same(p,a->name,b->name));
}
static void implementation_signature(VxnProgram *p,Constraint *c,Type t,Function *f,Function *contract) {
    if(f->operator_code>=0) return;
    if(f->param_count!=contract->param_count ||
       f->result!=vx_substitute_type(p,contract->result,&c->self,&t,1,f->name))
        vx_fail(p,f->name,"implementation signature does not match constraint operation");
    for(int i=0;i<f->param_count;++i) {
        Param *a=&f->params[i],*b=&contract->params[i];
        if(a->type!=vx_substitute_type(p,b->type,&c->self,&t,1,a->name) || !vx_same(p,a->name,b->name) || a->noescape!=b->noescape)
            vx_fail(p,a->name,"implementation parameter type, name and noescape must match constraint operation");
    }
    if(f->from_mask!=contract->from_mask) vx_fail(p,f->name,"implementation from origins must match constraint operation");
}
/* Coherence ignores bounds. Budget traversal of shared/phantom type DAGs. */
typedef struct { int depth,steps; Token at; } PatternWork;
static void pattern_enter(VxnProgram *p,PatternWork *w) {
    if(++w->depth>VX_MAX_DEPTH || ++w->steps>8192)
        vx_fail(p,w->at,"implementation type pattern complexity limit exceeded");
}
static unsigned pattern_parameters(VxnProgram *p,const Implementation *impl,Type t,PatternWork *w) {
    for(int i=0;i<impl->generic_count;++i) if(t==impl->generic_types[i]) return 1u<<i;
    if(!vx_abstract(p,t)) return 0;
    pattern_enter(p,w); unsigned used=0;
    if(t<TY_AGGREGATE) vx_fail(p,impl->name,"unsupported abstract implementation type pattern");
    const TypeDef *d=p->types[t];
    if(d->template_type&&(d->kind==TD_STRUCT||d->kind==TD_CHOICE)) {
        for(int i=0;i<d->type_arg_count;++i) used|=pattern_parameters(p,impl,d->type_args[i],w);
    } else if(d->kind==TD_ARRAY||d->kind==TD_BOX||d->kind==TD_VEC)
        used=pattern_parameters(p,impl,d->element,w);
    else vx_fail(p,impl->name,"generic implementations support parameter, struct, choice, array, Box and Vec type patterns");
    --w->depth; return used;
}
static bool patterns_overlap(VxnProgram *p,Type a,Type b,PatternWork *w) {
    if(a==b) return true;
    if((a>=TY_AGGREGATE&&p->types[a]->kind==TD_PARAMETER)||
       (b>=TY_AGGREGATE&&p->types[b]->kind==TD_PARAMETER)) return true;
    if(a<TY_AGGREGATE||b<TY_AGGREGATE) return false;
    pattern_enter(p,w); const TypeDef *x=p->types[a],*y=p->types[b]; bool result=false;
    if(x->template_type&&x->template_type==y->template_type&&x->type_arg_count==y->type_arg_count) {
        result=true;
        for(int i=0;result&&i<x->type_arg_count;++i) result=patterns_overlap(p,x->type_args[i],y->type_args[i],w);
    } else if(x->kind==y->kind&&(x->kind==TD_ARRAY||x->kind==TD_BOX||x->kind==TD_VEC)&&x->count==y->count)
        result=patterns_overlap(p,x->element,y->element,w);
    --w->depth; return result;
}
static bool bind_pattern(VxnProgram *p,const Implementation *impl,Type pattern,Type actual,Type *types,PatternWork *w) {
    for(int i=0;i<impl->generic_count;++i) if(pattern==impl->generic_types[i]) {
        if(!types[i]) { types[i]=actual; return true; }
        return types[i]==actual;
    }
    if(!vx_abstract(p,pattern)) return pattern==actual;
    if(pattern<TY_AGGREGATE||actual<TY_AGGREGATE) return false;
    pattern_enter(p,w); const TypeDef *x=p->types[pattern],*y=p->types[actual]; bool result=false;
    if(x->template_type&&x->template_type==y->template_type&&x->type_arg_count==y->type_arg_count) {
        result=true;
        for(int i=0;result&&i<x->type_arg_count;++i) result=bind_pattern(p,impl,x->type_args[i],y->type_args[i],types,w);
    } else if(x->kind==y->kind&&(x->kind==TD_ARRAY||x->kind==TD_BOX||x->kind==TD_VEC)&&x->count==y->count)
        result=bind_pattern(p,impl,x->element,y->element,types,w);
    --w->depth; return result;
}
void vx_prepare_generics(VxnProgram *p) {
    for(int t=TY_AGGREGATE;t<p->type_count;++t) {
        TypeDef *d=p->types[t];
        if(d->kind==TD_PARAMETER && d->bound_count) {
            bool requires_copy=false;
            for(int j=0;j<d->bound_count;++j) {
                d->bounds[j]=vx_constraint_id(p,d->bound_names[j]);
                for(int k=0;k<j;++k) if(d->bounds[k]==d->bounds[j]) vx_fail(p,d->bound_names[j],"duplicate operation bound");
                requires_copy|=p->types[p->constraints[d->bounds[j]].self]->ownership==1;
            }
            if(d->ownership==2&&requires_copy) vx_fail(p,d->name,"Move parameter cannot assume a Copy constraint");
            if(!d->ownership) d->ownership=requires_copy?1:2;
        }
    }

    for(int n=0;n<p->function_count;++n) for(int j=0;j<p->functions[n]->generic_count;++j) {
        TypeDef *d=p->types[p->functions[n]->generic_types[j]];
        if(!d->ownership) d->ownership=d->callable_bound?2:1;
    }
    for(int i=0;i<p->constraint_count;++i) {
        Constraint *c=&p->constraints[i];
        for(int j=0;j<i;++j) if(c->name.module==p->constraints[j].name.module && vx_same(p,c->name,p->constraints[j].name))
            vx_fail(p,c->name,"duplicate constraint");
        for(int j=TY_AGGREGATE;j<p->type_count;++j) {
            TypeDef *d=p->types[j];
            if(!d->template_type && (d->kind==TD_STRUCT||d->kind==TD_CHOICE||d->kind==TD_UNRESOLVED) &&
               d->name.module==c->name.module && vx_same(p,c->name,d->name))
                vx_fail(p,c->name,"constraint name conflicts with a value type");
        }
        for(int m=0;m<c->method_count;++m) {
            operation_shape(p,c->methods[m],c->self);
            for(int n=0;n<m;++n) if(same_operation(p,c->methods[m],c->methods[n]))
                vx_fail(p,c->methods[m]->name,c->methods[m]->operator_code>=0?"duplicate constraint operator":"duplicate constraint operation");
        }
    }
    for(int n=0;n<p->implementation_count;++n) {
        Implementation *i=&p->implementations[n];
        for(int j=0;j<i->generic_count;++j) {
            TypeDef *d=p->types[i->generic_types[j]]; if(!d->ownership) d->ownership=1;
        }
    }
    for(int n=0;n<p->implementation_count;++n) {
        Implementation *i=&p->implementations[n]; i->constraint=vx_constraint_id(p,i->name);
        i->type=vx_resolve_type(p,i->type);
        if(i->type==TY_VOID||vx_dependent(p,i->type)||vx_dyn_fn(p,i->type))
            vx_fail(p,i->name,"implementation requires an owned non-void value type");
        PatternWork work={.at=i->name};
        if(pattern_parameters(p,i,i->type,&work)!=(1u<<i->generic_count)-1)
            vx_fail(p,i->name,"every implementation type parameter must occur in its implementing type");
        for(int j=0;j<n;++j) if(p->implementations[j].constraint==i->constraint) {
            work=(PatternWork){.at=i->name};
            if(patterns_overlap(p,p->implementations[j].type,i->type,&work))
                vx_fail(p,i->name,"overlapping implementations for one constraint/type combination");
        }
        Constraint *c=&p->constraints[i->constraint];
        int owner=p->modules[i->name.module].package;
        bool owns_constraint=c->name.module&&p->modules[c->name.module].package==owner;
        bool owns_type=false;
        if(i->type>=TY_AGGREGATE) {
            const TypeDef *d=p->types[i->type];
            owns_type=(d->kind==TD_STRUCT||d->kind==TD_CHOICE)&&d->name.module&&p->modules[d->name.module].package==owner;
        }
        if(!owns_constraint&&!owns_type) vx_fail(p,i->name,"implementation must belong to the package owning the constraint or the outer nominal type");
        if(p->types[c->self]->ownership==1&&vx_owned(p,i->type)) vx_fail(p,i->name,"Copy constraint cannot be implemented for an owner-bearing type; declare its self parameter Move");
        bool seen[VX_MAX_FIELDS]={0}; int ordered[VX_MAX_FIELDS];
        for(int m=0;m<i->method_count;++m) {
            Function *f=p->functions[i->methods[m]]; operation_shape(p,f,i->type);
            int index=0; while(index<c->method_count && !same_operation(p,c->methods[index],f)) ++index;
            if(index==c->method_count) vx_fail(p,f->name,"operator is not declared by this constraint");
            if(seen[index]) vx_fail(p,f->name,f->operator_code>=0?"duplicate implementation operator":"duplicate implementation operation");
            implementation_signature(p,c,i->type,f,c->methods[index]);
            seen[index]=true; ordered[index]=i->methods[m];
        }
        for(int m=0;m<c->method_count;++m) if(!seen[m]) vx_fail(p,i->name,c->methods[m]->operator_code>=0?"implementation is missing a required operator":"implementation is missing a required operation");
        memcpy(i->methods,ordered,(size_t)c->method_count*sizeof(int));
    }
    for(int n=0;n<p->function_count;++n) {
        Function *f=p->functions[n];
        if(f->generic_count && !f->anonymous && !f->caps) vx_fail(p,f->name,"generic functions require explicit effect annotations");
        for(int j=0;j<f->generic_count;++j) {
            TypeDef *d=p->types[f->generic_types[j]];
            for(int k=0;k<d->bound_count;++k) if(f->public_api && !p->constraints[d->bounds[k]].public_api)
                vx_fail(p,f->name,"public generic function exposes a private constraint");
        }
    }
    p->generics_ready=true;
}
static bool has_bound(const VxnProgram *p,Type t,int bound) {
    if(t<TY_AGGREGATE||p->types[t]->kind!=TD_PARAMETER) return false;
    const TypeDef *d=p->types[t];
    for(int i=0;i<d->bound_count;++i) if(d->bounds[i]==bound) return true;
    return false;
}
static Implementation *implementation(VxnProgram *p,int constraint,Type t,Type *types) {
    if(t==TY_VOID||vx_dependent(p,t)||vx_dyn_fn(p,t)) return NULL;
    unsigned char *cache=&p->implementation_cache[t][constraint];
    if(*cache==1) vx_fail(p,p->constraints[constraint].name,"recursive implementation constraint resolution");
    if(*cache==2) return NULL;
    if(*cache>=3) {
        Implementation *impl=&p->implementations[*cache-3];
        if(types) {
            memset(types,0,VX_MAX_GENERICS*sizeof *types); PatternWork w={.at=impl->name};
            (void)bind_pattern(p,impl,impl->type,t,types,&w);
        }
        return impl;
    }
    if(++p->implementation_depth>VX_MAX_DEPTH)
        vx_fail(p,p->constraints[constraint].name,"implementation constraint dependency depth limit exceeded");
    *cache=1;
    for(int i=0;i<p->implementation_count;++i) {
        Implementation *impl=&p->implementations[i]; if(impl->constraint!=constraint) continue;
        Type inferred[VX_MAX_GENERICS]={0}; PatternWork w={.at=impl->name};
        if(!bind_pattern(p,impl,impl->type,t,inferred,&w)) continue;
        bool valid=true;
        for(int j=0;valid&&j<impl->generic_count;++j) {
            Type actual=inferred[j]; const TypeDef *formal=p->types[impl->generic_types[j]];
            if(!actual||actual==TY_VOID||vx_dependent(p,actual)||vx_dyn_fn(p,actual)||
               (formal->ownership==1&&vx_owned(p,actual))) { valid=false; break; }
            for(int k=0;valid&&k<formal->bound_count;++k) {
                int bound=formal->bounds[k];
                valid=actual>=TY_AGGREGATE&&p->types[actual]->kind==TD_PARAMETER ?
                    has_bound(p,actual,bound) : implementation(p,bound,actual,NULL)!=NULL;
            }
        }
        if(!valid) continue;
        *cache=(unsigned char)(i+3); --p->implementation_depth;
        if(types) memcpy(types,inferred,sizeof inferred);
        return impl;
    }
    *cache=2; --p->implementation_depth; return NULL;
}
static void validate_arguments(VxnProgram *p,const Type *params,int expected,const Type *types,int count,Token at,bool marker,bool lazy) {
    if(count!=expected) vx_fail(p,at,"expected %d explicit type arguments, got %d",expected,count);
    for(int i=0;i<count;++i) {
        Type t=types[i];
        const TypeDef *formal=p->types[params[i]];
        if(formal->constructor_arity) {
            if(vx_kind_arity(p,t)!=formal->constructor_arity) vx_fail(p,at,"type constructor argument has incorrect kind/arity");
            const TypeDef *actual=p->types[t];
            if(vx_dependent(p,t)||actual->lazy||actual->recursive_cell)
                vx_fail(p,at,"constructor argument must produce ordinary owned values without sealed-state requirements");
            for(int j=0;j<actual->generic_count;++j) {
                const TypeDef *slot=p->types[actual->generic_types[j]];
                if(slot->ownership==1||slot->bound_count||slot->callable_bound||slot->constructor_arity)
                    vx_fail(p,at,"constructor argument must accept arbitrary owned value types");
            }
            continue;
        }
        if(vx_kind_arity(p,t)) vx_fail(p,at,"value type argument cannot be an unapplied constructor");
        if(vx_dyn_fn(p,t)&&!marker) vx_fail(p,at,"DynFn requires Box storage, not an owning generic argument");
        if(t==TY_VOID || (vx_dependent(p,t)&&!(lazy&&i==1))) vx_fail(p,at,"type arguments must be owned non-void value types; pass borrows through ref T or Slice[T] parameters");
        if(p->types[params[i]]->ownership==1 && vx_owned(p,t))
            vx_fail(p,at,"type argument requires Copy; owner-bearing generic function arguments require explicit copy/move constraints (use Move)");
        const TypeDef *d=p->types[params[i]];
        for(int j=0;j<d->bound_count;++j) {
            int bound=d->bounds[j];
            bool satisfied=t>=TY_AGGREGATE && p->types[t]->kind==TD_PARAMETER ? has_bound(p,t,bound) : implementation(p,bound,t,NULL)!=NULL;
            if(!satisfied) vx_fail(p,at,"type argument does not satisfy its declared constraint");
        }
    }
}
void vx_check_type_arguments(VxnProgram *p,const Function *f,const Type *types,int count,Token at) {
    validate_arguments(p,f->generic_types,f->generic_count,types,count,at,false,false);
    if(f->comptime) for(int i=0;i<count;++i) if(!vx_stage_template_type(p,types[i]))
        vx_fail(p,at,"comptime generic calls require reifiable data type arguments");
    vx_check_callable_arguments(p,f,types,at,false);
}
/* Invocation bounds are independent of ownership. Concrete effects/origins
   require whole-program fixed points. */
void vx_check_callable_arguments(VxnProgram *p,const Function *f,const Type *types,Token at,bool complete) {
    for(int i=0;i<f->generic_count;++i) {
        Type bound=p->types[f->generic_types[i]]->callable_bound;
        if(!bound) continue;
        Type t=types[i];
        if(!vx_invocable(p,t)) vx_fail(p,at,"type argument does not satisfy its callable bound");
        bound=vx_substitute_type(p,bound,f->generic_types,types,f->generic_count,at);
        const Function *target=p->types[bound]->signature,*source=p->types[t]->signature;
        if(!vx_callable_shape(p,source,target)||source->call_mode>target->call_mode)
            vx_fail(p,at,"type argument violates callable bound signature or invocation mode");
        if(p->types[t]->kind==TD_CLOSURE||vx_function_item(p,t)) {
            if(complete&&!vx_callable_contract(p,source,target))
                vx_fail(p,at,"closure violates callable bound effects, static targets or borrow origins");
        } else {
            unsigned effects=EF_ALL&~vx_forbidden(source->caps);
            if((effects&vx_forbidden(target->caps))||(source->from_mask&~target->from_mask))
                vx_fail(p,at,"callable value contract exceeds callable bound effects or borrow origins");
            if((target->caps&CP_HOT)&&(vx_callable(p,t)||vx_box_fn(p,t)||!(source->caps&CP_HOT)))
                vx_fail(p,at,"hot callable bound requires a statically known closure target and a hot contract when opaque");
        }
    }
}
static bool inference_pending(const VxnProgram *p,const Function *f,Type t,const Type *types) {
    for(int i=0;i<f->generic_count;++i) if(t==f->generic_types[i]) return !types[i];
    if(vx_ref(t)) return inference_pending(p,f,vx_referent(t),types);
    if(t<TY_AGGREGATE) return false;
    const TypeDef *d=p->types[t];
    if(d->template_type) {
        if(p->types[d->template_type-1]->constructor_arity&&inference_pending(p,f,d->template_type-1,types)) return true;
        for(int i=0;i<d->type_arg_count;++i) if(inference_pending(p,f,d->type_args[i],types)) return true;
    } else if(d->kind==TD_ARRAY||vx_slice(p,t)||vx_heap_type(p,t)||vx_fn_view(p,t)||vx_dyn_fn(p,t)) return inference_pending(p,f,d->element,types);
    else if(vx_callable(p,t)) {
        if(inference_pending(p,f,d->signature->result,types)) return true;
        for(int i=0;i<d->signature->param_count;++i) if(inference_pending(p,f,d->signature->params[i].type,types)) return true;
    }
    return false;
}
Type vx_argument_hint(VxnProgram *p,const Function *f,Type pattern,const Type *types,Token at) {
    if(inference_pending(p,f,pattern,types)) return TY_INFER;
    Type substitutions[VX_MAX_GENERICS];
    for(int i=0;i<f->generic_count;++i) substitutions[i]=types[i]?types[i]:f->generic_types[i];
    return vx_substitute_type(p,pattern,f->generic_types,substitutions,f->generic_count,at);
}
void vx_infer_type_arguments(VxnProgram *p,const Function *f,Type pattern,Type actual,Type *types,Token at) {
    for(int i=0;i<f->generic_count;++i) if(pattern==f->generic_types[i]) {
        if(!types[i]) {
            types[i]=actual;
            /* Record the callable before inferring its signature to break bound cycles.
               Check mode/effects/origins after inference. */
            const TypeDef *bound=p->types[pattern];
            if(bound->callable_bound&&vx_invocable(p,actual)) {
                const Function *wanted=p->types[bound->callable_bound]->signature;
                const Function *given=p->types[actual]->signature;
                if(wanted->param_count==given->param_count) {
                    vx_infer_type_arguments(p,f,wanted->result,given->result,types,at);
                    for(int j=0;j<wanted->param_count;++j)
                        vx_infer_type_arguments(p,f,wanted->params[j].type,given->params[j].type,types,at);
                }
            }
        }
        else if(types[i]!=actual) vx_fail(p,at,"conflicting inferred type arguments; supply explicit type arguments");
        return;
    }
    if(vx_ref(pattern)&&vx_ref(actual)&&vx_retarget(pattern,vx_referent(actual))==actual) {
        vx_infer_type_arguments(p,f,vx_referent(pattern),vx_referent(actual),types,at); return;
    }
    if(pattern>=TY_AGGREGATE&&actual>=TY_AGGREGATE) {
        const TypeDef *a=p->types[pattern],*b=p->types[actual];
        if(a->template_type&&p->types[a->template_type-1]->constructor_arity&&a->type_arg_count==1&&vx_heap_type(p,actual)) {
            vx_infer_type_arguments(p,f,a->template_type-1,vx_builtin_constructor(p,b->kind,at),types,at);
            vx_infer_type_arguments(p,f,a->type_args[0],b->element,types,at); return;
        }
        if(a->template_type&&p->types[a->template_type-1]->constructor_arity&&b->template_type&&a->type_arg_count==b->type_arg_count) {
            vx_infer_type_arguments(p,f,a->template_type-1,b->template_type-1,types,at);
            for(int i=0;i<a->type_arg_count;++i) vx_infer_type_arguments(p,f,a->type_args[i],b->type_args[i],types,at);
            return;
        }
        if(a->template_type&&a->template_type==b->template_type&&a->type_arg_count==b->type_arg_count) {
            for(int i=0;i<a->type_arg_count;++i) vx_infer_type_arguments(p,f,a->type_args[i],b->type_args[i],types,at);
            return;
        }
        if(a->kind==b->kind&&(a->kind==TD_ARRAY||vx_slice(p,pattern)||vx_heap_type(p,pattern)||vx_fn_view(p,pattern)||vx_dyn_fn(p,pattern))&&a->count==b->count) {
            vx_infer_type_arguments(p,f,a->element,b->element,types,at); return;
        }
        if(vx_callable(p,pattern)&&vx_callable(p,actual)&&a->signature->param_count==b->signature->param_count) {
            vx_infer_type_arguments(p,f,a->signature->result,b->signature->result,types,at);
            for(int i=0;i<a->signature->param_count;++i)
                vx_infer_type_arguments(p,f,a->signature->params[i].type,b->signature->params[i].type,types,at);
            if(vx_argument_hint(p,f,pattern,types,at)==actual) return;
        }
    }
    if(pattern==actual) return;
    vx_fail(p,at,"argument does not match the inferred generic parameter type; supply explicit type arguments");
}
Function *vx_operator_contract(VxnProgram *p,Type t,int op,Token at,int *method,int *constraint) {
    if(!vx_abstract(p,t)) return NULL;
    const TypeDef *d=p->types[t]; Function *found=NULL;
    for(int bound=0;bound<p->constraint_count;++bound) {
        Constraint *c=&p->constraints[bound];
        for(int i=0;i<c->method_count;++i) if(c->methods[i]->operator_code==op) {
            if(d->kind==TD_PARAMETER ? !has_bound(p,t,bound) : !implementation(p,bound,t,NULL)) continue;
            if(found) vx_fail(p,at,"ambiguous operator in generic bounds; forward to a helper with one operator bound");
            found=c->methods[i]; *method=i; *constraint=bound;
        }
    }
    if(!found) vx_fail(p,at,"operator is unavailable in the declared generic bounds");
    return found;
}
int vx_operator_implementation(VxnProgram *p,Type t,int op,int constraint,Token at) {
    int found=-1,selected=-1,method=-1;
    for(int bound=0;bound<p->constraint_count;++bound) {
        if(constraint>=0&&constraint!=bound) continue;
        Constraint *c=&p->constraints[bound];
        for(int j=0;j<c->method_count;++j) {
            if(c->methods[j]->operator_code!=op) continue;
            Implementation *impl=implementation(p,bound,t,NULL); if(!impl) continue;
            if(found>=0) vx_fail(p,at,"ambiguous operator across constraints; use a constrained generic function");
            found=impl->methods[j]; selected=bound; method=j;
        }
    }
    if(found<0) vx_fail(p,at,"operator requires scalar operands or a matching constraint implementation");
    return vx_method_implementation(p,selected,t,method,at);
}
const Function *vx_call_contract(const VxnProgram *p,const Expr *e) {
    if(e->call_contract) return p->types[e->call_contract]->signature;
    if(e->kind==EX_INVOKE||e->kind==EX_VIEW_CALL||e->kind==EX_BOX_CALL) return p->types[e->operand_type]->signature;
    if(e->kind==EX_CLOSURE_CALL) return p->types[e->operand_type]->signature;
    return e->kind==EX_METHOD?p->constraints[e->op].methods[e->id]:p->functions[e->id];
}
Function *vx_method_contract(VxnProgram *p,Expr *e) {
    if(!p->generics_ready) vx_fail(p,e->token,"constraint calls require completed constraint/implementation signatures; cyclic staged layout dependency");
    if(e->type_arg_count!=1) vx_fail(p,e->token,"constraint operation requires exactly one implementing type");
    e->op=vx_constraint_id(p,e->token);
    e->operand_type=vx_resolve_type(p,e->type_args[0]);
    Type t=e->operand_type; Constraint *c=&p->constraints[e->op];
    if(t>=TY_AGGREGATE&&p->types[t]->kind==TD_PARAMETER) {
        if(!has_bound(p,t,e->op)) vx_fail(p,e->token,"operation is unavailable in the declared generic bounds");
    } else if(!implementation(p,e->op,t,NULL)) vx_fail(p,e->token,"type argument does not satisfy its declared constraint");
    for(int j=0;j<c->method_count;++j) if(c->methods[j]->operator_code<0&&vx_same(p,e->member,c->methods[j]->name)) {
        e->id=j; return c->methods[j];
    }
    vx_fail(p,e->member,"unknown constraint operation");
}
int vx_method_implementation(VxnProgram *p,int constraint,Type t,int method,Token at) {
    Type types[VX_MAX_GENERICS]; Implementation *impl=implementation(p,constraint,t,types);
    if(!impl) vx_fail(p,at,"type argument does not satisfy its declared constraint");
    int id=impl->methods[method];
    return impl->generic_count?specialize_function(p,id,types,at):id;
}
void vx_check_implementations(VxnProgram *p) {
    for(int i=0;i<p->implementation_count;++i) {
        Implementation *impl=&p->implementations[i]; Constraint *c=&p->constraints[impl->constraint];
        for(int j=0;j<c->method_count;++j) {
            Function *f=p->functions[impl->methods[j]];
            if(f->effects & vx_forbidden(c->methods[j]->caps))
                vx_fail(p,f->name,"implementation effects exceed the declared constraint operation contract");
        }
    }
}

typedef struct { VxnProgram *p; const Function *source; const Type *types; } Clone;
static Type replace(Clone *c,Type t,Token at) { return vx_substitute_type(c->p,t,c->source->generic_types,c->types,c->source->generic_count,at); }
static Expr *clone_expression(Clone *c,const Expr *e);
static Stmt *clone_statements(Clone *c,const Stmt *s);
static int specialized_target(Clone *c,int id,Token at) {
    const Function *f=c->p->functions[id];
    if(!f->anonymous||!f->generic_count) return id;
    Type types[VX_MAX_GENERICS];
    for(int i=0;i<f->generic_count;++i) types[i]=replace(c,f->generic_types[i],at);
    return specialize_function(c->p,id,types,at);
}
static void *copy_node(VxnProgram *p,const void *from,size_t size,Token at) {
    if(++p->nodes>VX_MAX_NODES) vx_fail(p,at,"specialization exceeds syntax node budget");
    void *to=vx_alloc(p,size); memcpy(to,from,size); return to;
}
static Pattern *clone_pattern(Clone *c,const Pattern *n) {
    Pattern *first=NULL,**tail=&first;
    for(;n;n=n->next) {
        Pattern *b=copy_node(c->p,n,sizeof *b,n->token); b->type=replace(c,n->type,n->token);
        b->children=clone_pattern(c,n->children); *tail=b; tail=&b->next;
    }
    *tail=NULL; return first;
}
static Arm *clone_arms(Clone *c,const Arm *a) {
    Arm *first=NULL,**tail=&first;
    for(;a;a=a->next) {
        Arm *b=copy_node(c->p,a,sizeof *b,a->token); *tail=b; tail=&b->next;
        b->pattern=clone_pattern(c,a->pattern); vx_pattern_layout(c->p,b,b->pattern,0);
        for(int i=0;i<b->count;++i) b->bindings[i].type=replace(c,b->bindings[i].type,a->token);
        b->guard=clone_expression(c,a->guard); b->expr=clone_expression(c,a->expr); b->body=clone_statements(c,a->body);
    }
    *tail=NULL; return first;
}
void vx_specialize_call(VxnProgram *p,Expr *e) {
    if(e->kind==EX_GENERIC_CALL) {
        e->id=specialize_function(p,e->id,e->type_args,e->token); e->kind=EX_CALL; e->type_arg_count=0;
    }
}
static Expr *clone_expression(Clone *c,const Expr *e) {
    if(!e) return NULL;
    Expr *n=copy_node(c->p,e,sizeof *n,e->token);
    n->type=replace(c,e->type,e->token); n->operand_type=replace(c,e->operand_type,e->token);
    n->call_contract=replace(c,e->call_contract,e->token);
    n->left=clone_expression(c,e->left); n->right=clone_expression(c,e->right); n->arms=clone_arms(c,e->arms);
    Arg **tail=&n->args;
    for(const Arg *a=e->args;a;a=a->next) {
        Arg *b=copy_node(c->p,a,sizeof *b,e->token); b->expr=clone_expression(c,a->expr); *tail=b; tail=&b->next;
    }
    *tail=NULL;
    for(int i=0;i<e->type_arg_count;++i) n->type_args[i]=replace(c,e->type_args[i],e->token);
    if(e->kind==EX_OPERATOR) {
        int constraint=(int)e->value;
        n->id=vx_operator_implementation(c->p,n->operand_type,e->op,constraint,e->token);
        n->kind=EX_CALL;
    }
    if(e->kind==EX_METHOD) {
        n->id=vx_method_implementation(c->p,e->op,n->operand_type,e->id,e->token);
        n->kind=EX_CALL; n->type_arg_count=0;
    }
    if(n->kind==EX_INVOKE) {
        if(vx_box_fn(c->p,n->operand_type)) {
            n->kind=EX_BOX_CALL; c->p->types[n->operand_type]->invoked=true;
        } else if(vx_closure(c->p,n->operand_type)) {
            n->kind=EX_CLOSURE_CALL; n->id=c->p->types[n->operand_type]->closure_target;
        } else if(vx_function_item(c->p,n->operand_type)) {
            n->kind=EX_CALL; n->id=c->p->types[n->operand_type]->closure_target;
        } else c->p->types[n->operand_type]->invoked=true;
    }
    if(n->kind==EX_CLOSURE_CALL) n->id=c->p->types[n->operand_type]->closure_target;
    else if(n->kind==EX_CLOSURE) n->id=c->p->types[n->type]->closure_target;
    else if(n->kind==EX_CALL&&vx_function_item(c->p,n->operand_type)) n->id=c->p->types[n->operand_type]->closure_target;
    else if(n->kind==EX_FUNCTION||n->kind==EX_CALL) n->id=specialized_target(c,n->id,n->token);
    if(n->kind==EX_BOX_FN) n->id=vx_box_fn(c->p,n->operand_type)?-1:vx_box_fn_payload(c->p,n->operand_type,n->token);
    if(n->kind==EX_VIEW_CALL||n->kind==EX_BOX_CALL) c->p->types[n->operand_type]->invoked=true;
    if(n->kind==EX_FN_VIEW) n->id=(vx_closure(c->p,n->operand_type)||vx_function_item(c->p,n->operand_type))?c->p->types[n->operand_type]->closure_target:-1;
    vx_specialize_call(c->p,n); return n;
}
static Stmt *clone_statements(Clone *c,const Stmt *s) {
    Stmt *first=NULL,**tail=&first;
    for(;s;s=s->next) {
        Stmt *n=copy_node(c->p,s,sizeof *n,s->token); *tail=n; tail=&n->next;
        n->type=replace(c,s->type,s->token);
        n->expr=clone_expression(c,s->expr); n->target=clone_expression(c,s->target);
        n->body=clone_statements(c,s->body); n->otherwise=clone_statements(c,s->otherwise);
        n->initial=clone_statements(c,s->initial); n->step=clone_statements(c,s->step); n->arms=clone_arms(c,s->arms);
    }
    *tail=NULL; return first;
}
static int specialize_function(VxnProgram *p,int id,const Type *types,Token at) {
    Function *source=p->functions[id];
    vx_check_function(p,source);
    for(int i=0;i<source->generic_count;++i) if(vx_abstract(p,types[i])) vx_fail(p,at,"internal error: unresolved specialization type");
    for(int i=0;i<p->function_count;++i) {
        Function *f=p->functions[i];
        if(f->instance && f->instance_of==id && !memcmp(f->instance_types,types,(size_t)source->generic_count*sizeof(Type))) return i;
    }
    if(++p->specialize_depth>VX_MAX_DEPTH) vx_fail(p,at,"specialization dependency depth limit exceeded");
    if(p->function_count==VX_MAX_FUNCTIONS) vx_fail(p,at,"specialization exceeds function/variant budget (256)");
    Function *f=vx_alloc(p,sizeof *f); *f=*source;
    int result=p->function_count++; p->functions[result]=f;
    memcpy(f->instance_types,types,(size_t)source->generic_count*sizeof(Type));
    f->generic_count=0; f->instance=true; f->instance_of=id; f->public_api=false; f->borrow_state=NULL;
    f->closure_type=f->item_type=0; f->capture_context=NULL; f->stage_state=0;
    Clone c={p,source,f->instance_types};
    f->result=replace(&c,f->result,at);
    for(int i=0;i<f->param_count;++i) f->params[i].type=replace(&c,f->params[i].type,at);
    for(int i=0;i<f->local_count;++i) f->local_types[i]=replace(&c,f->local_types[i],at);
    for(int i=0;i<f->capture_count;++i) f->captures[i].type=replace(&c,f->captures[i].type,at);
    if(f->anonymous) {
        if(f->capture_count) (void)vx_closure_type(p,result,source->name);
        else if(source->item_type) (void)vx_function_item_type(p,result,source->name);
    }
    f->body=clone_statements(&c,source->body); vx_frame_layout(p,f);
    --p->specialize_depth; return result;
}
static void specialize_expressions(VxnProgram *p,Expr *e);
static void specialize_statements(VxnProgram *p,Stmt *s);
static void specialize_arms(VxnProgram *p,Arm *a) {
    for(;a;a=a->next) { specialize_expressions(p,a->guard); specialize_expressions(p,a->expr); specialize_statements(p,a->body); }
}
static void specialize_expressions(VxnProgram *p,Expr *e) {
    if(!e) return;
    specialize_expressions(p,e->left); specialize_expressions(p,e->right); specialize_arms(p,e->arms);
    for(Arg *a=e->args;a;a=a->next) specialize_expressions(p,a->expr);
    vx_specialize_call(p,e);
}
static void specialize_statements(VxnProgram *p,Stmt *s) {
    for(;s;s=s->next) {
        specialize_expressions(p,s->expr); specialize_expressions(p,s->target);
        specialize_statements(p,s->body); specialize_statements(p,s->otherwise);
        specialize_statements(p,s->initial); specialize_statements(p,s->step); specialize_arms(p,s->arms);
    }
}
void vx_specialize(VxnProgram *p) {
    int count=p->function_count;
    for(int i=0;i<count;++i) if(!p->functions[i]->generic_count) specialize_statements(p,p->functions[i]->body);
}
