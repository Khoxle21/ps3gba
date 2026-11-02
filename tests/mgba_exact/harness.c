// mGBA exactness harness, run by tools/mgba_exact.sh.
//
// Proves that our mGBA changes (patches/mgba/ and uncommitted work in third_party/mgba) emulate
// exactly like pristine mGBA: this file is linked against both builds, each runs the same game
// with the same save, clock and pseudo-random input, and prints one line per frame:
//   <frame> v=<video> m=<memory> s=<state> sa=<sound state> ne=<next event> a=<sound output>
//           t=<cycles> pc=<pc> cpsr=<cpsr> r=<r0-r15>
// m: EWRAM, IWRAM, VRAM, OAM, palette, IO and save memory. s: the whole savestate (timers, DMA,
// CPU, event times, memory...) but its sound parts, which are sa (so a sound-only change shows as
// such), and the CPU's cycles to the next event of any kind, which is ne. t: mTimingCurrentTime.
// Any byte or cycle of difference shows up in that frame's line.
//
// usage: harness <rom> <save, or - for none> <frames> <seed>
// ROM and save are read into memory; mGBA writes to its copy of the save only.
// MGBA_EXACT_SHOTS=<dir> also writes the screen every 1000 frames as <dir>/<frame>.ppm.
#include <mgba/core/core.h>
#include <mgba/gba/core.h>
#include <mgba/internal/arm/arm.h>
#include <mgba/internal/gba/gba.h>
#include <mgba/internal/gba/input.h>
#include <mgba/internal/gba/serialize.h>
#include <mgba-util/audio-buffer.h>
#include <mgba-util/vfs.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void* read_file(const char* path, size_t* size) {
	FILE* f = fopen(path, "rb");
	if (!f) {
		perror(path);
		exit(1);
	}
	fseek(f, 0, SEEK_END);
	*size = ftell(f);
	rewind(f);
	void* data = malloc(*size ? *size : 1);
	if (fread(data, 1, *size, f) != *size) {
		perror(path);
		exit(1);
	}
	fclose(f);
	return data;
}

static void no_log(struct mLogger* logger, int category, enum mLogLevel level, const char* format, va_list args) {
	(void) logger, (void) category, (void) level, (void) format, (void) args;
}

// FNV-1a, 8 bytes at a time
static uint64_t hash(uint64_t h, const void* data, size_t size) {
	const uint64_t* w = data;
	for (size_t i = 0; i < size / 8; ++i) {
		h = (h ^ w[i]) * 0x100000001B3ULL;
	}
	for (size_t i = size & ~7; i < size; ++i) {
		h = (h ^ ((const uint8_t*) data)[i]) * 0x100000001B3ULL;
	}
	return h;
}

static uint64_t rng;
static uint32_t next(void) { // xorshift64*
	rng ^= rng >> 12;
	rng ^= rng << 25;
	rng ^= rng >> 27;
	return (rng * 0x2545F4914F6CDD1DULL) >> 32;
}

// Walking, A/B in menus and dialogs, now and then Start, Select, L or R, idle stretches. One key
// at a time (never the A+B+Start+Select reset), released 1-4 frames between presses.
static uint32_t pick_keys(int* hold) {
	uint32_t r = next() % 100;
	if (r < 50) {
		*hold = 4 + next() % 37;
		return 1 << (GBA_KEY_RIGHT + next() % 4);
	}
	*hold = 2 + next() % 5;
	if (r < 70) {
		return 1 << GBA_KEY_A;
	}
	if (r < 85) {
		return 1 << GBA_KEY_B;
	}
	if (r < 90) {
		return 1 << GBA_KEY_START;
	}
	if (r < 92) {
		return 1 << GBA_KEY_SELECT;
	}
	if (r < 94) {
		return 1 << (next() & 1 ? GBA_KEY_L : GBA_KEY_R);
	}
	*hold = 2 + next() % 29;
	return 0;
}

int main(int argc, char** argv) {
	if (argc != 5) {
		fprintf(stderr, "usage: %s <rom> <save|-> <frames> <seed>\n", argv[0]);
		return 1;
	}
	int frames = atoi(argv[3]);
	rng = strtoull(argv[4], NULL, 0) * 0x9E3779B97F4A7C15ULL + 1;
	const char* shots = getenv("MGBA_EXACT_SHOTS");

	size_t rom_size, save_size = 0;
	void* rom = read_file(argv[1], &rom_size);
	void* save = strcmp(argv[2], "-") ? read_file(argv[2], &save_size) : NULL;

	static struct mLogger quiet = { .log = no_log }; // mGBA logs to stdout by default
	mLogSetDefaultLogger(&quiet);
	// Set up as source/emu.c does on the PS3.
	struct mCore* core = GBACoreCreate();
	core->init(core);
	mCoreInitConfig(core, NULL);
	core->loadROM(core, VFileFromConstMemory(rom, rom_size));
	core->loadSave(core, VFileMemChunk(save, save_size));
	static mColor video[GBA_VIDEO_HORIZONTAL_PIXELS * GBA_VIDEO_VERTICAL_PIXELS];
	core->setVideoBuffer(core, video, GBA_VIDEO_HORIZONTAL_PIXELS);
	// The RTC starts at a fixed date and follows emulated time, the same in every run.
	core->rtc.override = RTC_FAKE_EPOCH;
	core->rtc.value = 1767268800000LL; // 2026-01-01 12:00 UTC
	core->reset(core);

	struct GBA* gba = core->board;
	struct ARMCore* cpu = core->cpu;
	struct GBASerializedState* state = calloc(1, sizeof(*state));
	static int16_t audio[0x10000];
	uint32_t keys = 0;
	int hold = 0;
	for (int frame = 1; frame <= frames; ++frame) {
		if (--hold <= 0) {
			if (keys) {
				keys = 0;
				hold = 1 + next() % 4;
			} else {
				keys = pick_keys(&hold);
			}
		}
		core->setKeys(core, keys);
		core->runFrame(core);

		uint64_t h = 0xCBF29CE484222325ULL;
		uint64_t v = hash(h, video, sizeof(video));
		uint64_t m = hash(h, gba->memory.wram, GBA_SIZE_EWRAM);
		m = hash(m, gba->memory.iwram, GBA_SIZE_IWRAM);
		m = hash(m, gba->video.vram, GBA_SIZE_VRAM);
		m = hash(m, gba->video.oam.raw, GBA_SIZE_OAM);
		m = hash(m, gba->video.palette, GBA_SIZE_PALETTE_RAM);
		m = hash(m, gba->memory.io, sizeof(gba->memory.io));
		if (gba->memory.savedata.data) {
			m = hash(m, gba->memory.savedata.data, GBASavedataSize(&gba->memory.savedata));
		}
		core->saveState(core, state);
		uint64_t sa = hash(h, &state->audio, sizeof(state->audio));
		sa = hash(sa, &state->samples, sizeof(state->samples));
		sa = hash(sa, state->currentSamples, sizeof(state->currentSamples));
		memset(&state->audio, 0, sizeof(state->audio));
		memset(&state->samples, 0, sizeof(state->samples));
		memset(state->currentSamples, 0, sizeof(state->currentSamples));
		state->cpu.nextEvent = 0;
		uint64_t s = hash(h, state, sizeof(*state));
		struct mAudioBuffer* buffer = core->getAudioBuffer(core);
		size_t samples = mAudioBufferRead(buffer, audio, sizeof(audio) / sizeof(*audio) / buffer->channels);
		uint64_t a = hash(h, audio, samples * buffer->channels * sizeof(*audio));
		printf("%d v=%016llx m=%016llx s=%016llx sa=%016llx ne=%d a=%016llx t=%08x pc=%08x cpsr=%08x r=%016llx\n",
		       frame, (unsigned long long) v, (unsigned long long) m, (unsigned long long) s,
		       (unsigned long long) sa, cpu->nextEvent, (unsigned long long) a,
		       (unsigned) mTimingCurrentTime(core->timing), (unsigned) cpu->gprs[ARM_PC],
		       (unsigned) cpu->cpsr.packed, (unsigned long long) hash(h, cpu->gprs, sizeof(cpu->gprs)));

		if (shots && frame % 1000 == 0) {
			char path[4096];
			snprintf(path, sizeof(path), "%s/%06d.ppm", shots, frame);
			FILE* f = fopen(path, "wb");
			fprintf(f, "P6 %d %d 255\n", GBA_VIDEO_HORIZONTAL_PIXELS, GBA_VIDEO_VERTICAL_PIXELS);
			for (size_t i = 0; i < sizeof(video) / sizeof(*video); ++i) {
				uint16_t c = video[i]; // RGB565 (COLOR_16_BIT, COLOR_5_6_5)
				fputc((c >> 11) << 3, f);
				fputc((c >> 5 & 0x3F) << 2, f);
				fputc((c & 0x1F) << 3, f);
			}
			fclose(f);
		}
	}
	core->deinit(core);
	return 0;
}
