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
static const ble_uuid128_t LINK_UUID = BTN_UUID(5);

enum { CHR_STATE = 1, CHR_CONFIG, CHR_CMD, CHR_LINK };

// Two advertising sets: legacy 1M (Chrome can only connect through legacy adverts) and
// extended connectable on Coded PHY, primary and secondary (range; buttons and apps).
enum { ADV_LEGACY = 0, ADV_CODED = 1, ADV_SETS };

// Connection parameters the button asks the central for, by PHY (units: 1.25 ms; 10 ms).
// Coded events cost about 5x the airtime of 1M ones, so the interval is longer there.
// The long supervision timeout rides out fades at range instead of dropping the link.
#define CONN_ITVL_1M_MIN 24     // 30 ms
#define CONN_ITVL_1M_MAX 40     // 50 ms
#define CONN_ITVL_CODED_MIN 64  // 80 ms
#define CONN_ITVL_CODED_MAX 80  // 100 ms
#define CONN_SUPERVISION 2000   // 20 s

#define VAL_MAX 64
#define LINK_LEN 9

static SemaphoreHandle_t s_mu;
static QueueHandle_t s_q;
static uint8_t s_state[VAL_MAX], s_config[VAL_MAX];
static size_t s_state_len, s_config_len;
static uint16_t s_state_h, s_config_h;
static uint8_t s_own_addr_type;
static volatile bool s_synced;
static volatile int s_conns;
static volatile bool s_slow;  // "off": slow adverts at lower power
static bool s_adv_ready;      // both sets configured with the current speed and name (host task only)
static struct ble_npl_event s_restart_ev;

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
                {
                    .uuid = &LINK_UUID.u,
                    .access_cb = chr_access,
                    .arg = (void *)CHR_LINK,
                    .flags = BLE_GATT_CHR_F_READ,
                },
                {0},
            },
    },
    {0},
};

static const char *phy_name(uint8_t phy)
{
    return phy == BLE_GAP_LE_PHY_1M      ? "1M"
           : phy == BLE_GAP_LE_PHY_2M    ? "2M"
           : phy == BLE_GAP_LE_PHY_CODED ? "Coded"
                                         : "?";
}

// The reading connection's own link: PHYs, interval, latency, supervision timeout, RSSI.
static void link_info(uint16_t conn, uint8_t *buf)
{
    struct ble_gap_conn_desc d;
    uint8_t tx = 0, rx = 0;
    int8_t rssi = 127;  // unknown
    memset(buf, 0, LINK_LEN);
    int rc = ble_gap_conn_find(conn, &d);
    if (rc) {
        ESP_LOGW(TAG, "conn %d: link read, not found (%d)", conn, rc);
        return;
    }
    rc = ble_gap_read_le_phy(conn, &tx, &rx);
    if (rc) ESP_LOGW(TAG, "conn %d: read phy: %d", conn, rc);
    rc = ble_gap_conn_rssi(conn, &rssi);
    if (rc) {
        ESP_LOGW(TAG, "conn %d: read rssi: %d", conn, rc);
        rssi = 127;
    }
    buf[0] = tx;
    buf[1] = rx;
    buf[2] = d.conn_itvl & 0xff;
    buf[3] = d.conn_itvl >> 8;
    buf[4] = d.conn_latency & 0xff;
    buf[5] = d.conn_latency >> 8;
    buf[6] = d.supervision_timeout & 0xff;
    buf[7] = d.supervision_timeout >> 8;
    buf[8] = (uint8_t)rssi;
}

static int chr_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    int which = (int)(intptr_t)arg;
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        uint8_t buf[VAL_MAX];
        size_t n = 0;
        if (which == CHR_LINK) {
            link_info(conn, buf);
            n = LINK_LEN;
        } else {
            xSemaphoreTake(s_mu, portMAX_DELAY);
            if (which == CHR_STATE) {
                n = s_state_len;
                memcpy(buf, s_state, n);
            } else if (which == CHR_CONFIG) {
                n = s_config_len;
                memcpy(buf, s_config, n);
            }
            xSemaphoreGive(s_mu);
        }
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

// Builds adv data into an mbuf and hands it to the set (set_data frees the mbuf, success or not).
static int set_adv_fields(uint8_t inst, const struct ble_hs_adv_fields *f, bool rsp)
{
    const char *what = rsp ? "scan response" : "data";
    struct os_mbuf *om = os_msys_get_pkthdr(64, 0);
    if (!om) {
        ESP_LOGW(TAG, "adv %d: no mbuf for %s", inst, what);
        return BLE_HS_ENOMEM;
    }
    int rc = ble_hs_adv_set_fields_mbuf(f, om);
    if (rc) {
        ESP_LOGW(TAG, "adv %d: build %s: %d", inst, what, rc);
        os_mbuf_free_chain(om);
        return rc;
    }
    rc = rsp ? ble_gap_ext_adv_rsp_set_data(inst, om) : ble_gap_ext_adv_set_data(inst, om);
    if (rc) ESP_LOGW(TAG, "adv %d: set %s: %d", inst, what, rc);
    return rc;
}

// Configures both sets with the current speed and name. They must not be running.
static void adv_configure(void)
{
    s_adv_ready = false;
    const char *name = ble_svc_gap_device_name();
    int8_t pwr = s_slow ? 6 : 9;
    esp_err_t err = esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_ADV, s_slow ? ESP_PWR_LVL_P6 : ESP_PWR_LVL_P9);
    if (err != ESP_OK) ESP_LOGW(TAG, "adv tx power: %s", esp_err_to_name(err));

    // Legacy ADV_IND: the service UUID; the name in the scan response (the UUID fills most of the 31 bytes).
    struct ble_gap_ext_adv_params p = {0};
    p.legacy_pdu = 1;
    p.connectable = 1;
    p.scannable = 1;
    p.own_addr_type = s_own_addr_type;
    p.primary_phy = BLE_HCI_LE_PHY_1M;
    p.secondary_phy = BLE_HCI_LE_PHY_1M;
    p.itvl_min = s_slow ? BLE_GAP_ADV_ITVL_MS(1000) : BLE_GAP_ADV_ITVL_MS(100);
    p.itvl_max = s_slow ? BLE_GAP_ADV_ITVL_MS(1100) : BLE_GAP_ADV_ITVL_MS(150);
    p.tx_power = pwr;
    p.sid = ADV_LEGACY;
    int rc = ble_gap_ext_adv_configure(ADV_LEGACY, &p, NULL, gap_event, NULL);
    if (rc) {
        ESP_LOGE(TAG, "legacy adv configure: %d", rc);
        return;
    }
    struct ble_hs_adv_fields f = {0};
    f.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    f.uuids128 = &SVC_UUID;
    f.num_uuids128 = 1;
    f.uuids128_is_complete = 1;
    if (set_adv_fields(ADV_LEGACY, &f, false)) return;
    struct ble_hs_adv_fields r = {0};
    r.name = (const uint8_t *)name;
    r.name_len = (uint8_t)strlen(name);
    r.name_is_complete = 1;
    if (set_adv_fields(ADV_LEGACY, &r, true)) return;

    // Coded: a short ADV_EXT_IND on each primary channel pointing at one AUX_ADV_IND with UUID and name.
    // Connectable extended adverts can't be scannable or chained, so it all has to fit in that AUX packet.
    memset(&p, 0, sizeof p);
    p.connectable = 1;
    p.own_addr_type = s_own_addr_type;
    p.primary_phy = BLE_HCI_LE_PHY_CODED;
    p.secondary_phy = BLE_HCI_LE_PHY_CODED;
    p.itvl_min = s_slow ? BLE_GAP_ADV_ITVL_MS(2000) : BLE_GAP_ADV_ITVL_MS(500);
    p.itvl_max = s_slow ? BLE_GAP_ADV_ITVL_MS(2200) : BLE_GAP_ADV_ITVL_MS(550);
    p.tx_power = pwr;
    p.sid = ADV_CODED;
    rc = ble_gap_ext_adv_configure(ADV_CODED, &p, NULL, gap_event, NULL);
    if (rc) {
        ESP_LOGE(TAG, "coded adv configure: %d", rc);
        return;
    }
    f.name = r.name;
    f.name_len = r.name_len;
    f.name_is_complete = 1;
    if (set_adv_fields(ADV_CODED, &f, false)) return;
    s_adv_ready = true;
}

// Starts whichever sets aren't running (a set stops when a central connects through it).
static void advertise(void)
{
    if (!s_synced) return;
    if (!s_adv_ready) adv_configure();
    if (!s_adv_ready) return;
    if (s_conns >= CONFIG_BT_NIMBLE_MAX_CONNECTIONS) {
        ESP_LOGI(TAG, "not advertising: %d connected (the most it takes)", s_conns);
        return;
    }
    for (uint8_t i = 0; i < ADV_SETS; i++) {
        if (ble_gap_ext_adv_active(i)) continue;
        int rc = ble_gap_ext_adv_start(i, 0, 0);
        if (rc && rc != BLE_HS_EALREADY) ESP_LOGW(TAG, "adv %s start: %d", i == ADV_CODED ? "coded" : "legacy", rc);
    }
}

// Name or speed changed: stop both sets, reconfigure, start again. Runs on the host task
// (queued from the game loop) so it can't race the GAP callbacks.
static void restart_ev_fn(struct ble_npl_event *ev)
{
    if (!s_synced) return;
    for (uint8_t i = 0; i < ADV_SETS; i++) {
        if (!ble_gap_ext_adv_active(i)) continue;
        int rc = ble_gap_ext_adv_stop(i);
        if (rc && rc != BLE_HS_EALREADY) ESP_LOGW(TAG, "adv %d stop: %d", i, rc);
    }
    s_adv_ready = false;
    advertise();
}

static void restart_adverts(void)
{
    if (s_synced) ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &s_restart_ev);
}

// Asks the central for an interval suited to the link's PHY, and a long supervision timeout.
static void request_params(uint16_t conn, bool coded)
{
    struct ble_gap_upd_params u = {
        .itvl_min = coded ? CONN_ITVL_CODED_MIN : CONN_ITVL_1M_MIN,
        .itvl_max = coded ? CONN_ITVL_CODED_MAX : CONN_ITVL_1M_MAX,
        .latency = 0,
        .supervision_timeout = CONN_SUPERVISION,
    };
    int rc = ble_gap_update_params(conn, &u);
    if (rc) ESP_LOGW(TAG, "conn %d: param update request: %d", conn, rc);
}

static void log_conn(uint16_t conn, const char *what)
{
    struct ble_gap_conn_desc d;
    uint8_t tx = 0, rx = 0;
    int rc = ble_gap_conn_find(conn, &d);
    if (rc) {
        ESP_LOGW(TAG, "conn %d %s, but not found (%d)", conn, what, rc);
        return;
    }
    rc = ble_gap_read_le_phy(conn, &tx, &rx);
    if (rc) ESP_LOGW(TAG, "conn %d: read phy: %d", conn, rc);
    ESP_LOGI(TAG, "conn %d %s: %s, phy tx %s rx %s, interval %d.%02d ms, latency %d, timeout %d ms", conn, what,
             d.role == BLE_GAP_ROLE_MASTER ? "we are central" : "we are peripheral", phy_name(tx), phy_name(rx),
             d.conn_itvl * 125 / 100, d.conn_itvl * 125 % 100, d.conn_latency, d.supervision_timeout * 10);
}

static int gap_event(struct ble_gap_event *ev, void *arg)
{
    switch (ev->type) {
    case BLE_GAP_EVENT_CONNECT: {
        if (ev->connect.status != 0) {
            ESP_LOGW(TAG, "connect failed: %d", ev->connect.status);
            advertise();
            break;
        }
        uint16_t h = ev->connect.conn_handle;
        s_conns++;
        log_conn(h, "connected");
        ESP_LOGI(TAG, "%d connected", s_conns);
        // Chrome can only connect through the legacy 1M adverts; move every link to Coded where the
        // central supports it. Interval and timeout are requested once the PHY has settled.
        uint8_t tx = 0, rx = 0;
        int rc = ble_gap_read_le_phy(h, &tx, &rx);
        if (rc) ESP_LOGW(TAG, "conn %d: read phy: %d", h, rc);
        if (rc == 0 && tx == BLE_GAP_LE_PHY_CODED && rx == BLE_GAP_LE_PHY_CODED) {
            request_params(h, true);
        } else {
            rc = ble_gap_set_prefered_le_phy(h, BLE_GAP_LE_PHY_CODED_MASK, BLE_GAP_LE_PHY_CODED_MASK,
                                             BLE_GAP_LE_PHY_CODED_S8);
            if (rc) {
                ESP_LOGW(TAG, "conn %d: coded phy request: %d", h, rc);
                request_params(h, false);
            }
        }
        advertise();  // stay visible for more centrals while there are free slots
        break;
    }
    case BLE_GAP_EVENT_DISCONNECT:
        if (s_conns > 0) s_conns--;
        ESP_LOGI(TAG, "conn %d disconnected (reason 0x%x, %d left)", ev->disconnect.conn.conn_handle,
                 ev->disconnect.reason, s_conns);
        advertise();
        break;
    case BLE_GAP_EVENT_PHY_UPDATE_COMPLETE: {
        uint16_t h = ev->phy_updated.conn_handle;
        bool coded = ev->phy_updated.status == 0 && ev->phy_updated.tx_phy == BLE_GAP_LE_PHY_CODED &&
                     ev->phy_updated.rx_phy == BLE_GAP_LE_PHY_CODED;
        if (ev->phy_updated.status)
            ESP_LOGW(TAG, "conn %d: phy update failed: %d", h, ev->phy_updated.status);
        else
            ESP_LOGI(TAG, "conn %d: phy tx %s rx %s", h, phy_name(ev->phy_updated.tx_phy),
                     phy_name(ev->phy_updated.rx_phy));
        request_params(h, coded);
        break;
    }
    case BLE_GAP_EVENT_CONN_UPDATE:
        if (ev->conn_update.status)
            ESP_LOGW(TAG, "conn %d: param update failed: %d", ev->conn_update.conn_handle, ev->conn_update.status);
        else
            log_conn(ev->conn_update.conn_handle, "updated");
        break;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        if (ev->adv_complete.reason)
            ESP_LOGI(TAG, "adv %d ended: %d", ev->adv_complete.instance, ev->adv_complete.reason);
        advertise();
        break;
    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "conn %d: mtu %d", ev->mtu.conn_handle, ev->mtu.value);
        break;
    default:
        break;
    }
    return 0;
}

static void on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc) ESP_LOGE(TAG, "no BT address: %d", rc);
    rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc) ESP_LOGE(TAG, "address type: %d", rc);
    s_synced = true;
    s_adv_ready = false;
    advertise();
    ESP_LOGI(TAG, "advertising as \"%s\" (legacy 1M + Coded)", ble_svc_gap_device_name());
}

static void on_reset(int reason)
{
    ESP_LOGW(TAG, "host reset, reason %d", reason);
    s_synced = false;
    s_adv_ready = false;
}

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
    rc = ble_svc_gap_device_name_set(name);
    if (rc) ESP_LOGW(TAG, "set name: %d", rc);

    ble_npl_event_init(&s_restart_ev, restart_ev_fn, NULL);
    nimble_port_freertos_init(host_task);
}

void ble_set_name(const char *name)
{
    int rc = ble_svc_gap_device_name_set(name);
    if (rc) ESP_LOGW(TAG, "set name: %d", rc);
    restart_adverts();
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
    restart_adverts();
}
