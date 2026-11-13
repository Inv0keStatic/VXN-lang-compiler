#ifndef VX_SCALAR_RUNTIME_H
#define VX_SCALAR_RUNTIME_H
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <float.h>
#include <math.h>
/* Integers use width-limited two's-complement bit patterns. No overflowing
   signed C arithmetic or out-of-range signed conversions occur. */
enum { VX_ADD, VX_SUB, VX_MUL, VX_DIV, VX_MOD, VX_EQ, VX_NE, VX_LT, VX_LE,
    VX_GT, VX_GE, VX_AND, VX_OR, VX_XOR, VX_SHL, VX_SHR, VX_LAND, VX_LOR,
    VX_NOT, VX_NEG, VX_INV, VX_POS };
static inline uint64_t vx_mask(unsigned width) { return width == 64 ? UINT64_MAX : (UINT64_C(1) << width) - 1; }
static inline int64_t vx_signed_value(uint64_t a, unsigned width) {
    uint64_t mask = vx_mask(width); a &= mask;
    return (a & (UINT64_C(1) << (width - 1))) ? -(int64_t)(mask ^ a) - 1 : (int64_t)a;
}
static inline bool vx_slice_range_valid(uint64_t start,uint64_t count,uint64_t length,
                                        unsigned sw,bool ss,unsigned cw,bool cs) {
    return !(ss&&vx_signed_value(start,sw)<0) && !(cs&&vx_signed_value(count,cw)<0) &&
        start<=length && count<=length-start;
}
static inline uint64_t vx_magnitude(uint64_t a, unsigned width, bool sign) {
    return sign && (a & (UINT64_C(1) << (width - 1))) ? (UINT64_C(0) - a) & vx_mask(width) : a;
}
static inline uint64_t vx_integer_op(int op, uint64_t a, uint64_t b, unsigned width,
                                    bool sign, unsigned mode, bool *ok) {
    uint64_t mask = vx_mask(width), top = UINT64_C(1) << (width - 1), result = 0;
    bool negative_a = sign && (a & top), negative_b = sign && (b & top);
    bool overflow = false, negative_result = false;
    a &= mask;
    if (op != VX_SHL && op != VX_SHR) b &= mask;
    switch (op) {
    case VX_EQ: return a == b; case VX_NE: return a != b;
    case VX_LT: return sign ? vx_signed_value(a,width) < vx_signed_value(b,width) : a < b;
    case VX_LE: return sign ? vx_signed_value(a,width) <= vx_signed_value(b,width) : a <= b;
    case VX_GT: return sign ? vx_signed_value(a,width) > vx_signed_value(b,width) : a > b;
    case VX_GE: return sign ? vx_signed_value(a,width) >= vx_signed_value(b,width) : a >= b;
    case VX_AND: return a & b; case VX_OR: return a | b; case VX_XOR: return a ^ b;
    case VX_LAND: return a && b; case VX_LOR: return a || b;
    case VX_NOT: return !a; case VX_INV: return mask ^ a; case VX_POS: return a;
    case VX_SHL: case VX_SHR:
        if (b >= width) { *ok = false; return 0; }
        if (op == VX_SHL) return (a << b) & mask;
        return (a >> b) | (negative_a ? mask ^ (mask >> b) : 0);
    case VX_ADD:
        result = (a + b) & mask;
        overflow = sign ? ((a ^ result) & (b ^ result) & top) != 0 : a > mask - b;
        negative_result = negative_a; break;
    case VX_SUB:
        result = (a - b) & mask;
        overflow = sign ? ((a ^ b) & (a ^ result) & top) != 0 : a < b;
        negative_result = sign ? negative_a : true; break;
    case VX_NEG:
        result = (UINT64_C(0) - a) & mask;
        overflow = sign ? a == top : a != 0;
        negative_result = !sign; break;
    case VX_MUL: {
        uint64_t ma = vx_magnitude(a,width,sign), mb = vx_magnitude(b,width,sign);
        negative_result = negative_a != negative_b;
        uint64_t limit = sign ? (negative_result ? top : top - 1) : mask;
        overflow = mb != 0 && ma > limit / mb;
        result = (a * b) & mask; break;
    }
    case VX_DIV: case VX_MOD: {
        if (!b) { *ok = false; return 0; }
        uint64_t ma = vx_magnitude(a,width,sign), mb = vx_magnitude(b,width,sign);
        result = op == VX_DIV ? ma / mb : ma % mb;
        bool neg = op == VX_DIV ? negative_a != negative_b : negative_a;
        if (neg) result = (UINT64_C(0) - result) & mask;
        overflow = op == VX_DIV && sign && a == top && b == mask;
        break;
    }
    default: *ok = false; return 0;
    }
    if (overflow && mode == 1) { *ok = false; return 0; }
    if (overflow && mode == 2) return sign ? (negative_result ? top : top - 1) : (negative_result ? 0 : mask);
    return result;
}
static inline uint64_t vx_integer_cast(uint64_t value, unsigned from_width, bool from_signed, unsigned to_width) {
    if (from_signed && (value & (UINT64_C(1) << (from_width - 1)))) value |= ~vx_mask(from_width);
    return value & vx_mask(to_width);
}
_Static_assert(sizeof(float) == 4 && sizeof(double) == 8 && FLT_RADIX == 2 &&
               FLT_MANT_DIG == 24 && DBL_MANT_DIG == 53, "Vxn requires IEEE binary32/binary64 storage");
static inline uint64_t vx_float_bits(double value, unsigned width) {
    uint64_t bits;
    if (width == 32) { float f = value > FLT_MAX ? INFINITY : value < -FLT_MAX ? -INFINITY : (float)value; uint32_t small; memcpy(&small,&f,4); return small; }
    memcpy(&bits,&value,8); return bits;
}
static inline double vx_float_value(uint64_t bits, unsigned width) {
    if (width == 32) { uint32_t small = (uint32_t)bits; float f; memcpy(&f,&small,4); return (double)f; }
    double d; memcpy(&d,&bits,8); return d;
}
static inline uint64_t vx_float_op(int op, uint64_t aa, uint64_t bb, unsigned width) {
    double a = vx_float_value(aa,width), b = vx_float_value(bb,width), r = 0;
    switch (op) {
    case VX_EQ: return a == b; case VX_NE: return a != b;
    case VX_LT: return a < b; case VX_LE: return a <= b;
    case VX_GT: return a > b; case VX_GE: return a >= b;
    case VX_NEG: r = -a; break; case VX_POS: r = a; break;
    case VX_ADD: if (width == 32) { volatile float x = (float)a, y = (float)b; r = x + y; } else r = a + b; break;
    case VX_SUB: if (width == 32) { volatile float x = (float)a, y = (float)b; r = x - y; } else r = a - b; break;
    case VX_MUL: if (width == 32) { volatile float x = (float)a, y = (float)b; r = x * y; } else r = a * b; break;
    case VX_DIV:
        if (b == 0) r = a == 0 || isnan(a) ? NAN : ((signbit(a) != signbit(b)) ? -INFINITY : INFINITY);
        else if (width == 32) { volatile float x = (float)a, y = (float)b; r = x / y; }
        else r = a / b;
        break;
    default: r = NAN; break;
    }
    return vx_float_bits(r,width);
}
/* narrow(float -> integer): truncate, saturate at the endpoints, NaN -> 0.
   Integer narrowing keeps low bits; widening sign-extends signed sources. */
static inline uint64_t vx_convert(uint64_t a, unsigned fw, unsigned fk, unsigned tw, unsigned tk) {
    if (fk < 2 && tk < 2) return vx_integer_cast(a,fw,fk == 1,tw);
    if (fk == 2 && tk == 2) return vx_float_bits(vx_float_value(a,fw),tw);
    if (tk == 2) {
        if (tw == 32) {
            float f = fk == 1 ? (float)vx_signed_value(a,fw) : (float)a;
            return vx_float_bits((double)f,32);
        }
        return vx_float_bits(fk == 1 ? (double)vx_signed_value(a,fw) : (double)a,64);
    }
    double d = vx_float_value(a,fw);
    if (isnan(d)) return 0;
    uint64_t top = UINT64_C(1) << (tw - 1), mask = vx_mask(tw);
    if (tk == 1) {
        if (d >= (double)top) return top - 1;
        if (d <= -(double)top) return top;
        return (uint64_t)(int64_t)d & mask;
    }
    double limit = tw == 64 ? 18446744073709551616.0 : (double)(UINT64_C(1) << tw);
    if (d <= 0) return 0;
    if (d >= limit) return mask;
    return (uint64_t)d;
}
#endif
