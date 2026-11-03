// Phone web server: each player scans their QR code on the TV and gets a page served from here
// to add games, covers and saves, download saves and set their name. LAN only: every URL
// carries the player's per-boot token, http://<ip>:WEB_PORT/<token>/...
// The server threads run below the emulators and only meet the main loop through `share`
// (which the main loop never waits for) and the library's off-thread rescan.
#include <ctype.h>
#include <dirent.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <net/net.h>
#include <net/netctl.h>
#include <netinet/in.h>
#include <ppu-lv2.h>
#include <sys/systime.h>
#include <sys/thread.h>
#include <sysmodule/sysmodule.h>
#include <mgba/gb/interface.h>
#include <mgba/internal/gba/gba.h>
#include <mgba-util/vfs.h>

#include "emu.h"
#include "library.h"
#include "phone_html.h"
#include "pressstart2p_ttf.h"
#include "rompatch.h"
#include "web.h"

#define WEB_PRIORITY 1300 // below the emulators (1200): a transfer must never cost a frame
#define WEB_CONNS 4       // connections served at once; more wait in the listen backlog
#define WEB_TIMEOUT 10    // seconds a phone may stall before it is dropped
#define BUF_SIZE (64 * 1024)
#define HEAD_MAX 4096
#define ROM_MAX (32 << 20)
#define SAVE_MAX (256 << 10) // 128 KB flash plus what other emulators append
#define COVER_MAX (2 << 20)  // the page sends covers already shrunk to 256 px
#define ROM_NAME_MAX 128
#define SAVE_NAME_MAX 16     // what fits a save list row
#define GB_ROM_DIR ROM_DIR "/gb" // GB/GBC uploads, apart from the GBA games

char web_urls[MAX_PLAYERS][64];
char web_names[MAX_PLAYERS][PLAYER_NAME_MAX];

struct conn {
	int fd;
	int player;
	char head[HEAD_MAX + 1];
	char* body;    // start of the body bytes already received with the head
	long body_len;
	long length;   // Content-Length, -1 if none
	bool expect;   // Expect: 100-continue (curl): nothing sent until we answer
	size_t out;    // bytes waiting in buf
	u8 buf[BUF_SIZE];
};

static volatile int active_conns;
static unsigned tmp_counter;

// ---------------------------------------------------------------------------------------------
// HTTP

static bool send_all(struct conn* c, const void* data, size_t n) {
	const char* p = data;
	while (n > 0) {
		ssize_t k = send(c->fd, p, n, 0);
		if (k <= 0) {
			return false;
		}
		p += k;
		n -= k;
	}
	return true;
}

static const char* status_text(int status) {
	switch (status) {
	case 200: return "OK";
	case 301: return "Moved Permanently";
	case 400: return "Bad Request";
	case 404: return "Not Found";
	case 409: return "Conflict";
	case 411: return "Length Required";
	case 413: return "Payload Too Large";
	case 415: return "Unsupported Media Type";
	default: return "Internal Server Error";
	}
}

// length -1: the body runs until the connection closes.
static void send_head(struct conn* c, int status, const char* type, long length, const char* extra) {
	char h[1024];
	int n = snprintf(h, sizeof(h), "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nConnection: close\r\n%s",
	                 status, status_text(status), type, extra ? extra : "Cache-Control: no-store\r\n");
	if (length >= 0) {
		n += snprintf(h + n, sizeof(h) - n, "Content-Length: %ld\r\n", length);
	}
	n += snprintf(h + n, sizeof(h) - n, "\r\n");
	send_all(c, h, n < (int) sizeof(h) ? n : (int) sizeof(h) - 1);
}

// Plain text answer, shown as is by the page.
static void reply(struct conn* c, int status, const char* format, ...) {
	char msg[512];
	va_list args;
	va_start(args, format);
	int n = vsnprintf(msg, sizeof(msg), format, args);
	va_end(args);
	if (n >= (int) sizeof(msg)) n = sizeof(msg) - 1;
	if (status >= 400) { // the page only shows it on the phone
		log_printf("web: P%d %d %.*s\n", c->player + 1, status, n, msg);
	}
	send_head(c, status, "text/plain; charset=utf-8", n, NULL);
	send_all(c, msg, n);
}

static void flush(struct conn* c) {
	send_all(c, c->buf, c->out);
	c->out = 0;
}

static void out_char(struct conn* c, char ch) {
	if (c->out == BUF_SIZE) {
		flush(c);
	}
	c->buf[c->out++] = ch;
}

static void out(struct conn* c, const char* format, ...) {
	char s[256];
	va_list args;
	va_start(args, format);
	vsnprintf(s, sizeof(s), format, args);
	va_end(args);
	for (const char* p = s; *p; ++p) {
		out_char(c, *p);
	}
}

static void out_json(struct conn* c, const char* s) {
	out_char(c, '"');
	for (; *s; ++s) {
		u8 ch = *s;
		if (ch == '"' || ch == '\\') {
			out_char(c, '\\');
			out_char(c, ch);
		} else if (ch < 0x20) {
			out(c, "\\u%04x", ch);
		} else {
			out_char(c, ch);
		}
	}
	out_char(c, '"');
}

// Reads the request line and headers; the start of the body may come with them.
static bool read_head(struct conn* c, char* method, char* target) {
	s64 deadline = sysGetSystemTime() + WEB_TIMEOUT * 1000000ll; // also against a trickle
	int len = 0;
	char* end;
	c->head[0] = '\0';
	while (!(end = strstr(c->head, "\r\n\r\n"))) {
		if (len >= HEAD_MAX || sysGetSystemTime() > deadline) {
			return false;
		}
		ssize_t n = recv(c->fd, c->head + len, HEAD_MAX - len, 0);
		if (n <= 0) {
			return false;
		}
		len += n;
		c->head[len] = '\0';
	}
	c->body = end + 4;
	c->body_len = len - (c->body - c->head);
	c->length = -1;
	if (sscanf(c->head, "%7s %1023s", method, target) != 2) {
		return false;
	}
	for (char* line = strstr(c->head, "\r\n") + 2; line < end; line = strstr(line, "\r\n") + 2) {
		if (strncasecmp(line, "Content-Length:", 15) == 0) {
			char* num_end;
			c->length = strtol(line + 15, &num_end, 10);
			if (num_end == line + 15 || c->length < 0) {
				return false;
			}
		} else if (strncasecmp(line, "Expect:", 7) == 0) {
			c->expect = true;
		} else if (strncasecmp(line, "Transfer-Encoding:", 18) == 0) {
			return false; // chunked uploads: browsers and curl send a length for files
		}
	}
	if (c->body_len > c->length) {
		c->body_len = c->length < 0 ? 0 : c->length;
	}
	return true;
}

// The request is accepted: let a client waiting on Expect: 100-continue send its body.
static void go_ahead(struct conn* c) {
	if (c->expect) {
		send_all(c, "HTTP/1.1 100 Continue\r\n\r\n", 25);
	}
}

// Before refusing an upload, read its body so the browser gets our answer and not a reset.
// Clients that asked first (Expect) never sent it.
static void drain(struct conn* c) {
	long left = c->length - c->body_len;
	if (c->expect || left > PATCH_FILE_MAX) { // more than any upload we take
		return;
	}
	while (left > 0) {
		ssize_t n = recv(c->fd, c->buf, left < BUF_SIZE ? left : BUF_SIZE, 0);
		if (n <= 0) {
			return;
		}
		left -= n;
	}
}

// Streams the body into a new temp file. On failure, nothing is left behind.
static bool receive(struct conn* c, char* tmp, size_t tmp_len) {
	snprintf(tmp, tmp_len, "%s/%u.part", TMP_DIR, __sync_add_and_fetch(&tmp_counter, 1));
	FILE* f = fopen(tmp, "wb");
	if (!f) {
		return false;
	}
	setvbuf(f, NULL, _IONBF, 0);
	go_ahead(c);
	bool ok = fwrite(c->body, 1, c->body_len, f) == (size_t) c->body_len;
	for (long left = c->length - c->body_len; ok && left > 0;) {
		ssize_t n = recv(c->fd, c->buf, left < BUF_SIZE ? left : BUF_SIZE, 0);
		ok = n > 0 && fwrite(c->buf, 1, n, f) == (size_t) n;
		left -= n;
	}
	ok = fclose(f) == 0 && ok;
	if (!ok) {
		remove(tmp);
	}
	return ok;
}

static void send_file(struct conn* c, const char* path, const char* type, const char* download) {
	FILE* f = fopen(path, "rb");
	struct stat st;
	if (!f || fstat(fileno(f), &st) != 0) {
		if (f) fclose(f);
		reply(c, 404, "NOT FOUND");
		return;
	}
	setvbuf(f, NULL, _IONBF, 0);
	char extra[512] = "Cache-Control: no-cache\r\n";
	if (download) {
		// RFC 6266 filename*: percent-encode everything but plain characters.
		int n = snprintf(extra, sizeof(extra), "Cache-Control: no-store\r\nContent-Disposition: attachment; filename*=UTF-8''");
		for (const u8* s = (const u8*) download; *s && n < (int) sizeof(extra) - 8; ++s) {
			n += isalnum(*s) || strchr(".-_", *s) ? snprintf(extra + n, 2, "%c", *s) : snprintf(extra + n, 4, "%%%02X", *s);
		}
		snprintf(extra + n, sizeof(extra) - n, "\r\n");
	}
	send_head(c, 200, type, st.st_size, extra);
	size_t n;
	while ((n = fread(c->buf, 1, BUF_SIZE, f)) > 0 && send_all(c, c->buf, n)) {
	}
	fclose(f);
}

// Copies query parameter `key`, URL-decoded. False if missing, too long or malformed.
static bool param(const char* query, const char* key, char* out, size_t len) {
	size_t k = strlen(key);
	for (const char* p = query; p; p = strchr(p, '&') ? strchr(p, '&') + 1 : NULL) {
		if (strncmp(p, key, k) != 0 || p[k] != '=') {
			continue;
		}
		size_t n = 0;
		for (p += k + 1; *p && *p != '&'; ++p) {
			unsigned ch = (u8) *p;
			if (ch == '+') {
				ch = ' ';
			} else if (ch == '%') {
				if (!isxdigit((u8) p[1]) || !isxdigit((u8) p[2])) {
					return false;
				}
				sscanf(p + 1, "%2x", &ch);
				p += 2;
			}
			if (ch == 0 || n + 1 >= len) {
				return false;
			}
			out[n++] = ch;
		}
		out[n] = '\0';
		return true;
	}
	return false;
}

// ---------------------------------------------------------------------------------------------
// Files

// Every name from the network becomes a single path component: printable ASCII (plus UTF-8
// for files that are already there), none of the characters file systems or URLs choke on,
// no leading dot (so no "..") and no spaces or dots at the ends.
static bool valid_name(const char* s, size_t max, bool utf8) {
	size_t n = strlen(s);
	if (n == 0 || n > max || s[0] == '.' || s[0] == ' ' || s[n - 1] == '.' || s[n - 1] == ' ') {
		return false;
	}
	for (; *s; ++s) {
		u8 ch = *s;
		if (ch < 0x20 || ch == 0x7f || (ch > 0x7f && !utf8) || strchr("/\\:*?\"<>|", ch)) {
			return false;
		}
	}
	return true;
}

static bool exists(const char* path) {
	struct stat st;
	return stat(path, &st) == 0;
}

// The `game` parameter: an existing game's file name.
static bool game_param(const char* query, char* game, size_t len) {
	char path[512];
	if (!param(query, "game", game, len) || !valid_name(game, 200, true)) {
		return false;
	}
	library_rom_path(game, path, sizeof(path));
	return exists(path);
}

static bool read_start(const char* path, u8* head, size_t len) {
	FILE* f = fopen(path, "rb");
	if (!f) {
		return false;
	}
	bool ok = fread(head, 1, len, f) == len;
	fclose(f);
	return ok;
}

// Its first bytes are an IPS, UPS or BPS patch's.
static bool is_patch(const char* path) {
	u8 head[5];
	return read_start(path, head, sizeof(head)) && rompatch_format(head);
}

// mGBA's own check, on the header (GB/GBC: the logo at 0x104).
static bool is_rom(const char* path, enum game_system system) {
	u8 head[0x200];
	size_t len = system == SYS_GBA ? 0xc0 : sizeof(head);
	if (!read_start(path, head, len)) {
		return false;
	}
	struct VFile* vf = VFileFromConstMemory(head, len);
	bool ok = system == SYS_GBA ? GBAIsROM(vf) : GBIsROM(vf);
	vf->close(vf);
	return ok;
}

// A PNG small enough to decode without a memory spike.
static bool is_cover(const char* path) {
	u8 head[24];
	if (!read_start(path, head, sizeof(head)) || memcmp(head, "\x89PNG\r\n\x1a\n\0\0\0\x0dIHDR", 16) != 0) {
		return false;
	}
	u32 w = head[16] << 24 | head[17] << 16 | head[18] << 8 | head[19];
	u32 h = head[20] << 24 | head[21] << 16 | head[22] << 8 | head[23];
	return w > 0 && h > 0 && w <= 1024 && h <= 1024;
}

static void delete_tmp_files(void) {
	DIR* dir = opendir(TMP_DIR);
	if (!dir) {
		return;
	}
	struct dirent* e;
	char path[512];
	while ((e = readdir(dir))) {
		if (e->d_name[0] != '.') {
			snprintf(path, sizeof(path), "%s/%.255s", TMP_DIR, e->d_name);
			remove(path);
		}
	}
	closedir(dir);
}

// ---------------------------------------------------------------------------------------------
// Routes

static void get_games(struct conn* c) {
	char name[PLAYER_NAME_MAX];
	share_lock();
	memcpy(name, share.names[c->player], sizeof(name));
	share_unlock();
	send_head(c, 200, "application/json", -1, NULL);
	out(c, "{\"player\":%d,\"name\":", c->player + 1);
	out_json(c, name);
	out(c, ",\"games\":[");
	struct game* games = malloc(LIB_MAX_GAMES * sizeof(*games));
	int n = games ? library_read_games(games) : 0;
	for (int i = 0; i < n; ++i) {
		if (i) out_char(c, ',');
		out_json(c, games[i].file);
	}
	free(games);
	out(c, "]}");
	flush(c);
}

static void get_saves(struct conn* c, const char* query) {
	char game[256];
	if (!game_param(query, game, sizeof(game))) {
		reply(c, 404, "GAME NOT FOUND");
		return;
	}
	struct save_info saves[LIB_MAX_SAVES];
	int n = library_list_saves(game, saves, LIB_MAX_SAVES);
	typeof(share.in_use) in_use;
	share_lock();
	memcpy(in_use, share.in_use, sizeof(in_use));
	share_unlock();
	send_head(c, 200, "application/json", -1, NULL);
	out(c, "{\"now\":%ld,\"saves\":[", (long) time(NULL));
	for (int i = 0; i < n; ++i) {
		int owner = 0;
		for (int j = 0; j < MAX_PLAYERS; ++j) {
			if (strcmp(in_use[j].game, game) == 0 && strcmp(in_use[j].save, saves[i].name) == 0) {
				owner = j + 1;
			}
		}
		out(c, "%s{\"name\":", i ? "," : "");
		out_json(c, saves[i].name);
		out(c, ",\"time\":%ld,\"player\":%d}", (long) saves[i].mtime, owner);
	}
	out(c, "]}");
	flush(c);
}

static void get_save(struct conn* c, const char* query) {
	char game[256], name[64], path[600], download[400];
	if (!game_param(query, game, sizeof(game)) || !param(query, "name", name, sizeof(name)) || !valid_name(name, 63, true)) {
		reply(c, 404, "SAVE NOT FOUND");
		return;
	}
	snprintf(path, sizeof(path), "%s/%s/%s.sav", SAVE_DIR, game, name);
	snprintf(download, sizeof(download), "%s - %s.sav", game, name);
	send_file(c, path, "application/octet-stream", download);
}

static void get_cover(struct conn* c, const char* query) {
	char game[256], path[512];
	if (!game_param(query, game, sizeof(game))) {
		reply(c, 404, "GAME NOT FOUND");
		return;
	}
	snprintf(path, sizeof(path), "%s/%s.png", COVER_DIR, game);
	send_file(c, path, "image/png", NULL);
}

static void post_rom(struct conn* c, const char* query) {
	char name[256], path[512], tmp[128];
	int system = param(query, "name", name, sizeof(name)) ? library_rom_system(name) : -1;
	if (system < 0) {
		drain(c);
		reply(c, 400, "ONLY .GBA .GB .GBC FILES");
		return;
	}
	char* dot = strrchr(name, '.');
	for (char* s = dot; *s; ++s) {
		*s = tolower((unsigned char) *s);
	}
	*dot = '\0'; // checked without its extension
	bool ok = valid_name(name, ROM_NAME_MAX, false);
	*dot = '.';
	if (!ok) {
		drain(c);
		reply(c, 400, "BAD FILE NAME");
		return;
	}
	if (c->length < (system == SYS_GBA ? 0xc0 : 0x200) || c->length > ROM_MAX) {
		drain(c);
		reply(c, 413, "NOT A GAME (32 MB AT MOST)");
		return;
	}
	if (system == SYS_GBA) {
		*dot = '\0'; // GBA games are named without it (struct game)
	}
	library_rom_path(name, path, sizeof(path)); // finds it in any folder
	if (exists(path)) {
		drain(c);
		reply(c, 409, "ALREADY ON THE PS3");
		return;
	}
	if (system != SYS_GBA) {
		mkdir(GB_ROM_DIR, 0777);
		snprintf(path, sizeof(path), "%s/%s", GB_ROM_DIR, name);
	}
	if (!receive(c, tmp, sizeof(tmp))) {
		reply(c, 500, "UPLOAD FAILED");
		return;
	}
	if (!is_rom(tmp, system)) {
		remove(tmp);
		reply(c, 415, system == SYS_GBA ? "NOT A GBA GAME" : "NOT A GB OR GBC GAME");
		return;
	}
	library_place_lock();
	ok = !library_game_exists(name) && library_place(tmp, path); // in any folder (a ROM patch build meanwhile)
	library_place_unlock();
	if (!ok) {
		remove(tmp);
		reply(c, 409, "ALREADY ON THE PS3");
		return;
	}
	log_printf("web: P%d added %s\n", c->player + 1, name);
	library_rescan(NULL);
	reply(c, 200, "ADDED %s", name);
}

// Patches go to PATCH_DIR under their own name; the player then builds the game on the TV.
static void post_patch(struct conn* c, const char* query) {
	char name[256], path[512], tmp[128];
	if (!param(query, "name", name, sizeof(name)) || !rompatch_is_patch_file(name)) {
		drain(c);
		reply(c, 400, "ONLY .IPS .UPS .BPS FILES");
		return;
	}
	char* dot = strrchr(name, '.');
	for (char* s = dot; *s; ++s) {
		*s = tolower((unsigned char) *s);
	}
	*dot = '\0'; // checked without its extension
	bool ok = valid_name(name, ROM_NAME_MAX, false);
	*dot = '.';
	if (!ok) {
		drain(c);
		reply(c, 400, "BAD FILE NAME");
		return;
	}
	if (c->length < 8 || c->length > PATCH_FILE_MAX) {
		drain(c);
		reply(c, 413, "NOT A PATCH (64 MB AT MOST)");
		return;
	}
	snprintf(path, sizeof(path), "%s/%s", PATCH_DIR, name);
	if (exists(path)) {
		drain(c);
		reply(c, 409, "ALREADY ON THE PS3");
		return;
	}
	if (!receive(c, tmp, sizeof(tmp))) {
		reply(c, 500, "UPLOAD FAILED");
		return;
	}
	if (!is_patch(tmp)) {
		remove(tmp);
		reply(c, 415, "NOT AN IPS, UPS OR BPS PATCH");
		return;
	}
	library_place_lock();
	ok = library_place(tmp, path);
	library_place_unlock();
	if (!ok) {
		remove(tmp);
		reply(c, 409, "ALREADY ON THE PS3");
		return;
	}
	log_printf("web: P%d added patch %s\n", c->player + 1, name);
	reply(c, 200, "ADDED %s: PRESS SQUARE ON ITS GAME ON THE TV", name);
}

static void post_cover(struct conn* c, const char* query) {
	char game[256], path[512], tmp[128];
	if (!game_param(query, game, sizeof(game))) {
		drain(c);
		reply(c, 404, "GAME NOT FOUND");
		return;
	}
	if (c->length < 24 || c->length > COVER_MAX) {
		drain(c);
		reply(c, 413, "IMAGE TOO BIG");
		return;
	}
	if (!receive(c, tmp, sizeof(tmp))) {
		reply(c, 500, "UPLOAD FAILED");
		return;
	}
	if (!is_cover(tmp)) {
		remove(tmp);
		reply(c, 415, "NOT A PNG UP TO 1024 PX");
		return;
	}
	snprintf(path, sizeof(path), "%s/%s.png", COVER_DIR, game);
	library_place_lock();
	remove(path); // lv2 may refuse to rename over a file
	bool ok = rename(tmp, path) == 0;
	library_place_unlock();
	if (!ok) {
		remove(tmp);
		reply(c, 500, "COULD NOT SAVE THE COVER");
		return;
	}
	library_rescan(game);
	reply(c, 200, "COVER SET");
}

static void post_save(struct conn* c, const char* query) {
	char game[256], name[64], want[64], path[600], tmp[128];
	if (!game_param(query, game, sizeof(game))) {
		drain(c);
		reply(c, 404, "GAME NOT FOUND");
		return;
	}
	if (!param(query, "name", want, sizeof(want)) || !valid_name(want, SAVE_NAME_MAX, false)) {
		drain(c);
		reply(c, 400, "BAD SAVE NAME (16 LETTERS AT MOST)");
		return;
	}
	if (c->length < 1 || c->length > SAVE_MAX) {
		drain(c);
		reply(c, 413, "NOT A SAVE (256 KB AT MOST)");
		return;
	}
	if (!receive(c, tmp, sizeof(tmp))) {
		reply(c, 500, "UPLOAD FAILED");
		return;
	}
	// Always a new save: the first free name among "NAME", "NAME 2"... A name a player has
	// open counts as taken even before it reaches the drive (a new save only does at its first
	// write). Shortcut: a player starting a new save in the same instant could still pick the
	// same name (main.c's new_save_name reads the drive before); names differ in practice.
	library_place_lock();
	// Its game may have been deleted meanwhile (ROM patching's cleanup checks for saves under
	// this lock): no save for a game that is gone.
	bool gone = !library_game_exists(game), ok = false;
	for (int i = 1; i < 100 && !ok && !gone; ++i) {
		snprintf(name, sizeof(name), i == 1 ? "%s" : "%s %d", want, i);
		share_lock();
		bool open = false;
		for (int j = 0; j < MAX_PLAYERS; ++j) {
			open |= strcmp(share.in_use[j].game, game) == 0 && strcmp(share.in_use[j].save, name) == 0;
		}
		share_unlock();
		library_save_path(game, name, path, sizeof(path));
		ok = !open && library_place(tmp, path);
	}
	library_place_unlock();
	if (gone) {
		remove(tmp);
		reply(c, 404, "GAME NOT FOUND");
		return;
	}
	if (!ok) {
		remove(tmp);
		reply(c, 409, "TOO MANY SAVES NAMED %s", want);
		return;
	}
	share_lock();
	share.saves_changed = true;
	share_unlock();
	log_printf("web: P%d added save %s/%s\n", c->player + 1, game, name);
	reply(c, 200, "SAVED AS %s", name);
}

static void post_name(struct conn* c, const char* query) {
	char name[64];
	if (!param(query, "name", name, sizeof(name)) || strlen(name) >= PLAYER_NAME_MAX) {
		reply(c, 400, "12 LETTERS AT MOST");
		return;
	}
	for (const char* s = name; *s; ++s) {
		if ((u8) *s < 0x20 || (u8) *s > 0x7e) {
			reply(c, 400, "LETTERS, DIGITS AND SYMBOLS ONLY");
			return;
		}
	}
	share_lock();
	snprintf(share.names[c->player], PLAYER_NAME_MAX, "%s", name);
	share_save_names();
	share_unlock();
	reply(c, 200, name[0] ? "HELLO %s" : "NAME CLEARED", name);
}

static void handle(struct conn* c) {
	char method[8], target[1024];
	if (!read_head(c, method, target)) {
		return; // timed out, closed or not HTTP: not worth an answer
	}
	// "/<token>/<route>?<query>"
	c->player = -1;
	for (int i = 0; i < MAX_PLAYERS; ++i) {
		size_t n = strlen(share.tokens[i]); // set once at boot
		if (target[0] == '/' && strncmp(target + 1, share.tokens[i], n) == 0 && (target[n + 1] == '/' || target[n + 1] == '\0')) {
			c->player = i;
		}
	}
	if (c->player < 0) {
		drain(c);
		reply(c, 404, "NOT FOUND");
		return;
	}
	char* route = target + 1 + strlen(share.tokens[c->player]);
	if (*route == '\0') {
		char location[64];
		snprintf(location, sizeof(location), "Location: /%.8s/\r\n", share.tokens[c->player]);
		send_head(c, 301, "text/plain", 0, location);
		return;
	}
	++route;
	char* query = strchr(route, '?');
	if (query) {
		*query++ = '\0';
	}
	bool get = strcmp(method, "GET") == 0, post = strcmp(method, "POST") == 0;
	if (post && c->length < 0) {
		reply(c, 411, "LENGTH REQUIRED");
	} else if (get && strcmp(route, "") == 0) {
		send_head(c, 200, "text/html; charset=utf-8", phone_html_size, NULL);
		send_all(c, phone_html, phone_html_size);
	} else if (get && strcmp(route, "font.ttf") == 0) {
		send_head(c, 200, "font/ttf", pressstart2p_ttf_size, "Cache-Control: max-age=86400\r\n");
		send_all(c, pressstart2p_ttf, pressstart2p_ttf_size);
	} else if (get && strcmp(route, "games") == 0) {
		get_games(c);
	} else if (get && strcmp(route, "saves") == 0) {
		get_saves(c, query);
	} else if (get && strcmp(route, "save") == 0) {
		get_save(c, query);
	} else if (get && strcmp(route, "cover") == 0) {
		get_cover(c, query);
	} else if (post && strcmp(route, "rom") == 0) {
		post_rom(c, query);
	} else if (post && strcmp(route, "patch") == 0) {
		post_patch(c, query);
	} else if (post && strcmp(route, "cover") == 0) {
		post_cover(c, query);
	} else if (post && strcmp(route, "save") == 0) {
		post_save(c, query);
	} else if (post && strcmp(route, "name") == 0) {
		post_name(c, query);
	} else {
		drain(c);
		reply(c, 404, "NOT FOUND");
	}
}

// ---------------------------------------------------------------------------------------------
// Threads

static void conn_thread(void* arg) {
	struct conn* c = arg;
	struct timeval timeout = { WEB_TIMEOUT, 0 };
	setsockopt(c->fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
	setsockopt(c->fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
	handle(c);
	// Lingering close: closing with unread body bytes (a refused upload too big to drain) resets
	// the connection, and the client may lose our answer. Discard what still comes for up to ~2 s.
	shutdown(c->fd, SHUT_WR);
	struct timeval linger = { 0, 250 * 1000 };
	setsockopt(c->fd, SOL_SOCKET, SO_RCVTIMEO, &linger, sizeof(linger));
	for (s64 until = sysGetSystemTime() + 2000000; sysGetSystemTime() < until && recv(c->fd, c->buf, BUF_SIZE, 0) > 0;) {
	}
	close(c->fd);
	free(c);
	__sync_sub_and_fetch(&active_conns, 1);
	sysThreadExit(0);
}

// The link shown on the TV follows the console's IP (none while the network is down).
static void refresh_url(void) {
	char url[sizeof(share.url)] = "";
	s32 state;
	union net_ctl_info info;
	if (netCtlGetState(&state) == 0 && state == NET_CTL_STATE_IPObtained &&
	    netCtlGetInfo(NET_CTL_INFO_IP_ADDRESS, &info) == 0) {
		snprintf(url, sizeof(url), "http://%s:%d", info.ip_address, WEB_PORT);
	}
	share_lock();
	if (strcmp(share.url, url) != 0) {
		log_printf("web: link %s\n", url[0] ? url : "down");
#ifdef DEV_TOOLS
		for (int i = 0; i < MAX_PLAYERS && url[0]; ++i) {
			log_printf("web: P%d %s/%s/\n", i + 1, url, share.tokens[i]); // for test scripts
		}
#endif
		memcpy(share.url, url, sizeof(url));
	}
	share_unlock();
}

static int open_listener(void) {
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) {
		return -1;
	}
	int one = 1;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	struct sockaddr_in addr = { .sin_len = sizeof(addr), .sin_family = AF_INET, .sin_port = htons(WEB_PORT) };
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	if (bind(fd, (struct sockaddr*) &addr, sizeof(addr)) != 0 || listen(fd, 8) != 0) {
		log_printf("web: cannot listen on port %d\n", WEB_PORT);
		close(fd);
		return -1;
	}
	return fd;
}

static void accept_thread(void* arg) {
	(void) arg;
	int listener;
	while ((listener = open_listener()) < 0) {
		sysUsleep(2 * 1000 * 1000);
	}
	for (;;) {
		while (active_conns >= WEB_CONNS) {
			sysUsleep(20 * 1000); // later connections wait in the backlog
		}
		int fd = accept(listener, NULL, NULL);
		if (fd < 0) {
			sysUsleep(100 * 1000);
			continue;
		}
		struct conn* c = malloc(sizeof(*c));
		sys_ppu_thread_t thread;
		if (c) {
			memset(c, 0, offsetof(struct conn, buf));
			c->fd = fd;
			__sync_add_and_fetch(&active_conns, 1);
			if (sysThreadCreate(&thread, conn_thread, c, WEB_PRIORITY, 64 * 1024, 0, "web conn") == 0) {
				continue;
			}
			__sync_sub_and_fetch(&active_conns, 1);
			free(c);
		}
		close(fd);
	}
}

static void server_thread(void* arg) {
	(void) arg;
	sysModuleLoad(SYSMODULE_NETCTL);
	netInitialize();
	netCtlInit();
	mkdir(TMP_DIR, 0777);
	delete_tmp_files(); // uploads cut short by a crash or power loss
	sys_ppu_thread_t thread;
	sysThreadCreate(&thread, accept_thread, NULL, WEB_PRIORITY, 64 * 1024, 0, "web accept");
	for (;;) {
		refresh_url();
		sysUsleep(2 * 1000 * 1000);
	}
}

void web_init(void) {
	share_init();
	sys_ppu_thread_t thread;
	sysThreadCreate(&thread, server_thread, NULL, WEB_PRIORITY, 64 * 1024, 0, "web server");
}

// ---------------------------------------------------------------------------------------------
// Main thread side

// Save lists read again after an upload: the old list stays on screen until the new one is in.
static bool refreshing[MAX_PLAYERS];
static char keep_save[MAX_PLAYERS][64]; // the selected save, "" for + NEW SAVE

static void refresh_saves(struct player* p, bool changed) {
	int i = p->index;
	if (changed && p->state == PS_SAVES && p->num_saves >= 0) {
		snprintf(keep_save[i], sizeof(keep_save[i]), "%s", p->save_sel < p->num_saves ? p->saves[p->save_sel].name : "");
		library_list_saves_async(i, p->game_file);
		refreshing[i] = true;
	}
	if (refreshing[i] && (p->state != PS_SAVES || p->num_saves < 0)) {
		refreshing[i] = false; // left the list, or reopened it (reading its own request)
	}
	struct save_info saves[LIB_MAX_SAVES];
	int n;
	if (refreshing[i] && (n = library_saves_result(i, saves, NULL)) >= 0) {
		memcpy(p->saves, saves, n * sizeof(*saves));
		p->num_saves = n;
		p->save_sel = n; // + NEW SAVE, unless the selected save is still there
		for (int j = 0; j < n; ++j) {
			if (strcmp(saves[j].name, keep_save[i]) == 0) {
				p->save_sel = j;
			}
		}
		refreshing[i] = false;
	}
}

// A rescanned game list: everyone keeps the game they were on, or if it is gone the one at its
// place in the same catalog (system).
static void take_rescan(void) {
	char selected[MAX_PLAYERS][256];
	int system[MAX_PLAYERS], pos[MAX_PLAYERS];
	for (int i = 0; i < MAX_PLAYERS; ++i) {
		int first = 0;
		system[i] = lib_num_games ? (int) lib_games[players[i].game_sel].system : SYS_GBA;
		library_catalog(system[i], &first);
		pos[i] = players[i].game_sel - first;
		snprintf(selected[i], sizeof(selected[i]), "%s", lib_num_games ? lib_games[players[i].game_sel].file : "");
	}
	library_take_rescan();
	log_printf("web: library now has %d games\n", lib_num_games);
	for (int i = 0; i < MAX_PLAYERS; ++i) {
		struct player* p = &players[i];
		int g = library_find(selected[i]), first, n = library_catalog(system[i], &first);
		if (g < 0 && n > 0) {
			g = first + (pos[i] < n ? pos[i] : n - 1);
		} else if (g < 0) {
			g = p->game_sel < lib_num_games ? p->game_sel : lib_num_games - 1;
			g = g < 0 ? 0 : g;
		}
		p->game_scroll += g - p->game_sel; // no carousel jump
		p->game_sel = g;
		g = library_find(p->game_file);
		if (g >= 0) {
			p->game = g;
		} else {
			p->game = 0; // never picked one, or it is gone (deleted over FTP)
			if (p->state == PS_SAVES || p->state == PS_PATCHES || p->state == PS_PATCH_CONFIRM) p->state = PS_GAMES;
		}
	}
}

void web_frame(void) {
	bool saves_changed = false;
	if (share_trylock()) { // else next frame: the server may hold it while writing to the drive
		for (int i = 0; i < MAX_PLAYERS; ++i) {
			const struct player* p = &players[i];
			snprintf(share.in_use[i].game, sizeof(share.in_use[i].game), "%s", p->started ? p->game_file : "");
			snprintf(share.in_use[i].save, sizeof(share.in_use[i].save), "%s", p->started ? p->save_name : "");
			memcpy(web_names[i], share.names[i], PLAYER_NAME_MAX);
			if (share.url[0]) {
				snprintf(web_urls[i], sizeof(web_urls[i]), "%s/%s/", share.url, share.tokens[i]);
			} else {
				web_urls[i][0] = '\0';
			}
		}
		saves_changed = share.saves_changed;
		share.saves_changed = false;
		share_unlock();
	}
	for (int i = 0; i < MAX_PLAYERS; ++i) {
		refresh_saves(&players[i], saves_changed);
	}
	if (library_rescan_ready()) {
		take_rescan();
	}
}
