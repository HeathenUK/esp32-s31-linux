/*
 * s31fp v2 - IEEE-754 binary64 helpers for rv32imafc + Zbb, bit-exact to THIS
 * target's libgcc (soft-fp, riscv sfp-machine.h) including fflags and frm.
 *
 * Contract: every routine returns exactly what libgcc's routine returns - the
 * same bits (NaNs included: libgcc here always produces the canonical quiet
 * NaN 0x7ff8000000000000, sign clear) and the same accrued fflags - under
 * every rounding mode. It gets there by splitting the work:
 *
 *   fast paths  normal operands, a normal result, frm == RNE. Handled here,
 *               raising NX (and OF for add overflow) exactly as soft-fp does.
 *   everything  zero x inf, subnormals, NaNs, inf, results that leave the
 *   else        normal range, and any frm other than RNE: a tail call into
 *               libgcc's OWN routine (linked in renamed, s31lg_*), with the
 *               argument registers untouched. Those cases are libgcc by
 *               construction, not by imitation.
 *
 * S31V2_FENV=0 (build flag) drops the frm test and the fflags writes: the
 * results stay identical under RNE, but fetestexcept() after a double op and
 * fesetround() for doubles stop working. Measured separately; not the default.
 *
 * Calling convention: soft-float ilp32, double x in a1:a0, y in a3:a2,
 * float in a0. Leaf routines, caller-saved registers only, no stack.
 */
#ifndef S31V2_FENV
#define S31V2_FENV 1
#endif
/* S31V2_FFLAGS=0 with FENV=1: frm honoured (non-RNE -> libgcc), but the fast
 * paths do not raise NX - a stated deviation, measured as its own arm */
#ifndef S31V2_FFLAGS
#define S31V2_FFLAGS S31V2_FENV
#endif

/*
 * CSR accesses cost ~25-30 ns each on this core (measured: libgcc's own
 * frrm + csrs is most of the difference between a 250 and a 305 ns multiply).
 * So each routine reads fcsr ONCE - frm (bits 7:5) and the accrued flags
 * (bits 4:0) together - and writes NX only when it is not already set. The
 * flags are sticky, so the result is the same as libgcc's unconditional
 * `csrs fflags`, except if a signal handler clears NX between the read and
 * the end of the routine (then libgcc would set it again and this does not).
 * \save keeps fcsr for NX_IF; t0 is free at every entry.
 */
.macro RNE_OR ref, save=t6
#if S31V2_FENV
	frcsr	\save
	srli	t0, \save, 5
	bnez	t0, \ref
#endif
.endm
/* accrue NX when \reg != 0 and NX is not yet set in \save (tmp: scratch) */
.macro NX_IF reg, save=t6, tmp=t0
#if S31V2_FFLAGS
	beqz	\reg, 99f
#if S31V2_FENV
	andi	\tmp, \save, 1
	bnez	\tmp, 99f
#endif
	csrsi	fflags, 1
99:
#endif
.endm

/* conversions that ignore frm (truncation) read no CSR: a plain accrue */
.macro NX_ALWAYS reg
#if S31V2_FFLAGS
	beqz	\reg, 99f
	csrsi	fflags, 1
99:
#endif
.endm

.macro FUNC name
	.globl	\name
	.type	\name, @function
.endm

	.text
	.align	2

/* ------------------------------------------------------------------ mul */
/*
 * Fast paths, in the order they are tested:
 *  - either operand +-0 and the other finite: signed zero, exact
 *  - either operand a power of two (mantissa 0): exponent add, exact
 *  - an operand whose low word is 0 (every double made from an int or a
 *    float): 2 multiplies (1 when both are)
 *  - general: 53x64 product from 4 mul + 4 mulhu, branchless rounding
 * Needs 1 <= ea+eb-1023 <= 0x7fc, so neither the normalise step nor the
 * rounding carry can reach 0x7ff; outside that libgcc raises OF/UF.
 */
FUNC s31v2_muldf3
s31v2_muldf3:
	RNE_OR	.Lm_ref
	srli	t0, a1, 20
	srli	t1, a3, 20
	andi	t0, t0, 0x7ff		/* ea */
	andi	t1, t1, 0x7ff		/* eb */
	li	t3, 0x7fe
	addi	t2, t0, -1
	bgeu	t2, t3, .Lm_speca
	addi	t2, t1, -1
	bgeu	t2, t3, .Lm_specb
	add	a4, t0, t1
	addi	a4, a4, -1023		/* pe0 */
	addi	t2, a4, -1
	li	t3, 0x7fc
	bgeu	t2, t3, .Lm_ref
#if S31V2_FENV
	mv	t3, t6			/* fcsr, for NX_IF (t3 is free from here on) */
#endif
	xor	t6, a1, a3		/* sign in bit 31 */
	beqz	a0, .Lm_a0z
	beqz	a2, .Lm_b0z
	/* general: A = a1:a0 (53 bits), B = a3:a2 << 11 (64 bits) */
	slli	a1, a1, 12
	srli	a1, a1, 12
	bseti	a1, a1, 20
	slli	a3, a3, 12
	srli	a3, a3, 1
	srli	t5, a2, 21
	or	a3, a3, t5
	bseti	a3, a3, 31
	slli	a2, a2, 11
	mul	t4, a0, a2		/* L00 */
	mulhu	t5, a0, a2		/* H00 */
	mul	t0, a0, a3		/* L01 */
	mulhu	a5, a0, a3		/* H01 */
	mul	t1, a1, a2		/* L10 */
	mulhu	a7, a1, a2		/* H10 */
	mul	a6, a1, a3		/* L11 */
	mulhu	a1, a1, a3		/* H11 */
	add	t5, t5, t0		/* w1 = H00 + L01 + L10 */
	sltu	t0, t5, t0
	add	t5, t5, t1
	sltu	t1, t5, t1
	add	t0, t0, t1		/* carry into w2, 0..2 */
	add	a5, a5, a7		/* w2 = H01 + H10 + L11 + c */
	sltu	a7, a5, a7
	add	a5, a5, a6
	sltu	a6, a5, a6
	add	a7, a7, a6
	add	a5, a5, t0
	sltu	t0, a5, t0
	add	a7, a7, t0
	add	a1, a1, a7		/* w3 */
	snez	t4, t4			/* w0 only matters as sticky */
	or	t5, t5, t4
	j	.Lm_norm
.Lm_a0z:
	slli	t2, a1, 12
	beqz	t2, .Lm_pow2a
	beqz	a2, .Lm_both0
	/* A = a1:0 -> w1 = L10, w2 = H10 + L11, w3 = H11 */
	slli	a1, a1, 12
	srli	a1, a1, 12
	bseti	a1, a1, 20
	slli	a3, a3, 12
	srli	a3, a3, 1
	srli	t5, a2, 21
	or	a3, a3, t5
	bseti	a3, a3, 31
	slli	a2, a2, 11
	mul	t5, a1, a2
	mulhu	a5, a1, a2
	mul	a6, a1, a3
	mulhu	a1, a1, a3
	add	a5, a5, a6
	sltu	a6, a5, a6
	add	a1, a1, a6
	j	.Lm_norm
.Lm_both0:				/* A = a1:0, B = b1':0 -> exact */
	slli	t2, a3, 12
	beqz	t2, .Lm_pow2b
	slli	a1, a1, 12
	srli	a1, a1, 12
	bseti	a1, a1, 20
	slli	a3, a3, 12
	srli	a3, a3, 1
	bseti	a3, a3, 31
	mul	a5, a1, a3
	mulhu	a1, a1, a3
	li	t5, 0
	j	.Lm_norm
.Lm_b0z:				/* a0 != 0, B = b1':0 -> w1 = L01, w2 = H01 + L11, w3 = H11 */
	slli	t2, a3, 12
	beqz	t2, .Lm_pow2b
	slli	a1, a1, 12
	srli	a1, a1, 12
	bseti	a1, a1, 20
	slli	a3, a3, 12
	srli	a3, a3, 1
	bseti	a3, a3, 31
	mul	t5, a0, a3
	mulhu	a5, a0, a3
	mul	a6, a1, a3
	mulhu	a1, a1, a3
	add	a5, a5, a6
	sltu	a6, a5, a6
	add	a1, a1, a6
	/* fall through */
.Lm_norm:	/* a1:a5 = top 53/54 bits, t5 = round word (sticky folded), a4 = pe0, t6 sign */
	srli	t1, a1, 20
	bnez	t1, 1f
	slli	a1, a1, 1
	srli	t1, a5, 31
	or	a1, a1, t1
	slli	a5, a5, 1
	srli	t1, t5, 31
	or	a5, a5, t1
	slli	t5, t5, 1
	addi	a4, a4, -1
1:	slli	a4, a4, 20		/* implicit bit adds the missing 1 */
	add	a1, a1, a4
	srli	t6, t6, 31
	slli	t6, t6, 31
	or	a1, a1, t6
	srli	t1, t5, 31		/* round bit */
	slli	t2, t5, 1		/* sticky */
	andi	a2, a5, 1		/* lsb: ties to even */
	or	t2, t2, a2
	snez	t2, t2
	and	t1, t1, t2
	add	a0, a5, t1
	sltu	t1, a0, t1
	add	a1, a1, t1
	NX_IF	t5, t3, t1
	ret
.Lm_pow2a:				/* x = +-2^k: y's exponent moves, exact */
	mv	a0, a2
	sub	t2, a4, t1		/* pe0 - eb */
	slli	t2, t2, 20
	add	a1, a3, t2
	srli	t2, t6, 31
	slli	t2, t2, 31
	lui	t3, 0x80000
	and	t3, a3, t3
	xor	t2, t2, t3		/* = sign(x) */
	xor	a1, a1, t2
	ret
.Lm_pow2b:				/* y = +-2^k */
	sub	t2, a4, t0		/* pe0 - ea */
	slli	t2, t2, 20
	add	a1, a1, t2
	srli	t2, a3, 31
	slli	t2, t2, 31
	xor	a1, a1, t2
	ret
.Lm_speca:				/* x is 0, subnormal, inf or NaN */
	slli	t2, a1, 1
	or	t2, t2, a0
	bnez	t2, .Lm_ref
	li	t3, 0x7ff
	beq	t1, t3, .Lm_ref		/* 0 x inf/NaN */
	j	.Lm_zero
.Lm_specb:				/* x normal, y special */
	slli	t2, a3, 1
	or	t2, t2, a2
	bnez	t2, .Lm_ref
.Lm_zero:
	xor	a1, a1, a3
	srli	a1, a1, 31
	slli	a1, a1, 31
	li	a0, 0
	ret
.Lm_ref:
	tail	s31lg_muldf3
	.size	s31v2_muldf3, . - s31v2_muldf3

/* ------------------------------------------------------------- add/sub */
/*
 * Fast paths: a zero operand (x + 0 = x, the +-0 sign rule under RNE);
 * both normal. Three guard bits and a sticky; a subtraction that cancels
 * more than one bit renormalises with clz (exact by construction, since that
 * only happens when the exponents differ by <= 1). Subnormal results,
 * inf/NaN operands, an exponent above 0x7fc (so overflow never happens
 * here) and frm != RNE go to libgcc. The subnormal-result exit comes after
 * the operands may have been swapped and y negated for subtract: addition is
 * commutative bit-for-bit, and subtract is add with y negated - true of
 * libgcc here too, whose NaN results are canonical whatever the signs.
 */
FUNC s31v2_subdf3
s31v2_subdf3:
	lui	t0, 0x80000
	xor	a3, a3, t0
	/* fall through */
FUNC s31v2_adddf3
s31v2_adddf3:
	RNE_OR	.La_ref
	srli	t0, a1, 20
	srli	t1, a3, 20
	andi	t0, t0, 0x7ff		/* ae */
	andi	t1, t1, 0x7ff		/* be */
	li	t2, 0x7fc		/* exponents <= 0x7fc: no carry can reach inf */
	addi	t3, t0, -1
	bgeu	t3, t2, .La_speca
	addi	t3, t1, -1
	bgeu	t3, t2, .La_specb
	slli	t3, a1, 1		/* |x|, |y| high words */
	slli	t4, a3, 1
	bltu	t4, t3, .La_ordered
	bne	t3, t4, .La_swap
	bgeu	a0, a2, .La_ordered
.La_swap:
	mv	t5, a0
	mv	a0, a2
	mv	a2, t5
	mv	t5, a1
	mv	a1, a3
	mv	a3, t5
	mv	t5, t0
	mv	t0, t1
	mv	t1, t5
.La_ordered:				/* |x| >= |y| */
	sub	t2, t0, t1		/* d >= 0 */
	li	t3, 56
	bgeu	t2, t3, .La_far		/* y below a quarter ulp of x */
	lui	t3, 0x100
	slli	a5, a1, 12		/* X = a5:a4 = (1.m) << 3 */
	srli	a5, a5, 9
	slli	t3, t3, 3
	or	a5, a5, t3
	srli	t5, a0, 29
	or	a5, a5, t5
	slli	a4, a0, 3
	slli	a7, a3, 12		/* Y = a7:a6 */
	srli	a7, a7, 9
	or	a7, a7, t3
	srli	t5, a2, 29
	or	a7, a7, t5
	slli	a6, a2, 3
	beqz	t2, .La_aligned
	li	t5, 32
	bgeu	t2, t5, .La_shfar
	sub	t5, t5, t2		/* 1 <= d <= 31 */
	sll	t3, a6, t5		/* bits shifted out */
	srl	a6, a6, t2
	sll	t4, a7, t5
	or	a6, a6, t4
	srl	a7, a7, t2
	snez	t3, t3
	or	a6, a6, t3
	j	.La_aligned
.La_shfar:				/* 32 <= d <= 55 */
	addi	t5, t2, -32
	srl	t4, a7, t5
	beqz	t5, 1f
	li	t3, 32
	sub	t3, t3, t5
	sll	t3, a7, t3
	or	a6, a6, t3
1:	snez	t3, a6
	or	a6, t4, t3
	li	a7, 0
.La_aligned:
	xor	t5, a1, a3
	bltz	t5, .La_sub
	add	a4, a4, a6
	sltu	t3, a4, a6
	add	a5, a5, a7
	add	a5, a5, t3
	srli	t3, a5, 24		/* carried into bit 56? */
	beqz	t3, .La_round
	andi	t3, a4, 1
	srli	a4, a4, 1
	slli	t4, a5, 31
	or	a4, a4, t4
	or	a4, a4, t3
	srli	a5, a5, 1
	addi	t0, t0, 1		/* <= 0x7fd */
	j	.La_round
.La_sub:
	sltu	t3, a4, a6
	sub	a4, a4, a6
	sub	a5, a5, a7
	sub	a5, a5, t3
	srli	t3, a5, 23
	bnez	t3, .La_round		/* still normalised */
	or	t3, a4, a5
	beqz	t3, .La_pzero		/* x - x = +0 under RNE */
	/* renormalise: leading one to bit 55 (bit 23 of a5) */
	beqz	a5, 2f
	clz	t3, a5
	addi	t3, t3, -8		/* shift, 1..23 */
	j	3f
2:	clz	t3, a4
	addi	t3, t3, 24		/* 24..55 */
3:	sub	t0, t0, t3
	blez	t0, .La_ref		/* subnormal result: libgcc (args intact) */
	li	t4, 32
	bgeu	t3, t4, 4f
	sub	t4, t4, t3
	srl	t4, a4, t4
	sll	a5, a5, t3
	or	a5, a5, t4
	sll	a4, a4, t3
	j	.La_round
4:	addi	t4, t3, -32
	sll	a5, a4, t4
	li	a4, 0
.La_round:	/* a5:a4 = 1.m << 3 (bit 55 set), t0 = exponent, a1 = sign of result */
	andi	t3, a4, 7
	srli	a4, a4, 3
	slli	t4, a5, 29
	or	a4, a4, t4
	srli	a5, a5, 3
	addi	t0, t0, -1		/* the implicit bit adds it back */
	slli	t0, t0, 20
	add	a5, a5, t0
	srli	t4, a1, 31
	slli	t4, t4, 31
	or	a5, a5, t4
	/* round up iff g && (r|s|lsb): t3 in 0..7, g = bit 2 */
	andi	t4, a4, 1
	or	t4, t4, t3		/* bit 0|1 = r,s or lsb */
	andi	t4, t4, 3
	snez	t4, t4
	srli	t5, t3, 2
	and	t4, t4, t5
	add	a0, a4, t4
	sltu	t4, a0, t4
	add	a1, a5, t4
	NX_IF	t3, t6, t4
	ret
.La_far:				/* |y| < ulp(x)/4: x, inexact */
	li	t3, 1
	NX_IF	t3, t6, t4
	ret
.La_pzero:
	li	a0, 0
	li	a1, 0
	ret
.La_speca:				/* x zero/sub/inf/NaN */
	slli	t3, a1, 1
	or	t3, t3, a0
	bnez	t3, .La_ref
	/* x = +-0: y finite -> y (and +-0 + +-0 -> AND of the signs) */
	li	t2, 0x7ff
	beq	t1, t2, .La_ref
	slli	t3, a3, 1
	or	t3, t3, a2
	bnez	t3, 1f
	and	a1, a1, a3
	ret
1:	mv	a0, a2
	mv	a1, a3
	ret
.La_specb:				/* x normal, y zero/sub/inf/NaN */
	slli	t3, a3, 1
	or	t3, t3, a2
	bnez	t3, .La_ref
	ret				/* x + 0 = x */
.La_ref:
	tail	s31lg_adddf3
	.size	s31v2_adddf3, . - s31v2_adddf3

/* --------------------------------------------------------- conversions */
FUNC s31v2_floatsidf
s31v2_floatsidf:			/* exact */
	beqz	a0, 9f
	srai	t0, a0, 31
	xor	t1, a0, t0
	sub	t1, t1, t0		/* |x| */
	clz	t2, t1
	sll	t1, t1, t2		/* bit 31 = implicit one */
	li	t3, 1053
	sub	t3, t3, t2		/* 1023 + 31 - clz - 1: implicit bit adds 1 */
	srli	a1, t1, 11
	slli	a0, t1, 21
	slli	t3, t3, 20
	add	a1, a1, t3
	slli	t0, t0, 31
	or	a1, a1, t0
	ret
9:	li	a1, 0
	ret
	.size	s31v2_floatsidf, . - s31v2_floatsidf

FUNC s31v2_floatunsidf
s31v2_floatunsidf:
	beqz	a0, 9f
	clz	t2, a0
	sll	t1, a0, t2
	li	t3, 1053
	sub	t3, t3, t2
	srli	a1, t1, 11
	slli	a0, t1, 21
	slli	t3, t3, 20
	add	a1, a1, t3
	ret
9:	li	a1, 0
	ret
	.size	s31v2_floatunsidf, . - s31v2_floatunsidf

/* truncate toward zero; NX when fraction bits are dropped (as soft-fp does),
 * out of range / NaN -> libgcc (NV and its saturation value) */
FUNC s31v2_fixdfsi
s31v2_fixdfsi:
	srli	t0, a1, 20
	andi	t0, t0, 0x7ff
	addi	t0, t0, -1023		/* unbiased exponent */
	bltz	t0, 8f			/* |x| < 1 */
	li	t1, 31
	bge	t0, t1, 7f		/* too big, inf or NaN */
	slli	t3, a1, 11		/* 1.m as a 32-bit value, bit 31 set */
	srli	t4, a0, 21
	or	t3, t3, t4
	bseti	t3, t3, 31
#if S31V2_FFLAGS
	addi	t2, t0, 1
	sll	t4, t3, t2		/* fraction bits of the window */
	slli	t5, a0, 11		/* and below it */
	or	t4, t4, t5
#endif
	sub	t1, t1, t0
	srl	t3, t3, t1
	srai	t5, a1, 31
	xor	a0, t3, t5
	sub	a0, a0, t5
	NX_ALWAYS t4
	ret
8:
#if S31V2_FFLAGS
	slli	t4, a1, 1
	or	t4, t4, a0
#endif
	li	a0, 0
	NX_ALWAYS t4
	ret
7:	tail	s31lg_fixdfsi
	.size	s31v2_fixdfsi, . - s31v2_fixdfsi

FUNC s31v2_fixunsdfsi
s31v2_fixunsdfsi:
	bltz	a1, 6f			/* negative (or -0, -NaN) */
	srli	t0, a1, 20
	addi	t0, t0, -1023
	bltz	t0, 8f
	li	t1, 31
	blt	t1, t0, 7f		/* >= 2^32, inf, NaN */
	slli	t3, a1, 11
	srli	t4, a0, 21
	or	t3, t3, t4
	bseti	t3, t3, 31
#if S31V2_FFLAGS
	addi	t2, t0, 1
	sll	t4, t3, t2
	beq	t0, t1, 5f		/* e = 31: window has no fraction bits (shift by 32) */
	j	4f
5:	li	t4, 0
4:	slli	t5, a0, 11
	or	t4, t4, t5
#endif
	sub	t1, t1, t0
	srl	a0, t3, t1
	NX_ALWAYS t4
	ret
8:
#if S31V2_FFLAGS
	or	t4, a1, a0
#endif
	li	a0, 0
	NX_ALWAYS t4
	ret
6:	slli	t0, a1, 1		/* -0 -> 0, exact */
	or	t0, t0, a0
	bnez	t0, 7f
	li	a0, 0
	ret
7:	tail	s31lg_fixunsdfsi
	.size	s31v2_fixunsdfsi, . - s31v2_fixunsdfsi

/* float -> double: exact for normals and zeros; subnormal/inf/NaN -> libgcc */
FUNC s31v2_extendsfdf2
s31v2_extendsfdf2:
	slli	t0, a0, 1
	srli	t0, t0, 24		/* ef */
	addi	t1, t0, -1
	li	t2, 254
	bgeu	t1, t2, 5f
	slli	a1, a0, 1
	srli	a1, a1, 4		/* e8:m23 >> 3 into place */
	li	t2, 0x38000000		/* (1023 - 127) << 20 */
	add	a1, a1, t2
	srli	t2, a0, 31
	slli	t2, t2, 31
	or	a1, a1, t2
	slli	a0, a0, 29
	ret
5:	slli	t1, a0, 1
	bnez	t1, 6f
	mv	a1, a0			/* +-0 */
	li	a0, 0
	ret
6:	tail	s31lg_extendsfdf2
	.size	s31v2_extendsfdf2, . - s31v2_extendsfdf2

/* double -> float, RNE; result must be a normal float that cannot round into
 * the infinities (ef <= 253), else libgcc */
FUNC s31v2_truncdfsf2
s31v2_truncdfsf2:
	RNE_OR	6f
	srli	t0, a1, 20
	andi	t0, t0, 0x7ff
	addi	t0, t0, -897		/* ef - 1 */
	li	t1, 253
	bgeu	t0, t1, 5f
	slli	t2, a1, 12
	srli	t2, t2, 9		/* m20 << 3 */
	srli	t3, a0, 29
	or	t2, t2, t3		/* m23 */
	addi	t0, t0, 1
	slli	t0, t0, 23
	or	t2, t2, t0
	srli	t3, a1, 31
	slli	t3, t3, 31
	or	t2, t2, t3
	slli	t4, a0, 3		/* bit 31 = round, rest sticky */
	srli	t5, t4, 31
	slli	a1, t4, 1
	andi	t3, t2, 1
	or	a1, a1, t3
	snez	a1, a1
	and	t5, t5, a1
	add	a0, t2, t5
	NX_IF	t4, t6, t3
	ret
5:	slli	t1, a1, 1		/* +-0 -> +-0f */
	or	t1, t1, a0
	bnez	t1, 6f
	mv	a0, a1
	ret
6:	tail	s31lg_truncdfsf2
	.size	s31v2_truncdfsf2, . - s31v2_truncdfsf2

/* --------------------------------------------------------- comparisons */
/* NaN test: |hi| + (lo != 0) > 0x7ff00000. Any NaN -> libgcc, which returns
 * the unordered value (ge/gt -2, le/lt 2, eq/ne 1, unord 1) and raises NV
 * (ge/gt/le/lt: any NaN; eq/ne/unord: signalling only). */
.macro NANCHK hi, lo, ref
	slli	t0, \hi, 1
	srli	t0, t0, 1
	snez	t1, \lo
	add	t1, t0, t1
	lui	t2, 0x7ff00
	bltu	t2, t1, \ref
.endm

/* sign-magnitude compare of two non-NaN doubles: a0 = -1/0/1 */
.macro CMP3
	slli	t0, a1, 1
	srli	t0, t0, 1
	slli	t1, a3, 1
	srli	t1, t1, 1
	xor	t4, a1, a3
	bltz	t4, 3f
	bne	t0, t1, 1f
	sltu	t5, a2, a0
	sltu	t6, a0, a2
	j	2f
1:	sltu	t5, t1, t0
	sltu	t6, t0, t1
2:	sub	a0, t5, t6
	srai	t4, a1, 31
	xor	a0, a0, t4
	sub	a0, a0, t4
	ret
3:	or	t5, t0, t1
	or	t5, t5, a0
	or	t5, t5, a2
	beqz	t5, 4f
	srai	a0, a1, 31
	ori	a0, a0, 1
	ret
4:	li	a0, 0
	ret
.endm

/* Both operands non-negative (the usual case - an envelope against its
 * target): their bit patterns order like unsigned integers, NaNs above inf,
 * so when the high words differ only the LARGER one can be a NaN, and it
 * cannot be one if its high word is below 0x7ff00000. Anything else takes
 * the full path. */
.macro POSCMP slow
	or	t0, a1, a3
	bltz	t0, \slow
	lui	t2, 0x7ff00
	bltu	a1, a3, 5f
	bne	a1, a3, 6f
	bgeu	a1, t2, \slow		/* equal high words in the inf/NaN range */
	sltu	t5, a2, a0
	sltu	t6, a0, a2
	sub	a0, t5, t6
	ret
5:	bgeu	a3, t2, \slow
	li	a0, -1
	ret
6:	bgeu	a1, t2, \slow
	li	a0, 1
	ret
.endm

FUNC s31v2_gedf2
FUNC s31v2_gtdf2
s31v2_gedf2:
s31v2_gtdf2:
	POSCMP	7f
7:	NANCHK	a1, a0, 8f
	NANCHK	a3, a2, 8f
	CMP3
8:	tail	s31lg_gedf2
	.size	s31v2_gedf2, . - s31v2_gedf2

FUNC s31v2_ledf2
FUNC s31v2_ltdf2
s31v2_ledf2:
s31v2_ltdf2:
	POSCMP	7f
7:	NANCHK	a1, a0, 8f
	NANCHK	a3, a2, 8f
	CMP3
8:	tail	s31lg_ledf2
	.size	s31v2_ledf2, . - s31v2_ledf2

FUNC s31v2_eqdf2
FUNC s31v2_nedf2
s31v2_eqdf2:
s31v2_nedf2:
	NANCHK	a1, a0, 8f
	NANCHK	a3, a2, 8f
	xor	t0, a0, a2
	xor	t1, a1, a3
	or	t0, t0, t1		/* bits differ? */
	or	t1, a1, a3
	slli	t1, t1, 1
	or	t1, t1, a0
	or	t1, t1, a2		/* both zero? */
	snez	t0, t0
	snez	t1, t1
	and	a0, t0, t1
	ret
8:	tail	s31lg_eqdf2
	.size	s31v2_eqdf2, . - s31v2_eqdf2

FUNC s31v2_unorddf2
s31v2_unorddf2:
	NANCHK	a1, a0, 8f
	NANCHK	a3, a2, 8f
	li	a0, 0
	ret
8:	tail	s31lg_unorddf2
	.size	s31v2_unorddf2, . - s31v2_unorddf2
