// ARM/Thumb -> PowerPC JIT for mGBA's GBA core (design: docs/jit.md).
#ifndef JIT_H
#define JIT_H

#include <stdbool.h>
#include <stdint.h>

struct mCore;
struct jit;

struct jit_stats {
	uint32_t blocks;        // live translated blocks
	uint32_t translated;    // blocks translated since the last reset_stats
	uint64_t translate_ticks; // timebase ticks spent translating them
	uint32_t invalidated;   // blocks killed by writes to their code
	uint32_t revived;       // dead blocks reused because their code was written back
	uint32_t flushes;       // whole-cache flushes (cache full or waitstates changed)
	uint32_t cache_used;    // bytes of the player's code cache in use
	uint32_t interpreted;   // instructions run by the interpreter (near events, untranslatable)
	uint32_t resumed;       // dispatches entering a RAM block mid-way (after an event)
	uint32_t checked;       // self-check: blocks compared with the reference interpreter
	uint32_t idle_skipped;  // instructions of idle loop iterations skipped
	uint32_t dispatches;    // returns from translated code to the C dispatcher
	uint32_t writepc;       // of which: branches needing mGBA's WritePC (indirect, BX...)
	uint32_t run_loops;     // jit_run_loop calls
	// TIMES=1 builds: timebase ticks (wall time: includes other threads running meanwhile) in
	// jit_run_loop, and of those in translated code (with the mGBA memory calls it makes), the
	// interpreter and mGBA's event handlers (video, timers, audio, DMA, IRQs).
	uint64_t loop_ticks, code_ticks, interp_ticks, event_ticks;
	bool in_jit;            // translated code is running right now
};

// Translates the core's code into code cache slice `slot` (one per player). NULL if the JIT
// can't run this core (unexpected memory handlers).
struct jit* jit_create(struct mCore* core, int slot);
void jit_destroy(struct jit* j);
// Drop-in for core->runLoop (mGBA's ARMRunLoop): runs until the next event, then processes it.
void jit_run_loop(struct jit* j);
void jit_get_stats(struct jit* j, struct jit_stats* out, bool reset);

// Dev builds: run a second, interpreted core in lockstep and compare after every block (or,
// with `chain`, every return to the dispatcher, so chained blocks are checked too). `ref` must
// have been loaded and reset exactly like the JIT core. The first difference is logged
// (jit_check_report), then checking stops. Call before the core runs.
bool jit_attach_check(struct jit* j, struct mCore* ref, bool chain);
const char* jit_check_report(struct jit* j);

// Dev builds: count how often each block runs (call before the core runs), and log where the
// GBA instructions ran since the last dump: native, mGBA handlers, ARM, hottest blocks.
void jit_profile_start(struct jit* j);
void jit_profile_dump(struct jit* j);

#endif
