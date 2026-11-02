// Welcome screen: the XMB icon (tools/gen_art.py, icon()) comes alive. The fat PS3 juggles the
// four GBAs under its spotlight until someone presses Start or Cross, then an iris closes on its
// wink and opens on the main menu, where the scene goes on as a logo. Positions are art pixels
// on the icon's canvas, one art pixel being K UI units (gfx_unit): the still parts sit on that
// grid, moving sprites snap to units.
// The whole scene is one draw call: sprites come from one sheet, the rest is its white pixel tinted.
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "art.h"
#include "gfx.h"
#include "intro.h"

#define K 3 // UI units per art pixel (an integer: the moving sprites stay on the still parts' grid)

// The juggle is a shower: the left hand throws high over the PS3 to the right hand, which lobs
// it back over its head. Each hand throws every SLOT frames.
#define SLOT 24
#define DWELL 8                 // frames a hand carries a GBA
#define HIGH 70                 // frames of the high throw
#define CYCLE (4 * SLOT)        // one GBA's round trip
#define CAUGHT (DWELL + HIGH)   // end of the high throw in the round trip
#define LOBBED (CAUGHT + DWELL) // start of the lob back
// Glove anchors (as in the icon) where the hands catch and throw.
static const float L_IN[2] = { 17, 25 }, L_OUT[2] = { 9, 27 }, R_OUT[2] = { 70, 27 }, R_IN[2] = { 62, 25 };

// Timeline in frames: a fade in at start; after leaving, the iris closes on the face, holds for
// the wink, shuts, stays black, then opens on the main menu.
#define FADE 16
#define IRIS_FACE 20
#define WINK 26
#define IRIS_SHUT 42
#define DARK 50
#define OPEN 56
#define DONE 80
#define FACE_X 40.0f // iris center and radius around the face, in art pixels
#define FACE_Y 33.5f
#define FACE_R 11.0f

// Under PRESS START, one at a time: typed in like a game's text box (2 frames a letter), held,
// erased (a letter a frame), then a short blank.
static const char* const quotes[] = {
	"A NOSTALGIA TRIP WORTH PLAYING!",
	"TIME TRAVEL BACK TO 2004 :)",
	"NO AA BATTERIES REQUIRED!",
	"BACKLIGHT INCLUDED THIS TIME!",
	"NO LINK CABLE? NO PROBLEM!",
	"BLOW ON THE CARTRIDGE, JUST IN CASE!",
	"COUCH CO-OP NEVER GETS OLD!",
	"PIXELS NEVER AGE!",
	"YOUR CHILDHOOD CALLED: IT WANTS TO PLAY!",
};
#define QUOTE_PERIOD 330 // frames per quote
#define QUOTE_GAP 18     // blank frames before the next
#define PROMPT_Y 3.5f    // PRESS START's top, in art pixels below the canvas
#define QUOTE_SPACE 6    // UI units between PRESS START and the quote

static struct gfx_tex sheet;
static int u, S;     // UI unit and art pixel, in screen pixels
static int ox, oy;   // screen position of the canvas
static int t;        // frames since start
static int out = -1; // frames since leaving
static bool held;    // dev: frozen on one frame
static int prompt_size, quote_size; // ui.c's large and medium text

void intro_init(void) {
	u = gfx_unit;
	prompt_size = u >= 4 ? 4 : u >= 3 ? 3 : 2;
	quote_size = u >= 4 ? 3 : 2;
	int w = gfx_w, h = gfx_h, k = K;
	if (k > w / (ICON_W * u)) k = w / (ICON_W * u);
	if (k > h / (ICON_H * u)) k = h / (ICON_H * u);
	S = (k > 0 ? k : 1) * u;
	ox = (w - ICON_W * S) / 2 / u * u;
	// The scene, PRESS START and the quote under it, centered as one block.
	float block = (ICON_H + PROMPT_Y) * S + 8 * prompt_size + QUOTE_SPACE * u + 8 * quote_size;
	oy = (int) ((h - block) / 2) / u * u;
	gfx_tex_from_art(&sheet, art_intro, art_intro_palette);
}

bool intro_playing(void) {
	return out < OPEN;
}

void intro_leave(void) {
	if (out < 0) {
		out = 0;
	}
}

void intro_skip(void) {
	if (out < DONE) {
		out = DONE;
		printf("intro: skipped\n");
	}
}

void intro_seek(int frame, int leaving) {
	t = frame;
	out = leaving;
	held = true;
}

static void quad_uv(float x, float y, float w, float h, float u0, float v0, float u1, float v1, u32 rgba) {
	if (w > 0 && h > 0) {
		gfx_image_uv(&sheet, x, y, w, h, u0, v0, u1, v1, rgba, false);
	}
}

static void quad(float x, float y, float w, float h, u32 rgba) {
	const unsigned char* r = art_intro_rects[INTRO_PIXEL];
	float pu = (r[0] + 0.5f) / sheet.w, pv = (r[1] + 0.5f) / sheet.h;
	quad_uv(x, y, w, h, pu, pv, pu, pv, rgba);
}

// One art pixel of the canvas grid, color 0xRRGGBB.
static void dot(int x, int y, u32 rgb) {
	quad(ox + x * S, oy + y * S, S, S, rgb << 8 | 0xff);
}

static float snap(float v) {
	return floorf(v / u + 0.5f) * u;
}

static void sprite_px(int id, float x, float y, int scale, u32 tint) {
	const unsigned char* r = art_intro_rects[id];
	quad_uv(x, y, r[2] * scale, r[3] * scale, (float) r[0] / sheet.w, (float) r[1] / sheet.h,
	        (float) (r[0] + r[2]) / sheet.w, (float) (r[1] + r[3]) / sheet.h, tint);
}

// A sprite at art pixel coordinates, snapped to UI units.
static void sprite(int id, float x, float y) {
	sprite_px(id, ox + snap(x * S), oy + snap(y * S), S, 0xffffffff);
}

// 0xRRGGBB blend, rounded half to even like gen_art.py's mix().
static u32 mix(u32 a, u32 b, double f) {
	u32 c = 0;
	for (int s = 0; s < 24; s += 8) {
		double x = (a >> s) & 0xff, y = (b >> s) & 0xff;
		c |= (u32) rint(x + (y - x) * f) << s;
	}
	return c;
}

// Columns of row y whose center is within `half` of the canvas center, as in the icon.
static void span(int y, float half, u32 rgb) {
	int x0 = floorf(ICON_W / 2 - 0.5f - half) + 1, x1 = ceilf(ICON_W / 2 - 0.5f + half) - 1;
	quad(ox + x0 * S, oy + y * S, (x1 - x0 + 1) * S, S, rgb << 8 | 0xff);
}

// The spotlight's lit spot on the floor, row y.
static void floor_spot(int y) {
	float d = (y + 0.5f - 43) / 2.6f;
	if (d * d < 1) {
		span(y, 26 * sqrtf(1 - d * d), mix(ICON_SKY_BOTTOM, ICON_SPOT, 0.35));
	}
}

// The icon's banded sky, spotlight cone, floor spot and stars, extended to the whole screen.
static void draw_sky(void) {
	int top = floorf(-oy / (float) S), bottom = ceilf((gfx_h - oy) / (float) S);
	int left = floorf(-ox / (float) S), right = ceilf((gfx_w - ox) / (float) S);
	for (int y = top; y < bottom; ++y) {
		int row = y < 0 ? 0 : y >= ICON_H ? ICON_H - 1 : y;
		u32 base = mix(ICON_SKY_TOP, ICON_SKY_BOTTOM, rint(row * 4.0 / (ICON_H - 1)) / 4);
		quad(ox + left * S, oy + y * S, (right - left) * S, S, base << 8 | 0xff);
		float half = 8 + y * 0.45f;
		if (y < ICON_H && half > 0) { // the cone stops at the floor
			span(y, half, mix(base, ICON_SPOT, 0.18));
		}
		floor_spot(y);
		for (int x = left; x < right; ++x) { // more stars around the canvas
			unsigned hash = (x * 73856093u) ^ (y * 19349663u);
			bool outside = (x < 0 || x >= ICON_W || y < 0) && y < ICON_H; // none behind the prompt
			if (outside && hash % 61 == 0 && (t + hash) % 140 >= 10) {
				dot(x, y, ICON_STAR);
			}
		}
	}
	for (int i = 0; i < ICON_STARS; ++i) {
		if ((t + i * 37) % 140 >= 10) { // twinkle
			dot(art_icon_stars[i][0], art_icon_stars[i][1], ICON_STAR);
		}
	}
}

// Silver arm from the shoulder to the hand: the icon's sagging curve, on the art grid.
static void arm(int sx, int sy, const float* hand) {
	float cx = (sx + hand[0]) / 2, cy = sy + 3;
	int lx = -1000, ly = -1000;
	for (int k = 0; k <= 40; ++k) {
		float s = k / 40.0f;
		int x = rintf((1 - s) * (1 - s) * sx + 2 * (1 - s) * s * cx + s * s * hand[0]);
		int y = rintf((1 - s) * (1 - s) * sy + 2 * (1 - s) * s * cy + s * s * hand[1]);
		if (x != lx || y != ly) {
			dot(x, y, ICON_SILVER);
			dot(x, y + 1, ICON_INK);
		}
		lx = x;
		ly = y;
	}
}

// From a to b at s (0-1), raised by `lift` art pixels in the middle (a parabola).
static void arc(const float* a, const float* b, float s, float lift, float* p) {
	p[0] = a[0] + (b[0] - a[0]) * s;
	p[1] = a[1] + (b[1] - a[1]) * s - 4 * lift * s * (1 - s);
}

static void draw_scene(void) {
	// Each hand carries a GBA for DWELL frames (dipping), then swings back for the next one.
	float left[2], right[2];
	int q = t % SLOT;
	if (q < DWELL) arc(L_IN, L_OUT, (float) q / DWELL, -2, left);
	else arc(L_OUT, L_IN, (float) (q - DWELL) / (SLOT - DWELL), 2, left);
	q = (t + CYCLE - CAUGHT % SLOT) % SLOT;
	if (q < DWELL) arc(R_OUT, R_IN, (float) q / DWELL, -2, right);
	else arc(R_IN, R_OUT, (float) (q - DWELL) / (SLOT - DWELL), 2, right);
	arm(ICON_PS3_X, ICON_PS3_Y + 8, left);
	arm(ICON_PS3_X + 33, ICON_PS3_Y + 8, right);
	sprite(INTRO_PS3, ICON_PS3_X, ICON_PS3_Y);

	float gba[4][2], watch = FACE_X;
	bool falling[4];
	int newest = CYCLE;
	for (int i = 0; i < 4; ++i) {
		int p = (t + SLOT * i) % CYCLE;
		float* g = gba[i];
		falling[i] = false;
		if (p < DWELL) {
			g[0] = left[0], g[1] = left[1];
		} else if (p < CAUGHT) {
			float s = (float) (p - DWELL) / (CAUGHT - DWELL);
			arc(L_OUT, R_OUT, s, 16, g);
			falling[i] = s > 0.5f;
		} else if (p < LOBBED) {
			g[0] = right[0], g[1] = right[1];
		} else {
			float s = (float) (p - LOBBED) / (CYCLE - LOBBED);
			arc(R_IN, L_IN, s, 4, g);
			falling[i] = s > 0.5f;
			if (p < newest) { // the eyes follow the latest lob over the head
				newest = p;
				watch = g[0];
			}
		}
	}

	// Eyes: a blink now and then; after leaving, a look at the viewer and a wink.
	for (int e = 0; e < 2; ++e) {
		int ex = ICON_PS3_X + 11 + 8 * e, ey = ICON_PS3_Y + 4;
		if (out >= WINK && e == 1) {
			sprite(INTRO_EYE_WINK, ex, ey);
		} else if (out < 0 && t % 150 < 6) {
			sprite(INTRO_EYE_BLINK, ex, ey);
		} else {
			int look = out >= 0 ? 1 : watch < FACE_X - 6 ? 0 : watch > FACE_X + 6 ? 2 : 1;
			quad(ox + (ex + look) * S, oy + (ey + 1) * S, 2 * S, 2 * S, ICON_INK << 8 | 0xff);
		}
	}
	// Sweat: a drop flies off a top corner every 16 frames, alternating sides.
	for (int age = t % 16; age < 44 && age <= t; age += 16) {
		int side = (t - age) / 16 % 2;
		float x = ICON_PS3_X + (side ? 28 : 1) + (side ? 0.22f : -0.22f) * age;
		sprite(INTRO_DROP, x, ICON_PS3_Y - 1 - 0.3f * age + 0.015f * age * age);
	}
	for (int pass = 0; pass < 2; ++pass) { // the lob back passes behind the high throws
		for (int i = 0; i < 4; ++i) {
			if (((t + SLOT * i) % CYCLE >= LOBBED) != pass) {
				sprite(INTRO_GBA1_UP + 2 * i + falling[i], gba[i][0] - 9, gba[i][1] - 10);
			}
		}
	}
	sprite(INTRO_GLOVE, left[0] - 2, left[1] - 1);
	sprite(INTRO_GLOVE, right[0] - 2, right[1] - 1);
	sprite(INTRO_LABEL_PS3, 3, 37);
	sprite(INTRO_LABEL_GBA, 66, 37);
}

// Black outside a circle (screen pixels), in rows one UI unit tall.
static void iris(float cx, float cy, float r) {
	cx = snap(cx);
	int y0 = snap(cy - r), y1 = snap(cy + r);
	if (y0 < 0) y0 = 0;
	if (y1 > (int) gfx_h) y1 = gfx_h;
	if (r <= 0 || y1 <= y0) {
		quad(0, 0, gfx_w, gfx_h, 0x000000ff);
		return;
	}
	quad(0, 0, gfx_w, y0, 0x000000ff);
	quad(0, y1, gfx_w, gfx_h - y1, 0x000000ff);
	for (int y = y0; y < y1; y += u) {
		float dy = y + u / 2.0f - cy, half = r * r > dy * dy ? snap(sqrtf(r * r - dy * dy)) : 0;
		quad(0, y, cx - half, u, 0x000000ff);
		quad(cx + half, y, gfx_w - cx - half, u, 0x000000ff);
	}
}

void intro_draw_logo(float x, float y, int k, int frame) {
	// The scene runs on the welcome screen's state: borrowed for this one draw.
	int saved[] = { ox, oy, S, t, out };
	ox = x, oy = y, S = k * u, t = frame, out = -1;
	for (int row = ICON_H - 4; row < ICON_H + 3; ++row) { // the stage, without the sky around it
		floor_spot(row);
	}
	draw_scene();
	ox = saved[0], oy = saved[1], S = saved[2], t = saved[3], out = saved[4];
}

void intro_iris(float cx, float cy, float r) {
	iris(cx, cy, r);
}

// Bottom right, barely visible: who made it and the version (CHANGELOG.md's newest, via the Makefile).
static void draw_credit(void) {
	const char* credit = "DEVELOPED BY KHOXLE - V" APP_VERSION;
	int size = u >= 3 ? 2 : 1; // ui.c's small text
	float x = gfx_w - 6 * u - gfx_text_width(credit, size), y = gfx_h - 6 * u - 8 * size;
	gfx_text(x, y, size, credit, 0x4a4378ff);
}

// The current quote at y, centered on its full width (typing doesn't shift it).
static void draw_quote(float y) {
	if (t < FADE) {
		return;
	}
	int k = t - FADE, local = k % QUOTE_PERIOD;
	const char* q = quotes[k / QUOTE_PERIOD % (sizeof(quotes) / sizeof(*quotes))];
	int len = strlen(q), shown = local / 2;
	if (shown > QUOTE_PERIOD - QUOTE_GAP - local) shown = QUOTE_PERIOD - QUOTE_GAP - local;
	if (shown > len) shown = len;
	if (shown <= 0) {
		return;
	}
	char typed[64];
	snprintf(typed, sizeof(typed), "%.*s", shown, q);
	float x = snap((gfx_w - gfx_text_width(q, quote_size)) / 2);
	gfx_text_shadow(x, y, quote_size, typed, 0xb4a8ffff, 0x0b0820ff); // the icon's "GBA" label color
}

void intro_draw(void) {
	if (out >= DONE) {
		return;
	}
	// Prompt in the style of ui.c's button hints, blinking like the join screen's.
	const char* label = "PRESS START";
	int size = prompt_size;
	float pw = (7 + 4) * size + gfx_text_width(label, size);
	float px = snap((gfx_w - pw) / 2), py = snap(oy + (ICON_H + PROMPT_Y) * S);
	bool prompt = out < 0 && t >= FADE && t / 30 % 2 == 0;
	if (intro_playing()) {
		draw_sky();
		draw_scene();
		draw_quote(py + 8 * size + QUOTE_SPACE * u); // these two under the fade and the iris
		draw_credit();
		if (prompt) {
			sprite_px(INTRO_CROSS, px, py, size, 0x7aa7ffff);
		}
	}
	float corner = hypotf(gfx_w, gfx_h) / 2;
	if (out < 0) {
		if (t < FADE) { // stepped, like old consoles
			quad(0, 0, gfx_w, gfx_h, t < 4 ? 0x000000ff : (4 - t / 4) * 64);
		}
	} else if (out < OPEN) {
		float fx = ox + FACE_X * S, fy = oy + FACE_Y * S, face = FACE_R * S;
		float far = hypotf(fx > gfx_w / 2 ? fx : gfx_w - fx, fy > gfx_h / 2 ? fy : gfx_h - fy);
		float s = out < IRIS_FACE ? 1 - (float) out / IRIS_FACE : 0;
		float shut = out < IRIS_SHUT ? 0 : (float) (out - IRIS_SHUT) / (DARK - IRIS_SHUT);
		iris(fx, fy, out >= DARK ? 0 : face + (far - face) * s * s - face * shut * shut);
	} else {
		float s = (float) (out - OPEN) / (DONE - OPEN);
		iris(gfx_w / 2.0f, gfx_h / 2.0f, corner * s * s);
	}
	if (prompt) {
		gfx_text_shadow(px + (7 + 4) * size, py, size, label, 0x9a92c8ff, 0x0b0820ff);
	}
	if (!held) {
		++t;
		if (out >= 0 && ++out == DONE) {
			printf("intro: done\n");
		}
	}
}
