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
press **Connect a button**. You can connect several buttons; each gets its own panel. Per button:

- **Controls:** start, pause, reset, identify (displays and ring flash with beeps for 3 s), battery, and power off.
- **Holder:** set the holder (uncaptured / team 1 / team 2), and lock the button so holds do nothing (taps still show the battery).
- **Sounds:** play any of the button's sounds, or stop them.
- **Clocks:** live clocks, ±10 s / ±1 min, or set an exact time, even while running.
- **Settings (saved on the button):**
  - name and mode (teams only / one timer / two timers), plus which team has the clock in one-timer mode;
  - count down or up, and whether the numbers show on the displays;
  - durations, the holder after reset, the first capture (random / team 1 / team 2), the hold time, the team colours, the volume and the LED brightness;
  - "Copy to all" sends the settings to every connected button, keeping each name.
- **All connected buttons** (shown when 2+ are connected): resume, pause, identify, play a sound (for example the whistle) or power off on all of them at once.
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
  - **Up to H:** the ring spins in the colour of the team about to capture (white if random), with a rising whoop. For holds over 4 s, it ticks each second and the whoop covers the last 3 s.
  - **At H, capture:** while running it goes to the other team; while paused it resumes with the same team.
  - **Uncaptured:** 1 tap then hold gives team 1, and 2 taps then hold gives team 2. A plain hold uses the "first capture" setting, which is random by default.
  - **H to H+3 s:** a ding each second, then a **pause** at H+3 s (the capture is undone).
  - **H+3 to H+7 s:** doom each second, then a **reset** at H+7 s.
  - **After a win**, the fanfare repeats for 20 s. Holding dings every second: H silences the fanfare, and H+7 s resets.
- **Idle cues:**
  - **Uncaptured:** the ring pulses faintly (0–10 %, 0.5 Hz) in the next capture's colour, alternating colours if random. That team's display is lit, with the colour steady.
  - **Paused with a holder:** the ring blinks faintly in their colour.
- **After reset:** uncaptured, or held by the team set in "after reset". In teams-only mode that team is simply live. With clocks it waits, paused, for Start or a hold.
- **Triple tap:** battery ("bAtt" plus a percent for 6 s, with a green gauge on the ring). It works in every mode and with the numbers hidden.
- **Power:** see "Power" below.
- **BOOT:**
  - Tap: diagnostic sound.
  - Hold 1.5 s (low beep) and release: **power off**.
  - Hold 5 s: restore the default settings.
- **Volume and LEDs** default to 50 %. Full volume browns out on USB-only power. The brightness setting dims the displays too.
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
  - The amp switches off after 1.5 s of silence, and the PDM output after 20 s. The first sound after that comes about 60 ms late.
  - **Idle dim:** after 5 min with no presses, no page commands and no running clock, the displays and LEDs dim to 25 % of the brightness setting.
  - **Auto off:** after 60 min idle with no phone connected, it powers off. A teams-only button that someone holds counts as running, so it never auto-offs mid-game.
- **Turning it off:**
  - Release BOOT after 1.5–5 s, use the page's Power off, or let it auto-off.
  - Clocks pause; a teams-only holder stays.
- **Off:**
  - PWR_EN is low, so only the ESP is powered, and it light-sleeps.
  - BLE keeps advertising slowly, so the page can connect and power it on, or identify it.
  - It checks the battery hourly, briefly powering VSYS to read it.
- **Peek:**
  - A lid press while off shows the battery for about 6 s.
  - Keep holding (or hold again) for 3 s to turn it on; the ring fills white.
  - Otherwise it goes back to off.
- **Low battery (not on USB):**
  - Below 3.45 V average while on: a battery warning every 5 min.
  - Below 3.3 V: it shows "LO" and goes into deep sleep. A press shows "LO" again, and plugging in USB wakes it.
  - Below 3.0 V: deep sleep, and only USB wakes it.
  - Off for a week and below 3.5 V: deep sleep (BLE off).
- **On USB:** the firmware never light-sleeps, so the serial console keeps working, and the battery levels don't apply.
- **Not measured yet:** the actual currents. BLE sleep keeps the main crystal running (there's no 32 kHz crystal), so "off" will draw more than the ~130 µA bare light-sleep figure.
