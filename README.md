# Capture button firmware

Firmware for the LARP capture button (ESP32-C6, board b43). See
[docs/design-notes.md](docs/design-notes.md) and [docs/hardware.md](docs/hardware.md).

Built with PlatformIO, framework **ESP-IDF 5.5.1** (pioarduino platform 55.03.31).
Chip options are in `sdkconfig.defaults`. Delete the generated `sdkconfig.button`
after editing it. The flash layout is in `partitions.csv`: a 3 MB app and a 4 MB `audio` partition.

In VS Code, use the PlatformIO **Upload** task with the port picked in the status bar.
`custom_audio` in `platformio.ini` sets what that Upload does with the audio partition, on the same port:

- `keep`: leave the audio partition alone, for fast uploads.
- `upload`: also write `audio/test_audio.bin` (about 3.8 MB, so the upload is slower).
- `erase`: also blank the clip header, so nothing plays at power-up.

Set it to `upload` once per board, then back to `keep`. From a terminal, `pio` isn't
on PATH, so use the full path:

```
~\.platformio\penv\Scripts\pio.exe run -t upload --upload-port COM5
~\.platformio\penv\Scripts\pio.exe device monitor --port COM5
```

## Speaker-test clip

`audio/test_audio.bin` is raw 16-bit mono PCM at 48 kHz (the firmware's audio rate) with a 16-byte header. It's
gitignored, so regenerate it from any MP3:

```
powershell -File tools\transcode.ps1 -Src "music test.mp3" -OutDir <dir> -OutName music.wav
python tools\make_test_audio.py <dir>\music.wav audio\test_audio.bin
```

- `transcode.ps1` uses Windows' built-in decoder, so no ffmpeg is needed.
- `make_test_audio.py` takes 40 s from the very start of the song (`--seconds`; `--start <s>`, `--start auto` to skip leading silence, or `--start loudest`). That's about the most that fits in 4 MB at 48 kHz.
  - By default it does no high-pass and no compression, just a level normalise and 10 ms de-click ramps at the ends, so it's a clean speaker test.
  - `--hp` sets a high-pass in Hz; 0 turns it off.
  - `--compress` is `none`, `light` or `heavy`.
- It also writes **`audio/test_audio.wav`**: the exact samples the ESP plays, so you can listen on a PC.
  - What the board adds after that: the PDM high-pass (~35 Hz), the RC filter (~16 kHz), the amp and the speaker.

## Control page (Web Bluetooth)

[docs/index.html](docs/index.html) is published by GitHub Pages at
<https://bucketshoes.github.io/Button/>. Open it in Chrome (Android, or Chrome/Edge on a computer) and
press **Connect a button**. You can connect several buttons.

- **Game bar** (top): pause and resume the whole game, and play a sound (whistle by default) on every button. Game resume undoes the game pause: each button goes back to exactly what it was doing, so a button that was paused or uncaptured stays that way.
- **Overview:** one row per button, sized so about 5 fit on a phone screen. Name and battery on the left, then a box per team (team names once at the top of each column):
  - thick border in the team colour, filled for the holder;
  - underlined time while that clock is ticking, blinking on a win;
  - a small pie fills while someone holds the lid, in the box of the team about to capture;
  - tap a row to jump to that button's panel.
- **Each button** gets its own panel below:

- **Controls:** start, pause, reset, identify (displays and ring flash with beeps for 3 s), battery, and power off.
- **Holder:** set the holder (uncaptured / team 1 / team 2).
- **Reboot** (in settings), for when something is stuck. Settings are kept; the game state is lost.
- **Sounds:** play any of the button's sounds, or stop them.
- **Clocks:** live clocks, −1 min / −10 s, +1 min / +10 s, or set an exact time (m:ss), even while running.
- **Settings (saved on the button):**
  - name and mode (teams only / one timer / two timers), plus which team has the clock in one-timer mode;
  - count down or up, and whether the numbers show on the displays;
  - durations, the holder after reset, the first capture (random / team 1 / team 2), the hold time, the team colours, the volume and the LED brightness;
  - "Copy to all" sends the settings to every connected button, keeping each name.
- **More for all buttons** (under the overview): identify, power on and power off all of them at once.
- **Reboots:** the panel shows how many times the button rebooted since power-on, and why (for example BROWN-OUT).
- **Wall clock** with seconds in the top corner.

The protocol is in [docs/ble-protocol.md](docs/ble-protocol.md).

## Behaviour

- **Modes:**
  - **Teams only:** the ring shows who holds the button. The displays stay off (except battery and identify).
  - **One timer:** only one team's clock runs, while that team holds the button. It shows only on that team's display.
  - **Two timers:** a reverse chess clock; the holder's clock runs.
- **Clocks:** count down (win at 0:00) or up (win at the duration; a duration of 0 means no limit). Default: two timers counting down from 10:00.
- **Face press:** any of the three switches counts (an OR). The lid counts as pressed until 50 ms after the last contact.
- **Hold**, where H is the hold time (default 3 s):
  - **Up to H:** the ring fills one LED at a time in the colour of the team about to capture, with a bright spinning LED on top, and a rising whoop. Letting go early puts it back as it was. The whoop is stretched over the whole hold time, so it sounds the same at 3 s or 20 s, just slower.
  - **At H, capture:** while running it goes to the other team; while paused it resumes with the same team.
  - **Uncaptured:** the team is decided the moment the hold starts and shows straight away: the ring fills in its colour and its display lights. Wrong colour? Let go and press again: every press that doesn't capture flips the team for the next press. The first press uses the "first capture" setting, which is random by default.
  - **H to H+3 s:** a ding each second, then a **pause** at H+3 s. The clocks go back to where they were before the hold, and the paused holder is:
    - **was running:** whoever had it before the hold;
    - **was uncaptured:** the team just picked, so a 6 s hold pre-sets the owner at 10:00 (for example at a respawn);
    - **was paused:** the other team, so each further 6 s hold swaps it.
  - **H+3 to H+7 s:** doom each second, then a **reset** at H+7 s.
  - **After a win**, the fanfare repeats for 20 s. Holding dings every second all the way to the reset (there is no pause to warn about): H silences the fanfare, and H+7 s resets.
- **Idle cues:**
  - **Not held, the button shows only the true state.** A hold let go early snaps straight back, so nobody runs off thinking they captured it.
  - **Uncaptured:** a faint white pulse on the ring (0–10 %, 0.5 Hz), no team colour, and neither display highlighted. The team the next press goes for has one colon dot missing: subtle, for admins.
  - **Paused with a holder:** the ring blinks faintly in their colour, and their display shows a single slow-blinking dot instead of the colon.
- **After reset:** uncaptured, or held by the team set in "after reset". In teams-only mode that team is simply live. With clocks it waits, paused, for Start or a hold.
- **Triple tap:** battery ("bAtt" plus a percent for 6 s, with a green gauge on the ring). It works in every mode and with the numbers hidden.
- **Power:** see "Power" below.
- **BOOT:**
  - Hold: the displays say "HoLd" "oFF" with a falling tone, and at 3 s it **powers off** (no need to let go).
  - Keep holding to 8 s: restore the default settings (it wakes briefly to play the sound).
  - A press while the music clip plays only stops the clip.
- **Volume and LEDs** default to 50 %. Full volume browns out on USB-only power. The brightness setting dims the displays too.
- **Brown-out recovery:** after a brown-out reset, volume and brightness start capped at 30 % (not saved) so the startup sound can't cause another one.
- **Display brightness:** 1/8 is the GN1650's lowest step. At low settings the highlighted display stays one step above the dim one, so you can still tell them apart.
- **Displays:** the running holder's display is bright, with its colon flashing at 4 Hz. The others are dim.
- **Countdown blips:** at 60, 30, 20 and 10..1 s from a win. Skipped when the numbers are hidden.
- **LEDs:**
  - LED1 and LED2 show the team colours at medium brightness.
  - The ring shows the holder.
  - Each switch lights its two nearest ring LEDs white.
  - The external strip (16 assumed) mirrors the ring.
- **Music clip:** no longer plays at power-up; play it from the page. A face press or BOOT stops it.
- **Serial:** a status line every second plus an event log.

Not used yet: the pedals (only logged), storage-charge management on USB, coded PHY or a mesh between buttons, and the event log/timeline. See [docs/plans.md](docs/plans.md).

## Power

- **On:**
  - Everything is powered.
  - Between loop passes (20 ms when idle), the CPU light-sleeps.
  - The amp and PDM output stop 1 s after the last sound, because a running PDM keeps the chip awake. The PDM and amp then start together, which may give a small click. The first sound of an interaction is queued behind exactly 50 ms of silence (`AMP_WAKE_MS` in `src/audio.c`) that plays while the amp starts, so it's a fixed 50 ms late but never cut short. Sounds that start while already awake wait behind up to 30 ms of DMA buffering. Quick follow-ups (taps, holds) aren't delayed. In the last 61 s of a countdown the speaker stays awake.
  - **Idle** means no press and no page command; the game state is kept throughout, and a tap brings it straight back.
    - After 10 min, every second ring and strip LED goes off (LED1 and LED2 stay).
    - After 1 h, brightness drops to 10 % (or stays lower if it's already set lower).
    - After 4 h, it powers off.
- **Turning it off:**
  - Hold BOOT for 3 s, use the page's Power off, or let it auto-off.
  - Clocks pause; a teams-only holder stays.
- **Off:**
  - PWR_EN is low, so only the ESP is powered, and it light-sleeps.
  - BLE keeps advertising slowly, so the page can connect and power it on, or identify it.
  - It checks the battery hourly, briefly powering VSYS to read it.
- **Peek:**
  - A lid press while off shows the battery for about 6 s.
  - Anything that looks like trying to wake it turns it on: press again (tap, or several taps), or keep holding for 1 s; the ring fills white.
  - Otherwise it goes back to off.
- **Low battery (not on USB):**
  - Below 3.45 V average while on: a battery warning every 5 min.
  - Below 3.3 V: it shows "bAtt Lo" and goes into deep sleep. A press shows "bAtt Lo" again (below 3.5 V), and plugging in USB should wake it.
  - Below 3.0 V: deep sleep, and only USB wakes it.
  - While off, BLE only runs until the battery reads below 3.5 V or 8 days have passed; then it goes to deep sleep. A press wakes it, and it peeks if the cell is still at 3.5 V or more.
- **On USB:** the firmware never light-sleeps, so the serial console keeps working, and the battery levels don't apply.
- **Not measured yet:** the actual currents. BLE sleep uses the main crystal as its low-power clock (Espressif's RC-clock option supports legacy adverts and scanning only, not connections). A 32 kHz crystal on IO0/IO1 would allow lower sleep current.
- **Volume:** the slider is in dB (-40 dB to full), so each step sounds about the same.

