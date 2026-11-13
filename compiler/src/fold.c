#include "internal.h"
bool vx_constant(const Expr *e,Type *type,uint64_t *value) {
    if(!e||vx_extended(e->type)||vx_extended(e->operand_type)) return false;
    *type=e->type;
    if(e->kind==EX_WORD||e->kind==EX_BOOL||e->kind==EX_REAL) { *value=e->value; return true; }
    Type t; uint64_t a,b=0;
    if(e->kind==EX_CAST && vx_constant(e->left,&t,&a)) {
        *value=vx_convert(a,vx_width(t),vx_float(t)?2:vx_signed_type(t),vx_width(e->type),vx_float(e->type)?2:vx_signed_type(e->type));
        return true;
    }
    if(e->kind!=EX_UNARY&&e->kind!=EX_BINARY) return false;
    if(!vx_constant(e->left,&t,&a)) return false;
    if(e->op==VX_LAND&&!a) { *value=0; return true; }
    if(e->op==VX_LOR&&a) { *value=1; return true; }
    if(e->right&&!vx_constant(e->right,&t,&b)) return false;
    if(vx_float(e->operand_type)) return false;
    bool ok=true; Type operand=e->operand_type;
    *value=vx_integer_op(e->op,a,b,operand==TY_BOOL?1:vx_width(operand),vx_signed_type(operand),vx_integer(operand)?vx_mode(operand):0,&ok);
    return ok;
}
