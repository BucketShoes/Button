# Plans and ideas

This is a running log of what's planned, what's an idea, and what we've decided. Newest decisions go at the
top of their section.

## Done

- **Stage 1, BLE control page:**
  - modes (teams only / one timer / two timers), count up or down, hidden numbers;
  - hold time, colours, start team, and tap-to-pick-team;
  - identify, battery, set holder, lock, sounds, live time adjust;
  - wall clock, and all-buttons commands (resume, pause, identify, sound, power off, copy settings).
- **Volume and LED brightness settings,** defaulting to 50 %, because full volume browns out on USB-only power.
- **Reboot counter and last reset reason** in the page, so brown-outs show up.
- **Basic power off:** from the page, or by releasing BOOT after 1.5–5 s. It's deep sleep, with PWR_EN low, and a press on the lid wakes it and shows the battery.

## Next stages

1. **Power lifecycle** (see below): stay-on vs idle, light-sleep BLE while "off", low-battery levels, storage-charge management.
2. **Timeline / audit log:**
   - **What it records:** every capture, pause, reset, tap, toggle, setting change, and BLE command, with which central sent it. Timestamps come from the button's clock, synced to the phone's clock on connect.
   - **Storage:** a ring buffer in flash, downloadable from the page as CSV/JSON.
   - **Uses:** scoring, post-game review ("when was each point contested"), and catching admin-page meddling.
   - **Visibility:** a changes list on the admin page. Changes aren't announced on the button, since balance tweaks should stay quiet.
3. **Game clock in the page:**
   - round start and end, and a whistle gap (usually 5 min) with a countdown beside the wall clock;
   - optionally play the whistle on all buttons;
   - score a point per whistle for whoever holds each team-only button;
   - scores hidden from players by default.
4. **Button-to-button mesh over BLE adverts:**
   - extended adverts on coded PHY (S=8) carrying a small state packet (owner, running, sequence number);
   - scan most of the time, and keep legacy connectable adverts so Chrome still works;
   - relay commands, so any button (or one phone) controls all of them;
   - prototype range and scan duty cycle on two bare boards first, and compare with 802.15.4.
5. **Tug-of-war interlock:** buttons in a line, where only the frontier can be captured. It needs the mesh, so the first version is plain buttons and admin judgement.
6. **Player proximity, shadow mode only:** log nearby BLE devices for post-game review, and never drive game mechanics from it until it's proven. See the notes below.

## Power lifecycle (design, not built)

- **Waking:** a tap brings it online for a few seconds, showing battery %. Something deliberate keeps it on for a game; something deliberate turns it off. Otherwise it idles off after about an hour with no presses, no connection and no running clock.
  - Careful: a teams-only objective can sit untouched for over an hour mid-game, so "in a game" has to be an explicit state.
- **"Off" (light sleep, about 130 µA):**
  - PWR_EN low, so only the ESP is powered.
  - Plain legacy BLE adverts at about a 1 s interval and about +5 dBm, connectable, so the page can find it and wake it, and identify still works for a lost button.
  - Any face switch wakes it.
- **Cell at or below about 3.3 V:** deep sleep (about 10 µA). A press wakes it just long enough to say "batt low", then it sleeps again.
- **Cell at or below about 3.0 V:** deep sleep with no wake sources, for maximum shelf life.
  - The ESP's brown-out reset is the hardware backstop; set BOD to SEL_3, 3.10 V, for field use.
- **Averaging:** threshold decisions use a long average, not instantaneous readings, so speaker and LED dips can't trigger them.
- **Storage charge on USB:**
  - Charge normally to 4.2 V.
  - After 8 days continuously on charge, stay awake with PWR_EN low, so the cell only gets the body-diode trickle and sits well below the charger. Let it drift down to about 4.0 V.
  - Check hourly, and briefly re-enable charging to keep it at 3.8 V or more.
  - Caveat: with USB present VSYS reads the charger, not the cell. Measuring the cell needs PWR_EN high and a moment to settle, or a hardware change (JP1).
- **Not on charge:** after a week idle, if the cell is below about 3.5 V, stop BLE adverts (deep sleep, press to wake).
- **Context:** games are weekly. Forgotten buttons, especially ones left full on a charger, should drift to storage charge.
- **Mundane access:**
  - **BOOT:** power off (release after 1.5–5 s) and default settings (5 s). It's inside the case, which is fine.
  - **Avoid more tap codes:** nobody remembers them. Keep taps to capture-team (1 or 2 taps then hold) and battery (3 taps).

## Player proximity notes

- **Rotating addresses:**
  - Phones use resolvable private addresses that rotate about every 15 minutes (iOS and Android both default to around 15 min). They don't rotate on every advert.
  - So "seen at respawn, then minutes later at an objective" can often link within one address period, but it breaks at each rotation.
- **Phones mostly don't advertise when idle.** What you hear is:
  - iPhones' Apple Continuity / Find My frames, whose payloads also rotate;
  - watches and earbuds;
  - fitness bands;
  - Android only when an app or feature is advertising.
- **Device names are almost never in BLE adverts.** The name is in the classic Bluetooth inquiry or the GATT device name, which needs a connection. So names won't work passively.
- **Team affiliation can't be inferred** from random devices. Realistic options:
  - a cheap beacon per player or per team (tags with fixed IDs);
  - a tiny web/app advertiser that players opt into;
  - devices "seen at team X's respawn" being tagged X for the next N minutes (heuristic; fine for shadow logging).
- Start with raw logging (address, RSSI, time, manufacturer-data type) at the respawn buttons and an objective, then look at the data after a game.

## Smaller ideas

- **Admin interference:** a hardcoded game PIN only if cheating becomes a problem. A PIN set from the page could lock one admin out by another. The audit log covers "you will be caught".
- **Battery list:** all connected buttons' batteries at a glance.
- **Remember paired buttons** (`navigator.bluetooth.getDevices()`, where Chrome supports it) and reconnect on page load.
- **BLE TX power** as a setting.
- **The speaker is good enough for music and voice:** voice lines for capture ("Purple has taken the gate") or lore cues from the audio partition.
- **Pedals:** foot-switch capture for objectives that need hands free.
