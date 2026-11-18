// Two GN1650 (TM1650 clone) 4-digit drivers, each on its own bit-banged bus.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define SEG_COLON 0x80  // DP bit; on this display only digits 3 and 4 have it, as the colon

void display_init(void);  // needs +3V3_SW on (PWR_EN high)

// idx 0 = DISP1 (team 1, north), 1 = DISP2 (team 2, south).
// brightness 1 (dim) .. 8 (full); 0 = display off.
// Writes only what changed since the last call.
void display_set(int idx, const uint8_t segs[4], int brightness);

// Rewrite everything on both drivers (they forget their state if +3V3_SW blips).
void display_force_refresh(void);

uint8_t display_digit(int d);
uint8_t display_char(char c);
void display_text(uint8_t segs[4], const char *s);         // up to 4 chars
void display_number(uint8_t segs[4], int n);               // right aligned, 0..9999
void display_colon(uint8_t segs[4], bool on);

uint32_t display_nak_count(int idx);  // missing ACKs seen (driver absent / bus fault)
