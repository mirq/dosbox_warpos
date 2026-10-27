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

// some configuring defines that specify the capabilities of this architecture
// or aspects of the recompiling

// protect FC_ADDR over function calls if necessaray
//#define DRC_PROTECT_ADDR_REG

// try to use non-flags generating functions if possible
#define DRC_FLAGS_INVALIDATION
// try to replace _simple functions by code
#define DRC_FLAGS_INVALIDATION_DCODE

// type with the same size as a pointer
#define DRC_PTR_SIZE_IM Bit32u

// calling convention modifier
#define DRC_FC /* nothing */
#define DRC_CALL_CONV /* nothing */

#define DRC_USE_REGS_ADDR
#define DRC_USE_SEGS_ADDR

#if defined(_CALL_SYSV)
// disable if your toolchain doesn't provide a _SDA_BASE_ symbol (r13 constant value)
//#define USE_SDA_BASE
#endif
#ifndef __WARPOS__
#include <proto/exec.h>
#endif
// register mapping
enum HostReg {
	HOST_R0=0,
	HOST_R1,
	HOST_R2,
	HOST_R3,
	HOST_R4,
	HOST_R5,
	HOST_R6,
	HOST_R7,
	HOST_R8,
	HOST_R9,
	HOST_R10,
	HOST_R11,
	HOST_R12,
	HOST_R13,
	HOST_R14,
	HOST_R15,
	HOST_R16,
	HOST_R17,
	HOST_R18,
	HOST_R19,
	HOST_R20,
	HOST_R21,
	HOST_R22,
	HOST_R23,
	HOST_R24,  // base of paging.tlb.read[] for the PPC dynrec RAM fast path
	HOST_R25,  // base of paging.tlb.write[] for the PPC dynrec RAM fast path
	HOST_R26,  // generic non-volatile (used for inline adc/sbb)
	HOST_R27,  // points to current CacheBlockDynRec (decode.block)
	HOST_R28,  // points to fpu
	HOST_R29,  // FC_ADDR
	HOST_R30,  // points to Segs
	HOST_R31,  // points to cpu_regs

	HOST_NONE
};

static const HostReg RegParams[] = {
	HOST_R3, HOST_R4, HOST_R5, HOST_R6,
	HOST_R7, HOST_R8, HOST_R9, HOST_R10
};

#if C_FPU
#include "fpu.h"
extern FPU_rec fpu;
#endif

#if defined(USE_SDA_BASE)
extern Bit32u _SDA_BASE_[];
#endif

// register that holds function return values
#define FC_RETOP HOST_R3

// register used for address calculations, if the ABI does not
// state that this register is preserved across function calls
// then define DRC_PROTECT_ADDR_REG above
#define FC_ADDR HOST_R29

// register that points to Segs[]
#define FC_SEGS_ADDR HOST_R30
// register that points to cpu_regs[]
#define FC_REGS_ADDR HOST_R31

// register that holds the first parameter
#define FC_OP1 RegParams[0]

// register that holds the second parameter
#define FC_OP2 RegParams[1]

// special register that holds the third parameter for _R3 calls (byte accessible)
#define FC_OP3 RegParams[2]

// register that holds byte-accessible temporary values
#define FC_TMP_BA1 FC_OP2

// register that holds byte-accessible temporary values
#define FC_TMP_BA2 FC_OP1

// temporary register for LEA
#define TEMP_REG_DRC HOST_R10

#if defined(USE_FULL_TLB)
/* MorphOS/PPC merge note: ordinary guest RAM reads are frequent enough that
 * entering a C helper for every byte/word/dword dominates many DOS games.
 * r24 is loaded once with &paging.tlb.read[0] by gen_run_code().  A zero TLB
 * entry still falls back to the original checked helper, preserving MMIO,
 * paging faults, and all special PageHandler behaviour. */
#define PPC_TLB_READ_BASE HOST_R24
#define PPC_TLB_WRITE_BASE HOST_R25
#define DRC_PPC_TLB_FAST_READ 1
#define DRC_PPC_TLB_FAST_WRITE 1
#endif


#define IMM(op, regsd, rega, imm)           (((op)<<26)|((regsd)<<21)|((rega)<<16)|             (((Bit32u)(imm))&0xFFFF))
#define EXT(regsd, rega, regb, op, rc)      (  (31<<26)|((regsd)<<21)|((rega)<<16)|((regb)<<11)|          ((op)<<1)|(rc))
#define RLW(op, regs, rega, sh, mb, me, rc) (((op)<<26)|((regs) <<21)|((rega)<<16)|  ((sh)<<11)|((mb)<<6)|((me)<<1)|(rc))

#define IMM_OP(op, regsd, rega, imm)           cache_addd(IMM(op, regsd, rega, imm))
#define EXT_OP(regsd, rega, regb, op, rc)      cache_addd(EXT(regsd, rega, regb, op, rc))
#define RLW_OP(op, regs, rega, sh, mb, me, rc) cache_addd(RLW(op, regs, rega, sh, mb, me, rc))

// move a full register from reg_src to reg_dst
static void gen_mov_regs(HostReg reg_dst,HostReg reg_src)
{
	if (reg_dst != reg_src)
		EXT_OP(reg_src,reg_dst,reg_src,444,0); // or dst,src,src (mr dst,src)
}

// move a 16bit constant value into dest_reg
// the upper 16bit of the destination register may be destroyed
static void gen_mov_word_to_reg_imm(HostReg dest_reg,Bit16u imm)
{
	IMM_OP(14, dest_reg, 0, imm); // li dest,imm
}

DRC_PTR_SIZE_IM block_ptr;

// Helper for loading addresses
static HostReg INLINE gen_addr(Bit32s &addr, HostReg dest)
{
	Bit32s off;

	if ((Bit16s)addr == addr)
		return HOST_R0;

	off = addr - (Bit32s)&Segs;
	if ((Bit16s)off == off)
	{
		addr = off;
		return FC_SEGS_ADDR;
	}

	off = addr - (Bit32s)&cpu_regs;
	if ((Bit16s)off == off)
	{
		addr = off;
		return FC_REGS_ADDR;
	}

	off = addr - (Bit32s)block_ptr;
	if ((Bit16s)off == off)
	{
		addr = off;
		return HOST_R27;
	}

#if C_FPU
	off = addr - (Bit32s)&fpu;
	if ((Bit16s)off == off)
	{
		addr = off;
		return HOST_R28;
	}
#endif

#if defined(USE_SDA_BASE)
	off = addr - (Bit32s)_SDA_BASE_;
	if ((Bit16s)off == off)
	{
		addr = off;
		return HOST_R13;
	}
#endif

	IMM_OP(15, dest, 0, (addr+0x8000)>>16); // lis dest, addr@ha
	addr = (Bit16s)addr;
	return dest;
}

// move a 32bit constant value into dest_reg
static void gen_mov_dword_to_reg_imm(HostReg dest_reg,Bit32u imm)
{
	HostReg ld = gen_addr((Bit32s&)imm, dest_reg);
	if (imm || ld != dest_reg)
		IMM_OP(14, dest_reg, ld, imm);   // addi dest_reg, ldr, imm@l
}

// move a 32bit (dword==true) or 16bit (dword==false) value from memory into dest_reg
// 16bit moves may destroy the upper 16bit of the destination register
static void gen_mov_word_to_reg(HostReg dest_reg,void* data,bool dword)
{
	Bit32s addr = (Bit32s)data;
	HostReg ld = gen_addr(addr, dest_reg);
	IMM_OP(dword ? 32:40, dest_reg, ld, addr);  // lwz/lhz dest, addr@l(ld)
}

// move a 32bit (dword==true) or 16bit (dword==false) value from host memory into dest_reg
static void gen_mov_LE_word_to_reg(HostReg dest_reg,void* data, bool dword) {
	Bit32u addr = (Bit32u)data;
	gen_mov_dword_to_reg_imm(dest_reg, addr);
	EXT_OP(dest_reg, 0, dest_reg, dword ? 534 : 790, 0); // lwbrx/lhbrx dest, 0, dest
}

// move an 8bit constant value into dest_reg
// the upper 24bit of the destination register can be destroyed
// this function does not use FC_OP1/FC_OP2 as dest_reg as these
// registers might not be directly byte-accessible on some architectures
static void gen_mov_byte_to_reg_low_imm(HostReg dest_reg,Bit8u imm) {
	gen_mov_word_to_reg_imm(dest_reg, imm);
}

// move an 8bit constant value into dest_reg
// the upper 24bit of the destination register can be destroyed
// this function can use FC_OP1/FC_OP2 as dest_reg which are
// not directly byte-accessible on some architectures
static void gen_mov_byte_to_reg_low_imm_canuseword(HostReg dest_reg,Bit8u imm) {
	gen_mov_word_to_reg_imm(dest_reg, imm);
}

// move 32bit (dword==true) or 16bit (dword==false) of a register into memory
static void gen_mov_word_from_reg(HostReg src_reg,void* dest,bool dword)
{
	Bit32s addr = (Bit32s)dest;
	HostReg ld = gen_addr(addr, HOST_R8);
	IMM_OP(dword ? 36 : 44, src_reg, ld, addr);  // stw/sth src,addr@l(ld)
}

// move an 8bit value from memory into dest_reg
// the upper 24bit of the destination register can be destroyed
// this function does not use FC_OP1/FC_OP2 as dest_reg as these
// registers might not be directly byte-accessible on some architectures
static void gen_mov_byte_to_reg_low(HostReg dest_reg,void* data)
{
	Bit32s addr = (Bit32s)data;
	HostReg ld = gen_addr(addr, dest_reg);
	IMM_OP(34, dest_reg, ld, addr);  // lbz dest,addr@l(ld)
}

// move an 8bit value from memory into dest_reg
// the upper 24bit of the destination register can be destroyed
// this function can use FC_OP1/FC_OP2 as dest_reg which are
// not directly byte-accessible on some architectures
static void gen_mov_byte_to_reg_low_canuseword(HostReg dest_reg,void* data) {
	gen_mov_byte_to_reg_low(dest_reg, data);
}

// move the lowest 8bit of a register into memory
static void gen_mov_byte_from_reg_low(HostReg src_reg,void* dest)
{
	Bit32s addr = (Bit32s)dest;
	HostReg ld = gen_addr(addr, HOST_R8);
	IMM_OP(38, src_reg, ld, addr);  // stb src_reg,addr@l(ld)
}

// convert an 8bit word to a 32bit dword
// the register is zero-extended (sign==false) or sign-extended (sign==true)
static void gen_extend_byte(bool sign,HostReg reg)
{
	if (sign)
		EXT_OP(reg, reg, 0, 954, 0); // extsb reg, src
	else
		RLW_OP(21, reg, reg, 0, 24, 31, 0); // rlwinm reg, src, 0, 24, 31
}

// convert a 16bit word to a 32bit dword
// the register is zero-extended (sign==false) or sign-extended (sign==true)
static void gen_extend_word(bool sign,HostReg reg)
{
	if (sign)
		EXT_OP(reg, reg, 0, 922, 0); // extsh reg, reg
	else
		RLW_OP(21, reg, reg, 0, 16, 31, 0); // rlwinm reg, reg, 0, 16, 31
}

// add a 32bit value from memory to a full register
static void gen_add(HostReg reg,void* op)
{
	gen_mov_word_to_reg(HOST_R8, op, true); // r8 = *(Bit32u*)op
	EXT_OP(reg,reg,HOST_R8,266,0);          // add reg,reg,r8
}

// add a 32bit value from host memory to a full register
static void gen_add_LE(HostReg reg,void* op)
{
	gen_mov_LE_word_to_reg(HOST_R8, op, true); // r8 = op[0]|(op[1]<<8)|(op[2]<<16)|(op[3]<<24);
	EXT_OP(reg,reg,HOST_R8,266,0);       // add reg,reg,r8
}

// add a 32bit constant value to a full register
static void gen_add_imm(HostReg reg,Bit32u imm)
{
	if ((Bit16s)imm != (Bit32s)imm)
		IMM_OP(15, reg, reg, (imm+0x8000)>>16); // addis reg,reg,imm@ha
	if ((Bit16s)imm)
		IMM_OP(14, reg, reg, imm);              // addi reg, reg, imm@l
}

// and a 32bit constant value with a full register
static void gen_and_imm(HostReg reg,Bit32u imm) {
	Bits sbit,ebit,tbit,bbit,abit,i;

	// sbit = number of leading 0 bits
	// ebit = number of trailing 0 bits
	// tbit = number of total 0 bits
	// bbit = number of leading 1 bits
	// abit = number of trailing 1 bits

	if (imm == 0xFFFFFFFF)
		return;

	if (!imm)
		return gen_mov_word_to_reg_imm(reg, 0);

	sbit = ebit = tbit = bbit = abit = 0;
	for (i=0; i < 32; i++)
	{
		if (!(imm & (Bit32u(1) << (31-i))))
		{
			abit = 0;
			tbit++;
			if (sbit == i)
				sbit++;
			ebit++;
		}
		else
		{
			ebit = 0;
			if (bbit == i)
				bbit++;
			abit++;
		}
	}

	if (sbit + ebit == tbit)
	{
		RLW_OP(21,reg,reg,0,sbit,31-ebit,0); // rlwinm reg,reg,0,sbit,31-ebit
		return;
	}

	if (sbit >= 16)
	{
		IMM_OP(28,reg,reg,imm); // andi. reg,reg,imm
		return;
	}
	if (ebit >= 16)
	{
		IMM_OP(29,reg,reg,imm>>16); // andis. reg,reg,(imm>>16)
		return;
	}

	if (bbit + abit == (32 - tbit))
	{
		RLW_OP(21,reg,reg,0,32-abit,bbit-1,0); // rlwinm reg,reg,0,32-abit,bbit-1
		return;
	}

	IMM_OP(28, reg, HOST_R0, imm); // andi. r0, reg, imm@l
	/* MorphOS/PPC merge note: this must use the upper 16 bits.  The old
	 * `imm > 16` expression collapsed every non-trivial mask to 1 and could
	 * generate incorrect guest results for sparse 32-bit AND masks. */
	IMM_OP(29, reg, reg, imm>>16); // andis. reg, reg, imm@h
	EXT_OP(reg, reg, HOST_R0, 444, 0); // or reg, reg, r0
}

#if C_FPU
/*
 * PPC x87 fast paths.  These helpers emit native loads/stores for the simple
 * stack-manipulation instructions that do not require x87 exception handling.
 * HOST_R28 is initialized to &fpu by gen_run_code().
 */
typedef char ppc_fpu_tag_must_be_32_bit[(sizeof(FPU_Tag) == 4) ? 1 : -1];
typedef char ppc_fpu_reg_must_be_64_bit[(sizeof(FPU_Reg) == 8) ? 1 : -1];

static INLINE void gen_ppc_fpu_indexed_address(HostReg dest, HostReg index, Bitu shift)
{
	RLW_OP(21, index, dest, shift, 0, 31-shift, 0); // slwi dest,index,shift
	EXT_OP(dest, dest, HOST_R28, 266, 0);            // add dest,dest,r28
}

static void gen_ppc_fpu_adjust_top(Bits delta)
{
	gen_mov_word_to_reg(HOST_R8, (void *)&TOP, true);
	gen_add_imm(HOST_R8, delta);
	gen_and_imm(HOST_R8, 7);
	gen_mov_word_from_reg(HOST_R8, (void *)&TOP, true);
}

static void gen_ppc_fpu_free(HostReg index)
{
	gen_ppc_fpu_indexed_address(HOST_R8, index, 2);
	gen_mov_word_to_reg_imm(HOST_R9, TAG_Empty);
	IMM_OP(36, HOST_R9, HOST_R8, offsetof(FPU_rec, tags)); // stw TAG_Empty,tags[index]
}

static void gen_ppc_fpu_pop(void)
{
	gen_mov_word_to_reg(HOST_R10, (void *)&TOP, true);
	gen_ppc_fpu_indexed_address(HOST_R8, HOST_R10, 2);
	gen_mov_word_to_reg_imm(HOST_R9, TAG_Empty);
	IMM_OP(36, HOST_R9, HOST_R8, offsetof(FPU_rec, tags));
	gen_add_imm(HOST_R10, 1);
	gen_and_imm(HOST_R10, 7);
	gen_mov_word_from_reg(HOST_R10, (void *)&TOP, true);
}

static void gen_ppc_fpu_store(HostReg source, HostReg destination)
{
	/* Copy the tag. */
	gen_ppc_fpu_indexed_address(HOST_R8, source, 2);
	gen_ppc_fpu_indexed_address(HOST_R9, destination, 2);
	IMM_OP(32, HOST_R10, HOST_R8, offsetof(FPU_rec, tags));
	IMM_OP(36, HOST_R10, HOST_R9, offsetof(FPU_rec, tags));

	/* Copy both 32-bit halves of the host double. */
	gen_ppc_fpu_indexed_address(HOST_R8, source, 3);
	gen_ppc_fpu_indexed_address(HOST_R9, destination, 3);
	IMM_OP(32, HOST_R10, HOST_R8, offsetof(FPU_rec, regs));
	IMM_OP(32, HOST_R11, HOST_R8, offsetof(FPU_rec, regs) + 4);
	IMM_OP(36, HOST_R10, HOST_R9, offsetof(FPU_rec, regs));
	IMM_OP(36, HOST_R11, HOST_R9, offsetof(FPU_rec, regs) + 4);
}

static void gen_ppc_fpu_exchange(HostReg first, HostReg second)
{
	/* Swap tags. */
	gen_ppc_fpu_indexed_address(HOST_R8, first, 2);
	gen_ppc_fpu_indexed_address(HOST_R9, second, 2);
	IMM_OP(32, HOST_R10, HOST_R8, offsetof(FPU_rec, tags));
	IMM_OP(32, HOST_R11, HOST_R9, offsetof(FPU_rec, tags));
	IMM_OP(36, HOST_R11, HOST_R8, offsetof(FPU_rec, tags));
	IMM_OP(36, HOST_R10, HOST_R9, offsetof(FPU_rec, tags));

	/* Swap both halves of the register values. */
	gen_ppc_fpu_indexed_address(HOST_R8, first, 3);
	gen_ppc_fpu_indexed_address(HOST_R9, second, 3);
	IMM_OP(32, HOST_R10, HOST_R8, offsetof(FPU_rec, regs));
	IMM_OP(32, HOST_R11, HOST_R9, offsetof(FPU_rec, regs));
	IMM_OP(36, HOST_R11, HOST_R8, offsetof(FPU_rec, regs));
	IMM_OP(36, HOST_R10, HOST_R9, offsetof(FPU_rec, regs));
	IMM_OP(32, HOST_R10, HOST_R8, offsetof(FPU_rec, regs) + 4);
	IMM_OP(32, HOST_R11, HOST_R9, offsetof(FPU_rec, regs) + 4);
	IMM_OP(36, HOST_R11, HOST_R8, offsetof(FPU_rec, regs) + 4);
	IMM_OP(36, HOST_R10, HOST_R9, offsetof(FPU_rec, regs) + 4);
}

static void gen_ppc_fpu_sign(bool absolute_value)
{
	gen_mov_word_to_reg(HOST_R8, (void *)&TOP, true);
	gen_ppc_fpu_indexed_address(HOST_R9, HOST_R8, 3);
	/* MorphOS PowerPC is big-endian, so the sign bit is in the first word. */
	IMM_OP(32, HOST_R10, HOST_R9, offsetof(FPU_rec, regs));
	if (absolute_value)
		RLW_OP(21, HOST_R10, HOST_R10, 0, 1, 31, 0); // clear bit 31
	else
		IMM_OP(27, HOST_R10, HOST_R10, 0x8000);       // xoris sign bit
	IMM_OP(36, HOST_R10, HOST_R9, offsetof(FPU_rec, regs));
}
#endif


/* Emit the common TLB lookup.  The returned branch points at the zero-entry
 * fallback and is patched by decoder_basic.h after the direct access is emitted. */
static Bit32u gen_ppc_tlb_lookup(HostReg address, HostReg tlb_base)
{
	RLW_OP(21, address, HOST_R8, 20, 12, 31, 0); // srwi r8,address,12
	RLW_OP(21, HOST_R8, HOST_R8, 2, 0, 29, 0);   // slwi r8,r8,2
	EXT_OP(HOST_R9, tlb_base, HOST_R8, 23, 0);    // lwzx r9,tlb_base,r8
	IMM_OP(11, 0, HOST_R9, 0);                    // cmpwi cr0,r9,0
	IMM_OP(16, 0x0c, 2, 0);                       // beq slow path
	return (Bit32u)cache.pos - 4;
}

static Bit32u gen_ppc_tlb_read_lookup(HostReg address)
{
	return gen_ppc_tlb_lookup(address, PPC_TLB_READ_BASE);
}

static Bit32u gen_ppc_tlb_write_lookup(HostReg address)
{
	return gen_ppc_tlb_lookup(address, PPC_TLB_WRITE_BASE);
}

static Bit32u gen_ppc_unaligned_branch(HostReg address, Bit16u mask)
{
	IMM_OP(28, address, HOST_R0, mask); // andi. r0,address,mask
	IMM_OP(16, 0x04, 2, 0);             // bne slow path
	return (Bit32u)cache.pos - 4;
}

static void gen_ppc_direct_read_byte(HostReg destination, HostReg address)
{
	EXT_OP(destination, HOST_R9, address, 87, 0); // lbzx destination,r9,address
}

static void gen_ppc_direct_read_word(HostReg destination, HostReg address, bool dword)
{
	if (dword)
		EXT_OP(destination, HOST_R9, address, 534, 0); // lwbrx destination,r9,address
	else
		EXT_OP(destination, HOST_R9, address, 790, 0); // lhbrx destination,r9,address
}

/* MorphOS/PPC merge note: write fast paths use paging.tlb.write[], never the
 * read TLB.  A zero write entry deliberately falls back to the checked helper;
 * code pages clear PFLAG_WRITEABLE, so self-modifying code invalidation, MMIO,
 * VGA handlers, and page faults retain their original behaviour. */
static void gen_ppc_direct_write_byte(HostReg value, HostReg address)
{
	EXT_OP(value, HOST_R9, address, 215, 0); // stbx value,r9,address
}

static void gen_ppc_direct_write_word(HostReg value, HostReg address, bool dword)
{
	if (dword)
		EXT_OP(value, HOST_R9, address, 662, 0); // stwbrx value,r9,address
	else
		EXT_OP(value, HOST_R9, address, 918, 0); // sthbrx value,r9,address
}

static Bit32u gen_ppc_branch_always(void)
{
	IMM_OP(16, 0x14, 0, 0); // bc 20,0,target: unconditional short branch
	return (Bit32u)cache.pos - 4;
}


// move a 32bit constant value into memory
static void gen_mov_direct_dword(void* dest,Bit32u imm) {
	gen_mov_dword_to_reg_imm(HOST_R9, imm);
	gen_mov_word_from_reg(HOST_R9, dest, 1);
}

// move an address into memory (assumes address != NULL)
static void INLINE gen_mov_direct_ptr(void* dest,DRC_PTR_SIZE_IM imm)
{
	block_ptr = 0;
	gen_mov_dword_to_reg_imm(HOST_R27, imm);
	// this will be used to look-up the linked blocks
	block_ptr = imm;
	gen_mov_word_from_reg(HOST_R27, dest, 1);
}

// add a 32bit (dword==true) or 16bit (dword==false) constant value to a 32bit memory value
static void gen_add_direct_word(void* dest,Bit32u imm,bool dword)
{
	HostReg ld;
	Bit32s addr = (Bit32s)dest;

	if (!dword)
	{
		imm &= 0xFFFF;
		addr += 2;
	}

	if (!imm)
		return;

	ld = gen_addr(addr, HOST_R8);
	IMM_OP(dword ? 32 : 40, HOST_R9, ld, addr); // lwz/lhz r9, addr@l(ld)
	if (dword && (Bit16s)imm != (Bit32s)imm)
		IMM_OP(15, HOST_R9, HOST_R9, (imm+0x8000)>>16); // addis r9,r9,imm@ha
	if (!dword || (Bit16s)imm)
		IMM_OP(14, HOST_R9, HOST_R9, imm);      // addi r9,r9,imm@l
	IMM_OP(dword ? 36 : 44, HOST_R9, ld, addr); // stw/sth r9, addr@l(ld)
}

// subtract a 32bit (dword==true) or 16bit (dword==false) constant value from a 32-bit memory value
static void gen_sub_direct_word(void* dest,Bit32u imm,bool dword) {
	gen_add_direct_word(dest, -(Bit32s)imm, dword);
}

// effective address calculation, destination is dest_reg
// scale_reg is scaled by scale (scale_reg*(2^scale)) and
// added to dest_reg, then the immediate value is added
static INLINE void gen_lea(HostReg dest_reg,HostReg scale_reg,Bitu scale,Bits imm)
{
	if (scale)
	{
		RLW_OP(21, scale_reg, HOST_R8, scale, 0, 31-scale, 0); // slwi scale_reg,r8,scale
		scale_reg = HOST_R8;
	}

	gen_add_imm(dest_reg, imm);
	EXT_OP(dest_reg, dest_reg, scale_reg, 266, 0); // add dest,dest,scaled
}

// effective address calculation, destination is dest_reg
// dest_reg is scaled by scale (dest_reg*(2^scale)),
// then the immediate value is added
static INLINE void gen_lea(HostReg dest_reg,Bitu scale,Bits imm)
{
	if (scale)
	{
		RLW_OP(21, dest_reg, dest_reg, scale, 0, 31-scale, 0); // slwi dest,dest,scale
	}

	gen_add_imm(dest_reg, imm);
}

// helper function to choose direct or indirect call
static int INLINE do_gen_call(void *func, Bit32u *pos, bool pad)
{
	Bit32s f = (Bit32s)func;
	Bit32s off = f - (Bit32s)pos;

	// relative branches are limited to +/- ~32MB
	if (off < 0x02000000 && off >= -0x02000000)
	{
		pos[0] = 0x48000001 | (off & 0x03FFFFFC); // bl func
		if (pad)
		{
			pos[1] = 0x4800000C;       // b 12+
			pos[2] = pos[3] = IMM(24, 0, 0, 0); // nop
			return 16;
		}
		return 4;
	}

	pos[0] = IMM(15, HOST_R8, 0, f>>16);      // lis r8,imm@h
	pos[1] = IMM(24, HOST_R8, HOST_R8, f);    // ori r8,r8,imm@l
	pos[2] = EXT(HOST_R8, 9, 0, 467, 0);      // mtctr r8
	pos[3] = IMM(19, 0x14, 0, (528<<1)|1); // bctrl
	return 16;
}

// Generate a normal helper call.  In the MorphOS build the JIT arena and
// helper functions live in the executable text hunk, so the common case is a
// single 4-byte relative BL.  A far helper still uses the 16-byte indirect
// sequence produced by do_gen_call().
static void INLINE gen_call_function_raw(void * func)
{
	cache.pos += do_gen_call(func, (Bit32u*)cache.pos, false);
}

// Generate a fixed-size call site which may later be rewritten by
// gen_fill_function_ptr().  Near helpers use BL followed by a branch and two
// NOPs; far helpers already occupy the complete 16-byte slot.
#define DRC_HAS_PATCHABLE_FUNCTION_CALLS 1
#define DRC_HAS_DIRECT_ZF_BRANCH 1
#define DRC_HAS_INLINE_ZF_PRODUCER 1
static void INLINE gen_call_function_patchable(void * func)
{
	cache.pos += do_gen_call(func, (Bit32u*)cache.pos, true);
}

/* Locate data through a base register that is already live in generated code.
 * This keeps the direct lazy-flags producer inside its fixed 16-byte call slot
 * and avoids adding work to the dynrec entry prologue. */
static bool ppc_direct_data_address(const void *data,HostReg &base,Bit16s &disp)
{
	const Bit32s addr=(Bit32s)data;
	Bit32s off=addr-(Bit32s)&Segs;
	if ((Bit16s)off==off) {
		base=FC_SEGS_ADDR;
		disp=(Bit16s)off;
		return true;
	}

	off=addr-(Bit32s)&cpu_regs;
	if ((Bit16s)off==off) {
		base=FC_REGS_ADDR;
		disp=(Bit16s)off;
		return true;
	}

	return false;
}

/* Replace the not-yet-executed full lazy-flags helper call belonging to an
 * immediately adjacent CMP/TEST/AND/OR/XOR + JZ/JNZ pair.  Logic and TEST
 * need only res+type to preserve every x86 condition flag.  CMP also needs
 * var1+var2; its type store is deferred until immediately before Jcc because
 * six instructions do not fit in the fixed four-instruction slot.
 *
 * Returns true when the helper was removed.  deferred_type is true only for
 * CMP and tells the architecture-independent decoder to emit lflags.type after
 * dyn_reduce_cycles() and before testing lflags.res. */
static bool gen_fill_direct_zf_producer(Bit8u *pos,Bitu flags_type,bool &deferred_type)
{
	Bit32u *const op=(Bit32u*)pos;
	HostReg base1=HOST_NONE,base2=HOST_NONE,base_res=HOST_NONE,base_type=HOST_NONE;
	Bit16s off1=0,off2=0,off_res=0,off_type=0;
	Bitu store_op=36;

	deferred_type=false;
	switch (flags_type) {
	case t_CMPb: case t_TESTb: case t_ANDb: case t_ORb: case t_XORb:
		store_op=38;
		break;
	case t_CMPw: case t_TESTw: case t_ANDw: case t_ORw: case t_XORw:
		store_op=44;
		break;
	case t_CMPd: case t_TESTd: case t_ANDd: case t_ORd: case t_XORd:
		store_op=36;
		break;
	default:
		return false;
	}

	void *var1_ptr=0;
	void *var2_ptr=0;
	void *res_ptr=0;
	switch (flags_type) {
	case t_CMPb: case t_TESTb: case t_ANDb: case t_ORb: case t_XORb:
		var1_ptr=&lf_var1b; var2_ptr=&lf_var2b; res_ptr=&lf_resb;
		break;
	case t_CMPw: case t_TESTw: case t_ANDw: case t_ORw: case t_XORw:
		var1_ptr=&lf_var1w; var2_ptr=&lf_var2w; res_ptr=&lf_resw;
		break;
	default:
		var1_ptr=&lf_var1d; var2_ptr=&lf_var2d; res_ptr=&lf_resd;
		break;
	}

	if (!ppc_direct_data_address(res_ptr,base_res,off_res)) return false;

	switch (flags_type) {
	case t_CMPb: case t_CMPw: case t_CMPd:
		if (!ppc_direct_data_address(var1_ptr,base1,off1) ||
		    !ppc_direct_data_address(var2_ptr,base2,off2)) return false;
		op[0]=IMM(store_op,FC_OP1,base1,off1);
		op[1]=IMM(store_op,FC_OP2,base2,off2);
		op[2]=EXT(FC_RETOP,FC_OP2,FC_OP1,40,0); // subf FC_RETOP,FC_OP2,FC_OP1
		op[3]=IMM(store_op,FC_RETOP,base_res,off_res);
		deferred_type=true;
		return true;
	default:
		break;
	}

	/* Resolve every store before modifying the call slot so a failed address
	 * check leaves the original helper call completely intact. */
	if (!ppc_direct_data_address(&lflags.type,base_type,off_type)) return false;

	switch (flags_type) {
	case t_TESTb: case t_TESTw: case t_TESTd:
	case t_ANDb: case t_ANDw: case t_ANDd:
		op[0]=EXT(FC_OP1,FC_RETOP,FC_OP2,28,0); // and FC_RETOP,FC_OP1,FC_OP2
		break;
	case t_ORb: case t_ORw: case t_ORd:
		op[0]=EXT(FC_OP1,FC_RETOP,FC_OP2,444,0); // or FC_RETOP,FC_OP1,FC_OP2
		break;
	case t_XORb: case t_XORw: case t_XORd:
		op[0]=EXT(FC_OP1,FC_RETOP,FC_OP2,316,0); // xor FC_RETOP,FC_OP1,FC_OP2
		break;
	default:
		return false;
	}

	op[1]=IMM(store_op,FC_RETOP,base_res,off_res);
	op[2]=IMM(14,HOST_R0,HOST_R0,flags_type); // li r0,flags_type
	op[3]=IMM(36,HOST_R0,base_type,off_type);  // stw r0,lflags.type
	return true;
}

// generate a call to a function with paramcount parameters
// note: the parameters are loaded in the architecture specific way
// using the gen_load_param_ functions below
static Bit32u INLINE gen_call_function_setup(void * func,Bitu paramcount,bool fastcall=false)
{
	(void)paramcount;
	(void)fastcall;
	Bit32u proc_addr=(Bit32u)cache.pos;
	gen_call_function_raw(func);
	return proc_addr;
}

static Bit32u INLINE gen_call_function_setup_patchable(void * func,Bitu paramcount,bool fastcall=false)
{
	(void)paramcount;
	(void)fastcall;
	Bit32u proc_addr=(Bit32u)cache.pos;
	gen_call_function_patchable(func);
	return proc_addr;
}

// load an immediate value as param'th function parameter
static void INLINE gen_load_param_imm(Bitu imm,Bitu param) {
	gen_mov_dword_to_reg_imm(RegParams[param], imm);
}

// load an address as param'th function parameter
static void INLINE gen_load_param_addr(Bitu addr,Bitu param) {
	gen_load_param_imm(addr, param);
}

// load a host-register as param'th function parameter
static void INLINE gen_load_param_reg(Bitu reg,Bitu param) {
	gen_mov_regs(RegParams[param], (HostReg)reg);
}

// load a value from memory as param'th function parameter
static void INLINE gen_load_param_mem(Bitu mem,Bitu param) {
	gen_mov_word_to_reg(RegParams[param], (void*)mem, true);
}

/* Build one unconditional relative PPC branch.  The LI field is signed and
 * scaled by four, giving a range of -32 MiB through +32 MiB-4. */
static bool ppc_make_direct_branch(Bit8u *from,Bit8u *target,Bit32u &opcode)
{
	const Bit32s off=(Bit32s)target-(Bit32s)from;
	if ((off & 3) || off < -0x02000000 || off >= 0x02000000) return false;
	opcode=0x48000000 | ((Bit32u)off & 0x03fffffc);
	return true;
}

// jump to an address pointed at by ptr, offset is in imm
static void gen_jmp_ptr(void * ptr,Bits imm=0) {
#if defined(__MORPHOS__)
	/*
	 * Default MorphOS build is tuned for G5.  The first version of a link
	 * branches to the resolver stub; LinkTo() later patches these same four
	 * bytes directly to the translated target.  A -DG4 build uses the same
	 * correct direct-link mechanism, while retaining separate cache-line
	 * fallback tuning in cache.h.
	 */
	if (dynrec_direct_links_enabled() &&
	    imm==(Bits)offsetof(CacheBlockDynRec,cache.start) && cache.block.active) {
		CacheBlockDynRec *const block=cache.block.active;
		Bitu index=2;
		if (ptr==(void *)&block->link[0].to) index=0;
		else if (ptr==(void *)&block->link[1].to) index=1;
		if (index<2) {
			Bit32u opcode=0;
			Bit8u *const patch=cache.pos;
			if (ppc_make_direct_branch(patch,link_blocks[index].cache.start,opcode)) {
				block->link[index].patch=patch;
				cache_addd(opcode);
				dynrec_stats.direct_link_sites++;
				return;
			}
			dynrec_stats.direct_links_out_of_range++;
		}
	}
#endif
	gen_mov_word_to_reg(HOST_R8,ptr,true);                // r8 = *(Bit32u*)ptr
	if ((Bit16s)imm != (Bit32s)imm)
		IMM_OP(15, HOST_R8, HOST_R8, (imm + 0x8000)>>16); // addis r8, r8, imm@ha
	IMM_OP(32, HOST_R8, HOST_R8, imm);                    // lwz r8, imm@l(r8)
	EXT_OP(HOST_R8, 9, 0, 467, 0);                        // mtctr r8
	IMM_OP(19, 0x14, 0, 528<<1);                         // bctr
}

// short conditional jump (+-127 bytes) if register is zero
// the destination is set by gen_fill_branch() later
static Bit32u gen_create_branch_on_zero(HostReg reg,bool dword)
{
	if (!dword)
		IMM_OP(28,reg,HOST_R0,0xFFFF); // andi. r0,reg,0xFFFF
	else
		IMM_OP(11, 0, reg, 0);         // cmpwi cr0, reg, 0

	IMM_OP(16, 0x0C, 2, 0); // bc 12,CR0[Z] (beq)
	return ((Bit32u)cache.pos-4);
}

// short conditional jump (+-127 bytes) if register is nonzero
// the destination is set by gen_fill_branch() later
static Bit32u gen_create_branch_on_nonzero(HostReg reg,bool dword)
{
	if (!dword)
		IMM_OP(28,reg,HOST_R0,0xFFFF); // andi. r0,reg,0xFFFF
	else
		IMM_OP(11, 0, reg, 0);         // cmpwi cr0, reg, 0

	IMM_OP(16, 0x04, 2, 0); // bc 4,CR0[Z] (bne)
	return ((Bit32u)cache.pos-4);
}

// calculate relative offset and fill it into the location pointed to by data
static void gen_fill_branch(DRC_PTR_SIZE_IM data)
{
#if C_DEBUG
	Bits len=(Bit32u)cache.pos-data;
	if (len<0) len=-len;
	if (len >= 0x8000) LOG_MSG("Big jump %d",len);
#endif

	((Bit16u*)data)[1] =((Bit32u)cache.pos-data) & 0xFFFC;
}


// conditional jump if register is nonzero
// for isdword==true the 32bit of the register are tested
// for isdword==false the lowest 8bit of the register are tested
static Bit32u gen_create_branch_long_nonzero(HostReg reg,bool dword)
{
	if (!dword)
		IMM_OP(28,reg,HOST_R0,0xFF); // andi. r0,reg,0xFF
	else
		IMM_OP(11, 0, reg, 0);       // cmpwi cr0, reg, 0

	IMM_OP(16, 0x04, 2, 0); // bne
	return ((Bit32u)cache.pos-4);
}

// compare 32bit-register against zero and jump if value less/equal than zero
static Bit32u gen_create_branch_long_leqzero(HostReg reg)
{
	IMM_OP(11, 0, reg, 0); // cmpwi cr0, reg, 0

	IMM_OP(16, 0x04, 1, 0); // ble
	return ((Bit32u)cache.pos-4);
}

// calculate long relative offset and fill it into the location pointed to by data
static void gen_fill_branch_long(Bit32u data) {
	return gen_fill_branch((DRC_PTR_SIZE_IM)data);
}

#ifdef __APPLE__
extern "C" void sys_icache_invalidate(void* start, size_t len);
#elif defined(__WARPOS__)
#include "wos_platform.h"
#endif

static void cache_block_closing(Bit8u* block_start,Bitu block_size)
{
#ifdef __APPLE__
	sys_icache_invalidate((void*)block_start, (size_t)block_size);
#elif defined(__WARPOS__)
	WOS_FlushCodeCache(block_start, (size_t)block_size);
#else
	CacheFlushDataInstArea(block_start, block_size);
#endif
}

#if defined(__MORPHOS__)
static bool dynrec_direct_links_enabled(void)
{
	return dynrec_stats.direct_links_enabled;
}

static bool dynrec_patch_link(Bit8u *patch,Bit8u *target)
{
	Bit32u opcode=0;
	if (!patch || !target || !ppc_make_direct_branch(patch,target,opcode)) return false;
	*(Bit32u *)patch=opcode;
	/* MorphOS performs the required D-cache writeback and I-cache
	 * invalidation.  This is particularly important on split-cache G5s. */
	cache_block_closing(patch,sizeof(opcode));
	return true;
}
#endif

static void cache_block_before_close(void) {}

static void gen_function(void* func)
{
	Bit32s off = (Bit32s)func - (Bit32s)cache.pos;

	// relative branches are limited to +/- 32MB
	if (off < 0x02000000 && off >= -0x02000000) {
		cache_addd(0x48000000 | (off & 0x03FFFFFC)); // b func
		return;
	}

	gen_mov_dword_to_reg_imm(HOST_R8, (Bit32u)func); // r8 = func
	EXT_OP(HOST_R8, 9, 0, 467, 0);  // mtctr r8
	IMM_OP(19, 0x14, 0, 528<<1); // bctr
}

// gen_run_code is assumed to be called exactly once, gen_return_function() jumps back to it
static void* epilog_addr;
static Bit8u *getCF_glue;
static void gen_run_code(void)
{
	// prolog
	IMM_OP(37, HOST_R1, HOST_R1, -256); // stwu sp,-256(sp)
	EXT_OP(FC_OP1, 9, 0, 467, 0); // mtctr FC_OP1
	EXT_OP(HOST_R0, 8, 0, 339, 0); // mflr r0

#if defined(DRC_PPC_TLB_FAST_READ)
	/* r24/r25 are non-volatile under the MorphOS PPC ABI.  Save them because
	 * they are reserved for the direct read/write TLB bases. */
	IMM_OP(47, HOST_R24, HOST_R1, 120); // stmw r24, 120(sp)
#else
	IMM_OP(47, HOST_R26, HOST_R1, 128); // stmw r26, 128(sp)
#endif

	IMM_OP(15, FC_SEGS_ADDR, 0, ((Bit32u)&Segs)>>16);  // lis FC_SEGS_ADDR, Segs@h
	IMM_OP(24, FC_SEGS_ADDR, FC_SEGS_ADDR, &Segs);     // ori FC_SEGS_ADDR, FC_SEGS_ADDR, Segs@l

	IMM_OP(15, FC_REGS_ADDR, 0, ((Bit32u)&cpu_regs)>>16);  // lis FC_REGS_ADDR, cpu_regs@h
	IMM_OP(24, FC_REGS_ADDR, FC_REGS_ADDR, &cpu_regs);     // ori FC_REGS_ADDR, FC_REGS_ADDR, cpu_regs@l

#if defined(DRC_PPC_TLB_FAST_READ)
	IMM_OP(15, PPC_TLB_READ_BASE, 0, ((Bit32u)&paging.tlb.read[0])>>16);
	IMM_OP(24, PPC_TLB_READ_BASE, PPC_TLB_READ_BASE, &paging.tlb.read[0]);
	IMM_OP(15, PPC_TLB_WRITE_BASE, 0, ((Bit32u)&paging.tlb.write[0])>>16);
	IMM_OP(24, PPC_TLB_WRITE_BASE, PPC_TLB_WRITE_BASE, &paging.tlb.write[0]);
#endif

#if C_FPU
	IMM_OP(15, HOST_R28, 0, ((Bit32u)&fpu)>>16);  // lis r28, fpu@h
	IMM_OP(24, HOST_R28, HOST_R28, &fpu);         // ori r28, r28, fpu@l
#endif

	IMM_OP(36, HOST_R0, HOST_R1, 256+4); // stw r0,256+4(sp)
	IMM_OP(19, 0x14, 0, 528<<1);     // bctr

	// epilog
	epilog_addr = cache.pos;
	IMM_OP(32, HOST_R0, HOST_R1, 256+4); // lwz r0,256+4(sp)
#if defined(DRC_PPC_TLB_FAST_READ)
	IMM_OP(46, HOST_R24, HOST_R1, 120);   // lmw r24, 120(sp)
#else
	IMM_OP(46, HOST_R26, HOST_R1, 128);   // lmw r26, 128(sp)
#endif
	EXT_OP(HOST_R0, 8, 0, 467, 0);       // mtlr r0
	IMM_OP(14, HOST_R1, HOST_R1, 256);   // addi sp, sp, 256
	IMM_OP(19, 0x14, 0, 16<<1);       // blr

	// trampoline to call get_CF()
	getCF_glue = cache.pos;
	gen_function((void*)get_CF);
}

// return from a function
static void gen_return_function(void)
{
	gen_function(epilog_addr);
}

// called when a call to a function can be replaced by a
// call to a simpler function
static void gen_fill_function_ptr(Bit8u * pos,void* fct_ptr,Bitu flags_type)
{
	Bit32u *op = (Bit32u*)pos;
	Bit32u *end = op+4;

	switch (flags_type) {
#if defined(DRC_FLAGS_INVALIDATION_DCODE)
		// try to avoid function calls but rather directly fill in code
		case t_ADDb:
		case t_ADDw:
		case t_ADDd:
			*op++ = EXT(FC_RETOP, FC_OP1, FC_OP2, 266, 0); // add FC_RETOP, FC_OP1, FC_OP2
			break;
		case t_ORb:
		case t_ORw:
		case t_ORd:
			*op++ = EXT(FC_OP1, FC_RETOP, FC_OP2, 444, 0); // or FC_RETOP, FC_OP1, FC_OP2
			break;
		case t_ADCb:
		case t_ADCw:
		case t_ADCd:
			op[0] = EXT(HOST_R26, FC_OP1, FC_OP2, 266, 0); // r26 = FC_OP1 + FC_OP2
			op[1] = 0x48000001 | ((getCF_glue-(pos+4)) & 0x03FFFFFC); // bl get_CF
			op[2] = IMM(12, HOST_R0, FC_RETOP, -1);        // addic r0, FC_RETOP, 0xFFFFFFFF (XER[CA] = !!CF)
			op[3] = EXT(FC_RETOP, HOST_R26, 0, 202, 0);    // addze; FC_RETOP = r26 + !!CF
			return;
		case t_SBBb:
		case t_SBBw:
		case t_SBBd:
			op[0] = EXT(HOST_R26, FC_OP2, FC_OP1, 40, 0);  // r26 = FC_OP1 - FC_OP2
			op[1] = 0x48000001 | ((getCF_glue-(pos+4)) & 0x03FFFFFC); // bl get_CF
			op[2] = IMM(8, HOST_R0, FC_RETOP, 0);          // subfic r0, FC_RETOP, 0 (XER[CA] = !CF)
			op[3] = EXT(FC_RETOP, HOST_R26, 0, 234, 0);    // addme; FC_RETOP = r26 - 1 + !CF
			return;
		case t_ANDb:
		case t_ANDw:
		case t_ANDd:
			*op++ = EXT(FC_OP1, FC_RETOP, FC_OP2, 28, 0); // and FC_RETOP, FC_OP1, FC_OP2
			break;
		case t_SUBb:
		case t_SUBw:
		case t_SUBd:
			*op++ = EXT(FC_RETOP, FC_OP2, FC_OP1, 40, 0); // subf FC_RETOP, FC_OP2, FC_OP1
			break;
		case t_XORb:
		case t_XORw:
		case t_XORd:
			*op++ = EXT(FC_OP1, FC_RETOP, FC_OP2, 316, 0); // xor FC_RETOP, FC_OP1, FC_OP2
			break;
		case t_CMPb:
		case t_CMPw:
		case t_CMPd:
		case t_TESTb:
		case t_TESTw:
		case t_TESTd:
			break;
		case t_INCb:
		case t_INCw:
		case t_INCd:
			*op++ = IMM(14, FC_RETOP, FC_OP1, 1); // addi FC_RETOP, FC_OP1, #1
			break;
		case t_DECb:
		case t_DECw:
		case t_DECd:
			*op++ = IMM(14, FC_RETOP, FC_OP1, -1); // addi FC_RETOP, FC_OP1, #-1
			break;
		case t_NEGb:
		case t_NEGw:
		case t_NEGd:
			*op++ = EXT(FC_RETOP, FC_OP1, 0, 104, 0); // neg FC_RETOP, FC_OP1
			break;
		case t_SHLb:
		case t_SHLw:
		case t_SHLd:
			*op++ = EXT(FC_OP1, FC_RETOP, FC_OP2, 24, 0); // slw FC_RETOP, FC_OP1, FC_OP2
			break;
		case t_SHRb:
		case t_SHRw:
		case t_SHRd:
			*op++ = EXT(FC_OP1, FC_RETOP, FC_OP2, 536, 0); // srw FC_RETOP, FC_OP1, FC_OP2
			break;
		case t_SARb:
			/* MorphOS/PPC merge note: sraw must consume the sign-extended
			 * temporary.  The old fall-through used FC_OP1 again, discarding
			 * the extsb result and treating negative bytes as positive values. */
			*op++ = EXT(FC_OP1, FC_RETOP, 0, 954, 0);       // extsb FC_RETOP, FC_OP1
			*op++ = EXT(FC_RETOP, FC_RETOP, FC_OP2, 792, 0); // sraw FC_RETOP, FC_RETOP, FC_OP2
			break;
		case t_SARw:
			/* Same issue as the byte form: shift the extsh result, not FC_OP1. */
			*op++ = EXT(FC_OP1, FC_RETOP, 0, 922, 0);       // extsh FC_RETOP, FC_OP1
			*op++ = EXT(FC_RETOP, FC_RETOP, FC_OP2, 792, 0); // sraw FC_RETOP, FC_RETOP, FC_OP2
			break;
		case t_SARd:
			*op++ = EXT(FC_OP1, FC_RETOP, FC_OP2, 792, 0); // sraw FC_RETOP, FC_OP1, FC_OP2
			break;

		case t_ROLb:
			*op++ = RLW(20, FC_OP1, FC_OP1, 24, 0, 7, 0); // rlwimi FC_OP1, FC_OP1, 24, 0, 7
		case t_ROLw:
			if (flags_type == t_ROLw)
				*op++ = RLW(20, FC_OP1, FC_OP1, 16, 0, 15, 0); // rlwimi FC_OP1, FC_OP1, 16, 0, 15
		case t_ROLd:
			*op++ = RLW(23, FC_OP1, FC_RETOP, FC_OP2, 0, 31, 0); // rotlw FC_RETOP, FC_OP1, FC_OP2
			break;

		case t_RORb:
			*op++ = RLW(20, FC_OP1, FC_OP1, 8, 16, 23, 0); // rlwimi FC_OP1, FC_OP1, 8, 16, 23
		case t_RORw:
			if (flags_type == t_RORw)
				*op++ = RLW(20, FC_OP1, FC_OP1, 16, 0, 15, 0); // rlwimi FC_OP1, FC_OP1, 16, 0, 15
		case t_RORd:
			*op++ = IMM(8, FC_OP2, FC_OP2, 32); // subfic FC_OP2, FC_OP2, 32 (FC_OP2 = 32 - FC_OP2)
			*op++ = RLW(23, FC_OP1, FC_RETOP, FC_OP2, 0, 31, 0); // rotlw FC_RETOP, FC_OP1, FC_OP2
			break;

		case t_DSHLw: // technically not correct for FC_OP3 > 16
			*op++ = RLW(20, FC_OP2, FC_RETOP, 16, 0, 15, 0); // rlwimi FC_RETOP, FC_OP2, 16, 0, 5
			*op++ = RLW(23, FC_RETOP, FC_RETOP, FC_OP3, 0, 31, 0); // rotlw FC_RETOP, FC_RETOP, FC_OP3
			break;
		case t_DSHLd:
			op[0] = EXT(FC_OP1, FC_RETOP, FC_OP3, 24, 0); // slw FC_RETOP, FC_OP1, FC_OP3
			op[1] = IMM(8, FC_OP3, FC_OP3, 32); // subfic FC_OP3, FC_OP3, 32 (FC_OP3 = 32 - FC_OP3)
			op[2] = EXT(FC_OP2, FC_OP2, FC_OP3, 536, 0); // srw FC_OP2, FC_OP2, FC_OP3
			op[3] = EXT(FC_RETOP, FC_RETOP, FC_OP2, 444, 0); // or FC_RETOP, FC_RETOP, FC_OP2
			return;
		case t_DSHRw: // technically not correct for FC_OP3 > 16
			*op++ = RLW(20, FC_OP2, FC_RETOP, 16, 0, 15, 0); // rlwimi FC_RETOP, FC_OP2, 16, 0, 5
			*op++ = EXT(FC_RETOP, FC_RETOP, FC_OP3, 536, 0); // srw FC_RETOP, FC_RETOP, FC_OP3
			break;
		case t_DSHRd:
			op[0] = EXT(FC_OP1, FC_RETOP, FC_OP3, 536, 0); // srw FC_RETOP, FC_OP1, FC_OP3
			op[1] = IMM(8, FC_OP3, FC_OP3, 32); // subfic FC_OP3, FC_OP3, 32 (FC_OP32 = 32 - FC_OP3)
			op[2] = EXT(FC_OP2, FC_OP2, FC_OP3, 24, 0); // slw FC_OP2, FC_OP2, FC_OP3
			op[3] = EXT(FC_RETOP, FC_RETOP, FC_OP2, 444, 0); // or FC_RETOP, FC_RETOP, FC_OP2
			return;
#endif
		default:
			do_gen_call(fct_ptr, op, true);
			return;
	}

	*op = 0x48000000 + 4*(end-op); // b end
}


// mov 16bit value from Segs[index] into dest_reg using FC_SEGS_ADDR (index modulo 2 must be zero)
// 16bit moves may destroy the upper 16bit of the destination register
static void gen_mov_seg16_to_reg(HostReg dest_reg,Bitu index) {
	gen_mov_word_to_reg(dest_reg, (Bit8u*)&Segs + index, false);
}

// mov 32bit value from Segs[index] into dest_reg using FC_SEGS_ADDR (index modulo 4 must be zero)
static void gen_mov_seg32_to_reg(HostReg dest_reg,Bitu index) {
	gen_mov_word_to_reg(dest_reg, (Bit8u*)&Segs + index, true);
}

// add a 32bit value from Segs[index] to a full register using FC_SEGS_ADDR (index modulo 4 must be zero)
static void gen_add_seg32_to_reg(HostReg reg,Bitu index) {
	gen_add(reg, (Bit8u*)&Segs + index);
}

// mov 16bit value from cpu_regs[index] into dest_reg using FC_REGS_ADDR (index modulo 2 must be zero)
// 16bit moves may destroy the upper 16bit of the destination register
static void gen_mov_regval16_to_reg(HostReg dest_reg,Bitu index)
{
	gen_mov_word_to_reg(dest_reg, (Bit8u*)&cpu_regs + index, false);
}

// mov 32bit value from cpu_regs[index] into dest_reg using FC_REGS_ADDR (index modulo 4 must be zero)
static void gen_mov_regval32_to_reg(HostReg dest_reg,Bitu index)
{
	gen_mov_word_to_reg(dest_reg, (Bit8u*)&cpu_regs + index, true);
}

// move an 8bit value from cpu_regs[index]  into dest_reg using FC_REGS_ADDR
// the upper 24bit of the destination register can be destroyed
// this function does not use FC_OP1/FC_OP2 as dest_reg as these
// registers might not be directly byte-accessible on some architectures
static void gen_mov_regbyte_to_reg_low(HostReg dest_reg,Bitu index)
{
	gen_mov_byte_to_reg_low(dest_reg, (Bit8u*)&cpu_regs + index);
}

// move an 8bit value from cpu_regs[index]  into dest_reg using FC_REGS_ADDR
// the upper 24bit of the destination register can be destroyed
// this function can use FC_OP1/FC_OP2 as dest_reg which are
// not directly byte-accessible on some architectures
static void INLINE gen_mov_regbyte_to_reg_low_canuseword(HostReg dest_reg,Bitu index) {
	gen_mov_byte_to_reg_low_canuseword(dest_reg, (Bit8u*)&cpu_regs + index);
}

// move 16bit of register into cpu_regs[index] using FC_REGS_ADDR (index modulo 2 must be zero)
static void gen_mov_regval16_from_reg(HostReg src_reg,Bitu index)
{
	gen_mov_word_from_reg(src_reg, (Bit8u*)&cpu_regs + index, false);
}

// move 32bit of register into cpu_regs[index] using FC_REGS_ADDR (index modulo 4 must be zero)
static void gen_mov_regval32_from_reg(HostReg src_reg,Bitu index)
{
	gen_mov_word_from_reg(src_reg, (Bit8u*)&cpu_regs + index, true);
}

// move the lowest 8bit of a register into cpu_regs[index] using FC_REGS_ADDR
static void gen_mov_regbyte_from_reg_low(HostReg src_reg,Bitu index)
{
	gen_mov_byte_from_reg_low(src_reg, (Bit8u*)&cpu_regs + index);
}

// add a 32bit value from cpu_regs[index] to a full register using FC_REGS_ADDR (index modulo 4 must be zero)
static void gen_add_regval32_to_reg(HostReg reg,Bitu index)
{
	gen_add(reg, (Bit8u*)&cpu_regs + index);
}

// move 32bit (dword==true) or 16bit (dword==false) of a register into cpu_regs[index] using FC_REGS_ADDR (if dword==true index modulo 4 must be zero) (if dword==false index modulo 2 must be zero)
static void gen_mov_regword_from_reg(HostReg src_reg,Bitu index,bool dword) {
	if (dword)
		gen_mov_regval32_from_reg(src_reg, index);
	else
		gen_mov_regval16_from_reg(src_reg, index);
}

// move a 32bit (dword==true) or 16bit (dword==false) value from cpu_regs[index] into dest_reg using FC_REGS_ADDR (if dword==true index modulo 4 must be zero) (if dword==false index modulo 2 must be zero)
// 16bit moves may destroy the upper 16bit of the destination register
static void gen_mov_regword_to_reg(HostReg dest_reg,Bitu index,bool dword) {
	if (dword)
		gen_mov_regval32_to_reg(dest_reg, index);
	else
		gen_mov_regval16_to_reg(dest_reg, index);
}
