// BLE (NimBLE): one GATT service the web page and other buttons talk to.
// Advertises on legacy 1M (Chrome can only connect through that) and Coded (range); phone links are
// then asked to switch to Coded PHY. Now and then it scans on Coded for other buttons and connects
// to them as central, so the buttons form a network (see mesh.c). Each central can read its own
// link (PHY, interval, RSSI).
// The NimBLE host runs in its own task; writes and network frames are queued for the game loop
// to pick up with ble_poll() / ble_poll_event(), and the game loop publishes state/config here.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BLE_MSG_MAX 64
#define BLE_FRAME_MAX 244  // ATT payload with a 247 MTU

typedef enum { BLE_MSG_COMMAND, BLE_MSG_CONFIG } ble_msg_kind_t;

typedef struct {
    uint8_t kind;  // ble_msg_kind_t
    uint8_t len;
    uint8_t data[BLE_MSG_MAX];
} ble_msg_t;

// Network link events for the mesh, in order per connection.
typedef enum {
    BLE_EV_UP,     // we are central and the peer's network characteristic is ready to use
    BLE_EV_FRAME,  // a network frame arrived (written by a central, or notified by a peripheral)
    BLE_EV_DOWN,   // disconnected
} ble_ev_kind_t;

typedef struct {
    uint8_t kind;  // ble_ev_kind_t
    uint8_t central;  // we are central on this link
    uint16_t conn;
    uint16_t len;
    uint8_t data[BLE_FRAME_MAX];
} ble_ev_t;

void ble_init(const char *name);
void ble_set_name(const char *name);
bool ble_poll(ble_msg_t *m);                       // non-blocking
void ble_publish_state(const void *p, size_t n);   // read value + notify subscribers
void ble_publish_config(const void *p, size_t n);  // read value + notify subscribers
int ble_connections(void);
// This task gets a notification (xTaskNotifyGive) whenever a network event is queued.
void ble_set_notify_task(void *task);
void ble_set_slow(bool slow);  // true (powered off): legacy only, every 2-2.5 s, +6 dBm; no Coded adverts or scanning,
                               // so no button links; phone links asked for 400-500 ms. false: legacy 100 ms +
                               // Coded 500 ms, +9 dBm, phone links 30-50 ms (1M) or 80-100 ms (Coded)

// Network transport (for mesh.c, called from the game loop).
uint16_t ble_node_id(void);  // last two bytes of the BT MAC (as in "Button-XXXX"); 0 until the stack is up
bool ble_poll_event(ble_ev_t *e);
int ble_send_frame(uint16_t conn, const uint8_t *p, size_t n);  // 0 or a NimBLE error (BLE_HS_ENOMEM: retry later)
size_t ble_frame_max(uint16_t conn);                            // largest frame worth sending on this link
uint32_t ble_conn_itvl_ms(uint16_t conn);                       // 0 if not connected
bool ble_link_quality(uint16_t conn, int8_t *rssi, uint8_t *phy);
void ble_drop(uint16_t conn);
