/* test-only (-Wl,--wrap): count every fall-back from a v2 routine into
 * libgcc's, so the check can report what share of operand sets the fast
 * paths decided on their own. Touches memory only - fflags/frm untouched. */
#include <stdint.h>
typedef uint64_t u64; typedef uint32_t u32;
volatile unsigned long v2_refcalls;
#define W2(n, R, A, B) R __real_s31lg_##n(A, B); R __wrap_s31lg_##n(A a, B b) { v2_refcalls++; return __real_s31lg_##n(a, b); }
#define W1(n, R, A) R __real_s31lg_##n(A); R __wrap_s31lg_##n(A a) { v2_refcalls++; return __real_s31lg_##n(a); }
W2(muldf3, u64, u64, u64) W2(adddf3, u64, u64, u64) W2(subdf3, u64, u64, u64) W2(divdf3, u64, u64, u64)
W2(gedf2, int, u64, u64) W2(ledf2, int, u64, u64) W2(eqdf2, int, u64, u64) W2(unorddf2, int, u64, u64)
W1(fixdfsi, int32_t, u64) W1(fixunsdfsi, u32, u64) W1(extendsfdf2, u64, u32) W1(truncdfsf2, u32, u64)
W1(floatsidf, u64, int32_t) W1(floatunsidf, u64, u32)
