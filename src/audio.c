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

static const char *TAG = "audio";

#define SR        48000      // highest rate IDF recommends for PDM TX; clip must match
#define BLOCK     (SR / 100) // 10 ms
#define MASTER    256        // /256 master volume: full. Loud is the point (contact sport,
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
} chan_t;

static i2s_chan_handle_t s_tx;
static QueueHandle_t s_q;
static chan_t s_ch[SLOT_COUNT];
static int16_t s_sine[256];
static int16_t s_buf[BLOCK];

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
void audio_pcm_start(void) { s_pcm_req = 1; }
void audio_pcm_stop(void) { s_pcm_req = 2; }
bool audio_pcm_playing(void) { return s_pcm_playing || s_pcm_req == 1; }

static void note_begin(chan_t *c)
{
    const note_t *nt = &c->notes[c->idx];
    c->pos = 0;
    c->len = (uint32_t)nt->ms * SR / 1000;
    if (!c->len) c->len = 1;
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
        return s_sine[c->phase >> 24];
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
            if (nt->f0) {
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
        int32_t v = (acc[i] * MASTER) >> 8;
        if (v > 32767) v = 32767;
        if (v < -32767) v = -32767;
        s_buf[i] = (int16_t)v;
    }
}

static void audio_task(void *arg)
{
    for (;;) {
        render_block();
        size_t w;
        i2s_channel_write(s_tx, s_buf, sizeof(s_buf), &w, portMAX_DELAY);
    }
}

void audio_init(void)
{
    for (int i = 0; i < 256; i++) s_sine[i] = (int16_t)(sinf(i * 2.0f * (float)M_PI / 256.0f) * 24000.0f);

    gpio_config_t amp = {
        .pin_bit_mask = 1ULL << PIN_AMP_EN,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&amp);
    gpio_set_level(PIN_AMP_EN, 0);

    s_q = xQueueCreate(16, sizeof(cmd_t));
    pcm_init();

    i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    cc.auto_clear = true;  // underrun -> zeros -> 50% PDM -> silence
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
    ESP_ERROR_CHECK(i2s_channel_enable(s_tx));

    xTaskCreate(audio_task, "audio", 6144, NULL, 10, NULL);

    // Let the PDM midpoint settle through CIN1 (10 ms tau) before the amp wakes.
    vTaskDelay(pdMS_TO_TICKS(60));
    gpio_set_level(PIN_AMP_EN, 1);
    ESP_LOGI(TAG, "PDM audio on IO%d at %d Hz, amp enabled", PIN_PDM, SR);
}

void audio_play(int slot, const note_t *notes, int n)
{
    if (slot < 0 || slot >= SLOT_COUNT || !s_q) return;
    cmd_t c = {.slot = slot, .notes = notes, .n = n};
    xQueueSend(s_q, &c, 0);
}

void audio_stop(int slot) { audio_play(slot, NULL, 0); }
