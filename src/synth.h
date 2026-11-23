// The synth: note sequences on a few channels, mixed into 16-bit samples. Plain C, no ESP-IDF, so the
// same code runs in the firmware (audio.c feeds it to I2S PDM) and on a PC (tools/render_sounds.c
// writes WAV files). Integer maths per sample; the instruments' envelopes run in float at a control
// rate (every SYNTH_CTRL samples), which is soft float on the C6 (no FPU) and cheap enough there.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define SYNTH_SR    48000              // the sample rate (the firmware's PDM rate)
#define SYNTH_BLOCK (SYNTH_SR / 100)   // 10 ms: what synth_render makes per call

// The plain waves (with a fixed 2 ms attack and 8 ms release), then the instruments: FM voices with
// their own envelopes, pitch and timbre, which are what the themed sets use.
//   W_BELL       struck bell: inharmonic, bright at the strike, rings down over the note
//   W_TRUMPET    bright brass: a fast attack with a slight scoop up into the pitch, vibrato on long notes
//   W_HORN       natural / animal horn: darker, slower, a bigger scoop and some breath
//   W_FLUTE      soft breathy tone, slow attack, vibrato
//   W_DRUM       tabor / tom: a pitched thump with a quick pitch drop and a little rattle
//   W_SNARE      mostly rattle, a short tone under it
//   W_TIMPANI    a tuned drum: holds its pitch and rings
//   W_SWELL      the horn, growing from nothing over the whole note (louder and brighter): a crescendo
// For the struck ones (bell, drums, timpani) ms is how long they ring: they fade to about -45 dB by then.
typedef enum { W_SQUARE, W_TRI, W_SINE, W_NOISE, W_BELL, W_TRUMPET, W_HORN, W_FLUTE, W_DRUM, W_SNARE, W_TIMPANI, W_SWELL } wave_t;

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
    SLOT_COUNT  // (the synth's channels)
};

// Start a sequence on a channel, replacing what it was playing (notes = NULL or n = 0 stops it). The
// notes are read in place while they play, so they must stay valid until then.
void synth_play(int slot, const note_t *notes, int n);
void synth_stop_all(void);
void synth_reset(void);  // stop all and clear every channel's state (phases, noise): a fresh start, for repeatable renders
bool synth_busy(void);  // any channel playing
// Add the next SYNTH_BLOCK samples of every channel into acc (which the caller zeroes, and may already
// hold other audio, e.g. the PCM clip).
void synth_render(int32_t *acc);
// The master stage: gain (Q16, 65536 = full), then a hard clip at -2 dBFS.
int16_t synth_out(int32_t acc, int32_t gain_q16);
// The plain square and triangle waves' level, Q8 (256 = as written). The medieval set's instruments are
// quieter than full-scale squares, so the admin beeps (shared by both sets) are turned down to match.
void synth_set_plain_gain(int q8);
void synth_init(void);
