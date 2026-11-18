# Capture button firmware

Firmware for the LARP capture button (ESP32-C6, board b43). See
[docs/design-notes.md](docs/design-notes.md) and [docs/hardware.md](docs/hardware.md).

Built with PlatformIO, framework **ESP-IDF 5.5.1** (pioarduino platform 55.03.31).
Chip options are in `sdkconfig.defaults`. Delete the generated `sdkconfig.button`
after editing it. The flash layout is in `partitions.csv`: a 3 MB app and a 4 MB `audio` partition.

In VS Code, use the PlatformIO **Upload** task with the port picked in the status bar.
`custom_audio` in `platformio.ini` sets what that Upload does with the audio partition, on the same port:

- `keep`: leave the audio partition alone, for fast uploads.
- `upload`: also write `audio/test_audio.bin` (about 2 MB, so the upload is slower).
- `erase`: also blank the clip header, so nothing plays at power-up.

Set it to `upload` once per board, then back to `keep`. From a terminal, `pio` isn't
on PATH, so use the full path:

```
~\.platformio\penv\Scripts\pio.exe run -t upload --upload-port COM5
~\.platformio\penv\Scripts\pio.exe device monitor --port COM5
```

## Speaker-test clip

`audio/test_audio.bin` is raw 16-bit mono PCM at 24 kHz with a 16-byte header. It's
gitignored, so regenerate it from any MP3:

```
powershell -File tools	ranscode.ps1 -Src "music test.mp3" -OutDir <dir> -OutName music.wav
python tools\make_test_audio.py <dir>\music.wav audio	est_audio.bin --hp 30 --compress light
```

- `transcode.ps1` uses Windows' built-in decoder, so no ffmpeg is needed.
- `make_test_audio.py` picks the loudest 45 s (`--seconds`).
  - `--hp` sets a high-pass in Hz; 0 turns it off.
  - `--compress` is `none`, `light` or `heavy`.
- It also writes **`audio/test_audio.wav`**: the exact samples the ESP plays, so you can listen on a PC.
  - What the board adds after that: the PDM high-pass (~35 Hz), the RC filter (~16 kHz), the amp and the speaker.

## Current behaviour (bench test, USB only)

- **Clocks:** each team starts at 10:00. The owner's clock counts down, and a team **wins** when its clock reaches 0:00.
- **Face press:** any of the three switches counts (an OR), so it doesn't matter which one you hit. A press registers on first contact. The lid counts as pressed until 100 ms after the last contact on any switch, for both holds and taps.
- **Hold:**
  - 0–3 s: the ring spins in the colour of the team about to capture (white if random), with a rising tone.
  - At 3 s the point is **captured**: it goes to the other team, or to a random team if unowned. The capturing team's sound plays.
  - 3–6 s: a loud ding each second and a white fill warn that a pause is coming.
  - At 6 s it **pauses**: the capture is undone and the clocks stop. The ring blinks faintly in the paused owner's colour, and the next 3 s hold gives the point back to that team.
  - 6–10 s: a low "dun-dunn" each second and a red fill. At 10 s it **resets** both clocks to 10:00 with an ominous growl.
  - Once a team has won, the win fanfare repeats for 20 s. A 3 s hold silences it, and the 10 s reset still works.
- **Triple tap:** three taps under 500 ms each, with gaps under 500 ms between them.
  - About 0.5 s after the last tap, DISP1 shows "bAtt" and DISP2 shows the estimated charge percent for 6 s, with a green gauge on the ring.
  - The percent is estimated from voltage: 3.3 V is 0 % and 4.2 V is 100 %, steepest around 3.7 V. It can go below 0 % or above 100 %.
  - On USB, VSYS reads the charger output, not the cell. The serial log shows the raw mV.
- **Displays:** the owner's display is bright with its colon flashing at 4 Hz. The non-owner's is dim.
- **LEDs:**
  - LED1 is purple and LED2 is yellow, always at medium brightness.
  - The ring shows the owner's colour at full brightness.
  - Each switch lights the two ring LEDs nearest it white while it's held.
  - The external strip (16 LEDs assumed) mirrors the ring at half brightness.
- **Countdown:** the owner's clock blips at 60, 30, 20 and 10..1 s left. Nearer zero, the blips get more pulses, higher and faster.
- **Sounds:** full volume. Each face switch has its own blip. BOOT plays a diagnostic sweep, or stops the music clip if it's playing.
- **Speaker test:** the flashed clip plays at power-up. Any face press or BOOT stops it.
- **Status LED:** kept off.
- **Serial:** a status line every second on USB Serial/JTAG, plus an event log that includes each tap and the running tap count.
- **Startup self-test:** every segment lights, and the LED chain shows red, green, then blue.

Not used yet: the pedals (only logged), Wi-Fi/BLE/802.15.4, and sleep.
