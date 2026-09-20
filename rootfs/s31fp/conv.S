/*
 * int <-> double conversions, rv32im + Zbb (clz). All exact or truncating,
 * so there is no rounding to get wrong; out-of-range and NaN inputs of the
 * fix routines go to the C versions, which reproduce libgcc's saturation.
 */
	.text
	.align	2
	.globl	s31fp_floatsidf
	.type	s31fp_floatsidf, @function
s31fp_floatsidf:
	beqz	a0, 9f
	srai	t0, a0, 31
	xor	t1, a0, t0
	sub	t1, t1, t0		/* |x| */
	clz	t2, t1
	sll	t1, t1, t2		/* bit 31 = implicit one */
	li	t3, 1054
	sub	t3, t3, t2		/* 1023 + 31 - clz */
	slli	t4, t1, 1
	srli	a1, t4, 12
	slli	a0, t1, 21
	slli	t3, t3, 20
	or	a1, a1, t3
	slli	t0, t0, 31
	or	a1, a1, t0
	ret
9:	li	a1, 0
	ret
	.size	s31fp_floatsidf, . - s31fp_floatsidf

	.globl	s31fp_floatunsidf
	.type	s31fp_floatunsidf, @function
s31fp_floatunsidf:
	beqz	a0, 9f
	clz	t2, a0
	sll	t1, a0, t2
	li	t3, 1054
	sub	t3, t3, t2
	slli	t4, t1, 1
	srli	a1, t4, 12
	slli	a0, t1, 21
	slli	t3, t3, 20
	or	a1, a1, t3
	ret
9:	li	a1, 0
	ret
	.size	s31fp_floatunsidf, . - s31fp_floatunsidf

	.globl	s31fp_fixdfsi
	.type	s31fp_fixdfsi, @function
s31fp_fixdfsi:
	srli	t0, a1, 20
	andi	t0, t0, 0x7ff
	addi	t0, t0, -1023		/* unbiased exponent */
	bltz	t0, 8f			/* |x| < 1 */
	li	t1, 31
	bge	t0, t1, 7f		/* too big, inf or NaN */
	lui	t2, 0x100
	addi	t3, t2, -1
	and	t3, a1, t3
	or	t3, t3, t2
	slli	t3, t3, 11
	srli	t4, a0, 21
	or	t3, t3, t4		/* 1.m as a 32-bit value, bit 31 set */
	sub	t1, t1, t0
	srl	t3, t3, t1
	srai	t4, a1, 31
	xor	a0, t3, t4
	sub	a0, a0, t4
	ret
8:	li	a0, 0
	ret
7:	tail	s31fp_fixdfsi_c
	.size	s31fp_fixdfsi, . - s31fp_fixdfsi
