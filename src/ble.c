#include "ble.h"
#include <string.h>
#include "esp_bt.h"
#include "esp_log.h"
#include "esp_random.h"
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
static const ble_uuid128_t NET_UUID = BTN_UUID(6);
static const ble_uuid128_t WAKES_UUID = BTN_UUID(7);

enum { CHR_STATE = 1, CHR_CONFIG, CHR_CMD, CHR_LINK, CHR_NET, CHR_WAKES };

// Two advertising sets: legacy 1M (Chrome can only connect through legacy adverts) and
// extended connectable on Coded PHY, primary and secondary (range; buttons and apps).
enum { ADV_LEGACY = 0, ADV_CODED = 1, ADV_SETS };

// Connection parameters for phones, asked of the central by PHY (units: 1.25 ms; 10 ms).
// Coded events cost about 5x the airtime of 1M ones, so the interval is longer there.
// The long supervision timeout rides out fades at range instead of dropping the link.
#define CONN_ITVL_1M_MIN 24     // 30 ms
#define CONN_ITVL_1M_MAX 40     // 50 ms
#define CONN_ITVL_CODED_MIN 64  // 80 ms
#define CONN_ITVL_CODED_MAX 80  // 100 ms
#define CONN_ITVL_OFF_MIN 320   // 400 ms: powered off, phones only, nothing is time-critical
#define CONN_ITVL_OFF_MAX 400   // 500 ms
#define CONN_SUPERVISION 2000   // 20 s
// Button-to-button links (we are central): slower, since state is only sent when it changes.
#define BTN_ITVL_MIN 180        // 225 ms
#define BTN_ITVL_MAX 220        // 275 ms

// Coded scans for other buttons: 600 ms of listening (more than one 500-550 ms advert interval),
// often for the first minute after boot, then every 15 s or so. Not while powered off.
#define SCAN_MS 600
#define SCAN_EARLY_EVERY_MS 3000
#define SCAN_EARLY_FOR_MS 60000
#define SCAN_EVERY_MS 15000
#define PHONE_SLOTS 2  // connections kept free for phones (buttons don't connect out into these)
#define CONNECT_TIMEOUT_MS 3000

// Largest network frame per link: on Coded a long packet holds the radio (about 64 us a byte),
// so keep frames short there; 1M can use the whole ATT payload.
#define FRAME_MAX_CODED 120

#define VAL_MAX 64
#define WAKES_MAX 256  // the wake log's value (a long read past the MTU)
#define LINK_LEN 9
#define MAX_CONN CONFIG_BT_NIMBLE_MAX_CONNECTIONS

typedef struct {
    bool used;
    bool central;  // we initiated (a button link)
    uint16_t h;
    ble_addr_t peer;
    uint16_t net_h;  // central: the peer's network characteristic value handle (0 until discovered)
    uint16_t svc_end;
    volatile bool coded;
    uint8_t param_tries;  // phone links: interval requests since the last change of speed
} conn_t;

static SemaphoreHandle_t s_mu;
static QueueHandle_t s_q, s_evq;
static uint8_t s_state[VAL_MAX], s_config[VAL_MAX];
static size_t s_state_len, s_config_len;
static uint16_t s_state_h, s_config_h, s_net_h;
static uint8_t s_own_addr_type;
static volatile uint16_t s_node_id;
static volatile bool s_synced;
static volatile int s_conns;
static volatile bool s_slow;  // "off": slow adverts at lower power, no scanning
static bool s_adv_ready;      // both sets configured with the current speed and name (host task only)
static bool s_connecting;     // an outgoing connection is being set up
static uint32_t s_synced_at;
static conn_t s_conn[MAX_CONN];
static struct ble_npl_event s_restart_ev;
static struct ble_npl_callout s_scan_co;
static struct ble_npl_callout s_param_co;

static int chr_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg);
static void request_params(uint16_t conn, bool coded);

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
                {
                    // Network frames: centrals (phones, buttons) write them, we notify ours to them.
                    // The CCCD follows the value handle (button centrals rely on that).
                    .uuid = &NET_UUID.u,
                    .access_cb = chr_access,
                    .arg = (void *)CHR_NET,
                    .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_NOTIFY,
                    .val_handle = &s_net_h,
                },
                {
                    // The wake log (main.c wake_log_read): read on demand, filled by the reader.
                    .uuid = &WAKES_UUID.u,
                    .access_cb = chr_access,
                    .arg = (void *)CHR_WAKES,
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

static uint16_t addr_node_id(const ble_addr_t *a) { return (uint16_t)(a->val[1] << 8 | a->val[0]); }

static conn_t *conn_get(uint16_t h)
{
    for (int i = 0; i < MAX_CONN; i++)
        if (s_conn[i].used && s_conn[i].h == h) return &s_conn[i];
    return NULL;
}

static TaskHandle_t s_notify_task;
void ble_set_notify_task(void *task) { s_notify_task = (TaskHandle_t)task; }
static size_t (*s_wakes_reader)(uint8_t *buf, size_t max);
void ble_set_wakes_reader(size_t (*fn)(uint8_t *buf, size_t max)) { s_wakes_reader = fn; }

static void post_ev(uint8_t kind, uint16_t h, bool central, struct os_mbuf *om)
{
    static ble_ev_t e;  // host task only; too big for its stack
    e.kind = kind;
    e.conn = h;
    e.central = central;
    e.len = 0;
    if (om) {
        uint16_t len = 0;
        if (OS_MBUF_PKTLEN(om) > BLE_FRAME_MAX) {
            ESP_LOGW(TAG, "conn %d: frame too long (%d), dropped", h, OS_MBUF_PKTLEN(om));
            return;
        }
        if (ble_hs_mbuf_to_flat(om, e.data, sizeof e.data, &len) != 0) return;
        e.len = len;
    }
    if (xQueueSend(s_evq, &e, 0) != pdTRUE) ESP_LOGW(TAG, "conn %d: network queue full, event %d dropped", h, kind);
    else if (s_notify_task) xTaskNotifyGive(s_notify_task);  // the game loop may be in a long sleep (off)
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
        static uint8_t buf[WAKES_MAX];  // host task only
        size_t n = 0;
        if (which == CHR_WAKES) {
            n = s_wakes_reader ? s_wakes_reader(buf, sizeof buf) : 0;
        } else if (which == CHR_LINK) {
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
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR && which == CHR_NET) {
        if (OS_MBUF_PKTLEN(ctxt->om) > BLE_FRAME_MAX) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        if (uxQueueSpacesAvailable(s_evq) == 0) return BLE_ATT_ERR_INSUFFICIENT_RES;
        post_ev(BLE_EV_FRAME, conn, false, ctxt->om);
        return 0;
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

// ---- advertising ------------------------------------------------------------

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
    p.itvl_min = s_slow ? BLE_GAP_ADV_ITVL_MS(2000) : BLE_GAP_ADV_ITVL_MS(100);
    p.itvl_max = s_slow ? BLE_GAP_ADV_ITVL_MS(2500) : BLE_GAP_ADV_ITVL_MS(150);
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
    p.itvl_min = BLE_GAP_ADV_ITVL_MS(500);  // (not used while off)
    p.itvl_max = BLE_GAP_ADV_ITVL_MS(550);
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
    if (s_conns >= MAX_CONN) {
        ESP_LOGI(TAG, "not advertising: %d connected (the most it takes)", s_conns);
        return;
    }
    for (uint8_t i = 0; i < ADV_SETS; i++) {
        if (ble_gap_ext_adv_active(i)) continue;
        if (i == ADV_CODED && s_slow) continue;  // off: phones only (legacy), no button links
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
    // Phone links follow the speed too: a 30 ms interval while off costs far more than the adverts.
    for (int i = 0; i < MAX_CONN; i++)
        if (s_conn[i].used && !s_conn[i].central) {
            s_conn[i].param_tries = 0;
            request_params(s_conn[i].h, s_conn[i].coded);
        }
}

static void restart_adverts(void)
{
    if (s_synced) ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &s_restart_ev);
}

// ---- scanning for buttons and connecting to them ------------------------------

static void scan_later(uint32_t ms)
{
    int rc = ble_npl_callout_reset(&s_scan_co, ble_npl_time_ms_to_ticks32(ms));
    if (rc) ESP_LOGW(TAG, "scan timer: %d", rc);
}

static uint32_t scan_gap_ms(void)
{
    uint32_t up = (uint32_t)(ble_npl_time_ticks_to_ms32(ble_npl_time_get()) - s_synced_at);
    uint32_t base = up < SCAN_EARLY_FOR_MS ? SCAN_EARLY_EVERY_MS : SCAN_EVERY_MS;
    return base - base / 5 + esp_random() % (base * 2 / 5 + 1);  // +-20 %, so buttons don't stay in step
}

static void scan_start(struct ble_npl_event *ev)
{
    if (!s_synced) return;
    if (s_slow || s_connecting || s_conns >= MAX_CONN - PHONE_SLOTS || ble_gap_disc_active()) {
        scan_later(scan_gap_ms());
        return;
    }
    struct ble_gap_ext_disc_params coded = {
        .itvl = BLE_GAP_SCAN_ITVL_MS(SCAN_MS),
        .window = BLE_GAP_SCAN_WIN_MS(SCAN_MS),
        .passive = 1,
    };
    int rc = ble_gap_ext_disc(s_own_addr_type, SCAN_MS / 10, 0, 0, BLE_HCI_SCAN_FILT_NO_WL, 0, NULL, &coded,
                              gap_event, NULL);
    if (rc) {
        ESP_LOGW(TAG, "coded scan start: %d", rc);
        scan_later(scan_gap_ms());
    }
}

static bool linked_to(const ble_addr_t *a)
{
    for (int i = 0; i < MAX_CONN; i++)
        if (s_conn[i].used && ble_addr_cmp(&s_conn[i].peer, a) == 0) return true;
    return false;
}

// A Coded connectable extended advert carrying our service UUID: another button.
static bool is_button_advert(const struct ble_gap_ext_disc_desc *d)
{
    if (d->props & BLE_HCI_ADV_LEGACY_MASK) return false;
    if (!(d->props & BLE_HCI_ADV_CONN_MASK)) return false;
    if (d->prim_phy != BLE_HCI_LE_PHY_CODED) return false;
    if ((d->data_status & BLE_HCI_ADV_DATA_STATUS_MASK) != BLE_HCI_ADV_DATA_STATUS_COMPLETE) return false;
    struct ble_hs_adv_fields f;
    if (ble_hs_adv_parse_fields(&f, d->data, d->length_data) != 0) return false;
    for (int i = 0; i < f.num_uuids128; i++)
        if (ble_uuid_cmp(&f.uuids128[i].u, &SVC_UUID.u) == 0) return true;
    return false;
}

static void connect_to(const ble_addr_t *a, int8_t rssi)
{
    int rc = ble_gap_disc_cancel();
    if (rc && rc != BLE_HS_EALREADY) ESP_LOGW(TAG, "scan cancel: %d", rc);
    struct ble_gap_conn_params cp = {
        .scan_itvl = BLE_GAP_SCAN_ITVL_MS(100),
        .scan_window = BLE_GAP_SCAN_WIN_MS(100),
        .itvl_min = BTN_ITVL_MIN,
        .itvl_max = BTN_ITVL_MAX,
        .latency = 0,
        .supervision_timeout = CONN_SUPERVISION,
    };
    rc = ble_gap_ext_connect(s_own_addr_type, a, CONNECT_TIMEOUT_MS, BLE_GAP_LE_PHY_CODED_MASK, NULL, NULL, &cp,
                             gap_event, NULL);
    if (rc) {
        ESP_LOGW(TAG, "connect to button %04X: %d", addr_node_id(a), rc);
        scan_later(scan_gap_ms());
        return;
    }
    s_connecting = true;
    ESP_LOGI(TAG, "connecting to button %04X (%d dBm, Coded)", addr_node_id(a), rssi);
}

// ---- central side: find the peer's network characteristic and subscribe --------

static void gattc_fail(uint16_t h, const char *what, int status)
{
    ESP_LOGW(TAG, "conn %d: %s failed (%d), dropping the link", h, what, status);
    int rc = ble_gap_terminate(h, BLE_ERR_REM_USER_CONN_TERM);
    if (rc && rc != BLE_HS_ENOTCONN) ESP_LOGW(TAG, "conn %d: terminate: %d", h, rc);
}

static int on_cccd(uint16_t h, const struct ble_gatt_error *err, struct ble_gatt_attr *attr, void *arg)
{
    if (err->status) {
        gattc_fail(h, "subscribe", err->status);
        return 0;
    }
    ESP_LOGI(TAG, "conn %d: network link up (central)", h);
    post_ev(BLE_EV_UP, h, true, NULL);
    return 0;
}

static int on_chr(uint16_t h, const struct ble_gatt_error *err, const struct ble_gatt_chr *chr, void *arg)
{
    conn_t *c = conn_get(h);
    if (!c) return 0;
    if (err->status == 0 && chr) {
        c->net_h = chr->val_handle;
        return 0;
    }
    if (err->status != BLE_HS_EDONE) {
        gattc_fail(h, "characteristic discovery", err->status);
        return 0;
    }
    if (!c->net_h) {
        gattc_fail(h, "network characteristic", BLE_HS_ENOENT);
        return 0;
    }
    static const uint8_t on[2] = {1, 0};
    int rc = ble_gattc_write_flat(h, c->net_h + 1, on, sizeof on, on_cccd, NULL);
    if (rc) gattc_fail(h, "subscribe request", rc);
    return 0;
}

static int on_svc(uint16_t h, const struct ble_gatt_error *err, const struct ble_gatt_svc *svc, void *arg)
{
    conn_t *c = conn_get(h);
    if (!c) return 0;
    if (err->status == 0 && svc) {
        c->svc_end = svc->end_handle;
        int rc = ble_gattc_disc_chrs_by_uuid(h, svc->start_handle, svc->end_handle, &NET_UUID.u, on_chr, NULL);
        if (rc) gattc_fail(h, "characteristic discovery request", rc);
        return 0;
    }
    if (err->status == BLE_HS_EDONE && !c->svc_end) gattc_fail(h, "service discovery", BLE_HS_ENOENT);
    else if (err->status != BLE_HS_EDONE) gattc_fail(h, "service discovery", err->status);
    return 0;
}

static int on_mtu(uint16_t h, const struct ble_gatt_error *err, uint16_t mtu, void *arg)
{
    if (err->status) ESP_LOGW(TAG, "conn %d: mtu exchange: %d (carrying on with the default)", h, err->status);
    int rc = ble_gattc_disc_svc_by_uuid(h, &SVC_UUID.u, on_svc, NULL);
    if (rc) gattc_fail(h, "service discovery request", rc);
    return 0;
}

// ---- connections ---------------------------------------------------------------

// Asks the central for an interval suited to the link's PHY, and a long supervision timeout.
static void request_params(uint16_t conn, bool coded)
{
    struct ble_gap_upd_params u = {
        .itvl_min = s_slow ? CONN_ITVL_OFF_MIN : coded ? CONN_ITVL_CODED_MIN : CONN_ITVL_1M_MIN,
        .itvl_max = s_slow ? CONN_ITVL_OFF_MAX : coded ? CONN_ITVL_CODED_MAX : CONN_ITVL_1M_MAX,
        .latency = 0,
        .supervision_timeout = CONN_SUPERVISION,
    };
    int rc = ble_gap_update_params(conn, &u);
    if (rc) ESP_LOGW(TAG, "conn %d: param update request: %d", conn, rc);
}

// Every few seconds: a phone link whose interval is outside what this speed asks for gets asked
// again (a request can be lost to a PHY change in progress, or never sent when the PHY update
// event doesn't come). A few tries per change of speed, so a central that won't is left alone.
#define PARAM_CHECK_MS 5000
#define PARAM_TRIES 3

static void param_check(struct ble_npl_event *ev)
{
    for (int i = 0; i < MAX_CONN; i++) {
        conn_t *c = &s_conn[i];
        if (!c->used || c->central || c->param_tries >= PARAM_TRIES) continue;
        struct ble_gap_conn_desc d;
        if (ble_gap_conn_find(c->h, &d)) continue;
        uint16_t lo = s_slow ? CONN_ITVL_OFF_MIN : c->coded ? CONN_ITVL_CODED_MIN : CONN_ITVL_1M_MIN;
        uint16_t hi = s_slow ? CONN_ITVL_OFF_MAX : c->coded ? CONN_ITVL_CODED_MAX : CONN_ITVL_1M_MAX;
        if (d.conn_itvl >= lo && d.conn_itvl <= hi) continue;
        c->param_tries++;
        ESP_LOGI(TAG, "conn %d: interval %d x 1.25 ms, asking for %d-%d (try %d)", c->h, d.conn_itvl, lo, hi,
                 c->param_tries);
        request_params(c->h, c->coded);
    }
    ble_npl_callout_reset(&s_param_co, ble_npl_time_ms_to_ticks32(PARAM_CHECK_MS));
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
    ESP_LOGI(TAG, "conn %d %s: %s %04X, phy tx %s rx %s, interval %d.%02d ms, latency %d, timeout %d ms", conn, what,
             d.role == BLE_GAP_ROLE_MASTER ? "we are central to" : "we are peripheral to",
             addr_node_id(&d.peer_id_addr), phy_name(tx), phy_name(rx), d.conn_itvl * 125 / 100,
             d.conn_itvl * 125 % 100, d.conn_latency, d.supervision_timeout * 10);
}

// Two links to the same button (both connected to each other at once): keep the one whose central
// has the lower node id. Both ends work this out the same way.
static bool drop_if_duplicate(conn_t *nc)
{
    for (int i = 0; i < MAX_CONN; i++) {
        conn_t *c = &s_conn[i];
        if (!c->used || c == nc || ble_addr_cmp(&c->peer, &nc->peer) != 0) continue;
        uint16_t me = s_node_id, peer = addr_node_id(&nc->peer);
        uint16_t central_old = c->central ? me : peer, central_new = nc->central ? me : peer;
        conn_t *drop = central_new >= central_old ? nc : c;
        ESP_LOGI(TAG, "two links to %04X: dropping conn %d", peer, drop->h);
        int rc = ble_gap_terminate(drop->h, BLE_ERR_REM_USER_CONN_TERM);
        if (rc && rc != BLE_HS_ENOTCONN) ESP_LOGW(TAG, "conn %d: terminate: %d", drop->h, rc);
        return drop == nc;
    }
    return false;
}

static void on_connect(uint16_t h)
{
    struct ble_gap_conn_desc d;
    int rc = ble_gap_conn_find(h, &d);
    if (rc) {
        ESP_LOGW(TAG, "conn %d: connected, but not found (%d)", h, rc);
        return;
    }
    conn_t *c = NULL;
    for (int i = 0; i < MAX_CONN && !c; i++)
        if (!s_conn[i].used) c = &s_conn[i];
    if (!c) {
        ESP_LOGW(TAG, "conn %d: no slot", h);
        return;
    }
    memset(c, 0, sizeof *c);
    c->used = true;
    c->h = h;
    c->central = d.role == BLE_GAP_ROLE_MASTER;
    c->peer = d.peer_id_addr;
    uint8_t tx = 0, rx = 0;
    rc = ble_gap_read_le_phy(h, &tx, &rx);
    if (rc) ESP_LOGW(TAG, "conn %d: read phy: %d", h, rc);
    c->coded = rc == 0 && tx == BLE_GAP_LE_PHY_CODED && rx == BLE_GAP_LE_PHY_CODED;
    if (drop_if_duplicate(c)) return;

    if (c->central) {
        // A button we found on Coded: find its network characteristic.
        rc = ble_gattc_exchange_mtu(h, on_mtu, NULL);
        if (rc) gattc_fail(h, "mtu exchange request", rc);
    } else if (c->coded) {
        // Connected on Coded already (a button or an app): the central has chosen its parameters.
    } else {
        // Chrome can only connect through the legacy 1M adverts; move the link to Coded where the
        // central supports it. Interval and timeout are requested once the PHY has settled.
        rc = ble_gap_set_prefered_le_phy(h, BLE_GAP_LE_PHY_CODED_MASK, BLE_GAP_LE_PHY_CODED_MASK,
                                         BLE_GAP_LE_PHY_CODED_S8);
        if (rc) {
            ESP_LOGW(TAG, "conn %d: coded phy request: %d", h, rc);
            request_params(h, false);
        }
    }
}

static int gap_event(struct ble_gap_event *ev, void *arg)
{
    switch (ev->type) {
    case BLE_GAP_EVENT_CONNECT: {
        bool was_connecting = s_connecting;
        if (ev->connect.status != 0) {
            // Includes our own connect attempts timing out.
            ESP_LOGW(TAG, "connect failed: %d", ev->connect.status);
            s_connecting = false;
            if (was_connecting) scan_later(scan_gap_ms());
            advertise();
            break;
        }
        uint16_t h = ev->connect.conn_handle;
        s_conns++;
        log_conn(h, "connected");
        ESP_LOGI(TAG, "%d connected", s_conns);
        struct ble_gap_conn_desc d;
        if (ble_gap_conn_find(h, &d) == 0 && d.role == BLE_GAP_ROLE_MASTER) {
            s_connecting = false;
            scan_later(scan_gap_ms());
        }
        on_connect(h);
        advertise();  // stay visible for more centrals while there are free slots
        break;
    }
    case BLE_GAP_EVENT_DISCONNECT: {
        uint16_t h = ev->disconnect.conn.conn_handle;
        if (s_conns > 0) s_conns--;
        ESP_LOGI(TAG, "conn %d disconnected (reason 0x%x, %d left)", h, ev->disconnect.reason, s_conns);
        conn_t *c = conn_get(h);
        if (c) {
            post_ev(BLE_EV_DOWN, h, c->central, NULL);
            c->used = false;
        }
        advertise();
        break;
    }
    case BLE_GAP_EVENT_EXT_DISC: {
        const struct ble_gap_ext_disc_desc *d = &ev->ext_disc;
        if (s_connecting || !is_button_advert(d) || linked_to(&d->addr)) break;
        if (addr_node_id(&d->addr) == s_node_id) break;
        connect_to(&d->addr, d->rssi);
        break;
    }
    case BLE_GAP_EVENT_DISC_COMPLETE:
        if (!s_connecting) scan_later(scan_gap_ms());
        break;
    case BLE_GAP_EVENT_NOTIFY_RX: {
        conn_t *c = conn_get(ev->notify_rx.conn_handle);
        if (c && c->central && ev->notify_rx.attr_handle == c->net_h)
            post_ev(BLE_EV_FRAME, c->h, true, ev->notify_rx.om);
        break;
    }
    case BLE_GAP_EVENT_PHY_UPDATE_COMPLETE: {
        uint16_t h = ev->phy_updated.conn_handle;
        bool coded = ev->phy_updated.status == 0 && ev->phy_updated.tx_phy == BLE_GAP_LE_PHY_CODED &&
                     ev->phy_updated.rx_phy == BLE_GAP_LE_PHY_CODED;
        conn_t *c = conn_get(h);
        if (ev->phy_updated.status)
            ESP_LOGW(TAG, "conn %d: phy update failed: %d", h, ev->phy_updated.status);
        else
            ESP_LOGI(TAG, "conn %d: phy tx %s rx %s", h, phy_name(ev->phy_updated.tx_phy),
                     phy_name(ev->phy_updated.rx_phy));
        if (c) c->coded = coded;
        if (c && !c->central) request_params(h, coded);
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
    uint8_t a[6] = {0};
    rc = ble_hs_id_copy_addr(s_own_addr_type, a, NULL);
    if (rc) ESP_LOGE(TAG, "own address: %d", rc);
    s_node_id = (uint16_t)(a[1] << 8 | a[0]);
    s_synced_at = ble_npl_time_ticks_to_ms32(ble_npl_time_get());
    s_synced = true;
    s_adv_ready = false;
    advertise();
    scan_later(500 + esp_random() % 1500);
    ble_npl_callout_reset(&s_param_co, ble_npl_time_ms_to_ticks32(PARAM_CHECK_MS));
    ESP_LOGI(TAG, "advertising as \"%s\" (legacy 1M + Coded), node %04X", ble_svc_gap_device_name(), s_node_id);
}

static void on_reset(int reason)
{
    ESP_LOGW(TAG, "host reset, reason %d", reason);
    s_synced = false;
    s_adv_ready = false;
    s_connecting = false;
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
    s_evq = xQueueCreate(16, sizeof(ble_ev_t));

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
    ble_npl_callout_init(&s_scan_co, nimble_port_get_dflt_eventq(), scan_start, NULL);
    ble_npl_callout_init(&s_param_co, nimble_port_get_dflt_eventq(), param_check, NULL);
    nimble_port_freertos_init(host_task);
}

// Before deep sleep: the host stopped and the controller disabled and freed, as IDF asks (the radio is
// powered down in deep sleep either way, but this takes the controller's sleep-clock and wake-source
// requests down with it). Links just drop. Not from the host task.
void ble_stop(void)
{
    if (!s_mu) return;
    s_synced = false;
    int rc = nimble_port_stop();
    if (rc) ESP_LOGW(TAG, "nimble stop: %d", rc);
    else nimble_port_deinit();
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

// ---- network transport, for mesh.c --------------------------------------------

uint16_t ble_node_id(void) { return s_synced ? s_node_id : 0; }

bool ble_poll_event(ble_ev_t *e) { return s_evq && xQueueReceive(s_evq, e, 0) == pdTRUE; }

int ble_send_frame(uint16_t conn, const uint8_t *p, size_t n)
{
    conn_t *c = conn_get(conn);
    if (!c) return BLE_HS_ENOTCONN;
    if (c->central) {
        if (!c->net_h) return BLE_HS_ENOTCONN;
        return ble_gattc_write_no_rsp_flat(conn, c->net_h, p, (uint16_t)n);
    }
    struct os_mbuf *om = ble_hs_mbuf_from_flat(p, (uint16_t)n);
    if (!om) return BLE_HS_ENOMEM;
    return ble_gatts_notify_custom(conn, s_net_h, om);  // consumes om
}

size_t ble_frame_max(uint16_t conn)
{
    conn_t *c = conn_get(conn);
    uint16_t mtu = ble_att_mtu(conn);
    size_t n = mtu > 3 ? mtu - 3u : 20u;
    if (n > BLE_FRAME_MAX) n = BLE_FRAME_MAX;
    if (c && c->coded && n > FRAME_MAX_CODED) n = FRAME_MAX_CODED;
    return n;
}

uint32_t ble_conn_itvl_ms(uint16_t conn)
{
    struct ble_gap_conn_desc d;
    if (ble_gap_conn_find(conn, &d) != 0) return 0;
    return d.conn_itvl * 5u / 4u;
}

bool ble_link_quality(uint16_t conn, int8_t *rssi, uint8_t *phy)
{
    conn_t *c = conn_get(conn);
    if (!c) return false;
    *phy = c->coded ? BLE_GAP_LE_PHY_CODED : BLE_GAP_LE_PHY_1M;
    int rc = ble_gap_conn_rssi(conn, rssi);
    if (rc) {
        ESP_LOGW(TAG, "conn %d: read rssi: %d", conn, rc);
        *rssi = 127;
    }
    return true;
}

void ble_drop(uint16_t conn)
{
    int rc = ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
    if (rc && rc != BLE_HS_ENOTCONN) ESP_LOGW(TAG, "conn %d: terminate: %d", conn, rc);
}
