# Capture button firmware

Firmware for the LARP capture button (ESP32-C6, board b43). See
[docs/design-notes.md](docs/design-notes.md) and [docs/hardware.md](docs/hardware.md).

Built with PlatformIO, framework **ESP-IDF 5.5.1** (pioarduino platform 55.03.31).
Chip options are in `sdkconfig.defaults`. Delete the generated `sdkconfig.button`
after editing it. The flash layout is in `partitions.csv`: a 3 MB app and a 4 MB `audio` partition.

In VS Code, use the PlatformIO tasks: **Upload** for the firmware, and
**Custom > Upload audio** for the speaker-test clip (only needed once, or after
changing the clip). From a terminal, `pio` isn't on PATH, so use the full path:

```
~\.platformio\penv\Scripts\pio.exe run -t upload
~\.platformio\penv\Scripts\pio.exe run -t upload_audio
~\.platformio\penv\Scripts\pio.exe device monitor
```

## Speaker-test clip

`audio/test_audio.bin` is raw 16-bit mono PCM at 24 kHz with a 16-byte header. It's
gitignored, so regenerate it from any MP3:

```
powershell -File tools\transcode.ps1 -Src "music test.mp3" -OutDir <dir> -OutName music.wav
python tools\make_test_audio.py <dir>\music.wav audio\test_audio.bin 45
```

`transcode.ps1` uses Windows' built-in decoder, so no ffmpeg is needed. `make_test_audio.py`
picks the loudest 45 s, high-passes at 250 Hz and compresses the clip for loudness.

## Current behaviour (bench test, USB only)

- **Clocks:** each team starts at 10:00. The owner's clock counts down, and a team **wins** when its clock reaches 0:00.
- **Face press:** any of the three switches counts (an OR), so it doesn't matter which one you hit. A press registers on first contact.
  - **Holds** survive up to 250 ms with every switch open.
  - **Taps** use a separate 40 ms debounce so quick taps stay separate.
- **Hold:**
  - 0–3 s: the ring spins in the colour of the team about to capture (white if random), with a rising tone.
  - At 3 s the point is **captured**: it goes to the other team, or to a random team if unowned. The capturing team's sound plays.
  - 3–6 s: a loud ding each second and a white fill warn that a pause is coming.
  - At 6 s it **pauses**: the capture is undone and the clocks stop. The ring blinks faintly in the paused owner's colour, and the next 3 s hold gives the point back to that team.
  - 6–10 s: a low "dun-dunn" each second and a red fill. At 10 s it **resets** both clocks to 10:00 with an ominous growl.
  - Once a team has won, the win fanfare repeats for 20 s. A 3 s hold silences it, and the 10 s reset still works.
- **Triple tap:** three taps under 400 ms each, with gaps under 500 ms. DISP1 then shows VSYS in mV and DISP2 shows VBUS in mV for 4 s, with a green gauge on the ring. On USB, VSYS reads the charger output, not a cell.
- **Displays:** the owner's display is bright with its colon flashing at 4 Hz. The non-owner's is dim.
- **LEDs:**
  - LED1 is purple and LED2 is yellow, always at medium brightness.
  - The ring shows the owner's colour at full brightness.
  - Each switch lights the two ring LEDs nearest it white while it's held.
  - The external strip (16 LEDs assumed) mirrors the ring at half brightness.
- **Sounds:** full volume. Each face switch has its own blip. BOOT plays a diagnostic sweep, or stops the music clip if it's playing.
- **Speaker test:** the flashed clip plays at power-up. Any face press or BOOT stops it.
- **Status LED:** kept off.
- **Serial:** a status line every second on USB Serial/JTAG, plus an event log that includes each tap and the running tap count.
- **Startup self-test:** every segment lights, and the LED chain shows red, green, then blue.

Not used yet: the pedals (only logged), Wi-Fi/BLE/802.15.4, and sleep.
