// BLE peripheral (NimBLE): one GATT service the web page talks to.
// Advertises on legacy 1M (Chrome can only connect through that) and Coded (range); every link is
// then asked to switch to Coded PHY. Each central can read its own link (PHY, interval, RSSI).
// The NimBLE host runs in its own task; writes are queued for the game loop
// to pick up with ble_poll(), and the game loop publishes state/config here.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BLE_MSG_MAX 64

typedef enum { BLE_MSG_COMMAND, BLE_MSG_CONFIG } ble_msg_kind_t;

typedef struct {
    uint8_t kind;  // ble_msg_kind_t
    uint8_t len;
    uint8_t data[BLE_MSG_MAX];
} ble_msg_t;

void ble_init(const char *name);
void ble_set_name(const char *name);
bool ble_poll(ble_msg_t *m);                       // non-blocking
void ble_publish_state(const void *p, size_t n);   // read value + notify subscribers
void ble_publish_config(const void *p, size_t n);  // read value + notify subscribers
int ble_connections(void);
void ble_set_slow(bool slow);  // true (powered off): legacy ~1 s, Coded ~2 s, +6 dBm; false: 100 ms / 500 ms, +9 dBm
