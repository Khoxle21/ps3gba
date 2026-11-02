#ifndef AUDIO_H
#define AUDIO_H

#include <stdbool.h>

struct mCore;

void audio_init(void);
void audio_shutdown(void);

// Call once per frame: drains the core's samples into the PS3 audio port (NULL: no game heard).
void audio_update(struct mCore* core);

// Menu music: fades in (true) or out (false). Call before audio_update.
void audio_music(bool on);

#ifdef DEV_TOOLS
// Records the next `seconds` of what goes to the audio port; audio_capture_poll (main loop)
// then writes it to `path` as a 48 kHz float stereo WAV.
void audio_capture(int seconds);
void audio_capture_poll(const char* path);
#endif

#endif
