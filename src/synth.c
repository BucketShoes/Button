#include "synth.h"
#include <math.h>
#include <stddef.h>
#include <string.h>

#define SR      SYNTH_SR
#define BLOCK   SYNTH_BLOCK
#define SQ_AMP  28000      // square wave amplitude
#define ATTACK  (SR / 500) // 2 ms
#define RELEASE (SR / 125) // 8 ms

typedef struct {
    const note_t *notes;
    int n, idx;
    uint32_t pos, len;
    uint32_t phase, inc;
    uint32_t lfsr;
    bool active;
    // Instruments (control rate, every CTRL samples): envelopes as floats, the sample loop in integers.
    uint32_t phm, incm;     // FM modulator phase and step
    uint32_t inc_eff;       // the carrier step with the scoop / drop / vibrato applied
    uint32_t vib;           // vibrato phase
    float a, ak;            // decaying amplitude and its factor per control step (struck instruments)
    float q, qk;            // decaying pitch-drop / strike term
    float nz, nzk;          // decaying noise amount
    int32_t amp_t, amp_n;   // tone and noise amplitudes, Q15
    uint32_t dev;           // FM deviation per unit of modulator (index * 2^32 / (2 pi) / 32767)
} chan_t;

static chan_t s_ch[SLOT_COUNT];
#define SINE_BITS 10
static int16_t s_sine[1 << SINE_BITS];  // full scale (32767)
#define SINE(ph) s_sine[(ph) >> (32 - SINE_BITS)]
#define CTRL 32                          // instrument control period in samples (0.67 ms)
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#define LIM_T 20000                      // soft limiter knee (see synth_out)
#define LIM_R (32767 - LIM_T)

static bool is_inst(int wave) { return wave >= W_BELL; }

// The factor per control step that takes a value to `to` (0..1) in `ms`.
static float decay_k(float ms, float to)
{
    float steps = ms * (SR / 1000.0f) / CTRL;
    return steps < 1 ? to : expf(logf(to) / steps);
}

static void inst_begin(chan_t *c, const note_t *nt)
{
    float ms = nt->ms ? nt->ms : 1;
    c->a = c->q = c->nz = 1.0f;
    c->ak = c->qk = c->nzk = 1.0f;
    c->vib = 0;
    c->phase = c->phm = 0;  // (each note starts the same way; the last one released to silence)
    switch (nt->wave) {
    case W_BELL:
        c->ak = decay_k(ms, 0.006f);         // rings down to about -45 dB over the note
        c->qk = decay_k(ms * 0.35f, 0.006f); // the clang (FM index) fades faster: bright strike, pure tail
        break;
    case W_DRUM:
        c->ak = decay_k(ms, 0.006f);
        c->qk = decay_k(25, 0.05f);           // the pitch drops quickly after the hit
        c->nzk = decay_k(30, 0.01f);
        break;
    case W_SNARE:
        c->ak = decay_k(ms * 0.5f, 0.006f);
        c->qk = decay_k(15, 0.05f);
        c->nzk = decay_k(ms, 0.006f);
        break;
    case W_TIMPANI:
        c->ak = decay_k(ms, 0.006f);
        c->qk = decay_k(80, 0.05f);
        c->nzk = decay_k(12, 0.01f);
        break;
    default:
        break;
    }
}

// Sustained instruments: attack, then a gentle fall, then the release at the note's end.
static float sus_env(const chan_t *c, float attack_ms, float release_ms)
{
    float t = c->pos * (1000.0f / SR), rem = (c->len - c->pos) * (1000.0f / SR);
    float e = t < attack_ms ? t / attack_ms : 1.0f - 0.15f * (t - attack_ms) / (t - attack_ms + 600.0f);
    if (rem < release_ms) e *= rem / release_ms;
    return e < 0 ? 0 : e;
}

static void inst_ctrl(chan_t *c, const note_t *nt)
{
    float t = c->pos * (1000.0f / SR);
    float pitch = 1.0f, index = 0, tone = 0, noise = 0, ratio = 1.0f;
    float vib_depth = 0;
    switch (nt->wave) {
    case W_BELL:
        ratio = 1.4f;  // inharmonic: what makes it a bell rather than a tone
        tone = c->a * (t < 1.5f ? t / 1.5f : 1.0f);
        index = 0.8f + 7.0f * c->q;
        break;
    case W_TRUMPET: {
        float e = sus_env(c, 35, 50);
        tone = e;
        index = 0.7f + 4.3f * e * e;  // brighter as it gets louder: the brass "blat"
        pitch = t < 35 ? 0.97f + 0.03f * t / 35 : 1.0f;
        vib_depth = t > 300 ? 0.004f : 0;
        noise = 0.03f * e;
        break;
    }
    case W_HORN: {
        float e = sus_env(c, 90, 90);
        tone = e;
        index = 0.3f + 2.0f * e * e;
        pitch = t < 90 ? 0.94f + 0.06f * t / 90 : 1.0f;
        vib_depth = t > 400 ? 0.003f : 0;
        noise = 0.08f * e;
        break;
    }
    case W_SWELL: {
        float e = sus_env(c, c->len * (1000.0f / SR), 1);  // the attack is the whole note
        e = e * e;                                          // (slow at first, so it grows from nothing)
        tone = e;
        index = 0.3f + 3.0f * e;
        noise = 0.08f * e;
        break;
    }
    case W_FLUTE: {
        float e = sus_env(c, 70, 60);
        tone = e;
        index = 0.35f * e;
        ratio = 2.0f;  // a touch of the octave
        vib_depth = t > 180 ? 0.006f : 0;
        noise = 0.10f * e;
        break;
    }
    case W_DRUM:
        tone = c->a;
        pitch = 1.0f + 0.9f * c->q;
        index = 0.6f * c->q;
        noise = 0.7f * c->nz;
        break;
    case W_SNARE:
        tone = 0.5f * c->a;
        pitch = 1.0f + 0.6f * c->q;
        noise = 1.0f * c->nz;
        break;
    case W_TIMPANI:
        tone = c->a;
        pitch = 1.0f + 0.04f * c->q;
        index = 0.9f * c->q + 0.2f * c->a;
        ratio = 1.5f;
        noise = 0.25f * c->nz;
        break;
    default:
        break;
    }
    c->a *= c->ak;
    c->q *= c->qk;
    c->nz *= c->nzk;
    if (vib_depth > 0) {
        c->vib += (uint32_t)(5.2 * 4294967296.0 / (SR / CTRL));  // 5.2 Hz
        pitch *= 1.0f + vib_depth * SINE(c->vib) / 32767.0f;
    }
    // Short release on every instrument, so a cut-off note doesn't click.
    uint32_t rem = c->len - c->pos;
    float rel = rem < SR / 200 ? rem / (SR / 200.0f) : 1.0f;
    c->inc_eff = (uint32_t)(c->inc * pitch);
    c->incm = (uint32_t)(c->inc_eff * ratio);
    c->dev = (uint32_t)(index * 20861.0f);  // index radians at full modulator
    c->amp_t = (int32_t)(tone * rel * 30000.0f);
    c->amp_n = (int32_t)(noise * rel * 30000.0f);
}

static inline int32_t inst_sample(chan_t *c)
{
    int32_t m = SINE(c->phm);
    c->phm += c->incm;
    int32_t v = (SINE(c->phase + (uint32_t)m * c->dev) * c->amp_t) >> 15;
    c->phase += c->inc_eff;
    if (c->amp_n) {
        c->lfsr ^= c->lfsr << 13;
        c->lfsr ^= c->lfsr >> 17;
        c->lfsr ^= c->lfsr << 5;
        v += ((int16_t)c->lfsr * c->amp_n) >> 15;
    }
    return v;
}

static void note_begin(chan_t *c)
{
    const note_t *nt = &c->notes[c->idx];
    c->pos = 0;
    c->len = (uint32_t)nt->ms * SR / 1000;
    if (!c->len) c->len = 1;
    if (is_inst(nt->wave)) inst_begin(c, nt);
}

static void update_inc(chan_t *c)
{
    const note_t *nt = &c->notes[c->idx];
    int64_t f = nt->f0 + ((int64_t)((int)nt->f1 - (int)nt->f0) * c->pos) / c->len;
    if (f < 0) f = 0;
    c->inc = (uint32_t)(((uint64_t)f << 32) / SR);
}

static inline int32_t wave_sample(chan_t *c, int wave)
{
    switch (wave) {
    case W_SQUARE:
        return (c->phase & 0x80000000u) ? SQ_AMP : -SQ_AMP;
    case W_TRI: {
        int32_t p = (int32_t)(c->phase >> 16);  // 0..65535
        int32_t s = p < 32768 ? (2 * p - 32768) : (98303 - 2 * p);
        return s;
    }
    case W_SINE:
        return SINE(c->phase) * 3 / 4;
    case W_NOISE:
    default:
        c->lfsr ^= c->lfsr << 13;
        c->lfsr ^= c->lfsr >> 17;
        c->lfsr ^= c->lfsr << 5;
        return (int16_t)c->lfsr * 3 / 4;
    }
}

void synth_play(int slot, const note_t *notes, int n)
{
    if (slot < 0 || slot >= SLOT_COUNT) return;
    chan_t *c = &s_ch[slot];
    if (n <= 0 || !notes) {
        c->active = false;
        return;
    }
    c->notes = notes;
    c->n = n;
    c->idx = 0;
    if (!c->lfsr) c->lfsr = 0x12345u + slot;
    note_begin(c);
    c->active = true;
}

void synth_stop_all(void)
{
    for (int s = 0; s < SLOT_COUNT; s++) s_ch[s].active = false;
}

bool synth_busy(void)
{
    for (int s = 0; s < SLOT_COUNT; s++)
        if (s_ch[s].active) return true;
    return false;
}

void synth_render(int32_t *acc)
{
    for (int s = 0; s < SLOT_COUNT; s++) {
        chan_t *c = &s_ch[s];
        if (!c->active) continue;
        update_inc(c);
        for (int i = 0; i < BLOCK; i++) {
            const note_t *nt = &c->notes[c->idx];
            if (nt->f0 && is_inst(nt->wave)) {
                if (c->pos % CTRL == 0) inst_ctrl(c, nt);
                acc[i] += (inst_sample(c) * nt->vol) >> 8;
            } else if (nt->f0) {
                uint32_t rem = c->len - c->pos;
                int32_t env = 256;
                if (c->pos < ATTACK) env = (int32_t)(c->pos * 256 / ATTACK);
                if (rem < RELEASE && (int32_t)(rem * 256 / RELEASE) < env) env = (int32_t)(rem * 256 / RELEASE);
                int32_t v = wave_sample(c, nt->wave);
                acc[i] += (v * nt->vol * env) >> 16;
                c->phase += c->inc;
            }
            if (++c->pos >= c->len) {
                if (++c->idx >= c->n) {
                    c->active = false;
                    break;
                }
                note_begin(c);
                update_inc(c);
            } else if ((c->pos & 127) == 0) {
                update_inc(c);  // sweep resolution ~2.7 ms
            }
        }
    }
}

int16_t synth_out(int32_t acc, int32_t gain_q16)
{
    int32_t v = (int32_t)(((int64_t)acc * gain_q16) >> 16);
    // Soft limit above LIM_T: a smooth knee towards full scale instead of a hard clip, so voices
    // that overlap at full volume (the drums under the horn, the bells under the fanfare) thicken
    // rather than buzz. Unchanged below the knee (all of it at the default volume).
    int32_t a = v < 0 ? -v : v;
    if (a > LIM_T) {
        int32_t d = a - LIM_T;
        a = LIM_T + (int32_t)((int64_t)LIM_R * d / (LIM_R + d));
        v = v < 0 ? -a : a;
    }
    return (int16_t)v;
}

void synth_init(void)
{
    for (int i = 0; i < (1 << SINE_BITS); i++) s_sine[i] = (int16_t)(sinf(i * 2.0f * (float)M_PI / (1 << SINE_BITS)) * 32767.0f);
}

void synth_reset(void) { memset(s_ch, 0, sizeof s_ch); }
