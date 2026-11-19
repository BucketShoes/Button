# BLE protocol (version 3)

The button is a BLE peripheral using legacy advertising, so Chrome's Web Bluetooth can find it. It advertises
the service UUID and puts its name in the scan response. The name is the one set in the settings, or
`Button-XXXX` (the last 4 hex digits of the BT MAC). Up to 4 centrals can connect at once.

All values are little-endian. UUIDs are `5c0b000N-7a1e-4d2b-9f3a-6b1e2d3c4f50`:

| N | What | Access |
|---|---|---|
| 1 | service | |
| 2 | state (36 bytes) | read, notify (5 Hz on, 1 Hz off) |
| 3 | config (42 bytes) | read, write, notify (on change) |
| 4 | command | write |

## State

| Offset | Type | Field |
|---|---|---|
| 0 | u8 | protocol version (3) |
| 1 | u8 | flags: bit0 running, bit1 won, bit2 unused (was lock), bit3 face held, bit4 identifying, bit5 USB power, bit6 music clip playing |
| 2 | i8 | owner (-1 uncaptured, 0 team 1, 1 team 2) |
| 3 | i8 | winner (-1 none) |
| 4 | i8 | next capture target with no taps, or the current hold's target while held (-1 random) |
| 5 | u8 | hold stage while held: 0 building, 1 captured, 2 paused, 3 reset |
| 6 | u16 | held ms |
| 8 | i32 ×2 | team 1 and team 2 clock values in ms, as shown |
| 16 | u16 | VSYS mV |
| 18 | u16 | VBUS mV |
| 20 | i16 | battery % estimate (can be below 0 or above 100) |
| 22 | u8 | connected centrals |
| 23 | u8 | reset reason of the last boot (esp_reset_reason_t: 1 power-on, 8 woke from off, 9 brown-out, 4 crash, 5-7 watchdog) |
| 24 | u32 | uptime s |
| 28 | u32 | boots since power-on (more than 1 means it has rebooted) |
| 32 | u8 | power: 0 on, 1 peek (woken, showing battery), 2 off |
| 33 | u8 | bit0 dimmed (idle 1 h), bit1 battery low warning, bit2 sparse LEDs (idle 10 min) |
| 34 | u16 | battery mV: long average of VSYS while on; last unloaded reading while off; 0 = unknown |

## Config

This is the `config_t` struct in [src/config.h](../src/config.h). The button saves it to NVS when it's written.

| Offset | Type | Field |
|---|---|---|
| 0 | u8 | config version (2); writes with a different version are ignored |
| 1 | u8 | mode: 0 teams only, 1 one timer, 2 two timers |
| 2 | u8 | timer team (one-timer mode) |
| 3 | u8 | count up (0/1) |
| 4 | u8 | show numbers (0/1) |
| 5 | u8 | owner after reset: 0 uncaptured, 1 team 1, 2 team 2 |
| 6 | u8 | first capture of an uncaptured button with no taps: 0 random, 1, 2 |
| 7 | u8 | volume: 0 mute, 1..255 = -40..0 dB (default 128, about -20 dB) |
| 8 | u16 | hold ms (500..60000); pause at +3 s, reset at +7 s |
| 10 | u8 | LED brightness 0..255 (default 128) |
| 11 | u8 | reserved |
| 12 | u32 ×2 | durations in ms |
| 20 | u8 ×6 | team 1 RGB, team 2 RGB |
| 26 | char ×16 | name, NUL-terminated (empty = default) |

- **Mode, timer team or direction changes:** the game resets.
- **Other changes:** they take effect at once only if nothing has happened since the last reset.

## Commands

The first byte is the opcode.

| Op | Args | |
|---|---|---|
| 1 | | start (clocks run) |
| 2 | | pause |
| 3 | | reset |
| 4 | [u16 ms] | identify (default 3000, max 10000) |
| 5 | u8 team, i32 ms | set clock value |
| 6 | u8 team, i32 ms | add to clock value (negative to subtract); undoes a win if the winner is no longer at the line |
| 7 | i8 owner | set holder (-1 uncaptured) |
| 8 | u8 id | play a sound (see below) |
| 9 | | stop sounds and the music clip |
| 10 | | (removed: was lock) |
| 11 | | show battery |
| 12 | | restore default settings |
| 13 | | power off |
| 14 | | power on |
| 15 | | reboot (settings kept) |

Sound ids:

| Id | Sound | Id | Sound |
|---|---|---|---|
| 0 | team 1 capture | 7 | countdown blip |
| 1 | team 2 capture | 8 | whistle |
| 2 | win fanfare | 9 | horn |
| 3 | pause | 10 | battery chirp |
| 4 | reset | 11 | startup |
| 5 | ding | 12 | diagnostic |
| 6 | doom | 13 | music clip |

There is no pairing or PIN yet, so anyone with the page can connect.

While off, the button keeps advertising slowly (about every 1 s, +6 dBm), so the page can still connect. Identify, battery and play sound wake it briefly (peek). Other commands just change its state, and power on turns it on.
