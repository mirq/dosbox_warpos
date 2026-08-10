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

#if (C_DYNREC) 

#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stddef.h>
#include <stdlib.h>

#if defined (WIN32)
#include <windows.h>
#include <winbase.h>
#endif

#if (C_HAVE_MPROTECT)
#include <sys/mman.h>

#include <limits.h>
#ifndef PAGESIZE
#define PAGESIZE 4096
#endif
#endif /* C_HAVE_MPROTECT */

#include "callback.h"
#include "regs.h"
#include "mem.h"
#include "cpu.h"
#include "debug.h"
#include "paging.h"
#include "inout.h"
#include "lazyflags.h"
#include "pic.h"

#define CACHE_MAXSIZE	(65536*2)
#define CACHE_TOTAL		(1024*1024*8)
#define CACHE_PAGES		(512)
#define CACHE_BLOCKS	(128*1024)
/* MorphOS/PPC cache layout tuning.
 *
 * The G5 has a 64 KiB direct-mapped L1 instruction cache with 128-byte
 * cache blocks.  Starting generated blocks at only 16-byte boundaries makes
 * several independently generated blocks share an I-cache block and leaves
 * the code-cache colour dependent on the address returned by AllocVecAligned.
 * That produced large run-to-run performance differences on real G5 systems.
 *
 * G4 remains a separately buildable target with CPPFLAGS=-DG4. */
#if defined(__MORPHOS__)
# if defined(G4)
#  define CACHE_ALIGN		(32)
# else
#  define CACHE_ALIGN		(128)
# endif
#else
# define CACHE_ALIGN		(16)
#endif
#define DYN_HASH_SHIFT	(4)
#define DYN_PAGE_HASH	(4096>>DYN_HASH_SHIFT)
#define DYN_LINKS		(16)


//#define DYN_LOG 1 //Turn Logging on.


#if C_FPU
#define CPU_FPU 1                                               //Enable FPU escape instructions
#endif


// the emulated x86 registers
#define DRC_REG_EAX 0
#define DRC_REG_ECX 1
#define DRC_REG_EDX 2
#define DRC_REG_EBX 3
#define DRC_REG_ESP 4
#define DRC_REG_EBP 5
#define DRC_REG_ESI 6
#define DRC_REG_EDI 7

// the emulated x86 segment registers
#define DRC_SEG_ES 0
#define DRC_SEG_CS 1
#define DRC_SEG_SS 2
#define DRC_SEG_DS 3
#define DRC_SEG_FS 4
#define DRC_SEG_GS 5


// access to a general register
#define DRCD_REG_VAL(reg) (&cpu_regs.regs[reg].dword)
// access to a segment register
#define DRCD_SEG_VAL(seg) (&Segs.val[seg])
// access to the physical value of a segment register/selector
#define DRCD_SEG_PHYS(seg) (&Segs.phys[seg])

// access to an 8bit general register
#define DRCD_REG_BYTE(reg,idx) (&cpu_regs.regs[reg].byte[idx?BH_INDEX:BL_INDEX])
// access to  16/32bit general registers
#define DRCD_REG_WORD(reg,dwrd) ((dwrd)?((void*)(&cpu_regs.regs[reg].dword[DW_INDEX])):((void*)(&cpu_regs.regs[reg].word[W_INDEX])))


enum BlockReturn {
	BR_Normal=0,
	BR_Cycles,
	BR_Link1,BR_Link2,
	BR_Opcode,
#if (C_DEBUG)
	BR_OpcodeFull,
#endif
	BR_Iret,
	BR_CallBack,
	BR_SMCBlock
};

// identificator to signal self-modification of the currently executed block
#define SMC_CURRENT_BLOCK	0xffff


static void IllegalOptionDynrec(const char* msg) {
	E_Exit("DynrecCore: illegal option in %s",msg);
}

static struct {
	BlockReturn (*runcode)(Bit8u*);		// points to code that can start a block
	Bitu callback;				// the occurred callback
	Bitu readdata;				// spare space used when reading from memory
	Bit32u protected_regs[8];	// space to save/restore register values
} core_dynrec;

/*
 * Optional dynrec instrumentation.  The 64-bit fields are updated by the C++
 * dispatcher/translator.  The 32-bit FPU fields are updated by generated PPC
 * code only when -fpustats was enabled before the code cache was built.
 */
static struct DynrecStats {
	bool jit_enabled;
	bool fpu_enabled;
	bool mem_enabled;
	bool mem_fast_enabled;
	bool mem_write_fast_enabled;
	bool link_stats_enabled;
	bool direct_links_enabled;
	Bit64u cache_hits;
	Bit64u cache_misses;
	Bit64u dispatch_entries;
	Bit64u blocks_created;
	Bit64u instructions_translated;
	Bit64u host_bytes_generated;
	Bit64u zf_jcc_direct;
	Bit64u zf_jcc_inline_producer;
	Bit64u link_attempts;
	Bit64u links_resolved;
	Bit64u normal_fallbacks;
	Bit64u opcode_fallbacks;
	Bit64u smc_fallbacks;
	Bit64u direct_link_sites;
	Bit64u direct_links_patched;
	Bit64u direct_links_invalidated;
	Bit64u direct_links_out_of_range;
	Bit64u cache_restarts;
	Bitu cache_code_address;
	Bitu cache_base_alignment;
	Bitu cache_block_alignment;
	Bit32u block_entries;
	Bit32u fpu_instructions;
	Bit32u fpu_fully_inlined;
	Bit32u fpu_pop_ops;
	Bit32u fpu_free_ops;
	Bit32u fpu_store_ops;
	Bit32u fpu_exchange_ops;
	Bit32u fpu_sign_ops;
	Bit32u fpu_top_ops;
	Bit32u fpu_nop_ops;
	Bit32u mem_read_fast_hits;
	Bit32u mem_read_slow_tlb;
	Bit32u mem_read_slow_unaligned;
	Bit32u mem_read_slow_page_cross;
	Bit32u mem_read_slow_other;
	Bit32u mem_write_fast_hits;
	Bit32u mem_write_slow_tlb;
	Bit32u mem_write_slow_unaligned;
	Bit32u mem_write_slow_page_cross;
	Bit32u mem_write_slow_other;
} dynrec_stats;

void CPU_Dynrec_SetStats(bool jit_stats, bool fpu_stats, bool mem_stats, bool mem_fast,
                         bool mem_write_fast, bool link_stats, bool direct_links)
{
	memset(&dynrec_stats, 0, sizeof(dynrec_stats));
	dynrec_stats.jit_enabled = jit_stats;
	dynrec_stats.fpu_enabled = fpu_stats;
	dynrec_stats.mem_enabled = mem_stats;
	dynrec_stats.mem_fast_enabled = mem_fast;
	dynrec_stats.mem_write_fast_enabled = mem_write_fast;
	dynrec_stats.link_stats_enabled = link_stats;
	dynrec_stats.direct_links_enabled = direct_links;
}

void CPU_Dynrec_DumpStats(void)
{
	if (dynrec_stats.jit_enabled) {
		const Bit64u lookups = dynrec_stats.cache_hits + dynrec_stats.cache_misses;
		const double hit_rate = lookups ?
		        (100.0 * (double)dynrec_stats.cache_hits / (double)lookups) : 0.0;
		printf("DYNREC JIT statistics:\n");
		printf("  Cache lookups: %llu, hits: %llu (%.2f%%), misses: %llu\n",
		       (unsigned long long)lookups,
		       (unsigned long long)dynrec_stats.cache_hits, hit_rate,
		       (unsigned long long)dynrec_stats.cache_misses);
		printf("  Dispatcher entries: %llu, generated block entries: %lu, "
		       "translated blocks: %llu\n",
		       (unsigned long long)dynrec_stats.dispatch_entries,
		       (unsigned long)dynrec_stats.block_entries,
		       (unsigned long long)dynrec_stats.blocks_created);
		printf("  Guest instructions translated: %llu, host code bytes: %llu\n",
		       (unsigned long long)dynrec_stats.instructions_translated,
		       (unsigned long long)dynrec_stats.host_bytes_generated);
		printf("  Direct CMP/TEST/logic + JZ/JNZ branches: %llu\n",
		       (unsigned long long)dynrec_stats.zf_jcc_direct);
		printf("  Inline CMP/TEST/logic producers (no helper): %llu\n",
		       (unsigned long long)dynrec_stats.zf_jcc_inline_producer);
		printf("  Links: %llu/%llu resolved, fallbacks normal/opcode/SMC: %llu/%llu/%llu\n",
		       (unsigned long long)dynrec_stats.links_resolved,
		       (unsigned long long)dynrec_stats.link_attempts,
		       (unsigned long long)dynrec_stats.normal_fallbacks,
		       (unsigned long long)dynrec_stats.opcode_fallbacks,
		       (unsigned long long)dynrec_stats.smc_fallbacks);
		printf("  Code cache: base=%p, base alignment=%lu, block alignment=%lu, restarts=%llu\n",
		       (void *)dynrec_stats.cache_code_address,
		       (unsigned long)dynrec_stats.cache_base_alignment,
		       (unsigned long)dynrec_stats.cache_block_alignment,
		       (unsigned long long)dynrec_stats.cache_restarts);
	}

	if (dynrec_stats.fpu_enabled) {
		const Bit32u helper_backed = dynrec_stats.fpu_instructions -
		                              dynrec_stats.fpu_fully_inlined;
		printf("DYNREC FPU statistics (32-bit runtime counters):\n");
		printf("  x87 instructions: %lu, fully inlined: %lu, helper-backed: %lu\n",
		       (unsigned long)dynrec_stats.fpu_instructions,
		       (unsigned long)dynrec_stats.fpu_fully_inlined,
		       (unsigned long)helper_backed);
		printf("  Inlined micro-ops pop/free/store/exchange/sign/top/nop: "
		       "%lu/%lu/%lu/%lu/%lu/%lu/%lu\n",
		       (unsigned long)dynrec_stats.fpu_pop_ops,
		       (unsigned long)dynrec_stats.fpu_free_ops,
		       (unsigned long)dynrec_stats.fpu_store_ops,
		       (unsigned long)dynrec_stats.fpu_exchange_ops,
		       (unsigned long)dynrec_stats.fpu_sign_ops,
		       (unsigned long)dynrec_stats.fpu_top_ops,
		       (unsigned long)dynrec_stats.fpu_nop_ops);
	}

	if (dynrec_stats.link_stats_enabled) {
#if defined(G4)
		const char *const tuning = "G4";
#else
		const char *const tuning = "G5";
#endif
		printf("DYNREC PPC direct-link statistics (%s tuning, direct links %s):\n",
		       tuning,dynrec_stats.direct_links_enabled ? "enabled" : "disabled");
		printf("  Emitted link sites: %llu, patched: %llu, invalidated: %llu\n",
		       (unsigned long long)dynrec_stats.direct_link_sites,
		       (unsigned long long)dynrec_stats.direct_links_patched,
		       (unsigned long long)dynrec_stats.direct_links_invalidated);
		printf("  Out-of-range/fallback patches: %llu\n",
		       (unsigned long long)dynrec_stats.direct_links_out_of_range);
	}

	if (dynrec_stats.mem_enabled) {
		const Bit64u read_slow = (Bit64u)dynrec_stats.mem_read_slow_tlb +
		                         dynrec_stats.mem_read_slow_unaligned +
		                         dynrec_stats.mem_read_slow_page_cross +
		                         dynrec_stats.mem_read_slow_other;
		const Bit64u read_total = (Bit64u)dynrec_stats.mem_read_fast_hits + read_slow;
		const double read_hit_rate = read_total ?
		        (100.0 * (double)dynrec_stats.mem_read_fast_hits / (double)read_total) : 0.0;
		const Bit64u write_slow = (Bit64u)dynrec_stats.mem_write_slow_tlb +
		                          dynrec_stats.mem_write_slow_unaligned +
		                          dynrec_stats.mem_write_slow_page_cross +
		                          dynrec_stats.mem_write_slow_other;
		const Bit64u write_total = (Bit64u)dynrec_stats.mem_write_fast_hits + write_slow;
		const double write_hit_rate = write_total ?
		        (100.0 * (double)dynrec_stats.mem_write_fast_hits / (double)write_total) : 0.0;
		printf("DYNREC PPC memory statistics (32-bit runtime counters, read/write fast paths %s/%s):\n",
		       dynrec_stats.mem_fast_enabled ? "enabled" : "disabled",
		       dynrec_stats.mem_write_fast_enabled ? "enabled" : "disabled");
		printf("  Read fast hits: %lu of %llu (%.2f%%)\n",
		       (unsigned long)dynrec_stats.mem_read_fast_hits,
		       (unsigned long long)read_total, read_hit_rate);
		printf("  Slow reads TLB/unaligned/page-cross/other: %lu/%lu/%lu/%lu\n",
		       (unsigned long)dynrec_stats.mem_read_slow_tlb,
		       (unsigned long)dynrec_stats.mem_read_slow_unaligned,
		       (unsigned long)dynrec_stats.mem_read_slow_page_cross,
		       (unsigned long)dynrec_stats.mem_read_slow_other);
		printf("  Write fast hits: %lu of %llu (%.2f%%)\n",
		       (unsigned long)dynrec_stats.mem_write_fast_hits,
		       (unsigned long long)write_total, write_hit_rate);
		printf("  Slow writes TLB/unaligned/page-cross/other: %lu/%lu/%lu/%lu\n",
		       (unsigned long)dynrec_stats.mem_write_slow_tlb,
		       (unsigned long)dynrec_stats.mem_write_slow_unaligned,
		       (unsigned long)dynrec_stats.mem_write_slow_page_cross,
		       (unsigned long)dynrec_stats.mem_write_slow_other);
	}
}


#include "core_dynrec/cache.h"

#define X86			0x01
#define X86_64		0x02
#define MIPSEL		0x03
#define ARMV4LE		0x04
#define ARMV7LE		0x05
#define POWERPC		0x06
#define ARMV8LE		0x07

#if C_TARGETCPU == X86_64
#include "core_dynrec/risc_x64.h"
#elif C_TARGETCPU == X86
#include "core_dynrec/risc_x86.h"
#elif C_TARGETCPU == MIPSEL
#include "core_dynrec/risc_mipsel32.h"
#elif (C_TARGETCPU == ARMV4LE) || (C_TARGETCPU == ARMV7LE)
#include "core_dynrec/risc_armv4le.h"
#elif C_TARGETCPU == POWERPC
#include "core_dynrec/risc_ppc.h"
#elif C_TARGETCPU == ARMV8LE
#include "core_dynrec/risc_armv8le.h"
#endif

#if !defined(WORDS_BIGENDIAN)
#define gen_add_LE gen_add
#define gen_mov_LE_word_to_reg gen_mov_word_to_reg
#endif

#if defined(__MORPHOS__)
/* Static process storage for the 8 MiB PPC JIT cache.  Placing the
 * arena immediately before decoder.h keeps generated code and dynrec
 * helpers in the same executable text hunk. */
Bit8u ppc_jit_storage[CACHE_TOTAL + CACHE_MAXSIZE + 65536]
        __attribute__((section(".text"), aligned(65536), used));
#endif

#include "core_dynrec/decoder.h"

CacheBlockDynRec * LinkBlocks(BlockReturn ret) {
	CacheBlockDynRec * block=NULL;
	if (dynrec_stats.jit_enabled) dynrec_stats.link_attempts++;
	// the last instruction was a control flow modifying instruction
	Bitu temp_ip=SegPhys(cs)+reg_eip;
	CodePageHandlerDynRec * temp_handler=(CodePageHandlerDynRec *)get_tlb_readhandler(temp_ip);
	if (temp_handler->flags & (cpu.code.big ? PFLAG_HASCODE32:PFLAG_HASCODE16)) {
		// see if the target is an already translated block
		block=temp_handler->FindCacheBlock(temp_ip & 4095);
		if (block) { // found it, link the current block to
			if (dynrec_stats.jit_enabled) dynrec_stats.links_resolved++;
			cache.block.running->LinkTo(ret==BR_Link2,block);
		}
	}
	return block;
}

/*
	The core tries to find the block that should be executed next.
	If such a block is found, it is run, otherwise the instruction
	stream starting at ip_point is translated (see decoder.h) and
	makes up a new code block that will be run.
	When control is returned to CPU_Core_Dynrec_Run (which might
	be right after the block is run, or somewhen long after that
	due to the direct cacheblock linking) the returncode decides
	the next action. This might be continuing the translation and
	execution process, or returning from the core etc.
*/

Bits CPU_Core_Dynrec_Run(void) {
	for (;;) {
		// Determine the linear address of CS:EIP
		PhysPt ip_point=SegPhys(cs)+reg_eip;
		#if C_HEAVY_DEBUG
			if (DEBUG_HeavyIsBreakpoint()) return debugCallback;
		#endif

		CodePageHandlerDynRec * chandler=0;
		// see if the current page is present and contains code
		if (GCC_UNLIKELY(MakeCodePage(ip_point,chandler))) {
			// page not present, throw the exception
			CPU_Exception(cpu.exception.which,cpu.exception.error);
			continue;
		}

		// page doesn't contain code or is special
		if (GCC_UNLIKELY(!chandler)) {
			if (dynrec_stats.jit_enabled) dynrec_stats.normal_fallbacks++;
			return CPU_Core_Normal_Run();
		}

		// find correct Dynamic Block to run
		CacheBlockDynRec * block=chandler->FindCacheBlock(ip_point&4095);
		if (dynrec_stats.jit_enabled) {
			if (block) dynrec_stats.cache_hits++;
			else dynrec_stats.cache_misses++;
		}
		if (!block) {
			// no block found, thus translate the instruction stream
			// unless the instruction is known to be modified
			if (!chandler->invalidation_map || (chandler->invalidation_map[ip_point&4095]<4)) {
				// translate up to 32 instructions
				block=CreateCacheBlock(chandler,ip_point,32);
			} else {
				// let the normal core handle this instruction to avoid zero-sized blocks
				Bitu old_cycles=CPU_Cycles;
				CPU_Cycles=1;
				Bits nc_retcode=CPU_Core_Normal_Run();
				if (!nc_retcode) {
					CPU_Cycles=old_cycles-1;
					continue;
				}
				CPU_CycleLeft+=old_cycles;
				return nc_retcode;
			}
		}

run_block:
		cache.block.running=0;
		if (dynrec_stats.jit_enabled) dynrec_stats.dispatch_entries++;
		// now we're ready to run the dynamic code block
		//BlockReturn ret=((BlockReturn (*)(void))(block->cache.start))();
     	BlockReturn ret=core_dynrec.runcode(block->cache.start);

		switch (ret) {
		case BR_Iret:
#if C_DEBUG
#if C_HEAVY_DEBUG
			if (DEBUG_HeavyIsBreakpoint()) return debugCallback;
#endif
#endif
			if (!GETFLAG(TF)) {
				if (GETFLAG(IF) && PIC_IRQCheck) return CBRET_NONE;
				break;
			}
			// trapflag is set, switch to the trap-aware decoder
			cpudecoder=CPU_Core_Dynrec_Trap_Run;
			return CBRET_NONE;

		case BR_Normal:
			if (dynrec_stats.jit_enabled) dynrec_stats.normal_fallbacks++;
			// the block was exited due to a non-predictable control flow
			// modifying instruction (like ret) or some nontrivial cpu state
			// changing instruction (for example switch to/from pmode),
			// or the maximum number of instructions to translate was reached
#if C_DEBUG
#if C_HEAVY_DEBUG
			if (DEBUG_HeavyIsBreakpoint()) return debugCallback;
#endif
#endif
			break;

		case BR_Cycles:
			// cycles went negative, return from the core to handle
			// external events, schedule the pic...
#if C_DEBUG
#if C_HEAVY_DEBUG
			if (DEBUG_HeavyIsBreakpoint()) return debugCallback;
#endif
#endif
			return CBRET_NONE;

		case BR_CallBack:
			// the callback code is executed in dosbox.conf, return the callback number
			FillFlags();
			return core_dynrec.callback;

		case BR_SMCBlock:
//			LOG_MSG("selfmodification of running block at %x:%x",SegValue(cs),reg_eip);
			if (dynrec_stats.jit_enabled) dynrec_stats.smc_fallbacks++;
			cpu.exception.which=0;
			// fallthrough, let the normal core handle the block-modifying instruction
		case BR_Opcode:
			if (dynrec_stats.jit_enabled) dynrec_stats.opcode_fallbacks++;
			// some instruction has been encountered that could not be translated
			// (thus it is not part of the code block), the normal core will
			// handle this instruction
			CPU_CycleLeft+=CPU_Cycles;
			CPU_Cycles=1;
			return CPU_Core_Normal_Run();

#if (C_DEBUG)
		case BR_OpcodeFull:
			CPU_CycleLeft+=CPU_Cycles;
			CPU_Cycles=1;
			return CPU_Core_Full_Run();
#endif

		case BR_Link1:
		case BR_Link2:
			block=LinkBlocks(ret);
			if (block) goto run_block;
			break;

		default:
			E_Exit("Invalid return code %d", ret);
		}
	}
	return CBRET_NONE;
}

Bits CPU_Core_Dynrec_Trap_Run(void) {
	Bits oldCycles = CPU_Cycles;
	CPU_Cycles = 1;
	cpu.trap_skip = false;

	// let the normal core execute the next (only one!) instruction
	Bits ret=CPU_Core_Normal_Run();

	// trap to int1 unless the last instruction deferred this
	// (allows hardware interrupts to be served without interaction)
	if (!cpu.trap_skip) CPU_HW_Interrupt(1);

	CPU_Cycles = oldCycles-1;
	// continue (either the trapflag was clear anyways, or the int1 cleared it)
	cpudecoder = &CPU_Core_Dynrec_Run;

	return ret;
}

void CPU_Core_Dynrec_Init(void) {
}

void CPU_Core_Dynrec_Cache_Init(bool enable_cache) {
	// Initialize code cache and dynamic blocks
	cache_init(enable_cache);
}

void CPU_Core_Dynrec_Cache_Close(void) {
	cache_close();
}

#endif
