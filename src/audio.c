#include "audio.h"
#include "board.h"
#include <math.h>
#include <stdbool.h>
#include <string.h>
#include "esp_partition.h"
#include "driver/i2s_pdm.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "audio";

#define SR        48000      // highest rate IDF recommends for PDM TX; clip must match
#define BLOCK     (SR / 100) // 10 ms
#define MASTER_DEFAULT 65536 // Q16 master gain (65536 = full); set by audio_set_volume. Loud is the point (contact sport,
                             // yelling). The FS-1540 is rated 0.5 W and on USB-only the
                             // 400 mA charger feeds LEDs + amp, so VSYS may sag on peaks.
#define SQ_AMP    28000      // square wave amplitude
#define ATTACK    (SR / 500) // 2 ms
#define RELEASE   (SR / 125) // 8 ms

typedef struct {
    int slot;
    const note_t *notes;
    int n;
} cmd_t;

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

static i2s_chan_handle_t s_tx;
static QueueHandle_t s_q;
static chan_t s_ch[SLOT_COUNT];
#define SINE_BITS 10
static int16_t s_sine[1 << SINE_BITS];  // full scale (32767)
#define SINE(ph) s_sine[(ph) >> (32 - SINE_BITS)]
#define CTRL 32                          // instrument control period in samples (0.67 ms)
#define LIM_T 20000                      // soft limiter knee (see render_block)
#define LIM_R (32767 - LIM_T)
static int16_t s_buf[BLOCK];
static volatile int32_t s_master = MASTER_DEFAULT;

// PCM clip state. s_pcm_req is set by other tasks, consumed by the audio task.
static const esp_partition_t *s_pcm_part;
static uint32_t s_pcm_count;
static uint32_t s_pcm_pos;
static volatile bool s_pcm_playing;
static volatile int s_pcm_req;  // 0 none, 1 start, 2 stop

static void pcm_init(void)
{
    s_pcm_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, (esp_partition_subtype_t)0x40, "audio");
    if (!s_pcm_part) {
        ESP_LOGW(TAG, "no audio partition");
        return;
    }
    uint8_t hdr[16];
    if (esp_partition_read(s_pcm_part, 0, hdr, sizeof hdr) != ESP_OK || memcmp(hdr, "PCM1", 4) != 0) {
        ESP_LOGW(TAG, "audio partition empty (flash it with the upload_audio target)");
        s_pcm_part = NULL;
        return;
    }
    uint32_t rate, count;
    memcpy(&rate, hdr + 4, 4);
    memcpy(&count, hdr + 8, 4);
    if (rate != SR || count == 0 || 16 + (uint64_t)count * 2 > s_pcm_part->size) {
        ESP_LOGW(TAG, "audio clip header bad (rate %lu, samples %lu)", (unsigned long)rate, (unsigned long)count);
        s_pcm_part = NULL;
        return;
    }
    s_pcm_count = count;
    ESP_LOGI(TAG, "audio clip: %lu samples, %.1f s", (unsigned long)count, count / (float)SR);
}

static void pcm_mix(int32_t *acc)
{
    int req = s_pcm_req;
    if (req) {
        s_pcm_req = 0;
        if (req == 1 && s_pcm_part) {
            s_pcm_pos = 0;
            s_pcm_playing = true;
        } else {
            s_pcm_playing = false;
        }
    }
    if (!s_pcm_playing) return;
    static int16_t tmp[BLOCK];
    uint32_t n = s_pcm_count - s_pcm_pos;
    if (n > BLOCK) n = BLOCK;
    if (esp_partition_read(s_pcm_part, 16 + s_pcm_pos * 2, tmp, n * 2) != ESP_OK) {
        s_pcm_playing = false;
        return;
    }
    for (uint32_t i = 0; i < n; i++) acc[i] += tmp[i];
    s_pcm_pos += n;
    if (s_pcm_pos >= s_pcm_count) s_pcm_playing = false;
}

bool audio_pcm_available(void) { return s_pcm_part != NULL; }
static TaskHandle_t s_task;
void audio_pcm_start(void)
{
    s_pcm_req = 1;
    if (s_task) xTaskNotifyGive(s_task);
}
void audio_pcm_stop(void) { s_pcm_req = 2; }
bool audio_pcm_playing(void) { return s_pcm_playing || s_pcm_req == 1; }

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

static void render_block(void)
{
    cmd_t cmd;
    while (xQueueReceive(s_q, &cmd, 0) == pdTRUE) {
        chan_t *c = &s_ch[cmd.slot];
        if (cmd.n <= 0 || !cmd.notes) {
            c->active = false;
            continue;
        }
        c->notes = cmd.notes;
        c->n = cmd.n;
        c->idx = 0;
        if (!c->lfsr) c->lfsr = 0x12345u + cmd.slot;
        note_begin(c);
        c->active = true;
    }

    static int32_t acc[BLOCK];
    memset(acc, 0, sizeof acc);
    pcm_mix(acc);
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
    for (int i = 0; i < BLOCK; i++) {
        int32_t v = (int32_t)(((int64_t)acc[i] * s_master) >> 16);
        // Soft limit above LIM_T: a smooth knee towards full scale instead of a hard clip, so voices
        // that overlap at full volume (the drums under the horn, the bells under the fanfare) thicken
        // rather than buzz. Unchanged below the knee (all of it at the default volume).
        int32_t a = v < 0 ? -v : v;
        if (a > LIM_T) {
            int32_t d = a - LIM_T;
            a = LIM_T + (int32_t)((int64_t)LIM_R * d / (LIM_R + d));
            v = v < 0 ? -a : a;
        }
        s_buf[i] = (int16_t)v;
    }
}

// Power: amp and PDM both stop QUIET_MS after the last sound (a running PDM
// holds a lock that stops the chip light-sleeping), unless main asks to keep
// them awake (final minute of a countdown). Waking takes a fixed time, so
// the first sound of an interaction is always late by the same amount, and
// nothing is lost: sounds wait in the queue until the speaker is ready.
//   PDM and amp start together (the PDM midpoint settling through CIN1 can
//   give a small click; a click beats a longer delay). Then AMP_WAKE_MS of
//   silence covers the amp's own turn-on, so the sound isn't cut short.
//   If the start of sounds is still clipped, raise AMP_WAKE_MS; if nothing
//   is clipped, try lowering it.
// Light-sleep wake itself is well under 1 ms; it isn't part of this.
#define QUIET_MS      1100
#define AMP_WAKE_MS   120

static volatile bool s_allowed = true;  // false while VSYS is off (powered down)
static volatile bool s_keep_awake;
static bool s_i2s_on, s_amp_on;

static bool busy(void)
{
    if (s_pcm_playing || s_pcm_req == 1 || uxQueueMessagesWaiting(s_q)) return true;
    for (int s = 0; s < SLOT_COUNT; s++)
        if (s_ch[s].active) return true;
    return false;
}

static void amp(bool on)
{
    if (on == s_amp_on) return;
    s_amp_on = on;
    gpio_set_level(PIN_AMP_EN, on);
}

// Queue ms of silence. Latency is set by what's queued ahead of a sound, not
// by waiting, so the sound written next starts exactly this much later.
static void write_zeros_ms(int ms)
{
    memset(s_buf, 0, sizeof s_buf);
    size_t w;
    for (int i = 0; i < ms / 10; i++) i2s_channel_write(s_tx, s_buf, sizeof s_buf, &w, portMAX_DELAY);
}

static void wake(void)
{
    if (s_i2s_on && s_amp_on) return;
    if (!s_i2s_on) {
        i2s_channel_enable(s_tx);
        s_i2s_on = true;
    }
    amp(true);
    write_zeros_ms(AMP_WAKE_MS);
}

static void i2s_off(void)
{
    amp(false);
    if (!s_i2s_on) return;
    i2s_channel_disable(s_tx);
    s_i2s_on = false;
}

static void audio_task(void *arg)
{
    uint32_t quiet_ms = 0;
    for (;;) {
        if (!s_allowed) {
            i2s_off();
            cmd_t c;
            while (xQueueReceive(s_q, &c, 0) == pdTRUE) {}  // drop sounds while powered down
            for (int s = 0; s < SLOT_COUNT; s++) s_ch[s].active = false;
            s_pcm_playing = false;
            s_pcm_req = 0;
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }
        if (!s_i2s_on && !busy() && !s_keep_awake) {
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);  // sleep until audio_play / pcm start / keep awake
            continue;
        }
        if (busy() || s_keep_awake) {
            quiet_ms = 0;
            wake();  // no-op when already awake
        }
        render_block();
        size_t w;
        i2s_channel_write(s_tx, s_buf, sizeof(s_buf), &w, portMAX_DELAY);
        if (!busy() && !s_keep_awake) {
            quiet_ms += 10;
            if (quiet_ms >= QUIET_MS) i2s_off();  // amp first, then PDM
        }
    }
}

void audio_keep_awake(bool on)
{
    if (on == s_keep_awake) return;
    s_keep_awake = on;
    if (on && s_task) xTaskNotifyGive(s_task);
}

void audio_init(void)
{
    for (int i = 0; i < (1 << SINE_BITS); i++) s_sine[i] = (int16_t)(sinf(i * 2.0f * (float)M_PI / (1 << SINE_BITS)) * 32767.0f);

    gpio_config_t io = {
        .pin_bit_mask = 1ULL << PIN_AMP_EN,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&io);
    gpio_set_level(PIN_AMP_EN, 0);

    s_q = xQueueCreate(16, sizeof(cmd_t));
    pcm_init();

    i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    cc.auto_clear = true;  // underrun -> zeros -> 50% PDM -> silence
    // 6 x 5 ms DMA buffers (the IDF default): 30 ms of margin against the task
    // being held up. It only adds latency to sounds that start while already awake.
    cc.dma_desc_num = 6;
    cc.dma_frame_num = SR / 200;
    ESP_ERROR_CHECK(i2s_new_channel(&cc, &s_tx, NULL));

    i2s_pdm_tx_config_t pc = {
        .clk_cfg = I2S_PDM_TX_CLK_DAC_DEFAULT_CONFIG(SR),
        .slot_cfg = I2S_PDM_TX_SLOT_DAC_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .clk = I2S_GPIO_UNUSED,
            .dout = PIN_PDM,
            .dout2 = I2S_GPIO_UNUSED,
            .invert_flags = {.clk_inv = false},
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_pdm_tx_mode(s_tx, &pc));

    xTaskCreate(audio_task, "audio", 6144, NULL, 10, &s_task);
    ESP_LOGI(TAG, "PDM audio on IO%d at %d Hz (amp and PDM on demand)", PIN_PDM, SR);
}

void audio_set_allowed(bool on)
{
    s_allowed = on;
    if (s_task) xTaskNotifyGive(s_task);
}

void audio_play(int slot, const note_t *notes, int n)
{
    if (slot < 0 || slot >= SLOT_COUNT || !s_q) return;
    cmd_t c = {.slot = slot, .notes = notes, .n = n};
    xQueueSend(s_q, &c, 0);
    if (s_task) xTaskNotifyGive(s_task);
}

void audio_stop(int slot) { audio_play(slot, NULL, 0); }

void audio_set_volume(int v)
{
    // 0 = mute, 1..255 = -40 dB .. 0 dB, evenly spaced in dB (loudness, not amplitude).
    if (v <= 0) {
        s_master = 0;
        return;
    }
    if (v > 255) v = 255;
    s_master = (int32_t)(65536.0f * powf(10.0f, (v / 255.0f - 1.0f) * 2.0f));
}
