#ifndef EMU_H
#define EMU_H

#include <stdbool.h>

#include "player.h"

void emu_init(void);
// Starts loading p->game on p->save_name on the player's thread; false if it can't start.
// emu_load_status() then reports LOAD_OK, or LOAD_FAILED (call emu_stop()).
bool emu_start(struct player* p);
enum load_status emu_load_status(struct player* p);
// Writes the save and frees the core.
void emu_stop(struct player* p);
// Releases the next frame to every running player's emulation thread (non-blocking).
void emu_frame_begin(void);
// Waits for those frames (bounded, never hangs) and copies finished screens to textures.
void emu_frame_end(void);
// The running game's screen in pixels: 240x160 (GBA) or 160x144 (GB/GBC), drawn from the
// top-left corner of the player's frames and textures.
void emu_screen_size(const struct player* p, unsigned* w, unsigned* h);
// A running GBA game can be plugged into the link cable or have a Wireless Adapter on its link
// port, not both (GB/GBC games have neither; LINK_CABLES=0 builds no cable). The switches take
// effect on the player's emulation thread; the next game starts with neither.
bool emu_link_available(const struct player* p);
bool emu_wireless_available(const struct player* p);
void emu_link_set(struct player* p, bool on);
void emu_wireless_set(struct player* p, bool on);
// Player currently using this save, or -1.
int emu_save_owner(const char* game_file, const char* save);
// Someone is in a game (loading, playing or paused): no ROM patching then.
bool emu_anyone_playing(void);

#include <stdio.h>

// Log output for every thread: newlib's stdout is per thread, so main sets this after
// redirecting its own stdout to the dev log file (NULL = the calling thread's stdout).
extern FILE* log_file;
void log_printf(const char* format, ...) __attribute__((format(printf, 1, 2)));
// Logs lv2's free user memory and newlib's heap use (`when` names the moment).
void log_memory(const char* when);

// Dev builds: where the main loop currently is, for the watchdog (see main.c).
extern volatile const char* emu_where;
extern volatile int emu_where_player;
// Dev builds: every game makes sound samples, heard or not (SAMPLEALL command, to measure).
extern volatile bool emu_sample_all;
// Dev builds: mGBA's lockstep state for a player, to debug link stalls.
void emu_link_debug(const struct player* p);
// Dev sound self-check (soundcheck.c): what a game can observe, and the whole audio state, of
// two cores of the same game. False (0) with what differs; -1: audio not comparable right now.
struct mCore;
bool soundcheck_same_state(struct mCore* a, struct mCore* b, char* what, size_t size);
int soundcheck_same_audio(struct mCore* a, struct mCore* b, char* what, size_t size);

#endif
