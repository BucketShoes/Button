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

#define MASTER_DEFAULT 65536 // Q16 master gain (65536 = full); set by audio_set_volume. Loud is the point (contact sport,
                             // yelling). The FS-1540 is rated 0.5 W and on USB-only the
                             // 400 mA charger feeds LEDs + amp, so VSYS may sag on peaks.

#define SR    SYNTH_SR     // highest rate IDF recommends for PDM TX; the clip must match
#define BLOCK SYNTH_BLOCK  // 10 ms

typedef struct {
    int slot;
    const note_t *notes;
    int n;
} cmd_t;


static i2s_chan_handle_t s_tx;
static QueueHandle_t s_q;
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


static void render_block(void)
{
    cmd_t cmd;
    while (xQueueReceive(s_q, &cmd, 0) == pdTRUE) synth_play(cmd.slot, cmd.notes, cmd.n);
    static int32_t acc[BLOCK];
    memset(acc, 0, sizeof acc);
    pcm_mix(acc);
    synth_render(acc);
    for (int i = 0; i < BLOCK; i++) s_buf[i] = synth_out(acc[i], s_master);
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
    if (synth_busy()) return true;
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
            synth_stop_all();
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
    synth_init();

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
