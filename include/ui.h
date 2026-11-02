#ifndef UI_H
#define UI_H

#include <stdbool.h>

#include "player.h"

extern const u32 player_colors[MAX_PLAYERS];

void ui_init(void);
void ui_draw_background(int frame);
// Draws one player's slot: join screen, menus or their game.
void ui_draw_slot(const struct player* p, float x, float y, float w, float h, int frame, bool show_perf, bool multi);

// What fills the screen: the players' slots, or the main menu (one screen for everyone) and its
// pages. The menu's entries open the screen of the same number, then QUIT goes back to the XMB.
enum screen { SCREEN_PLAY, SCREEN_ABOUT, SCREEN_HELP, SCREEN_MENU };
enum { MENU_QUIT = SCREEN_MENU, MENU_ENTRIES };
// Draws the main menu or one of its pages; clamps the About page's scroll (in lines).
void ui_draw_menu(enum screen s, int sel, int* scroll, int frame);

// Pause menu entries, shared with the input handling in main.c. LINK is left out for games
// without a cable (emu_link_available).
enum { PAUSE_RESUME, PAUSE_QUIT, PAUSE_LINK, PAUSE_PERF, PAUSE_COUNT };
// Save options menu entries (Triangle on a save).
enum { SAVE_MENU_RENAME, SAVE_MENU_COPY, SAVE_MENU_DELETE, SAVE_MENU_BACK, SAVE_MENU_COUNT };
// Rename keyboard: a run of the same character is one wide key; ' ' is the space bar, '<'
// deletes, '>' is OK. Characters safe in file names on the drive and over FTP.
#define KB_ROWS 5
#define KB_COLS 10
extern const char* const kb_rows[KB_ROWS];

#endif
