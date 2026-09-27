/*
 * s31opl_out: exact replacement for OpenTyrian opl.c:355
 *     cval = (Bit32s)(step_amp*vol*cur_wform[i]*trem/16.0)
 * a1:a0 = step_amp, a3:a2 = vol, a4 = w (int16, sign-extended), a5 = trem.
 * Returns cval in a0. Algorithm and proof: fused2.c. The arguments stay
 * untouched until every fallback decision is made, so .Lref can tail-call
 * the original chain with them.
 */
	.text
	.option	norelax
	.globl	s31opl_out
	.type	s31opl_out, @function
	.align	2
s31opl_out:
#ifndef OPL_FPROBE
	frcsr	t0
	andi	t0, t0, 0xe1		/* frm | NX */
	addi	t0, t0, -1
	bnez	t0, .Lref		/* not (RNE and NX already set) */
#endif
	blez	a5, .Lref
	/* memo index: 64 entries x 32 B, keyed on both inputs' bits */
	xor	t1, a0, a1
	srli	t2, a1, 7
	xor	t1, t1, a3
	xor	t1, t1, t2
	andi	t1, t1, 63
	slli	t1, t1, 5		/* 32-byte entries */
	lla	t2, s31opl_memo
	add	t1, t1, t2
	lw	t2, 0(t1)
	bne	t2, a0, .Lmiss
	lw	t2, 4(t1)
	bne	t2, a1, .Lmiss
	lw	t2, 8(t1)
	bne	t2, a2, .Lmiss
	lw	t2, 12(t1)
	bne	t2, a3, .Lmiss
	lw	t3, 16(t1)		/* P low */
	lw	t4, 20(t1)		/* P high */
.Lhave:
	srli	t0, t4, 20		/* sign:exponent */
	addi	t2, t0, -1
	li	t5, 0x7fe
	bgeu	t2, t5, .Lref		/* P zero/subnormal/inf/NaN or negative */
#ifdef OPL_FPROBE
	beqz	a4, .Lw0		/* w = 0: NX iff the chain's P = sa*vol was inexact */
#else
	beqz	a4, .Lzero		/* w = 0: result 0 (NX is already set) */
#endif
	li	t5, 1079
	sub	t5, t5, t0		/* sh: V = (m*k) >> sh */
	li	t2, 85
	bge	t5, t2, .Lsmall		/* 0 < V < 1/2 */
	addi	t5, t5, -64		/* s2 = sh - 64 */
	blez	t5, .Lref		/* sh <= 64: |V| may be large - rare */
	srai	t2, a4, 31		/* sign mask of w */
	xor	a6, a4, t2
	sub	a6, a6, t2		/* |w| */
	mul	a6, a6, a5		/* k = |w|*trem <= 2^31 */
	slli	a7, t4, 12
	srli	a7, a7, 12
	bseti	a7, a7, 20		/* mh, 21 bits */
	mulhu	t6, t3, a6		/* hi(pl*k) */
	mul	t0, a7, a6		/* lo(mh*k) */
	mulhu	a7, a7, a6		/* hi(mh*k) */
	add	t0, t0, t6		/* w1 */
	sltu	t6, t0, t6
	add	a7, a7, t6		/* w2 */
	srl	t1, a7, t5		/* n = W >> sh */
	neg	t6, t5			/* 32 - s2 (mod 32) */
	sll	a7, a7, t6
	srl	t0, t0, t5
	or	t0, t0, a7		/* top 32 fraction bits */
#ifdef OPL_FPROBE
	beqz	t0, .Lref		/* inexactness not proven here: exact path */
#endif
	srli	t0, t0, 12		/* top 20 */
	beqz	t0, 1f
	addi	t6, t0, 1
	srli	t6, t6, 20
	bnez	t6, .Lref		/* within 2^-20 below the next integer */
	j	2f
1:	bnez	t1, .Lref		/* within 2^-20 above an integer n >= 1 */
2:
#ifdef OPL_FPROBE
	/* The chain is inexact here (nonzero fraction: fixdfsi raises NX), so
	 * an inexact probe is allowed to set NX. Two dynamic-rounding fadd.s on
	 * halfway values: 1+2^-24 and 1+3*2^-24 round to 1.0 and 1+2ulp under
	 * RNE (difference 2); RTZ/RDN/RUP/RMM give a difference of 1. */
	lui	t6, 0x3f800
	fmv.w.x	ft0, t6
	lui	t6, 0x33800
	fmv.w.x	ft1, t6
	lui	t6, 0x34400
	fmv.w.x	ft2, t6
	fadd.s	ft1, ft0, ft1
	fadd.s	ft2, ft0, ft2
	fmv.x.w	t6, ft1
	fmv.x.w	t0, ft2
	sub	t0, t0, t6
	addi	t0, t0, -2
	bnez	t0, .Lref
#endif
	xor	a0, t1, t2
	sub	a0, a0, t2		/* trunc toward zero, sign of w */
	ret
.Lw0:
#ifdef OPL_FPROBE
	lw	t0, 24(t1)		/* 0 exact, 1 inexact, 2 unknown */
	beqz	t0, .Lzero		/* result 0, no flag */
	addi	t0, t0, -1
	bnez	t0, .Lref
	/* inexact: fall through - result 0 in every mode, the probe sets NX */
#endif
.Lsmall:
#ifdef OPL_FPROBE
	li	t1, 0			/* 0 < V < 1/2: result 0 in every mode, but */
	li	t2, 0			/* the chain raises NX: probe sets it */
	j	2b
#endif
.Lzero:
	li	a0, 0
	ret
.Lmiss:					/* P = step_amp*vol with the exact v2 helper */
	addi	sp, sp, -32
	sw	ra, 28(sp)
	sw	a0, 0(sp)
	sw	a1, 4(sp)
	sw	a2, 8(sp)
	sw	a3, 12(sp)
	sw	a4, 16(sp)
	sw	a5, 20(sp)
	sw	t1, 24(sp)
	call	s31v2_muldf3
	mv	t3, a0
	mv	t4, a1
	lw	t1, 24(sp)
	sw	t3, 16(t1)		/* value first, then the keys */
	sw	t4, 20(t1)
	lw	a0, 0(sp)
	lw	a1, 4(sp)
	lw	a2, 8(sp)
	lw	a3, 12(sp)
#ifdef OPL_FPROBE
	/* Was sa*vol exact? Mantissas with trailing zeros stripped have
	 * la = 53-tza and lb = 53-tzb bits; the product has la+lb-1 or la+lb:
	 * tza+tzb >= 53 exact, <= 51 inexact, 52 unknown (and so is any
	 * operand that is not normal). */
	li	t6, 2
	li	t5, 0x7fe
	srli	t0, a1, 20
	andi	t0, t0, 0x7ff
	addi	t0, t0, -1
	bgeu	t0, t5, 3f
	srli	t0, a3, 20
	andi	t0, t0, 0x7ff
	addi	t0, t0, -1
	bgeu	t0, t5, 3f
	ctz	t0, a0
	bnez	a0, 4f
	slli	t0, a1, 12
	srli	t0, t0, 12
	bseti	t0, t0, 20
	ctz	t0, t0
	addi	t0, t0, 32
4:	ctz	t2, a2
	bnez	a2, 5f
	slli	t2, a3, 12
	srli	t2, t2, 12
	bseti	t2, t2, 20
	ctz	t2, t2
	addi	t2, t2, 32
5:	add	t0, t0, t2
	li	t6, 0
	li	t5, 53
	bge	t0, t5, 3f
	li	t6, 1
	li	t5, 52
	blt	t0, t5, 3f
	li	t6, 2
3:	sw	t6, 24(t1)		/* before the keys */
#endif
	sw	a0, 0(t1)
	sw	a1, 4(t1)
	sw	a2, 8(t1)
	sw	a3, 12(t1)
	lw	a4, 16(sp)
	lw	a5, 20(sp)
	lw	ra, 28(sp)
	addi	sp, sp, 32
	j	.Lhave
.Lref:
	tail	chain_ref
	.size	s31opl_out, . - s31opl_out

	.bss
	.align	3
	.globl	s31opl_memo
s31opl_memo:				/* keys start as 0:0, which P = 0*0 = 0 satisfies */
	.space	64 * 32
