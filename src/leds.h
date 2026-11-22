// WS2812 chain via Espressif's led_strip component (RMT backend).
#pragma once
#include <stdint.h>

typedef struct { uint8_t r, g, b; } rgb_t;

void leds_init(void);
void leds_show(const rgb_t *frame, int count);  // count <= LED_COUNT

static inline rgb_t rgb_scale(rgb_t c, int level /*0..255*/)
{
    // (Rounded, not truncated: at low brightness truncation shifted the mix towards the strongest channel.)
    rgb_t o = {(uint8_t)((c.r * level + 127) / 255), (uint8_t)((c.g * level + 127) / 255), (uint8_t)((c.b * level + 127) / 255)};
    return o;
}

static inline rgb_t rgb_add(rgb_t a, rgb_t b)
{
    int r = a.r + b.r, g = a.g + b.g, bl = a.b + b.b;
    rgb_t o = {(uint8_t)(r > 255 ? 255 : r), (uint8_t)(g > 255 ? 255 : g), (uint8_t)(bl > 255 ? 255 : bl)};
    return o;
}
