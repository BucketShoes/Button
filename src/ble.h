// BLE peripheral (NimBLE): one GATT service the web page talks to.
// Legacy advertising so Chrome (Web Bluetooth) can find and connect.
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
void ble_set_slow(bool slow);  // true: ~1 s advert interval, +6 dBm (powered off); false: 100 ms, +9 dBm
