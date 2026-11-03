// Wireless Adapter harness, run by tools/rfu_test.sh: 2-4 GBA cores in one process, each with
// source/rfu.c's adapter plugged in, driven by a script on one thread (a frame of each game in
// turn: runs repeat, but for the game clock, which is the host's).
//
// usage: harness <script> <rom> <save> [<rom> <save>...]   (one pair per player, save "-": none)
// Script, one command per line (players are 0-3, as in ps3gba's dev commands):
//   <p> <A|B|SELECT|START|UP|DOWN|LEFT|RIGHT|L|R>  press (held 2 frames, released 6)
//   ALL <key>               every player presses it at once
//   <p> HOLD <key> <frames> hold it
//   WAIT <frames>           run every game
//   <p> STALL <frames>      that game alone stops while the others run (a late emulation thread)
//   <p> WIRELESS ON|OFF     plug the adapter in or out (all start plugged in)
//   <p> RESET               reset that GBA (as its power switch: the game starts over)
//   SHOT <name>             all screens side by side, to <name>.ppm in the current folder
//   TRACE ON|OFF            log every adapter command (rfu_trace)
//   # comment
// ROMs and saves are read into memory; nothing is written back. At the end each game's save
// memory goes to P<n>.sav in the current folder (e.g. to check what a trade did).
#include <mgba/core/core.h>
#include <mgba/gba/core.h>
#include <mgba/internal/gba/gba.h>
#include <mgba/internal/gba/input.h>
#include <mgba-util/vfs.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rfu.h"

static int frame;

void log_printf(const char* format, ...) {
	va_list args;
	va_start(args, format);
	vprintf(format, args);
	va_end(args);
}

static void* read_file(const char* path, size_t* size) {
	FILE* f = fopen(path, "rb");
	if (!f) {
		perror(path);
		exit(1);
	}
	fseek(f, 0, SEEK_END);
	*size = ftell(f);
	rewind(f);
	void* data = malloc(*size ? *size : 1);
	if (fread(data, 1, *size, f) != *size) {
		perror(path);
		exit(1);
	}
	fclose(f);
	return data;
}

static void no_log(struct mLogger* logger, int category, enum mLogLevel level, const char* format, va_list args) {
	(void) logger, (void) category, (void) level, (void) format, (void) args;
}

static struct {
	struct mCore* core;
	struct rfu rfu;
	bool plugged;
	mColor video[GBA_VIDEO_HORIZONTAL_PIXELS * GBA_VIDEO_VERTICAL_PIXELS];
	uint32_t keys;
	int stall;
} players[RFU_MAX];
static int count;

static int key_of(const char* name) {
	static const char* const names[] = { "A", "B", "SELECT", "START", "RIGHT", "LEFT", "UP", "DOWN", "R", "L" };
	for (int k = 0; k < 10; ++k) {
		if (strcmp(name, names[k]) == 0) {
			return k;
		}
	}
	fprintf(stderr, "unknown key %s\n", name);
	exit(1);
}

static void run(int frames) {
	for (int f = 0; f < frames; ++f, ++frame) {
		for (int i = 0; i < count; ++i) {
			if (players[i].stall > 0) {
				--players[i].stall;
				continue;
			}
			players[i].core->setKeys(players[i].core, players[i].keys);
			players[i].core->runFrame(players[i].core);
		}
	}
}

static void press(int player, uint32_t keys, int hold) {
	for (int i = 0; i < count; ++i) {
		if (player < 0 || player == i) {
			players[i].keys = keys;
		}
	}
	run(hold);
	for (int i = 0; i < count; ++i) {
		players[i].keys = 0;
	}
	run(6);
}

static void plug(int i, bool on) {
	if (on != players[i].plugged) {
		players[i].core->setPeripheral(players[i].core, mPERIPH_GBA_LINK_PORT, on ? &players[i].rfu.d : NULL);
		players[i].plugged = on;
	}
}

static void shot(const char* name) {
	char path[300];
	snprintf(path, sizeof(path), "%s.ppm", name);
	FILE* f = fopen(path, "wb");
	int w = GBA_VIDEO_HORIZONTAL_PIXELS, h = GBA_VIDEO_VERTICAL_PIXELS;
	fprintf(f, "P6 %d %d 255\n", w * count + 4 * (count - 1), h);
	for (int y = 0; y < h; ++y) {
		for (int i = 0; i < count; ++i) {
			if (i) {
				for (int g = 0; g < 4; ++g) {
					fputc(255, f), fputc(255, f), fputc(255, f);
				}
			}
			for (int x = 0; x < w; ++x) {
				uint16_t c = players[i].video[y * w + x]; // RGB565 (COLOR_16_BIT, COLOR_5_6_5)
				fputc((c >> 11) << 3, f);
				fputc((c >> 5 & 0x3F) << 2, f);
				fputc((c & 0x1F) << 3, f);
			}
		}
	}
	fclose(f);
	printf("[%d] shot %s\n", frame, name);
}

int main(int argc, char** argv) {
	if (argc < 4 || argc % 2) {
		fprintf(stderr, "usage: %s <script> <rom> <save|-> [<rom> <save|->...]\n", argv[0]);
		return 1;
	}
	static struct mLogger quiet = { .log = no_log };
	mLogSetDefaultLogger(&quiet);
	rfu_init();
	count = (argc - 2) / 2;
	if (count > RFU_MAX) {
		fprintf(stderr, "at most %d players\n", RFU_MAX);
		return 1;
	}
	for (int i = 0; i < count; ++i) {
		size_t rom_size, save_size = 0;
		void* rom = read_file(argv[2 + 2 * i], &rom_size);
		void* save = strcmp(argv[3 + 2 * i], "-") ? read_file(argv[3 + 2 * i], &save_size) : NULL;
		struct mCore* core = GBACoreCreate();
		core->init(core);
		mCoreInitConfig(core, NULL);
		core->loadROM(core, VFileFromConstMemory(rom, rom_size));
		core->loadSave(core, VFileMemChunk(save, save_size));
		core->setVideoBuffer(core, players[i].video, GBA_VIDEO_HORIZONTAL_PIXELS);
		// The host's clock: games such as Unbound refuse a clock earlier than their save's.
		core->reset(core);
		players[i].core = core;
		rfu_create(&players[i].rfu, i);
		plug(i, true);
	}

	FILE* script = fopen(argv[1], "r");
	if (!script) {
		perror(argv[1]);
		return 1;
	}
	char line[256], a[32], b[32], name[256];
	int p, n;
	while (fgets(line, sizeof(line), script)) {
		line[strcspn(line, "\r\n#")] = 0;
		if (sscanf(line, "WAIT %d", &n) == 1) {
			run(n);
		} else if (sscanf(line, "SHOT %255s", name) == 1) {
			shot(name);
		} else if (sscanf(line, "TRACE %3s", a) == 1) {
			rfu_trace = strcmp(a, "ON") == 0;
		} else if (sscanf(line, "ALL %31s", a) == 1) {
			press(-1, 1 << key_of(a), 2);
		} else if (sscanf(line, "%d HOLD %31s %d", &p, a, &n) == 3 && p >= 0 && p < count) {
			press(p, 1 << key_of(a), n);
		} else if (sscanf(line, "%d STALL %d", &p, &n) == 2 && p >= 0 && p < count) {
			players[p].stall = n;
		} else if (sscanf(line, "%d RESE%1[T]", &p, a) == 2 && p >= 0 && p < count) {
			players[p].core->reset(players[p].core);
			printf("[%d] P%d reset\n", frame, p + 1);
		} else if (sscanf(line, "%d WIRELESS %3s", &p, a) == 2 && p >= 0 && p < count) {
			plug(p, strcmp(a, "ON") == 0);
			printf("[%d] P%d adapter %s\n", frame, p + 1, a);
		} else if (sscanf(line, "%d %31s", &p, a) == 2 && p >= 0 && p < count) {
			press(p, 1 << key_of(a), 2);
		} else if (sscanf(line, "%31s %31s", a, b) >= 1) {
			fprintf(stderr, "bad line: %s\n", line);
			return 1;
		}
	}
	for (int i = 0; i < count; ++i) {
		char state[64];
		rfu_describe(i, state, sizeof(state));
		printf("[%d] P%d: %s\n", frame, i + 1, state);
		void* sram;
		size_t size = players[i].core->savedataClone(players[i].core, &sram);
		char path[16];
		snprintf(path, sizeof(path), "P%d.sav", i + 1); // what the game saved, to check a trade
		FILE* f = fopen(path, "wb");
		if (f && size) {
			fwrite(sram, 1, size, f);
		}
		if (f) {
			fclose(f);
		}
		free(sram);
		players[i].core->deinit(players[i].core);
	}
	return 0;
}
