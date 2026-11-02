// Memory access routines called by translated code (one set per core, in its code cache slice).
// IWRAM, EWRAM (and ROM for loads) are accessed inline, everything else through mGBA, with
// exactly mGBA's cycles. Design: docs/jit.md ("Memory").
//
// Call: r4 = GBA address, r5 = the instruction's PC as the interpreter sees it (+4 Thumb, +8 ARM),
// r6 = value (stores). Returns loads in r3. Adds the access's wait to r30 (cycles); the caller
// adds the instruction's own cycles. Clobbers r0, r3-r12, cr0, cr1, CTR, XER (never cr2-cr4).
#include <stddef.h>

#include <mgba/internal/gba/gba.h>
#include <mgba/internal/gba/memory.h>

#include "jit_internal.h"

#define E(insn) jit_emit(j, insn)
#define OFF(field) ((int) offsetof(struct jit, field))
#define OFF_SEQ16 ((int) offsetof(struct ARMCore, memory.activeSeqCycles16))
#define OFF_NONSEQ16 ((int) offsetof(struct ARMCore, memory.activeNonseqCycles16))
#define OFF_ACTIVE_REGION ((int) offsetof(struct ARMCore, memory.activeRegion))
#define OFF_ACTIVE_MASK ((int) offsetof(struct ARMCore, memory.activeMask))
#define OFF_EXEC_MODE ((int) offsetof(struct ARMCore, executionMode))

_Static_assert(offsetof(struct jit, slog) + sizeof(((struct jit*) 0)->slog) < 32768, "RJIT offsets must fit 16 bits");

static void patch_bc(uint32_t* at, const uint32_t* target) {
	*at |= ((const char*) target - (const char*) at) & 0xFFFC;
}

// mGBA's GBAMemoryStall (memory.c), for code running from ROM with the prefetch buffer on:
// r0 = wait in, r5 = PC; adds the stalled wait to r30 and returns to the translated code.
// Clobbers r7-r12.
static void emit_stall(struct jit* j) {
	j->stall = j->cur;
	E(P_LD(11, OFF(lpp), RJIT));
	E(P_LWZ(7, 0, 11));                    // lastPrefetchedPc
	E(P_SUBF(7, 5, 7));                    // dist = lastPrefetchedPc - PC
	E(P_LI(8, 0));                         // previousLoads
	E(P_LI(9, 8));                         // maxLoads
	E(P_CMPLWI(1, 7, 16));
	uint32_t* far = j->cur;
	E(P_BC(BO_FALSE, CR_LT(1), 0));
	E(P_SRWI(8, 7, 1));                    // previousLoads = dist >> 1
	E(P_SUBF(9, 8, 9));                    // maxLoads -= previousLoads
	patch_bc(far, j->cur);
	E(P_LWZ(10, OFF_SEQ16, RCPU));         // s
	E(P_ADDI(12, 10, 1));                  // stall = s + 1
	E(P_LI(7, 1));                         // loads = 1
	uint32_t* loop = j->cur;               // while (stall < wait && loads < maxLoads)
	E(P_CMPW(1, 12, 0));
	uint32_t* done1 = j->cur;
	E(P_BC(BO_FALSE, CR_LT(1), 0));
	E(P_CMPW(1, 7, 9));
	uint32_t* done2 = j->cur;
	E(P_BC(BO_FALSE, CR_LT(1), 0));
	E(P_ADD(12, 12, 10));                  // stall += s
	E(P_ADDI(7, 7, 1));                    // ++loads
	E(P_B((const char*) loop - (const char*) j->cur));
	patch_bc(done1, j->cur);
	patch_bc(done2, j->cur);
	E(P_ADD(7, 7, 8));                     // lastPrefetchedPc = PC + 2 * (loads + previousLoads - 1)
	E(P_ADDI(7, 7, -1));
	E(P_SLWI(7, 7, 1));
	E(P_ADD(7, 5, 7));
	E(P_STW(7, 0, 11));
	E(P_CMPW(1, 12, 0));                   // if (stall > wait) wait = stall
	uint32_t* keep = j->cur;
	E(P_BC(BO_FALSE, CR_GT(1), 0));
	E(P_MR(0, 12));
	patch_bc(keep, j->cur);
	E(P_LWZ(9, OFF_NONSEQ16, RCPU));       // wait -= N - s; wait -= stall
	E(P_SUBF(9, 10, 9));
	E(P_SUBF(0, 9, 0));
	E(P_SUBF(0, 12, 0));
	E(P_ADD(RCYC, RCYC, 0));
	E(P_BLR);
}

// Self-check builds: append (address of r4 aligned to `size`, size or the register `size_reg`
// if >= 0) to the JIT core's store log, like after_store does for stores through mGBA.
// Clobbers r0, r7, r8, cr1.
static void emit_log_store(struct jit* j, int size, int size_reg) {
	E(P_LWZ(8, OFF(slog_n), RJIT));
	E(P_CMPLWI(1, 8, JIT_LOG_MAX));
	uint32_t* full = j->cur;
	E(P_BC(BO_FALSE, CR_LT(1), 0));
	E(P_SLWI(7, 8, 3));
	E(P_ADDI(7, 7, OFF(slog)));
	E(P_RLWINM(0, 4, 0, 0, 31 - (size == 4 ? 2 : size == 2 ? 1 : 0)));
	E(P_STWX(0, RJIT, 7));
	E(P_ADDI(7, 7, 4));
	if (size_reg < 0) {
		E(P_LI(0, size));
		size_reg = 0;
	}
	E(P_STWX(size_reg, RJIT, 7));
	patch_bc(full, j->cur);
	E(P_ADDI(8, 8, 1)); // past JIT_LOG_MAX: counted, marks the overflow
	E(P_STW(8, OFF(slog_n), RJIT));
}

// The wait (in r0, or the byte at RJIT + `table`) of a RAM access, through the stall routine
// when the code runs from ROM with prefetch, then return.
static void emit_ram_tail(struct jit* j, int table, int constant, bool stall) {
	if (table >= 0) {
		E(P_LBZ(0, table, RJIT));
	} else {
		E(P_LI(0, constant));
	}
	if (stall) {
		jit_b(j, j->stall);
	} else {
		E(P_ADD(RCYC, RCYC, 0));
		E(P_BLR);
	}
}

// The two opcodes after the running instruction (r5 = its PC): at PC - w and PC.
static void emit_prefetch_pair(struct jit* j, int w) {
	E(P_ADDI(9, 5, -w));
	E(P_AND(9, 9, 11));
	E(w == 2 ? P_LHBRX(0, 10, 9) : P_LWBRX(0, 10, 9));
	E(P_STW(0, OFF_PREF, RCPU));
	E(P_AND(9, 5, 11));
	E(w == 2 ? P_LHBRX(0, 10, 9) : P_LWBRX(0, 10, 9));
	E(P_STW(0, OFF_PREF + 4, RCPU));
}

// A load from an unmapped region returns "open bus": mGBA's GBALoadBad reads cpu->prefetch[],
// which the interpreter keeps current and translated code doesn't. Set it for loads from
// regions 0, 1 and above 15 (found by the self-check: a game loading through a garbage pointer).
static void emit_open_bus_prefetch(struct jit* j) {
	E(P_RLWINM(0, 4, 8, 24, 31));
	E(P_CMPLWI(1, 0, GBA_REGION_BIOS + 1));
	uint32_t* fix = j->cur;
	E(P_BC(BO_FALSE, CR_GT(1), 0)); // regions 0, 1
	E(P_CMPLWI(1, 0, 0x0F));
	uint32_t* skip = j->cur;
	E(P_BC(BO_FALSE, CR_GT(1), 0)); // 2-15: mapped
	patch_bc(fix, j->cur);
	E(P_LD(10, OFF_ACTIVE_REGION, RCPU));
	E(P_LWZ(11, OFF_ACTIVE_MASK, RCPU));
	E(P_LWZ(0, OFF_EXEC_MODE, RCPU));
	E(P_CMPWI(1, 0, MODE_THUMB));
	uint32_t* arm = j->cur;
	E(P_BC(BO_FALSE, CR_EQ(1), 0));
	emit_prefetch_pair(j, WORD_SIZE_THUMB);
	uint32_t* done = j->cur;
	E(P_B(0));
	patch_bc(arm, j->cur);
	emit_prefetch_pair(j, WORD_SIZE_ARM);
	*done = P_B((char*) j->cur - (char*) done);
	patch_bc(skip, j->cur);
}

static void emit_routine(struct jit* j, int kind, bool stall) {
	bool store = kind >= JIT_ST32;
	int size = kind == JIT_LD32 || kind == JIT_ST32 ? 4 : kind == JIT_LD16 || kind == JIT_ST16 ? 2 : 1;
	j->mem[stall][kind] = j->cur;
	uint32_t* slow[8];
	int nslow = 0;

	E(P_RLWINM(12, 4, 8, 24, 31)); // region
	E(P_CMPLWI(1, 12, GBA_REGION_IWRAM));
	uint32_t* to_iwram = j->cur;
	E(P_BC(BO_TRUE, CR_EQ(1), 0));
	E(P_CMPLWI(1, 12, GBA_REGION_EWRAM));
	uint32_t* to_ewram = j->cur;
	E(P_BC(BO_TRUE, CR_EQ(1), 0));
	if (!store) {
		// ROM: regions 8-12 (not 13: EEPROM), within the ROM, past the header (GPIO), aligned.
		E(P_ADDI(11, 12, -GBA_REGION_ROM0));
		E(P_CMPLWI(1, 11, GBA_REGION_ROM2 - GBA_REGION_ROM0));
		slow[nslow++] = j->cur;
		E(P_BC(BO_TRUE, CR_GT(1), 0));
		E(P_RLWINM(12, 4, 0, 7, 31));
		E(P_LWZ(0, OFF(rom_size), RJIT));
		E(P_CMPLW(1, 12, 0));
		slow[nslow++] = j->cur;
		E(P_BC(BO_FALSE, CR_LT(1), 0));
		E(P_CMPLWI(1, 12, 0x100));
		slow[nslow++] = j->cur;
		E(P_BC(BO_TRUE, CR_LT(1), 0));
		if (size > 1) {
			E(P_ANDI_(0, 4, size - 1));
			slow[nslow++] = j->cur;
			E(P_BC(BO_FALSE, CR_EQ(0), 0));
		}
		E(P_LD(10, OFF(rom), RJIT));
		E(size == 4 ? P_LWBRX(3, 10, 12) : size == 2 ? P_LHBRX(3, 10, 12) : P_LBZX(3, 10, 12));
		E(P_ADDI(11, 11, size == 4 ? OFF(rom_wait32) : OFF(rom_wait16)));
		E(P_LBZX(0, RJIT, 11));
		E(P_ADD(RCYC, RCYC, 0)); // ROM: no prefetch stall
		E(P_BLR);
	} else {
		slow[nslow++] = j->cur;
		E(P_B(0));
	}

	for (int region = GBA_REGION_EWRAM; region <= GBA_REGION_IWRAM; ++region) {
		bool iwram = region == GBA_REGION_IWRAM;
		patch_bc(iwram ? to_iwram : to_ewram, j->cur);
		int top = iwram ? 17 : 14; // offset = address & (size of the region - 1), aligned
		if (!store && size > 1) {
			E(P_ANDI_(0, 4, size - 1)); // unaligned loads rotate: mGBA's job
			uint32_t* unaligned = j->cur;
			E(P_BC(BO_FALSE, CR_EQ(0), 0));
			slow[nslow++] = unaligned; // patched below with the others
		}
		E(P_RLWINM(12, 4, 0, top, 31 - (size == 4 ? 2 : size == 2 ? 1 : 0)));
		if (store) {
			// Translated code in that halfword (or word)? mGBA's wrapper invalidates it.
			E(P_SRWI(11, 12, 4));
			E(P_ADDI(11, 11, iwram ? OFF(iwram_bits) : OFF(ewram_bits)));
			E(P_LBZX(0, RJIT, 11));
			E(P_RLWINM(11, 12, 31, 29, 31));
			E(P_SRW(0, 0, 11));
			E(P_ANDI_(0, 0, size == 4 ? 3 : 1));
			uint32_t* code = j->cur;
			E(P_BC(BO_FALSE, CR_EQ(0), 0));
			slow[nslow++] = code;
		}
		E(P_LD(10, iwram ? OFF(iwram) : OFF(ewram), RJIT));
		if (store) {
			E(size == 4 ? P_STWBRX(6, 10, 12) : size == 2 ? P_STHBRX(6, 10, 12) : P_STBX(6, 10, 12));
			if (j->check) {
				emit_log_store(j, size, -1);
			}
		} else {
			E(size == 4 ? P_LWBRX(3, 10, 12) : size == 2 ? P_LHBRX(3, 10, 12) : P_LBZX(3, 10, 12));
		}
		// Waits: IWRAM 0, EWRAM its non-sequential waitstate; +2 for a load, +1 for a store.
		if (iwram) {
			emit_ram_tail(j, -1, store ? 1 : 2, stall);
		} else {
			int table = store ? (size == 4 ? OFF(ew_wait_st32) : OFF(ew_wait_st16))
			                  : (size == 4 ? OFF(ew_wait_ld32) : OFF(ew_wait_ld16));
			emit_ram_tail(j, table, 0, stall);
		}
	}

	// Anything else: mGBA (or our store wrapper), with the interpreter's PC and cycles.
	for (int i = 0; i < nslow; ++i) {
		uint32_t insn = *slow[i];
		if ((insn >> 26) == 18) {
			*slow[i] = P_B((char*) j->cur - (char*) slow[i]);
		} else {
			patch_bc(slow[i], j->cur);
		}
	}
	if (j->profile) { // count it, by region
		E(P_RLWINM(11, 4, 10, 26, 29));
		jit_la(j, 12, (uintptr_t) j->prof_mem_slow[store]);
		E(P_LWZX(10, 12, 11));
		E(P_ADDI(10, 10, 1));
		E(P_STWX(10, 12, 11));
	}
	E(P_MFLR(7));
	E(P_STD(7, FRAME_SCRATCH_LR, 1));
	E(P_STW(5, OFF_PC, RCPU));
	E(P_STW(RCYC, OFF_CYC, RCPU));
	if (!store) {
		emit_open_bus_prefetch(j);
	}
	E(P_MR(3, RCPU));
	if (store) {
		E(P_MR(5, 6));
		E(P_ADDI(6, RCPU, OFF_CYC));
		jit_bl(j, size == 4 ? j->st32_stub : size == 2 ? j->st16_stub : j->st8_stub);
	} else {
		E(P_ADDI(5, RCPU, OFF_CYC));
		jit_call(j, size == 4 ? (const void*) GBALoad32 : size == 2 ? (const void*) GBALoad16 : (const void*) GBALoad8);
	}
	E(P_LWZ(RCYC, OFF_CYC, RCPU));
	E(P_LD(7, FRAME_SCRATCH_LR, 1));
	E(P_MTLR(7));
	E(P_BLR);
}

// LDM/STM/PUSH/POP whose words are all in IWRAM or EWRAM, inline (translate.c, emit_lsm).
// Kind 0 load, 1 store, 2 load with PC in the list. Call: r4 = address of the lowest word, r6 =
// number of words (1-16), r5 = PC (stall variant, kinds 0 and 1).
// Inline possible: cr5.eq clear, r3 = host address of the lowest word, and mGBA's wait added to
// r30 (GBALoadMultiple/GBAStoreMultiple: S - N + n * (1 + S), +1 for a load, through the stall).
// Else cr5.eq set, nothing done: the caller goes through mGBA. That is when the words wrap around
// the region (mGBA masks each address), and for stores when one of them holds translated code
// (mGBA's wrapper invalidates it). Clobbers r0, r3, r7-r12, cr0, cr1.
static void emit_lsm_routine(struct jit* j, int kind, bool stall) {
	bool store = kind == 1;
	j->lsm[stall][kind] = j->cur;
	uint32_t* slow[8];
	int nslow = 0;
	E(P_RLWINM(12, 4, 8, 24, 31)); // region
	E(P_CMPLWI(1, 12, GBA_REGION_IWRAM));
	uint32_t* to_iwram = j->cur;
	E(P_BC(BO_TRUE, CR_EQ(1), 0));
	E(P_CMPLWI(1, 12, GBA_REGION_EWRAM));
	slow[nslow++] = j->cur;
	E(P_BC(BO_FALSE, CR_EQ(1), 0));
	for (int region = GBA_REGION_EWRAM; region <= GBA_REGION_IWRAM; ++region) {
		bool iwram = region == GBA_REGION_IWRAM;
		if (iwram) {
			patch_bc(to_iwram, j->cur);
		}
		E(P_RLWINM(11, 4, 0, iwram ? 17 : 14, 29)); // offset of the lowest word (mGBA aligns it)
		E(P_SLWI(10, 6, 2));                        // 4n
		E(P_ADD(0, 11, 10));
		if (iwram) {
			E(P_CMPLWI(1, 0, GBA_SIZE_IWRAM));
		} else {
			E(P_LIS(9, GBA_SIZE_EWRAM >> 16));
			E(P_CMPLW(1, 0, 9));
		}
		slow[nslow++] = j->cur;
		E(P_BC(BO_TRUE, CR_GT(1), 0)); // wraps around the region
		if (store) {
			// The 2n code bits from the halfword at offset/2: 64 bits read from the aligned word
			// holding the first one (lwbrx: bit k of the value is halfword k of that word).
			E(P_RLWINM(12, 11, 28, 4, 29));
			E(P_ADDI(12, 12, iwram ? OFF(iwram_bits) : OFF(ewram_bits)));
			E(P_LWBRX(9, RJIT, 12));
			E(P_ADDI(12, 12, 4));
			E(P_LWBRX(0, RJIT, 12));
			E(P_RLDIMI(9, 0, 32, 0));
			E(P_RLWINM(0, 11, 31, 27, 31));
			E(P_SRD(9, 9, 0));
			E(P_SRWI(12, 10, 1));
			E(P_SUBFIC(12, 12, 64));
			E(P_SLD_(9, 9, 12)); // only the 2n bits are left
			slow[nslow++] = j->cur;
			E(P_BC(BO_FALSE, CR_EQ(0), 0));
			if (j->check) {
				emit_log_store(j, 4, 10); // like jit_stm_c: the lowest word, 4n bytes
			}
		}
		E(P_LD(3, iwram ? OFF(iwram) : OFF(ewram), RJIT));
		E(P_ADD(3, 3, 11));
		if (iwram) {
			E(P_ADDI(0, 6, store ? 0 : 1)); // IWRAM: no waitstates
		} else {
			E(P_SLWI(12, 6, 1));
			E(P_ADDI(12, 12, OFF(ew_lsm_wait) + store * (int) sizeof(j->ew_lsm_wait[0])));
			E(P_LHZX(0, RJIT, 12));
		}
		E(P_CRXOR(CR_EQ(5), CR_EQ(5), CR_EQ(5)));
		if (stall && kind == 2) {
			// GBALoadMultiple sets gprs[15] before GBAMemoryStall reads it: the loaded PC (the last
			// word), not the instruction's (found by the self-check: POP {pc} 2 cycles off).
			E(P_SLWI(12, 6, 2));
			E(P_ADDI(12, 12, -4));
			E(P_LWBRX(5, 3, 12));
		}
		if (stall) {
			jit_b(j, j->stall); // GBAMemoryStall: the final address is below ROM
		} else {
			E(P_ADD(RCYC, RCYC, 0));
			E(P_BLR);
		}
	}
	for (int i = 0; i < nslow; ++i) {
		patch_bc(slow[i], j->cur);
	}
	E(P_CREQV(CR_EQ(5), CR_EQ(5), CR_EQ(5)));
	E(P_BLR);
}

// LDM/STM through mGBA (GBALoadMultiple, the STM wrapper): r4 = base, r5 = list, r6 = direction,
// r7 = PC as the interpreter sees it. Every GBA register goes through cpu->gprs (mGBA reads and
// writes the listed ones; one shared copy instead of syncing the list at each call site), the
// cycles through cpu->cycles. Returns the final address in r3, a loaded PC in gprs[15].
static void emit_lsm_mgba(struct jit* j, bool store) {
	j->lsm_mgba[store] = j->cur;
	if (j->profile) { // count it, by the base's region
		E(P_RLWINM(11, 4, 10, 26, 29));
		jit_la(j, 12, (uintptr_t) j->prof_lsm_slow[store]);
		E(P_LWZX(10, 12, 11));
		E(P_ADDI(10, 10, 1));
		E(P_STWX(10, 12, 11));
	}
	E(P_MFLR(0));
	E(P_STD(0, FRAME_SCRATCH_LR, 1));
	E(P_STW(7, OFF_PC, RCPU));
	E(P_STW(RCYC, OFF_CYC, RCPU));
	for (int r = 0; r < 15; ++r) {
		E(P_STW(RG(r), OFF_GPRS + 4 * r, RCPU));
	}
	E(P_MR(3, RCPU));
	E(P_ADDI(7, RCPU, OFF_CYC));
	if (store) {
		jit_bl(j, j->stm_stub);
	} else {
		jit_call(j, GBALoadMultiple);
		for (int r = 0; r < 15; ++r) {
			E(P_LWZ(RG(r), OFF_GPRS + 4 * r, RCPU));
		}
	}
	E(P_LWZ(RCYC, OFF_CYC, RCPU));
	E(P_LD(0, FRAME_SCRATCH_LR, 1));
	E(P_MTLR(0));
	E(P_BLR);
}

void jit_emit_memory_routines(struct jit* j) {
	emit_stall(j);
	for (int stall = 0; stall < 2; ++stall) {
		for (int kind = 0; kind < JIT_MEM_KINDS; ++kind) {
			emit_routine(j, kind, stall);
		}
		for (int kind = 0; kind < (stall ? 3 : 2); ++kind) {
			emit_lsm_routine(j, kind, stall);
		}
	}
	j->lsm[0][2] = j->lsm[0][0]; // without the stall, PC makes no difference
	emit_lsm_mgba(j, false);
	emit_lsm_mgba(j, true);
}

// Values the routines read through RJIT; called whenever waitstates may have changed (flush).
void jit_memory_refresh(struct jit* j) {
	struct GBAMemory* m = &j->gba->memory;
	j->iwram = (uint8_t*) m->iwram;
	j->ewram = (uint8_t*) m->wram;
	j->rom = (uint8_t*) m->rom;
	j->rom_size = m->romSize;
	j->lpp = &m->lastPrefetchedPc;
	for (int i = 0; i <= GBA_REGION_ROM2 - GBA_REGION_ROM0; ++i) {
		j->rom_wait32[i] = m->waitstatesNonseq32[GBA_REGION_ROM0 + i] + 2;
		j->rom_wait16[i] = m->waitstatesNonseq16[GBA_REGION_ROM0 + i] + 2;
	}
	j->ew_wait_ld32 = m->waitstatesNonseq32[GBA_REGION_EWRAM] + 2;
	j->ew_wait_ld16 = m->waitstatesNonseq16[GBA_REGION_EWRAM] + 2;
	j->ew_wait_st32 = m->waitstatesNonseq32[GBA_REGION_EWRAM] + 1;
	j->ew_wait_st16 = m->waitstatesNonseq16[GBA_REGION_EWRAM] + 1;
	int s = m->waitstatesSeq32[GBA_REGION_EWRAM], n = m->waitstatesNonseq32[GBA_REGION_EWRAM];
	for (int words = 1; words <= 16; ++words) {
		j->ew_lsm_wait[0][words] = s - n + words * (1 + s) + 1; // +1: the load's internal cycle
		j->ew_lsm_wait[1][words] = s - n + words * (1 + s);
	}
}
