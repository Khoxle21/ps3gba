// ARM/Thumb -> PowerPC JIT for mGBA's GBA core: code cache, dispatcher, invalidation.
// Design and invariants: docs/jit.md. Translation: translate.c.
#include <limits.h>
#include <malloc.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <mgba/core/core.h>
#include <mgba/core/timing.h>
#include <mgba/internal/arm/isa-arm.h>
#include <mgba/internal/arm/isa-inlines.h>
#include <mgba/internal/arm/isa-thumb.h>
#include <mgba/internal/gba/gba.h>
#include <mgba/internal/gba/io.h>
#include <mgba/internal/gba/memory.h>

#include "emu.h"
#include "jit_internal.h"

// The code cache: zeros reserved inside .text, the only memory a PS3 app can both write and
// execute (tests/jitprobe). One slice per player. Size: make JIT_CACHE_MB=n (default 16).
#ifndef JIT_CACHE_MB
#define JIT_CACHE_MB 16
#endif
#define CACHE_BYTES ((uint32_t) JIT_CACHE_MB << 20)
#define SLOTS 4
#define STR_(x) #x
#define STR(x) STR_(x)
__asm__(".section .text\n.balign 65536\n.globl jit_cache\njit_cache:\n.space " STR(JIT_CACHE_MB) " * 1048576\n.previous\n");
extern uint32_t jit_cache[];

// Waitstate tables are baked into translations: any change flushes the cache.
static void ws_snapshot(const struct GBA* gba, uint8_t* out) {
	const struct GBAMemory* m = &gba->memory;
	memcpy(out, m->waitstatesSeq32, 16);
	memcpy(out + 16, m->waitstatesSeq16, 16);
	memcpy(out + 32, m->waitstatesNonseq32, 16);
	memcpy(out + 48, m->waitstatesNonseq16, 16);
	out[64] = m->prefetch;
}

void jit_flush_icache(const void* start, const void* end) {
	uintptr_t a;
	for (a = (uintptr_t) start & ~127UL; a < (uintptr_t) end; a += 128) {
		__asm__ volatile("dcbst 0,%0" ::"r"(a) : "memory");
	}
	__asm__ volatile("sync" ::: "memory");
	for (a = (uintptr_t) start & ~127UL; a < (uintptr_t) end; a += 128) {
		__asm__ volatile("icbi 0,%0" ::"r"(a) : "memory");
	}
	__asm__ volatile("isync" ::: "memory");
}

// ---- Block map ----

static uint32_t map_hash(uint32_t key) {
	return (key >> 1) * 2654435761u >> (32 - MAP_BITS);
}

struct jit_block* jit_lookup(struct jit* j, uint32_t key) {
	for (uint32_t i = map_hash(key);; i = (i + 1) & (MAP_SIZE - 1)) {
		struct map_entry* e = &j->map[i];
		if (e->key == key) {
			return &j->blocks[e->block];
		}
		if (e->key == MAP_EMPTY) {
			return NULL;
		}
	}
}

// Dead blocks stay in the map (the newest translation of each address), so a write that puts
// their code back can revive them.
static void map_set(struct jit* j, uint32_t key, uint32_t block) {
	for (uint32_t i = map_hash(key);; i = (i + 1) & (MAP_SIZE - 1)) {
		struct map_entry* e = &j->map[i];
		if (e->key == key || e->key == MAP_EMPTY) {
			e->key = key;
			e->block = block;
			return;
		}
	}
}

// ---- RAM code tracking: a bit per halfword holding translated code, and per-256-byte-page
// lists of the blocks in that page (RAM blocks never cross a page) ----

static uint8_t* code_bits(struct jit* j, int region) {
	return region == GBA_REGION_IWRAM ? j->iwram_bits : j->ewram_bits;
}

int jit_ram_page(uint32_t address) {
	switch (address >> 24) {
	case GBA_REGION_IWRAM:
		return (address & (GBA_SIZE_IWRAM - 1)) >> 8;
	case GBA_REGION_EWRAM:
		return IWRAM_PAGES + ((address & (GBA_SIZE_EWRAM - 1)) >> 8);
	default:
		return -1;
	}
}

static void mark_code(struct jit* j, const struct jit_block* b) {
	uint8_t* bits = code_bits(j, b->start >> 24);
	uint32_t mask = (b->start >> 24) == GBA_REGION_IWRAM ? GBA_SIZE_IWRAM - 1 : GBA_SIZE_EWRAM - 1;
	for (uint32_t a = b->start; a < b->end; a += 2) {
		uint32_t h = (a & mask) >> 1;
		bits[h >> 3] |= 1 << (h & 7);
	}
}

static const uint8_t* ram_ptr(const struct jit* j, uint32_t address) {
	return (address >> 24) == GBA_REGION_IWRAM ? j->iwram + (address & (GBA_SIZE_IWRAM - 1))
	                                           : j->ewram + (address & (GBA_SIZE_EWRAM - 1));
}

static void add_to_page(struct jit* j, struct jit_block* b, int page) {
	b->page_next = j->page_head[page];
	j->page_head[page] = b - j->blocks;
	mark_code(j, b);
}

static void kill_block(struct jit* j, struct jit_block* b) {
	if (!b->alive) {
		return;
	}
	b->alive = false;
	jit_unlink_incoming(j, b);
	--j->stats.blocks;
	++j->stats.invalidated;
}

// A write hit translated code in [off, off + size) of a RAM page: kill the blocks it overlaps.
static void invalidate_page(struct jit* j, int page, uint32_t address, uint32_t size) {
	int32_t* link = &j->page_head[page];
	uint32_t lo = address, hi = address + size;
	while (*link >= 0) {
		struct jit_block* b = &j->blocks[*link];
		// Compare physical offsets: mirrors of the same RAM share the page.
		uint32_t mask = page < IWRAM_PAGES ? GBA_SIZE_IWRAM - 1 : GBA_SIZE_EWRAM - 1;
		uint32_t bs = b->start & mask, be = bs + (b->end - b->start), ws = lo & mask, we = ws + (hi - lo);
		if (b->alive && bs < we && ws < be) {
			kill_block(j, b);
		}
		if (!b->alive) {
			*link = b->page_next; // drop dead blocks from the list
		} else {
			link = &b->page_next;
		}
	}
	// Rebuild the page's code bits from the blocks left.
	int region = page < IWRAM_PAGES ? GBA_REGION_IWRAM : GBA_REGION_EWRAM;
	int first = page < IWRAM_PAGES ? page : page - IWRAM_PAGES;
	memset(&code_bits(j, region)[first * 16], 0, 16);
	for (int32_t i = j->page_head[page]; i >= 0; i = j->blocks[i].page_next) {
		mark_code(j, &j->blocks[i]);
	}
	if (j->in_jit && !j->forced) {
		// The running block may be the one written: leave JIT code at the next instruction.
		// Lowering nextEvent makes the block's next event check exit; the dispatcher restores it.
		j->forced = true;
		j->saved_next = j->cpu->nextEvent;
		j->cpu->nextEvent = j->cpu->cycles;
	}
}

static void after_store(struct jit* j, uint32_t address, uint32_t size) {
	int region = address >> 24;
	if (region == GBA_REGION_IWRAM || region == GBA_REGION_EWRAM) {
		uint8_t* bits = code_bits(j, region);
		uint32_t mask = region == GBA_REGION_IWRAM ? GBA_SIZE_IWRAM - 1 : GBA_SIZE_EWRAM - 1;
		for (uint32_t a = address; a < address + size; a += 2) {
			uint32_t h = (a & mask) >> 1;
			if (bits[h >> 3] & (1 << (h & 7))) {
				// The rest of the write within this page at once (one walk and bit rebuild):
				// whatever overlaps it overlaps a written halfword holding code.
				uint32_t hi = address + size < (a | 0xFF) + 1 ? address + size : (a | 0xFF) + 1;
				invalidate_page(j, jit_ram_page(a), a & ~1u, hi - (a & ~1u));
				a = hi - 2;
			}
		}
	} else if (region == GBA_REGION_IO) {
		uint32_t reg = address & 0x00FFFFFC;
		if (reg == GBA_REG_WAITCNT || reg == 0x800) {
			uint8_t ws[sizeof(j->ws)];
			ws_snapshot(j->gba, ws);
			if (memcmp(ws, j->ws, sizeof(ws))) {
				j->flush_pending = true;
				if (j->in_jit && !j->forced) {
					j->forced = true;
					j->saved_next = j->cpu->nextEvent;
					j->cpu->nextEvent = j->cpu->cycles;
				}
			}
		}
	}
	if (j->check) {
		jit_check_log_store(j, address, size);
	}
}

// Store wrappers installed in cpu->memory. They get the jit as an extra argument from a small
// per-core stub (see make_store_stub), since mGBA's handlers have no context pointer.
// Not static: GCC dropped static functions only referenced through a void* (undefined .opd
// symbols at link time).
void jit_st8_c(struct ARMCore* cpu, uint32_t address, int8_t value, int* cc, struct jit* j);
void jit_st16_c(struct ARMCore* cpu, uint32_t address, int16_t value, int* cc, struct jit* j);
void jit_st32_c(struct ARMCore* cpu, uint32_t address, int32_t value, int* cc, struct jit* j);
uint32_t jit_stm_c(struct ARMCore* cpu, uint32_t base, int mask, enum LSMDirection dir, int* cc, struct jit* j);

void jit_st8_c(struct ARMCore* cpu, uint32_t address, int8_t value, int* cc, struct jit* j) {
	j->store8(cpu, address, value, cc);
	after_store(j, address, 1);
}

void jit_st16_c(struct ARMCore* cpu, uint32_t address, int16_t value, int* cc, struct jit* j) {
	j->store16(cpu, address, value, cc);
	after_store(j, address & ~1u, 2);
}

void jit_st32_c(struct ARMCore* cpu, uint32_t address, int32_t value, int* cc, struct jit* j) {
	j->store32(cpu, address, value, cc);
	after_store(j, address & ~3u, 4);
}

uint32_t jit_stm_c(struct ARMCore* cpu, uint32_t base, int mask, enum LSMDirection dir, int* cc, struct jit* j) {
	uint32_t r = j->storeMultiple(cpu, base, mask, dir, cc);
	// Exactly the words mGBA stored (GBAStoreMultiple): a word more would hit the RAM code that
	// Pokemon's flash library keeps right above a push, on every byte it saves.
	uint32_t n = __builtin_popcount(mask & 0xFFFF);
	uint32_t lo = (dir & LSM_D) ? base - 4 * n : base;
	if (!(dir & LSM_B) != !(dir & LSM_D)) { // IB, DA
		lo += 4;
	}
	after_store(j, lo & ~3u, n ? 4 * n : 4); // an empty list stores PC
	return r;
}

// ---- Code emission basics ----

void jit_emit(struct jit* j, uint32_t insn) {
	if (j->cur < j->end) {
		*j->cur++ = insn;
	} else {
		j->overflow = true;
	}
}

// 32-bit constant; the upper half of the 64-bit register is undefined (fine for 32-bit ops).
void jit_li32(struct jit* j, int reg, uint32_t v) {
	if ((int32_t) v == (int16_t) v) {
		jit_emit(j, P_LI(reg, v));
	} else {
		jit_emit(j, P_LIS(reg, v >> 16));
		if (v & 0xFFFF) {
			jit_emit(j, P_ORI(reg, reg, v & 0xFFFF));
		}
	}
}

// Zero-extended constant, for 64-bit addresses (all of ours are below 4 GB).
void jit_la(struct jit* j, int reg, uint64_t v) {
	if (v < 0x8000) {
		jit_emit(j, P_LI(reg, v));
	} else if (v < 0x80000000u) {
		jit_li32(j, reg, v);
	} else {
		jit_emit(j, P_LI(reg, 0));
		jit_emit(j, P_ORIS(reg, reg, v >> 16));
		jit_emit(j, P_ORI(reg, reg, v & 0xFFFF));
	}
}

static void branch_to(struct jit* j, const uint32_t* target, bool link) {
	intptr_t disp = (const char*) target - (const char*) j->cur;
	if (disp >= -0x2000000 && disp < 0x2000000) {
		jit_emit(j, link ? P_BL(disp) : P_B(disp));
	} else {
		jit_la(j, 12, (uintptr_t) target);
		jit_emit(j, P_MTCTR(12));
		jit_emit(j, link ? P_BCTRL : P_BCTR);
	}
}

void jit_bl(struct jit* j, const uint32_t* target) {
	branch_to(j, target, true);
}

void jit_b(struct jit* j, const uint32_t* target) {
	branch_to(j, target, false);
}

// C functions are called through their ELFv1 descriptor's entry; r2 (TOC) never changes.
void jit_call(struct jit* j, const void* fn) {
	branch_to(j, (const uint32_t*) (uintptr_t) *(const uint64_t*) fn, true);
}

// ---- Shared routines, emitted at the start of each slice ----

#define CR_SAVED 0x38 // cr2, cr3, cr4: the non-volatile fields, holding the GBA flags

// GBA flags live in CR bits: N = cr2.lt, Z = cr2.eq, C = cr3.lt, V = cr4.lt.
static void emit_flags_to_cpsr(struct jit* j) {
	jit_emit(j, P_MFCR(0));
	jit_emit(j, P_LWZ(12, OFF_CPSR, RCPU));
	jit_emit(j, P_RLWIMI(12, 0, 8, 0, 0));
	jit_emit(j, P_RLWIMI(12, 0, 9, 1, 1));
	jit_emit(j, P_RLWIMI(12, 0, 10, 2, 2));
	jit_emit(j, P_RLWIMI(12, 0, 13, 3, 3));
	jit_emit(j, P_STW(12, OFF_CPSR, RCPU));
}

static void emit_flags_from_cpsr(struct jit* j) {
	jit_emit(j, P_LWZ(0, OFF_CPSR, RCPU));
	jit_emit(j, P_RLWINM(12, 0, 24, 8, 8));
	jit_emit(j, P_RLWIMI(12, 0, 23, 10, 10));
	jit_emit(j, P_RLWIMI(12, 0, 22, 12, 12));
	jit_emit(j, P_RLWIMI(12, 0, 19, 16, 16));
	jit_emit(j, P_MTCRF(CR_SAVED, 12));
}

static uint32_t* make_store_stub(struct jit* j, const void* fn, int arg_reg) {
	uint32_t* stub = j->cur;
	jit_la(j, arg_reg, (uintptr_t) j);
	jit_b(j, (const uint32_t*) (uintptr_t) *(const uint64_t*) fn);
	return stub;
}

static void emit_shared(struct jit* j) {
	// sync_out: GBA state from PPC registers to the ARMCore. Clobbers r0, r12.
	j->sync_out = j->cur;
	for (int n = 0; n < 15; ++n) {
		jit_emit(j, P_STW(RG(n), OFF_GPRS + 4 * n, RCPU));
	}
	jit_emit(j, P_STW(RCYC, OFF_CYC, RCPU));
	emit_flags_to_cpsr(j);
	jit_emit(j, P_BLR);

	// sync_in: the reverse, plus nextEvent. Clobbers r0, r12.
	j->sync_in = j->cur;
	for (int n = 0; n < 15; ++n) {
		jit_emit(j, P_LWZ(RG(n), OFF_GPRS + 4 * n, RCPU));
	}
	jit_emit(j, P_LWZ(RCYC, OFF_CYC, RCPU));
	emit_flags_from_cpsr(j);
	jit_emit(j, P_BLR);

	// enter(r3 = cpu, r4 = code): an ABI frame saving r14-r31, LR and CR, then jump to code.
	j->enter = j->cur;
	jit_emit(j, P_MFLR(0));
	jit_emit(j, P_STD(0, 16, 1));
	jit_emit(j, P_MFCR(12));
	jit_emit(j, P_STW(12, 8, 1));
	jit_emit(j, P_STDU(1, -FRAME, 1));
	for (int r = 14; r < 32; ++r) {
		jit_emit(j, P_STD(r, 112 + 8 * (r - 14), 1));
	}
	jit_emit(j, P_MR(RCPU, 3));
	jit_la(j, RJIT, (uintptr_t) j);
	jit_bl(j, j->sync_in);
	jit_emit(j, P_MTCTR(4));
	jit_emit(j, P_BCTR);

	// exit_regs: state in PPC registers; exit_mem: state already in the ARMCore.
	// Both return r3 = next GBA pc, r4 = reason | instructions << 8 to enter's caller.
	// exit_link: from a patchable exit (emit_chain_exit), r5 = its address + 16.
	j->exit_link = j->cur;
	jit_emit(j, P_MFLR(5));
	j->exit_regs = j->cur;
	jit_bl(j, j->sync_out);
	j->exit_mem = j->cur;
	for (int r = 14; r < 32; ++r) {
		jit_emit(j, P_LD(r, 112 + 8 * (r - 14), 1));
	}
	jit_emit(j, P_ADDI(1, 1, FRAME));
	jit_emit(j, P_LWZ(12, 8, 1));
	jit_emit(j, P_MTCRF(CR_SAVED, 12));
	jit_emit(j, P_LD(0, 16, 1));
	jit_emit(j, P_MTLR(0));
	jit_emit(j, P_BLR);

	j->st8_stub = make_store_stub(j, jit_st8_c, 7);
	j->st16_stub = make_store_stub(j, jit_st16_c, 7);
	j->st32_stub = make_store_stub(j, jit_st32_c, 7);
	j->stm_stub = make_store_stub(j, jit_stm_c, 8);
	j->mem_start = j->cur;
	jit_emit_memory_routines(j);
	j->code_start = j->cur;
}

static void flush_all(struct jit* j) {
	j->cur = j->code_start;
	j->overflow = false;
	j->nblocks = 0;
	j->nlinks = 0;
	j->free_links = -1;
	memset(j->map, 0xFF, sizeof(j->map)); // MAP_EMPTY
	memset(j->page_head, 0xFF, sizeof(j->page_head));
	memset(j->iwram_bits, 0, sizeof(j->iwram_bits));
	memset(j->ewram_bits, 0, sizeof(j->ewram_bits));
	ws_snapshot(j->gba, j->ws);
	jit_memory_refresh(j);
	j->flush_pending = false;
	j->stats.blocks = 0;
	++j->stats.flushes;
}

// The memory routines again (self-check logging on or off), before any block exists.
void jit_reemit_memory(struct jit* j) {
	j->cur = j->mem_start;
	jit_emit_memory_routines(j);
	j->code_start = j->cur;
	jit_flush_icache(j->mem_start, j->cur);
	flush_all(j);
}

// Registers a translated block; returns NULL when the cache or block table is full.
struct jit_block* jit_new_block(struct jit* j) {
	if (j->nblocks >= MAX_BLOCKS || j->end - j->cur < MIN_FREE_WORDS) {
		return NULL;
	}
	struct jit_block* b = &j->blocks[j->nblocks];
	memset(b, 0, sizeof(*b));
	b->page_next = -1;
	return b;
}

void jit_commit_block(struct jit* j, struct jit_block* b) {
	uint32_t key = b->start | b->thumb;
	struct jit_block* old = jit_lookup(j, key); // dead (or none): live addresses aren't translated
	b->variant = old ? old - j->blocks : -1;
	++j->nblocks;
	b->alive = true;
	map_set(j, key, b - j->blocks);
	b->code_end = j->cur;
	jit_flush_icache(b->code, j->cur);
	int page = jit_ram_page(b->start);
	if (page >= 0) {
		// A copy of the GBA code, to revive the block if a write later puts this code back.
		uint32_t size = b->end - b->start, words = (size + 3) / 4;
		if (j->end - j->cur >= words) {
			memcpy(j->cur, ram_ptr(j, b->start), size);
			b->src = (const uint8_t*) j->cur;
			j->cur += words;
		}
		add_to_page(j, b, page);
	}
	++j->stats.blocks;
	++j->stats.translated;
}

// The live translation at `key`, reviving a dead one whose GBA code is in RAM again. Unbound's
// sound mixer rewrites its unrolled loop with one of three templates for each channel it mixes
// (~100 rewrites per second): each template's blocks get translated once instead of after every
// rewrite. A translation depends only on its code bytes (waitstate changes flush everything),
// so the revived block is exactly what translating again would give.
static struct jit_block* live_block(struct jit* j, uint32_t key) {
	struct jit_block* head = jit_lookup(j, key);
	if (!head || head->alive) {
		return head;
	}
	struct jit_block* prev = NULL;
	struct jit_block* b = head;
	for (int n = 0; n < MAX_VARIANTS; ++n) {
		if (b->src && !memcmp(b->src, ram_ptr(j, b->start), b->end - b->start)) {
			if (prev) { // the newest translation first
				prev->variant = b->variant;
				b->variant = head - j->blocks;
				map_set(j, key, b - j->blocks);
			}
			b->alive = true;
			add_to_page(j, b, jit_ram_page(b->start));
			++j->stats.blocks;
			++j->stats.revived;
			return b;
		}
		if (b->variant < 0) {
			break;
		}
		prev = b;
		b = &j->blocks[b->variant];
	}
	return NULL;
}

// ---- Dispatcher ----

// Resuming in sequence (after an event or a forced exit) at `pc` inside live RAM blocks: the
// nearest entry point at or after pc among them, or NULL. Translating a block from pc instead
// made a partial copy of the code around it, and Unbound's sound mixer rewrites the loop such
// copies sat in ~100 times per second: dozens of them were killed and revived at each rewrite.
static struct jit_block* enclosing_entry(struct jit* j, uint32_t pc, bool thumb, uint32_t* entry) {
	int page = jit_ram_page(pc);
	if (page < 0) {
		return NULL;
	}
	struct jit_block* best = NULL;
	uint32_t best_at = UINT32_MAX;
	for (int32_t i = j->page_head[page]; i >= 0; i = j->blocks[i].page_next) {
		struct jit_block* c = &j->blocks[i];
		if (!c->alive || c->thumb != thumb || c->start >= pc || pc >= c->end) {
			continue; // (exact addresses: a mirror's block shares the page)
		}
		for (int k = 0; k < c->nentries; ++k) {
			uint32_t at = c->start + ((c->entries[k] >> 24) << (thumb ? 1 : 2));
			if (at >= pc) {
				if (at < best_at) {
					best = c;
					best_at = at;
					*entry = c->entries[k];
				}
				break;
			}
		}
	}
	return best;
}

// mGBA's static ThumbStep/ARMStep (arm.c), for running instructions near events.
static const uint16_t condition_lut[16] = {
	0xF0F0, 0x0F0F, 0xCCCC, 0x3333, 0xFF00, 0x00FF, 0xAAAA, 0x5555,
	0x0C0C, 0xF3F3, 0xAA55, 0x55AA, 0x0A05, 0xF5FA, 0xFFFF, 0x0000,
};

// One ARM instruction already fetched, with mGBA's condition check (used by translations too).
void jit_arm_exec(struct ARMCore* cpu, uint32_t opcode) {
	unsigned condition = opcode >> 28;
	if (condition != 0xE) {
		unsigned flags = cpu->cpsr.flags >> 4;
		if (!(condition_lut[condition] & (1 << flags))) {
			cpu->cycles += ARM_PREFETCH_CYCLES;
			return;
		}
	}
	_armTable[((opcode >> 16) & 0xFF0) | ((opcode >> 4) & 0x00F)](cpu, opcode);
}

void jit_step(struct ARMCore* cpu) {
	if (cpu->executionMode == MODE_THUMB) {
		uint32_t opcode = cpu->prefetch[0];
		cpu->prefetch[0] = cpu->prefetch[1];
		cpu->gprs[ARM_PC] += WORD_SIZE_THUMB;
		LOAD_16(cpu->prefetch[1], cpu->gprs[ARM_PC] & cpu->memory.activeMask, cpu->memory.activeRegion);
		_thumbTable[opcode >> 6](cpu, opcode);
	} else {
		uint32_t opcode = cpu->prefetch[0];
		cpu->prefetch[0] = cpu->prefetch[1];
		cpu->gprs[ARM_PC] += WORD_SIZE_ARM;
		LOAD_32(cpu->prefetch[1], cpu->gprs[ARM_PC] & cpu->memory.activeMask, cpu->memory.activeRegion);
		jit_arm_exec(cpu, opcode);
	}
}

uint32_t jit_fetch(struct ARMCore* cpu, uint32_t address, bool thumb) {
	uint32_t v;
	if (thumb) {
		LOAD_16(v, address & cpu->memory.activeMask, cpu->memory.activeRegion);
	} else {
		LOAD_32(v, address & cpu->memory.activeMask, cpu->memory.activeRegion);
	}
	return v;
}

// The interpreter's state right before executing `address` (same region as the block).
static void set_next(struct ARMCore* cpu, uint32_t address) {
	bool thumb = cpu->executionMode == MODE_THUMB;
	uint32_t w = thumb ? WORD_SIZE_THUMB : WORD_SIZE_ARM;
	cpu->gprs[ARM_PC] = address + w;
	cpu->prefetch[0] = jit_fetch(cpu, address, thumb);
	cpu->prefetch[1] = jit_fetch(cpu, address + w, thumb);
}

// Calls translated code: r3 = cpu, r4 = code in; r3 = next pc, r4 = exit info, r5 = extra
// (EXIT_CHAIN: the chain exit's address + 16; EXIT_WRITEPC: the branch's own cycles).
static uint32_t run_code(struct jit* j, const uint32_t* code, uint32_t* info, uint64_t* extra) {
	register uint64_t r3 __asm__("r3") = (uintptr_t) j->cpu;
	register uint64_t r4 __asm__("r4") = (uintptr_t) code;
	register uint64_t r5 __asm__("r5");
	register uint64_t r12 __asm__("r12") = (uintptr_t) j->enter;
	__asm__ volatile("mtctr 12\n\tbctrl"
		: "+r"(r3), "+r"(r4), "=r"(r5), "+r"(r12)
		:
		: "r0", "r6", "r7", "r8", "r9", "r10", "r11", "ctr", "lr", "xer",
		  "cr0", "cr1", "cr5", "cr6", "cr7",
		  "fr0", "fr1", "fr2", "fr3", "fr4", "fr5", "fr6", "fr7", "fr8", "fr9", "fr10", "fr11", "fr12", "fr13",
		  "v0", "v1", "v2", "v3", "v4", "v5", "v6", "v7", "v8", "v9", "v10", "v11", "v12", "v13",
		  "v14", "v15", "v16", "v17", "v18", "v19", "memory");
	*info = r4;
	*extra = r5;
	return r3;
}

// TIMES=1 builds time the run loop's parts (jit_stats): mftb is not free on the PPE (~1% of a
// frame with 4 players).
#ifdef JIT_TIMES
#define DEV_TB() __builtin_ppc_get_timebase()
#else
#define DEV_TB() 0
#endif

static void interpret(struct jit* j) {
	uint64_t t0 = DEV_TB();
	jit_step(j->cpu);
	j->stats.interp_ticks += DEV_TB() - t0;
	++j->stats.interpreted;
	if (j->check) {
		jit_check_after(j, 1);
	}
}

static bool idle_address(uint32_t address) {
	int region = address >> 24;
	return region == GBA_REGION_EWRAM || region == GBA_REGION_IWRAM ||
	       (region >= GBA_REGION_ROM0 && region <= GBA_REGION_ROM2_EX);
}

// The block at `key` branched back to its start (EXIT_LOOP). If the previous dispatch was the
// same loop's previous iteration and it started with the same registers and flags, it is idle:
// skip the whole iterations that end before the next event (the interpreter would run exactly
// those, all identical), and let the block run the rest. Returns false if it isn't idle.
static bool idle_loop(struct jit* j, uint32_t key, uint32_t* skipped) {
	struct ARMCore* cpu = j->cpu;
	struct jit_block* b = jit_lookup(j, key);
	if (!b || !b->alive || b->not_idle) {
		j->idle_block = NULL;
		return false;
	}
	bool same = j->idle_block == b && !memcmp(j->idle_regs, cpu->gprs, sizeof(j->idle_regs)) &&
	            (j->idle_cpsr & 0xF0000000) == (cpu->cpsr.packed & 0xF0000000);
	if (!same) {
		// Against an entry snapshot, a first iteration may differ (the loop was just entered).
		if (j->idle_block == b && !j->idle_entry && ++b->idle_fails >= 8) {
			b->not_idle = true; // a loop doing work (e.g. a delay counter): let it chain
			j->idle_block = NULL;
			return false;
		}
		j->idle_block = b;
		j->idle_entry = false;
		memcpy(j->idle_regs, cpu->gprs, sizeof(j->idle_regs));
		j->idle_cpsr = cpu->cpsr.packed;
		j->idle_cycles = cpu->cycles;
		return true;
	}
	j->idle_entry = false;
	for (int i = 0; i < b->nloads; ++i) {
		uint32_t address = b->loads[i].imm;
		if (b->loads[i].base >= 0) {
			address += cpu->gprs[b->loads[i].base];
		}
		if (b->loads[i].offset >= 0) {
			address += cpu->gprs[b->loads[i].offset];
		}
		if (!idle_address(address)) { // IO may change without an event (timers)
			b->not_idle = true;
			j->idle_block = NULL;
			return false;
		}
	}
	b->idle_proven = true;
	int32_t period = cpu->cycles - j->idle_cycles;
	int32_t k = period > 0 ? (cpu->nextEvent - cpu->cycles) / period - 1 : 0;
	if (k > 0) {
		cpu->cycles += k * period;
		*skipped = k * b->ninstr;
		j->stats.idle_skipped += *skipped;
	}
	j->idle_cycles = cpu->cycles;
	return true;
}

void jit_run_loop(struct jit* j) {
	struct ARMCore* cpu = j->cpu;
	uint64_t loop_t0 = DEV_TB();
	++j->stats.run_loops;
	j->idle_block = NULL;
	j->rom = (uint8_t*) j->gba->memory.rom; // mGBA may copy the ROM (writes to GPIO carts)
	j->rom_size = j->gba->memory.romSize;
	bool resume = true; // pc was reached in sequence (an event, a forced exit), not by a branch
	while (cpu->cycles < cpu->nextEvent) {
		if (j->flush_pending) {
			flush_all(j);
		}
		bool thumb = cpu->executionMode == MODE_THUMB;
		uint32_t w = thumb ? WORD_SIZE_THUMB : WORD_SIZE_ARM;
		uint32_t pc = cpu->gprs[ARM_PC] - w;
		struct jit_block* b = jit_lookup(j, pc | thumb);
		const uint32_t* code = b ? b->code : NULL;
		uint32_t entered = 0; // instructions of b before its entry point `code`
		uint32_t entry;
		struct jit_block* c;
		if ((!b || !b->alive) && resume && (c = enclosing_entry(j, pc, thumb, &entry))) {
			// Inside a live block: the interpreter runs up to its entry point (blocks end at
			// branches, so in sequence), then the block runs from there.
			uint32_t at = c->start + (entry >> 24) * w;
			while (pc != at && cpu->cycles < cpu->nextEvent) {
				interpret(j);
				if (cpu->gprs[ARM_PC] - w != pc + w || (cpu->executionMode == MODE_THUMB) != thumb) {
					resume = false; // not in sequence after all (a handler moved PC)
					break;
				}
				pc += w;
			}
			if (pc != at || !c->alive || cpu->cycles >= cpu->nextEvent || j->flush_pending) {
				continue;
			}
			b = c;
			code = c->code + (entry & 0xFFFFFF);
			entered = entry >> 24;
			++j->stats.resumed;
		} else if (!b || !b->alive) {
			b = live_block(j, pc | thumb);
			code = b ? b->code : NULL;
		}
		if (!b) {
			uint64_t t0 = __builtin_ppc_get_timebase();
			b = jit_translate(j, pc, thumb);
			j->stats.translate_ticks += __builtin_ppc_get_timebase() - t0;
			if (!b && !j->overflow) {
				interpret(j); // nothing translatable here (e.g. outside RAM/ROM/BIOS)
				continue;
			}
			if (!b) {
				flush_all(j); // cache or block table full: start over
				b = jit_translate(j, pc, thumb);
				if (!b) {
					interpret(j);
					continue;
				}
			}
			code = b->code;
		}
		uint32_t info;
		uint64_t extra;
		uint32_t skipped = 0; // idle loop instructions skipped
		if (b->idle_proven && !b->not_idle && !entered) {
			// A loop proven idle before: its next EXIT_LOOP compares against this state, so
			// one iteration is enough (at its head lastPrefetchedPc is 0, as in every iteration).
			j->idle_block = b;
			j->idle_entry = true;
			memcpy(j->idle_regs, cpu->gprs, sizeof(j->idle_regs));
			j->idle_cpsr = cpu->cpsr.packed;
			j->idle_cycles = cpu->cycles;
		}
		if (j->check) {
			jit_check_before(j, b);
		}
		j->in_jit = true;
		uint64_t t0 = DEV_TB();
		uint32_t next = run_code(j, code, &info, &extra);
		j->stats.code_ticks += DEV_TB() - t0;
		j->in_jit = false;
		bool forced = j->forced;
		if (forced) {
			// An invalidation made the block exit early: restore the real next event.
			j->forced = false;
			// mTimingNextEvent counts from now; nextEvent is in cycles like cpu->cycles.
			int32_t n = mTimingNextEvent(&j->gba->timing);
			if (n != INT_MAX) {
				n += cpu->cycles;
			}
			cpu->nextEvent = j->saved_next < n ? j->saved_next : n;
		}
		++j->stats.dispatches;
		if ((info & 0xFF) == EXIT_WRITEPC || (info & 0xFF) == EXIT_BX) {
			++j->stats.writepc;
		}
		if ((info & 0xFF) != EXIT_LOOP) {
			j->idle_block = NULL;
		}
		switch (info & 0xFF) {
		case EXIT_PCSET:
			break; // the instruction left the interpreter's state (branch taken)
		case EXIT_WRITEPC: {
			// As the interpreter: WritePC (may halt: nextEvent = cycles) before the branch's own
			// cycles are added.
			int32_t cycles = (int32_t) extra;
			cpu->gprs[ARM_PC] = next;
			cycles += thumb ? ThumbWritePC(cpu) : ARMWritePC(cpu);
			cpu->cycles += cycles;
			break;
		}
		case EXIT_BX: {
			// Exactly the interpreter's BX: its cycles are added after the mode switch, which
			// sets nextEvent = cycles when the mode changes.
			int32_t cycles = 1 + (thumb ? cpu->memory.activeSeqCycles16 : cpu->memory.activeSeqCycles32);
			_ARMSetMode(cpu, next & 1);
			cpu->gprs[ARM_PC] = next & 0xFFFFFFFE;
			cycles += cpu->executionMode == MODE_THUMB ? ThumbWritePC(cpu) : ARMWritePC(cpu);
			cpu->cycles += cycles;
			break;
		}
		case EXIT_LOOP:
			set_next(cpu, next);
			if (idle_loop(j, next | thumb, &skipped)) {
				break;
			}
			// fall through: not idle, chain it like any other exit
		case EXIT_CHAIN:
			set_next(cpu, next);
			if ((!j->check || j->check_chain) && !j->flush_pending) {
				// Next time, jump straight from that exit to the target's code.
				struct jit_block* target = jit_lookup(j, next | thumb);
				if (target && target->alive) {
					jit_link(j, (uint32_t*) (uintptr_t) extra - 4, target);
				}
			}
			break;
		default:
			set_next(cpu, next);
			break;
		}
		if (j->check) {
			// Chain exits counted themselves in j->chained (when check_chain is on).
			bool counted = ((info & 0xFF) == EXIT_CHAIN || (info & 0xFF) == EXIT_LOOP) && j->check_chain;
			// Exits count instructions from the block's start, also when entered mid-block.
			uint32_t n = j->chained + (counted ? 0 : info >> 8) + skipped - entered;
			j->chained = 0;
			jit_check_after(j, n);
		}
		// The BIOS returns from interrupts (`subs pc, lr, #4`) to wherever the event came.
		resume = (info & 0xFF) == EXIT_INTERP || ((info & 0xFF) == EXIT_PCSET && (b->start >> 24) == GBA_REGION_BIOS);
		if ((info & 0xFF) == EXIT_INTERP && !forced) {
			// An event falls inside the next instructions: run them like ARMRunLoop does. (A
			// forced exit only had nextEvent lowered: back to translated code instead.)
			while (cpu->cycles < cpu->nextEvent) {
				interpret(j);
			}
		}
	}
	uint64_t t0 = DEV_TB();
	cpu->irqh.processEvents(cpu);
	uint64_t t1 = DEV_TB();
	j->stats.event_ticks += t1 - t0;
	j->stats.loop_ticks += t1 - loop_t0;
	if (j->check) {
		jit_check_events(j);
	}
}

// ---- Setup ----

static void install_store_wrappers(struct jit* j) {
	struct ARMCore* cpu = j->cpu;
	j->store8 = cpu->memory.store8;
	j->store16 = cpu->memory.store16;
	j->store32 = cpu->memory.store32;
	j->storeMultiple = cpu->memory.storeMultiple;
	uint64_t toc = ((const uint64_t*) GBAStore32)[1];
	j->st8_desc = (struct jit_desc) { (uintptr_t) j->st8_stub, toc, 0 };
	j->st16_desc = (struct jit_desc) { (uintptr_t) j->st16_stub, toc, 0 };
	j->st32_desc = (struct jit_desc) { (uintptr_t) j->st32_stub, toc, 0 };
	j->stm_desc = (struct jit_desc) { (uintptr_t) j->stm_stub, toc, 0 };
	cpu->memory.store8 = (void*) &j->st8_desc;
	cpu->memory.store16 = (void*) &j->st16_desc;
	cpu->memory.store32 = (void*) &j->st32_desc;
	cpu->memory.storeMultiple = (void*) &j->stm_desc;
}

struct jit* jit_create(struct mCore* core, int slot) {
	struct GBA* gba = core->board;
	struct ARMCore* cpu = core->cpu;
	// Translations call mGBA's memory functions directly: refuse anything unexpected.
	if (cpu->memory.load32 != GBALoad32 || cpu->memory.store32 != GBAStore32 ||
	    cpu->memory.storeMultiple != GBAStoreMultiple || cpu->memory.loadMultiple != GBALoadMultiple ||
	    cpu->memory.stall == NULL || slot < 0 || slot >= SLOTS) {
		log_printf("jit: unexpected memory handlers, JIT off\n");
		return NULL;
	}
	struct jit* j = memalign(64, sizeof(*j));
	if (!j || (uintptr_t) j >> 32) {
		log_printf("jit: slot %d, no memory for its state (%u KB), JIT off\n", slot, (unsigned) sizeof(*j) / 1024);
		free(j);
		return NULL;
	}
	memset(j, 0, sizeof(*j));
	// Everything allocated here, nothing while emulating.
	j->blocks = malloc(MAX_BLOCKS * sizeof(*j->blocks));
	j->links = malloc(MAX_LINKS * sizeof(*j->links));
	j->tctx = malloc(jit_tctx_size());
	if (!j->blocks || !j->links || !j->tctx) {
		log_printf("jit: slot %d, no memory for blocks (%u KB), links (%u KB) or translator (%u KB), JIT off\n", slot,
		           (unsigned) (MAX_BLOCKS * sizeof(*j->blocks) / 1024), (unsigned) (MAX_LINKS * sizeof(*j->links) / 1024),
		           (unsigned) (jit_tctx_size() / 1024));
		free(j->blocks);
		free(j->links);
		free(j->tctx);
		free(j);
		return NULL;
	}
	j->core = core;
	j->cpu = cpu;
	j->gba = gba;
	j->base = jit_cache + slot * (CACHE_BYTES / 4 / SLOTS);
	j->cur = j->base;
	j->end = j->base + CACHE_BYTES / 4 / SLOTS - REF_STUB_WORDS; // the tail: jit_create_ref
	emit_shared(j);
	jit_flush_icache(j->base, j->cur);

	install_store_wrappers(j);
	flush_all(j);
	j->stats.flushes = 0;
	log_printf("jit: slot %d, cache %p-%p\n", slot, (void*) j->base, (void*) j->end);
	return j;
}

// Store wrappers (logging only) for the self-check's reference core, in the tail of main's slice.
struct jit* jit_create_ref(struct jit* main, struct mCore* core) {
	struct jit* j = memalign(64, sizeof(*j));
	if (!j || (uintptr_t) j >> 32) {
		free(j);
		return NULL;
	}
	memset(j, 0, sizeof(*j));
	j->core = core;
	j->cpu = core->cpu;
	j->gba = core->board;
	j->base = j->cur = main->end;
	j->end = main->end + REF_STUB_WORDS;
	j->check = main->check;
	j->st8_stub = make_store_stub(j, jit_st8_c, 7);
	j->st16_stub = make_store_stub(j, jit_st16_c, 7);
	j->st32_stub = make_store_stub(j, jit_st32_c, 7);
	j->stm_stub = make_store_stub(j, jit_stm_c, 8);
	jit_flush_icache(j->base, j->cur);
	install_store_wrappers(j);
	ws_snapshot(j->gba, j->ws);
	memset(j->page_head, 0xFF, sizeof(j->page_head));
	return j;
}

void jit_destroy_ref(struct jit* j) {
	if (j) {
		j->cpu->memory.store8 = j->store8;
		j->cpu->memory.store16 = j->store16;
		j->cpu->memory.store32 = j->store32;
		j->cpu->memory.storeMultiple = j->storeMultiple;
		free(j);
	}
}

void jit_destroy(struct jit* j) {
	if (!j) {
		return;
	}
	j->cpu->memory.store8 = j->store8;
	j->cpu->memory.store16 = j->store16;
	j->cpu->memory.store32 = j->store32;
	j->cpu->memory.storeMultiple = j->storeMultiple;
	jit_check_free(j);
	free(j->blocks);
	free(j->links);
	free(j->tctx);
	free(j);
}

void jit_get_stats(struct jit* j, struct jit_stats* out, bool reset) {
	j->stats.cache_used = (j->cur - j->base) * 4;
	j->stats.in_jit = j->in_jit;
	*out = j->stats;
	if (reset) {
		j->stats.translated = j->stats.invalidated = j->stats.revived = j->stats.flushes = 0;
		j->stats.translate_ticks = 0;
		j->stats.interpreted = j->stats.resumed = j->stats.checked = j->stats.idle_skipped = 0;
		j->stats.dispatches = j->stats.writepc = j->stats.run_loops = 0;
		j->stats.loop_ticks = j->stats.code_ticks = j->stats.interp_ticks = j->stats.event_ticks = 0;
	}
}
