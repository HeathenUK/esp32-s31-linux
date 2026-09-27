000695d0 <__muldf3>:
   695d0:	addi	sp,sp,-32
   695d2:	frrm	t4
   695d6:	srli	a5,a1,0x14
   695da:	slli	a7,a1,0xc
   695de:	andi	a5,a5,2047
   695e2:	srli	a7,a7,0xc
   695e6:	srli	a1,a1,0x1f
   695e8:	beqz	a5,699aa <__muldf3+0x3da>
   695ec:	li	a4,2047
   695f0:	beq	a5,a4,6985e <__muldf3+0x28e>
   695f4:	srli	a4,a0,0x1d
   695f8:	slli	a7,a7,0x3
   695fa:	or	a7,a4,a7
   695fe:	addi	t6,a5,-1023
   69602:	bseti	a7,a7,0x17
   69606:	slli	t1,a0,0x3
   6960a:	li	a4,0
   6960c:	li	t5,0
   6960e:	li	a6,0
   69610:	srli	a0,a3,0x14
   69614:	andi	a5,a0,2047
   69618:	slli	a0,a3,0xc
   6961c:	srli	t0,a0,0xc
   69620:	srli	a3,a3,0x1f
   69622:	beqz	a5,69886 <__muldf3+0x2b6>
   69626:	li	t3,2047
   6962a:	beq	a5,t3,69986 <__muldf3+0x3b6>
   6962e:	addi	a5,a5,-1023
   69632:	li	a0,10
   69634:	add	t6,t6,a5
   69636:	blt	a0,a4,698da <__muldf3+0x30a>
   6963a:	slli	a0,t0,0x3
   6963e:	srli	a5,a2,0x1d
   69642:	or	a5,a5,a0
   69644:	xor	t3,a1,a3
   69648:	li	t2,2
   6964a:	slli	a0,a2,0x3
   6964e:	bseti	t0,a5,0x17
   69652:	mv	a1,t3
   69654:	li	a2,0
   69656:	bge	t2,a4,69676 <__muldf3+0xa6>
   6965a:	bset	a5,zero,a4
   6965e:	andi	a4,a5,1328
   69662:	bnez	a4,698da <__muldf3+0x30a>
   69666:	andi	a4,a5,576
   6966a:	bnez	a4,69a5e <__muldf3+0x48e>
   6966e:	andi	a5,a5,136
   69672:	bnez	a5,698d2 <__muldf3+0x302>
   69676:	srli	t5,t1,0x10
   6967a:	zext.h	a3,a0
   6967e:	srli	a5,a0,0x10
   69682:	zext.h	t1,t1
   69686:	mul	a2,a3,t1
   6968a:	sw	s3,16(sp)
   6968c:	mv	s3,a3
   6968e:	sw	s0,28(sp)
   69690:	sw	s1,24(sp)
   69692:	sw	s2,20(sp)
   69694:	sw	s4,12(sp)
   69696:	mv	s0,t5
   69698:	mul	a0,t5,a3
   6969c:	srli	a4,a2,0x10
   696a0:	mul	a3,a5,t1
   696a4:	add	a3,a3,a0
   696a6:	add	a4,a4,a3
   696a8:	mul	a3,t5,a5
   696ac:	bltu	a4,a0,69a58 <__muldf3+0x488>
   696b0:	zext.h	a0,t0
   696b4:	slli	t5,a4,0x10
   696b8:	srli	t2,t0,0x10
   696bc:	zext.h	a2,a2
   696c0:	add	s2,t5,a2
   696c4:	srli	s1,a4,0x10
   696c8:	mul	a2,s0,a0
   696cc:	mul	a4,t1,a0
   696d0:	mul	t1,t2,t1
   696d4:	srli	t5,a4,0x10
   696d8:	add	t1,t1,a2
   696da:	add	t5,t5,t1
   696dc:	mul	t0,s0,t2
   696e0:	bgeu	t5,a2,696e8 <__muldf3+0x118>
   696e4:	lui	t1,0x10
   696e6:	add	t0,t0,t1
   696e8:	srli	a2,a7,0x10
   696ec:	zext.h	t1,a4
   696f0:	zext.h	a7,a7
   696f4:	slli	a4,t5,0x10
   696f8:	srli	t5,t5,0x10
   696fc:	add	a4,a4,t1
   696fe:	mul	s0,a5,a7
   69702:	add	s4,t5,t0
   69706:	mul	t1,s3,a7
   6970a:	mul	t5,a2,s3
   6970e:	srli	t0,t1,0x10
   69712:	add	s3,s1,a4
   69716:	add	s0,s0,t5
   69718:	add	t0,t0,s0
   6971a:	mul	a5,a5,a2
   6971e:	bgeu	t0,t5,69726 <__muldf3+0x156>
   69722:	lui	s0,0x10
   69724:	add	a5,a5,s0
   69726:	zext.h	t1,t1
   6972a:	slli	s0,t0,0x10
   6972e:	add	s0,s0,t1
   69730:	srli	t0,t0,0x10
   69734:	mv	s1,s0
   69736:	add	s0,t0,a5
   6973a:	mul	a5,a0,a7
   6973e:	mul	t0,a2,a0
   69742:	srli	a0,a5,0x10
   69746:	mul	a7,t2,a7
   6974a:	add	a7,a7,t0
   6974c:	add	t1,a0,a7
   69750:	mul	t2,t2,a2
   69754:	bgeu	t1,t0,6975c <__muldf3+0x18c>
   69758:	lui	a0,0x10
   6975a:	add	t2,t2,a0
   6975c:	zext.h	a0,a5
   69760:	slli	a5,t1,0x10
   69764:	add	a3,a3,s3
   69766:	add	a5,a5,a0
   69768:	add	a7,a5,s4
   6976c:	sltu	a4,a3,a4
   69770:	add	t5,a3,s1
   69774:	add	a4,a4,a7
   69776:	add	a0,a4,s0
   6977a:	sltu	a3,t5,a3
   6977e:	add	a3,a3,a0
   69780:	sltu	a5,a7,a5
   69784:	sltu	a7,a4,a7
   69788:	or	a5,a5,a7
   6978c:	sltu	a4,a0,a4
   69790:	srli	a7,t1,0x10
   69794:	sltu	a0,a3,a0
   69798:	add	a5,a5,a7
   6979a:	or	a4,a4,a0
   6979c:	slli	t1,t5,0x9
   697a0:	add	a4,a4,a5
   697a2:	or	t1,t1,s2
   697a6:	add	a4,a4,t2
   697a8:	slli	a4,a4,0x9
   697aa:	snez	t1,t1
   697ae:	srli	t5,t5,0x17
   697b2:	srli	a7,a3,0x17
   697b6:	or	t1,t1,t5
   697ba:	slli	a3,a3,0x9
   697bc:	slli	a5,a4,0x7
   697c0:	or	a7,a4,a7
   697c4:	or	t1,t1,a3
   697c8:	bltz	a5,69908 <__muldf3+0x338>
   697cc:	addi	a4,t6,1023
   697d0:	blez	a4,69a80 <__muldf3+0x4b0>
   697d4:	andi	a5,t1,7
   697d8:	beqz	a5,69c9c <__muldf3+0x6cc>
   697dc:	li	a5,2
   697de:	ori	a6,a6,1
   697e2:	beq	t4,a5,69d96 <__muldf3+0x7c6>
   697e6:	li	a5,3
   697e8:	beq	t4,a5,69dc2 <__muldf3+0x7f2>
   697ec:	bnez	t4,69dae <__muldf3+0x7de>
   697f0:	andi	a3,t1,15
   697f4:	li	a2,4
   697f6:	mv	a5,t6
   697f8:	beq	a3,a2,69c9c <__muldf3+0x6cc>
   697fc:	lw	s0,28(sp)
   697fe:	lw	s1,24(sp)
   69800:	lw	s2,20(sp)
   69802:	lw	s3,16(sp)
   69804:	lw	s4,12(sp)
   69806:	addi	a3,t1,4 # 10004 <__ehdr_start+0x10004>
   6980a:	sltu	t1,a3,t1
   6980e:	add	a7,a7,t1
   69810:	mv	t1,a3
   69812:	slli	a3,a7,0x7
   69816:	bgez	a3,69822 <__muldf3+0x252>
   6981a:	bclri	a7,a7,0x18
   6981e:	addi	a4,a5,1024
   69822:	li	a5,2046
   69826:	bge	a5,a4,6996c <__muldf3+0x39c>
   6982a:	li	a5,2
   6982c:	beq	t4,a5,69c76 <__muldf3+0x6a6>
   69830:	li	a5,3
   69832:	beq	t4,a5,69c68 <__muldf3+0x698>
   69836:	beqz	t4,69c6c <__muldf3+0x69c>
   6983a:	lui	a4,0x100
   6983e:	addi	a4,a4,-1 # fffff <builtin_tls+0x550bf>
   69840:	li	a5,2046
   69844:	li	a3,-1
   69846:	slli	a5,a5,0x14
   69848:	or	a5,a5,a4
   6984a:	slli	t3,t3,0x1f
   6984c:	mv	a0,a3
   6984e:	or	a1,a5,t3
   69852:	ori	a6,a6,5
   69856:	csrs	fflags,a6
   6985a:	addi	sp,sp,32
   6985c:	ret
   6985e:	or	t1,a7,a0
   69862:	bnez	t1,699f4 <__muldf3+0x424>
   69866:	srli	a0,a3,0x14
   6986a:	mv	t6,a5
   6986c:	andi	a5,a0,2047
   69870:	slli	a0,a3,0xc
   69874:	li	a7,0
   69876:	li	a4,8
   69878:	li	t5,2
   6987a:	li	a6,0
   6987c:	srli	t0,a0,0xc
   69880:	srli	a3,a3,0x1f
   69882:	bnez	a5,69626 <__muldf3+0x56>
   69886:	or	a0,t0,a2
   6988a:	beqz	a0,69a08 <__muldf3+0x438>
   6988e:	beqz	t0,69b44 <__muldf3+0x574>
   69892:	clz	t3,t0
   69896:	addi	a5,t3,-11
   6989a:	li	t2,29
   6989c:	sub	t2,t2,a5
   698a0:	addi	a5,t3,-8
   698a4:	sll	a0,t0,a5
   698a8:	srl	t2,a2,t2
   698ac:	or	t0,t2,a0
   698b0:	sll	a0,a2,a5
   698b4:	sub	t3,t6,t3
   698b8:	li	a5,10
   698ba:	addi	t6,t3,-1011
   698be:	blt	a5,a4,698da <__muldf3+0x30a>
   698c2:	xor	t3,a1,a3
   698c6:	li	a5,2
   698c8:	mv	a1,t3
   698ca:	li	a2,0
   698cc:	blt	a5,a4,6965a <__muldf3+0x8a>
   698d0:	j	69676 <__muldf3+0xa6>
   698d2:	mv	a1,a3
   698d4:	mv	a7,t0
   698d6:	mv	t1,a0
   698d8:	mv	t5,a2
   698da:	li	a5,2
   698dc:	beq	t5,a5,69b22 <__muldf3+0x552>
   698e0:	li	a5,3
   698e2:	beq	t5,a5,69a72 <__muldf3+0x4a2>
   698e6:	li	a5,1
   698e8:	bne	t5,a5,6992a <__muldf3+0x35a>
   698ec:	mv	t3,a1
   698ee:	li	a5,0
   698f0:	li	a4,0
   698f2:	li	a3,0
   698f4:	slli	a5,a5,0x14
   698f6:	or	a5,a5,a4
   698f8:	slli	t3,t3,0x1f
   698fa:	mv	a0,a3
   698fc:	or	a1,a5,t3
   69900:	bnez	a6,69856 <__muldf3+0x286>
   69904:	addi	sp,sp,32
   69906:	ret
   69908:	srli	a5,t1,0x1
   6990c:	lw	s0,28(sp)
   6990e:	andi	t1,t1,1
   69912:	lw	s1,24(sp)
   69914:	lw	s2,20(sp)
   69916:	lw	s3,16(sp)
   69918:	lw	s4,12(sp)
   6991a:	or	a5,a5,t1
   6991e:	slli	t1,a7,0x1f
   69922:	or	t1,a5,t1
   69926:	srli	a7,a7,0x1
   6992a:	addi	a4,t6,1024
   6992e:	addi	a5,t6,1
   69932:	mv	t3,a1
   69934:	blez	a4,69a8c <__muldf3+0x4bc>
   69938:	andi	a3,t1,7
   6993c:	beqz	a3,69812 <__muldf3+0x242>
   69940:	li	a3,2
   69942:	ori	a6,a6,1
   69946:	beq	t4,a3,69c4e <__muldf3+0x67e>
   6994a:	li	a3,3
   6994c:	beq	t4,a3,69cc2 <__muldf3+0x6f2>
   69950:	beqz	t4,69cb6 <__muldf3+0x6e6>
   69954:	slli	a5,a7,0x7
   69958:	bgez	a5,69d7c <__muldf3+0x7ac>
   6995c:	addi	a4,t6,1025
   69960:	li	a5,2046
   69964:	bclri	a7,a7,0x18
   69968:	blt	a5,a4,69830 <__muldf3+0x260>
   6996c:	srli	t1,t1,0x3
   69970:	slli	a3,a7,0x1d
   69974:	slli	a2,a7,0x9
   69978:	andi	a5,a4,2047
   6997c:	or	a3,a3,t1
   69980:	srli	a4,a2,0xc
   69984:	j	698f4 <__muldf3+0x324>
   69986:	or	a0,t0,a2
   6998a:	addi	t6,t6,2047
   6998e:	bnez	a0,69a34 <__muldf3+0x464>
   69990:	ori	a5,a4,2
   69994:	li	a2,10
   69996:	blt	a2,a5,698da <__muldf3+0x30a>
   6999a:	xor	a1,a1,a3
   6999c:	mv	t3,a1
   6999e:	li	a2,2
   699a0:	beqz	a4,69a20 <__muldf3+0x450>
   699a2:	mv	a4,a5
   699a4:	li	t0,0
   699a6:	li	a2,2
   699a8:	j	6965a <__muldf3+0x8a>
   699aa:	or	t1,a7,a0
   699ae:	beqz	t1,699e8 <__muldf3+0x418>
   699b2:	beqz	a7,69b60 <__muldf3+0x590>
   699b6:	clz	a4,a7
   699ba:	addi	a6,a4,-11
   699be:	li	a5,29
   699c0:	addi	t1,a4,-8
   699c4:	sub	a5,a5,a6
   699c8:	sll	a7,a7,t1
   699cc:	srl	a5,a0,a5
   699d0:	or	a7,a5,a7
   699d4:	sll	t1,a0,t1
   699d8:	li	a5,-1011
   699dc:	sub	t6,a5,a4
   699e0:	li	t5,0
   699e2:	li	a4,0
   699e4:	li	a6,0
   699e6:	j	69610 <__muldf3+0x40>
   699e8:	li	a7,0
   699ea:	li	a4,4
   699ec:	li	t6,0
   699ee:	li	t5,1
   699f0:	li	a6,0
   699f2:	j	69610 <__muldf3+0x40>
   699f4:	lui	a6,0x80
   699f8:	sltu	a6,a7,a6
   699fc:	mv	t1,a0
   699fe:	mv	t6,a5
   69a00:	slli	a6,a6,0x4
   69a02:	li	a4,12
   69a04:	li	t5,3
   69a06:	j	69610 <__muldf3+0x40>
   69a08:	ori	a4,a4,1
   69a0c:	li	a5,10
   69a0e:	blt	a5,a4,698da <__muldf3+0x30a>
   69a12:	xor	a1,a1,a3
   69a14:	li	a5,2
   69a16:	mv	t3,a1
   69a18:	li	t0,0
   69a1a:	li	a2,1
   69a1c:	blt	a5,a4,6965a <__muldf3+0x8a>
   69a20:	li	a5,2
   69a22:	beq	a2,a5,69b22 <__muldf3+0x552>
   69a26:	li	t5,1
   69a28:	li	a5,1
   69a2a:	li	a7,0
   69a2c:	li	t1,0
   69a2e:	bne	t5,a5,6992a <__muldf3+0x35a>
   69a32:	j	698ec <__muldf3+0x31c>
   69a34:	lui	a0,0x80
   69a38:	ori	a4,a4,3
   69a3c:	bgeu	t0,a0,69b2e <__muldf3+0x55e>
   69a40:	li	a6,10
   69a42:	blt	a6,a4,69b0e <__muldf3+0x53e>
   69a46:	xor	t3,a1,a3
   69a4a:	mv	a0,a2
   69a4c:	bset	a5,zero,a4
   69a50:	mv	a1,t3
   69a52:	li	a6,16
   69a54:	li	a2,3
   69a56:	j	6966e <__muldf3+0x9e>
   69a58:	lui	a0,0x10
   69a5a:	add	a3,a3,a0
   69a5c:	j	696b0 <__muldf3+0xe0>
   69a5e:	auipc	a5,0xa
   69a62:	addi	a5,a5,1746 # 74130 <tbl+0x1810>
   69a66:	lw	a0,0(a5)
   69a68:	lw	a1,4(a5)
   69a6a:	li	a6,16
   69a6c:	csrs	fflags,a6
   69a70:	j	6985a <__muldf3+0x28a>
   69a72:	li	t3,0
   69a74:	li	a5,2047
   69a78:	lui	a4,0x80
   69a7c:	li	a3,0
   69a7e:	j	698f4 <__muldf3+0x324>
   69a80:	lw	s0,28(sp)
   69a82:	lw	s1,24(sp)
   69a84:	lw	s2,20(sp)
   69a86:	lw	s3,16(sp)
   69a88:	lw	s4,12(sp)
   69a8a:	mv	a5,t6
   69a8c:	bnez	a4,69b86 <__muldf3+0x5b6>
   69a8e:	andi	a4,t1,7
   69a92:	bnez	a4,69c1a <__muldf3+0x64a>
   69a96:	bexti	a2,a7,0x18
   69a9a:	xori	a2,a2,1
   69a9e:	li	a4,1
   69aa0:	addi	a5,a5,1054
   69aa4:	sll	a3,t1,a5
   69aa8:	snez	a3,a3
   69aac:	sll	a5,a7,a5
   69ab0:	or	a5,a5,a3
   69ab2:	srl	t1,t1,a4
   69ab6:	or	t1,t1,a5
   69aba:	andi	a5,t1,7
   69abe:	srl	a4,a7,a4
   69ac2:	beqz	a5,69aee <__muldf3+0x51e>
   69ac4:	li	a5,2
   69ac6:	ori	a6,a6,1
   69aca:	beq	t4,a5,69e2a <__muldf3+0x85a>
   69ace:	li	a5,3
   69ad0:	beq	t4,a5,69e18 <__muldf3+0x848>
   69ad4:	bnez	t4,69aee <__muldf3+0x51e>
   69ad8:	andi	a5,t1,15
   69adc:	li	a3,4
   69ade:	beq	a5,a3,69aee <__muldf3+0x51e>
   69ae2:	add	a5,t1,a3
   69ae6:	sltu	t1,a5,t1
   69aea:	add	a4,a4,t1
   69aec:	mv	t1,a5
   69aee:	slli	a5,a4,0x8
   69af2:	bgez	a5,69df2 <__muldf3+0x822>
   69af6:	ori	a6,a6,1
   69afa:	bnez	a2,69ce4 <__muldf3+0x714>
   69afe:	slli	a5,t3,0x1f
   69b02:	li	a0,0
   69b04:	bseti	a1,a5,0x14
   69b08:	csrs	fflags,a6
   69b0c:	j	6985a <__muldf3+0x28a>
   69b0e:	li	a3,15
   69b10:	bne	a4,a3,69b7c <__muldf3+0x5ac>
   69b14:	li	a0,0
   69b16:	lui	a1,0x7ff80
   69b1a:	li	a6,16
   69b1c:	csrs	fflags,a6
   69b20:	j	6985a <__muldf3+0x28a>
   69b22:	mv	t3,a1
   69b24:	li	a5,2047
   69b28:	li	a4,0
   69b2a:	li	a3,0
   69b2c:	j	698f4 <__muldf3+0x324>
   69b2e:	li	t3,10
   69b30:	blt	t3,a4,69c7a <__muldf3+0x6aa>
   69b34:	xor	t3,a1,a3
   69b38:	mv	a0,a2
   69b3a:	bset	a5,zero,a4
   69b3e:	mv	a1,t3
   69b40:	li	a2,3
   69b42:	j	6966e <__muldf3+0x9e>
   69b44:	clz	a0,a2
   69b48:	addi	a5,a0,21 # 10015 <__ehdr_start+0x10015>
   69b4c:	li	t2,28
   69b4e:	addi	t3,a0,32
   69b52:	bge	t2,a5,6989a <__muldf3+0x2ca>
   69b56:	addi	a0,a0,-8
   69b58:	sll	t0,a2,a0
   69b5c:	li	a0,0
   69b5e:	j	698b4 <__muldf3+0x2e4>
   69b60:	clz	a5,a0
   69b64:	addi	a6,a5,21
   69b68:	li	t1,28
   69b6a:	addi	a4,a5,32
   69b6e:	bge	t1,a6,699be <__muldf3+0x3ee>
   69b72:	addi	a5,a5,-8
   69b74:	sll	a7,a0,a5
   69b78:	li	t1,0
   69b7a:	j	699d8 <__muldf3+0x408>
   69b7c:	mv	a4,a0
   69b7e:	li	t3,0
   69b80:	li	a3,0
   69b82:	li	a6,16
   69b84:	j	698f4 <__muldf3+0x324>
   69b86:	li	a3,-55
   69b8a:	blt	a4,a3,69c02 <__muldf3+0x632>
   69b8e:	li	a3,-30
   69b90:	bge	a4,a3,69e52 <__muldf3+0x882>
   69b94:	li	a3,-31
   69b96:	sub	a2,a3,a4
   69b9a:	srl	a2,a7,a2
   69b9e:	beq	a4,a3,69bae <__muldf3+0x5de>
   69ba2:	addi	a5,a5,1086
   69ba6:	sll	a5,a7,a5
   69baa:	or	t1,t1,a5
   69bae:	snez	a3,t1
   69bb2:	or	a3,a3,a2
   69bb4:	andi	a5,a3,7
   69bb8:	beqz	a5,69cdc <__muldf3+0x70c>
   69bbc:	li	a5,2
   69bbe:	ori	a6,a6,1
   69bc2:	beq	t4,a5,69e30 <__muldf3+0x860>
   69bc6:	li	a5,3
   69bc8:	beq	t4,a5,69e3c <__muldf3+0x86c>
   69bcc:	bnez	t4,69d74 <__muldf3+0x7a4>
   69bd0:	andi	a5,a3,15
   69bd4:	li	a4,4
   69bd6:	beq	a5,a4,69d74 <__muldf3+0x7a4>
   69bda:	add	a5,a3,a4
   69bde:	sltu	a3,a5,a3
   69be2:	srli	a5,a5,0x3
   69be4:	slli	a3,a3,0x1d
   69be6:	or	a3,a3,a5
   69be8:	li	a4,0
   69bea:	li	a5,0
   69bec:	slli	a5,a5,0x14
   69bee:	or	a5,a5,a4
   69bf0:	slli	t3,t3,0x1f
   69bf2:	mv	a0,a3
   69bf4:	or	a1,a5,t3
   69bf8:	ori	a6,a6,2
   69bfc:	csrs	fflags,a6
   69c00:	j	6985a <__muldf3+0x28a>
   69c02:	or	t1,a7,t1
   69c06:	bnez	t1,69c82 <__muldf3+0x6b2>
   69c0a:	mv	a0,t1
   69c0c:	slli	a1,t3,0x1f
   69c10:	ori	a6,a6,2
   69c14:	csrs	fflags,a6
   69c18:	j	6985a <__muldf3+0x28a>
   69c1a:	li	a4,2
   69c1c:	ori	a6,a6,1
   69c20:	beq	t4,a4,69ca8 <__muldf3+0x6d8>
   69c24:	li	a4,3
   69c26:	beq	t4,a4,69d5c <__muldf3+0x78c>
   69c2a:	bnez	t4,69caa <__muldf3+0x6da>
   69c2e:	andi	a4,t1,15
   69c32:	li	a3,4
   69c34:	beq	a4,a3,69caa <__muldf3+0x6da>
   69c38:	sltiu	a2,t1,-4
   69c3c:	seqz	a2,a2
   69c40:	add	a2,a2,a7
   69c42:	bexti	a2,a2,0x18
   69c46:	xori	a2,a2,1
   69c4a:	li	a4,1
   69c4c:	j	69aa0 <__muldf3+0x4d0>
   69c4e:	bnez	a1,69cf8 <__muldf3+0x728>
   69c50:	slli	a5,a7,0x7
   69c54:	bgez	a5,69da2 <__muldf3+0x7d2>
   69c58:	addi	a4,t6,1025
   69c5c:	li	a5,2046
   69c60:	bge	a5,a4,69d86 <__muldf3+0x7b6>
   69c64:	li	t3,0
   69c66:	j	6983a <__muldf3+0x26a>
   69c68:	bnez	a1,6983a <__muldf3+0x26a>
   69c6c:	li	a5,2047
   69c70:	li	a4,0
   69c72:	li	a3,0
   69c74:	j	69846 <__muldf3+0x276>
   69c76:	bnez	a1,69c6c <__muldf3+0x69c>
   69c78:	j	6983a <__muldf3+0x26a>
   69c7a:	mv	a4,a0
   69c7c:	li	t3,0
   69c7e:	li	a3,0
   69c80:	j	698f4 <__muldf3+0x324>
   69c82:	li	a5,2
   69c84:	ori	a6,a6,1
   69c88:	mv	t1,a1
   69c8a:	beq	t4,a5,69c0a <__muldf3+0x63a>
   69c8e:	li	a5,3
   69c90:	li	t1,0
   69c92:	bne	t4,a5,69c0a <__muldf3+0x63a>
   69c96:	xori	t1,a1,1
   69c9a:	j	69c0a <__muldf3+0x63a>
   69c9c:	lw	s0,28(sp)
   69c9e:	lw	s1,24(sp)
   69ca0:	lw	s2,20(sp)
   69ca2:	lw	s3,16(sp)
   69ca4:	lw	s4,12(sp)
   69ca6:	j	69822 <__muldf3+0x252>
   69ca8:	bnez	a1,69d5e <__muldf3+0x78e>
   69caa:	bexti	a2,a7,0x18
   69cae:	xori	a2,a2,1
   69cb2:	li	a4,1
   69cb4:	j	69aa0 <__muldf3+0x4d0>
   69cb6:	andi	a3,t1,15
   69cba:	li	a2,4
   69cbc:	bne	a3,a2,69806 <__muldf3+0x236>
   69cc0:	j	69812 <__muldf3+0x242>
   69cc2:	beqz	a1,69d32 <__muldf3+0x762>
   69cc4:	slli	a5,a7,0x7
   69cc8:	bgez	a5,69dce <__muldf3+0x7fe>
   69ccc:	addi	a4,t6,1025
   69cd0:	li	a5,2046
   69cd4:	bge	a5,a4,69d8e <__muldf3+0x7be>
   69cd8:	li	t3,1
   69cda:	j	69830 <__muldf3+0x260>
   69cdc:	srli	a3,a3,0x3
   69cde:	li	a5,0
   69ce0:	li	a4,0
   69ce2:	j	698f4 <__muldf3+0x324>
   69ce4:	li	a4,0
   69ce6:	li	a5,1
   69ce8:	li	a3,0
   69cea:	j	69bec <__muldf3+0x61c>
   69cec:	lw	s0,28(sp)
   69cee:	lw	s1,24(sp)
   69cf0:	lw	s2,20(sp)
   69cf2:	lw	s3,16(sp)
   69cf4:	lw	s4,12(sp)
   69cf6:	mv	a5,t6
   69cf8:	addi	a2,t1,8
   69cfc:	sltu	t1,a2,t1
   69d00:	add	a7,a7,t1
   69d02:	lui	a3,0x1000
   69d06:	and	a3,a7,a3
   69d0a:	mv	t1,a2
   69d0c:	beqz	a3,69de0 <__muldf3+0x810>
   69d0e:	addi	a4,a5,1024
   69d12:	li	a5,2046
   69d16:	bge	a5,a4,69d8e <__muldf3+0x7be>
   69d1a:	li	t3,1
   69d1c:	li	a5,2
   69d1e:	mv	a1,t3
   69d20:	bne	t4,a5,69830 <__muldf3+0x260>
   69d24:	j	69c6c <__muldf3+0x69c>
   69d26:	lw	s0,28(sp)
   69d28:	lw	s1,24(sp)
   69d2a:	lw	s2,20(sp)
   69d2c:	lw	s3,16(sp)
   69d2e:	lw	s4,12(sp)
   69d30:	mv	a5,t6
   69d32:	addi	a3,t1,8
   69d36:	sltu	t1,a3,t1
   69d3a:	add	a7,a7,t1
   69d3c:	mv	t1,a3
   69d3e:	slli	a3,a7,0x7
   69d42:	bgez	a3,69e0c <__muldf3+0x83c>
   69d46:	addi	a4,a5,1024
   69d4a:	li	a5,2046
   69d4e:	bge	a5,a4,69d86 <__muldf3+0x7b6>
   69d52:	li	a5,2
   69d54:	li	t3,0
   69d56:	bne	t4,a5,69c6c <__muldf3+0x69c>
   69d5a:	j	6983a <__muldf3+0x26a>
   69d5c:	bnez	a1,69caa <__muldf3+0x6da>
   69d5e:	sltiu	a2,t1,-8
   69d62:	seqz	a2,a2
   69d66:	add	a2,a2,a7
   69d68:	bexti	a2,a2,0x18
   69d6c:	xori	a2,a2,1
   69d70:	li	a4,1
   69d72:	j	69aa0 <__muldf3+0x4d0>
   69d74:	srli	a3,a3,0x3
   69d76:	li	a4,0
   69d78:	li	a5,0
   69d7a:	j	69bec <__muldf3+0x61c>
   69d7c:	li	a5,2046
   69d80:	blt	a5,a4,69836 <__muldf3+0x266>
   69d84:	j	6996c <__muldf3+0x39c>
   69d86:	bclri	a7,a7,0x18
   69d8a:	li	t3,0
   69d8c:	j	6996c <__muldf3+0x39c>
   69d8e:	bclri	a7,a7,0x18
   69d92:	li	t3,1
   69d94:	j	6996c <__muldf3+0x39c>
   69d96:	bnez	a1,69cec <__muldf3+0x71c>
   69d98:	lw	s0,28(sp)
   69d9a:	lw	s1,24(sp)
   69d9c:	lw	s2,20(sp)
   69d9e:	lw	s3,16(sp)
   69da0:	lw	s4,12(sp)
   69da2:	li	a5,2046
   69da6:	li	t3,0
   69da8:	blt	a5,a4,6983a <__muldf3+0x26a>
   69dac:	j	6996c <__muldf3+0x39c>
   69dae:	li	a5,2046
   69db2:	lw	s0,28(sp)
   69db4:	lw	s1,24(sp)
   69db6:	lw	s2,20(sp)
   69db8:	lw	s3,16(sp)
   69dba:	lw	s4,12(sp)
   69dbc:	blt	a5,a4,6983a <__muldf3+0x26a>
   69dc0:	j	6996c <__muldf3+0x39c>
   69dc2:	beqz	a1,69d26 <__muldf3+0x756>
   69dc4:	lw	s0,28(sp)
   69dc6:	lw	s1,24(sp)
   69dc8:	lw	s2,20(sp)
   69dca:	lw	s3,16(sp)
   69dcc:	lw	s4,12(sp)
   69dce:	li	a5,2046
   69dd2:	bge	a5,a4,69e4e <__muldf3+0x87e>
   69dd6:	li	a5,3
   69dd8:	li	t3,1
   69dda:	bne	t4,a5,69836 <__muldf3+0x266>
   69dde:	j	6983a <__muldf3+0x26a>
   69de0:	li	a5,2046
   69de4:	li	t3,1
   69de6:	bge	a5,a4,6996c <__muldf3+0x39c>
   69dea:	li	a5,2047
   69dee:	li	a4,0
   69df0:	j	69846 <__muldf3+0x276>
   69df2:	slli	a3,a4,0x1d
   69df6:	srli	t1,t1,0x3
   69dfa:	slli	a5,a4,0x9
   69dfe:	or	a3,a3,t1
   69e02:	srli	a4,a5,0xc
   69e06:	bnez	a2,69e42 <__muldf3+0x872>
   69e08:	li	a5,0
   69e0a:	j	698f4 <__muldf3+0x324>
   69e0c:	li	a5,2046
   69e10:	li	t3,0
   69e12:	blt	a5,a4,69c6c <__muldf3+0x69c>
   69e16:	j	6996c <__muldf3+0x39c>
   69e18:	bnez	a1,69aee <__muldf3+0x51e>
   69e1c:	addi	a5,t1,8
   69e20:	sltu	t1,a5,t1
   69e24:	add	a4,a4,t1
   69e26:	mv	t1,a5
   69e28:	j	69aee <__muldf3+0x51e>
   69e2a:	beqz	a1,69aee <__muldf3+0x51e>
   69e2e:	j	69e1c <__muldf3+0x84c>
   69e30:	beqz	a1,69d74 <__muldf3+0x7a4>
   69e32:	addi	a5,a3,8 # 1000008 <builtin_tls+0xf550c8>
   69e36:	sltu	a3,a5,a3
   69e3a:	j	69be2 <__muldf3+0x612>
   69e3c:	beqz	a1,69e32 <__muldf3+0x862>
   69e3e:	srli	a3,a3,0x3
   69e40:	li	a4,0
   69e42:	andi	a2,a6,1
   69e46:	li	a5,0
   69e48:	beqz	a2,698f4 <__muldf3+0x324>
   69e4c:	j	69bec <__muldf3+0x61c>
   69e4e:	li	t3,1
   69e50:	j	6996c <__muldf3+0x39c>
   69e52:	li	a2,1
   69e54:	sub	a4,a2,a4
   69e58:	j	69aa0 <__muldf3+0x4d0>

