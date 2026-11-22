// The audio device: the synth (synth.c) and the PCM clip -> I2S PDM (DAC line mode) on IO18 -> RC filter
// -> NS4150C. A task renders 10 ms blocks while anything plays and powers the amp and PDM down between.
// audio_play hands a sequence to the task (queued, so it is safe from any task).
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "synth.h"  // note_t, the waves and the slots


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
