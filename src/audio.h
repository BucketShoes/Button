// Tiny tone synth -> I2S PDM (DAC line mode) on IO18 -> RC filter -> NS4150C.
// Each slot plays one note sequence; slots are mixed, so sounds can overlap.
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef enum { W_SQUARE, W_TRI, W_SINE, W_NOISE } wave_t;

// One step of a sound: frequency sweeps linearly f0 -> f1 over ms.
// f0 = 0 is a rest. vol 0..255.
typedef struct {
    uint16_t f0, f1;
    uint16_t ms;
    uint8_t wave;
    uint8_t vol;
} note_t;

enum {
    SLOT_CLICK,
    SLOT_SW1,
    SLOT_SW2,
    SLOT_SW3,
    SLOT_BUILD,
    SLOT_EVENT,
    SLOT_DIAG,
    SLOT_COUNT
};

void audio_init(void);
void audio_play(int slot, const note_t *notes, int n);  // replaces whatever the slot was playing
void audio_stop(int slot);

// Raw PCM clip in the "audio" flash partition (speaker / music test).
// Header "PCM1", u32 rate (must match SR in audio.c, 48000), u32 samples, u32 reserved, then int16 LE.
bool audio_pcm_available(void);
void audio_pcm_start(void);
void audio_pcm_stop(void);
bool audio_pcm_playing(void);

#define AUDIO_PLAY(slot, arr) audio_play((slot), (arr), (int)(sizeof(arr) / sizeof((arr)[0])))
