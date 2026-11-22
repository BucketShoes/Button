// Tiny tone synth -> I2S PDM (DAC line mode) on IO18 -> RC filter -> NS4150C.
// Each slot plays one note sequence; slots are mixed, so sounds can overlap.
#pragma once
#include <stdbool.h>
#include <stdint.h>

// The plain waves (with a fixed 2 ms attack and 8 ms release), then the instruments: FM voices with
// their own envelopes, pitch and timbre, which are what the themed sets use.
//   W_BELL       struck bell: inharmonic, bright at the strike, rings down over the note
//   W_TRUMPET    bright brass: a fast attack with a slight scoop up into the pitch, vibrato on long notes
//   W_HORN       natural / animal horn: darker, slower, a bigger scoop and some breath
//   W_FLUTE      soft breathy tone, slow attack, vibrato
//   W_DRUM       tabor / tom: a pitched thump with a quick pitch drop and a little rattle
//   W_SNARE      mostly rattle, a short tone under it
//   W_TIMPANI    a tuned drum: holds its pitch and rings
// For the struck ones (bell, drums, timpani) ms is how long they ring: they fade to about -45 dB by then.
typedef enum { W_SQUARE, W_TRI, W_SINE, W_NOISE, W_BELL, W_TRUMPET, W_HORN, W_FLUTE, W_DRUM, W_SNARE, W_TIMPANI } wave_t;

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
    SLOT_BUILD2,  // a second voice for the build-up (the drums under it)
    SLOT_EVENT,
    SLOT_EVENT2,  // a second voice for event sounds (bells that ring on, harmony)
    SLOT_DIAG,
    SLOT_COUNT
};

void audio_init(void);
void audio_play(int slot, const note_t *notes, int n);  // replaces whatever the slot was playing
void audio_stop(int slot);
void audio_set_volume(int v);  // 0 mute, 1..255 = -40..0 dB
void audio_set_allowed(bool on);  // false while VSYS is off: amp + PDM off, sounds dropped
void audio_keep_awake(bool on);   // keep amp + PDM running (no wake delay), e.g. the last minute of a countdown

// Raw PCM clip in the "audio" flash partition (speaker / music test).
// Header "PCM1", u32 rate (must match SR in audio.c, 48000), u32 samples, u32 reserved, then int16 LE.
bool audio_pcm_available(void);
void audio_pcm_start(void);
void audio_pcm_stop(void);
bool audio_pcm_playing(void);

#define AUDIO_PLAY(slot, arr) audio_play((slot), (arr), (int)(sizeof(arr) / sizeof((arr)[0])))
