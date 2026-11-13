#ifndef VXN_EXTENDED_H
#define VXN_EXTENDED_H
#include <stdint.h>
/* Stable cell ABI: float80 = {significand, sign/exponent};
   float128 = {low fraction, high sign/exponent/fraction}. */
void vxn_ext_op(uint64_t width,uint64_t op,const uint64_t *a,const uint64_t *b,uint64_t *out);
void vxn_ext_convert(const uint64_t *a,uint64_t fw,uint64_t fk,uint64_t tw,uint64_t tk,uint64_t *out);
void vxn_ext_parse(const uint64_t *bytes,uint64_t length,uint64_t width,uint64_t out[3]);
/* Round-trip decimal, 21 / 36 significant digits; buffer needs 64 bytes. */
int vxn_ext_format(const uint64_t *value,uint64_t width,char *out);
#endif
