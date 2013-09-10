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
/* void *memchr(void *s1, int ch, size_t n);  */
	.text
	.globl	lmemchr
	.ent	lmemchr,0
	.set	noreorder	
	.set	mips64	
lmemchr:
	/* check if the copy size is less than 8. if so, we just set byte by byte */
	andi	 a1,  a1, 0xff
	slti	 t0,  a2, 8		
	bne	 t0,  zero, L_less_8_byte

	/* check if the start address is aligned */
	andi	 t0,  a0, 0x7
	beq	 t0,  zero, L_aligned
	li	 t1, 8
	dsub	 t1,  t1,  t0	
	dadd	 t0,  a0,  t1
	dsubu	 a2,  a2,  t1
	uld	 t2, 0(a0)
	andi	 t3, t2, 0xff
L_unaligned:
	beq	 t3,  a1, L_less_8_byteef
	dsrl     t2,  t2, 8
	daddiu	 a0,  a0, 1		
	bne	 a0,  t0, L_unaligned	
	andi	 t3, t2, 0xff

L_aligned:

	/* use this to check whether there is a zero in the 8 bytes */
	/* (((X) - 0x0101010101010101LL) & ~(X) & 0x8080808080808080LL) */
	move	 t1,  a1
	dsll	 t1,  t1, 8
	or	 t1,  t1,  a1
	dsll	 t2, t1, 16
	or	 t2, t2, t1
	dsll32	 t3, t2, 0
	or	 t3, t3, t2

	li	 t1,0x101
	dsll	 t4, t1,16
	or	 t4, t4, t1
	dsll32	 t5, t4, 0
	or	 t5, t5, t4

	dsll 	 t6, t5, 7

	/* check one long long word once */
L_check_4_word:	
	andi	 t0,  a2, 0x1f		
	beq	 t0,  a2, L_less_8_byte
	dsubu	 a3,  a2,  t0		
	daddu	 a3,  a0,  a3			/*  a3 = end address of loop */	
	move	 a2,  t0	


L_loop_check_8_w:	
	ld	 t2, 0( a0)
	xor	 t1,  t2, t3	
	/* check if there is a zero in  t1. X =  t1 */
	/* (((X) - 0x0101010101010101LL) & ~(X) & 0x8080808080808080LL) */
	dsubu	 t7, t1 , t5
	not	 t4, t1
	and	 t7, t7, t4
	and	 t7, t7, t6
	bne	 t7,  zero,L_found
	daddiu	 a0,  a0, 8
	ld	 t2, 0( a0)
	xor	 t1,  t2, t3	
	dsubu	 t7, t1 , t5
	not	 t4, t1
	and	 t7, t7, t4
	and	 t7, t7, t6
	bne	 t7,  zero,L_found
	daddiu	 a0,  a0, 8
	ld	 t2, 0( a0)
	xor	 t1,  t2, t3	
	dsubu	 t7, t1 , t5
	not	 t4, t1
	and	 t7, t7, t4
	and	 t7, t7, t6
	bne	 t7,  zero,L_found
	daddiu	 a0,  a0, 8
	ld	 t2, 0( a0)
	xor	 t1,  t2, t3	
	dsubu	 t7, t1 , t5
	not	 t4, t1
	and	 t7, t7, t4
	and	 t7, t7, t6
	bne	 t7,  zero,L_found
	daddiu	 a0,  a0, 8
	bne	 a0,  a3, L_loop_check_8_w
	nop
	
	/* last 8 bytes */
L_less_8_byte:	
	blez	 a2, L_finish
	daddu	 a3,  a2,  a0
	lbu	 t3, 0( a0)
L_loop_check_1_byte:	
	beq	 t3,  a1, L_less_8_byteef
	nop
	daddiu	 a0,  a0, 1
	bne	 a0,  a3, L_loop_check_1_byte
	lbu	 t3, 0( a0)
L_finish:
	move	 v0,  zero		
	jr	 ra			/* not found, exit */
	nop

	/* we have found a character in the 8 bytes, now find it out */
L_found:
	xor	 t0,  a0, zero
	daddiu	 a0,  a0, -8
	lbu	 t3, 0(a0)
L_loop_check1:
	beq	 t3,  a1, L_less_8_byteef
	nop
	daddiu	 a0, 1
	bne	 a0, t0, L_loop_check1
	lbu	 t3, 0(a0)
L_less_8_byteef:
	move	 v0,  a0		/* set exit value */
	jr	 ra
	nop


	.end	lmemchr
	.set	reorder
