// Translation of GBA code blocks into PowerPC (design: docs/jit.md).
// Thumb: native code with GBA registers in PPC registers, flags in CR fields computed only when
// live, cycles counted per segment. ARM, and Thumb instructions without a native translation,
// call mGBA's own handler with the interpreter's exact view of PC, prefetch and cycles.
#include <stdlib.h>
#include <string.h>

#include <mgba/gba/interface.h>
#include <mgba/internal/arm/isa-arm.h>
#include <mgba/internal/arm/isa-thumb.h>
#include <mgba/internal/gba/gba.h>
#include <mgba/internal/gba/memory.h>

#include "emu.h"
#include "jit_internal.h"

#define MAX_INSTR 64

enum { F_V = 1, F_C = 2, F_Z = 4, F_N = 8, F_ALL = 15 };

enum kind {
	K_ALU,      // native, static cycles
	K_CALL,     // native around a C call: dynamic cycles, may change nextEvent
	K_DYN,      // native, dynamic cycles (multiply)
	K_FALLBACK, // mGBA's handler
	K_BRANCH,   // native, ends the block
};

struct insn {
	uint32_t a, op;
	uint8_t kind, def, use, live_out;
	uint8_t cond;   // ARM condition (0xE = always; Thumb: always)
	bool check;     // a segment check (possible exit) before it
	int seg_cycles; // for a check: static cycles of the segment before its last instruction
	int cycles;     // static cycles (for K_CALL: the part besides the memory wait)
};

struct stub {
	uint32_t* at; // the bc to patch
	uint32_t pc;
	uint16_t count;
	uint8_t reason, mem, c_inv;
	int pending;
};

struct tctx {
	struct jit* j;
	struct ARMCore* cpu;
	struct GBA* gba;
	bool thumb, rom;
	int S, N;        // sequential / non-sequential 16-bit waitstates of the code region
	int pf;          // prefetch cycles of one instruction (1 + S)
	int pending;     // cycles not yet added to r30
	bool c_inv;      // the C flag is held inverted (!cr3.lt) after a subtraction
	bool mem;        // MEM state: GBA state in the ARMCore (ARM blocks)
	bool fast_branches; // static branches may skip setActiveRegion (no idle loop handling)
	bool idle;       // the block may be an idle loop (see find_idle_loop)
	bool lr_known;   // GBA LR holds lr_value (set by BL's first half)
	uint32_t lr_value;
	bool entries_on; // RAM block: record its entry points (see emit_check)
	int nentries;
	int nlsm;
	int n;
	struct insn ins[MAX_INSTR];
	int nstubs;
	struct stub stubs[2 * MAX_INSTR + 8];
	struct { uint32_t* at; uint8_t index; bool c_inv; } entries[MAX_INSTR];
	// LDM/STM done inline: their mGBA path, emitted after the block (emit_lsm_slow).
	struct { uint32_t *at, *back; uint8_t index, base, dir; bool load, wb; uint16_t mask; } lsm[MAX_INSTR];
};

// ---- Emission helpers ----

#define E(insn) jit_emit(t->j, insn)

static void flush_cycles(struct tctx* t) {
	if (t->pending) {
		E(P_ADDI(RCYC, RCYC, t->pending));
		t->pending = 0;
	}
}

// Canonical flags for code outside this block: C held as cr3.lt (not inverted).
static void canon_c(struct tctx* t) {
	if (t->c_inv) {
		E(P_CRNOT(CR_C, CR_C));
		t->c_inv = false;
	}
}

static void stub_bc(struct tctx* t, int bo, int bi, uint32_t pc, int reason, int count) {
	t->stubs[t->nstubs++] = (struct stub) { t->j->cur, pc, count, reason, t->mem, t->c_inv, t->pending };
	E(P_BC(bo, bi, 0));
}

// r3 = pc, r4 = reason | count << 8, then out through exit_regs or exit_mem.
static void emit_exit(struct jit* j, uint32_t pc, int reason, int count, bool mem) {
	jit_li32(j, 3, pc);
	jit_emit(j, P_LI(4, reason | count << 8));
	jit_b(j, mem ? j->exit_mem : j->exit_regs);
}

// A patchable exit to another block in the same region and mode (EXIT_NEXT). Fixed layout:
// 4 words, the first of which jit_link() rewrites into a direct branch to the target's code.
static void emit_chain_exit(struct tctx* t, uint32_t pc, int count, int reason) {
	flush_cycles(t);
	canon_c(t);
	if (t->j->check_chain) {
		// Self-check across chained blocks: count what runs before the exit or the jump.
		jit_la(t->j, 12, (uintptr_t) &t->j->chained);
		E(P_LWZ(11, 0, 12));
		E(P_ADDI(11, 11, count)); // not r0: addi reads rA = r0 as the constant 0
		E(P_STW(11, 0, 12));
	}
	E(P_LIS(3, pc >> 16));
	E(P_ORI(3, 3, pc & 0xFFFF));
	E(P_LI(4, reason | count << 8));
	jit_bl(t->j, t->j->exit_link);
}

// Exit for a branch whose target needs mGBA's WritePC (indirect, other region, BX). Like the
// interpreter, the dispatcher adds the branch's own cycles after WritePC (which may halt the CPU
// and set nextEvent = cycles): for EXIT_WRITEPC they go in r5, `cycles` if static, else (-1)
// the caller left them in r5 with r30 at the instruction's start. EXIT_BX: the dispatcher
// computes them.
static void emit_writepc_exit(struct tctx* t, int reg, int reason, int count, int cycles) {
	if (reason == EXIT_WRITEPC && cycles >= 0) {
		t->pending -= cycles;
	}
	flush_cycles(t);
	canon_c(t);
	if (reason == EXIT_WRITEPC && cycles >= 0) {
		E(P_LI(5, cycles));
	}
	if ((t->ins[0].a >> 24) == GBA_REGION_BIOS) {
		// Leaving the BIOS, setActiveRegion keeps prefetch[1] as the BIOS's open-bus value:
		// give it the interpreter's (the opcodes after the branch).
		const struct insn* in = &t->ins[count - 1];
		uint32_t w = t->thumb ? WORD_SIZE_THUMB : WORD_SIZE_ARM;
		jit_li32(t->j, 0, jit_fetch(t->cpu, in->a + w, t->thumb));
		E(P_STW(0, OFF_PREF, RCPU));
		jit_li32(t->j, 0, jit_fetch(t->cpu, in->a + 2 * w, t->thumb));
		E(P_STW(0, OFF_PREF + 4, RCPU));
	}
	if (reg != 3) {
		E(P_MR(3, reg));
	}
	E(P_LI(4, reason | count << 8));
	jit_b(t->j, t->j->exit_regs);
}

// For a branch through a memory call (POP {pc}, LDM with PC): remember r30 at the instruction's
// start, then after the call put the instruction's whole cycles in r5 and r30 back to the start.
static void emit_start_save(struct tctx* t) {
	flush_cycles(t);
	E(P_STW(RCYC, FRAME_SCRATCH, 1));
}

static void emit_dynamic_cycles(struct tctx* t, const struct insn* in) {
	E(P_LWZ(0, FRAME_SCRATCH, 1));
	E(P_SUBF(5, 0, RCYC));
	E(P_ADDI(5, 5, t->pending));
	E(P_MR(RCYC, 0));
	t->pending = 0;
	(void) in;
}

static void lastprefetch_reset(struct tctx* t) {
	// setActiveRegion zeroes it on every branch; it only matters for code in ROM (prefetch).
	if (t->rom) {
		E(P_LI(0, 0));
		jit_la(t->j, 12, (uintptr_t) &t->gba->memory.lastPrefetchedPc);
		E(P_STW(0, 0, 12));
	}
}

// ---- Thumb decoding: kind, cycles, flags ----

static uint8_t cond_use(int cond) {
	static const uint8_t use[16] = {
		F_Z, F_Z, F_C, F_C, F_N, F_N, F_V, F_V, F_C | F_Z, F_C | F_Z, F_N | F_V, F_N | F_V,
		F_N | F_Z | F_V, F_N | F_Z | F_V, 0, 0,
	};
	return use[cond];
}

static bool same_region_target(struct tctx* t, uint32_t target) {
	if ((target >> 24) != (t->ins[0].a >> 24)) {
		return false;
	}
	return !t->rom || (target & (GBA_SIZE_ROM0 - 1)) < t->gba->memory.romSize;
}

// Literal pool loads from ROM code read immutable data: fold them into constants.
static bool foldable_literal(struct tctx* t, uint32_t address) {
	return t->rom && (address & (GBA_SIZE_ROM0 - 1)) + 4 <= t->gba->memory.romSize &&
	       (address & 0x00FFFFFF) >= 0x100 && address < 0x09FE0000; // GPIO, AGB print
}

static void thumb_decode(struct tctx* t, struct insn* in) {
	uint32_t op = in->op;
	in->cond = 0xE;
	in->kind = K_ALU;
	in->def = in->use = 0;
	in->cycles = t->pf;
	switch (op >> 11) {
	case 0x00: // LSL imm
		in->def = (op >> 6) & 0x1F ? F_N | F_Z | F_C : F_N | F_Z;
		return;
	case 0x01: // LSR imm
	case 0x02: // ASR imm
		in->def = F_N | F_Z | F_C;
		return;
	case 0x03: // ADD/SUB reg/imm3
	case 0x05: // CMP imm8
	case 0x06: // ADD imm8
	case 0x07: // SUB imm8
		in->def = F_ALL;
		return;
	case 0x04: // MOV imm8
		in->def = F_N | F_Z;
		return;
	case 0x08:
		if (!(op & 0x0400)) { // data processing
			switch ((op >> 6) & 0xF) {
			case 0x0: case 0x1: case 0x8: case 0xC: case 0xE: case 0xF: // AND EOR TST ORR BIC MVN
				in->def = F_N | F_Z;
				return;
			case 0x9: case 0xA: case 0xB: // NEG CMP CMN
				in->def = F_ALL;
				return;
			case 0xD: // MUL: dynamic cycles
				in->kind = K_DYN;
				in->def = F_N | F_Z;
				in->cycles = t->pf + t->N - t->S;
				return;
			default: // shifts by register, ADC, SBC, MUL
				break;
			}
		} else if ((op & 0x0300) != 0x0300) { // ADD/CMP/MOV with high registers
			int rd = (op & 7) | ((op >> 4) & 8);
			if ((op & 0x0300) == 0x0100) {
				in->def = F_ALL;
				return;
			}
			if (rd != ARM_PC) {
				return;
			}
			in->kind = K_BRANCH;
			return;
		} else if (!(op & 0x0080)) { // BX
			in->kind = K_BRANCH;
			return;
		}
		break;
	case 0x09: // LDR PC-relative
		if (foldable_literal(t, ((in->a + 4) & ~3u) + ((op & 0xFF) << 2))) {
			in->cycles = t->pf + t->gba->memory.waitstatesNonseq32[in->a >> 24] + 2 + t->N - t->S;
			return;
		}
		in->kind = K_CALL;
		in->cycles = 1 + t->N;
		return;
	case 0x0A: case 0x0B: // load/store register offset
		if (((op >> 9) & 7) == 7) { // LDRSH: odd addresses sign-extend a byte
			break;
		}
		in->kind = K_CALL;
		in->cycles = 1 + t->N;
		return;
	case 0x0C: case 0x0D: case 0x0E: case 0x0F: case 0x10: case 0x11: // immediate offsets
	case 0x12: case 0x13: // SP-relative
		in->kind = K_CALL;
		in->cycles = 1 + t->N;
		return;
	case 0x14: case 0x15: // ADD rd, PC/SP, #imm
		return;
	case 0x16: case 0x17:
		if ((op & 0x0F00) == 0x0000) { // ADD/SUB SP, #imm
			return;
		}
		if ((op & 0x0600) == 0x0400) { // PUSH/POP
			in->kind = (op & 0x0900) == 0x0900 ? K_BRANCH : K_CALL; // POP {pc}
			in->cycles = 1 + t->N;
			return;
		}
		break;
	case 0x18: // STMIA
		in->kind = K_CALL;
		in->cycles = 1 + t->N;
		return;
	case 0x19: // LDMIA (an empty list loads PC: leave that quirk to mGBA)
		if (op & 0xFF) {
			in->kind = K_CALL;
			in->cycles = 1 + t->N;
			return;
		}
		break;
	case 0x1A: case 0x1B: // conditional branch, SWI, undefined
		if (((op >> 8) & 0xF) < 0xE) {
			in->kind = K_BRANCH;
			in->use = cond_use((op >> 8) & 0xF);
			return;
		}
		break;
	case 0x1C: // B
		in->kind = K_BRANCH;
		return;
	case 0x1E: // BL first half
		return;
	case 0x1F: // BL second half
		in->kind = K_BRANCH;
		return;
	default:
		break;
	}
	in->kind = K_FALLBACK;
	in->def = in->use = F_ALL;
}

// ---- Thumb emission ----

static int reg_or_pc(struct tctx* t, const struct insn* in, int r, int tmp) {
	if (r == ARM_PC) {
		jit_li32(t->j, tmp, in->a + 4);
		return tmp;
	}
	return RG(r);
}

// d = a + b (b a register, or bimm when b < 0), flags as live.
static void emit_add(struct tctx* t, int d, int a, int b, int32_t bimm, uint8_t live) {
	bool keep = live & (F_C | F_V);
	int res = keep && (d == a || d == b) ? 11 : d;
	if (b >= 0) {
		E(P_ADD(res, a, b));
	} else {
		E(P_ADDI(res, a, bimm));
	}
	if (live & (F_N | F_Z)) {
		E(P_CMPWI(2, res, 0));
	}
	if (live & F_C) { // carry iff the sum wrapped: res < b (unsigned)
		if (b >= 0) {
			E(P_CMPLW(3, res, b));
		} else {
			E(P_CMPLWI(3, res, bimm));
		}
		t->c_inv = false;
	}
	if (live & F_V) {
		if (b >= 0) { // operands of equal sign, result of the other
			E(P_XOR(0, a, res));
			E(P_XOR(12, b, res));
			E(P_AND(0, 0, 12));
		} else { // b >= 0: a >= 0 and res < 0
			E(P_ANDC(0, res, a));
		}
		E(P_CMPWI(4, 0, 0));
	}
	if (res != d) {
		E(P_MR(d, res));
	}
}

// d = a - b (b a register, or bimm when b < 0); d < 0 = compare only.
static void emit_sub(struct tctx* t, int d, int a, int b, int32_t bimm, uint8_t live) {
	bool keep = live & (F_C | F_V);
	int res = d < 0 || (keep && (d == a || d == b)) ? 11 : d;
	if (b >= 0) {
		E(P_SUBF(res, b, a));
	} else {
		E(P_ADDI(res, a, -bimm));
	}
	if (live & (F_N | F_Z)) {
		E(P_CMPWI(2, res, 0));
	}
	if (live & F_C) { // no borrow iff a >= b (unsigned): held inverted as cr3.lt = a < b
		if (b >= 0) {
			E(P_CMPLW(3, a, b));
		} else {
			E(P_CMPLWI(3, a, bimm));
		}
		t->c_inv = true;
	}
	if (live & F_V) {
		if (b >= 0) { // operands of different sign, result's sign differs from a
			E(P_XOR(0, a, b));
			E(P_XOR(12, a, res));
			E(P_AND(0, 0, 12));
		} else { // b >= 0: a < 0 and res >= 0
			E(P_ANDC(0, a, res));
		}
		E(P_CMPWI(4, 0, 0));
	}
	if (d >= 0 && res != d) {
		E(P_MR(d, res));
	}
}

static void emit_nz(struct tctx* t, int reg, uint8_t live) {
	if (live & (F_N | F_Z)) {
		E(P_CMPWI(2, reg, 0));
	}
}

// C = bit `bit` of reg.
static void emit_c_bit(struct tctx* t, int reg, int bit) {
	E(P_RLWINM(0, reg, (31 - bit) & 31, 0, 0));
	E(P_CMPWI(3, 0, 0));
	t->c_inv = false;
}

enum { LD32, LD16, LD8, LDS8, ST32, ST16, ST8 };

// One load or store through the core's memory routines (memory.c): address in r4; stores take
// their value from GBA register rd, or from r6 if rd < 0; loads with rd < 0 leave it in r3.
static void emit_mem(struct tctx* t, const struct insn* in, int kind, int rd) {
	static const uint8_t routine[] = { JIT_LD32, JIT_LD16, JIT_LD8, JIT_LD8, JIT_ST32, JIT_ST16, JIT_ST8 };
	struct jit* j = t->j;
	flush_cycles(t); // the slow path hands mGBA the interpreter's cycles
	jit_li32(j, 5, in->a + (t->thumb ? 4 : 8));
	if (kind >= ST32 && rd >= 0) {
		E(P_MR(6, RG(rd)));
	}
	jit_bl(j, j->mem[t->rom && t->gba->memory.prefetch][routine[kind]]);
	if (kind < ST32) {
		int d = rd >= 0 ? RG(rd) : 3; // rd < 0: leave it in r3 (never RG(-1): that is r13)
		if (kind == LDS8) {
			E(P_EXTSB(d, 3));
		} else if (d != 3) {
			E(P_MR(d, 3));
		}
	}
	t->pending = in->cycles;
}

// LDM/STM/PUSH/POP through mGBA, by the core's shared routine (memory.c, emit_lsm_mgba), with
// the cycles flushed. Leaves the final address in r3, a loaded PC in gprs[15].
static void emit_lsm_mgba(struct tctx* t, const struct insn* in, int base, int mask, int dir, bool load) {
	E(P_MR(4, RG(base)));
	jit_li32(t->j, 5, mask);
	E(P_LI(6, dir));
	jit_li32(t->j, 7, in->a + (t->thumb ? 4 : 8));
	jit_bl(t->j, t->j->lsm_mgba[!load]);
}

// LDM/STM/PUSH/POP: inline when all its words are in IWRAM or EWRAM (the core's lsm routine
// checks that and adds mGBA's wait), else through mGBA, after the block. Writes the final address
// back to the base if `wb`. A loaded PC is left in r3 (for the caller's WritePC exit). Same
// registers, memory, cycles and store log as mGBA: it stores the base's old value if listed.
// Not for STM of PC.
static void emit_lsm(struct tctx* t, const struct insn* in, int index, int base, int mask, enum LSMDirection dir,
                     bool load, bool wb) {
	struct jit* j = t->j;
	int n = __builtin_popcount(mask);
	if (j->profile) {
		jit_la(j, 12, (uintptr_t) &j->prof_lsm[!load]);
		E(P_LWZ(11, 0, 12));
		E(P_ADDI(11, 11, 1));
		E(P_STW(11, 0, 12));
	}
	flush_cycles(t); // the mGBA path hands it the interpreter's cycles
	if (!n) { // empty list: mGBA's quirk (PC, 64 bytes)
		emit_lsm_mgba(t, in, base, mask, dir, load);
		if (wb) {
			E(P_MR(RG(base), 3));
		}
		t->pending = in->cycles;
		return;
	}
	int lowest = dir & LSM_D ? -4 * n : 0; // IA, DB
	if (!(dir & LSM_B) != !(dir & LSM_D)) { // IB, DA
		lowest += 4;
	}
	E(P_ADDI(4, RG(base), lowest));
	bool stall = t->rom && t->gba->memory.prefetch;
	int kind = !load ? 1 : mask & (1 << ARM_PC) ? 2 : 0;
	if (stall && kind != 2) { // a loaded PC is what the stall sees (memory.c)
		jit_li32(j, 5, in->a + (t->thumb ? 4 : 8));
	}
	E(P_LI(6, n));
	jit_bl(j, j->lsm[stall][kind]);
	t->lsm[t->nlsm].at = j->cur;
	E(P_BC(BO_TRUE_UNLIKELY, CR_EQ(5), 0));
	for (int r = 0, k = 0; r < 16; ++r) {
		if (mask & (1 << r)) {
			if (k) {
				E(P_LI(0, 4 * k));
			}
			// r3 = the lowest word in host memory; rA = 0 reads as 0, so the first is (0, r3)
			int d = r == ARM_PC ? 3 : RG(r); // PC: the last word, r3 is free then
			E(load ? P_LWBRX(d, k ? 3 : 0, k ? 0 : 3) : P_STWBRX(d, k ? 3 : 0, k ? 0 : 3));
			++k;
		}
	}
	if (wb) {
		E(P_ADDI(RG(base), RG(base), dir & LSM_D ? -4 * n : 4 * n));
	}
	t->lsm[t->nlsm].back = j->cur;
	t->lsm[t->nlsm].index = index;
	t->lsm[t->nlsm].base = base;
	t->lsm[t->nlsm].mask = mask;
	t->lsm[t->nlsm].dir = dir;
	t->lsm[t->nlsm].load = load;
	t->lsm[t->nlsm++].wb = wb;
	t->pending = in->cycles;
}

// The mGBA paths of the block's inline LDM/STM, after its code.
static void emit_lsm_slow(struct tctx* t) {
	for (int i = 0; i < t->nlsm; ++i) {
		*t->lsm[i].at |= ((char*) t->j->cur - (char*) t->lsm[i].at) & 0xFFFC;
		emit_lsm_mgba(t, &t->ins[t->lsm[i].index], t->lsm[i].base, t->lsm[i].mask, t->lsm[i].dir, t->lsm[i].load);
		if (t->lsm[i].wb) {
			E(P_MR(RG(t->lsm[i].base), 3));
		}
		if (t->lsm[i].mask & (1 << ARM_PC)) {
			E(P_LWZ(3, OFF_PC, RCPU)); // mGBA loaded it into gprs[15]
		}
		jit_b(t->j, t->lsm[i].back);
	}
}

// ARM_WAIT_SMUL: 1-4 internal cycles by how many top bytes of R are all 0 or all 1, plus
// `extra`, through mGBA's stall logic (identity unless code runs from ROM with prefetch on).
// Adds them to r30. Clobbers r0, r5, r7-r12.
static void emit_mul_wait(struct tctx* t, const struct insn* in, int reg, int extra) {
	E(P_SRAWI(0, reg, 31));
	E(P_XOR(0, 0, reg));
	E(P_ORI(0, 0, 1));
	E(P_CNTLZW(0, 0));
	E(P_SRWI(0, 0, 3));
	E(P_SUBFIC(0, 0, 4 + extra));
	if (t->rom && t->gba->memory.prefetch) {
		jit_li32(t->j, 5, in->a + (t->thumb ? 4 : 8));
		jit_bl(t->j, t->j->stall); // mGBA's GBAMemoryStall: r0 = wait, adds it to r30
	} else {
		E(P_ADD(RCYC, RCYC, 0));
	}
}

// mGBA's handler for one instruction, from REG state.
static void emit_fallback(struct tctx* t, const struct insn* in, int index) {
	struct jit* j = t->j;
	uint32_t w = t->thumb ? WORD_SIZE_THUMB : WORD_SIZE_ARM;
	flush_cycles(t);
	canon_c(t);
	if (!t->mem) {
		jit_bl(j, j->sync_out);
	}
	jit_li32(j, 0, in->a + 2 * w);
	E(P_STW(0, OFF_PC, RCPU));
	jit_li32(j, 0, jit_fetch(t->cpu, in->a + w, t->thumb));
	E(P_STW(0, OFF_PREF, RCPU));
	jit_li32(j, 0, jit_fetch(t->cpu, in->a + 2 * w, t->thumb));
	E(P_STW(0, OFF_PREF + 4, RCPU));
	E(P_MR(3, RCPU));
	jit_li32(j, 4, in->op);
	if (t->thumb) {
		jit_call(j, _thumbTable[in->op >> 6]);
	} else {
		jit_call(j, jit_arm_exec);
	}
	if (t->mem) {
		E(P_LWZ(RCYC, OFF_CYC, RCPU));
	} else {
		jit_bl(j, j->sync_in);
	}
	// A branch or exception moved PC: the handler left the interpreter's state, just exit.
	E(P_LWZ(0, OFF_PC, RCPU));
	jit_li32(j, 12, in->a + 2 * w);
	E(P_CMPLW(1, 0, 12));
	bool mem = t->mem;
	t->mem = true; // the state in memory is current either way
	stub_bc(t, BO_FALSE_UNLIKELY, CR_EQ(1), 0, EXIT_PCSET, index + 1);
	t->mem = mem;
	t->lr_known = false;
}

// cr0.lt = condition, true/false polarity in *bo. Returns the BI to test.
static int emit_cond(struct tctx* t, int cond, int* bo) {
	bool c = !t->c_inv; // cr3.lt is C itself
	*bo = BO_TRUE;
	switch (cond) {
	case 0x0: return CR_Z;
	case 0x1: *bo = BO_FALSE; return CR_Z;
	case 0x2: *bo = c ? BO_TRUE : BO_FALSE; return CR_C;
	case 0x3: *bo = c ? BO_FALSE : BO_TRUE; return CR_C;
	case 0x4: return CR_N;
	case 0x5: *bo = BO_FALSE; return CR_N;
	case 0x6: return CR_V;
	case 0x7: *bo = BO_FALSE; return CR_V;
	case 0x8: // HI: C && !Z
	case 0x9: // LS
		if (c) {
			E(P_CRANDC(CR_LT(0), CR_C, CR_Z));
		} else {
			E(P_CRNOR(CR_LT(0), CR_C, CR_Z));
		}
		*bo = cond == 0x8 ? BO_TRUE : BO_FALSE;
		return CR_LT(0);
	case 0xA: // GE: N == V
	case 0xB:
		E(P_CREQV(CR_LT(0), CR_N, CR_V));
		*bo = cond == 0xA ? BO_TRUE : BO_FALSE;
		return CR_LT(0);
	default: // GT: !Z && N == V; LE
		E(P_CREQV(CR_LT(0), CR_N, CR_V));
		E(P_CRANDC(CR_LT(0), CR_LT(0), CR_Z));
		*bo = cond == 0xC ? BO_TRUE : BO_FALSE;
		return CR_LT(0);
	}
}

// Taken static branch: WritePC within the region costs 2 + N + S more.
static void emit_static_branch(struct tctx* t, uint32_t target, int count) {
	// mGBA halts on branches to the game's known idle loop (setActiveRegion): those go through it.
	if (t->fast_branches && same_region_target(t, target) && target != t->gba->idleLoop) {
		lastprefetch_reset(t);
		t->pending += 2 + t->N + t->S;
		emit_chain_exit(t, target, count, t->idle && target == t->ins[0].a ? EXIT_LOOP : EXIT_CHAIN);
	} else {
		jit_li32(t->j, 3, target);
		emit_writepc_exit(t, 3, EXIT_WRITEPC, count, t->pf);
	}
}

static void emit_thumb(struct tctx* t, struct insn* in, int index) {
	uint32_t op = in->op;
	uint8_t live = in->def & in->live_out;
	int rd = op & 7, rn = (op >> 3) & 7, rm = (op >> 6) & 7;
	int imm5 = (op >> 6) & 0x1F, imm8 = op & 0xFF;
	if (in->kind == K_FALLBACK) {
		emit_fallback(t, in, index);
		return;
	}
	bool pop_pc = (op & 0xFF00) == 0xBD00; // its cycles come with its memory call
	if (in->kind == K_ALU || (in->kind == K_BRANCH && !pop_pc)) {
		t->pending += in->cycles;
	}
	switch (op >> 11) {
	case 0x00: // LSL rd, rm, #imm
		if (imm5 && (live & F_C)) {
			emit_c_bit(t, RG(rn), 32 - imm5);
		}
		E(P_RLWINM(RG(rd), RG(rn), imm5, 0, 31 - imm5));
		emit_nz(t, RG(rd), live);
		return;
	case 0x01: // LSR rd, rm, #imm (0 = 32)
		if (live & F_C) {
			emit_c_bit(t, RG(rn), imm5 ? imm5 - 1 : 31);
		}
		if (imm5) {
			E(P_RLWINM(RG(rd), RG(rn), 32 - imm5, imm5, 31));
		} else {
			E(P_LI(RG(rd), 0));
		}
		emit_nz(t, RG(rd), live);
		return;
	case 0x02: // ASR rd, rm, #imm (0 = 32)
		if (live & F_C) {
			emit_c_bit(t, RG(rn), imm5 ? imm5 - 1 : 31);
		}
		E(P_SRAWI(RG(rd), RG(rn), imm5 ? imm5 : 31));
		emit_nz(t, RG(rd), live);
		return;
	case 0x03: { // ADD/SUB rd, rn, rm/#imm3
		bool imm = op & 0x0400, sub = op & 0x0200;
		int b = imm ? -1 : RG(rm);
		if (sub) {
			emit_sub(t, RG(rd), RG(rn), b, rm, live);
		} else {
			emit_add(t, RG(rd), RG(rn), b, rm, live);
		}
		return;
	}
	case 0x04: // MOV rd, #imm8
		E(P_LI(RG((op >> 8) & 7), imm8));
		emit_nz(t, RG((op >> 8) & 7), live);
		return;
	case 0x05: // CMP rd, #imm8
		emit_sub(t, -1, RG((op >> 8) & 7), -1, imm8, live);
		return;
	case 0x06: // ADD rd, #imm8
		emit_add(t, RG((op >> 8) & 7), RG((op >> 8) & 7), -1, imm8, live);
		return;
	case 0x07: // SUB rd, #imm8
		emit_sub(t, RG((op >> 8) & 7), RG((op >> 8) & 7), -1, imm8, live);
		return;
	case 0x08:
		if (!(op & 0x0400)) {
			int d = RG(rd), s = RG(rn);
			switch ((op >> 6) & 0xF) {
			case 0x0: E(P_AND(d, d, s)); emit_nz(t, d, live); return;
			case 0x1: E(P_XOR(d, d, s)); emit_nz(t, d, live); return;
			case 0x8: E(P_AND(11, d, s)); emit_nz(t, 11, live); return; // TST
			case 0x9: // NEG rd, rn = 0 - rn
				E(P_LI(12, 0));
				if (live & (F_C | F_V)) {
					E(P_SUBF(11, s, 12));
					emit_nz(t, 11, live);
					if (live & F_C) {
						E(P_CMPLW(3, 12, s));
						t->c_inv = true;
					}
					if (live & F_V) {
						E(P_AND(0, s, 11));
						E(P_CMPWI(4, 0, 0));
					}
					E(P_MR(d, 11));
				} else {
					E(P_NEG(d, s));
					emit_nz(t, d, live);
				}
				return;
			case 0xA: emit_sub(t, -1, d, s, 0, live); return; // CMP
			case 0xB: emit_add(t, 11, d, s, 0, live); return; // CMN (result unused)
			case 0xC: E(P_OR(d, d, s)); emit_nz(t, d, live); return;
			case 0xD: // MUL rd, rn: wait from rd's old value
				emit_mul_wait(t, in, d, 0);
				E(P_MULLW(d, d, s));
				emit_nz(t, d, live);
				t->pending += in->cycles;
				return;
			case 0xE: E(P_ANDC(d, d, s)); emit_nz(t, d, live); return;
			case 0xF: E(P_NOT(d, s)); emit_nz(t, d, live); return;
			}
		} else {
			int hd = (op & 7) | ((op >> 4) & 8), hm = (op >> 3) & 0xF;
			int s = reg_or_pc(t, in, hm, 12);
			switch ((op >> 8) & 3) {
			case 0: // ADD
				if (hd == ARM_PC) {
					E(P_ADDI(3, s, 0));
					jit_li32(t->j, 0, in->a + 4);
					E(P_ADD(3, 3, 0));
					emit_writepc_exit(t, 3, EXIT_WRITEPC, index + 1, in->cycles);
					return;
				}
				E(P_ADD(RG(hd), RG(hd), s));
				if (hd == ARM_LR) t->lr_known = false;
				return;
			case 1: // CMP
				emit_sub(t, -1, reg_or_pc(t, in, hd, 11), s, 0, live);
				return;
			case 2: // MOV
				if (hd == ARM_PC) {
					emit_writepc_exit(t, s, EXIT_WRITEPC, index + 1, in->cycles);
					return;
				}
				E(P_MR(RG(hd), s));
				if (hd == ARM_LR) t->lr_known = false;
				return;
			case 3: // BX: the dispatcher switches mode and adds this instruction's cycles
				t->pending -= in->cycles;
				emit_writepc_exit(t, s, EXIT_BX, index + 1, 0);
				return;
			}
		}
		break;
	case 0x09: { // LDR rd, [PC, #imm]
		uint32_t address = ((in->a + 4) & ~3u) + (imm8 << 2);
		int d = RG((op >> 8) & 7);
		if (in->kind == K_ALU) {
			jit_li32(t->j, d, jit_fetch(t->cpu, address, false));
			return;
		}
		jit_li32(t->j, 4, address);
		emit_mem(t, in, LD32, (op >> 8) & 7);
		return;
	}
	case 0x0A: case 0x0B: { // load/store with register offset
		static const uint8_t kinds[8] = { ST32, ST16, ST8, LDS8, LD32, LD16, LD8, 0 };
		E(P_ADD(4, RG(rn), RG(rm)));
		emit_mem(t, in, kinds[(op >> 9) & 7], rd);
		return;
	}
	case 0x0C: // STR rd, [rn, #imm5 * 4]
	case 0x0D: // LDR
		E(P_ADDI(4, RG(rn), imm5 << 2));
		emit_mem(t, in, op & 0x0800 ? LD32 : ST32, rd);
		return;
	case 0x0E: case 0x0F: // STRB/LDRB rd, [rn, #imm5]
		E(P_ADDI(4, RG(rn), imm5));
		emit_mem(t, in, op & 0x0800 ? LD8 : ST8, rd);
		return;
	case 0x10: case 0x11: // STRH/LDRH rd, [rn, #imm5 * 2]
		E(P_ADDI(4, RG(rn), imm5 << 1));
		emit_mem(t, in, op & 0x0800 ? LD16 : ST16, rd);
		return;
	case 0x12: case 0x13: // STR/LDR rd, [SP, #imm8 * 4]
		E(P_ADDI(4, RG(ARM_SP), imm8 << 2));
		emit_mem(t, in, op & 0x0800 ? LD32 : ST32, (op >> 8) & 7);
		return;
	case 0x14: // ADD rd, PC, #imm8 * 4
		jit_li32(t->j, RG((op >> 8) & 7), ((in->a + 4) & ~3u) + (imm8 << 2));
		return;
	case 0x15: // ADD rd, SP, #imm8 * 4
		E(P_ADDI(RG((op >> 8) & 7), RG(ARM_SP), imm8 << 2));
		return;
	case 0x16: case 0x17:
		if ((op & 0x0F00) == 0x0000) { // ADD/SUB SP, #imm7 * 4
			int v = (op & 0x7F) << 2;
			E(P_ADDI(RG(ARM_SP), RG(ARM_SP), op & 0x80 ? -v : v));
			return;
		}
		if (!(op & 0x0800)) { // PUSH {list, lr?}
			emit_lsm(t, in, index, ARM_SP, imm8 | (op & 0x0100 ? 1 << ARM_LR : 0), LSM_DB, false, true);
			return;
		}
		if (!(op & 0x0100)) { // POP {list}
			emit_lsm(t, in, index, ARM_SP, imm8, LSM_IA, true, true);
			return;
		}
		// POP {list, pc}
		emit_start_save(t);
		emit_lsm(t, in, index, ARM_SP, imm8 | 1 << ARM_PC, LSM_IA, true, true);
		emit_dynamic_cycles(t, in);
		emit_writepc_exit(t, 3, EXIT_WRITEPC, index + 1, -1);
		return;
	case 0x18: // STMIA rn!, {list}
		emit_lsm(t, in, index, (op >> 8) & 7, imm8, LSM_IA, false, true);
		return;
	case 0x19: // LDMIA rn!, {list} (not empty): no writeback if rn is in the list
		emit_lsm(t, in, index, (op >> 8) & 7, imm8, LSM_IA, true, !(imm8 & (1 << ((op >> 8) & 7))));
		return;
	case 0x1A: case 0x1B: { // B<cond>
		int bo, bi = emit_cond(t, (op >> 8) & 0xF, &bo);
		uint32_t target = in->a + 4 + ((int32_t) (int8_t) imm8 << 1);
		// Not taken: skip over the taken path.
		uint32_t* skip = t->j->cur;
		E(P_BC(bo ^ 8, bi, 0)); // BO_TRUE <-> BO_FALSE
		int pending = t->pending;
		bool c_inv = t->c_inv;
		emit_static_branch(t, target, index + 1);
		*skip |= ((char*) t->j->cur - (char*) skip) & 0xFFFC;
		t->pending = pending;
		t->c_inv = c_inv;
		emit_chain_exit(t, in->a + 2, index + 1, EXIT_CHAIN);
		return;
	}
	case 0x1C: { // B
		int32_t off = (int32_t) ((op & 0x7FF) << 21) >> 20;
		emit_static_branch(t, in->a + 4 + off, index + 1);
		return;
	}
	case 0x1E: { // BL first half: LR = PC + (offset << 12)
		int32_t off = (int32_t) ((op & 0x7FF) << 21) >> 9;
		t->lr_value = in->a + 4 + off;
		t->lr_known = true;
		jit_li32(t->j, RG(ARM_LR), t->lr_value);
		return;
	}
	case 0x1F: { // BL second half: PC = LR + (imm << 1), LR = return address | 1
		uint32_t off = (op & 0x7FF) << 1;
		if (t->lr_known) {
			jit_li32(t->j, RG(ARM_LR), (in->a + 2) | 1);
			emit_static_branch(t, t->lr_value + off, index + 1);
		} else {
			E(P_ADDI(3, RG(ARM_LR), off));
			jit_li32(t->j, RG(ARM_LR), (in->a + 2) | 1);
			emit_writepc_exit(t, 3, EXIT_WRITEPC, index + 1, in->cycles);
		}
		return;
	}
	}
	// Decoded as native but not emitted: a bug in this file.
	log_printf("jit: no emitter for %04x at %08x\n", op, in->a);
	E(0);
}

// ---- ARM ----

static bool arm_logical(int opcode) {
	return opcode <= 1 || (opcode >= 8 && opcode <= 9) || opcode >= 12;
}

static void arm_decode(struct tctx* t, struct insn* in) {
	uint32_t op = in->op;
	int rd = (op >> 12) & 0xF, rn = (op >> 16) & 0xF, rm = op & 0xF;
	in->cond = op >> 28;
	in->kind = K_FALLBACK;
	in->def = in->use = F_ALL;
	in->cycles = t->pf;
	if (in->cond == 0xF) {
		return;
	}
	switch ((op >> 25) & 7) {
	case 0: case 1:
		if ((op & 0x0FC000F0) == 0x00000090) { // MUL, MLA
			int dest = (op >> 16) & 0xF;
			if ((op & 0x00100000) || dest == ARM_PC || rd == ARM_PC || rm == ARM_PC || ((op >> 8) & 0xF) == ARM_PC) {
				return; // MULS: mGBA sets C from a stale shifter carry; keep its handler
			}
			in->kind = K_DYN;
			in->def = 0;
			in->cycles = t->pf + t->N - t->S;
			break;
		}
		if (!(op & 0x02000000) && (op & 0x90) == 0x90) { // halfword transfers, SWP, long multiply
			int sh = (op >> 5) & 3;
			bool load = op & 0x00100000, writeback = !(op & 0x01000000) || (op & 0x00200000);
			if ((op & 0x0F8000F0) == 0x00800090 || (op & 0x0F800FF0) == 0x01000090 || sh == 0 ||
			    sh == 3 || (!load && sh != 1) || rd == ARM_PC || (rn == ARM_PC && writeback) ||
			    (!(op & 0x00400000) && rm == ARM_PC)) {
				return; // long multiply, SWP, LDRSH (odd address quirk), odd forms
			}
			in->kind = K_CALL;
			in->def = 0;
			in->cycles = 1 + t->N;
			break;
		}
		if ((op & 0x0FFFFFF0) == 0x012FFF10) { // BX
			if (rm != ARM_PC) {
				in->kind = K_BRANCH;
				in->def = 0;
			}
			break;
		}
		{
			int opcode = (op >> 21) & 0xF;
			bool s = op & 0x00100000;
			if ((opcode >= 8 && opcode <= 11 && !s) || (!(op & 0x02000000) && (op & 0x10)) || rd == ARM_PC ||
			    opcode == 5 || opcode == 6 || opcode == 7) {
				return; // MRS/MSR, shift by register, writes PC, ADC/SBC/RSC
			}
			if (!(op & 0x02000000) && ((op >> 5) & 3) == 3 && !(op & 0xF80)) {
				return; // RRX
			}
			in->kind = K_ALU;
			in->def = 0;
			if (s) {
				if (!arm_logical(opcode)) {
					in->def = F_ALL;
				} else {
					in->def = F_N | F_Z;
					bool imm = op & 0x02000000;
					if ((imm && (op & 0xF00)) || (!imm && (((op >> 5) & 3) != 0 || (op & 0xF80)))) {
						in->def |= F_C; // the shifter sets C (else it keeps it)
					}
				}
			}
		}
		break;
	case 2: case 3: { // LDR, STR
		bool pre = op & 0x01000000, writeback = !pre || (op & 0x00200000), load = op & 0x00100000;
		bool reg = op & 0x02000000;
		if ((reg && (op & 0x10)) || (load && rd == ARM_PC) || (rn == ARM_PC && writeback) || (!pre && (op & 0x00200000)) ||
		    (reg && (rm == ARM_PC || (((op >> 5) & 3) == 3 && !(op & 0xF80))))) {
			return; // undefined, loads into PC, LDRT/STRT, RRX offsets
		}
		in->kind = K_CALL;
		in->def = 0;
		in->cycles = 1 + t->N;
		break;
	}
	case 4: { // LDM, STM
		bool load = op & 0x00100000;
		if ((op & 0x00400000) || !(op & 0xFFFF) || rn == ARM_PC || (!load && (op & 0x8000))) {
			return; // user bank / CPSR, empty list, STM of PC
		}
		in->kind = load && (op & 0x8000) ? K_BRANCH : K_CALL;
		in->def = 0;
		in->cycles = 1 + t->N;
		break;
	}
	case 5: // B, BL
		in->kind = K_BRANCH;
		in->def = 0;
		break;
	default:
		return; // coprocessor, SWI
	}
	in->use = in->cond != 0xE ? cond_use(in->cond) : 0;
}

// The barrel shifter's operand: an immediate (*imm set, returns -1) or a register. Sets C when
// the S bit asks for it and the shifter defines it.
static int arm_shifted_reg(struct tctx* t, const struct insn* in, bool set_c);

static int arm_operand(struct tctx* t, const struct insn* in, bool set_c, uint32_t* imm) {
	uint32_t op = in->op;
	if (!(op & 0x02000000)) {
		return arm_shifted_reg(t, in, set_c);
	}
	{
		int rot = (op >> 7) & 0x1E;
		uint32_t v = op & 0xFF;
		*imm = rot ? (v >> rot) | (v << (32 - rot)) : v;
		if (set_c && rot) {
			int bit = CR_C;
			E(*imm >> 31 ? P_CREQV(bit, bit, bit) : P_CRXOR(bit, bit, bit));
			t->c_inv = false;
		}
		return -1;
	}
}

// rm shifted by an immediate (data processing operand, or a LDR/STR register offset, where
// bit 25 means the opposite). Returns the register holding it.
static int arm_shifted_reg(struct tctx* t, const struct insn* in, bool set_c) {
	uint32_t op = in->op;
	int rm = op & 0xF, type = (op >> 5) & 3, n = (op >> 7) & 0x1F;
	int src = rm == ARM_PC ? 10 : RG(rm);
	if (rm == ARM_PC) {
		jit_li32(t->j, 10, in->a + 8);
	}
	switch (type) {
	case 0: // LSL
		if (!n) {
			return src;
		}
		if (set_c) {
			emit_c_bit(t, src, 32 - n);
		}
		E(P_RLWINM(10, src, n, 0, 31 - n));
		return 10;
	case 1: // LSR (0 = 32)
		if (set_c) {
			emit_c_bit(t, src, n ? n - 1 : 31);
		}
		if (n) {
			E(P_RLWINM(10, src, 32 - n, n, 31));
		} else {
			E(P_LI(10, 0));
		}
		return 10;
	case 2: // ASR (0 = 32)
		if (set_c) {
			emit_c_bit(t, src, n ? n - 1 : 31);
		}
		E(P_SRAWI(10, src, n ? n : 31));
		return 10;
	default: // ROR (#0 = RRX is left to mGBA)
		if (set_c) {
			emit_c_bit(t, src, n - 1);
		}
		E(P_ROTLWI(10, src, 32 - n));
		return 10;
	}
}

static bool fits_u15(uint32_t v) {
	return v < 0x8000;
}

static void arm_alu(struct tctx* t, const struct insn* in) {
	uint32_t op = in->op;
	int opcode = (op >> 21) & 0xF, rd = (op >> 12) & 0xF, rn = (op >> 16) & 0xF;
	bool s = op & 0x00100000;
	uint8_t live = s ? in->def & in->live_out : 0;
	uint32_t imm = 0;
	int b = arm_operand(t, in, s && (live & F_C) && arm_logical(opcode), &imm);
	int a = 0;
	if (opcode != 13 && opcode != 15) { // MOV and MVN ignore rn
		a = rn == ARM_PC ? 9 : RG(rn);
		if (rn == ARM_PC) {
			jit_li32(t->j, 9, in->a + 8);
		}
	}
	int d = RG(rd);
	if (!arm_logical(opcode)) {
		if (b < 0 && !fits_u15(imm)) {
			jit_li32(t->j, 10, imm);
			b = 10;
		}
		switch (opcode) {
		case 2: emit_sub(t, d, a, b, imm, live); return;          // SUB
		case 3:                                                   // RSB = op2 - rn
			if (b < 0) {
				jit_li32(t->j, 10, imm);
				b = 10;
			}
			emit_sub(t, d, b, a, 0, live);
			return;
		case 4: emit_add(t, d, a, b, imm, live); return;          // ADD
		case 10: emit_sub(t, -1, a, b, imm, live); return;        // CMP
		case 11: emit_add(t, 11, a, b, imm, live); return;        // CMN
		}
	}
	if (b < 0) {
		jit_li32(t->j, 10, imm);
		b = 10;
	}
	switch (opcode) {
	case 0: E(P_AND(d, a, b)); break;
	case 1: E(P_XOR(d, a, b)); break;
	case 8: d = 11; E(P_AND(d, a, b)); break; // TST
	case 9: d = 11; E(P_XOR(d, a, b)); break; // TEQ
	case 12: E(P_OR(d, a, b)); break;
	case 13: if (d != b) E(P_MR(d, b)); break;
	case 14: E(P_ANDC(d, a, b)); break;
	case 15: E(P_NOT(d, b)); break;
	}
	emit_nz(t, d, live);
}

// LDR/STR/LDRB/STRB and the halfword forms. The address goes to r4; the base is written back
// before a load (the loaded value wins if rd == rn) and after a store, like mGBA.
static void arm_mem(struct tctx* t, const struct insn* in) {
	uint32_t op = in->op;
	int rd = (op >> 12) & 0xF, rn = (op >> 16) & 0xF;
	bool pre = op & 0x01000000, up = op & 0x00800000, writeback = !pre || (op & 0x00200000);
	bool load = op & 0x00100000, half = !(op & 0x0C000000);
	int kind;
	int32_t off = 0;
	int offreg = -1;
	if (half) {
		if (op & 0x00400000) {
			off = ((op >> 4) & 0xF0) | (op & 0xF);
		} else {
			offreg = RG(op & 0xF);
		}
		int sh = (op >> 5) & 3;
		kind = !load ? ST16 : sh == 1 ? LD16 : LDS8;
	} else {
		if (op & 0x02000000) {
			offreg = arm_shifted_reg(t, in, false);
		} else {
			off = op & 0xFFF;
		}
		bool byte = op & 0x00400000;
		kind = load ? (byte ? LD8 : LD32) : (byte ? ST8 : ST32);
	}
	int base = RG(rn);
	if (rn == ARM_PC) {
		jit_li32(t->j, 9, in->a + 8);
		base = 9;
	}
	// r4 = address
	if (!pre) {
		E(P_MR(4, base));
	} else if (offreg >= 0) {
		E(up ? P_ADD(4, base, offreg) : P_SUBF(4, offreg, base));
	} else {
		E(P_ADDI(4, base, up ? off : -off));
	}
	if (!load) {
		if (rd == ARM_PC) {
			jit_li32(t->j, 6, in->a + 12);
		} else {
			E(P_MR(6, RG(rd)));
		}
	}
	// Writeback (its register may be r10, which the call clobbers): the new base in r8 for
	// stores, done now for loads.
	int wb = load ? base : 8;
	if (writeback) {
		if (offreg >= 0) {
			E(up ? P_ADD(wb, base, offreg) : P_SUBF(wb, offreg, base));
		} else {
			E(P_ADDI(wb, base, up ? off : -off));
		}
	}
	if (!load && writeback) {
		E(P_STW(8, FRAME_SCRATCH, 1)); // r8 is volatile: keep it in the frame across the call
	}
	emit_mem(t, in, kind, -1);
	if (load) {
		E(P_MR(RG(rd), 3));
	} else if (writeback) {
		E(P_LWZ(RG(rn), FRAME_SCRATCH, 1));
	}
}

static void emit_arm(struct tctx* t, struct insn* in, int index) {
	uint32_t op = in->op;
	if (in->kind == K_FALLBACK) {
		emit_fallback(t, in, index);
		return;
	}
	bool cond = in->cond != 0xE;
	uint32_t* skip = NULL;
	int pending = 0;
	bool c_inv = false;
	if (in->kind == K_ALU || (in->kind == K_BRANCH && (op & 0x0FFFFFF0) != 0x012FFF10 && ((op >> 25) & 7) != 4)) {
		t->pending += in->cycles; // runs or not, the same cycles
	}
	if (cond) {
		if (in->kind == K_CALL || in->kind == K_DYN) {
			flush_cycles(t);
		}
		canon_c(t); // both paths must agree on C's polarity
		int bo, bi = emit_cond(t, in->cond, &bo);
		skip = t->j->cur;
		E(P_BC(bo ^ 8, bi, 0));
		pending = t->pending;
		c_inv = t->c_inv;
	}
	switch ((op >> 25) & 7) {
	case 0: case 1:
		if ((op & 0x0FC000F0) == 0x00000090) { // MUL, MLA
			int dest = RG((op >> 16) & 0xF), rs = RG((op >> 8) & 0xF), rm = RG(op & 0xF);
			bool mla = op & 0x00200000;
			emit_mul_wait(t, in, rs, mla);
			E(P_MULLW(11, rm, rs));
			if (mla) {
				E(P_ADD(11, 11, RG((op >> 12) & 0xF)));
			}
			E(P_MR(dest, 11));
			t->pending += in->cycles;
		} else if (in->kind == K_CALL) {
			arm_mem(t, in);
		} else if (in->kind == K_BRANCH) { // BX: the dispatcher adds its cycles after the switch
			emit_writepc_exit(t, RG(op & 0xF), EXIT_BX, index + 1, 0);
		} else {
			arm_alu(t, in);
		}
		break;
	case 2: case 3:
		arm_mem(t, in);
		break;
	case 4: {
		int rn = (op >> 16) & 0xF;
		bool load = op & 0x00100000, writeback = op & 0x00200000;
		int dir = (op & 0x01000000 ? LSM_B : 0) | (op & 0x00800000 ? 0 : LSM_D);
		bool wb = writeback && !(load && (op & (1 << rn)));
		if (in->kind == K_BRANCH) { // LDM with PC
			emit_start_save(t);
		}
		emit_lsm(t, in, index, rn, op & 0xFFFF, dir, load, wb); // not empty, no STM of PC (arm_decode)
		if (in->kind == K_BRANCH) {
			emit_dynamic_cycles(t, in);
			emit_writepc_exit(t, 3, EXIT_WRITEPC, index + 1, -1);
		}
		break;
	}
	case 5: { // B, BL
		uint32_t target = in->a + 8 + ((int32_t) (op << 8) >> 6);
		if (op & 0x01000000) {
			jit_li32(t->j, RG(ARM_LR), in->a + 4);
		}
		emit_static_branch(t, target, index + 1);
		break;
	}
	}
	if (!cond) {
		return;
	}
	if (in->kind == K_BRANCH) {
		// Not taken: the next instruction, in another block.
		*skip |= ((char*) t->j->cur - (char*) skip) & 0xFFFC;
		t->pending = pending; // B/BL counted theirs before the condition
		t->c_inv = c_inv;
		if ((op & 0x0FFFFFF0) == 0x012FFF10 || ((op >> 25) & 7) == 4) {
			t->pending = pending + t->pf; // BX and LDM didn't count theirs
		}
		emit_chain_exit(t, in->a + 4, index + 1, EXIT_CHAIN);
		return;
	}
	canon_c(t);
	if (in->kind == K_ALU) {
		*skip |= ((char*) t->j->cur - (char*) skip) & 0xFFFC;
		return;
	}
	// Calls and multiplies: cycles differ by path, settle them in r30 on both.
	flush_cycles(t);
	uint32_t* done = t->j->cur;
	E(P_B(0));
	*skip |= ((char*) t->j->cur - (char*) skip) & 0xFFFC;
	E(P_ADDI(RCYC, RCYC, t->pf));
	*done |= ((char*) t->j->cur - (char*) done) & 0x03FFFFFC;
}

// ---- Block translation ----

static bool thumb_ends_block(const struct insn* in) {
	return in->kind == K_BRANCH || (in->kind == K_FALLBACK && (
	       (in->op & 0xF000) == 0xD000                    // SWI, undefined
	    || (in->op & 0xF000) == 0xE000                    // undefined BLX suffix
	    || (in->op & 0xFF00) == 0xBE00                    // BKPT
	    || ((in->op & 0xF800) == 0xC800 && !(in->op & 0xFF)))); // LDMIA with an empty list
}

// Only sizes ARM blocks: a PC write it misses is still caught by the PC check after the handler.
static bool arm_ends_block(uint32_t op) {
	return (op & 0x0E000000) == 0x0A000000                                  // B, BL
	    || (op & 0x0FFFFFF0) == 0x012FFF10                                  // BX
	    || (op & 0x0E108000) == 0x08108000                                  // LDM with PC
	    || (op & 0x0C10F000) == 0x0410F000                                  // LDR into PC
	    || ((op & 0x0C000000) == 0x00000000 && (op & 0x0000F000) == 0x0000F000) // ALU into PC
	    || (op & 0x0C000000) == 0x0C000000                                  // coprocessor, SWI
	    || (op & 0x0E000010) == 0x06000010;                                 // undefined
}

static bool translatable(struct jit* j, uint32_t pc) {
	switch (pc >> 24) {
	case GBA_REGION_BIOS:
		return pc < GBA_SIZE_BIOS;
	case GBA_REGION_EWRAM:
	case GBA_REGION_IWRAM:
		return true;
	case GBA_REGION_ROM0:
	case GBA_REGION_ROM0_EX:
	case GBA_REGION_ROM1:
	case GBA_REGION_ROM1_EX:
	case GBA_REGION_ROM2:
	case GBA_REGION_ROM2_EX:
		return (pc & (GBA_SIZE_ROM0 - 1)) < j->gba->memory.romSize;
	default:
		return false;
	}
}

// Segment checks and flag liveness. A check (possible exit) precedes the first instruction and
// every instruction after a call; at those, and at the end, all flags are live.
static void analyze(struct tctx* t) {
	int seg = 0;
	for (int i = 0; i < t->n; ++i) {
		struct insn* in = &t->ins[i];
		in->check = i == 0 || t->ins[i - 1].kind != K_ALU;
		if (in->check) {
			seg = i;
			in->seg_cycles = 0;
		} else {
			// The segment runs whole if cycles before its last instruction stay below nextEvent.
			t->ins[seg].seg_cycles += t->ins[i - 1].cycles;
		}
	}
	uint8_t live = F_ALL;
	for (int i = t->n - 1; i >= 0; --i) {
		struct insn* in = &t->ins[i];
		in->live_out = live;
		// A conditional instruction may not run: its flags don't kill the old ones.
		live = (in->cond == 0xE ? live & ~in->def : live) | in->use;
		if (in->check) {
			live = F_ALL;
		}
	}
}

// Thumb registers an instruction writes (for the instructions an idle loop may contain).
static uint16_t thumb_writes(uint32_t op) {
	switch (op >> 11) {
	case 0x00: case 0x01: case 0x02: case 0x03:
		return 1 << (op & 7);
	case 0x04: case 0x06: case 0x07: case 0x09: case 0x13: case 0x14: case 0x15:
		return 1 << ((op >> 8) & 7);
	case 0x05:
		return 0;
	case 0x08:
		if (!(op & 0x0400)) {
			int alu = (op >> 6) & 0xF;
			return alu == 0x8 || alu == 0xA || alu == 0xB ? 0 : 1 << (op & 7);
		}
		return (op & 0x0300) == 0x0100 ? 0 : 1 << ((op & 7) | ((op >> 4) & 8));
	case 0x16: case 0x17:
		return 1 << ARM_SP;
	case 0x1E:
		return 1 << ARM_LR;
	default:
		return 1 << (op & 7); // loads
	}
}

// An idle loop candidate: a block that branches back to its own start and otherwise only
// computes and loads, with load addresses built from registers the loop never writes. When
// two consecutive iterations start with the same registers and flags, every later iteration
// is identical until something outside the CPU changes memory, which only happens at an event:
// the dispatcher may then skip whole iterations (jit_run_loop, EXIT_LOOP).
static void find_idle_loop(struct tctx* t, struct jit_block* b) {
	const struct insn* last = &t->ins[t->n - 1];
	uint32_t op = last->op;
	uint32_t target;
	if ((op >> 11) == 0x1C) {
		target = last->a + 4 + ((int32_t) ((op & 0x7FF) << 21) >> 20);
	} else if ((op >> 12) == 0xD && ((op >> 8) & 0xF) < 0xE) {
		target = last->a + 4 + ((int32_t) (int8_t) op << 1);
	} else {
		return;
	}
	if (target != t->ins[0].a || !t->thumb) {
		return;
	}
	uint16_t written = 0;
	for (int i = 0; i < t->n - 1; ++i) {
		written |= thumb_writes(t->ins[i].op);
	}
	for (int i = 0; i < t->n - 1; ++i) {
		const struct insn* in = &t->ins[i];
		op = in->op;
		if (in->kind == K_ALU && (op >> 11) != 0x1E) {
			continue;
		}
		if (in->kind != K_CALL || b->nloads == 4) {
			return;
		}
		int base, offset = -1;
		int32_t imm = 0;
		switch (op >> 11) {
		case 0x09: base = -1; imm = ((in->a + 4) & ~3u) + ((op & 0xFF) << 2); break;
		case 0x0A: case 0x0B:
			if (((op >> 9) & 7) < 3) {
				return; // a store
			}
			base = (op >> 3) & 7;
			offset = (op >> 6) & 7;
			break;
		case 0x0D: base = (op >> 3) & 7; imm = ((op >> 6) & 0x1F) << 2; break;
		case 0x0F: base = (op >> 3) & 7; imm = (op >> 6) & 0x1F; break;
		case 0x11: base = (op >> 3) & 7; imm = ((op >> 6) & 0x1F) << 1; break;
		case 0x13: base = ARM_SP; imm = (op & 0xFF) << 2; break;
		default: return; // stores, LDM/POP...
		}
		if ((base >= 0 && (written & (1 << base))) || (offset >= 0 && (written & (1 << offset)))) {
			return;
		}
		b->loads[b->nloads].base = base;
		b->loads[b->nloads].offset = offset;
		b->loads[b->nloads].imm = imm;
		++b->nloads;
	}
	t->idle = true;
}

static void emit_check(struct tctx* t, const struct insn* in, int index) {
	flush_cycles(t);
	// An entry point: r30 is exact here and every flag is live, so the dispatcher may start the
	// block here (jit_run_loop, enclosing_entry) with the state the interpreter left.
	if (t->entries_on && index > 0 && !t->lr_known && !t->mem) {
		t->entries[t->nentries].at = t->j->cur;
		t->entries[t->nentries].index = index;
		t->entries[t->nentries++].c_inv = t->c_inv;
	}
	E(P_LWZ(0, OFF_NEXT, RCPU));
	if (in->seg_cycles) {
		E(P_ADDI(12, RCYC, in->seg_cycles));
		E(P_CMPW(1, 12, 0));
	} else {
		E(P_CMPW(1, RCYC, 0));
	}
	stub_bc(t, BO_FALSE_UNLIKELY, CR_LT(1), in->a, EXIT_INTERP, index);
}

struct jit_block* jit_translate(struct jit* j, uint32_t pc, bool thumb) {
	if (!translatable(j, pc)) {
		return NULL;
	}
	struct jit_block* b = jit_new_block(j);
	if (!b) {
		j->overflow = true;
		return NULL;
	}
	// Per core (jit_create): each player translates on its own thread (a shared static
	// corrupted both).
	struct tctx* t = j->tctx;
	memset(t, 0, offsetof(struct tctx, ins));
	t->nstubs = 0; // after ins[], not covered by the memset
	t->j = j;
	t->cpu = j->cpu;
	t->gba = j->gba;
	t->thumb = thumb;
	t->rom = (pc >> 24) >= GBA_REGION_ROM0;
	t->S = thumb ? j->gba->memory.waitstatesSeq16[pc >> 24] : j->gba->memory.waitstatesSeq32[pc >> 24];
	t->N = thumb ? j->gba->memory.waitstatesNonseq16[pc >> 24] : j->gba->memory.waitstatesNonseq32[pc >> 24];
	t->pf = 1 + t->S;
	t->fast_branches = j->gba->idleOptimization < IDLE_LOOP_DETECT; // DETECT watches every branch
	t->entries_on = jit_ram_page(pc) >= 0;
	uint32_t w = thumb ? WORD_SIZE_THUMB : WORD_SIZE_ARM;

	uint32_t a = pc;
	for (;;) {
		struct insn* in = &t->ins[t->n++];
		in->a = a;
		in->op = jit_fetch(j->cpu, a, thumb);
		a += w;
		if (thumb) {
			thumb_decode(t, in);
			if (thumb_ends_block(in)) {
				break;
			}
		} else {
			arm_decode(t, in);
			if (in->kind == K_BRANCH || (in->kind == K_FALLBACK && arm_ends_block(in->op))) {
				break;
			}
		}
		if (t->n >= MAX_INSTR || !translatable(j, a) || (!t->rom && !(a & 0xFF))) {
			break; // RAM blocks stay within one 256-byte page (see invalidation)
		}
	}
	analyze(t);
	find_idle_loop(t, b);

	b->code = j->cur;
	b->start = pc;
	b->thumb = thumb;
	b->links_in = -1;
	if (j->profile) {
		jit_la(j, 12, (uintptr_t) &b->runs);
		jit_emit(j, P_LWZ(11, 0, 12));
		jit_emit(j, P_ADDI(11, 11, 1));
		jit_emit(j, P_STW(11, 0, 12));
	}
	if (t->mem) {
		jit_bl(j, j->sync_out);
	}
	bool ended = false;
	for (int i = 0; i < t->n; ++i) {
		struct insn* in = &t->ins[i];
		if (in->check) {
			emit_check(t, in, i);
		}
		if (thumb) {
			emit_thumb(t, in, i);
		} else {
			emit_arm(t, in, i);
		}
		ended = in->kind == K_BRANCH;
	}
	if (!ended) {
		if (t->mem) {
			emit_exit(j, a, EXIT_NEXT, t->n, true);
		} else {
			emit_chain_exit(t, a, t->n, EXIT_CHAIN);
		}
	}

	for (int i = 0; i < t->nstubs; ++i) {
		struct stub* s = &t->stubs[i];
		*s->at |= ((char*) j->cur - (char*) s->at) & 0xFFFC;
		if (s->pending) {
			jit_emit(j, P_ADDI(RCYC, RCYC, s->pending));
		}
		if (s->c_inv) {
			jit_emit(j, P_CRNOT(CR_C, CR_C));
		}
		emit_exit(j, s->pc, s->reason, s->count, s->mem);
	}
	emit_lsm_slow(t);
	// Entry points: (instruction index << 24 | word offset from b->code). Where the block holds
	// C inverted, a stub inverts the canonical C that enter loads, then joins the code.
	for (int i = 0; i < t->nentries; ++i) {
		if (t->entries[i].c_inv) {
			uint32_t* at = t->entries[i].at;
			t->entries[i].at = j->cur;
			jit_emit(j, P_CRNOT(CR_C, CR_C));
			jit_b(j, at);
		}
	}
	b->entries = j->cur;
	b->nentries = t->nentries;
	for (int i = 0; i < t->nentries; ++i) {
		jit_emit(j, (uint32_t) t->entries[i].index << 24 | (uint32_t) (t->entries[i].at - b->code));
	}
	if (j->overflow) {
		j->cur = b->code;
		return NULL;
	}
	b->end = a;
	b->ninstr = t->n;
	for (int i = 0; i < t->n; ++i) {
		b->nfallback += t->ins[i].kind == K_FALLBACK;
		b->ncall += t->ins[i].kind == K_CALL;
	}
	jit_commit_block(j, b);
	return b;
}

size_t jit_tctx_size(void) {
	return sizeof(struct tctx);
}

// Chains an exit (emit_chain_exit's first word) straight to the target block.
void jit_link(struct jit* j, uint32_t* site, struct jit_block* target) {
	intptr_t disp = (char*) target->code - (char*) site;
	if (disp < -0x2000000 || disp >= 0x2000000) {
		return;
	}
	int32_t i = j->free_links;
	if (i >= 0) {
		j->free_links = j->links[i].next;
	} else if (j->nlinks < MAX_LINKS) {
		i = j->nlinks++;
	} else {
		return;
	}
	struct jit_link* l = &j->links[i];
	l->site = site;
	l->original = *site;
	l->next = target->links_in;
	target->links_in = i;
	*site = P_B(disp);
	jit_flush_icache(site, site + 1);
}

void jit_unlink_incoming(struct jit* j, struct jit_block* b) {
	for (int32_t i = b->links_in, next; i >= 0; i = next) {
		next = j->links[i].next;
		*j->links[i].site = j->links[i].original;
		jit_flush_icache(j->links[i].site, j->links[i].site + 1);
		j->links[i].next = j->free_links; // recycled: revived blocks get linked again and again
		j->free_links = i;
	}
	b->links_in = -1;
}
