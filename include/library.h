#ifndef LIBRARY_H
#define LIBRARY_H

#include <stdbool.h>
#include <stddef.h>
#include <time.h>

#include "gfx.h"

#define DATA_DIR  "/dev_hdd0/ps3gba"
#define ROM_DIR   DATA_DIR "/roms"
#define SAVE_DIR  DATA_DIR "/saves"
#define COVER_DIR DATA_DIR "/covers"
#define PATCH_DIR DATA_DIR "/patches" // IPS, UPS and BPS patches (rompatch.c)
#define TMP_DIR   DATA_DIR "/tmp"     // files being written, renamed into place whole (emptied at boot)

#define LIB_MAX_GAMES 256
#define LIB_MAX_SAVES 32
// Longest save name the keyboard types: still fits a saves row beside "JUST NOW" in the
// smallest slot (2x2 at 720p).
#define LIB_SAVE_NAME_MAX 10

// One catalog per system in the carousel, in this order.
enum game_system { SYS_GBA, SYS_GBC, SYS_GB, SYS_COUNT };

struct game {
	// Names its saves (saves/<file>/) and cover (covers/<file>.png): the ROM's file name, without
	// the extension for .gba (e.g. "Some Game (Europe)"), with it for .gb/.gbc,
	// so games of different systems never share saves. Subfolders of roms/ don't change it.
	char file[256];
	char title[96];  // display name, e.g. "Some Game"
	char code[17];   // from the ROM header: GBA game code ("BPRE"), GB/GBC title; "" if unusable
	enum game_system system;
	struct gfx_tex cover;
	bool has_cover;
	bool cover_shared; // another game's cover (same code): freed with that game
};

struct save_info {
	char name[64];
	time_t mtime;
};

// Sorted by system, then title: each system's games (a catalog) are contiguous.
extern struct game lib_games[LIB_MAX_GAMES];
extern int lib_num_games;
// A system's catalog: its games are lib_games[*first .. *first + count), count returned.
int library_catalog(int system, int* first);

// Boot only: lists the games on the calling thread, then loads their covers on a thread of its own
// (a rescan: the main loop swaps them in like library_rescan's).
void library_scan(void);
// Reads the game folder again on the calling thread (one below the emulators), reusing the
// loaded covers except new_cover's (a game file, or NULL). The main loop then swaps the result
// in between frames: library_rescan_ready(), then library_take_rescan().
void library_rescan(const char* new_cover);
bool library_rescan_ready(void);
void library_take_rescan(void);
// The games on the drive, sorted like lib_games, covers not loaded (out holds LIB_MAX_GAMES): for
// threads that can't read lib_games.
int library_read_games(struct game* out);
// The system of a ROM file name by its extension (.gba, .gb, .gbc), or -1.
int library_rom_system(const char* name);
// Index of a game by name (struct game.file), or -1.
int library_find(const char* file);
// Versions of one game: the same header code, for GBA without its region letter (BPRE/BPRF:
// FireRed). Hacks keep their original's.
bool library_same_game(const struct game* a, const struct game* b);
// Games are identified by file name, which stays valid when the library is rescanned.
// Reads a game's saves on a background thread: a folder the drive hasn't cached takes up to
// ~60 ms, which would stall every player's game. One request per slot (player) at a time.
#define LIB_LIST_SLOTS 4
void library_list_saves_async(int slot, const char* file);
// Same thread, first renaming, copying or deleting the save `save` (to `to`), then listing.
// Never overwrites a save: RENAME and COPY fail if `to` exists.
enum lib_save_op { LIB_LIST, LIB_RENAME, LIB_COPY, LIB_DELETE };
void library_save_op_async(int slot, const char* file, enum lib_save_op op, const char* save, const char* to);
// The slot's latest request: its saves, most recently played first, or -1 while still reading.
// `op_ok` (may be NULL) tells whether its rename, copy or delete worked.
int library_saves_result(int slot, struct save_info* out, bool* op_ok);
// Same, blocking: for background threads.
int library_list_saves(const char* file, struct save_info* out, int max);
void library_save_path(const char* file, const char* save, char* out, size_t len);
// The ROM's path: in roms/ or one of its subfolders (in roms/ itself if it doesn't exist).
void library_rom_path(const char* file, char* out, size_t len);
// Whether a game's ROM is on the drive.
bool library_game_exists(const char* file);
// Files moved into the library, or deleted from it, after checking what is there (phone uploads,
// ROM patching) take this lock: a check and its rename or removal are then one step.
void library_place_lock(void);
void library_place_unlock(void);
// Renames tmp to path if path doesn't exist yet (with the place lock held).
bool library_place(const char* tmp, const char* path);

#endif
