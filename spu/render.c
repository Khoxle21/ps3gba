// SPU program: replays one player's video stream (see include/spu_render.h) through mGBA's
// software renderer, compiled for the SPU, and DMAs each finished line out. Local store holds
// the code, a copy of VRAM/palette/OAM and the renderer state; nothing else of the GBA.
#include <string.h>

#include <spu_intrinsics.h>
#include <spu_mfcio.h>
#include <sys/spu_thread.h>

#include <mgba/core/cache-set.h>
#include <mgba/feature/video-logger.h>
#include <mgba/internal/gba/memory.h>
#include <mgba/internal/gba/renderers/cache-set.h>
#include <mgba/internal/gba/renderers/video-software.h>
#include <mgba-util/memory.h>

#include "spu_render.h"

enum { TAG_STREAM = 1, TAG_LINE = 2 /* and 3 */, TAG_STATUS = 4 };
#define CHUNK 4096
#define LINE_BYTES (GBA_VIDEO_HORIZONTAL_PIXELS * sizeof(mColor))

static uint16_t vram[GBA_SIZE_VRAM / 2] __attribute__((aligned(128)));
static uint16_t palette[GBA_SIZE_PALETTE_RAM / 2] __attribute__((aligned(16)));
static uint16_t oam[GBA_SIZE_OAM / 2] __attribute__((aligned(16)));
static mColor lines[2][GBA_VIDEO_HORIZONTAL_PIXELS] __attribute__((aligned(128)));
static uint8_t chunk[CHUNK] __attribute__((aligned(128)));
static volatile struct sr_control control;
static struct sr_status status;
static struct GBAVideoSoftwareRenderer renderer;

static uint64_t control_ea, status_ea, ring_ea, out_ea;
static uint32_t rd, wr;                 // stream bytes consumed / known to be written
static uint32_t chunk_start, chunk_len; // stream bytes held in chunk

// The renderer links against these but never uses them here: no tile cache, no logging.
int _mLOG_CAT_GBA_VIDEO;
void mLog(int category, enum mLogLevel level, const char* format, ...) {}
void mCacheSetWriteVRAM(struct mCacheSet* cache, uint32_t address) {}
void mCacheSetWritePalette(struct mCacheSet* cache, uint32_t entry, mColor color) {}
void GBAVideoCacheWriteVideoRegister(struct mCacheSet* cache, uint32_t address, uint16_t value) {}
void mappedMemoryFree(void* memory, size_t size) {}

// Copied from src/gba/video.c, which needs the whole GBA core.
const int GBAVideoObjSizes[16][2] = {
	{ 8, 8 }, { 16, 16 }, { 32, 32 }, { 64, 64 },
	{ 16, 8 }, { 32, 8 }, { 32, 16 }, { 64, 32 },
	{ 8, 16 }, { 8, 32 }, { 16, 32 }, { 32, 64 },
	{ 0, 0 }, { 0, 0 }, { 0, 0 }, { 0, 0 },
};

static void dma_wait(unsigned tags) {
	mfc_write_tag_mask(tags);
	mfc_read_tag_status_all();
}

// Tells the PPU how far we got. Waiting for every DMA first makes "read" a promise that all
// lines before it are in main memory (and the previous status put is done with the buffer).
static void publish(void) {
	dma_wait(1 << TAG_LINE | 1 << (TAG_LINE + 1) | 1 << TAG_STATUS);
	status.read = rd;
	mfc_put(&status, status_ea, 16, TAG_STATUS, 0, 0);
}

// Blocks until the PPU has written more than wr. A PPU store to the control line cancels our
// reservation on it, which raises the lock-line-lost event the channel read waits for.
static void wait_written(void) {
	publish();
	uint32_t t0 = spu_readch(SPU_RdDec);
	for (;;) {
		mfc_getllar((void*) &control, control_ea, 0, 0);
		mfc_read_atomic_status();
		if (control.write != wr) {
			wr = control.write;
			status.idle += t0 - spu_readch(SPU_RdDec);
			return;
		}
		spu_readch(SPU_RdEventStat);
		spu_writech(SPU_WrEventAck, MFC_LLR_LOST_EVENT);
	}
}

static void need(uint32_t bytes) {
	while (wr - rd < bytes) {
		wait_written();
	}
}

static struct mVideoLoggerDirtyInfo next_packet(void) {
	need(16);
	if (rd - chunk_start + 16 > chunk_len) {
		uint32_t off = rd & (SR_RING_SIZE - 1);
		uint32_t n = wr - rd;
		if (n > CHUNK) n = CHUNK;
		if (n > SR_RING_SIZE - off) n = SR_RING_SIZE - off;
		mfc_get(chunk, ring_ea + off, n, TAG_STREAM, 0, 0);
		dma_wait(1 << TAG_STREAM);
		chunk_start = rd;
		chunk_len = n;
	}
	struct mVideoLoggerDirtyInfo item;
	memcpy(&item, &chunk[rd - chunk_start], sizeof(item));
	rd += 16;
	return item;
}

// A 4 KB VRAM block straight from the ring into our VRAM copy (split at the ring's end).
static void read_vram_block(uint32_t address) {
	need(0x1000);
	uint32_t off = rd & (SR_RING_SIZE - 1);
	uint32_t first = SR_RING_SIZE - off < 0x1000 ? SR_RING_SIZE - off : 0x1000;
	mfc_get((uint8_t*) vram + address, ring_ea + off, first, TAG_STREAM, 0, 0);
	if (first < 0x1000) {
		mfc_get((uint8_t*) vram + address + first, ring_ea, 0x1000 - first, TAG_STREAM, 0, 0);
	}
	dma_wait(1 << TAG_STREAM);
	rd += 0x1000;
}

static void draw_line(int y) {
	int b = y & 1;
	dma_wait(1 << (TAG_LINE + b)); // this buffer's previous line has left
	renderer.outputBuffer = lines[b];
	// Lines leave by DMA and nothing stays in local store, so every line must be drawn (mGBA
	// skips lines it thinks are unchanged in its output buffer).
	renderer.scanlineDirty[y >> 5] |= 1U << (y & 0x1F);
	uint32_t t0 = spu_readch(SPU_RdDec);
	renderer.d.drawScanline(&renderer.d, y);
	status.busy += t0 - spu_readch(SPU_RdDec);
	mfc_put(lines[b], out_ea + y * LINE_BYTES, LINE_BYTES, TAG_LINE + b, 0, 0);
}

static void reset(void) {
	// mGBA zeroes video memory right before it resets the renderer (GBAVideoReset).
	memset(vram, 0, sizeof(vram));
	memset(palette, 0, sizeof(palette));
	memset(oam, 0, sizeof(oam));
	renderer.d.reset(&renderer.d);
}

// Arguments: effective addresses of the sr_control, the sr_status and the ring.
int main(uint64_t control_addr, uint64_t status_addr, uint64_t ring_addr) {
	control_ea = control_addr;
	status_ea = status_addr;
	ring_ea = ring_addr;
	spu_writech(SPU_WrEventMask, MFC_LLR_LOST_EVENT);
	spu_writech(SPU_WrDec, 0xFFFFFFFF);

	GBAVideoSoftwareRendererCreate(&renderer);
	renderer.d.vram = vram;
	renderer.d.palette = palette;
	renderer.d.oam = (union GBAOAM*) oam;
	renderer.d.cache = NULL;
	renderer.outputBuffer = lines[0];
	renderer.outputBufferStride = 0; // every line lands in a line buffer, see draw_line
	renderer.d.init(&renderer.d);

	for (;;) {
		struct mVideoLoggerDirtyInfo item = next_packet();
		switch ((int) item.type) {
		case DIRTY_REGISTER:
			if (item.address < GBA_REG_SOUND1CNT_LO) {
				renderer.d.writeVideoRegister(&renderer.d, item.address, item.value);
			}
			break;
		case DIRTY_PALETTE:
			if (item.address < GBA_SIZE_PALETTE_RAM) {
				STORE_16LE(item.value, item.address, palette);
				renderer.d.writePalette(&renderer.d, item.address, item.value);
			}
			break;
		case DIRTY_OAM:
			if (item.address < GBA_SIZE_OAM / 2) {
				// The proxy sends OAM halfwords as stored (little endian), not as values.
				oam[item.address] = item.value;
				renderer.d.writeOAM(&renderer.d, item.address);
			}
			break;
		case DIRTY_VRAM:
			if (item.address <= GBA_SIZE_VRAM - 0x1000) {
				read_vram_block(item.address);
				renderer.d.writeVRAM(&renderer.d, item.address);
			} else {
				need(0x1000);
				rd += 0x1000;
			}
			break;
		case DIRTY_SCANLINE:
			if (item.address < GBA_VIDEO_VERTICAL_PIXELS) {
				draw_line(item.address);
			}
			break;
		case DIRTY_FRAME:
			renderer.d.finishFrame(&renderer.d);
			break;
		case DIRTY_FLUSH:
			break;
		case SR_OUTPUT:
			out_ea = item.address;
			break;
		case SR_RESET:
			reset();
			break;
		case SR_QUIT:
			publish();
			dma_wait(1 << TAG_STATUS);
			spu_thread_exit(0);
			break;
		default: // corrupt stream: report and stop
			status.error = item.type ? item.type : 0xFFFF;
			publish();
			dma_wait(1 << TAG_STATUS);
			spu_thread_exit(1);
		}
	}
}
