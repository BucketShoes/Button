#include "ble.h"
#include <string.h>
#include "esp_bt.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *TAG = "ble";

// 5c0b000N-7a1e-4d2b-9f3a-6b1e2d3c4f50 (NimBLE wants the bytes little-endian).
#define BTN_UUID(n) \
    BLE_UUID128_INIT(0x50, 0x4f, 0x3c, 0x2d, 0x1e, 0x6b, 0x3a, 0x9f, 0x2b, 0x4d, 0x1e, 0x7a, (n), 0x00, 0x0b, 0x5c)
static const ble_uuid128_t SVC_UUID = BTN_UUID(1);
static const ble_uuid128_t STATE_UUID = BTN_UUID(2);
static const ble_uuid128_t CONFIG_UUID = BTN_UUID(3);
static const ble_uuid128_t CMD_UUID = BTN_UUID(4);

enum { CHR_STATE = 1, CHR_CONFIG, CHR_CMD };

#define VAL_MAX 64

static SemaphoreHandle_t s_mu;
static QueueHandle_t s_q;
static uint8_t s_state[VAL_MAX], s_config[VAL_MAX];
static size_t s_state_len, s_config_len;
static uint16_t s_state_h, s_config_h;
static uint8_t s_own_addr_type;
static volatile bool s_synced;
static volatile int s_conns;
static volatile bool s_slow;  // "off": 1 s adverts at lower power

static int chr_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg);

static const struct ble_gatt_svc_def SVCS[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &SVC_UUID.u,
        .characteristics =
            (struct ble_gatt_chr_def[]){
                {
                    .uuid = &STATE_UUID.u,
                    .access_cb = chr_access,
                    .arg = (void *)CHR_STATE,
                    .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
                    .val_handle = &s_state_h,
                },
                {
                    .uuid = &CONFIG_UUID.u,
                    .access_cb = chr_access,
                    .arg = (void *)CHR_CONFIG,
                    .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_NOTIFY,
                    .val_handle = &s_config_h,
                },
                {
                    .uuid = &CMD_UUID.u,
                    .access_cb = chr_access,
                    .arg = (void *)CHR_CMD,
                    .flags = BLE_GATT_CHR_F_WRITE,
                },
                {0},
            },
    },
    {0},
};

static int chr_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    int which = (int)(intptr_t)arg;
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        uint8_t buf[VAL_MAX];
        size_t n = 0;
        xSemaphoreTake(s_mu, portMAX_DELAY);
        if (which == CHR_STATE) {
            n = s_state_len;
            memcpy(buf, s_state, n);
        } else if (which == CHR_CONFIG) {
            n = s_config_len;
            memcpy(buf, s_config, n);
        }
        xSemaphoreGive(s_mu);
        return os_mbuf_append(ctxt->om, buf, n) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        ble_msg_t m = {.kind = which == CHR_CONFIG ? BLE_MSG_CONFIG : BLE_MSG_COMMAND};
        uint16_t len = 0;
        if (OS_MBUF_PKTLEN(ctxt->om) > BLE_MSG_MAX) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        if (ble_hs_mbuf_to_flat(ctxt->om, m.data, sizeof m.data, &len) != 0) return BLE_ATT_ERR_UNLIKELY;
        m.len = (uint8_t)len;
        if (xQueueSend(s_q, &m, 0) != pdTRUE) return BLE_ATT_ERR_INSUFFICIENT_RES;
        return 0;
    }
    return BLE_ATT_ERR_UNLIKELY;
}

static int gap_event(struct ble_gap_event *ev, void *arg);

static void advertise(void)
{
    if (!s_synced) return;
    struct ble_hs_adv_fields f = {0};
    f.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    f.uuids128 = &SVC_UUID;
    f.num_uuids128 = 1;
    f.uuids128_is_complete = 1;
    int rc = ble_gap_adv_set_fields(&f);
    if (rc) ESP_LOGW(TAG, "adv fields: %d", rc);

    // Name in the scan response (the 128-bit UUID fills most of the advert).
    const char *name = ble_svc_gap_device_name();
    struct ble_hs_adv_fields r = {0};
    r.name = (const uint8_t *)name;
    r.name_len = (uint8_t)strlen(name);
    r.name_is_complete = 1;
    rc = ble_gap_adv_rsp_set_fields(&r);
    if (rc) ESP_LOGW(TAG, "scan rsp fields: %d", rc);

    struct ble_gap_adv_params p = {0};
    p.conn_mode = BLE_GAP_CONN_MODE_UND;
    p.disc_mode = BLE_GAP_DISC_MODE_GEN;
    p.itvl_min = s_slow ? BLE_GAP_ADV_ITVL_MS(1000) : BLE_GAP_ADV_ITVL_MS(100);
    p.itvl_max = s_slow ? BLE_GAP_ADV_ITVL_MS(1100) : BLE_GAP_ADV_ITVL_MS(150);
    esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_ADV, s_slow ? ESP_PWR_LVL_P6 : ESP_PWR_LVL_P9);
    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER, &p, gap_event, NULL);
    if (rc && rc != BLE_HS_EALREADY) ESP_LOGI(TAG, "not advertising (%d, %d connected)", rc, s_conns);
}

static int gap_event(struct ble_gap_event *ev, void *arg)
{
    switch (ev->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (ev->connect.status == 0) {
            s_conns++;
            ESP_LOGI(TAG, "connected (handle %d, %d total)", ev->connect.conn_handle, s_conns);
        }
        advertise();  // stay visible for more phones, while there are free slots
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        if (s_conns > 0) s_conns--;
        ESP_LOGI(TAG, "disconnected (reason 0x%x, %d left)", ev->disconnect.reason, s_conns);
        advertise();
        break;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        advertise();
        break;
    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "mtu %d", ev->mtu.value);
        break;
    default:
        break;
    }
    return 0;
}

static void on_sync(void)
{
    ble_hs_util_ensure_addr(0);
    ble_hs_id_infer_auto(0, &s_own_addr_type);
    s_synced = true;
    advertise();
    ESP_LOGI(TAG, "advertising as \"%s\"", ble_svc_gap_device_name());
}

static void on_reset(int reason) { ESP_LOGW(TAG, "host reset, reason %d", reason); }

static void host_task(void *arg)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void ble_init(const char *name)
{
    s_mu = xSemaphoreCreateMutex();
    s_q = xQueueCreate(8, sizeof(ble_msg_t));

    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble init failed: %s", esp_err_to_name(err));
        return;
    }
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;

    ble_svc_gap_init();
    ble_svc_gatt_init();
    int rc = ble_gatts_count_cfg(SVCS);
    if (rc == 0) rc = ble_gatts_add_svcs(SVCS);
    if (rc) ESP_LOGE(TAG, "gatt setup failed: %d", rc);
    ble_svc_gap_device_name_set(name);

    nimble_port_freertos_init(host_task);
}

void ble_set_name(const char *name)
{
    ble_svc_gap_device_name_set(name);
    if (s_synced) {
        ble_gap_adv_stop();
        advertise();
    }
}

bool ble_poll(ble_msg_t *m) { return s_q && xQueueReceive(s_q, m, 0) == pdTRUE; }

static void publish(uint8_t *dst, size_t *dst_len, uint16_t handle, const void *p, size_t n)
{
    if (!s_mu) return;
    if (n > VAL_MAX) n = VAL_MAX;
    xSemaphoreTake(s_mu, portMAX_DELAY);
    memcpy(dst, p, n);
    *dst_len = n;
    xSemaphoreGive(s_mu);
    if (s_synced && s_conns > 0 && handle) ble_gatts_chr_updated(handle);
}

void ble_publish_state(const void *p, size_t n) { publish(s_state, &s_state_len, s_state_h, p, n); }
void ble_publish_config(const void *p, size_t n) { publish(s_config, &s_config_len, s_config_h, p, n); }
int ble_connections(void) { return s_conns; }

void ble_set_slow(bool slow)
{
    if (slow == s_slow) return;
    s_slow = slow;
    if (s_synced) {
        ble_gap_adv_stop();
        advertise();
    }
}
