// mGBA cores for each player: loading, saving, the link cable and one emulation thread per
// player, so emulators use both PPU hardware threads and the link cable can block properly.
// GB/GBC games run on mGBA's GB core with its interpreter and software renderer on the PPU (the
// JIT and the SPU renderer are GBA only) and are not linked (see docs/ps3-homebrew-notes.md).
#include <malloc.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <sys/systime.h>
#include <ppu-lv2.h>
#include <sys/cond.h>
#include <sys/mutex.h>
#include <sys/thread.h>
#include <sysutil/sysutil.h>

#include <mgba/core/log.h>
#include <mgba/gb/interface.h>
#include <mgba/gba/core.h>
#include <mgba/gba/interface.h>
#include <mgba/internal/gb/gb.h>
#include <mgba/internal/gba/gba.h>
#include <mgba/internal/gba/input.h>
#include <mgba/internal/gba/sio/lockstep.h>
#include <mgba-util/table.h>
#include <mgba-util/vfs.h>

#include "audio.h"
#include "rfu.h"
#include "emu.h"
#include "jit/jit.h"

struct player players[MAX_PLAYERS];
volatile const char* emu_where = "";
volatile int emu_where_player = -1;

// Cable layout (build with LINK_CABLES=n): 1 = one cable for everyone, 2 = two cables
// (P1+P3, P2+P4), 0 = none. Each cable is one lockstep coordinator. Games start unplugged; the
// pause menu plugs a player's GBA into its cable (emu_link_set).
#ifndef LINK_CABLES
#define LINK_CABLES 1
#endif
static struct GBASIOLockstepCoordinator link_cables[2];

static struct GBASIOLockstepCoordinator* cable_of(const struct player* p) {
	return &link_cables[LINK_CABLES == 2 ? p->index % 2 : 0];
}

// Guards the thread fields of every player (see player.h) and the frame handshake. Native lv2
// primitives: PSL1GHT's pthread condition variables neither woke waiters nor timed out.
static sys_mutex_t emu_lock;
static sys_cond_t frame_done; // signalled by emulation threads when they finish a frame

static void lock(void) {
	sysMutexLock(emu_lock, 0);
}

static void unlock(void) {
	sysMutexUnlock(emu_lock);
}

static void link_plug(struct player* p, struct mCore* core);
static void link_unplug(struct player* p);
static void rfu_plug(struct player* p, struct mCore* core);
static void rfu_unplug(struct player* p);

// ROMs are shared between players running the same game; keyed by file name so the cache
// survives the library being rescanned (or a game file being removed) mid-session.
static struct {
	char file[256];
	void* data;
	size_t size;
	int refs;
} roms[MAX_PLAYERS];

FILE* log_file;

void log_printf(const char* format, ...) {
	va_list args;
	va_start(args, format);
	vfprintf(log_file ? log_file : stdout, format, args);
	va_end(args);
}

// mGBA logs every DMA and BIOS call by default; printing those costs real frame time.
static void log_errors(struct mLogger* logger, int category, enum mLogLevel level, const char* format, va_list args) {
	if (level & (mLOG_FATAL | mLOG_ERROR)) {
		FILE* out = log_file ? log_file : stdout;
		fprintf(out, "mGBA %s: ", mLogCategoryName(category));
		vfprintf(out, format, args);
		fputc('\n', out);
	}
}

static struct mLogger logger = { .log = log_errors };

static void* read_file(const char* path, size_t* size) {
	FILE* f = fopen(path, "rb");
	if (!f) {
		return NULL;
	}
	setvbuf(f, NULL, _IONBF, 0); // read straight into data: buffered, a 16 MB ROM took 1-3 s
	fseek(f, 0, SEEK_END);
	*size = ftell(f);
	fseek(f, 0, SEEK_SET);
	void* data = malloc(*size ? *size : 1);
	if (data && fread(data, 1, *size, f) != *size) {
		free(data);
		data = NULL;
	}
	fclose(f);
	return data;
}

// Write to a temp file then rename, so a crash or power cut never leaves a half-written save.
static bool write_file_atomic(const char* path, const void* data, size_t size) {
	char tmp[600];
	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	FILE* f = fopen(tmp, "wb");
	if (!f) {
		return false;
	}
	bool ok = fwrite(data, 1, size, f) == size;
	ok = fclose(f) == 0 && ok;
	if (!ok) {
		remove(tmp);
		return false;
	}
	if (rename(tmp, path) != 0) {
		// lv2 may refuse to rename over an existing file; open_save() recovers a leftover .tmp.
		remove(path);
		return rename(tmp, path) == 0;
	}
	return true;
}

// mGBA calls sync once the game has finished writing its save; persist it then.
static bool save_sync(struct VFile* vf, void* buffer, size_t size) {
	for (int i = 0; i < MAX_PLAYERS; ++i) {
		struct player* p = &players[i];
		if (p->save_vf == vf) {
			emu_where = "save write";
			// The whole file: GB cartridges with a clock (MBC3, e.g. Pokemon Gold/Silver/Crystal)
			// keep its state after the battery RAM, which is all mGBA passes here.
			void* data = buffer;
			size_t all = vf->size(vf);
			if (all > size) {
				data = vf->map(vf, all, MAP_READ);
			} else {
				all = size;
			}
			log_printf("save: P%d writing %u bytes to %s\n", i + 1, (unsigned) all, p->save_path);
			if (data && all && !write_file_atomic(p->save_path, data, all)) {
				log_printf("save: failed to write %s\n", p->save_path);
			}
			log_printf("save: P%d done\n", i + 1);
			return p->memchunk_sync(vf, buffer, size);
		}
	}
	return true;
}

// mGBA keeps a GBA cartridge clock in a 16-byte footer after the save: the date and time the game
// last read (BCD), the control byte, then the time(0) it read them at (little-endian). On load
// their difference is the clock's offset.
static void rtc_bcd(const struct tm* t, u8 control, u8* out) {
	int v[7] = { t->tm_year - 100, t->tm_mon + 1, t->tm_mday, t->tm_wday,
	             control & 0x40 ? t->tm_hour : t->tm_hour % 12, t->tm_min, t->tm_sec }; // as gpio.c
	for (int i = 0; i < 7; ++i) {
		out[i] = v[i] / 10 % 10 << 4 | v[i] % 10;
	}
}

// Up to 0.17.8 newlib had no time zone, so the clocks ps3gba saved ran on UTC: a footer dated UTC
// at its time(0) moves to local time. Any other clock (e.g. from an emulator on a computer) keeps
// running as it was.
static void rtc_utc_to_local(u8* save, size_t size) {
	if (size % 256 != 16) { // GBA saves are 512 B-128 KB; GB clock footers 40-136 bytes
		return;
	}
	u8* footer = save + size - 16;
	time_t latch = 0;
	for (int i = 15; i >= 8; --i) {
		latch = latch << 8 | footer[i];
	}
	struct tm t;
	u8 utc[7];
	gmtime_r(&latch, &t);
	rtc_bcd(&t, footer[7], utc);
	if (memcmp(footer, utc, 7) == 0) {
		localtime_r(&latch, &t);
		rtc_bcd(&t, footer[7], footer);
	}
}

static struct VFile* open_save(struct player* p) {
	char tmp[600];
	snprintf(tmp, sizeof(tmp), "%s.tmp", p->save_path);
	size_t size = 0;
	void* data = read_file(p->save_path, &size);
	if (!data) {
		data = read_file(tmp, &size); // interrupted rename
	}
	if (data) {
		rtc_utc_to_local(data, size);
	}
	struct VFile* vf = VFileMemChunk(data, data ? size : 0);
	free(data);
	p->memchunk_sync = vf->sync;
	vf->sync = save_sync;
	p->save_vf = vf;
	return vf;
}

// mGBA's lockstep calls these from inside runLoop. A sleeping player's thread blocks after
// runLoop returns (mGBA zeroes nextEvent so it returns promptly) until another player's thread
// wakes it; mGBA expects exactly this from a thread per GBA.
static void link_sleep(struct mLockstepUser* user) {
	struct player* p = containerof(user, struct player, link_user);
	lock();
	++p->link_sleeps;
	p->asleep = true;
	sysCondBroadcast(frame_done); // the main loop doesn't wait for a sleeping player
	unlock();
}

static void link_wake(struct mLockstepUser* user) {
	struct player* p = containerof(user, struct player, link_user);
	lock();
	p->asleep = false;
	sysCondBroadcast(p->wake);
	unlock();
}

// Plugging order = link port: the first GBA plugged in is the parent (id 0), the others keep
// their IDs and a newcomer comes last. mGBA tells nobody when an ID changes: by screen slot, a
// game plugged in below running ones shifted them up while the parent waited for an ack under
// the old ID, and every GBA on the cable slept forever (docs/ps3-homebrew-notes.md). mGBA calls
// this under the cable's mutex.
static int link_requested_id(struct mLockstepUser* user) {
	struct player* p = containerof(user, struct player, link_user);
	const struct GBASIOLockstepPlayer* lp = TableLookup(&cable_of(p)->players, p->link.lockstepId);
	return lp && lp->playerId >= 0 ? lp->playerId : MAX_GBAS - 1;
}

// Linked players may finish one frame ahead: whoever another player waits on must be able to
// keep running to the next sync point.
static int frame_lead(const struct player* p) {
	return p->link_attached ? 1 : 0;
}

// Times mGBA's renderer (called once per scanline) to split a frame's cost between the ARM
// core and the renderer (with the SPU renderer: just queueing the line for the SPU). mftb is
// cheap; sysGetSystemTime is a syscall.
static void timed_draw_scanline(struct GBAVideoRenderer* renderer, int y) {
	for (int i = 0; i < MAX_PLAYERS; ++i) {
		struct player* p = &players[i];
		if (p->draw_scanline && p->core && ((struct GBA*) p->core->board)->video.renderer == renderer) { // GBA only
			u64 t0 = __builtin_ppc_get_timebase();
			p->draw_scanline(renderer, y);
			p->render_ticks += __builtin_ppc_get_timebase() - t0;
			return;
		}
	}
}

// lv2 schedules strictly by priority (lower number = higher): emulator threads run below the
// main loop (1001) so a spinning emulator can never starve the UI, input or audio. The heard
// game and the games on its cable (p->favored) run one step above the others: when the PPU is
// short, the other games slow down first (see docs/ps3-homebrew-notes.md, Threads).
#define EMU_THREAD_PRIORITY 1200
#define FAVORED_THREAD_PRIORITY 1199

// A game nobody hears skips its sample events (patches/mgba/skip-sound-samples.patch): the game
// can't tell, and the CPU (the JIT too) runs longer between events. Called every frame: while
// skipping, that also runs the skipped samples' effects so far, a frame's worth at most.
static void skip_samples(struct mCore* core, bool skip) {
	if (core->platform(core) == mPLATFORM_GBA) {
		GBAAudioSkipSamples(&((struct GBA*) core->board)->audio, skip);
	} else {
		GBAudioSkipSamples(&((struct GB*) core->board)->audio, skip);
	}
}

#ifdef DEV_TOOLS
volatile bool emu_sample_all;
static void twin_begin(struct player* p, bool skip, bool toggled);
static void twin_frame(struct player* p);
#endif

static struct mCore* load_game(struct player* p);

// What lv2 still has for us (syscall 352: total and available user memory) and newlib's heap:
// a failed allocation (the JIT's ~3-4 MB per player) used to fail silently.
static s32 user_memory(u32 info[2]) {
	lv2syscall1(352, (u64) (uintptr_t) info);
	return_to_user_prog(s32);
}

static u32 free_kb(void) {
	u32 info[2] = { 0, 0 };
	user_memory(info);
	return info[1] / 1024;
}

void log_memory(const char* when) {
	u32 info[2] = { 0, 0 };
	user_memory(info);
	struct mallinfo mi = mallinfo();
	log_printf("mem: %s: lv2 %u of %u KB free, heap %u KB (%u KB free inside)\n", when, info[1] / 1024,
	           info[0] / 1024, (unsigned) mi.arena / 1024, (unsigned) mi.fordblks / 1024);
}

static void player_thread(void* context) {
	struct player* p = context;
	struct mCore* core = load_game(p);
	sys_ppu_thread_t self;
	sysThreadGetId(&self);
	int priority = EMU_THREAD_PRIORITY;
	int aimed = 0; // frames[] buffer the core and the SPU draw into (load_game starts at 0)
	lock();
	p->core = core;
	p->load_status = core ? LOAD_OK : LOAD_FAILED;
	while (core) {
		u64 w0 = __builtin_ppc_get_timebase();
		bool linked = p->asleep;
		while (!p->quit && p->link_wanted == p->link_attached && p->rfu_wanted == p->rfu_attached &&
		       (p->asleep || p->frames_done >= p->frames_wanted + frame_lead(p))) {
			sysCondWait(p->wake, 0);
		}
		if (linked) {
			p->link_wait_ticks += __builtin_ppc_get_timebase() - w0;
		}
		if (p->quit) {
			break;
		}
		if (p->link_wanted != p->link_attached || p->rfu_wanted != p->rfu_attached) {
			// Here, between two slices, our core doesn't run (mGBA's frontend interrupts a core's
			// thread the same way); asleep or not, mGBA handles it. The other cores keep running:
			// the cable's mutex orders us with them. One port: what goes out goes first.
			bool link = p->link_wanted, rfu = p->rfu_wanted;
			unlock();
			if (!link) {
				link_unplug(p);
			}
			if (!rfu) {
				rfu_unplug(p);
			}
			if (link && !p->link_attached) {
				link_plug(p, core);
			}
			if (rfu) {
				rfu_plug(p, core);
			}
			lock();
			continue;
		}
		p->core->setKeys(p->core, p->input);
		bool heard = p->heard;
		int wanted = p->favored ? FAVORED_THREAD_PRIORITY : EMU_THREAD_PRIORITY;
		unlock();
		if (aimed != p->frame_cur) {
			aimed = p->frame_cur;
			p->core->setVideoBuffer(p->core, p->frames[aimed], GBA_SCREEN_W);
			spu_render_output(p, p->frames[aimed]);
		}
		if (wanted != priority) { // once per change (a failure is logged, not retried)
			s32 now = -1;
			sysThreadSetPriority(self, wanted);
			sysThreadGetPriority(self, &now);
			log_printf("emu: P%d thread priority %d (wanted %d)\n", p->index + 1, (int) now, wanted);
			priority = wanted;
		}
		// Between run loops, where the JIT self-check's reference core is too. A linked game can
		// get here mid-frame (after a lockstep sleep): switching is exact at any point.
		bool skip = !heard;
#ifdef DEV_TOOLS
		skip &= !emu_sample_all;
		bool toggled = false;
		if (p->soundcheck) { // dev self-check: skipping toggles at random frame counts
			if (--p->soundcheck_left <= 0) {
				p->soundcheck_skip ^= 1;
				p->soundcheck_left = 1 + (p->core->frameCounter(p->core) * 7919u) % 29;
				toggled = true;
			}
			skip = p->soundcheck_skip;
		}
#endif
		skip_samples(p->core, skip);
		if (p->ref) {
			skip_samples(p->ref, skip);
		}
#ifdef DEV_TOOLS
		if (p->twin) {
			twin_begin(p, skip, toggled);
		}
#endif

		// One frame, in slices so a lockstep sleep is noticed between them.
		u32 start = p->core->frameCounter(p->core);
		s64 t0 = sysGetSystemTime();
		while (p->core->frameCounter(p->core) == start && !p->asleep && !p->quit) {
			if (p->jit) {
				jit_run_loop(p->jit);
			} else {
				p->core->runLoop(p->core);
			}
		}
		s64 spent = sysGetSystemTime() - t0;
		bool finished = p->core->frameCounter(p->core) != start;
		if (finished) {
			spu_render_frame_end(p);
			if (heard) {
				audio_update(p->core);
			} else {
				mAudioBufferClear(p->core->getAudioBuffer(p->core)); // what it made while heard
			}
#ifdef DEV_TOOLS
			if (p->twin) {
				twin_frame(p);
			}
#endif
		}

		lock();
		p->time_sum += spent; // this frame so far, possibly over several slices
		if (finished) {
			// Hand the finished frame over; the next one goes into the buffer the main loop
			// is neither showing nor about to show (triple buffering: neither side waits).
			p->frame_ready = p->frame_cur;
			p->frame_cur = 0;
			while (p->frame_cur == p->frame_ready || p->frame_cur == p->frame_shown) {
				++p->frame_cur;
			}
			++p->frames_done;
			p->time_acc += p->time_sum;
			if (p->time_sum > p->time_max) p->time_max = p->time_sum;
			p->time_sum = 0;
			if (++p->time_frames == 60) { // refresh the readout once per second
				s64 now = sysGetSystemTime();
				p->shown_fps = p->fps_since ? 60e6f / (now - p->fps_since) : 0;
				p->fps_since = now;
				p->shown_avg = p->time_acc / 60;
				p->shown_max = p->time_max;
				p->shown_render = p->render_ticks * 1000000 / sysGetTimebaseFrequency() / 60;
				p->render_ticks = 0;
				p->time_acc = p->time_max = 0;
				p->time_frames = 0;
			}
			sysCondBroadcast(frame_done);
		}
	}
	unlock();
	link_unplug(p); // at once: peers on the cable would wait for this stopped core
	rfu_unplug(p);  // and its clients or host lose it
	sysThreadExit(0);
}

void emu_init(void) {
	mLogSetDefaultLogger(&logger);
	// The PS3's clock runs on UTC and newlib knows no time zone: give it the console's, so GBA
	// cartridge clocks (mGBA's localtime_r) read local time.
	s32 zone = 0, summer = 0; // minutes east of UTC; the XMB's daylight saving time switch
	sysUtilGetSystemParamInt(SYSUTIL_SYSTEMPARAM_ID_TIMEZONE, &zone);
	sysUtilGetSystemParamInt(SYSUTIL_SYSTEMPARAM_ID_SUMMERTIME, &summer);
	int east = zone + (summer ? 60 : 0);
	char tz[32];
	snprintf(tz, sizeof(tz), "LOC%c%d:%02d", east < 0 ? '+' : '-', abs(east) / 60, abs(east) % 60); // POSIX: west
	setenv("TZ", tz, 1);
	tzset();
	time_t now = time(0);
	struct tm t;
	localtime_r(&now, &t);
	log_printf("emu: time(0) %lld, time zone %d min, summer time %d, TZ=%s: game clock %d-%02d-%02d %02d:%02d:%02d\n",
	           (long long) now, (int) zone, (int) summer, tz, t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour,
	           t.tm_min, t.tm_sec);
	GBASIOLockstepCoordinatorInit(&link_cables[0]);
	GBASIOLockstepCoordinatorInit(&link_cables[1]);
	rfu_init();
	sys_mutex_attr_t mutex_attr;
	sysMutexAttrInitialize(mutex_attr);
	sysMutexCreate(&emu_lock, &mutex_attr);
	sys_cond_attr_t cond_attr;
	sysCondAttrInitialize(cond_attr);
	sysCondCreate(&frame_done, emu_lock, &cond_attr);
	for (int i = 0; i < MAX_PLAYERS; ++i) {
		struct player* p = &players[i];
		p->index = i;
		// Aligned for the SPU's DMA.
		for (int f = 0; f < 3; ++f) {
			p->frames[f] = memalign(128, GBA_SCREEN_W * GBA_SCREEN_H * sizeof(mColor));
		}
		gfx_tex_create(&p->screen[0], GBA_SCREEN_W, GBA_SCREEN_H, GFX_RGB565);
		gfx_tex_create(&p->screen[1], GBA_SCREEN_W, GBA_SCREEN_H, GFX_RGB565);
		sysCondCreate(&p->wake, emu_lock, &cond_attr);
	}
	spu_render_init();
}

// The JIT is on unless DATA_DIR/nojit exists (every game on mGBA's interpreter) or the game's
// file is listed in DATA_DIR/nojit_games.txt (one per line, as named in the roms folder).
static bool jit_wanted(const char* game_file) {
	FILE* f = fopen(DATA_DIR "/nojit", "r");
	if (f) {
		fclose(f);
		return false;
	}
	f = fopen(DATA_DIR "/nojit_games.txt", "r");
	if (!f) {
		return true;
	}
	char line[300];
	bool listed = false;
	while (fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\r\n")] = 0;
		listed |= strcmp(line, game_file) == 0;
	}
	fclose(f);
	return !listed;
}

#ifdef DEV_TOOLS
// The self-check's reference core: loaded and reset exactly like the player's core (same ROM,
// same save contents, a renderer too since it masks some video registers), never on the cable.
static struct mCore* start_ref_core(struct player* p) {
	struct mCore* core = GBACoreCreate();
	core->init(core);
	mCoreInitConfig(core, NULL);
	core->loadROM(core, VFileFromConstMemory(roms[p->rom].data, roms[p->rom].size));
	size_t size = 0;
	void* data = read_file(p->save_path, &size);
	if (data) {
		rtc_utc_to_local(data, size); // as the player's save
	}
	core->loadSave(core, VFileMemChunk(data, data ? size : 0));
	free(data);
	p->ref_frame = calloc(GBA_SCREEN_W * GBA_SCREEN_H, sizeof(mColor));
	core->setVideoBuffer(core, p->ref_frame, GBA_SCREEN_W);
	core->reset(core);
	return core;
}
#endif

// The player's game, before its save and reset.
static struct mCore* create_core(const struct player* p) {
	struct VFile* rom = VFileFromConstMemory(roms[p->rom].data, roms[p->rom].size);
	struct mCore* core = mCoreFindVF(rom); // GBA or GB/GBC, from the ROM header
	if (!core) {
		core = GBACoreCreate(); // as before: GBA homebrew with an incomplete header
	}
	core->init(core);
	mCoreInitConfig(core, NULL);
	core->loadROM(core, rom);
	if (core->platform(core) != mPLATFORM_GBA) {
		struct GB* gb = core->board;
		// Original Game Boy games run on a Game Boy Color, which colors them like the real one
		// (mGBA's table of its palettes). Super Game Boy games keep mGBA's default, the SGB: their
		// own palettes, per screen. GBC games run in color.
		if (GBValidModels(gb->memory.rom) == GB_MODEL_MGB) {
			mCoreConfigSetValue(&core->config, "gb.model", "CGB");
			mCoreConfigSetIntValue(&core->config, "gb.colors", GB_COLORS_CGB);
		}
		gb->video.sgbBorders = false; // 160x144: the border would overflow the 240x160 frames
	}
	return core;
}

#ifdef DEV_TOOLS
// Sound self-check (DATA_DIR/soundcheck, docs/performance-plan.md): a twin of the player's core,
// started the same way and run on mGBA's interpreter right after each of its frames, always with
// the opposite sample setting. Every frame all the state a game can observe is compared; at each
// toggle (when one of the two catches up on the samples it skipped) the whole audio state too.
#define SOUNDCHECK_EPOCH 1767225600000LL // 2026-01-01, in ms: game clocks run on emulated time

static struct mCore* start_twin(struct player* p) {
	struct mCore* core = create_core(p);
	core->rtc.override = RTC_FAKE_EPOCH;
	core->rtc.value = SOUNDCHECK_EPOCH;
	size_t size = 0;
	void* data = read_file(p->save_path, &size);
	if (data) {
		rtc_utc_to_local(data, size); // as the player's save
	}
	core->loadSave(core, VFileMemChunk(data, data ? size : 0));
	free(data);
	p->twin_frame = calloc(GBA_SCREEN_W * GBA_SCREEN_H, sizeof(mColor));
	core->setVideoBuffer(core, p->twin_frame, GBA_SCREEN_W);
	core->reset(core);
	return core;
}

static void twin_check(struct player* p, int kind, bool same, const char* what) {
	if (!same && !(p->twin_failed & kind)) {
		p->twin_failed |= kind;
		log_printf("soundcheck P%d: MISMATCH (%s) at frame %u, after %d toggles\n", p->index + 1, what,
		           p->core->frameCounter(p->core), p->twin_toggles);
	}
}

// At the start of a frame, both cores at the same point.
static void twin_begin(struct player* p, bool skip, bool toggled) {
	skip_samples(p->twin, !skip);
	if (toggled) {
		char what[128];
		int same = soundcheck_same_audio(p->core, p->twin, what, sizeof(what));
		twin_check(p, 2, same != 0, what);
		p->twin_toggles += same >= 0;
	}
}

static void twin_frame(struct player* p) {
	struct mCore* twin = p->twin;
	twin->setKeys(twin, p->core->getKeys(p->core));
	u32 start = twin->frameCounter(twin);
	while (twin->frameCounter(twin) == start) {
		twin->runLoop(twin);
	}
	mAudioBufferClear(twin->getAudioBuffer(twin));
	char what[128];
	twin_check(p, 1, soundcheck_same_state(p->core, twin, what, sizeof(what)), what);
	if (++p->twin_frames % 600 == 0) {
		log_printf("soundcheck P%d: %d frames, %d toggles compared, game state %s, audio state %s\n", p->index + 1,
		           p->twin_frames, p->twin_toggles, p->twin_failed & 1 ? "MISMATCH" : "identical",
		           p->twin_failed & 2 ? "MISMATCH" : "identical");
	}
}
#endif

// The slow part of starting a game (reading a 16 MB ROM takes ~1 s, creating the core and
// loading the save ~0.1 s), run on the player's own thread so the main loop and the other
// players' games never stall. Returns the core, or NULL if the ROM could not be read.
static struct mCore* load_game(struct player* p) {
	s64 t[6];
	u32 kb[6]; // free memory along the way
	t[0] = sysGetSystemTime();
	kb[0] = free_kb();
	// ROM cache. An entry with refs but no data is still being read by another thread: two
	// players starting the same game at once each read their own copy.
	bool read = false;
	lock();
	p->rom = -1;
	for (int i = 0; i < MAX_PLAYERS && p->rom < 0; ++i) {
		if (roms[i].data && strcmp(roms[i].file, p->game_file) == 0) {
			++roms[i].refs;
			p->rom = i;
		}
	}
	for (int i = 0; i < MAX_PLAYERS && p->rom < 0; ++i) {
		if (!roms[i].refs) {
			snprintf(roms[i].file, sizeof(roms[i].file), "%s", p->game_file);
			roms[i].refs = 1;
			p->rom = i;
			read = true;
		}
	}
	unlock();
	if (read) {
		char path[512];
		library_rom_path(p->game_file, path, sizeof(path));
		size_t size = 0;
		void* data = read_file(path, &size);
		lock();
		roms[p->rom].data = data;
		roms[p->rom].size = size;
		if (!data) {
			roms[p->rom].refs = 0;
		}
		unlock();
		if (!data) {
			log_printf("emu: P%d could not read %s\n", p->index + 1, path);
			return NULL;
		}
	}
	t[1] = sysGetSystemTime();
	kb[1] = free_kb();
	library_save_path(p->game_file, p->save_name, p->save_path, sizeof(p->save_path));

	struct mCore* core = create_core(p);
	bool gba = core->platform(core) == mPLATFORM_GBA;
	core->loadSave(core, open_save(p));
#ifdef DEV_TOOLS
	FILE* dev_file = fopen(DATA_DIR "/soundcheck", "r");
	p->soundcheck = dev_file;
	p->soundcheck_left = 0;
	p->soundcheck_skip = false;
	if (dev_file) {
		fclose(dev_file);
		core->rtc.override = RTC_FAKE_EPOCH; // as the twin's
		core->rtc.value = SOUNDCHECK_EPOCH;
	}
#endif
	t[2] = sysGetSystemTime();
	kb[2] = free_kb();
	// Must be set before reset: mGBA only attaches its renderer if a buffer already exists.
	for (int f = 0; f < 3; ++f) {
		memset(p->frames[f], 0, GBA_SCREEN_W * GBA_SCREEN_H * sizeof(mColor));
	}
	p->frame_cur = 0; // emulation thread only; the SPU renderer aims there too
	core->setVideoBuffer(core, p->frames[0], GBA_SCREEN_W);
	if (gba) {
		spu_render_attach(p, core); // the SPU program is mGBA's GBA renderer
	}
	t[3] = sysGetSystemTime();
	kb[3] = free_kb();
	core->reset(core);
	t[4] = sysGetSystemTime();
	kb[4] = free_kb();
	p->draw_scanline = NULL;
	p->jit = NULL;
	if (gba) {
		struct GBAVideoRenderer* renderer = ((struct GBA*) core->board)->video.renderer;
		p->draw_scanline = renderer->drawScanline;
		renderer->drawScanline = timed_draw_scanline;
		p->jit = jit_wanted(p->game_file) ? jit_create(core, p->index) : NULL;
	}
	t[5] = sysGetSystemTime();
	kb[5] = free_kb();
	log_printf("emu: P%d %s, loaded in %d ms (rom %d, core+save %d, renderer %d, reset %d, jit %d), "
	           "took %d KB (%d, %d, %d, %d, %d), %u KB left\n",
	           p->index + 1, p->jit ? "JIT" : gba ? "interpreter" : GBModelToName(((struct GB*) core->board)->model),
	           (int) (t[5] - t[0]) / 1000, (int) (t[1] - t[0]) / 1000,
	           (int) (t[2] - t[1]) / 1000, (int) (t[3] - t[2]) / 1000, (int) (t[4] - t[3]) / 1000, (int) (t[5] - t[4]) / 1000,
	           (int) (kb[0] - kb[5]), (int) (kb[0] - kb[1]), (int) (kb[1] - kb[2]), (int) (kb[2] - kb[3]),
	           (int) (kb[3] - kb[4]), (int) (kb[4] - kb[5]), kb[5]);
#ifdef DEV_TOOLS
	// Self-check (docs/jit.md): DATA_DIR/jitcheck runs an interpreted twin in lockstep.
	FILE* profile = p->jit ? fopen(DATA_DIR "/jitprof", "r") : NULL;
	if (profile) {
		fclose(profile);
		jit_profile_start(p->jit);
	}
	// It holds "chain" to check chained blocks too (coarser: compared at each return to C).
	FILE* check = p->jit ? fopen(DATA_DIR "/jitcheck", "r") : NULL;
	if (check) {
		char mode[16] = "";
		fgets(mode, sizeof(mode), check);
		fclose(check);
		p->ref = start_ref_core(p);
		if (!p->ref || !jit_attach_check(p->jit, p->ref, strncmp(mode, "chain", 5) == 0)) {
			log_printf("jit check: could not start the reference core\n");
		}
	}
	// With the JIT self-check its reference core follows the toggles instead (no twin), on the
	// same game clock (Unbound reads its cartridge's).
	if (p->soundcheck && p->ref) {
		p->ref->rtc.override = RTC_FAKE_EPOCH;
		p->ref->rtc.value = SOUNDCHECK_EPOCH;
		log_printf("soundcheck P%d: on, the JIT check's reference core follows\n", p->index + 1);
	}
	if (p->soundcheck && !p->ref) {
		p->twin = start_twin(p);
		p->twin_frames = p->twin_toggles = 0;
		p->twin_failed = 0;
		log_printf("soundcheck P%d: on\n", p->index + 1);
	}
#endif

	lock();
	p->frame_ready = p->frame_shown = 1;
	p->asleep = false;
	p->link_attached = false;
	p->rfu_attached = false;
	p->active = false;
	p->heard = false;
	p->frames_wanted = p->frames_done = 0;
	p->time_sum = p->time_acc = p->time_max = p->shown_avg = p->shown_max = 0;
	p->fps_since = 0;
	p->shown_fps = 0;
	p->time_frames = 0;
	p->render_ticks = p->shown_render = 0;
	unlock();
	return core; // unplugged: the cable's lockstep costs frames even when nobody trades
}

bool emu_start(struct player* p) {
	p->quit = false;
	p->link_wanted = false;
	p->rfu_wanted = false;
	p->load_status = LOAD_PENDING;
	char name[32];
	snprintf(name, sizeof(name), "emu P%d", p->index + 1);
	if (sysThreadCreate(&p->thread, player_thread, p, EMU_THREAD_PRIORITY, 256 * 1024, THREAD_JOINABLE, name) != 0) {
		return false;
	}
	p->started = true;
	return true;
}

enum load_status emu_load_status(struct player* p) {
	lock();
	enum load_status status = p->load_status;
	unlock();
	return status;
}

void emu_stop(struct player* p) {
	if (!p->started) {
		return;
	}
	lock();
	p->quit = true;
	sysCondBroadcast(p->wake);
	unlock();
	u64 exit_code;
	sysThreadJoin(p->thread, &exit_code); // a game still loading finishes loading first
	p->started = false;
	if (!p->core) {
		return; // the ROM could not be read: nothing to save or free
	}

	// Flush whatever the game wrote since the last settled save.
	void* sram;
	size_t size = p->core->savedataClone(p->core, &sram);
	if (size) {
		write_file_atomic(p->save_path, sram, size);
		free(sram);
	}
	jit_destroy(p->jit);
	p->jit = NULL;
	if (p->ref) {
		p->ref->deinit(p->ref);
		p->ref = NULL;
		free(p->ref_frame);
	}
	if (p->twin) {
		p->twin->deinit(p->twin);
		p->twin = NULL;
		free(p->twin_frame);
	}
	p->core->deinit(p->core);
	spu_render_detach(p);
	p->core = NULL;
	p->draw_scanline = NULL;
	p->save_vf = NULL;
	lock();
	if (--roms[p->rom].refs == 0) {
		free(roms[p->rom].data);
		roms[p->rom].data = NULL;
	}
	unlock();
}

// A paused game keeps running while others play: linked games must not stall, and a real
// handheld doesn't stop either. Alone, pausing freezes it.
static bool is_running(const struct player* p) {
	if (!p->core) {
		return false;
	}
	if (p->state == PS_PLAYING) {
		return true;
	}
	for (int i = 0; i < MAX_PLAYERS; ++i) {
		if (&players[i] != p && players[i].state == PS_PLAYING) {
			return true;
		}
	}
	return false;
}

bool emu_wireless_available(const struct player* p) {
	// Not while self-checking: the JIT's reference core or the sound check's twin can't be
	// linked too, so the two would differ.
	return p->core && p->core->platform(p->core) == mPLATFORM_GBA && !p->ref && !p->twin;
}

bool emu_link_available(const struct player* p) {
	return LINK_CABLES && emu_wireless_available(p);
}

void emu_link_set(struct player* p, bool on) {
	if (!emu_link_available(p)) {
		return;
	}
	lock();
	p->link_wanted = on;
	p->rfu_wanted &= !on;
	sysCondBroadcast(p->wake); // its thread plugs or unplugs, even while it has no frame to run
	unlock();
}

void emu_wireless_set(struct player* p, bool on) {
	if (!emu_wireless_available(p)) {
		return;
	}
	lock();
	p->rfu_wanted = on;
	p->link_wanted &= !on;
	sysCondBroadcast(p->wake);
	unlock();
}

// On the player's thread while its core doesn't run, WITHOUT emu_lock: mGBA calls link_sleep and
// link_wake (which take it) under the cable's mutex, so the lock order is cable, then emu_lock.
static void link_plug(struct player* p, struct mCore* core) {
	lock();
	p->asleep = false;
	unlock();
	p->link_user = (struct mLockstepUser) {
		.sleep = link_sleep,
		.wake = link_wake,
		.requestedId = link_requested_id,
	};
	GBASIOLockstepDriverCreate(&p->link, &p->link_user);
	GBASIOLockstepCoordinatorAttach(cable_of(p), &p->link);
	// mGBA tells the other GBAs a game's SIO mode only when it changes, so a game already in
	// multiplayer mode (Pokemon is, from its title screen on) was never seen as ready. Plug in with
	// the mode forgotten, then re-read it from the game's registers: that announces it.
	struct GBASIO* sio = &((struct GBA*) core->board)->sio;
	sio->mode = -1;
	core->setPeripheral(core, mPERIPH_GBA_LINK_PORT, &p->link.d);
	GBASIOWriteRCNT(sio, sio->rcnt);
	p->link_attached = true;
	log_printf("link: P%d plugged in (GBA %d of %d on its cable)\n", p->index + 1,
	           p->link.d.deviceId(&p->link.d) + 1, (int) GBASIOLockstepCoordinatorAttached(cable_of(p)));
}

static void link_unplug(struct player* p) {
	if (!p->link_attached) {
		return;
	}
	p->core->setPeripheral(p->core, mPERIPH_GBA_LINK_PORT, NULL);
	GBASIOLockstepCoordinatorDetach(cable_of(p), &p->link);
	log_printf("link: P%d unplugged\n", p->index + 1);
	lock();
	p->link_attached = false;
	p->asleep = false;
	sysCondBroadcast(p->wake);
	unlock();
}

// On the player's thread while its core doesn't run (see link_plug). The adapter only ever runs
// on this thread, inside mGBA; its hub has its own lock.
static void rfu_plug(struct player* p, struct mCore* core) {
	if (p->rfu_attached) {
		return;
	}
	rfu_create(&p->rfu, p->index);
	core->setPeripheral(core, mPERIPH_GBA_LINK_PORT, &p->rfu.d);
	lock();
	p->rfu_attached = true;
	unlock();
	log_printf("rfu: P%d plugged in its Wireless Adapter\n", p->index + 1);
}

static void rfu_unplug(struct player* p) {
	if (!p->rfu_attached) {
		return;
	}
	p->core->setPeripheral(p->core, mPERIPH_GBA_LINK_PORT, NULL);
	lock();
	p->rfu_attached = false;
	unlock();
	log_printf("rfu: P%d unplugged its Wireless Adapter\n", p->index + 1);
}

static u32 gba_keys(const struct player* p) {
	if (p->state != PS_PLAYING) {
		return 0;
	}
	u32 b = p->held, keys = 0;
	if (b & BTN_CROSS) keys |= 1 << GBA_KEY_A;
	if (b & BTN_CIRCLE) keys |= 1 << GBA_KEY_B;
	if (b & BTN_SELECT) keys |= 1 << GBA_KEY_SELECT;
	if (b & BTN_START) keys |= 1 << GBA_KEY_START;
	if (b & BTN_RIGHT) keys |= 1 << GBA_KEY_RIGHT;
	if (b & BTN_LEFT) keys |= 1 << GBA_KEY_LEFT;
	if (b & BTN_UP) keys |= 1 << GBA_KEY_UP;
	if (b & BTN_DOWN) keys |= 1 << GBA_KEY_DOWN;
	if (b & BTN_R1) keys |= 1 << GBA_KEY_R;
	if (b & BTN_L1) keys |= 1 << GBA_KEY_L;
	return keys;
}

void emu_frame_begin(void) {
	lock();
	// Player 1 is heard when playing, else the first one playing.
	struct player* heard = NULL;
	for (int i = 0; i < MAX_PLAYERS; ++i) {
		struct player* p = &players[i];
		if (!heard && is_running(p) && p->state == PS_PLAYING) {
			heard = p;
		}
		// A second's grace after a session: FireRed's Union Room goes from hosting to searching
		// and back through idle frames.
		if (p->rfu_attached && rfu_in_session(p->index)) {
			p->rfu_session = 60;
		} else if (p->rfu_session > 0) {
			--p->rfu_session;
		}
	}
	bool game = false; // a game loading or running silences the menu music
	for (int i = 0; i < MAX_PLAYERS; ++i) {
		struct player* p = &players[i];
		game |= p->started;
		p->active = is_running(p);
		p->favored = false;
		if (!p->active) {
			continue;
		}
		p->heard = p == heard;
		// Linked games run in lockstep (mGBA's hard syncs too, whatever the link mode): the heard
		// one can only keep its pace if the games on its cable keep theirs. Wireless games don't
		// wait for each other, but kept in step their exchanges stay a frame apart at most: only
		// while in a session, an adapter merely plugged in costs nothing.
		p->favored = p->heard || (heard && heard->link_attached && p->link_attached && cable_of(p) == cable_of(heard)) ||
		             (heard && heard->rfu_session && p->rfu_session);
		p->input = gba_keys(p);
		++p->frames_wanted;
		sysCondBroadcast(p->wake);
	}
	unlock();
	audio_music(!game);
	if (!heard) {
		audio_update(NULL); // keep the port fed with silence or the menu music
	}
}

void emu_frame_end(void) {
	// Wait for the heard game's frame and its cable partners' (favored), but never more than ~2
	// display frames: a stuck emulator slows its own screen down instead of freezing the whole
	// app. The others aren't waited for: when the PPU is short they fall behind on their own
	// (lower thread priority, frames released but late) while the heard game stays at 60 fps.
	emu_where = "waiting for emulators";
	s64 deadline = sysGetSystemTime() + 33000;
	lock();
	for (;;) {
		bool all = true;
		for (int i = 0; i < MAX_PLAYERS; ++i) {
			// Not one asleep on its cable: it waits for a partner, which may need our next release
			// to get there (games plugged in at different times end their frames at different
			// points of the cable's time). Waiting for it held every linked game at 30 fps.
			if (players[i].favored && !players[i].asleep && players[i].frames_done < players[i].frames_wanted) {
				all = false;
			}
		}
		s64 left = deadline - sysGetSystemTime();
		if (all || left <= 0) {
			break;
		}
		sysCondWait(frame_done, left);
	}
	// A late player keeps at most one frame queued after the one it is on: it never idles while
	// behind (cancelling the queued frame made it wait for the next release after each late
	// frame), and never builds a backlog it would then rush through. Not when linked: its cable
	// partners may only run one frame ahead of their own release (frame_lead), so a target one
	// frame further would wait on them until the main loop gave up, every frame (30 fps).
	for (int i = 0; i < MAX_PLAYERS; ++i) {
		struct player* p = &players[i];
		int most = p->frames_done + 2 - frame_lead(p);
		if (p->active && p->frames_wanted > most) {
			p->frames_wanted = most;
		}
		if (p->active) {
			p->frame_shown = p->frame_ready; // the emulation thread won't draw into it now
		}
	}
	unlock();
	emu_where = "main loop";

	for (int i = 0; i < MAX_PLAYERS; ++i) {
		struct player* p = &players[i];
		if (p->active) {
			p->screen_cur ^= 1;
			gfx_tex_upload(&p->screen[p->screen_cur], p->frames[p->frame_shown], GBA_SCREEN_W * sizeof(mColor));
		}
	}
}

void emu_link_debug(const struct player* p) {
	if (!p->link_attached) {
		return;
	}
	// Racy read without the cable's mutex: this runs when something is stuck holding it.
	const struct GBASIOLockstepCoordinator* cable = cable_of(p);
	const struct GBASIOLockstepPlayer* lp = TableLookup(&cable->players, p->link.lockstepId);
	if (lp) {
		const struct GBASIO* sio = &((struct GBA*) p->core->board)->sio;
		log_printf("link P%d: siocnt=%04x rcnt=%04x id=%d mode=%d asleep=%d time=%d queue=%d@%d | cable n=%d waiting=%x active=%d cycle=%d\n",
		       p->index + 1, sio->siocnt, sio->rcnt, lp->playerId, lp->mode, lp->asleep,
		       (int) (mTimingCurrentTime(&((struct GBA*) p->core->board)->timing) - lp->cycleOffset),
		       lp->queue ? (int) lp->queue->type : -1, lp->queue ? lp->queue->timestamp : 0,
		       cable->nAttached, (unsigned) cable->waiting, cable->transferActive, cable->cycle);
	}
}

void emu_screen_size(const struct player* p, unsigned* w, unsigned* h) {
	*w = GBA_SCREEN_W;
	*h = GBA_SCREEN_H;
	if (p->core) {
		p->core->currentVideoSize(p->core, w, h);
	}
}

bool emu_anyone_playing(void) {
	for (int i = 0; i < MAX_PLAYERS; ++i) {
		if (players[i].started) {
			return true;
		}
	}
	return false;
}

int emu_save_owner(const char* game_file, const char* save) {
	for (int i = 0; i < MAX_PLAYERS; ++i) {
		const struct player* p = &players[i];
		if (p->started && strcmp(p->game_file, game_file) == 0 && strcmp(p->save_name, save) == 0) {
			return i;
		}
	}
	return -1;
}
