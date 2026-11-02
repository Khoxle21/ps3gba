// GPU drawing on top of Tiny3D: 2D quads in screen pixels, textures in RSX memory and an
// 8x8 pixel font rendered once from the embedded TTF.
//
// Quads are batched automatically: consecutive quads that sample the same texture the same way
// go out as one draw call. The font, a white texel (solid rectangles) and every sprite built from
// art share one sheet, so whole menus are a single draw call; only game screens and covers have
// textures of their own.
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <rsx/gcm_sys.h>
#include <rsx/rsx.h>
#include <sys/systime.h>
#include <tiny3d.h>

#include <cairo/cairo.h>
#include <cairo/cairo-ft.h>
#include <ft2build.h>
#include FT_FREETYPE_H

#include "gfx.h"
#include "pressstart2p_ttf.h"

#define Z 1000.0f // constant depth: with LEQUAL testing, later draws land on top
#define POOL_SLOTS 256
#define SLOT_BYTES (GFX_SLOT_SIZE * GFX_SLOT_SIZE * 4)
#define SHEET_W 512
#define SHEET_H 256
#define FONT_FIRST 32 // the font's glyphs are Latin-1 32-255 in a 16x14 grid of 8x8 cells

u32 gfx_w, gfx_h;
int gfx_unit;

static u8* slot_pool;
static bool slot_used[POOL_SLOTS];
static struct gfx_tex sheet, font, white;
static int sheet_x, sheet_y, shelf_h; // shelf packing: next free spot, current shelf's height

// The open draw call.
static const struct gfx_tex* batch_tex;
static bool batch_smooth, batch_open;

#ifdef DEV_TOOLS
// Per-second sums for the dev log (gfx_log_stats). Counts don't depend on the platform, unlike
// RPCS3's timings: draw calls, texture binds, shader switches (Tiny3D reloads its shader state
// when a draw goes from solid to textured or back), quads, pixels filled, command bytes.
static struct {
	u32 draws, binds, switches, quads, cmd_bytes, jumps, flip_jumps, rsx_frames;
	double fill;
	s64 blocked, flip_blocked, vsync, upload; // microseconds
	u64 rsx, rsx_max;                         // nanoseconds
} st;
static int last_textured = -1, batch_quads;
static bool in_flip;
static u32 frame_no, cmd_mark;
#define STAT(x) x
#define TIMESTAMP_BASE 1000 // RSX report slots for the frame start/end timestamps

// Has the RSX write a report with its timestamp (read back with gcmGetTimeStamp): a zpass pixel
// count report, the standard occlusion query, whose count is ignored. Not PSL1GHT's
// rsxSetTimeStamp: it writes its argument one word too far, leaving the RSX a stale word as the
// report's type and offset (RPCS3 died on a report out of range).
static void timestamp(u32 index) {
	tiny3d_DoCmd_Space(2);
	u32* fields = (u32*) tiny3d_Get_GCM_Context(); // begin, end, current, callback
	u32* cmd = (u32*) (u64) fields[2];
	cmd[0] = 1 << 18 | 0x1800;     // NV4097_GET_REPORT, one argument
	cmd[1] = 1 << 24 | index << 4; // CELL_GCM_ZPASS_PIXEL_CNT, report slot
	fields[2] += 8;
}

static void count_fill(float x, float y, float w, float h) {
	float x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
	float x1 = x + w > gfx_w ? gfx_w : x + w, y1 = y + h > gfx_h ? gfx_h : y + h;
	if (x1 > x0 && y1 > y0) st.fill += (x1 - x0) * (y1 - y0);
}

// The command buffer's callback: the system's moves to the next 32 KB segment of the buffer and
// waits there until the RSX has left it. Wrapped to count those jumps and the time blocked.
s32 rsxContextCallback(gcmContextData* context, u32 count);
static u32 sys_callback, hook_opd[2]; // 32-bit function descriptors (entry, TOC), as gcm calls them

static s32 cmd_hook(gcmContextData* ctx, u32 count) {
	s64 t = sysGetSystemTime();
	u32* fields = (u32*) ctx; // begin, end, current, callback: 32-bit pointers
	st.cmd_bytes += fields[2] - cmd_mark;
	fields[3] = sys_callback;
	s32 r = rsxContextCallback(ctx, count);
	fields[3] = (u32) (u64) hook_opd;
	cmd_mark = fields[2];
	t = sysGetSystemTime() - t;
	if (in_flip) {
		st.flip_jumps++;
		st.flip_blocked += t;
	} else {
		st.jumps++;
		st.blocked += t;
	}
	return r;
}

static void hook_callback(void) {
	u32* fields = (u32*) tiny3d_Get_GCM_Context();
	const u64* fd = (const u64*) (void*) cmd_hook; // ELFv1 descriptor: entry, TOC, environment
	hook_opd[0] = fd[0];
	hook_opd[1] = fd[1];
	sys_callback = fields[3];
	fields[3] = (u32) (u64) hook_opd;
	cmd_mark = fields[2];
}

void gfx_log_stats(int frames) {
	if (frames <= 0) {
		return;
	}
	float n = frames;
	printf("gfx (per frame): %.1f draws, %.1f binds, %.1f shader switches, %.0f quads, %.2f Mpx filled, "
	       "%.1f KB commands, %.2f+%.2f segment jumps (%.2f+%.2f ms blocked), rsx %.2f ms (max %.2f), "
	       "vsync wait %.2f ms, upload %.2f ms\n",
	       st.draws / n, st.binds / n, st.switches / n, st.quads / n, st.fill / n / 1e6,
	       st.cmd_bytes / n / 1024, st.jumps / n, st.flip_jumps / n, st.blocked / n / 1000, st.flip_blocked / n / 1000,
	       st.rsx_frames ? st.rsx / 1e6 / st.rsx_frames : 0, st.rsx_max / 1e6, st.vsync / n / 1000, st.upload / n / 1000);
	memset(&st, 0, sizeof(st));
}
#else
#define STAT(x)
#endif

static u32 bytes_per_pixel(enum gfx_format f) {
	return f == GFX_RGB565 ? 2 : 4;
}

static text_format tiny_format(enum gfx_format f) {
	return f == GFX_RGB565 ? TINY3D_TEX_FORMAT_R5G6B5 : TINY3D_TEX_FORMAT_A8R8G8B8;
}

bool gfx_tex_create(struct gfx_tex* tex, int w, int h, enum gfx_format format) {
	memset(tex, 0, sizeof(*tex));
	tex->pitch = (w * bytes_per_pixel(format) + 63) & ~63u;
	tex->mem = tiny3d_AllocTexture(tex->pitch * h);
	if (!tex->mem) {
		return false;
	}
	tex->offset = tiny3d_TextureOffset(tex->mem);
	tex->w = w;
	tex->h = h;
	tex->format = format;
	tex->slot = -1;
	memset(tex->mem, 0, tex->pitch * h);
	return true;
}

bool gfx_tex_create_slot(struct gfx_tex* tex, int w, int h) {
	if (w > GFX_SLOT_SIZE || h > GFX_SLOT_SIZE) {
		return false;
	}
	for (int i = 0; i < POOL_SLOTS; ++i) {
		if (!slot_used[i]) {
			slot_used[i] = true;
			memset(tex, 0, sizeof(*tex));
			tex->mem = slot_pool + (size_t) i * SLOT_BYTES;
			tex->offset = tiny3d_TextureOffset(tex->mem);
			tex->w = w;
			tex->h = h;
			tex->pitch = GFX_SLOT_SIZE * 4; // the RSX needs aligned row pitches
			tex->format = GFX_ARGB8888;
			tex->slot = i;
			return true;
		}
	}
	return false;
}

void gfx_tex_free(struct gfx_tex* tex) {
	if (tex->slot >= 0) {
		slot_used[tex->slot] = false;
	}
	memset(tex, 0, sizeof(*tex));
	tex->slot = -1;
}

void gfx_tex_upload(struct gfx_tex* tex, const void* src, u32 src_pitch) {
	STAT(s64 t = sysGetSystemTime());
	u32 row = tex->w * bytes_per_pixel(tex->format);
	for (int y = 0; y < tex->h; ++y) {
		memcpy((u8*) tex->mem + y * tex->pitch, (const u8*) src + y * src_pitch, row);
	}
	STAT(st.upload += sysGetSystemTime() - t);
}

bool gfx_tex_from_pixels(struct gfx_tex* tex, const void* argb, int w, int h, u32 src_pitch) {
	if (sheet_x + w > SHEET_W) { // next shelf
		sheet_x = 0;
		sheet_y += shelf_h;
		shelf_h = 0;
	}
	if (w > SHEET_W || sheet_y + h > SHEET_H) { // full: a texture of its own
		if (!gfx_tex_create(tex, w, h, GFX_ARGB8888)) {
			return false;
		}
		gfx_tex_upload(tex, argb, src_pitch);
		return true;
	}
	memset(tex, 0, sizeof(*tex));
	tex->w = w;
	tex->h = h;
	tex->format = GFX_ARGB8888;
	tex->slot = -1;
	tex->sheet = &sheet;
	tex->sx = sheet_x;
	tex->sy = sheet_y;
	for (int y = 0; y < h; ++y) {
		memcpy((u8*) sheet.mem + (sheet_y + y) * sheet.pitch + sheet_x * 4, (const u8*) argb + y * src_pitch, w * 4);
	}
	sheet_x += w;
	if (h > shelf_h) shelf_h = h;
	return true;
}

bool gfx_tex_from_art(struct gfx_tex* tex, const char* const* rows, const char* palette) {
	int h = 0, w = 0;
	for (; rows[h]; ++h) {
		int len = strlen(rows[h]);
		if (len > w) w = len;
	}
	u32 colors[256] = { 0 };
	for (const char* p = palette; p[0] && p[1] == '='; p += 11) {
		colors[(u8) p[0]] = strtoul(p + 2, NULL, 16);
		if (!p[10]) break;
	}
	u32* pixels = calloc(w * h, 4);
	for (int y = 0; y < h; ++y) {
		for (int x = 0; rows[y][x]; ++x) {
			pixels[y * w + x] = colors[(u8) rows[y][x]];
		}
	}
	bool ok = gfx_tex_from_pixels(tex, pixels, w, h, w * 4);
	free(pixels);
	return ok;
}

// Rasterizes Latin-1 glyphs 32-255 into a 16x14 grid of 8x8 cells, white with alpha.
static void build_font(void) {
	const int cols = 16, rows = 14, cell = 8;
	FT_Library ft;
	FT_Face face;
	FT_Init_FreeType(&ft);
	FT_New_Memory_Face(ft, pressstart2p_ttf, pressstart2p_ttf_size, 0, &face);

	cairo_surface_t* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, cols * cell, rows * cell);
	cairo_t* cr = cairo_create(surface);
	cairo_font_face_t* cface = cairo_ft_font_face_create_for_ft_face(face, 0);
	cairo_set_font_face(cr, cface);
	cairo_set_font_size(cr, cell);
	cairo_font_options_t* opts = cairo_font_options_create();
	cairo_font_options_set_antialias(opts, CAIRO_ANTIALIAS_NONE);
	cairo_font_options_set_hint_style(opts, CAIRO_HINT_STYLE_NONE);
	cairo_set_font_options(cr, opts);
	cairo_set_source_rgba(cr, 1, 1, 1, 1);

	cairo_font_extents_t ext;
	cairo_font_extents(cr, &ext);
	for (int c = FONT_FIRST; c < 256; ++c) {
		// Latin-1 code point as UTF-8
		char utf8[3] = { 0 };
		if (c < 0x80) {
			utf8[0] = c;
		} else {
			utf8[0] = 0xC0 | (c >> 6);
			utf8[1] = 0x80 | (c & 0x3F);
		}
		int i = c - FONT_FIRST;
		cairo_move_to(cr, (i % cols) * cell, (i / cols) * cell + (int) (ext.ascent + 0.5));
		cairo_show_text(cr, utf8);
	}
	cairo_surface_flush(surface);

	// cairo's ARGB32 is native-endian u32, which on the PPU matches the RSX's A8R8G8B8.
	gfx_tex_from_pixels(&font, cairo_image_surface_get_data(surface), cols * cell, rows * cell,
	                    cairo_image_surface_get_stride(surface));

	cairo_font_options_destroy(opts);
	cairo_destroy(cr);
	cairo_surface_destroy(surface);
	cairo_font_face_destroy(cface);
	FT_Done_Face(face);
	FT_Done_FreeType(ft);
}

void gfx_init(void) {
	// Tiny3D's command buffer is 64 KB, two 32 KB segments: a frame's commands passing into the
	// segment the RSX still reads (waiting for the display refresh to flip the previous frame)
	// blocked the CPU until that refresh, in the middle of drawing. Set up gcm with a 1 MB buffer
	// first (Tiny3D's init keeps it): 32 segments, the RSX is never that far behind. The I/O size
	// is the one tiny3d_Init(2 MB) would map.
	extern void* tiny_host_addr; // Tiny3D's own globals (rsxutil.c)
	extern gcmContextData* context;
	const u32 io_size = 4 * 1024 * 1024, cmd_size = 1024 * 1024;
	tiny_host_addr = memalign(1024 * 1024, io_size);
	gcmInitBody(&context, cmd_size, io_size, tiny_host_addr);
	// 1 MB of vertices per frame (the size Tiny3D samples use), doubled by our ping-pong patch.
	tiny3d_Init(2 * 1024 * 1024);
	gfx_w = Video_Resolution.width;
	gfx_h = Video_Resolution.height;
	gfx_unit = (gfx_h + 135) / 270; // 1080p: 4, 720p: 3, 480/576p: 2
	if (gfx_unit < 1) gfx_unit = 1;
	// 2D coordinates in screen pixels.
	tiny3d_UserViewport(1, 0.0f, 0.0f, 1.0f, 1.0f, gfx_w / 1920.0f, gfx_h / 1080.0f);
	slot_pool = tiny3d_AllocTexture(POOL_SLOTS * SLOT_BYTES);
	gfx_tex_create(&sheet, SHEET_W, SHEET_H, GFX_ARGB8888);
	build_font();
	const u32 opaque_white = 0xffffffff;
	gfx_tex_from_pixels(&white, &opaque_white, 1, 1, 4);
	STAT(hook_callback());
}

void gfx_shutdown(void) {
	tiny3d_Exit();
}

void gfx_begin(u32 rgb) {
	STAT(last_textured = -1); // Tiny3D's flip resets its shader state
	STAT(timestamp(TIMESTAMP_BASE + 2 * (frame_no & 1)));
	tiny3d_Clear(0xff000000 | rgb, TINY3D_CLEAR_COLOR);
	tiny3d_AlphaTest(0, 0, TINY3D_ALPHA_FUNC_ALWAYS);
	tiny3d_BlendFunc(1, TINY3D_BLEND_FUNC_SRC_RGB_SRC_ALPHA | TINY3D_BLEND_FUNC_SRC_ALPHA_SRC_ALPHA,
	                 TINY3D_BLEND_FUNC_DST_RGB_ONE_MINUS_SRC_ALPHA | TINY3D_BLEND_FUNC_DST_ALPHA_ZERO,
	                 TINY3D_BLEND_RGB_FUNC_ADD | TINY3D_BLEND_ALPHA_FUNC_ADD);
	tiny3d_Project2D();
	// 2D layers simply draw in order. Tiny3D's depth test reads and writes the depth buffer
	// for every pixel, which alone pushed the RSX past the frame budget at 1080p.
	gcmContextData* ctx = tiny3d_Get_GCM_Context();
	tiny3d_DoCmd_Space(64);
	rsxSetDepthTestEnable(ctx, GCM_FALSE);
	rsxSetDepthWriteEnable(ctx, GCM_FALSE);
}

static void flush(void) {
	if (batch_open) {
		tiny3d_End();
		batch_open = false;
#ifdef DEV_TOOLS
		st.draws++;
		st.quads += batch_quads;
		st.switches += last_textured != 1; // everything is textured: solid quads use the white texel
		last_textured = 1;
#endif
	}
}

void gfx_submit(void) {
	flush();
	// Tiny3D only kicks the GPU at flip time; push the commands written so far right away.
	rsxFlushBuffer(tiny3d_Get_GCM_Context());
}

void gfx_end(void) {
	flush();
#ifdef DEV_TOOLS
	gcmContextData* ctx = tiny3d_Get_GCM_Context();
	timestamp(TIMESTAMP_BASE + 2 * (frame_no & 1) + 1);
	// Tiny3D's flip first waits for the previous one (display refresh, or the RSX finishing it
	// late) the same way: waiting here first times it.
	s64 t = sysGetSystemTime();
	for (int i = 0; i < 10000 && gcmGetFlipStatus() > 0; ++i) {
		usleep(200);
	}
	st.vsync += sysGetSystemTime() - t;
	in_flip = true;
#endif
	// Tiny3D flips on vsync and waits for the previous flip: standard double buffering, one
	// frame per refresh while a frame takes under 16.7 ms. An extra wait on the vblank
	// counter on top of it fell out of step on real hardware and dropped to 40 fps.
	tiny3d_Flip();
#ifdef DEV_TOOLS
	in_flip = false;
	st.cmd_bytes += ((u32*) ctx)[2] - cmd_mark;
	cmd_mark = ((u32*) ctx)[2];
	if (frame_no > 0) { // the previous frame is on screen: its timestamps are written
		u32 prev = TIMESTAMP_BASE + 2 * ((frame_no - 1) & 1);
		u64 rsx = gcmGetTimeStamp(prev + 1) - gcmGetTimeStamp(prev);
		st.rsx += rsx;
		st.rsx_frames++;
		if (rsx > st.rsx_max) st.rsx_max = rsx;
	}
	frame_no++;
#endif
}

// One textured quad, in the open draw call when it samples the same texture the same way.
// Sheet sprites are drawn from the sheet, their UVs mapped into their spot.
static void quad(const struct gfx_tex* tex, float x, float y, float w, float h,
                 float u0, float v0, float u1, float v1, u32 rgba, bool smooth) {
	if (tex->sheet) {
		const struct gfx_tex* s = tex->sheet;
		u0 = (tex->sx + u0 * tex->w) / s->w;
		u1 = (tex->sx + u1 * tex->w) / s->w;
		v0 = (tex->sy + v0 * tex->h) / s->h;
		v1 = (tex->sy + v1 * tex->h) / s->h;
		tex = s;
	}
	if (!batch_open || tex != batch_tex || smooth != batch_smooth) {
		flush();
		tiny3d_SetTextureWrap(0, tex->offset, tex->w, tex->h, tex->pitch, tiny_format(tex->format),
		                      TEXTWRAP_CLAMP, TEXTWRAP_CLAMP, smooth ? TEXTURE_LINEAR : TEXTURE_NEAREST);
		tiny3d_SetPolygon(TINY3D_QUADS); // only now: an empty polygon loses the rest of the frame
		batch_tex = tex;
		batch_smooth = smooth;
		batch_open = true;
		STAT(st.binds++);
		STAT(batch_quads = 0);
	}
	STAT(batch_quads++);
	STAT(count_fill(x, y, w, h));
	// Every vertex carries the same attributes, as Tiny3D requires within one polygon.
	tiny3d_VertexPos(x, y, Z);
	tiny3d_VertexColor(rgba);
	tiny3d_VertexTexture(u0, v0);
	tiny3d_VertexPos(x + w, y, Z);
	tiny3d_VertexColor(rgba);
	tiny3d_VertexTexture(u1, v0);
	tiny3d_VertexPos(x + w, y + h, Z);
	tiny3d_VertexColor(rgba);
	tiny3d_VertexTexture(u1, v1);
	tiny3d_VertexPos(x, y + h, Z);
	tiny3d_VertexColor(rgba);
	tiny3d_VertexTexture(u0, v1);
}

void gfx_rect(float x, float y, float w, float h, u32 rgba) {
	// The white texel times the color (exact: the texel is 1.0).
	quad(&white, x, y, w, h, 0.5f, 0.5f, 0.5f, 0.5f, rgba, false);
}

void gfx_image_uv(const struct gfx_tex* tex, float x, float y, float w, float h,
                  float u0, float v0, float u1, float v1, u32 tint, bool smooth) {
	quad(tex, x, y, w, h, u0, v0, u1, v1, tint, smooth);
}

void gfx_image(const struct gfx_tex* tex, float x, float y, float w, float h, u32 tint, bool smooth) {
	quad(tex, x, y, w, h, 0, 0, 1, 1, tint, smooth);
}

int gfx_text_width(const char* text, int size) {
	return strlen(text) * 8 * size;
}

void gfx_text(float x, float y, int size, const char* text, u32 rgba) {
	const float cw = 8.0f / font.w, ch = 8.0f / font.h, s = 8.0f * size;
	for (const u8* c = (const u8*) text; *c; ++c, x += s) {
		if (*c > FONT_FIRST) { // spaces and control characters draw nothing
			int i = *c - FONT_FIRST;
			float u = (i % 16) * cw, v = (i / 16) * ch;
			quad(&font, x, y, s, s, u, v, u + cw, v + ch, rgba, false);
		}
	}
}

void gfx_text_shadow(float x, float y, int size, const char* text, u32 rgba, u32 shadow) {
	gfx_text(x + size, y + size, size, text, shadow);
	gfx_text(x, y, size, text, rgba);
}
