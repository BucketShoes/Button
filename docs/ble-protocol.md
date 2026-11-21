# BLE protocol (version 3)

The button is a BLE peripheral with two connectable advertising sets:

- **Legacy, on 1M,** so Chrome's Web Bluetooth can find it (Chrome can only connect through legacy adverts). It
  advertises the service UUID and puts its name in the scan response.
- **Extended, on Coded PHY** (primary and secondary), for range. The service UUID and name go in the AUX packet.

The name is the one set in the settings, or `Button-XXXX` (the last 4 hex digits of the BT MAC, which are also
its node id on the network). Up to 9 connections at once: other buttons plus phones, with 2 always kept free for
phones.

After a phone connects on 1M, the button asks for the link to move to Coded PHY (S=8). If the phone's controller
doesn't support Coded, the link stays on 1M. Once the PHY has settled, the button asks for a 30–50 ms interval on
1M (80–100 ms on Coded) and a 20 s supervision timeout. A central that connects on Coded already keeps its own
parameters.

Buttons find each other with short Coded scans (600 ms, every 3 s for the first minute after boot, then about
every 15 s; none while powered off) and connect directly on Coded as central, at about a 250 ms interval with a
20 s supervision timeout. If two buttons connect to each other at the same moment, the link whose central has the
lower node id is kept. See [Network](#network) for what they send each other.

All values are little-endian. UUIDs are `5c0b000N-7a1e-4d2b-9f3a-6b1e2d3c4f50`:

| N | What | Access |
|---|---|---|
| 1 | service | |
| 2 | state (36 bytes) | read, notify (5 Hz on, 1 Hz off) |
| 3 | config (42 bytes) | read, write, notify (on change) |
| 4 | command | write |
| 5 | link (9 bytes) | read |
| 6 | network frames | write, write without response, notify |

The control page only uses 5 and 6. Characteristics 2–4 still work for talking to one button directly.

## Link

Each read describes the reading central's own connection.

| Offset | Type | Field |
|---|---|---|
| 0 | u8 | TX PHY: 1 = 1M, 2 = 2M, 3 = Coded (0 = unknown) |
| 1 | u8 | RX PHY |
| 2 | u16 | connection interval, in 1.25 ms units |
| 4 | u16 | peripheral latency |
| 6 | u16 | supervision timeout, in 10 ms units |
| 8 | i8 | RSSI of the link in dBm (127 = unknown) |

## State

| Offset | Type | Field |
|---|---|---|
| 0 | u8 | protocol version (3) |
| 1 | u8 | flags: bit0 running, bit1 won, bit2 game paused (op 16; ends with op 17, start, reset, or a hold to the pause point on the button itself, which leaves it in a regular pause), bit3 face held, bit4 identifying, bit5 USB power, bit6 music clip playing |
| 2 | i8 | owner (-1 uncaptured, 0 team 1, 1 team 2) |
| 3 | i8 | winner (-1 none) |
| 4 | i8 | next capture target with no taps, or the current hold's target while held (-1 random) |
| 5 | u8 | hold stage while held: 0 building, 1 captured, 2 paused, 3 reset |
| 6 | u16 | held ms |
| 8 | i32 ×2 | team 1 and team 2 clock values in ms, as shown (one timer: the shared clock, the same in both; set time works on either) |
| 16 | u16 | VSYS mV |
| 18 | u16 | VBUS mV |
| 20 | i16 | battery % estimate (can be below 0 or above 100) |
| 22 | u8 | connected centrals |
| 23 | u8 | reset reason of the last boot (esp_reset_reason_t: 1 power-on, 8 woke from off, 9 brown-out, 4 crash, 5-7 watchdog) |
| 24 | u32 | uptime s |
| 28 | u32 | boots since power-on (more than 1 means it has rebooted) |
| 32 | u8 | power: 0 on, 1 peek (woken, showing battery), 2 off |
| 33 | u8 | bit0 dimmed (idle 1 h), bit1 battery low warning, bit2 sparse LEDs (idle 10 min) |
| 34 | u16 | battery mV: long average of VSYS while on; last unloaded reading while off; 0 = unknown |

## Config

This is the `config_t` struct in [src/config.h](../src/config.h). The button saves it to NVS when it's written.

| Offset | Type | Field |
|---|---|---|
| 0 | u8 | config version (2); writes with a different version are ignored |
| 1 | u8 | mode: 0 teams only, 1 one timer, 2 two timers |
| 2 | u8 | unused, write 0 (was the one-timer team; one timer is now one shared clock) |
| 3 | u8 | count up (0/1) |
| 4 | u8 | show numbers (0/1) |
| 5 | u8 | owner after reset: 0 uncaptured, 1 team 1, 2 team 2 |
| 6 | u8 | first capture of an uncaptured button with no taps: 0 random, 1, 2 |
| 7 | u8 | volume: 0 mute, 1..255 = -40..0 dB (default 128, about -20 dB) |
| 8 | u16 | hold ms (500..60000); pause at +4 s, reset at +9 s, power off at +14 s |
| 10 | u8 | LED brightness 0..255 (default 128) |
| 11 | u8 | debug options: bit0 awake LED (the red status LED is lit whenever the chip is out of light sleep, so every wake shows as a flash; default off) |
| 12 | u32 ×2 | durations in ms, per team (one timer uses the first) |
| 20 | u8 ×6 | team 1 RGB, team 2 RGB |
| 26 | char ×16 | name, NUL-terminated (empty = default) |

- **Mode or direction changes:** the game resets.
- **Other changes:** they take effect at once only if nothing has happened since the last reset.

## Commands

The first byte is the opcode.

| Op | Args | |
|---|---|---|
| 1 | | start (clocks run) |
| 2 | | pause |
| 3 | | reset |
| 4 | [u16 ms] | identify (default 3000, max 10000) |
| 5 | u8 team, i32 ms | set clock value |
| 6 | u8 team, i32 ms | add to clock value (negative to subtract); undoes a win if the winner is no longer at the line |
| 7 | i8 owner | set owner (-1 uncaptured) |
| 8 | u8 id | play a sound (see below) |
| 9 | | stop sounds and the music clip |
| 10 | | (removed: was lock) |
| 11 | | show battery |
| 12 | | restore default settings |
| 13 | | power off |
| 14 | | power on |
| 15 | | reboot (settings kept) |
| 16 | | game pause: stop the clocks, remembering whether they were running |
| 17 | | game resume: undo a game pause (runs again only if it was running before) |
| 18 | | shipping mode (was "factory off"): what a low battery does. Deep sleep with BLE off after about 1.5 s (so the network passes the command on); only a lid press or USB wakes it |
| 19 | | (removed: was the awake LED switch, now config byte 11) |

Sound ids:

| Id | Sound | Id | Sound |
|---|---|---|---|
| 0 | team 1 capture | 7 | countdown blip |
| 1 | team 2 capture | 8 | whistle |
| 2 | win fanfare | 9 | horn |
| 3 | pause | 10 | battery chirp |
| 4 | reset | 11 | startup |
| 5 | ding | 12 | diagnostic |
| 6 | doom | 13 | music clip |

There is no pairing or PIN yet, so anyone with the page can connect, and a command sent to one button reaches
them all.

While off, the button only advertises legacy, slowly (every 2–2.5 s, +6 dBm), so the page can still connect directly. It stops the Coded adverts and scans, and about 3 s after turning off (time for the others to hear it is off) it drops its links to other buttons and refuses new ones; phone connections stay, asked for a 400–500 ms interval instead of 30–50 ms (checked every 5 s and asked again, up to 3 times per change between on and off, if a link is outside the interval it should have). So an off button drops out of the network: other buttons show its last state ("off") until it goes stale, and it can only be woken or powered on over a direct connection or with its lid. Identify, battery and play sound wake it briefly (peek), which also lets it rejoin the network while peeking. Other commands just change its state, and power on turns it on.

## Network

Every button holds the latest state, status and settings of every button it has heard of, and passes them on.
Each button is the authority on its own; admins never write another button's state, they send commands. A phone
connected to any one button sees, and can control, the whole network. The design notes are in
`design/ble-network.md` in the repository.

### Frames

Everything on characteristic 6 is a frame: centrals write them, the button notifies its own to subscribed centrals.

| Offset | Type | Field |
|---|---|---|
| 0 | u8 | frame version (1) |
| 1 | u8 | type: 1 hello, 2 items, 3 heartbeat, 4 summary |
| 2 | u32 | sender's network time in ms (0 from phones) |
| 6 | | body, by type |

- **Hello:** u16 node or origin id, u8 kind (1 button, 2 admin), u8 flags (bit0: this admin link is *active*).
  A phone sends one after subscribing, and again to switch between active and passive. The button answers with
  its own hello, which tells the phone its node id. Button links exchange hellos too.
- **Items:** any number of `u16 origin, u8 stream, u32 version, u8 len, payload`.
- **Heartbeat:** u32 digest (to phones: 0). Sent once a second on every link, active or passive.
- **Summary:** u8 flags (bit0 first part, bit1 last part), then entries of `u16 id, u8 kind, u32 version[3]
  (hot, status, config; 0 = none), u16 last command seq`. It says what the sender has, so the receiver sends what
  is missing; anything not listed by the last part is taken to be missing.

### Streams

| Stream | Payload | Sent |
|---|---|---|
| 0 hot | game state (below) | on change, and every 10 s |
| 1 status | status (below) | every 30 s, and when the button's links or phone count change |
| 2 config | `config_t` (42 bytes, as characteristic 3) | on change |
| 3 command | command (below) | from admins only |

For streams 0–2 the highest version wins; versions start with the button's boot count in the top byte, so a
rebooted button's items are newer. Commands are numbered per admin and taken strictly in order, each once: a later
one waits up to 10 s for a missing earlier one.

**Hot** (21 bytes): u32 network ms when generated, u8 flags (as the state characteristic), i8 owner, i8 winner,
i8 target, u8 hold stage, u16 held ms, i32 ×2 clocks, u8 power, u8 flags2. The clocks and held time are as of
the timestamp; receivers extrapolate while running or held.

**Status** (28 bytes, then the links): u32 network ms, u16 VSYS mV (not read while off), u16 VBUS mV (0 without USB), i16 battery % (while off, from
the last rest reading), u16 battery mV,
u8 phone connections, u8 reset reason, u32 uptime s, u32 boots since power-on, u16 raw VSYS ADC average
(0..4095), u16 VSYS ADC pin mV (VSYS is twice this), u16 time awake since the last status in per mille (light
sleep the rest), then u8 n and n × (u16 node id,
i8 RSSI, u8 PHY) for each other button it has a working link to.

**Command:** u16 the button it entered the network at, u32 expiry (network ms), u16 target node (0xFFFF = all),
u8 kind (0 = an opcode and its arguments as for characteristic 4, 1 = a 42-byte config write), then the data. A
command past its expiry is still passed on but not acted on.

### Network time

Each button keeps `network ms = its own ms + an offset`, and moves the offset forward whenever a button's frame
shows a later time, so all buttons settle on the furthest-ahead clock. Phones take it from the button they're
connected to and use it to set command expiry (now + 10 s) and to tell how old an item is.

### Relaying

A button passes an item on to a neighbour unless that neighbour has a working direct link to the item's origin
(by the neighbour's own status), or, for commands, to the button the command entered at. With every button in
range of every other, nothing is relayed. If two heartbeat digests stay different for 2.5 s, the two buttons
exchange summaries and send each other what's missing.

### Phones

A phone can be connected to several buttons. One link is active and gets every item; the rest are passive and
only get heartbeats. When the active link goes quiet the page makes another one active (a hello with bit0 set)
and sends a summary, so it gets only what it missed. Commands go through the active link with the page's own
random origin id and a sequence number; if a write fails it's sent again through another link, and the buttons
act on it once.
