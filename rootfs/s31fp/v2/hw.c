/* Independent oracle: the D extension as QEMU implements it (softfloat,
 * RISC-V NaN and flag rules). Compiled with -march=...d -mabi=ilp32, so the
 * operands arrive in integer registers exactly as the soft-float helpers'
 * do. Host/QEMU only - the board has no D. */
#include <stdint.h>
#include <string.h>
typedef uint64_t u64; typedef uint32_t u32;
static inline double D(u64 a) { double d; memcpy(&d, &a, 8); return d; }
static inline u64 U(double d) { u64 a; memcpy(&a, &d, 8); return a; }
u64 hw_mul(u64 a, u64 b) { return U(D(a) * D(b)); }
u64 hw_add(u64 a, u64 b) { return U(D(a) + D(b)); }
u64 hw_sub(u64 a, u64 b) { return U(D(a) - D(b)); }
u64 hw_div(u64 a, u64 b) { return U(D(a) / D(b)); }
int hw_gt(u64 a, u64 b) { return D(a) > D(b); }
int hw_ge(u64 a, u64 b) { return D(a) >= D(b); }
int hw_lt(u64 a, u64 b) { return D(a) < D(b); }
int hw_le(u64 a, u64 b) { return D(a) <= D(b); }
int hw_eq(u64 a, u64 b) { return D(a) == D(b); }
int hw_unord(u64 a, u64 b) { return __builtin_isunordered(D(a), D(b)); }
u64 hw_floatsidf(int32_t i) { return U((double)i); }
u64 hw_floatunsidf(u32 i) { return U((double)i); }
int32_t hw_fixdfsi(u64 a) { return (int32_t)D(a); }
u32 hw_fixunsdfsi(u64 a) { return (u32)D(a); }
u64 hw_extendsfdf2(u32 f) { float x; memcpy(&x, &f, 4); return U((double)x); }
u32 hw_truncdfsf2(u64 a) { float x = (float)D(a); u32 f; memcpy(&f, &x, 4); return f; }
