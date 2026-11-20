// Button network over BLE links (see design/ble-network.md and docs/ble-protocol.md).
// Every button holds every button's latest state, status and settings; each button is the
// authority on its own. Items spread on change over the links to other buttons, relayed only
// where a neighbour has no direct link to where the item came from. A once-a-second heartbeat
// with a digest of everything known catches anything a dropped link lost. Phones (admins) get
// everything on their active link and send commands, which every button passes on in order and
// exactly once, and the target acts on unless they have expired.
// All of it runs on the game loop (mesh_tick); ble.c only carries frames.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MESH_ALL 0xFFFF  // command target: every button

// Game state, sent when it changes (plus every 10 s). Clocks and the hold time are as of t;
// receivers extrapolate from there.
typedef struct __attribute__((packed)) {
    uint32_t t;           //  0 network ms when generated (filled in by mesh)
    uint8_t flags;        //  4 as the state characteristic's flags
    int8_t owner;         //  5
    int8_t winner;        //  6
    int8_t target;        //  7
    uint8_t hold_stage;   //  8
    uint16_t held_ms;     //  9
    int32_t time_ms[2];   // 11, 15
    uint8_t power;        // 19
    uint8_t flags2;       // 20
} mesh_hot_t;             // 21 bytes
_Static_assert(sizeof(mesh_hot_t) == 21, "mesh_hot_t layout is part of the BLE protocol");

// Status, every 30 s and when the links change. The mesh appends the neighbour list.
typedef struct __attribute__((packed)) {
    uint32_t t;            //  0 network ms (filled in by mesh)
    uint16_t vsys_mv;      //  4
    uint16_t vbus_mv;      //  6
    int16_t batt_pct;      //  8
    uint16_t batt_mv;      // 10
    uint8_t phones;        // 12 phone (and other non-button) connections
    uint8_t reset_reason;  // 13
    uint32_t uptime_s;     // 14
    uint32_t boots;        // 18
} mesh_status_t;           // 22 bytes, then u8 n, n x (u16 node, i8 rssi, u8 phy)
_Static_assert(sizeof(mesh_status_t) == 22, "mesh_status_t layout is part of the BLE protocol");

enum { MESH_CMD_OP = 0, MESH_CMD_CONFIG = 1 };
typedef void (*mesh_cmd_fn)(uint8_t kind, const uint8_t *d, int n);

void mesh_init(mesh_cmd_fn on_cmd);
void mesh_tick(uint32_t now);
void mesh_publish_hot(const mesh_hot_t *h);
void mesh_publish_status(const mesh_status_t *s);
void mesh_publish_config(const void *cfg, size_t n);
uint32_t mesh_net_ms(void);
int mesh_button_links(void);
