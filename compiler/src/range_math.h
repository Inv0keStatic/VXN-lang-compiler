#ifndef VXN_RANGE_MATH_H
#define VXN_RANGE_MATH_H
/* Inclusive numeric intervals. Sign-bit-biased ranks avoid host signed overflow. */
typedef struct { uint64_t lo,hi; Type type; bool nonzero; } Interval;
static uint64_t range_zero(Type t) { return vx_signed_type(t)?UINT64_C(1)<<(vx_width(t)-1):0; }
static uint64_t range_rank(Type t,uint64_t bits) { return (bits&vx_mask(vx_width(t)))^range_zero(t); }
static uint64_t range_bits(Interval range,uint64_t rank) { return rank^range_zero(range.type); }
static Interval range_full(Type t) {
    if(!vx_integer(t)) return (Interval){0};
    return (Interval){.hi=vx_mask(vx_width(t)),.type=t};
}
static Interval range_finish(Interval r) {
    if(r.type&&(r.hi<range_zero(r.type)||r.lo>range_zero(r.type))) r.nonzero=true;
    return r;
}
static Interval range_single(Type t,uint64_t bits) {
    if(!vx_integer(t)) return (Interval){0};
    uint64_t rank=range_rank(t,bits);
    return range_finish((Interval){rank,rank,t,false});
}
static bool range_contains(Interval r,uint64_t bits) {
    uint64_t rank=range_rank(r.type,bits);
    return r.lo<=rank&&rank<=r.hi&&(!r.nonzero||bits!=0);
}
static Interval range_calculate(int op,Type type,Interval left,Interval right,bool *safe) {
    Interval result=range_full(type); *safe=false;
    if(!result.type||left.type!=type) return result;
    unsigned width=vx_width(type),mode=vx_mode(type); bool sign=vx_signed_type(type);
    if(op==VX_POS) { *safe=true; return left; }
    if(op==VX_SHL||op==VX_SHR) {
        if(right.type&&right.lo>=range_zero(right.type)&&range_bits(right,right.hi)<width) *safe=true;
        return result;
    }
    if(op==VX_DIV||op==VX_MOD) {
        if(right.type!=type||range_contains(right,0)) return result;
        *safe=op==VX_MOD||mode!=1||!sign||
            !range_contains(left,UINT64_C(1)<<(width-1))||!range_contains(right,vx_mask(width));
        return result;
    }
    if(op==VX_AND&&!sign&&right.type==type) {
        *safe=true;
        if(left.lo==left.hi) result.hi=range_bits(left,left.hi);
        if(right.lo==right.hi&&range_bits(right,right.hi)<result.hi) result.hi=range_bits(right,right.hi);
        return result;
    }
    if(op!=VX_ADD&&op!=VX_SUB&&op!=VX_MUL&&op!=VX_NEG) return result;
    if(op!=VX_NEG&&right.type!=type) return result;
    uint64_t a[2]={range_bits(left,left.lo),range_bits(left,left.hi)};
    uint64_t b[2]={0,0}; if(op!=VX_NEG) { b[0]=range_bits(right,right.lo); b[1]=range_bits(right,right.hi); }
    Interval bounded={.lo=vx_mask(width),.type=type};
    bool all=true;
    for(int i=0;i<2;++i) for(int j=0;j<(op==VX_NEG?1:2);++j) {
        bool ok=true; uint64_t value=vx_integer_op(op,a[i],b[j],width,sign,1,&ok);
        all&=ok;
        uint64_t rank=range_rank(type,value);
        if(rank<bounded.lo) bounded.lo=rank;
        if(rank>bounded.hi) bounded.hi=rank;
    }
    *safe=all||mode!=1;
    return all?range_finish(bounded):result;
}
#endif
