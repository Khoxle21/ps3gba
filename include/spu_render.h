// SPU renderer: one SPU thread per player replays mGBA's video stream (the proxy renderer's
// packets, struct mVideoLoggerDirtyInfo) with mGBA's software renderer and DMAs finished lines
// into the player's frame buffer. The PPU side is source/spu_render.c, the SPU side spu/render.c.
//
// The stream is a ring in main memory: 16-byte packets, each DIRTY_VRAM packet followed by its
// 4 KB block. The PPU publishes how far it wrote in sr_control (a store to that line wakes the
// waiting SPU through its lock-line reservation); the SPU publishes how far it got in sr_status
// once every line before that point is in memory.
#ifndef SPU_RENDER_H
#define SPU_RENDER_H

#include <stdint.h>

#define SR_RING_SIZE (256 * 1024) // bytes, a power of two

// Packet types after mGBA's enum mVideoLoggerDirtyType, same 16-byte layout.
enum {
	SR_OUTPUT = 0x100, // address: where the following lines go (240 RGB565 pixels per line)
	SR_RESET,          // mGBA reset the renderer; video memory is all zero at that point
	SR_QUIT,
};

// Written by the PPU, one 128-byte line (the SPU's reservation granule).
struct sr_control {
	uint32_t write; // bytes written to the ring so far (wraps around 2^32)
	uint32_t pad[31];
} __attribute__((aligned(128)));

// Written by the SPU (16-byte DMA).
struct sr_status {
	uint32_t read;  // bytes consumed: every line drawn before this point is in memory
	uint32_t busy;  // decrementer ticks spent drawing lines (timebase frequency), wraps
	uint32_t error; // packet type that made the SPU stop, 0 if fine
	uint32_t idle;  // decrementer ticks spent waiting for the PPU to write more, wraps
	uint32_t pad[28];
} __attribute__((aligned(128)));

#ifndef __SPU__
#include <stdbool.h>

#include <ppu-types.h>
#include <sys/spu.h>
#include <mgba/core/interface.h>
#include <mgba/feature/video-logger.h>

enum render_mode {
	RENDER_PPU,   // mGBA's software renderer on the emulation thread
	RENDER_SPU,   // on the player's SPU
	RENDER_CHECK, // both, comparing every frame (dev)
};
// For games started from now on: RENDER_SPU once spu_render_init() succeeded.
extern enum render_mode render_mode;

struct spu_renderer {
	struct mVideoLogger logger; // what mGBA's proxy renderer writes into
	enum render_mode mode;      // of the running game
	bool ok;                    // SPU thread running and keeping up
	struct sr_control* control;
	volatile struct sr_status* status;
	u8* ring;
	u32 write, read; // stream positions: ours, and the SPU's as last seen
	sys_spu_group_t group;
	sys_spu_thread_t thread;
	mColor* check; // RENDER_CHECK: the SPU's frame
	// Dev log readout
	u64 wait_ticks, end_wait_ticks; // all CPU waits, and those at frame end
	u32 busy_seen, idle_seen, write_seen;
	int checked, diff_frames, diff_pixels, diff_x, diff_y;
	mColor diff_ppu, diff_spu;
};

struct player;
// Starts one SPU thread per player slot; false (and RENDER_PPU) if the SPUs are unavailable.
bool spu_render_init(void);
// Before core->reset(): routes this game's video to its SPU according to render_mode.
struct mCore;
void spu_render_attach(struct player* p, struct mCore* core);
// After a frame: waits until the SPU has drawn it.
void spu_render_frame_end(struct player* p);
// Before the next frame: the frames[] buffer it goes into.
void spu_render_output(struct player* p, mColor* next);
// After core->deinit().
void spu_render_detach(struct player* p);
void spu_render_log(struct player* p, int frames);
void spu_render_shutdown(void);
#endif

#endif
