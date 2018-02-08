/*
 * compiler/codegen_arm.cpp - ARM code generator
 *
 * Copyright (c) 2013 Jens Heitmann of ARAnyM dev team (see AUTHORS)
 *
 * Inspired by Christian Bauer's Basilisk II
 *
 * This file is part of the ARAnyM project which builds a new and powerful
 * TOS/FreeMiNT compatible virtual machine running on almost any hardware.
 *
 * JIT compiler m68k -> ARM
 *
 * Original 68040 JIT compiler for UAE, copyright 2000-2002 Bernd Meyer
 * Adaptation for Basilisk II and improvements, copyright 2000-2004 Gwenole Beauchesne
 * Portions related to CPU detection come from linux/arch/i386/kernel/setup.c
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 * Current state:
 * 	- Experimental
 *	- Still optimizable
 *	- Not clock cycle optimized
 *	- as a first step this compiler emulates x86 instruction to be compatible
 *	  with gencomp. Better would be a specialized version of gencomp compiling
 *	  68k instructions to ARM compatible instructions. This is a step for the
 *	  future
 *
 */

#include "flags_arm.h"

// Declare the built-in __clear_cache function.
extern void __clear_cache (char*, char*);

/*************************************************************************
 * Some basic information about the the target CPU                       *
 *************************************************************************/

#define R0_INDEX 0
#define R1_INDEX 1
#define R2_INDEX 2
#define R3_INDEX 3
#define R4_INDEX 4
#define R5_INDEX 5
#define R6_INDEX 6
#define R7_INDEX 7
#define R8_INDEX  8
#define R9_INDEX  9
#define R10_INDEX 10
#define R11_INDEX 11
#define R12_INDEX 12
#define R13_INDEX 13
#define R14_INDEX 14
#define R15_INDEX 15

#define RSP_INDEX 13
#define RLR_INDEX 14
#define RPC_INDEX 15

/* The register in which subroutines return an integer return value */
#define REG_RESULT R0_INDEX

/* The registers subroutines take their first and second argument in */
#define REG_PAR1 R0_INDEX
#define REG_PAR2 R1_INDEX

#define REG_WORK1 R2_INDEX
#define REG_WORK2 R3_INDEX
#define REG_WORK3 R12_INDEX

//#define REG_DATAPTR R10_INDEX

#define REG_PC_PRE R0_INDEX /* The register we use for preloading regs.pc_p */
#define REG_PC_TMP R1_INDEX /* Another register that is not the above */

#define MUL_NREG1 R0_INDEX /* %r4 will hold the low 32 bits after a 32x32 mul */
#define MUL_NREG2 R1_INDEX /* %r5 will hold the high 32 bits */

#define STACK_ALIGN		4
#define STACK_OFFSET	sizeof(void *)

#define R_REGSTRUCT 11
uae_s8 always_used[]={2,3,R_REGSTRUCT,12,-1}; // r2, r3 and r12 are work register in emitted code

uae_u8 call_saved[]={0,0,0,0, 1,1,1,1, 1,1,1,1, 0,1,1,1};

/* This *should* be the same as call_saved. But:
   - We might not really know which registers are saved, and which aren't,
     so we need to preserve some, but don't want to rely on everyone else
     also saving those registers
   - Special registers (such like the stack pointer) should not be "preserved"
     by pushing, even though they are "saved" across function calls
*/
/* Without save and restore R12, we sometimes get seg faults when entering gui...
   Don't understand why. */
static const uae_u8 need_to_preserve[]={0,0,0,0, 1,1,1,1, 1,1,1,1, 1,0,0,0};
static const uae_u32 PRESERVE_MASK = ((1<<R4_INDEX)|(1<<R5_INDEX)|(1<<R6_INDEX)|(1<<R7_INDEX)|(1<<R8_INDEX)|(1<<R9_INDEX)
		|(1<<R10_INDEX)|(1<<R11_INDEX)|(1<<R12_INDEX));

#include "codegen_arm.h"

STATIC_INLINE void UNSIGNED8_IMM_2_REG(W4 r, IMM v) {
	MOV_ri8(r, (uae_u8) v);
}

STATIC_INLINE void SIGNED8_IMM_2_REG(W4 r, IMM v) {
	if (v & 0x80) {
		MVN_ri8(r, (uae_u8) ~v);
	} else {
		MOV_ri8(r, (uae_u8) v);
	}
}

STATIC_INLINE void UNSIGNED16_IMM_2_REG(W4 r, IMM v) {
#ifdef ARMV6T2
  MOVW_ri16(r, v);
#else
	MOV_ri8(r, (uae_u8) v);
	ORR_rri8RORi(r, r, (uae_u8)(v >> 8), 24);
#endif
}

STATIC_INLINE void SIGNED16_IMM_2_REG(W4 r, IMM v) {
#ifdef ARMV6T2
  MOVW_ri16(r, v);
  SXTH_rr(r, r);
#else
  uae_s32 offs = data_long_offs((uae_s32)(uae_s16) v);
  LDR_rRI(r, RPC_INDEX, offs);
#endif
}

STATIC_INLINE void UNSIGNED8_REG_2_REG(W4 d, RR4 s) {
	UXTB_rr(d, s);
}

STATIC_INLINE void SIGNED8_REG_2_REG(W4 d, RR4 s) {
	SXTB_rr(d, s);
}

STATIC_INLINE void UNSIGNED16_REG_2_REG(W4 d, RR4 s) {
	UXTH_rr(d, s);
}

STATIC_INLINE void SIGNED16_REG_2_REG(W4 d, RR4 s) {
	SXTH_rr(d, s);
}

#define ZERO_EXTEND_8_REG_2_REG(d,s) UNSIGNED8_REG_2_REG(d,s)
#define ZERO_EXTEND_16_REG_2_REG(d,s) UNSIGNED16_REG_2_REG(d,s)
#define SIGN_EXTEND_8_REG_2_REG(d,s) SIGNED8_REG_2_REG(d,s)
#define SIGN_EXTEND_16_REG_2_REG(d,s) SIGNED16_REG_2_REG(d,s)


#define jit_unimplemented(fmt, ...) do{ jit_log("**** Unimplemented ****\n"); jit_log(fmt, ## __VA_ARGS__); abort(); }while (0)

LOWFUNC(WRITE,NONE,2,raw_add_l,(RW4 d, RR4 s))
{
	ADD_rrr(d, d, s);
}
LENDFUNC(WRITE,NONE,2,raw_add_l,(RW4 d, RR4 s))

LOWFUNC(WRITE,NONE,2,raw_add_l_ri,(RW4 d, IMM i))
{
  if(CHECK32(i)) {
    ADD_rri(d, d, i);
  } else {
#ifdef ARMV6T2
    MOVW_ri16(REG_WORK1, i);
    if(i >> 16)
      MOVT_ri16(REG_WORK1, i >> 16);
#else
    uae_s32 offs = data_long_offs(i);
  	LDR_rRI(REG_WORK1, RPC_INDEX, offs);
#endif
  	ADD_rrr(d, d, REG_WORK1);
  }
}
LENDFUNC(WRITE,NONE,2,raw_add_l_ri,(RW4 d, IMM i))

LOWFUNC(NONE,NONE,3,raw_lea_l_brr,(W4 d, RR4 s, IMM offset))
{
  if(CHECK32(offset)) {
    ADD_rri(d, s, offset);
  } else {
#ifdef ARMV6T2
    MOVW_ri16(REG_WORK1, offset);
    if(offset >> 16)
      MOVT_ri16(REG_WORK1, offset >> 16);
#else
    uae_s32 offs = data_long_offs(offset);
  	LDR_rRI(REG_WORK1, RPC_INDEX, offs);
#endif
  	ADD_rrr(d, s, REG_WORK1);
  }
}
LENDFUNC(NONE,NONE,3,raw_lea_l_brr,(W4 d, RR4 s, IMM offset))

LOWFUNC(NONE,NONE,5,raw_lea_l_brr_indexed,(W4 d, RR4 s, RR4 index, IMM factor, IMM offset))
{
	int shft;
	switch(factor) {
  	case 1: shft=0; break;
  	case 2: shft=1; break;
  	case 4: shft=2; break;
  	case 8: shft=3; break;
  	default: abort();
	}

  SIGNED8_IMM_2_REG(REG_WORK1, offset);
  
	ADD_rrr(REG_WORK1, s, REG_WORK1);
	ADD_rrrLSLi(d, REG_WORK1, index, shft);
}
LENDFUNC(NONE,NONE,5,raw_lea_l_brr_indexed,(W4 d, RR4 s, RR4 index, IMM factor, IMM offset))

LOWFUNC(NONE,NONE,4,raw_lea_l_rr_indexed,(W4 d, RR4 s, RR4 index, IMM factor))
{
	int shft;
	switch(factor) {
  	case 1: shft=0; break;
  	case 2: shft=1; break;
  	case 4: shft=2; break;
  	case 8: shft=3; break;
  	default: abort();
	}

	ADD_rrrLSLi(d, s, index, shft);
}
LENDFUNC(NONE,NONE,4,raw_lea_l_rr_indexed,(W4 d, RR4 s, RR4 index, IMM factor))

LOWFUNC(NONE,NONE,2,raw_mov_b_ri,(W1 d, IMM s))
{
	BIC_rri(d, d, 0xff);
	ORR_rri(d, d, (s & 0xff));
}
LENDFUNC(NONE,NONE,2,raw_mov_b_ri,(W1 d, IMM s))

LOWFUNC(NONE,NONE,2,raw_mov_b_rr,(W1 d, RR1 s))
{
#ifdef ARMV6T2
  BFI_rrii(d, s, 0, 7);
#else
	AND_rri(REG_WORK1, s, 0xff);
	BIC_rri(d, d, 0xff);
	ORR_rrr(d, d, REG_WORK1);
#endif
}
LENDFUNC(NONE,NONE,2,raw_mov_b_rr,(W1 d, RR1 s))

LOWFUNC(NONE,WRITE,2,raw_mov_l_mi,(MEMW d, IMM s))
{
#ifdef ARMV6T2
  if(d >= (uae_u32) &regs && d < ((uae_u32) &regs) + sizeof(struct regstruct)) {
    MOVW_ri16(REG_WORK2, s);
    if(s >> 16)
      MOVT_ri16(REG_WORK2, s >> 16);
    uae_s32 idx = d - (uae_u32) &regs;
    STR_rRI(REG_WORK2, R_REGSTRUCT, idx);
  } else {
    MOVW_ri16(REG_WORK1, d);
    MOVT_ri16(REG_WORK1, d >> 16);
    MOVW_ri16(REG_WORK2, s);
    if(s >> 16)
      MOVT_ri16(REG_WORK2, s >> 16);
  	STR_rR(REG_WORK2, REG_WORK1);
  }    
#else
  if(d >= (uae_u32) &regs && d < ((uae_u32) &regs) + sizeof(struct regstruct)) {
    uae_s32 offs = data_long_offs(s);
    LDR_rRI(REG_WORK2, RPC_INDEX, offs);
    uae_s32 idx = d - (uae_u32) & regs;
    STR_rRI(REG_WORK2, R_REGSTRUCT, idx);
  } else {
    data_check_end(8, 12);
    uae_s32 offs = data_long_offs(d);
  
  	LDR_rRI(REG_WORK1, RPC_INDEX, offs); 	// ldr    r2, [pc, #offs]    ; d
  
  	offs = data_long_offs(s);
  	LDR_rRI(REG_WORK2, RPC_INDEX, offs); 	// ldr    r3, [pc, #offs]    ; s
  
  	STR_rR(REG_WORK2, REG_WORK1);      	  // str    r3, [r2]
  }
#endif
}
LENDFUNC(NONE,WRITE,2,raw_mov_l_mi,(MEMW d, IMM s))

LOWFUNC(NONE,NONE,2,raw_mov_w_ri,(W2 d, IMM s))
{
  if(CHECK32(s)) {
    MOV_ri(REG_WORK2, s);
  } else {
#ifdef ARMV6T2
    MOVW_ri16(REG_WORK2, s);
#else
    uae_s32 offs = data_word_offs(s);
  	LDR_rRI(REG_WORK2, RPC_INDEX, offs);
#endif
  }

  PKHBT_rrr(d, REG_WORK2, d);
}
LENDFUNC(NONE,NONE,2,raw_mov_w_ri,(W2 d, IMM s))

LOWFUNC(NONE,WRITE,2,raw_mov_l_mr,(IMM d, RR4 s))
{
  if(d >= (uae_u32) &regs && d < ((uae_u32) &regs) + sizeof(struct regstruct)) {
    uae_s32 idx = d - (uae_u32) &regs;
    STR_rRI(s, R_REGSTRUCT, idx);
  } else {
#ifdef ARMV6T2
    MOVW_ri16(REG_WORK1, d);
    MOVT_ri16(REG_WORK1, d >> 16);
#else
    uae_s32 offs = data_long_offs(d);
  	LDR_rRI(REG_WORK1, RPC_INDEX, offs);
#endif
  	STR_rR(s, REG_WORK1);
  }
}
LENDFUNC(NONE,WRITE,2,raw_mov_l_mr,(IMM d, RR4 s))

LOWFUNC(NONE,READ,2,raw_mov_l_rm,(W4 d, MEMR s))
{
  if(s >= (uae_u32) &regs && s < ((uae_u32) &regs) + sizeof(struct regstruct)) {
    uae_s32 idx = s - (uae_u32) & regs;
    LDR_rRI(d, R_REGSTRUCT, idx);
  } else {
#ifdef ARMV6T2
    MOVW_ri16(REG_WORK1, s);
    MOVT_ri16(REG_WORK1, s >> 16);
#else
    uae_s32 offs = data_long_offs(s);
	  LDR_rRI(REG_WORK1, RPC_INDEX, offs);
#endif
	  LDR_rR(d, REG_WORK1);
  }
}
LENDFUNC(NONE,READ,2,raw_mov_l_rm,(W4 d, MEMR s))

LOWFUNC(NONE,NONE,2,raw_mov_w_rr,(W2 d, RR2 s))
{
  PKHBT_rrr(d, s, d);
}
LENDFUNC(NONE,NONE,2,raw_mov_w_rr,(W2 d, RR2 s))

LOWFUNC(WRITE,NONE,2,raw_shll_l_ri,(RW4 r, IMM i))
{
	LSL_rri(r,r, i & 0x1f);
}
LENDFUNC(WRITE,NONE,2,raw_shll_l_ri,(RW4 r, IMM i))

LOWFUNC(WRITE,NONE,2,raw_sub_l_ri,(RW4 d, IMM i))
{
  if(CHECK32(i)) {
    SUB_rri(d, d, i);
  } else {
#ifdef ARMV6T2
    MOVW_ri16(REG_WORK1, i);
    if(i >> 16)
      MOVT_ri16(REG_WORK1, i >> 16);
#else
    uae_s32 offs = data_long_offs(i);
	  LDR_rRI(REG_WORK1, RPC_INDEX, offs);
#endif
	  SUB_rrr(d, d, REG_WORK1);
  }
}
LENDFUNC(WRITE,NONE,2,raw_sub_l_ri,(RW4 d, IMM i))

LOWFUNC(WRITE,NONE,2,raw_sub_w_ri,(RW2 d, IMM i))
{
  // This function is only called with i = 1
  // Caller needs flags...
  
	LSL_rri(REG_WORK2, d, 16);

	SUBS_rri(REG_WORK2, REG_WORK2, (i & 0xff) << 16);
  PKHTB_rrrASRi(d, d, REG_WORK2, 16);

	MRS_CPSR(REG_WORK1);
	EOR_rri(REG_WORK1, REG_WORK1, ARM_C_FLAG);
	MSR_CPSRf_r(REG_WORK1);
}
LENDFUNC(WRITE,NONE,2,raw_sub_w_ri,(RW2 d, IMM i))


STATIC_INLINE void raw_dec_sp(int off)
{
	if (off) {
    if(CHECK32(off)) {
      SUB_rri(RSP_INDEX, RSP_INDEX, off);
    } else {
  		LDR_rRI(REG_WORK1, RPC_INDEX, 4);
  		SUB_rrr(RSP_INDEX, RSP_INDEX, REG_WORK1);
  		B_i(0);
  		//<value>:
  		emit_long(off);
  	}
	}
}

STATIC_INLINE void raw_inc_sp(int off)
{
	if (off) {
    if(CHECK32(off)) {
      ADD_rri(RSP_INDEX, RSP_INDEX, off);
    } else {
  		LDR_rRI(REG_WORK1, RPC_INDEX, 4);
  		ADD_rrr(RSP_INDEX, RSP_INDEX, REG_WORK1);
  		B_i(0);
  		//<value>:
  		emit_long(off);
  	}
	}
}

STATIC_INLINE void raw_push_regs_to_preserve(void) {
	PUSH_REGS(PRESERVE_MASK);
}

STATIC_INLINE void raw_pop_preserved_regs(void) {
	POP_REGS(PRESERVE_MASK);
}

STATIC_INLINE void raw_load_flagx(uae_u32 t, uae_u32 r)
{
  LDR_rRI(t, R_REGSTRUCT, 17 * 4); // X flag are next to 8 Dregs, 8 Aregs and CPSR in struct regstruct
}

STATIC_INLINE void raw_flags_evicted(int r)
{
  live.state[FLAGTMP].status = INMEM;
  live.state[FLAGTMP].realreg = -1;
  /* We just "evicted" FLAGTMP. */
  if (live.nat[r].nholds != 1) {
      /* Huh? */
      abort();
  }
  live.nat[r].nholds = 0;
}

STATIC_INLINE void raw_flags_to_reg(int r)
{
	MRS_CPSR(r);
	STR_rRI(r, R_REGSTRUCT, 16 * 4); // Flags are next to 8 Dregs and 8 Aregs in struct regstruct
	raw_flags_evicted(r);
}

STATIC_INLINE void raw_reg_to_flags(int r)
{
	MSR_CPSRf_r(r);
}

STATIC_INLINE void raw_load_flagreg(uae_u32 t, uae_u32 r)
{
	LDR_rRI(t, R_REGSTRUCT, 16 * 4); // Flags are next to 8 Dregs and 8 Aregs in struct regstruct
}

/* %eax register is clobbered if target processor doesn't support fucomi */
#define FFLAG_NREG_CLOBBER_CONDITION 0
#define FFLAG_NREG R0_INDEX
#define FLAG_NREG2 -1
#define FLAG_NREG1 -1
#define FLAG_NREG3 -1

STATIC_INLINE void raw_emit_nop_filler(int nbytes)
{
	nbytes >>= 2;
	while(nbytes--) { NOP(); }
}

//
// Arm instructions
//
LOWFUNC(WRITE,NONE,2,raw_ADD_l_rr,(RW4 d, RR4 s))
{
	ADD_rrr(d, d, s);
}
LENDFUNC(WRITE,NONE,2,raw_ADD_l_rr,(RW4 d, RR4 s))

LOWFUNC(WRITE,NONE,2,raw_ADD_l_rri,(RW4 d, RR4 s, IMM i))
{
	ADD_rri(d, s, i);
}
LENDFUNC(WRITE,NONE,2,raw_ADD_l_rri,(RW4 d, RR4 s, IMM i))

LOWFUNC(WRITE,NONE,2,raw_SUB_l_rri,(RW4 d, RR4 s, IMM i))
{
	SUB_rri(d, s, i);
}
LENDFUNC(WRITE,NONE,2,raw_SUB_l_rri,(RW4 d, RR4 s, IMM i))

LOWFUNC(WRITE,NONE,2,raw_LDR_l_ri,(RW4 d, IMM i))
{
#ifdef ARMV6T2
  MOVW_ri16(d, i);
  if(i >> 16)
    MOVT_ri16(d, i >> 16);
#else
  uae_s32 offs = data_long_offs(i);
	LDR_rRI(d, RPC_INDEX, offs);
#endif
}
LENDFUNC(WRITE,NONE,2,raw_LDR_l_ri,(RW4 d, IMM i))

//
// compuemu_support used raw calls
//
LOWFUNC(WRITE,NONE,2,compemu_raw_MERGE_rr,(RW4 d, RR4 s))
{
	PKHBT_rrr(d, d, s);
}
LENDFUNC(WRITE,NONE,2,compemu_raw_MERGE_rr,(RW4 d, RR4 s))

LOWFUNC(WRITE,RMW,2,compemu_raw_add_l_mi,(IMM d, IMM s))
{
#ifdef ARMV6T2
  MOVW_ri16(REG_WORK1, d);
  MOVT_ri16(REG_WORK1, d >> 16);
  LDR_rR(REG_WORK2, REG_WORK1);

  MOVW_ri16(REG_WORK3, s);
  if(s >> 16)
    MOVT_ri16(REG_WORK3, s >> 16);
  ADD_rrr(REG_WORK2, REG_WORK2, REG_WORK3);

  STR_rR(REG_WORK2, REG_WORK1);  
#else
  uae_s32 offs = data_long_offs(d);
	LDR_rRI(REG_WORK1, RPC_INDEX, offs);
	LDR_rR(REG_WORK2, REG_WORK1);

  offs = data_long_offs(s);
	LDR_rRI(REG_WORK3, RPC_INDEX, offs);

	ADD_rrr(REG_WORK2, REG_WORK2, REG_WORK3);

	STR_rR(REG_WORK2, REG_WORK1);
#endif
}
LENDFUNC(WRITE,RMW,2,compemu_raw_add_l_mi,(IMM d, IMM s))

LOWFUNC(WRITE,NONE,2,compemu_raw_and_TAGMASK,(RW4 d))
{
  // TAGMASK is 0x0000ffff
#ifdef ARMV6T2
  BFC_rii(d, 16, 31);
#else
	BIC_rri(d, d, 0x00ff0000);
	BIC_rri(d, d, 0xff000000);
#endif
}
LENDFUNC(WRITE,NONE,2,compemu_raw_and_TAGMASK,(RW4 d))

LOWFUNC(WRITE,READ,2,compemu_raw_cmp_l_mi,(MEMR d, IMM s))
{
  clobber_flags();
  
 #ifdef ARMV6T2
  if(d >= (uae_u32) &regs && d < ((uae_u32) &regs) + sizeof(struct regstruct)) {
    MOVW_ri16(REG_WORK2, s);
    if(s >> 16)
      MOVT_ri16(REG_WORK2, s >> 16);
    uae_s32 idx = d - (uae_u32) & regs;
    LDR_rRI(REG_WORK1, R_REGSTRUCT, idx);
  } else {
    MOVW_ri16(REG_WORK1, d);
    MOVT_ri16(REG_WORK1, d >> 16);
	  LDR_rR(REG_WORK1, REG_WORK1);
    MOVW_ri16(REG_WORK2, s);
    if(s >> 16)
      MOVT_ri16(REG_WORK2, s >> 16);
  }
#else
  if(d >= (uae_u32) &regs && d < ((uae_u32) &regs) + sizeof(struct regstruct)) {
    uae_s32 offs = data_long_offs(s);
    LDR_rRI(REG_WORK2, RPC_INDEX, offs);
    uae_s32 idx = d - (uae_u32) & regs;
    LDR_rRI(REG_WORK1, R_REGSTRUCT, idx);
  } else {
    data_check_end(8, 16);
    uae_s32 offs = data_long_offs(d);
	  LDR_rRI(REG_WORK1, RPC_INDEX, offs);
	  LDR_rR(REG_WORK1, REG_WORK1);

	  offs = data_long_offs(s);
	  LDR_rRI(REG_WORK2, RPC_INDEX, offs);
  }
#endif

	CMP_rr(REG_WORK1, REG_WORK2);
}
LENDFUNC(WRITE,READ,2,compemu_raw_cmp_l_mi,(MEMR d, IMM s))

LOWFUNC(NONE,NONE,3,compemu_raw_lea_l_brr,(W4 d, RR4 s, IMM offset))
{
  if(CHECK32(offset)) {
    ADD_rri(d, s, offset);
  } else {
#ifdef ARMV6T2
    MOVW_ri16(REG_WORK1, offset);
    if(offset >> 16)
      MOVT_ri16(REG_WORK1, offset >> 16);
#else
    uae_s32 offs = data_long_offs(offset);
  	LDR_rRI(REG_WORK1, RPC_INDEX, offs);
#endif
  	ADD_rrr(d, s, REG_WORK1);
  }
}
LENDFUNC(NONE,NONE,3,compemu_raw_lea_l_brr,(W4 d, RR4 s, IMM offset))

LOWFUNC(NONE,WRITE,2,compemu_raw_mov_b_mr,(MEMW d, RR1 s))
{
  if(d >= (uae_u32) &regs && d < ((uae_u32) &regs) + sizeof(struct regstruct)) {
    uae_s32 idx = d - (uae_u32) & regs;
    STRB_rRI(s, R_REGSTRUCT, idx);
  } else {
#ifdef ARMV6T2
    MOVW_ri16(REG_WORK1, d);
    MOVT_ri16(REG_WORK1, d >> 16);
#else
    uae_s32 offs = data_long_offs(d);
	  LDR_rRI(REG_WORK1, RPC_INDEX, offs);
#endif
	  STRB_rR(s, REG_WORK1);
  }
}
LENDFUNC(NONE,WRITE,2,compemu_raw_mov_b_mr,(MEMW d, RR1 s))

LOWFUNC(NONE,WRITE,2,compemu_raw_mov_l_mi,(MEMW d, IMM s))
{
#ifdef ARMV6T2
  if(d >= (uae_u32) &regs && d < ((uae_u32) &regs) + sizeof(struct regstruct)) {
    MOVW_ri16(REG_WORK2, s);
    if(s >> 16)
      MOVT_ri16(REG_WORK2, s >> 16);
    uae_s32 idx = d - (uae_u32) & regs;
    STR_rRI(REG_WORK2, R_REGSTRUCT, idx);
  } else {
    MOVW_ri16(REG_WORK1, d);
    MOVT_ri16(REG_WORK1, d >> 16);
    MOVW_ri16(REG_WORK2, s);
    if(s >> 16)
      MOVT_ri16(REG_WORK2, s >> 16);
	  STR_rR(REG_WORK2, REG_WORK1);
  }
#else
  if(d >= (uae_u32) &regs && d < ((uae_u32) &regs) + sizeof(struct regstruct)) {
    uae_s32 offs = data_long_offs(s);
    LDR_rRI(REG_WORK2, RPC_INDEX, offs);
    uae_s32 idx = d - (uae_u32) & regs;
    STR_rRI(REG_WORK2, R_REGSTRUCT, idx);
  } else {
    data_check_end(8, 12);
    uae_s32 offs = data_long_offs(d);
	  LDR_rRI(REG_WORK1, RPC_INDEX, offs);
	  offs = data_long_offs(s);
	  LDR_rRI(REG_WORK2, RPC_INDEX, offs);
	  STR_rR(REG_WORK2, REG_WORK1);
  }
#endif
}
LENDFUNC(NONE,WRITE,2,compemu_raw_mov_l_mi,(MEMW d, IMM s))

LOWFUNC(NONE,WRITE,2,compemu_raw_mov_l_mr,(MEMW d, RR4 s))
{
  if(d >= (uae_u32) &regs && d < ((uae_u32) &regs) + sizeof(struct regstruct)) {
    uae_s32 idx = d - (uae_u32) & regs;
    STR_rRI(s, R_REGSTRUCT, idx);
  } else {
#ifdef ARMV6T2
    MOVW_ri16(REG_WORK1, d);
    MOVT_ri16(REG_WORK1, d >> 16);
#else
    uae_s32 offs = data_long_offs(d);
	  LDR_rRI(REG_WORK1, RPC_INDEX, offs);
#endif
	  STR_rR(s, REG_WORK1);
  }
}
LENDFUNC(NONE,WRITE,2,compemu_raw_mov_l_mr,(MEMW d, RR4 s))

LOWFUNC(NONE,NONE,2,compemu_raw_mov_l_ri,(W4 d, IMM s))
{
#ifdef ARMV6T2
  MOVW_ri16(d, s);
  if(s >> 16)
    MOVT_ri16(d, s >> 16);
#else
  uae_s32 offs = data_long_offs(s);
	LDR_rRI(d, RPC_INDEX, offs);
#endif
}
LENDFUNC(NONE,NONE,2,compemu_raw_mov_l_ri,(W4 d, IMM s))

LOWFUNC(NONE,READ,2,compemu_raw_mov_l_rm,(W4 d, MEMR s))
{
  if(s >= (uae_u32) &regs && s < ((uae_u32) &regs) + sizeof(struct regstruct)) {
    uae_s32 idx = s - (uae_u32) & regs;
    LDR_rRI(d, R_REGSTRUCT, idx);
  } else {
#ifdef ARMV6T2
    MOVW_ri16(REG_WORK1, s);
    MOVT_ri16(REG_WORK1, s >> 16);
#else
    uae_s32 offs = data_long_offs(s);
	  LDR_rRI(REG_WORK1, RPC_INDEX, offs);
#endif
	  LDR_rR(d, REG_WORK1);
  }
}
LENDFUNC(NONE,READ,2,compemu_raw_mov_l_rm,(W4 d, MEMR s))

LOWFUNC(NONE,NONE,2,compemu_raw_mov_l_rr,(W4 d, RR4 s))
{
	MOV_rr(d, s);
}
LENDFUNC(NONE,NONE,2,compemu_raw_mov_l_rr,(W4 d, RR4 s))

LOWFUNC(NONE,WRITE,2,compemu_raw_mov_w_mr,(IMM d, RR2 s))
{
  if(d >= (uae_u32) &regs && d < ((uae_u32) &regs) + sizeof(struct regstruct)) {
    uae_s32 idx = d - (uae_u32) & regs;
    STRH_rRI(s, R_REGSTRUCT, idx);
  } else {
#ifdef ARMV6T2
    MOVW_ri16(REG_WORK1, d);
    MOVT_ri16(REG_WORK1, d >> 16);
#else
    uae_s32 offs = data_long_offs(d);
	  LDR_rRI(REG_WORK1, RPC_INDEX, offs);
#endif
	  STRH_rR(s, REG_WORK1);
  }
}
LENDFUNC(NONE,WRITE,2,compemu_raw_mov_w_mr,(IMM d, RR2 s))

LOWFUNC(WRITE,RMW,2,compemu_raw_sub_l_mi,(MEMRW d, IMM s))
{
  clobber_flags();

  if(CHECK32(s)) {
    if(d >= (uae_u32) &regs && d < ((uae_u32) &regs) + sizeof(struct regstruct)) {
      uae_s32 idx = d - (uae_u32) & regs;
      LDR_rRI(REG_WORK2, R_REGSTRUCT, idx);
      SUBS_rri(REG_WORK2, REG_WORK2, s);
      STR_rRI(REG_WORK2, R_REGSTRUCT, idx);
    } else {
#ifdef ARMV6T2
      MOVW_ri16(REG_WORK1, d);
      MOVT_ri16(REG_WORK1, d >> 16);
#else
    	uae_s32 offs = data_long_offs(d);
    	LDR_rRI(REG_WORK1, RPC_INDEX, offs);
#endif
      LDR_rR(REG_WORK2, REG_WORK1);
      SUBS_rri(REG_WORK2, REG_WORK2, s);
      STR_rR(REG_WORK2, REG_WORK1);
    }
  } else {
    if(d >= (uae_u32) &regs && d < ((uae_u32) &regs) + sizeof(struct regstruct)) {
      uae_s32 idx = d - (uae_u32) & regs;
  	  LDR_rRI(REG_WORK2, R_REGSTRUCT, idx);
  
#ifdef ARMV6T2
      MOVW_ri16(REG_WORK1, s);
      if(s >> 16)
        MOVT_ri16(REG_WORK1, s >> 16);
#else
  	  uae_s32 offs = data_long_offs(s);
  	  LDR_rRI(REG_WORK1, RPC_INDEX, offs);
#endif

  	  SUBS_rrr(REG_WORK2, REG_WORK2, REG_WORK1);
  	  STR_rRI(REG_WORK2, R_REGSTRUCT, idx);
    } else {
#ifdef ARMV6T2
      MOVW_ri16(REG_WORK1, d);
      MOVT_ri16(REG_WORK1, d >> 16);
  	  LDR_rR(REG_WORK2, REG_WORK1);
  
      MOVW_ri16(REG_WORK3, s);
      if(s >> 16)
        MOVT_ri16(REG_WORK3, s >> 16);
#else
      uae_s32 offs = data_long_offs(d);
  	  LDR_rRI(REG_WORK1, RPC_INDEX, offs);
  	  LDR_rR(REG_WORK2, REG_WORK1);
  
  	  offs = data_long_offs(s);
  	  LDR_rRI(REG_WORK3, RPC_INDEX, offs);
#endif
  
  	  SUBS_rrr(REG_WORK2, REG_WORK2, REG_WORK3);
  	  STR_rR(REG_WORK2, REG_WORK1);
    }
  }
}
LENDFUNC(WRITE,RMW,2,compemu_raw_sub_l_mi,(MEMRW d, IMM s))

LOWFUNC(WRITE,NONE,2,compemu_raw_test_l_rr,(RR4 d, RR4 s))
{
  clobber_flags();
	TST_rr(d, s);
}
LENDFUNC(WRITE,NONE,2,compemu_raw_test_l_rr,(RR4 d, RR4 s))

STATIC_INLINE void compemu_raw_call(uae_u32 t)
{
#ifdef ARMV6T2
  MOVW_ri16(REG_WORK1, t);
  MOVT_ri16(REG_WORK1, t >> 16);
#else
  uae_s32 offs = data_long_offs(t);
	LDR_rRI(REG_WORK1, RPC_INDEX, offs);
#endif
	PUSH(RLR_INDEX);
	BLX_r(REG_WORK1);
	POP(RLR_INDEX);
}

STATIC_INLINE void compemu_raw_call_r(RR4 r)
{
	PUSH(RLR_INDEX);
	BLX_r(r);
	POP(RLR_INDEX);
}

STATIC_INLINE void compemu_raw_jcc_l_oponly(int cc)
{
	switch (cc) {
		case NATIVE_CC_HI: // HI
			BEQ_i(2);										// beq no jump
			BCS_i(1);										// bcs no jump
		  // jump
			LDR_rRI(RPC_INDEX, RPC_INDEX, -4); 
			// no jump
			break;

		case NATIVE_CC_LS: // LS
			BEQ_i(0);										// beq jump
			BCC_i(1);										// bcc no jump
			// jump
			LDR_rRI(RPC_INDEX, RPC_INDEX, -4);
			// no jump
			break;

		case NATIVE_CC_F_OGT: // Jump if valid and greater than
			BVS_i(2);		// do not jump if NaN
			BLE_i(1);		// do not jump if less or equal
			LDR_rRI(RPC_INDEX, RPC_INDEX, -4);
			break;

		case NATIVE_CC_F_OGE: // Jump if valid and greater or equal
			BVS_i(2);		// do not jump if NaN
			BCC_i(1);		// do not jump if carry cleared
			LDR_rRI(RPC_INDEX, RPC_INDEX, -4);
			break;
			
		case NATIVE_CC_F_OLT: // Jump if vaild and less than
			BVS_i(2);		// do not jump if NaN
			BCS_i(1);		// do not jump if carry set
			LDR_rRI(RPC_INDEX, RPC_INDEX, -4);
			break;
			
		case NATIVE_CC_F_OLE: // Jump if valid and less or equal
			BVS_i(2);		// do not jump if NaN
			BGT_i(1);		// do not jump if greater than
			LDR_rRI(RPC_INDEX, RPC_INDEX, -4);
			break;
			
		case NATIVE_CC_F_OGL: // Jump if valid and greator or less
			BVS_i(2);		// do not jump if NaN
			BEQ_i(1);		// do not jump if equal
			LDR_rRI(RPC_INDEX, RPC_INDEX, -4);
			break;

		case NATIVE_CC_F_OR: // Jump if valid
			BVS_i(1); 	// do not jump if NaN
			LDR_rRI(RPC_INDEX, RPC_INDEX, -4);
			break;
			
		case NATIVE_CC_F_UN: // Jump if NAN
			BVC_i(1); 	// do not jump if valid
			LDR_rRI(RPC_INDEX, RPC_INDEX, -4);
			break;

		case NATIVE_CC_F_UEQ: // Jump if NAN or equal
			BVS_i(0); 	// jump if NaN
			BNE_i(1);		// do not jump if greater or less
			// jump
			LDR_rRI(RPC_INDEX, RPC_INDEX, -4);
			break;

		case NATIVE_CC_F_UGT: // Jump if NAN or greater than
			BVS_i(0); 	// jump if NaN
			BLS_i(1);		// do not jump if lower or same
			// jump
			LDR_rRI(RPC_INDEX, RPC_INDEX, -4);
			break;

		case NATIVE_CC_F_UGE: // Jump if NAN or greater or equal
			BVS_i(0); 	// jump if NaN
			BMI_i(1);		// do not jump if lower
			// jump
			LDR_rRI(RPC_INDEX, RPC_INDEX, -4);
			break;

		case NATIVE_CC_F_ULT: // Jump if NAN or less than
			BVS_i(0); 	// jump if NaN
			BGE_i(1);		// do not jump if greater or equal
			// jump
			LDR_rRI(RPC_INDEX, RPC_INDEX, -4);
			break;

		case NATIVE_CC_F_ULE: // Jump if NAN or less or equal
			BVS_i(0); 	// jump if NaN
			BGT_i(1);		// do not jump if greater
			// jump
			LDR_rRI(RPC_INDEX, RPC_INDEX, -4);
			break;
	
		default:
	    CC_B_i(cc^1, 1);
      LDR_rRI(RPC_INDEX, RPC_INDEX, -4);
			break;
	}
  // emit of target will be done by caller
}

STATIC_INLINE void compemu_raw_handle_except(IMM cycles)
{
	uae_u32* branchadd;	

  clobber_flags();

  MOVW_ri16(REG_WORK2, (uae_u32)(&jit_exception));
  MOVT_ri16(REG_WORK2, ((uae_u32)(&jit_exception)) >> 16);
  LDR_rR(REG_WORK1, REG_WORK2);
	TST_rr(REG_WORK1, REG_WORK1);
	BNE_i(1);		// exception, skip LDR and target
	
	LDR_rRI(RPC_INDEX, RPC_INDEX, -4);	// jump to next opcode
	branchadd = (uae_u32*)get_target();
	skip_long();	// emit of target (next opcode handler) will be done later

  // countdown -= scaled_cycles(totcycles);
  uae_s32 offs = (uae_u32)&countdown - (uae_u32)&regs;
	LDR_rRI(REG_WORK1, R_REGSTRUCT, offs);
  if(CHECK32(cycles)) {
	  SUBS_rri(REG_WORK1, REG_WORK1, cycles);
	} else {
#ifdef ARMV6T2
    MOVW_ri16(REG_WORK2, cycles);
    if(cycles >> 16)
      MOVT_ri16(REG_WORK2, cycles >> 16);
#else
  	int offs2 = data_long_offs(cycles);
  	LDR_rRI(REG_WORK2, RPC_INDEX, offs2);
#endif
  	SUBS_rrr(REG_WORK1, REG_WORK1, REG_WORK2);
  }
	STR_rRI(REG_WORK1, R_REGSTRUCT, offs);

  LDR_rRI(RPC_INDEX, RPC_INDEX, -4); // <popall_execute_exception>
	emit_long((uintptr)popall_execute_exception);
	
	// Write target of next instruction
	write_jmp_target(branchadd, (cpuop_func*)get_target());
	
}

STATIC_INLINE void compemu_raw_jl(uae_u32 t)
{
#ifdef ARMV6T2
  MOVW_ri16(REG_WORK1, t);
  MOVT_ri16(REG_WORK1, t >> 16);
  CC_BX_r(NATIVE_CC_LT, REG_WORK1);
#else
  uae_s32 offs = data_long_offs(t);
	CC_LDR_rRI(NATIVE_CC_LT, RPC_INDEX, RPC_INDEX, offs);
#endif
}

STATIC_INLINE void compemu_raw_jmp(uae_u32 t)
{
  LDR_rRI(RPC_INDEX, RPC_INDEX, -4);
	emit_long(t);
}

STATIC_INLINE void compemu_raw_jmp_m_indexed(uae_u32 base, uae_u32 r, uae_u32 m)
{
	int shft;
	switch(m) {
  	case 1: shft=0; break;
  	case 2: shft=1; break;
  	case 4: shft=2; break;
  	case 8: shft=3; break;
  	default: abort();
	}

	LDR_rR(REG_WORK1, RPC_INDEX);
	LDR_rRR_LSLi(RPC_INDEX, REG_WORK1, r, shft);
	emit_long(base);
}

STATIC_INLINE void compemu_raw_jnz(uae_u32 t)
{
#ifdef ARMV6T2
  BEQ_i(1);
  LDR_rRI(RPC_INDEX, RPC_INDEX, -4);
  emit_long(t);
#else
  uae_s32 offs = data_long_offs(t);
	CC_LDR_rRI(NATIVE_CC_NE, RPC_INDEX, RPC_INDEX, offs);
#endif
}

STATIC_INLINE void compemu_raw_jz_b_oponly(void)
{
  BEQ_i(0);   // Real distance set by caller
}

STATIC_INLINE void compemu_raw_branch(IMM d)
{
	B_i((d >> 2) - 1);
}


// Optimize access to struct regstruct with r11

LOWFUNC(NONE,NONE,1,compemu_raw_init_r_regstruct,(IMM s))
{
#ifdef ARMV6T2
  MOVW_ri16(R_REGSTRUCT, s);
  MOVT_ri16(R_REGSTRUCT, s >> 16);
#else
  uae_s32 offs = data_long_offs(s);
	LDR_rRI(R_REGSTRUCT, RPC_INDEX, offs);
#endif
}
LENDFUNC(NONE,NONE,1,compemu_raw_init_r_regstruct,(IMM s))


// Handle end of compiled block
LOWFUNC(NONE,NONE,2,compemu_raw_endblock_pc_inreg,(RR4 rr_pc, IMM cycles))
{
  clobber_flags();

  // countdown -= scaled_cycles(totcycles);
  uae_s32 offs = (uae_u32)&countdown - (uae_u32)&regs;
	LDR_rRI(REG_WORK1, R_REGSTRUCT, offs);
  if(CHECK32(cycles)) {
	  SUBS_rri(REG_WORK1, REG_WORK1, cycles);
	} else {
#ifdef ARMV6T2
    MOVW_ri16(REG_WORK2, cycles);
    if(cycles >> 16)
      MOVT_ri16(REG_WORK2, cycles >> 16);
#else
  	int offs2 = data_long_offs(cycles);
  	LDR_rRI(REG_WORK2, RPC_INDEX, offs2);
#endif
  	SUBS_rrr(REG_WORK1, REG_WORK1, REG_WORK2);
  }
	STR_rRI(REG_WORK1, R_REGSTRUCT, offs);

#ifdef ARMV6T2
	CC_B_i(NATIVE_CC_MI, 2);
	BFC_rii(rr_pc, 16, 31); // apply TAGMASK
#else
	CC_B_i(NATIVE_CC_MI, 3);
	BIC_rri(rr_pc, rr_pc, 0x00ff0000);
	BIC_rri(rr_pc, rr_pc, 0xff000000);
#endif
  LDR_rRI(REG_WORK1, RPC_INDEX, 4); // <cache_tags>
	LDR_rRR_LSLi(RPC_INDEX, REG_WORK1, rr_pc, 2);

  LDR_rRI(RPC_INDEX, RPC_INDEX, 0); // <popall_do_nothing>

	emit_long((uintptr)cache_tags);
	emit_long((uintptr)popall_do_nothing);
}
LENDFUNC(NONE,NONE,2,compemu_raw_endblock_pc_inreg,(RR4 rr_pc, IMM cycles))


LOWFUNC(NONE,NONE,2,compemu_raw_endblock_pc_isconst,(IMM cycles, IMM v))
{
  clobber_flags();

  // countdown -= scaled_cycles(totcycles);
  uae_s32 offs = (uae_u32)&countdown - (uae_u32)&regs;
	LDR_rRI(REG_WORK1, R_REGSTRUCT, offs);
  if(CHECK32(cycles)) {
	  SUBS_rri(REG_WORK1, REG_WORK1, cycles);
	} else {
#ifdef ARMV6T2
    MOVW_ri16(REG_WORK2, cycles);
    if(cycles >> 16)
      MOVT_ri16(REG_WORK2, cycles >> 16);
#else
  	int offs2 = data_long_offs(cycles);
  	LDR_rRI(REG_WORK2, RPC_INDEX, offs2);
#endif
  	SUBS_rrr(REG_WORK1, REG_WORK1, REG_WORK2);
  }
	STR_rRI(REG_WORK1, R_REGSTRUCT, offs);

  CC_LDR_rRI(NATIVE_CC_MI^1, RPC_INDEX, RPC_INDEX, 16); // <target>
  
  LDR_rRI(REG_WORK1, RPC_INDEX, 4); // <v>
  offs = (uae_u32)&regs.pc_p - (uae_u32)&regs;
  STR_rRI(REG_WORK1, R_REGSTRUCT, offs);
  LDR_rRI(RPC_INDEX, RPC_INDEX, 0); // <popall_do_nothing>

	emit_long(v);
	emit_long((uintptr)popall_do_nothing);
  
  // <target emitted by caller>
}
LENDFUNC(NONE,NONE,2,compemu_raw_endblock_pc_isconst,(IMM cycles, IMM v))


/*************************************************************************
* FPU stuff                                                             *
*************************************************************************/

LOWFUNC(NONE,NONE,2,raw_fmov_rr,(FW d, FR s))
{
	VMOV64_dd(d, s);
}
LENDFUNC(NONE,NONE,2,raw_fmov_rr,(FW d, FR s))

LOWFUNC(NONE,WRITE,2,compemu_raw_fmov_mr_drop,(MEMW mem, FR s))
{
  if(mem >= (uae_u32) &regs && mem < (uae_u32) &regs + 1020 && ((mem - (uae_u32) &regs) & 0x3) == 0) {
    VSTR64_dRi(s, R_REGSTRUCT, (mem - (uae_u32) &regs));
  } else {
#ifdef ARMV6T2
    MOVW_ri16(REG_WORK1, mem);
    MOVT_ri16(REG_WORK1, mem >> 16);
#else
	  auto offs = data_long_offs(mem);
	  LDR_rRI(REG_WORK1, RPC_INDEX, offs);
#endif
    VSTR64_dRi(s, REG_WORK1, 0);
  }
}
LENDFUNC(NONE,WRITE,2,compemu_raw_fmov_mr_drop,(MEMW mem, FR s))


LOWFUNC(NONE,READ,2,compemu_raw_fmov_rm,(FW d, MEMR mem))
{
  if(mem >= (uae_u32) &regs && mem < (uae_u32) &regs + 1020 && ((mem - (uae_u32) &regs) & 0x3) == 0) {
    VLDR64_dRi(d, R_REGSTRUCT, (mem - (uae_u32) &regs));
  } else {
#ifdef ARMV6T2
    MOVW_ri16(REG_WORK1, mem);
    MOVT_ri16(REG_WORK1, mem >> 16);
#else
	  auto offs = data_long_offs(mem);
	  LDR_rRI(REG_WORK1, RPC_INDEX, offs);
#endif
    VLDR64_dRi(d, REG_WORK1, 0);
  }
}
LENDFUNC(NONE,READ,2,compemu_raw_fmov_rm,(FW d, MEMW mem))

LOWFUNC(NONE,NONE,2,raw_fmov_l_rr,(FW d, RR4 s))
{
  VMOVi_from_ARM_dr(SCRATCH_F64_1, s, 0);
  VCVTIto64_ds(d, SCRATCH_F32_1);
}
LENDFUNC(NONE,NONE,2,raw_fmov_l_rr,(FW d, RR4 s))

LOWFUNC(NONE,NONE,2,raw_fmov_s_rr,(FW d, RR4 s))
{
  VMOV32_sr(SCRATCH_F32_1, s);
  VCVT32to64_ds(d, SCRATCH_F32_1);
}
LENDFUNC(NONE,NONE,2,raw_fmov_s_rr,(FW d, RR4 s))

LOWFUNC(NONE,NONE,2,raw_fmov_w_rr,(FW d, RR2 s))
{
  SIGN_EXTEND_16_REG_2_REG(REG_WORK1, s);
  VMOVi_from_ARM_dr(SCRATCH_F64_1, REG_WORK1, 0);
  VCVTIto64_ds(d, SCRATCH_F32_1);
}
LENDFUNC(NONE,NONE,2,raw_fmov_w_rr,(FW d, RR2 s))

LOWFUNC(NONE,NONE,2,raw_fmov_b_rr,(FW d, RR1 s))
{
  SIGN_EXTEND_8_REG_2_REG(REG_WORK1, s);
  VMOVi_from_ARM_dr(SCRATCH_F64_1, REG_WORK1, 0);
  VCVTIto64_ds(d, SCRATCH_F32_1);
}
LENDFUNC(NONE,NONE,2,raw_fmov_b_rr,(FW d, RR1 s))

LOWFUNC(NONE,NONE,2,raw_fmov_d_rrr,(FW d, RR4 s1, RR4 s2))
{
  VMOV64_drr(d, s1, s2);
}
LENDFUNC(NONE,NONE,2,raw_fmov_d_rrr,(FW d, RR4 s1, RR4 s2))

LOWFUNC(NONE,NONE,2,raw_fmov_to_l_rr,(W4 d, FR s))
{
  VCVTR64toI_sd(SCRATCH_F32_1, s);
  VMOV32_rs(d, SCRATCH_F32_1);
}
LENDFUNC(NONE,NONE,2,raw_fmov_to_l_rr,(W4 d, FR s))

LOWFUNC(NONE,NONE,2,raw_fmov_to_s_rr,(W4 d, FR s))
{
  VCVT64to32_sd(SCRATCH_F32_1, s);
  VMOV32_rs(d, SCRATCH_F32_1);
}
LENDFUNC(NONE,NONE,2,raw_fmov_to_s_rr,(W4 d, FR s))

LOWFUNC(NONE,NONE,2,raw_fmov_to_w_rr,(W4 d, FR s))
{
  VCVTR64toI_sd(SCRATCH_F32_1, s);
  VMOV32_rs(REG_WORK1, SCRATCH_F32_1);
	SSAT_rir(d, 15, REG_WORK1);
}
LENDFUNC(NONE,NONE,2,raw_fmov_to_w_rr,(W4 d, FR s))

LOWFUNC(NONE,NONE,2,raw_fmov_to_b_rr,(W4 d, FR s))
{
  VCVTR64toI_sd(SCRATCH_F32_1, s);
  VMOV32_rs(REG_WORK1, SCRATCH_F32_1);
  SSAT_rir(d, 7, REG_WORK1);
}
LENDFUNC(NONE,NONE,2,raw_fmov_to_b_rr,(W4 d, FR s))

LOWFUNC(NONE,NONE,1,raw_fmov_d_ri_0,(FW r))
{
	VMOV_I64_dimmI(r, 0x00);		// load imm #0 into reg
}
LENDFUNC(NONE,NONE,1,raw_fmov_d_ri_0,(FW r))

LOWFUNC(NONE,NONE,1,raw_fmov_d_ri_1,(FW r))
{
  VMOV_F64_dimmF(r, 0x70); // load imm #1 into reg
}
LENDFUNC(NONE,NONE,1,raw_fmov_d_ri_1,(FW r))

LOWFUNC(NONE,NONE,1,raw_fmov_d_ri_10,(FW r))
{
  VMOV_F64_dimmF(r, 0x24); // load imm #10 into reg
}
LENDFUNC(NONE,NONE,1,raw_fmov_d_ri_10,(FW r))

LOWFUNC(NONE,NONE,1,raw_fmov_d_ri_100,(FW r))
{
  VMOV_F64_dimmF(r, 0x24); // load imm #10 into reg
  VMUL64_ddd(r, r, r);
}
LENDFUNC(NONE,NONE,1,raw_fmov_d_ri_10,(FW r))

LOWFUNC(NONE,READ,2,raw_fmov_d_rm,(FW r, MEMR m))
{
#ifdef ARMV6T2
  MOVW_ri16(REG_WORK1, m);
  MOVT_ri16(REG_WORK1, m >> 16);
#else
	auto offs = data_long_offs(m);
	LDR_rRI(REG_WORK1, RPC_INDEX, offs);
#endif
  VLDR64_dRi(r, REG_WORK1, 0);
}
LENDFUNC(NONE,READ,2,raw_fmov_d_rm,(FW r, MEMR m))

LOWFUNC(NONE,READ,2,raw_fmovs_rm,(FW r, MEMR m))
{
#ifdef ARMV6T2
	MOVW_ri16(REG_WORK1, m);
	MOVT_ri16(REG_WORK1, m >> 16);
#else
	auto offs = data_long_offs(m);
	LDR_rRI(REG_WORK1, RPC_INDEX, offs);
#endif
  VLDR32_sRi(SCRATCH_F32_1, REG_WORK1, 0);
  VCVT32to64_ds(r, SCRATCH_F32_1);
}
LENDFUNC(NONE,READ,2,raw_fmovs_rm,(FW r, MEMR m))

LOWFUNC(NONE,NONE,3,raw_fmov_to_d_rrr,(W4 d1, W4 d2, FR s))
{
  VMOV64_rrd(d1, d2, s);
}
LENDFUNC(NONE,NONE,3,raw_fmov_to_d_rrr,(W4 d1, W4 d2, FR s))

LOWFUNC(NONE,NONE,2,raw_fsqrt_rr,(FW d, FR s))
{
	VSQRT64_dd(d, s);
}
LENDFUNC(NONE,NONE,2,raw_fsqrt_rr,(FW d, FR s))

LOWFUNC(NONE,NONE,2,raw_fabs_rr,(FW d, FR s))
{
	VABS64_dd(d, s);
}
LENDFUNC(NONE,NONE,2,raw_fabs_rr,(FW d, FR s))

LOWFUNC(NONE,NONE,2,raw_fneg_rr,(FW d, FR s))
{
	VNEG64_dd(d, s);
}
LENDFUNC(NONE,NONE,2,raw_fneg_rr,(FW d, FR s))

LOWFUNC(NONE,NONE,2,raw_fdiv_rr,(FRW d, FR s))
{
	VDIV64_ddd(d, d, s);
}
LENDFUNC(NONE,NONE,2,raw_fdiv_rr,(FRW d, FR s))

LOWFUNC(NONE,NONE,2,raw_fadd_rr,(FRW d, FR s))
{
	VADD64_ddd(d, d, s);
}
LENDFUNC(NONE,NONE,2,raw_fadd_rr,(FRW d, FR s))

LOWFUNC(NONE,NONE,2,raw_fmul_rr,(FRW d, FR s))
{
	VMUL64_ddd(d, d, s);
}
LENDFUNC(NONE,NONE,2,raw_fmul_rr,(FRW d, FR s))

LOWFUNC(NONE,NONE,2,raw_fsub_rr,(FRW d, FR s))
{
	VSUB64_ddd(d, d, s);
}
LENDFUNC(NONE,NONE,2,raw_fsub_rr,(FRW d, FR s))

LOWFUNC(NONE,NONE,2,raw_frndint_rr,(FW d, FR s))
{
	VCVTR64toI_sd(SCRATCH_F32_1, s);
	VCVTIto64_ds(d, SCRATCH_F32_1);
}
LENDFUNC(NONE,NONE,2,raw_frndint_rr,(FW d, FR s))

LOWFUNC(NONE,NONE,2,raw_frndintz_rr,(FW d, FR s))
{
	VCVT64toI_sd(SCRATCH_F32_1, s);
	VCVTIto64_ds(d, SCRATCH_F32_1);
}
LENDFUNC(NONE,NONE,2,raw_frndintz_rr,(FW d, FR s))

LOWFUNC(NONE,NONE,2,raw_fmod_rr,(FRW d, FR s))
{
	VDIV64_ddd(SCRATCH_F64_2, d, s);
	VCVT64toI_sd(SCRATCH_F32_1, SCRATCH_F64_2);
	VCVTIto64_ds(SCRATCH_F64_2, SCRATCH_F32_1);
	VMUL64_ddd(SCRATCH_F64_1, SCRATCH_F64_2, s);
	VSUB64_ddd(d, d, SCRATCH_F64_1);
}
LENDFUNC(NONE,NONE,2,raw_fmod_rr,(FRW d, FR s))

LOWFUNC(NONE,NONE,2,raw_fsgldiv_rr,(FRW d, FR s))
{
	VCVT64to32_sd(SCRATCH_F32_1, d);
	VCVT64to32_sd(SCRATCH_F32_2, s);
	VDIV32_sss(SCRATCH_F32_1, SCRATCH_F32_1, SCRATCH_F32_2);
	VCVT32to64_ds(d, SCRATCH_F32_1);
}
LENDFUNC(NONE,NONE,2,raw_fsgldiv_rr,(FRW d, FR s))

LOWFUNC(NONE,NONE,1,raw_fcuts_r,(FRW r))
{
	VCVT64to32_sd(SCRATCH_F32_1, r);
	VCVT32to64_ds(r, SCRATCH_F32_1);
}
LENDFUNC(NONE,NONE,1,raw_fcuts_r,(FRW r))

LOWFUNC(NONE,NONE,2,raw_frem1_rr,(FRW d, FR s))
{
	VMRS_r(REG_WORK1);
	BIC_rri(REG_WORK2, REG_WORK1, 0x00c00000);
	VMSR_r(REG_WORK2);
	
	VDIV64_ddd(SCRATCH_F64_2, d, s);
	VCVTR64toI_sd(SCRATCH_F32_1, SCRATCH_F64_2);
	VCVTIto64_ds(SCRATCH_F64_2, SCRATCH_F32_1);
	VMUL64_ddd(SCRATCH_F64_1, SCRATCH_F64_2, s);
	VSUB64_ddd(d, d, SCRATCH_F64_1);
	
	VMRS_r(REG_WORK2);
	UBFX_rrii(REG_WORK1, REG_WORK1, 22, 2);
	BFI_rrii(REG_WORK2, REG_WORK1, 22, 23);
	VMSR_r(REG_WORK2);
}
LENDFUNC(NONE,NONE,2,raw_frem1_rr,(FRW d, FR s))

LOWFUNC(NONE,NONE,2,raw_fsglmul_rr,(FRW d, FR s))
{
	VCVT64to32_sd(SCRATCH_F32_1, d);
	VCVT64to32_sd(SCRATCH_F32_2, s);
	VMUL32_sss(SCRATCH_F32_1, SCRATCH_F32_1, SCRATCH_F32_2);
	VCVT32to64_ds(d, SCRATCH_F32_1);
}
LENDFUNC(NONE,NONE,2,raw_fsglmul_rr,(FRW d, FR s))

LOWFUNC(NONE,NONE,2,raw_fmovs_rr,(FW d, FR s))
{
	VCVT64to32_sd(SCRATCH_F32_1, s);
	VCVT32to64_ds(d, SCRATCH_F32_1);
}
LENDFUNC(NONE,NONE,2,raw_fmovs_rr,(FW d, FR s))

LOWFUNC(NONE,NONE,3,raw_ffunc_rr,(double (*func)(double), FW d, FR s))
{
	VMOV64_dd(0, s);
#ifdef ARMV6T2
  MOVW_ri16(REG_WORK1, (uae_u32)func);
  MOVT_ri16(REG_WORK1, ((uae_u32)func) >> 16);
#else
	auto offs = data_long_offs(uae_u32(func));
	LDR_rRI(REG_WORK1, RPC_INDEX, offs);
#endif
	PUSH(RLR_INDEX);
	BLX_r(REG_WORK1);
	POP(RLR_INDEX);

	VMOV64_dd(d, 0);
}
LENDFUNC(NONE,NONE,3,raw_ffunc_rr,(double (*func)(double), FW d, FR s))

LOWFUNC(NONE,NONE,3,raw_fpowx_rr,(uae_u32 x, FW d, FR s))
{
	double (*func)(double,double) = pow;

	if(x == 2) {
		VMOV_F64_dimmF(0, 0x00); // load imm #2 into first reg
	} else {
		VMOV_F64_dimmF(0, 0x24); // load imm #10 into first reg
	}

	VMOV64_dd(1, s);
		
#ifdef ARMV6T2
	MOVW_ri16(REG_WORK1, (uae_u32)func);
	MOVT_ri16(REG_WORK1, ((uae_u32)func) >> 16);
#else
	auto offs = data_long_offs(uae_u32(func));
	LDR_rRI(REG_WORK1, RPC_INDEX, offs);
#endif

	PUSH(RLR_INDEX);
	BLX_r(REG_WORK1);
	POP(RLR_INDEX);

	VMOV64_dd(d, 0);
}
LENDFUNC(NONE,NONE,3,raw_fpowx_rr,(uae_u32 x, FW d, FR s))

LOWFUNC(NONE,WRITE,2,raw_fp_from_exten_mr,(RR4 adr, FR s))
{
	uae_s32 offs = (uae_u32)&NATMEM_OFFSETX - (uae_u32) &regs;
 
  VMOVi_to_ARM_rd(REG_WORK2, s, 1);    				// get high part of double
  VCMP64_d0(s);
  VMRS_CPSR();
  BEQ_i(22);          // iszero

  UBFX_rrii(REG_WORK3, REG_WORK2, 20, 11);  	// get exponent
	MOVW_ri16(REG_WORK1, 2047);
	CMP_rr(REG_WORK3, REG_WORK1);
	BEQ_i(15); 				// isnan

  MOVW_ri16(REG_WORK1, 15360);              	// diff of bias between double and long double
  ADD_rrr(REG_WORK3, REG_WORK3, REG_WORK1); 	// exponent done
  AND_rri(REG_WORK2, REG_WORK2, 0x80000000);        // extract sign
  ORR_rrrLSLi(REG_WORK3, REG_WORK2, REG_WORK3, 16); // merge sign and exponent

  LDR_rRI(REG_WORK1, R_REGSTRUCT, offs);
	ADD_rrr(REG_WORK1, adr, REG_WORK1);

  REV_rr(REG_WORK3, REG_WORK3);
  STRH_rR(REG_WORK3, REG_WORK1);             	// write exponent

  VSHL64_ddi(SCRATCH_F64_1, s, 11);             // shift mantissa to correct position
  VMOV64_rrd(REG_WORK3, REG_WORK2, SCRATCH_F64_1);
  ORR_rri(REG_WORK2, REG_WORK2, 0x80000000);  // insert explicit 1
  REV_rr(REG_WORK2, REG_WORK2);
  REV_rr(REG_WORK3, REG_WORK3);
  STR_rRI(REG_WORK2, REG_WORK1, 4);
  STR_rRI(REG_WORK3, REG_WORK1, 8);
  B_i(10);            // end_of_op

// isnan
  MOVW_ri16(REG_WORK2, 0x7fff);
  LSL_rri(REG_WORK2, REG_WORK2, 16);
  MVN_ri(REG_WORK3, 0);

// iszero
  CC_AND_rri(NATIVE_CC_EQ, REG_WORK2, REG_WORK2, 0x80000000); // extract sign
  CC_MOV_ri(NATIVE_CC_EQ, REG_WORK3, 0);

  LDR_rRI(REG_WORK1, R_REGSTRUCT, offs);
	ADD_rrr(REG_WORK1, adr, REG_WORK1);

  REV_rr(REG_WORK2, REG_WORK2);
  STR_rR(REG_WORK2, REG_WORK1);
  STR_rRI(REG_WORK3, REG_WORK1, 4);
  STR_rRI(REG_WORK3, REG_WORK1, 8);

// end_of_op
 
}
LENDFUNC(NONE,WRITE,2,raw_fp_from_exten_mr,(RR4 adr, FR s))

LOWFUNC(NONE,READ,2,raw_fp_to_exten_rm,(FW d, RR4 adr))
{
	uae_s32 offs = (uae_u32)&NATMEM_OFFSETX - (uae_u32) &regs;

  LDR_rRI(REG_WORK1, R_REGSTRUCT, offs);
	ADD_rrr(REG_WORK1, adr, REG_WORK1);

	LDR_rRI(REG_WORK2, REG_WORK1, 4);
	LDR_rRI(REG_WORK3, REG_WORK1, 8);
	REV_rr(REG_WORK2, REG_WORK2);
	REV_rr(REG_WORK3, REG_WORK3);
	BIC_rri(REG_WORK2, REG_WORK2, 0x80000000); // clear explicit 1
	VMOV64_drr(d, REG_WORK3, REG_WORK2);

	LDR_rR(REG_WORK2, REG_WORK1);
  REV_rr(REG_WORK2, REG_WORK2);
	LSR_rri(REG_WORK2, REG_WORK2, 16);	// exponent now in lower half
	MOVW_ri16(REG_WORK3, 0x7fff);
	ANDS_rrr(REG_WORK3, REG_WORK3, REG_WORK2);
	BNE_i(9);				// not_zero
	VCMP64_d0(d);
	VMRS_CPSR();
	BNE_i(6);				// not zero
// zero
	VMOV_I64_dimmI(d, 0x00);
	TST_ri(REG_WORK2, 0x8000);								// check sign
	BEQ_i(12);			// end_of_op
	MOV_ri(REG_WORK2, 0x80000000);
	MOV_ri(REG_WORK3, 0);
	VMOV64_drr(d, REG_WORK3, REG_WORK2);
	B_i(8);					// end_of_op

// not_zero
	MOVW_ri16(REG_WORK1, 15360);              // diff of bias between double and long double
	SUB_rrr(REG_WORK3, REG_WORK3, REG_WORK1);	// exponent done, ToDo: check for carry -> result gets Inf in double
	UBFX_rrii(REG_WORK2, REG_WORK2, 15, 1);		// extract sign
	BFI_rrii(REG_WORK3, REG_WORK2, 11, 11);		// insert sign
	VSHR64_ddi(d, d, 11);											// shift mantissa to correct position
	LSL_rri(REG_WORK3, REG_WORK3, 20);
	VMOV_I64_dimmI(0, 0x00);
	VMOVi_from_ARM_dr(0, REG_WORK3, 1);
	VORR_ddd(d, d, 0);
// end_of_op

}
LENDFUNC(NONE,READ,2,raw_fp_to_exten_rm,(FW d, RR4 adr))

STATIC_INLINE void raw_fflags_into_flags(int r)
{
	VCMP64_d0(r);
	VMRS_CPSR();
}

LOWFUNC(NONE,NONE,2,raw_fp_fscc_ri,(RW4 d, int cc))
{
	switch (cc) {
		case NATIVE_CC_F_NEVER:
			BIC_rri(d, d, 0xff);
			break;
			
		case NATIVE_CC_NE: // Set if not equal
			CC_BIC_rri(NATIVE_CC_EQ, d, d, 0xff); // do not set if equal
			CC_ORR_rri(NATIVE_CC_NE, d, d, 0xff);
			break;

		case NATIVE_CC_EQ: // Set if equal
			CC_BIC_rri(NATIVE_CC_NE, d, d, 0xff); // do not set if not equal
			CC_ORR_rri(NATIVE_CC_EQ, d, d, 0xff);
			break;

		case NATIVE_CC_F_OGT: // Set if valid and greater than
  		BVS_i(2);		// do not set if NaN
			BLE_i(1);		// do not set if less or equal
			ORR_rri(d, d, 0xff);
			B_i(0);
			BIC_rri(d, d, 0xff);
			break;

		case NATIVE_CC_F_OGE: // Set if valid and greater or equal
			BVS_i(2);		// do not set if NaN
			BCC_i(1);		// do not set if carry cleared
			ORR_rri(d, d, 0xff);
			B_i(0);
			BIC_rri(d, d, 0xff);
			break;
			
		case NATIVE_CC_F_OLT: // Set if vaild and less than
			BVS_i(2);		// do not set if NaN
			BCS_i(1);		// do not set if carry set
			ORR_rri(d, d, 0xff);
			B_i(0);
			BIC_rri(d, d, 0xff);
			break;
			
		case NATIVE_CC_F_OLE: // Set if valid and less or equal
			BVS_i(2);		// do not set if NaN
			BGT_i(1);		// do not set if greater than
			ORR_rri(d, d, 0xff);
			B_i(0);
			BIC_rri(d, d, 0xff);
			break;
			
		case NATIVE_CC_F_OGL: // Set if valid and greator or less
			BVS_i(2);		// do not set if NaN
			BEQ_i(1);		// do not set if equal
			ORR_rri(d, d, 0xff);
			B_i(0);
			BIC_rri(d, d, 0xff);
			break;

		case NATIVE_CC_F_OR: // Set if valid
			CC_BIC_rri(NATIVE_CC_VS, d, d, 0xff); // do not set if NaN
			CC_ORR_rri(NATIVE_CC_VC, d, d, 0xff);
			break;
			
		case NATIVE_CC_F_UN: // Set if NAN
			CC_BIC_rri(NATIVE_CC_VC, d, d, 0xff);	// do not set if valid
			CC_ORR_rri(NATIVE_CC_VS, d, d, 0xff);
			break;

		case NATIVE_CC_F_UEQ: // Set if NAN or equal
			BVS_i(0); 	// set if NaN
			BNE_i(1);		// do not set if greater or less
			ORR_rri(d, d, 0xff);
			B_i(0);
			BIC_rri(d, d, 0xff);
			break;

		case NATIVE_CC_F_UGT: // Set if NAN or greater than
			BVS_i(0); 	// set if NaN
			BLS_i(1);		// do not set if lower or same
			ORR_rri(d, d, 0xff);
			B_i(0);
			BIC_rri(d, d, 0xff);
			break;

		case NATIVE_CC_F_UGE: // Set if NAN or greater or equal
			BVS_i(0); 	// set if NaN
			BMI_i(1);		// do not set if lower
			ORR_rri(d, d, 0xff);
			B_i(0);
			BIC_rri(d, d, 0xff);
			break;

		case NATIVE_CC_F_ULT: // Set if NAN or less than
			BVS_i(0); 	// set if NaN
			BGE_i(1);		// do not set if greater or equal
			ORR_rri(d, d, 0xff);
			B_i(0);
			BIC_rri(d, d, 0xff);
			break;

		case NATIVE_CC_F_ULE: // Set if NAN or less or equal
			BVS_i(0); 	// set if NaN
			BGT_i(1);		// do not set if greater
			ORR_rri(d, d, 0xff);
			B_i(0);
			BIC_rri(d, d, 0xff);
			break;
	}
}
LENDFUNC(NONE,NONE,2,raw_fp_fscc_ri,(RW4 d, int cc))

LOWFUNC(NONE,NONE,1,raw_roundingmode,(IMM mode))
{
  VMRS_r(REG_WORK1);
  BIC_rri(REG_WORK1, REG_WORK1, 0x00c00000);
  ORR_rri(REG_WORK1, REG_WORK1, mode);
  VMSR_r(REG_WORK1);
}
LENDFUNC(NONE,NONE,1,raw_roundingmode,(IMM mode))

