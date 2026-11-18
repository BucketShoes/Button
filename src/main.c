// Capture button: basic two-team "reverse chess clock" + hardware test.
//
// - Each team starts with 10:00. The owning team's clock counts down; a team
//   wins when its clock reaches 0:00 (it has held the point for 10 minutes).
// - Face press = OR of the three face switches, 100 ms release debounce.
// - Hold 3 s: capture (switch to the other team; random team if unowned).
// - Keep holding to 6 s: pause (undo the capture, ring off); the next
//   3 s hold resumes with the owner from before the pause.
// - Keep holding to 10 s: reset both clocks to 10:00.
// - Triple tap: show VSYS / VBUS on the displays for a few seconds.
//
// USB-only bench test: no Wi-Fi, BLE or sleep.

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"

#include "board.h"
#include "audio.h"
#include "display.h"
#include "leds.h"
#include "sense.h"

static const char *TAG = "button";

// ---------------------------------------------------------------- tuning
#define GAME_MS          (10 * 60 * 1000)
#define HOLD_CAPTURE_MS  3000
#define HOLD_PAUSE_MS    6000
#define HOLD_RESET_MS    10000
#define DEBOUNCE_MS      100   // release must be seen this long before it counts
#define TAP_MAX_MS       500   // a press shorter than this is a tap
#define TAP_GAP_MS       600   // max gap between taps; also the wait after the last one
#define BATT_SHOW_MS     4000
#define LOOP_MS          5

#define CENTRE_LEVEL     64    // team-colour centre LEDs, "medium"
#define RING_LEVEL       255   // ring while owned
#define EXT_LEVEL        128   // external strip, relative to the ring
#define DISP_FULL        8
#define DISP_DIM         2
#define DISP_IDLE        6

static const rgb_t TEAM_COLOUR[2] = {
    {150, 0, 255},  // team 1: purple
    {255, 170, 0},  // team 2: yellow
};
static const rgb_t WHITE = {255, 255, 255};
static const rgb_t RED = {255, 0, 0};
static const rgb_t GREEN = {0, 255, 0};

// Ring positions in clockwise order from north (chain indices).
static const int RING_CW[6] = {3 /*N*/, 2 /*NE*/, 7 /*SE*/, 6 /*S*/, 5 /*SW*/, 4 /*NW*/};

// ---------------------------------------------------------------- sounds
static const note_t SND_CLICK[] = {{1, 1, 6, W_NOISE, 200}};
static const note_t SND_SW[3][1] = {
    {{700, 700, 35, W_SQUARE, 110}},
    {{1050, 1050, 35, W_TRI, 200}},
    {{1500, 1500, 35, W_SQUARE, 110}},
};
static const note_t SND_BUILD[] = {{250, 1200, HOLD_CAPTURE_MS, W_SQUARE, 70}};
static const note_t SND_TEAM[2][5] = {
    {   // team 1: rising square arpeggio (sine was too quiet on the FS-1540)
        {1047, 1047, 100, W_SQUARE, 160}, {1319, 1319, 100, W_SQUARE, 160}, {1568, 1568, 100, W_SQUARE, 160},
        {2093, 2093, 350, W_SQUARE, 160}, {0, 0, 1, W_SQUARE, 0},
    },
    {   // team 2: square "da-da-DAAA"
        {880, 880, 90, W_SQUARE, 160}, {0, 0, 40, W_SQUARE, 0}, {880, 880, 90, W_SQUARE, 160},
        {0, 0, 40, W_SQUARE, 0}, {1319, 1319, 400, W_SQUARE, 160},
    },
};
static const note_t SND_TICK[] = {{1600, 1600, 15, W_TRI, 180}};
static const note_t SND_PAUSE[] = {{1200, 300, 450, W_TRI, 230}};
static const note_t SND_RESET[] = {
    {1400, 1400, 100, W_SQUARE, 140}, {1100, 1100, 100, W_SQUARE, 140},
    {800, 800, 100, W_SQUARE, 140}, {500, 500, 400, W_SQUARE, 140},
};
static const note_t SND_WIN[] = {
    {784, 784, 150, W_SQUARE, 150}, {1047, 1047, 150, W_SQUARE, 150}, {1319, 1319, 150, W_SQUARE, 150},
    {1568, 1568, 500, W_SQUARE, 150}, {0, 0, 100, W_SQUARE, 0}, {1319, 1319, 150, W_SQUARE, 150},
    {1568, 1568, 800, W_SQUARE, 150},
};
static const note_t SND_BOOT[] = {  // diagnostic: sine 440 / 880 / 1760, then noise, then sweep
    {440, 440, 200, W_SINE, 255}, {880, 880, 200, W_SINE, 255}, {1760, 1760, 200, W_SINE, 255},
    {1, 1, 150, W_NOISE, 160}, {200, 4000, 600, W_TRI, 200},
};
static const note_t SND_STARTUP[] = {{600, 1800, 150, W_TRI, 200}, {1800, 1800, 60, W_TRI, 200}};
static const note_t SND_BATT[] = {{1000, 1000, 60, W_SINE, 200}, {1500, 1500, 60, W_SINE, 200}};

// ---------------------------------------------------------------- inputs
typedef struct {
    gpio_num_t pin;
    const char *name;
    bool state;               // debounced pressed
    uint32_t press_ms;        // when the current press started
    uint32_t last_active_ms;  // last time the raw contact was seen closed
} input_t;

enum { IN_FACE1, IN_FACE2, IN_FACE3, IN_BOOT, IN_PEDAL1, IN_PEDAL2, IN_COUNT };
static input_t s_in[IN_COUNT] = {
    {PIN_FACE1, "face1(W)"}, {PIN_FACE2, "face2(SE)"}, {PIN_FACE3, "face3(NE)"},
    {PIN_BOOT, "boot"},      {PIN_PEDAL1, "pedal1"},   {PIN_PEDAL2, "pedal2"},
};
static input_t s_face = {GPIO_NUM_NC, "face"};  // OR of the three switches

// Press registers on the first closed sample; release only after DEBOUNCE_MS
// of continuously open. Returns +1 on press, -1 on release, 0 otherwise.
static int input_update(input_t *in, bool raw, uint32_t now)
{
    if (raw) {
        in->last_active_ms = now;
        if (!in->state) {
            in->state = true;
            in->press_ms = now;
            return 1;
        }
    } else if (in->state && now - in->last_active_ms >= DEBOUNCE_MS) {
        in->state = false;
        return -1;
    }
    return 0;
}

// Held time measured to the last closed sample, so the debounce tail
// doesn't add time after a real release.
static uint32_t input_held_ms(const input_t *in) { return in->state ? in->last_active_ms - in->press_ms : 0; }

static void inputs_init(void)
{
    uint64_t mask = 0;
    for (int i = 0; i < IN_COUNT; i++) mask |= 1ULL << s_in[i].pin;
    gpio_config_t io = {
        .pin_bit_mask = mask,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,  // no external pull-ups on any of these
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
}

// ---------------------------------------------------------------- game state
typedef enum { ST_IDLE, ST_RUNNING, ST_WON } state_t;
static const char *const STATE_NAME[] = {"idle", "running", "won"};

static state_t s_state = ST_IDLE;
static int s_owner = -1;   // -1 none, 0 team 1, 1 team 2
static int s_winner = -1;
static int32_t s_rem[2] = {GAME_MS, GAME_MS};

static struct {
    state_t state;
    int owner;
    int32_t rem[2];
} s_snap;                  // state when the current hold started
static int s_hold_stage;   // 0 building, 1 captured, 2 paused, 3 reset
static int s_hold_target;  // team the capture will go to, -1 = random
static int s_resume_owner = -1;  // owner before a pause; the next capture goes back to them
static int s_hold_last_sec;

static int s_taps;
static uint32_t s_last_tap_ms;
static uint32_t s_batt_until;

static int s_vsys_mv, s_vbus_mv;

static void fmt_clock(char *buf, size_t n, int32_t ms)
{
    int s = (ms + 999) / 1000;
    snprintf(buf, n, "%02d:%02d", s / 60, s % 60);
}

static void on_face_press(uint32_t now)
{
    s_snap.state = s_state;
    s_snap.owner = s_owner;
    s_snap.rem[0] = s_rem[0];
    s_snap.rem[1] = s_rem[1];
    s_hold_stage = 0;
    s_hold_last_sec = 0;
    s_hold_target = (s_owner >= 0) ? 1 - s_owner : s_resume_owner;

    if (s_taps > 0 && now - s_last_tap_ms > TAP_GAP_MS) s_taps = 0;

    AUDIO_PLAY(SLOT_CLICK, SND_CLICK);
    if (s_state != ST_WON) AUDIO_PLAY(SLOT_BUILD, SND_BUILD);
    ESP_LOGI(TAG, "face press (state %s, owner %d)", STATE_NAME[s_state], s_owner + 1);
}

static void on_face_release(uint32_t now, uint32_t held)
{
    audio_stop(SLOT_BUILD);
    if (held < TAP_MAX_MS) {
        s_taps++;
        s_last_tap_ms = s_face.last_active_ms;
    } else {
        s_taps = 0;
    }
    ESP_LOGI(TAG, "face release after %lu ms (taps %d)", (unsigned long)held, s_taps);
}

static void hold_progress(uint32_t held)
{
    if (s_hold_stage == 0 && held >= HOLD_CAPTURE_MS) {
        s_hold_stage = 1;
        audio_stop(SLOT_BUILD);
        if (s_state != ST_WON) {
            int team = s_hold_target >= 0 ? s_hold_target : (int)(esp_random() & 1);
            s_owner = team;
            s_resume_owner = -1;
            s_state = ST_RUNNING;
            AUDIO_PLAY(SLOT_EVENT, SND_TEAM[team]);
            ESP_LOGI(TAG, "CAPTURED by team %d", team + 1);
        } else {
            AUDIO_PLAY(SLOT_EVENT, SND_TICK);
        }
    }
    if (s_hold_stage == 1 && held >= HOLD_PAUSE_MS) {
        s_hold_stage = 2;
        if (s_state != ST_WON) {
            // Undo this hold entirely, then pause. The owner is remembered and
            // gets the point back on the next 3 s hold.
            s_rem[0] = s_snap.rem[0];
            s_rem[1] = s_snap.rem[1];
            if (s_snap.owner >= 0) s_resume_owner = s_snap.owner;
            s_owner = -1;
            s_state = ST_IDLE;
            ESP_LOGI(TAG, "PAUSED");
        }
        AUDIO_PLAY(SLOT_EVENT, SND_PAUSE);
    }
    if (s_hold_stage == 2 && held >= HOLD_RESET_MS) {
        s_hold_stage = 3;
        s_rem[0] = s_rem[1] = GAME_MS;
        s_owner = s_winner = s_resume_owner = -1;
        s_state = ST_IDLE;
        AUDIO_PLAY(SLOT_EVENT, SND_RESET);
        ESP_LOGI(TAG, "RESET to 10:00");
    }
    // Warning ticks every second after the capture, until the reset.
    int sec = (int)(held / 1000);
    if (s_hold_stage >= 1 && s_hold_stage < 3 && sec != s_hold_last_sec && sec != 3 && sec != 6) {
        AUDIO_PLAY(SLOT_CLICK, SND_TICK);
    }
    s_hold_last_sec = sec;
}

static void game_tick(uint32_t now, uint32_t dt)
{
    if (s_state == ST_RUNNING && s_owner >= 0) {
        s_rem[s_owner] -= (int32_t)dt;
        if (s_rem[s_owner] <= 0) {
            s_rem[s_owner] = 0;
            s_winner = s_owner;
            s_state = ST_WON;
            AUDIO_PLAY(SLOT_EVENT, SND_WIN);
            ESP_LOGI(TAG, "TEAM %d WINS", s_winner + 1);
        }
    }

    if (!s_face.state && s_taps > 0 && now - s_last_tap_ms > TAP_GAP_MS) {
        if (s_taps == 3) {
            s_batt_until = now + BATT_SHOW_MS;
            AUDIO_PLAY(SLOT_EVENT, SND_BATT);
            ESP_LOGI(TAG, "triple tap: VSYS %d mV, VBUS %d mV", s_vsys_mv, s_vbus_mv);
        }
        s_taps = 0;
    }
}

// ---------------------------------------------------------------- rendering
static bool batt_showing(uint32_t now) { return (int32_t)(s_batt_until - now) > 0; }

static void render_displays(uint32_t now)
{
    for (int t = 0; t < 2; t++) {
        uint8_t seg[4];
        int bright;
        if (batt_showing(now)) {
            display_number(seg, t == 0 ? s_vsys_mv : s_vbus_mv);
            bright = DISP_FULL;
        } else {
            int s = (s_rem[t] + 999) / 1000;
            int m = s / 60;
            s %= 60;
            seg[0] = m >= 10 ? display_digit(m / 10) : 0;
            seg[1] = display_digit(m % 10);
            seg[2] = display_digit(s / 10);
            seg[3] = display_digit(s % 10);
            bool colon = true;
            bright = DISP_IDLE;
            if (s_state == ST_RUNNING) {
                if (t == s_owner) {
                    colon = (s_rem[t] % 250) >= 125;  // 4 Hz, so the owner is obvious at a glance
                    bright = DISP_FULL;
                } else {
                    bright = DISP_DIM;
                }
            } else if (s_state == ST_WON) {
                if (t == s_winner) {
                    bright = ((now / 250) & 1) ? DISP_FULL : 0;
                } else {
                    bright = DISP_DIM;
                }
            }
            display_colon(seg, colon);
        }
        display_set(t, seg, bright);
    }
}

static void render_leds(uint32_t now)
{
    rgb_t frame[LED_COUNT] = {0};
    rgb_t ring[6] = {0};  // clockwise from north

    frame[LED_IDX_TEAM1] = rgb_scale(TEAM_COLOUR[0], CENTRE_LEVEL);
    frame[LED_IDX_TEAM2] = rgb_scale(TEAM_COLOUR[1], CENTRE_LEVEL);

    // Base: owner colour.
    if (s_state == ST_RUNNING && s_owner >= 0) {
        for (int i = 0; i < 6; i++) ring[i] = rgb_scale(TEAM_COLOUR[s_owner], RING_LEVEL);
    } else if (s_state == ST_WON && s_winner >= 0 && ((now / 250) & 1)) {
        for (int i = 0; i < 6; i++) ring[i] = rgb_scale(TEAM_COLOUR[s_winner], RING_LEVEL);
    }

    // Hold progress.
    if (s_face.state) {
        uint32_t held = input_held_ms(&s_face);
        if (s_state == ST_WON && s_hold_stage < 3) {
            int n = (int)(held * 6 / HOLD_RESET_MS);
            for (int i = 0; i < 6; i++) ring[i] = (i < n) ? rgb_scale(RED, 160) : (rgb_t){0};
        } else if (s_hold_stage == 0) {
            rgb_t c = s_hold_target >= 0 ? TEAM_COLOUR[s_hold_target] : WHITE;
            int n = (int)(held * 6 / HOLD_CAPTURE_MS);
            int head = (int)((now / 80) % 6);
            for (int i = 0; i < 6; i++) ring[i] = (i < n) ? rgb_scale(c, 90) : (rgb_t){0};
            ring[head] = rgb_scale(c, 255);
        } else if (s_hold_stage == 1) {
            int n = (int)((held - HOLD_CAPTURE_MS) * 6 / (HOLD_PAUSE_MS - HOLD_CAPTURE_MS));
            for (int i = 0; i < n && i < 6; i++) ring[i] = rgb_scale(WHITE, 70);
        } else if (s_hold_stage == 2) {
            int n = (int)((held - HOLD_PAUSE_MS) * 6 / (HOLD_RESET_MS - HOLD_PAUSE_MS));
            for (int i = 0; i < 6; i++) ring[i] = (i < n) ? rgb_scale(RED, 160) : (rgb_t){0};
        } else {
            for (int i = 0; i < 6; i++) ring[i] = rgb_scale(WHITE, 120);
        }
    }

    // Battery gauge: green fill, 3.3 V empty .. 4.2 V full.
    if (batt_showing(now)) {
        int n = (s_vsys_mv - 3300) * 6 / 900 + 1;
        for (int i = 0; i < 6; i++) ring[i] = (i < n) ? rgb_scale(GREEN, 120) : (rgb_t){0};
    }

    for (int i = 0; i < 6; i++) frame[RING_CW[i]] = ring[i];
    for (int i = 0; i < LED_COUNT_EXT; i++) frame[LED_IDX_EXT + i] = rgb_scale(ring[i * 6 / LED_COUNT_EXT], EXT_LEVEL);

    // Per-switch feedback: white on the two ring LEDs nearest each switch.
    static const int NEAR[3][2] = {{4, 5} /*W: NW,SW*/, {6, 7} /*SE: S,SE*/, {2, 3} /*NE: NE,N*/};
    for (int s = 0; s < 3; s++) {
        if (s_in[IN_FACE1 + s].state) {
            for (int k = 0; k < 2; k++) frame[NEAR[s][k]] = rgb_add(frame[NEAR[s][k]], rgb_scale(WHITE, 90));
        }
    }

    leds_show(frame, LED_COUNT);
}

static void status_line(uint32_t now)
{
    char t1[8], t2[8];
    fmt_clock(t1, sizeof t1, s_rem[0]);
    fmt_clock(t2, sizeof t2, s_rem[1]);
    printf("[%7.1fs] %-7s owner=%d T1=%s T2=%s hold=%4lums sw=%d%d%d boot=%d ped=%d%d "
           "vsys=%dmV vbus=%dmV disp_nak=%lu/%lu heap=%u\n",
           now / 1000.0, STATE_NAME[s_state], s_owner + 1, t1, t2, (unsigned long)input_held_ms(&s_face),
           s_in[IN_FACE1].state, s_in[IN_FACE2].state, s_in[IN_FACE3].state, s_in[IN_BOOT].state,
           s_in[IN_PEDAL1].state, s_in[IN_PEDAL2].state, s_vsys_mv, s_vbus_mv,
           (unsigned long)display_nak_count(0), (unsigned long)display_nak_count(1),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_DEFAULT));
}

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

// ---------------------------------------------------------------- startup
static void power_on(void)
{
    // PWR_EN high: VSYS (LEDs, amp, VSYS sense) and +3V3_SW (displays) on.
    gpio_config_t io = {.pin_bit_mask = (1ULL << PIN_PWR_EN) | (1ULL << PIN_STAT_LED), .mode = GPIO_MODE_OUTPUT};
    gpio_config(&io);
    gpio_set_level(PIN_PWR_EN, 1);
    gpio_set_level(PIN_STAT_LED, 1);
}

static void startup_test(void)
{
    // Displays: all segments + colon. LEDs: red, green, blue across the chain
    // (checks the GRB order and every LED). Fits inside the 1 s status LED on.
    uint8_t all[4] = {0xFF, 0xFF, 0xFF, 0xFF};
    display_set(0, all, DISP_FULL);
    display_set(1, all, DISP_FULL);
    AUDIO_PLAY(SLOT_EVENT, SND_STARTUP);
    static const rgb_t test[3] = {{60, 0, 0}, {0, 60, 0}, {0, 0, 60}};
    rgb_t frame[LED_COUNT];
    for (int c = 0; c < 3; c++) {
        for (int i = 0; i < LED_COUNT; i++) frame[i] = test[c];
        leds_show(frame, LED_COUNT);
        vTaskDelay(pdMS_TO_TICKS(250));
    }
}

void app_main(void)
{
    power_on();
    uint32_t t0 = now_ms();
    vTaskDelay(pdMS_TO_TICKS(30));  // rails settle; GN1650 power-up

    ESP_LOGI(TAG, "capture button firmware, board b43");
    inputs_init();
    sense_init();
    display_init();
    leds_init();
    audio_init();
    startup_test();

    s_vsys_mv = sense_vsys_mv();
    s_vbus_mv = sense_vbus_mv();
    ESP_LOGI(TAG, "VSYS %d mV, VBUS %d mV, display NAKs %lu/%lu", s_vsys_mv, s_vbus_mv,
             (unsigned long)display_nak_count(0), (unsigned long)display_nak_count(1));

    TickType_t wake = xTaskGetTickCount();
    uint32_t last = now_ms();
    uint32_t next_led = 0, next_disp = 0, next_adc = 0, next_status = 0, next_refresh = 0;

    for (;;) {
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(LOOP_MS));
        uint32_t now = now_ms();
        uint32_t dt = now - last;
        last = now;

        // Status LED: 1 s on at start, then a 20 ms blink each second.
        uint32_t up = now - t0;
        gpio_set_level(PIN_STAT_LED, (up < 1000 || (up % 1000) < 20) ? 1 : 0);

        // Inputs.
        bool any_face = false;
        for (int i = 0; i < IN_COUNT; i++) {
            bool raw = gpio_get_level(s_in[i].pin) == 0;
            if (i <= IN_FACE3) any_face |= raw;
            int e = input_update(&s_in[i], raw, now);
            if (e) ESP_LOGI(TAG, "%s %s", s_in[i].name, e > 0 ? "down" : "up");
            if (e > 0 && i <= IN_FACE3) AUDIO_PLAY(SLOT_SW1 + i, SND_SW[i]);
            if (e > 0 && i == IN_BOOT) AUDIO_PLAY(SLOT_DIAG, SND_BOOT);
        }
        int fe = input_update(&s_face, any_face, now);
        if (fe > 0) on_face_press(now);
        if (s_face.state) hold_progress(input_held_ms(&s_face));
        if (fe < 0) on_face_release(now, s_face.last_active_ms - s_face.press_ms);

        game_tick(now, dt);

        if ((int32_t)(now - next_adc) >= 0) {
            next_adc = now + 250;
            s_vsys_mv = sense_vsys_mv();
            s_vbus_mv = sense_vbus_mv();
        }
        if ((int32_t)(now - next_led) >= 0) {
            next_led = now + 20;
            render_leds(now);
        }
        if ((int32_t)(now - next_refresh) >= 0) {
            next_refresh = now + 2000;
            display_force_refresh();
        }
        if ((int32_t)(now - next_disp) >= 0) {
            next_disp = now + 50;
            render_displays(now);
        }
        if ((int32_t)(now - next_status) >= 0) {
            next_status = now + 1000;
            status_line(now);
        }
    }
}
