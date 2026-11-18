// Capture button: team flag / one or two game clocks, controlled by hand or
// over BLE from the web page (docs/index.html).
//
// Modes (settings, stored in NVS, set from the page):
//   teams only   - the ring shows who holds it; no clocks on the displays.
//   one timer    - only one team's clock runs, while that team holds it
//                  (break down the wall). Shown on both displays.
//   two timers   - reverse chess clock: the holder's clock runs.
// Clocks count down (win at 0:00) or up (win at the duration, 0 = no limit).
//
// Hand controls (face press = OR of the three face switches; the lid counts
// as pressed until DEBOUNCE_MS after the last contact). H = hold time
// setting, default 3 s:
//   hold H        capture: running -> other team; paused -> same team again;
//                 uncaptured -> 1 tap first = team 1, 2 taps = team 2,
//                 no taps = default capture setting (random by default)
//   hold H+3 s    pause (undoes the capture)
//   hold H+7 s    reset
//   after a win, a hold of H silences the alarm
//   3 taps        battery
//   BOOT 5 s      restore default settings
//
// USB-only bench test: no Wi-Fi or sleep. BLE is on.

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_timer.h"

#include "audio.h"
#include "ble.h"
#include "board.h"
#include "config.h"
#include "display.h"
#include "leds.h"
#include "sense.h"

static const char *TAG = "button";

// ---------------------------------------------------------------- tuning
#define PAUSE_EXTRA_MS   3000  // pause at hold + this
#define RESET_EXTRA_MS   7000  // reset at hold + this
#define LONG_HOLD_MS     4000  // longer holds tick each second, then the usual 3 s whoop
#define BUILD_MS         3000
#define DEBOUNCE_MS      50    // the lid is assumed still pressed this long after any contact
#define TAP_MAX_MS       500   // a press shorter than this is a tap
#define TAP_GAP_MS       500   // max hand-off time between taps; also the wait after the last one
#define WIN_ALARM_MS     20000 // win sound repeats this long (or until a hold)
#define WIN_REPEAT_MS    2500
#define PAUSED_LEVEL     14    // ring blink while paused with an owner: only visible close up
#define UNCAPTURED_LEVEL 25    // ring pulse peak while uncaptured (~10 %)
#define BATT_SHOW_MS     6000
#define IDENT_MS         3000
#define FACTORY_HOLD_MS  5000  // BOOT held this long restores default settings
#define STATE_PUBLISH_MS 200
#define LOOP_MS          5

#define CENTRE_LEVEL     64    // team-colour centre LEDs, "medium"
#define RING_LEVEL       255   // ring while owned
#define EXT_LEVEL        128   // external strip, relative to the ring
#define DISP_FULL        8
#define DISP_DIM         2
#define DISP_IDLE        6

#define PROTO_VERSION    1

static const rgb_t WHITE = {255, 255, 255};
static const rgb_t RED = {255, 0, 0};
static const rgb_t GREEN = {0, 255, 0};

// Ring positions in clockwise order from north (chain indices).
static const int RING_CW[6] = {3 /*N*/, 2 /*NE*/, 7 /*SE*/, 6 /*S*/, 5 /*SW*/, 4 /*NW*/};

static config_t s_cfg;

// ---------------------------------------------------------------- sounds
// Square waves carry best on the FS-1540 (resonance ~850 Hz); sines sound weak.
static const note_t SND_CLICK[] = {{1, 1, 6, W_NOISE, 255}};
static const note_t SND_SW[3][1] = {
    {{700, 700, 35, W_SQUARE, 180}},
    {{1050, 1050, 35, W_TRI, 255}},
    {{1500, 1500, 35, W_SQUARE, 180}},
};
static note_t s_build[1];  // rising whoop up to the capture; length set per hold
static const note_t SND_TICK[] = {{500, 500, 25, W_SQUARE, 150}};  // each second of a long hold
static const note_t SND_TEAM[2][5] = {
    {   // team 1: rising square arpeggio
        {1047, 1047, 100, W_SQUARE, 255}, {1319, 1319, 100, W_SQUARE, 255}, {1568, 1568, 100, W_SQUARE, 255},
        {2093, 2093, 350, W_SQUARE, 255}, {0, 0, 1, W_SQUARE, 0},
    },
    {   // team 2: square "da-da-DAAA"
        {880, 880, 90, W_SQUARE, 255}, {0, 0, 40, W_SQUARE, 0}, {880, 880, 90, W_SQUARE, 255},
        {0, 0, 40, W_SQUARE, 0}, {1319, 1319, 400, W_SQUARE, 255},
    },
};
// Pause-ward: bright "ding" with a short tail.
static const note_t SND_DING[] = {{1760, 1760, 30, W_SQUARE, 255}, {1760, 1760, 90, W_SQUARE, 110}};
// Reset-ward: low, dissonant "dun-dunn".
static const note_t SND_DOOM[] = {
    {233, 220, 80, W_SQUARE, 255}, {0, 0, 30, W_SQUARE, 0}, {165, 150, 240, W_SQUARE, 255},
};
static const note_t SND_PAUSE[] = {{1200, 300, 450, W_SQUARE, 220}};
static const note_t SND_RESET[] = {  // ominous: slow falling minor steps into a long low growl
    {330, 311, 250, W_SQUARE, 255}, {0, 0, 40, W_SQUARE, 0}, {247, 233, 250, W_SQUARE, 255},
    {0, 0, 40, W_SQUARE, 0}, {175, 110, 1200, W_SQUARE, 255},
};
static const note_t SND_SILENCE[] = {{1200, 1200, 40, W_SQUARE, 200}, {600, 600, 80, W_SQUARE, 200}};
static const note_t SND_WIN[] = {
    {784, 784, 150, W_SQUARE, 255}, {1047, 1047, 150, W_SQUARE, 255}, {1319, 1319, 150, W_SQUARE, 255},
    {1568, 1568, 500, W_SQUARE, 255}, {0, 0, 100, W_SQUARE, 0}, {1319, 1319, 150, W_SQUARE, 255},
    {1568, 1568, 800, W_SQUARE, 255},
};
static const note_t SND_BOOT[] = {  // diagnostic: sine 440 / 880 / 1760, then noise, then sweep
    {440, 440, 200, W_SINE, 255}, {880, 880, 200, W_SINE, 255}, {1760, 1760, 200, W_SINE, 255},
    {1, 1, 150, W_NOISE, 160}, {200, 4000, 600, W_TRI, 200},
};
static const note_t SND_STARTUP[] = {{600, 1800, 150, W_TRI, 200}, {1800, 1800, 60, W_TRI, 200}};
static const note_t SND_START[] = {{880, 880, 80, W_SQUARE, 220}, {0, 0, 40, W_SQUARE, 0}, {1760, 1760, 200, W_SQUARE, 255}};
static const note_t SND_BATT[] = {{1000, 1000, 60, W_SINE, 200}, {1500, 1500, 60, W_SINE, 200}};
static const note_t SND_DENIED[] = {{150, 150, 250, W_SQUARE, 200}};
static const note_t SND_FACTORY[] = {
    {2000, 500, 300, W_SQUARE, 220}, {0, 0, 60, W_SQUARE, 0}, {2000, 500, 300, W_SQUARE, 220},
};
static const note_t SND_HORN[] = {{220, 220, 900, W_SQUARE, 255}, {0, 0, 120, W_SQUARE, 0}, {220, 220, 900, W_SQUARE, 255}};
#define WHISTLE_NOTES 32
static note_t s_whistle[WHISTLE_NOTES];  // pea-whistle trill, built in sounds_init
#define IDENT_NOTES 80  // up to 10 s of beeps
static note_t s_ident[IDENT_NOTES];

// Last-seconds warning blips: more pulses, higher and faster as it gets closer.
// Built once into static buffers (the synth plays from the pointer).
static const int BLIP_AT[] = {60, 30, 20, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1};
#define BLIP_COUNT ((int)(sizeof BLIP_AT / sizeof BLIP_AT[0]))
#define BLIP_MAX_NOTES 9
static note_t s_blip[BLIP_COUNT][BLIP_MAX_NOTES];
static int s_blip_len[BLIP_COUNT];
static int s_blip_last_sec[2];

static void sounds_init(void)
{
    for (int i = 0; i < BLIP_COUNT; i++) {
        int pulses = i == 0 ? 1 : i < 3 ? 2 : i < 8 ? 3 : 4;
        uint16_t f = (uint16_t)(880 + i * 130);  // 880 Hz at 60 s .. ~2440 Hz at 1 s
        uint16_t on = (uint16_t)(90 - i * 5);    // 90 ms .. 30 ms
        uint16_t gap = (uint16_t)(60 - i * 3);   // 60 ms .. 24 ms
        int k = 0;
        for (int p = 0; p < pulses; p++) {
            s_blip[i][k++] = (note_t){f, f, on, W_SQUARE, 255};
            if (p < pulses - 1) s_blip[i][k++] = (note_t){0, 0, gap, W_SQUARE, 0};
        }
        s_blip_len[i] = k;
    }
    for (int i = 0; i < WHISTLE_NOTES; i++) {
        uint16_t f = (i & 1) ? 2100 : 1850;
        s_whistle[i] = (note_t){f, f, 28, W_SQUARE, 255};
    }
    for (int i = 0; i < IDENT_NOTES; i++) {
        s_ident[i] = (i & 1) ? (note_t){0, 0, 150, W_SQUARE, 0} : (note_t){1400, 1400, 100, W_SQUARE, 255};
    }
}

// Sounds the page can play (ids are part of the BLE protocol).
static void play_sound_id(int id)
{
    switch (id) {
    case 0: AUDIO_PLAY(SLOT_EVENT, SND_TEAM[0]); break;
    case 1: AUDIO_PLAY(SLOT_EVENT, SND_TEAM[1]); break;
    case 2: AUDIO_PLAY(SLOT_EVENT, SND_WIN); break;
    case 3: AUDIO_PLAY(SLOT_EVENT, SND_PAUSE); break;
    case 4: AUDIO_PLAY(SLOT_EVENT, SND_RESET); break;
    case 5: AUDIO_PLAY(SLOT_EVENT, SND_DING); break;
    case 6: AUDIO_PLAY(SLOT_EVENT, SND_DOOM); break;
    case 7: audio_play(SLOT_EVENT, s_blip[BLIP_COUNT - 1], s_blip_len[BLIP_COUNT - 1]); break;
    case 8: AUDIO_PLAY(SLOT_EVENT, s_whistle); break;
    case 9: AUDIO_PLAY(SLOT_EVENT, SND_HORN); break;
    case 10: AUDIO_PLAY(SLOT_EVENT, SND_BATT); break;
    case 11: AUDIO_PLAY(SLOT_EVENT, SND_STARTUP); break;
    case 12: AUDIO_PLAY(SLOT_EVENT, SND_BOOT); break;
    case 13: if (audio_pcm_available()) audio_pcm_start(); break;
    default: break;
    }
}

// ---------------------------------------------------------------- inputs
typedef struct {
    gpio_num_t pin;
    const char *name;
    uint32_t release_ms;      // open this long before a release counts
    bool state;               // debounced pressed
    uint32_t press_ms;        // when the current press started
    uint32_t last_active_ms;  // last time the raw contact was seen closed
} input_t;

enum { IN_FACE1, IN_FACE2, IN_FACE3, IN_BOOT, IN_PEDAL1, IN_PEDAL2, IN_COUNT };
static input_t s_in[IN_COUNT] = {
    {PIN_FACE1, "face1(W)", DEBOUNCE_MS},  {PIN_FACE2, "face2(SE)", DEBOUNCE_MS},
    {PIN_FACE3, "face3(NE)", DEBOUNCE_MS}, {PIN_BOOT, "boot", DEBOUNCE_MS},
    {PIN_PEDAL1, "pedal1", DEBOUNCE_MS},   {PIN_PEDAL2, "pedal2", DEBOUNCE_MS},
};
// The lid: OR of the three face switches. Used for both holds and taps.
static input_t s_face = {GPIO_NUM_NC, "face", DEBOUNCE_MS};

// Press registers on the first closed sample; release only after release_ms
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
    } else if (in->state && now - in->last_active_ms >= in->release_ms) {
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
static bool s_running;     // clocks run (for whoever holds it)
static int s_owner = -1;   // -1 uncaptured, 0 team 1, 1 team 2
static int s_winner = -1;  // >= 0: game over
static int32_t s_val[2];   // clock values as shown (ms)
static bool s_fresh = true;  // nothing has happened since the last reset
static bool s_locked;      // page lock-out: holds do nothing (taps still show battery)

static struct {
    bool running;
    int owner;
    int32_t val[2];
} s_snap;                  // state when the current hold started

enum { HS_BUILD, HS_CAPTURED, HS_PAUSED, HS_RESET, HS_LOCKED };
static int s_hold_stage;
static int s_hold_target;  // team the capture will go to, -1 = random
static int s_hold_last_sec;
static bool s_build_started;

static int s_taps;
static uint32_t s_last_tap_ms;
static uint32_t s_batt_until, s_ident_until;
static uint32_t s_win_alarm_until, s_win_next_sound;
static uint32_t s_boot_factory_done;

static int s_vsys_mv, s_vbus_mv;

static rgb_t team_colour(int t) { return (rgb_t){s_cfg.colour[t][0], s_cfg.colour[t][1], s_cfg.colour[t][2]}; }
static uint32_t hold_capture_ms(void) { return s_cfg.hold_ms; }
static uint32_t hold_pause_ms(void) { return s_cfg.hold_ms + PAUSE_EXTRA_MS; }
static uint32_t hold_reset_ms(void) { return s_cfg.hold_ms + RESET_EXTRA_MS; }
static bool has_clock(int t)
{
    return s_cfg.mode == MODE_TWO_TIMERS || (s_cfg.mode == MODE_ONE_TIMER && t == s_cfg.timer_team);
}
// ms until team t wins on its clock, or -1 if it can't.
static int32_t to_win_ms(int t)
{
    if (!has_clock(t)) return -1;
    if (!s_cfg.count_up) return s_val[t];
    return s_cfg.duration_ms[t] ? (int32_t)s_cfg.duration_ms[t] - s_val[t] : -1;
}

static const char *state_name(void)
{
    if (s_winner >= 0) return "won";
    if (s_running) return "running";
    return s_owner >= 0 ? "paused" : "idle";
}

// Who the next capture goes to, given the taps just before the hold.
static int capture_target(int taps)
{
    if (s_owner >= 0) return s_running ? 1 - s_owner : s_owner;
    if (taps == 1) return 0;
    if (taps == 2) return 1;
    return (int)s_cfg.default_capture - 1;  // -1 = random
}

// Rough Li-ion state of charge from voltage: 3.3 V = 0 %, 4.2 V = 100 %, with
// the steep middle (most of the energy) around 3.6..3.8 V. Extrapolates past
// both ends, so it can read below 0 % or above 100 %. Under load, or on USB
// (VSYS = charger output), this is only an estimate.
static int batt_percent(int mv)
{
    static const int16_t T[][2] = {
        {3300, 0}, {3400, 4}, {3500, 9}, {3600, 18}, {3650, 25}, {3700, 35},
        {3750, 45}, {3800, 55}, {3900, 68}, {4000, 80}, {4100, 90}, {4200, 100},
    };
    const int n = sizeof T / sizeof T[0];
    int i = 0;
    while (i < n - 2 && mv > T[i + 1][0]) i++;
    return T[i][1] + (mv - T[i][0]) * (T[i + 1][1] - T[i][1]) / (T[i + 1][0] - T[i][0]);
}

static void game_reset(uint32_t now)
{
    for (int t = 0; t < 2; t++) s_val[t] = s_cfg.count_up ? 0 : (int32_t)s_cfg.duration_ms[t];
    s_winner = -1;
    s_win_alarm_until = now;
    s_owner = (int)s_cfg.initial_owner - 1;
    // Teams only: an initial owner is simply live. With clocks, it waits
    // paused for a start (page or a hold).
    s_running = s_owner >= 0 && s_cfg.mode == MODE_TEAMS;
    s_fresh = true;
    for (int t = 0; t < 2; t++) s_blip_last_sec[t] = -2;
    ESP_LOGI(TAG, "RESET (mode %d, owner %d)", s_cfg.mode, s_owner + 1);
}

static void game_win(int t, uint32_t now)
{
    s_winner = t;
    s_running = false;
    s_win_alarm_until = now + WIN_ALARM_MS;
    s_win_next_sound = now;
    ESP_LOGI(TAG, "TEAM %d WINS", t + 1);
}

// After an admin changes a clock: un-win if the winner is no longer at the line.
static void clock_changed(void)
{
    s_fresh = false;
    for (int t = 0; t < 2; t++) {
        if (s_val[t] < 0) s_val[t] = 0;
        if (s_val[t] > 24 * 3600 * 1000) s_val[t] = 24 * 3600 * 1000;
    }
    if (s_winner >= 0 && to_win_ms(s_winner) > 0) {
        s_winner = -1;
        audio_stop(SLOT_EVENT);
        ESP_LOGI(TAG, "win undone by a clock change (paused)");
    }
}

static void on_face_press(uint32_t now)
{
    s_snap.running = s_running;
    s_snap.owner = s_owner;
    s_snap.val[0] = s_val[0];
    s_snap.val[1] = s_val[1];
    s_hold_stage = HS_BUILD;
    s_hold_last_sec = 0;
    s_build_started = false;

    if (s_taps > 0 && now - s_last_tap_ms > TAP_GAP_MS) s_taps = 0;
    s_hold_target = capture_target(s_taps);

    audio_pcm_stop();
    AUDIO_PLAY(SLOT_CLICK, SND_CLICK);
    if (s_locked) {
        s_hold_stage = HS_LOCKED;
        AUDIO_PLAY(SLOT_BUILD, SND_DENIED);
    }
    ESP_LOGI(TAG, "face press (%s, owner %d, taps %d, target %d)", state_name(), s_owner + 1, s_taps,
             s_hold_target + 1);
}

// held = first to last contact; the gap to the next tap is measured from the
// last contact too, so the debounce doesn't eat into the hand-off time.
static void on_face_release(uint32_t held)
{
    if (s_hold_stage == HS_BUILD) audio_stop(SLOT_BUILD);
    if (held < TAP_MAX_MS) {
        s_taps++;
        s_last_tap_ms = s_face.last_active_ms;
    } else {
        s_taps = 0;
    }
    ESP_LOGI(TAG, "face release after %lu ms (taps %d)", (unsigned long)held, s_taps);
}

static void hold_progress(uint32_t now, uint32_t held)
{
    if (s_hold_stage == HS_LOCKED) return;
    const uint32_t h1 = hold_capture_ms(), h2 = hold_pause_ms(), h3 = hold_reset_ms();
    const bool won = s_winner >= 0;

    // Rising whoop into the capture: the whole hold for short holds, the last
    // 3 s of a long one (ticks each second before that).
    if (s_hold_stage == HS_BUILD && !won && !s_build_started && (h1 <= LONG_HOLD_MS || held + BUILD_MS >= h1)) {
        s_build_started = true;
        uint32_t ms = h1 > held ? h1 - held : 1;
        s_build[0] = (note_t){250, 1200, (uint16_t)ms, W_SQUARE, 140};
        AUDIO_PLAY(SLOT_BUILD, s_build);
    }

    if (s_hold_stage == HS_BUILD && held >= h1) {
        s_hold_stage = HS_CAPTURED;
        audio_stop(SLOT_BUILD);
        if (won) {
            if ((int32_t)(s_win_alarm_until - now) > 0) {
                s_win_alarm_until = now;
                audio_stop(SLOT_EVENT);
                AUDIO_PLAY(SLOT_CLICK, SND_SILENCE);
                ESP_LOGI(TAG, "win alarm silenced");
            } else {
                AUDIO_PLAY(SLOT_CLICK, SND_DING);  // keep the once-a-second rhythm
            }
        } else {
            int team = s_hold_target >= 0 ? s_hold_target : (int)(esp_random() & 1);
            s_owner = team;
            s_running = true;
            s_fresh = false;
            AUDIO_PLAY(SLOT_EVENT, SND_TEAM[team]);
            ESP_LOGI(TAG, "CAPTURED by team %d", team + 1);
        }
    }
    if (s_hold_stage == HS_CAPTURED && held >= h2) {
        s_hold_stage = HS_PAUSED;
        if (!won) {
            // Undo this hold entirely, then pause. The owner stays, and the
            // next hold resumes with them.
            s_val[0] = s_snap.val[0];
            s_val[1] = s_snap.val[1];
            s_owner = s_snap.owner;
            s_running = false;
            ESP_LOGI(TAG, "PAUSED");
        }
        AUDIO_PLAY(SLOT_EVENT, SND_PAUSE);
    }
    if (s_hold_stage == HS_PAUSED && held >= h3) {
        s_hold_stage = HS_RESET;
        game_reset(now);
        AUDIO_PLAY(SLOT_EVENT, SND_RESET);
    }
    // A sound every second: ticks before a long hold's whoop (or all the way
    // after a win, where there is no whoop), dings towards pause, doom towards reset.
    int sec = (int)(held / 1000);
    if (sec != s_hold_last_sec && (uint32_t)sec * 1000 != h1 && (uint32_t)sec * 1000 != h2) {
        if (s_hold_stage == HS_BUILD && won) AUDIO_PLAY(SLOT_CLICK, SND_DING);
        else if (s_hold_stage == HS_BUILD && held + BUILD_MS < h1) AUDIO_PLAY(SLOT_CLICK, SND_TICK);
        if (s_hold_stage == HS_CAPTURED) AUDIO_PLAY(SLOT_CLICK, SND_DING);
        if (s_hold_stage == HS_PAUSED) AUDIO_PLAY(SLOT_CLICK, SND_DOOM);
    }
    s_hold_last_sec = sec;
}

static void game_tick(uint32_t now, uint32_t dt)
{
    if (s_running && s_owner >= 0 && s_winner < 0 && has_clock(s_owner)) {
        int t = s_owner;
        if (!s_cfg.count_up) {
            s_val[t] -= (int32_t)dt;
            if (s_val[t] <= 0) {
                s_val[t] = 0;
                game_win(t, now);
            }
        } else {
            s_val[t] += (int32_t)dt;
            if (s_cfg.duration_ms[t] && s_val[t] >= (int32_t)s_cfg.duration_ms[t]) {
                s_val[t] = (int32_t)s_cfg.duration_ms[t];
                game_win(t, now);
            }
        }
    }

    // Win alarm repeats until it times out or a hold silences it.
    if (s_winner >= 0 && (int32_t)(s_win_alarm_until - now) > 0 && (int32_t)(now - s_win_next_sound) >= 0) {
        s_win_next_sound = now + WIN_REPEAT_MS;
        AUDIO_PLAY(SLOT_EVENT, SND_WIN);
    }

    if (!s_face.state && s_taps > 0 && now - s_last_tap_ms > TAP_GAP_MS) {
        if (s_taps == 3) {
            s_batt_until = now + BATT_SHOW_MS;
            AUDIO_PLAY(SLOT_EVENT, SND_BATT);
            ESP_LOGI(TAG, "triple tap: VSYS %d mV (~%d %%), VBUS %d mV", s_vsys_mv, batt_percent(s_vsys_mv),
                     s_vbus_mv);
        }
        s_taps = 0;
    }
}

static void countdown_blips(void)
{
    for (int t = 0; t < 2; t++) {
        int32_t w = to_win_ms(t);
        int sec = w < 0 ? -1 : (w + 999) / 1000;
        if (s_running && t == s_owner && s_winner < 0 && s_cfg.show_numbers && sec != s_blip_last_sec[t]) {
            for (int i = 0; i < BLIP_COUNT; i++) {
                if (BLIP_AT[i] == sec) audio_play(SLOT_DIAG, s_blip[i], s_blip_len[i]);
            }
        }
        s_blip_last_sec[t] = sec;
    }
}

// ---------------------------------------------------------------- BLE
// Command opcodes (first byte of a write to the command characteristic).
enum {
    OP_START = 1,
    OP_PAUSE = 2,
    OP_RESET = 3,
    OP_IDENTIFY = 4,     // [u16 ms] (0 = default)
    OP_SET_TIME = 5,     // u8 team, i32 ms (value as shown)
    OP_ADD_TIME = 6,     // u8 team, i32 ms (+/- on the value as shown)
    OP_SET_OWNER = 7,    // i8 owner (-1 uncaptured, 0, 1)
    OP_PLAY_SOUND = 8,   // u8 id
    OP_STOP_SOUND = 9,
    OP_LOCK = 10,        // u8 0/1
    OP_SHOW_BATTERY = 11,
    OP_FACTORY_RESET = 12,
};

typedef struct __attribute__((packed)) {
    uint8_t version;      //  0 PROTO_VERSION
    uint8_t flags;        //  1 bit0 running, 1 won, 2 locked, 3 face held, 4 identifying, 5 usb, 6 clip playing
    int8_t owner;         //  2 -1 uncaptured, 0, 1
    int8_t winner;        //  3 -1 none
    int8_t target;        //  4 next capture with no taps (-1 random); while held: the hold's target
    uint8_t hold_stage;   //  5 HS_* while held
    uint16_t held_ms;     //  6
    int32_t time_ms[2];   //  8, 12 clock values as shown
    uint16_t vsys_mv;     // 16
    uint16_t vbus_mv;     // 18
    int16_t batt_pct;     // 20
    uint8_t connections;  // 22
    uint8_t reserved;     // 23
    uint32_t uptime_s;    // 24
} state_pkt_t;            // 28 bytes
_Static_assert(sizeof(state_pkt_t) == 28, "state_pkt_t layout is part of the BLE protocol");

static void default_name(char *buf, size_t n)
{
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_BT);
    snprintf(buf, n, "Button-%02X%02X", mac[4], mac[5]);
}

static void apply_name(void)
{
    char name[24];
    if (s_cfg.name[0]) {
        snprintf(name, sizeof name, "%s", s_cfg.name);
    } else {
        default_name(name, sizeof name);
    }
    ble_set_name(name);
}

static void publish_state(uint32_t now)
{
    bool held = s_face.state;
    uint32_t hm = input_held_ms(&s_face);
    state_pkt_t p = {
        .version = PROTO_VERSION,
        .flags = (uint8_t)((s_running ? 1 : 0) | (s_winner >= 0 ? 2 : 0) | (s_locked ? 4 : 0) | (held ? 8 : 0) |
                           ((int32_t)(s_ident_until - now) > 0 ? 16 : 0) | (s_vbus_mv > 4000 ? 32 : 0) |
                           (audio_pcm_playing() ? 64 : 0)),
        .owner = (int8_t)s_owner,
        .winner = (int8_t)s_winner,
        .target = (int8_t)(held ? s_hold_target : capture_target(0)),
        .hold_stage = (uint8_t)(held ? s_hold_stage : 0),
        .held_ms = (uint16_t)(hm > 65535 ? 65535 : hm),
        .time_ms = {s_val[0], s_val[1]},
        .vsys_mv = (uint16_t)s_vsys_mv,
        .vbus_mv = (uint16_t)s_vbus_mv,
        .batt_pct = (int16_t)batt_percent(s_vsys_mv),
        .connections = (uint8_t)ble_connections(),
        .uptime_s = now / 1000,
    };
    ble_publish_state(&p, sizeof p);
}

static void apply_config(const config_t *in, uint32_t now)
{
    config_t old = s_cfg;
    s_cfg = *in;
    config_sanitise(&s_cfg);
    config_save(&s_cfg);
    bool structural = old.mode != s_cfg.mode || old.timer_team != s_cfg.timer_team || old.count_up != s_cfg.count_up;
    // Before anything has happened, new durations / initial owner apply at
    // once; mid-game they wait for the next reset (or use set time).
    if (structural || s_fresh) game_reset(now);
    if (strcmp(old.name, s_cfg.name) != 0) apply_name();
    ble_publish_config(&s_cfg, sizeof s_cfg);
    ESP_LOGI(TAG, "settings: mode %d, hold %u ms, %s, numbers %s, durations %lu/%lu s", s_cfg.mode, s_cfg.hold_ms,
             s_cfg.count_up ? "up" : "down", s_cfg.show_numbers ? "shown" : "hidden",
             (unsigned long)(s_cfg.duration_ms[0] / 1000), (unsigned long)(s_cfg.duration_ms[1] / 1000));
}

static void factory_reset(uint32_t now)
{
    config_t d;
    config_defaults(&d);
    s_locked = false;
    apply_config(&d, now);
    game_reset(now);
    AUDIO_PLAY(SLOT_EVENT, SND_FACTORY);
    ESP_LOGI(TAG, "default settings restored");
}

static int32_t rd_i32(const uint8_t *p) { return (int32_t)(p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24); }

static void handle_command(const uint8_t *d, int n, uint32_t now)
{
    if (n < 1) return;
    switch (d[0]) {
    case OP_START:
        if (s_winner < 0 && !s_running) {
            s_running = true;
            s_fresh = false;
            AUDIO_PLAY(SLOT_EVENT, SND_START);
        }
        break;
    case OP_PAUSE:
        if (s_running) {
            s_running = false;
            AUDIO_PLAY(SLOT_EVENT, SND_PAUSE);
        }
        break;
    case OP_RESET:
        game_reset(now);
        AUDIO_PLAY(SLOT_EVENT, SND_RESET);
        break;
    case OP_IDENTIFY: {
        uint32_t ms = n >= 3 ? (uint32_t)(d[1] | d[2] << 8) : 0;
        if (!ms) ms = IDENT_MS;
        if (ms > 10000) ms = 10000;
        s_ident_until = now + ms;
        int notes = (int)(ms / 250) * 2;
        if (notes > IDENT_NOTES) notes = IDENT_NOTES;
        audio_play(SLOT_EVENT, s_ident, notes);
        break;
    }
    case OP_SET_TIME:
    case OP_ADD_TIME:
        if (n >= 6 && d[1] < 2) {
            int32_t v = rd_i32(d + 2);
            s_val[d[1]] = d[0] == OP_SET_TIME ? v : s_val[d[1]] + v;
            clock_changed();
        }
        break;
    case OP_SET_OWNER:
        if (n >= 2) {
            int o = (int8_t)d[1];
            if (o < -1 || o > 1) break;
            s_owner = o;
            s_fresh = false;
            if (o >= 0 && s_cfg.mode == MODE_TEAMS && s_winner < 0) s_running = true;
            if (o >= 0) AUDIO_PLAY(SLOT_EVENT, SND_TEAM[o]);
            ESP_LOGI(TAG, "owner set to %d from the page", o + 1);
        }
        break;
    case OP_PLAY_SOUND:
        if (n >= 2) play_sound_id(d[1]);
        break;
    case OP_STOP_SOUND:
        audio_stop(SLOT_EVENT);
        audio_pcm_stop();
        break;
    case OP_LOCK:
        if (n >= 2) s_locked = d[1] != 0;
        ESP_LOGI(TAG, "button %s", s_locked ? "locked" : "unlocked");
        break;
    case OP_SHOW_BATTERY:
        s_batt_until = now + BATT_SHOW_MS;
        AUDIO_PLAY(SLOT_EVENT, SND_BATT);
        break;
    case OP_FACTORY_RESET:
        factory_reset(now);
        break;
    default:
        ESP_LOGW(TAG, "unknown command %d", d[0]);
    }
}

static void ble_service(uint32_t now)
{
    ble_msg_t m;
    while (ble_poll(&m)) {
        if (m.kind == BLE_MSG_CONFIG) {
            if (m.len == sizeof(config_t) && m.data[0] == CONFIG_VERSION) {
                config_t c;
                memcpy(&c, m.data, sizeof c);
                apply_config(&c, now);
            } else {
                ESP_LOGW(TAG, "config write ignored (%d bytes, version %d)", m.len, m.data[0]);
            }
        } else {
            handle_command(m.data, m.len, now);
        }
    }
}

// ---------------------------------------------------------------- rendering
static bool batt_showing(uint32_t now) { return (int32_t)(s_batt_until - now) > 0; }
static bool identifying(uint32_t now) { return (int32_t)(s_ident_until - now) > 0; }

static void render_clock(uint8_t seg[4], int32_t ms, bool round_up)
{
    int s = round_up ? (ms + 999) / 1000 : ms / 1000;
    if (s > 99 * 60 + 59) s = 99 * 60 + 59;
    int m = s / 60;
    s %= 60;
    seg[0] = m >= 10 ? display_digit(m / 10) : 0;
    seg[1] = display_digit(m % 10);
    seg[2] = display_digit(s / 10);
    seg[3] = display_digit(s % 10);
}

static void render_displays(uint32_t now)
{
    for (int t = 0; t < 2; t++) {
        uint8_t seg[4] = {0};
        int bright = 0;
        if (identifying(now)) {
            memset(seg, 0xFF, sizeof seg);
            bright = ((now / 125) & 1) ? DISP_FULL : 0;
        } else if (batt_showing(now)) {
            if (t == 0) {
                // "bAtt", with each t drawn as a mirrored 7 (segments A, F, E).
                seg[0] = display_char('b');
                seg[1] = display_char('A');
                seg[2] = seg[3] = 0x31;
            } else {
                int pct = batt_percent(s_vsys_mv);
                if (pct < -999) pct = -999;
                if (pct > 9999) pct = 9999;
                display_number(seg, pct < 0 ? -pct : pct);
                if (pct < 0) {  // minus sign just left of the digits
                    int first = 0;
                    while (first < 3 && seg[first] == 0) first++;
                    if (first > 0) seg[first - 1] = display_char('-');
                }
            }
            bright = DISP_FULL;
        } else if (s_cfg.mode != MODE_TEAMS && s_cfg.show_numbers) {
            int src = s_cfg.mode == MODE_ONE_TIMER ? s_cfg.timer_team : t;
            render_clock(seg, s_val[src], !s_cfg.count_up);
            bool colon = true;
            if (s_winner >= 0) {
                bright = src == s_winner ? (((now / 250) & 1) ? DISP_FULL : 0) : DISP_DIM;
            } else if (s_running && s_owner >= 0) {
                if (src == s_owner) {
                    colon = (now % 250) >= 125;  // 4 Hz, so the owner is obvious at a glance
                    bright = DISP_FULL;
                } else {
                    bright = DISP_DIM;
                }
            } else {
                // Not running: the display of whoever the next capture goes to is lit.
                int target = capture_target(0);
                bright = target < 0 ? DISP_IDLE : target == src ? DISP_FULL : DISP_DIM;
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

    frame[LED_IDX_TEAM1] = rgb_scale(team_colour(0), CENTRE_LEVEL);
    frame[LED_IDX_TEAM2] = rgb_scale(team_colour(1), CENTRE_LEVEL);

    // Base: owner colour.
    if (s_winner >= 0) {
        if ((now / 250) & 1)
            for (int i = 0; i < 6; i++) ring[i] = rgb_scale(team_colour(s_winner), RING_LEVEL);
    } else if (s_owner >= 0 && s_running) {
        for (int i = 0; i < 6; i++) ring[i] = rgb_scale(team_colour(s_owner), RING_LEVEL);
    } else if (s_owner >= 0) {
        // Paused with an owner: faint slow blink, only noticeable up close.
        if ((now / 500) & 1)
            for (int i = 0; i < 6; i++) ring[i] = rgb_scale(team_colour(s_owner), PAUSED_LEVEL);
    } else {
        // Uncaptured: slow (0.5 Hz) faint pulse in the colour the next capture
        // gives; random alternates the two colours each pulse.
        int target = capture_target(0);
        int c = target >= 0 ? target : (int)((now / 2000) & 1);
        float ph = (float)(now % 2000) / 2000.0f;
        int level = (int)(UNCAPTURED_LEVEL * 0.5f * (1.0f - cosf(2.0f * (float)M_PI * ph)) + 0.5f);
        for (int i = 0; i < 6; i++) ring[i] = rgb_scale(team_colour(c), level);
    }

    // Hold progress.
    if (s_face.state) {
        uint32_t held = input_held_ms(&s_face);
        uint32_t h1 = hold_capture_ms(), h2 = hold_pause_ms(), h3 = hold_reset_ms();
        if (s_hold_stage == HS_LOCKED) {
            for (int i = 0; i < 6; i++) ring[i] = rgb_scale(RED, 40);
        } else if (s_winner >= 0 && s_hold_stage < HS_RESET) {
            int n = (int)(held * 6 / h3);
            for (int i = 0; i < 6; i++) ring[i] = (i < n) ? rgb_scale(RED, 160) : (rgb_t){0};
        } else if (s_hold_stage == HS_BUILD) {
            rgb_t c = s_hold_target >= 0 ? team_colour(s_hold_target) : WHITE;
            int n = (int)(held * 6 / h1);
            int head = (int)((now / 80) % 6);
            for (int i = 0; i < 6; i++) ring[i] = (i < n) ? rgb_scale(c, 90) : (rgb_t){0};
            ring[head] = rgb_scale(c, 255);
        } else if (s_hold_stage == HS_CAPTURED) {
            int n = (int)((held - h1) * 6 / (h2 - h1));
            for (int i = 0; i < n && i < 6; i++) ring[i] = rgb_scale(WHITE, 70);
        } else if (s_hold_stage == HS_PAUSED) {
            int n = (int)((held - h2) * 6 / (h3 - h2));
            for (int i = 0; i < 6; i++) ring[i] = (i < n) ? rgb_scale(RED, 160) : (rgb_t){0};
        } else {
            for (int i = 0; i < 6; i++) ring[i] = rgb_scale(WHITE, 120);
        }
    }

    // Battery gauge: green fill by estimated percent (at least one LED).
    if (batt_showing(now)) {
        int n = (batt_percent(s_vsys_mv) * 6 + 50) / 100;
        if (n < 1) n = 1;
        for (int i = 0; i < 6; i++) ring[i] = (i < n) ? rgb_scale(GREEN, 120) : (rgb_t){0};
    }
    if (identifying(now)) {
        for (int i = 0; i < 6; i++) ring[i] = ((now / 125) & 1) ? WHITE : (rgb_t){0};
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

static void fmt_clock(char *buf, size_t n, int32_t ms)
{
    int s = (ms + 999) / 1000;
    snprintf(buf, n, "%02d:%02d", s / 60, s % 60);
}

static void status_line(uint32_t now)
{
    char t1[8], t2[8];
    fmt_clock(t1, sizeof t1, s_val[0]);
    fmt_clock(t2, sizeof t2, s_val[1]);
    printf("[%7.1fs] %-7s mode=%d owner=%d T1=%s T2=%s hold=%4lums sw=%d%d%d boot=%d ped=%d%d "
           "vsys=%dmV vbus=%dmV ble=%d%s disp_nak=%lu/%lu heap=%u\n",
           now / 1000.0, state_name(), s_cfg.mode, s_owner + 1, t1, t2, (unsigned long)input_held_ms(&s_face),
           s_in[IN_FACE1].state, s_in[IN_FACE2].state, s_in[IN_FACE3].state, s_in[IN_BOOT].state,
           s_in[IN_PEDAL1].state, s_in[IN_PEDAL2].state, s_vsys_mv, s_vbus_mv, ble_connections(),
           s_locked ? " LOCKED" : "", (unsigned long)display_nak_count(0), (unsigned long)display_nak_count(1),
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
    gpio_set_level(PIN_STAT_LED, 0);  // kept off: it's distracting through the case
}

static void startup_test(void)
{
    // Displays: all segments + colon. LEDs: red, green, blue across the chain
    // (checks the GRB order and every LED).
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
    vTaskDelay(pdMS_TO_TICKS(30));  // rails settle; GN1650 power-up

    ESP_LOGI(TAG, "capture button firmware, board b43");
    config_load(&s_cfg);
    inputs_init();
    sounds_init();
    sense_init();
    display_init();
    leds_init();
    audio_init();
    startup_test();

    char name[24];
    if (s_cfg.name[0]) {
        snprintf(name, sizeof name, "%s", s_cfg.name);
    } else {
        default_name(name, sizeof name);
    }
    ble_init(name);
    ble_publish_config(&s_cfg, sizeof s_cfg);

    s_vsys_mv = sense_vsys_mv();
    s_vbus_mv = sense_vbus_mv();
    ESP_LOGI(TAG, "VSYS %d mV, VBUS %d mV, display NAKs %lu/%lu", s_vsys_mv, s_vbus_mv,
             (unsigned long)display_nak_count(0), (unsigned long)display_nak_count(1));

    uint32_t last = now_ms();
    game_reset(last);

    // Speaker test: play the flash clip once at power-up, if one is flashed.
    if (audio_pcm_available()) audio_pcm_start();

    TickType_t wake = xTaskGetTickCount();
    uint32_t next_led = 0, next_disp = 0, next_adc = 0, next_status = 0, next_refresh = 0, next_ble = 0;

    for (;;) {
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(LOOP_MS));
        uint32_t now = now_ms();
        uint32_t dt = now - last;
        last = now;

        // Inputs.
        bool any_face = false;
        for (int i = 0; i < IN_COUNT; i++) {
            bool raw = gpio_get_level(s_in[i].pin) == 0;
            if (i <= IN_FACE3) any_face |= raw;
            int e = input_update(&s_in[i], raw, now);
            if (e) ESP_LOGI(TAG, "%s %s", s_in[i].name, e > 0 ? "down" : "up");
            if (e > 0 && i <= IN_FACE3) AUDIO_PLAY(SLOT_SW1 + i, SND_SW[i]);
            if (e > 0 && i == IN_BOOT) {
                if (audio_pcm_playing()) {
                    audio_pcm_stop();
                } else {
                    AUDIO_PLAY(SLOT_DIAG, SND_BOOT);
                }
            }
        }
        // BOOT held: restore default settings (once per press).
        if (s_in[IN_BOOT].state && input_held_ms(&s_in[IN_BOOT]) >= FACTORY_HOLD_MS &&
            s_boot_factory_done != s_in[IN_BOOT].press_ms) {
            s_boot_factory_done = s_in[IN_BOOT].press_ms;
            factory_reset(now);
        }
        int fe = input_update(&s_face, any_face, now);
        if (fe > 0) on_face_press(now);
        if (s_face.state) hold_progress(now, input_held_ms(&s_face));
        if (fe < 0) on_face_release(s_face.last_active_ms - s_face.press_ms);

        ble_service(now);
        game_tick(now, dt);
        countdown_blips();

        if ((int32_t)(now - next_adc) >= 0) {
            next_adc = now + 250;
            s_vsys_mv = sense_vsys_mv();
            s_vbus_mv = sense_vbus_mv();
        }
        if ((int32_t)(now - next_ble) >= 0) {
            next_ble = now + STATE_PUBLISH_MS;
            publish_state(now);
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
