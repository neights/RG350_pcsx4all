/*
 * Mips-to-mips recompiler for pcsx4all
 *
 * Copyright (c) 2009 Ulrich Hecht
 * Copyright (c) 2016 modified by Dmitry Smagin
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without restriction,
 * including without limitation the rights to use, copy, modify, merge,
 * publish, distribute, sublicense, and/or sell copies of the Software,
 * and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE
 * OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 */

#include "psxcommon.h"
#include "psxhle.h"
#include "psxmem.h"
#include "psxhw.h"
#include "r3000a.h"
#include "gte.h"

/* Use inlined-asm version of block dispatcher: */
#define ASM_EXECUTE_LOOP

//#define WITH_DISASM
//#define DEBUGG printf

#include "mips_codegen.h"
#include "disasm.h"

typedef struct {
	u32 s;
	u32 r;
} iRegisters;

#define IsConst(reg) (iRegs[reg].s)
#define SetUndef(reg) do { iRegs[reg].s = 0; } while (0)
#define SetConst(reg, val) do { iRegs[reg].s = 1; iRegs[reg].r = (val); } while (0)

static iRegisters iRegs[32]; /* used for imm caching and back up of regs in dynarec */

static u32 psxRecLUT[0x010000];

#undef PC_REC
#undef PC_REC8
#undef PC_REC16
#undef PC_REC32
#define PC_REC(x)	(psxRecLUT[(x) >> 16] + ((x) & 0xffff))
#define PC_REC8(x)	(*(u8 *)PC_REC(x))
#define PC_REC16(x)	(*(u16*)PC_REC(x))
#define PC_REC32(x)	(*(u32*)PC_REC(x))

#define RECMEM_SIZE		(12 * 1024 * 1024)
#define RECMEM_SIZE_MAX 	(RECMEM_SIZE-(512*1024))
#define REC_MAX_OPCODES		80

static u8 recMemBase[RECMEM_SIZE + (REC_MAX_OPCODES*2) + 0x4000] __attribute__ ((__aligned__ (32)));
static u32 *recMem;				/* the recompiled blocks will be here */
static s8 recRAM[0x200000];			/* and the ptr to the blocks here */
static s8 recROM[0x080000];			/* and here */
static u32 pc;					/* recompiler pc */
static u32 oldpc;
static u32 branch = 0;

#ifdef WITH_DISASM
char	disasm_buffer[512];
#endif

#include "regcache.h"

static void recReset();
static void recRecompile();
static void recClear(u32 Addr, u32 Size);

extern void (*recBSC[64])();
extern void (*recSPC[64])();
extern void (*recREG[32])();
extern void (*recCP0[32])();
extern void (*recCP2[64])();
extern void (*recCP2BSC[32])();

u32	*recMemStart;
u32	end_block = 0;
u32	cycles_pending = 0;

#ifdef WITH_DISASM

#define make_stub_label(name) \
 { (void *)name, (char*)#name }

disasm_label stub_labels[] =
{
  make_stub_label(gteMFC2),
  make_stub_label(gteMTC2),
  make_stub_label(gteLWC2),
  make_stub_label(gteSWC2),
  make_stub_label(gteRTPS),
  make_stub_label(gteOP),
  make_stub_label(gteNCLIP),
  make_stub_label(gteDPCS),
  make_stub_label(gteINTPL),
  make_stub_label(gteMVMVA),
  make_stub_label(gteNCDS),
  make_stub_label(gteNCDT),
  make_stub_label(gteCDP),
  make_stub_label(gteNCCS),
  make_stub_label(gteCC),
  make_stub_label(gteNCS),
  make_stub_label(gteNCT),
  make_stub_label(gteSQR),
  make_stub_label(gteDCPL),
  make_stub_label(gteDPCT),
  make_stub_label(gteAVSZ3),
  make_stub_label(gteAVSZ4),
  make_stub_label(gteRTPT),
  make_stub_label(gteGPF),
  make_stub_label(gteGPL),
  make_stub_label(gteNCCT),
  make_stub_label(psxMemRead8),
  make_stub_label(psxMemRead16),
  make_stub_label(psxMemRead32),
  make_stub_label(psxMemWrite8),
  make_stub_label(psxMemWrite16),
  make_stub_label(psxMemWrite32),
  make_stub_label(psxMemWrite32_error),
  make_stub_label(psxHwRead8),
  make_stub_label(psxHwRead16),
  make_stub_label(psxHwRead32),
  make_stub_label(psxHwWrite8),
  make_stub_label(psxHwWrite16),
  make_stub_label(psxHwWrite32),
  make_stub_label(psxException),
  make_stub_label(psxBranchTest_rec)
};

const u32 num_stub_labels = sizeof(stub_labels) / sizeof(disasm_label);

#define DISASM_INIT() \
do { \
	printf("Block PC %x (MIPS) -> %p\n", pc, recMemStart); \
} while (0)

#define DISASM_PSX(_PC_) \
do { \
	u32 opcode = *(u32 *)((char *)PSXM(_PC_)); \
	disasm_mips_instruction(opcode, disasm_buffer, _PC_, 0, 0); \
	printf("%08x: %08x %s\n", _PC_, opcode, disasm_buffer); \
} while (0)

#define DISASM_HOST() \
do { \
	printf("\n"); \
	u8 *tr_ptr = (u8*)recMemStart; \
	for (; (u32)tr_ptr < (u32)recMem; tr_ptr += 4) { \
		u32 opcode = *(u32*)tr_ptr; \
		disasm_mips_instruction(opcode, disasm_buffer, \
					(u32)tr_ptr, stub_labels, \
					num_stub_labels); \
		printf("%08x: %s\t(0x%08x)\n", \
			(u32)tr_ptr, disasm_buffer, opcode); \
	} \
	printf("\n"); \
} while (0)

#else

#define DISASM_PSX(_PC_)
#define DISASM_HOST()
#define DISASM_INIT()

#endif

#include "opcodes.h"
#include <sys/cachectl.h>

void clear_insn_cache(void *start, void *end, int flags)
{
	cacheflush(start, (char *)end - (char *)start, ICACHE);
}

static void recRecompile()
{
	psxRegs.reserved = (void *)recRAM;

	if ((u32)recMem - (u32)recMemBase >= RECMEM_SIZE_MAX )
		recReset();

	recMem = (u32*)(((u32)recMem + 64) & ~(63));
	recMemStart = recMem;

	regReset();

	cycles_pending = 0;

	PC_REC32(psxRegs.pc) = (u32)recMem;
	oldpc = pc = psxRegs.pc;

	DISASM_INIT();

	rec_recompile_start();
	memset(iRegs, 0, sizeof(iRegs));

	do {
		psxRegs.code = *(u32 *)((char *)PSXM(pc));
		DISASM_PSX(pc);
		pc += 4;
		recBSC[psxRegs.code>>26]();
		regUpdate();
		branch = 0;
	} while (!end_block);

	end_block = 0;
	rec_recompile_end();
	DISASM_HOST();
	clear_insn_cache(recMemStart, recMem, 0);
}

static int recInit()
{
	int i;

	recMem = (u32*)recMemBase;
	memset(recMem, 0, RECMEM_SIZE + (REC_MAX_OPCODES*2) + 0x4000);

	recReset();

	if (recRAM == NULL || recROM == NULL || recMemBase == NULL || psxRecLUT == NULL) {
		printf("Error allocating memory\n"); return -1;
	}

	for (i = 0; i < 0x80; i++)
		psxRecLUT[i + 0x0000] = (u32)&recRAM[(i & 0x1f) << 16];

	memcpy(psxRecLUT + 0x8000, psxRecLUT, 0x80 * 4);
	memcpy(psxRecLUT + 0xa000, psxRecLUT, 0x80 * 4);

	for (i = 0; i < 0x08; i++)
		psxRecLUT[i + 0xbfc0] = (u32)&recROM[i << 16];

	return 0;
}

static void recShutdown() { }

/* It seems there's no way to tell GCC that something is being called inside
 * asm() blocks and GCC doesn't bother to save temporaries to stack.
 * That's why we have two options:
 * 1. Call recompiled blocks via recFunc() trap which is strictly noinline and
 * saves registers $s[0-7], $fp and $ra on each call, or
 * 2. Code recExecute() and recExecuteBlock() entirely in assembler taking into
 * account that no registers except $ra are saved in recompiled blocks and
 * thus put all temporaries to stack. In this case $s[0-7], $fp and $ra are saved
 * in recExecute() and recExecuteBlock() only once.
 */
#ifndef ASM_EXECUTE_LOOP
static __attribute__ ((noinline)) void recFunc(void *fn)
{
	/* This magic code calls fn address saving registers $s[0-7], $fp and $ra. */
	__asm__ __volatile__ (
		"jalr   %0 \n"
		:
		: "r" (fn)
		: "%s0", "%s1", "%s2", "%s3", "%s4", "%s5", "%s6", "%s7", "%fp", "%ra");
}
#endif

static void recExecute()
{
#ifndef ASM_EXECUTE_LOOP
	for (;;) {
		u32 *p = (u32*)PC_REC(psxRegs.pc);
		if (*p == 0)
			recRecompile();

		recFunc((void *)*p);
	}
#else
__asm__ __volatile__ (
// NOTE: <BD> indicates an instruction in a branch-delay slot
".set noreorder                               \n"

// $fp/$s8 remains set to &psxRegs across all calls to dynarec blocks
"lui    $fp,      %%hi(%[psxRegs])            \n"
"addiu  $fp, $fp, %%lo(%[psxRegs])            \n"

// Set up our own stack frame. Should have 8-byte alignment, and have 16 bytes
// empty at 0($sp) for use by functions called from within recompiled code.
".equ  frame_size,                  24        \n"
".equ  f_off_temp_var1,             20        \n"
".equ  f_off_block_ret_addr,        16        \n" // NOTE: blocks assume this is at 16($sp)!
"addiu $sp, $sp, -frame_size                  \n"

// Store const block return address at fixed location in stack frame
"lui   $t0,      %%hi(return_from_block%=)    \n"
"addiu $t0, $t0, %%lo(return_from_block%=)    \n"
"sw    $t0, f_off_block_ret_addr($sp)         \n"

// Load $v0 once with psxRegs.pc, it will be set to new
//  value at end of each loop
"lw    $v0, %[psxRegs_pc_off]($fp)            \n" // $v0 = psxRegs.pc

// Align loop on cache-line boundary
".balign 32, 0, 31                            \n"

// Infinite loop:
"loop%=:                                      \n"

// NOTE: End of loop will have set $v0 to psxRegs.pc
// $t2 = psxRecLUT[pxsRegs.pc >> 16] + (psxRegs.pc & 0xffff)
// $t0 = *($t2)
"lui   $t1, %%hi(%[psxRecLUT])                \n"
"srl   $t2, $v0, 16                           \n"
"sll   $t2, $t2, 2                            \n"
"addu  $t1, $t1, $t2                          \n"
"lw    $t1, %%lo(%[psxRecLUT])($t1)           \n"
"andi  $v0, $v0, 0xffff                       \n"
"addu  $t2, $v0, $t1                          \n"
"lw    $t0, 0($t2)                            \n"

// Recompile block, if necessary
"beqz  $t0, recompile_block%=                 \n"
"nop                                          \n" // <BD>

// Execute already-compiled block
"execute_block%=:                             \n"
"jr    $t0                                    \n"
"nop                                          \n" // <BD>

// Return point for all executed blocks
"return_from_block%=:                         \n"
"b     loop%=                                 \n" // Loop, loading PC in BD slot
"lw    $v0, %[psxRegs_pc_off]($fp)            \n" // <BD>  $v0 = psxRegs.pc

// Recompile block and execute it. It will return to label 'return_from_block'
"recompile_block%=:                           \n"
"jal   %[recRecompile]                        \n"
"sw    $t2, f_off_temp_var1($sp)              \n" // <BD> Save block ptr
"lw    $t2, f_off_temp_var1($sp)              \n" // Restore block ptr
"lw    $t0, 0($t2)                            \n"
"jr    $t0                                    \n" // Execute block
"nop                                          \n" // <BD>

// Destroy stack frame (we'd never actually reach here)
"addiu $sp, $sp, frame_size                   \n"
".set reorder                                 \n"

: // Output
: // Input
     [psxRegs]                    "i" (&psxRegs),
     [psxRegs_pc_off]             "i" (off(pc)),
     [psxRecLUT]                  "i" (&psxRecLUT),
     [recRecompile]               "i" (&recRecompile)
: // Clobber - Don't care, the ASM loop above is infinite
);
#endif
}

static void recExecuteBlock(unsigned target_pc)
{
#ifndef ASM_EXECUTE_LOOP
	do {
		u32 *p = (u32*)PC_REC(psxRegs.pc);
		if (*p == 0)
			recRecompile();

		recFunc((void *)*p);
	} while (psxRegs.pc != target_pc);
#else
__asm__ __volatile__ (
// NOTE: <BD> indicates an instruction in a branch-delay slot
".set noreorder                               \n"

// $fp/$s8 remains set to &psxRegs across all calls to dynarec blocks
"lui   $fp,      %%hi(%[psxRegs])             \n"
"addiu $fp, $fp, %%lo(%[psxRegs])             \n"

// Set up our own stack frame. Should have 8-byte alignment, and have 16 bytes
// empty at 0($sp) for use by functions called from within recompiled code.
".equ  frame_size,                  32        \n"
".equ  f_off_orig_gp_regval,        28        \n"
".equ  f_off_target_pc,             24        \n"
".equ  f_off_temp_var1,             20        \n"
".equ  f_off_block_ret_addr,        16        \n" // NOTE: blocks assume this is at 16($sp)!
"addiu $sp, $sp, -frame_size                  \n"

// Save original $gp val. GCC requires inline ASM to do this manually.
"sw    $gp, f_off_orig_gp_regval($sp)         \n"

// Store const copy of 'target_pc' parameter in stack frame
"sw    %[target_pc], f_off_target_pc($sp)     \n"

// Store const block return address at fixed location in stack frame
"lui   $t0,      %%hi(return_from_block%=)    \n"
"addiu $t0, $t0, %%lo(return_from_block%=)    \n"
"sw    $t0, f_off_block_ret_addr($sp)         \n"

// Load $v0 once with psxRegs.pc, it will be set to new
//  value at end of each loop
"lw    $v0, %[psxRegs_pc_off]($fp)            \n"

// Load $t1 once with upper base addr of psxRecLUT[],
//  it will be loaded again at end of each loop
"lui   $t1, %%hi(%[psxRecLUT])                \n"

// Align loop on cache-line boundary
".balign 32, 0, 31                            \n"

// Loop until psxRegs.pc == target_pc
"loop%=:                                      \n"
// NOTE: End of loop will have set $t1 to %%hi(%[psxRecLUT])
//       and $v0 to psxRegs.pc
// $t2 = psxRecLUT[pxsRegs.pc >> 16] + (psxRegs.pc & 0xffff)
// $t0 = *($t2)
"srl   $t2, $v0, 16                           \n"
"sll   $t2, $t2, 2                            \n"
"addu  $t1, $t1, $t2                          \n"
"lw    $t1, %%lo(%[psxRecLUT])($t1)           \n"
"andi  $v0, $v0, 0xffff                       \n"
"addu  $t2, $v0, $t1                          \n"
"lw    $t0, 0($t2)                            \n"

// Recompile block, if necessary
"beqz  $t0, recompile_block%=                 \n"
"nop                                          \n" // <BD>

// Execute already-compiled block
"execute_block%=:                             \n"
"jr    $t0                                    \n"
"nop                                          \n" // <BD>

// Return point for all executed blocks
"return_from_block%=:                         \n"
// Check if target_pc has been reached, looping if not
"lw    $v0, %[psxRegs_pc_off]($fp)            \n" // $v0 = psxRegs.pc
"lw    $t0, f_off_target_pc($sp)              \n" // $t0 = target_pc
"bne   $v0, $t0, loop%=                       \n" // loop if psxRegs.pc != target_pc
"lui   $t1, %%hi(%[psxRecLUT])                \n" // <BD> Top of loop needs this

// Since target_pc has been reached, goto 'end_label'
"b     end_label%=                            \n"
"lw    $gp, f_off_orig_gp_regval($sp)         \n" // <BD> Restore $gp reg in BD slot

// Recompile block and execute it. It will return to label 'return_from_block'
"recompile_block%=:                           \n"
"jal   %[recRecompile]                        \n"
"sw    $t2, f_off_temp_var1($sp)              \n" // <BD> Save block code ptr on stack
"lw    $t2, f_off_temp_var1($sp)              \n" // Restore block code ptr from stack
"lw    $t0, 0($t2)                            \n"
"jr    $t0                                    \n" // Execute block
"nop                                          \n" // <BD>

// Destroy stack frame, exiting inlined ASM block
"end_label%=:                                 \n"
"addiu $sp, $sp, frame_size                   \n"
".set reorder                                 \n"

: // Output
: // Input
  [target_pc]                  "r" (target_pc),
  [psxRegs]                    "i" (&psxRegs),
  [psxRegs_pc_off]             "i" (off(pc)),
  [psxRecLUT]                  "i" (&psxRecLUT),
  [recRecompile]               "i" (&recRecompile)
: // Clobber
  "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7", "t8", "t9",
  "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7",
  "v0", "v1", "at", "fp", "ra",
  "memory"
);
#endif
}

static void recClear(u32 Addr, u32 Size)
{
	memset((u32*)PC_REC(Addr), 0, (Size * 4));

	if (Addr == 0x8003d000) {
		// temp fix for Buster Bros Collection and etc.
		memset(recRAM+0x4d88, 0, 0x8);
	}
}

static void recReset()
{
	memset(recRAM, 0, 0x200000);
	memset(recROM, 0, 0x080000);

	recMem = (u32*)recMemBase;

	regReset();

	branch = 0;	
	end_block = 0;
}

R3000Acpu psxRec =
{
	recInit,
	recReset,
	recExecute,
	recExecuteBlock,
	recClear,
	recShutdown
};
