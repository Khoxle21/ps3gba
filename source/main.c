// ps3gba: up to four players, each with their own controller, game and save, side by side
// and linked together by an emulated link cable.
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <io/pad.h>
#include <sys/file.h>
#include <sys/process.h>
#include <sys/systime.h>
#include <sys/thread.h>
#include <mgba/internal/arm/arm.h>
#include <mgba/internal/gba/gba.h>
#include <sysutil/sysutil.h>
#include <tiny3d.h>

#include "audio.h"
#include "emu.h"
#include "gfx.h"
#include "intro.h"
#include "jit/jit.h"
#include "library.h"
#include "rompatch.h"
#include "ui.h"
#include "web.h"

SYS_PROCESS_PARAM(1001, 0x100000);

#define DIRS (BTN_UP | BTN_DOWN | BTN_LEFT | BTN_RIGHT)
#define REPEAT_DELAY 18 // frames before a held direction repeats in menus
#define REPEAT_RATE 5

static volatile bool running = true;
static bool show_perf;

// The main menu (one screen for everyone) until PLAY, then the players' slots.
static enum screen screen = SCREEN_MENU;
static int menu_sel, about_scroll;
// PLAY and back: an iris like the welcome screen's shuts, the screen changes in the dark, and
// it opens again. Frames since it started, -1: none.
#define WIPE_SHUT 16
#define WIPE_DARK 20
#define WIPE_DONE 44
static int wipe = -1;
static enum screen wipe_to;

static void set_screen(enum screen to) {
	static const char* const names[] = { "play", "about", "help", "menu" };
	printf("menu: %s -> %s\n", names[screen], names[to]);
	screen = to;
}

static void start_wipe(enum screen to) {
	if (wipe < 0) {
		wipe = 0;
		wipe_to = to;
	}
}

static void sysutil_cb(u64 status, u64 param, void* user) {
	if (status == SYSUTIL_EXIT_GAME) {
		running = false;
	}
}

static u32 map_buttons(const padData* pad) {
	u32 b = 0;
	if (pad->BTN_UP) b |= BTN_UP;
	if (pad->BTN_DOWN) b |= BTN_DOWN;
	if (pad->BTN_LEFT) b |= BTN_LEFT;
	if (pad->BTN_RIGHT) b |= BTN_RIGHT;
	if (pad->BTN_CROSS) b |= BTN_CROSS;
	if (pad->BTN_CIRCLE) b |= BTN_CIRCLE;
	if (pad->BTN_SQUARE) b |= BTN_SQUARE;
	if (pad->BTN_TRIANGLE) b |= BTN_TRIANGLE;
	if (pad->BTN_L1) b |= BTN_L1;
	if (pad->BTN_R1) b |= BTN_R1;
	if (pad->BTN_L2) b |= BTN_L2;
	if (pad->BTN_R2) b |= BTN_R2;
	if (pad->BTN_L3) b |= BTN_L3;
	if (pad->BTN_R3) b |= BTN_R3;
	if (pad->BTN_START) b |= BTN_START;
	if (pad->BTN_SELECT) b |= BTN_SELECT;
	// Left stick doubles as the d-pad.
	if (pad->ANA_L_H < 64) b |= BTN_LEFT;
	if (pad->ANA_L_H > 192) b |= BTN_RIGHT;
	if (pad->ANA_L_V < 64) b |= BTN_UP;
	if (pad->ANA_L_V > 192) b |= BTN_DOWN;
	return b;
}

static void read_input(void) {
	padInfo info;
	ioPadGetInfo(&info);
	for (int i = 0; i < MAX_PLAYERS; ++i) {
		struct player* p = &players[i];
		u32 prev = p->held;
		p->connected = info.status[i];
		if (p->connected) {
			padData pad;
			ioPadGetData(i, &pad);
			if (pad.len > 0) { // len 0: unchanged since the last poll
				p->pad_held = map_buttons(&pad);
			}
		} else {
			p->pad_held = 0;
		}
		p->held = p->pad_held;
		p->pressed = p->held & ~prev;
		// Menu auto-repeat for a held direction.
		if ((p->held & DIRS) && (p->held & DIRS) == (prev & DIRS)) {
			if (++p->repeat_frames >= REPEAT_DELAY && (p->repeat_frames - REPEAT_DELAY) % REPEAT_RATE == 0) {
				p->pressed |= p->held & DIRS;
			}
		} else {
			p->repeat_frames = 0;
		}
	}
}

// A character the rename keyboard can type.
static bool kb_allowed(char c) {
	for (int r = 0; r < KB_ROWS; ++r) {
		if (c && c != '<' && c != '>' && strchr(kb_rows[r], c)) {
			return true;
		}
	}
	return false;
}

// Appends c to a save name, without leading or double spaces; false when the name is full.
static bool kb_type(char* name, char c) {
	size_t n = strlen(name);
	if (c == ' ' && (n == 0 || name[n - 1] == ' ')) {
		return true;
	}
	if (n >= LIB_SAVE_NAME_MAX) {
		return false;
	}
	name[n] = c;
	name[n + 1] = '\0';
	return true;
}

#ifdef DEV_TOOLS
// Dev builds only: scripted input for testing without real controllers. Write commands to
// DEV_INPUT, one per line: "<player 0-3> <UP|DOWN|LEFT|RIGHT|CROSS|CIRCLE|START|SELECT|L3|R3|...>",
// "<player> CONNECT" / "<player> DISCONNECT" for virtual controllers, "WAIT <frames>",
// "RENDER PPU|SPU|CHECK" to pick the renderer of games started afterwards,
// "WAITGBA <frames>" (frames emulated by player 1: deterministic on slow hosts),
// "<player> TYPE <text>" (types it on the rename keyboard, as if key by key),
// "<player> GAME <text>" (moves the carousel to the first game whose file name contains it),
// "<player> PATCH <text>" (selects the first patch whose file name contains it, once listed),
// "<player> HOLD <button> <frames>" (a press held that long, e.g. to walk),
// "<player> LINK ON|OFF" (the pause menu's link cable switch, for a running GBA game),
// "<player> WIRELESS ON|OFF" (its Wireless Adapter switch, the same way),
// "SHOT" to save the displayed frame to DEV_SHOT, "AUDIO <seconds>" to record what goes to
// the audio port to DEV_AUDIO (48 kHz float stereo WAV), or "SAMPLEALL 1|0" to make unheard
// games compute their sound anyway (as before they skipped it, to measure the gain). Player
// commands skip the welcome screen and the main menu at once, so scripts can start right after
// "ready:"; "INTRO <frame> [<frames since Start>]" shows and holds one of the welcome screen's
// frames instead, and "MENU" opens the main menu, which later player commands then drive.
// "FRAME <n>" draws every UI animation as at frame n from then on (-1: live), so screenshots of
// two builds can be compared pixel by pixel. Everything printed goes to DEV_LOG, with a
// performance line every second, so a console can be checked over FTP.
#define DEV_INPUT DATA_DIR "/dev_input.txt"
#define DEV_LOG DATA_DIR "/log.txt"
#define DEV_SHOT DATA_DIR "/shot.bmp"
#define DEV_AUDIO DATA_DIR "/audio.wav"
#define DEV_QUEUE 4096 // ~320 KB, dev builds only: random-play scripts send thousands of lines

static struct { int player; u32 buttons; int wait; char text[16]; char game[48]; char patch[48]; int link, wireless; } dev_queue[DEV_QUEUE];
static int dev_head, dev_tail, dev_hold, dev_wait, dev_wait_gba = -1;
static bool dev_shot;
static bool dev_connected[MAX_PLAYERS];
static bool dev_menu; // MENU given: player commands no longer skip the menus
static int dev_frame = -1; // FRAME given: the UI's animations are held at that frame

// Before any press is applied: scripts expect the join screen.
static void dev_skip(void) {
	intro_skip();
	if (!dev_menu && screen != SCREEN_PLAY) {
		wipe = -1;
		set_screen(SCREEN_PLAY);
	}
}

static u32 dev_button(const char* name) {
	static const struct { const char* name; u32 bit; } names[] = {
		{ "UP", BTN_UP }, { "DOWN", BTN_DOWN }, { "LEFT", BTN_LEFT }, { "RIGHT", BTN_RIGHT },
		{ "CROSS", BTN_CROSS }, { "CIRCLE", BTN_CIRCLE }, { "SQUARE", BTN_SQUARE }, { "TRIANGLE", BTN_TRIANGLE },
		{ "L1", BTN_L1 }, { "R1", BTN_R1 }, { "L2", BTN_L2 }, { "R2", BTN_R2 }, { "L3", BTN_L3 }, { "R3", BTN_R3 },
		{ "START", BTN_START }, { "SELECT", BTN_SELECT },
	};
	for (size_t i = 0; i < sizeof(names) / sizeof(*names); ++i) {
		if (strcmp(name, names[i].name) == 0) return names[i].bit;
	}
	return 0;
}

static void dev_poll_file(void) {
	FILE* f = fopen(DEV_INPUT, "r");
	if (!f) {
		return;
	}
	char line[96], word[48], text[16];
	int player, frames;
	while (fgets(line, sizeof(line), f)) {
		int next = (dev_tail + 1) % DEV_QUEUE;
		if (next == dev_head) {
			printf("dev: input queue full (%d commands), the rest of the file is dropped\n", DEV_QUEUE - 1);
			break;
		}
		if (sscanf(line, "%d GAME %47[^\r\n]", &player, word) == 2 && player >= 0 && player < MAX_PLAYERS) {
			intro_skip();
			dev_queue[dev_tail] = (typeof(dev_queue[0])) { player, 0, 0 };
			snprintf(dev_queue[dev_tail].game, sizeof(dev_queue[0].game), "%s", word);
		} else if (sscanf(line, "%d PATCH %47[^\r\n]", &player, word) == 2 && player >= 0 && player < MAX_PLAYERS) {
			dev_skip();
			dev_queue[dev_tail] = (typeof(dev_queue[0])) { player, 0, 0 };
			snprintf(dev_queue[dev_tail].patch, sizeof(dev_queue[0].patch), "%s", word);
		} else if (strncmp(line, "SHOT", 4) == 0) {
			dev_queue[dev_tail] = (typeof(dev_queue[0])) { -2, 0, 0 }; // in order, after earlier commands
		} else if (sscanf(line, "RENDER %31s", word) == 1) { // for games started from now on
			render_mode = strcmp(word, "PPU") == 0 ? RENDER_PPU : strcmp(word, "CHECK") == 0 ? RENDER_CHECK : RENDER_SPU;
			printf("dev: renderer %s\n", word);
			continue;
		} else if (sscanf(line, "INTRO %d", &player) == 1) {
			int leaving = -1;
			sscanf(line, "INTRO %*d %d", &leaving);
			intro_seek(player, leaving);
			continue;
		} else if (sscanf(line, "FRAME %d", &player) == 1) {
			dev_frame = player;
			continue;
		} else if (strncmp(line, "MENU", 4) == 0) {
			intro_skip();
			dev_menu = true;
			wipe = -1;
			menu_sel = about_scroll = 0;
			set_screen(SCREEN_MENU);
			continue;
		} else if (sscanf(line, "SAMPLEALL %d", &player) == 1) {
			emu_sample_all = player;
			printf("dev: unheard games %s sound samples\n", player ? "make" : "skip");
			continue;
		} else if (sscanf(line, "AUDIO %d", &player) == 1) {
			dev_queue[dev_tail] = (typeof(dev_queue[0])) { -4, 0, player }; // in order too
		} else if (sscanf(line, "WAITGBA %d", &player) == 1) {
			dev_queue[dev_tail] = (typeof(dev_queue[0])) { -3, 0, player };
		} else if (sscanf(line, "WAIT %d", &player) == 1) {
			dev_queue[dev_tail] = (typeof(dev_queue[0])) { -1, 0, player };
		} else if (sscanf(line, "%d LINK %3s", &player, word) == 2 && player >= 0 && player < MAX_PLAYERS) {
			dev_queue[dev_tail] = (typeof(dev_queue[0])) { player, 0, 0 };
			dev_queue[dev_tail].link = strcmp(word, "ON") == 0 ? 2 : 1;
		} else if (sscanf(line, "%d WIRELESS %3s", &player, word) == 2 && player >= 0 && player < MAX_PLAYERS) {
			dev_queue[dev_tail] = (typeof(dev_queue[0])) { player, 0, 0 };
			dev_queue[dev_tail].wireless = strcmp(word, "ON") == 0 ? 2 : 1;
		} else if (sscanf(line, "%d HOLD %31s %d", &player, word, &frames) == 3 && player >= 0 && player < MAX_PLAYERS) {
			dev_skip();
			dev_queue[dev_tail] = (typeof(dev_queue[0])) { player, dev_button(word), frames > 2 ? frames : 2 };
		} else if (sscanf(line, "%d TYPE %15[^\r\n]", &player, text) == 2 && player >= 0 && player < MAX_PLAYERS) {
			dev_skip();
			dev_queue[dev_tail] = (typeof(dev_queue[0])) { player, 0, 0 };
			snprintf(dev_queue[dev_tail].text, sizeof(dev_queue[dev_tail].text), "%s", text);
		} else if (sscanf(line, "%d %31s", &player, word) == 2 && player >= 0 && player < MAX_PLAYERS) {
			dev_skip();
			if (strcmp(word, "CONNECT") == 0 || strcmp(word, "DISCONNECT") == 0) {
				dev_connected[player] = word[0] == 'C';
				continue;
			}
			dev_queue[dev_tail] = (typeof(dev_queue[0])) { player, dev_button(word), 0 };
		} else {
			continue;
		}
		dev_tail = next;
	}
	fclose(f);
	remove(DEV_INPUT);
}

// Saves the frame currently on screen as a 24-bit BMP. Reading video memory from the PPU is
// slow (~0.5 s for 1080p), which is fine for a debug tool.
static void dev_screenshot(void) {
	u32 w = Video_Resolution.width, h = Video_Resolution.height;
	const u8* fb = (const u8*) Video_buffer[Video_currentBuffer ^ 1]; // the one being displayed
	FILE* f = fopen(DEV_SHOT, "wb");
	if (!f) {
		return;
	}
	u32 row = (w * 3 + 3) & ~3u, size = 54 + row * h;
	u8 header[54] = { 'B', 'M' };
	#define LE32(o, v) (header[o] = (v), header[o + 1] = (v) >> 8, header[o + 2] = (v) >> 16, header[o + 3] = (v) >> 24)
	LE32(2, size);
	LE32(10, 54);
	LE32(14, 40);
	LE32(18, w);
	LE32(22, h);
	header[26] = 1;
	header[28] = 24;
	LE32(34, row * h);
	#undef LE32
	fwrite(header, 1, sizeof(header), f);
	u8* line = calloc(row, 1);
	u32* src = malloc(w * 4);
	for (int y = h - 1; y >= 0; --y) { // BMP rows go bottom-up
		memcpy(src, fb + y * Video_pitch, w * 4);
		for (u32 x = 0; x < w; ++x) {
			u32 px = src[x]; // XRGB
			line[x * 3] = px;
			line[x * 3 + 1] = px >> 8;
			line[x * 3 + 2] = px >> 16;
		}
		fwrite(line, 1, row, f);
	}
	free(src);
	free(line);
	fclose(f);
	printf("dev: screenshot saved\n");
}

// Wall-clock frame stats, measured in main(): work = everything except waiting for the display.
static s64 dev_work_sum, dev_work_max, dev_emu_sum, dev_last_log;
static int dev_frames;
// Per-phase CPU time over the last second (microseconds).
enum { PH_INPUT, PH_BEGIN, PH_BACKGROUND, PH_SLOTS, PH_UPLOAD, PH_FLIP, PH_COUNT };
static const char* const dev_phase_names[PH_COUNT] = { "input", "begin", "background", "slots", "emulators+upload", "flip" };
static s64 dev_phase[PH_COUNT], dev_phase_mark;
#define PHASE(id) do { s64 now_ = sysGetSystemTime(); dev_phase[id] += now_ - dev_phase_mark; dev_phase_mark = now_; } while (0)
#define PHASE_START() (dev_phase_mark = sysGetSystemTime())

// Watchdog: if the main loop stops completing frames, report where it is.
static volatile int dev_frame_counter;

static void dev_watchdog(void* arg) {
	int last = -1, stuck = 0;
	for (;;) {
		sysUsleep(1000000);
		if (dev_frame_counter == last) {
			if (++stuck == 3) {
				log_printf("watchdog: no frame for 3 s, at '%s' (player %d)\n", (const char*) emu_where, emu_where_player + 1);
				for (int i = 0; i < MAX_PLAYERS; ++i) {
					struct player* p = &players[i];
					if (p->core) {
						bool gba = p->core->platform(p->core) == mPLATFORM_GBA;
						log_printf("watchdog:   P%d asleep=%d pc=%08x frame=%u\n", i + 1, p->asleep,
						       gba ? (unsigned) ((struct GBA*) p->core->board)->cpu->gprs[15] : 0, p->core->frameCounter(p->core));
						emu_link_debug(p);
					}
				}
			}
		} else {
			stuck = 0;
		}
		last = dev_frame_counter;
	}
}

static void dev_log_perf(int frame) {
	if (frame % 60) {
		return;
	}
	s64 now = sysGetSystemTime();
	if (dev_last_log && dev_frames) {
		printf("frame: %.1f fps, work avg %.2f ms (waiting for emulators %.2f), max %.2f ms\n",
		       dev_frames * 1e6 / (now - dev_last_log), dev_work_sum / 1000.0 / dev_frames,
		       dev_emu_sum / 1000.0 / dev_frames, dev_work_max / 1000.0);
		log_memory("now");
	}
	if (dev_frames) {
		printf("phases (ms/frame):");
		for (int i = 0; i < PH_COUNT; ++i) {
			printf(" %s %.2f", dev_phase_names[i], dev_phase[i] / 1000.0 / dev_frames);
			dev_phase[i] = 0;
		}
		printf("\n");
		gfx_log_stats(dev_frames);
	}
	dev_last_log = now;
	dev_work_sum = dev_work_max = dev_emu_sum = 0;
	dev_frames = 0;
	static int frames_seen[MAX_PLAYERS];
	for (int i = 0; i < MAX_PLAYERS; ++i) {
		const struct player* p = &players[i];
		if (p->core) {
			// Emulated frames since the last line: the avg/max figures only refresh every 60.
			printf("perf P%d %s: %d frames, avg %.2f ms (renderer %.2f), max %.2f ms\n", i + 1,
			       p->game_file, p->frames_done - frames_seen[i], p->shown_avg / 1000.0,
			       p->shown_render / 1000.0, p->shown_max / 1000.0);
			printf("link P%d: %d sleeps, %.2f ms blocked per frame\n", i + 1, p->link_sleeps,
			       p->link_wait_ticks * 1000.0 / sysGetTimebaseFrequency() / 60);
			players[i].link_sleeps = 0;
			players[i].link_wait_ticks = 0;
			spu_render_log(&players[i], p->frames_done - frames_seen[i]);
			if (p->jit) {
				// Racy reads of the emulation thread's counters: fine for a log line.
				struct jit_stats s;
				jit_get_stats(p->jit, &s, true);
				printf("jit P%d: %u blocks (%u KB), %u translated (%.1f ms), %u invalidated, %u revived, %u flushes, %u interpreted, %u resumed, %u idle skipped, %u dispatches (%u writepc), %u checked\n",
				       i + 1, s.blocks, s.cache_used / 1024, s.translated,
				       s.translate_ticks * 1000.0 / sysGetTimebaseFrequency(), s.invalidated, s.revived, s.flushes,
				       s.interpreted, s.resumed, s.idle_skipped, s.dispatches, s.writepc, s.checked);
				int frames = p->frames_done - frames_seen[i];
				if (frames > 0 && s.loop_ticks) { // TIMES=1 builds
					double tb = sysGetTimebaseFrequency() / 1000.0 * frames; // ticks per ms per frame
					printf("time P%d (ms/frame): loop %.2f = code %.2f + events %.2f + interpreter %.2f + "
					       "translation %.2f + dispatch %.2f\n", i + 1, s.loop_ticks / tb, s.code_ticks / tb,
					       s.event_ticks / tb, s.interp_ticks / tb, s.translate_ticks / tb,
					       (double) (s.loop_ticks - s.code_ticks - s.event_ticks - s.interp_ticks - s.translate_ticks) / tb);
				}
				if (frame % 600 == 0) {
					jit_profile_dump(p->jit); // only logs when profiling (DATA_DIR/jitprof)
				}
			}
		}
		// GBA details: link cable, JIT (GB/GBC games have neither).
		if (p->core && p->frames_done == frames_seen[i] && p->core->platform(p->core) == mPLATFORM_GBA) {
			const struct GBA* gba = p->core->board;
			printf("perf P%d stalled: asleep=%d link=%d done=%d wanted=%d pc=%08x cycles=%d next=%d halted=%d blocked=%d early=%d\n",
			       i + 1, p->asleep, p->link_attached, p->frames_done, p->frames_wanted, gba->cpu->gprs[15],
			       gba->cpu->cycles, gba->cpu->nextEvent, gba->cpu->halted, gba->cpuBlocked, gba->earlyExit);
			if (p->jit) {
				struct jit_stats s;
				jit_get_stats(p->jit, &s, false);
				printf("jit P%d stalled: in_jit=%d run_loops=%u\n", i + 1, s.in_jit, s.run_loops);
			}
			emu_link_debug(p);
		}
		frames_seen[i] = p->frames_done;
	}
}

// Each queued press is held 2 frames, then released for 6 so menus see distinct presses.
static void dev_apply(int frame) {
	if (frame % 15 == 0) {
		dev_poll_file();
	}
	if (dev_shot) {
		dev_shot = false;
		dev_screenshot();
	}
	dev_log_perf(frame);
	audio_capture_poll(DEV_AUDIO);
	for (int i = 0; i < MAX_PLAYERS; ++i) {
		if (dev_connected[i] && !players[i].connected) {
			players[i].connected = true;
		}
	}
	if (dev_wait > 0) {
		--dev_wait;
		return;
	}
	if (dev_wait_gba >= 0) {
		if (players[0].frames_done < dev_wait_gba) { // racy read, fine for a counter
			return;
		}
		dev_wait_gba = -1;
	}
	if (dev_head == dev_tail) {
		return;
	}
	typeof(dev_queue[0])* cmd = &dev_queue[dev_head];
	if (cmd->player == -3) { // WAITGBA: frames emulated by player 1, whatever the host speed
		dev_wait_gba = players[0].frames_done + cmd->wait;
		dev_head = (dev_head + 1) % DEV_QUEUE;
		return;
	}
	if (cmd->player == -2) {
		dev_shot = true;
		dev_head = (dev_head + 1) % DEV_QUEUE;
		return;
	}
	if (cmd->player == -4) {
		audio_capture(cmd->wait);
		dev_head = (dev_head + 1) % DEV_QUEUE;
		return;
	}
	if (cmd->player < 0) {
		dev_wait = cmd->wait;
		dev_head = (dev_head + 1) % DEV_QUEUE;
		return;
	}
	if (cmd->game[0]) { // GAME: independent of the carousel's order, which new games change
		int i = 0;
		while (i < lib_num_games && !strstr(lib_games[i].file, cmd->game)) {
			++i;
		}
		if (i < lib_num_games) {
			players[cmd->player].game_sel = i; // and so its catalog
			players[cmd->player].game_scroll = i;
		}
		printf("dev: P%d game '%s': %s\n", cmd->player + 1, cmd->game, i < lib_num_games ? lib_games[i].file : "no match");
		dev_head = (dev_head + 1) % DEV_QUEUE;
		return;
	}
	struct player* p = &players[cmd->player];
	if (cmd->patch[0]) { // PATCH: by name, whatever the list's order
		if (p->state == PS_PATCHES && p->num_patches == -1) {
			return; // still reading: try again next frame
		}
		int i = 0;
		while (p->state == PS_PATCHES && i < p->num_patches && !strstr(p->patches[i].file, cmd->patch)) {
			++i;
		}
		bool found = p->state == PS_PATCHES && i < p->num_patches;
		if (found) {
			p->patch_sel = i;
		}
		printf("dev: P%d patch '%s': %s\n", cmd->player + 1, cmd->patch, found ? p->patches[i].file : "no match");
		dev_head = (dev_head + 1) % DEV_QUEUE;
		return;
	}
	if (cmd->link) {
		emu_link_set(p, cmd->link == 2);
		printf("dev: P%d link %s%s\n", cmd->player + 1, cmd->link == 2 ? "on" : "off",
		       emu_link_available(p) ? "" : " ignored: no running GBA game");
		dev_head = (dev_head + 1) % DEV_QUEUE;
		return;
	}
	if (cmd->wireless) {
		emu_wireless_set(p, cmd->wireless == 2);
		printf("dev: P%d wireless adapter %s%s\n", cmd->player + 1, cmd->wireless == 2 ? "on" : "off",
		       emu_wireless_available(p) ? "" : " ignored: no running GBA game");
		dev_head = (dev_head + 1) % DEV_QUEUE;
		return;
	}
	if (cmd->text[0]) {
		for (const char* c = cmd->text; *c && p->state == PS_SAVE_RENAME; ++c) {
			char k = toupper((unsigned char) *c);
			if (kb_allowed(k) && !kb_type(p->op_to, k)) {
				p->flash_frames = 16;
			}
		}
		dev_head = (dev_head + 1) % DEV_QUEUE;
		return;
	}
	// Held for 2 frames (HOLD: its count), then released for 6, and for as many emulated frames
	// too, so a slow emulator sees the press. Only in game: a press that pauses a lone game stops
	// its emulator, which then never would.
	static int gba_mark;
	int len = cmd->wait > 2 ? cmd->wait : 2;
	bool in_game = p->core && p->state == PS_PLAYING;
	int gba = in_game ? p->frames_done : 1 << 30; // menus: display frames only
	if (dev_hold == 0) {
		gba_mark = in_game ? gba : 0;
	}
	if (dev_hold < len || (dev_hold == len && gba < gba_mark + len)) {
		if (dev_hold == 0) p->pressed |= cmd->buttons;
		p->held |= cmd->buttons;
		if (dev_hold < len) {
			++dev_hold; // then stays at len until the emulator has seen as many frames of it
		}
		return;
	}
	if (dev_hold == len) {
		gba_mark = in_game ? gba : 0;
	}
	if (++dev_hold >= len + 6 && gba >= gba_mark + 2) {
		dev_hold = 0;
		dev_head = (dev_head + 1) % DEV_QUEUE;
	}
}
#endif

static void open_saves(struct player* p) {
	library_list_saves_async(p->index, p->game_file);
	p->num_saves = -1;
	p->save_sel = 0;
	p->state = PS_SAVES;
}

// Whether another player holds this save of p's game: playing or loading it (a new save only
// reaches the drive when its game saves or quits), or renaming, copying or deleting it. Names
// compare case-insensitively everywhere: the keyboard types capitals, and FTP copies to
// case-insensitive file systems would collide.
static bool save_taken(const struct player* p, const char* name) {
	for (int i = 0; i < MAX_PLAYERS; ++i) {
		const struct player* q = &players[i];
		if (q == p || strcmp(q->game_file, p->game_file) != 0) {
			continue;
		}
		if (q->started && strcasecmp(q->save_name, name) == 0) {
			return true;
		}
		if (q->state == PS_SAVE_BUSY && (strcasecmp(q->op_save, name) == 0 || strcasecmp(q->op_to, name) == 0)) {
			return true;
		}
	}
	return false;
}

// Whether a save other than `except` (NULL: none) has this name, or another player holds it.
static bool name_used(const struct player* p, const char* name, const char* except) {
	for (int i = 0; i < p->num_saves; ++i) {
		if (strcasecmp(p->saves[i].name, name) == 0 && !(except && strcmp(p->saves[i].name, except) == 0)) {
			return true;
		}
	}
	return save_taken(p, name);
}

// First unused name of "<base>", "<base> 2", "<base> 3"... (from number `from` on), the base
// shortened to stay within LIB_SAVE_NAME_MAX.
static void free_save_name(const struct player* p, const char* base, int from, char* out, size_t len) {
	for (int i = from;; ++i) {
		char num[12] = "";
		if (i > 1) {
			snprintf(num, sizeof(num), " %d", i);
		}
		int n = strlen(base), keep = LIB_SAVE_NAME_MAX - (int) strlen(num);
		if (n > keep) n = keep;
		while (n > 0 && base[n - 1] == ' ') --n;
		snprintf(out, len, "%.*s%s", n, base, num);
		if (!name_used(p, out, NULL)) {
			return;
		}
	}
}

// Selects the save with this name, else stays on the same row.
static void select_save(struct player* p, const char* name) {
	for (int i = 0; i < p->num_saves; ++i) {
		if (strcmp(p->saves[i].name, name) == 0) {
			p->save_sel = i;
			return;
		}
	}
	if (p->save_sel > p->num_saves) {
		p->save_sel = p->num_saves;
	}
}

static void save_request(struct player* p, enum lib_save_op op) {
	p->save_op = op;
	library_save_op_async(p->index, p->game_file, op, p->op_save, p->op_to);
	p->state = PS_SAVE_BUSY;
}

// A file operation's fresh list goes to everyone else in that game's saves menus: with a stale
// one they could start a save that was just renamed or deleted (a blank game).
static void share_saves(const struct player* p) {
	for (int i = 0; i < MAX_PLAYERS; ++i) {
		struct player* q = &players[i];
		bool menus = q->state == PS_SAVES || q->state == PS_LOADING || q->state == PS_SAVE_MENU ||
		             q->state == PS_SAVE_RENAME || q->state == PS_SAVE_DELETE;
		if (q == p || !menus || strcmp(q->game_file, p->game_file) != 0) {
			continue;
		}
		if (q->num_saves < 0) {
			library_list_saves_async(q->index, q->game_file); // its listing may predate the change
			continue;
		}
		char sel[64] = "";
		if (q->save_sel < q->num_saves) {
			snprintf(sel, sizeof(sel), "%s", q->saves[q->save_sel].name);
		}
		memcpy(q->saves, p->saves, p->num_saves * sizeof(*p->saves));
		q->num_saves = p->num_saves;
		if (sel[0]) {
			select_save(q, sel);
		} else {
			q->save_sel = q->num_saves; // stays on NEW SAVE
		}
	}
}

// Keyboard OK: a trimmed, non-empty name no other save uses, on a save nobody took meanwhile.
static void rename_confirm(struct player* p) {
	size_t n = strlen(p->op_to);
	while (n > 0 && p->op_to[n - 1] == ' ') {
		p->op_to[--n] = '\0';
	}
	if (strcmp(p->op_to, p->op_save) == 0) {
		p->state = PS_SAVES; // unchanged
	} else if (n == 0 || name_used(p, p->op_to, p->op_save) || save_taken(p, p->op_save)) {
		p->flash_frames = 16;
	} else {
		save_request(p, LIB_RENAME);
	}
}

// A refusal with its reason under the player's eyes for a few seconds.
static void refuse(struct player* p, const char* why) {
	p->flash_frames = 16;
	snprintf(p->toast, sizeof(p->toast), "%s", why);
	p->toast_frames = 150;
}

// The carousel on this game (e.g. one just built), without sliding through the others.
static void show_game(struct player* p, const char* file) {
	int g = library_find(file);
	if (g >= 0) {
		p->game_sel = g;
		p->game_scroll = g;
	}
}

static void patch_error(struct player* p, const char* error) {
	snprintf(p->patch_result.error, sizeof(p->patch_result.error), "%s", error);
	p->state = PS_PATCH_ERROR;
}

// L1/R1 in the carousel: the previous or next system that has games, where the player left it.
static void switch_catalog(struct player* p, int dir) {
	int system = lib_games[p->game_sel].system, first;
	library_catalog(system, &first);
	p->catalog_pos[system] = p->game_sel - first;
	for (int s = system + dir; s >= 0 && s < SYS_COUNT; s += dir) {
		int n = library_catalog(s, &first);
		if (n > 0) {
			p->game_sel = first + (p->catalog_pos[s] < n ? p->catalog_pos[s] : n - 1);
			p->game_scroll = p->game_sel; // another shelf: no slide through it
			return;
		}
	}
}

static void update_player(struct player* p) {
	u32 in = p->pressed;
	if (p->flash_frames > 0) {
		--p->flash_frames;
	}
	if (p->toast_frames > 0) {
		--p->toast_frames;
	}
	switch (p->state) {
	case PS_EMPTY:
		if (p->connected && (in & (BTN_START | BTN_CROSS))) {
			p->game_scroll = p->game_sel;
			p->state = PS_GAMES;
		}
		break;
	case PS_GAMES:
		if (lib_num_games > 0) {
			int first, n = library_catalog(lib_games[p->game_sel].system, &first);
			if ((in & BTN_LEFT) && p->game_sel > first) --p->game_sel;
			if ((in & BTN_RIGHT) && p->game_sel < first + n - 1) ++p->game_sel;
			if (in & (BTN_L1 | BTN_R1)) {
				switch_catalog(p, in & BTN_R1 ? 1 : -1);
			}
			if ((in & BTN_CROSS) && rompatch_busy()) {
				refuse(p, "WAIT: A GAME IS BEING PATCHED");
			} else if (in & (BTN_CROSS | BTN_SQUARE)) {
				p->game = p->game_sel;
				snprintf(p->game_file, sizeof(p->game_file), "%s", lib_games[p->game].file);
				if (in & BTN_CROSS) {
					open_saves(p);
				} else {
					rompatch_list_async(p->index, p->game_file);
					p->num_patches = -1;
					p->patch_sel = 0;
					p->patch_cleanup = false;
					p->state = PS_PATCHES;
					break;
				}
			}
		}
		if (in & BTN_CIRCLE) p->state = PS_EMPTY;
		if (in & BTN_TRIANGLE) p->state = PS_PHONE;
		break;
	case PS_PHONE:
		if (in & (BTN_CIRCLE | BTN_TRIANGLE)) p->state = PS_GAMES;
		break;
	case PS_SAVES: {
		if (p->num_saves < 0) {
			p->num_saves = library_saves_result(p->index, p->saves, NULL);
		}
		if (in & BTN_CIRCLE) {
			p->state = PS_GAMES;
			break;
		}
		if (p->num_saves < 0) {
			break; // still reading the folder
		}
		int rows = p->num_saves + 1;
		if (in & BTN_UP) p->save_sel = (p->save_sel + rows - 1) % rows;
		if (in & BTN_DOWN) p->save_sel = (p->save_sel + 1) % rows;
		if ((in & BTN_TRIANGLE) && p->save_sel < p->num_saves) {
			snprintf(p->op_save, sizeof(p->op_save), "%s", p->saves[p->save_sel].name);
			p->op_to[0] = '\0';
			p->menu_sel = SAVE_MENU_RENAME;
			p->state = PS_SAVE_MENU;
			break;
		}
		if ((in & BTN_CROSS) && rompatch_busy()) {
			refuse(p, "WAIT: A GAME IS BEING PATCHED");
		} else if (in & BTN_CROSS) {
			if (p->save_sel == p->num_saves) {
				free_save_name(p, "SAVE", 1, p->save_name, sizeof(p->save_name));
			} else if (save_taken(p, p->saves[p->save_sel].name)) {
				p->flash_frames = 16; // someone else is playing on it, or renaming or deleting it
				break;
			} else {
				snprintf(p->save_name, sizeof(p->save_name), "%s", p->saves[p->save_sel].name);
			}
			if (emu_start(p)) {
				p->state = PS_LOADING;
			} else {
				p->flash_frames = 16;
			}
		}
		break;
	}
	case PS_SAVE_MENU:
		if (in & BTN_UP) p->menu_sel = (p->menu_sel + SAVE_MENU_COUNT - 1) % SAVE_MENU_COUNT;
		if (in & BTN_DOWN) p->menu_sel = (p->menu_sel + 1) % SAVE_MENU_COUNT;
		if ((in & (BTN_CIRCLE | BTN_TRIANGLE)) || ((in & BTN_CROSS) && p->menu_sel == SAVE_MENU_BACK)) {
			p->state = PS_SAVES;
		} else if ((in & BTN_CROSS) && p->menu_sel == SAVE_MENU_COPY) {
			// Copies of "SAVE 2" are "SAVE 3", "SAVE 4"...: the number isn't part of the name.
			char base[64];
			snprintf(base, sizeof(base), "%s", p->op_save);
			char* digits = base + strlen(base);
			while (digits > base && isdigit((unsigned char) digits[-1])) --digits;
			if (*digits && digits > base + 1 && digits[-1] == ' ') digits[-1] = '\0';
			free_save_name(p, base, 2, p->op_to, sizeof(p->op_to));
			save_request(p, LIB_COPY); // copying a save in use is fine: it reads the drive's copy
		} else if ((in & BTN_CROSS) && save_taken(p, p->op_save)) {
			p->flash_frames = 16; // can't rename or delete a save someone plays
		} else if ((in & BTN_CROSS) && p->menu_sel == SAVE_MENU_RENAME) {
			// The keyboard starts on the current name, as far as it can type it.
			p->op_to[0] = '\0';
			for (const char* c = p->op_save; *c; ++c) {
				char k = toupper((unsigned char) *c);
				if (kb_allowed(k)) kb_type(p->op_to, k);
			}
			p->key_sel = 0;
			p->name_taken = false;
			p->state = PS_SAVE_RENAME;
		} else if (in & BTN_CROSS) {
			p->op_to[0] = '\0'; // whatever the keyboard left
			p->menu_sel = 0; // NO
			p->state = PS_SAVE_DELETE;
		}
		break;
	case PS_SAVE_DELETE:
		if (in & (BTN_UP | BTN_DOWN)) p->menu_sel ^= 1;
		if ((in & BTN_CIRCLE) || ((in & BTN_CROSS) && p->menu_sel == 0)) {
			p->menu_sel = SAVE_MENU_DELETE;
			p->state = PS_SAVE_MENU;
		} else if ((in & BTN_CROSS) && save_taken(p, p->op_save)) {
			p->flash_frames = 16; // someone started it meanwhile
		} else if (in & BTN_CROSS) {
			save_request(p, LIB_DELETE);
		}
		break;
	case PS_SAVE_RENAME: {
		int r = p->key_sel / KB_COLS, c = p->key_sel % KB_COLS;
		if (in & BTN_UP) r = (r + KB_ROWS - 1) % KB_ROWS;
		if (in & BTN_DOWN) r = (r + 1) % KB_ROWS;
		int dx = (in & BTN_LEFT) ? KB_COLS - 1 : (in & BTN_RIGHT) ? 1 : 0;
		if (dx) {
			char from = kb_rows[r][c];
			do c = (c + dx) % KB_COLS; while (kb_rows[r][c] == from); // over a wide key at once
		}
		p->key_sel = r * KB_COLS + c;
		char key = kb_rows[r][c];
		size_t n = strlen(p->op_to);
		if (in & BTN_CIRCLE) {
			p->menu_sel = SAVE_MENU_RENAME;
			p->state = PS_SAVE_MENU;
		} else if ((in & BTN_SQUARE) || ((in & BTN_CROSS) && key == '<')) {
			if (n > 0) p->op_to[n - 1] = '\0';
		} else if ((in & BTN_START) || ((in & BTN_CROSS) && key == '>')) {
			rename_confirm(p);
		} else if ((in & BTN_CROSS) && !kb_type(p->op_to, key)) {
			p->flash_frames = 16; // full
		}
		p->name_taken = name_used(p, p->op_to, p->op_save);
		break;
	}
	case PS_SAVE_BUSY: {
		static const char* const names[] = { "list", "rename", "copy", "delete" };
		bool ok;
		int n = library_saves_result(p->index, p->saves, &ok);
		if (n < 0) {
			break;
		}
		p->num_saves = n;
		printf("P%d: %s %s%s%s %s\n", p->index + 1, names[p->save_op], p->op_save, p->op_to[0] ? " -> " : "",
		       p->op_to, ok ? "done" : "failed");
		select_save(p, ok && p->save_op != LIB_DELETE ? p->op_to : p->op_save);
		if (!ok) {
			p->flash_frames = 16;
		}
		p->state = PS_SAVES;
		share_saves(p);
		break;
	}
	case PS_PATCHES: {
		if (p->num_patches == -1) {
			p->num_patches = rompatch_list_result(p->index, p->patches);
		}
		if (in & BTN_CIRCLE) {
			p->state = PS_GAMES;
			break;
		}
		if (p->num_patches <= 0 || emu_anyone_playing()) {
			break; // reading, none, or "all players must quit their games"
		}
		int rows = p->num_patches;
		if (in & BTN_UP) p->patch_sel = (p->patch_sel + rows - 1) % rows;
		if (in & BTN_DOWN) p->patch_sel = (p->patch_sel + 1) % rows;
		const struct patch_info* patch = &p->patches[p->patch_sel];
		if ((in & BTN_CROSS) && patch->fit == FIT_OTHER) {
			patch_error(p, "THIS PATCH IS MADE FOR ANOTHER VERSION OF THE GAME (ANOTHER REGION OR REVISION).");
		} else if ((in & BTN_CROSS) && patch->fit == FIT_BUILT) {
			patch_error(p, "ITS GAME IS ALREADY THERE. RENAME THE PATCH FILE TO MAKE ANOTHER ONE.");
		} else if (in & BTN_CROSS) {
			p->menu_sel = 0; // NO
			p->state = PS_PATCH_CONFIRM;
		}
		break;
	}
	case PS_PATCH_CONFIRM:
		if (in & (BTN_UP | BTN_DOWN)) p->menu_sel ^= 1;
		if ((in & BTN_CIRCLE) || ((in & BTN_CROSS) && p->menu_sel == 0) || emu_anyone_playing()) {
			p->state = PS_PATCHES; // which says why when someone started a game meanwhile
		} else if ((in & BTN_CROSS) && !rompatch_build_async(p->index, p->game_file, p->patches[p->patch_sel].file)) {
			patch_error(p, "ANOTHER PLAYER IS PATCHING A GAME. TRY AGAIN IN A MOMENT.");
		} else if (in & BTN_CROSS) {
			p->patch_cleanup = false;
			p->state = PS_PATCH_BUSY;
		}
		break;
	case PS_PATCH_BUSY: {
		// A new game comes in with a library rescan, which web_frame() takes after this.
		if (rompatch_job_step(p->index) != STEP_DONE || library_rescan_ready()) {
			break;
		}
		struct patch_result r;
		rompatch_job_take(p->index, &r);
		if (!r.ok) {
			if (p->patch_cleanup) show_game(p, p->patch_result.game);
			patch_error(p, r.error);
		} else if (!p->patch_cleanup) {
			p->patch_result = r;
			show_game(p, r.game);
			p->menu_sel = 0; // KEEP
			p->state = PS_PATCH_DONE;
		} else {
			show_game(p, p->patch_result.game);
			p->state = PS_GAMES;
		}
		break;
	}
	case PS_PATCH_DONE: {
		// KEEP, DELETE (the patch), and in a chain DELETE BOTH (the patch and the base).
		const char* base = p->patch_result.base;
		int rows = base[0] ? 3 : 2;
		if (in & BTN_UP) p->menu_sel = (p->menu_sel + rows - 1) % rows;
		if (in & BTN_DOWN) p->menu_sel = (p->menu_sel + 1) % rows;
		bool both = p->menu_sel == 2, base_played = false;
		for (int i = 0; i < MAX_PLAYERS; ++i) {
			base_played |= players[i].started && strcmp(players[i].game_file, base) == 0;
		}
		if ((in & BTN_CIRCLE) || ((in & BTN_CROSS) && p->menu_sel == 0)) {
			p->state = PS_GAMES;
		} else if ((in & BTN_CROSS) && both && base_played) {
			p->flash_frames = 16; // someone started it meanwhile
		} else if ((in & BTN_CROSS) && !rompatch_clean_async(p->index, p->patches[p->patch_sel].file, both ? base : "")) {
			p->flash_frames = 16; // another player's build or cleanup: try again
		} else if (in & BTN_CROSS) {
			p->patch_cleanup = true;
			p->state = PS_PATCH_BUSY;
		}
		break;
	}
	case PS_PATCH_ERROR:
		if (in & (BTN_CROSS | BTN_CIRCLE)) {
			p->state = p->patch_cleanup ? PS_GAMES : PS_PATCHES;
		}
		break;
	case PS_LOADING:
		switch (emu_load_status(p)) {
		case LOAD_OK:
			p->state = PS_PLAYING;
			break;
		case LOAD_FAILED:
			emu_stop(p);
			p->flash_frames = 16;
			p->state = PS_SAVES;
			break;
		case LOAD_PENDING:
			break;
		}
		break;
	case PS_PLAYING:
		if (in & (BTN_L3 | BTN_R3)) {
			p->pause_sel = PAUSE_RESUME;
			p->state = PS_PAUSED;
		}
		break;
	case PS_PAUSED:
		if (in & BTN_UP) p->pause_sel = (p->pause_sel + PAUSE_COUNT - 1) % PAUSE_COUNT;
		if (in & BTN_DOWN) p->pause_sel = (p->pause_sel + 1) % PAUSE_COUNT;
		while ((p->pause_sel == PAUSE_LINK && !emu_link_available(p)) ||
		       (p->pause_sel == PAUSE_WIRELESS && !emu_wireless_available(p))) {
			p->pause_sel += in & BTN_UP ? -1 : 1; // not shown
		}
		if (in & (BTN_CIRCLE | BTN_L3 | BTN_R3)) p->state = PS_PLAYING;
		if (in & BTN_CROSS) {
			switch (p->pause_sel) {
			case PAUSE_RESUME:
				p->state = PS_PLAYING;
				break;
			case PAUSE_QUIT:
				emu_stop(p);
				p->state = PS_GAMES;
				break;
			case PAUSE_LINK:
				emu_link_set(p, !p->link_wanted);
				break;
			case PAUSE_WIRELESS:
				emu_wireless_set(p, !p->rfu_wanted);
				break;
			case PAUSE_PERF:
				show_perf = !show_perf;
				break;
			}
		}
		break;
	}
	// Ease the carousel towards the selection.
	p->game_scroll += (p->game_sel - p->game_scroll) * 0.25f;
	if (p->game_scroll - p->game_sel < 0.01f && p->game_sel - p->game_scroll < 0.01f) {
		p->game_scroll = p->game_sel;
	}
}

// The main menu and its pages: any controller drives them (`in`: everyone's presses).
static void update_menu(u32 in) {
	switch (screen) {
	case SCREEN_MENU:
		if (in & BTN_UP) menu_sel = (menu_sel + MENU_ENTRIES - 1) % MENU_ENTRIES;
		if (in & BTN_DOWN) menu_sel = (menu_sel + 1) % MENU_ENTRIES;
		if ((in & BTN_CROSS) && menu_sel == SCREEN_PLAY) {
			start_wipe(SCREEN_PLAY);
		} else if ((in & BTN_CROSS) && menu_sel == MENU_QUIT) {
			running = false; // no game runs while the menu shows: same exit as the XMB's quit
		} else if (in & BTN_CROSS) {
			about_scroll = 0;
			set_screen(menu_sel);
		}
		break;
	case SCREEN_ABOUT:
		if ((in & BTN_UP) && about_scroll > 0) --about_scroll;
		if (in & BTN_DOWN) ++about_scroll; // the page stops it at its end
		if (in & BTN_CIRCLE) set_screen(SCREEN_MENU);
		break;
	case SCREEN_HELP:
		if (in & BTN_CIRCLE) set_screen(SCREEN_MENU);
		break;
	case SCREEN_PLAY:
		break;
	}
}

// One slot per controller, laid out full screen, side by side, or 2x2.
static int visible_slots(void) {
	int highest = 1;
	for (int i = 0; i < MAX_PLAYERS; ++i) {
		if (players[i].connected || players[i].state != PS_EMPTY) {
			highest = i + 1;
		}
	}
	return highest <= 2 ? highest : 4;
}

#ifdef DEV_TOOLS
// Startup steps, a line each in BOOT_TRACE, written through lv2 and closed at once: it survives a
// hang or a console crash right after, and works before main and without newlib's stdio. Made to
// find where the app stops on PS3HEN (docs/ps3-homebrew-notes.md).
#define BOOT_TRACE "/dev_hdd0/ps3gba_boot.txt"
static void boot_trace(const char* step, bool first) {
	// " +<ms since the constructors>", formatted by hand: newlib may not be ready yet.
	static s64 start;
	s64 now = sysGetSystemTime();
	if (first) start = now;
	char line[64], digits[20];
	int n = 0, d = 0;
	for (u64 ms = (now - start) / 1000; d == 0 || ms; ms /= 10) digits[d++] = '0' + ms % 10;
	for (const char* c = step; *c && n < 40; ++c) line[n++] = *c;
	line[n++] = ' ';
	line[n++] = '+';
	while (d) line[n++] = digits[--d];
	line[n++] = ' ';
	line[n++] = 'm';
	line[n++] = 's';
	line[n++] = '\n';
	s32 fd;
	s32 flags = SYS_O_WRONLY | SYS_O_CREAT | (first ? SYS_O_TRUNC : SYS_O_APPEND);
	if (sysLv2FsOpen(BOOT_TRACE, flags, &fd, 0666, NULL, 0) == 0) {
		u64 written;
		sysLv2FsWrite(fd, line, n, &written);
		sysLv2FsClose(fd);
	}
}

__attribute__((constructor)) static void boot_trace_start(void) {
	boot_trace("constructors", true);
}
#define BOOT(step) boot_trace(step, false)
#else
#define BOOT(step) do { } while (0)
#endif

int main(void) {
	BOOT("main");
#ifdef DEV_TOOLS
	mkdir(DATA_DIR, 0777);
	bool log_ok = freopen(DEV_LOG, "w", stdout) != NULL;
	setvbuf(stdout, NULL, _IOLBF, 0);
	log_file = stdout;
	printf("dev: starting\n");
	BOOT(log_ok ? "log open" : "log failed");
#endif
	gfx_init();
	BOOT("gfx");
	audio_init();
	BOOT("audio");
	ioPadInit(7);
	sysUtilRegisterCallback(SYSUTIL_EVENT_SLOT0, sysutil_cb, NULL);
	BOOT("pad, sysutil");
	library_scan();
	BOOT("library");
	emu_init();
	BOOT("emu");
	ui_init();
	BOOT("ui");
	intro_init();
	BOOT("intro");
	web_init();
	BOOT("web");
	printf("ready: %ux%u, %d games\n", gfx_w, gfx_h, lib_num_games);
	log_memory("ready");
#ifdef DEV_TOOLS
	sys_ppu_thread_t watchdog;
	sysThreadCreate(&watchdog, dev_watchdog, NULL, 900, 0x4000, 0, "watchdog"); // above everything
#endif

	int frame = 0;
	while (running) {
#ifdef DEV_TOOLS
		// Breadcrumbs for the first frames, to locate a hang from the log alone.
		#define TRACE(step) do { if (frame < 3) printf("frame %d: %s\n", frame, step); } while (0)
#else
		#define TRACE(step) do { } while (0)
#endif
		TRACE("start");
#ifdef DEV_TOOLS
		s64 work_start = sysGetSystemTime();
#else
		#define PHASE(id) do { } while (0)
		#define PHASE_START() do { } while (0)
#endif
		PHASE_START();
		sysUtilCheckCallback();
		read_input();
		PHASE(PH_INPUT);
		TRACE("input");
#ifdef DEV_TOOLS
		dev_apply(frame);
#endif
		bool intro = intro_playing(), nobody = true;
		u32 menu_in = 0;
		for (int i = 0; i < MAX_PLAYERS; ++i) {
			if (wipe >= 0 && wipe < WIPE_DARK) {
				players[i].pressed = 0; // nothing reaches the screen being left
			}
			menu_in |= players[i].pressed;
			nobody &= players[i].state == PS_EMPTY;
		}
		if (!intro && screen != SCREEN_PLAY) {
			update_menu(menu_in);
		}
		for (int i = 0; i < MAX_PLAYERS; ++i) {
			if (intro) { // the welcome screen takes every press
				if (players[i].pressed & (BTN_START | BTN_CROSS)) intro_leave();
				continue;
			}
			if (screen != SCREEN_PLAY) {
				continue;
			}
			if (nobody && (players[i].pressed & BTN_CIRCLE)) {
				start_wipe(SCREEN_MENU); // back from the join screen while no one has joined
			}
			enum player_state before = players[i].state;
			update_player(&players[i]);
			if (players[i].state != before || players[i].flash_frames == 16) {
				printf("P%d: state %d -> %d%s %s/%s\n", i + 1, before, players[i].state,
				       players[i].flash_frames == 16 ? " (refused)" : "", players[i].game_file, players[i].save_name);
			}
		}
		web_frame();
		// The emulator threads run this frame while the main thread draws and the GPU renders
		// (the screens shown are last frame's: one frame of latency).
		emu_frame_begin();
		gfx_begin(0);
		PHASE(PH_BEGIN);
		int anim = frame; // drives the UI's animations
#ifdef DEV_TOOLS
		if (dev_frame >= 0) anim = dev_frame;
#endif
		if (!intro_playing()) {
			ui_draw_background(anim);
			PHASE(PH_BACKGROUND);
			if (screen != SCREEN_PLAY) {
				ui_draw_menu(screen, menu_sel, &about_scroll, anim);
			} else {
				int n = visible_slots();
				int cols = n == 1 ? 1 : 2, rows = n == 4 ? 2 : 1;
				float w = (float) gfx_w / cols, h = (float) gfx_h / rows;
				for (int i = 0; i < n; ++i) {
					ui_draw_slot(&players[i], (i % cols) * w, (i / cols) * h, w, h, anim, show_perf, n > 1);
				}
			}
		}
		intro_draw();
		if (wipe >= 0) {
			float s = wipe < WIPE_SHUT ? 1 - (float) wipe / WIPE_SHUT
			        : wipe < WIPE_DARK ? 0 : (float) (wipe - WIPE_DARK) / (WIPE_DONE - WIPE_DARK);
			intro_iris(gfx_w / 2.0f, gfx_h / 2.0f, hypotf(gfx_w, gfx_h) / 2 * s * s);
			if (++wipe == WIPE_SHUT) {
				set_screen(wipe_to);
			} else if (wipe == WIPE_DONE) {
				wipe = -1;
			}
		}
		gfx_submit();
		PHASE(PH_SLOTS);
		TRACE("drawn");
#ifdef DEV_TOOLS
		s64 emu_start = sysGetSystemTime();
#endif
		emu_frame_end();
#ifdef DEV_TOOLS
		dev_emu_sum += sysGetSystemTime() - emu_start;
#endif
		PHASE(PH_UPLOAD);
		TRACE("emulated");
#ifdef DEV_TOOLS
		s64 work = sysGetSystemTime() - work_start;
		dev_work_sum += work;
		if (work > dev_work_max) dev_work_max = work;
		++dev_frames;
#endif
		gfx_end();
		PHASE(PH_FLIP);
#ifdef DEV_TOOLS
		++dev_frame_counter;
#endif
		TRACE("flipped");
		if (frame == 0) {
			BOOT("first frame");
		}
		++frame;
	}

	for (int i = 0; i < MAX_PLAYERS; ++i) {
		emu_stop(&players[i]);
	}
	spu_render_shutdown();
	audio_shutdown();
	gfx_shutdown();
	return 0;
}
