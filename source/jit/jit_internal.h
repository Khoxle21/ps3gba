// Shared between the JIT's files (jit.c, translate.c, check.c). See docs/jit.md.
#ifndef JIT_INTERNAL_H
#define JIT_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <mgba/internal/arm/arm.h>

#include "jit.h"
#include "ppc.h"

// Register use inside translated code (all non-volatile, so C helpers preserve them):
// GBA r0-r14 in r14-r28, the struct jit in r29, cycles in r30, the ARMCore in r31.
// GBA flags in CR: N = cr2.lt, Z = cr2.eq, C = cr3.lt, V = cr4.lt.
#define RG(n) (14 + (n)) // n in 0..14 only: RG(-1) is r13 (thread pointer), RG(15) r29
#define RJIT 29 // struct jit: fields read by translated code come first (offsets < 32 KB)
#define RCYC 30
#define RCPU 31
#define CR_N CR_LT(2)
#define CR_Z CR_EQ(2)
#define CR_C CR_LT(3)
#define CR_V CR_LT(4)

// enter's stack frame: 48 header + 64 parameter save area (callees may use it) + r14-r31 saved
// at 112, then a word of scratch for translated code.
#define FRAME 288
#define FRAME_SCRATCH 256    // a word for translated code
#define FRAME_SCRATCH_LR 264 // memory routines' slow paths save LR here

#define OFF_GPRS offsetof(struct ARMCore, gprs)
#define OFF_PC (OFF_GPRS + 4 * ARM_PC)
#define OFF_CYC offsetof(struct ARMCore, cycles)
#define OFF_NEXT offsetof(struct ARMCore, nextEvent)
#define OFF_CPSR offsetof(struct ARMCore, cpsr)
#define OFF_PREF offsetof(struct ARMCore, prefetch)

// Why translated code returned to the dispatcher (low byte of r4; instructions run << 8).
enum {
	EXIT_EVENT,  // before the instruction at r3: cycles reached nextEvent
	EXIT_PCSET,  // an instruction set PC itself (cpu state already consistent)
	EXIT_NEXT,   // end of block, continue at r3
	EXIT_INTERP, // an event falls within the next instructions: interpret from r3
	EXIT_CHAIN,  // like EXIT_NEXT, from a patchable exit (r5 = its address + 16): link it
	EXIT_WRITEPC, // branch to r3 needing mGBA's WritePC (cycles in r30 exclude WritePC's)
	EXIT_BX,     // BX to r3: mode switch + WritePC (cycles exclude this instruction's)
	EXIT_LOOP,   // like EXIT_CHAIN, from the backward branch of a possible idle loop
};

#define MAP_BITS 16
#define MAP_SIZE (1 << MAP_BITS)
#define MAP_EMPTY 0xFFFFFFFFu
#define MAX_BLOCKS 24576 // FireRed's overworld + menus: ~16000 blocks in 3.5-4 MB
#define MAX_LINKS 32768
#define MIN_FREE_WORDS (32 * 1024 / 4) // room for the largest block
#define REF_STUB_WORDS 256 // end of each slice: the self-check reference core's store stubs
#define MAX_VARIANTS 8 // dead translations of one address compared for revival
#define IWRAM_PAGES 128 // 32 KB / 256
#define EWRAM_PAGES 1024 // 256 KB / 256

struct jit_block {
	uint32_t start, end; // [start, end): GBA addresses of the translated instructions
	uint32_t *code, *code_end;
	int32_t page_next;   // next block in the same RAM page, -1 = last
	int32_t links_in;    // first chained branch into this block (index into jit.links), -1 = none
	int32_t variant;     // older (dead) translation at the same address, -1 = none
	const uint8_t* src;  // RAM blocks: the GBA code translated (kept in the cache, after the code)
	// RAM blocks: where the dispatcher may enter mid-block (instruction index << 24 | word
	// offset from code), ascending; in the cache after the code.
	const uint32_t* entries;
	uint8_t nentries;
	uint16_t ninstr;
	uint8_t thumb, alive;
	uint8_t nfallback, ncall; // instructions run by mGBA's handlers / calling C (profile)
	uint32_t runs;            // profile builds: times entered
	// Possible idle loop (branches back to its start, only reads memory): the loads' address
	// registers (base, offset; -1 = none) and immediate, to check they read RAM.
	uint8_t idle_fails, nloads;
	bool not_idle, idle_proven; // proven: snapshot at dispatch to its head (one iteration then)
	struct { int8_t base, offset; int32_t imm; } loads[4];
};

// A patched direct branch from one block into another; undone when the target dies.
struct jit_link {
	uint32_t* site;    // the patched branch instruction
	uint32_t original; // what it was before patching (a branch to its exit stub)
	int32_t next;      // next link into the same target
};

struct jit_desc {
	uint64_t entry, toc, env; // ELFv1 function descriptor
};

struct jit_check;
struct tctx;

struct map_entry {
	uint32_t key, block;
};

// Memory routines (memory.c), by kind.
enum { JIT_LD32, JIT_LD16, JIT_LD8, JIT_ST32, JIT_ST16, JIT_ST8, JIT_MEM_KINDS };
#define JIT_LOG_MAX 512 // self-check: stores logged per comparison

struct jit {
	// Read by translated code through RJIT: first, so offsets fit a 16-bit displacement.
	uint8_t *iwram, *ewram, *rom;
	uint32_t* lpp; // &gba->memory.lastPrefetchedPc
	uint32_t rom_size;
	uint8_t rom_wait32[5], rom_wait16[5]; // ROM regions 8-12: non-sequential waitstate + 2
	uint8_t ew_wait_ld32, ew_wait_ld16, ew_wait_st32, ew_wait_st16;
	uint16_t ew_lsm_wait[2][17]; // EWRAM LDM/STM of n words: mGBA's wait [load, store][n]
	// A bit per halfword holding translated code. Aligned: the multiple-store test reads words.
	uint8_t iwram_bits[32 * 1024 / 16] __attribute__((aligned(8)));
	uint8_t ewram_bits[256 * 1024 / 16];
	uint32_t slog_n;                    // self-check: stores since the last comparison
	uint32_t slog[JIT_LOG_MAX * 2];     // (aligned address, size)

	struct mCore* core;
	struct ARMCore* cpu;
	struct GBA* gba;
	uint32_t *base, *end, *cur, *code_start; // this player's slice of the code cache
	bool overflow; // an emission ran past the slice
	uint32_t *enter, *exit_regs, *exit_mem, *exit_link, *sync_out, *sync_in;
	uint32_t *st8_stub, *st16_stub, *st32_stub, *stm_stub;
	uint32_t *mem_start, *stall, *mem[2][JIT_MEM_KINDS]; // [code from ROM with prefetch][kind]
	uint32_t* lsm[2][3]; // LDM/STM in IWRAM/EWRAM: [code from ROM with prefetch][load, store, load PC]
	uint32_t* lsm_mgba[2]; // LDM/STM through mGBA: [store]
	struct jit_desc st8_desc, st16_desc, st32_desc, stm_desc;
	void (*store8)(struct ARMCore*, uint32_t, int8_t, int*);
	void (*store16)(struct ARMCore*, uint32_t, int16_t, int*);
	void (*store32)(struct ARMCore*, uint32_t, int32_t, int*);
	uint32_t (*storeMultiple)(struct ARMCore*, uint32_t, int, enum LSMDirection, int*);

	struct map_entry map[MAP_SIZE];
	struct jit_block* blocks;
	uint32_t nblocks;
	struct jit_link* links;
	uint32_t nlinks;
	int32_t free_links; // links undone by kills, reused first (-1 = none)
	int32_t page_head[IWRAM_PAGES + EWRAM_PAGES];
	uint8_t ws[65]; // waitstates the translations were made with

	uint32_t chained;   // self-check: instructions run by blocks that chained since the last exit
	bool check_chain;   // self-check with block chaining (else every block returns)
	bool profile;       // count block runs (jit_profile_dump)
	uint32_t prof_lsm[2];          // profile: LDM/STM/PUSH/POP run [load, store]
	uint32_t prof_lsm_slow[2][16]; // of which through mGBA, by the base's region
	uint32_t prof_mem_slow[2][16]; // single loads/stores through mGBA, by region [load, store]
	uint32_t prof_frame;           // GBA frame of the last profile dump
	struct jit_block* idle_block; // last EXIT_LOOP's block, its registers and cycles then
	int32_t idle_regs[15], idle_cpsr, idle_cycles;
	bool idle_entry; // that state was snapshot at dispatch, not after an iteration
	bool in_jit;        // translated code is running (stores may force an exit)
	bool forced;        // nextEvent was lowered to force that exit
	bool flush_pending; // drop all translations at the next dispatch
	int32_t saved_next;
	struct jit_stats stats;
	struct jit_check* check;
	struct tctx* tctx; // the translator's scratch state (translate.c)
};

// jit.c
void jit_emit(struct jit* j, uint32_t insn);
void jit_li32(struct jit* j, int reg, uint32_t v);
void jit_la(struct jit* j, int reg, uint64_t v);
void jit_bl(struct jit* j, const uint32_t* target);
void jit_b(struct jit* j, const uint32_t* target);
void jit_call(struct jit* j, const void* fn);
void jit_flush_icache(const void* start, const void* end);
struct jit_block* jit_lookup(struct jit* j, uint32_t key);
struct jit_block* jit_new_block(struct jit* j);
void jit_commit_block(struct jit* j, struct jit_block* b);
int jit_ram_page(uint32_t address);
uint32_t jit_fetch(struct ARMCore* cpu, uint32_t address, bool thumb);
void jit_arm_exec(struct ARMCore* cpu, uint32_t opcode);
void jit_step(struct ARMCore* cpu);

struct jit* jit_create_ref(struct jit* main, struct mCore* core);
void jit_reemit_memory(struct jit* j);

// memory.c
void jit_emit_memory_routines(struct jit* j);
void jit_memory_refresh(struct jit* j);
void jit_destroy_ref(struct jit* j);

// translate.c
struct jit_block* jit_translate(struct jit* j, uint32_t pc, bool thumb);
void jit_unlink_incoming(struct jit* j, struct jit_block* b);
void jit_link(struct jit* j, uint32_t* site, struct jit_block* target);
size_t jit_tctx_size(void);

// check.c (dev builds; only called when a reference core is attached)
void jit_check_before(struct jit* j, const struct jit_block* b);
void jit_check_after(struct jit* j, uint32_t instructions);
void jit_check_events(struct jit* j);
void jit_check_log_store(struct jit* j, uint32_t address, uint32_t size);
void jit_check_free(struct jit* j);

#endif
