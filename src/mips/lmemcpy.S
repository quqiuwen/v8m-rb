/*********************************************************************************
 * Copyright (c) 2011 University of Science and Technology of China (PRC).
 * All rights reserved.
 *
 * This file is a part of glibc ; before you use, modify and/or redistribute
 * it, you should get the permission from the Network Computing and 
 * High-performance Algorithm Laboratory(http://lab508.ustc.edu.cn).
 *
 * Author(s)	: hxcao <chx319@mail.ustc.edu.cn>
 *********************************************************************************/
#include "reg-def.h"

/* void *memcpy(void *s1, const void *s2, size_t n);  */
	.text	
	.globl	lmemcpy
	.ent	lmemcpy,0
	.set	noreorder	
	.set	mips64	
lmemcpy:

	/* check if the copy size is less than 8. if so, we just copy byte by byte */
	slti	 t0,  a2, 8		 
	bne	 t0,  zero, L_less_8_byte
	move	 v0,  a0				/* exit value */

	/* check the last 3 bits of the address to see if the s1 & s2 are aligned. */
	xor	 t0,  a1,  a0		
	andi	 t0, 0x7
	bne	 t0,  zero, L_unaligned	/* unaligned case */

	/* the last 3 bits of s1 & s2 are the same. now check if the start address
	   is in the middle of a long long word */
	li	 t2,  65500
	bge	 a2,  t2, L_big_num
	andi	 t2,  a1, 0x7
	beq	 t2,  zero, L_cp_4_word	/* we are starting from the start of a long long word */
	li	 t1,  8
	dsub     t1,  t1,  t2

	/* we are starting from the middle, just copy until we meet the next aligned adress */
	dsubu	 a2,  t1
	ldr	 t0, 0( a1)		
	daddu	 a1,  t1
	sdr	 t0, 0( a0)
	daddu	 a0,  t1

	/* now copy 32 bytes(2 long long words) once */
L_cp_4_word:	
	/* check if there are more than 64bytes remaining */
	andi	 t0,  a2, 0x1f			/* 32 bytes mask : 11111 */
	beq	 t0,  a2, L_cp_1_word
	dsubu	 a3,  a2,  t0	
	daddu	 a3,  a1		 	/*  a3 : end address of loop */
	move	 a2,  t0			

	ldr	 t0,  0( a1)		
L_loop_cp_4_word:	
	ldr	 t1,  8( a1)	
	ldr	 t2,  16( a1)	
	ldr	 t3,  24( a1)	

	daddiu	 a0, 32
	daddiu	 a1, 32

	sdr	 t0, -32( a0)
	sdr	 t1, -24( a0)
	sdr	 t2, -16( a0)
	sdr	 t3,  -8( a0)
	bne	 a1,  a3, L_loop_cp_4_word
	ldr	 t0,  0( a1)		

	/* less than 32 bytes remaining, copy one long long word once */
L_cp_1_word:	
	andi	 t0,  a2, 0x7		
	beq	 t0,  a2, L_less_8_byte
	dsubu	 a3,  a2,  t0	
	daddu	 a3,  a1		
	move	 a2,  t0

	ld	 t0, 0( a1)
L_loop_cp_1_word:	
	daddiu	 a0, 8
	daddiu	 a1, 8	
	sd	 t0, -8( a0)
	bne	 a1,  a3, L_loop_cp_1_word
	ld	 t0, 0( a1)

	/* less than 8 bytes remaining, copy one byte once */
L_less_8_byte:	
	blez	 a2, L_finish	
	daddu	 a3,  a2,  a1
	lb	 t0, 0( a1)
L_loop_cp_1_byte:	
	daddiu	 a0, 1
	daddiu	 a1, 1
	sb	 t0, -1( a0)
	bne	 a1,  a3, L_loop_cp_1_byte
	lb	 t0, 0( a1)

L_finish:	
	jr	 ra			/* exit */
	nop

L_big_num:
	li	 t1,  8
	dsub     t1,  t1,  t2
	beq	 t1,  zero, L_big_cp_8_word	/* we are starting from the start of a long long word */

	/* we are starting from the middle, copy 8 bytes */
	dsubu	 a2,  t1
	ldr	 t0, 0( a1)		
	daddu	 a1,  t1
	sdr	 t0, 0( a0)
	daddu	 a0,  t1

	/* now copy 64 bytes(8 long long words) once */
L_big_cp_8_word:	
	/* check if there are more than 64bytes remaining */
	andi	 t0,  a2, 0x3f			/* 64 bytes mask : 111111 */
	beq	 t0,  a2, L_big_cp_1_word
	dsubu	 a3,  a2,  t0	
	daddu	 a3,  a1		 	/*  a3 : end address of loop */
	move	 a2,  t0			

	ld	 t0,  0( a1)		
L_big_loop_cp_8_word:	
	ld	 t1,  8( a1)	
	ld	 t2,  16( a1)	
	ld	 t3,  24( a1)	
	ld	 t4,  32( a1)	
	ld	 t5,  40( a1)	
	ld	 t6,  48( a1)	
	ld	 t7,  56( a1)	

	daddiu	 a0, 64
	daddiu	 a1, 64

	sd	 t0, -64( a0)
	sd	 t1, -56( a0)
	sd	 t2, -48( a0)
	sd	 t3, -40( a0)
	sd	 t4, -32( a0)
	sd	 t5, -24( a0)
	sd	 t6, -16( a0)
	sd	 t7,  -8( a0)
	bne	 a1,  a3, L_big_loop_cp_8_word
	ld	 t0,  0( a1)		

	/* less than 64 bytes remaining, copy one long long word once */
L_big_cp_1_word:	
	andi	 t0,  a2, 0x7		
	beq	 t0,  a2, L_big_less_8_byte
	dsubu	 a3,  a2,  t0	
	daddu	 a3,  a1		
	move	 a2,  t0

	ld	 t0, 0( a1)
L_big_loop_cp_1_word:	
	daddiu	 a0, 8
	daddiu	 a1, 8	
	sd	 t0, -8( a0)
	bne	 a1,  a3, L_big_loop_cp_1_word
	ld	 t0, 0( a1)

	/* less than 8 bytes remaining, copy one byte once */
L_big_less_8_byte:	
	blez	 a2, L_big_finish	
	daddu	 a3,  a2,  a1
	lb	 t0, 0( a1)
L_big_loop_1_byte:	
	daddiu	 a0, 1
	daddiu	 a1, 1
	sb	 t0, -1( a0)
	bne	 a1,  a3, L_big_loop_1_byte
	lb	 t0, 0( a1)

L_big_finish:	
	jr	 ra			/* exit */
	nop

	/* handle the unaligned case, use unaligned load/store instructions. 
	   s1 & s2 are both unaligned. we can only make one of them aligned */
L_unaligned:	
	andi	 t0,  a0, 0x7
	li	 a3,  8
	dsub	 a3,  a3, t0
	beq	 a3, 0, Lun_cp_1_word
	dsubu	 a2,  a3
	ldr	 t0, 0( a1)	
	ldl	 t0, 7( a1)
	daddu	 a1,  a3
	sdr	 t0, 0( a0)
	sdl	 t0, 7( a0)
	daddu	 a0,  a3


	/* now copy 64 bytes(8 long long words) once */
Lun_cp_8_word:	
	/* check if there are more than 64bytes remaining */
	andi	 t0,  a2, 0x3f			/* 64 bytes mask : 111111 */
	beq	 t0,  a2, Lun_cp_1_word
	dsubu	 a3,  a2,  t0	
	daddu	 a3,  a1		 	/*  a3 : end address of loop */
	move	 a2,  t0			

Lun_loop_8_word:	
	uld	 t0,  0( a1)		
	uld	 t1,  8( a1)	
	uld	 t2, 16( a1)
	uld	 t3, 24( a1)
	uld	 t4, 32( a1)
	uld	 t5, 40( a1)
	uld	 t6, 48( a1)
	uld	 t7, 56( a1)

	daddiu	 a0, 64
	daddiu	 a1, 64

	sdr	 t0, -64( a0)
	sdr	 t1, -56( a0)
	sdr	 t2, -48( a0)
	sdr	 t3, -40( a0)
	sdr	 t4, -32( a0)
	sdr	 t5, -24( a0)
	sdr	 t6, -16( a0)
	sdr	 t7,  -8( a0)
	bnel	 a1,  a3, Lun_loop_8_word
	nop


	/* copy one long long word once */
Lun_cp_1_word:	
	andi	 t0,  a2, 0x7
	beq	 t0,  a2, L_less_8_byte
	dsubu	 a3,  a2,  t0	
	daddu	 a3,  a1		
	move	 a2,  t0

	ldr	 t1, 0( a1)		
Lun_loop_1_word:	
	ldl	 t1, 7( a1)
	daddiu	 a0, 8
	daddiu	 a1, 8
	sdr	 t1, -8( a0)
	bne	 a1,  a3, Lun_loop_1_word
	ldr	 t1, 0( a1)		

	/* less than 8 bytes left */
	b	L_less_8_byte	

	move	 a2,  t0

	.end	lmemcpy
	.set	reorder
