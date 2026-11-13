#include "internal.h"
#include <inttypes.h>
#include <assert.h>

typedef struct {
    const VxnProgram *p;
    const Function *fn;
    FILE *out, *entry;
    int next, label, loops, breaks[VX_MAX_DEPTH], continues[VX_MAX_DEPTH];
    bool dead, optimize;
    const Stmt *loop_stmts[VX_MAX_DEPTH];
} Ir;

static int expr(Ir *g,const Expr *e);
static void leave(Ir *g);
static void block(Ir *g,const Stmt *s);
static int constant(Ir *g,uint64_t value) {
    int id=g->next++;
    fprintf(g->out,"  %%t%d = add i64 0, %" PRIu64 "\n",id,value);
    return id;
}
static int allocation(Ir *g,int cells) {
    int id=g->next++;
    fprintf(g->entry,"  %%t%d = alloca [%d x i64], align 8\n",id,cells?cells:1);
    return id;
}
static void zero(Ir *g,int ptr,int cells) {
    fprintf(g->out,"  call void @llvm.memset.p0.i64(ptr %%t%d, i8 0, i64 %d, i1 false)\n",ptr,cells*8);
}
static int offset(Ir *g,int ptr,int cells) {
    int id=g->next++;
    fprintf(g->out,"  %%t%d = getelementptr i64, ptr %%t%d, i64 %d\n",id,ptr,cells);
    return id;
}
static int local(Ir *g,int id) {
    int ptr=g->next++;
    int capture=vx_capture(g->fn,id);
    fprintf(g->out,"  %%t%d = getelementptr i64, ptr %s, i64 %d\n",ptr,capture>=0?"%environment":"%locals",capture>=0?g->fn->captures[capture].offset:g->fn->offsets[id]);
    return ptr;
}
static void copy(Ir *g,int dst,int src,int cells) {
    fprintf(g->out,"  call void @llvm.memcpy.p0.p0.i64(ptr %%t%d, ptr %%t%d, i64 %d, i1 false)\n",dst,src,cells*8);
}
static void store_value(Ir *g,Type type,int ptr,int value) {
    if(vx_indirect(type)) copy(g,ptr,value,vx_slots(g->p,type));
    else fprintf(g->out,"  store i64 %%t%d, ptr %%t%d, align 8\n",value,ptr);
}
static int load_value(Ir *g,Type type,int ptr) {
    int id;
    if(vx_indirect(type)) {
        id=allocation(g,vx_slots(g->p,type)); copy(g,id,ptr,vx_slots(g->p,type));
    } else {
        id=g->next++; fprintf(g->out,"  %%t%d = load i64, ptr %%t%d, align 8\n",id,ptr);
    }
    return id;
}
static void adopt(Ir *g,Type t,int value,int parent,int slot) {
    if(vx_owned(g->p,t)) fprintf(g->out,"  call void @vxn_rt_adopt(ptr %%t%d, ptr @vxn_owner_%d, i64 %d, i64 %d)\n",value,t,parent,slot);
}
static void drop(Ir *g,int slot) { if(!g->dead) fprintf(g->out,"  call void @vxn_rt_drop(i64 %d)\n",slot); }
static void region(Ir *g,int id) {
    Type t=g->fn->local_types[id]; if(!vx_owned(g->p,t)) return;
    int record=allocation(g,4),storage=local(g,id);
    fprintf(g->out,"  call void @vxn_rt_region_bind(ptr %%t%d, ptr %%t%d, i64 %d, i64 %d)\n",record,storage,vx_slots(g->p,t),id+1);
}
static void end_local(Ir *g,int id) {
    drop(g,id+1); if(!g->dead) fprintf(g->out,"  call void @vxn_rt_region_end(i64 %d)\n",id+1);
}
static void end_scope(Ir *g,size_t end,size_t begin) {
    if(!end||g->dead) return;
    for(int i=g->fn->local_count-1;i>=0;--i) if(g->fn->local_ends[i]==end&&g->fn->local_names[i].start>=begin&&vx_owned(g->p,g->fn->local_types[i])) end_local(g,i);
}
static void loop_cleanup(Ir *g) {
    const Stmt *s=g->loop_stmts[g->loops-1]; size_t begin=s->body?s->body->token.start:s->end;
    for(int i=g->fn->local_count-1;i>=0;--i)
        if(g->fn->local_names[i].start>=begin&&g->fn->local_ends[i]<=s->end&&vx_owned(g->p,g->fn->local_types[i])) end_local(g,i);
    drop(g,0);
}
static void label(Ir *g,int id) { fprintf(g->out,"L%d:\n",id); g->dead=false; }
static void jump(Ir *g,int id) {
    if(!g->dead) fprintf(g->out,"  br label %%L%d\n",id);
    g->dead=true;
}
static void branch(Ir *g,int condition,int yes,int no) {
    int id=g->next++;
    fprintf(g->out,"  %%t%d = icmp ne i64 %%t%d, 0\n  br i1 %%t%d, label %%L%d, label %%L%d\n",id,condition,id,yes,no);
    g->dead=true;
}
static int integer_width(Ir *g,int a,unsigned width) {
    if(width==64) return a;
    int id=g->next++;
    fprintf(g->out,"  %%t%d = trunc i64 %%t%d to i%u\n",id,a,width);
    return id;
}
static int widen(Ir *g,int a,unsigned width) {
    if(width==64) return a;
    int id=g->next++;
    fprintf(g->out,"  %%t%d = zext i%u %%t%d to i64\n",id,width,a);
    return id;
}
static int float_value(Ir *g,int a,unsigned width) {
    a=integer_width(g,a,width);
    int id=g->next++;
    fprintf(g->out,"  %%t%d = bitcast i%u %%t%d to %s\n",id,width,a,width==32?"float":"double");
    return id;
}
static int calc(Ir *g,int op,Type operand,int a,int b) {
    if(vx_extended(operand)) {
        int result=allocation(g,2);
        fprintf(g->out,"  call void @vxn_ext_op(i64 %u, i64 %d, ptr %%t%d, ptr %%t%d, ptr %%t%d)\n",vx_width(operand),op,a,b<0?a:b,result);
        return op>=VX_EQ&&op<=VX_GE?load_value(g,TY_BOOL,result):result;
    }
    unsigned width=operand==TY_BOOL?1:vx_width(operand);
    bool sign=vx_signed_type(operand);
    unsigned mode=vx_integer(operand)?vx_mode(operand):0;
    if(b<0) b=constant(g,0);
    if(!vx_float(operand)) {
        const char *instruction=NULL;
        if(!mode) {
            if(op==VX_ADD) instruction="add";
            if(op==VX_SUB||op==VX_NEG) instruction="sub";
            if(op==VX_MUL) instruction="mul";
        }
        if(op==VX_AND) instruction="and";
        if(op==VX_OR) instruction="or";
        if(op==VX_XOR||op==VX_INV||op==VX_NOT) instruction="xor";
        if(op==VX_POS) return a;
        if(instruction|| (op>=VX_EQ&&op<=VX_GE)) {
            if(op==VX_NEG) { b=a; a=constant(g,0); }
            if(op==VX_INV||op==VX_NOT) b=constant(g,vx_mask(width));
            a=integer_width(g,a,width); b=integer_width(g,b,width);
            int id=g->next++;
            if(instruction) {
                fprintf(g->out,"  %%t%d = %s i%u %%t%d, %%t%d\n",id,instruction,width,a,b);
                return widen(g,id,width);
            }
            const char *pred=op==VX_EQ?"eq":op==VX_NE?"ne":
                op==VX_LT?(sign?"slt":"ult"):op==VX_LE?(sign?"sle":"ule"):
                op==VX_GT?(sign?"sgt":"ugt"):(sign?"sge":"uge");
            fprintf(g->out,"  %%t%d = icmp %s i%u %%t%d, %%t%d\n",id,pred,width,a,b);
            return widen(g,id,1);
        }
    } else if(op!=VX_DIV) {
        a=float_value(g,a,width); b=float_value(g,b,width);
        int id=g->next++;
        const char *ft=width==32?"float":"double";
        if(op>=VX_EQ&&op<=VX_GE) {
            const char *pred=op==VX_EQ?"oeq":op==VX_NE?"une":op==VX_LT?"olt":
                op==VX_LE?"ole":op==VX_GT?"ogt":"oge";
            fprintf(g->out,"  %%t%d = fcmp %s %s %%t%d, %%t%d\n",id,pred,ft,a,b);
            return widen(g,id,1);
        }
        if(op==VX_POS) id=a;
        else if(op==VX_NEG) fprintf(g->out,"  %%t%d = fneg %s %%t%d\n",id,ft,a);
        else fprintf(g->out,"  %%t%d = %s %s %%t%d, %%t%d\n",id,op==VX_ADD?"fadd":op==VX_SUB?"fsub":"fmul",ft,a,b);
        int bits=g->next++;
        fprintf(g->out,"  %%t%d = bitcast %s %%t%d to i%u\n",bits,ft,id,width);
        return widen(g,bits,width);
    }
    int id=g->next++;
    if(vx_float(operand)) fprintf(g->out,"  %%t%d = call i64 @vxn_rt_float(i64 %d, i64 %%t%d, i64 %%t%d, i64 %u)\n",id,op,a,b,width);
    else fprintf(g->out,"  %%t%d = call i64 @vxn_rt_calc(i64 %d, i64 %%t%d, i64 %%t%d, i64 %u, i64 %d, i64 %u)\n",id,op,a,b,width,sign,mode);
    return id;
}
static int index_pointer(Ir *g,int ptr,const Expr *index,const TypeDef *d) {
    int value=expr(g,index),bound=g->next++,cells=g->next++,id=g->next++;
    fprintf(g->out,"  %%t%d = call i64 @vxn_rt_bound(i64 %%t%d, i64 %d, i64 %u, i64 %d)\n",
        bound,value,d->count,vx_width(index->type),vx_signed_type(index->type));
    fprintf(g->out,"  %%t%d = mul i64 %%t%d, %d\n  %%t%d = getelementptr i64, ptr %%t%d, i64 %%t%d\n",
        cells,bound,vx_slots(g->p,d->element),id,ptr,cells);
    return id;
}
static int slice_index(Ir *g,const Expr *e) {
    int view=expr(g,e->left),address=load_value(g,TY_WORD,view),length=load_value(g,TY_WORD,offset(g,view,1));
    int index=expr(g,e->right),bound=g->next++,ptr=g->next++,cells=g->next++,result=g->next++;
    fprintf(g->out,"  %%t%d = call i64 @vxn_rt_bound(i64 %%t%d, i64 %%t%d, i64 %u, i64 %d)\n",
        bound,index,length,vx_width(e->right->type),vx_signed_type(e->right->type));
    fprintf(g->out,"  %%t%d = inttoptr i64 %%t%d to ptr\n  %%t%d = mul i64 %%t%d, %d\n  %%t%d = getelementptr i64, ptr %%t%d, i64 %%t%d\n",
        ptr,address,cells,bound,vx_slots(g->p,e->type),result,ptr,cells);
    return result;
}
static int place(Ir *g,const Expr *e) {
    if(e->kind==EX_VAR) return local(g,e->id);
    if(e->kind==EX_DEREF) {
        int reference=expr(g,e->left),ptr=g->next++;
        fprintf(g->out,"  %%t%d = inttoptr i64 %%t%d to ptr\n",ptr,reference); return ptr;
    }
    if(e->kind==EX_INDEX&&vx_slice(g->p,e->left->type)) return slice_index(g,e);
    int base=place(g,e->left); const TypeDef *d=g->p->types[e->left->type];
    return e->kind==EX_FIELD?offset(g,base,d->fields[e->id].offset):index_pointer(g,base,e->right,d);
}
static void pattern_test(Ir *g,const Arm *a,const Pattern *p,int source,int failure) {
    if(p->kind==PAT_OR) {
        int done=g->label++;
        for(const Pattern *child=p->children;child;child=child->next) {
            int next=child->next?g->label++:failure;
            pattern_test(g,a,child,source,next); jump(g,done);
            if(child->next) label(g,next);
        }
        label(g,done); return;
    }
    if(p->kind==PAT_BIND) {
        int value=load_value(g,p->type,offset(g,source,p->offset));
        store_value(g,p->type,local(g,a->locals[p->binding]),value);
    }
    if(p->kind==PAT_RANGE) {
        int value=load_value(g,p->type,offset(g,source,p->offset)),low=constant(g,p->value),high=constant(g,p->upper);
        int comparison=calc(g,VX_GE,p->type,value,low),success=g->label++;
        branch(g,comparison,success,failure); label(g,success);
        comparison=calc(g,VX_LE,p->type,value,high); success=g->label++;
        branch(g,comparison,success,failure); label(g,success);
    }
    if(p->kind==PAT_TEXT) {
        int args=allocation(g,4),result=allocation(g,1),literal=expr(g,p->literal);
        copy(g,args,offset(g,source,p->offset),2); copy(g,offset(g,args,2),literal,2);
        fprintf(g->out,"  call void @vxn_rt_text(i64 %d, ptr %%t%d, ptr %%t%d)\n",VX_STR_EQUAL,args,result);
        int comparison=load_value(g,TY_BOOL,result),success=g->label++;
        branch(g,comparison,success,failure); label(g,success);
    }
    if(p->kind==PAT_LITERAL||p->kind==PAT_CONSTRUCTOR) {
        int value=load_value(g,TY_WORD,offset(g,source,p->offset));
        int expected=constant(g,p->kind==PAT_LITERAL?p->value:(uint64_t)p->variant);
        int comparison=calc(g,VX_EQ,TY_INT_BASE+3,value,expected),success=g->label++;
        branch(g,comparison,success,failure); label(g,success);
    }
    for(const Pattern *child=p->children;child;child=child->next) pattern_test(g,a,child,source,failure);
}
static int match(Ir *g,const Expr *scrutinee,const Arm *arms,Type result) {
    int source=expr(g,scrutinee);
    if(!vx_indirect(scrutinee->type)) { int scalar=source; source=allocation(g,1); store_value(g,scrutinee->type,source,scalar); }
    int destination=result==TY_VOID?-1:allocation(g,vx_slots(g->p,result)),done=g->label++;
    for(const Arm *a=arms;a;a=a->next) {
        int following=g->label++; pattern_test(g,a,a->pattern,source,following);
        if(a->guard) {
            int guard=expr(g,a->guard),selected=g->label++;
            branch(g,guard,selected,following); label(g,selected);
        }
        for(int i=0;i<a->count;++i) {
            Type t=a->bindings[i].type;
            if(vx_owned(g->p,t)) adopt(g,t,local(g,a->locals[i]),0,a->locals[i]+1);
            region(g,a->locals[i]);
        }
        if(a->expr) {
            int value=expr(g,a->expr);
            if(destination>=0) store_value(g,result,destination,value);
        } else block(g,a->body);
        end_scope(g,a->end,a->token.start); jump(g,done); label(g,following);
    }
    fputs("  call void @vxn_rt_host_error()\n  unreachable\n",g->out); g->dead=true;
    label(g,done); return destination<0?-1:load_value(g,result,destination);
}
static int expr(Ir *g,const Expr *e) {
    Type type; uint64_t value;
    if(g->optimize&&vx_constant(e,&type,&value)) return constant(g,value);
    switch(e->kind) {
    case EX_VEC:case EX_STRING:case EX_SLICE_SPLIT:case EX_MEMORY: {
        int values[3]={0},count=0;
        for(const Arg *a=e->args;a;a=a->next) { values[a->field]=expr(g,a->expr); count+=vx_slots(g->p,a->expr->type); }
        int args=allocation(g,count?count:1);
        for(const Arg *a=e->args;a;a=a->next) { int ptr=offset(g,args,vx_argument_offset(g->p,e,a->field)); store_value(g,a->expr->type,ptr,values[a->field]); }
        int result=allocation(g,vx_slots(g->p,e->kind==EX_MEMORY?e->operand_type:e->type));
        if(e->kind==EX_MEMORY) {
            fprintf(g->out,"  call void @vxn_rt_memory(i64 %d, ptr %%t%d, i64 %d, ptr ",e->op,args,vx_slots(g->p,e->operand_type));
            if(vx_owned(g->p,e->operand_type)) fprintf(g->out,"@vxn_owner_%d",e->operand_type); else fputs("null",g->out);
            fprintf(g->out,", ptr %%t%d)\n",result);
        } else if(e->kind==EX_SLICE_SPLIT) fprintf(g->out,"  call void @vxn_rt_split(ptr %%t%d, i64 %d, ptr %%t%d)\n",args,vx_slots(g->p,e->operand_type),result);
        else if(e->kind==EX_STRING) fprintf(g->out,"  call void @vxn_rt_text(i64 %d, ptr %%t%d, ptr %%t%d)\n",e->op,args,result);
        else {
            fprintf(g->out,"  call void @vxn_rt_vec(i64 %d, ptr %%t%d, i64 %d, ptr ",e->op,args,vx_slots(g->p,e->operand_type));
            if(vx_owned(g->p,e->operand_type)) fprintf(g->out,"@vxn_owner_%d",e->operand_type); else fputs("null",g->out);
            fprintf(g->out,", ptr %%t%d)\n",result);
        }
        return e->type==TY_VOID?-1:load_value(g,e->type,result);
    }
    case EX_BOX_FN: {
        int value_id=expr(g,e->left);
        if(vx_box_fn(g->p,e->operand_type)) {
            int result=allocation(g,2),tag=constant(g,0),address=load_value(g,TY_WORD,value_id);
            store_value(g,TY_WORD,result,tag); store_value(g,TY_WORD,offset(g,result,1),address); return result;
        }
        int payload=allocation(g,vx_slots(g->p,e->id));
        int target=vx_callable(g->p,e->operand_type)?load_value(g,TY_WORD,value_id):constant(g,(uint64_t)g->p->types[e->operand_type]->closure_target+1);
        store_value(g,TY_WORD,payload,target); store_value(g,e->operand_type,offset(g,payload,1),value_id);
        int address=g->next++,failed=g->next++,tag=g->next++;
        fprintf(g->out,"  %%t%d = call i64 @vxn_rt_box_new(ptr %%t%d, i64 %d, ptr ",address,payload,vx_slots(g->p,e->id));
        if(vx_owned(g->p,e->id)) fprintf(g->out,"@vxn_owner_%d",e->id); else fputs("null",g->out);
        fputs(")\n",g->out);
        fprintf(g->out,"  %%t%d = icmp eq i64 %%t%d, 0\n  %%t%d = zext i1 %%t%d to i64\n",failed,address,tag,failed);
        int result=allocation(g,2); store_value(g,TY_WORD,result,tag); store_value(g,TY_WORD,offset(g,result,1),address); return result;
    }
    case EX_BOX_NEW: {
        int value_id=expr(g,e->args->expr),src=allocation(g,vx_slots(g->p,e->operand_type));
        store_value(g,e->operand_type,src,value_id);
        int address=g->next++,failed=g->next++,tag=g->next++;
        fprintf(g->out,"  %%t%d = call i64 @vxn_rt_box_new(ptr %%t%d, i64 %d, ptr ",address,src,vx_slots(g->p,e->operand_type));
        if(vx_owned(g->p,e->operand_type)) fprintf(g->out,"@vxn_owner_%d",e->operand_type); else fputs("null",g->out);
        fputs(")\n",g->out);
        fprintf(g->out,"  %%t%d = icmp eq i64 %%t%d, 0\n  %%t%d = zext i1 %%t%d to i64\n",failed,address,tag,failed);
        int result=allocation(g,2); store_value(g,TY_WORD,result,tag); store_value(g,TY_WORD,offset(g,result,1),address); return result;
    }
    case EX_BOX_TAKE: {
        int owner=expr(g,e->args->expr),address=load_value(g,TY_WORD,owner),result=allocation(g,vx_slots(g->p,e->type));
        fprintf(g->out,"  call void @vxn_rt_box_into_inner(i64 %%t%d, i64 %d, ptr ",address,vx_slots(g->p,e->type));
        if(vx_owned(g->p,e->type)) fprintf(g->out,"@vxn_owner_%d",e->type); else fputs("null",g->out);
        fprintf(g->out,", ptr %%t%d)\n",result); return load_value(g,e->type,result);
    }
    case EX_BOX_REF: {
        int ref=expr(g,e->args->expr),ptr=g->next++;
        fprintf(g->out,"  %%t%d = inttoptr i64 %%t%d to ptr\n",ptr,ref); return load_value(g,TY_WORD,ptr);
    }
    case EX_HOLE:case EX_GENERIC_CALL:case EX_OPERATOR:case EX_METHOD:assert(!"unlowered generic expression"); return -1;
    case EX_TRY: {
        int input=expr(g,e->left),tag=load_value(g,TY_WORD,input),condition=g->next++;
        int yes=g->label++,no=g->label++;
        fprintf(g->out,"  %%t%d = icmp eq i64 %%t%d, %d\n  br i1 %%t%d, label %%L%d, label %%L%d\n",condition,tag,e->id,condition,yes,no);
        label(g,no); int result=g->next++;
        fprintf(g->out,"  %%t%d = getelementptr i64, ptr %%result, i64 0\n",result);
        zero(g,result,vx_slots(g->p,g->fn->result));
        int error_tag=constant(g,(uint64_t)e->op); store_value(g,TY_WORD,result,error_tag);
        Type error=g->p->types[e->left->type]->variants[1-e->id].fields[0];
        int destination=offset(g,result,1),source=offset(g,input,1); copy(g,destination,source,vx_slots(g->p,error)); adopt(g,g->fn->result,result,1,0); leave(g);
        label(g,yes); return load_value(g,e->type,offset(g,input,1));
    }
    case EX_REAL:if(vx_extended(e->type)) {
        int result=allocation(g,2);
        for(int i=0;i<2;++i) { int value=constant(g,e->cells[i]); store_value(g,TY_WORD,offset(g,result,i),value); }
        return result;
    }
        /* fall through */
    case EX_WORD:case EX_BOOL:return constant(g,e->value);
    case EX_FUNCTION: {
        int result=allocation(g,1),target=constant(g,(uint64_t)e->id+1);
        store_value(g,TY_WORD,result,target); return result;
    }
    case EX_FN_COERCE:return expr(g,e->left);
    case EX_STATIC: {
        int id=allocation(g,vx_slots(g->p,e->type));
        for(int i=0;i<vx_slots(g->p,e->type);++i) { int value=constant(g,e->cells[i]); store_value(g,TY_WORD,offset(g,id,i),value); }
        return id;
    }
    case EX_REF: {
        int ptr=place(g,e->left),id=g->next++;
        fprintf(g->out,"  %%t%d = ptrtoint ptr %%t%d to i64\n",id,ptr); return id;
    }
    case EX_DEREF: {
        int ptr=place(g,e); return load_value(g,e->type,ptr);
    }
    case EX_VAR: { int ptr=local(g,e->id),value_id=load_value(g,e->type,ptr); if(e->move) { adopt(g,e->type,value_id,0,0); if(vx_owned(g->p,e->type)) zero(g,ptr,vx_slots(g->p,e->type)); } return value_id; }
    case EX_UNARY: { int a=expr(g,e->left); return calc(g,e->op,e->operand_type,a,-1); }
    case EX_BINARY: {
        int a=expr(g,e->left);
        if(e->op==VX_LAND||e->op==VX_LOR) {
            int dst=allocation(g,1),rhs=g->label++,done=g->label++;
            store_value(g,TY_BOOL,dst,a);
            branch(g,a,e->op==VX_LAND?rhs:done,e->op==VX_LAND?done:rhs);
            label(g,rhs); int b=expr(g,e->right); store_value(g,TY_BOOL,dst,b); jump(g,done);
            label(g,done); return load_value(g,TY_BOOL,dst);
        }
        int b=expr(g,e->right); return calc(g,e->op,e->operand_type,a,b);
    }
    case EX_CALL:case EX_LIBRARY:case EX_INVOKE:case EX_CLOSURE_CALL:case EX_VIEW_CALL:case EX_BOX_CALL: {
        int target=-1,environment=-1,temporary_region=-1,boxed=-1;
        if(e->kind==EX_BOX_CALL) {
            boxed=e->op!=2&&vx_stable_place(g->p,e->left)?place(g,e->left):expr(g,e->left);
            int address=load_value(g,TY_WORD,boxed),payload=g->next++;
            fprintf(g->out,"  %%t%d = inttoptr i64 %%t%d to ptr\n",payload,address);
            target=load_value(g,TY_WORD,payload); environment=offset(g,payload,1);
        } else if(e->kind==EX_VIEW_CALL) {
            int view=expr(g,e->left); target=load_value(g,TY_WORD,view);
            int address=load_value(g,TY_WORD,offset(g,view,1)); environment=g->next++;
            fprintf(g->out,"  %%t%d = inttoptr i64 %%t%d to ptr\n",environment,address);
        }
        else if(e->kind==EX_INVOKE) { int value_id=expr(g,e->left); target=load_value(g,TY_WORD,value_id); }
        else if(e->kind==EX_CLOSURE_CALL) environment=e->op!=2&&vx_stable_place(g->p,e->left)?place(g,e->left):expr(g,e->left);
        else if(e->left) (void)expr(g,e->left);
        if(e->kind==EX_CLOSURE_CALL&&vx_owned(g->p,e->operand_type)&&(e->op==2||!vx_stable_place(g->p,e->left))) {
            temporary_region=allocation(g,4);
            fprintf(g->out,"  call void @vxn_rt_region_bind(ptr %%t%d, ptr %%t%d, i64 %d, i64 0)\n",temporary_region,environment,vx_slots(g->p,e->operand_type));
        }
        int values[VX_MAX_PARAMS],n=0,cells=0;
        for(const Arg *a=e->args;a;a=a->next) { values[n++]=expr(g,a->expr); cells+=vx_slots(g->p,a->expr->type); }
        int args=allocation(g,cells); n=0;
        for(const Arg *a=e->args;a;a=a->next) {
            int ptr=offset(g,args,vx_argument_offset(g->p,e,a->field)); store_value(g,a->expr->type,ptr,values[n++]);
        }
        int dst=allocation(g,e->type==TY_VOID?1:vx_slots(g->p,e->type));
        if(e->kind==EX_LIBRARY)
            fprintf(g->out,"  call void @vxn_rt_library(i64 %d, ptr %%t%d, i64 %" PRIu64 ", ptr %%t%d)\n",e->op,args,e->value,dst);
        else if(e->kind==EX_VIEW_CALL||e->kind==EX_BOX_CALL) fprintf(g->out,"  call void @vxn_dispatch_%d(i64 %%t%d, ptr %%t%d, ptr %%t%d, ptr %%t%d)\n",e->operand_type,target,dst,args,environment);
        else if(e->kind==EX_INVOKE) fprintf(g->out,"  call void @vxn_dispatch_%d(i64 %%t%d, ptr %%t%d, ptr %%t%d)\n",e->operand_type,target,dst,args);
        else if(e->kind==EX_CLOSURE_CALL&&g->p->functions[e->id]->capture_count) fprintf(g->out,"  call void @vxn_fn_%d(ptr %%t%d, ptr %%t%d, ptr %%t%d)\n",e->id,dst,args,environment);
        else fprintf(g->out,"  call void @vxn_fn_%d(ptr %%t%d, ptr %%t%d)\n",e->id,dst,args);
        if(temporary_region>=0) fprintf(g->out,"  call void @vxn_rt_region_pop(ptr %%t%d)\n",temporary_region);
        if(e->kind==EX_BOX_CALL&&(e->op==2||!vx_stable_place(g->p,e->left)))
            fprintf(g->out,"  call void @vxn_rt_destroy_value(ptr %%t%d, ptr @vxn_owner_%d)\n",boxed,e->operand_type);
        return e->type==TY_VOID?-1:load_value(g,e->type,dst);
    }
    case EX_OUTPUT: {
        int a=expr(g,e->args->expr); Type t=e->args->expr->type;
        if(vx_extended(t)) fprintf(g->out,"  call void @vxn_rt_output_extended(ptr %%t%d, i64 %u)\n",a,vx_width(t));
        else fprintf(g->out,"  call void @vxn_rt_output(i64 %%t%d, i64 %u, i64 %d)\n",a,t==TY_BOOL?1:vx_width(t),t==TY_BOOL?3:vx_float(t)?2:vx_signed_type(t));
        return -1;
    }
    case EX_PANIC: {
        fputs("  call void @vxn_rt_panic()\n",g->out);
        if(e->type==TY_VOID) return -1;
        if(!vx_indirect(e->type)) return constant(g,0);
        int id=allocation(g,vx_slots(g->p,e->type)); zero(g,id,vx_slots(g->p,e->type)); return id;
    }
    case EX_LENGTH:(void)expr(g,e->args->expr);return constant(g,e->value);
    case EX_FN_VIEW: {
        int source=expr(g,e->left),id=allocation(g,2),target;
        if(vx_box_fn(g->p,e->operand_type)) {
            int descriptor=g->next++; fprintf(g->out,"  %%t%d = inttoptr i64 %%t%d to ptr\n",descriptor,source);
            int address=load_value(g,TY_WORD,descriptor),payload=g->next++;
            fprintf(g->out,"  %%t%d = inttoptr i64 %%t%d to ptr\n",payload,address);
            target=load_value(g,TY_WORD,payload); store_value(g,TY_WORD,id,target);
            int environment=offset(g,payload,1),encoded=g->next++;
            fprintf(g->out,"  %%t%d = ptrtoint ptr %%t%d to i64\n",encoded,environment);
            store_value(g,TY_WORD,offset(g,id,1),encoded); return id;
        }
        if(e->id>=0) target=constant(g,(uint64_t)e->id+1);
        else {
            int address=g->next++; fprintf(g->out,"  %%t%d = inttoptr i64 %%t%d to ptr\n",address,source);
            target=load_value(g,TY_WORD,address);
        }
        store_value(g,TY_WORD,id,target);
        int environment=e->id>=0&&g->p->functions[e->id]->capture_count?source:constant(g,0);
        store_value(g,TY_WORD,offset(g,id,1),environment); return id;
    }
    case EX_SLICE_BORROW:case EX_VIEW_BORROW: { int view=expr(g,e->left); return load_value(g,e->type,view); }
    case EX_SLICE_LENGTH: {
        int view=expr(g,e->args->expr); return load_value(g,TY_WORD,offset(g,view,1));
    }
    case EX_SLICE: {
        int args[3]={0}; for(const Arg *a=e->args;a;a=a->next) args[a->field]=expr(g,a->expr);
        bool borrowed=vx_ref(e->operand_type); Type base=borrowed?vx_referent(e->operand_type):e->operand_type;
        int address=borrowed?args[0]:load_value(g,TY_WORD,args[0]);
        int length=borrowed?constant(g,(uint64_t)g->p->types[base]->count):load_value(g,TY_WORD,offset(g,args[0],1));
        int start,count=length;
        if(e->argc==3) {
            Type st=vx_argument(e,1)->expr->type,ct=vx_argument(e,2)->expr->type; start=g->next++; count=args[2];
            fprintf(g->out,"  %%t%d = call i64 @vxn_rt_slice_range(i64 %%t%d, i64 %%t%d, i64 %%t%d, i64 %u, i64 %d, i64 %u, i64 %d)\n",
                start,args[1],count,length,vx_width(st),vx_signed_type(st),vx_width(ct),vx_signed_type(ct));
        } else start=constant(g,0);
        int ptr=g->next++,cells=g->next++,shifted=g->next++,bits=g->next++;
        fprintf(g->out,"  %%t%d = inttoptr i64 %%t%d to ptr\n  %%t%d = mul i64 %%t%d, %d\n  %%t%d = getelementptr i64, ptr %%t%d, i64 %%t%d\n  %%t%d = ptrtoint ptr %%t%d to i64\n",
            ptr,address,cells,start,vx_slots(g->p,g->p->types[base]->element),shifted,ptr,cells,bits,shifted);
        int dst=allocation(g,2); store_value(g,TY_WORD,dst,bits); store_value(g,TY_WORD,offset(g,dst,1),count); return dst;
    }
    case EX_CAST: {
        int a=expr(g,e->left),id=g->next++; Type from=e->operand_type,to=e->type;
        if(vx_extended(from)||vx_extended(to)) {
            if(!vx_extended(from)) { int source=allocation(g,1); store_value(g,from,source,a); a=source; }
            int result=allocation(g,vx_slots(g->p,to));
            fprintf(g->out,"  call void @vxn_ext_convert(ptr %%t%d, i64 %u, i64 %d, i64 %u, i64 %d, ptr %%t%d)\n",a,vx_width(from),vx_float(from)?2:vx_signed_type(from),vx_width(to),vx_float(to)?2:vx_signed_type(to),result);
            return load_value(g,to,result);
        }
        fprintf(g->out,"  %%t%d = call i64 @vxn_rt_convert(i64 %%t%d, i64 %u, i64 %d, i64 %u, i64 %d)\n",
            id,a,vx_width(from),vx_float(from)?2:vx_signed_type(from),vx_width(to),vx_float(to)?2:vx_signed_type(to));
        return id;
    }
    case EX_BYTES:case EX_CLOSURE:case EX_RECORD:case EX_ARRAY:case EX_CHOICE: {
        const TypeDef *d=g->p->types[e->type]; int id=allocation(g,d->slots),pos=0,index=0;
        zero(g,id,d->slots);
        if(e->kind==EX_BYTES) {
            for(int i=0;i<e->argc;++i) { int ptr=offset(g,id,i),byte=constant(g,e->bytes[i]); store_value(g,TY_WORD,ptr,byte); }
        } else {
            if(e->kind==EX_CHOICE) { int tag=constant(g,(uint64_t)e->id); store_value(g,TY_WORD,id,tag); pos=1; }
            for(const Arg *a=e->args;a;a=a->next) {
                Type t=(e->kind==EX_RECORD||e->kind==EX_CLOSURE)?d->fields[a->field].type:e->kind==EX_ARRAY?d->element:d->variants[e->id].fields[index++];
                if(e->kind==EX_RECORD||e->kind==EX_CLOSURE) pos=d->fields[a->field].offset;
                int item=expr(g,a->expr),ptr=offset(g,id,pos); store_value(g,t,ptr,item); pos+=vx_slots(g->p,t);
            }
        }
        return id;
    }
    case EX_FIELD:case EX_INDEX: {
        if(e->move) {
            int ptr=place(g,e),id=load_value(g,e->type,ptr); adopt(g,e->type,id,0,0);
            zero(g,ptr,vx_slots(g->p,e->type)); return id;
        }
        if(e->kind==EX_INDEX&&vx_slice(g->p,e->left->type)) return load_value(g,e->type,slice_index(g,e));
        int source=expr(g,e->left); const TypeDef *d=g->p->types[e->left->type];
        int ptr=e->kind==EX_FIELD?offset(g,source,d->fields[e->id].offset):index_pointer(g,source,e->right,d);
        return load_value(g,e->type,ptr);
    }
    case EX_MATCH:return match(g,e->left,e->arms,e->type);
    case EX_TEXT: {
        int result=allocation(g,2),address=g->next++;
        fprintf(g->out,"  %%t%d = ptrtoint ptr @vxn_text_%d to i64\n",address,e->id);
        store_value(g,TY_WORD,result,address);
        int length=constant(g,(uint64_t)e->argc);
        store_value(g,TY_WORD,offset(g,result,1),length); return result;
    }
    case EX_STRING_INTO:return expr(g,e->args->expr);
    case EX_STRING_FROM: {
        int source=expr(g,e->args->expr),address=load_value(g,TY_WORD,source),result=allocation(g,4);
        fprintf(g->out,"  call void @vxn_rt_string_from_utf8(i64 %%t%d, ptr %%t%d)\n",address,result);
        return load_value(g,e->type,result);
    }
    case EX_UTF8: {
        int args[2]={0};
        for(const Arg *a=e->args;a;a=a->next) args[a->field]=expr(g,a->expr);
        const Expr *input=vx_argument(e,0)->expr; int pointer=args[0],length;
        if(vx_shared_slice(g->p,input->type)) {
            int address=load_value(g,TY_WORD,pointer);
            length=load_value(g,TY_WORD,offset(g,pointer,1)); pointer=g->next++;
            fprintf(g->out,"  %%t%d = inttoptr i64 %%t%d to ptr\n",pointer,address);
        } else length=constant(g,(uint64_t)g->p->types[input->type]->count);
        int start=e->op?args[1]:constant(g,0),result=allocation(g,3);
        fprintf(g->out,"  call void @vxn_rt_utf8(i64 %d, ptr %%t%d, i64 %%t%d, i64 %%t%d, ptr %%t%d)\n",e->op,pointer,length,start,result);
        return load_value(g,e->type,result);
    }
    case EX_PARSE_NUMBER: {
        const Expr *input=e->args->expr; int pointer=expr(g,input),length;
        if(vx_shared_slice(g->p,input->type)) {
            int address=load_value(g,TY_WORD,pointer);
            length=load_value(g,TY_WORD,offset(g,pointer,1)); pointer=g->next++;
            fprintf(g->out,"  %%t%d = inttoptr i64 %%t%d to ptr\n",pointer,address);
        } else length=constant(g,(uint64_t)g->p->types[input->type]->count);
        int result=allocation(g,3);
        if(vx_extended(e->operand_type)) fprintf(g->out,"  call void @vxn_ext_parse(ptr %%t%d, i64 %%t%d, i64 %u, ptr %%t%d)\n",pointer,length,vx_width(e->operand_type),result);
        else if(vx_float(e->operand_type)) fprintf(g->out,"  call void @vxn_rt_parse_float(ptr %%t%d, i64 %%t%d, i64 %u, ptr %%t%d)\n",pointer,length,vx_width(e->operand_type),result);
        else fprintf(g->out,"  call void @vxn_rt_parse_int(ptr %%t%d, i64 %%t%d, i64 %u, i64 %d, ptr %%t%d)\n",pointer,length,vx_width(e->operand_type),vx_signed_type(e->operand_type),result);
        return load_value(g,e->type,result);
    }
    case EX_IO: {
        int args[3],n=0;
        for(const Arg *a=e->args;a;a=a->next) { args[a->field]=expr(g,a->expr); ++n; }
        if(e->op==VX_IO_READ_LINE) {
            int dst=allocation(g,4);
            fprintf(g->out,"  call void @vxn_rt_io_line(i64 %%t%d, i64 %%t%d, i64 %%t%d, ptr %%t%d)\n",args[0],args[1],args[2],dst);
            return load_value(g,e->type,dst);
        }
        if(e->op==VX_IO_READ_BUFFER) {
            int address=load_value(g,TY_WORD,args[1]),length=load_value(g,TY_WORD,offset(g,args[1],1)),pointer=g->next++;
            fprintf(g->out,"  %%t%d = inttoptr i64 %%t%d to ptr\n",pointer,address);
            int dst=allocation(g,3);
            fprintf(g->out,"  call void @vxn_rt_io_read_into(i64 %%t%d, ptr %%t%d, i64 %%t%d, ptr %%t%d)\n",args[0],pointer,length,dst);
            return load_value(g,e->type,dst);
        }
        bool path=e->op>=VX_IO_OPEN_READ&&e->op<=VX_IO_OPEN_APPEND;
        int handle=n&&!path?args[0]:constant(g,0),val=n==2&&e->op!=VX_IO_WRITE?args[1]:constant(g,0);
        int pointer=-1,length=constant(g,0);
        if(n==2&&vx_extended(vx_argument(e,1)->expr->type)) { pointer=args[1]; val=constant(g,0); }
        if(path||e->op==VX_IO_WRITE) {
            const Arg *data=vx_argument(e,path?0:1);
            pointer=args[path?0:1];
            if(vx_slice(g->p,data->expr->type)) {
                int address=load_value(g,TY_WORD,pointer);
                length=load_value(g,TY_WORD,offset(g,pointer,1));
                pointer=g->next++;
                fprintf(g->out,"  %%t%d = inttoptr i64 %%t%d to ptr\n",pointer,address);
            } else length=constant(g,(uint64_t)g->p->types[data->expr->type]->count);
        }
        int dst=allocation(g,2);
        fprintf(g->out,"  call void @vxn_rt_io(i64 %d, i64 %%t%d, ptr ",e->op,handle);
        if(pointer>=0) fprintf(g->out,"%%t%d",pointer); else fputs("null",g->out);
        fprintf(g->out,", i64 %%t%d, i64 %%t%d, i64 %u, ptr %%t%d)\n",length,val,(unsigned)e->value,dst);
        return load_value(g,e->type,dst);
    }
    }
    return -1;
}
static void leave(Ir *g) {
    fprintf(g->out,"  call void @vxn_rt_leave(i64 %d)\n  ret %s\n",g->fn->stack_cost,g->fn->tail_member?"i32 -1":"void"); g->dead=true;
}
static void block(Ir *g,const Stmt *s) {
    size_t end=s?s->scope_end:0,begin=s?s->token.start:0;
    for(;s&&!g->dead;s=s->next) { switch(s->kind) {
    case ST_BIND: {
        int value=s->expr?expr(g,s->expr):-1,ptr=local(g,s->local);
        if(value>=0) store_value(g,s->type,ptr,value); else zero(g,ptr,vx_slots(g->p,s->type));
        if(value>=0) adopt(g,s->type,value,0,s->local+1);
        region(g,s->local);
        break;
    }
    case ST_ASSIGN: {
        int ptr=place(g,s->target),prior=s->op>=0?load_value(g,s->type,ptr):-1;
        int value=expr(g,s->expr);
        if(s->op>=0) value=calc(g,s->op,s->type,prior,value);
        if(vx_owned(g->p,s->type)&&(s->target->kind!=EX_VAR||vx_capture(g->fn,s->local)>=0)) {
            fprintf(g->out,"  call void @vxn_rt_replace(ptr %%t%d, ptr %%t%d, i64 %d, ptr @vxn_owner_%d)\n",ptr,value,vx_slots(g->p,s->type),s->type);
            break;
        }
        if(vx_owned(g->p,s->type)) {
            if(s->target->kind==EX_VAR) drop(g,s->local+1);
            else fprintf(g->out,"  call void @vxn_rt_destroy_value(ptr %%t%d, ptr @vxn_owner_%d)\n",ptr,s->type);
        }
        adopt(g,s->type,value,0,s->local+1);
        store_value(g,s->type,ptr,value); break;
    }
    case ST_EXPR:(void)expr(g,s->expr);break;
    case ST_RETURN: {
        if(s->tail) {
            const Expr *e=s->expr; const Function *target=vx_call_contract(g->p,e);
            int args[VX_MAX_PARAMS],pos=0,dynamic=-1,environment=-1;
            bool closure=e->kind==EX_CLOSURE_CALL||e->kind==EX_BOX_CALL;
            if(closure) {
                environment=e->op!=2&&vx_stable_place(g->p,e->left)?place(g,e->left):expr(g,e->left);
                if(e->kind==EX_BOX_CALL) {
                    int address=load_value(g,TY_WORD,environment),payload=g->next++;
                    fprintf(g->out,"  %%t%d = inttoptr i64 %%t%d to ptr\n",payload,address);
                    dynamic=load_value(g,TY_WORD,payload); environment=offset(g,payload,1);
                }
            } else if(e->left) {
                int value=expr(g,e->left);
                if(e->kind==EX_INVOKE||e->kind==EX_VIEW_CALL) dynamic=load_value(g,TY_WORD,value);
                if(e->kind==EX_VIEW_CALL) {
                    int address=load_value(g,TY_WORD,offset(g,value,1)); environment=g->next++;
                    fprintf(g->out,"  %%t%d = inttoptr i64 %%t%d to ptr\n",environment,address);
                }
            }
            for(const Arg *a=e->args;a;a=a->next) args[a->field]=expr(g,a->expr);
            for(int i=0;i<target->param_count;++i) {
                Type t=target->params[i].type; int ptr=g->next++;
                fprintf(g->out,"  %%t%d = getelementptr i64, ptr %%args, i64 %d\n",ptr,pos);
                store_value(g,t,ptr,args[i]); pos+=vx_slots(g->p,t); adopt(g,t,args[i],1,0);
            }
            int target_id=-1;
            if(dynamic>=0) {
                int adjusted=g->next++; target_id=g->next++;
                fprintf(g->out,"  %%t%d = sub i64 %%t%d, 1\n  %%t%d = trunc i64 %%t%d to i32\n",adjusted,dynamic,target_id,adjusted);
            }
            if(environment>=0) fprintf(g->out,"  store ptr %%t%d, ptr %%context, align 8\n",environment);
            else fputs("  store ptr null, ptr %context, align 8\n",g->out);
            int mode=closure?vx_tail_environment_mode(g->p,g->fn,e):0;
            fprintf(g->out,"  store i64 %s, ptr %%ownership, align 8\n",mode==2?"%owned_environment":mode==1?"1":"0");
            if(closure) {
                if(target_id>=0) fprintf(g->out,"  call void @vxn_tail_capture(i32 %%t%d, ptr %%context)\n",target_id);
                else fprintf(g->out,"  call void @vxn_tail_capture(i32 %d, ptr %%context)\n",e->id);
            }
            fprintf(g->out,"  call void @vxn_rt_leave(i64 %d)\n",g->fn->stack_cost);
            if(target_id>=0) fprintf(g->out,"  ret i32 %%t%d\n",target_id);
            else fprintf(g->out,"  ret i32 %d\n",e->id);
            g->dead=true; break;
        }
        int value=s->expr?expr(g,s->expr):-1;
        if(value>=0) {
            int ptr=g->next++;
            fprintf(g->out,"  %%t%d = getelementptr i64, ptr %%result, i64 0\n",ptr);
            store_value(g,g->fn->result,ptr,value);
            adopt(g,g->fn->result,value,1,0);
        }
        leave(g); break;
    }
    case ST_IF: {
        Type t; uint64_t value;
        if(g->optimize&&vx_constant(s->expr,&t,&value)) block(g,value?s->body:s->otherwise);
        else {
            int cond=expr(g,s->expr),yes=g->label++,no=g->label++,done=g->label++;
            drop(g,0); branch(g,cond,yes,no);
            label(g,yes); block(g,s->body); jump(g,done);
            label(g,no); block(g,s->otherwise); jump(g,done);
            label(g,done);
        }
        break;
    }
    case ST_BLOCK:block(g,s->body);break;
    case ST_LOOP: {
        int test=g->label++,body=g->label++,step=g->label++,done=g->label++;
        g->loop_stmts[g->loops]=s; g->breaks[g->loops]=done; g->continues[g->loops++]=step;
        block(g,s->initial); jump(g,test);
        label(g,test); int cond=expr(g,s->expr); drop(g,0); branch(g,cond,body,done);
        label(g,body); block(g,s->body); jump(g,step);
        label(g,step); block(g,s->step); jump(g,test);
        label(g,done); --g->loops; end_scope(g,s->end,s->token.start); break;
    }
    case ST_BREAK:loop_cleanup(g);jump(g,g->breaks[g->loops-1]);break;
    case ST_CONTINUE:loop_cleanup(g);jump(g,g->continues[g->loops-1]);break;
    case ST_MATCH:(void)match(g,s->expr,s->arms,TY_VOID);break;
    }
    drop(g,0);
    }
    end_scope(g,end,begin);
}
static int emit_program(const VxnProgram *p,FILE *out,int optimize,int unit) {
    if(!vxn_ok(p)) return 2;
    fputs("; Vxn 0.54: direct LLVM backend; not formally verified.\n"
        "declare void @llvm.memcpy.p0.p0.i64(ptr, ptr, i64, i1 immarg)\n"
        "declare void @llvm.memset.p0.i64(ptr, i8, i64, i1 immarg)\n"
        "declare void @vxn_rt_init()\n"
        "declare i64 @vxn_rt_finish()\n"
        "declare void @vxn_rt_panic() noreturn\n"
        "declare void @vxn_rt_host_error() noreturn\n"
        "declare void @vxn_rt_enter(i64)\n"
        "declare void @vxn_rt_leave(i64)\n"
        "declare i64 @vxn_rt_calc(i64, i64, i64, i64, i64, i64)\n"
        "declare i64 @vxn_rt_float(i64, i64, i64, i64)\n"
        "declare i64 @vxn_rt_convert(i64, i64, i64, i64, i64)\n"
        "declare i64 @vxn_rt_bound(i64, i64, i64, i64)\n"
        "declare i64 @vxn_rt_slice_range(i64, i64, i64, i64, i64, i64, i64)\n"
        "declare void @vxn_rt_output(i64, i64, i64)\n"
        "declare void @vxn_rt_io(i64, i64, ptr, i64, i64, i64, ptr)\n"
        "declare void @vxn_rt_io_read_into(i64, ptr, i64, ptr)\n"
        "declare void @vxn_rt_io_line(i64, i64, i64, ptr)\n"
        "declare void @vxn_rt_parse_int(ptr, i64, i64, i64, ptr)\n"
        "declare void @vxn_rt_parse_float(ptr, i64, i64, ptr)\n"
        "declare void @vxn_rt_utf8(i64, ptr, i64, i64, ptr)\n"
        "declare void @vxn_rt_string_from_utf8(i64, ptr)\n"
        "declare void @vxn_rt_text(i64, ptr, ptr)\n"
        "declare void @vxn_rt_split(ptr, i64, ptr)\n"
        "declare void @vxn_rt_library(i64, ptr, i64, ptr)\n",out);
    fputs("declare void @vxn_ext_op(i64, i64, ptr, ptr, ptr)\ndeclare void @vxn_ext_convert(ptr, i64, i64, i64, i64, ptr)\ndeclare void @vxn_ext_parse(ptr, i64, i64, ptr)\ndeclare void @vxn_rt_output_extended(ptr, i64)\n",out);
    fputs("declare void @vxn_rt_vec(i64, ptr, i64, ptr, ptr)\n",out);
    fputs("declare void @vxn_rt_box_into_inner(i64, i64, ptr, ptr)\n",out);
    fputs("declare i64 @vxn_rt_box_new(ptr, i64, ptr)\ndeclare void @vxn_rt_adopt(ptr, ptr, i64, i64)\ndeclare void @vxn_rt_drop(i64)\ndeclare void @vxn_rt_destroy_value(ptr, ptr)\n",out);
    fputs("declare void @vxn_rt_region_bind(ptr, ptr, i64, i64)\ndeclare void @vxn_rt_region_end(i64)\ndeclare void @vxn_rt_region_pop(ptr)\ndeclare void @vxn_rt_replace(ptr, ptr, i64, ptr)\ndeclare void @vxn_rt_memory(i64, ptr, i64, ptr, ptr)\n",out);
    for(const TextLiteral *literal=p->text_literals;literal;literal=literal->next) {
        fprintf(out,"@vxn_text_%d = private unnamed_addr constant [%d x i64] [",literal->id,literal->count?literal->count:1);
        if(!literal->count) fputs("i64 0",out);
        for(int i=0;i<literal->count;++i) fprintf(out,"%si64 %u",i?", ":"",(unsigned)literal->cells[i]);
        fputs("], align 8\n",out);
    }
    for(int i=TY_AGGREGATE;i<p->type_count;++i) if(p->types[i]->owner_map) {
        fprintf(out,"@vxn_owner_%d = private constant [%zu x i64] [",i,p->types[i]->owner_map_count);
        for(size_t j=0;j<p->types[i]->owner_map_count;++j) fprintf(out,"%si64 %" PRIu64,j?", ":"",p->types[i]->owner_map[j]);
        fputs("]\n",out);
    }
    if(unit>=0) {
        for(int i=0;i<p->function_count;++i) {
            const Function *f=p->functions[i];
            if(f->comptime||f->generic_count||f->name.module==unit) continue;
            fprintf(out,"declare void @vxn_fn_%d(ptr, ptr%s)\n",i,f->capture_count?", ptr":"");
            if(f->tail_member) fprintf(out,"declare i32 @vxn_step_%d(ptr, ptr, ptr)\n",i);
        }
        if(unit) {
            fputs("declare void @vxn_tail_capture(i32, ptr)\ndeclare void @vxn_tail_run(i32, ptr, ptr, ptr)\n",out);
            for(int t=TY_AGGREGATE;t<p->type_count;++t)
                if((vx_callable(p,t)||vx_fn_view(p,t)||vx_box_fn(p,t))&&p->types[t]->invoked&&!vx_abstract(p,t))
                    fprintf(out,"declare void @vxn_dispatch_%d(i64, ptr, ptr%s)\n",t,vx_fn_view(p,t)||vx_box_fn(p,t)?", ptr":"");
        }
    }
    for(int i=0;i<p->function_count;++i) {
        const Function *f=p->functions[i];
        if(f->comptime||f->generic_count) continue;
        if(unit>=0&&f->name.module!=unit) continue;
        FILE *body=tmpfile(); if(!body) return 2;
        Ir g={.p=p,.fn=f,.out=body,.entry=out,.optimize=optimize!=0};
        if(f->tail_member) {
            fprintf(out,"\ndefine void @vxn_fn_%d(ptr %%result, ptr %%args%s) {\nentry:\n  %%buffer = alloca [%d x i64], align 8\n  %%context = alloca [%d x i64], align 8\n  store ptr %s, ptr %%context, align 8\n  %%ownership = getelementptr i64, ptr %%context, i64 1\n  store i64 0, ptr %%ownership, align 8\n  %%bank = getelementptr i64, ptr %%context, i64 2\n  store i64 0, ptr %%bank, align 8\n",i,f->capture_count?", ptr %environment":"",p->tail_slots?p->tail_slots:1,3+2*p->tail_environment_slots,f->capture_count?"%environment":"null");
            int cells=vx_parameter_slots(p,f);
            if(cells) fprintf(out,"  call void @llvm.memcpy.p0.p0.i64(ptr %%buffer, ptr %%args, i64 %d, i1 false)\n",cells*8);
            fprintf(out,"  call void @vxn_tail_run(i32 %d, ptr %%result, ptr %%buffer, ptr %%context)\n  ret void\n}\n",i);
            fprintf(out,"define i32 @vxn_step_%d(ptr %%result, ptr %%args, ptr %%context) {\nentry:\n  %%environment = load ptr, ptr %%context, align 8\n  %%ownership = getelementptr i64, ptr %%context, i64 1\n  %%owned_environment = load i64, ptr %%ownership, align 8\n",i);
        } else fprintf(out,"\ndefine void @vxn_fn_%d(ptr %%result, ptr %%args%s) {\nentry:\n",i,f->capture_count?", ptr %environment":"");
        fprintf(out,"  %%locals = alloca [%d x i64], align 8\n",f->local_slots?f->local_slots:1);
        fprintf(body,"  call void @vxn_rt_enter(i64 %d)\n",f->stack_cost);
        int pos=0;
        for(int j=0;j<f->param_count;++j) {
            int src=g.next++,dst=local(&g,j),cells=vx_slots(p,f->params[j].type);
            fprintf(body,"  %%t%d = getelementptr i64, ptr %%args, i64 %d\n",src,pos);
            copy(&g,dst,src,cells); pos+=cells;
            adopt(&g,f->params[j].type,dst,0,j+1);
            region(&g,j);
        }
        if(f->call_mode==2) for(int j=0;j<f->capture_count;++j) {
            const Capture *capture=&f->captures[j];
            adopt(&g,capture->type,local(&g,capture->local),0,capture->local+1);
            region(&g,capture->local);
        }
        if(f->tail_member&&f->call_mode!=2&&f->capture_count) {
            fputs("  %adopt_captures = icmp ne i64 %owned_environment, 0\n  br i1 %adopt_captures, label %capture_owners, label %body\ncapture_owners:\n",body);
            for(int j=0;j<f->capture_count;++j) {
                const Capture *capture=&f->captures[j];
                adopt(&g,capture->type,local(&g,capture->local),0,capture->local+1);
                region(&g,capture->local);
            }
            fputs("  br label %body\nbody:\n",body);
        }
        block(&g,f->body);
        if(!g.dead) {
            if(f->result==TY_VOID) leave(&g);
            else fputs("  call void @vxn_rt_host_error()\n  unreachable\n",body);
        }
        if(ferror(body)||fflush(body)||fseek(body,0,SEEK_SET)) { fclose(body); return 2; }
        char buffer[8192]; size_t n;
        while((n=fread(buffer,1,sizeof buffer,body))!=0) fwrite(buffer,1,n,out);
        int failed=ferror(body); if(fclose(body)) failed=1;
        if(failed) return 2;
        fputs("}\n",out);
    }
    if(unit>0) return ferror(out)?2:0;
    bool tails=false;
    for(int i=0;i<p->function_count;++i) tails|=p->functions[i]->tail_member;
    if(tails) {
        fprintf(out,"\ndefine void @vxn_tail_capture(i32 %%target, ptr %%context) {\nentry:\n  %%ownership = getelementptr i64, ptr %%context, i64 1\n  %%owned = load i64, ptr %%ownership, align 8\n  %%move = icmp ne i64 %%owned, 0\n  br i1 %%move, label %%transfer, label %%done\ntransfer:\n  %%source = load ptr, ptr %%context, align 8\n  %%bankptr = getelementptr i64, ptr %%context, i64 2\n  %%bank = load i64, ptr %%bankptr, align 8\n  %%nextbank = xor i64 %%bank, 1\n  store i64 %%nextbank, ptr %%bankptr, align 8\n  %%index = mul i64 %%bank, %d\n  %%offset = add i64 %%index, 3\n  %%storage = getelementptr i64, ptr %%context, i64 %%offset\n  store ptr %%storage, ptr %%context, align 8\n  switch i32 %%target, label %%invalid [\n",p->tail_environment_slots);
        for(int i=0;i<p->function_count;++i) if(p->functions[i]->tail_member) fprintf(out,"    i32 %d, label %%fn_%d\n",i,i);
        fputs("  ]\n",out);
        for(int i=0;i<p->function_count;++i) if(p->functions[i]->tail_member) {
            const Function *f=p->functions[i]; fprintf(out,"fn_%d:\n",i);
            if(f->capture_count) {
                fprintf(out,"  call void @llvm.memcpy.p0.p0.i64(ptr %%storage, ptr %%source, i64 %d, i1 false)\n",8*vx_slots(p,f->closure_type));
                if(vx_owned(p,f->closure_type)) fprintf(out,"  call void @vxn_rt_adopt(ptr %%storage, ptr @vxn_owner_%d, i64 1, i64 0)\n",f->closure_type);
            }
            fputs("  br label %done\n",out);
        }
        fputs("invalid:\n  call void @vxn_rt_host_error()\n  unreachable\ndone:\n  ret void\n}\n",out);
        fputs("\ndefine void @vxn_tail_run(i32 %initial, ptr %result, ptr %args, ptr %context) {\nentry:\n  br label %dispatch\ndispatch:\n  %target = phi i32 [ %initial, %entry ]",out);
        for(int i=0;i<p->function_count;++i) if(p->functions[i]->tail_member) fprintf(out,", [ %%next_%d, %%step_%d ]",i,i);
        fputs("\n  switch i32 %target, label %invalid [\n    i32 -1, label %done\n",out);
        for(int i=0;i<p->function_count;++i) if(p->functions[i]->tail_member) fprintf(out,"    i32 %d, label %%step_%d\n",i,i);
        fputs("  ]\n",out);
        for(int i=0;i<p->function_count;++i) if(p->functions[i]->tail_member)
            fprintf(out,"step_%d:\n  %%next_%d = call i32 @vxn_step_%d(ptr %%result, ptr %%args, ptr %%context)\n  br label %%dispatch\n",i,i,i);
        fputs("invalid:\n  call void @vxn_rt_host_error()\n  unreachable\ndone:\n  ret void\n}\n",out);
    }
    for(int t=TY_AGGREGATE;t<p->type_count;++t) if((vx_callable(p,t)||vx_fn_view(p,t)||vx_box_fn(p,t))&&p->types[t]->invoked&&!vx_abstract(p,t)) {
        const Function *contract=p->types[t]->signature; bool view=vx_fn_view(p,t)||vx_box_fn(p,t);
        fprintf(out,"\ndefine void @vxn_dispatch_%d(i64 %%target, ptr %%result, ptr %%args%s) {\nentry:\n  switch i64 %%target, label %%invalid [\n",t,view?", ptr %environment":"");
        for(int i=0;i<p->function_count;++i) if(!p->functions[i]->generic_count&&(view?vx_callable_contract(p,p->functions[i],contract):vx_callable_target(p,p->functions[i],contract)))
            fprintf(out,"    i64 %d, label %%fn_%d\n",i+1,i);
        fputs("  ]\n",out);
        for(int i=0;i<p->function_count;++i) if(!p->functions[i]->generic_count&&(view?vx_callable_contract(p,p->functions[i],contract):vx_callable_target(p,p->functions[i],contract)))
            fprintf(out,"fn_%d:\n  call void @vxn_fn_%d(ptr %%result, ptr %%args%s)\n  ret void\n",i,i,view&&p->functions[i]->capture_count?", ptr %environment":"");
        fputs("invalid:\n  call void @vxn_rt_host_error()\n  unreachable\n}\n",out);
    }
    if(p->main_id>=0) fprintf(out,"\ndefine i32 @main() {\nentry:\n"
        "  call void @vxn_rt_init()\n"
        "  call void @vxn_fn_%d(ptr null, ptr null)\n"
        "  %%status = call i64 @vxn_rt_finish()\n"
        "  %%code = trunc i64 %%status to i32\n"
        "  ret i32 %%code\n}\n",p->main_id);
    return ferror(out)?2:0;
}
int vxn_emit_llvm(const VxnProgram *p,FILE *out,int optimize) {
    return emit_program(p,out,optimize,-1);
}
int vxn_emit_llvm_unit(const VxnProgram *p,FILE *out,int optimize,int unit) {
    if(!vxn_ok(p)||!out||unit<0||unit>=p->module_count) return 2;
    return emit_program(p,out,optimize,unit);
}
