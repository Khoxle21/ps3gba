// The look: a pixel-art night sky with drifting GBAs, Pokemon-style boxes, and one accent color
// per player. Sizes are in screen pixels; `u` (gfx_unit) is one art pixel.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "art.h"
#include "emu.h"
#include "intro.h"
#include "qrcodegen.h"
#include "ui.h"
#include "web.h"

// The author's message at the top of the About page. Lines break at '\n' and wrap to the page;
// accented letters are fine (the font has Latin-1).
#define ABOUT_SIGNATURE "PLAY GBA AS IT WAS INTENDED, ON THE COUCH WITH FRIENDS!\n" \
	"REVISIT YOUR OLD FAVORITE GBA GAMES WITH FAMILY AND FRIENDS.\nUP TO 4 PLAYERS AT A TIME :)"
// PLACEHOLDER, to replace: the web manual's address, a QR code on the Help page.
#define MANUAL_URL "https://example.com/ps3gba/manual"

const u32 player_colors[MAX_PLAYERS] = { 0x4aa8ffff, 0xff5a6eff, 0x4ad66dff, 0xffcf3fff };
const char* const kb_rows[KB_ROWS] = { "ABCDEFGHIJ", "KLMNOPQRST", "UVWXYZ-!  ", "0123456789", "<<<<<>>>>>" };

#define COL_TEXT   0xf2eeffff
#define COL_DIM    0x9a92c8ff
#define COL_SHADOW 0x0b0820ff
#define COL_PANEL  0x171233f2
#define COL_EDGE   0x06040fff
#define COL_DROP   0x00000080
#define COL_SCRIM  0x0b0920b0 // dims a slot behind a menu box
#define COL_WARN   0xff7a7aff
#define COL_FAINT  0x4e4878ff // unavailable, e.g. a catalog without games
#define COL_MENU   0xb4a8ffff // the shared screens' accent: the icon's "GBA" label
#define COL_SKY_TOP    0x1d1546
#define COL_SKY_BOTTOM 0x0b0920

#define TILE 128 // background tile, art pixels

static int u;              // art pixel
static int ts, tm, tl;     // text sizes: small, medium, large (screen px per font pixel)
static struct gfx_tex bg_tile, tex_sparkle, tex_cursor, tex_lock, tex_cross, tex_circle, tex_triangle,
	tex_square, tex_start, tex_sticks, tex_shoulders, tex_arrow, tex_space, tex_dualshock[MAX_PLAYERS],
	tex_dualshock_off, tex_dualshock_rim, tex_cart[3];

static u32 with_alpha(u32 rgba, u8 a) {
	return (rgba & 0xffffff00) | a;
}

static float snap(float v) {
	return floorf(v / u) * u;
}

// Stamps art into an ARGB buffer, wrapping around the edges so the tile repeats seamlessly.
static void stamp(u32* buf, int size, const char* const* rows, int x, int y, u32 (*color)(char)) {
	for (int j = 0; rows[j]; ++j) {
		for (int i = 0; rows[j][i]; ++i) {
			u32 c = color(rows[j][i]);
			if (c) {
				buf[((y + j) % size) * size + (x + i) % size] = c;
			}
		}
	}
}

static u32 bg_device_color(char c) {
	switch (c) {
	case 'o': return 0xff2b2458;
	case 'h': return 0xff372e6c;
	case 'b': return 0xff221b48;
	case 'k': case 'd': return 0xff181333;
	case 's': return 0xff1b3343;
	case 'w': return 0xff2c4e5a;
	case 'm': return 0xff1c173d;
	default: return 0;
	}
}

static u32 bg_sparkle_color(char c) {
	return c == 'w' ? 0xff332b6a : c == 'h' ? 0xff4a4196 : 0;
}

static void build_background(void) {
	u32* buf = calloc(TILE * TILE, 4);
	// A loose diagonal lattice, so the drift reads as a stream of handhelds.
	stamp(buf, TILE, art_gba, 6, 8, bg_device_color);
	stamp(buf, TILE, art_gba_sp, 80, 30, bg_device_color);
	stamp(buf, TILE, art_gba_sp, 20, 70, bg_device_color);
	stamp(buf, TILE, art_gba, 70, 92, bg_device_color);
	stamp(buf, TILE, art_sparkle, 52, 44, bg_sparkle_color);
	stamp(buf, TILE, art_sparkle, 116, 10, bg_sparkle_color);
	stamp(buf, TILE, art_sparkle, 50, 112, bg_sparkle_color);
	stamp(buf, TILE, art_sparkle, 112, 76, bg_sparkle_color);
	stamp(buf, TILE, art_sparkle, 2, 50, bg_sparkle_color);
	gfx_tex_from_pixels(&bg_tile, buf, TILE, TILE, TILE * 4);
	free(buf);
}

void ui_init(void) {
	u = gfx_unit;
	ts = u >= 4 ? 2 : u >= 3 ? 2 : 1;
	tm = u >= 4 ? 3 : 2;
	tl = u >= 4 ? 4 : u >= 3 ? 3 : 2;
	build_background();
	const char* white = "w=ffffffff h=ffffffff";
	gfx_tex_from_art(&tex_sparkle, art_sparkle, white);
	gfx_tex_from_art(&tex_cursor, art_cursor, white);
	gfx_tex_from_art(&tex_lock, art_lock, white);
	gfx_tex_from_art(&tex_cross, art_btn_cross, white);
	gfx_tex_from_art(&tex_circle, art_btn_circle, white);
	gfx_tex_from_art(&tex_triangle, art_btn_triangle, white);
	gfx_tex_from_art(&tex_square, art_btn_square, white);
	gfx_tex_from_art(&tex_start, art_btn_start, white);
	gfx_tex_from_art(&tex_sticks, art_btn_sticks, white);
	gfx_tex_from_art(&tex_shoulders, art_btn_shoulders, white);
	gfx_tex_from_art(&tex_arrow, art_arrow, white);
	gfx_tex_from_art(&tex_space, art_key_space, white);
	// Glossy black DualShock 3, one per player with that player's LED lit.
	for (int i = 0; i < MAX_PLAYERS; ++i) {
		char palette[256];
		u32 lit = 0xff000000 | (player_colors[i] >> 8);
		snprintf(palette, sizeof(palette),
			"o=ff0b0a14 h=ff5d5c80 b=ff3a3950 d=ff14131c g=ff14131c r=ff6a6990 s=ff1e1d2a P=ff4e4d6c "
			"t=ff5ad79a q=fff28fd0 c=ffff6b6b x=ff7aa7ff 1=%08x 2=%08x 3=%08x 4=%08x",
			i == 0 ? lit : 0xff2a2938, i == 1 ? lit : 0xff2a2938, i == 2 ? lit : 0xff2a2938, i == 3 ? lit : 0xff2a2938);
		gfx_tex_from_art(&tex_dualshock[i], art_dualshock, palette);
	}
	// All LEDs off: alternating with the lit one makes the LED blink like a real pad waiting.
	gfx_tex_from_art(&tex_dualshock_off, art_dualshock,
		"o=ff0b0a14 h=ff5d5c80 b=ff3a3950 d=ff14131c g=ff14131c r=ff6a6990 s=ff1e1d2a P=ff4e4d6c "
		"t=ff5ad79a q=fff28fd0 c=ffff6b6b x=ff7aa7ff 1=ff2a2938 2=ff2a2938 3=ff2a2938 4=ff2a2938");
	// Its silhouette grown by one pixel in all 8 directions, in white: drawn in the player color
	// under the pad as a rim (one quad instead of 8 shifted silhouettes).
	int sw = 0, sh = 0;
	for (; art_dualshock[sh]; ++sh) {
		int len = strlen(art_dualshock[sh]);
		if (len > sw) sw = len;
	}
	u32* rim = calloc((sw + 2) * (sh + 2), 4);
	for (int y = 0; y < sh; ++y) {
		for (int x = 0; art_dualshock[y][x]; ++x) {
			if (!strchr("ohbdgrsPtqcx1234", art_dualshock[y][x])) continue; // the pads' palette
			for (int dy = -1; dy <= 1; ++dy) {
				for (int dx = -1; dx <= 1; ++dx) {
					if (dx || dy) rim[(y + 1 + dy) * (sw + 2) + x + 1 + dx] = 0xffffffff;
				}
			}
		}
	}
	gfx_tex_from_pixels(&tex_dualshock_rim, rim, sw + 2, sh + 2, (sw + 2) * 4);
	free(rim);
	// Blank Game Paks in each system's plastic, for games without a cover.
	gfx_tex_from_art(&tex_cart[SYS_GB], art_cart_gb, "o=ff2a2840 b=ffb4b2c2 h=ffd8d6e4 s=ff8a889c "
		"k=ff5c5a70 l=ffeeeae2 L=ffa6a4b8 A=ffcfcde0");
	gfx_tex_from_art(&tex_cart[SYS_GBC], art_cart_gbc, "o=ff0c0a16 b=ff3c3852 h=ff6c6890 s=ff26233a "
		"k=ff16141f l=ffeeeae2 L=ff8a86a8 A=ffcfcde0 g=ffc8a24a p=ff6c6890");
	gfx_tex_from_art(&tex_cart[SYS_GBA], art_cart_gba, "o=ff2a2840 b=ffa2a2b4 h=ffcacada s=ff78788e "
		"k=ff4e4e64 l=ffeeeae2 L=ff9a9ab0 A=ffcfcde0");
}

// ---------------------------------------------------------------------------------------------
// Primitives

static void sprite(const struct gfx_tex* t, float x, float y, int scale, u32 tint) {
	gfx_image(t, x, y, t->w * scale, t->h * scale, tint, false);
}

// Rectangle with its corners notched by one art pixel, drawn as three non-overlapping strips
// so translucent colors don't double up.
static void notched(float x, float y, float w, float h, u32 color) {
	gfx_rect(x + u, y, w - 2 * u, h, color);
	gfx_rect(x, y + u, u, h - 2 * u, color);
	gfx_rect(x + w - u, y + u, u, h - 2 * u, color);
}

// Pokemon-style box: hard drop shadow, 1 art pixel border (always opaque), notched corners. Only
// what shows is drawn, the RSX's time going into filling pixels: the shadow beside the border, the
// border's ring around an opaque fill, and with `covered` (something opaque covers the inside, 2
// art pixels in, right after) the fill's ring.
static void box_layers(float x, float y, float w, float h, u32 fill, u32 border, bool covered) {
	gfx_rect(x + 2 * u, y + h, w - 2 * u, u, COL_DROP); // shadow below,
	gfx_rect(x + w - u, y + h - u, u, u, COL_DROP);     // in the border's notch
	gfx_rect(x + w, y + 2 * u, u, h - 2 * u, COL_DROP); // and on the right
	if ((fill & 0xff) == 0xff) {
		gfx_rect(x + u, y, w - 2 * u, 2 * u, border);
		gfx_rect(x + u, y + h - 2 * u, w - 2 * u, 2 * u, border);
		gfx_rect(x, y + u, u, h - 2 * u, border);
		gfx_rect(x + w - u, y + u, u, h - 2 * u, border);
	} else {
		notched(x, y, w, h, border);
	}
	if (covered) {
		gfx_rect(x + 2 * u, y + u, w - 4 * u, u, fill);
		gfx_rect(x + 2 * u, y + h - 2 * u, w - 4 * u, u, fill);
		gfx_rect(x + u, y + 2 * u, u, h - 4 * u, fill);
		gfx_rect(x + w - 2 * u, y + 2 * u, u, h - 4 * u, fill);
	} else {
		notched(x + u, y + u, w - 2 * u, h - 2 * u, fill);
	}
}

static void box(float x, float y, float w, float h, u32 fill, u32 border) {
	box_layers(x, y, w, h, fill, border, false);
}

static void text(float x, float y, int size, const char* s, u32 color) {
	gfx_text_shadow(snap(x), snap(y), size, s, color, COL_SHADOW);
}

static void text_centered(float cx, float y, int size, const char* s, u32 color) {
	text(cx - gfx_text_width(s, size) / 2, y, size, s, color);
}

// Shrinks the text size, then truncates, until `s` fits in `max_w`.
static int fit_text(char* s, int size, float max_w) {
	while (size > 1 && gfx_text_width(s, size) > max_w) {
		--size;
	}
	int max_chars = max_w / (8 * size);
	if ((int) strlen(s) > max_chars && max_chars > 2) {
		strcpy(s + max_chars - 2, "..");
	}
	return size;
}

// A label on a colored tab; returns its width.
static float tab(float x, float y, int size, const char* label, u32 color) {
	float w = gfx_text_width(label, size) + 4 * u, h = 8 * size + 4 * u;
	box(x, y, w, h, color, COL_EDGE);
	gfx_text(x + 2 * u, y + 2 * u, size, label, COL_EDGE);
	return w;
}

// The "P1" tab (or the player's name) in the player's color; returns its width.
static float player_tab(const struct player* p, float x, float y, int size, const char* name) {
	char label[PLAYER_NAME_MAX];
	if (name && name[0]) snprintf(label, sizeof(label), "%s", name);
	else snprintf(label, sizeof(label), "P%d", p->index + 1);
	return tab(x, y, size, label, player_colors[p->index]);
}

// Tab plus screen title on the same line, e.g. [P1] CHOOSE A GAME. The title is left out when it
// would pass `right` (a long player name in a small slot).
static void header(const struct player* p, float x, float y, const char* title, float right) {
	float w = player_tab(p, x, y, tm, web_names[p->index]);
	if (x + w + 4 * u + gfx_text_width(title, tm) <= right) {
		text(x + w + 4 * u, y + 2 * u, tm, title, COL_TEXT);
	}
}

static const char* const system_names[SYS_COUNT] = { [SYS_GBA] = "GBA", [SYS_GBC] = "GBC", [SYS_GB] = "GB" };

// The catalogs as tabs ending at `right`, e.g. GBA GBC GB: the shown one in the player's color,
// the others dim, empty ones fainter (L1/R1 skips them). Returns their left edge.
static float catalog_tabs(const struct player* p, float right, float y, int shown) {
	float x = right;
	for (int s = SYS_COUNT - 1; s >= 0; --s) {
		float w = gfx_text_width(system_names[s], tm) + 4 * u;
		x -= w;
		int first;
		if (s == shown) {
			box(x, y, w, 8 * tm + 4 * u, player_colors[p->index], COL_EDGE);
			gfx_text(x + 2 * u, y + 2 * u, tm, system_names[s], COL_EDGE);
		} else {
			text(x + 2 * u, y + 2 * u, tm, system_names[s], library_catalog(s, &first) ? COL_DIM : COL_FAINT);
		}
		x -= 2 * u;
	}
	return x + 2 * u;
}

#define HINT_ICON_GAP 4  // font pixels between a button icon and its label
#define HINT_GAP 14      // font pixels between two hints

static float hint(float x, float y, const struct gfx_tex* icon, u32 icon_color, const char* label) {
	sprite(icon, x, y, ts, icon_color);
	x += (icon->w + HINT_ICON_GAP) * ts;
	text(x, y, ts, label, COL_DIM);
	return x + gfx_text_width(label, ts) + HINT_GAP * ts;
}

// Button colors as printed on the pad.
#define COL_BTN_CROSS    0x7aa7ffff
#define COL_BTN_CIRCLE   0xff7a7aff
#define COL_BTN_TRIANGLE 0x5ad79aff
#define COL_BTN_SQUARE   0xf28fd0ff
#define COL_BTN_PLAIN    0xc8c2e6ff // labeled buttons (START, L3/R3)

struct hint_def {
	const struct gfx_tex* icon;
	u32 color;
	const char* label;
};

static float hints_width(const struct hint_def* h, int n) {
	float w = -HINT_GAP * ts;
	for (int i = 0; i < n; ++i) {
		w += (h[i].icon->w + HINT_ICON_GAP + HINT_GAP) * ts + gfx_text_width(h[i].label, ts);
	}
	return w;
}

// Button hints right-aligned at `right`, e.g. [X] PLAY  [O] BACK. Every screen lists each button
// that does something there (the d-pad aside), on one row even in a 2x2 slot at 720p.
static void hint_row(float right, float y, const struct hint_def* h, int n) {
	float x = right - hints_width(h, n);
	for (int i = 0; i < n; ++i) {
		x = hint(x, y, h[i].icon, h[i].color, h[i].label);
	}
}

static void hints(float right, float y, const char* confirm, const char* back) {
	const struct hint_def h[] = { { &tex_cross, COL_BTN_CROSS, confirm }, { &tex_circle, COL_BTN_CIRCLE, back } };
	hint_row(right, y, h, back ? 2 : 1);
}

// Cover art, or a blank Game Pak of the game's system at a whole-number scale.
static void cover(const struct game* g, float x, float y, float size, u32 tint) {
	if (g->has_cover) {
		float w = size, h = size;
		if (g->cover.w > g->cover.h) h = size * g->cover.h / g->cover.w;
		else w = size * g->cover.w / g->cover.h;
		gfx_image(&g->cover, x + (size - w) / 2, y + (size - h) / 2, w, h, tint, true);
		return;
	}
	const struct gfx_tex* cart = &tex_cart[g->system];
	int k = size / (cart->w > cart->h ? cart->w : cart->h);
	if (k < 1) k = 1;
	sprite(cart, snap(x + (size - cart->w * k) / 2), snap(y + (size - cart->h * k) / 2), k, tint);
}

// ---------------------------------------------------------------------------------------------
// Background

void ui_draw_background(int frame) {
	// Stepped gradient: hard bands instead of a smooth ramp, like a 16-bit sky.
	const int bands = 12;
	for (int i = 0; i < bands; ++i) {
		float t = (float) i / (bands - 1);
		u32 r = ((COL_SKY_TOP >> 16) & 0xff) * (1 - t) + ((COL_SKY_BOTTOM >> 16) & 0xff) * t;
		u32 g = ((COL_SKY_TOP >> 8) & 0xff) * (1 - t) + ((COL_SKY_BOTTOM >> 8) & 0xff) * t;
		u32 b = (COL_SKY_TOP & 0xff) * (1 - t) + (COL_SKY_BOTTOM & 0xff) * t;
		float y0 = snap(gfx_h * i / bands), y1 = i == bands - 1 ? gfx_h : snap(gfx_h * (i + 1) / bands);
		gfx_rect(0, y0, gfx_w, y1 - y0, r << 24 | g << 16 | b << 8 | 0xff);
	}
	// The handheld tile drifts from top-right to bottom-left, one screen pixel per frame at
	// 1080p: whole screen pixels keep the art crisp while the motion stays smooth. Drawn as a
	// grid of plain quads: the RSX can't repeat-wrap linear textures (it shows garbage).
	int tile = TILE * u;
	int shift = (int) (frame * 0.25f * u) % tile;
	for (int ty = -tile + shift; ty < (int) gfx_h; ty += tile) {
		for (int tx = -shift; tx < (int) gfx_w; tx += tile) {
			gfx_image(&bg_tile, tx, ty, tile, tile, 0xffffffff, false);
		}
	}
	// A few brighter sparkles that twinkle in place.
	for (int i = 0; i < 14; ++i) {
		u32 h = (i + 1) * 2654435761u;
		float x = snap((h % 1000) / 1000.0f * gfx_w), y = snap(((h >> 10) % 1000) / 1000.0f * gfx_h);
		float phase = sinf(frame * 0.035f + i * 1.7f);
		if (phase > 0.4f) {
			u8 a = (u8) ((phase - 0.4f) / 0.6f * 160);
			sprite(&tex_sparkle, x, y, u, with_alpha(0xc8c0ffff, a));
		}
	}
}

// ---------------------------------------------------------------------------------------------
// Slots

static void draw_join(const struct player* p, float x, float y, float w, float h, int frame) {
	float cx = x + w / 2;
	u32 color = player_colors[p->index];
	char label[12];
	snprintf(label, sizeof(label), "P%d", p->index + 1);
	int big = tl * 2;
	const struct gfx_tex* pad = &tex_dualshock[p->index];
	int pad_scale = h > 600 ? 3 * u : 2 * u;
	if (pad->w * pad_scale > w * 0.8f) pad_scale = u;
	float total = 8 * big + 6 * u + pad->h * pad_scale + 6 * u + 8 * tm;
	float top = y + (h - total) / 2;
	text_centered(cx, top, big, label, p->connected ? color : COL_DIM);
	top += 8 * big + 6 * u;
	float px = snap(cx - pad->w * pad_scale / 2), py = snap(top);
	// Blink the player LED while waiting to join, like a real DualShock 3.
	if (p->connected && (frame / 20) % 2) {
		pad = &tex_dualshock_off;
	}
	// Rim one controller pixel thick.
	sprite(&tex_dualshock_rim, px - pad_scale, py - pad_scale, pad_scale, p->connected ? color : 0x3a3560ff);
	sprite(pad, px, py, pad_scale, p->connected ? 0xffffffff : 0x8a86a8ff);
	top += pad->h * pad_scale + 6 * u;
	if (p->connected) {
		if ((frame / 30) % 2 == 0) {
			text_centered(cx, top, tm, "PRESS START", COL_TEXT);
		}
	} else {
		text_centered(cx, top, ts, "CONNECT A CONTROLLER", COL_DIM);
	}
	if (p->connected) {
		bool nobody = true; // then Circle goes back to the main menu
		for (int i = 0; i < MAX_PLAYERS; ++i) {
			nobody &= players[i].state == PS_EMPTY;
		}
		const struct hint_def legend[] = { { &tex_start, COL_BTN_PLAIN, "JOIN" }, { &tex_circle, COL_BTN_CIRCLE, "MENU" } };
		hint_row(x + w - 6 * u, y + h - 6 * u - 8 * ts, legend, nobody ? 2 : 1);
	}
}

// Horizontal offset of a carousel item `d` steps away from the selection.
static float carousel_x(float d, float big, float small, float gap) {
	float a = fabsf(d), first = big / 2 + gap + small / 2;
	float off = a <= 1 ? a * first : first + (a - 1) * (small + gap);
	return d < 0 ? -off : off;
}

static void draw_games(const struct player* p, float x, float y, float w, float h, int frame) {
	u32 color = player_colors[p->index];
	float pad = 6 * u;
	if (lib_num_games == 0) {
		header(p, x + pad, y + pad, "CHOOSE A GAME", x + w - pad);
		const char* add = "ADD SOME FROM YOUR PHONE";
		float add_w = (tex_triangle.w + HINT_ICON_GAP) * ts + gfx_text_width(add, ts);
		text_centered(x + w / 2, y + h / 2 - 8 * tm, tm, "NO GAMES YET", COL_TEXT);
		hint(snap(x + (w - add_w) / 2), y + h / 2 + 4 * u, &tex_triangle, COL_BTN_TRIANGLE, add);
		text_centered(x + w / 2, y + h / 2 + 8 * ts + 8 * u, ts, "OR COPY GAMES TO /dev_hdd0/ps3gba/roms", COL_DIM);
		const struct hint_def legend[] = { { &tex_triangle, COL_BTN_TRIANGLE, "PHONE" }, { &tex_circle, COL_BTN_CIRCLE, "BACK" } };
		hint_row(x + w - pad, y + h - pad - 8 * ts, legend, 2);
		return;
	}
	// The carousel shows one catalog (system) at a time: the selected game's.
	int system = lib_games[p->game_sel].system, first, n = library_catalog(system, &first), catalogs = 0;
	for (int s = 0; s < SYS_COUNT; ++s) {
		int unused;
		catalogs += library_catalog(s, &unused) > 0;
	}
	float tabs = catalog_tabs(p, x + w - pad, y + pad, system);
	header(p, x + pad, y + pad, "CHOOSE A GAME", tabs - 4 * u);

	float big = snap(fminf(h * 0.48f, w * 0.34f)), small = snap(big * 0.7f), gap = 4 * u;
	// Raised when the title and count under the cover would reach the legend (2x2 at 720p).
	float legend_y = y + h - pad - 8 * ts, below = 8 * u + 8 * tm + 6 * u + 8 * ts;
	float cx = x + w / 2, cy = fminf(y + h * 0.47f, legend_y - 3 * u - below - big / 2);
	// Far items first, so the selection is drawn on top.
	for (int pass = 3; pass >= 0; --pass) {
		for (int i = first; i < first + n; ++i) {
			float d = i - p->game_scroll;
			if ((int) roundf(fabsf(d)) != pass) {
				continue;
			}
			float t = fminf(fabsf(d), 1.0f);
			float size = big + (small - big) * t;
			float ix = cx + carousel_x(d, big, small, gap) - size / 2;
			if (ix < x + pad || ix + size > x + w - pad) {
				continue;
			}
			float iy = cy - size / 2;
			if (i == p->game_sel) {
				iy += sinf(frame * 0.07f) > 0 ? -u : 0; // gentle bob
				box(snap(ix) - 2 * u, snap(iy) - 2 * u, snap(size) + 4 * u, snap(size) + 4 * u, color, COL_EDGE);
			}
			// Neighbors are darkened, not faded, so the background never shows through.
			u8 shade = 255 - (u8) (t * 120);
			cover(&lib_games[i], snap(ix), snap(iy), snap(size), shade << 24 | shade << 16 | (shade + (255 - shade) / 4) << 8 | 0xff);
		}
	}
	char title[96];
	snprintf(title, sizeof(title), "%s", lib_games[p->game_sel].title);
	float arrow_w = (tex_cursor.w + 4) * tm;
	int size = fit_text(title, tm, w - 2 * pad - 2 * arrow_w);
	float ty = snap(cy + big / 2 + 8 * u);
	text_centered(cx, ty, size, title, COL_TEXT);
	// Blinking arrows around the title when there is more to scroll to.
	if ((frame / 24) % 2 == 0) {
		float half = gfx_text_width(title, size) / 2, ay = ty + (8 * size - tex_cursor.h * size) / 2;
		if (p->game_sel > first) {
			gfx_image_uv(&tex_cursor, snap(cx - half - arrow_w), snap(ay), tex_cursor.w * size, tex_cursor.h * size,
			             1, 0, 0, 1, color, false);
		}
		if (p->game_sel < first + n - 1) {
			sprite(&tex_cursor, snap(cx + half + 4 * size), snap(ay), size, color);
		}
	}
	char count[16];
	snprintf(count, sizeof(count), "%d/%d", p->game_sel - first + 1, n);
	text_centered(cx, ty + 8 * size + 6 * u, ts, count, COL_DIM);
	// L1/R1 only when there is another catalog to switch to.
	const struct hint_def legend[] = {
		{ &tex_shoulders, COL_BTN_PLAIN, "SYSTEM" }, { &tex_cross, COL_BTN_CROSS, "PICK" },
		{ &tex_triangle, COL_BTN_TRIANGLE, "PHONE" }, { &tex_circle, COL_BTN_CIRCLE, "BACK" },
	};
	hint_row(x + w - pad, legend_y, legend + (catalogs < 2), 4 - (catalogs < 2));
}

static void relative_time(time_t then, char* out, size_t len) {
	long ago = (long) (time(NULL) - then);
	if (then <= 0) snprintf(out, len, "-");
	else if (ago < 60) snprintf(out, len, "JUST NOW");
	else if (ago < 3600) snprintf(out, len, "%ldM AGO", ago / 60);
	else if (ago < 86400) snprintf(out, len, "%ldH AGO", ago / 3600);
	else snprintf(out, len, "%ldD AGO", ago / 86400);
}

// Centered box like the pause menu: a title in the player's color over rows with a cursor.
static void menu_box(const struct player* p, float x, float y, float w, float h, const char* title,
                     const char* const* labels, int n, int sel) {
	u32 color = player_colors[p->index];
	float inner = 6 * u, gap = 3 * u, cursor_w = tex_cursor.w * tm;
	char t[80];
	snprintf(t, sizeof(t), "%s", title);
	int title_size = fit_text(t, tm, w - 2 * inner - 16 * u); // long names copied over FTP
	float rows_w = 0;
	for (int i = 0; i < n; ++i) {
		rows_w = fmaxf(rows_w, cursor_w + gap + gfx_text_width(labels[i], tm));
	}
	float row_h = 8 * tm + 4 * u;
	float title_h = 8 * tm + 6 * u;
	float bw = snap(2 * inner + fmaxf(rows_w, gfx_text_width(t, title_size)));
	float bh = snap(2 * inner + title_h + n * row_h - 4 * u);
	float bx = snap(x + (w - bw) / 2), by = snap(y + (h - bh) / 2);
	box(bx, by, bw, bh, COL_PANEL, color);
	text_centered(bx + bw / 2, by + inner + (8 * tm - 8 * title_size) / 2, title_size, t, color);
	float rx = snap(bx + (bw - rows_w) / 2);
	float shake = p->flash_frames > 0 ? ((p->flash_frames / 2) % 2 ? u : -u) : 0;
	for (int i = 0; i < n; ++i) {
		float ry = by + inner + title_h + i * row_h, dx = i == sel ? shake : 0;
		if (i == sel) {
			sprite(&tex_cursor, rx + dx, ry, tm, color); // 7 rows like the capitals (glyph row 8 is empty)
		}
		text(rx + cursor_w + gap + dx, ry, tm, labels[i], i == sel ? COL_TEXT : COL_DIM);
	}
}

// Rename keyboard: the name on one slot per character (Pokemon style), then the keys.
static void draw_keyboard(const struct player* p, float x, float y, float w, float h, int frame) {
	u32 color = player_colors[p->index];
	float inner = 6 * u, cell = 8 * tm + 4 * u; // a key: one glyph with 2 art pixels around it
	float title_h = 8 * tm + 6 * u, field_h = 8 * tm + 6 * u;
	float bw = snap(2 * inner + KB_COLS * cell), bh = snap(2 * inner + title_h + field_h + KB_ROWS * cell);
	float bx = snap(x + (w - bw) / 2), by = snap(y + (h - bh) / 2);
	box(bx, by, bw, bh, COL_PANEL, color);
	text_centered(bx + bw / 2, by + inner, tm, p->name_taken ? "NAME TAKEN" : "RENAME", p->name_taken ? COL_WARN : color);

	float glyph = 8 * tm, shake = p->flash_frames > 0 ? ((p->flash_frames / 2) % 2 ? u : -u) : 0;
	float fx = snap(bx + (bw - LIB_SAVE_NAME_MAX * glyph) / 2) + shake, fy = by + inner + title_h;
	int len = strlen(p->op_to);
	// Lines under the free slots only: under typed text they'd read as underscores.
	for (int i = len; i < LIB_SAVE_NAME_MAX; ++i) {
		// Glyphs are 7 font pixels wide plus one of spacing, and leave their bottom row empty.
		bool next = i == len && (frame / 16) % 2 == 0;
		gfx_rect(fx + i * glyph + tm, fy + glyph + tm, 7 * tm, tm, COL_SHADOW);
		gfx_rect(fx + i * glyph, fy + glyph, 7 * tm, tm, next ? color : COL_DIM);
	}
	text(fx, fy, tm, p->op_to, COL_TEXT);

	float gx = bx + inner, gy = fy + field_h;
	for (int r = 0; r < KB_ROWS; ++r) {
		for (int c = 0; c < KB_COLS;) {
			char k = kb_rows[r][c];
			int span = 1;
			while (c + span < KB_COLS && kb_rows[r][c + span] == k) {
				++span;
			}
			float kx = gx + c * cell, ky = gy + r * cell, kw = span * cell;
			bool sel = p->key_sel / KB_COLS == r && p->key_sel % KB_COLS >= c && p->key_sel % KB_COLS < c + span;
			if (sel) {
				notched(kx + u, ky + u, kw - 2 * u, cell - 2 * u, color);
			}
			u32 fg = sel ? COL_EDGE : COL_TEXT;
			if (k == ' ') {
				float sx = snap(kx + (kw - tex_space.w * tm) / 2), sy = ky + 2 * u + 4 * tm;
				if (!sel) sprite(&tex_space, sx + tm, sy + tm, tm, COL_SHADOW);
				sprite(&tex_space, sx, sy, tm, fg);
			} else {
				char label[4] = { k, 0 };
				if (k == '<') snprintf(label, sizeof(label), "DEL");
				if (k == '>') snprintf(label, sizeof(label), "OK");
				float lx = snap(kx + (kw - gfx_text_width(label, tm)) / 2), ly = ky + 2 * u;
				if (sel) gfx_text(lx, ly, tm, label, fg);
				else text(lx, ly, tm, label, fg);
			}
			c += span;
		}
	}
}

static void draw_saves(const struct player* p, float x, float y, float w, float h, int frame, bool multi) {
	u32 color = player_colors[p->index];
	float pad = 6 * u;
	const struct game* g = &lib_games[p->game];
	header(p, x + pad, y + pad, "CHOOSE A SAVE", x + w - pad);

	// Cover and save list form one block, centered between the header and the hints, with
	// the list panel exactly as tall as the framed cover.
	float margin = 8 * u;
	float top = y + pad + 8 * tm + 4 * u + margin;
	float bottom = y + h - pad - 8 * ts - margin;
	float framed = snap(fminf(bottom - top, w * 0.34f));
	float cover_size = framed - 4 * u;
	float bx = x + margin, by = snap(top + (bottom - top - framed) / 2);
	box(bx, by, framed, framed, color, COL_EDGE);
	cover(g, bx + 2 * u, by + 2 * u, cover_size, 0xffffffff);

	float lx = bx + framed + 6 * u, lw = x + w - margin - lx;
	char title[96];
	snprintf(title, sizeof(title), "%s", g->title);
	int title_size = fit_text(title, ts, lw);
	text(lx, by, title_size, title, COL_DIM);

	float list_top = by + 8 * title_size + 4 * u;
	float list_h = by + framed - list_top;
	float row_h = 8 * tm + 6 * u;
	int rows = p->num_saves < 0 ? 0 : p->num_saves + 1; // + "NEW SAVE", none while reading
	int visible = (list_h - 4 * u) / row_h;
	if (visible < 1) visible = 1;
	int first = p->save_sel - visible + 1;
	if (first < 0) first = 0;
	box(lx, list_top, lw, list_h, COL_PANEL, COL_EDGE);

	for (int r = first; r < rows && r < first + visible; ++r) {
		float ry = list_top + 2 * u + (r - first) * row_h;
		bool sel = r == p->save_sel;
		float shake = sel && p->flash_frames > 0 ? ((p->flash_frames / 2) % 2 ? u : -u) : 0;
		float tx = lx + 4 * u + tex_cursor.w * tm + 3 * u + shake;
		float ty = ry + 3 * u;
		if (sel) {
			gfx_rect(lx + u, ry, lw - 2 * u, row_h, with_alpha(color, 0x38));
			sprite(&tex_cursor, lx + 3 * u + shake, ty + u, tm, color);
		}
		static const char* const busy_labels[] = { [LIB_RENAME] = "RENAMING", [LIB_COPY] = "COPYING", [LIB_DELETE] = "DELETING" };
		const char* busy = !sel ? NULL : p->state == PS_LOADING ? "LOADING" : p->state == PS_SAVE_BUSY ? busy_labels[p->save_op] : NULL;
		if (busy) {
			text(lx + lw - 4 * u - gfx_text_width(busy, ts), ty + (8 * tm - 8 * ts) / 2, ts, busy, color);
		}
		if (r == p->num_saves) {
			text(tx, ty, tm, "+ NEW SAVE", sel ? COL_TEXT : COL_DIM);
			continue;
		}
		const struct save_info* s = &p->saves[r];
		int owner = emu_save_owner(p->game_file, s->name);
		bool locked = owner >= 0 && owner != p->index;
		char name[64];
		snprintf(name, sizeof(name), "%s", s->name);
		text(tx, ty, tm, name, locked ? COL_DIM : COL_TEXT);
		if (busy) {
			continue;
		}
		char right[24];
		u32 right_color = COL_DIM;
		if (locked) {
			snprintf(right, sizeof(right), "P%d", owner + 1);
			right_color = player_colors[owner];
		} else {
			relative_time(s->mtime, right, sizeof(right));
		}
		float rx = lx + lw - 4 * u - gfx_text_width(right, ts);
		text(rx, ty + (8 * tm - 8 * ts) / 2, ts, right, right_color);
		if (locked) {
			sprite(&tex_lock, rx - (tex_lock.w + 2) * ts, ty + (8 * tm - 8 * ts) / 2, ts, right_color);
		}
	}

	if (p->state == PS_SAVE_MENU || p->state == PS_SAVE_DELETE || p->state == PS_SAVE_RENAME) {
		float in = multi ? 2 * u : 0; // inside the slot's outline
		gfx_rect(x + in, y + in, w - 2 * in, h - 2 * in, COL_SCRIM);
	}
	float hy = y + h - pad - 8 * ts;
	if (p->state == PS_SAVE_MENU) {
		static const char* const labels[SAVE_MENU_COUNT] = { "RENAME", "COPY", "DELETE", "BACK" };
		menu_box(p, x, y, w, h, p->op_save, labels, SAVE_MENU_COUNT, p->menu_sel);
		hints(x + w - pad, hy, "OK", "BACK");
	} else if (p->state == PS_SAVE_DELETE) {
		static const char* const labels[] = { "NO", "YES" };
		char title[80];
		snprintf(title, sizeof(title), "DELETE %s?", p->op_save);
		menu_box(p, x, y, w, h, title, labels, 2, p->menu_sel);
		hints(x + w - pad, hy, "OK", "BACK");
	} else if (p->state == PS_SAVE_RENAME) {
		draw_keyboard(p, x, y, w, h, frame);
		const struct hint_def h3[] = {
			{ &tex_cross, COL_BTN_CROSS, "TYPE" }, { &tex_square, COL_BTN_SQUARE, "DEL" },
			{ &tex_start, COL_BTN_PLAIN, "OK" }, { &tex_circle, COL_BTN_CIRCLE, "CANCEL" },
		};
		hint_row(x + w - pad, hy, h3, 4);
	} else {
		// How to reach the pause menu, told before the game starts: in game it would cover it.
		const struct hint_def pause = { &tex_sticks, COL_BTN_PLAIN, "PAUSE" };
		hint_row(x + pad + hints_width(&pause, 1), hy, &pause, 1);
		if (p->state == PS_LOADING) {
			return; // nothing else to press until the game runs
		}
		if (p->save_sel < p->num_saves) {
			const struct hint_def h3[] = {
				{ &tex_cross, COL_BTN_CROSS, "PLAY" }, { &tex_triangle, COL_BTN_TRIANGLE, "OPTIONS" }, { &tex_circle, COL_BTN_CIRCLE, "BACK" },
			};
			hint_row(x + w - pad, hy, h3, 3);
		} else {
			hints(x + w - pad, hy, "PLAY", "BACK");
		}
	}
}

// Whole screen pixels per QR module (whole art pixels when big enough, at most 3) for a code
// and its 4-module quiet zone to fit in `room`.
static int qr_scale(const u8* qr, float room) {
	int k = room / (qrcodegen_getSize(qr) + 8);
	if (k > 3 * u) k = 3 * u;
	if (k > u) k -= k % u;
	return k < 1 ? 1 : k;
}

// A QR code with its quiet zone at (x, y), k pixels per module, framed like a game screen.
static void draw_qr(const u8* qr, float x, float y, int k, u32 color) {
	int n = qrcodegen_getSize(qr);
	float size = (n + 8) * k;
	box_layers(x - 2 * u, y - 2 * u, size + 4 * u, size + 4 * u, color, COL_EDGE, true);
	gfx_rect(x, y, size, size, 0xffffffff);
	for (int my = 0; my < n; ++my) {
		for (int mx = 0; mx < n;) {
			int end = mx;
			while (end < n && qrcodegen_getModule(qr, end, my)) ++end;
			if (end > mx) {
				gfx_rect(x + (4 + mx) * k, y + (4 + my) * k, (end - mx) * k, k, 0x000000ff);
			}
			mx = end + 1;
		}
	}
}

// Each player's phone link as a QR code, encoded again only when the link changes.
static u8 qr_codes[MAX_PLAYERS][qrcodegen_BUFFER_LEN_FOR_VERSION(6)];
static char qr_urls[MAX_PLAYERS][64];

static void draw_phone(const struct player* p, float x, float y, float w, float h) {
	float pad = 6 * u, cx = x + w / 2;
	header(p, x + pad, y + pad, "PHONE", x + w - pad);
	const struct hint_def back = { &tex_circle, COL_BTN_CIRCLE, "BACK" };
	hint_row(x + w - pad, y + h - pad - 8 * ts, &back, 1);
	const char* url = web_urls[p->index];
	if (!url[0]) {
		text_centered(cx, y + h / 2 - 8 * tm, tm, "NO NETWORK", COL_TEXT);
		text_centered(cx, y + h / 2 + 4 * u, ts, "CONNECT THE PS3 TO YOUR WI-FI", COL_DIM);
		return;
	}
	u8* qr = qr_codes[p->index];
	if (strcmp(qr_urls[p->index], url) != 0) {
		u8 temp[sizeof(qr_codes[0])];
		// A link is under 64 bytes: version 6 always holds it.
		qrcodegen_encodeText(url, temp, qr, qrcodegen_Ecc_MEDIUM, 1, 6, qrcodegen_Mask_AUTO, true);
		snprintf(qr_urls[p->index], sizeof(qr_urls[0]), "%s", url);
	}
	// Whole screen pixels per module (whole art pixels when big enough) and a 4-module quiet
	// zone, framed like a game screen, with the link and a caption below, centered between the
	// header and the hints.
	float top = y + pad + 8 * tm + 4 * u + 6 * u, bottom = y + h - pad - 8 * ts - 6 * u;
	float caption_h = 6 * u + 2 * 8 * ts + 3 * u;
	int k = qr_scale(qr, fminf(bottom - top - caption_h - 4 * u, w - 2 * pad - 4 * u));
	float size = (qrcodegen_getSize(qr) + 8) * k;
	float qx = snap(cx - size / 2), qy = snap(top + (bottom - top - size - 4 * u - caption_h) / 2 + 2 * u);
	draw_qr(qr, qx, qy, k, player_colors[p->index]);
	char link[64];
	snprintf(link, sizeof(link), "%s", url);
	int size_link = fit_text(link, ts, w - 2 * pad);
	float ty = qy + size + 2 * u + 6 * u;
	text_centered(cx, ty, size_link, link, COL_TEXT);
	text_centered(cx, ty + 8 * ts + 3 * u, ts, "SCAN IT WITH YOUR PHONE", COL_DIM);
}

// Largest whole-number zoom of the game screen (sw x sh) that fits, keeping a margin for the frame.
static void game_rect(float x, float y, float w, float h, int sw, int sh, float* gx, float* gy, float* gw, float* gh) {
	float margin = 4 * u;
	int scale = fminf((w - 2 * margin) / sw, (h - 2 * margin) / sh);
	if (scale < 1) scale = 1;
	*gw = sw * scale;
	*gh = sh * scale;
	*gx = snap(x + (w - *gw) / 2);
	*gy = snap(y + (h - *gh) / 2);
}

static void draw_game(const struct player* p, float x, float y, float w, float h, bool dim, bool multi) {
	unsigned sw, sh; // GB/GBC: 160x144 in the top-left corner of the GBA-sized texture
	emu_screen_size(p, &sw, &sh);
	float gx, gy, gw, gh;
	game_rect(x, y, w, h, sw, sh, &gx, &gy, &gw, &gh);
	box_layers(gx - 2 * u, gy - 2 * u, gw + 4 * u, gh + 4 * u, player_colors[p->index], COL_EDGE, true);
	gfx_image_uv(&p->screen[p->screen_cur], gx, gy, gw, gh, 0, 0, (float) sw / GBA_SCREEN_W, (float) sh / GBA_SCREEN_H,
	             dim ? 0x605a80ff : 0xffffffff, false);
	if (multi && !dim) {
		// Beside the frame's top-left corner: split screens have spare room at the sides.
		float tab_w = gfx_text_width("P0", ts) + 4 * u;
		player_tab(p, gx - 2 * u - tab_w - 3 * u, gy - 2 * u, ts, NULL);
	}
}

static void draw_pause(const struct player* p, float x, float y, float w, float h, bool show_perf) {
	static const char* const labels[PAUSE_COUNT] = { "RESUME", "QUIT GAME", "LINK CABLE: OFF", "PERF: OFF" };
	bool link = emu_link_available(p); // GB/GBC games have no cable: no entry
	int rows = PAUSE_COUNT - !link;
	u32 color = player_colors[p->index];
	float inner = 6 * u, gap = 3 * u, cursor_w = tex_cursor.w * tm;
	float label_w = 0;
	for (int i = 0; i < PAUSE_COUNT; ++i) {
		if (i != PAUSE_LINK || link) label_w = fmaxf(label_w, gfx_text_width(labels[i], tm));
	}
	float row_h = 8 * tm + 4 * u;
	float title_h = 8 * tm + 6 * u;
	const struct hint_def legend[] = { { &tex_cross, COL_BTN_CROSS, "OK" }, { &tex_circle, COL_BTN_CIRCLE, "BACK" } };
	float legend_w = hints_width(legend, 2), legend_h = 8 * ts + 6 * u;
	float bw = snap(2 * inner + fmaxf(cursor_w + gap + label_w, legend_w));
	float bh = snap(2 * inner + title_h + rows * row_h - 4 * u + legend_h);
	float bx = snap(x + (w - bw) / 2), by = snap(y + (h - bh) / 2);
	box(bx, by, bw, bh, COL_PANEL, color);
	text_centered(bx + bw / 2, by + inner, tm, "PAUSED", color);
	for (int i = 0, row = 0; i < PAUSE_COUNT; ++i) {
		if (i == PAUSE_LINK && !link) continue;
		float ry = by + inner + title_h + row++ * row_h;
		char label[24];
		if (i == PAUSE_PERF) snprintf(label, sizeof(label), "PERF: %s", show_perf ? "ON" : "OFF");
		else if (i == PAUSE_LINK) snprintf(label, sizeof(label), "LINK CABLE: %s", p->link_wanted ? "ON" : "OFF");
		else snprintf(label, sizeof(label), "%s", labels[i]);
		if (i == p->pause_sel) {
			sprite(&tex_cursor, bx + inner, ry, tm, color);
		}
		text(bx + inner + cursor_w + gap, ry, tm, label, i == p->pause_sel ? COL_TEXT : COL_DIM);
	}
	hint_row(snap(bx + (bw + legend_w) / 2), by + bh - inner - 8 * ts, legend, 2);
}

// The game's frame rate first (red below 59), then its emulation time per frame: average, worst.
static void draw_perf(const struct player* p, float x, float y) {
	char s[32];
	snprintf(s, sizeof(s), "%.1fFPS %.1fMS %.1f", p->shown_fps, p->shown_avg / 1000.0, p->shown_max / 1000.0);
	u32 color = p->shown_fps > 0 && p->shown_fps < 59.0f ? 0xff6060ff : 0x7ae08aff;
	text(x, y, ts, s, color);
}

void ui_draw_slot(const struct player* p, float x, float y, float w, float h, int frame, bool show_perf, bool multi) {
	if (multi) {
		// Faint outline so each player's area reads as its own screen.
		u32 c = with_alpha(player_colors[p->index], 0x30);
		gfx_rect(x + u, y + u, w - 2 * u, u, c);
		gfx_rect(x + u, y + h - 2 * u, w - 2 * u, u, c);
		gfx_rect(x + u, y + 2 * u, u, h - 4 * u, c);
		gfx_rect(x + w - 2 * u, y + 2 * u, u, h - 4 * u, c);
	}
	switch (p->state) {
	case PS_EMPTY:
		draw_join(p, x, y, w, h, frame);
		break;
	case PS_GAMES:
		draw_games(p, x, y, w, h, frame);
		break;
	case PS_SAVES:
	case PS_LOADING:
	case PS_SAVE_MENU:
	case PS_SAVE_RENAME:
	case PS_SAVE_DELETE:
	case PS_SAVE_BUSY:
		draw_saves(p, x, y, w, h, frame, multi);
		break;
	case PS_PHONE:
		draw_phone(p, x, y, w, h);
		break;
	case PS_PLAYING:
		draw_game(p, x, y, w, h, false, multi);
		break;
	case PS_PAUSED:
		draw_game(p, x, y, w, h, true, multi);
		draw_pause(p, x, y, w, h, show_perf);
		break;
	}
	if (show_perf && p->core) {
		draw_perf(p, x + 4 * u, y + h - 4 * u - 8 * ts);
	}
}

// ---------------------------------------------------------------------------------------------
// Main menu and its pages: one screen for everyone, before the slots

// The tab and title at the top of a page, as in the slots but with the app's name on the tab.
static void menu_header(const char* title) {
	float pad = 6 * u, w = tab(pad, pad, tm, "ps3gba", COL_MENU);
	text(pad + w + 4 * u, pad + 2 * u, tm, title, COL_TEXT);
}

static void menu_legend(const struct gfx_tex* icon, u32 color, const char* label) {
	const struct hint_def h = { icon, color, label };
	hint_row(gfx_w - 6 * u, gfx_h - 6 * u - 8 * ts, &h, 1);
}

// The welcome screen's PS3, still juggling, over big entries in a box, all centered above the
// legend.
static void draw_main_menu(int sel, int frame) {
	static const char* const labels[MENU_ENTRIES] = { "PLAY", "ABOUT", "HELP", "QUIT" };
	menu_legend(&tex_cross, COL_BTN_CROSS, "OK");
	int k = 2; // UI units per art pixel of the logo
	float logo_w = ICON_W * k * u, logo_h = (ICON_H + 2) * k * u; // the floor spot overhangs 2 rows
	float inner = 6 * u, gap = 4 * u, cursor_w = tex_cursor.w * tl, row_h = 8 * tl + 6 * u, rows_w = 0;
	for (int i = 0; i < MENU_ENTRIES; ++i) {
		rows_w = fmaxf(rows_w, cursor_w + gap + gfx_text_width(labels[i], tl));
	}
	// The last row's empty bottom font row counts as padding: the same margin above and below.
	float bw = snap(fmaxf(2 * inner + rows_w, logo_w / 2)), bh = 2 * inner + MENU_ENTRIES * row_h - 6 * u - tl;
	float top = snap((gfx_h - 6 * u - 8 * ts - logo_h - 8 * u - bh) / 2);
	intro_draw_logo(snap((gfx_w - logo_w) / 2), top, k, frame);
	float bx = snap((gfx_w - bw) / 2), by = top + logo_h + 8 * u, rx = snap(bx + (bw - rows_w) / 2);
	box(bx, by, bw, bh, COL_PANEL, COL_MENU);
	for (int i = 0; i < MENU_ENTRIES; ++i) {
		float ry = by + inner + i * row_h;
		if (i == sel) {
			sprite(&tex_cursor, rx, ry, tl, COL_MENU);
		}
		text(rx + cursor_w + gap, ry, tl, labels[i], i == sel ? COL_TEXT : COL_DIM);
	}
}

// The About page: "#" starts a heading, "- " an item whose wrapped lines are indented.
static const char* const about_text[] = {
	"#ps3gba",
	ABOUT_SIGNATURE,
	"",
	"#MUSIC",
	"\"CHILL OVERWORLD\" BY ATMANAN (LOOPED AND RE-ENCODED)",
	"opengameart.org/content/chill-overworld",
	"LICENSE: CC-BY 4.0, creativecommons.org/licenses/by/4.0",
	"",
	"#THANKS TO",
	"- MGBA BY JEFFREY PFAU (ENDRIFT) AND ITS CONTRIBUTORS, THE EMULATOR AT THE HEART OF THIS APP "
	"(MPL-2.0, SOURCE AT github.com/mgba-emu/mgba)",
	"- PSL1GHT (MIT), PS3TOOLCHAIN AND PS3LIBRARIES BY PS3DEV",
	"- TINY3D BY HERMES (MIT)",
	"- CAIRO (LGPL-2.1 OR MPL-1.1), PIXMAN (MIT), LIBPNG AND ZLIB",
	"- FREETYPE (FTL): PORTIONS OF THIS SOFTWARE ARE COPYRIGHT (C) 2010 THE FREETYPE PROJECT "
	"(www.freetype.org). ALL RIGHTS RESERVED.",
	"- STB_VORBIS BY SEAN BARRETT (PUBLIC DOMAIN)",
	"- QR CODE GENERATOR BY PROJECT NAYUKI (MIT)",
	"- PRESS START 2P FONT BY CODEMAN38 (SIL OFL 1.1)",
	"- RPCS3, THE EMULATOR IT WAS TESTED ON",
	"- EVILNAT CFW, WEBMAN MOD AND PS3HEN, FOR OPENING UP THE PS3",
	"",
	"NOT AFFILIATED WITH SONY OR NINTENDO.",
};
#define ABOUT_COLS 64 // characters per line at most: short enough to read on a wide screen
#define ABOUT_MAX 96
static char about_lines[ABOUT_MAX][ABOUT_COLS + 2]; // wrapped once, at the first draw
static int about_count, about_cols;

// One line of the About page, wrapped between words (or through a word longer than a line).
static void about_wrap(const char* p) {
	int len = strlen(p), cols = p[0] == '#' ? len : about_cols, indent = 0; // headings stay whole
	do {
		int cut = len;
		if (len > cols) {
			for (cut = cols; cut > 0 && p[cut] != ' '; --cut) {
			}
			if (cut == 0) cut = cols;
		}
		if (about_count < ABOUT_MAX) {
			snprintf(about_lines[about_count++], sizeof(about_lines[0]), "%*s%.*s", indent, "", cut, p);
		}
		if (indent == 0 && strncmp(p, "- ", 2) == 0) {
			indent = 2;
			cols -= 2;
		}
		for (p += cut, len -= cut; *p == ' '; ++p, --len) {
		}
	} while (len > 0);
}

static void about_build(void) {
	about_cols = (gfx_w - 24 * u) / (8 * ts); // the panel's margins and padding
	if (about_cols > ABOUT_COLS) about_cols = ABOUT_COLS;
	for (size_t i = 0; i < sizeof(about_text) / sizeof(*about_text); ++i) {
		// UTF-8 to the font's Latin-1, anything beyond as '?'; then line by line.
		char buf[512];
		int n = 0;
		for (const u8* c = (const u8*) about_text[i]; *c && n < (int) sizeof(buf) - 1;) {
			u32 ch = *c++;
			if (ch >= 0xc0) {
				ch = (ch == 0xc2 || ch == 0xc3) && (*c & 0xc0) == 0x80 ? (ch & 3) << 6 | (*c & 0x3f) : '?';
				while ((*c & 0xc0) == 0x80) ++c;
			}
			buf[n++] = ch;
		}
		buf[n] = '\0';
		for (char* line = buf;;) {
			char* end = line + strcspn(line, "\n");
			bool last = !*end;
			*end = '\0';
			about_wrap(line);
			if (last) break;
			line = end + 1;
		}
	}
}

static float about_line_h(int i) {
	return about_lines[i][0] == '#' ? 8 * tm + 4 * u : 8 * ts + 3 * u;
}

// The credits in a panel between the header and the legend, scrolled by whole lines.
static void draw_about(int* scroll, int frame) {
	if (!about_count) {
		about_build();
	}
	menu_header("ABOUT");
	menu_legend(&tex_circle, COL_BTN_CIRCLE, "BACK");
	float inner = 6 * u, top = 6 * u + 8 * tm + 4 * u + 6 * u, bottom = gfx_h - 6 * u - 8 * ts - 6 * u;
	float pw = snap(2 * inner + about_cols * 8 * ts), px = snap((gfx_w - pw) / 2), view = bottom - top - 2 * inner;
	box(px, top, pw, bottom - top, COL_PANEL, COL_MENU);
	int first = about_count; // the furthest scroll: from there on, every line shows
	for (float fill = 0; first > 0 && fill + about_line_h(first - 1) <= view;) {
		fill += about_line_h(--first);
	}
	if (*scroll > first) *scroll = first;
	int i = *scroll;
	for (float y = top + inner; i < about_count && y + about_line_h(i) <= bottom - inner; y += about_line_h(i++)) {
		const char* s = about_lines[i] + (about_lines[i][0] == '#');
		int size = s == about_lines[i] ? ts : tm;
		gfx_text_shadow(px + inner, snap(y), size, s, s == about_lines[i] ? COL_TEXT : COL_MENU, COL_SHADOW);
	}
	// Blinking arrows in the panel's right margin when there is more above or below.
	if (frame / 24 % 2 == 0) {
		float ax = snap(px + pw - inner + (inner - u - tex_arrow.w * ts) / 2), ah = tex_arrow.h * ts;
		if (*scroll > 0) {
			gfx_image_uv(&tex_arrow, ax, top + inner, tex_arrow.w * ts, ah, 0, 1, 1, 0, COL_MENU, false);
		}
		if (i < about_count) {
			sprite(&tex_arrow, ax, bottom - inner - ah, ts, COL_MENU);
		}
	}
}

static u8 manual_qr[qrcodegen_BUFFER_LEN_FOR_VERSION(10)];
static int manual_qr_state; // 0: not encoded yet, 1: done, -1: the link doesn't fit

// The manual's QR code and link, then the essential controls, centered between the header and
// the legend.
static void draw_help(void) {
	static const struct hint_def controls[][2] = {
		{ { &tex_start, COL_BTN_PLAIN, "JOIN" }, { &tex_sticks, COL_BTN_PLAIN, "PAUSE A GAME" } },
		{ { &tex_cross, COL_BTN_CROSS, "OK / A" }, { &tex_circle, COL_BTN_CIRCLE, "BACK / B" } },
		{ { &tex_shoulders, COL_BTN_PLAIN, "L / R" }, { &tex_triangle, COL_BTN_TRIANGLE, "PHONE, SAVE OPTIONS" } },
	};
	int rows = sizeof(controls) / sizeof(*controls);
	if (!manual_qr_state) {
		u8 temp[sizeof(manual_qr)];
		manual_qr_state = qrcodegen_encodeText(MANUAL_URL, temp, manual_qr, qrcodegen_Ecc_MEDIUM, 1, 10,
		                                       qrcodegen_Mask_AUTO, true) ? 1 : -1;
	}
	menu_header("HELP");
	menu_legend(&tex_circle, COL_BTN_CIRCLE, "BACK");
	float cx = gfx_w / 2.0f, top = 6 * u + 8 * tm + 4 * u + 6 * u, bottom = gfx_h - 6 * u - 8 * ts - 6 * u;
	float row_h = 8 * ts + 4 * u, text_h = 6 * u + 2 * 8 * ts + 3 * u + 8 * u + rows * row_h - 4 * u;
	int k = manual_qr_state > 0 ? qr_scale(manual_qr, fminf(bottom - top - text_h - 4 * u, gfx_w - 16 * u)) : 0;
	float size = k ? (qrcodegen_getSize(manual_qr) + 8) * k + 4 * u : 0; // with its frame
	float y = snap(top + (bottom - top - size - text_h) / 2);
	if (k) {
		draw_qr(manual_qr, snap(cx - size / 2) + 2 * u, y + 2 * u, k, COL_MENU);
	}
	y += size + 6 * u;
	char link[sizeof(MANUAL_URL)] = MANUAL_URL;
	text_centered(cx, y, fit_text(link, ts, gfx_w - 12 * u), link, COL_TEXT);
	text_centered(cx, y + 8 * ts + 3 * u, ts, "SCAN IT FOR THE MANUAL", COL_DIM);
	y += 2 * 8 * ts + 3 * u + 8 * u;
	for (int r = 0; r < rows; ++r, y += row_h) {
		hint_row(snap(cx + hints_width(controls[r], 2) / 2), y, controls[r], 2);
	}
}

void ui_draw_menu(enum screen s, int sel, int* scroll, int frame) {
	switch (s) {
	case SCREEN_ABOUT:
		draw_about(scroll, frame);
		break;
	case SCREEN_HELP:
		draw_help();
		break;
	default:
		draw_main_menu(sel, frame);
		break;
	}
}
