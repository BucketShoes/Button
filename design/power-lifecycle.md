# Power lifecycle and battery levels

Notes on how the button moves between power states, what each state keeps running, and what is still planned. The user-facing summary is the README's "Power" section; the code is the power section of `src/main.c`.

## The two things it tracks

- **The power choice** (`intent_t`, saved in NVS as `pwr`): on, off or shipping. Only a press, a hold or a page command changes it. Any reset comes back to it; so does the end of a peek. A new firmware image comes up on once, so a flash shows the startup test.
- **What it's doing** (`pwr_t`, the state byte's `power`): on, auto-off, peek or off, plus the deep sleeps. Auto-off and the battery's deep sleeps don't change the choice. A peek from shipping is still "shipping" until it reaches on.

## States

| State | Peripherals | Radio | Game | Leaves by |
|---|---|---|---|---|
| On | powered | full: network, Coded, phones | runs | idle → auto-off; lid 17 s / BOOT 3 s / page → off; dead battery → dead sleep |
| Auto-off | dark; ring and displays 1.5 s every 20 s (won: the ring blinks on-off-on-off-on; battery read then) | as on | runs | tap or any command → on (no startup test); countdown at 5:05 or a win → on; idle (setting) → off, not while a countdown runs; dead battery → dead sleep |
| Peek | powered (battery shown 6 s) | as on | as it was | 2 more presses or a 1.5 s hold → on; otherwise back to off / shipping / low sleep |
| Off | dark | legacy adverts every 2–2.5 s, phones only | clocks paused (teams owner stays) | lid → peek; page → on; low battery or 8 days → low sleep |
| Shipping (deep) | off | none | saved | lid or USB appearing → peek → shipping again; USB going → sleeps again |
| Low (deep) | off | none | saved | lid → peek (can turn on); USB → peek → off; timer every 6 h (overdischarge watch) |
| Dead (deep) | off | none | saved, clocks run on the LP timer | lid → "bAtt Lo" → sleeps again; USB → auto-off (or peek → off); timer hourly |
| Overdischarge (deep) | off | none | saved, clocks stop | USB appearing only |

Every deep sleep writes the game to RTC RAM and to flash. RTC RAM and the LP timer survive deep sleep and every reset but a power-on, so the clocks advance by the time away. After a power-on (the cell swapped) the flash copy comes back with the clocks paused. The flash copy is cleared whenever it's on or auto-off again, so a later power-on never brings back a stale game.

## Battery levels

The estimate (`s_batt_mv`) aims at the open-circuit voltage:

- A rest reading (VSYS powered for 50 ms with nothing else on) is taken as it is.
- A loaded reading above the estimate pushes it up at once.
- A loaded reading below pulls it down by at most 3 mV a minute.

A one-minute average of loaded readings below 3.0 V also counts as dead, because the estimate can lag a fast fall and the brown-out detector watches the loaded rail. On USB nothing is learnt, and the estimate starts again when USB comes or goes.

| Level | Range | On | Off |
|---|---|---|---|
| Normal | ≥ 3.5 V | as usual | BLE (legacy, slow) |
| Low | < 3.5 V | as usual, battery flash + beep every 30 s, auto-off after 11 min idle at most | no BLE (low sleep) |
| Dead | < 3.3 V (0 %) | goes dark at once into the dead sleep; won't turn on | low sleep; a press shows "bAtt Lo" |
| Overdischarge | < 3.1 V | cut-out: deep sleep, USB only | the same |

The cell is unprotected, so the 3.1 V cut-out is the firmware's job. Deep sleep with everything off plus the LDO should be well under 30 µA, comparable to self-discharge. The RTC slow clock and RTC RAM keep running in every deep sleep (they can't be stopped on the C6); the overdischarge sleep just has no timer armed.

The brown-out detector stays at the IDF default (2.51 V on the 3.3 V rail). It's only a backstop for sag under load. A brown-out reset doesn't save power over deep sleep (the chip keeps restarting as the rail recovers), so a high level would just turn load dips into reboot loops.

Shipping has no timer, as asked: only a press or USB wakes it. That's the same as the overdischarge sleep, except that the lid can wake it. So if shipping drifts below 3.1 V, nothing changes until someone taps it. That wake reads the cell and goes straight into the overdischarge sleep, with no peek. A year in shipping at about 30 µA is about 260 mAh, so a cell put away near empty can still sit below 3.1 V unnoticed until then.

The C6 keeps its RTC slow clock, the LP timer counter and RTC RAM running in every deep sleep. IDF can't power them down on this chip (there's no RTC memory power-down option for it). They're part of the deep-sleep current, not something added on top. Nothing else runs in shipping or overdischarge: no timer wake, no ULP, no peripherals, no radio.

## USB: power and data are separate

- **USB power** is the VBUS rail, read digitally on IO4 (0.6 × VBUS, high above about 4.1 V). It's the only USB input that any power or battery decision uses:
  - the battery levels don't apply while it's there, because VSYS is then the charger;
  - in every deep sleep it's an ext1 wake: appearing in all of them, going in all but overdischarge;
  - in light sleep it's read whenever something else wakes the chip.
  A power bank, a charger and a computer all look the same here.
- **USB data** means a computer has enumerated the button and sends it a frame every millisecond. Only the awake states care about it:
  - **On and peek:** the button holds itself awake for a while, so a computer can enumerate it: 10 s after a boot, after turning on and after USB power appears, and 5 min after a BOOT press.
  - **On, peek, auto-off and off:** while a computer is sending frames, ESP-IDF keeps the chip out of light sleep (`CONFIG_USJ_NO_AUTO_LS_ON_CONNECTION`). In auto-off and off a computer only gets there if it enumerated the button while it was awake, because light sleep takes the USB pad down. Once it has, it keeps the chip awake (not on) for as long as it stays connected, which is harmless on a computer's power.
  - **Deep sleeps:** data counts for nothing. The USB peripheral is off and only the VBUS rail can wake the chip.

## Idle times (settings, config bytes 42–45)

| Setting | Default | Effect |
|---|---|---|
| Half the LEDs | 11 min | every second ring and strip LED off |
| Dim | 31 min | 10 % brightness |
| Auto-off | 61 min | dark, still in the game and network (11 min at most when low) |
| Off | 121 min | auto-off becomes off; not while a countdown runs on this button |

Idle means no press and no BLE command on this button.

## Not done yet

- **Network-wide idle rule:** if every timer the button can see in the network is paused, unstarted or finished, go to off 11 min after the last event (a tick, a local tap, entering auto-off). It needs the button to track the other buttons' hot state, which it relays but doesn't keep. For now each button only looks at its own countdown.
- **Auto-off with a shrinking network** (for multi-day events where the network stops at night):
  - After some idle time in auto-off, leave the network: join no one, but keep advertising and accepting on Coded.
  - Every 3–6 min, drop a link to another button that also reports auto-off. Links to on buttons stay, so the network never splits while someone is still playing.
  - When every button is in auto-off, the links fall to zero, and every button still accepts incoming connections.
  - A tap turns one button on. It connects out, and wake commands go out one hop at a time until all are on again.
  - Until then, auto-off keeps the network exactly as on, because there can be fast traffic on one side and an admin watching from the other.
- **Measuring:** none of the currents are measured yet: on, auto-off, off and each deep sleep.
- **Storage charge on USB** (docs/plans.md): unchanged.

## Wake time while on or off (BLE)

- **Other devices scanning:** an active scanner (any phone with Bluetooth on scans now and then) answers our adverts with scan requests, and the controller replies. That stretches an advert event by a fraction of a millisecond. It's handled inside the radio's own wake for that advert, so it adds little and doesn't wake the CPU for longer.
- **Our own scanning while on:** this costs much more. The network scans (the "extended discovery" in the log every few seconds) and keeps Coded adverts going, so on and auto-off spend far more time awake than off.
- **Connections:** each link wakes the radio every interval (30–50 ms to phones while on, 400–500 ms while off). A phone left connected keeps that going, which is why Power off on the page now lets go of the link.
- **Settling after boot:** the high wake rate in the first few minutes after a boot or a connection is the network forming. That means scans, connection setup, parameter updates (with retries every 5 s), and the state and config resyncs. It settles once the links are stable.
