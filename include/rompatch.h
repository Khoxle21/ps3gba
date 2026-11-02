#ifndef ROMPATCH_H // not PATCH_H: mGBA's patch.h uses it
#define ROMPATCH_H

#include <stdbool.h>
#include <stddef.h>

// ROM patching: IPS, UPS and BPS patches in PATCH_DIR, applied with mGBA's patch code into a new
// ROM file in roms/, which is then an ordinary game. Everything runs on one thread of its own
// (it reads whole ROMs): lists per player (slot), one build or cleanup at a time for everyone.

#define PATCH_LIST_MAX 32

// How a patch fits the game it is listed for.
enum patch_fit {
	FIT_OK,        // UPS or BPS made for this ROM (its checksum)
	FIT_UNCHECKED, // IPS (no checksum in the format), or not recognized as a patch
	FIT_OTHER,     // UPS or BPS made for another version of this game (a ROM with its code)
	FIT_BUILT,     // the game it makes is already there
};

struct patch_info {
	char file[256]; // in PATCH_DIR, with its extension
	char format[4]; // "IPS", "UPS" or "BPS", from the file's start (else its extension)
	int fit;        // enum patch_fit
};

enum patch_step { STEP_READING, STEP_PATCHING, STEP_WRITING, STEP_DELETING, STEP_DONE };

struct patch_result {
	bool ok;
	char error[160]; // what went wrong and what to do
	char game[256];  // build: the new game (as struct game.file)
	// build: its base when ps3gba built that one too (a chain) and nobody has saves for it, which
	// the cleanup may then delete; else "".
	char base[256];
};

// A patch file name: .ips, .ups or .bps.
bool rompatch_is_patch_file(const char* name);
// The format from a file's first 5 bytes: "IPS", "UPS", "BPS", or NULL.
const char* rompatch_format(const unsigned char* head);
// The patches for a game (struct game.file), read off the main thread. -1 while still reading.
void rompatch_list_async(int slot, const char* game);
int rompatch_list_result(int slot, struct patch_info* out);
// Starts building a game from `game` and the patch, or deleting the patch file (and `base`, a
// game ps3gba built without saves, if not ""). False if another job runs.
bool rompatch_build_async(int slot, const char* game, const char* patch);
bool rompatch_clean_async(int slot, const char* patch, const char* base);
// The slot's job: its step while it runs, STEP_DONE once its result is in (or it isn't this
// slot's). Then rompatch_job_take() reads the result and ends the job.
enum patch_step rompatch_job_step(int slot);
void rompatch_job_take(int slot, struct patch_result* out);
// A job runs: no game may start (the build reads and replaces games, and needs the memory).
bool rompatch_busy(void);

#endif
