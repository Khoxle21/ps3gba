#ifndef GFX_H
#define GFX_H

#include <stdbool.h>
#include <ppu-types.h>

// Screen size in pixels, and the pixel-art unit: one art pixel is gfx_unit screen pixels
// (4 at 1080p, 3 at 720p). Everything in the UI snaps to it so it stays crisp.
extern u32 gfx_w, gfx_h;
extern int gfx_unit;

enum gfx_format { GFX_RGB565, GFX_ARGB8888 };

struct gfx_tex {
	void* mem; // in RSX local memory (CPU-writable)
	u32 offset;
	u16 w, h;
	u32 pitch;
	enum gfx_format format;
	int slot; // pool slot for freeable textures, -1 for permanent ones
	const struct gfx_tex* sheet; // set: a sprite drawn from this sheet, at (sx, sy)
	u16 sx, sy;
};

void gfx_init(void);
void gfx_shutdown(void);
void gfx_begin(u32 rgb); // clears to a 0xRRGGBB color
void gfx_submit(void);   // starts the GPU on what has been drawn so far
void gfx_end(void);      // presents, paced to one frame per display refresh

// Permanent texture (emulator screens).
bool gfx_tex_create(struct gfx_tex* tex, int w, int h, enum gfx_format format);
// Freeable texture up to GFX_SLOT_SIZE x GFX_SLOT_SIZE ARGB (covers).
#define GFX_SLOT_SIZE 256
bool gfx_tex_create_slot(struct gfx_tex* tex, int w, int h);
void gfx_tex_free(struct gfx_tex* tex);
void gfx_tex_upload(struct gfx_tex* tex, const void* src, u32 src_pitch);
// Sprites (permanent, ARGB): packed into one shared sheet with the font, so drawing them
// doesn't break a draw call; drawn unsmoothed (smoothing would bleed in their neighbors).
bool gfx_tex_from_pixels(struct gfx_tex* tex, const void* argb, int w, int h, u32 src_pitch);
// From rows of characters, mapped through a palette string of "c=AARRGGBB" pairs, e.g.
// ". =00000000 #=ff101820". Unknown characters are transparent.
bool gfx_tex_from_art(struct gfx_tex* tex, const char* const* rows, const char* palette);

// Colors are 0xRRGGBBAA. Consecutive quads drawn from the same texture (the sheet: sprites, text,
// rectangles) go out as one draw call.
void gfx_rect(float x, float y, float w, float h, u32 rgba);
void gfx_image(const struct gfx_tex* tex, float x, float y, float w, float h, u32 tint, bool smooth);
void gfx_image_uv(const struct gfx_tex* tex, float x, float y, float w, float h,
                  float u0, float v0, float u1, float v1, u32 tint, bool smooth);

// Pixel font: glyphs are 8x8 art pixels, drawn at `size` screen pixels per art pixel.
void gfx_text(float x, float y, int size, const char* text, u32 rgba);
// Same, with a hard drop shadow one art pixel down-right.
void gfx_text_shadow(float x, float y, int size, const char* text, u32 rgba, u32 shadow);
int gfx_text_width(const char* text, int size);

// Dev builds: prints this second's drawing counters per frame (draw calls, quads, pixels filled,
// command buffer, RSX time, waits) and resets them.
void gfx_log_stats(int frames);

#endif
