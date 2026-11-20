// Capture button: team flag / one or two game clocks, controlled by hand or
// over BLE from the web page (docs/index.html).
//
// Modes (settings, stored in NVS, set from the page):
//   teams only   - the ring shows who owns it; no clocks, dashes on the owner's display.
//   one timer    - one shared clock that runs while either team owns it; whoever
//                  owns it when it runs out wins. Shown only on the owner's display.
//   two timers   - reverse chess clock: the owner's clock runs.
// Clocks count down (win at 0:00) or up (win at the duration, 0 = no limit).
// "Own" is the team in control; "hold" is someone pressing the lid.
//
// Hand controls (face press = OR of the three face switches; the lid counts
// as pressed until DEBOUNCE_MS after the last contact). H = hold time
// setting, default 3 s:
//   hold H        capture: running -> other team; paused -> same team again;
//                 uncaptured -> the team shown from the first moment of the
//                 hold; each press that doesn't capture flips it for the next
//                 press (first one: the first-capture setting, random by default)
//   hold H+4 s    pause (undoes the capture)
//   hold H+9 s    reset
//   hold H+14 s   power off ("HoLd" "oFF" and a falling tone from H+10 s)
//   after a win, a hold of H silences the alarm
//   3 taps        battery
//   BOOT 3 s      power off ("HoLd" "oFF", falling tone; no need to let go)
//   when off      a press peeks (battery shown); 2 more presses, or a hold of
//                 1.5 s started during the peek: on
//   BOOT 8 s      restore default settings
//
// No Wi-Fi. BLE is on, also while off (slow adverts). Light sleep when idle.

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/rtc_io.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_pm.h"
#include "esp_random.h"
#include "esp_rom_crc.h"
#include "esp_rtc_time.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_timer.h"

#include "audio.h"
#include "ble.h"
#include "board.h"
#include "mesh.h"
#include "config.h"
#include "display.h"
#include "leds.h"
#include "sense.h"

static const char *TAG = "button";

// ---------------------------------------------------------------- tuning
#define PAUSE_EXTRA_MS   4000  // pause at hold + this
#define RESET_EXTRA_MS   9000  // reset at hold + this
#define OFF_EXTRA_MS     14000 // power off at hold + this (lid held on past the reset)
#define OFF_WARN_DELAY   1000  // "HoLd" "oFF" and the falling tone start this long after the reset
#define DEBOUNCE_MS      50    // the lid is assumed still pressed this long after any contact
#define TAP_MAX_MS       500   // a press shorter than this is a tap
#define TAP_GAP_MS       500   // max hand-off time between taps; also the wait after the last one
#define WIN_ALARM_MS     20000 // win sound repeats this long (or until a hold)
#define WIN_REPEAT_MS    2500
#define PAUSED_LEVEL     14    // ring blink while paused with an owner: only visible close up
#define UNCAPTURED_LEVEL 25    // ring pulse peak while uncaptured (~10 %)
#define BATT_SHOW_MS     6000
#define IDENT_MS         3000
#define BOOT_OFF_MS      3000  // BOOT held this long powers off
#define BOOT_FACTORY_MS  8000  // ... and this long restores default settings
#define STATE_PUBLISH_MS 200
#define LOOP_MS          5

#define CENTRE_LEVEL     64    // team-colour centre LEDs, "medium"
#define RING_LEVEL       255   // ring while owned
#define EXT_LEVEL        128   // external strip, relative to the ring
#define DISP_FULL        8
#define DISP_DIM         2
#define DISP_IDLE        6

#define PROTO_VERSION    3

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
static note_t s_off_warn[1];  // falling tone from the reset to the lid power-off
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
static const note_t SND_FACTORY[] = {
    {2000, 500, 300, W_SQUARE, 220}, {0, 0, 60, W_SQUARE, 0}, {2000, 500, 300, W_SQUARE, 220},
};
static const note_t SND_OFF_WARN[] = {{1200, 300, 3000, W_SQUARE, 150}};  // BOOT held: falling to power-off
static const note_t SND_OFF[] = {{1200, 200, 500, W_SQUARE, 220}};
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
static bool s_running;     // clocks run (for whoever owns it)
static int s_owner = -1;   // -1 uncaptured, 0 team 1, 1 team 2
static int s_winner = -1;  // >= 0: game over
static int32_t s_val[2];   // clock values as shown (ms)
static bool s_fresh = true;  // nothing has happened since the last reset
// Game pause from the page: remembers whether the clocks were running, so a
// game resume puts each button back exactly as it was (running or not).
static bool s_game_paused, s_game_was_running;

static struct {
    bool running;
    bool fresh;
    int owner;
    int32_t val[2];
} s_snap;                  // state when the current hold started

enum { HS_BUILD, HS_CAPTURED, HS_PAUSED, HS_RESET, HS_OFF };
static int s_hold_stage;
static int s_hold_target;  // team the capture will go to (decided at the press, shown during the hold)
static int s_hold_last_sec;
static bool s_build_started;
// This hold started during a game pause: no capture. Held to the pause point it
// releases the button from the game pause into a regular pause, so BLE is never needed.
static bool s_hold_gp;
static bool s_off_warn_started;
static bool s_lid_off_request;  // held to the power-off point; the main loop turns it off

static int s_taps;
static uint32_t s_last_tap_ms;
static uint32_t s_batt_until, s_ident_until;
static uint32_t s_win_alarm_until, s_win_next_sound;
static uint32_t s_boot_factory_done, s_boot_off_done;
static bool s_boot_off_showing;  // BOOT held towards power-off: displays say "HoLd" "oFF"

static int s_vsys_mv, s_vbus_mv;

static rgb_t team_colour(int t) { return (rgb_t){s_cfg.colour[t][0], s_cfg.colour[t][1], s_cfg.colour[t][2]}; }
static uint32_t hold_capture_ms(void) { return s_cfg.hold_ms; }
static uint32_t hold_pause_ms(void) { return s_cfg.hold_ms + PAUSE_EXTRA_MS; }
static uint32_t hold_reset_ms(void) { return s_cfg.hold_ms + RESET_EXTRA_MS; }
static uint32_t hold_off_ms(void) { return s_cfg.hold_ms + OFF_EXTRA_MS; }
static uint32_t hold_off_warn_ms(void) { return hold_reset_ms() + OFF_WARN_DELAY; }
static bool has_clock(int t)
{
    (void)t;
    return s_cfg.mode != MODE_TEAMS;
}
// One timer: a single shared clock, kept in both s_val slots (so the page, set time and
// the pause undo need no special case) with duration_ms[0] as its length.
static int32_t duration(int t) { return (int32_t)s_cfg.duration_ms[s_cfg.mode == MODE_ONE_TIMER ? 0 : t]; }
static void share_clock(int from)
{
    if (s_cfg.mode == MODE_ONE_TIMER) s_val[1 - from] = s_val[from];
}
// ms until team t wins on its clock, or -1 if it can't.
static int32_t to_win_ms(int t)
{
    if (!has_clock(t)) return -1;
    if (!s_cfg.count_up) return s_val[t];
    return duration(t) ? duration(t) - s_val[t] : -1;
}

static const char *state_name(void)
{
    if (s_winner >= 0) return "won";
    if (s_running) return "running";
    return s_owner >= 0 ? "paused" : "idle";
}

// Uncaptured: the team the next press goes for. Every press that doesn't
// capture flips it, so "that's the wrong colour, let go, press again" works.
// -1 = not decided yet (use the first-capture setting).
static int s_next_pick = -1;
// Who the next capture goes to (-1 = random, decided at the press).
static int capture_target(void)
{
    if (s_owner >= 0) return s_running ? 1 - s_owner : s_owner;
    if (s_next_pick >= 0) return s_next_pick;
    return (int)s_cfg.default_capture - 1;
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
    s_game_paused = false;
    s_next_pick = -1;
    for (int t = 0; t < 2; t++) s_val[t] = s_cfg.count_up ? 0 : duration(t);
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
    s_snap.fresh = s_fresh;
    s_snap.owner = s_owner;
    s_snap.val[0] = s_val[0];
    s_snap.val[1] = s_val[1];
    s_hold_stage = HS_BUILD;
    s_hold_last_sec = 0;
    s_build_started = false;
    s_hold_gp = s_game_paused;
    s_off_warn_started = false;

    if (s_taps > 0 && now - s_last_tap_ms > TAP_GAP_MS) s_taps = 0;
    s_hold_target = capture_target();
    // Random is decided now, so the hold shows the team from the first moment.
    if (s_hold_target < 0) s_hold_target = (int)(esp_random() & 1);

    audio_pcm_stop();
    AUDIO_PLAY(SLOT_CLICK, SND_CLICK);
    if (s_hold_gp) AUDIO_PLAY(SLOT_EVENT, SND_PAUSE);  // "the game is paused"
    ESP_LOGI(TAG, "face press (%s, owner %d, taps %d, target %d)", state_name(), s_owner + 1, s_taps,
             s_hold_target + 1);
}

// held = first to last contact; the gap to the next tap is measured from the
// last contact too, so the debounce doesn't eat into the hand-off time.
static void on_face_release(uint32_t held)
{
    if (s_hold_stage == HS_BUILD) audio_stop(SLOT_BUILD);
    if (s_hold_stage == HS_RESET) audio_stop(SLOT_DIAG);  // let go before the power-off
    // Uncaptured and let go before the capture: the next press tries the other team.
    if (s_hold_stage == HS_BUILD && s_owner < 0 && s_winner < 0 && !s_hold_gp) s_next_pick = 1 - s_hold_target;
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
    const uint32_t h1 = hold_capture_ms(), h2 = hold_pause_ms(), h3 = hold_reset_ms();
    const bool won = s_winner >= 0;

    // Rising whoop into the capture, stretched over the whole hold time, so it
    // means the same thing at 3 s or 20 s.
    if (s_hold_stage == HS_BUILD && !won && !s_hold_gp && !s_build_started) {
        s_build_started = true;
        uint32_t ms = h1 > held ? h1 - held : 1;
        s_build[0] = (note_t){250, 1200, (uint16_t)ms, W_SQUARE, 140};
        AUDIO_PLAY(SLOT_BUILD, s_build);
    }

    if (s_hold_stage == HS_BUILD && held >= h1) {
        s_hold_stage = HS_CAPTURED;
        audio_stop(SLOT_BUILD);
        if (s_hold_gp && !won) {
            AUDIO_PLAY(SLOT_CLICK, SND_DING);  // game paused: no capture, dings towards the release
        } else if (won) {
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
            s_next_pick = -1;
            s_owner = team;
            s_running = true;
            s_game_paused = false;
            s_fresh = false;
            AUDIO_PLAY(SLOT_EVENT, SND_TEAM[team]);
            ESP_LOGI(TAG, "CAPTURED by team %d", team + 1);
        }
    }
    if (s_hold_stage == HS_CAPTURED && held >= h2) {
        s_hold_stage = HS_PAUSED;
        if (s_hold_gp && !won) {
            // Release from the game pause into a regular pause: owner and clocks as they are.
            s_game_paused = false;
            s_running = false;
            ESP_LOGI(TAG, "game pause released by a hold, owner team %d", s_owner + 1);
        } else if (!won) {
            // Undo this hold's clock changes, then pause (the next hold resumes
            // with that owner). Who owns it while paused:
            //   was running     -> whoever had it before this hold
            //   was uncaptured  -> the team just picked (pre-set the owner at 10:00)
            //   was paused      -> the other team (each 6 s hold swaps it)
            s_val[0] = s_snap.val[0];
            s_val[1] = s_snap.val[1];
            s_fresh = s_snap.fresh;
            if (s_snap.owner >= 0) s_owner = s_snap.running ? s_snap.owner : 1 - s_snap.owner;
            s_running = false;
            ESP_LOGI(TAG, "PAUSED, owner team %d", s_owner + 1);
        }
        if (!won) AUDIO_PLAY(SLOT_EVENT, SND_PAUSE);  // after a win there is nothing to pause
    }
    if (s_hold_stage == HS_PAUSED && held >= h3) {
        s_hold_stage = HS_RESET;
        game_reset(now);
        AUDIO_PLAY(SLOT_EVENT, SND_RESET);
    }
    // Still held after the reset: "HoLd" "oFF" with a falling tone, then power off.
    if (s_hold_stage == HS_RESET && held >= hold_off_warn_ms() && !s_off_warn_started) {
        s_off_warn_started = true;
        uint32_t ms = hold_off_ms() > held ? hold_off_ms() - held : 1;
        s_off_warn[0] = (note_t){1200, 300, (uint16_t)ms, W_SQUARE, 150};
        AUDIO_PLAY(SLOT_DIAG, s_off_warn);
    }
    if (s_hold_stage == HS_RESET && held >= hold_off_ms()) {
        s_hold_stage = HS_OFF;
        s_lid_off_request = true;
        ESP_LOGI(TAG, "lid held to power-off");
    }
    // A sound every second after the capture: dings towards pause, doom towards
    // reset. After a win: dings all the way to the reset (nothing to lose).
    int sec = (int)(held / 1000);
    if (sec != s_hold_last_sec && (uint32_t)sec * 1000 != h1 && (uint32_t)sec * 1000 != h2) {
        if (won) {
            if (s_hold_stage < HS_RESET) AUDIO_PLAY(SLOT_CLICK, SND_DING);
        } else if (s_hold_stage == HS_CAPTURED) {
            AUDIO_PLAY(SLOT_CLICK, SND_DING);
        } else if (s_hold_stage == HS_PAUSED) {
            AUDIO_PLAY(SLOT_CLICK, SND_DOOM);
        }
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
            if (duration(t) && s_val[t] >= duration(t)) {
                s_val[t] = duration(t);
                game_win(t, now);
            }
        }
        share_clock(t);
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
    // 10 was "lock" (removed: a stuck lock was a failure mode)
    OP_SHOW_BATTERY = 11,
    OP_FACTORY_RESET = 12,
    OP_POWER_OFF = 13,
    OP_POWER_ON = 14,
    OP_REBOOT = 15,
    OP_GAME_PAUSE = 16,  // pause, remembering whether it was running
    OP_GAME_RESUME = 17, // undo a game pause (runs again only if it was running)
};

// Boots since power-on (survives resets, not power loss), to spot brown-outs.
static RTC_NOINIT_ATTR uint32_t s_boot_count;
static uint8_t s_reset_reason;

typedef struct __attribute__((packed)) {
    uint8_t version;      //  0 PROTO_VERSION
    uint8_t flags;        //  1 bit0 running, 1 won, 2 locked, 3 face held, 4 identifying, 5 usb, 6 clip playing
    int8_t owner;         //  2 -1 uncaptured, 0, 1
    int8_t winner;        //  3 -1 none
    int8_t target;        //  4 next capture (-1 random); while held: the hold's target
    uint8_t hold_stage;   //  5 HS_* while held
    uint16_t held_ms;     //  6
    int32_t time_ms[2];   //  8, 12 clock values as shown
    uint16_t vsys_mv;     // 16
    uint16_t vbus_mv;     // 18
    int16_t batt_pct;     // 20
    uint8_t connections;  // 22
    uint8_t reset_reason; // 23 esp_reset_reason_t of the last boot (9 = brown-out)
    uint32_t uptime_s;    // 24
    uint32_t boots;       // 28 boots since power-on
    uint8_t power;        // 32 0 on, 1 peek, 2 off
    uint8_t flags2;       // 33 bit0 dimmed (idle), bit1 battery low warning
    uint16_t batt_mv;     // 34 averaged VSYS while on, last rest reading while off (0 = unknown)
} state_pkt_t;            // 36 bytes
_Static_assert(sizeof(state_pkt_t) == 36, "state_pkt_t layout is part of the BLE protocol");

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


static void apply_config(const config_t *in, uint32_t now)
{
    config_t old = s_cfg;
    s_cfg = *in;
    config_sanitise(&s_cfg);
    config_save(&s_cfg);
    bool structural = old.mode != s_cfg.mode || old.count_up != s_cfg.count_up;
    // Before anything has happened, new durations / initial owner apply at
    // once; mid-game they wait for the next reset (or use set time).
    if (structural || s_fresh) game_reset(now);
    if (strcmp(old.name, s_cfg.name) != 0) apply_name();
    audio_set_volume(s_cfg.volume);
    ble_publish_config(&s_cfg, sizeof s_cfg);
    mesh_publish_config(&s_cfg, sizeof s_cfg);
    ESP_LOGI(TAG, "settings: mode %d, hold %u ms, %s, numbers %s, durations %lu/%lu s, volume %d, leds %d",
             s_cfg.mode, s_cfg.hold_ms, s_cfg.count_up ? "up" : "down", s_cfg.show_numbers ? "shown" : "hidden",
             (unsigned long)(s_cfg.duration_ms[0] / 1000), (unsigned long)(s_cfg.duration_ms[1] / 1000), s_cfg.volume,
             s_cfg.led_level);
}

static bool any_face_raw(void)
{
    return gpio_get_level(PIN_FACE1) == 0 || gpio_get_level(PIN_FACE2) == 0 || gpio_get_level(PIN_FACE3) == 0;
}

// ---------------------------------------------------------------- power
// ON    everything powered; the CPU light-sleeps between 20 ms loop passes
//       when nothing is happening (auto light sleep, not on USB).
// PEEK  woken from OFF by a press or the page: battery shown for a few
//       seconds. Turns on only on a clear intent: PEEK_ON_PRESSES
//       presses during the peek, or a press started during it held for
//       POWER_ON_HOLD_MS (the waking press doesn't count, so a bump can't).
//       Otherwise back to OFF when the battery display ends.
// OFF   PWR_EN low (only the ESP powered), light sleep, slow low-power BLE
//       adverts so the page can still find it. Battery checked hourly.
// Low battery (not on USB): deep sleep. Below BATT_LOW_MV a press wakes it
// to show "bAtt Lo" and sleep again; below BATT_DEAD_MV only USB wakes it.
typedef enum { PWR_ON, PWR_PEEK, PWR_OFF } pwr_t;
static const char *const PWR_NAME[] = {"on", "peek", "off"};

#define POWER_ON_HOLD_MS   1500  // a hold that starts during a peek turns it on
#define PEEK_ON_PRESSES    2     // ... or this many presses during the peek
// Presses during the current peek (the press that woke it doesn't count), and
// whether the current press started during the peek.
static int s_peek_presses;
static bool s_peek_hold;
// Idle = no press and no page command (game state is kept throughout; a tap
// brings it straight back). Mid-game something always happens well within these.
#define IDLE_SPARSE_MS     (10 * 60 * 1000)      // every 2nd ring/strip LED off (LED1/2 stay)
#define IDLE_DIM_MS        (60 * 60 * 1000)      // probably forgotten: 10 % brightness
#define AUTO_OFF_MS        (4 * 60 * 60 * 1000)  // then off
#define DIM_LEVEL          26                    // brightness cap while dimmed (10 %)
#define OFF_BATT_CHECK_MS  (60 * 60 * 1000)
#define OFF_RECHECK_MS     60000             // a low reading is confirmed once before acting
#define BATT_TAU_MS        60000             // averaging while on, so speaker/LED dips don't count
#define BATT_WARN_MV       3450
#define BATT_WARN_EVERY_MS (5 * 60 * 1000)
#define BATT_LOW_MV        3300
#define BATT_DEAD_MV       3000
#define BATT_STORAGE_MV    3500              // off below this (or after OFF_BLE_MAX_MS): BLE off too
#define OFF_BLE_MAX_MS     (8ULL * 24 * 3600 * 1000)  // off-BLE lasts this long at most
#define USB_MV             4000

static pwr_t s_pwr = PWR_ON;
static bool s_periph_on;
static uint32_t s_last_activity;
static int64_t s_off_since_us;
static uint32_t s_next_batt_check, s_next_batt_warn;
static int s_batt_mv;     // averaged VSYS while on, last rest reading while off
static int s_low_readings;
static bool s_dim, s_sparse;
static esp_pm_lock_handle_t s_usb_lock;
static bool s_usb_lock_held;

static void startup_test(void);
static uint8_t s_last_frame_valid;

static bool on_usb(void) { return s_vbus_mv > USB_MV; }

static void activity(uint32_t now) { s_last_activity = now; }

// On USB, stay awake: light sleep would drop the USB serial console, and the
// power is free anyway.
static void usb_lock_update(void)
{
    if (!s_usb_lock) return;
    bool want = on_usb();
    if (want == s_usb_lock_held) return;
    s_usb_lock_held = want;
    if (want) esp_pm_lock_acquire(s_usb_lock);
    else esp_pm_lock_release(s_usb_lock);
}

static void periph_power(bool on)
{
    if (on == s_periph_on) return;
    if (on) {
        gpio_set_level(PIN_PWR_EN, 1);
        vTaskDelay(pdMS_TO_TICKS(30));  // rails settle; GN1650 power-up
        display_init();
        s_last_frame_valid = 0;
        s_periph_on = true;
        audio_set_allowed(true);
    } else {
        audio_set_allowed(false);
        uint8_t blank[4] = {0};
        display_set(0, blank, 0);
        display_set(1, blank, 0);
        rgb_t frame[LED_COUNT] = {0};
        leds_show(frame, LED_COUNT);  // matters on USB, where VSYS stays up
        vTaskDelay(pdMS_TO_TICKS(5));
        s_periph_on = false;
        gpio_set_level(PIN_PWR_EN, 0);
    }
}

// Unloaded battery reading while off: power VSYS just long enough to read it.
static int read_batt_rest(void)
{
    if (s_periph_on) return sense_vsys_mv();
    gpio_set_level(PIN_PWR_EN, 1);
    vTaskDelay(pdMS_TO_TICKS(50));  // divider filter is ~5 ms; the LED caps charge too
    int mv = sense_vsys_mv();
    gpio_set_level(PIN_PWR_EN, 0);
    return mv;
}

// Low battery: deep sleep (~10 uA) with everything unpowered. USB always
// wakes it; a lid press only if press_wake.
static void deep_sleep(bool press_wake)
{
    ESP_LOGW(TAG, "battery %d mV: deep sleep (%s)", s_batt_mv, press_wake ? "press or USB wakes" : "only USB wakes");
    periph_power(false);
    for (int i = 0; i < 500 && press_wake && any_face_raw(); i++) vTaskDelay(pdMS_TO_TICKS(10));
    if (press_wake) {
        const gpio_num_t wake[3] = {PIN_FACE1, PIN_FACE2, PIN_FACE3};
        uint64_t mask = 0;
        for (int i = 0; i < 3; i++) {
            rtc_gpio_pullup_en(wake[i]);
            rtc_gpio_pulldown_dis(wake[i]);
            mask |= 1ULL << wake[i];
        }
        esp_sleep_enable_ext1_wakeup_io(mask, ESP_EXT1_WAKEUP_ANY_LOW);
    }
    // 0.6 x VBUS: high when a charger is plugged in (per-pin levels on the C6).
    if (!on_usb()) esp_sleep_enable_ext1_wakeup_io(1ULL << PIN_VBUS_SENSE, ESP_EXT1_WAKEUP_ANY_HIGH);
    esp_deep_sleep_start();
}

static void show_low_and_sleep(int mv)
{
    // "bAtt Lo" for 3 s, quietly, then back to deep sleep.
    periph_power(true);
    // "bAtt" (t as a mirrored 7) / "  Lo".
    uint8_t a[4] = {display_char('b'), display_char('A'), 0x31, 0x31};
    uint8_t b[4] = {0, 0, 0x38 /*L*/, 0x5C /*o*/};
    display_set(0, a, 8);
    display_set(1, b, 8);
    vTaskDelay(pdMS_TO_TICKS(3000));
    s_batt_mv = mv;
    deep_sleep(mv >= BATT_DEAD_MV);
}

static void power_set(pwr_t to, uint32_t now)
{
    if (to == s_pwr) return;
    pwr_t from = s_pwr;
    ESP_LOGI(TAG, "power %s -> %s", PWR_NAME[from], PWR_NAME[to]);
    if (to == PWR_OFF) {
        if (s_periph_on) {
            AUDIO_PLAY(SLOT_EVENT, SND_OFF);
            vTaskDelay(pdMS_TO_TICKS(550));
        }
        periph_power(false);
        ble_set_slow(true);
        mesh_set_off(true);
        s_off_since_us = esp_timer_get_time();
        s_next_batt_check = now + 10000;  // first check soon, then hourly
        s_low_readings = 0;
        s_batt_until = s_ident_until = 0;
        if (s_cfg.mode != MODE_TEAMS) s_running = false;  // clocks pause while off
    } else {
        periph_power(true);
        ble_set_slow(false);
        mesh_set_off(false);
        s_batt_mv = s_vsys_mv = sense_vsys_mv();
        if (to == PWR_PEEK) {
            s_peek_presses = 0;
            s_peek_hold = false;
            s_batt_until = now + BATT_SHOW_MS;
            AUDIO_PLAY(SLOT_EVENT, SND_BATT);
        } else {
            startup_test();
            activity(now);
            s_next_batt_warn = now;
        }
    }
    s_pwr = to;
}

// Called every loop pass while on or peeking (after the ADC is read).
static void power_tick_on(uint32_t now, uint32_t dt)
{
    if (s_pwr == PWR_PEEK) {
        // Hold to turn on (a second press also does, in the main loop);
        // otherwise drop back to off once the battery display ends.
        if (s_peek_hold && s_face.state && input_held_ms(&s_face) >= POWER_ON_HOLD_MS) {
            power_set(PWR_ON, now);
            return;
        }
        if (s_face.state || audio_pcm_playing() || (int32_t)(s_ident_until - now) > 0) s_batt_until = now + 1000;
        if ((int32_t)(now - s_batt_until) >= 0) power_set(PWR_OFF, now);
        return;
    }

    // (Activity stamped a moment after this loop's "now" must count as no idle time, not as
    // 49 days of it.)
    int32_t since = (int32_t)(now - s_last_activity);
    uint32_t idle = since > 0 ? (uint32_t)since : 0;
    s_sparse = idle >= IDLE_SPARSE_MS;
    s_dim = idle >= IDLE_DIM_MS;
    if (idle >= AUTO_OFF_MS) {
        ESP_LOGI(TAG, "idle for %lu min: auto off", (unsigned long)(idle / 60000));
        power_set(PWR_OFF, now);
        return;
    }

    if (on_usb()) return;  // VSYS is the charger, not the cell
    // Long average of VSYS (the cell less QBAT1's drop).
    s_batt_mv += (int)(((int64_t)(s_vsys_mv - s_batt_mv) * (int64_t)dt) / BATT_TAU_MS);
    if (s_batt_mv < BATT_LOW_MV) {
        show_low_and_sleep(s_batt_mv);
    } else if (s_batt_mv < BATT_WARN_MV && (int32_t)(now - s_next_batt_warn) >= 0) {
        s_next_batt_warn = now + BATT_WARN_EVERY_MS;
        s_batt_until = now + BATT_SHOW_MS;
        AUDIO_PLAY(SLOT_EVENT, SND_BATT);
        ESP_LOGW(TAG, "battery low: %d mV average", s_batt_mv);
    }
}

// Called once a second while off.
static void power_tick_off(uint32_t now)
{
    if (on_usb() || (int32_t)(now - s_next_batt_check) < 0) return;
    int mv = read_batt_rest();
    s_batt_mv = mv;
    bool long_off = esp_timer_get_time() - s_off_since_us > (int64_t)OFF_BLE_MAX_MS * 1000;
    bool low = mv < BATT_STORAGE_MV || long_off;
    ESP_LOGI(TAG, "off: battery %d mV%s", mv, low ? " (low)" : "");
    if (!low) {
        s_low_readings = 0;
        s_next_batt_check = now + OFF_BATT_CHECK_MS;
        return;
    }
    if (++s_low_readings < 2) {
        s_next_batt_check = now + OFF_RECHECK_MS;
        return;
    }
    deep_sleep(mv >= BATT_DEAD_MV);
}

// Face switches wake the CPU from light sleep, and the interrupt wakes the
// main loop at once (it otherwise sleeps 20 ms when on, 1 s when off). The
// interrupt is level-triggered (needed for light-sleep wake), so it disables
// itself and the loop re-arms it once the switch is open again.
static TaskHandle_t s_main_task;
static const gpio_num_t FACE_PINS[3] = {PIN_FACE1, PIN_FACE2, PIN_FACE3};

static void face_isr(void *arg)
{
    gpio_intr_disable((gpio_num_t)(intptr_t)arg);
    BaseType_t hp = pdFALSE;
    vTaskNotifyGiveFromISR(s_main_task, &hp);
    portYIELD_FROM_ISR(hp);
}

static void wake_init(void)
{
    s_main_task = xTaskGetCurrentTaskHandle();
    gpio_install_isr_service(0);
    for (int i = 0; i < 3; i++) {
        gpio_wakeup_enable(FACE_PINS[i], GPIO_INTR_LOW_LEVEL);
        gpio_isr_handler_add(FACE_PINS[i], face_isr, (void *)(intptr_t)FACE_PINS[i]);
        gpio_intr_enable(FACE_PINS[i]);
    }
    esp_sleep_enable_gpio_wakeup();
}

static void wake_rearm(void)
{
    for (int i = 0; i < 3; i++)
        if (gpio_get_level(FACE_PINS[i])) gpio_intr_enable(FACE_PINS[i]);
}

static uint8_t state_flags(uint32_t now)
{
    return (uint8_t)((s_running ? 1 : 0) | (s_winner >= 0 ? 2 : 0) | (s_game_paused ? 4 : 0) |
                     (s_face.state ? 8 : 0) | ((int32_t)(s_ident_until - now) > 0 ? 16 : 0) |
                     (s_vbus_mv > 4000 ? 32 : 0) | (audio_pcm_playing() ? 64 : 0));
}
static uint8_t state_flags2(void)
{
    return (uint8_t)((s_dim ? 1 : 0) | (s_sparse ? 4 : 0) |
                     (!on_usb() && s_batt_mv && s_batt_mv < BATT_WARN_MV ? 2 : 0));
}
// Phones (and anything else not a button) connected to this button.
static int phone_links(void)
{
    int n = ble_connections() - mesh_button_links();
    return n < 0 ? 0 : n;
}

static void publish_state(uint32_t now)
{
    bool held = s_face.state;
    uint32_t hm = input_held_ms(&s_face);
    state_pkt_t p = {
        .version = PROTO_VERSION,
        .flags = state_flags(now),
        .owner = (int8_t)s_owner,
        .winner = (int8_t)s_winner,
        .target = (int8_t)(held ? s_hold_target : capture_target()),
        .hold_stage = (uint8_t)(held ? s_hold_stage : 0),
        .held_ms = (uint16_t)(hm > 65535 ? 65535 : hm),
        .time_ms = {s_val[0], s_val[1]},
        .reset_reason = s_reset_reason,
        .boots = s_boot_count,
        .power = (uint8_t)s_pwr,
        .flags2 = state_flags2(),
        .batt_mv = (uint16_t)(s_batt_mv > 0 ? s_batt_mv : 0),
        .vsys_mv = (uint16_t)s_vsys_mv,
        .vbus_mv = (uint16_t)s_vbus_mv,
        .batt_pct = (int16_t)batt_percent(s_vsys_mv),
        .connections = (uint8_t)phone_links(),
        .uptime_s = now / 1000,
    };
    ble_publish_state(&p, sizeof p);
}

// The network copy of the state: sent when something changes (clocks and the hold time are
// extrapolated by whoever receives it), plus every 10 s. Status every 30 s and when the phone
// count changes.
#define HOT_EVERY_MS 10000
#define STATUS_EVERY_MS 30000
#define CLOCK_SLACK_MS 300
static mesh_hot_t s_hot_last;
static uint32_t s_hot_last_ms, s_status_next;
static bool s_hot_sent;
static int s_status_phones = -1;

static void net_publish(uint32_t now)
{
    bool held = s_face.state;
    uint32_t hm = input_held_ms(&s_face);
    mesh_hot_t h = {
        .flags = state_flags(now),
        .owner = (int8_t)s_owner,
        .winner = (int8_t)s_winner,
        .target = (int8_t)(held ? s_hold_target : capture_target()),
        .hold_stage = (uint8_t)(held ? s_hold_stage : 0),
        .held_ms = (uint16_t)(hm > 65535 ? 65535 : hm),
        .time_ms = {s_val[0], s_val[1]},
        .power = (uint8_t)s_pwr,
        .flags2 = state_flags2(),
    };
    bool send = !s_hot_sent || now - s_hot_last_ms >= HOT_EVERY_MS;
    if (!send) {
        mesh_hot_t a = h, b = s_hot_last;
        a.held_ms = b.held_ms = 0;
        a.time_ms[0] = a.time_ms[1] = b.time_ms[0] = b.time_ms[1] = 0;
        send = memcmp(&a, &b, sizeof a) != 0;
        // A clock that moved other than by running (set or add time): its own change.
        uint32_t e = now - s_hot_last_ms;
        for (int t = 0; t < 2 && !send; t++) {
            int32_t d = h.time_ms[t] - s_hot_last.time_ms[t];
            uint32_t ad = (uint32_t)(d < 0 ? -d : d);
            send = ad > CLOCK_SLACK_MS && (ad > e ? ad - e : e - ad) > CLOCK_SLACK_MS;
        }
    }
    if (send) {
        mesh_publish_hot(&h);
        s_hot_last = h;
        s_hot_last_ms = now;
        s_hot_sent = true;
    }
    int phones = phone_links();
    if (phones != s_status_phones || (int32_t)(now - s_status_next) >= 0) {
        s_status_phones = phones;
        s_status_next = now + STATUS_EVERY_MS;
        mesh_status_t s = {
            .vsys_mv = (uint16_t)s_vsys_mv,
            .vbus_mv = (uint16_t)s_vbus_mv,
            .batt_pct = (int16_t)batt_percent(s_vsys_mv),
            .batt_mv = (uint16_t)(s_batt_mv > 0 ? s_batt_mv : 0),
            .phones = (uint8_t)phones,
            .reset_reason = s_reset_reason,
            .uptime_s = now / 1000,
            .boots = s_boot_count,
        };
        mesh_publish_status(&s);
    }
}
static void factory_reset(uint32_t now)
{
    config_t d;
    config_defaults(&d);
    apply_config(&d, now);
    game_reset(now);
    AUDIO_PLAY(SLOT_EVENT, SND_FACTORY);
    ESP_LOGI(TAG, "default settings restored");
}

static int32_t rd_i32(const uint8_t *p) { return (int32_t)(p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24); }

static void handle_command(const uint8_t *d, int n, uint32_t now)
{
    if (n < 1) return;
    activity(now);
    // Off: things that need the displays, LEDs or speaker wake it briefly (peek).
    if (s_pwr == PWR_OFF && (d[0] == OP_IDENTIFY || d[0] == OP_SHOW_BATTERY || d[0] == OP_PLAY_SOUND))
        power_set(PWR_PEEK, now);
    switch (d[0]) {
    case OP_POWER_ON:
        power_set(PWR_ON, now);
        break;
    case OP_START:
        s_game_paused = false;
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
            share_clock(d[1]);
        }
        break;
    case OP_SET_OWNER:
        if (n >= 2) {
            int o = (int8_t)d[1];
            if (o < -1 || o > 1) break;
            s_owner = o;
            s_fresh = false;
            if (o >= 0 && s_cfg.mode == MODE_TEAMS && s_winner < 0) {
                if (s_game_paused) s_game_was_running = true;  // goes live at the game resume
                else s_running = true;
            }
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
    case OP_GAME_PAUSE:
        if (!s_game_paused) {
            s_game_paused = true;
            s_game_was_running = s_running;
            if (s_running) {
                s_running = false;
                AUDIO_PLAY(SLOT_EVENT, SND_PAUSE);
            }
            ESP_LOGI(TAG, "GAME PAUSE (was %s)", s_game_was_running ? "running" : "stopped");
        }
        break;
    case OP_GAME_RESUME:
        if (s_game_paused) {
            s_game_paused = false;
            if (s_game_was_running && s_winner < 0 && !s_running) {
                s_running = true;
                AUDIO_PLAY(SLOT_EVENT, SND_START);
            }
            ESP_LOGI(TAG, "GAME RESUME");
        }
        break;
    case OP_REBOOT:
        ESP_LOGW(TAG, "reboot requested from the page");
        vTaskDelay(pdMS_TO_TICKS(100));  // let the write response go out
        esp_restart();
        break;
    case OP_SHOW_BATTERY:
        s_batt_until = now + BATT_SHOW_MS;
        AUDIO_PLAY(SLOT_EVENT, SND_BATT);
        break;
    case OP_FACTORY_RESET:
        factory_reset(now);
        break;
    case OP_POWER_OFF:
        power_set(PWR_OFF, now);
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
        activity(now);
    }
}

// Brightness after the setting and idle dimming: LEDs 0..255, displays 1..8 (0 = off).
// Dimmed means "at most 10 %", not "10 % of the setting" (which could be invisible).
static int led_level(void) { return s_dim && s_cfg.led_level > DIM_LEVEL ? DIM_LEVEL : s_cfg.led_level; }
static int disp_level(int bright)
{
    if (bright <= 0) return 0;
    int b = (bright * led_level() + 254) / 255;
    if (b < 1) b = 1;
    // 1/8 is the GN1650's lowest step: keep highlighted displays one step above
    // dim ones so the difference survives a low brightness setting.
    if (bright > DISP_DIM && b < 2) b = 2;
    return b;
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
        bool lid_off = s_pwr == PWR_ON && s_face.state && s_hold_stage == HS_RESET &&
                       input_held_ms(&s_face) >= hold_off_warn_ms();
        if (s_boot_off_showing || lid_off) {
            static const uint8_t HOLD[4] = {0x76 /*H*/, 0x5C /*o*/, 0x38 /*L*/, 0x5E /*d*/};
            static const uint8_t OFF[4] = {0, 0x5C /*o*/, 0x71 /*F*/, 0x71 /*F*/};
            memcpy(seg, t == 0 ? HOLD : OFF, sizeof seg);
            bright = DISP_FULL;
        } else if (identifying(now)) {
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
        } else if (s_cfg.mode == MODE_TEAMS || s_cfg.show_numbers) {
            // Two timers: both clocks, always. One timer (the shared clock) and teams only
            // (dashes, as a place for the colon and highlight): only on the owner's display,
            // the other stays blank, so it reads as one thing that belongs to the owner.
            // Uncaptured, it's on both and nobody is lit; a hold towards a capture also
            // lights the team it goes to.
            const int src = t;
            // Held towards a capture: the old state is gone at once. The new team is
            // lit; the old owner's clock keeps ticking (dim) until the capture.
            bool holding = s_face.state && s_hold_stage == HS_BUILD && s_winner < 0 && !s_hold_gp;
            bool shown = s_cfg.mode == MODE_TWO_TIMERS ||
                         (s_winner >= 0 ? src == s_winner : s_owner < 0 || src == s_owner || (holding && src == s_hold_target));
            if (shown) {
                if (s_cfg.mode == MODE_TEAMS) {
                    for (int i = 0; i < 4; i++) seg[i] = display_char('-');
                } else {
                    render_clock(seg, s_val[src], !s_cfg.count_up);
                }
                bool colon = true;
                if (s_winner >= 0) {
                    bright = src == s_winner ? (((now / 250) & 1) ? DISP_FULL : 0) : DISP_DIM;
                } else if (holding) {
                    bright = src == s_hold_target ? DISP_FULL : DISP_DIM;
                    if (s_running && src == s_owner) colon = (now % 250) >= 125;
                } else if (s_running && s_owner >= 0) {
                    if (src == s_owner) {
                        colon = (now % 250) >= 125;  // 4 Hz, so the owner is obvious at a glance
                        bright = DISP_FULL;
                    } else {
                        bright = DISP_DIM;
                    }
                } else if (s_owner >= 0) {
                    bright = src == s_owner ? DISP_FULL : DISP_DIM;  // paused: the owner is lit
                } else {
                    bright = DISP_IDLE;  // uncaptured: nobody is lit
                }
                display_colon(seg, colon);
                if (!holding && s_winner < 0 && !s_running && s_owner == src) {
                    // Paused: a single slow-blinking dot instead of the colon.
                    seg[2] &= (uint8_t)~SEG_COLON;
                    seg[3] = (uint8_t)((seg[3] & ~SEG_COLON) | (((now / 1000) & 1) ? SEG_COLON : 0));
                } else if (!holding && s_winner < 0 && s_owner < 0 && capture_target() == src) {
                    // Uncaptured: one colon dot missing on the team the next press goes
                    // for. Subtle on purpose: it must never look claimed.
                    seg[2] &= (uint8_t)~SEG_COLON;
                }
            }
        }
        display_set(t, seg, identifying(now) ? bright : disp_level(bright));
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
        // Uncaptured: slow (0.5 Hz) faint white pulse. No team colour, so it can
        // never be mistaken for claimed.
        float ph = (float)(now % 2000) / 2000.0f;
        int level = (int)(UNCAPTURED_LEVEL * 0.5f * (1.0f - cosf(2.0f * (float)M_PI * ph)) + 0.5f);
        for (int i = 0; i < 6; i++) ring[i] = rgb_scale(WHITE, level);
    }

    // Hold progress.
    if (s_face.state) {
        uint32_t held = input_held_ms(&s_face);
        uint32_t h1 = hold_capture_ms(), h2 = hold_pause_ms(), h3 = hold_reset_ms();
        if (s_winner >= 0 && s_hold_stage < HS_RESET) {
            int n = (int)(held * 6 / h3);
            for (int i = 0; i < 6; i++) ring[i] = (i < n) ? rgb_scale(RED, 160) : (rgb_t){0};
        } else if (s_hold_gp && s_hold_stage <= HS_CAPTURED) {
            // Game paused: a slow white fill towards the release, never a team colour.
            int n = (int)(held * 6 / h2);
            for (int i = 0; i < 6; i++) ring[i] = (i < n) ? rgb_scale(WHITE, 70) : (rgb_t){0};
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
        } else if (held >= hold_off_warn_ms()) {
            // Past the reset, still held: the ring drains towards the power-off.
            uint32_t w = hold_off_warn_ms(), span = hold_off_ms() - w;
            int n = 6 - (int)((held - w) * 6 / span);
            for (int i = 0; i < 6; i++) ring[i] = (i < n) ? rgb_scale(WHITE, 120) : (rgb_t){0};
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
    // Peek: holding fills the ring white towards power-on.
    if (s_pwr == PWR_PEEK && s_face.state && s_peek_hold) {
        int n = (int)(input_held_ms(&s_face) * 6 / POWER_ON_HOLD_MS);
        for (int i = 0; i < 6; i++) ring[i] = (i < n) ? rgb_scale(WHITE, 160) : (rgb_t){0};
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

    // Idle 10 min+: every second ring and strip LED off (LED1/2 stay), still visible, less power.
    if (s_sparse) {
        for (int i = 1; i < 6; i += 2) frame[RING_CW[i]] = (rgb_t){0};
        for (int i = 1; i < LED_COUNT_EXT; i += 2) frame[LED_IDX_EXT + i] = (rgb_t){0};
    }
    int level = identifying(now) ? 255 : led_level();
    if (level < 255)
        for (int i = 0; i < LED_COUNT; i++) frame[i] = rgb_scale(frame[i], level);
    // Only send when something changed (saves the RMT transfer and lets the CPU sleep).
    static rgb_t last[LED_COUNT];
    if (s_last_frame_valid && memcmp(frame, last, sizeof frame) == 0) return;
    memcpy(last, frame, sizeof frame);
    s_last_frame_valid = 1;
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
    printf("[%7.1fs] %-4s %-7s mode=%d owner=%d T1=%s T2=%s hold=%4lums sw=%d%d%d boot=%d ped=%d%d "
           "vsys=%dmV vbus=%dmV ble=%d%s disp_nak=%lu/%lu heap=%u\n",
           now / 1000.0, PWR_NAME[s_pwr], state_name(), s_cfg.mode, s_owner + 1, t1, t2, (unsigned long)input_held_ms(&s_face),
           s_in[IN_FACE1].state, s_in[IN_FACE2].state, s_in[IN_FACE3].state, s_in[IN_BOOT].state,
           s_in[IN_PEDAL1].state, s_in[IN_PEDAL2].state, s_vsys_mv, s_vbus_mv, ble_connections(),
           "", (unsigned long)display_nak_count(0), (unsigned long)display_nak_count(1),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_DEFAULT));
}

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

// A command for this button that came through the network (from an admin on any button).
static void on_mesh_cmd(uint8_t kind, const uint8_t *d, int n, uint32_t now)
{
    if (kind == MESH_CMD_CONFIG) {
        if (n == sizeof(config_t) && d[0] == CONFIG_VERSION) {
            config_t c;
            memcpy(&c, d, sizeof c);
            apply_config(&c, now);
        } else {
            ESP_LOGW(TAG, "network config ignored (%d bytes, version %d)", n, n ? d[0] : 0);
        }
    } else if (kind == MESH_CMD_OP) {
        handle_command(d, n, now);
    }
    activity(now);
}

// ---------------------------------------------------------------- startup
static void power_pins_init(void)
{
    // PWR_EN (VSYS + 3V3_SW) starts low; power_set turns it on.
    gpio_config_t io = {.pin_bit_mask = (1ULL << PIN_PWR_EN) | (1ULL << PIN_STAT_LED), .mode = GPIO_MODE_OUTPUT};
    gpio_config(&io);
    gpio_set_level(PIN_PWR_EN, 0);
    gpio_set_level(PIN_STAT_LED, 0);  // kept off: it's distracting through the case
}

static void startup_test(void)
{
    // Displays: all segments + colon. LEDs: red, green, blue across the chain
    // (checks the GRB order and every LED).
    uint8_t all[4] = {0xFF, 0xFF, 0xFF, 0xFF};
    display_set(0, all, disp_level(DISP_FULL));
    display_set(1, all, disp_level(DISP_FULL));
    AUDIO_PLAY(SLOT_EVENT, SND_STARTUP);
    static const rgb_t test[3] = {{60, 0, 0}, {0, 60, 0}, {0, 0, 60}};
    rgb_t frame[LED_COUNT];
    for (int c = 0; c < 3; c++) {
        for (int i = 0; i < LED_COUNT; i++) frame[i] = rgb_scale(test[c], s_cfg.led_level);
        leds_show(frame, LED_COUNT);
        vTaskDelay(pdMS_TO_TICKS(250));
    }
    s_last_frame_valid = 0;
}

static void pm_init(void)
{
    esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "usb", &s_usb_lock);
    esp_pm_config_t pm = {.max_freq_mhz = 160, .min_freq_mhz = 40, .light_sleep_enable = true};
    esp_err_t err = esp_pm_configure(&pm);
    if (err != ESP_OK) ESP_LOGW(TAG, "power management: %s", esp_err_to_name(err));
    usb_lock_update();
}

// ---------------------------------------------------------------- surviving a reset
// The game state is copied to RTC RAM every loop pass (a plain memory write, no flash wear).
// A brown-out is a system reset on the C6 (IDF's setting), which leaves the low-power domain
// alone: RTC RAM keeps its contents and the LP timer keeps counting. So after a brown-out
// (or a crash / watchdog) the button carries on where it was, with the clock advanced by the
// time it spent rebooting. A real power loss (power-on reset) or a reboot asked for from the
// page starts fresh. The checksum catches RAM that didn't survive.
#define SAVED_MAGIC (0x47414D45u ^ (uint32_t)sizeof(saved_game_t))  // changes with the layout
#define RESTORE_MAX_GAP_US (10ULL * 60 * 1000 * 1000)  // a longer gap means the timer can't be trusted
typedef struct {
    uint32_t magic;
    uint8_t mode, count_up, pwr;
    bool running, fresh, game_paused, game_was_running;
    int8_t owner, winner, next_pick;
    int32_t val[2];
    uint64_t rtc_us;  // LP timer when saved
    uint32_t crc;     // over everything above
} saved_game_t;
static RTC_NOINIT_ATTR saved_game_t s_saved;

static uint32_t saved_crc(void) { return esp_rom_crc32_le(0, (const uint8_t *)&s_saved, offsetof(saved_game_t, crc)); }

static void game_save(void)
{
    s_saved = (saved_game_t){
        .magic = SAVED_MAGIC, .mode = s_cfg.mode, .count_up = s_cfg.count_up, .pwr = (uint8_t)s_pwr,
        .running = s_running, .fresh = s_fresh, .game_paused = s_game_paused, .game_was_running = s_game_was_running,
        .owner = (int8_t)s_owner, .winner = (int8_t)s_winner, .next_pick = (int8_t)s_next_pick,
        .val = {s_val[0], s_val[1]}, .rtc_us = esp_rtc_get_time_us(),
    };
    s_saved.crc = saved_crc();
}

// After game_reset at boot. Returns the power state to come back in, or -1 to boot as usual.
static int game_restore(uint32_t now)
{
    esp_reset_reason_t why = (esp_reset_reason_t)s_reset_reason;
    bool unexpected = why == ESP_RST_BROWNOUT || why == ESP_RST_PANIC || why == ESP_RST_INT_WDT ||
                      why == ESP_RST_TASK_WDT || why == ESP_RST_WDT;
    if (!unexpected) return -1;
    if (s_saved.magic != SAVED_MAGIC || s_saved.crc != saved_crc()) {
        ESP_LOGW(TAG, "reset: no saved game state survived");
        return -1;
    }
    if (s_saved.mode != s_cfg.mode || s_saved.count_up != s_cfg.count_up || s_saved.owner < -1 || s_saved.owner > 1 ||
        s_saved.winner < -1 || s_saved.winner > 1 || s_saved.pwr > PWR_OFF) {
        ESP_LOGW(TAG, "reset: saved game state doesn't fit, starting fresh");
        return -1;
    }
    s_running = s_saved.running;
    s_fresh = s_saved.fresh;
    s_game_paused = s_saved.game_paused;
    s_game_was_running = s_saved.game_was_running;
    s_owner = s_saved.owner;
    s_winner = s_saved.winner;
    s_next_pick = s_saved.next_pick;
    s_val[0] = s_saved.val[0];
    s_val[1] = s_saved.val[1];
    // The time lost rebooting, from the LP timer (if it went backwards or jumped, it was reset: skip).
    uint64_t t = esp_rtc_get_time_us();
    uint32_t gap = t > s_saved.rtc_us && t - s_saved.rtc_us < RESTORE_MAX_GAP_US ? (uint32_t)((t - s_saved.rtc_us) / 1000) : 0;
    if (gap) game_tick(now, gap);  // a clock that ran out meanwhile wins now
    ESP_LOGW(TAG, "reset: game state restored (%s, owner %d, %ld/%ld ms, %lu ms lost rebooting)", state_name(),
             s_owner + 1, (long)s_val[0], (long)s_val[1], (unsigned long)gap);
    return s_saved.pwr == PWR_ON ? PWR_ON : PWR_OFF;  // a peek goes back to off
}

void app_main(void)
{
    s_reset_reason = (uint8_t)esp_reset_reason();
    if (s_reset_reason == ESP_RST_POWERON) s_boot_count = 0;
    s_boot_count++;
    bool from_deep = esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT1;
    if (from_deep) {
        rtc_gpio_deinit(PIN_FACE1);
        rtc_gpio_deinit(PIN_FACE2);
        rtc_gpio_deinit(PIN_FACE3);
        rtc_gpio_deinit(PIN_VBUS_SENSE);
    }
    power_pins_init();
    ESP_LOGI(TAG, "capture button firmware, board b43; reset reason %d%s, boot %lu since power-on", s_reset_reason,
             s_reset_reason == ESP_RST_BROWNOUT ? " (BROWN-OUT)" : from_deep ? " (woke from low-battery sleep)" : "",
             (unsigned long)s_boot_count);
    config_load(&s_cfg);
    if (s_reset_reason == ESP_RST_BROWNOUT) {
        // Don't brown out again on the startup sound: 50 % volume and brightness
        // until changed (not saved, so the stored settings are untouched).
        if (s_cfg.volume > 128) s_cfg.volume = 128;
        if (s_cfg.led_level > 128) s_cfg.led_level = 128;
        ESP_LOGW(TAG, "brown-out reset: volume and brightness capped at 50 %% for now");
    }
    inputs_init();
    sounds_init();
    sense_init();
    display_init();
    leds_init();
    audio_init();
    audio_set_volume(s_cfg.volume);
    s_vbus_mv = sense_vbus_mv();
    pm_init();
    wake_init();

    char name[24];
    if (s_cfg.name[0]) {
        snprintf(name, sizeof name, "%s", s_cfg.name);
    } else {
        default_name(name, sizeof name);
    }
    ble_init(name);
    ble_publish_config(&s_cfg, sizeof s_cfg);
    mesh_init(on_mesh_cmd);
    mesh_publish_config(&s_cfg, sizeof s_cfg);

    uint32_t last = now_ms();
    game_reset(last);
    int restored = game_restore(last);

    // Woken from low-battery deep sleep: charger -> off (slow BLE); press ->
    // peek if the cell has recovered, else show "LO" and sleep again.
    // Any other boot (power-on, flashing, brown-out) comes up on.
    s_pwr = PWR_OFF;
    if (from_deep && on_usb()) {
        s_pwr = PWR_PEEK;  // (peripherals still off, so no "off" sound)
        power_set(PWR_OFF, last);
    } else if (from_deep) {
        int mv = read_batt_rest();
        s_batt_mv = mv;
        ESP_LOGI(TAG, "woken by a press, battery %d mV", mv);
        if (mv < BATT_DEAD_MV) deep_sleep(false);
        if (mv < BATT_STORAGE_MV) show_low_and_sleep(mv);
        s_pwr = PWR_PEEK;
        power_set(PWR_OFF, last);
        power_set(PWR_PEEK, last);
    } else if (restored == PWR_OFF) {
        s_pwr = PWR_PEEK;  // was off before the reset: straight back to off, quietly
        power_set(PWR_OFF, last);
    } else {
        power_set(PWR_ON, last);
    }
    ESP_LOGI(TAG, "VSYS %d mV, VBUS %d mV, display NAKs %lu/%lu", s_vsys_mv, s_vbus_mv,
             (unsigned long)display_nak_count(0), (unsigned long)display_nak_count(1));

    uint32_t boot_skip_press = 0;
    uint32_t next_led = 0, next_disp = 0, next_adc = 0, next_status = 0, next_refresh = 0, next_ble = 0;
    bool ignore_face = false;
    pwr_t prev_pwr = s_pwr;

    for (;;) {
        // Short sleeps while someone is pressing, longer when idle, 1 s when
        // off. A face press wakes the loop at once either way.
        uint32_t t = now_ms();
        bool busy = s_face.state || s_in[IN_BOOT].state || t - s_face.last_active_ms < 1000;
        uint32_t period = s_pwr == PWR_OFF ? 1000 : busy ? LOOP_MS : 20;
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(period));
        uint32_t now = now_ms();
        uint32_t dt = now - last;
        last = now;
        wake_rearm();

        // Inputs.
        bool any_face = false;
        for (int i = 0; i < IN_COUNT; i++) {
            bool raw = gpio_get_level(s_in[i].pin) == 0;
            if (i <= IN_FACE3) any_face |= raw;
            int e = input_update(&s_in[i], raw, now);
            if (e) ESP_LOGI(TAG, "%s %s", s_in[i].name, e > 0 ? "down" : "up");
            if (e > 0 && i <= IN_FACE3 && s_pwr == PWR_ON) AUDIO_PLAY(SLOT_SW1 + i, SND_SW[i]);
            if (e > 0 && i == IN_BOOT && audio_pcm_playing()) {
                audio_pcm_stop();
                boot_skip_press = s_in[IN_BOOT].press_ms;  // that press only stops the clip
            }
            else if (e > 0 && i == IN_BOOT && s_pwr != PWR_OFF) AUDIO_PLAY(SLOT_DIAG, SND_OFF_WARN);
            if (e < 0 && i == IN_BOOT && s_boot_off_done != s_in[IN_BOOT].press_ms) audio_stop(SLOT_DIAG);
        }
        // BOOT held: "HoLd" / "oFF" with a falling tone, off at 3 s (no need to
        // let go). Still held at 8 s: default settings (it wakes briefly to say so).
        s_boot_off_showing = false;
        if (s_in[IN_BOOT].state && boot_skip_press != s_in[IN_BOOT].press_ms) {
            uint32_t held = input_held_ms(&s_in[IN_BOOT]);
            uint32_t press = s_in[IN_BOOT].press_ms;
            s_boot_off_showing = s_pwr != PWR_OFF && held < BOOT_OFF_MS;
            if (held >= BOOT_OFF_MS && s_boot_off_done != press) {
                s_boot_off_done = press;
                power_set(PWR_OFF, now);
            }
            if (held >= BOOT_FACTORY_MS && s_boot_factory_done != press) {
                s_boot_factory_done = press;
                power_set(PWR_PEEK, now);
                factory_reset(now);
            }
        }

        // A press that turned the button on (peek -> on) isn't a game press.
        if (prev_pwr != PWR_ON && s_pwr == PWR_ON && s_face.state) ignore_face = true;
        prev_pwr = s_pwr;
        if (ignore_face) {
            if (any_face) any_face = false;
            else ignore_face = false;
        }
        int fe = input_update(&s_face, any_face, now);
        if (fe > 0 && s_pwr == PWR_PEEK) {
            s_peek_hold = true;
            if (++s_peek_presses >= PEEK_ON_PRESSES) power_set(PWR_ON, now);  // tapping away: wants it on
        } else if (fe > 0 && s_pwr == PWR_OFF) power_set(PWR_PEEK, now);
        if (s_pwr == PWR_ON) {
            if (fe > 0) {
                on_face_press(now);
                activity(now);
            }
            if (s_face.state) hold_progress(now, input_held_ms(&s_face));
            if (s_lid_off_request) {
                s_lid_off_request = false;
                power_set(PWR_OFF, now);
            }
            if (fe < 0) {
                on_face_release(s_face.last_active_ms - s_face.press_ms);
                activity(now);
            }
        }

        ble_service(now);
        mesh_tick(now);
        game_tick(now, dt);
        net_publish(now);
        if (s_pwr == PWR_ON) countdown_blips();
        // Final minute of a countdown: keep the speaker awake so the blips are on time.
        int32_t tw = s_owner >= 0 ? to_win_ms(s_owner) : -1;
        audio_keep_awake(s_pwr == PWR_ON && s_running && s_winner < 0 && s_cfg.show_numbers && tw >= 0 && tw <= 61000);

        if ((int32_t)(now - next_adc) >= 0) {
            next_adc = now + 250;
            if (s_periph_on) s_vsys_mv = sense_vsys_mv();
            s_vbus_mv = sense_vbus_mv();
            usb_lock_update();
        }
        if (s_pwr == PWR_OFF) power_tick_off(now);
        else power_tick_on(now, dt);

        if ((int32_t)(now - next_ble) >= 0) {
            next_ble = now + (s_pwr == PWR_OFF ? 1000 : STATE_PUBLISH_MS);
            publish_state(now);
        }
        if (s_periph_on) {
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
        }
        if ((int32_t)(now - next_status) >= 0) {
            next_status = now + (s_pwr == PWR_OFF ? 10000 : 1000);
            status_line(now);
        }
        game_save();
    }
}
