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
#include "hal/gpio_ll.h"
#include "soc/gpio_struct.h"

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
#define OFF_WARN_DELAY   2000  // "HoLd" "oFF" and the falling tone start this long after the reset
#define DEBOUNCE_MS      50    // the lid is assumed still pressed this long after any contact
#define TAP_MAX_MS       500   // a press shorter than this is a tap
#define TAP_GAP_MS       500   // max hand-off time between taps; also the wait after the last one
#define WIN_ALARM_MS     20000 // win sound repeats this long (or until a hold)
#define WIN_REPEAT_MS    2500
#define PAUSED_LEVEL     14    // ring blink while paused with an owner: only visible close up
#define UNCAPTURED_LEVEL 50    // ring pulse peak while uncaptured, relative to the brightness setting
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
#define DISP_DIM         4     // un-highlighted display: half of the highlighted one
#define DISP_IDLE        6

#define PROTO_VERSION    3

static const rgb_t WHITE = {255, 255, 255};
// Uncaptured: a pale mint green, unlike any team colour and unlike the white used for counts.
static const rgb_t UNCAPTURED_COLOUR = {70, 255, 128}; //blue over 128 so its cyan at 2% brightness with rounding
static const rgb_t RED = {255, 0, 0};
static const rgb_t GREEN = {0, 255, 0};

// Ring positions in clockwise order from north (chain indices).
static const int RING_CW[6] = {3 /*N*/, 2 /*NE*/, 7 /*SE*/, 6 /*S*/, 5 /*SW*/, 4 /*NW*/};

static config_t s_cfg;

// ---------------------------------------------------------------- sounds
// The admin sounds: the same in every sound set (plain synth beeps, so they read as not part of the
// game). The players' sounds are in sounds.c. Square waves carry best on the FS-1540 (resonance
// ~850 Hz); sines sound weak.
static const note_t SND_CLICK[] = {{1, 1, 6, W_NOISE, 255}};
static const note_t SND_SW[3][1] = {
    {{700, 700, 35, W_SQUARE, 180}},
    {{1050, 1050, 35, W_TRI, 255}},
    {{1500, 1500, 35, W_SQUARE, 180}},
};
static note_t s_off_warn[1];  // falling tone from the reset to the lid power-off
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

static int s_blip_last_sec[2];  // (the countdown sounds are in sounds.c)

static void admin_sounds_init(void)
{
    for (int i = 0; i < WHISTLE_NOTES; i++) {
        uint16_t f = (i & 1) ? 2100 : 1850;
        s_whistle[i] = (note_t){f, f, 28, W_SQUARE, 255};
    }
    for (int i = 0; i < IDENT_NOTES; i++) {
        s_ident[i] = (i & 1) ? (note_t){0, 0, 150, W_SQUARE, 0} : (note_t){1400, 1400, 100, W_SQUARE, 255};
    }
}

// ---------------------------------------------------------------- the sound set
// The player-facing sounds come from sounds.c, per the set chosen in the settings.
static bool medieval(void) { return s_cfg.sound_set == SOUNDS_MEDIEVAL; }
// The admin sounds are square and triangle waves at full scale, made to match the sci-fi set. The
// medieval instruments come out 5 to 7 dB quieter (RMS over a call or the fanfare), so there the plain
// waves are turned down 6 dB, and the beeps no longer stand over the game's sounds.
#define PLAIN_GAIN_MEDIEVAL 128
static void play(sound_t s)
{
    for (int i = 0; i < s.count; i++) audio_play(s.v[i].slot, s.v[i].notes, s.v[i].n);
}
static void start_build(uint32_t ms) { play(sounds_build(s_cfg.sound_set, ms)); }  // ends at the capture
static void stop_build(void)
{
    audio_stop(SLOT_BUILD);
    audio_stop(SLOT_BUILD2);
}
static void stop_event(void)
{
    audio_stop(SLOT_EVENT);
    audio_stop(SLOT_EVENT2);
}
static void play_team(int team) { play(sounds_team(s_cfg.sound_set, team)); }
static void play_win(void) { play(sounds_win(s_cfg.sound_set)); }
static void play_blip(int i) { play(sounds_blip(s_cfg.sound_set, i)); }
static void play_let_go(void) { play(sounds_let_go(s_cfg.sound_set)); }

// Sounds the page can play (ids are part of the BLE protocol). The game sounds follow the sound set.
static void play_sound_id(int id)
{
    switch (id) {
    case 0: play_team(0); break;
    case 1: play_team(1); break;
    case 2: play_win(); break;
    case 3: AUDIO_PLAY(SLOT_EVENT, SND_PAUSE); break;
    case 4: AUDIO_PLAY(SLOT_EVENT, SND_RESET); break;
    case 5: AUDIO_PLAY(SLOT_EVENT, SND_DING); break;
    case 6: AUDIO_PLAY(SLOT_EVENT, SND_DOOM); break;
    case 7: play_blip(SOUNDS_BLIPS - 1); break;
    case 8: AUDIO_PLAY(SLOT_EVENT, s_whistle); break;
    case 9: AUDIO_PLAY(SLOT_EVENT, SND_HORN); break;
    case 10: AUDIO_PLAY(SLOT_EVENT, SND_BATT); break;
    case 11: AUDIO_PLAY(SLOT_EVENT, SND_STARTUP); break;
    case 12: AUDIO_PLAY(SLOT_EVENT, SND_BOOT); break;
    case 13: if (audio_pcm_available()) audio_pcm_start(); break;
    case 14: start_build(3000); break;  // the capture build-up, as for a 3 s, 8 s and 20 s hold
    case 15: start_build(8000); break;
    case 16: start_build(20000); break;
    case 17: stop_build(); break;
    case 18: stop_build(); play_let_go(); break;  // let go early (medieval; nothing in sci-fi)
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
// A capture's call plays out before the count towards the pause starts: the pause, reset and power-off
// points of this hold move later by its length (0 for a hold that didn't capture).
static uint32_t s_hold_shift;
#define CAPTURE_DARK_MS 100  // at the capture the ring and displays go dark this long: "done", then the new owner
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
static int batt_now_mv(void);  // the battery estimate (see the power section)

// Team colours are picked on a screen, where the channels are gamma-encoded (sRGB): 128 looks about
// half as bright as 255. A WS2812 is linear in current, so sent as they are, the weaker channel
// comes out far too strong (255/128/0 is meant as yellow and showed green, purple went
// pinkish white). Decoded with the screen's curve first, the mix gets much closer to the picker (these
// LEDs' own primaries still differ: see the defaults in config.c). The
// brightness setting is applied afterwards, separately, so this changes only the mix.
static uint8_t s_gamma[256];
static void gamma_init(void)
{
    for (int i = 0; i < 256; i++) s_gamma[i] = (uint8_t)(255.0f * powf(i / 255.0f, 2.2f) + 0.5f);
}
static rgb_t team_colour(int t)
{
    return (rgb_t){s_gamma[s_cfg.colour[t][0]], s_gamma[s_cfg.colour[t][1]], s_gamma[s_cfg.colour[t][2]]};
}
static uint32_t hold_capture_ms(void) { return s_cfg.hold_ms; }
static uint32_t hold_count_ms(void) { return s_cfg.hold_ms + s_hold_shift; }  // the count to the pause starts
static uint32_t hold_pause_ms(void) { return hold_count_ms() + PAUSE_EXTRA_MS; }
static uint32_t hold_reset_ms(void) { return hold_count_ms() + RESET_EXTRA_MS; }
static uint32_t hold_off_ms(void) { return hold_count_ms() + OFF_EXTRA_MS; }
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
// both ends, so it can read below 0 % or above 100 %. Under load or while
// charging the terminal voltage is off the open-circuit one, so this is only an estimate.
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

static void activity(uint32_t now);
static void game_win(int t, uint32_t now)
{
    s_winner = t;
    s_running = false;
    s_win_alarm_until = now + WIN_ALARM_MS;
    s_win_next_sound = now;
    activity(now);  // a win is an event: it stays on (lit, the fanfare) for the whole idle time
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
        stop_event();
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
    s_hold_shift = 0;
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
    if (s_hold_stage == HS_BUILD) {
        stop_build();
        if (s_build_started && held >= 150) play_let_go();  // (not for taps)
    }
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

    // The build-up into the capture (the sound set's), made for the whole hold time, so it
    // means the same thing at 3 s or 20 s.
    if (s_hold_stage == HS_BUILD && !won && !s_hold_gp && !s_build_started) {
        s_build_started = true;
        uint32_t ms = h1 > held ? h1 - held : 1;
        start_build(ms);
    }

    if (s_hold_stage == HS_BUILD && held >= h1) {
        s_hold_stage = HS_CAPTURED;
        stop_build();
        if (s_hold_gp && !won) {
            AUDIO_PLAY(SLOT_CLICK, SND_DING);  // game paused: no capture, dings towards the release
        } else if (won) {
            if ((int32_t)(s_win_alarm_until - now) > 0) {
                s_win_alarm_until = now;
                stop_event();
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
            sound_t call = sounds_team(s_cfg.sound_set, team);
            play(call);
            s_hold_shift = sounds_ms(call);
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
    // A sound every second: dings towards pause, doom towards reset. After a capture they count from
    // the end of its call (silent while it plays). After a win: dings all the way to the reset, from the
    // press (nothing to lose). None at the capture or the pause, which have their own.
    uint32_t base = s_hold_shift ? hold_count_ms() : 0;
    int sec = held >= base ? (int)((held - base) / 1000) : 0;
    uint32_t at = base + (uint32_t)sec * 1000;
    if (sec != s_hold_last_sec && sec != 0 && at != h1 && at != hold_pause_ms()) {
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
        play_win();
    }

    if (!s_face.state && s_taps > 0 && now - s_last_tap_ms > TAP_GAP_MS) {
        if (s_taps == 3) {
            s_batt_until = now + BATT_SHOW_MS;
            AUDIO_PLAY(SLOT_EVENT, SND_BATT);
            ESP_LOGI(TAG, "triple tap: VSYS %d mV (~%d %%), VBUS %d mV", s_vsys_mv, batt_percent(batt_now_mv()),
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
            for (int i = 0; i < SOUNDS_BLIPS; i++) {
                if (SOUNDS_BLIP_AT[i] == sec) play_blip(i);
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
    OP_FACTORY_OFF = 18, // what a low battery does: deep sleep, no BLE; a lid press or USB wakes it
    // 19 was the awake LED switch; it is a setting now (config_t.debug)
};

// Restarts since power-on (they survive resets, not power loss), to spot brown-outs. Wakes from deep
// sleep aren't restarts and don't count. The last restart's reason and LP-timer time are kept with it.
static RTC_NOINIT_ATTR uint32_t s_boot_count;
static RTC_NOINIT_ATTR uint32_t s_boot_check;  // s_boot_count ^ s_restart_reason ^ BOOT_CHECK when valid
static RTC_NOINIT_ATTR uint32_t s_restart_reason;  // esp_reset_reason_t of the last restart
static RTC_NOINIT_ATTR uint64_t s_restart_rtc_us;  // LP timer at the last restart
static uint32_t since_restart_s(void) { return (uint32_t)((esp_rtc_get_time_us() - s_restart_rtc_us) / 1000000u); }
#define BOOT_CHECK 0x5A3CB007u
static uint32_t s_factory_off_at;  // OP_FACTORY_OFF: when to go (0 = not asked)
static volatile uint32_t s_slept_us;  // light sleep total (wraps; only differences over a status period are used)
static volatile bool s_awake_led;     // debug: red LED lit while out of light sleep (see pm_init)
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
    uint8_t reset_reason; // 23 esp_reset_reason_t of the last restart (9 = brown-out; deep-sleep wakes are not restarts)
    uint32_t uptime_s;    // 24 seconds since the last restart
    uint32_t boots;       // 28 restarts since power-on (counting it)
    uint8_t power;        // 32 0 on, 1 peek, 2 off
    uint8_t flags2;       // 33 bit0 dimmed (idle), bit1 battery low warning, bit2 sparse
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


static void awake_led_set(bool on);

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
    synth_set_plain_gain(medieval() ? PLAIN_GAIN_MEDIEVAL : 256);
    awake_led_set(s_cfg.debug & CFG_DEBUG_AWAKE_LED);
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
// See design/power-lifecycle.md for the whole picture.
// ON    everything powered; the CPU light-sleeps between 20 ms loop passes when nothing is happening.
// AUTO  auto-off after an idle time: dark (PWR_EN low), but still in the game (clocks tick) and in
//       the network exactly as when on. A tap or any command is back on at once, as if it had stayed
//       on; so is a countdown reaching AUTO_WAKE_TO_WIN_MS. The displays flash every AUTO_FLASH_EVERY_MS.
//       After a longer idle time it becomes OFF (not while a countdown runs).
// PEEK  woken from OFF (or a deep sleep) by a press or the page: battery shown for a few seconds.
//       Turns on only on a clear intent: PEEK_ON_PRESSES presses during the peek, or a press started
//       during it held for POWER_ON_HOLD_MS (the waking press doesn't count, so a bump can't).
//       Otherwise back to where it was when the battery display ends.
// OFF   switched off: PWR_EN low, light sleep, slow legacy BLE adverts so the page can still find it.
//       Battery checked hourly.
// Deep sleeps (everything unpowered, the game state in RTC RAM and flash): shipping (a choice), and
// the battery's: low (off with a low cell), dead and overdischarge. See batt_level_t.
typedef enum { PWR_ON, PWR_PEEK, PWR_OFF, PWR_AUTO } pwr_t;  // values are in the BLE state
static const char *const PWR_NAME[] = {"on", "peek", "off", "auto-off"};

#define POWER_ON_HOLD_MS   1500  // a hold that starts during a peek turns it on
#define PEEK_ON_PRESSES    2     // ... or this many presses during the peek
// Presses during the current peek (the press that woke it doesn't count), and
// whether the current press started during the peek.
static int s_peek_presses;
static bool s_peek_hold;
#define DIM_LEVEL            26                 // brightness cap while dimmed (10 %)
#define LOW_AUTO_OFF_MS      (11 * 60 * 1000)   // low battery: auto-off at most this long after the last event
#define AUTO_WAKE_TO_WIN_MS  (5 * 60 * 1000 + 5000)  // a countdown in auto-off turns it on here, for the last 5 min
#define AUTO_FLASH_EVERY_MS  20000              // auto-off: the displays show for a moment, for anyone looking
#define AUTO_FLASH_MS        1500  // won: the ring blinks on-off-on-off-on in it (AUTO_WON_BLINK_MS each)
#define AUTO_WON_BLINK_MS    300
#define OFF_BATT_CHECK_MS    (60 * 60 * 1000)
#define OFF_RECHECK_MS       60000              // off: a low reading is confirmed once before acting
#define DEAD_CHECK_MS        (60u * 60 * 1000)            // dead (deep sleep): watching for overdischarge
#define LOW_DEEP_CHECK_MS    (6u * 60 * 60 * 1000)    // low (deep sleep): the same, less often
#define DEAD_USB_CHECK_MS    (60u * 1000)            // dead on USB: a reading a minute (charging out, or not)
#define DEAD_FIRST_MS        (15u * 1000)            // just out of overdischarge: dead's first reading, the
                                                          // body diode charging meanwhile (PWR_EN still off)
// Overdischarge never turns PWR_EN on, so its only reading is VSYS with PWR_EN off: with USB that's the
// charger's output, a body diode (about 0.6-0.7 V) above the cell while it charges.
#define OVER_EXIT_MV         3900   // at least this: the cell is about 3.2 V or more: on to dead
#define OVER_SOON_MV         3700   // at least this: charging, not there yet: look again in OVER_SOON_MS
#define OVER_SOON_MS         (60u * 1000)
#define OVER_RETRY_MS        (15u * 60 * 1000)       // otherwise, or a USB that came and went: the only wake
#define OFF_BLE_MAX_MS       (8ULL * 24 * 3600 * 1000)    // off with BLE lasts this long at most
#define BATT_WARN_EVERY_MS   30000              // low battery while on: the battery shows briefly, with a beep
#define BATT_WARN_MS         900
#define BATT_WARN_QUIET_MS   10000              // ... but not within this long of any other sound (a capture,
                                                // the fanfare): it would read as part of it. It waits, and
                                                // goes only if the battery is still low by then.

// Battery levels, from the estimate below, on USB as on battery: a USB pin reading high isn't proof of
// charging (a weak supply can read high and charge nothing). Overdischarge is a state, not a reading: once
// in it, nothing turns PWR_EN on, so nothing reads the cell; only over_wake's rule leaves it.
//   normal          everything.
//   low      < 3.5  on works as usual, with warnings, and auto-off after 11 min idle at most. Off: no
//                   BLE (deep sleep); a press peeks and can turn it on.
//   dead     < 3.3  (0 %) out of regulation: no radio, speaker or LEDs. On (or auto-off) goes dark at
//                   once, keeping the game (clocks run on the LP timer) and checking the cell hourly. A
//                   press shows "bAtt Lo" at the lowest brightness and sleeps again. Only USB (or a new
//                   cell) brings it back: on USB it comes back in auto-off (or off).
//   overdischarge < 3.1  the cell is unprotected, so this is the only cut-out: deep sleep with only
//                   USB as a wake source, no timer. The clocks stop.
typedef enum { BATT_NORMAL, BATT_LOW, BATT_DEAD, BATT_OVER } batt_level_t;
static const char *const BATT_LEVEL_NAME[] = {"normal", "low", "dead", "overdischarge"};
#define BATT_LOW_MV        3500
#define BATT_DEAD_MV       3300
#define BATT_OVER_MV       3100
#define BATT_HYST_MV       50    // a level is left upwards only this far above its threshold (see batt_level)
#define BATT_SAG_OVER_MV   3000  // the 1 min average of the loaded reading this low counts as overdischarge-near: dead
#define BATT_FALL_NV_PER_MS 500  // the OCV estimate falls at most 30 mV a minute (see batt_loaded)...
#define BATT_FAST_GAP_MV   150   // ...unless the 1 min loaded average is more than this far below it,
#define BATT_FAST_NV_PER_MS 1667 // then at up to 100 mV a minute, but only down to that distance above the average
#define BATT_TAU_MS        60000

static pwr_t s_pwr = PWR_ON;
static bool s_periph_on;
static uint32_t s_last_activity;

// What the user last chose, saved in flash: any reset (USB, a crash, the cell swapped) comes back
// to it, and a peek goes back to it. Only a press, a hold or a page command changes it; nothing
// else turns the button on, and nothing but a press or command takes it out of shipping.
// (Auto-off isn't a choice: it's still "on". A dead battery doesn't change it either.)
typedef enum { INTENT_ON, INTENT_OFF, INTENT_SHIP } intent_t;
static const char *const INTENT_NAME[] = {"on", "off", "shipping"};
static intent_t s_intent;

static void intent_set(intent_t i)
{
    if (i == s_intent) return;
    ESP_LOGI(TAG, "power choice: %s -> %s", INTENT_NAME[s_intent], INTENT_NAME[i]);
    s_intent = i;
    config_save_power((uint8_t)i);
}

// RTC RAM (kept through deep sleep and every reset but a power-on; check words catch that).
// When it was turned off, on the LP timer; and which deep sleep it's in.
typedef enum { DEEP_SHIP, DEEP_LOW, DEEP_DEAD, DEEP_OVER } deep_t;
static const char *const DEEP_NAME[] = {"shipping", "low battery", "dead battery", "overdischarge"};
static RTC_NOINIT_ATTR uint64_t s_off_since_rtc;
static RTC_NOINIT_ATTR uint32_t s_off_since_check;
static RTC_NOINIT_ATTR uint32_t s_deep_kind, s_deep_check;
#define OFF_SINCE_CHECK 0x0FF51ACEu
#define DEEP_CHECK 0xDEE95EEFu
static void off_since_mark(void)
{
    s_off_since_rtc = esp_rtc_get_time_us();
    s_off_since_check = (uint32_t)s_off_since_rtc ^ OFF_SINCE_CHECK;
}
static bool long_off(void)
{
    if (s_off_since_check != ((uint32_t)s_off_since_rtc ^ OFF_SINCE_CHECK)) off_since_mark();
    return esp_rtc_get_time_us() - s_off_since_rtc > OFF_BLE_MAX_MS * 1000;
}
// Deep-sleep wakes since the last restart (they aren't restarts), and the last one's cause and time: to
// see what's waking it, on the page once it's back on. WAKE_*: what woke it.
enum { WAKE_OTHER, WAKE_TIMER, WAKE_LID, WAKE_USB, WAKE_EXT1_NONE };
static RTC_NOINIT_ATTR uint32_t s_wakes, s_wake_cause, s_wake_check;
static RTC_NOINIT_ATTR uint64_t s_wake_rtc_us;
#define WAKE_CHECK 0x3A4E5EEDu
static void wake_record(bool restart, int cause)
{
    if (restart || s_wake_check != (s_wakes ^ s_wake_cause ^ WAKE_CHECK)) s_wakes = s_wake_cause = 0;
    if (!restart) {
        s_wakes++;
        s_wake_cause = (uint32_t)cause;
        s_wake_rtc_us = esp_rtc_get_time_us();
    }
    s_wake_check = s_wakes ^ s_wake_cause ^ WAKE_CHECK;
}
// Every start (a reset of any kind, or a deep-sleep wake), in RTC RAM, for the page (the wake log
// characteristic): what started it, what it found, what it did. The last WAKE_LOG_N; only a power-on
// clears it, so a run of wakes or brown-outs that nobody could watch is still there once it's back on.
#define WAKE_LOG_N 20
typedef struct {
    uint32_t ms;        // LP timer at the start (ms)
    uint8_t reset;      // esp_reset_reason()
    uint8_t cause;      // esp_sleep_get_wakeup_cause()
    uint8_t pins;       // the ext1 pins that woke it (bit n = IOn)
    uint8_t from;       // the deep sleep it was in (deep_t, 0xF = none) | the saved power choice << 4
    uint8_t went;       // WENT_*, | 0x80 once the radio started
    uint8_t mv10;       // the last rest reading: (mV - 2500) / 10, 0 = none
    uint16_t awake_ms;  // since the start (after the boot loader), at the last update
} wake_log_t;
enum { WENT_RUNNING = 0, WENT_DEEP = 1, WENT_BACK = 5, WENT_PEEK = 9, WENT_ON, WENT_OFF, WENT_AUTO };  // DEEP, BACK: + deep_t
static RTC_NOINIT_ATTR wake_log_t s_wlog[WAKE_LOG_N];
static RTC_NOINIT_ATTR uint32_t s_wlog_head, s_wlog_count, s_wlog_check;
#define WLOG_CHECK 0x7A6E1060u
static wake_log_t *s_wlog_cur;
static void wake_log_start(uint8_t reset, uint8_t cause, uint8_t pins, uint8_t from)
{
    if (reset == ESP_RST_POWERON || s_wlog_check != (s_wlog_head ^ s_wlog_count ^ WLOG_CHECK) ||
        s_wlog_head >= WAKE_LOG_N || s_wlog_count > WAKE_LOG_N)
        s_wlog_head = s_wlog_count = 0;
    s_wlog_cur = &s_wlog[s_wlog_head];
    *s_wlog_cur = (wake_log_t){.ms = (uint32_t)(esp_rtc_get_time_us() / 1000), .reset = reset, .cause = cause, .pins = pins, .from = from};
    s_wlog_head = (s_wlog_head + 1) % WAKE_LOG_N;
    if (s_wlog_count < WAKE_LOG_N) s_wlog_count++;
    s_wlog_check = s_wlog_head ^ s_wlog_count ^ WLOG_CHECK;
}
static void wake_log_went(int went)
{
    if (!s_wlog_cur) return;
    s_wlog_cur->went = (uint8_t)((s_wlog_cur->went & 0x80) | went);
    int64_t ms = esp_timer_get_time() / 1000;
    s_wlog_cur->awake_ms = ms > 65535 ? 65535 : (uint16_t)ms;
}
static void wake_log_batt(int mv)
{
    if (s_wlog_cur && mv > 2500) s_wlog_cur->mv10 = mv >= 5050 ? 255 : (uint8_t)((mv - 2500) / 10);
}
// The characteristic's value: version 1, count, entry size, 0, the LP timer now (u32 ms), then the
// entries, oldest first. (Read on the host task; an entry being updated meanwhile is at worst a moment old.)
static size_t wake_log_read(uint8_t *buf, size_t max)
{
    if (max < 8) return 0;
    uint32_t n = s_wlog_count <= WAKE_LOG_N ? s_wlog_count : 0, now = (uint32_t)(esp_rtc_get_time_us() / 1000);
    if (8 + n * sizeof(wake_log_t) > max) n = (max - 8) / sizeof(wake_log_t);
    buf[0] = 1;
    buf[1] = (uint8_t)n;
    buf[2] = sizeof(wake_log_t);
    buf[3] = 0;
    memcpy(buf + 4, &now, 4);
    for (uint32_t i = 0; i < n; i++)
        memcpy(buf + 8 + i * sizeof(wake_log_t), &s_wlog[(s_wlog_head + WAKE_LOG_N - n + i) % WAKE_LOG_N], sizeof(wake_log_t));
    return 8 + n * sizeof(wake_log_t);
}
static int deep_kind_saved(void) { return s_deep_check == (s_deep_kind ^ DEEP_CHECK) && s_deep_kind <= DEEP_OVER ? (int)s_deep_kind : -1; }

static uint32_t s_next_batt_check, s_next_batt_warn, s_next_flash, s_flash_until;
static int s_low_readings;
static uint32_t s_last_sound;  // while on: the last loop pass with a sound playing
static bool s_dim, s_sparse;
static esp_pm_lock_handle_t s_usb_lock;
static bool s_usb_lock_held;

static void startup_test(void);
static uint16_t awake_permille(void);
static void game_save(void);
static void game_save_flash(void);
static void game_flash_clear(void);
static uint8_t s_last_frame_valid;

static bool s_usb;  // USB power, from the VBUS pin's digital level (updated every loop pass)
static bool s_radio;  // BLE and the network started (see radio_start)
static bool on_usb(void) { return s_usb; }

// The battery estimate: an estimate of the cell's open-circuit voltage, held as a peak of the readings.
// (While charging the terminal sits above the OCV, so then it reads high; discharging, it's the nearest
// thing to the OCV there is.) A rest reading (nothing powered) is taken as it is. A loaded reading above
// the estimate pushes it up at once: the moment the load lets go (between a sound's notes, say) is the
// one nearest the OCV. Below, it pulls it down by at most 30 mV a minute: within that of the highest
// reading of the last minute or so, which a moment with the LEDs, speaker and radio all quiet keeps near
// the OCV, while a real fall (a dying or cooling cell) shows within minutes. A small cell can fall faster
// still (a 200 mAh cell at 200 mA: 100 mV a minute or more at the knee below 3.6 V), so when the one-minute loaded average
// is more than BATT_FAST_GAP_MV below the estimate it falls at up to 100 mV a minute, but never to less
// than BATT_FAST_GAP_MV above the average. The average sits below the OCV by the load's droop, so as long
// as that droop (a loud sound included) is under BATT_FAST_GAP_MV, the fast fall can't take the estimate
// below the OCV: a fanfare can start it, but it stops at the OCV, and the next recovered reading holds it.
// The average also backs it up near the bottom (below BATT_SAG_OVER_MV counts as dead), and the
// brown-out detector watches the loaded 3.3 V rail.
// VSYS is the cell's terminal whenever QBAT1 is on (PWR_EN high), on USB too: then it's the cell charging,
// a little above its open-circuit voltage, and counts like any loaded reading. (Only with QBAT1 off and USB
// present is VSYS the charger's output, and nothing reads it then: a rest reading turns PWR_EN on first.)
// USB appearing or going starts the estimate again, from the next reading.
static int64_t s_batt_nv;  // the OCV estimate in nV (0 = none yet; 30 mV a minute is 500 nV a ms)
static int s_batt_mv;  // the same in mV
static int s_batt_avg_uv;  // 1 min average of the loaded readings, in uV (it moves a few uV a loop pass)
static int s_batt_avg_mv;  // the same in mV (0 = none)
static uint32_t s_batt_at;
static void batt_rest(int mv, uint32_t now)
{
    wake_log_batt(mv);
    s_batt_nv = mv * 1000000LL;
    s_batt_mv = mv;
    s_batt_avg_uv = s_batt_avg_mv = 0;
    s_batt_at = now;
}
static void batt_loaded(int mv, uint32_t now)
{
    uint32_t dt = now - s_batt_at;
    s_batt_at = now;
    s_batt_avg_uv = s_batt_avg_uv ? s_batt_avg_uv + (int)(((int64_t)(mv * 1000 - s_batt_avg_uv) * (int64_t)(dt > BATT_TAU_MS ? BATT_TAU_MS : dt)) / BATT_TAU_MS) : mv * 1000;
    s_batt_avg_mv = s_batt_avg_uv / 1000;
    int64_t nv = mv * 1000000LL;
    if (!s_batt_nv || nv >= s_batt_nv) {
        s_batt_nv = nv;
    } else {
        int64_t t = dt > 60000 ? 60000 : dt;
        int64_t fall = t * BATT_FALL_NV_PER_MS;
        int64_t floor_nv = ((int64_t)s_batt_avg_uv + BATT_FAST_GAP_MV * 1000) * 1000;  // the fast fall's floor
        if (s_batt_avg_uv && s_batt_nv > floor_nv) {
            int64_t fast = t * BATT_FAST_NV_PER_MS;
            if (fast > s_batt_nv - floor_nv) fast = s_batt_nv - floor_nv;
            if (fast > fall) fall = fast;
        }
        int64_t gap = s_batt_nv - nv;
        s_batt_nv -= fall < gap ? fall : gap;
    }
    s_batt_mv = (int)(s_batt_nv / 1000000);
}
static void batt_forget(void) { s_batt_nv = 0; s_batt_mv = s_batt_avg_uv = s_batt_avg_mv = 0; }
static batt_level_t level_at(int mv, int avg)
{
    if (mv < BATT_OVER_MV) return BATT_OVER;
    if (mv < BATT_DEAD_MV || (avg && avg < BATT_SAG_OVER_MV)) return BATT_DEAD;
    if (mv < BATT_LOW_MV) return BATT_LOW;
    return BATT_NORMAL;
}
// With hysteresis: getting worse is at once, getting better needs BATT_HYST_MV over the threshold. The
// estimate jumps up on any loaded reading above it and falls back (at 3 V a minute, as it was by mistake), so a cell sitting
// at a threshold crossed it both ways every minute or two (the page's "low" came and went). With no
// reading yet (after a start, or USB appearing or going) it stays where it was.
static batt_level_t s_cell_lvl;  // the cell's level, on USB too (charge_hold)
static batt_level_t batt_level(void)
{
    if (s_batt_mv) {
        batt_level_t l = level_at(s_batt_mv, s_batt_avg_mv);
        if (l < s_cell_lvl) {
            batt_level_t up = level_at(s_batt_mv - BATT_HYST_MV, s_batt_avg_mv ? s_batt_avg_mv - BATT_HYST_MV : 0);
            l = up < s_cell_lvl ? up : s_cell_lvl;
        }
        s_cell_lvl = l;
    }
    return s_cell_lvl;
}
// For the battery % and the page: the estimate (the latest reading until there is one).
static int batt_now_mv(void) { return s_batt_mv ? s_batt_mv : s_vsys_mv; }

static void activity(uint32_t now) { s_last_activity = now; }
static uint32_t idle_ms(uint32_t now)
{
    // (Activity stamped a moment after this loop's "now" must count as no idle time, not as 49 days of it.)
    int32_t since = (int32_t)(now - s_last_activity);
    return since > 0 ? (uint32_t)since : 0;
}
// A countdown running on this button (it decides the game, so it keeps the button in it).
static bool countdown_running(void)
{
    return s_running && s_owner >= 0 && s_winner < 0 && has_clock(s_owner) && !s_cfg.count_up;
}

// USB power alone (a power bank) shouldn't keep the chip awake; a USB host (the serial console,
// flashing) does. IDF holds its own no-light-sleep lock for as long as the host is sending USB
// frames (CONFIG_USJ_NO_AUTO_LS_ON_CONNECTION). But light sleep takes the USB pad down, so a host
// only gets to enumerate if the chip stays awake for a moment first. While on (or peeking) this
// lock gives it USB_PLUG_MS after a boot, after turning on with USB there, and after USB power
// appears; a BOOT press gives USB_BOOT_PRESS_MS. While off (or auto-off) it never holds.
#define USB_PLUG_MS       10000
#define USB_BOOT_PRESS_MS (5 * 60 * 1000)
static uint32_t s_usb_enum_until;
static bool s_usb_seen;
static bool usb_lock_mode(void) { return s_pwr == PWR_ON || s_pwr == PWR_PEEK; }

static void usb_enum_window(uint32_t now, uint32_t ms)
{
    if ((int32_t)(s_usb_enum_until - (now + ms)) >= 0) return;  // a longer window is already open
    s_usb_enum_until = now + ms;
    ESP_LOGI(TAG, "USB: staying awake %lu s for a host to connect", (unsigned long)(ms / 1000));
}

static void usb_lock_update(uint32_t now)
{
    if (!s_usb_lock) return;
    if (on_usb() && !s_usb_seen && usb_lock_mode()) usb_enum_window(now, USB_PLUG_MS);
    s_usb_seen = on_usb();
    bool want = usb_lock_mode() && on_usb() && (int32_t)(s_usb_enum_until - now) > 0;
    if (want == s_usb_lock_held) return;
    s_usb_lock_held = want;
    if (want) esp_pm_lock_acquire(s_usb_lock);
    else esp_pm_lock_release(s_usb_lock);
}

// PWR_EN also closes QBAT1, the charger's (TP4057 on VSYS) only full path into the cell: with it open the
// cell charges only through QBAT1's body diode, to a diode drop under the charger. So with USB's digital
// level high it's held on, in every state and through deep sleep, except in overdischarge (never PWR_EN
// there: the diode charges the cell regardless).
static bool s_dead_first;  // this start came out of overdischarge: the first dead sleep keeps PWR_EN off
static bool charge_hold(void) { batt_level(); return on_usb() && s_cell_lvl != BATT_OVER && !s_dead_first; }
static void pwr_en_apply(void) { gpio_set_level(PIN_PWR_EN, s_periph_on || charge_hold()); }

static void periph_power(bool on)
{
    if (on == s_periph_on) return;
    if (on) {
        gpio_set_level(PIN_PWR_EN, 1);
        vTaskDelay(pdMS_TO_TICKS(30));  // rails settle; GN1650 power-up
        display_init();
        s_last_frame_valid = 0;
        s_periph_on = true;
        audio_set_allowed(batt_level() < BATT_DEAD);
    } else {
        audio_set_allowed(false);
        uint8_t blank[4] = {0};
        display_set(0, blank, 0);
        display_set(1, blank, 0);
        rgb_t frame[LED_COUNT] = {0};
        leds_show(frame, LED_COUNT);  // matters on USB, where VSYS stays up
        vTaskDelay(pdMS_TO_TICKS(5));
        s_periph_on = false;
        pwr_en_apply();  // (stays on while charging)
    }
}

// Unloaded battery reading while dark: power VSYS just long enough to read it.
static int read_batt_rest(void)
{
    if (s_periph_on) return sense_vsys_mv();
    gpio_set_level(PIN_PWR_EN, 1);
    vTaskDelay(pdMS_TO_TICKS(50));  // divider filter is ~5 ms; the LED caps charge too
    int mv = sense_vsys_mv();
    pwr_en_apply();
    return mv;
}
static void batt_check_rest(uint32_t now)
{
    batt_rest(read_batt_rest(), now);
}

// Deep sleep (~10 uA) with everything unpowered. The game state goes to RTC RAM (kept, with the LP
// timer for the time) and to flash (for a power-on). Wakes:
//   shipping, low  USB appearing or going, a lid press (and for low, a timer: overdischarge watch)
//   dead           the same, the timer hourly; a press only shows "bAtt Lo"
//   overdischarge  USB appearing only: no timer, no press (the pull-ups are off too)
// BOOT (IO9) isn't an LP pin, so it can't wake deep sleep.
static void sleep_now(deep_t kind, bool back);
static void deep_sleep(deep_t kind)
{
    if (kind == DEEP_OVER) s_running = false;  // nothing keeps time any more
    game_save();
    game_save_flash();
    sleep_now(kind, false);
}
// The same without saving the game: for a wake that goes straight back, before the game is restored
// (the saved copy, and its LP-timer time, are still right).
static uint32_t s_over_timer_ms;  // the next overdischarge sleep's only wake (0: by rule, see sleep_now)
static void sleep_now(deep_t kind, bool back)
{
    if (kind == DEEP_OVER) s_cell_lvl = BATT_OVER;  // (before anything below looks at charge_hold)
    s_deep_kind = kind;
    s_deep_check = kind ^ DEEP_CHECK;
    bool press_wake = kind != DEEP_OVER;
    ESP_LOGW(TAG, "deep sleep: %s (battery %d mV%s)", DEEP_NAME[kind], s_batt_mv, on_usb() ? ", USB" : "");
    wake_log_went((back ? WENT_BACK : WENT_DEEP) + kind);
    if (s_radio) ble_stop();  // IDF: host and controller down before sleeping
    periph_power(false);
    for (int i = 0; i < 500 && press_wake && any_face_raw(); i++) vTaskDelay(pdMS_TO_TICKS(10));
    // Clear every wake source first. Auto light sleep arms a timer wake before each nap and leaves
    // it armed: deep sleep inherited it and woke a second or two later. The lid switches and BOOT
    // are light-sleep GPIO wakes, which also apply in deep sleep. (Nothing below yields, so no
    // light sleep can re-arm the timer before esp_deep_sleep_start.)
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    const gpio_num_t light_wake[4] = {PIN_FACE1, PIN_FACE2, PIN_FACE3, PIN_BOOT};
    for (int i = 0; i < 4; i++) gpio_wakeup_disable(light_wake[i]);
    gpio_wakeup_disable(PIN_VBUS_SENSE);
    // 0.6 x VBUS, its digital level: wake on a change (ext1 levels are per pin on the C6). A wake that finds
    // the opposite level again goes back with the opposite armed (see app_main). Overdischarge: see below.
    // Every LP pad (IO0..IO7) is set for the sleep and held (app_main releases them at every start):
    //   lid switches IO2, IO6, IO7 (to GND, no external pulls): as a wake source, inputs with the LP
    //     pull-up; in overdischarge (USB only) isolated, so a held lid draws no pull-up current
    //   pedals IO0, IO1 (no external pulls, never a wake source), VSYS sense IO3 (divider from the switched
    //     VSYS): isolated
    //   PWR_EN IO5: driven high and held while charging (charge_hold), else isolated (its 100k pull-down
    //     holds it low), input, output and pulls all off
    //   VBUS sense IO4: input with no pulls, its 100k/150k divider sets the level
    // (IO8's 10k pull-up is external and it isn't an LP pad.)
    const gpio_num_t iso[3] = {PIN_PEDAL1, PIN_PEDAL2, PIN_VSYS_SENSE};
    for (int i = 0; i < 3; i++) rtc_gpio_isolate(iso[i]);
    if (charge_hold()) {  // charging: PWR_EN held high (USB going wakes it, and then it's let go)
        rtc_gpio_init(PIN_PWR_EN);
        rtc_gpio_set_direction(PIN_PWR_EN, RTC_GPIO_MODE_OUTPUT_ONLY);
        rtc_gpio_set_level(PIN_PWR_EN, 1);
        rtc_gpio_hold_en(PIN_PWR_EN);
    } else {
        rtc_gpio_isolate(PIN_PWR_EN);
    }
    rtc_gpio_init(PIN_VBUS_SENSE);
    rtc_gpio_set_direction(PIN_VBUS_SENSE, RTC_GPIO_MODE_INPUT_ONLY);
    rtc_gpio_pullup_dis(PIN_VBUS_SENSE);
    rtc_gpio_pulldown_dis(PIN_VBUS_SENSE);
    rtc_gpio_hold_en(PIN_VBUS_SENSE);
    const gpio_num_t face[3] = {PIN_FACE1, PIN_FACE2, PIN_FACE3};
    uint64_t mask = 0;
    for (int i = 0; i < 3; i++) {
        if (press_wake) {
            rtc_gpio_init(face[i]);
            rtc_gpio_set_direction(face[i], RTC_GPIO_MODE_INPUT_ONLY);
            rtc_gpio_pullup_en(face[i]);
            rtc_gpio_pulldown_dis(face[i]);
            rtc_gpio_hold_en(face[i]);
            mask |= 1ULL << face[i];
        } else {
            rtc_gpio_isolate(face[i]);
        }
    }
    if (mask) esp_sleep_enable_ext1_wakeup_io(mask, ESP_EXT1_WAKEUP_ANY_LOW);
    if (kind == DEEP_OVER) {
        // One wake only. With USB: the timer (a supply that reads high but can't charge can't keep waking
        // it). Without: USB appearing, or the timer after a USB that came and went (a flickering supply).
        uint32_t t = s_over_timer_ms ? s_over_timer_ms : on_usb() ? OVER_RETRY_MS : 0;
        if (t) esp_sleep_enable_timer_wakeup((uint64_t)t * 1000);
        else esp_sleep_enable_ext1_wakeup_io(1ULL << PIN_VBUS_SENSE, ESP_EXT1_WAKEUP_ANY_HIGH);
    } else {
        esp_sleep_enable_ext1_wakeup_io(1ULL << PIN_VBUS_SENSE, on_usb() ? ESP_EXT1_WAKEUP_ANY_LOW : ESP_EXT1_WAKEUP_ANY_HIGH);
        if (kind == DEEP_DEAD)
            esp_sleep_enable_timer_wakeup((uint64_t)(s_dead_first ? DEAD_FIRST_MS : on_usb() ? DEAD_USB_CHECK_MS : DEAD_CHECK_MS) * 1000);
        if (kind == DEEP_LOW) esp_sleep_enable_timer_wakeup((uint64_t)LOW_DEEP_CHECK_MS * 1000);
    }
    esp_deep_sleep_start();
}

// "bAtt" / "Lo" for 3 s at the lowest brightness, quietly, then deep sleep.
static void show_low_and_sleep(deep_t kind)
{
    periph_power(true);
    uint8_t a[4], b[4];
    display_text(a, "bAtt");
    display_text(b, "  Lo");
    display_set(0, a, 1);
    display_set(1, b, 1);
    vTaskDelay(pdMS_TO_TICKS(3000));
    deep_sleep(kind);
}

// The deep sleep for the battery as it is now, if it wants one (on USB too). Off: from low down.
// On or auto-off: from dead down.
static int batt_deep(bool off)
{
    batt_level_t l = batt_level();
    if (l == BATT_OVER) return DEEP_OVER;
    if (l == BATT_DEAD) return DEEP_DEAD;  // (a press there shows "bAtt Lo", never a peek)
    if (off && (l == BATT_LOW || (!on_usb() && long_off()))) return DEEP_LOW;
    return -1;
}

static void power_set(pwr_t to, uint32_t now)
{
    if (to == s_pwr) return;
    pwr_t from = s_pwr;
    // Dead (0 %): it won't turn on, only peek.
    if ((to == PWR_ON || to == PWR_AUTO) && batt_level() >= BATT_DEAD) {
        ESP_LOGW(TAG, "battery %d mV: too low to turn on", s_batt_mv);
        if (from == PWR_PEEK) s_batt_until = now + BATT_SHOW_MS;
        return;
    }
    ESP_LOGI(TAG, "power %s -> %s", PWR_NAME[from], PWR_NAME[to]);
    if (to == PWR_ON) intent_set(INTENT_ON);
    if (to == PWR_ON || to == PWR_AUTO) game_flash_clear();  // the live state is the truth again
    if (to == PWR_OFF && (from == PWR_ON || from == PWR_AUTO)) {
        intent_set(INTENT_OFF);
        off_since_mark();
    }
    if (to == PWR_OFF || to == PWR_AUTO) {
        if (s_periph_on && to == PWR_OFF && from != PWR_AUTO) {
            AUDIO_PLAY(SLOT_EVENT, SND_OFF);
            vTaskDelay(pdMS_TO_TICKS(550));
        }
        periph_power(false);
        s_batt_until = s_ident_until = 0;
        s_next_batt_check = now + 10000;  // off: first check soon, then hourly (auto-off reads it at each flash)
        s_low_readings = 0;
        if (to == PWR_AUTO) {
            ble_set_slow(false);  // still in the network, as when on
            mesh_set_off(false);
            s_next_flash = now + AUTO_FLASH_EVERY_MS;
            s_flash_until = 0;
        } else {
            ble_set_slow(true);
            mesh_set_off(true);
            if (s_cfg.mode != MODE_TEAMS) s_running = false;  // clocks pause while off
            game_save();
            game_save_flash();
        }
    } else {
        periph_power(true);
        audio_set_allowed(batt_level() < BATT_DEAD);  // (already powered mid auto-off flash, with the speaker held off)
        s_flash_until = 0;
        ble_set_slow(false);
        mesh_set_off(false);
        s_vsys_mv = sense_vsys_mv();
        batt_loaded(s_vsys_mv, now);
        if (to == PWR_PEEK) {
            s_peek_presses = 0;
            s_peek_hold = false;
            s_batt_until = now + BATT_SHOW_MS;
            AUDIO_PLAY(SLOT_EVENT, SND_BATT);
        } else {
            if (from != PWR_AUTO) startup_test();  // back from auto-off: as if it had stayed on
            if (on_usb()) usb_enum_window(now, USB_PLUG_MS);
            activity(now);
            s_next_batt_warn = now;
        }
    }
    s_pwr = to;
    wake_log_went(to == PWR_ON ? WENT_ON : to == PWR_PEEK ? WENT_PEEK : to == PWR_OFF ? WENT_OFF : WENT_AUTO);
}

static uint32_t auto_off_ms(void)
{
    uint32_t ms = s_cfg.auto_off_min ? s_cfg.auto_off_min * 60000u : 0;
    if (batt_level() == BATT_LOW && (!ms || ms > LOW_AUTO_OFF_MS)) ms = LOW_AUTO_OFF_MS;
    return ms;
}

// Called every loop pass while on or peeking (after the ADC is read).
static void power_tick_on(uint32_t now)
{
    if (s_periph_on) batt_loaded(s_vsys_mv, now);
    if (s_pwr == PWR_PEEK) {
        // Hold to turn on (a second press also does, in the main loop);
        // otherwise drop back once the battery display ends.
        if (s_peek_hold && s_face.state && input_held_ms(&s_face) >= POWER_ON_HOLD_MS) {
            power_set(PWR_ON, now);
            return;
        }
        if (s_face.state || audio_pcm_playing() || (int32_t)(s_ident_until - now) > 0) s_batt_until = now + 1000;
        if ((int32_t)(now - s_batt_until) >= 0) {
            // Back to what it was left in: off, or deep sleep for shipping (or off with a low cell).
            power_set(PWR_OFF, now);
            batt_check_rest(now);
            if (s_intent == INTENT_SHIP) deep_sleep(DEEP_SHIP);
            int d = batt_deep(true);
            if (d >= 0) deep_sleep((deep_t)d);
        }
        return;
    }

    uint32_t idle = idle_ms(now);
    s_sparse = s_cfg.sparse_min && idle >= s_cfg.sparse_min * 60000u;
    s_dim = s_cfg.dim_min && idle >= s_cfg.dim_min * 60000u;
    int d = batt_deep(false);
    if (d >= 0) {
        ESP_LOGW(TAG, "battery %d mV (%s): going dark", s_batt_mv, BATT_LEVEL_NAME[batt_level()]);
        show_low_and_sleep((deep_t)d);
    }
    uint32_t ao = auto_off_ms();
    if (ao && idle >= ao && !(countdown_running() && to_win_ms(s_owner) <= AUTO_WAKE_TO_WIN_MS)) {
        ESP_LOGI(TAG, "idle for %lu min: auto-off", (unsigned long)(idle / 60000));
        power_set(PWR_AUTO, now);
        return;
    }
    if (audio_busy()) s_last_sound = now;
    if (batt_level() == BATT_LOW && (int32_t)(now - s_next_batt_warn) >= 0 && now - s_last_sound >= BATT_WARN_QUIET_MS) {
        s_next_batt_warn = now + BATT_WARN_EVERY_MS;
        s_batt_until = now + BATT_WARN_MS;
        AUDIO_PLAY(SLOT_EVENT, SND_BATT);
        ESP_LOGW(TAG, "battery low: %d mV (estimate), %d mV (1 min average)", s_batt_mv, s_batt_avg_mv);
    }
}

// Called every loop pass in auto-off.
static void power_tick_auto(uint32_t now)
{
    // (Only a win that happens in auto-off: a game already over when it went dark mustn't bring
    // it straight back, which it did, every auto-off time.)
    bool won = s_winner >= 0 && (int32_t)(s_win_alarm_until - now) > 0;  // just won (not a game already over)
    if (won || (countdown_running() && to_win_ms(s_owner) <= AUTO_WAKE_TO_WIN_MS)) {
        ESP_LOGI(TAG, "auto-off: %s, back on", won ? "a win" : "the last 5 min of the countdown");
        power_set(PWR_ON, now);
        if (won) s_win_next_sound = now;  // the fanfare was due while the speaker was off: now
        return;
    }
    // The ring and displays, for a moment, so the state can be read from a distance (at the set
    // brightness: idle dimming and the sparse ring don't apply to the flash). The battery is read
    // as it starts, with VSYS just powered and nothing lit yet: close to a rest reading, for free.
    if (s_flash_until && (int32_t)(now - s_flash_until) >= 0) {
        s_flash_until = 0;
        periph_power(false);
    } else if (!s_flash_until && (int32_t)(now - s_next_flash) >= 0) {
        s_next_flash = now + AUTO_FLASH_EVERY_MS;
        periph_power(true);
        audio_set_allowed(false);
        s_flash_until = now + 30 + AUTO_FLASH_MS;  // (30 ms of that is the rails starting)
        if (!s_flash_until) s_flash_until = 1;
        batt_loaded(sense_vsys_mv(), now);
        int d = batt_deep(false);  // (none on USB)
        if (d >= 0) {
            ESP_LOGW(TAG, "auto-off: battery %d mV (%s)", s_batt_mv, BATT_LEVEL_NAME[batt_level()]);
            show_low_and_sleep((deep_t)d);
        }
    }
    uint32_t so = s_cfg.switch_off_min * 60000u;
    if (s_cfg.switch_off_min && idle_ms(now) >= so && !countdown_running()) {
        ESP_LOGI(TAG, "idle for %lu min: off", (unsigned long)(idle_ms(now) / 60000));
        power_set(PWR_OFF, now);
    }
}

// Called on every loop pass while off (the loop only runs on events, and for this check).
static void power_tick_off(uint32_t now)
{
    if ((int32_t)(now - s_next_batt_check) < 0) return;
    batt_check_rest(now);
    int d = batt_deep(true);
    ESP_LOGI(TAG, "off: battery %d mV (%s)", s_batt_mv, BATT_LEVEL_NAME[batt_level()]);
    if (d < 0) {
        s_low_readings = 0;
        s_next_batt_check = now + OFF_BATT_CHECK_MS;
        return;
    }
    if (++s_low_readings < 2 && d != DEEP_OVER) {
        s_next_batt_check = now + OFF_RECHECK_MS;
        return;
    }
    deep_sleep((deep_t)d);
}

// Face switches (and BOOT) wake the CPU from light sleep, and the interrupt wakes the
// main loop at once (it otherwise sleeps 20 ms when on, 1 s when off). The
// interrupt is level-triggered (needed for light-sleep wake), so it disables
// itself and the loop re-arms it once the switch is open again.
static TaskHandle_t s_main_task;
static const gpio_num_t WAKE_PINS[4] = {PIN_FACE1, PIN_FACE2, PIN_FACE3, PIN_BOOT};

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
    for (int i = 0; i < 4; i++) {
        gpio_wakeup_enable(WAKE_PINS[i], GPIO_INTR_LOW_LEVEL);
        gpio_isr_handler_add(WAKE_PINS[i], face_isr, (void *)(intptr_t)WAKE_PINS[i]);
        gpio_intr_enable(WAKE_PINS[i]);
    }
    // VBUS sense too, on either edge (armed for the level it isn't at): USB going while off must reach
    // the loop at once, not at its next hourly pass, or PWR_EN (held for charging) stays on on the cell.
    gpio_isr_handler_add(PIN_VBUS_SENSE, face_isr, (void *)(intptr_t)PIN_VBUS_SENSE);
    esp_sleep_enable_gpio_wakeup();
}

static void wake_rearm(void)
{
    for (int i = 0; i < 4; i++)
        if (gpio_get_level(WAKE_PINS[i])) gpio_intr_enable(WAKE_PINS[i]);
    gpio_wakeup_enable(PIN_VBUS_SENSE, gpio_get_level(PIN_VBUS_SENSE) ? GPIO_INTR_LOW_LEVEL : GPIO_INTR_HIGH_LEVEL);
    gpio_intr_enable(PIN_VBUS_SENSE);
}

static uint8_t state_flags(uint32_t now)
{
    return (uint8_t)((s_running ? 1 : 0) | (s_winner >= 0 ? 2 : 0) | (s_game_paused ? 4 : 0) |
                     (s_face.state ? 8 : 0) | ((int32_t)(s_ident_until - now) > 0 ? 16 : 0) |
                     (on_usb() ? 32 : 0) | (audio_pcm_playing() ? 64 : 0));
}
static uint8_t state_flags2(void)
{
    return (uint8_t)((s_dim ? 1 : 0) | (s_sparse ? 4 : 0) |
                     (batt_level() >= BATT_LOW ? 2 : 0));
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
        .reset_reason = (uint8_t)s_restart_reason,
        .boots = s_boot_count,
        .power = (uint8_t)s_pwr,
        .flags2 = state_flags2(),
        .batt_mv = (uint16_t)batt_now_mv(),
        .vsys_mv = (uint16_t)s_vsys_mv,
        .vbus_mv = (uint16_t)s_vbus_mv,
        .batt_pct = (int16_t)batt_percent(batt_now_mv()),
        .connections = (uint8_t)phone_links(),
        .uptime_s = since_restart_s(),
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
            .batt_pct = (int16_t)batt_percent(batt_now_mv()),
            .batt_mv = (uint16_t)batt_now_mv(),
            .phones = (uint8_t)phones,
            .reset_reason = (uint8_t)s_restart_reason,
            .uptime_s = since_restart_s(),
            .boots = s_boot_count,
            .awake_pm = awake_permille(),
        };
        int raw, pin;
        sense_vsys_debug(&raw, &pin);
        s.vsys_adc = (uint16_t)(raw > 0 ? raw : 0);
        s.vsys_pin_mv = (uint16_t)(pin > 0 ? pin : 0);
        // Tail: u16 deep-sleep wakes since the last restart, u8 the last one's WAKE_*, u8 0, u32 seconds since it.
        uint8_t tail[8] = {0};
        uint32_t ago = s_wakes ? (uint32_t)((esp_rtc_get_time_us() - s_wake_rtc_us) / 1000000u) : 0;
        uint16_t w = s_wakes > 65535 ? 65535 : (uint16_t)s_wakes;
        memcpy(tail, &w, 2);
        tail[2] = (uint8_t)s_wake_cause;
        memcpy(tail + 4, &ago, 4);
        mesh_publish_status(&s, tail, sizeof tail);
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
    // Auto-off: any command (from the page or the network) is back on first, as if it had stayed on.
    if (s_pwr == PWR_AUTO) power_set(PWR_ON, now);
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
            if (o >= 0) play_team(o);
            ESP_LOGI(TAG, "owner set to %d from the page", o + 1);
        }
        break;
    case OP_PLAY_SOUND:
        if (n >= 2) play_sound_id(d[1]);
        break;
    case OP_STOP_SOUND:
        stop_event();
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
        intent_set(INTENT_OFF);  // (also out of shipping, from a shipping peek)
        break;
    case OP_FACTORY_OFF:
        // A moment first, so the network passes the command on (and hears this button's last state).
        s_factory_off_at = now + 1500;
        if (!s_factory_off_at) s_factory_off_at = 1;
        ESP_LOGW(TAG, "factory off requested");
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
                if (s_pwr == PWR_AUTO) power_set(PWR_ON, now);
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

// Brightness from the setting (the page's slider, 0..255) and idle dimming.
// LEDs: logarithmic, 2 % at the bottom of the slider (never fully off) to 100 % at the top, since
// the eye sees ratios. Dimmed means "at most 10 %".
// Displays (GN1650, steps 1..8): 1/8 at the bottom, full from 30 % of the slider up, so they keep
// up with the LEDs, which outshine them at the same setting.
#define LED_MIN_FRAC 0.02f
static int led_curve(int setting)
{
    int v = (int)(255.0f * LED_MIN_FRAC * powf(1.0f / LED_MIN_FRAC, setting / 255.0f) + 0.5f);
    return v < 1 ? 1 : v > 255 ? 255 : v;
}
static int led_level(void)
{
    int v = led_curve(s_cfg.led_level);
    return s_dim && s_pwr != PWR_AUTO && v > DIM_LEVEL ? DIM_LEVEL : v;
}
static int disp_max(void)
{
    int m = 1 + (s_cfg.led_level * 7 + 38) / 77;  // 77 = 30 % of 255
    if (m > DISP_FULL) m = DISP_FULL;
    return s_dim && s_pwr != PWR_AUTO && m > 2 ? 2 : m;
}
static int disp_level(int bright)
{
    if (bright <= 0) return 0;
    int b = (bright * disp_max() + DISP_FULL - 1) / DISP_FULL;
    if (b < 1) b = 1;
    // 1/8 is the GN1650's lowest step: keep highlighted displays one step above
    // dim ones so the difference survives a low brightness setting.
    if (bright > DISP_DIM && b < 2) b = 2;
    return b;
}

// ---------------------------------------------------------------- rendering
static bool capture_dark(void);
static bool batt_showing(uint32_t now) { return (int32_t)(s_batt_until - now) > 0; }
static bool identifying(uint32_t now) { return (int32_t)(s_ident_until - now) > 0; }

static void render_clock(uint8_t seg[4], int32_t ms, bool round_up)
{
    int s = round_up ? (ms + 999) / 1000 : ms / 1000;
    if (s > 99 * 60 + 59) {
        // Over 99:59, hours and minutes: "9h:59" while it fits, then "12:34" (99:59 at most).
        // (Counting down, the minute shown is the one under way, as the seconds are below this.)
        int m = round_up ? (s + 59) / 60 : s / 60;
        if (m > 99 * 60 + 59) m = 99 * 60 + 59;
        int h = m / 60;
        m %= 60;
        seg[0] = display_digit(h < 10 ? h : h / 10);
        seg[1] = h < 10 ? display_char('h') : display_digit(h % 10);
        seg[2] = display_digit(m / 10);
        seg[3] = display_digit(m % 10);
        return;
    }
    int m = s / 60;
    s %= 60;
    seg[0] = m >= 10 ? display_digit(m / 10) : 0;
    seg[1] = display_digit(m % 10);
    seg[2] = display_digit(s / 10);
    seg[3] = display_digit(s % 10);
}

// A won game in auto-off's flash: the ring blinks on-off-on-off-on (from the moment it lights, after
// the rails' 30 ms), so it reads as "already won" from a distance; the displays stay lit throughout.
static bool auto_won_ring_on(uint32_t now)
{
    int32_t left = (int32_t)(s_flash_until - now);
    int32_t in = AUTO_FLASH_MS - left;
    if (in < 0) in = 0;
    return ((in / AUTO_WON_BLINK_MS) & 1) == 0;
}

static void render_displays(uint32_t now)
{
    for (int t = 0; t < 2; t++) {
        uint8_t seg[4] = {0};
        int bright = 0;
        bool lid_off = s_pwr == PWR_ON && s_face.state && s_hold_stage == HS_RESET &&
                       input_held_ms(&s_face) >= hold_off_warn_ms();
        if (s_boot_off_showing || lid_off) {
            display_text(seg, t == 0 ? "HoLd" : " oFF");
            bright = DISP_FULL;
        } else if (identifying(now)) {
            memset(seg, 0xFF, sizeof seg);
            bright = ((now / 125) & 1) ? DISP_FULL : 0;
        } else if (batt_showing(now)) {
            if (t == 0) {
                display_text(seg, "bAtt");
            } else {
                int pct = batt_percent(batt_now_mv());
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
                    bool lit = s_pwr == PWR_AUTO || ((now / 250) & 1);  // (auto-off's flash: lit throughout)
                    bright = src == s_winner ? (lit ? DISP_FULL : 0) : DISP_DIM;
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
        if (capture_dark()) memset(seg, 0, sizeof seg);
        display_set(t, seg, identifying(now) ? bright : disp_level(bright));
    }
}

// Hold fills: the first LED is lit from the start, and the other five come on one after another, each
// at the end of its fifth of the hold, so the 6th reaches full at the end. Each one waits at `dim`
// (0 = off), then rises in a short smooth fade (FILL_RAMP_MS, or the whole fifth if that's shorter),
// even to the eye: a clear step around the ring, not a slow creep. Levels are 0..255 of the overall
// brightness.
//   The capture fill keeps its unfilled LEDs dim, so the whole ring shows the team. Dim is the lowest
//   brightness the setting allows (FILL_DIM_ABS, the same 2 % duty, which still holds the colour), and
//   while a capture builds the overall brightness is at least HOLD_MIN_LEVEL, 8 times that, so lit and
//   unlit are clearly apart even at the lowest setting (they look about 3 times apart). It goes back
//   to the setting as soon as it's captured.
#define FILL_RAMP_MS    200
#define FILL_DIM_ABS    5    // 2 % duty: led_curve(0)
#define HOLD_MIN_LEVEL  40   // 16 % duty
static void ring_fill(rgb_t ring[6], rgb_t c, int level, uint32_t done, uint32_t span, int dim)
{
    float slot = span / 5.0f, ramp = slot < FILL_RAMP_MS ? slot : FILL_RAMP_MS;
    float d = dim > 0 ? powf(dim / (float)level, 1.0f / 2.2f) : 0.0f;  // dim, in perceived terms
    for (int i = 0; i < 6; i++) {
        float t = i == 0 ? 1.0f : ((float)done - (slot * i - ramp)) / ramp;
        t = t < 0 ? 0 : t > 1 ? 1 : t;
        int v = (int)(level * powf(d + (1.0f - d) * t, 2.2f) + 0.5f);
        ring[i] = v > 0 ? rgb_scale(c, v) : (rgb_t){0};
    }
}
// The capture fill is showing (a press building to a capture): it gets the brightness floor.
static bool capture_building(void)
{
    return s_face.state && s_pwr == PWR_ON && s_hold_stage == HS_BUILD && s_winner < 0 && !s_hold_gp;
}
// The moment of a capture: everything dark for CAPTURE_DARK_MS (then the new owner, as normal, while
// the call plays; then the count towards the pause).
static bool capture_dark(void)
{
    return s_face.state && s_pwr == PWR_ON && s_hold_stage == HS_CAPTURED && s_hold_shift &&
           input_held_ms(&s_face) - hold_capture_ms() < CAPTURE_DARK_MS;
}
static int capture_level(void)
{
    int l = led_level();
    return l < HOLD_MIN_LEVEL ? HOLD_MIN_LEVEL : l;
}

static void render_leds(uint32_t now)
{
    rgb_t frame[LED_COUNT] = {0};
    rgb_t ring[6] = {0};  // clockwise from north

    // The team LEDs: medium, but while a capture builds as bright as the ring's lit LEDs.
    int centre = capture_building() ? 255 : CENTRE_LEVEL;
    frame[LED_IDX_TEAM1] = rgb_scale(team_colour(0), centre);
    frame[LED_IDX_TEAM2] = rgb_scale(team_colour(1), centre);

    // Base: owner colour.
    if (s_winner >= 0) {
        if (s_pwr == PWR_AUTO ? auto_won_ring_on(now) : (now / 250) & 1)
            for (int i = 0; i < 6; i++) ring[i] = rgb_scale(team_colour(s_winner), RING_LEVEL);
    } else if (s_owner >= 0 && s_running) {
        for (int i = 0; i < 6; i++) ring[i] = rgb_scale(team_colour(s_owner), RING_LEVEL);
    } else if (s_owner >= 0) {
        // Paused with an owner: faint slow blink, only noticeable up close.
        if (((now / 500) & 1) || s_pwr == PWR_AUTO)  // (auto-off's flash: lit throughout)
            for (int i = 0; i < 6; i++) ring[i] = rgb_scale(team_colour(s_owner), PAUSED_LEVEL);
    } else {
        // Uncaptured: slow (0.5 Hz) faint pale green pulse. No team colour, so it can
        // never be mistaken for claimed.
        float ph = (float)(now % 2000) / 2000.0f;
        int level = s_pwr == PWR_AUTO ? UNCAPTURED_LEVEL : (int)(UNCAPTURED_LEVEL * 0.5f * (1.0f - cosf(2.0f * (float)M_PI * ph)) + 0.5f);
        for (int i = 0; i < 6; i++) ring[i] = rgb_scale(UNCAPTURED_COLOUR, level);
    }

    // Hold progress.
    if (s_face.state) {
        uint32_t held = input_held_ms(&s_face);
        uint32_t h1 = hold_capture_ms(), h2 = hold_pause_ms(), h3 = hold_reset_ms();
        // (The fills other than the capture keep their unfilled LEDs off: they're not for players.)
        if (s_winner >= 0 && s_hold_stage < HS_RESET) {
            ring_fill(ring, RED, 160, held, h3, 0);
        } else if (s_hold_gp && s_hold_stage <= HS_CAPTURED) {
            // Game paused: a slow white fill towards the release, never a team colour.
            ring_fill(ring, WHITE, 70, held, h2, 0);
        } else if (s_hold_stage == HS_BUILD) {
            rgb_t c = s_hold_target >= 0 ? team_colour(s_hold_target) : WHITE;
            int cl = capture_level();  // (dim is absolute: relative to the overall level here)
            ring_fill(ring, c, 255, held, h1, (FILL_DIM_ABS * 255 + cl / 2) / cl);
        } else if (s_hold_stage == HS_CAPTURED) {
            uint32_t c0 = hold_count_ms();  // (until then the call plays over the normal display)
            if (held >= c0) ring_fill(ring, WHITE, 70, held - c0, h2 - c0, 0);
        } else if (s_hold_stage == HS_PAUSED) {
            ring_fill(ring, RED, 160, held - h2, h3 - h2, 0);
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
        int n = (batt_percent(batt_now_mv()) * 6 + 50) / 100;
        if (n < 1) n = 1;
        for (int i = 0; i < 6; i++) ring[i] = (i < n) ? rgb_scale(GREEN, 120) : (rgb_t){0};
    }
    // Peek: holding fills the ring white towards power-on.
    if (s_pwr == PWR_PEEK && s_face.state && s_peek_hold) {
        ring_fill(ring, WHITE, 160, input_held_ms(&s_face), POWER_ON_HOLD_MS, 0);
    }
    if (identifying(now)) {
        for (int i = 0; i < 6; i++) ring[i] = ((now / 125) & 1) ? WHITE : (rgb_t){0};
    }

    for (int i = 0; i < 6; i++) frame[RING_CW[i]] = ring[i];
    for (int i = 0; i < LED_COUNT_EXT; i++) frame[LED_IDX_EXT + i] = rgb_scale(ring[i * 6 / LED_COUNT_EXT], EXT_LEVEL);
    // (No per-switch highlight: to players the three switches are one lid, so the ring shows only
    // the hold animation, the same whichever switch is down.)

    // Idle 10 min+: every second ring and strip LED off (LED1/2 stay), still visible, less power.
    if (s_sparse && s_pwr != PWR_AUTO) {
        for (int i = 1; i < 6; i += 2) frame[RING_CW[i]] = (rgb_t){0};
        for (int i = 1; i < LED_COUNT_EXT; i += 2) frame[LED_IDX_EXT + i] = (rgb_t){0};
    }
    if (capture_dark()) memset(frame, 0, sizeof frame);
    int level = identifying(now) ? 255 : led_level();
    if (capture_building()) level = capture_level();
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
    int vraw = 0, vpin = 0;
    sense_vsys_debug(&vraw, &vpin);
    printf("[%7.1fs] %-4s %-7s mode=%d owner=%d T1=%s T2=%s hold=%4lums sw=%d%d%d boot=%d ped=%d%d "
           "vsys=%dmV (adc %d, pin %dmV) vbus=%dmV adc_rebuilds=%d ble=%d%s disp_nak=%lu/%lu heap=%u\n",
           now / 1000.0, PWR_NAME[s_pwr], state_name(), s_cfg.mode, s_owner + 1, t1, t2, (unsigned long)input_held_ms(&s_face),
           s_in[IN_FACE1].state, s_in[IN_FACE2].state, s_in[IN_FACE3].state, s_in[IN_BOOT].state,
           s_in[IN_PEDAL1].state, s_in[IN_PEDAL2].state, s_vsys_mv, vraw, vpin, s_vbus_mv, sense_adc_rebuilds(), ble_connections(),
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
            if (s_pwr == PWR_AUTO) power_set(PWR_ON, now);
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
    // CONFIG_PM_SLP_DISABLE_GPIO floats every pin during auto light sleep, which would let RQEN1
    // pull PWR_EN low and cut VSYS and 3V3_SW at every nap while on. Keep this one driven.
    gpio_sleep_sel_dis(PIN_PWR_EN);
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
        for (int i = 0; i < LED_COUNT; i++) frame[i] = rgb_scale(test[c], led_curve(s_cfg.led_level));
        leds_show(frame, LED_COUNT);
        vTaskDelay(pdMS_TO_TICKS(250));
    }
    s_last_frame_valid = 0;
}

// Light sleep accounting (awake_pm in the network status) and the debug "awake LED": the red
// status LED lit whenever the chip is out of light sleep, so every wake shows as a flash and a
// chip that never sleeps shows as a steady light. Both run in the idle task around each sleep.

static esp_err_t IRAM_ATTR sleep_enter_cb(int64_t expect_us, void *arg)
{
    if (s_awake_led) gpio_ll_set_level(&GPIO, PIN_STAT_LED, 0);
    return ESP_OK;
}

static esp_err_t IRAM_ATTR sleep_exit_cb(int64_t slept_us, void *arg)
{
    s_slept_us += (uint32_t)slept_us;
    if (s_awake_led) gpio_ll_set_level(&GPIO, PIN_STAT_LED, 1);
    return ESP_OK;
}

static void awake_led_set(bool on)
{
    if (on == s_awake_led) return;
    s_awake_led = on;
    gpio_set_level(PIN_STAT_LED, on);  // awake right now
    ESP_LOGI(TAG, "awake LED %s", on ? "on" : "off");
}

// Per mille of the time since the last call spent awake (not in light sleep).
static uint16_t awake_permille(void)
{
    static int64_t last_t;
    static uint32_t last_slept;
    int64_t t = esp_timer_get_time();
    uint32_t slept = s_slept_us;
    int64_t span = t - last_t;
    uint32_t d = slept - last_slept;
    last_t = t;
    last_slept = slept;
    if (span <= 0) return 1000;
    int64_t pm = 1000 - (int64_t)d * 1000 / span;
    return (uint16_t)(pm < 0 ? 0 : pm > 1000 ? 1000 : pm);
}

static void pm_init(void)
{
    esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "usb", &s_usb_lock);
    esp_pm_config_t pm = {.max_freq_mhz = 160, .min_freq_mhz = 40, .light_sleep_enable = true};
    esp_err_t err = esp_pm_configure(&pm);
    if (err != ESP_OK) ESP_LOGW(TAG, "power management: %s", esp_err_to_name(err));
    esp_pm_sleep_cbs_register_config_t cbs = {.enter_cb = sleep_enter_cb, .exit_cb = sleep_exit_cb};
    err = esp_pm_light_sleep_register_cbs(&cbs);
    if (err != ESP_OK) ESP_LOGW(TAG, "light sleep callbacks: %s", esp_err_to_name(err));
    usb_lock_update(now_ms());
}

// ---------------------------------------------------------------- surviving a reset
// The game state is copied to RTC RAM every loop pass (a plain memory write, no flash wear). RTC RAM
// and the LP timer survive deep sleep and every reset but a power-on (a brown-out is a system reset on
// the C6, IDF's setting). So after a deep sleep, a brown-out, a crash or a USB reset the button carries
// on where it was, with the clocks advanced by the time it was away (a clock that ran out meanwhile
// wins). Every shutdown (off, any deep sleep) also writes it to flash, for a power-on (the cell
// swapped): then the time away is unknown, so the clocks come back paused. A reboot asked for from
// the page starts fresh. The checksum catches RAM that didn't survive.
#define SAVED_MAGIC (0x47414D45u ^ (uint32_t)sizeof(saved_game_t))  // changes with the layout
#define RESTORE_MAX_GAP_US (10ULL * 60 * 1000 * 1000)       // after a reset: longer means the timer can't be trusted
#define RESTORE_MAX_DEEP_US (60ULL * 24 * 3600 * 1000000)  // after a deep sleep
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

static uint32_t game_crc(const saved_game_t *g) { return esp_rom_crc32_le(0, (const uint8_t *)g, offsetof(saved_game_t, crc)); }

static void game_save(void)
{
    s_saved = (saved_game_t){
        .magic = SAVED_MAGIC, .mode = s_cfg.mode, .count_up = s_cfg.count_up, .pwr = (uint8_t)s_pwr,
        .running = s_running, .fresh = s_fresh, .game_paused = s_game_paused, .game_was_running = s_game_was_running,
        .owner = (int8_t)s_owner, .winner = (int8_t)s_winner, .next_pick = (int8_t)s_next_pick,
        .val = {s_val[0], s_val[1]}, .rtc_us = esp_rtc_get_time_us(),
    };
    s_saved.crc = game_crc(&s_saved);
}
// (rtc_us zeroed in the flash copy, so an unchanged game isn't written again at every sleep.)
static void game_save_flash(void)
{
    saved_game_t g = s_saved;
    g.rtc_us = 0;
    g.crc = game_crc(&g);
    config_blob_save("game", &g, sizeof g);
}
static void game_flash_clear(void) { config_blob_erase("game"); }

static bool game_fits(const saved_game_t *g)
{
    return g->magic == SAVED_MAGIC && g->crc == game_crc(g) && g->mode == s_cfg.mode && g->count_up == s_cfg.count_up &&
           g->owner >= -1 && g->owner <= 1 && g->winner >= -1 && g->winner <= 1 && g->pwr <= PWR_AUTO;
}
static void game_apply(const saved_game_t *g)
{
    s_running = g->running;
    s_fresh = g->fresh;
    s_game_paused = g->game_paused;
    s_game_was_running = g->game_was_running;
    s_owner = g->owner;
    s_winner = g->winner;
    s_next_pick = g->next_pick;
    s_val[0] = g->val[0];
    s_val[1] = g->val[1];
}

// After game_reset at boot. Returns the power state it was in (on or auto-off; off for the rest)
// when the RTC copy was used after a reset, or -1 (a deep sleep, flash or nothing: boot as usual).
static int game_restore(uint32_t now, bool from_deep, bool new_firmware)
{
    esp_reset_reason_t why = (esp_reset_reason_t)s_reset_reason;
    bool reset = why == ESP_RST_BROWNOUT || why == ESP_RST_PANIC || why == ESP_RST_INT_WDT ||
                 why == ESP_RST_TASK_WDT || why == ESP_RST_WDT || why == ESP_RST_USB;
    if (why == ESP_RST_SW || new_firmware) return -1;  // the page's reboot (or a new image): fresh
    if ((from_deep || reset) && game_fits(&s_saved)) {
        game_apply(&s_saved);
        // The time away, from the LP timer (if it went backwards or jumped, it was reset: skip).
        uint64_t t = esp_rtc_get_time_us();
        uint64_t max = from_deep ? RESTORE_MAX_DEEP_US : RESTORE_MAX_GAP_US;
        uint32_t gap = t > s_saved.rtc_us && t - s_saved.rtc_us < max ? (uint32_t)((t - s_saved.rtc_us) / 1000) : 0;
        if (gap) game_tick(now, gap);  // a clock that ran out meanwhile wins now
        ESP_LOGI(TAG, "game state restored (%s, owner %d, %ld/%ld ms, %lu s away)", state_name(), s_owner + 1,
                 (long)s_val[0], (long)s_val[1], (unsigned long)(gap / 1000));
        if (from_deep) return -1;
        return s_saved.pwr == PWR_ON || s_saved.pwr == PWR_AUTO ? s_saved.pwr : PWR_OFF;  // a peek goes back to off
    }
    saved_game_t g;
    if (config_blob_load("game", &g, sizeof g) && game_fits(&g)) {
        game_apply(&g);
        if (s_cfg.mode != MODE_TEAMS) s_running = false;  // how long it was away is unknown
        ESP_LOGI(TAG, "game state restored from flash (%s, owner %d, %ld/%ld ms), clocks paused", state_name(),
                 s_owner + 1, (long)s_val[0], (long)s_val[1]);
    }
    return -1;
}

// BLE and the network. Not at boot: a start that only peeks or goes back to deep sleep never needs them.
static void radio_start(void)
{
    if (s_radio) return;
    s_radio = true;
    if (s_wlog_cur) s_wlog_cur->went |= 0x80;
    char name[24];
    if (s_cfg.name[0]) snprintf(name, sizeof name, "%s", s_cfg.name);
    else default_name(name, sizeof name);
    ESP_LOGI(TAG, "radio on (%s)", PWR_NAME[s_pwr]);
    ble_init(name);
    ble_publish_config(&s_cfg, sizeof s_cfg);
    mesh_init(on_mesh_cmd);
    mesh_publish_config(&s_cfg, sizeof s_cfg);
}

// A wake from overdischarge (app_main, before anything starts). PWR_EN stays off and nothing else is
// powered: the only reading is VSYS, which with USB is the charger's output. Every path ends in a sleep:
// overdischarge again (at least OVER_SOON_MS, or until USB appears), or dead for DEAD_FIRST_MS.
static void over_wake(bool by_usb)
{
    s_cell_lvl = BATT_OVER;
    if (!on_usb()) {
        // Timer, USB gone: wait for USB to appear. USB appeared and went already: the timer only.
        s_over_timer_ms = by_usb ? OVER_RETRY_MS : 0;
        sleep_now(DEEP_OVER, true);
    }
    int mv = sense_vsys_mv();
    wake_log_batt(mv);
    ESP_LOGW(TAG, "overdischarge, USB: VSYS %d mV with PWR_EN off", mv);
    if (mv >= OVER_EXIT_MV) {
        s_cell_lvl = BATT_DEAD;
        s_dead_first = true;
        sleep_now(DEEP_DEAD, true);
    }
    s_over_timer_ms = mv >= OVER_SOON_MV ? OVER_SOON_MS : OVER_RETRY_MS;
    sleep_now(DEEP_OVER, true);
}

void app_main(void)
{
    s_reset_reason = (uint8_t)esp_reset_reason();
    // RTC RAM that hasn't been through a power-on reset can hold anything (a cell plugged in can
    // come up as a brown-out reset), so the count is only trusted next to its check word.
    bool count_ok = s_boot_check == (s_boot_count ^ s_restart_reason ^ BOOT_CHECK);
    if (s_reset_reason == ESP_RST_POWERON || !count_ok) s_boot_count = 0;
    if (s_reset_reason != ESP_RST_DEEPSLEEP || !count_ok) {
        s_boot_count++;
        s_restart_reason = s_reset_reason;
        s_restart_rtc_us = esp_rtc_get_time_us();
    }
    s_boot_check = s_boot_count ^ s_restart_reason ^ BOOT_CHECK;
    // Any wake from deep sleep (low battery or shipping mode) counts, whatever woke it: one that
    // isn't a lid press or USB must not come up on.
    bool from_deep = s_reset_reason == ESP_RST_DEEPSLEEP;
    if (from_deep)
        ESP_LOGW(TAG, "woke from deep sleep: cause %d, ext1 pins 0x%llx", (int)esp_sleep_get_wakeup_cause(),
                 (unsigned long long)esp_sleep_get_ext1_wakeup_status());
    // The LP pads held through the last deep sleep (sleep_now), back to the digital pins. At every start:
    // a hold also lasts through any reset but a power-on.
    for (int i = 0; i <= 7; i++) {
        rtc_gpio_hold_dis((gpio_num_t)i);
        rtc_gpio_deinit((gpio_num_t)i);
    }
    power_pins_init();
    ESP_LOGI(TAG, "capture button firmware, board b43; reset reason %d%s, boot %lu since power-on", s_reset_reason,
             s_reset_reason == ESP_RST_BROWNOUT ? " (BROWN-OUT)" : from_deep ? " (woke from deep sleep)" : "",
             (unsigned long)s_boot_count);
    config_load(&s_cfg);
    s_intent = (intent_t)config_load_power();
    bool new_firmware = config_firmware_changed();
    awake_led_set(s_cfg.debug & CFG_DEBUG_AWAKE_LED);
    if (s_reset_reason == ESP_RST_BROWNOUT) {
        // Don't brown out again on the startup sound: 50 % volume and brightness
        // until changed (not saved, so the stored settings are untouched).
        if (s_cfg.volume > 50) s_cfg.volume = 50;
        if (s_cfg.led_level > 128) s_cfg.led_level = 128;
        ESP_LOGW(TAG, "brown-out reset: volume and brightness capped at 50 %% for now");
    }
    inputs_init();
    sense_init();
    s_usb = sense_vbus_present();
    {
        // A deep-sleep wake that isn't a lid press or USB (the battery check, or anything unexpected) goes
        // straight back, before the radio, the speaker or the LEDs are started: about a tenth of the
        // energy of a full start, no advertising, and no radio calibration to sag a weak cell. (It used to
        // start BLE first: a fast run of wakes showed as a button that advertised but couldn't be connected
        // to.) Each wake is counted, with its cause, for the page.
        esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
        uint64_t pins = cause == ESP_SLEEP_WAKEUP_EXT1 ? esp_sleep_get_ext1_wakeup_status() : 0;
        bool press = pins & ((1ULL << PIN_FACE1) | (1ULL << PIN_FACE2) | (1ULL << PIN_FACE3));
        bool usb_pin = pins & (1ULL << PIN_VBUS_SENSE);
        wake_record(!from_deep, cause == ESP_SLEEP_WAKEUP_TIMER ? WAKE_TIMER : press ? WAKE_LID : usb_pin ? WAKE_USB
                                : cause == ESP_SLEEP_WAKEUP_EXT1 ? WAKE_EXT1_NONE : WAKE_OTHER);
        int kind = deep_kind_saved();
        wake_log_start(s_reset_reason, (uint8_t)cause, (uint8_t)pins, (uint8_t)((from_deep && kind >= 0 ? kind : 0xF) | s_intent << 4));
        if (from_deep && kind == DEEP_OVER) over_wake(usb_pin);  // (never returns)
        if (from_deep && kind >= 0) {
            // The level it slept at, so the hysteresis carries on (shipping: unknown). Then a reading (PWR_EN
            // is allowed in every level but overdischarge) and the level decides; only a press, or USB
            // appearing, goes on to start anything.
            if (kind == DEEP_DEAD) s_cell_lvl = BATT_DEAD;
            else if (kind == DEEP_LOW) s_cell_lvl = BATT_LOW;
            batt_rest(read_batt_rest(), now_ms());
            batt_level_t l = batt_level();
            bool appeared = usb_pin && on_usb();
            ESP_LOGI(TAG, "%s sleep: battery %d mV (%s), wake %lu", DEEP_NAME[kind], s_batt_mv, BATT_LEVEL_NAME[l], (unsigned long)s_wakes);
            if (l == BATT_OVER) sleep_now(DEEP_OVER, true);
            if (l == BATT_DEAD && !press) sleep_now(DEEP_DEAD, true);  // (dead with a press: "bAtt Lo", below)
            if (!press && !appeared) sleep_now(kind == DEEP_DEAD ? DEEP_LOW : (deep_t)kind, true);
        }
    }
    sounds_init();
    admin_sounds_init();
    gamma_init();
    display_init();
    leds_init();
    audio_init();
    audio_set_volume(s_cfg.volume);
    synth_set_plain_gain(medieval() ? PLAIN_GAIN_MEDIEVAL : 256);
    s_vbus_mv = sense_vbus_mv();
    pm_init();
    wake_init();
    ble_set_notify_task(xTaskGetCurrentTaskHandle());  // BLE events wake the game loop
    ble_set_wakes_reader(wake_log_read);
    // (BLE and the network start in the loop, once it's on, auto-off or off: see radio_start.)

    uint32_t last = now_ms();
    game_reset(last);
    int restored = game_restore(last, from_deep, new_firmware);
    batt_rest(read_batt_rest(), last);

    // Where to come up. Nothing here turns it on except the saved choice being on (or a new
    // firmware image, so a flash shows the startup test). Out of deep sleep, see deep_sleep().
    ESP_LOGI(TAG, "power choice %s%s, battery %d mV (%s)", INTENT_NAME[s_intent], new_firmware ? ", new firmware" : "",
             s_batt_mv, BATT_LEVEL_NAME[batt_level()]);
    s_pwr = PWR_PEEK;  // nothing set up yet, so the first power_set does it all (quietly: peripherals are off)
    if (batt_level() == BATT_OVER) deep_sleep(DEEP_OVER);
    if (from_deep) {
        int kind = deep_kind_saved();
        if (kind < 0) kind = s_intent == INTENT_SHIP ? DEEP_SHIP : s_intent == INTENT_ON ? DEEP_DEAD : DEEP_LOW;
        esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
        uint64_t pins = cause == ESP_SLEEP_WAKEUP_EXT1 ? esp_sleep_get_ext1_wakeup_status() : 0;
        bool press = pins & ((1ULL << PIN_FACE1) | (1ULL << PIN_FACE2) | (1ULL << PIN_FACE3));
        // On USB out of a battery sleep: charging, so back as it was (auto-off, or off after a peek).
        bool plugged = on_usb() && ((pins & (1ULL << PIN_VBUS_SENSE)) || kind != DEEP_SHIP);
        ESP_LOGI(TAG, "%s sleep, woken by %s", DEEP_NAME[kind],
                 press ? "a press" : plugged ? "USB" : cause == ESP_SLEEP_WAKEUP_TIMER ? "the battery check" : "USB going (or nothing)");
        if (batt_level() >= BATT_DEAD) {  // dead (on USB too): no peek, no sound; a press shows "bAtt Lo"
            if (press) show_low_and_sleep(DEEP_DEAD);
            deep_sleep(DEEP_DEAD);
        }
        if (plugged && kind != DEEP_SHIP && s_intent == INTENT_ON) power_set(PWR_AUTO, last);
        else if (plugged) power_set(PWR_PEEK, last);
        else if (press && kind != DEEP_DEAD && batt_level() < BATT_DEAD) power_set(PWR_PEEK, last);
        else if (press) show_low_and_sleep((deep_t)kind);
        else deep_sleep((deep_t)kind);  // the battery check (still not overdischarged), or USB gone
    } else {
        bool coming_on = restored >= 0 ? restored != PWR_OFF : new_firmware || s_intent == INTENT_ON;
        int d = batt_deep(!coming_on);
        if (d >= 0) deep_sleep((deep_t)d);
        if (restored >= 0) power_set((pwr_t)restored, last);  // a crash, brown-out or USB reset: carry on
        else if (new_firmware || s_intent == INTENT_ON) power_set(PWR_ON, last);
        else if (s_intent == INTENT_SHIP) deep_sleep(DEEP_SHIP);
        else power_set(PWR_OFF, last);
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
        // Off with nobody connected, the only timed job is the battery check (hourly): the loop sleeps
        // until then. Lid presses, BOOT and BLE events (a phone connecting, its frames) wake it at once.
        // With a connection it runs at least once a second for the heartbeats, but frames arriving run
        // it anyway and restart that wait, so it mostly runs when the radio has woken the chip already.
        uint32_t period;
        if (s_pwr == PWR_AUTO) {
            // Auto-off: the game and the network run as when on, but nothing is shown: wake for the
            // display flash (and its end), else once a second. Presses and BLE wake it at once.
            int32_t until = (int32_t)((s_flash_until ? s_flash_until : s_next_flash) - t);
            int32_t most = s_flash_until && s_winner >= 0 ? 20 : 1000;  // a won game's flash blinks
            period = busy ? LOOP_MS : until < 1 ? 1 : until > most ? (uint32_t)most : (uint32_t)until;
        } else if (s_pwr != PWR_OFF) period = busy ? LOOP_MS : 20;
        else if (ble_connections() || s_factory_off_at) period = 1000;
        else {
            int32_t until = (int32_t)(s_next_batt_check - t);
            period = until < 1000 ? 1000 : until > (int32_t)OFF_BATT_CHECK_MS ? OFF_BATT_CHECK_MS : (uint32_t)until;
        }
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
            if (e > 0 && i <= IN_FACE3 && s_pwr == PWR_ON && !medieval()) AUDIO_PLAY(SLOT_SW1 + i, SND_SW[i]);
            if (e > 0 && i == IN_BOOT && audio_pcm_playing()) {
                audio_pcm_stop();
                boot_skip_press = s_in[IN_BOOT].press_ms;  // that press only stops the clip
            }
            else if (e > 0 && i == IN_BOOT && s_pwr != PWR_OFF) AUDIO_PLAY(SLOT_DIAG, SND_OFF_WARN);
            if (e > 0 && i == IN_BOOT && on_usb() && s_pwr != PWR_OFF) usb_enum_window(now, USB_BOOT_PRESS_MS);
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
                intent_set(INTENT_OFF);
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
        pwr_t pwr_before = s_pwr;  // a press that turns it on isn't a game press
        int fe = input_update(&s_face, any_face, now);
        if (fe > 0 && s_pwr == PWR_PEEK) {
            s_peek_hold = true;
            if (++s_peek_presses >= PEEK_ON_PRESSES) power_set(PWR_ON, now);  // tapping away: wants it on
        } else if (fe > 0 && s_pwr == PWR_OFF) power_set(PWR_PEEK, now);
        else if (fe > 0 && s_pwr == PWR_AUTO) power_set(PWR_ON, now);  // a tap: straight back, still ticking
        if (s_pwr == PWR_ON && pwr_before == PWR_ON) {
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

        // The radio starts once it's on, auto-off or off (off keeps slow legacy adverts for the page), never
        // for a peek: a wake that peeks and sleeps again (shipping, a low cell) never advertises, and every
        // decision to deep-sleep at a start is made before the first pass.
        if (!s_radio && s_pwr != PWR_PEEK && batt_level() < BATT_DEAD) radio_start();
        if (s_radio) {
            ble_service(now);
            mesh_tick(now);
        }
        if (s_factory_off_at && (int32_t)(now - s_factory_off_at) >= 0) {
            s_factory_off_at = 0;
            if (s_pwr != PWR_OFF) power_set(PWR_OFF, now);  // the off sound, peripherals down
            intent_set(INTENT_SHIP);  // kept through every wake but a press or command that turns it on
            deep_sleep(DEEP_SHIP);
        }
        game_tick(now, dt);
        if (s_radio) net_publish(now);
        if (s_pwr == PWR_ON) countdown_blips();
        // Final minute of a countdown: keep the speaker awake so the blips are on time.
        int32_t tw = s_owner >= 0 ? to_win_ms(s_owner) : -1;
        audio_keep_awake(s_pwr == PWR_ON && s_running && s_winner < 0 && s_cfg.show_numbers && tw >= 0 && tw <= 61000);

        // USB presence is a digital read every pass. The ADC runs only while on (or peeking), and for
        // VBUS while off only when USB is there: off on battery, nothing is converted.
        bool was_usb = s_usb;
        s_usb = sense_vbus_present();
        if (was_usb != s_usb) batt_forget();  // charging lifts the terminal voltage, and it falls when USB goes: start again
        pwr_en_apply();  // charging hold follows USB and the battery level
        // No sound at all in dead or overdischarge: the speaker is the largest load, on a cell that can't take it.
        static bool muted;
        if ((batt_level() >= BATT_DEAD) != muted) {
            muted = !muted;
            if (muted) audio_set_allowed(false);
        }
        if (!s_usb) s_vbus_mv = 0;
        if ((int32_t)(now - next_adc) >= 0) {
            next_adc = now + 250;
            if (s_periph_on) s_vsys_mv = sense_vsys_mv();
            if (s_periph_on || s_usb) s_vbus_mv = sense_vbus_mv();
        }
        usb_lock_update(now);
        if (s_pwr == PWR_OFF) power_tick_off(now);
        else if (s_pwr == PWR_AUTO) power_tick_auto(now);
        else power_tick_on(now);

        if ((int32_t)(now - next_ble) >= 0) {
            next_ble = now + (s_pwr == PWR_OFF || s_pwr == PWR_AUTO ? 1000 : STATE_PUBLISH_MS);
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
            // The capture's dark starts and ends on the next pass on the displays too, not on their 20 Hz grid.
            static bool was_dark;
            if (capture_dark() != was_dark) {
                was_dark = !was_dark;
                next_disp = next_led = now;
            }
            if ((int32_t)(now - next_disp) >= 0) {
                next_disp = now + 50;
                render_displays(now);
            }
        }
        if ((int32_t)(now - next_status) >= 0) {
            next_status = now + (s_pwr == PWR_OFF || s_pwr == PWR_AUTO ? 10000 : 1000);
            status_line(now);
        }
        game_save();
    }
}
