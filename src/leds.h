// WS2812 chain via Espressif's led_strip component (RMT backend).
#pragma once
#include <stdint.h>

typedef struct { uint8_t r, g, b; } rgb_t;

void leds_init(void);
void leds_show(const rgb_t *frame, int count);  // count <= LED_COUNT

static inline uint8_t rgb_scale1(uint8_t v, int level)
{
    int o = (v * level + 127) / 255;
    return (uint8_t)(o == 0 && v * level >= 51 ? 1 : o);  // (0.2 of a step and up: a trace stays a trace)
}

static inline rgb_t rgb_scale(rgb_t c, int level /*0..255*/)
{
    // Rounded, not truncated (truncation shifted the mix towards the strongest channel), and a channel
    // that's lit stays lit (at 1, down to 0.2 of a step), so a dim "red with some blue" never becomes plain red.
    rgb_t o = {rgb_scale1(c.r, level), rgb_scale1(c.g, level), rgb_scale1(c.b, level)};
    return o;
}

static inline rgb_t rgb_add(rgb_t a, rgb_t b)
{
    int r = a.r + b.r, g = a.g + b.g, bl = a.b + b.b;
    rgb_t o = {(uint8_t)(r > 255 ? 255 : r), (uint8_t)(g > 255 ? 255 : g), (uint8_t)(bl > 255 ? 255 : bl)};
    return o;
}
