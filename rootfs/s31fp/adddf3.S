/*
 * s31fp_adddf3 / s31fp_subdf3 - IEEE double add/subtract, rv32im + Zbb.
 *
 * Fast path: both operands normal; a same-sign add, or a subtract that needs
 * at most one bit of renormalisation. Anything else (zero, subnormal, inf,
 * NaN, heavy cancellation, exponent leaving the normal range) tail-calls the
 * C version with the arguments intact - the only changes ever made to a0-a3
 * before that point are the sign flip for subtract and the operand swap, both
 * of which the C version's result is invariant under.
 * Three guard bits with a sticky OR, round to nearest even, rounding carry
 * runs into the exponent field. Leaf, no stack.
 */
	.text
	.globl	s31fp_subdf3
	.type	s31fp_subdf3, @function
	.align	2
s31fp_subdf3:
	lui	t0, 0x80000
	xor	a3, a3, t0
	/* fall through */
	.globl	s31fp_adddf3
	.type	s31fp_adddf3, @function
s31fp_adddf3:
	srli	t0, a1, 20
	srli	t1, a3, 20
	andi	t0, t0, 0x7ff		/* ae */
	andi	t1, t1, 0x7ff		/* be */
	li	t2, 0x7fe
	addi	t3, t0, -1
	bgeu	t3, t2, .Lslow
	addi	t3, t1, -1
	bgeu	t3, t2, .Lslow
	slli	t3, a1, 1		/* magnitudes, sign dropped */
	slli	t4, a3, 1
	bltu	t4, t3, .Lordered
	bne	t3, t4, .Lswap
	bgeu	a0, a2, .Lordered
.Lswap:
	mv t5, a0;  mv a0, a2;  mv a2, t5
	mv t5, a1;  mv a1, a3;  mv a3, t5
	mv t5, t0;  mv t0, t1;  mv t1, t5
.Lordered:
	sub	t2, t0, t1		/* d = ae - be >= 0 */
	li	t3, 56
	bgeu	t2, t3, .Lret		/* b is below half an ulp of a */
	lui	t3, 0x100
	addi	t4, t3, -1
	and	a5, a1, t4
	and	a7, a3, t4
	or	a5, a5, t3
	or	a7, a7, t3
	slli	a5, a5, 3		/* A = a5:a4 = sig << 3 */
	srli	t5, a0, 29
	or	a5, a5, t5
	slli	a4, a0, 3
	slli	a7, a7, 3		/* B = a7:a6 */
	srli	t5, a2, 29
	or	a7, a7, t5
	slli	a6, a2, 3
	beqz	t2, .Laligned
	li	t5, 32
	bgeu	t2, t5, .Lfar
	sub	t5, t5, t2		/* 1 <= d <= 31 */
	sll	t3, a6, t5		/* bits shifted out */
	srl	a6, a6, t2
	sll	t4, a7, t5
	or	a6, a6, t4
	srl	a7, a7, t2
	snez	t3, t3
	or	a6, a6, t3
	j	.Laligned
.Lfar:					/* 32 <= d <= 55 */
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
.Laligned:
	xor	t5, a1, a3
	bltz	t5, .Lsub
	add	a4, a4, a6
	sltu	t3, a4, a6
	add	a5, a5, a7
	add	a5, a5, t3
	srli	t3, a5, 24		/* carried into bit 56? */
	beqz	t3, .Lround
	andi	t3, a4, 1
	srli	a4, a4, 1
	slli	t4, a5, 31
	or	a4, a4, t4
	or	a4, a4, t3
	srli	a5, a5, 1
	addi	t0, t0, 1
	li	t3, 0x7ff
	bne	t0, t3, .Lround
	j	.Linf			/* overflow: signed infinity */
.Lsub:
	sltu	t3, a4, a6
	sub	a4, a4, a6
	sub	a5, a5, a7
	sub	a5, a5, t3
	srli	t3, a5, 23
	bnez	t3, .Lround		/* still normalised */
	srli	t3, a5, 22
	beqz	t3, .Lslow		/* more than one bit cancelled */
	slli	a5, a5, 1
	srli	t3, a4, 31
	or	a5, a5, t3
	slli	a4, a4, 1
	addi	t0, t0, -1
	beqz	t0, .Lslow		/* fell into the subnormals */
.Lround:
	andi	t3, a4, 7
	srli	a4, a4, 3
	slli	t4, a5, 29
	or	a4, a4, t4
	srli	a5, a5, 3
	lui	t4, 0x100
	addi	t4, t4, -1
	and	a5, a5, t4
	slli	t0, t0, 20
	or	a5, a5, t0
	lui	t4, 0x80000
	and	t4, a1, t4		/* sign of the larger operand */
	or	a5, a5, t4
	li	t4, 4
	bltu	t3, t4, .Ldone
	bne	t3, t4, .Lup
	andi	t3, a4, 1
	beqz	t3, .Ldone
.Lup:
	addi	a4, a4, 1
	seqz	t3, a4
	add	a5, a5, t3
.Ldone:
	mv	a0, a4
	mv	a1, a5
.Lret:
	ret
.Linf:
	lui	t4, 0x80000
	and	t4, a1, t4
	lui	a1, 0x7ff00
	or	a1, a1, t4
	li	a0, 0
	ret
.Lslow:
	tail	s31fp_adddf3_c
	.size	s31fp_adddf3, . - s31fp_adddf3
