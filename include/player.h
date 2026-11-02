#ifndef PLAYER_H
#define PLAYER_H

#include <stdbool.h>

#include <mgba/core/core.h>
#include <mgba/internal/gba/sio/lockstep.h>
#include <ppu-lv2.h>
#include <sys/cond.h>
#include <sys/thread.h>

#include "gfx.h"
#include "library.h"
#include "rfu.h"
#include "rompatch.h"
#include "spu_render.h"

#define MAX_PLAYERS 4
#define GBA_SCREEN_W 240
#define GBA_SCREEN_H 160

enum player_state {
	PS_EMPTY,   // no one in this slot (join screen)
	PS_GAMES,   // picking a game
	PS_SAVES,   // picking a save for the game
	PS_PLAYING,
	PS_PAUSED,
	PS_LOADING, // the game loads on its emulation thread (saves list still shown)
	PS_SAVE_MENU,   // options for the selected save (rename, copy, delete)
	PS_SAVE_RENAME, // on-screen keyboard
	PS_SAVE_DELETE, // delete confirmation
	PS_SAVE_BUSY,   // the library thread renames, copies or deletes it
	PS_PHONE,   // QR code of the player's phone page (web.c)
	// ROM patching (rompatch.c), from Square in the carousel: the patches for the selected game,
	// "create it?", building it (or deleting after it), the new game's cleanup question, an error.
	PS_PATCHES,
	PS_PATCH_CONFIRM,
	PS_PATCH_BUSY,
	PS_PATCH_DONE,
	PS_PATCH_ERROR,
};

enum load_status { LOAD_PENDING, LOAD_OK, LOAD_FAILED };

// PS3 buttons, our own bit layout.
enum {
	BTN_UP = 1 << 0, BTN_DOWN = 1 << 1, BTN_LEFT = 1 << 2, BTN_RIGHT = 1 << 3,
	BTN_CROSS = 1 << 4, BTN_CIRCLE = 1 << 5, BTN_SQUARE = 1 << 6, BTN_TRIANGLE = 1 << 7,
	BTN_L1 = 1 << 8, BTN_R1 = 1 << 9, BTN_L2 = 1 << 10, BTN_R2 = 1 << 11,
	BTN_L3 = 1 << 12, BTN_R3 = 1 << 13, BTN_START = 1 << 14, BTN_SELECT = 1 << 15,
};

struct player {
	int index;
	enum player_state state;

	// Input
	bool connected;
	u32 pad_held;   // as last reported by the controller
	u32 held;       // pad_held plus any injected input, this frame
	u32 pressed;    // went down this frame (with auto-repeat on the d-pad in menus)
	int repeat_frames;

	// Menus
	int game_sel;      // its system is the catalog the carousel shows
	float game_scroll; // animated towards game_sel
	int catalog_pos[SYS_COUNT]; // where the player left each catalog (position in it)
	int save_sel;
	int num_saves; // -1 while the folder is being read
	struct save_info saves[LIB_MAX_SAVES];
	int pause_sel;
	int flash_frames; // feedback when picking something unavailable
	// Save management: the save, the operation (enum lib_save_op) and its new name, which is
	// also the keyboard's text.
	int menu_sel;
	int key_sel; // keyboard: row * KB_COLS + column
	int save_op;
	char op_save[64], op_to[64];
	bool name_taken; // the typed name is already used
	// A short refusal shown in the carousel and the saves list (e.g. no game starts while one is
	// being patched).
	char toast[48];
	int toast_frames;
	// ROM patching: the list for game_file, the chosen one (patch_sel), the build's result (the
	// new game and a base the cleanup may delete), whether the job running is that cleanup.
	int patch_sel;
	int num_patches; // -1 while it is being read
	struct patch_info patches[PATCH_LIST_MAX];
	struct patch_result patch_result;
	bool patch_cleanup;

	// Emulation (valid while PS_PLAYING / PS_PAUSED)
	int game;           // index in lib_games, re-mapped by game_file after a rescan
	char game_file[256];
	int rom;            // entry in the shared ROM cache
	char save_name[64];
	char save_path[512];
	struct mCore* core;
	struct jit* jit; // NULL: mGBA's interpreter
	struct mCore* ref; // dev self-check: interpreted twin of core (see docs/jit.md)
	mColor* ref_frame;
	// Dev sound self-check (DATA_DIR/soundcheck, see emu.c): a twin with the opposite setting.
	struct mCore* twin;
	mColor* twin_frame;
	bool soundcheck, soundcheck_skip;
	int soundcheck_left, twin_frames, twin_toggles;
	int twin_failed; // 1: the game-visible state differed, 2: the audio state (first one logged)
	// Triple-buffered under the emulator lock: the emulation thread renders into
	// frames[frame_cur]; frames[frame_ready] is the last complete frame; the main loop copies
	// frames[frame_shown] into screen[screen_cur] (double-buffered for the GPU). It doesn't
	// wait for every game's frame, so the thread must never draw into the one being copied.
	mColor* frames[3];
	int frame_cur, frame_ready, frame_shown;
	struct gfx_tex screen[2];
	int screen_cur;
	struct spu_renderer spu;
	void (*draw_scanline)(struct GBAVideoRenderer*, int); // mGBA's, wrapped to time it

	// Emulation thread (see emu.c). Guarded by the emulator lock unless noted.
	bool started;      // main thread only: the thread exists (loading or running)
	int load_status;   // enum load_status
	sys_ppu_thread_t thread;
	sys_cond_t wake;
	bool quit;
	bool active;       // running this frame (set by the main loop)
	bool heard;        // its audio goes to the speakers
	bool favored;      // heard, or on its cable: higher priority, the main loop waits for it
	u32 input;         // GBA keys for the frame being released
	int frames_wanted; // frames the main loop has released
	int frames_done;   // frames completed by the thread
	struct VFile* save_vf;
	bool (*memchunk_sync)(struct VFile*, void*, size_t);
	struct GBASIOLockstepDriver link;
	struct mLockstepUser link_user;
	bool link_wanted;   // the pause menu's cable switch (written by the main thread)
	bool link_attached; // the emulation thread follows link_wanted between two slices
	struct rfu rfu;     // its Wireless Adapter: on the same port, so never with the cable
	bool rfu_wanted;    // the pause menu's adapter switch (main thread)
	bool rfu_attached;  // followed like link_attached
	int rfu_session;    // frames its adapter still counts as in a session (main loop)
	bool asleep;

	// Performance readout (microseconds)
	s64 time_sum, time_acc, time_max, shown_avg, shown_max;
	s64 fps_since;    // when the current 60-frame readout window started
	float shown_fps;  // frames this game completed per second of wall time
	int time_frames;
	u64 render_ticks; // timebase ticks in mGBA's renderer this second (emulation thread only)
	s64 shown_render; // renderer share of shown_avg
	int link_sleeps;      // link cable sleeps (dev log, reset every second)
	u64 link_wait_ticks;  // timebase ticks spent blocked by the link cable
};

extern struct player players[MAX_PLAYERS];

#endif
