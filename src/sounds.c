#include "sounds.h"
#include <stdbool.h>
#include <stddef.h>

const int SOUNDS_BLIP_AT[SOUNDS_BLIPS] = {60, 30, 20, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1};
#define BLIP_AT SOUNDS_BLIP_AT
#define BLIP_COUNT SOUNDS_BLIPS

static sound_t one(int slot, const note_t *notes, int n) { return (sound_t){1, {{slot, notes, n}}}; }
static sound_t two(int s1, const note_t *n1, int c1, int s2, const note_t *n2, int c2)
{
    return (sound_t){2, {{s1, n1, c1}, {s2, n2, c2}}};
}
#define LEN(a) ((int)(sizeof(a) / sizeof((a)[0])))

// ---------------------------------------------------------------- sci-fi
// Square waves: they carry best on the FS-1540 (resonance ~850 Hz).
static const note_t SND_TEAM[2][5] = {
    {   // team 1: rising square arpeggio
        {1047, 1047, 100, W_SQUARE, 255}, {1319, 1319, 100, W_SQUARE, 255}, {1568, 1568, 100, W_SQUARE, 255},
        {2093, 2093, 350, W_SQUARE, 255}, {0, 0, 1, W_SQUARE, 0},
    },
    {   // team 2: square "da-da-DAAA"
        {880, 880, 90, W_SQUARE, 255}, {0, 0, 40, W_SQUARE, 0}, {880, 880, 90, W_SQUARE, 255},
        {0, 0, 40, W_SQUARE, 0}, {1319, 1319, 400, W_SQUARE, 255},
    },
};
static const note_t SND_WIN[] = {
    {784, 784, 150, W_SQUARE, 255}, {1047, 1047, 150, W_SQUARE, 255}, {1319, 1319, 150, W_SQUARE, 255},
    {1568, 1568, 500, W_SQUARE, 255}, {0, 0, 100, W_SQUARE, 0}, {1319, 1319, 150, W_SQUARE, 255},
    {1568, 1568, 800, W_SQUARE, 255},
};
static note_t s_build[1];  // the rising whoop, its length set per hold

// Last-seconds warning blips: more pulses, higher and faster as it gets closer.
#define BLIP_MAX_NOTES 9
static note_t s_blip[BLIP_COUNT][BLIP_MAX_NOTES];
static int s_blip_len[BLIP_COUNT];

static void scifi_init(void)
{
    for (int i = 0; i < BLIP_COUNT; i++) {
        int pulses = i == 0 ? 1 : i < 3 ? 2 : i < 8 ? 3 : 4;
        uint16_t f = (uint16_t)(880 + i * 130);  // 880 Hz at 60 s .. ~2440 Hz at 1 s
        uint16_t on = (uint16_t)(90 - i * 5);    // 90 ms .. 30 ms
        uint16_t gap = (uint16_t)(60 - i * 3);   // 60 ms .. 24 ms
        int k = 0;
        for (int p = 0; p < pulses; p++) {
            s_blip[i][k++] = (note_t){f, f, on, W_SQUARE, 255};
            if (p < pulses - 1) s_blip[i][k++] = (note_t){0, 0, gap, W_SQUARE, 0};
        }
        s_blip_len[i] = k;
    }
}

// ---------------------------------------------------------------- medieval set
// The player-facing sounds in instruments: the capture build-up (drums under a rising horn, as long as
// the hold), a horn call per team, bells for the countdown, a trumpet fanfare for the win. Everything
// for admins (pause, reset, power, battery) stays the plain synthetic set in both, so it's obviously
// not part of the game. Frequencies: the speaker carries little below ~500 Hz, so the tunes sit high
// and the drums rely on their attack and rattle.

// Team 1: a bright trumpet call. Team 2: a darker natural horn, lower and slower. Each with a drum hit.
static const note_t MED_TEAM[2][9] = {
    {{392, 392, 120, W_TRUMPET, 255}, {0, 0, 25, 0, 0}, {392, 392, 120, W_TRUMPET, 255}, {0, 0, 25, 0, 0},
     {523, 523, 140, W_TRUMPET, 255}, {659, 659, 140, W_TRUMPET, 255}, {784, 784, 650, W_TRUMPET, 255}},
    {{440, 440, 300, W_HORN, 255}, {0, 0, 40, 0, 0}, {659, 659, 300, W_HORN, 255}, {0, 0, 40, 0, 0},
     {880, 880, 800, W_HORN, 255}},
};
static const int MED_TEAM_LEN[2] = {7, 5};
static const note_t MED_TEAM_DRUM[] = {{196, 196, 500, W_TIMPANI, 255}};
// Win: a trumpet fanfare with bells ringing under the end.
static const note_t MED_WIN[] = {
    {523, 523, 130, W_TRUMPET, 255}, {0, 0, 20, 0, 0}, {523, 523, 130, W_TRUMPET, 255}, {0, 0, 20, 0, 0},
    {523, 523, 130, W_TRUMPET, 255}, {659, 659, 450, W_TRUMPET, 255}, {0, 0, 60, 0, 0},
    {587, 587, 150, W_TRUMPET, 255}, {659, 659, 150, W_TRUMPET, 255}, {784, 784, 1100, W_TRUMPET, 255},
};
static const note_t MED_WIN_BELLS[] = {
    {196, 196, 300, W_TIMPANI, 255}, {196, 196, 300, W_TIMPANI, 200}, {262, 262, 520, W_TIMPANI, 255},
    {1047, 1047, 500, W_BELL, 200}, {784, 784, 500, W_BELL, 200}, {1047, 1047, 2200, W_BELL, 230},
};

// Countdown: a horn at a minute, bells at 30 and 20 s, then a bell each second, rising a little to the end.
static note_t s_med_blip[BLIP_COUNT][4];
static int s_med_blip_len[BLIP_COUNT];
static void medieval_init(void)
{
    for (int i = 0; i < BLIP_COUNT; i++) {
        int sec = BLIP_AT[i], k = 0;
        note_t *b = s_med_blip[i];
        if (sec == 60) {
            b[k++] = (note_t){440, 440, 700, W_HORN, 255};
        } else if (sec >= 20) {
            int n = sec == 30 ? 2 : 3;
            for (int p = 0; p < n; p++) b[k++] = (note_t){659, 659, p < n - 1 ? 260 : 900, W_BELL, 255};
        } else {
            uint16_t f = (uint16_t)(784 + (10 - sec) * 40);  // rising a little as it closes
            b[k++] = (note_t){f, f, 700, W_BELL, 255};
        }
        s_med_blip_len[i] = k;
    }
}

// The capture build-up, made for each hold to last exactly `ms`. It has to say "keep holding" within
// the first second, so it starts from nothing and grows straight away:
//   - the opening (about the first 0.4-1 s): a snare roll growing from a whisper, under a horn
//     swelling up from silence and sliding up into its note;
//   - then drums (SLOT_BUILD2): a march, accelerating to ~300 beats a minute, a louder hit on each
//     bar, the off-beats filled in from halfway, and a snare roll for the last fifth;
//   - over it the horn (SLOT_BUILD) climbs a harmonic minor scale and ends on its leading note, so
//     the capture's horn call resolves it.
// Everything gets louder all the way; the drums sit well above the horn.
#define MED_BUILD_DRUMS 400
static note_t s_med_drums[MED_BUILD_DRUMS];
static note_t s_med_climb[8];
static sound_t build_medieval(uint32_t ms)
{
    static const uint16_t SCALE[7] = {440, 494, 523, 587, 659, 698, 831};  // A harmonic minor to G#
    uint32_t open = ms * 3 / 10;
    open = open < 400 ? 400 : open > 1000 ? 1000 : open;
    if (open > ms) open = ms;
    int n = 0;
    uint32_t t = 0, roll_at = ms * 4 / 5;
    // The opening roll: 45 ms strokes, from very quiet to the march's level.
    while (t < open && n < MED_BUILD_DRUMS - 2) {
        uint32_t r = open - t < 45 ? open - t : 45;
        s_med_drums[n++] = (note_t){330, 330, (uint16_t)r, W_SNARE, (uint8_t)(25 + 175 * t / open)};
        t += r;
    }
    for (int beat = 0; t < ms && n < MED_BUILD_DRUMS - 2; beat++) {
        uint32_t step = 420 - 220 * t / (ms ? ms : 1);  // ms per beat
        uint8_t vol = (uint8_t)(190 + 65 * t / (ms ? ms : 1));
        if (t >= roll_at) {  // the roll: 16ths
            uint32_t r = step / 4 < 35 ? 35 : step / 4;
            if (t + r > ms) r = ms - t;
            s_med_drums[n++] = (note_t){330, 330, (uint16_t)r, W_SNARE, vol};
            t += r;
            continue;
        }
        bool bar = beat % 4 == 0, half = t >= ms / 2;
        uint32_t len = half ? step / 2 : step;
        if (t + len > ms) len = ms - t;
        s_med_drums[n++] = (note_t){(uint16_t)(bar ? 165 : 220), (uint16_t)(bar ? 165 : 220), (uint16_t)len, W_DRUM,
                                    (uint8_t)(bar ? vol : vol * 4 / 5)};
        t += len;
        if (half && t < ms && n < MED_BUILD_DRUMS - 1) {  // the off-beat
            uint32_t l2 = step - len;
            if (t + l2 > ms) l2 = ms - t;
            s_med_drums[n++] = (note_t){300, 300, (uint16_t)l2, W_SNARE, (uint8_t)(vol * 3 / 5)};
            t += l2;
        }
    }
    if (t < ms) s_med_drums[n++] = (note_t){0, 0, (uint16_t)(ms - t), 0, 0};
    // The horn: the swell over the opening (sliding up three semitones into A), then the climb, 1 to 6
    // more notes, about one every 0.8 s, always ending on G#.
    s_med_climb[0] = (note_t){370, 440, (uint16_t)open, W_SWELL, 120};
    int c = 1;
    uint32_t rest = ms - open;
    if (rest > 0) {
        int k = (int)(rest / 800);
        k = k < 1 ? 1 : k > 6 ? 6 : k;
        uint32_t done = 0;
        for (int i = 0; i < k; i++) {
            uint32_t end = rest * (uint32_t)(i + 1) / (uint32_t)k;
            uint16_t f = SCALE[7 - k + i];
            s_med_climb[c++] = (note_t){f, f, (uint16_t)(end - done), W_HORN, (uint8_t)(50 + 60 * (i + 1) / k)};
            done = end;
        }
    }
    return two(SLOT_BUILD, s_med_climb, c, SLOT_BUILD2, s_med_drums, n);
}
// Let go before the capture: the horn sags, a let-down, so it's clear it didn't take.
static const note_t MED_CANCEL[] = {{440, 311, 380, W_HORN, 150}};

// ---------------------------------------------------------------- the API
void sounds_init(void)
{
    scifi_init();
    medieval_init();
}

sound_t sounds_build(int set, uint32_t ms)
{
    if (ms > 65000) ms = 65000;  // (note lengths are 16-bit)
    if (ms < 1) ms = 1;
    if (set == SOUNDS_MEDIEVAL) return build_medieval(ms);
    s_build[0] = (note_t){250, 1200, (uint16_t)ms, W_SQUARE, 140};
    return one(SLOT_BUILD, s_build, 1);
}

sound_t sounds_let_go(int set)
{
    if (set == SOUNDS_MEDIEVAL) return one(SLOT_BUILD, MED_CANCEL, LEN(MED_CANCEL));
    return (sound_t){0};
}

sound_t sounds_team(int set, int team)
{
    team = team ? 1 : 0;
    if (set == SOUNDS_MEDIEVAL)
        return two(SLOT_EVENT, MED_TEAM[team], MED_TEAM_LEN[team], SLOT_EVENT2, MED_TEAM_DRUM, LEN(MED_TEAM_DRUM));
    return one(SLOT_EVENT, SND_TEAM[team], LEN(SND_TEAM[team]));
}

sound_t sounds_win(int set)
{
    if (set == SOUNDS_MEDIEVAL) return two(SLOT_EVENT, MED_WIN, LEN(MED_WIN), SLOT_EVENT2, MED_WIN_BELLS, LEN(MED_WIN_BELLS));
    return one(SLOT_EVENT, SND_WIN, LEN(SND_WIN));
}

sound_t sounds_blip(int set, int i)
{
    if (i < 0 || i >= BLIP_COUNT) return (sound_t){0};
    if (set == SOUNDS_MEDIEVAL) return one(SLOT_DIAG, s_med_blip[i], s_med_blip_len[i]);
    return one(SLOT_DIAG, s_blip[i], s_blip_len[i]);
}

uint32_t sounds_ms(sound_t s)
{
    uint32_t most = 0;
    for (int v = 0; v < s.count; v++) {
        uint32_t ms = 0;
        for (int i = 0; i < s.v[v].n; i++) ms += s.v[v].notes[i].ms;
        if (ms > most) most = ms;
    }
    return most;
}
