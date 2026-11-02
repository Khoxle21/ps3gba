#ifndef INTRO_H
#define INTRO_H

#include <stdbool.h>

// Welcome screen at app start: the XMB icon's PS3 juggling four GBAs, in a loop until
// intro_leave(), then an iris transition into the main menu.
void intro_init(void);
bool intro_playing(void); // covers the screen: the players' input is not theirs yet
void intro_leave(void);   // Start or Cross: plays the transition
void intro_draw(void);    // the scene, or the iris opening over the UI drawn before it
// The juggling scene without its sky, k UI units per art pixel, at (x, y) on the unit grid:
// the main menu's logo.
void intro_draw_logo(float x, float y, int k, int frame);
// Black outside a circle (screen pixels), for other screen changes in the same style.
void intro_iris(float cx, float cy, float r);
// Dev tools: straight to the main menu, or hold one frame (frames since start, since leaving
// or -1) for screenshots.
void intro_skip(void);
void intro_seek(int frame, int leaving);

#endif
