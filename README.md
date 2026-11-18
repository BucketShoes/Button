# Capture button firmware

Firmware for the LARP capture button (ESP32-C6, board b43). See
[docs/design-notes.md](docs/design-notes.md) and [docs/hardware.md](docs/hardware.md).

Built with PlatformIO, framework **ESP-IDF 5.5.1** (pioarduino platform 55.03.31).
Chip options are in `sdkconfig.defaults`. Delete the generated `sdkconfig.button`
after editing it.

```
pio run -t upload
pio device monitor
```

## Current behaviour (bench test, USB only)

- **Clocks:** each team starts at 10:00. The owner's clock counts down, and a team **wins** when its clock reaches 0:00.
- **Face press:** any of the three switches counts (an OR). A press registers on the first contact. A release counts only after 100 ms fully open.
- **Hold:**
  - 0–3 s: the ring spins in the colour of the team about to capture (white if unowned), with a rising tone.
  - At 3 s the point is **captured**: it goes to the other team, or to a random team if unowned. The capturing team's sound plays.
  - Keep holding to 6 s to **pause**: the capture is undone, the clocks stop and the ring goes off. The next 3 s hold gives the point back to the team that owned it before the pause. A white fill and ticks warn as you approach it.
  - Keep holding to 10 s to **reset** both clocks to 10:00. A red fill shows progress.
  - Once a team has won, only the reset does anything.
- **Triple tap:** DISP1 shows VSYS in mV and DISP2 shows VBUS in mV for 4 s, with a green gauge on the ring. On USB, VSYS reads the charger output, not a cell.
- **Displays:** the owner's display is bright with its colon flashing at 4 Hz. The non-owner's is dim.
- **LEDs:**
  - LED1 is purple and LED2 is yellow, always at medium brightness.
  - The ring shows the owner's colour at full brightness.
  - Each switch lights the two ring LEDs nearest it white while it's held.
  - The external strip (16 LEDs assumed) mirrors the ring at half brightness.
- **Sounds:** each face switch has its own blip, and the BOOT button plays a diagnostic sweep.
- **Status LED:** on for 1 s at boot, then a 20 ms blink every second.
- **Serial:** a status line every second on USB Serial/JTAG, plus an event log.
- **Startup self-test:** every segment lights, and the LED chain shows red, green, then blue.

Not used yet: the pedals (only logged), Wi-Fi/BLE/802.15.4, and sleep.
