// Feeds one mGBA core's audio to the PS3 audio port (48 kHz float stereo, 256-sample blocks).
// Polled once per frame from the main loop: the port's 16 blocks (~85 ms) cover a frame easily.
// While no game runs it plays the menu music (data/music.ogg, credits in data/CREDITS.txt).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <audio/audio.h>
#include <sys/systime.h>

#include <mgba/core/core.h>
#include <mgba-util/audio-buffer.h>
#include <mgba-util/audio-resampler.h>

// stb_vorbis, in-memory float decoding only. Its uninitialized-header warning is a false positive.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#define STB_VORBIS_NO_STDIO
#define STB_VORBIS_NO_PUSHDATA_API
#define STB_VORBIS_NO_INTEGER_CONVERSION
#define STB_VORBIS_MAX_CHANNELS 2
#include "stb_vorbis.c"
#pragma GCC diagnostic pop

#include "audio.h"
#include "emu.h"
#include "music_ogg.h"

#define OUT_RATE 48000
#define NUM_BLOCKS AUDIO_BLOCK_16
#define TARGET_AHEAD 8 // blocks queued ahead of the hardware (~43 ms): absorbs a late frame
// The output rate bends up to 5% to keep the queue at its target: when the games run below 60 fps
// (link cable waits, busy scenes) the sound plays a little lower instead of cutting out.
#define MAX_SKEW 0.05
#define MUSIC_VOLUME 0.5f // -6 dB
#define MUSIC_FADE_STEP (1.0f / (OUT_RATE * 0.75f)) // per sample: fades take 0.75 s

static u32 port;
static audioPortConfig config;
static u32 write_block;
static struct mAudioBuffer resampled;
static struct mAudioResampler resampler;
static int16_t samples[AUDIO_BLOCK_SAMPLES * 2];

// The music is a 48 kHz stereo loop, decoded block by block as the port needs it. Only one
// thread at a time runs audio_update (main without a game, else the heard player's thread).
static stb_vorbis* music;
static volatile bool music_on; // set by the main loop; music_gain follows it
static float music_gain;

#ifdef DEV_TOOLS
static u64 music_ticks, music_samples; // decoding time, for the dev log
static float* capture; // AUDIO command: what goes to the port, interleaved stereo
static u32 capture_len, capture_pos; // in sample frames
#endif

void audio_init(void) {
	audioInit();
	audioPortParam params = {
		.numChannels = AUDIO_PORT_2CH,
		.numBlocks = NUM_BLOCKS,
		.attrib = 0,
		.level = 1.0f,
	};
	audioPortOpen(&params, &port);
	audioGetPortConfig(port, &config);
	memset((void*) (u64) config.audioDataStart, 0, NUM_BLOCKS * AUDIO_BLOCK_SAMPLES * 2 * sizeof(float));
	audioPortStart(port);

	mAudioBufferInit(&resampled, 4096, 2);
	mAudioResamplerInit(&resampler, mINTERPOLATOR_COSINE);
	mAudioResamplerSetDestination(&resampler, &resampled, OUT_RATE);

	int error;
	music = stb_vorbis_open_memory(music_ogg, music_ogg_size, &error, NULL);
	if (!music) {
		printf("audio: can't open the music (stb_vorbis error %d)\n", error);
	}
}

void audio_shutdown(void) {
	audioPortStop(port);
	audioPortClose(port);
	audioQuit();
	mAudioResamplerDeinit(&resampler);
	mAudioBufferDeinit(&resampled);
	if (music) {
		stb_vorbis_close(music);
	}
}

void audio_music(bool on) {
	music_on = on;
}

static bool music_audible(void) {
	return music && (music_on || music_gain > 0);
}

// Adds the next block of music to `block`, fading towards music_on (squared: even to the ear).
static void mix_music(float* block) {
	if (!music_audible()) {
		return;
	}
#ifdef DEV_TOOLS
	u64 t0 = __builtin_ppc_get_timebase();
#endif
	float pcm[AUDIO_BLOCK_SAMPLES * 2];
	bool rewound = false;
	for (int got = 0; got < AUDIO_BLOCK_SAMPLES;) {
		int n = stb_vorbis_get_samples_float_interleaved(music, 2, &pcm[got * 2], (AUDIO_BLOCK_SAMPLES - got) * 2);
		if (n == 0 && (rewound || !stb_vorbis_seek_start(music))) { // loops, unless nothing decodes
			stb_vorbis_close(music);
			music = NULL;
			return;
		}
		rewound = n == 0;
		got += n;
	}
	float target = music_on ? 1.0f : 0.0f;
	for (int i = 0; i < AUDIO_BLOCK_SAMPLES; ++i) {
		music_gain = target > music_gain ? fminf(music_gain + MUSIC_FADE_STEP, 1.0f) : fmaxf(music_gain - MUSIC_FADE_STEP, target);
		float gain = music_gain * music_gain * MUSIC_VOLUME;
		block[i * 2] += pcm[i * 2] * gain;
		block[i * 2 + 1] += pcm[i * 2 + 1] * gain;
	}
#ifdef DEV_TOOLS
	music_ticks += __builtin_ppc_get_timebase() - t0;
	if ((music_samples += AUDIO_BLOCK_SAMPLES) >= OUT_RATE) {
		log_printf("music: decoding %.3f ms per frame\n", music_ticks * 1000.0 / sysGetTimebaseFrequency() / 60);
		music_ticks = music_samples = 0;
	}
#endif
}

static u32 blocks_ahead(void) {
	u32 read_block = *(u64*) (u64) config.readIndex;
	return (write_block - read_block + NUM_BLOCKS) % NUM_BLOCKS;
}

#ifdef DEV_TOOLS
static u32 stat_samples, stat_padded, stat_ahead_sum, stat_calls;
#endif

void audio_update(struct mCore* core) {
	// The display runs at ~60 Hz but the GBA at 59.73 Hz, and below 60 fps the games make fewer
	// samples than the port plays: bend the output rate to keep the queue near its target,
	// smoothed (~0.3 s) so the pitch glides instead of wobbling.
	static double skew;
	u32 ahead = blocks_ahead();
	double drift = ((double) ahead - TARGET_AHEAD) / TARGET_AHEAD;
	if (drift > 1) drift = 1;
	if (drift < -1) drift = -1;
	skew += (MAX_SKEW * drift - skew) * 0.05;
	if (core) {
		mAudioResamplerSetSource(&resampler, core->getAudioBuffer(core), core->audioSampleRate(core), true);
		resampler.destRate = OUT_RATE * (1.0 - skew);
		mAudioResamplerProcess(&resampler);
	}
#ifdef DEV_TOOLS
	if (core) {
		stat_ahead_sum += ahead;
		++stat_calls;
	}
#endif

	float* start = (float*) (u64) config.audioDataStart;
	// Below `least` blocks we pad with silence rather than let the hardware replay stale audio.
	// The music alone keeps the queue as deep as a game does: a late frame must not replay it.
	u32 least = !core && music_audible() ? TARGET_AHEAD : 2;
	// Stay at least one block behind the block being read, so we never write under it.
	while ((ahead = blocks_ahead()) < NUM_BLOCKS - 2 && (ahead < least || mAudioBufferAvailable(&resampled) >= AUDIO_BLOCK_SAMPLES)) {
		size_t got = mAudioBufferRead(&resampled, samples, AUDIO_BLOCK_SAMPLES);
		memset(&samples[got * 2], 0, (AUDIO_BLOCK_SAMPLES - got) * 2 * sizeof(int16_t));
#ifdef DEV_TOOLS
		if (core) { // a game is heard: padding is a gap in its sound
			stat_padded += AUDIO_BLOCK_SAMPLES - got;
			if ((stat_samples += AUDIO_BLOCK_SAMPLES) >= OUT_RATE) {
				log_printf("audio: %u ms of silence padded, queue %.1f blocks, rate %+.1f%%\n",
				           stat_padded * 1000 / OUT_RATE, stat_calls ? (double) stat_ahead_sum / stat_calls : 0.0, -skew * 100);
				stat_samples = stat_padded = stat_ahead_sum = stat_calls = 0;
			}
		}
#endif
		float* block = start + write_block * AUDIO_BLOCK_SAMPLES * 2;
		for (int i = 0; i < AUDIO_BLOCK_SAMPLES * 2; ++i) {
			block[i] = samples[i] * (1.0f / 32768.0f);
		}
		mix_music(block);
#ifdef DEV_TOOLS
		if (capture && capture_pos < capture_len) {
			memcpy(&capture[capture_pos * 2], block, AUDIO_BLOCK_SAMPLES * 2 * sizeof(float));
			__sync_synchronize(); // the samples before the count that publishes them
			capture_pos += AUDIO_BLOCK_SAMPLES;
		}
#endif
		write_block = (write_block + 1) % NUM_BLOCKS;
	}
}

#ifdef DEV_TOOLS
void audio_capture(int seconds) {
	if (capture) {
		return;
	}
	capture_pos = 0;
	capture_len = seconds * OUT_RATE / AUDIO_BLOCK_SAMPLES * AUDIO_BLOCK_SAMPLES;
	capture = malloc(capture_len * 2 * sizeof(float));
	printf("dev: audio capture of %d s %s\n", seconds, capture ? "started" : "failed (out of memory)");
}

void audio_capture_poll(const char* path) {
	if (!capture || capture_pos < capture_len) {
		return;
	}
	__sync_synchronize();
	u32 bytes = capture_len * 2 * sizeof(float);
	u8 header[44] = { 'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ', 16, 0, 0, 0,
	                  3, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 8, 0, 32, 0, 'd', 'a', 't', 'a' }; // float32 stereo
	u32 fields[][2] = { { 4, 36 + bytes }, { 24, OUT_RATE }, { 28, OUT_RATE * 8 }, { 40, bytes } };
	for (int i = 0; i < 4; ++i) {
		u32 le = __builtin_bswap32(fields[i][1]); // WAV is little-endian
		memcpy(&header[fields[i][0]], &le, 4);
	}
	u32* words = (u32*) capture;
	for (u32 i = 0; i < capture_len * 2; ++i) {
		words[i] = __builtin_bswap32(words[i]);
	}
	FILE* f = fopen(path, "wb");
	if (f) {
		fwrite(header, 1, sizeof(header), f);
		fwrite(capture, 1, bytes, f);
		fclose(f);
	}
	printf("dev: audio capture %s (%u samples)\n", f ? "saved" : "failed", capture_len);
	free(capture);
	capture = NULL;
}
#endif
