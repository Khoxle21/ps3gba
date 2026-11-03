// ROM patching (docs/rom-patching.md): lists the patches that fit a game, builds a new ROM from a
// game and a patch with mGBA's patch code, and deletes what the player no longer needs after it.
// Everything runs on one thread of its own, below the emulators (it reads whole ROMs).
// ps3gba records the games it built in RECORD, one line per build: output, base, patch, base and
// output CRC32, so a chain's intermediate ROM can be told from the player's own dumps.
#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include <ppu-lv2.h>
#include <sys/cond.h>
#include <sys/mutex.h>
#include <sys/systime.h>
#include <sys/thread.h>
#include <mgba-util/crc32.h>
#include <mgba-util/patch.h>
#include <mgba-util/vfs.h>

#include "emu.h"
#include "library.h"
#include "rompatch.h"

#define RECORD PATCH_DIR "/built-games.txt"
#define ROM_MAX (32 << 20)
// Files are read and written a chunk at a time: one 32 MB read held the drive for ~0.5 s, and
// the main loop's dev log line waited for it (a UI hitch on the console).
#define CHUNK (256 << 10)
#define CRC_CACHE 64

static bool exists(const char* path) {
	struct stat st;
	return stat(path, &st) == 0;
}

bool rompatch_is_patch_file(const char* name) {
	const char* dot = strrchr(name, '.');
	return dot && (strcasecmp(dot, ".ips") == 0 || strcasecmp(dot, ".ups") == 0 || strcasecmp(dot, ".bps") == 0);
}

const char* rompatch_format(const unsigned char* head) {
	return memcmp(head, "PATCH", 5) == 0 ? "IPS" : memcmp(head, "UPS1", 4) == 0 ? "UPS" : memcmp(head, "BPS1", 4) == 0 ? "BPS" : NULL;
}

// UPS and BPS end with the base's, the result's and the patch's CRC32, little-endian.
static u32 le32(const u8* p) {
	return p[0] | p[1] << 8 | p[2] << 16 | (u32) p[3] << 24;
}

// Moves n bytes between buf and f a chunk at a time.
static bool transfer(FILE* f, u8* buf, size_t n, bool write) {
	for (size_t done = 0, k; done < n; done += k) {
		k = n - done < CHUNK ? n - done : CHUNK;
		if ((write ? fwrite(buf + done, 1, k, f) : fread(buf + done, 1, k, f)) != k) {
			return false;
		}
	}
	return true;
}

// The whole file, or NULL with what went wrong in *why.
static u8* read_whole(const char* path, size_t max, size_t* size, const char** why) {
	FILE* f = fopen(path, "rb");
	if (!f) {
		*why = "unreadable";
		return NULL;
	}
	setvbuf(f, NULL, _IONBF, 0); // straight into the buffer: buffered, 16 MB took 1-3 s
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	u8* data = NULL;
	*size = n;
	if (n < 0) {
		*why = "unreadable";
	} else if (n == 0) {
		*why = "empty"; // e.g. an FTP copy cut short
	} else if ((size_t) n > max) {
		*why = "too big";
	} else if (!(data = malloc(n))) {
		*why = "memory";
	} else if (!transfer(f, data, n, false)) {
		*why = "unreadable";
		free(data);
		data = NULL;
	}
	fclose(f);
	return data;
}

// ---------------------------------------------------------------------------------------------
// Checksums of ROMs, kept per file version. Only the patch thread touches them.

static struct {
	char game[256];
	off_t size;
	time_t mtime;
	u32 crc;
} crcs[CRC_CACHE];
static int crcs_next;

static void remember_crc(const char* game, u32 crc) {
	char path[512];
	library_rom_path(game, path, sizeof(path));
	struct stat st;
	if (stat(path, &st) != 0) {
		return;
	}
	int i = 0;
	while (i < CRC_CACHE && strcmp(crcs[i].game, game) != 0) {
		++i;
	}
	if (i == CRC_CACHE) {
		i = crcs_next++ % CRC_CACHE;
	}
	snprintf(crcs[i].game, sizeof(crcs[i].game), "%s", game);
	crcs[i].size = st.st_size;
	crcs[i].mtime = st.st_mtime;
	crcs[i].crc = crc;
}

// A game's CRC32: reads the ROM once per file version (16 MB: ~0.5 s on the console).
static bool rom_crc(const char* game, u32* crc) {
	char path[512];
	library_rom_path(game, path, sizeof(path));
	struct stat st;
	if (stat(path, &st) != 0) {
		return false;
	}
	for (int i = 0; i < CRC_CACHE; ++i) {
		if (strcmp(crcs[i].game, game) == 0 && crcs[i].size == st.st_size && crcs[i].mtime == st.st_mtime) {
			*crc = crcs[i].crc;
			return true;
		}
	}
	FILE* f = fopen(path, "rb");
	u8* buf = f ? malloc(CHUNK) : NULL;
	bool ok = buf != NULL;
	u32 c = 0;
	size_t n;
	if (ok) {
		setvbuf(f, NULL, _IONBF, 0);
		while ((n = fread(buf, 1, CHUNK, f)) > 0) {
			c = crc32(c, buf, n);
		}
		ok = !ferror(f);
	}
	free(buf);
	if (f) {
		fclose(f);
	}
	if (ok) {
		remember_crc(game, c);
		*crc = c;
	}
	return ok;
}

// ---------------------------------------------------------------------------------------------
// The record of built games

// Whether ps3gba built `game` and it is still that file (same checksum).
static bool record_has(const char* game, u32 crc) {
	FILE* f = fopen(RECORD, "r");
	char line[1024];
	bool found = false;
	while (f && !found && fgets(line, sizeof(line), f)) {
		char* field[5];
		int n = 0;
		for (char* s = strtok(line, "\t\r\n"); s && n < 5; s = strtok(NULL, "\t\r\n")) {
			field[n++] = s;
		}
		found = n == 5 && strcmp(field[0], game) == 0 && strtoul(field[4], NULL, 16) == crc;
	}
	if (f) {
		fclose(f);
	}
	return found;
}

static void record_add(const char* game, const char* base, const char* patch, u32 base_crc, u32 crc) {
	FILE* f = fopen(RECORD, "a");
	if (f) {
		fprintf(f, "%s\t%s\t%s\t%08x\t%08x\n", game, base, patch, (unsigned) base_crc, (unsigned) crc);
		fclose(f);
	}
}

// Drops `game`'s lines (it was deleted): a game of that name copied later is the player's own.
static void record_remove(const char* game) {
	FILE* in = fopen(RECORD, "r");
	FILE* out = in ? fopen(RECORD ".tmp", "w") : NULL;
	char line[1024];
	size_t n = strlen(game);
	while (out && fgets(line, sizeof(line), in)) {
		if (!(strncmp(line, game, n) == 0 && line[n] == '\t')) {
			fputs(line, out);
		}
	}
	if (in) {
		fclose(in);
	}
	if (out && fclose(out) == 0) {
		remove(RECORD); // lv2 may refuse to rename over a file
		rename(RECORD ".tmp", RECORD);
	}
}

// ---------------------------------------------------------------------------------------------
// Jobs

static bool has_saves(const char* game) {
	char path[512];
	snprintf(path, sizeof(path), "%s/%s", SAVE_DIR, game);
	DIR* dir = opendir(path);
	struct dirent* e;
	bool any = false;
	while (dir && !any && (e = readdir(dir))) {
		any = e->d_name[0] != '.'; // a save, or the leftover of an interrupted one
	}
	if (dir) {
		closedir(dir);
	}
	return any;
}

// The game a patch makes from `game`: the patch's name with the base ROM's extension, e.g.
// "Some Hack.ups" on a GBA game -> "Some Hack" (roms/Some Hack.gba), on "X.gbc" -> "Some Hack.gbc".
static void output_name(const char* game, const char* patch, char* out, size_t len, char* path, size_t path_len) {
	char rom[512], stem[256];
	library_rom_path(game, rom, sizeof(rom));
	const char* ext = strrchr(rom, '.');
	snprintf(stem, sizeof(stem), "%.*s", (int) (strrchr(patch, '.') - patch), patch);
	if (library_rom_system(stem) >= 0) {
		*strrchr(stem, '.') = '\0'; // "Some Hack.gba.ups"
	}
	bool gba = strcasecmp(ext, ".gba") == 0; // GBA games are named without it (struct game)
	snprintf(out, len, "%s%s", stem, gba ? "" : ext);
	snprintf(path, path_len, "%s/%s%s", ROM_DIR, stem, ext);
}

// A file's first 5 and last 12 bytes.
static bool read_ends(const char* path, u8* head, u8* foot) {
	FILE* f = fopen(path, "rb");
	bool ok = f && fread(head, 1, 5, f) == 5 && fseek(f, -12, SEEK_END) == 0 && fread(foot, 1, 12, f) == 12;
	if (f) {
		fclose(f);
	}
	return ok;
}

static int compare_patches(const void* a, const void* b) {
	return strcasecmp(((const struct patch_info*) a)->file, ((const struct patch_info*) b)->file);
}

// The patches listed for a game: UPS and BPS made for it (their base checksum is its) or for
// another version of it (another ROM with its header code: greyed), every IPS (no checksum).
static int list_patches(const char* game, struct patch_info* out) {
	u32 crc;
	bool have_crc = rom_crc(game, &crc);
	// Other versions of this game, checksummed only if a patch needs them.
	struct game* games = malloc(LIB_MAX_GAMES * sizeof(*games));
	int num_games = games ? library_read_games(games) : 0, self = 0;
	while (self < num_games && strcmp(games[self].file, game) != 0) {
		++self;
	}
	u32 others[LIB_MAX_GAMES];
	int num_others = -1;
	int n = 0;
	DIR* dir = opendir(PATCH_DIR);
	struct dirent* e;
	while (dir && (e = readdir(dir)) && n < PATCH_LIST_MAX) {
		if (e->d_name[0] == '.' || !rompatch_is_patch_file(e->d_name)) {
			continue;
		}
		struct patch_info* p = &out[n];
		snprintf(p->file, sizeof(p->file), "%.255s", e->d_name);
		char path[512];
		u8 head[5], foot[12];
		snprintf(path, sizeof(path), "%s/%s", PATCH_DIR, p->file);
		const char* format = read_ends(path, head, foot) ? rompatch_format(head) : NULL;
		if (format) {
			snprintf(p->format, sizeof(p->format), "%s", format);
		} else { // not recognized: the build says what is wrong
			const char* ext = strrchr(e->d_name, '.') + 1;
			for (int i = 0; i < 4; ++i) {
				p->format[i] = toupper((unsigned char) ext[i]);
			}
		}
		p->fit = FIT_UNCHECKED;
		if (format && strcmp(format, "IPS") != 0) {
			u32 base = le32(foot);
			p->fit = have_crc && base == crc ? FIT_OK : FIT_OTHER;
			if (p->fit == FIT_OTHER && num_others < 0) {
				num_others = 0;
				for (int i = 0; i < num_games && self < num_games; ++i) {
					if (i != self && library_same_game(&games[i], &games[self]) && rom_crc(games[i].file, &others[num_others])) {
						++num_others;
					}
				}
			}
			bool other = false;
			for (int i = 0; i < num_others && !other; ++i) {
				other = others[i] == base;
			}
			if (p->fit == FIT_OTHER && !other) {
				continue; // made for another game
			}
		}
		char made[256], made_path[512];
		output_name(game, p->file, made, sizeof(made), made_path, sizeof(made_path));
		if (p->fit != FIT_OTHER && library_game_exists(made)) {
			p->fit = FIT_BUILT;
		}
		++n;
	}
	if (dir) {
		closedir(dir);
	}
	free(games);
	qsort(out, n, sizeof(*out), compare_patches);
	log_printf("patch: %d patches for %s (crc32 %08x)\n", n, game, have_crc ? (unsigned) crc : 0);
	return n;
}

static volatile int job_step;

// Free bytes on the drive (lv2's sys_fs_disk_free), or -1 if it can't tell.
static s32 disk_free(const char* path, u64* total, u64* avail) {
	lv2syscall3(840, (u64) path, (u64) total, (u64) avail);
	return_to_user_prog(s32);
}

struct build {
	u8* patch;
	u8* rom;
	u8* out;
	size_t patch_size, rom_size, out_size;
	struct VFile* vf;
	s64 start, read, applied, written; // dev log: when it started and each step ended
	s64 free_mb;                       // dev log: the drive's free space, -1 if unknown
};

// What to tell the player when read_whole() failed on the patch or the game file.
static const char* read_error(const char* why, bool patch) {
	if (strcmp(why, "memory") == 0) {
		return "NOT ENOUGH MEMORY. RESTART PS3GBA AND TRY AGAIN.";
	} else if (strcmp(why, "too big") == 0) {
		return patch ? "THE PATCH FILE IS TOO BIG (64 MB AT MOST)." : "THE GAME FILE IS OVER 32 MB.";
	} else if (strcmp(why, "empty") == 0) {
		return patch ? "THE PATCH FILE IS EMPTY. COPY IT AGAIN." : "THE GAME FILE IS EMPTY.";
	}
	return patch ? "CAN'T READ THE PATCH FILE. COPY IT AGAIN." : "CAN'T READ THE GAME FILE.";
}

static bool fail(struct patch_result* r, const char* error) {
	snprintf(r->error, sizeof(r->error), "%s", error);
	return false;
}

static bool build_rom(const char* game, const char* patch_file, struct build* b, struct patch_result* r) {
	char rom_path[512], patch_path[512], out_path[512], tmp[64], error[160];
	library_rom_path(game, rom_path, sizeof(rom_path));
	snprintf(patch_path, sizeof(patch_path), "%s/%s", PATCH_DIR, patch_file);
	output_name(game, patch_file, r->game, sizeof(r->game), out_path, sizeof(out_path));
	if (library_game_exists(r->game)) {
		snprintf(error, sizeof(error), "%.80s IS ALREADY A GAME. RENAME THE PATCH FILE TO MAKE ANOTHER ONE.", r->game);
		return fail(r, error);
	}
	const char* why;
	if (!(b->patch = read_whole(patch_path, PATCH_FILE_MAX, &b->patch_size, &why))) {
		return fail(r, read_error(why, true));
	}
	struct Patch patch;
	const char* format = b->patch_size >= 12 ? rompatch_format(b->patch) : NULL;
	b->vf = VFileFromConstMemory(b->patch, b->patch_size);
	if (!format || !b->vf || !loadPatch(b->vf, &patch)) { // UPS and BPS: also the patch's own checksum
		return fail(r, "THE PATCH FILE IS DAMAGED, OR NOT AN IPS, UPS OR BPS PATCH. COPY IT AGAIN.");
	}
	if (!(b->rom = read_whole(rom_path, ROM_MAX, &b->rom_size, &why))) {
		return fail(r, read_error(why, false));
	}
	u32 base_crc = doCrc32(b->rom, b->rom_size);
	remember_crc(game, base_crc);
	bool checked = strcmp(format, "IPS") != 0; // UPS and BPS know their base
	if (checked && le32(b->patch + b->patch_size - 12) != base_crc) {
		return fail(r, "THIS PATCH IS MADE FOR ANOTHER VERSION OF THE GAME (ANOTHER REGION OR REVISION).");
	}
	b->out_size = patch.outputSize(&patch, b->rom_size);
	if (!b->out_size) {
		return fail(r, "THE PATCH FILE IS DAMAGED. COPY IT AGAIN.");
	}
	if (b->out_size > ROM_MAX) {
		return fail(r, "THE NEW GAME WOULD BE OVER 32 MB.");
	}
	b->read = sysGetSystemTime();
	job_step = STEP_PATCHING;
	// Zeroed: UPS leaves the bytes past the base's end to the caller.
	if (!(b->out = calloc(1, b->out_size))) {
		return fail(r, "NOT ENOUGH MEMORY. RESTART PS3GBA AND TRY AGAIN.");
	}
	// UPS and BPS check the result's checksum, BPS the base's too.
	if (!patch.applyPatch(&patch, b->rom, b->rom_size, b->out, b->out_size)) {
		return fail(r, "THE RESULT IS WRONG: THE PATCH OR THE GAME FILE IS DAMAGED.");
	}
	u32 crc = checked ? le32(b->patch + b->patch_size - 8) : doCrc32(b->out, b->out_size);
	b->applied = sysGetSystemTime();
	job_step = STEP_WRITING;
	u64 total, avail;
	b->free_mb = disk_free("/dev_hdd0", &total, &avail) == 0 ? (s64) (avail >> 20) : -1;
	if (b->free_mb >= 0 && avail < b->out_size + (1 << 20)) {
		snprintf(error, sizeof(error), "NOT ENOUGH SPACE ON THE PS3'S DRIVE: %u MB NEEDED.", (unsigned) (b->out_size >> 20) + 1);
		return fail(r, error);
	}
	// A temporary file renamed into place whole: a crash never leaves half a game in roms/.
	snprintf(tmp, sizeof(tmp), "%s/patch.part", TMP_DIR);
	mkdir(TMP_DIR, 0777);
	FILE* f = fopen(tmp, "wb");
	bool ok = f != NULL;
	if (f) {
		setvbuf(f, NULL, _IONBF, 0);
		ok = transfer(f, b->out, b->out_size, true);
		ok = fclose(f) == 0 && ok;
	}
	struct stat st;
	ok = ok && stat(tmp, &st) == 0 && (size_t) st.st_size == b->out_size;
	if (!ok) {
		remove(tmp);
		return fail(r, "COULD NOT WRITE THE NEW GAME. IS THE DRIVE FULL?");
	}
	library_place_lock(); // a phone upload of that name meanwhile: one of them gets it
	ok = !library_game_exists(r->game) && library_place(tmp, out_path);
	library_place_unlock();
	if (!ok) {
		remove(tmp);
		snprintf(error, sizeof(error), "%.80s IS ALREADY A GAME. RENAME THE PATCH FILE TO MAKE ANOTHER ONE.", r->game);
		return fail(r, error);
	}
	b->written = sysGetSystemTime();
	record_add(r->game, game, patch_file, base_crc, crc);
	remember_crc(r->game, crc);
	if (record_has(game, base_crc) && !has_saves(game)) {
		snprintf(r->base, sizeof(r->base), "%s", game); // a chain's intermediate: the cleanup offers it
	}
	return true;
}

static void build(const char* game, const char* patch_file, struct patch_result* r) {
	struct build b = { .start = sysGetSystemTime(), .free_mb = -1 };
	r->ok = build_rom(game, patch_file, &b, r);
	if (b.vf) {
		b.vf->close(b.vf);
	}
	free(b.patch);
	free(b.rom);
	free(b.out);
	if (r->ok) {
		library_rescan(NULL); // the new game appears, its cover by its header code
	}
	log_printf("patch: %s + %s -> %s %s%s (%u KB, %.2f s)\n", game, patch_file, r->game, r->ok ? "done" : "failed: ",
	           r->ok ? "" : r->error, (unsigned) (b.out_size >> 10), (sysGetSystemTime() - b.start) / 1e6);
	if (r->ok) {
		log_printf("patch: read %.2f s, applied %.2f s, written %.2f s, drive %lld MB free\n", (b.read - b.start) / 1e6,
		           (b.applied - b.read) / 1e6, (b.written - b.applied) / 1e6, (long long) b.free_mb);
	}
}

static void clean(const char* patch_file, const char* base, struct patch_result* r) {
	char path[512], error[160];
	snprintf(path, sizeof(path), "%s/%s", PATCH_DIR, patch_file);
	r->ok = remove(path) == 0 || !exists(path); // gone already: another player deleted it
	if (!r->ok) {
		fail(r, "COULD NOT DELETE THE PATCH FILE.");
	}
	// Checked again, under the place lock (a phone save upload for it waits): never a ROM ps3gba
	// did not build, nor one with saves. Gone already is fine (another player's cleanup).
	const char* kept = NULL;
	bool deleted = false;
	u32 crc;
	library_place_lock();
	if (base[0] && library_game_exists(base)) {
		if (!rom_crc(base, &crc)) {
			kept = "IT COULD NOT BE READ";
		} else if (!record_has(base, crc)) {
			kept = "IT CHANGED SINCE PS3GBA BUILT IT";
		} else if (has_saves(base)) {
			kept = "IT HAS SAVES NOW";
		} else {
			library_rom_path(base, path, sizeof(path));
			deleted = remove(path) == 0;
			kept = deleted ? NULL : "ITS FILE COULD NOT BE DELETED";
			snprintf(path, sizeof(path), "%s/%s", SAVE_DIR, base);
			rmdir(path); // empty, if there at all
		}
	}
	library_place_unlock();
	if (deleted) {
		record_remove(base);
		library_rescan(NULL);
	}
	if (kept) {
		snprintf(error, sizeof(error), "%.80s WAS KEPT: %s.", base, kept);
		r->ok = fail(r, error);
	}
	log_printf("patch: deleted %s%s%s %s%s\n", patch_file, base[0] ? " and " : "", base, r->ok ? "done" : "failed: ",
	           r->ok ? "" : r->error);
}

// ---------------------------------------------------------------------------------------------
// The patch thread and its requests

static sys_mutex_t lock;
static sys_cond_t wake;
static struct {
	char game[256];
	unsigned want, done; // request numbers: the result is in once done == want
	int n;
	struct patch_info patches[PATCH_LIST_MAX];
} lists[LIB_LIST_SLOTS];
static struct {
	int slot; // the player who asked, -1: none. Only the main thread changes it.
	bool clean, taken;
	char game[256], patch[256], base[256];
	struct patch_result result;
} job = { .slot = -1 };

static void patch_thread(void* arg) {
	(void) arg;
	sysMutexLock(lock, 0);
	for (;;) {
		if (job.slot >= 0 && !job.taken) {
			job.taken = true;
			char game[256], patch[256], base[256];
			snprintf(game, sizeof(game), "%s", job.game);
			snprintf(patch, sizeof(patch), "%s", job.patch);
			snprintf(base, sizeof(base), "%s", job.base);
			bool cleanup = job.clean;
			sysMutexUnlock(lock);
			struct patch_result r = { 0 };
			if (cleanup) {
				clean(patch, base, &r);
			} else {
				build(game, patch, &r);
			}
			sysMutexLock(lock, 0);
			job.result = r;
			job_step = STEP_DONE;
			continue;
		}
		int slot = -1;
		for (int i = 0; i < LIB_LIST_SLOTS && slot < 0; ++i) {
			if (lists[i].done != lists[i].want) {
				slot = i;
			}
		}
		if (slot < 0) {
			sysCondWait(wake, 0);
			continue;
		}
		char game[256];
		snprintf(game, sizeof(game), "%s", lists[slot].game);
		unsigned want = lists[slot].want;
		sysMutexUnlock(lock);
		struct patch_info patches[PATCH_LIST_MAX];
		int n = list_patches(game, patches);
		sysMutexLock(lock, 0);
		if (lists[slot].want == want) { // else a newer request replaced it: read again
			memcpy(lists[slot].patches, patches, n * sizeof(*patches));
			lists[slot].n = n;
			lists[slot].done = want;
		}
	}
}

// False if the thread can't be created (tried again at the next request).
static bool start_thread(void) {
	static bool ready, started;
	if (!ready) {
		sys_mutex_attr_t mutex_attr;
		sysMutexAttrInitialize(mutex_attr);
		sysMutexCreate(&lock, &mutex_attr);
		sys_cond_attr_t cond_attr;
		sysCondAttrInitialize(cond_attr);
		sysCondCreate(&wake, lock, &cond_attr);
		ready = true;
	}
	if (!started) {
		sys_ppu_thread_t thread;
		// Below the emulators (1200): checksumming ROMs for a list must not cost anyone a frame. A
		// build runs while nobody plays. 256 KB like the covers thread: library_rescan() decodes
		// cover PNGs.
		started = sysThreadCreate(&thread, patch_thread, NULL, 1250, 256 * 1024, 0, "patches") == 0;
		if (!started) {
			log_printf("patch: could not create its thread\n");
		}
	}
	return started;
}

void rompatch_list_async(int slot, const char* game) {
	bool thread = start_thread();
	sysMutexLock(lock, 0);
	snprintf(lists[slot].game, sizeof(lists[slot].game), "%s", game);
	if (!thread) { // the answer right away: it can't
		lists[slot].n = -2;
		lists[slot].done = lists[slot].want + 1;
	}
	++lists[slot].want;
	sysCondBroadcast(wake);
	sysMutexUnlock(lock);
}

int rompatch_list_result(int slot, struct patch_info* out) {
	sysMutexLock(lock, 0);
	int n = lists[slot].done == lists[slot].want ? lists[slot].n : -1;
	if (n > 0) {
		memcpy(out, lists[slot].patches, n * sizeof(*out));
	}
	sysMutexUnlock(lock);
	return n;
}

static bool start_job(int slot, bool cleanup, const char* game, const char* patch, const char* base) {
	bool thread = start_thread();
	if (job.slot >= 0) {
		return false;
	}
	sysMutexLock(lock, 0);
	if (!thread) { // done at once with what to do: no game stays refused
		job.result = (struct patch_result) { .error = "COULD NOT START PATCHING. RESTART PS3GBA AND TRY AGAIN." };
		job_step = STEP_DONE;
		job.slot = slot;
		sysMutexUnlock(lock);
		return true;
	}
	job.clean = cleanup;
	job.taken = false;
	snprintf(job.game, sizeof(job.game), "%s", game);
	snprintf(job.patch, sizeof(job.patch), "%s", patch);
	snprintf(job.base, sizeof(job.base), "%s", base);
	job_step = cleanup ? STEP_DELETING : STEP_READING;
	job.slot = slot;
	sysCondBroadcast(wake);
	sysMutexUnlock(lock);
	return true;
}

bool rompatch_build_async(int slot, const char* game, const char* patch) {
	return start_job(slot, false, game, patch, "");
}

bool rompatch_clean_async(int slot, const char* patch, const char* base) {
	return start_job(slot, true, "", patch, base);
}

enum patch_step rompatch_job_step(int slot) {
	enum patch_step step = job.slot == slot ? job_step : STEP_DONE;
	__sync_synchronize(); // what the job did before it is seen after (e.g. its library rescan)
	return step;
}

void rompatch_job_take(int slot, struct patch_result* out) {
	if (job.slot != slot || job_step != STEP_DONE) {
		*out = (struct patch_result) { .error = "NO RESULT" };
		return;
	}
	sysMutexLock(lock, 0);
	*out = job.result;
	job.slot = -1;
	sysMutexUnlock(lock);
}

bool rompatch_busy(void) {
	return job.slot >= 0;
}
