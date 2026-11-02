// PPU side of the SPU renderer (see include/spu_render.h): an mVideoLogger that mGBA's proxy
// renderer writes the video stream into, and one SPU thread per player slot replaying it.
#include <malloc.h>
#include <stdio.h>
#include <string.h>

#include <ppu-lv2.h>
#include <lv2/spu.h>
#include <sys/spu.h>
#include <sys/systime.h>
#include <sys/thread.h>

#include <mgba/feature/video-logger.h>
#include <mgba/internal/gba/gba.h>

#include "emu.h"
#include "render_spu_bin.h"

enum render_mode render_mode = RENDER_PPU;

// An SPU this late is considered dead (never hang the app). On hardware it answers within
// microseconds; RPCS3 compiles SPU code on first use, in bursts of up to ~0.22 s.
#define SPU_TIMEOUT_MS 2000

static sysSpuImage image;

static struct player* owner(struct mVideoLogger* logger) {
	return containerof(logger, struct player, spu.logger);
}

static void publish(struct spu_renderer* r) {
	__asm__ volatile("lwsync" ::: "memory"); // ring contents before the new write position
	r->control->write = r->write;
}

// Waits until at most max_pending written bytes are not yet drawn by the SPU. Spins at low SMT
// priority for short waits (the SPU trails the CPU by a line or so), then yields between
// checks: at frame end a busy scene can take the SPU ms more, and with 3-4 players other
// emulation threads are waiting for this PPU hardware thread.
static bool wait_pending(struct player* p, u32 max_pending) {
	struct spu_renderer* r = &p->spu;
	u64 t0 = __builtin_ppc_get_timebase();
	u64 timeout = sysGetTimebaseFrequency() / 1000 * SPU_TIMEOUT_MS;
	u64 spin = sysGetTimebaseFrequency() / 1000000 * 20;
	__asm__ volatile("or 1,1,1");
	while (r->write - (r->read = r->status->read) > max_pending) {
		u64 waited = __builtin_ppc_get_timebase() - t0;
		if (waited > spin) {
			sysThreadYield();
		}
		if (waited > timeout) {
			__asm__ volatile("or 2,2,2");
			log_printf("spu: P%d stalled (read %u, write %u, error %u), its screen stops updating\n",
			           p->index + 1, (unsigned) r->read, (unsigned) r->write, (unsigned) r->status->error);
			r->ok = false;
			return false;
		}
	}
	__asm__ volatile("or 2,2,2");
	__asm__ volatile("lwsync" ::: "memory"); // SPU output before anything we read after this
	r->wait_ticks += __builtin_ppc_get_timebase() - t0;
	return true;
}

static bool write_data(struct mVideoLogger* logger, const void* data, size_t length) {
	struct player* p = owner(logger);
	struct spu_renderer* r = &p->spu;
	if (!r->ok) {
		return false;
	}
	if (r->write - r->read > SR_RING_SIZE - length) {
		publish(r);
		if (!wait_pending(p, SR_RING_SIZE - length)) {
			return false;
		}
	}
	u32 pos = r->write & (SR_RING_SIZE - 1);
	u32 first = length < SR_RING_SIZE - pos ? length : SR_RING_SIZE - pos;
	memcpy(r->ring + pos, data, first);
	memcpy(r->ring, (const u8*) data + first, length - first);
	r->write += length;
	// Publish at each line and frame; the packets in between (registers, palette, OAM, VRAM)
	// go out with the line that needs them.
	if (length == sizeof(struct mVideoLoggerDirtyInfo)) {
		u32 type = ((const struct mVideoLoggerDirtyInfo*) data)->type;
		if (type == DIRTY_SCANLINE || type == DIRTY_FRAME || type >= SR_OUTPUT) {
			publish(r);
		}
	}
	return true;
}

static void send(struct player* p, u32 type, u32 address) {
	struct mVideoLoggerDirtyInfo item = { type, address, 0, 0 };
	write_data(&p->spu.logger, &item, sizeof(item));
}

static bool read_data(struct mVideoLogger* logger, void* data, size_t length, bool block) {
	return false; // only the SPU reads the stream
}

static void post_event(struct mVideoLogger* logger, enum mVideoLoggerEvent event) {
	switch (event) {
	case LOGGER_EVENT_INIT:
	case LOGGER_EVENT_RESET:
		send(owner(logger), SR_RESET, 0);
		break;
	case LOGGER_EVENT_DEINIT:
		break;
	default: // pixel reads and save states need a reply from the renderer: unused here
		log_printf("spu: P%d unsupported video event %d\n", owner(logger)->index + 1, event);
		break;
	}
}

bool spu_render_init(void) {
	sysSpuInitialize(6, 0);
	if (sysSpuImageImport(&image, render_spu_bin, SPU_IMAGE_PROTECT) != 0) {
		log_printf("spu: could not import the SPU program, rendering on the PPU\n");
		return false;
	}
	for (int i = 0; i < MAX_PLAYERS; ++i) {
		struct spu_renderer* r = &players[i].spu;
		r->control = memalign(128, sizeof(*r->control));
		r->status = memalign(128, sizeof(*r->status));
		r->ring = memalign(128, SR_RING_SIZE);
		r->check = memalign(128, GBA_SCREEN_W * GBA_SCREEN_H * sizeof(mColor));
		memset(r->control, 0, sizeof(*r->control));
		memset((void*) r->status, 0, sizeof(*r->status));

		static const char name[] = "spu render";
		sysSpuThreadGroupAttribute group_attr;
		sysSpuThreadGroupAttributeInitialize(group_attr);
		sysSpuThreadGroupAttributeName(group_attr, name);
		sysSpuThreadAttribute thread_attr;
		sysSpuThreadAttributeInitialize(thread_attr);
		sysSpuThreadAttributeName(thread_attr, name);
		sysSpuThreadArgument args = {
			(u64) (uintptr_t) r->control, (u64) (uintptr_t) r->status, (u64) (uintptr_t) r->ring, 0,
		};
		s32 err = sysSpuThreadGroupCreate(&r->group, 1, 100, &group_attr);
		if (!err) err = sysSpuThreadInitialize(&r->thread, r->group, 0, &image, &thread_attr, &args);
		if (!err) err = sysSpuThreadGroupStart(r->group);
		if (err) {
			log_printf("spu: P%d SPU thread failed (%08x), rendering on the PPU\n", i + 1, (unsigned) err);
			return false;
		}
		r->ok = true;
	}
	render_mode = RENDER_SPU;
	return true;
}

void spu_render_attach(struct player* p, struct mCore* core) {
	struct spu_renderer* r = &p->spu;
	r->mode = r->ok ? render_mode : RENDER_PPU;
	if (r->mode == RENDER_PPU) {
		return;
	}
	if (r->mode == RENDER_CHECK) {
		// mGBA initializes the renderer it wraps without giving it video memory unless that
		// renderer was attached before (NULL read, main thread dead): reset once without us.
		core->reset(core);
	}
	mVideoLoggerRendererCreate(&r->logger, false);
	r->logger.block = r->mode == RENDER_SPU; // RENDER_CHECK: the PPU renderer runs as well
	r->logger.waitOnFlush = false;
	r->logger.writeData = write_data;
	r->logger.readData = read_data;
	r->logger.postEvent = post_event;
	core->videoLogger = &r->logger;
	// The SPU may still hold the previous game's video memory.
	send(p, SR_RESET, 0);
	// Like mGBA's renderer init: the first frame starts at line 126 (BIOS skipped), lines above
	// stay white.
	mColor* out = r->mode == RENDER_SPU ? p->frames[p->frame_cur] : r->check;
	for (int i = 0; i < GBA_SCREEN_W * GBA_SCREEN_H; ++i) {
		out[i] = M_COLOR_WHITE;
	}
	send(p, SR_OUTPUT, (u32) (uintptr_t) out);
	r->checked = r->diff_frames = 0;
}

void spu_render_frame_end(struct player* p) {
	struct spu_renderer* r = &p->spu;
	if (r->mode == RENDER_PPU || !r->ok) {
		return;
	}
	publish(r);
	u64 waited = r->wait_ticks;
	bool ok = wait_pending(p, 0);
	r->end_wait_ticks += r->wait_ticks - waited;
	if (!ok || r->mode == RENDER_SPU) {
		return;
	}
	// RENDER_CHECK: the PPU renderer drew the same frame into frames[frame_cur].
	const mColor* ppu = p->frames[p->frame_cur];
	++r->checked;
	if (memcmp(ppu, r->check, GBA_SCREEN_W * GBA_SCREEN_H * sizeof(mColor)) != 0) {
		if (++r->diff_frames == 1) {
			int n = 0, first = -1;
			for (int i = 0; i < GBA_SCREEN_W * GBA_SCREEN_H; ++i) {
				if (ppu[i] != r->check[i]) {
					if (first < 0) first = i;
					++n;
				}
			}
			r->diff_pixels = n;
			r->diff_x = first % GBA_SCREEN_W;
			r->diff_y = first / GBA_SCREEN_W;
			r->diff_ppu = ppu[first];
			r->diff_spu = r->check[first];
		}
	}
}

void spu_render_output(struct player* p, mColor* next) {
	struct spu_renderer* r = &p->spu;
	if (r->mode == RENDER_SPU && r->ok) {
		send(p, SR_OUTPUT, (u32) (uintptr_t) next);
	}
}

void spu_render_detach(struct player* p) {
	struct spu_renderer* r = &p->spu;
	if (r->mode != RENDER_PPU && r->ok) {
		publish(r);
		wait_pending(p, 0); // nothing in flight into this player's frames
	}
	r->mode = RENDER_PPU;
}

void spu_render_log(struct player* p, int frames) {
	struct spu_renderer* r = &p->spu;
	if (r->mode == RENDER_PPU || !frames) {
		return;
	}
	u32 busy = r->status->busy, idle = r->status->idle;
	double tb_ms = sysGetTimebaseFrequency() / 1000.0;
	printf("spu P%d: %s, drawing %.2f ms/frame (idle %.2f), stream %.1f KB/frame, CPU waited %.3f ms/frame "
	       "(%.3f at frame end)", p->index + 1, r->ok ? "ok" : "DEAD", (u32) (busy - r->busy_seen) / tb_ms / frames,
	       (u32) (idle - r->idle_seen) / tb_ms / frames, (u32) (r->write - r->write_seen) / 1024.0 / frames,
	       r->wait_ticks / tb_ms / frames, r->end_wait_ticks / tb_ms / frames);
	if (r->mode == RENDER_CHECK) {
		printf(", %d/%d frames differ", r->diff_frames, r->checked);
		if (r->diff_frames) {
			printf(" (first: %d pixels, first at %d,%d ppu %04x spu %04x)", r->diff_pixels, r->diff_x, r->diff_y,
			       r->diff_ppu, r->diff_spu);
		}
	}
	printf("\n");
	r->busy_seen = busy;
	r->idle_seen = idle;
	r->write_seen = r->write;
	r->wait_ticks = r->end_wait_ticks = 0;
}

void spu_render_shutdown(void) {
	for (int i = 0; i < MAX_PLAYERS; ++i) {
		struct spu_renderer* r = &players[i].spu;
		if (!r->group) {
			continue;
		}
		u32 cause, status;
		if (r->ok) {
			send(&players[i], SR_QUIT, 0);
			sysSpuThreadGroupJoin(r->group, &cause, &status);
		} else {
			sysSpuThreadGroupTerminate(r->group, 0);
			sysSpuThreadGroupJoin(r->group, &cause, &status);
		}
		sysSpuThreadGroupDestroy(r->group);
		r->group = 0;
	}
}
