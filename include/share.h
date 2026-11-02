#ifndef SHARE_H
#define SHARE_H

#include <stdbool.h>
#include <ppu-types.h>

#include "player.h"

#define PLAYER_NAME_MAX 13 // 12 characters

// State exchanged between the main loop and the network threads. Hold the lock for any access.
struct share {
	char names[MAX_PLAYERS][PLAYER_NAME_MAX]; // set from the phone, empty = default "P1"..
	struct {
		char game[256]; // empty when the player is not playing
		char save[64];
	} in_use[MAX_PLAYERS];
	char tokens[MAX_PLAYERS][9];  // per-session secret in each player's phone link
	char url[48];                 // "http://192.168.1.20:8000" once the network is up
	bool saves_changed;           // a phone added a save: refresh the save lists on screen
};

extern struct share share;

void share_init(void);
void share_lock(void);
bool share_trylock(void); // for the main loop, which must never wait on a network thread
void share_unlock(void);
// Writes names to disk; call with the lock held.
void share_save_names(void);

#endif
