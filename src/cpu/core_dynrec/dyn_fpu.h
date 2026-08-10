/*
 *  Copyright (C) 2002-2019  The DOSBox Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */



#include "dosbox.h"
#if C_FPU

#include <math.h>
#include <float.h>
#include "cross.h"
#include "mem.h"
#include "fpu.h"
#include "cpu.h"


static void FPU_FDECSTP(){
	TOP = (TOP - 1) & 7;
}

static void FPU_FINCSTP(){
	TOP = (TOP + 1) & 7;
}

static void FPU_FNSTCW(PhysPt addr){
	mem_writew(addr,fpu.cw);
}

static void FPU_FFREE(Bitu st) {
	fpu.tags[st]=TAG_Empty;
}


#if C_FPU_X86
#include "../../fpu/fpu_instructions_x86.h"
#else
#include "../../fpu/fpu_instructions.h"
#endif

/* Runtime counters are emitted into translated code only for -fpustats. */
static INLINE void dyn_fpu_stat(Bit32u *counter)
{
	if (dynrec_stats.fpu_enabled) gen_add_direct_word(counter, 1, true);
}

static INLINE void dyn_fpu_stat_instruction()
{
	dyn_fpu_stat(&dynrec_stats.fpu_instructions);
}

static INLINE void dyn_fpu_stat_fully_inlined()
{
#if C_TARGETCPU == POWERPC
	dyn_fpu_stat(&dynrec_stats.fpu_fully_inlined);
#endif
}

static INLINE void dyn_fpu_stat_fully_inlined_with_pop()
{
#if (C_TARGETCPU == POWERPC) && (DB_FPU_STACK_CHECK_POP == DB_FPU_STACK_CHECK_NONE)
	dyn_fpu_stat(&dynrec_stats.fpu_fully_inlined);
#endif
}

static INLINE void dyn_fpu_pop()
{
#if (C_TARGETCPU == POWERPC) && (DB_FPU_STACK_CHECK_POP == DB_FPU_STACK_CHECK_NONE)
	dyn_fpu_stat(&dynrec_stats.fpu_pop_ops);
	gen_ppc_fpu_pop();
#else
	gen_call_function_raw((void *)&FPU_FPOP);
#endif
}

static INLINE void dyn_fpu_free(HostReg index)
{
#if C_TARGETCPU == POWERPC
	dyn_fpu_stat(&dynrec_stats.fpu_free_ops);
	gen_ppc_fpu_free(index);
#else
	gen_call_function_R((void *)&FPU_FFREE, index);
#endif
}

static INLINE void dyn_fpu_store(HostReg source, HostReg destination)
{
#if C_TARGETCPU == POWERPC
	dyn_fpu_stat(&dynrec_stats.fpu_store_ops);
	gen_ppc_fpu_store(source, destination);
#else
	gen_call_function_RR((void *)&FPU_FST, source, destination);
#endif
}

static INLINE void dyn_fpu_exchange(HostReg first, HostReg second)
{
#if C_TARGETCPU == POWERPC
	dyn_fpu_stat(&dynrec_stats.fpu_exchange_ops);
	gen_ppc_fpu_exchange(first, second);
#else
	gen_call_function_RR((void *)&FPU_FXCH, first, second);
#endif
}

static INLINE void dyn_fpu_sign(bool absolute_value)
{
#if C_TARGETCPU == POWERPC
	dyn_fpu_stat(&dynrec_stats.fpu_sign_ops);
	gen_ppc_fpu_sign(absolute_value);
#else
	gen_call_function_raw(absolute_value ? (void *)&FPU_FABS : (void *)&FPU_FCHS);
#endif
}

static INLINE void dyn_fpu_adjust_top(Bits delta)
{
#if C_TARGETCPU == POWERPC
	dyn_fpu_stat(&dynrec_stats.fpu_top_ops);
	gen_ppc_fpu_adjust_top(delta);
#else
	gen_call_function_raw(delta < 0 ? (void *)&FPU_FDECSTP : (void *)&FPU_FINCSTP);
#endif
}

static INLINE void dyn_fpu_nop()
{
#if C_TARGETCPU == POWERPC
	dyn_fpu_stat(&dynrec_stats.fpu_nop_ops);
#else
	gen_call_function_raw((void *)&FPU_FNOP);
#endif
}


static INLINE void dyn_fpu_top() {
	gen_mov_word_to_reg(FC_OP2,(void*)(&TOP),true);
	gen_add_imm(FC_OP2,decode.modrm.rm);
	gen_and_imm(FC_OP2,7);
	gen_mov_word_to_reg(FC_OP1,(void*)(&TOP),true);
}

static INLINE void dyn_fpu_top_swapped() {
	gen_mov_word_to_reg(FC_OP1,(void*)(&TOP),true);
	gen_add_imm(FC_OP1,decode.modrm.rm);
	gen_and_imm(FC_OP1,7);
	gen_mov_word_to_reg(FC_OP2,(void*)(&TOP),true);
}

/* MorphOS/PPC performance note:
 * Keep FADD/FMUL/FSUB/FDIV on the established C helper path.  Native PPC
 * inline and shared-stub versions were benchmarked with Quake timedemo and
 * reduced performance (about 22 FPS to 14/16 FPS), mainly from extra x87
 * state loads/stores and instruction-cache pressure.  Do not re-enable the
 * native path without a register-cached FPU design and repeatable benchmarks.
 */

static void dyn_eatree() {
//	Bitu group = (decode.modrm.val >> 3) & 7;
	Bitu group = decode.modrm.reg&7; //It is already that, but compilers.
	switch (group){
	case 0x00:		// FADD ST,STi
		gen_call_function_R((void*)&FPU_FADD_EA,FC_OP1);
		break;
	case 0x01:		// FMUL  ST,STi
		gen_call_function_R((void*)&FPU_FMUL_EA,FC_OP1);
		break;
	case 0x02:		// FCOM  STi
		gen_call_function_R((void*)&FPU_FCOM_EA,FC_OP1);
		break;
	case 0x03:		// FCOMP STi
		gen_call_function_R((void*)&FPU_FCOM_EA,FC_OP1);
		dyn_fpu_pop();
		break;
	case 0x04:		// FSUB  ST,STi
		gen_call_function_R((void*)&FPU_FSUB_EA,FC_OP1);
		break;	
	case 0x05:		// FSUBR ST,STi
		gen_call_function_R((void*)&FPU_FSUBR_EA,FC_OP1);
		break;
	case 0x06:		// FDIV  ST,STi
		gen_call_function_R((void*)&FPU_FDIV_EA,FC_OP1);
		break;
	case 0x07:		// FDIVR ST,STi
		gen_call_function_R((void*)&FPU_FDIVR_EA,FC_OP1);
		break;
	default:
		break;
	}
}

static void dyn_fpu_esc0(){
	dyn_get_modrm();
	dyn_fpu_stat_instruction(); 
//	if (decode.modrm.val >= 0xc0) {
	if (decode.modrm.mod == 3) { 
		dyn_fpu_top();
		switch (decode.modrm.reg){
		case 0x00:		//FADD ST,STi
			gen_call_function_RR((void*)&FPU_FADD,FC_OP1,FC_OP2);
			break;
		case 0x01:		// FMUL  ST,STi
			gen_call_function_RR((void*)&FPU_FMUL,FC_OP1,FC_OP2);
			break;
		case 0x02:		// FCOM  STi
			gen_call_function_RR((void*)&FPU_FCOM,FC_OP1,FC_OP2);
			break;
		case 0x03:		// FCOMP STi
			gen_call_function_RR((void*)&FPU_FCOM,FC_OP1,FC_OP2);
			dyn_fpu_pop();
			break;
		case 0x04:		// FSUB  ST,STi
			gen_call_function_RR((void*)&FPU_FSUB,FC_OP1,FC_OP2);
			break;	
		case 0x05:		// FSUBR ST,STi
			gen_call_function_RR((void*)&FPU_FSUBR,FC_OP1,FC_OP2);
			break;
		case 0x06:		// FDIV  ST,STi
			gen_call_function_RR((void*)&FPU_FDIV,FC_OP1,FC_OP2);
			break;
		case 0x07:		// FDIVR ST,STi
			gen_call_function_RR((void*)&FPU_FDIVR,FC_OP1,FC_OP2);
			break;
		default:
			break;
		}
	} else { 
		dyn_fill_ea(FC_ADDR);
		gen_call_function_R((void*)&FPU_FLD_F32_EA,FC_ADDR); 
		gen_mov_word_to_reg(FC_OP1,(void*)(&TOP),true);
		dyn_eatree();
	}
}


static void dyn_fpu_esc1(){
	dyn_get_modrm();
	dyn_fpu_stat_instruction();  
//	if (decode.modrm.val >= 0xc0) { 
	if (decode.modrm.mod == 3) {
		switch (decode.modrm.reg){
		case 0x00: /* FLD STi */
			gen_mov_word_to_reg(FC_OP1,(void*)(&TOP),true);
			gen_add_imm(FC_OP1,decode.modrm.rm);
			gen_and_imm(FC_OP1,7);
			gen_protect_reg(FC_OP1);
			gen_call_function_raw((void*)&FPU_PREP_PUSH); 
			gen_mov_word_to_reg(FC_OP2,(void*)(&TOP),true);
			gen_restore_reg(FC_OP1);
			dyn_fpu_store(FC_OP1,FC_OP2);
			break;
		case 0x01: /* FXCH STi */
			dyn_fpu_stat_fully_inlined();
			dyn_fpu_top();
			dyn_fpu_exchange(FC_OP1,FC_OP2);
			break;
		case 0x02: /* FNOP */
			dyn_fpu_stat_fully_inlined();
			dyn_fpu_nop();
			break;
		case 0x03: /* FSTP STi */
			dyn_fpu_stat_fully_inlined_with_pop();
			dyn_fpu_top();
			dyn_fpu_store(FC_OP1,FC_OP2);
			dyn_fpu_pop();
			break;   
		case 0x04:
			switch(decode.modrm.rm){
			case 0x00:       /* FCHS */
				dyn_fpu_stat_fully_inlined();
				dyn_fpu_sign(false);
				break;
			case 0x01:       /* FABS */
				dyn_fpu_stat_fully_inlined();
				dyn_fpu_sign(true);
				break;
			case 0x02:       /* UNKNOWN */
			case 0x03:       /* ILLEGAL */
				LOG(LOG_FPU,LOG_WARN)("ESC 1:Unhandled group %X subfunction %X",decode.modrm.reg,decode.modrm.rm);
				break;
			case 0x04:       /* FTST */
				gen_call_function_raw((void*)&FPU_FTST);
				break;
			case 0x05:       /* FXAM */
				gen_call_function_raw((void*)&FPU_FXAM);
				break;
			case 0x06:       /* FTSTP (cyrix)*/
			case 0x07:       /* UNKNOWN */
				LOG(LOG_FPU,LOG_WARN)("ESC 1:Unhandled group %X subfunction %X",decode.modrm.reg,decode.modrm.rm);
				break;
			}
			break;
		case 0x05:
			switch(decode.modrm.rm){	
			case 0x00:       /* FLD1 */
				gen_call_function_raw((void*)&FPU_FLD1);
				break;
			case 0x01:       /* FLDL2T */
				gen_call_function_raw((void*)&FPU_FLDL2T);
				break;
			case 0x02:       /* FLDL2E */
				gen_call_function_raw((void*)&FPU_FLDL2E);
				break;
			case 0x03:       /* FLDPI */
				gen_call_function_raw((void*)&FPU_FLDPI);
				break;
			case 0x04:       /* FLDLG2 */
				gen_call_function_raw((void*)&FPU_FLDLG2);
				break;
			case 0x05:       /* FLDLN2 */
				gen_call_function_raw((void*)&FPU_FLDLN2);
				break;
			case 0x06:       /* FLDZ*/
				gen_call_function_raw((void*)&FPU_FLDZ);
				break;
			case 0x07:       /* ILLEGAL */
				LOG(LOG_FPU,LOG_WARN)("ESC 1:Unhandled group %X subfunction %X",decode.modrm.reg,decode.modrm.rm);
				break;
			}
			break;
		case 0x06:
			switch(decode.modrm.rm){
			case 0x00:	/* F2XM1 */
				gen_call_function_raw((void*)&FPU_F2XM1);
				break;
			case 0x01:	/* FYL2X */
				gen_call_function_raw((void*)&FPU_FYL2X);
				break;
			case 0x02:	/* FPTAN  */
				gen_call_function_raw((void*)&FPU_FPTAN);
				break;
			case 0x03:	/* FPATAN */
				gen_call_function_raw((void*)&FPU_FPATAN);
				break;
			case 0x04:	/* FXTRACT */
				gen_call_function_raw((void*)&FPU_FXTRACT);
				break;
			case 0x05:	/* FPREM1 */
				gen_call_function_raw((void*)&FPU_FPREM1);
				break;
			case 0x06:	/* FDECSTP */
				dyn_fpu_stat_fully_inlined();
				dyn_fpu_adjust_top(-1);
				break;
			case 0x07:	/* FINCSTP */
				dyn_fpu_stat_fully_inlined();
				dyn_fpu_adjust_top(1);
				break;
			default:
				LOG(LOG_FPU,LOG_WARN)("ESC 1:Unhandled group %X subfunction %X",decode.modrm.reg,decode.modrm.rm);
				break;
			}
			break;
		case 0x07:
			switch(decode.modrm.rm){
			case 0x00:		/* FPREM */
				gen_call_function_raw((void*)&FPU_FPREM);
				break;
			case 0x01:		/* FYL2XP1 */
				gen_call_function_raw((void*)&FPU_FYL2XP1);
				break;
			case 0x02:		/* FSQRT */
				gen_call_function_raw((void*)&FPU_FSQRT);
				break;
			case 0x03:		/* FSINCOS */
				gen_call_function_raw((void*)&FPU_FSINCOS);
				break;
			case 0x04:		/* FRNDINT */
				gen_call_function_raw((void*)&FPU_FRNDINT);
				break;
			case 0x05:		/* FSCALE */
				gen_call_function_raw((void*)&FPU_FSCALE);
				break;
			case 0x06:		/* FSIN */
				gen_call_function_raw((void*)&FPU_FSIN);
				break;
			case 0x07:		/* FCOS */
				gen_call_function_raw((void*)&FPU_FCOS);
				break;
			default:
				LOG(LOG_FPU,LOG_WARN)("ESC 1:Unhandled group %X subfunction %X",decode.modrm.reg,decode.modrm.rm);
				break;
			}
			break;
		default:
			LOG(LOG_FPU,LOG_WARN)("ESC 1:Unhandled group %X subfunction %X",decode.modrm.reg,decode.modrm.rm);
			break;
		}
	} else {
		switch(decode.modrm.reg){
		case 0x00: /* FLD float*/
			gen_call_function_raw((void*)&FPU_PREP_PUSH);
			dyn_fill_ea(FC_OP1);
			gen_mov_word_to_reg(FC_OP2,(void*)(&TOP),true);
			gen_call_function_RR((void*)&FPU_FLD_F32,FC_OP1,FC_OP2);
			break;
		case 0x01: /* UNKNOWN */
			LOG(LOG_FPU,LOG_WARN)("ESC EA 1:Unhandled group %d subfunction %d",decode.modrm.reg,decode.modrm.rm);
			break;
		case 0x02: /* FST float*/
			dyn_fill_ea(FC_ADDR);
			gen_call_function_R((void*)&FPU_FST_F32,FC_ADDR);
			break;
		case 0x03: /* FSTP float*/
			dyn_fill_ea(FC_ADDR);
			gen_call_function_R((void*)&FPU_FST_F32,FC_ADDR);
			dyn_fpu_pop();
			break;
		case 0x04: /* FLDENV */
			dyn_fill_ea(FC_ADDR);
			gen_call_function_R((void*)&FPU_FLDENV,FC_ADDR);
			break;
		case 0x05: /* FLDCW */
			dyn_fill_ea(FC_ADDR);
			gen_call_function_R((void *)&FPU_FLDCW,FC_ADDR);
			break;
		case 0x06: /* FSTENV */
			dyn_fill_ea(FC_ADDR);
			gen_call_function_R((void *)&FPU_FSTENV,FC_ADDR);
			break;
		case 0x07:  /* FNSTCW*/
			dyn_fill_ea(FC_ADDR);
			gen_call_function_R((void *)&FPU_FNSTCW,FC_ADDR);
			break;
		default:
			LOG(LOG_FPU,LOG_WARN)("ESC EA 1:Unhandled group %d subfunction %d",decode.modrm.reg,decode.modrm.rm);
			break;
		}
	}
}

static void dyn_fpu_esc2(){
	dyn_get_modrm();
	dyn_fpu_stat_instruction();  
//	if (decode.modrm.val >= 0xc0) { 
	if (decode.modrm.mod == 3) {
		switch(decode.modrm.reg){
		case 0x05:
			switch(decode.modrm.rm){
			case 0x01:		/* FUCOMPP */
				gen_mov_word_to_reg(FC_OP2,(void*)(&TOP),true);
				gen_add_imm(FC_OP2,1);
				gen_and_imm(FC_OP2,7);
				gen_mov_word_to_reg(FC_OP1,(void*)(&TOP),true);
				gen_call_function_RR((void *)&FPU_FUCOM,FC_OP1,FC_OP2);
				dyn_fpu_pop();
				dyn_fpu_pop();
				break;
			default:
				LOG(LOG_FPU,LOG_WARN)("ESC 2:Unhandled group %d subfunction %d",decode.modrm.reg,decode.modrm.rm); 
				break;
			}
			break;
		default:
	   		LOG(LOG_FPU,LOG_WARN)("ESC 2:Unhandled group %d subfunction %d",decode.modrm.reg,decode.modrm.rm);
			break;
		}
	} else {
		dyn_fill_ea(FC_ADDR);
		gen_call_function_R((void*)&FPU_FLD_I32_EA,FC_ADDR); 
		gen_mov_word_to_reg(FC_OP1,(void*)(&TOP),true);
		dyn_eatree();
	}
}

static void dyn_fpu_esc3(){
	dyn_get_modrm();
	dyn_fpu_stat_instruction();  
//	if (decode.modrm.val >= 0xc0) { 
	if (decode.modrm.mod == 3) {
		switch (decode.modrm.reg) {
		case 0x04:
			switch (decode.modrm.rm) {
			case 0x00:				//FNENI
			case 0x01:				//FNDIS
				LOG(LOG_FPU,LOG_ERROR)("8087 only fpu code used esc 3: group 4: subfuntion: %d",decode.modrm.rm);
				break;
			case 0x02:				//FNCLEX FCLEX
				gen_call_function_raw((void*)&FPU_FCLEX);
				break;
			case 0x03:				//FNINIT FINIT
				gen_call_function_raw((void*)&FPU_FINIT);
				break;
			case 0x04:				//FNSETPM
			case 0x05:				//FRSTPM
//				LOG(LOG_FPU,LOG_ERROR)("80267 protected mode (un)set. Nothing done");
				break;
			default:
				E_Exit("ESC 3:ILLEGAL OPCODE group %d subfunction %d",decode.modrm.reg,decode.modrm.rm);
			}
			break;
		default:
			LOG(LOG_FPU,LOG_WARN)("ESC 3:Unhandled group %d subfunction %d",decode.modrm.reg,decode.modrm.rm);
			break;
		}
	} else {
		switch(decode.modrm.reg){
		case 0x00:	/* FILD */
			gen_call_function_raw((void*)&FPU_PREP_PUSH);
			dyn_fill_ea(FC_OP1); 
			gen_mov_word_to_reg(FC_OP2,(void*)(&TOP),true);
			gen_call_function_RR((void*)&FPU_FLD_I32,FC_OP1,FC_OP2);
			break;
		case 0x01:	/* FISTTP */
			LOG(LOG_FPU,LOG_WARN)("ESC 3 EA:Unhandled group %d subfunction %d",decode.modrm.reg,decode.modrm.rm);
			break;
		case 0x02:	/* FIST */
			dyn_fill_ea(FC_ADDR); 
			gen_call_function_R((void*)&FPU_FST_I32,FC_ADDR);
			break;
		case 0x03:	/* FISTP */
			dyn_fill_ea(FC_ADDR); 
			gen_call_function_R((void*)&FPU_FST_I32,FC_ADDR);
			dyn_fpu_pop();
			break;
		case 0x05:	/* FLD 80 Bits Real */
			gen_call_function_raw((void*)&FPU_PREP_PUSH);
			dyn_fill_ea(FC_ADDR); 
			gen_call_function_R((void*)&FPU_FLD_F80,FC_ADDR);
			break;
		case 0x07:	/* FSTP 80 Bits Real */
			dyn_fill_ea(FC_ADDR); 
			gen_call_function_R((void*)&FPU_FST_F80,FC_ADDR);
			dyn_fpu_pop();
			break;
		default:
			LOG(LOG_FPU,LOG_WARN)("ESC 3 EA:Unhandled group %d subfunction %d",decode.modrm.reg,decode.modrm.rm);
		}
	}
}

static void dyn_fpu_esc4(){
	dyn_get_modrm();
	dyn_fpu_stat_instruction();  
//	if (decode.modrm.val >= 0xc0) { 
	if (decode.modrm.mod == 3) {
		switch(decode.modrm.reg){
		case 0x00:	/* FADD STi,ST*/
			dyn_fpu_top_swapped();
			gen_call_function_RR((void*)&FPU_FADD,FC_OP1,FC_OP2);
			break;
		case 0x01:	/* FMUL STi,ST*/
			dyn_fpu_top_swapped();
			gen_call_function_RR((void*)&FPU_FMUL,FC_OP1,FC_OP2);
			break;
		case 0x02:  /* FCOM*/
			dyn_fpu_top();
			gen_call_function_RR((void*)&FPU_FCOM,FC_OP1,FC_OP2);
			break;
		case 0x03:  /* FCOMP*/
			dyn_fpu_top();
			gen_call_function_RR((void*)&FPU_FCOM,FC_OP1,FC_OP2);
			dyn_fpu_pop();
			break;
		case 0x04:  /* FSUBR STi,ST*/
			dyn_fpu_top_swapped();
			gen_call_function_RR((void*)&FPU_FSUBR,FC_OP1,FC_OP2);
			break;
		case 0x05:  /* FSUB  STi,ST*/
			dyn_fpu_top_swapped();
			gen_call_function_RR((void*)&FPU_FSUB,FC_OP1,FC_OP2);
			break;
		case 0x06:  /* FDIVR STi,ST*/
			dyn_fpu_top_swapped();
			gen_call_function_RR((void*)&FPU_FDIVR,FC_OP1,FC_OP2);
			break;
		case 0x07:  /* FDIV STi,ST*/
			dyn_fpu_top_swapped();
			gen_call_function_RR((void*)&FPU_FDIV,FC_OP1,FC_OP2);
			break;
		default:
			break;
		}
	} else { 
		dyn_fill_ea(FC_ADDR);
		gen_call_function_R((void*)&FPU_FLD_F64_EA,FC_ADDR); 
		gen_mov_word_to_reg(FC_OP1,(void*)(&TOP),true);
		dyn_eatree();
	}
}

static void dyn_fpu_esc5(){
	dyn_get_modrm();
	dyn_fpu_stat_instruction();  
//	if (decode.modrm.val >= 0xc0) { 
	if (decode.modrm.mod == 3) {
		dyn_fpu_top();
		switch(decode.modrm.reg){
		case 0x00: /* FFREE STi */
			dyn_fpu_stat_fully_inlined();
			dyn_fpu_free(FC_OP2);
			break;
		case 0x01: /* FXCH STi*/
			dyn_fpu_stat_fully_inlined();
			dyn_fpu_exchange(FC_OP1,FC_OP2);
			break;
		case 0x02: /* FST STi */
			dyn_fpu_stat_fully_inlined();
			dyn_fpu_store(FC_OP1,FC_OP2);
			break;
		case 0x03:  /* FSTP STi*/
			dyn_fpu_stat_fully_inlined_with_pop();
			dyn_fpu_store(FC_OP1,FC_OP2);
			dyn_fpu_pop();
			break;
		case 0x04:	/* FUCOM STi */
			gen_call_function_RR((void*)&FPU_FUCOM,FC_OP1,FC_OP2);
			break;
		case 0x05:	/*FUCOMP STi */
			gen_call_function_RR((void*)&FPU_FUCOM,FC_OP1,FC_OP2);
			dyn_fpu_pop();
			break;
		default:
			LOG(LOG_FPU,LOG_WARN)("ESC 5:Unhandled group %d subfunction %d",decode.modrm.reg,decode.modrm.rm);
			break;
        }
	} else {
		switch(decode.modrm.reg){
		case 0x00:  /* FLD double real*/
			gen_call_function_raw((void*)&FPU_PREP_PUSH);
			dyn_fill_ea(FC_OP1); 
			gen_mov_word_to_reg(FC_OP2,(void*)(&TOP),true);
			gen_call_function_RR((void*)&FPU_FLD_F64,FC_OP1,FC_OP2);
			break;
		case 0x01:  /* FISTTP longint*/
			LOG(LOG_FPU,LOG_WARN)("ESC 5 EA:Unhandled group %d subfunction %d",decode.modrm.reg,decode.modrm.rm);
			break;
		case 0x02:   /* FST double real*/
			dyn_fill_ea(FC_ADDR); 
			gen_call_function_R((void*)&FPU_FST_F64,FC_ADDR);
			break;
		case 0x03:	/* FSTP double real*/
			dyn_fill_ea(FC_ADDR); 
			gen_call_function_R((void*)&FPU_FST_F64,FC_ADDR);
			dyn_fpu_pop();
			break;
		case 0x04:	/* FRSTOR */
			dyn_fill_ea(FC_ADDR); 
			gen_call_function_R((void*)&FPU_FRSTOR,FC_ADDR);
			break;
		case 0x06:	/* FSAVE */
			dyn_fill_ea(FC_ADDR); 
			gen_call_function_R((void*)&FPU_FSAVE,FC_ADDR);
			break;
		case 0x07:   /*FNSTSW */
			gen_mov_word_to_reg(FC_OP1,(void*)(&TOP),true);
			gen_call_function_R((void*)&FPU_SET_TOP,FC_OP1);
			dyn_fill_ea(FC_OP1); 
			gen_mov_word_to_reg(FC_OP2,(void*)(&fpu.sw),false);
			gen_call_function_RR((void*)&mem_writew,FC_OP1,FC_OP2);
			break;
		default:
			LOG(LOG_FPU,LOG_WARN)("ESC 5 EA:Unhandled group %d subfunction %d",decode.modrm.reg,decode.modrm.rm);
		}
	}
}

static void dyn_fpu_esc6(){
	dyn_get_modrm();
	dyn_fpu_stat_instruction();  
//	if (decode.modrm.val >= 0xc0) { 
	if (decode.modrm.mod == 3) {
		switch(decode.modrm.reg){
		case 0x00:	/*FADDP STi,ST*/
			dyn_fpu_top_swapped();
			gen_call_function_RR((void*)&FPU_FADD,FC_OP1,FC_OP2);
			break;
		case 0x01:	/* FMULP STi,ST*/
			dyn_fpu_top_swapped();
			gen_call_function_RR((void*)&FPU_FMUL,FC_OP1,FC_OP2);
			break;
		case 0x02:  /* FCOMP5*/
			dyn_fpu_top();
			gen_call_function_RR((void*)&FPU_FCOM,FC_OP1,FC_OP2);
			break;	/* TODO IS THIS ALLRIGHT ????????? */
		case 0x03:  /*FCOMPP*/
			if(decode.modrm.rm != 1) {
				LOG(LOG_FPU,LOG_WARN)("ESC 6:Unhandled group %d subfunction %d",decode.modrm.reg,decode.modrm.rm);
				return;
			}
			gen_mov_word_to_reg(FC_OP2,(void*)(&TOP),true);
			gen_add_imm(FC_OP2,1);
			gen_and_imm(FC_OP2,7);
			gen_mov_word_to_reg(FC_OP1,(void*)(&TOP),true);
			gen_call_function_RR((void*)&FPU_FCOM,FC_OP1,FC_OP2);
			dyn_fpu_pop(); /* extra pop at the bottom*/
			break;
		case 0x04:  /* FSUBRP STi,ST*/
			dyn_fpu_top_swapped();
			gen_call_function_RR((void*)&FPU_FSUBR,FC_OP1,FC_OP2);
			break;
		case 0x05:  /* FSUBP  STi,ST*/
			dyn_fpu_top_swapped();
			gen_call_function_RR((void*)&FPU_FSUB,FC_OP1,FC_OP2);
			break;
		case 0x06:	/* FDIVRP STi,ST*/
			dyn_fpu_top_swapped();
			gen_call_function_RR((void*)&FPU_FDIVR,FC_OP1,FC_OP2);
			break;
		case 0x07:  /* FDIVP STi,ST*/
			dyn_fpu_top_swapped();
			gen_call_function_RR((void*)&FPU_FDIV,FC_OP1,FC_OP2);
			break;
		default:
			break;
		}
		dyn_fpu_pop();		
	} else {
		dyn_fill_ea(FC_ADDR);
		gen_call_function_R((void*)&FPU_FLD_I16_EA,FC_ADDR); 
		gen_mov_word_to_reg(FC_OP1,(void*)(&TOP),true);
		dyn_eatree();
	}
}

static void dyn_fpu_esc7(){
	dyn_get_modrm();
	dyn_fpu_stat_instruction();  
//	if (decode.modrm.val >= 0xc0) { 
	if (decode.modrm.mod == 3) {
		switch (decode.modrm.reg){
		case 0x00: /* FFREEP STi */
			dyn_fpu_stat_fully_inlined_with_pop();
			dyn_fpu_top();
			dyn_fpu_free(FC_OP2);
			dyn_fpu_pop();
			break;
		case 0x01: /* FXCH STi*/
			dyn_fpu_stat_fully_inlined();
			dyn_fpu_top();
			dyn_fpu_exchange(FC_OP1,FC_OP2);
			break;
		case 0x02:  /* FSTP STi*/
		case 0x03:  /* FSTP STi*/
			dyn_fpu_stat_fully_inlined_with_pop();
			dyn_fpu_top();
			dyn_fpu_store(FC_OP1,FC_OP2);
			dyn_fpu_pop();
			break;
		case 0x04:
			switch(decode.modrm.rm){
				case 0x00:     /* FNSTSW AX*/
					gen_mov_word_to_reg(FC_OP1,(void*)(&TOP),true);
					gen_call_function_R((void*)&FPU_SET_TOP,FC_OP1); 
					gen_mov_word_to_reg(FC_OP1,(void*)(&fpu.sw),false);
					MOV_REG_WORD16_FROM_HOST_REG(FC_OP1,DRC_REG_EAX);
					break;
				default:
					LOG(LOG_FPU,LOG_WARN)("ESC 7:Unhandled group %d subfunction %d",decode.modrm.reg,decode.modrm.rm);
					break;
			}
			break;
		default:
			LOG(LOG_FPU,LOG_WARN)("ESC 7:Unhandled group %d subfunction %d",decode.modrm.reg,decode.modrm.rm);
			break;
		}
	} else {
		switch(decode.modrm.reg){
		case 0x00:  /* FILD Bit16s */
			gen_call_function_raw((void*)&FPU_PREP_PUSH);
			dyn_fill_ea(FC_OP1); 
			gen_mov_word_to_reg(FC_OP2,(void*)(&TOP),true);
			gen_call_function_RR((void*)&FPU_FLD_I16,FC_OP1,FC_OP2);
			break;
		case 0x01:
			LOG(LOG_FPU,LOG_WARN)("ESC 7 EA:Unhandled group %d subfunction %d",decode.modrm.reg,decode.modrm.rm);
			break;
		case 0x02:   /* FIST Bit16s */
			dyn_fill_ea(FC_ADDR); 
			gen_call_function_R((void*)&FPU_FST_I16,FC_ADDR);
			break;
		case 0x03:	/* FISTP Bit16s */
			dyn_fill_ea(FC_ADDR); 
			gen_call_function_R((void*)&FPU_FST_I16,FC_ADDR);
			dyn_fpu_pop();
			break;
		case 0x04:   /* FBLD packed BCD */
			gen_call_function_raw((void*)&FPU_PREP_PUSH);
			dyn_fill_ea(FC_OP1);
			gen_mov_word_to_reg(FC_OP2,(void*)(&TOP),true);
			gen_call_function_RR((void*)&FPU_FBLD,FC_OP1,FC_OP2);
			break;
		case 0x05:  /* FILD Bit64s */
			gen_call_function_raw((void*)&FPU_PREP_PUSH);
			dyn_fill_ea(FC_OP1);
			gen_mov_word_to_reg(FC_OP2,(void*)(&TOP),true);
			gen_call_function_RR((void*)&FPU_FLD_I64,FC_OP1,FC_OP2);
			break;
		case 0x06:	/* FBSTP packed BCD */
			dyn_fill_ea(FC_ADDR); 
			gen_call_function_R((void*)&FPU_FBST,FC_ADDR);
			dyn_fpu_pop();
			break;
		case 0x07:  /* FISTP Bit64s */
			dyn_fill_ea(FC_ADDR); 
			gen_call_function_R((void*)&FPU_FST_I64,FC_ADDR);
			dyn_fpu_pop();
			break;
		default:
			LOG(LOG_FPU,LOG_WARN)("ESC 7 EA:Unhandled group %d subfunction %d",decode.modrm.reg,decode.modrm.rm);
			break;
		}
	}
}

#endif
