# Capture button: design notes

A capture button for LARP objective points. It's an ESP32-C6 device that sits on top of a pole and works like a chess clock: hold the face to capture the point for your team, and the owning team's clock runs on its own 4-digit display.

`hardware.md` describes the board as fabbed (b43): pins, nets, rails and parts. These notes cover how the device is meant to be used and the ideas that have come up. Some of the concept-stage material predates later board changes.

## Status (Nov 2026)

- **Hardware:** 5 boards (b43) and cases (v43) have been built. They haven't been powered yet, and the WS2812s and battery holders still need hand-fitting.
- **Firmware:** none written yet.

## Terms

- **Button:** the whole device.
- **Lid:** the moving top of the case. Pressing anywhere on it closes one or more of three tact **switches** on the PCB (BTN1–3).
- **BOOT:** an internal button, not reachable from outside the case.
- **Owning team / owner:** the team that currently controls the point. Always "own", never "hold": a hold is a player pressing the lid.

## Use

- **First use:** field games on a cricket pitch at night, under stadium lights. It can also be used in daylight at other LARPs.
- **Players:** they wear gloves and slap the face. From a distance, lights matter most, then sound. The 7-segment displays are for close range.
- **Storage:** units can spend months on a shelf between games.
- **Mounting:** one way up on a pole.

## Gameplay ideas

- **Hold to capture:** hold about 5 s to switch the point to the other team, with progress feedback while held (the LED ring filling, ticks or beeps). With two teams one input is enough, because the owning team has no reason to press.
- **Instant toggle:** an instant-toggle mode is also useful. Expect slaps and bounces.
- **Clocks:** each team has an MM:SS time on its own display, and the owner's clock runs. The colon can flash. The display has no decimal points, only the colon.
- **Face switches:** the three switches are separate circuits, treated together as one press. Separate pins also make a stuck or dead switch detectable.
- **Pedals:** two inputs for possible future use, such as one pedal per team or a remote switch. They're probably rarely used. Sustain pedals come in both normally-open and normally-closed versions.
- **Scoring:** expected to vary between games. Options mentioned: total time per team, points per capture, or time above a threshold.
- **More than two teams:** the button can't tell who pressed it. Colour cycling and RFID/NFC wristbands have been mentioned.

## Lights and sound

- **LEDs:** 8 WS2812s.
  - **Centre:** LED1 and LED2 show the two team colours, seen from above.
  - **Ring:** the 6 ring LEDs show the owner's colour, seen from the side. The ring also suits a hold-progress bar.
  - **Status patterns:** an idea raised was that 4 taps shows the battery level.
  - **With USB connected:** the LEDs stay powered and keep showing their last colour.
- **Sound:** mostly-intelligible voice callouts and real sounds, not just tones.
  - **Volume:** software volume scaling, with full volume still available at a low cell.
  - **Short loud chirps:** these can go above the speaker's continuous rating.
- **Audio storage options raised:**
  - The 8 MB flash, or RAM for short clips.
  - Codecs from IMA ADPCM or QOA (cheapest to decode) up to MP3.
  - Espressif's `esp_audio_codec`, which supports the C6 since v2.0.0. Its published benchmarks are for the S3 only.

## Power and sleep

- **Concept flow:**
  - Any press wakes it from deep sleep for a game.
  - Light sleep between events during a game.
  - Back to deep sleep after a long idle.
- **Deep sleep:** the switched rails are off. The current estimate was about 15 µA; it hasn't been measured.
- **Overdischarge:** the board has no protection IC (the DW01A was dropped). It's a single cell, so the worst case is one fully discharged cell, not reverse-charging a series pack.
  - **Firmware cutoff:**
    - At about 3.3 V on a cell that isn't charging, the button stops running normally. A button press still wakes it long enough to show low battery, then it goes back to sleep.
    - Below about 3.0 V it doesn't respond at all and stays in deep sleep.
  - **Brown-out reset as a backstop:** the ESP's brown-out reset floats PWR_EN, and QBAT1 then isolates everything except the LDO and the ESP. Whether to raise the brown-out level to about 3.0 V is still open. Details and unknowns are in `hardware.md`.
- **Charging and storage:** sitting at 4.2 V long term is bad for the cell. A week or two at full is fine, and useful. For units left in a cupboard, the aim is to drop to about 4.0 V and stop charging even while connected.
  - **What the hardware does by itself:** with QBAT1 off, the cell only charges through QBAT1's body diode, so it stops a diode drop below the charger voltage, away from 4.2 V.
  - **With QBAT1 on:** the cell charges to full.
  - **How the charger, VSYS and PWR_EN interact:** see `hardware.md`.

## Other ideas raised

- **Mesh between units:** over 802.15.4 or ESP-NOW, for shared status or a central scoreboard.
- **BLE admin web interface:** for rules, battery level, scores and resetting clocks.
- **External WS2812 strip down the pole:** on JLEDX1.
