# Capture button: hardware as fabbed (board b43)

Every pin, net and part below was read from the pads and nets in `larp-capture-timer b43.kicad_pcb`, the fabbed board; b43 is byte-identical to b38. Part numbers come from the JLC BOM for the order. Values taken from datasheets or from the PCB design discussion, rather than from the board file, are marked as such.

Board status (18 Nov 2026): 5 assembled boards have arrived, not yet powered. The WS2812s and the battery holder are not fitted.

## Orientation

Coordinates are KiCad board coordinates. The board centre is (100, 100), +x is east and +y is **south**. "North" means the top of the board as drawn in KiCad, which is the view from above once the button is mounted on its pole.

- **Antenna:** the module's antenna overhangs the east chord.
- **Displays:** DISP1 is north, centred at (100, 79.2). DISP2 is south, at (100, 120.8). Both are unrotated.
- **Team labels:** the TEAM1 silk is on the north side of centre and the TEAM2 silk on the south side, both at x ≈ 115. From that placement (an inference), team 1 is DISP1/DRV1 and team 2 is DISP2/DRV2.
- **Switches:** BTN1 is west (59.4, 100), BTN2 is south-east (114, 134.6) and BTN3 is north-east (114, 65.4). BOOT1 is inside the case at (91.5, 100).

## ESP32-C6-WROOM-1-N8 pin map

| Module pad | GPIO | Net | Function | Hardware on the net |
|---|---|---|---|---|
| 8 | IO0 | PEDAL1 | Pedal 1, contact to GND | JPED1 pin 1. No external pull-up. |
| 9 | IO1 | PEDAL2 | Pedal 2, contact to GND | JPED2 pin 1. No external pull-up. |
| 27 | IO2 | FACE_BTN1 | Switch W, closes to GND | BTN1. No external pull-up. |
| 26 | IO3 | VBAT_SENSE | VSYS / 2 | RDIV1 100k from VSYS, RDIV2 100k to GND, CDIV1 100 nF |
| 4 | IO4 | VBUS_SENSE | 0.6 × VBUS | RVB1 100k from VBUS, RVB2 150k to GND |
| 5 | IO5 | PWR_EN | High turns on VSYS and +3V3_SW | QEN1 gate, RQEN1 100k pull-down |
| 6 | IO6 | FACE_BTN2 | Switch SE, closes to GND | BTN2. No external pull-up. |
| 7 | IO7 | FACE_BTN3 | Switch NE, closes to GND | BTN3. No external pull-up. |
| 10 | IO8 | IO8 | Spare (strapping pin) | RIO8 10k pull-up to +3V3 |
| 15 | IO9 | IO9_BOOT | BOOT button, closes to GND | BOOT1 |
| 11 | IO10 | LED_DIN_MCU | WS2812 data | RLED1 1k in series to LED1 DIN. RLEDPD1 100k from LED_DIN to GND. |
| 12 | IO11 | AMP_EN | Amp CTRL, high = on | RAMP1 100k pull-down |
| 13 | IO12 | USB_DN | Native USB D− | USBC1, both D− pins. No series resistor. |
| 14 | IO13 | USB_DP | Native USB D+ | USBC1, both D+ pins. No series resistor. |
| 23 | IO15 | LED_XIO | Optional link to the end of the LED chain | JPXOUT1 solder jumper, open as built |
| 16 | IO18 | PDM_OUT | Audio PDM out | RPDM1 1k with CPDM1 10 nF, then CIN1 100 nF to the amp's INP |
| 17 | IO19 | DISP_DIO1 | DRV1 DIO | RDIO1 5.1k pull-up to +3V3_SW |
| 18 | IO20 | DISP_DIO2 | DRV2 DIO | RDIO2 5.1k pull-up to +3V3_SW |
| 19 | IO21 | DISP_CLK1 | DRV1 CLK | RCLK1 5.1k pull-up to +3V3_SW |
| 20 | IO22 | DISP_CLK2 | DRV2 CLK | RCLK2 5.1k pull-up to +3V3_SW |
| 21 | IO23 | STAT_LED | Red status LED, high = on | RSTAT1 1k to LED_STAT1, then GND (about 1.4 mA) |
| 24 | RXD0 (IO17) | RXD0 | UART0 RX | Test pad TP_RX1 only |
| 25 | TXD0 (IO16) | TXD0 | UART0 TX | Test pad TP_TX1 only |
| 3 | EN | EN | Reset | REN1 10k pull-up, CEN1 1 µF. There is no reset button. |
| 22 | — | NC | — | — |

### Related chip facts (from the C6 datasheet; check against the current revision)

- **Deep-sleep wake:** only IO0–IO7 are LP GPIOs that can wake the chip. All the switches, both pedals, VBUS_SENSE and PWR_EN are in that range. BOOT (IO9) and the display lines are not.
- **Inputs float when idle:** the switch and pedal pins have no external pull-ups, so they float unless an internal pull-up is enabled. With an internal pull-up enabled, any closed contact (a held switch, or a normally-closed pedal at rest) draws pull-up current for as long as it stays closed.
- **ADC:** IO3 and IO4 are on ADC1.
- **Strapping pins:**
  - IO8 and IO9 set the boot mode. RIO8 holds IO8 high, so holding BOOT at power-up gives download mode. IO8 and IO9 both low is not a valid boot mode.
  - IO15 selects the JTAG source, but only if an eFuse is set, which it isn't by default.
  - IO4 and IO5 (MTMS/MTDI) set only the SDIO-slave timing.

## Power

```
CELL (18650 in CELL1, or a pouch on JBAT1; silk: don't use both at once)
  └─ reverse-cell protection in the negative lead (QREV1, QREVH1 AO3400A; RREVG1, RREVH1)
VBAT ──┬── LDO1 XC6220B331 (CE tied to VBAT: always on) ── +3V3 ──┬── ESP1
       │                                                          └── QSW1 AO3401A ── +3V3_SW ── DRV1, DRV2, display pull-ups
       └── QBAT1 AO3401A ── VSYS ── 8 × WS2812, JLEDX1, AMP1, VBAT_SENSE divider
PWR_G = QBAT1 and QSW1 gates (RPWRG1 100k to VBAT); QEN1 AO3400A pulls PWR_G low when PWR_EN is high.
USB-C VBUS ── TP4057 (CHRG1) BAT pin = CHG_OUT ── net tie (REF**) ── VSYS
```

- **PWR_EN (IO5):** high turns on VSYS and +3V3_SW; floating or low turns both off. IO5 is an LP pin, so its level can be held through deep sleep.
- **The ESP's supply:** the ESP always runs from the cell through the LDO, including when USB is connected.
- **Charger on VSYS:** the TP4057 output is tied to VSYS by a net tie added during layout. JP1 (pads 1 = VSYS, 2 = CHG_OUT, 3 = VBAT) is open as built.
  - **PWR_EN high:** the cell charges through QBAT1's channel.
  - **PWR_EN low:** the cell charges only through QBAT1's body diode (VSYS to VBAT), so it stops about a diode drop below the charger voltage. The design discussion put that around 0.5 V.
  - **USB present:** the charger powers VSYS whatever PWR_EN is, so the LEDs and amp are live.
  - **JP1 pads 2–3:** bridging them with the net tie in place connects VSYS straight to VBAT, bypassing QBAT1.
- **Charger settings and status:**
  - **Current:** RPROG1 is 2k, which gives 400 mA (design notes).
  - **Termination:** charging stops below about 40 mA and restarts below about 4.05 V; float voltage is 4.2 V (TP4057-42 datasheet, as read during design).
  - **Status pins:** CHRG drives the white CHRG_LED1 from VBUS. STDBY is unconnected. Neither reaches the ESP.
  - **No disable line:** the ESP has no way to switch the charger off.
- **VBAT_SENSE reads VSYS, not the cell:**
  - While PWR_EN is high it shows the cell's terminal voltage (less QBAT1's drop), on battery or charging.
  - With USB present and PWR_EN low, it shows the charger output.
  - With PWR_EN low and no USB, VSYS is unpowered.
- **VBUS_SENSE:** about 3.0 V at 5 V VBUS.
- **Overdischarge:** there is no protection IC on the board; the DW01A was dropped. It's a single cell, so the worst case is one fully discharged cell, not reverse-charging a series pack.
  - **Hardware backstop: the ESP's brown-out reset (BOD).** With BOD enabled, a falling +3V3 holds the ESP in reset. Its pins float, RQEN1 pulls PWR_EN low, and QBAT1 and QSW1 cut VSYS and +3V3_SW. That disconnects everything except the LDO and the ESP itself.
  - **ESP current while held in brown-out reset: unknown.** It's estimated to fall somewhere between deep sleep and CHIP_PU held low, probably below the cell's self-discharge. Unverified.
  - **Brown-out level:** in ESP-IDF it's a compile-time choice (`CONFIG_ESP_BROWNOUT_DET_LVL_SEL`); a runtime change is only possible through a private API. A C6 user on Espressif's forum reports 2.51 V as the menuconfig setting. On the related ESP32-H2, the IDF levels run from 2.51 V to 3.27 V. The C6's own list of levels hasn't been checked.
  - **LDO accuracy:** the XC6220's output is ±1 % with about 60 mV typical dropout, so +3V3 shouldn't drop below about 3.26 V unless the cell is low.
  - **Protected cells:** cells with built-in protection also cut off on their own.
  - **Firmware cutoff:** the planned firmware low-battery behaviour is in `design-notes.md`.
  - **Silk:** the bottom silk reads "Use 18650 cell, 4.2V charge, 2.7v cutout". The LDO's output falls below 3.3 V once the cell gets within its dropout of 3.3 V.
- **Current limit (PCB thermal model):** VSYS load through QBAT1 is about 2 A continuous OK, 2.5 A marginal and 3 A not OK.
- **Sleep current:** the concept estimate was about 15 µA in deep sleep with the rails off. Not measured.

## Displays: 2 × GN1650 (TM1650 clone) driving FJ5462AH 4-digit common-cathode displays

- **Wiring:** DRV1 drives DISP1 (DIO IO19, CLK IO21). DRV2 drives DISP2 (DIO IO20, CLK IO22).
- **Supply:** both drivers and all four 5.1k pull-ups are on +3V3_SW.
  - While that rail is off, any display line the ESP drives high feeds the unpowered driver through its pin.
  - The drivers lose their settings whenever they're unpowered.
- **Segment mapping:** segment and digit nets are wired by name (driver A to display A, through G, DP and DIG1–DIG4), so the board adds no remapping.
  - Segment bits are B0 = A through B6 = G, and B7 = DP.
  - On this display, the DP bits of digits 3 and 4 are the two colon dots. There are no other decimal points.
- **Protocol** (GN1650 datasheet, as worked through earlier): I2C-style framing (start, byte, ACK, stop) with no device address.
  - **Control:** command 0x48, then a control byte: B0 = on, B2 = sleep, B3 = 0 for 8-segment mode (otherwise the DP/colon bit is ignored), B6..B4 = brightness (000 = brightest).
  - **Digits:** 0x68, 0x6A, 0x6C and 0x6E for digits 1–4.
  - **Hardware I2C:** these command bytes go out as the 7-bit "address" (0x48 is a write to 0x24; the digit bytes are writes to 0x34–0x37).
  - **Lines:** DIO is open-drain with the driver pulling it low for ACK. CLK is never stretched.
- **Two drivers, two buses:** both drivers answer the same command bytes, so they can't share one bus.
- **I2C controllers:** the C6 has one general I2C controller that can be routed to any pin. Its LP I2C is fixed to IO6/IO7, which are switches here.
- **Segment current:** no segment resistors; the GN1650 drives the segments directly. Display current is drawn from the LDO.

## LEDs: 8 × WS2812B-B-V6 on VSYS (hand-fitted; JLC wouldn't place them)

**Chain:** IO10 → RLED1 → LED1 → LED2 → … → LED8 → LED_XOUT. LED_XOUT goes to JLEDX1 pin 2 and to one side of JPXOUT1.

| Chain index | Ref | Position |
|---|---|---|
| 0 | LED1 | Centre, just south of DISP1 (106.1, 92.8) |
| 1 | LED2 | Centre, just north of DISP2 (106.1, 107.5) |
| 2 | LED3 | Ring NE (132.9, 81) |
| 3 | LED4 | Ring N (100, 62) |
| 4 | LED5 | Ring NW (67.1, 81) |
| 5 | LED6 | Ring SW (67.1, 119) |
| 6 | LED7 | Ring S (100, 138) |
| 7 | LED8 | Ring SE (132.9, 119) |

- **Ring direction:** indices 2–7 run anticlockwise seen from above.
- **Supply and thresholds:** VSYS is the cell voltage (up to 4.2 V) or the charger output. The data input threshold is 0.55 × VDD. The datasheet characterises the LEDs at 5 V.
- **Data-line parts:**
  - RLED1 limits the current into LED1's input clamp if IO10 is high while VSYS is off.
  - RLEDPD1 holds the data line low while IO10 floats.
- **External strip header:** JLEDX1 pins are 1 = VSYS, 2 = data (from LED8's DOUT), 3 = GND. Silk "+D-".
- **JPXOUT1 (IO15):** open as built, so IO15 is unconnected.
  - **Bridged:** it joins IO15 to LED8's DOUT, with no series resistor.
  - **With LED8 fitted:** LED8's DOUT swings up to VSYS (4.2 V), which is above the ESP's input rating.
  - **Why it's there:** the V6 is described as having swappable data pins, and this link was added for that. Untested.
- **Status LED:** LED_STAT1, driven by IO23, intended mostly for debug.

## Audio

- **Signal path:** IO18 PDM → RPDM1 1k + CPDM1 10 nF (RC at about 16 kHz) → CIN1 100 nF + RIN1 100k → NS4150C (AMP1) INP.
  - INN goes through RIN2 100k and CIN2 100 nF to GND.
  - The amp runs from VSYS; its CTRL pin is AMP_EN (IO11), pulled low by RAMP1.
- **Internal speaker:** SPK1 is a FUET FS-1540: 8 Ω, rated 0.5 W (0.8 W max), resonance around 850 Hz, 87 dB SPL (datasheet). It connects to SPK_P through JPSPK1, which is bridged as built.
- **External speaker:** SPKR1 (pin 1 = SPK_P, pin 2 = SPK_N, DNP pads) is wired in parallel with SPK1.
- **Power headroom:** the NS4150C can deliver about 1.3 W into 8 Ω at 5 V (datasheet), so from a full cell it can exceed the FS-1540's 0.5 W rating. 0.5 W into 8 Ω is about 2.0 V rms.
- **Pop and startup** (datasheet figures, from the design notes):
  - When the PDM stream starts, INP steps to mid-rail, and that step couples through CIN1. RIN1 × CIN1 is a 10 ms time constant.
  - The amp's startup time is about 165 ms at 5 V and 230 ms at 3 V with the 1 µF CBYP.
  - The amp has built-in pop suppression.
- **Underrun silence:** ESP-IDF's I2S TX `auto_clear` option sends zeros on an underrun, which is 50 % PDM, i.e. silence.

## Connectors (all DNP: wires solder to the pads from below)

- **Pedals, JPED1 / JPED2:** silk "1G" / "2G". Pin 1 = PEDAL1/2, pin 2 = GND. Sustain pedals exist in both normally-open and normally-closed versions.
- **Pouch cell, JBAT1:** pin 1 = VBAT, pin 2 = CELL_N. This is the alternative to the 18650 holder.
- **Test pads:** TP_RX1, TP_TX1, TP_GND1, TP_3V3.
- **USB-C:** 5.1k CC pull-downs (sink). Data goes straight to IO12/IO13 (USB Serial/JTAG).

## Differences from the concept design (netlist rev 15)

- **Display pins:** DIO1 moved from IO21 to IO19, DIO2 from IO22 to IO20, CLK1 from IO23 to IO21, and CLK2 from IO15 to IO22. IO19 and IO20 are no longer spare.
- **New pins:** IO23 is STAT_LED; IO15 is LED_XIO, behind the open jumper.
- **Display pull-ups:** 5.1k pull-ups were added on all four display lines, to +3V3_SW.
- **Speaker:** the MLT-8530 buzzer was replaced by the FS-1540 speaker, with JPSPK1 and SPKR1 in parallel. The PDM filter's pull-down (RPDPDM) was removed.
- **Protection:** reverse-cell protection was added.
- **Charging:** the charger now feeds VSYS through a net tie, and JP1 is open.
- **Charge LED:** CHRG_LED1 is white (C2290).
- **LED part:** the WS2812B-B-V6 is LCSC C52941391. It was out of stock at order time, and which equivalent gets fitted isn't recorded here.

## Not yet measured on a real board

- Deep-sleep current.
- Display bus behaviour with the 5.1k pull-ups, and display brightness against LDO temperature at a low cell.
- ext1 per-pin wake levels while PWR_EN is held.
- Audio level and intelligibility from the FS-1540.
- WS2812 behaviour as VSYS sags.
- VBAT_SENSE scaling and offset.
