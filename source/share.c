#include <stdio.h>
#include <string.h>

#include <sys/mutex.h>
#include <sys/systime.h>

#include "library.h"
#include "share.h"

#define NAMES_FILE DATA_DIR "/players.txt"

struct share share;
static sys_mutex_t lock;

void share_init(void) {
	sys_mutex_attr_t attr;
	sysMutexAttrInitialize(attr);
	sysMutexCreate(&lock, &attr);

	FILE* f = fopen(NAMES_FILE, "r");
	if (f) {
		char line[64];
		for (int i = 0; i < MAX_PLAYERS && fgets(line, sizeof(line), f); ++i) {
			line[strcspn(line, "\r\n")] = '\0';
			snprintf(share.names[i], PLAYER_NAME_MAX, "%.*s", PLAYER_NAME_MAX - 1, line); // long names are clipped
		}
		fclose(f);
	}
	// Tokens only need to be unguessable on a home network, not cryptographically strong.
	u64 seed = sysGetSystemTime() ^ 0x9e3779b97f4a7c15ull;
	for (int i = 0; i < MAX_PLAYERS; ++i) {
		seed = seed * 6364136223846793005ull + 1442695040888963407ull;
		snprintf(share.tokens[i], sizeof(share.tokens[i]), "%08x", (u32) (seed >> 32));
	}
}

void share_lock(void) {
	sysMutexLock(lock, 0);
}

bool share_trylock(void) {
	return sysMutexTryLock(lock) == 0;
}

void share_unlock(void) {
	sysMutexUnlock(lock);
}

void share_save_names(void) {
	FILE* f = fopen(NAMES_FILE, "w");
	if (!f) {
		return;
	}
	for (int i = 0; i < MAX_PLAYERS; ++i) {
		fprintf(f, "%s\n", share.names[i]);
	}
	fclose(f);
}
