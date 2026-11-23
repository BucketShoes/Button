// Render the player-facing sounds to WAV files on a PC, with the firmware's own synth (src/synth.c) and
// sounds (src/sounds.c): the same code, so the files are what the button sends to its amp at full
// volume, before the speaker.
//
//   gcc -O2 -std=c11 -Wall -ffp-contract=off -Isrc src/synth.c src/sounds.c tools/render_sounds.c -lm -o build/render_sounds.exe
//   build/render_sounds.exe [OUTDIR]      (default audio/preview)
//
// (-ffp-contract=off: the C6 has no fused multiply-add, so the PC mustn't fuse either. The integer
// parts are bit-exact; only the last bit of expf/logf/powf can differ between the maths libraries.)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sounds.h"
#include "synth.h"

#ifdef _WIN32
#include <direct.h>
#define mkdir_p(p) _mkdir(p)
#else
#include <sys/stat.h>
#define mkdir_p(p) mkdir(p, 0755)
#endif

// One thing to start at a time: a sound (all its voices) or, with stop >= 0, silencing a slot.
typedef struct {
    uint32_t at_ms;
    sound_t s;
    int stop;
} event_t;

#define MAX_EVENTS 32
static event_t s_ev[MAX_EVENTS];
static int s_nev;

static void at(uint32_t ms, sound_t s) { s_ev[s_nev++] = (event_t){ms, s, -1}; }
static void stop_at(uint32_t ms, int slot) { s_ev[s_nev++] = (event_t){ms, {0}, slot}; }

static uint32_t sound_ms(sound_t s)
{
    uint32_t longest = 0;
    for (int i = 0; i < s.count; i++) {
        uint32_t t = 0;
        for (int k = 0; k < s.v[i].n; k++) t += s.v[i].notes[k].ms;
        if (t > longest) longest = t;
    }
    return longest;
}

static void put16(FILE *f, uint16_t v) { fputc(v & 255, f), fputc(v >> 8, f); }
static void put32(FILE *f, uint32_t v) { put16(f, (uint16_t)v), put16(f, (uint16_t)(v >> 16)); }

// Render the events (in time order) to path, at full volume, until everything has finished.
static void render(const char *dir, const char *name)
{
    uint32_t end = 0;
    for (int i = 0; i < s_nev; i++) {
        uint32_t e = s_ev[i].at_ms + sound_ms(s_ev[i].s);
        if (e > end) end = e;
    }
    end = (end + 300 + 9) / 10 * 10;  // (whole 10 ms blocks)
    size_t n = (size_t)end / 10 * SYNTH_BLOCK;
    int16_t *pcm = malloc(n * sizeof *pcm);
    synth_reset();  // (each file from a fresh synth, not carrying on from the last one)
    int k = 0;
    for (uint32_t t = 0, i = 0; t < end; t += 10, i += SYNTH_BLOCK) {
        for (; k < s_nev && s_ev[k].at_ms <= t; k++) {
            if (s_ev[k].stop >= 0) synth_play(s_ev[k].stop, NULL, 0);
            for (int v = 0; v < s_ev[k].s.count; v++) synth_play(s_ev[k].s.v[v].slot, s_ev[k].s.v[v].notes, s_ev[k].s.v[v].n);
        }
        int32_t acc[SYNTH_BLOCK] = {0};
        synth_render(acc);
        for (int j = 0; j < SYNTH_BLOCK; j++) pcm[i + j] = synth_out(acc[j], 65536);
    }
    char path[512];
    snprintf(path, sizeof path, "%s/%s.wav", dir, name);
    FILE *f = fopen(path, "wb");
    if (!f) {
        perror(path);
        exit(1);
    }
    uint32_t bytes = (uint32_t)(n * 2);
    fwrite("RIFF", 1, 4, f), put32(f, 36 + bytes), fwrite("WAVEfmt ", 1, 8, f);
    put32(f, 16), put16(f, 1), put16(f, 1), put32(f, SYNTH_SR), put32(f, SYNTH_SR * 2), put16(f, 2), put16(f, 16);
    fwrite("data", 1, 4, f), put32(f, bytes);
    for (size_t j = 0; j < n; j++) put16(f, (uint16_t)pcm[j]);
    fclose(f);
    free(pcm);
    s_nev = 0;
    printf("%s\n", path);
}

static const char *SET_NAME[SOUNDS_COUNT] = {"scifi", "medieval"};

static void countdown(int set, int from, int to)  // the steps whose seconds are from..to, a second apart
{
    uint32_t t = 0;
    for (int i = 0; i < SOUNDS_BLIPS; i++) {
        if (SOUNDS_BLIP_AT[i] > from || SOUNDS_BLIP_AT[i] < to) continue;
        at(t, sounds_blip(set, i));
        t += 1000;
    }
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : "audio/preview";
    mkdir_p(dir);
    synth_init();
    sounds_init();
    char name[64];
    static const uint32_t HOLDS[] = {1000, 3000, 8000, 20000};
    for (int set = 0; set < SOUNDS_COUNT; set++) {
        const char *sn = SET_NAME[set];
        for (int team = 0; team < 2; team++) {
            at(0, sounds_team(set, team));
            snprintf(name, sizeof name, "%s_team%d_capture", sn, team + 1);
            render(dir, name);
        }
        // The build-up and the capture call that ends it, as in a game (team 1).
        for (size_t h = 0; h < sizeof HOLDS / sizeof HOLDS[0]; h++) {
            at(0, sounds_build(set, HOLDS[h]));
            at(HOLDS[h], sounds_team(set, 0));
            snprintf(name, sizeof name, "%s_hold_%us", sn, (unsigned)(HOLDS[h] / 1000));
            render(dir, name);
        }
        if (sounds_let_go(set).count) {  // a 3 s hold let go at 1.2 s
            at(0, sounds_build(set, 3000));
            stop_at(1200, SLOT_BUILD2);
            at(1200, sounds_let_go(set));
            snprintf(name, sizeof name, "%s_let_go_at_1.2s", sn);
            render(dir, name);
        }
        at(0, sounds_win(set));
        snprintf(name, sizeof name, "%s_win", sn);
        render(dir, name);
        countdown(set, 60, 20);
        snprintf(name, sizeof name, "%s_countdown_60_30_20", sn);
        render(dir, name);
        countdown(set, 10, 1);
        snprintf(name, sizeof name, "%s_countdown_10_to_1", sn);
        render(dir, name);
    }
    return 0;
}
