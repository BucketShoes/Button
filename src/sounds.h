// The game's player-facing sounds, per sound set: the capture build-up, the capture calls, the
// countdown and the win. Plain C, no ESP-IDF (like synth.c), so tools/render_sounds.c renders exactly
// these on a PC. main.c decides when each plays; the admin sounds (pause, reset, power, battery,
// dings) are the same in every set and stay in main.c.
//
// A sound is up to two voices, each a note sequence for one synth slot, all starting together. The
// sequences live in static buffers here; the generated ones (the build-up, the countdown) are rebuilt
// by the call that returns them, so a sound must have finished (or been replaced) before the same
// call is made again. That holds in the game: one build-up at a time, one countdown step a second.
#pragma once
#include <stdint.h>
#include "synth.h"

enum { SOUNDS_SCIFI = 0, SOUNDS_MEDIEVAL = 1, SOUNDS_COUNT };  // config byte 2

typedef struct {
    int slot;
    const note_t *notes;
    int n;
} voice_t;
typedef struct {
    int count;  // 0 = nothing in this set
    voice_t v[2];
} sound_t;

// The countdown steps: seconds to the win at which one plays.
#define SOUNDS_BLIPS 13
extern const int SOUNDS_BLIP_AT[SOUNDS_BLIPS];

void sounds_init(void);
sound_t sounds_build(int set, uint32_t ms);  // the build-up for a hold that captures in ms (ms <= 65000)
sound_t sounds_let_go(int set);              // let go before the capture
sound_t sounds_team(int set, int team);      // captured by team 0 / 1
sound_t sounds_win(int set);
sound_t sounds_blip(int set, int i);         // countdown step i (SOUNDS_BLIP_AT[i] s to go)
