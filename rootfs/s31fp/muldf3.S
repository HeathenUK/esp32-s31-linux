/*
 * s31fp_muldf3 - IEEE double multiply, hand-written for rv32im.
 *
 * Fast path: both operands normal and the result's exponent safely normal
 * (1 <= ae+be-1023 <= 0x7fd). Everything else - zeros, subnormals, inf, NaN,
 * overflow, underflow - tail-calls the C version (s31fp_muldf3_c), with the
 * arguments untouched because every test happens before anything is clobbered.
 *
 * 53 x 64-bit significand product from four mul/mulhu pairs; when an operand's
 * low word is zero (any double that came from an int or a float - most of an
 * FM synthesiser's operands) two of the four pairs are skipped.
 * Round to nearest even; a rounding carry runs into the exponent field by
 * construction, which is also how 0x7fe rounds up to infinity correctly.
 * Leaf, no stack, caller-saved registers only. a1:a0 = x, a3:a2 = y.
 */
	.text
	.globl	s31fp_muldf3
	.type	s31fp_muldf3, @function
	.align	2
s31fp_muldf3:
	srli	t0, a1, 20
	srli	t1, a3, 20
	andi	t0, t0, 0x7ff
	andi	t1, t1, 0x7ff
	li	t3, 0x7fe
	addi	t2, t0, -1
	bgeu	t2, t3, .Lslow
	addi	t2, t1, -1
	bgeu	t2, t3, .Lslow
	add	t0, t0, t1
	addi	t0, t0, -1023		/* pe0 */
	addi	t2, t0, -1
	li	t3, 0x7fd
	bgeu	t2, t3, .Lslow		/* need 1 <= pe0 <= 0x7fd */

	xor	t6, a1, a3
	lui	t3, 0x80000
	and	t6, t6, t3		/* result sign */
	lui	t3, 0x100		/* implicit bit, 0x00100000 */
	addi	t4, t3, -1		/* 0x000fffff */
	and	a1, a1, t4
	and	a3, a3, t4
	or	a1, a1, t3		/* A = a1:a0, 53 bits */
	or	a3, a3, t3
	slli	a3, a3, 11		/* B <<= 11, 64 bits, top bit set */
	srli	t5, a2, 21
	or	a3, a3, t5
	slli	a2, a2, 11

	/* product words: t2 (w0) t3 (w1) t5 (w2) a7 (w3) */
	mul	a6, a1, a3		/* p11 lo */
	mulhu	a7, a1, a3		/* p11 hi */
	beqz	a0, .La0zero
	beqz	a2, .Lb0zero
	mul	t2, a0, a2		/* p00 */
	mulhu	t3, a0, a2
	mul	t4, a0, a3		/* p01 */
	mulhu	t5, a0, a3
	mul	a4, a1, a2		/* p10 */
	mulhu	a5, a1, a2
	add	t3, t3, t4		/* w1 = H00 + L01 + L10 */
	sltu	t4, t3, t4
	add	t3, t3, a4
	sltu	a4, t3, a4
	add	t4, t4, a4		/* carry out of w1: 0..2 */
	add	t5, t5, a5		/* w2 = H01 + H10 + L11 + carry */
	sltu	a5, t5, a5
	add	t5, t5, a6
	sltu	a6, t5, a6
	add	a5, a5, a6
	add	t5, t5, t4
	sltu	t4, t5, t4
	add	a5, a5, t4
	add	a7, a7, a5		/* w3 */
	j	.Lnorm
.La0zero:				/* A.lo == 0: only p10 and p11 */
	li	t2, 0
	mul	t3, a1, a2		/* w1 = L10 */
	mulhu	t5, a1, a2		/* w2 = H10 + L11 */
	add	t5, t5, a6
	sltu	a6, t5, a6
	add	a7, a7, a6
	j	.Lnorm
.Lb0zero:				/* B.lo == 0: only p01 and p11 */
	li	t2, 0
	mul	t3, a0, a3		/* w1 = L01 */
	mulhu	t5, a0, a3		/* w2 = H01 + L11 */
	add	t5, t5, a6
	sltu	a6, t5, a6
	add	a7, a7, a6
.Lnorm:
	srli	t1, a7, 20		/* bit 52 of the high half set? */
	bnez	t1, .Lbig
	slli	a7, a7, 1		/* no: shift the whole product left one */
	srli	a4, t5, 31
	or	a7, a7, a4
	slli	t5, t5, 1
	srli	a4, t3, 31
	or	t5, t5, a4
	slli	t3, t3, 1
	srli	a4, t2, 31
	or	t3, t3, a4
	slli	t2, t2, 1
	j	.Lpack
.Lbig:
	addi	t0, t0, 1
.Lpack:
	lui	a4, 0x100
	addi	a4, a4, -1
	and	a7, a7, a4		/* drop the implicit bit */
	slli	t0, t0, 20
	or	a7, a7, t0
	or	a7, a7, t6
	bgez	t3, .Ldone		/* round bit clear: exact or below half */
	slli	a4, t3, 1
	or	a4, a4, t2		/* sticky */
	andi	a5, t5, 1		/* ties go to even */
	or	a4, a4, a5
	beqz	a4, .Ldone
	addi	t5, t5, 1
	seqz	a4, t5
	add	a7, a7, a4
.Ldone:
	mv	a0, t5
	mv	a1, a7
	ret
.Lslow:
	tail	s31fp_muldf3_c
	.size	s31fp_muldf3, . - s31fp_muldf3
