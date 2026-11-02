// Dev sound self-check (DATA_DIR/soundcheck, see start_twin in emu.c): compares two cores of the
// same game, one making sound samples and the other skipping them
// (patches/mgba/skip-sound-samples.patch).
#ifdef DEV_TOOLS
#include <stdio.h>
#include <string.h>

#include <mgba/core/core.h>
#include <mgba/internal/gb/gb.h>
#include <mgba/internal/gba/gba.h>
#include <mgba/internal/sm83/sm83.h>

#include "emu.h"

static bool same(const char* name, const void* a, const void* b, size_t size, char* what, size_t n) {
	if (!memcmp(a, b, size)) {
		return true;
	}
	size_t i = 0;
	while (((const u8*) a)[i] == ((const u8*) b)[i]) {
		++i;
	}
	snprintf(what, n, "%s differs at offset %#x", name, (unsigned) i);
	return false;
}

// Scheduled events in order, by name and time, except the sample events (only one core has its
// own scheduled).
static bool same_events(const struct mTiming* a, const struct mTiming* b, const struct mTimingEvent* sample_a,
                        const struct mTimingEvent* sample_b, char* what, size_t n) {
	const struct mTimingEvent* ea = a->root ? a->root : a->reroot;
	const struct mTimingEvent* eb = b->root ? b->root : b->reroot;
	for (;;) {
		ea = ea == sample_a ? ea->next : ea;
		eb = eb == sample_b ? eb->next : eb;
		if (!ea || !eb) {
			break;
		}
		if (strcmp(ea->name, eb->name) || ea->when != eb->when) {
			snprintf(what, n, "events: %s at %u, %s at %u", ea->name, (unsigned) ea->when, eb->name, (unsigned) eb->when);
			return false;
		}
		ea = ea->next;
		eb = eb->next;
	}
	if (ea || eb) {
		snprintf(what, n, "events: %s left over", ea ? ea->name : eb->name);
		return false;
	}
	return true;
}

static bool same_gba(struct GBA* a, struct GBA* b, char* what, size_t n) {
	const struct ARMCore *ca = a->cpu, *cb = b->cpu;
	if (memcmp(ca->gprs, cb->gprs, sizeof(ca->gprs)) || ca->cpsr.packed != cb->cpsr.packed ||
	    ca->spsr.packed != cb->spsr.packed || ca->halted != cb->halted ||
	    ca->prefetch[0] != cb->prefetch[0] || ca->prefetch[1] != cb->prefetch[1] ||
	    memcmp(ca->bankedRegisters, cb->bankedRegisters, sizeof(ca->bankedRegisters)) ||
	    memcmp(ca->bankedSPSRs, cb->bankedSPSRs, sizeof(ca->bankedSPSRs))) {
		snprintf(what, n, "CPU: pc %08x/%08x cpsr %08x/%08x", (unsigned) ca->gprs[15], (unsigned) cb->gprs[15],
		         (unsigned) ca->cpsr.packed, (unsigned) cb->cpsr.packed);
		return false;
	}
	int32_t ta = mTimingCurrentTime(&a->timing), tb = mTimingCurrentTime(&b->timing);
	if (ta != tb) {
		snprintf(what, n, "time: %d vs %d", (int) ta, (int) tb);
		return false;
	}
	for (int i = 0; i < 4; ++i) {
		const struct GBATimer *ma = &a->timers[i], *mb = &b->timers[i];
		if (ma->reload != mb->reload || ma->lastEvent != mb->lastEvent || ma->flags != mb->flags) {
			snprintf(what, n, "timer %d", i);
			return false;
		}
	}
	if (a->bus != b->bus || a->cpuBlocked != b->cpuBlocked || a->dmaPC != b->dmaPC) {
		snprintf(what, n, "bus/halt/DMA state: bus %08x/%08x", (unsigned) a->bus, (unsigned) b->bus);
		return false;
	}
	size_t save = GBASavedataSize(&a->memory.savedata);
	return same("EWRAM", a->memory.wram, b->memory.wram, GBA_SIZE_EWRAM, what, n) &&
	       same("IWRAM", a->memory.iwram, b->memory.iwram, GBA_SIZE_IWRAM, what, n) &&
	       same("IO", a->memory.io, b->memory.io, sizeof(a->memory.io), what, n) &&
	       same("palette", a->video.palette, b->video.palette, GBA_SIZE_PALETTE_RAM, what, n) &&
	       same("VRAM", a->video.vram, b->video.vram, GBA_SIZE_VRAM, what, n) &&
	       same("OAM", a->video.oam.raw, b->video.oam.raw, GBA_SIZE_OAM, what, n) &&
	       same("DMA", a->memory.dma, b->memory.dma, sizeof(a->memory.dma), what, n) &&
	       (!save || same("save", a->memory.savedata.data, b->memory.savedata.data, save, what, n)) &&
	       same_events(&a->timing, &b->timing, &a->audio.sampleEvent, &b->audio.sampleEvent, what, n);
}

static bool same_gb(struct GB* a, struct GB* b, char* what, size_t n) {
	const struct SM83Core *ca = a->cpu, *cb = b->cpu;
	if (memcmp(&ca->regs, &cb->regs, sizeof(ca->regs)) || ca->index != cb->index ||
	    ca->executionState != cb->executionState || ca->halted != cb->halted || ca->bus != cb->bus ||
	    ca->irqPending != cb->irqPending) {
		snprintf(what, n, "CPU: pc %04x/%04x state %d/%d", ca->regs.pc, cb->regs.pc, ca->executionState, cb->executionState);
		return false;
	}
	int32_t ta = mTimingCurrentTime(&a->timing), tb = mTimingCurrentTime(&b->timing);
	if (ta != tb) {
		snprintf(what, n, "time: %d vs %d", (int) ta, (int) tb);
		return false;
	}
	if (a->memory.ime != b->memory.ime || a->memory.ie != b->memory.ie || a->timer.internalDiv != b->timer.internalDiv ||
	    a->timer.nextDiv != b->timer.nextDiv || a->timer.timaPeriod != b->timer.timaPeriod || a->doubleSpeed != b->doubleSpeed) {
		snprintf(what, n, "interrupts/timer");
		return false;
	}
	return same("WRAM", a->memory.wram, b->memory.wram, GB_SIZE_WORKING_RAM, what, n) &&
	       same("HRAM", a->memory.hram, b->memory.hram, GB_SIZE_HRAM, what, n) &&
	       same("IO", a->memory.io, b->memory.io, GB_SIZE_IO, what, n) &&
	       same("VRAM", a->video.vram, b->video.vram, GB_SIZE_VRAM, what, n) &&
	       same("OAM", a->video.oam.raw, b->video.oam.raw, GB_SIZE_OAM, what, n) &&
	       same("wave RAM", a->audio.ch3.wavedata8, b->audio.ch3.wavedata8, sizeof(a->audio.ch3.wavedata8), what, n) &&
	       (!a->sramSize || same("save", a->memory.sram, b->memory.sram, a->sramSize, what, n)) &&
	       same_events(&a->timing, &b->timing, &a->audio.sampleEvent, &b->audio.sampleEvent, what, n);
}

bool soundcheck_same_state(struct mCore* a, struct mCore* b, char* what, size_t n) {
	return a->platform(a) == mPLATFORM_GBA ? same_gba(a->board, b->board, what, n) : same_gb(a->board, b->board, what, n);
}

// The whole audio state, minus what differs by design: pointers, the output buffer, event list
// links and which core skips.
static void scrub_psg(struct GBAudio* psg) {
	psg->p = NULL;
	psg->timing = NULL;
	psg->nr52 = NULL;
	memset(&psg->buffer, 0, sizeof(psg->buffer));
	psg->frameEvent.next = psg->sampleEvent.next = NULL;
	psg->frameEvent.context = psg->sampleEvent.context = NULL;
	psg->skipSamples = false;
}

// A GB core can stop with an event due (SM83Run returns once the instruction is done; the event
// then runs first thing): if it's a sample event, the sampling core is a sample behind the one
// that just replayed it, harmlessly. Not comparable then.
static bool sample_due(const struct mTiming* timing, const struct mTimingEvent* sample) {
	return (int32_t) (sample->when - mTimingCurrentTime(timing)) <= 0 && mTimingIsScheduled(timing, sample);
}

int soundcheck_same_audio(struct mCore* a, struct mCore* b, char* what, size_t n) {
	if (a->platform(a) == mPLATFORM_GBA) {
		struct GBAAudio sa = ((struct GBA*) a->board)->audio, sb = ((struct GBA*) b->board)->audio;
		sa.p = sb.p = NULL;
		sa.sampleEvent.next = sb.sampleEvent.next = NULL;
		sa.sampleEvent.context = sb.sampleEvent.context = NULL;
		sa.skipSamples = sb.skipSamples = false;
		scrub_psg(&sa.psg);
		scrub_psg(&sb.psg);
		return same("GBA audio", &sa, &sb, sizeof(sa), what, n);
	}
	struct GB *ga = a->board, *gb = b->board;
	if (sample_due(&ga->timing, &ga->audio.sampleEvent) || sample_due(&gb->timing, &gb->audio.sampleEvent)) {
		return -1;
	}
	struct GBAudio sa = ga->audio, sb = gb->audio;
	scrub_psg(&sa);
	scrub_psg(&sb);
	return same("GB audio", &sa, &sb, sizeof(sa), what, n);
}
#endif
