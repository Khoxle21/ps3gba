#ifndef WEB_H
#define WEB_H

#include "share.h"

#define WEB_PORT 8000 // webMAN MOD already serves 21, 80, 7887 and 38008

// Main thread copies, refreshed by web_frame(): each player's phone link
// ("http://192.168.1.20:8000/1a2b3c4d/", empty while the network is down) and name.
extern char web_urls[MAX_PLAYERS][64];
extern char web_names[MAX_PLAYERS][PLAYER_NAME_MAX];

// Starts the phone web server on its own threads (boot).
void web_init(void);
// Main loop, once per frame between input and drawing: exchanges state with the server without
// ever waiting on it, swaps in a rescanned game list and refreshes save lists being browsed.
void web_frame(void);

#endif
