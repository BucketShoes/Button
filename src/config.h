// Persistent settings (NVS), also the BLE config characteristic payload.
// Layout is part of the BLE protocol (see docs/ble-protocol.md and docs/index.html):
// keep it packed, little-endian, and bump CONFIG_VERSION on any change.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define CONFIG_VERSION 2

enum { MODE_TEAMS = 0, MODE_ONE_TIMER = 1, MODE_TWO_TIMERS = 2 };

typedef struct __attribute__((packed)) {
    uint8_t version;          //  0 CONFIG_VERSION
    uint8_t mode;             //  1 MODE_*
    uint8_t timer_team;       //  2 MODE_ONE_TIMER: whose clock (0 = team 1, 1 = team 2)
    uint8_t count_up;         //  3 0 = count down to 0, 1 = count up (to duration, 0 = no limit)
    uint8_t show_numbers;     //  4 1 = clocks on the displays, 0 = hidden (admins see them in the page)
    uint8_t initial_owner;    //  5 after reset: 0 uncaptured, 1 team 1, 2 team 2
    uint8_t default_capture;  //  6 first capture of an uncaptured button (no taps): 0 random, 1, 2
    uint8_t volume;           //  7 0..255 master volume (255 = full)
    uint16_t hold_ms;         //  8 hold to capture; pause at +3 s, reset at +7 s
    uint8_t led_level;        // 10 0..255 LED brightness scale
    uint8_t reserved1;        // 11
    uint32_t duration_ms[2];  // 12, 16
    uint8_t colour[2][3];     // 20, 23 RGB
    char name[16];            // 26 NUL-terminated, empty = default "Button-XXXX"
} config_t;                   // 42 bytes

_Static_assert(sizeof(config_t) == 42, "config_t layout is part of the BLE protocol");

void config_defaults(config_t *c);
void config_sanitise(config_t *c);  // clamps everything into range
void config_load(config_t *c);      // defaults if nothing (valid) is stored
void config_save(const config_t *c);
