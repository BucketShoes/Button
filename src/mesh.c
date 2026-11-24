#include "mesh.h"
#include <stddef.h>
#include <string.h>
#include "ble.h"
#include "esp_log.h"
#include "esp_rom_crc.h"
#include "esp_attr.h"
#include "esp_rtc_time.h"
#include "esp_timer.h"
#include "host/ble_hs.h"  // error codes from ble_send_frame
#include "nvs.h"

static const char *TAG = "mesh";

// ---- wire format (docs/ble-protocol.md, "Network") ------------------------------
// Frame: u8 version, u8 type, u32 sender's network ms (0 from phones), then by type:
//   HELLO      u16 node/origin id, u8 kind, u8 flags (bit0: admin wants items on this link)
//   ITEMS      items: u16 origin, u8 stream, u32 version, u8 len, payload
//   HEARTBEAT  u32 digest of everything the sender holds (0 to phones)
//   SUMMARY    u8 flags (bit0 first part, bit1 last part), entries: u16 id, u8 kind, u32 ver[3], u16 cmd seq
#define FRAME_VERSION 1
enum { F_HELLO = 1, F_ITEMS = 2, F_HEARTBEAT = 3, F_SUMMARY = 4 };
enum { S_HOT = 0, S_SLOW = 1, S_CFG = 2, S_CMD = 3, S_LATEST = 3 };
enum { K_NONE = 0, K_BUTTON = 1, K_ADMIN = 2 };
#define HDR 6
#define ITEM_HDR 8
#define SUM_ENTRY 17
// Command payload: u16 entry button, u32 expiry (network ms), u16 target, u8 kind, data
#define CMD_HDR 9

#define MAX_ORIGINS 24
#define MAX_LINKS CONFIG_BT_NIMBLE_MAX_CONNECTIONS
#define ITEM_MAX 64
#define CMD_MAX (CMD_HDR + 48)
#define LOG_N 8   // commands kept per admin, to pass on to buttons that missed them
#define PEND_N 4  // commands held while an earlier one is missing
#define EXEC_N 8
#define MAX_NEIGHBOURS 10

#define HB_MS 1000            // heartbeat on every link
#define FRESH_MS 3000         // a button link counts as working if heard from this recently
#define MISMATCH_MS 2500      // digests differ this long: exchange summaries
#define SUMMARY_GAP_MS 2000   // at most one summary per link this often
#define GAP_SKIP_MS 10000     // a missing command is given up on after this
#define ADMIN_IDLE_MS 600000  // an admin's commands are forgotten after 10 min of quiet
#define DATA_GAP_MIN_MS 30    // at most one data frame per link per connection interval, and this
#define OFF_DROP_MS 3000      // powered off: button links are dropped after this (time to say it's off)

typedef struct {
    uint16_t seq;
    uint8_t len;  // 0 = empty
    uint8_t d[CMD_MAX];
} cmd_t;

typedef struct {
    uint16_t id;
    uint8_t kind;
    uint32_t ver[S_LATEST];
    uint8_t len[S_LATEST];  // 0 = nothing yet
    uint8_t data[S_LATEST][ITEM_MAX];
    uint16_t cmd_h;  // admins: highest command seq taken in order
    cmd_t log[LOG_N];
    cmd_t pend[PEND_N];
    uint32_t pend_since[PEND_N];
    uint32_t touched;  // local ms of the last change
} origin_t;

typedef struct {
    bool used, ready, central, active;
    uint16_t conn;
    uint8_t kind;
    uint16_t peer;
    uint32_t last_rx, last_hb, last_data, last_sum, mismatch_since;
    bool want_sum, hello_sent;
    uint8_t pend[MAX_ORIGINS];      // bit per latest-wins stream to send
    uint16_t cmd_sent[MAX_ORIGINS]; // highest command seq the peer has (or we've sent)
    uint32_t seen;                  // origins listed in the summary being received
} link_t;

typedef struct {
    uint8_t kind, len;
    uint8_t d[CMD_MAX];
} exec_t;

static origin_t s_o[MAX_ORIGINS];  // [0] is this button
static link_t s_l[MAX_LINKS];
static exec_t s_exec[EXEC_N];
static int s_exec_n;
static mesh_cmd_fn s_on_cmd;
static uint16_t s_me;
static uint8_t s_epoch;
static uint32_t s_seq;
// Network time: 64-bit ms, forward-only (everyone moves to the furthest-ahead clock it hears). Local ms
// (esp_timer, from the crystal while awake) + s_net_off. It's carried across deep sleep and every reset
// but a power-on by the LP timer: s_nt keeps the offset and the LP timer's reading at local ms 0, so a
// start adds the time away. (The LP timer runs on the RC oscillator in deep sleep: about a percent.)
static int64_t s_net_off;
static RTC_NOINIT_ATTR struct {
    int64_t off;      // s_net_off
    int64_t rtc0_us;  // LP timer when esp_timer was 0, that start
    uint32_t check;
} s_nt;
static uint32_t nt_check(void) { return esp_rom_crc32_le(0x6E74u, (const uint8_t *)&s_nt, offsetof(typeof(s_nt), check)); }
static void nt_save(void)
{
    s_nt.off = s_net_off;
    s_nt.check = nt_check();
}
static uint32_t s_now;
static mesh_status_t s_status;
static bool s_status_ok;
static uint16_t s_nb[MAX_NEIGHBOURS];  // neighbour list last published
static int s_nb_n = -1;
static bool s_off;
static uint32_t s_off_since;

void mesh_set_off(bool off)
{
    if (off == s_off) return;
    s_off = off;
    s_off_since = s_now;
    ESP_LOGI(TAG, "%s", off ? "powered off: dropping button links shortly" : "on: button links allowed");
}

static bool links_dropped(void) { return s_off && s_now - s_off_since >= OFF_DROP_MS; }

static inline bool newer32(uint32_t a, uint32_t b) { return (int32_t)(a - b) > 0; }
static inline bool newer16(uint16_t a, uint16_t b) { return (int16_t)(a - b) > 0; }
static inline uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static inline uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static inline void wr16(uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
static inline void wr32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }

uint32_t mesh_net_ms(void) { return (uint32_t)((int64_t)s_now + s_net_off); }
int64_t mesh_net_ms64(void) { return esp_timer_get_time() / 1000 + s_net_off; }

void mesh_time_init(bool keep)
{
    int64_t rtc0 = esp_rtc_get_time_us() - esp_timer_get_time();
    if (keep && s_nt.check == nt_check() && rtc0 >= s_nt.rtc0_us) s_net_off = s_nt.off + (rtc0 - s_nt.rtc0_us) / 1000;
    else s_net_off = 0;
    s_nt.rtc0_us = rtc0;
    nt_save();
    ESP_LOGI(TAG, "network time %lld ms (%s)", (long long)mesh_net_ms64(), keep ? "carried" : "from 0");
}

// ---- origins and links ------------------------------------------------------------

static int origin_find(uint16_t id)
{
    for (int i = 0; i < MAX_ORIGINS; i++)
        if (s_o[i].kind != K_NONE && s_o[i].id == id) return i;
    return -1;
}

static void origin_clear(int i)
{
    memset(&s_o[i], 0, sizeof s_o[i]);
    for (int k = 0; k < MAX_LINKS; k++) {
        s_l[k].pend[i] = 0;
        s_l[k].cmd_sent[i] = 0;
        s_l[k].seen &= ~(1u << i);
    }
}

static int origin_add(uint16_t id, uint8_t kind)
{
    int slot = -1;
    for (int i = 1; i < MAX_ORIGINS && slot < 0; i++)
        if (s_o[i].kind == K_NONE) slot = i;
    // Full: forget the quietest admin, then the quietest button not heard from for a minute.
    for (int pass = 0; pass < 2 && slot < 0; pass++) {
        uint32_t oldest = 0;
        for (int i = 1; i < MAX_ORIGINS; i++) {
            origin_t *o = &s_o[i];
            uint32_t age = s_now - o->touched;
            if ((pass == 0 ? o->kind == K_ADMIN : age > 60000) && age >= oldest) {
                oldest = age;
                slot = i;
            }
        }
    }
    if (slot < 0) {
        ESP_LOGW(TAG, "no room for origin %04X", id);
        return -1;
    }
    if (s_o[slot].kind != K_NONE) ESP_LOGI(TAG, "forgetting origin %04X", s_o[slot].id);
    origin_clear(slot);
    s_o[slot].id = id;
    s_o[slot].kind = kind;
    s_o[slot].touched = s_now;
    return slot;
}

static link_t *link_get(uint16_t conn, bool create, bool central)
{
    link_t *free_l = NULL;
    for (int i = 0; i < MAX_LINKS; i++) {
        if (s_l[i].used && s_l[i].conn == conn) return &s_l[i];
        if (!s_l[i].used && !free_l) free_l = &s_l[i];
    }
    if (!create || !free_l) return NULL;
    memset(free_l, 0, sizeof *free_l);
    free_l->used = true;
    free_l->conn = conn;
    free_l->central = central;
    free_l->last_rx = s_now;
    // Until a summary says otherwise, assume the peer has every command we have.
    for (int i = 0; i < MAX_ORIGINS; i++) free_l->cmd_sent[i] = s_o[i].cmd_h;
    return free_l;
}

static bool link_fresh(const link_t *l)
{
    return l->used && l->ready && l->kind == K_BUTTON && s_now - l->last_rx < FRESH_MS;
}

// Does button a (by its published neighbour list) have a working direct link to node b?
static bool has_fresh_link(uint16_t a, uint16_t b)
{
    int i = origin_find(a);
    if (i < 0 || s_o[i].len[S_SLOW] < sizeof(mesh_status_t) + 1) return false;
    const uint8_t *p = s_o[i].data[S_SLOW] + sizeof(mesh_status_t);
    int n = p[0];
    if (sizeof(mesh_status_t) + 1 + n * 4 > s_o[i].len[S_SLOW]) return false;
    for (int k = 0; k < n; k++)
        if (rd16(p + 1 + k * 4) == b) return true;
    return false;
}

// Relay rule: pass state from origin o to link l unless l's button hears o directly.
static bool should_send_state(const link_t *l, const origin_t *o)
{
    if (!l->used || !l->ready) return false;
    if (l->kind == K_ADMIN) return o->kind == K_BUTTON;
    if (l->kind != K_BUTTON || l->peer == o->id) return false;
    if (o->id == s_me) return true;
    return !has_fresh_link(l->peer, o->id);
}

// Commands are judged by the button they entered the network at.
static bool should_send_cmd(const link_t *l, uint16_t via)
{
    if (!l->used || !l->ready || l->kind != K_BUTTON || l->peer == via) return false;
    if (via == s_me) return true;
    return !has_fresh_link(l->peer, via);
}

static uint32_t digest(void)
{
    uint32_t d = 0;
    for (int i = 0; i < MAX_ORIGINS; i++) {
        const origin_t *o = &s_o[i];
        if (o->kind == K_NONE || (i == 0 && !s_me)) continue;
        uint8_t b[17];
        wr16(b, i == 0 ? s_me : o->id);
        b[2] = o->kind;
        for (int s = 0; s < S_LATEST; s++) wr32(b + 3 + s * 4, o->len[s] ? o->ver[s] : 0);
        wr16(b + 15, o->kind == K_ADMIN ? o->cmd_h : 0);
        d ^= esp_rom_crc32_le(0, b, sizeof b);
    }
    return d;
}

// ---- frames -----------------------------------------------------------------------

typedef struct {
    uint8_t b[BLE_FRAME_MAX];
    size_t n, max;
} fb_t;

static void fb_start(fb_t *f, uint8_t type, size_t max)
{
    f->max = max > sizeof f->b ? sizeof f->b : max;
    f->b[0] = FRAME_VERSION;
    f->b[1] = type;
    wr32(f->b + 2, mesh_net_ms());
    f->n = HDR;
}

static bool fb_item(fb_t *f, uint16_t origin, uint8_t stream, uint32_t ver, const uint8_t *p, uint8_t len)
{
    if (f->n + ITEM_HDR + len > f->max) return false;
    uint8_t *q = f->b + f->n;
    wr16(q, origin);
    q[2] = stream;
    wr32(q + 3, ver);
    q[7] = len;
    memcpy(q + ITEM_HDR, p, len);
    f->n += ITEM_HDR + len;
    return true;
}

static bool fb_send(link_t *l, const fb_t *f)
{
    int rc = ble_send_frame(l->conn, f->b, f->n);
    if (rc == 0) return true;
    if (rc != BLE_HS_ENOMEM && rc != BLE_HS_EBUSY) ESP_LOGW(TAG, "conn %d: send frame type %d: %d", l->conn, f->b[1], rc);
    return false;
}

static void send_hello(link_t *l)
{
    fb_t f;
    fb_start(&f, F_HELLO, 32);
    wr16(f.b + HDR, s_me);
    f.b[HDR + 2] = K_BUTTON;
    f.b[HDR + 3] = 0;
    f.n = HDR + 4;
    if (fb_send(l, &f)) l->hello_sent = true;
}

static void send_heartbeat(link_t *l)
{
    fb_t f;
    fb_start(&f, F_HEARTBEAT, 32);
    wr32(f.b + HDR, l->kind == K_BUTTON ? digest() : 0);
    f.n = HDR + 4;
    if (fb_send(l, &f)) l->last_hb = s_now;
}

// Everything we hold, in as many frames as it takes. To phones: buttons' state only.
static bool send_summary(link_t *l)
{
    size_t max = ble_frame_max(l->conn);
    fb_t f;
    bool first = true;
    int i = 0;
    while (true) {
        fb_start(&f, F_SUMMARY, max);
        f.b[HDR] = first ? 1 : 0;
        f.n = HDR + 1;
        for (; i < MAX_ORIGINS && f.n + SUM_ENTRY <= f.max; i++) {
            const origin_t *o = &s_o[i];
            if (o->kind == K_NONE || (i == 0 && !s_me)) continue;
            if (l->kind == K_ADMIN && o->kind != K_BUTTON) continue;
            uint8_t *q = f.b + f.n;
            wr16(q, o->id);
            q[2] = o->kind;
            for (int s = 0; s < S_LATEST; s++) wr32(q + 3 + s * 4, o->len[s] ? o->ver[s] : 0);
            wr16(q + 15, o->kind == K_ADMIN ? o->cmd_h : 0);
            f.n += SUM_ENTRY;
        }
        bool last = i >= MAX_ORIGINS;
        if (last) f.b[HDR] |= 2;
        if (!fb_send(l, &f)) return false;
        first = false;
        if (last) break;
    }
    l->last_sum = s_now;
    l->want_sum = false;
    return true;
}

// Pending items for one link, as many as fit in one frame; marks them sent only if it went.
static void send_data(link_t *l)
{
    if (l->kind == K_ADMIN && !l->active) return;
    if (l->kind != K_BUTTON && l->kind != K_ADMIN) return;
    fb_t f;
    fb_start(&f, F_ITEMS, ble_frame_max(l->conn));
    uint8_t pend[MAX_ORIGINS];
    uint16_t sent[MAX_ORIGINS];
    memcpy(pend, l->pend, sizeof pend);
    memcpy(sent, l->cmd_sent, sizeof sent);
    bool full = false;
    for (int i = 0; i < MAX_ORIGINS && !full; i++) {
        origin_t *o = &s_o[i];
        if (o->kind == K_NONE) continue;
        for (int s = 0; s < S_LATEST && !full; s++) {
            if (!(pend[i] & (1 << s))) continue;
            if (!o->len[s]) {
                pend[i] &= ~(1 << s);
                continue;
            }
            if (fb_item(&f, i == 0 ? s_me : o->id, s, o->ver[s], o->data[s], o->len[s])) pend[i] &= ~(1 << s);
            else full = true;
        }
        if (o->kind != K_ADMIN || l->kind != K_BUTTON) continue;
        while (!full && newer16(o->cmd_h, sent[i])) {
            uint16_t next = sent[i] + 1;
            const cmd_t *c = &o->log[next % LOG_N];
            if (!c->len || c->seq != next) {  // older than the log: gone
                sent[i] = next;
                continue;
            }
            if (fb_item(&f, o->id, S_CMD, c->seq, c->d, c->len)) sent[i] = next;
            else full = true;
        }
    }
    if (f.n == HDR) {  // nothing (left) to send
        memcpy(l->pend, pend, sizeof pend);
        memcpy(l->cmd_sent, sent, sizeof sent);
        return;
    }
    if (!fb_send(l, &f)) return;
    memcpy(l->pend, pend, sizeof pend);
    memcpy(l->cmd_sent, sent, sizeof sent);
    l->last_data = s_now;
}

static bool link_has_data(const link_t *l)
{
    if (l->kind == K_ADMIN && !l->active) return false;
    for (int i = 0; i < MAX_ORIGINS; i++) {
        if (l->pend[i]) return true;
        if (l->kind == K_BUTTON && s_o[i].kind == K_ADMIN && newer16(s_o[i].cmd_h, l->cmd_sent[i])) return true;
    }
    return false;
}

// ---- taking items in ---------------------------------------------------------------

static void mark_state(int i, int s, const link_t *src)
{
    for (int k = 0; k < MAX_LINKS; k++) {
        link_t *l = &s_l[k];
        if (!l->used) continue;
        if (l != src && should_send_state(l, &s_o[i])) l->pend[i] |= 1 << s;
        else l->pend[i] &= ~(1 << s);
    }
}

static void own_publish(int s, const void *p, size_t n)
{
    origin_t *o = &s_o[0];
    if (n > ITEM_MAX) n = ITEM_MAX;
    o->kind = K_BUTTON;
    o->id = s_me;
    o->ver[s] = (uint32_t)s_epoch << 24 | (++s_seq & 0xFFFFFF);
    memcpy(o->data[s], p, n);
    o->len[s] = (uint8_t)n;
    o->touched = s_now;
    mark_state(0, s, NULL);
}

static void take_state(link_t *src, uint16_t id, int s, uint32_t ver, const uint8_t *p, uint8_t len)
{
    if (id == s_me || len > ITEM_MAX) return;
    int i = origin_find(id);
    if (i < 0) i = origin_add(id, K_BUTTON);
    if (i < 0 || s_o[i].kind != K_BUTTON) return;
    origin_t *o = &s_o[i];
    if (o->len[s] && !newer32(ver, o->ver[s])) {
        src->pend[i] &= ~(1 << s);  // they have it (or newer)
        return;
    }
    o->ver[s] = ver;
    memcpy(o->data[s], p, len);
    o->len[s] = len;
    o->touched = s_now;
    mark_state(i, s, src);
}

static void cmd_commit(int i, const cmd_t *c, const link_t *src)
{
    origin_t *o = &s_o[i];
    o->log[c->seq % LOG_N] = *c;
    o->cmd_h = c->seq;
    o->touched = s_now;
    uint16_t via = rd16(c->d);
    uint32_t expiry = rd32(c->d + 2);
    uint16_t target = rd16(c->d + 6);
    for (int k = 0; k < MAX_LINKS; k++) {
        link_t *l = &s_l[k];
        if (!l->used) continue;
        bool send = l != src && should_send_cmd(l, via);
        if (!send && l->cmd_sent[i] == (uint16_t)(c->seq - 1)) l->cmd_sent[i] = c->seq;
    }
    if (target != s_me && target != MESH_ALL) return;
    if (newer32(mesh_net_ms(), expiry)) {
        ESP_LOGW(TAG, "command %04X:%u expired %lu ms ago, not acted on", o->id, c->seq,
                 (unsigned long)(mesh_net_ms() - expiry));
        return;
    }
    if (s_exec_n >= EXEC_N) {
        ESP_LOGW(TAG, "command %04X:%u dropped: too many at once", o->id, c->seq);
        return;
    }
    exec_t *e = &s_exec[s_exec_n++];
    e->kind = c->d[8];
    e->len = c->len - CMD_HDR;
    memcpy(e->d, c->d + CMD_HDR, e->len);
}

// Commands from one admin are taken strictly in seq order, each once; a later one waits for a
// missing earlier one (up to GAP_SKIP_MS).
static void take_cmd(link_t *src, uint16_t id, uint16_t seq, const uint8_t *p, uint8_t len)
{
    if (len < CMD_HDR + 1 || len > CMD_MAX) return;
    int i = origin_find(id);
    if (i < 0) {
        i = origin_add(id, K_ADMIN);
        if (i < 0) return;
        s_o[i].cmd_h = seq - 1;
        for (int k = 0; k < MAX_LINKS; k++) s_l[k].cmd_sent[i] = seq - 1;
    }
    origin_t *o = &s_o[i];
    if (o->kind != K_ADMIN) return;
    if (src && newer16(seq, src->cmd_sent[i])) {
        if (src->cmd_sent[i] == (uint16_t)(seq - 1)) src->cmd_sent[i] = seq;
    }
    if (!newer16(seq, o->cmd_h)) return;  // already have it
    cmd_t c = {.seq = seq, .len = len};
    memcpy(c.d, p, len);
    if (seq != (uint16_t)(o->cmd_h + 1)) {
        if ((uint16_t)(seq - o->cmd_h) > 64) return;
        int free_k = -1;
        for (int k = 0; k < PEND_N; k++) {
            if (o->pend[k].len && o->pend[k].seq == seq) return;
            if (!o->pend[k].len && free_k < 0) free_k = k;
        }
        if (free_k < 0) {
            ESP_LOGW(TAG, "command %04X:%u dropped: waiting on too many", id, seq);
            return;
        }
        o->pend[free_k] = c;
        o->pend_since[free_k] = s_now;
        return;
    }
    cmd_commit(i, &c, src);
    // Anything held back that's now next in line.
    for (bool more = true; more;) {
        more = false;
        for (int k = 0; k < PEND_N; k++) {
            if (o->pend[k].len && o->pend[k].seq == (uint16_t)(o->cmd_h + 1)) {
                cmd_t n = o->pend[k];
                o->pend[k].len = 0;
                cmd_commit(i, &n, NULL);
                more = true;
            } else if (o->pend[k].len && !newer16(o->pend[k].seq, o->cmd_h)) {
                o->pend[k].len = 0;
            }
        }
    }
}

static void on_items(link_t *l, const uint8_t *p, size_t n)
{
    while (n >= ITEM_HDR) {
        uint16_t id = rd16(p);
        uint8_t s = p[2];
        uint32_t ver = rd32(p + 3);
        uint8_t len = p[7];
        if (ITEM_HDR + len > n) break;
        if (s == S_CMD) take_cmd(l, id, (uint16_t)ver, p + ITEM_HDR, len);
        else if (s < S_LATEST && l->kind == K_BUTTON) take_state(l, id, s, ver, p + ITEM_HDR, len);
        p += ITEM_HDR + len;
        n -= ITEM_HDR + len;
    }
}

static void on_summary(link_t *l, const uint8_t *p, size_t n)
{
    if (n < 1) return;
    uint8_t flags = p[0];
    p++, n--;
    if (flags & 1) l->seen = 0;
    for (; n >= SUM_ENTRY; p += SUM_ENTRY, n -= SUM_ENTRY) {
        uint16_t id = rd16(p);
        uint8_t kind = p[2];
        int i = id == s_me ? 0 : origin_find(id);
        if (i < 0) {
            if (l->kind == K_BUTTON) l->want_sum = true;  // they have something we don't
            continue;
        }
        origin_t *o = &s_o[i];
        l->seen |= 1u << i;
        for (int s = 0; s < S_LATEST; s++) {
            uint32_t theirs = rd32(p + 3 + s * 4);
            if (o->len[s] && (!theirs || newer32(o->ver[s], theirs))) l->pend[i] |= 1 << s;
            else if (theirs && (!o->len[s] || newer32(theirs, o->ver[s])) && l->kind == K_BUTTON) l->want_sum = true;
        }
        if (l->kind == K_BUTTON && kind == K_ADMIN && o->kind == K_ADMIN) {
            uint16_t theirs = rd16(p + 15);
            l->cmd_sent[i] = theirs;
            if (newer16(theirs, o->cmd_h)) l->want_sum = true;
        }
    }
    if (!(flags & 2)) return;
    // Last part: whatever they didn't list, they don't have.
    for (int i = 0; i < MAX_ORIGINS; i++) {
        origin_t *o = &s_o[i];
        if (o->kind == K_NONE || (l->seen & (1u << i)) || (i == 0 && !s_me)) continue;
        if (l->kind == K_ADMIN && o->kind != K_BUTTON) continue;
        for (int s = 0; s < S_LATEST; s++)
            if (o->len[s]) l->pend[i] |= 1 << s;
        if (o->kind == K_ADMIN && l->kind == K_BUTTON) l->cmd_sent[i] = o->cmd_h - LOG_N;
    }
}

static void on_hello(link_t *l, const uint8_t *p, size_t n)
{
    if (n < 4) return;
    uint16_t id = rd16(p);
    uint8_t kind = p[2];
    bool active = p[3] & 1;
    if (kind == K_BUTTON) {
        if (id == s_me) return;
        if (links_dropped()) {
            ESP_LOGI(TAG, "conn %d: button %04X while off, dropping", l->conn, id);
            ble_drop(l->conn);
            return;
        }
        // A button we already have a working link to (both connected at once): ble.c drops one.
        bool fresh_l = !l->ready || l->peer != id;
        l->kind = K_BUTTON;
        l->peer = id;
        l->ready = true;
        if (fresh_l) {
            ESP_LOGI(TAG, "conn %d: button %04X (%s)", l->conn, id, l->central ? "we connected" : "it connected");
            l->want_sum = true;
            l->last_sum = s_now - SUMMARY_GAP_MS;
        }
    } else if (kind == K_ADMIN) {
        bool was = l->ready && l->active;
        l->kind = K_ADMIN;
        l->peer = id;
        l->ready = true;
        l->active = active;
        if (active != was) ESP_LOGI(TAG, "conn %d: phone %04X, %s", l->conn, id, active ? "active" : "passive");
        if (!active) memset(l->pend, 0, sizeof l->pend);
    } else {
        return;
    }
    if (!l->hello_sent || kind == K_ADMIN) send_hello(l);
}

static void on_frame(link_t *l, const uint8_t *p, size_t n)
{
    if (n < HDR || p[0] != FRAME_VERSION) return;
    l->last_rx = s_now;
    uint32_t their_net = rd32(p + 2);
    if (l->kind == K_BUTTON && their_net) {
        int32_t d = (int32_t)(their_net - mesh_net_ms());
        if (d > 0) {
            s_net_off += d;  // network time only moves forward: everyone ends up on the furthest-ahead clock
            nt_save();
            if (d > 1000) ESP_LOGI(TAG, "network clock +%ld ms (from %04X)", (long)d, l->peer);
        }
    }
    switch (p[1]) {
    case F_HELLO:
        on_hello(l, p + HDR, n - HDR);
        break;
    case F_ITEMS:
        if (l->ready) on_items(l, p + HDR, n - HDR);
        break;
    case F_HEARTBEAT:
        if (l->ready && l->kind == K_BUTTON && n >= HDR + 4) {
            if (rd32(p + HDR) == digest()) l->mismatch_since = 0;
            else if (!l->mismatch_since) l->mismatch_since = s_now | 1;
            else if (s_now - l->mismatch_since > MISMATCH_MS) {
                l->want_sum = true;
                l->mismatch_since = s_now | 1;
            }
        }
        break;
    case F_SUMMARY:
        if (l->ready) on_summary(l, p + HDR, n - HDR);
        break;
    default:
        break;
    }
}

// ---- own items ---------------------------------------------------------------------

static int fresh_neighbours(uint16_t *ids, link_t **links)
{
    int n = 0;
    for (int k = 0; k < MAX_LINKS && n < MAX_NEIGHBOURS; k++) {
        if (!link_fresh(&s_l[k])) continue;
        bool dup = false;
        for (int j = 0; j < n; j++) dup |= ids[j] == s_l[k].peer;
        if (dup) continue;
        if (links) links[n] = &s_l[k];
        ids[n++] = s_l[k].peer;
    }
    // Sorted, so the list compares equal whatever order the links are in.
    for (int a = 1; a < n; a++)
        for (int b = a; b > 0 && ids[b - 1] > ids[b]; b--) {
            uint16_t t = ids[b];
            ids[b] = ids[b - 1];
            ids[b - 1] = t;
            if (links) {
                link_t *tl = links[b];
                links[b] = links[b - 1];
                links[b - 1] = tl;
            }
        }
    return n;
}

static uint8_t s_status_tail[MESH_STATUS_TAIL];
static int s_status_tail_n;
static void publish_slow(void)
{
    if (!s_status_ok) return;
    uint8_t b[ITEM_MAX];
    s_status.t = mesh_net_ms();
    memcpy(b, &s_status, sizeof s_status);
    uint16_t ids[MAX_NEIGHBOURS];
    link_t *links[MAX_NEIGHBOURS];
    int n = fresh_neighbours(ids, links);
    if (sizeof s_status + 1 + n * 4 + s_status_tail_n > sizeof b) n = (sizeof b - sizeof s_status - 1 - s_status_tail_n) / 4;
    uint8_t *q = b + sizeof s_status;
    q[0] = (uint8_t)n;
    for (int k = 0; k < n; k++) {
        int8_t rssi = 127;
        uint8_t phy = 0;
        ble_link_quality(links[k]->conn, &rssi, &phy);
        wr16(q + 1 + k * 4, ids[k]);
        q[3 + k * 4] = (uint8_t)rssi;
        q[4 + k * 4] = phy;
    }
    memcpy(s_nb, ids, n * sizeof ids[0]);
    s_nb_n = n;
    memcpy(q + 1 + n * 4, s_status_tail, s_status_tail_n);
    own_publish(S_SLOW, b, sizeof s_status + 1 + n * 4 + s_status_tail_n);
}

void mesh_publish_hot(const mesh_hot_t *h)
{
    mesh_hot_t c = *h;
    c.t = mesh_net_ms();
    own_publish(S_HOT, &c, sizeof c);
}

void mesh_publish_status(const mesh_status_t *s, const void *tail, int tail_n)
{
    s_status = *s;
    s_status_tail_n = tail_n < 0 ? 0 : tail_n > MESH_STATUS_TAIL ? MESH_STATUS_TAIL : tail_n;
    memcpy(s_status_tail, tail, s_status_tail_n);
    s_status_ok = true;
    publish_slow();
}

void mesh_publish_config(const void *cfg, size_t n) { own_publish(S_CFG, cfg, n); }

bool mesh_peer(int i, mesh_peer_t *p)
{
    if (i < 1 || i >= MAX_ORIGINS || s_o[i].kind != K_BUTTON) return false;
    p->id = s_o[i].id;
    for (int s = 0; s < S_LATEST; s++) {
        p->d[s] = s_o[i].data[s];
        p->len[s] = s_o[i].len[s];
    }
    return true;
}
uint16_t mesh_id(void) { return s_me; }

int mesh_button_links(void)
{
    uint16_t ids[MAX_NEIGHBOURS];
    return fresh_neighbours(ids, NULL);
}

// ---- init and tick ----------------------------------------------------------------

void mesh_init(mesh_cmd_fn on_cmd)
{
    s_on_cmd = on_cmd;
    // Boot epoch in the top byte of every version this button sends, so a reboot's items are
    // newer than the last boot's even though its counter starts again.
    nvs_handle_t h;
    esp_err_t err = nvs_open("mesh", NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_get_u8(h, "epoch", &s_epoch);
        if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) ESP_LOGW(TAG, "read epoch: %s", esp_err_to_name(err));
        s_epoch++;
        err = nvs_set_u8(h, "epoch", s_epoch);
        if (err == ESP_OK) err = nvs_commit(h);
        if (err != ESP_OK) ESP_LOGW(TAG, "save epoch: %s", esp_err_to_name(err));
        nvs_close(h);
    } else {
        ESP_LOGW(TAG, "nvs open: %s", esp_err_to_name(err));
    }
    s_o[0].kind = K_BUTTON;
    ESP_LOGI(TAG, "epoch %u", s_epoch);
}

void mesh_tick(uint32_t now)
{
    s_now = now;
    if (!s_me) {
        s_me = ble_node_id();
        if (!s_me) return;
        s_o[0].id = s_me;
        ESP_LOGI(TAG, "node %04X", s_me);
    }

    ble_ev_t e;
    for (int k = 0; k < 16 && ble_poll_event(&e); k++) {
        link_t *l = link_get(e.conn, e.kind != BLE_EV_DOWN, e.central);
        if (!l) continue;
        if (e.kind == BLE_EV_UP) {
            send_hello(l);
        } else if (e.kind == BLE_EV_DOWN) {
            if (l->kind) ESP_LOGI(TAG, "conn %d: %s %04X gone", l->conn, l->kind == K_BUTTON ? "button" : "phone", l->peer);
            l->used = false;
        } else {
            on_frame(l, e.data, e.len);
        }
    }

    // Powered off for a moment: let go of the other buttons (phones stay).
    if (links_dropped()) {
        for (int k = 0; k < MAX_LINKS; k++) {
            link_t *l = &s_l[k];
            if (!l->used || l->kind != K_BUTTON || l->ready == false) continue;
            ESP_LOGI(TAG, "conn %d: off, dropping button %04X", l->conn, l->peer);
            ble_drop(l->conn);
            l->ready = false;  // until the disconnect arrives
        }
    }

    // Neighbours changed (a link came up, went quiet or dropped): tell everyone at once.
    uint16_t ids[MAX_NEIGHBOURS];
    int n = fresh_neighbours(ids, NULL);
    if (n != s_nb_n || memcmp(ids, s_nb, n * sizeof ids[0]) != 0) {
        ESP_LOGI(TAG, "%d button link(s) working", n);
        publish_slow();
        // A button that went quiet may have missed things relayed past it; check once it's back.
        for (int k = 0; k < MAX_LINKS; k++)
            if (link_fresh(&s_l[k])) s_l[k].want_sum = true;
    }

    // Give up on commands held back for a missing earlier one; forget quiet admins.
    for (int i = 1; i < MAX_ORIGINS; i++) {
        origin_t *o = &s_o[i];
        if (o->kind != K_ADMIN) continue;
        int oldest = -1;
        for (int k = 0; k < PEND_N; k++)
            if (o->pend[k].len && (oldest < 0 || newer16(o->pend[oldest].seq, o->pend[k].seq))) oldest = k;
        if (oldest >= 0 && now - o->pend_since[oldest] > GAP_SKIP_MS) {
            ESP_LOGW(TAG, "commands %04X:%u..%u never arrived, skipped", o->id, (uint16_t)(o->cmd_h + 1),
                     (uint16_t)(o->pend[oldest].seq - 1));
            o->cmd_h = o->pend[oldest].seq - 1;
            cmd_t c = o->pend[oldest];
            o->pend[oldest].len = 0;
            take_cmd(NULL, o->id, c.seq, c.d, c.len);
        } else if (oldest < 0 && now - o->touched > ADMIN_IDLE_MS) {
            origin_clear(i);
        }
    }

    for (int k = 0; k < MAX_LINKS; k++) {
        link_t *l = &s_l[k];
        if (!l->used || !l->ready) continue;
        if (l->want_sum && l->kind == K_BUTTON && now - l->last_sum >= SUMMARY_GAP_MS) send_summary(l);
        uint32_t gap = ble_conn_itvl_ms(l->conn);
        if (gap < DATA_GAP_MIN_MS) gap = DATA_GAP_MIN_MS;
        if (now - l->last_data >= gap && link_has_data(l)) send_data(l);
        if (now - l->last_hb >= HB_MS) send_heartbeat(l);
    }

    // Act on commands for this button last, after they've been passed on (a reboot or power off
    // shouldn't stop the rest of the network hearing about it).
    if (s_exec_n) {
        for (int k = 0; k < MAX_LINKS; k++)
            if (s_l[k].used && s_l[k].ready && s_l[k].kind == K_BUTTON && link_has_data(&s_l[k])) send_data(&s_l[k]);
    }
    for (int k = 0; k < s_exec_n; k++) {
        if (s_on_cmd) s_on_cmd(s_exec[k].kind, s_exec[k].d, s_exec[k].len, now);
    }
    s_exec_n = 0;
}
