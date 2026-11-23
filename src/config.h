// Persistent settings (NVS), also the BLE config characteristic payload.
// Layout is part of the BLE protocol (see docs/ble-protocol.md and docs/index.html):
// keep it packed, little-endian, and bump CONFIG_VERSION on any change.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CONFIG_VERSION 4
#define CFG_DEBUG_AWAKE_LED 1  // config_t.debug

enum { MODE_TEAMS = 0, MODE_ONE_TIMER = 1, MODE_TWO_TIMERS = 2 };
#include "sounds.h"  // SOUNDS_*: the sound set (byte 2)

typedef struct __attribute__((packed)) {
    uint8_t version;          //  0 CONFIG_VERSION
    uint8_t mode;             //  1 MODE_*
    uint8_t sound_set;        //  2 SOUNDS_*: the capture build-up, captures, countdown and win (was unused, 0)
    uint8_t count_up;         //  3 0 = count down to 0, 1 = count up (to duration, 0 = no limit)
    uint8_t show_numbers;     //  4 1 = clocks on the displays, 0 = hidden (admins see them in the page)
    uint8_t initial_owner;    //  5 after reset: 0 uncaptured, 1 team 1, 2 team 2
    uint8_t default_capture;  //  6 first capture of an uncaptured button (no taps): 0 random, 1, 2
    uint8_t volume;           //  7 master volume, percent: 0..100 = -40..0 dB, up to 125 = +10 dB (v3: 0..255 = 0..100 %)
    uint16_t hold_ms;         //  8 hold to capture; pause at +3 s, reset at +7 s
    uint8_t led_level;        // 10 0..255 LED brightness scale
    uint8_t debug;            // 11 debug options: bit0 awake LED (red LED lit while out of light sleep)
    uint32_t duration_ms[2];  // 12, 16 per team; one timer uses [0]
    uint8_t colour[2][3];     // 20, 23 RGB
    char name[16];            // 26 NUL-terminated, empty = default "Button-XXXX"
    // Idle times in minutes (idle = no press, no page command); 0 = never.
    uint8_t sparse_min;       // 42 every 2nd LED off (default 11)
    uint8_t dim_min;          // 43 10 % brightness (31)
    uint8_t auto_off_min;     // 44 auto-off: dark, still in the game and the network; a tap is back (61)
    uint8_t switch_off_min;   // 45 auto-off becomes off, out of the network (121); not with a countdown running
} config_t;                   // 46 bytes (fits a network command, 48 at most)

_Static_assert(sizeof(config_t) == 46, "config_t layout is part of the BLE protocol");
#define CONFIG_V2_LEN 42  // version 2 was the same without the idle times
#define CONFIG_VOLUME_MAX 125

void config_defaults(config_t *c);
void config_sanitise(config_t *c);  // clamps everything into range
void config_load(config_t *c);      // defaults if nothing (valid) is stored
void config_save(const config_t *c);
// The power state the user last chose (0 on, 1 off, 2 shipping), kept apart from the settings so
// Defaults doesn't touch it. Missing: on. Call after config_load (which starts NVS).
uint8_t config_load_power(void);
void config_save_power(uint8_t p);
// True once after each new firmware image (it stores the image's hash).
bool config_firmware_changed(void);
// Small blobs in the same NVS namespace (the saved game). Load is true only for exactly n bytes.
bool config_blob_load(const char *key, void *p, size_t n);
void config_blob_save(const char *key, const void *p, size_t n);  // skips an unchanged value
void config_blob_erase(const char *key);
