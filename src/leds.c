#include "leds.h"
#include "board.h"
#include "led_strip.h"
#include "esp_log.h"

static const char *TAG = "leds";
static led_strip_handle_t s_strip;

void leds_init(void)
{
    led_strip_config_t cfg = {
        .strip_gpio_num = PIN_LED_DIN,
        .max_leds = LED_COUNT,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .flags.invert_out = false,
    };
    led_strip_rmt_config_t rmt = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
        .mem_block_symbols = 0,
        .flags.with_dma = false,
    };
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&cfg, &rmt, &s_strip));
    led_strip_clear(s_strip);
    ESP_LOGI(TAG, "WS2812 chain on IO%d, %d LEDs (8 board + %d ext)", PIN_LED_DIN, LED_COUNT, LED_COUNT_EXT);
}

void leds_show(const rgb_t *frame, int count)
{
    for (int i = 0; i < count && i < LED_COUNT; i++) {
        led_strip_set_pixel(s_strip, i, frame[i].r, frame[i].g, frame[i].b);
    }
    led_strip_refresh(s_strip);
}
