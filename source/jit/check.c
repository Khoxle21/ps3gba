// Self-check (dev builds): a reference core, loaded and reset exactly like the JIT core, runs
// mGBA's interpreter in lockstep. After every block (and every interpreted instruction) it runs
// the same number of instructions and both are compared: CPU state, the sequence of stores and
// the stored bytes, and once per frame all of RAM, VRAM, OAM, palette and IO registers. The JIT
// is cycle-exact by design (docs/jit.md), so any difference is a bug. The first one is logged
// with the block's code and both states, then checking stops (the game keeps running).
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <mgba/core/core.h>
#include <mgba/internal/gba/gba.h>
#include <mgba/internal/gba/memory.h>

#include "emu.h"
#include "jit_internal.h"

struct jit_check {
	struct mCore* ref_core;
	struct jit* ref; // the reference core's store wrappers
	bool failed;
	char report[4096];
	struct ARMCore before;   // JIT core state when the current block was entered
	uint32_t before_lpp;     // and its lastPrefetchedPc
	// The reference core's steps for the current comparison (PC, cycles, lastPrefetchedPc after
	// each, the first 64): shows which instruction's cycles differ.
	uint32_t steps;
	struct { uint32_t pc, lpp; int32_t cycles; } trace[64];
	const struct jit_block* block;
	uint32_t frame;          // last frame whose memory was compared
};

void jit_check_free(struct jit* j) {
	struct jit_check* c = j->check;
	if (!c) {
		return;
	}
	jit_destroy_ref(c->ref);
	free(c);
	j->check = NULL;
}

bool jit_attach_check(struct jit* j, struct mCore* ref, bool chain) {
	struct jit_check* c = calloc(1, sizeof(*c));
	if (!c) {
		return false;
	}
	j->check = c;
	j->check_chain = chain;
	c->ref_core = ref;
	c->ref = jit_create_ref(j, ref);
	if (!c->ref) {
		free(c);
		j->check = NULL;
		return false;
	}
	jit_reemit_memory(j); // its store routines now log too
	c->frame = ((struct GBA*) j->core->board)->video.frameCounter;
	log_printf("jit check: on%s\n", j->check_chain ? ", blocks chained" : "");
	return true;
}

const char* jit_check_report(struct jit* j) {
	return j->check ? j->check->report : "";
}

// Stores through mGBA (after_store); the inline paths append to the same log (memory.c).
void jit_check_log_store(struct jit* j, uint32_t address, uint32_t size) {
	if (j->slog_n < JIT_LOG_MAX) {
		j->slog[2 * j->slog_n] = address;
		j->slog[2 * j->slog_n + 1] = size;
	}
	++j->slog_n; // past JIT_LOG_MAX: overflowed
}

void jit_check_before(struct jit* j, const struct jit_block* b) {
	struct jit_check* c = j->check;
	if (c->failed) {
		return;
	}
	c->before = *j->cpu;
	c->before_lpp = ((struct GBA*) j->core->board)->memory.lastPrefetchedPc;
	c->block = b;
	// Same keys at the same emulated time (emu.c sets them on the JIT core between run loops).
	c->ref_core->setKeys(c->ref_core, j->core->getKeys(j->core));
}

static int appendf(struct jit_check* c, int at, const char* fmt, ...) __attribute__((format(printf, 3, 4)));

static int appendf(struct jit_check* c, int at, const char* fmt, ...) {
	if (at >= (int) sizeof(c->report)) {
		return at;
	}
	va_list args;
	va_start(args, fmt);
	int n = vsnprintf(c->report + at, sizeof(c->report) - at, fmt, args);
	va_end(args);
	return n < 0 ? at : at + n;
}

static int dump_cpu(struct jit_check* c, int at, const char* name, const struct ARMCore* cpu, const struct ARMCore* other) {
	at = appendf(c, at, "  %-6s", name);
	for (int i = 0; i < 16; ++i) {
		bool diff = other && cpu->gprs[i] != other->gprs[i];
		at = appendf(c, at, " r%d=%08x%s", i, (uint32_t) cpu->gprs[i], diff ? "*" : "");
	}
	at = appendf(c, at, " cpsr=%08x%s spsr=%08x cycles=%d%s next=%d%s halted=%d pf=%08x,%08x%s\n",
	             (uint32_t) cpu->cpsr.packed, other && cpu->cpsr.packed != other->cpsr.packed ? "*" : "",
	             (uint32_t) cpu->spsr.packed, cpu->cycles, other && cpu->cycles != other->cycles ? "*" : "",
	             cpu->nextEvent, other && cpu->nextEvent != other->nextEvent ? "*" : "", cpu->halted,
	             cpu->prefetch[0], cpu->prefetch[1],
	             other && (cpu->prefetch[0] != other->prefetch[0] || cpu->prefetch[1] != other->prefetch[1]) ? "*" : "");
	return at;
}

static void fail(struct jit* j, const char* what) {
	struct jit_check* c = j->check;
	struct ARMCore* ref = c->ref_core->cpu;
	c->failed = true;
	int at = appendf(c, 0, "jit check: MISMATCH (%s) after %u checks, frame %u\n", what, j->stats.checked,
	                 ((struct GBA*) j->core->board)->video.frameCounter);
	if (c->block) {
		const struct jit_block* b = c->block;
		at = appendf(c, at, "  block %08x-%08x %s, %u instructions:", b->start, b->end, b->thumb ? "thumb" : "arm", b->ninstr);
		for (uint32_t a = b->start; a < b->end && at < 3000; a += b->thumb ? 2 : 4) {
			at = appendf(c, at, " %0*x", b->thumb ? 4 : 8, jit_fetch(j->cpu, a, b->thumb));
		}
		at = appendf(c, at, "\n");
		at = dump_cpu(c, at, "before", &c->before, NULL);
	}
	at = dump_cpu(c, at, "jit", j->cpu, ref);
	at = dump_cpu(c, at, "interp", ref, j->cpu);
	const struct GBAMemory* ma = &((struct GBA*) j->core->board)->memory;
	const struct GBAMemory* mb = &((struct GBA*) c->ref_core->board)->memory;
	at = appendf(c, at, "  interp steps (pc/cycles/lastPrefetchedPc after each):");
	for (uint32_t i = 0; i < c->steps; ++i) {
		at = appendf(c, at, " %08x/%d/%08x", c->trace[i].pc, c->trace[i].cycles, c->trace[i].lpp);
	}
	at = appendf(c, at, "\n");
	at = appendf(c, at, "  lastPrefetchedPc before %08x, jit %08x, interp %08x\n", c->before_lpp, ma->lastPrefetchedPc,
	             mb->lastPrefetchedPc);
	for (int side = 0; side < 2; ++side) {
		const struct jit* l = side ? c->ref : j;
		at = appendf(c, at, "  %s stores:", side ? "interp" : "jit");
		for (uint32_t i = 0; i < l->slog_n && i < 16; ++i) {
			at = appendf(c, at, " %08x/%u", l->slog[2 * i], l->slog[2 * i + 1]);
		}
		at = appendf(c, at, "%s\n", l->slog_n > JIT_LOG_MAX ? " (overflow)" : "");
	}
	log_printf("%s", c->report);
	if (c->block) {
		// The block's PowerPC code, for: ppu-objdump -D -b binary -m powerpc:common64 -EB
		// --adjust-vma=<code address> jitblock.bin
		FILE* f = fopen("/dev_hdd0/ps3gba/jitblock.bin", "wb");
		if (f) {
			fwrite(c->block->code, 4, c->block->code_end - c->block->code, f);
			fclose(f);
			log_printf("jit check: block code at %p written to jitblock.bin\n", (void*) c->block->code);
		}
	}
}

static bool same_cpu(const struct ARMCore* a, const struct ARMCore* b) {
	return !memcmp(a->gprs, b->gprs, sizeof(a->gprs)) && a->cpsr.packed == b->cpsr.packed &&
	       a->spsr.packed == b->spsr.packed && a->cycles == b->cycles && a->nextEvent == b->nextEvent &&
	       a->halted == b->halted && a->executionMode == b->executionMode &&
	       a->privilegeMode == b->privilegeMode && a->prefetch[0] == b->prefetch[0] &&
	       a->prefetch[1] == b->prefetch[1] &&
	       !memcmp(a->bankedRegisters, b->bankedRegisters, sizeof(a->bankedRegisters)) &&
	       !memcmp(a->bankedSPSRs, b->bankedSPSRs, sizeof(a->bankedSPSRs));
}

// Bytes of a stored range in a core's memory, for the regions that are plain arrays.
static const uint8_t* memory_at(struct GBA* gba, uint32_t address, uint32_t* limit) {
	switch (address >> 24) {
	case GBA_REGION_EWRAM:
		*limit = GBA_SIZE_EWRAM - (address & (GBA_SIZE_EWRAM - 1));
		return (const uint8_t*) gba->memory.wram + (address & (GBA_SIZE_EWRAM - 1));
	case GBA_REGION_IWRAM:
		*limit = GBA_SIZE_IWRAM - (address & (GBA_SIZE_IWRAM - 1));
		return (const uint8_t*) gba->memory.iwram + (address & (GBA_SIZE_IWRAM - 1));
	case GBA_REGION_IO:
		if ((address & 0x00FFFFFF) >= GBA_SIZE_IO) {
			return NULL;
		}
		*limit = GBA_SIZE_IO - (address & 0x00FFFFFF);
		return (const uint8_t*) gba->memory.io + (address & 0x00FFFFFF);
	case GBA_REGION_PALETTE_RAM:
		*limit = GBA_SIZE_PALETTE_RAM - (address & (GBA_SIZE_PALETTE_RAM - 1));
		return (const uint8_t*) gba->video.palette + (address & (GBA_SIZE_PALETTE_RAM - 1));
	case GBA_REGION_OAM:
		*limit = GBA_SIZE_OAM - (address & (GBA_SIZE_OAM - 1));
		return (const uint8_t*) gba->video.oam.raw + (address & (GBA_SIZE_OAM - 1));
	default:
		return NULL; // VRAM mirrors and save chips: covered by the per-frame compare / not needed
	}
}

// Same stores (address, size) and stored bytes on both cores since the last comparison. On a
// difference, `what` says which and the logs are kept for the report.
static bool compare_stores(struct jit* j, char* what, size_t what_size) {
	struct jit_check* c = j->check;
	struct jit *a = j, *b = c->ref;
	uint32_t n = a->slog_n < JIT_LOG_MAX ? a->slog_n : JIT_LOG_MAX;
	if (a->slog_n != b->slog_n || memcmp(a->slog, b->slog, n * 2 * sizeof(*a->slog))) {
		snprintf(what, what_size, "stores differ: %u by the JIT, %u by the interpreter", a->slog_n, b->slog_n);
		return false;
	}
	struct GBA *ga = j->core->board, *gb = c->ref_core->board;
	if (a->slog_n > JIT_LOG_MAX) { // stores past the log: compare the RAM they most likely hit
		for (int k = 0; k < 2; ++k) {
			const uint8_t* pa = (const uint8_t*) (k ? ga->memory.iwram : ga->memory.wram);
			const uint8_t* pb = (const uint8_t*) (k ? gb->memory.iwram : gb->memory.wram);
			uint32_t size = k ? GBA_SIZE_IWRAM : GBA_SIZE_EWRAM;
			if (memcmp(pa, pb, size)) {
				uint32_t i = 0;
				while (pa[i] == pb[i]) {
					++i;
				}
				snprintf(what, what_size, "after %u stores, %s differs at %08x: jit %02x, interp %02x", a->slog_n,
				         k ? "IWRAM" : "EWRAM", (k ? 0x03000000 : 0x02000000) + i, pa[i], pb[i]);
				return false;
			}
		}
	}
	for (uint32_t i = 0; i < n; ++i) {
		uint32_t la, lb, address = a->slog[2 * i], size = a->slog[2 * i + 1];
		const uint8_t* pa = memory_at(j->core->board, address, &la);
		const uint8_t* pb = memory_at(c->ref_core->board, address, &lb);
		size = size < la ? size : la;
		if (pa && memcmp(pa, pb, size)) {
			uint32_t k = 0;
			while (pa[k] == pb[k]) {
				++k;
			}
			snprintf(what, what_size, "stored bytes differ at %08x: jit %02x, interp %02x", address + k, pa[k], pb[k]);
			return false;
		}
	}
	a->slog_n = b->slog_n = 0;
	return true;
}

static bool compare(struct jit* j, const char* what) {
	struct jit_check* c = j->check;
	if (!same_cpu(j->cpu, c->ref_core->cpu)) {
		fail(j, what);
		return false;
	}
	char why[96];
	if (!compare_stores(j, why, sizeof(why))) {
		fail(j, why);
		return false;
	}
	// ROM code's prefetch state (GBAMemoryStall): in RAM code mGBA ignores it, and leaving RAM
	// resets it, so translated RAM code doesn't keep it.
	struct GBA* ga = j->core->board;
	if (ga->memory.activeRegion >= GBA_REGION_ROM0 &&
	    ga->memory.lastPrefetchedPc != ((struct GBA*) c->ref_core->board)->memory.lastPrefetchedPc) {
		fail(j, "lastPrefetchedPc differs");
		return false;
	}
	return true;
}

void jit_check_after(struct jit* j, uint32_t instructions) {
	struct jit_check* c = j->check;
	if (c->failed) {
		return;
	}
	struct ARMCore* ref = c->ref_core->cpu;
	c->steps = 0;
	for (uint32_t i = 0; i < instructions; ++i) {
		if (ref->cycles >= ref->nextEvent) {
			fail(j, "the JIT ran an instruction after an event was due");
			return;
		}
		jit_step(ref);
		if (c->steps < 64) {
			c->trace[c->steps].pc = ref->gprs[15];
			c->trace[c->steps].lpp = ((struct GBA*) c->ref_core->board)->memory.lastPrefetchedPc;
			c->trace[c->steps++].cycles = ref->cycles;
		}
	}
	if (compare(j, "cpu state after a block")) {
		++j->stats.checked;
	}
}

static bool same_region(struct jit* j, const char* name, const void* a, const void* b, size_t size) {
	if (!memcmp(a, b, size)) {
		return true;
	}
	size_t i = 0;
	while (((const uint8_t*) a)[i] == ((const uint8_t*) b)[i]) {
		++i;
	}
	char what[96];
	snprintf(what, sizeof(what), "%s differs at offset %#x", name, (unsigned) i);
	fail(j, what);
	return false;
}

void jit_check_events(struct jit* j) {
	struct jit_check* c = j->check;
	if (c->failed) {
		return;
	}
	struct ARMCore* ref = c->ref_core->cpu;
	ref->irqh.processEvents(ref);
	if (!compare(j, "cpu state after events")) {
		return;
	}
	struct GBA *ga = j->core->board, *gb = c->ref_core->board;
	// DMA units and sound FIFOs: state that store and IO comparisons see only much later.
	size_t fifo = offsetof(struct GBAAudioFIFO, internalSample);
	if (!same_region(j, "DMA state", ga->memory.dma, gb->memory.dma, sizeof(ga->memory.dma)) ||
	    !same_region(j, "FIFO A", &ga->audio.chA, &gb->audio.chA, fifo) ||
	    !same_region(j, "FIFO B", &ga->audio.chB, &gb->audio.chB, fifo)) {
		return;
	}
	if (ga->video.frameCounter != c->frame) {
		c->frame = ga->video.frameCounter;
		mAudioBufferClear(c->ref_core->getAudioBuffer(c->ref_core)); // nobody listens to it
		if (!same_region(j, "IWRAM", ga->memory.iwram, gb->memory.iwram, GBA_SIZE_IWRAM) ||
		    !same_region(j, "EWRAM", ga->memory.wram, gb->memory.wram, GBA_SIZE_EWRAM) ||
		    !same_region(j, "IO", ga->memory.io, gb->memory.io, sizeof(ga->memory.io)) ||
		    !same_region(j, "palette", ga->video.palette, gb->video.palette, GBA_SIZE_PALETTE_RAM) ||
		    !same_region(j, "VRAM", ga->video.vram, gb->video.vram, GBA_SIZE_VRAM) ||
		    !same_region(j, "OAM", ga->video.oam.raw, gb->video.oam.raw, GBA_SIZE_OAM)) {
			return;
		}
	}
}

// ---- Profile ----

void jit_profile_start(struct jit* j) {
	j->profile = true;
	jit_reemit_memory(j); // its slow paths count too
	j->prof_frame = ((struct GBA*) j->core->board)->video.frameCounter;
	log_printf("jit profile: on\n");
}

static uint32_t view(struct jit* j, uint32_t address, bool thumb) {
	return thumb ? GBAView16(j->cpu, address) : GBAView32(j->cpu, address);
}

// Racy reads of the emulation thread's counters: fine for a dev log.
void jit_profile_dump(struct jit* j) {
	if (!j->profile) {
		return;
	}
	uint64_t thumb = 0, thumb_fallback = 0, calls = 0, arm = 0;
	enum { TOP = 24 };
	struct jit_block* top[TOP] = { 0 };
	uint64_t fallback_ops[256] = { 0 }; // Thumb fallbacks by opcode >> 8
	for (uint32_t i = 0; i < j->nblocks; ++i) {
		struct jit_block* b = &j->blocks[i];
		uint64_t n = (uint64_t) b->runs * b->ninstr;
		if (!n) {
			continue;
		}
		if (b->thumb) {
			thumb += n;
			thumb_fallback += (uint64_t) b->runs * b->nfallback;
			calls += (uint64_t) b->runs * b->ncall;
			if (b->nfallback) {
				for (uint32_t a = b->start; a < b->end; a += 2) {
					uint32_t op = view(j, a, true);
					// Same test as thumb_decode's fallbacks, roughly: count the usual suspects.
					if ((op & 0xFC00) == 0x4000 && ((op >> 6) & 0xF) >= 2 && ((op >> 6) & 0xF) <= 7) {
						fallback_ops[op >> 8] += b->runs;
					} else if ((op & 0xFFC0) == 0x4340 || (op & 0xFE00) == 0x5E00 || (op & 0xFF00) == 0xDF00) {
						fallback_ops[op >> 8] += b->runs;
					}
				}
			}
		} else {
			arm += n;
		}
		for (int k = 0; k < TOP; ++k) {
			if (!top[k] || n > (uint64_t) top[k]->runs * top[k]->ninstr) {
				memmove(&top[k + 1], &top[k], (TOP - 1 - k) * sizeof(*top));
				top[k] = b;
				break;
			}
		}
	}
	uint64_t total = thumb + arm;
	if (!total) {
		return;
	}
	log_printf("jit profile: %llu instructions: thumb %.1f%% (handlers %.1f%%, memory calls %.1f%%), arm %.1f%%\n",
	           (unsigned long long) total, 100.0 * thumb / total, 100.0 * thumb_fallback / total,
	           100.0 * calls / total, 100.0 * arm / total);
	log_printf("jit profile: thumb handler opcodes (>> 8):");
	for (int i = 0; i < 256; ++i) {
		if (fallback_ops[i] * 1000 > total) {
			log_printf(" %02x:%.1f%%", i, 100.0 * fallback_ops[i] / total);
		}
	}
	log_printf("\n");
	for (int k = 0; k < TOP && top[k]; ++k) {
		struct jit_block* b = top[k];
		log_printf("jit profile: %5.2f%% %08x %s %2u instr, %u handlers, %u calls:", 100.0 * b->runs * b->ninstr / total,
		           b->start, b->thumb ? "T" : "A", b->ninstr, b->nfallback, b->ncall);
		for (uint32_t a = b->start; a < b->end && a < b->start + 24 * (b->thumb ? 2 : 4); a += b->thumb ? 2 : 4) {
			log_printf(" %0*x", b->thumb ? 4 : 8, view(j, a, b->thumb));
		}
		log_printf("\n");
	}
	for (uint32_t i = 0; i < j->nblocks; ++i) {
		j->blocks[i].runs = 0;
	}
	// LDM/STM/PUSH/POP per GBA frame, and those through mGBA by region (L = loads, S = stores).
	uint32_t frame = ((struct GBA*) j->core->board)->video.frameCounter;
	double frames = frame != j->prof_frame ? frame - j->prof_frame : 1;
	uint32_t slow[2] = { 0, 0 };
	for (int r = 0; r < 16; ++r) {
		slow[0] += j->prof_lsm_slow[0][r];
		slow[1] += j->prof_lsm_slow[1][r];
	}
	log_printf("jit profile: per GBA frame (%.0f frames): LDM/POP %.1f, STM/PUSH %.1f; through mGBA %.1f / %.1f, by region:",
	           frames, j->prof_lsm[0] / frames, j->prof_lsm[1] / frames, slow[0] / frames, slow[1] / frames);
	for (int r = 0; r < 16; ++r) {
		if (j->prof_lsm_slow[0][r] || j->prof_lsm_slow[1][r]) {
			log_printf(" %X:L%.1f/S%.1f", r, j->prof_lsm_slow[0][r] / frames, j->prof_lsm_slow[1][r] / frames);
		}
	}
	log_printf("\n");
	log_printf("jit profile: single loads/stores through mGBA per GBA frame, by region:");
	for (int r = 0; r < 16; ++r) {
		if (j->prof_mem_slow[0][r] || j->prof_mem_slow[1][r]) {
			log_printf(" %X:L%.1f/S%.1f", r, j->prof_mem_slow[0][r] / frames, j->prof_mem_slow[1][r] / frames);
		}
	}
	log_printf("\n");
	memset(j->prof_mem_slow, 0, sizeof(j->prof_mem_slow));
	memset(j->prof_lsm, 0, sizeof(j->prof_lsm));
	memset(j->prof_lsm_slow, 0, sizeof(j->prof_lsm_slow));
	j->prof_frame = frame;
}
