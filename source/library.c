// Game library on the PS3 drive:
//   roms/[<folder>/]<name>.gba|.gb|.gbc, covers/<name>.png, saves/<name>/<save>.sav
// (<name> keeps the .gb/.gbc extension, see struct game).
#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <cairo/cairo.h>
#include <ppu-lv2.h>
#include <sys/cond.h>
#include <sys/mutex.h>
#include <sys/systime.h>
#include <sys/thread.h>

#include "emu.h"
#include "library.h"

struct game lib_games[LIB_MAX_GAMES];
int lib_num_games;

static bool has_ext(const char* name, const char* ext) {
	const char* dot = strrchr(name, '.');
	return dot && strcasecmp(dot, ext) == 0;
}

// The system of a ROM file name by its extension, or -1.
int library_rom_system(const char* name) {
	static const char* const exts[] = { [SYS_GBA] = ".gba", [SYS_GB] = ".gb", [SYS_GBC] = ".gbc" };
	for (int i = 0; i < (int) (sizeof(exts) / sizeof(*exts)); ++i) {
		if (has_ext(name, exts[i])) {
			return i;
		}
	}
	return -1;
}

// "Some Game (Europe) (Rev 1)" -> "Some Game"
static void make_title(const char* file, char* out, size_t len) {
	const char* paren = strstr(file, " (");
	size_t n = paren ? (size_t) (paren - file) : strlen(file);
	if (n >= len) {
		n = len - 1;
	}
	memcpy(out, file, n);
	out[n] = '\0';
}

// The game's identity in its ROM header, which most hacks keep: the GBA game code at 0xAC
// (e.g. BPRE for FireRed), the GB/GBC title at 0x134 (15 characters when 0x143 flags a GBC game).
static void read_code(const char* path, struct game* g) {
	u8 h[0x150];
	FILE* f = fopen(path, "rb");
	size_t n = f ? fread(h, 1, sizeof(h), f) : 0;
	if (f) {
		fclose(f);
	}
	if (n < sizeof(h)) {
		return;
	}
	bool gba = g->system == SYS_GBA;
	const u8* at = gba ? &h[0xAC] : &h[0x134];
	int len = gba ? 4 : (h[0x143] & 0x80) ? 15 : 16, k = 0;
	for (; k < len && at[k]; ++k) {
		bool ok = gba ? (at[k] >= 'A' && at[k] <= 'Z') || (at[k] >= '0' && at[k] <= '9') : at[k] >= ' ' && at[k] <= '~';
		if (!ok) {
			return; // not a code: homebrew or garbage
		}
		g->code[k] = strchr("/\\:*?\"<>|", at[k]) ? '_' : at[k]; // it names a cover file
	}
	while (k > 0 && g->code[k - 1] == ' ') {
		--k;
	}
	g->code[k] = '\0';
	if (k < (gba ? 4 : 3)) {
		g->code[0] = '\0';
	}
}

// Loads covers/<name>.png into the game, scaled to fit a texture slot with its aspect ratio kept.
static bool load_cover(struct game* g, const char* name) {
	char path[512];
	snprintf(path, sizeof(path), "%s/%s.png", COVER_DIR, name);
	cairo_surface_t* img = cairo_image_surface_create_from_png(path);
	if (cairo_surface_status(img) != CAIRO_STATUS_SUCCESS) {
		cairo_surface_destroy(img);
		return false;
	}
	int iw = cairo_image_surface_get_width(img), ih = cairo_image_surface_get_height(img);
	double scale = (double) GFX_SLOT_SIZE / (iw > ih ? iw : ih);
	int w = iw * scale + 0.5, h = ih * scale + 0.5;
	bool ok = gfx_tex_create_slot(&g->cover, w, h);
	if (ok) {
		cairo_surface_t* out = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
		cairo_t* cr = cairo_create(out);
		cairo_scale(cr, scale, scale);
		cairo_set_source_surface(cr, img, 0, 0);
		cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BEST);
		cairo_paint(cr);
		cairo_surface_flush(out);
		gfx_tex_upload(&g->cover, cairo_image_surface_get_data(out), cairo_image_surface_get_stride(out));
		cairo_destroy(cr);
		cairo_surface_destroy(out);
	}
	cairo_surface_destroy(img);
	return ok;
}

// Saves used to live at saves/<name>.sav; move them to saves/<name>/SAVE 1.sav.
static void migrate_legacy_save(const struct game* g) {
	char old_path[512], dir[512], new_path[600];
	if (snprintf(old_path, sizeof(old_path), "%s/%s.sav", SAVE_DIR, g->file) >= (int) sizeof(old_path) ||
	    snprintf(dir, sizeof(dir), "%s/%s", SAVE_DIR, g->file) >= (int) sizeof(dir)) {
		return;
	}
	snprintf(new_path, sizeof(new_path), "%s/SAVE 1.sav", dir);
	struct stat st;
	if (stat(old_path, &st) == 0 && stat(new_path, &st) != 0) {
		mkdir(dir, 0777);
		rename(old_path, new_path);
	}
}

static int compare_games(const void* a, const void* b) {
	const struct game *ga = a, *gb = b;
	int c = ga->system != gb->system ? (int) ga->system - (int) gb->system : strcasecmp(ga->title, gb->title);
	return c ? c : strcmp(ga->file, gb->file);
}

static int find_game(const struct game* games, int n, const char* file) {
	for (int i = 0; i < n; ++i) {
		if (strcmp(games[i].file, file) == 0) {
			return i;
		}
	}
	return -1;
}

// Adds the ROMs in roms/<sub> to games[*n] ("" for roms/ itself, whose other entries are tried
// as subfolders: one level deep). Games are named by file only, so the same name in another
// folder is skipped.
static void scan_dir(struct game* games, int* n, const char* sub) {
	char path[512];
	snprintf(path, sizeof(path), "%s%s%s", ROM_DIR, sub[0] ? "/" : "", sub);
	DIR* dir = opendir(path);
	if (!dir) {
		return;
	}
	struct dirent* e;
	while ((e = readdir(dir)) && *n < LIB_MAX_GAMES) {
		int system = library_rom_system(e->d_name);
		if (e->d_name[0] == '.') {
			continue;
		}
		if (system < 0) {
			if (!sub[0]) {
				scan_dir(games, n, e->d_name);
			}
			continue;
		}
		struct game* g = &games[*n];
		memset(g, 0, sizeof(*g));
		g->system = system;
		g->cover.slot = -1;
		int base = strrchr(e->d_name, '.') - e->d_name;
		snprintf(g->file, sizeof(g->file), "%.*s", system == SYS_GBA ? base : (int) strlen(e->d_name), e->d_name);
		if (find_game(games, *n, g->file) >= 0) {
			printf("library: %s/%s skipped, a game of that name is already listed\n", path, e->d_name);
			continue;
		}
		char name[256];
		snprintf(name, sizeof(name), "%.*s", base, e->d_name);
		make_title(name, g->title, sizeof(g->title));
		char rom[768];
		snprintf(rom, sizeof(rom), "%s/%s", path, e->d_name);
		read_code(rom, g);
		++*n;
	}
	closedir(dir);
}

// Lists roms/ into `games`, sorted by title, covers not loaded yet.
int library_read_games(struct game* games) {
	int n = 0;
	scan_dir(games, &n, "");
	qsort(games, n, sizeof(*games), compare_games);
	return n;
}

bool library_same_game(const struct game* a, const struct game* b) {
	bool gba = a->system == SYS_GBA;
	return a->code[0] && gba == (b->system == SYS_GBA) && strncmp(a->code, b->code, gba ? 3 : sizeof(a->code)) == 0;
}

// Covers, first found: covers/<file>.png (GB/GBC: also without the extension), one named after
// the header code (covers/BPRE.png), another game's with the same code (a FireRed hack shows
// FireRed's). Without one, the UI draws a blank cartridge of the game's system.
// Games also in `old` keep the cover they had there, except `reload` (a game file or NULL); the
// others try the files (at boot, `old` is the list without covers).
// A borrowed texture (cover_shared) is freed with its owner, never through the borrower.
static void find_covers(struct game* games, int n, const struct game* old, int old_n, const char* reload) {
	for (int i = 0; i < n; ++i) {
		struct game* g = &games[i];
		int o = find_game(old, old_n, g->file);
		if (o >= 0 && old[o].has_cover && !(reload && strcmp(g->file, reload) == 0)) {
			if (!old[o].cover_shared) { // a borrowed one is borrowed again below
				g->cover = old[o].cover;
				g->has_cover = true;
			}
			continue;
		}
		const char* dot = strrchr(g->file, '.');
		char name[256];
		snprintf(name, sizeof(name), "%.*s", (int) (dot ? dot - g->file : 0), g->file);
		g->has_cover = load_cover(g, g->file) || (g->system != SYS_GBA && load_cover(g, name)) ||
		               (g->code[0] && load_cover(g, g->code));
	}
	for (int i = 0; i < n; ++i) {
		struct game* g = &games[i];
		for (int j = 0; j < n && !g->has_cover; ++j) {
			if (games[j].has_cover && !games[j].cover_shared && library_same_game(g, &games[j])) {
				g->cover = games[j].cover;
				g->has_cover = g->cover_shared = true;
			}
		}
		log_printf("library: %s [%s] %s\n", g->file, g->code, !g->has_cover ? "no cover" : g->cover_shared ? "cover shared" : "cover");
	}
}

static sys_mutex_t rescan_lock, place_lock;

static void cover_thread(void* arg) {
	library_rescan(NULL); // the same list, with its covers
	sysThreadExit(0);
}

void library_scan(void) {
	for (int i = 0; i < lib_num_games; ++i) {
		if (lib_games[i].has_cover && !lib_games[i].cover_shared) {
			gfx_tex_free(&lib_games[i].cover);
		}
	}
	mkdir(DATA_DIR, 0777);
	mkdir(ROM_DIR, 0777);
	mkdir(SAVE_DIR, 0777);
	mkdir(COVER_DIR, 0777);
	mkdir(PATCH_DIR, 0777);
	lib_num_games = library_read_games(lib_games);
	for (int i = 0; i < lib_num_games; ++i) {
		migrate_legacy_save(&lib_games[i]);
	}
	sys_mutex_attr_t attr;
	sysMutexAttrInitialize(attr);
	sysMutexCreate(&rescan_lock, &attr);
	sysMutexCreate(&place_lock, &attr);
	// Covers decode off the main thread and come in like any rescan: on the console they held
	// the first frame back ~1.8 s for 5 games, and the welcome screen doesn't show them.
	sys_ppu_thread_t thread;
	sysThreadCreate(&thread, cover_thread, NULL, 1300, 256 * 1024, 0, "covers"); // below the emulators
}

// The next game list, built off the main thread. Only its builder touches it (and reads
// lib_games) until rescan_ready is set, then only the main thread until it clears it; the
// cover slots change hands the same way. So the main loop never waits on a lock for it.
static struct game rescan_games[LIB_MAX_GAMES];
static int rescan_num;
static volatile bool rescan_ready;

void library_rescan(const char* new_cover) {
	sysMutexLock(rescan_lock, 0); // one builder at a time
	while (rescan_ready) {
		sysUsleep(10 * 1000); // the main loop takes the previous list at its next frame
	}
	__sync_synchronize();
	int n = library_read_games(rescan_games);
	find_covers(rescan_games, n, lib_games, lib_num_games, new_cover);
	rescan_num = n;
	__sync_synchronize();
	rescan_ready = true;
	sysMutexUnlock(rescan_lock);
}

bool library_rescan_ready(void) {
	return rescan_ready;
}

void library_take_rescan(void) {
	__sync_synchronize();
	// Free the covers the new list doesn't keep (replaced, or their game is gone). Only through
	// their owner: a borrower's texture is its owner's.
	for (int i = 0; i < lib_num_games; ++i) {
		bool kept = false;
		for (int j = 0; j < rescan_num && !kept; ++j) {
			kept = rescan_games[j].has_cover && rescan_games[j].cover.slot == lib_games[i].cover.slot;
		}
		if (lib_games[i].has_cover && !lib_games[i].cover_shared && !kept) {
			gfx_tex_free(&lib_games[i].cover);
		}
	}
	memcpy(lib_games, rescan_games, rescan_num * sizeof(*lib_games));
	lib_num_games = rescan_num;
	__sync_synchronize();
	rescan_ready = false;
}

// Case-insensitive, digit runs by value: "SAVE 2" before "SAVE 10". Ties fall back to strcmp, so
// every player sees one order.
static int name_compare(const char* a, const char* b) {
	const char *x = a, *y = b;
	while (*x && *y) {
		if (isdigit((unsigned char) *x) && isdigit((unsigned char) *y)) {
			char *ex, *ey;
			unsigned long nx = strtoul(x, &ex, 10), ny = strtoul(y, &ey, 10);
			if (nx != ny) {
				return nx < ny ? -1 : 1;
			}
			x = ex;
			y = ey;
		} else {
			int cx = toupper((unsigned char) *x++), cy = toupper((unsigned char) *y++);
			if (cx != cy) {
				return cx - cy;
			}
		}
	}
	return *x ? 1 : *y ? -1 : strcmp(a, b);
}

// Most recently played first; equal times (1 s resolution, files copied together) by name.
static int compare_saves(const void* a, const void* b) {
	const struct save_info *sa = a, *sb = b;
	if (sa->mtime != sb->mtime) {
		return sa->mtime < sb->mtime ? 1 : -1;
	}
	return name_compare(sa->name, sb->name);
}

int library_find(const char* file) {
	return find_game(lib_games, lib_num_games, file);
}

int library_catalog(int system, int* first) {
	int i = 0;
	while (i < lib_num_games && (int) lib_games[i].system < system) {
		++i;
	}
	*first = i;
	while (i < lib_num_games && (int) lib_games[i].system == system) {
		++i;
	}
	return i - *first;
}

int library_list_saves(const char* file, struct save_info* out, int max) {
	char path[512];
	snprintf(path, sizeof(path), "%s/%s", SAVE_DIR, file);
	DIR* dir = opendir(path);
	if (!dir) {
		return 0;
	}
	int n = 0;
	struct dirent* e;
	while ((e = readdir(dir)) && n < max) {
		if (!has_ext(e->d_name, ".sav")) {
			continue;
		}
		struct save_info* s = &out[n];
		snprintf(s->name, sizeof(s->name), "%.*s", (int) (strrchr(e->d_name, '.') - e->d_name), e->d_name);
		char file[600];
		bool fits = snprintf(file, sizeof(file), "%s/%s", path, e->d_name) < (int) sizeof(file);
		struct stat st;
		s->mtime = fits && stat(file, &st) == 0 ? st.st_mtime : 0;
		++n;
	}
	closedir(dir);
	qsort(out, n, sizeof(*out), compare_saves);
	return n;
}

void library_save_path(const char* file, const char* save, char* out, size_t len) {
	char dir[512];
	snprintf(dir, sizeof(dir), "%s/%s", SAVE_DIR, file);
	mkdir(dir, 0777);
	snprintf(out, len, "%s/%s.sav", dir, save);
}

// <dir>/<file>.gba, or <dir>/<file> for a GB/GBC name (it keeps its extension).
static bool rom_in(const char* dir, const char* file, char* out, size_t len) {
	struct stat st;
	snprintf(out, len, "%s/%s.gba", dir, file);
	if (stat(out, &st) == 0) {
		return true;
	}
	snprintf(out, len, "%s/%s", dir, file);
	return library_rom_system(file) > SYS_GBA && stat(out, &st) == 0;
}

void library_rom_path(const char* file, char* out, size_t len) {
	if (rom_in(ROM_DIR, file, out, len)) {
		return;
	}
	DIR* dir = opendir(ROM_DIR);
	struct dirent* e;
	while (dir && (e = readdir(dir))) {
		char sub[512];
		bool fits = snprintf(sub, sizeof(sub), "%s/%s", ROM_DIR, e->d_name) < (int) sizeof(sub);
		if (fits && e->d_name[0] != '.' && rom_in(sub, file, out, len)) {
			closedir(dir);
			return;
		}
	}
	if (dir) {
		closedir(dir);
	}
	snprintf(out, len, library_rom_system(file) > SYS_GBA ? "%s/%s" : "%s/%s.gba", ROM_DIR, file);
}

static bool exists(const char* path) {
	struct stat st;
	return stat(path, &st) == 0;
}

bool library_game_exists(const char* file) {
	char path[512];
	library_rom_path(file, path, sizeof(path));
	return exists(path);
}

void library_place_lock(void) {
	sysMutexLock(place_lock, 0);
}

void library_place_unlock(void) {
	sysMutexUnlock(place_lock);
}

bool library_place(const char* tmp, const char* path) {
	return !exists(path) && rename(tmp, path) == 0; // lv2 may rename over a file: check first
}

// A save file, or the ".tmp" left alone by an interrupted write (open_save() in emu.c recovers it).
static bool save_exists(const char* path) {
	char tmp[620];
	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	return exists(path) || exists(tmp);
}

// Copies to a temp file renamed once complete, so a crash never leaves a half-written save.
// ".part", not ".tmp": open_save() would recover a partial ".tmp" as the save.
static bool copy_file(const char* from, const char* to) {
	char part[620];
	snprintf(part, sizeof(part), "%s.part", to);
	FILE* in = fopen(from, "rb");
	if (!in) {
		return false;
	}
	FILE* out = fopen(part, "wb");
	bool ok = out != NULL;
	static char buf[16 * 1024]; // only the library thread copies
	size_t n;
	while (ok && (n = fread(buf, 1, sizeof(buf), in)) > 0) {
		ok = fwrite(buf, 1, n, out) == n;
	}
	ok = ok && !ferror(in);
	fclose(in);
	if (out) {
		ok = fclose(out) == 0 && ok;
	}
	ok = ok && !save_exists(to) && rename(part, to) == 0;
	if (!ok) {
		remove(part);
	}
	return ok;
}

// A ".sav.tmp" next to an existing save is a stale, interrupted write (see write_file_atomic in
// emu.c): drop it with its save, or a new save of that name would load it.
static bool save_op(enum lib_save_op op, const char* file, const char* save, const char* to) {
	if (op == LIB_LIST) {
		return true;
	}
	char path[600], to_path[600], tmp[620];
	library_save_path(file, save, path, sizeof(path));
	library_save_path(file, to, to_path, sizeof(to_path));
	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	switch (op) {
	case LIB_RENAME:
		// lv2 may rename over an existing file: check first, never overwrite.
		if (!exists(path) || save_exists(to_path) || rename(path, to_path) != 0) {
			return false;
		}
		remove(tmp);
		return true;
	case LIB_COPY:
		return copy_file(path, to_path);
	case LIB_DELETE:
		if (remove(path) != 0) {
			return false;
		}
		remove(tmp);
		return true;
	case LIB_LIST:
		break;
	}
	return true;
}

static sys_mutex_t list_lock;
static sys_cond_t list_wake;
static struct {
	char file[256];
	enum lib_save_op op;
	char save[64], to[64];
	unsigned want, done; // request numbers: the result is in once done == want
	bool ok;
	int n;
	struct save_info saves[LIB_MAX_SAVES];
} lists[LIB_LIST_SLOTS];

static void list_thread(void* arg) {
	(void) arg;
	sysMutexLock(list_lock, 0);
	for (;;) {
		int slot = -1;
		for (int i = 0; i < LIB_LIST_SLOTS && slot < 0; ++i) {
			if (lists[i].done != lists[i].want) {
				slot = i;
			}
		}
		if (slot < 0) {
			sysCondWait(list_wake, 0);
			continue;
		}
		char file[256], save[64], to[64];
		snprintf(file, sizeof(file), "%s", lists[slot].file);
		snprintf(save, sizeof(save), "%s", lists[slot].save);
		snprintf(to, sizeof(to), "%s", lists[slot].to);
		enum lib_save_op op = lists[slot].op;
		unsigned want = lists[slot].want;
		sysMutexUnlock(list_lock);
		bool ok = save_op(op, file, save, to);
		struct save_info saves[LIB_MAX_SAVES];
		int n = library_list_saves(file, saves, LIB_MAX_SAVES);
		sysMutexLock(list_lock, 0);
		if (lists[slot].want == want) { // else a newer request replaced it: read again
			memcpy(lists[slot].saves, saves, n * sizeof(*saves));
			lists[slot].n = n;
			lists[slot].ok = ok;
			lists[slot].done = want;
		}
	}
}

void library_save_op_async(int slot, const char* file, enum lib_save_op op, const char* save, const char* to) {
	static bool started;
	if (!started) {
		sys_mutex_attr_t mutex_attr;
		sysMutexAttrInitialize(mutex_attr);
		sysMutexCreate(&list_lock, &mutex_attr);
		sys_cond_attr_t cond_attr;
		sysCondAttrInitialize(cond_attr);
		sysCondCreate(&list_wake, list_lock, &cond_attr);
		sys_ppu_thread_t thread;
		// Below the main loop (1001), above the emulators (1200): it mostly waits for the drive.
		sysThreadCreate(&thread, list_thread, NULL, 1100, 64 * 1024, 0, "save lists");
		started = true;
	}
	sysMutexLock(list_lock, 0);
	snprintf(lists[slot].file, sizeof(lists[slot].file), "%s", file);
	lists[slot].op = op;
	snprintf(lists[slot].save, sizeof(lists[slot].save), "%s", save);
	snprintf(lists[slot].to, sizeof(lists[slot].to), "%s", to);
	++lists[slot].want;
	sysCondBroadcast(list_wake);
	sysMutexUnlock(list_lock);
}

void library_list_saves_async(int slot, const char* file) {
	library_save_op_async(slot, file, LIB_LIST, "", "");
}

int library_saves_result(int slot, struct save_info* out, bool* op_ok) {
	sysMutexLock(list_lock, 0);
	int n = lists[slot].done == lists[slot].want ? lists[slot].n : -1;
	if (n > 0) {
		memcpy(out, lists[slot].saves, n * sizeof(*out));
	}
	if (op_ok) {
		*op_ok = lists[slot].ok;
	}
	sysMutexUnlock(list_lock);
	return n;
}
