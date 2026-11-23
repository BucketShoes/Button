# All-BLE button network: plan

Internal design notes, kept out of `docs/` because that folder is the public GitHub Pages site.
This covers admin one-to-many control and game state. Player tracking comes at the end, as a later stage.

## Goals

- **Every button knows every other button's state**, whether or not an admin is watching.
  Later game rules depend on it: tug-of-war frontiers, "build the ram before the gate".
- **An admin connected to any one button sees and controls the whole network.**
  The page can still connect to several buttons at once, for debugging and for walking across the field.
- **Admin clients are all treated the same way.** Chrome, a future app, and Wi-Fi if that happens all carry the same
  frames over a different transport.
- **The network adapts to topology changes.** Buttons may be stretched out, partitioned, or carried around
  ("grab the treasure and run").

## Where the code is now (2026-11-21)

- **Advertising and PHY:** legacy advertising only, on 1M. Nothing renegotiates the PHY after connecting;
  `src/ble.c` has no PHY update. The `sdkconfig.defaults` comment still says "coded PHY / extended adverts come later".
- **Roles and connections:** peripheral only (`CONFIG_BT_NIMBLE_ROLE_CENTRAL` off), with `MAX_CONNECTIONS=4`.
- **State notifications:** 5 Hz while running, 1 Hz when not.
- **The page:** buttons are added one at a time through the Chrome picker, and several can be connected at once.
  "All buttons" commands loop over the ones you've added.
  When a state notification arrives, the page stamps it with `rx = performance.now()` and extrapolates
  `heldMs` and the clocks from that, so it assumes zero latency.

## Radio plan

- **Two advertising sets** (needs NimBLE 5.0 / extended advertising):
  - **Legacy 1M connectable**, at about 100–150 ms. Chrome can only connect through this one.
    It's a single `ADV_IND` on each of the 3 primary channels, plus a scan response when asked.
  - **Extended connectable, primary and secondary Coded S=8**, at about 500 ms. Buttons and a future app connect
    through this one.
    - On each primary channel it sends only a short `ADV_EXT_IND` (ADI + AuxPtr, about 1.2 ms on S=8).
    - The data (flags, service UUID, 16-bit node id, about 25 B) goes in one `AUX_ADV_IND` on a data channel.
      Connectable extended adverts can't chain, so the data must fit in that one AUX packet.
    - The connect request and response also happen on the secondary channel.
- **Buttons connect to each other directly on Coded** (initiating on Coded PHY), so they never need to be brought
  close together to link up.
- **PHY renegotiation on every connection, whoever the central is.** On connect, the button requests Coded
  (`ble_gap_set_prefered_le_phy`, Coded S=8, perhaps S=2 for admins). If the phone's controller doesn't support it,
  the link stays on 1M.
  - Log which PHY each connection ends up on.
  - Show it on the page per connection, so we learn which phones accept Coded.
- **Connection parameters:**
  - **Button↔button:** 250 ms interval, supervision timeout about 20 s (the spec maximum is 32 s), so links ride out fades.
  - **Admin:** the button requests about 50 ms on 1M, or about 100 ms on Coded (Coded events cost about 5× more),
    with a long supervision timeout too.
- **Scanning:**
  - Mostly legacy. Players come later; for now legacy scanning just fills the time Coded scanning doesn't use.
  - **Every button does a Coded scan now and then**, whether or not it is lonely, otherwise two halves of the
    network could stay split forever. For example 600 ms (more than one advert interval) every 10–20 s, which is
    a 3–6 % duty.
  - Faster Coded adverts (500 ms) pay for rarer scans.
- **Who connects:** whoever hears an unlinked button's Coded advert initiates, so a lonely button can join on its
  own scan. If the pair ends up with two links, keep the one whose central has the lower address and drop the other.
- **Healing:** a dropped button link is re-initiated straight to the peer's known address on Coded. Gossip already
  carries everyone's address, so it doesn't wait for a discovery scan.
- **Connection count:** don't assume 5 buttons; 5 is the minimum the network has to work with.
  - Set `MAX_CONNECTIONS` from (buttons − 1) + admin phones + spare, for example 12–16, as RAM allows. The C6 guide
    says 70.
  - No cap on button links for now.
  - Past about 8–10 buttons, idle link cost grows with the number of links (see Scaling). Nothing in the protocol
    needs a full mesh, so a later version can keep only the best k links by RSSI and let relaying do the rest.

## Implementation rules

- **Check every NimBLE return code** (`ble_gap_*`, `ble_gattc_notify_custom`, `ble_gattc_write*`,
  `os_mbuf` allocations) and log failures with the call and the connection handle.
  - `BLE_HS_ENOMEM` (out of mbufs, likely when queueing to many links) means back off: keep the item pending for
    that neighbour and retry on the next tick, rather than dropping it silently.
  - Busy and "already in progress" results on connect, scan or advertise calls get a retry, not an abort.
- **Frame size:** an LL PDU carries 251 B with DLE. After L2CAP (4 B) and ATT notify (3 B) that leaves 244 B for
  application data, with MTU ≥ 247.
  - On Coded, cap a frame at about 100–120 B (about 7–8 ms on air) so one connection event doesn't hold the radio
    for long. Spread a larger backlog over later events.
  - On 1M the full 244 B is fine (about 2 ms).
  - Look at `CONFIG_BT_LE_COEX_PHY_CODED_TX_RX_TLIM` once Wi-Fi comes in.

## Identity

- **Node id:** u16, the last two bytes of the BT MAC (the same as the `Button-XXXX` name). Ids aren't positions,
  so when two partitions merge there's nothing to renumber, and no split-brain over "who is bit 0".
- **Neighbour lists:** lists of node ids (2 B each), not bitmaps.
- **If two buttons share an id:** show it on the page, and fall back to 3 bytes.
- **Admin clients:** each page or app session picks a random u16 origin id, checked against the origins already
  known, and gets a new one on every load.

## Network time

- **The clock:** each node keeps `net_ms = local_ms + offset`. Every frame header carries the sender's `net_ms`.
  When a received value is ahead of mine, I jump forward to it. My clock only moves forward.
  - All clocks converge on the furthest-ahead one, with no leader election, and partitions merge on their own.
  - Error is up to about one connection interval (the stamp is taken when queued, not when sent). That's fine for
    expiry (seconds) and for the timeline (5 s resolution).
  - It can be refined later with a ping round trip.
- **Wall time:** when an admin connects, the page sends its Unix time once. A button that knows the mapping
  "net X = Unix Y" gossips it in the slow tier, so the timeline can be shown in wall-clock time.
- **Timestamps inside records:** items carry the time they were *generated*, for example "hold started at net T",
  so the page extrapolates correctly however many hops an item took.
  A relayer's frame header is its own time, not the origin's, which is why the stamp lives in the item.

## State and commands

A button is the only authority on its own state (clocks, owner, hold). **Admins never write state; they send
operations.** The target button applies operations in the order it receives them and publishes the result.

- **Example:** "set owner purple" racing with a capture on the button is resolved by the button. Whatever it
  processes last wins, and everyone sees the outcome in its next state record. This is the same as with direct BLE
  today.
- **Exactly once:** "add 10 s" has to apply exactly once per click, so operations need ids and dedup. Absolute
  operations (set owner, start, pause) get the same treatment, so all operations behave alike.

### Streams

Everything on the wire is an item `(origin u16, stream u8, seq u16, payload)`. Each origin's version counter goes
up on every change to any of its streams.

| Stream | Kind | Payload | When |
|---|---|---|---|
| hot | latest wins | flags, owner, winner, target, hold stage, hold start (net ms), clocks + "as of" net ms. About 18 B | on change, plus every 5–10 s |
| slow | latest wins | battery, uptime, reset reason, centrals, neighbour list with link RSSI and PHY, wall-time mapping | on neighbour change, otherwise every 30–60 s |
| config | latest wins | `config_t` (42 B) | on change |
| commands | append log, expiring | target (u16, 0xFFFF = all), expiry (net ms), op + args | admin origins only |
| timeline | append log, kept for the round | ownership/hold/pause events: net ms, kind, team | on event |

- **Latest-wins streams:** keep only the highest seq. A repeat or an older copy is dropped harmlessly.
- **Command stream: in order, exactly once, per admin origin.**
  - The receiver keeps `H`, the highest seq it has contiguously from that origin.
  - It accepts only `H+1`, then advances `H`.
  - Anything `≤ H` is a duplicate and is dropped.
  - Anything past a gap is buffered until the gap fills. If the gap doesn't fill within the longest TTL (about
    10 s), the missing item is skipped.
  - So "a later one, then an earlier one" still applies both, in click order.
- **Expiry:**
  - Each command carries an absolute expiry in net time, which the page sets to about now + 10 s.
  - An expired command still advances `H`, so the stream stays contiguous, but it isn't executed.
  - Expiry depends only on the network clock, not on how long an item sat in a NimBLE queue.
  - Long-lived operations (config, set owner) can carry a longer expiry.
- **Timeline:**
  - Each button keeps its own log of owner changes, holds and pauses for the round, in a ring in RTC RAM or flash.
  - Events are sparse (about 1 per minute), so an hour is a few hundred bytes. That's cheaper than sampling every 5 s.
  - A button that joins late or reboots, or a page that arrives late, catches up by pulling the missing seqs.
  - This merges with the "timeline / audit log" stage in plans.md.

### Sync and suppression

- **Each link is reliable and in order while it's up.** The link layer acks and retransmits, and ATT keeps order.
  Data in flight when a link drops is lost, and there's no telling which part arrived. So:
  - **Senders send each stream to a neighbour in seq order,** starting from what that neighbour has, so order holds
    on every path.
  - **Heartbeat once a second on each button link:** `net_ms` plus a 4 B digest (CRC) of every `(origin, version)`
    the sender knows. That keeps it the same size whatever the button count.
    - If the digests differ, the two sides exchange the full `(origin, version)` list (4 B per origin), then
      per-stream seqs for the origins that differ, and send what's missing.
    - The heartbeat also tells a node whether the link is actually delivering (the host doesn't see empty connection
      events).
- **Suppression rule (start with only this): don't send origin O's items to neighbour N if N lists a fresh direct
  link to O.**
  - A link counts as fresh if a heartbeat crossed it in the last 2–3 s.
  - When a link goes stale or drops, both ends republish their neighbour list right away, and the neighbours start
    relaying to them again from the summaries.
  - **Full mesh:** zero relays.
  - **A line** (A–B–C–D–E): exactly one copy per hop, which is optimal.
  - **Partial meshes:** the rule alone doesn't pick a path. In a diamond (A linked to B and C, B and C linked to D),
    both B and C would send A's items to D.
- **Relay tree, which picks exactly one path:**
  - Every node has the whole link graph, because neighbour lists are in the slow tier.
  - For each origin O, every node runs the same BFS from O, breaking ties by lowest node id.
  - X sends O's items to N only if X is N's parent in that tree. In the diamond, B (lower id) relays to D and C stays
    quiet. Nothing is chosen in advance: each origin's tree is recomputed whenever the graph changes.
  - In a full mesh the tree is every node attached straight to O, so the result is the same as the simple rule.
  - **When views disagree** during churn (one node has seen a link change and another hasn't yet), either both relay
    (a harmless duplicate that dedup drops) or neither does.
  - **Fallback for "neither":** if N's heartbeat summary still shows it missing an item a neighbour has, one
    heartbeat later, every neighbour that has it sends it. That's a possible duplicate, but never a stall longer than
    about 1–2 s.
  - BFS over the graph is trivial even at 100 nodes, so this doesn't limit the button count.
- **Build order:** start with the simple rule plus the fallback, because it's correct and needs no graph. Add the
  tree when partial meshes show up in tests. Sending to both is wasteful, not wrong.
- **Batching:** pack all pending items for a neighbour into one packet per connection event, with DLE up to 251 B.
  The fixed per-packet cost dominates on Coded.
- **Admin links:** every button already holds the whole network's state, whether or not an admin is watching.
  An admin link just gets what that button knows (all origins, but not the timeline, which is sent on request).
  - Admins are leaves, so they never appear in neighbour lists or the relay tree.
  - Adding phones changes nothing in button-to-button forwarding. The only cost is each button's link to each phone.

## Admin page and app

- **Merging:** each connected button streams items from all origins. The page merges by `(origin, seq)`, so it
  doesn't matter whether an item arrived directly or was relayed.
- **The button list is the whole network,** including buttons never picked in the Chrome picker. Those can be
  commanded through relays.
- **One phone holds connections to many buttons at once, and keeps them all.** This matters for range: Chrome can
  only *establish* a connection on legacy 1M and then renegotiate to Coded. So the phone has to be connected
  already before the admin walks out of 1M range, or that button can't be reached directly at Coded range.
- **Active and passive connections:**
  - **One active** connection streams every item to the page.
  - **The rest stay passive:** connected, but sending only the heartbeat (`net_ms` + digest) and acting on
    commands. That keeps each button's cost per extra phone close to an idle link.
  - The page switches a connection with a write to a mode characteristic, for example making the freshest link
    active when the active one goes quiet. A debug toggle makes all of them active.
  - **On becoming active,** the button compares the page's digest with its own and sends what the page is missing.
    In sync terms the page is just another node, which is why switching loses nothing.
- **Commands** go out through the active connection. If it hasn't delivered anything for about 1–2 s, the page
  resends the same command id through another connection, active or passive. Dedup makes that safe, and it covers
  a dead link that a 20–32 s supervision timeout still reports as "up".
- **Auto-reconnect:**
  - Chrome can only connect to buttons picked in its chooser, so at setup (when the buttons are together anyway)
    the admin picks every button once.
  - After that, the page keeps retrying `gatt.connect()` on every picked button that isn't connected. Walking back
    into 1M range of a button that dropped (out of Coded range, then the supervision timeout) reconnects it on
    legacy, and the button renegotiates to Coded straight away.
  - Unverified: does `navigator.bluetooth.getDevices()` survive a page reload on the Android Chrome in use, so the
    picks don't have to be redone? Is there a per-phone cap on simultaneous LE connections (often around 7 on
    Android)?
- **Pending commands:** a command shows as pending until the connected button's summary says it has it, and the
  effect shows in the target's state.
- **Any number of admin phones,** each with its own origin id and its own set of connections. A button treats each
  phone's link like any other admin link.
- **Protocol v4:**
  - a "network" notify characteristic carrying frames of items;
  - a "send" write characteristic taking frames (command items with the admin's origin);
  - keep v3 state/config/command for now, as a direct per-button debug view.
- **Same frames everywhere:** the same frame format goes over BLE (Chrome or app) and over Wi-Fi if that happens.

## Airtime estimates (Coded S=8, per button's radio)

Packet cost: 376 µs fixed, plus 64 µs per byte (header, payload and CRC). An empty connection event (both sides)
is about 1.6 ms. One 20 B item adds about 1.7 ms.

| Item | Per second | Share |
|---|---|---|
| 4 button links, 250 ms, idle | 16 events × 1.6 ms | 2.6 % |
| 1-s heartbeat with summary, 4 links | 4 × ~1.5 ms | 0.6 % |
| 2 admins on Coded, 100 ms, idle (on 1M at 50 ms: about 0.6 % each) | 20 × 1.6 ms | 3.2 % |
| Legacy adverts, 100 ms | 10 × ~1.2 ms | 1.2 % |
| Coded extended adverts, 500 ms | 2 × ~7 ms | 1.4 % |
| Busy data: 5 changes/s across the network, received from 4, sent to 4 buttons, all 5 forwarded to 2 admins | ~18 × 1.7 ms | 3 % |
| Same, without suppression in a full mesh (each relays to 3, hears 12 duplicates) | +24 × 1.7 ms | +4 % |

- **Totals:** about 12 % busy with suppression, about 16 % without, and the rest goes to scanning.
- **Why the network isn't one collision domain:** connections hop over 37 data channels with independent hop
  sequences. Two links only collide when they land on the same channel at the same moment, and retransmission covers
  that.
- **What really runs out is each node's own radio time.** When B is peripheral on two links whose anchors happen to
  overlap, its controller skips one event, which costs latency (one interval), not bandwidth.
- **Primary advertising channels:** only short packets go there. Each button puts about 0.3 ms per 100 ms (legacy)
  plus about 1.2 ms per 500 ms (`ADV_EXT_IND`) on each primary channel. That's about 0.5 % occupancy per button per
  channel, so 5 buttons is about 3 %, before phones and tags. Not a bottleneck either.
  The real reason Coded adverts aren't faster is each button's own radio time: about 7 ms per advert event,
  counting all 3 primaries and the AUX.
- **Why this is far below "resend everything every interval":** if every button sent all 5 states (about 90 B) to
  all 6 links on every 250 ms event, that's about 6.5 ms × 4 events × 6 links ≈ 16 % for sending alone, plus the
  same again for receiving. Sending only on change, and batching, is what brings it down.

## Scaling past 5 buttons

N buttons, full mesh, per button's radio:

- **Idle links:** (N − 1) × 4 events/s × 1.6 ms. That's 2.6 % at 5, 5.8 % at 10, 12 % at 20.
  This is the term that eventually forces a cap on links per button (best k by RSSI), with relaying covering the rest.
- **Data with suppression or the tree:** each button receives each change once and sends its own to its neighbours,
  so the cost grows with N.
- **Data without suppression:** each change arrives about N − 1 times at every button, so the cost grows with N².
  This is fine at 5 and is what stops it working well at 10–20.
- **Heartbeat:** a fixed 4 B digest, so it doesn't grow with N. The full origin list is only sent on mismatch.
- **State the protocol keeps:** grows with N (one record set per origin, neighbour lists). It's fine into the tens
  of buttons. Truly unlimited would need nodes to know only their neighbourhood. Not needed now, and nothing
  above rules it out.

## Stages

Status (2026-11-21): stage 1 is tested on hardware (a phone link on Coded). Stages 2–4 are written in
`src/ble.c`, `src/mesh.c` and the page, build cleanly, and work in the page's demo; they are not yet tested on
buttons. The protocol as built is in `docs/ble-protocol.md` ("Network"). Where the build differs from this plan:
relaying uses the simple rule plus summaries (no tree yet); the command item carries the button it entered at;
the page has one active link and passive ones, and it follows the active link's clock rather than pushing its own.

1. **Coded on what exists:**
   - turn on 5.0 features;
   - two advertising sets (legacy + Coded connectable);
   - PHY update request and connection-parameter request on connect;
   - show the PHY and interval per connection on the page;
   - range-test two buttons; check which phones end up on Coded.
2. **Button links:**
   - central role;
   - occasional Coded scans on every button;
   - connect, duplicate-link rule, long supervision timeout;
   - neighbour lists;
   - show the link graph on the page (read only).
3. **Network time, plus hot/slow/config streams,** with the suppression rule and heartbeat summaries. The page shows
   every button from one connection.
4. **Commands:** admin origins, in-order exactly-once delivery, expiry, page failover across connections,
   active/passive links, auto-reconnect.
5. **Timeline stream,** with catch-up for late joiners.
6. **Later, opportunistic:** player counts (below), then tags and teams.

## Player tracking (later stages, not now)

- **First:** per-button counts only. Near/far × team 1 / unassigned / team 2 is 6 numbers, all shown in the overview.
  Counts come from legacy sightings over a window, excluding buttons and connected admins. They're about 6 B in the
  slow tier, and scanning stays opportunistic.
- **Teams later:**
  - phones mostly stay "unassigned", because rotating addresses (about 15 min) break any team inference;
  - only stable-address tags can carry a team across a rotation;
  - the respawn heuristic is ambiguous (dead and respawning vs fighting near the enemy respawn);
  - a phone user also carrying a tag counts twice.
- **Enrolment** maps a tag to a small index, so byte k of a compact RSSI list is tag k. It's only for special tags
  (for example the flag), not for every player.
- **Before relying on the trackers,** test them: do they advertise unconnected, how often, is the address stable,
  and do they stop after a while?
- **Device lists** (address, RSSI): on request from one admin, routed back to that admin only, never flooded.
- **Button GPS:** the page stamps a button's location from the phone into config, slow-gossiped, for a field map.

## Ideas: a quieter network (2026-11-23, not built)

**Where the radio time goes now, per button, while on** (ble.c):
- **Coded scans:** 600 ms windows (passive, continuous RX), every 3 s ±20 % for the first minute after boot (20 %
  RX duty), then every 15 s ±20 % (4 %). None while there's no free connection (all but the 2 phone slots used),
  and none while off.
- **Connecting:** a 100/100 ms scan, continuous, for up to 3 s per attempt (CONNECT_TIMEOUT_MS), only when an
  advert from an unlinked button has been heard.
- **Adverts:** legacy every 100–150 ms and Coded every 500–550 ms, about 2.6 % of the radio's time together (see
  the airtime table). Links to buttons at 225–275 ms, about 0.65 % each when idle.
- So once settled, the scan is the largest single radio cost: 4 % RX with the chip awake throughout. At the C6
  datasheet's RX current of about 71–74 mA (not measured here), that's roughly 3 mA averaged. Measure it
  before building anything: the page's "awake %" in Advanced is per status period, and the scan is most of what a
  settled, untouched button spends awake.

**1. Network quiet, as a command (the user's proposal).** One button gets "quiet" from the page. It floods like any
command: every button stops scanning and making outgoing connections, but keeps advertising (Coded, slower: every
2 s) and keeps accepting connections. Once the command has spread (when every button's status says quiet, or after
a fixed few seconds), each drops its links to other buttons. Any incoming connection, from a phone or another
button, turns growth back on at that button: it scans, connects out, and the buttons it connects to turn growth on
too, so one button that wants a network brings the whole network back, one hop every scan period (early-mode
scanning: about 3 s a hop).
- **What's lost while quiet:** the page sees only buttons it connects to directly, and "all" commands (start, pause,
  copy settings) reach only those. The game itself doesn't need the network: each button runs its own clocks.
- **Saves:** the scan (about 4 % RX) and the links' events; adverts stay.
- **Setting or command?** Better as a setting ("network: always on / quiet once settled") kept in config, so it
  survives a reboot, with the page's "wake the network" being just a connection. A reboot then comes up quiet,
  which is fine, since any connection brings it back.

**2. Known buttons.** Each button keeps a list in NVS of every button id it has ever linked to or heard items from
(16 ids is 32 bytes). For the first few minutes after a boot it scans as now, and so does a button whose network is
missing one of them. Once every known id is fresh in its network (directly or through others, from the items
every button already holds), it stops scanning, and starts again when any of them goes stale. So adding a button
needs a boot of either, or a gap in the network. A retired button would keep everyone scanning for ever: it needs
a "forget" (a page command, or dropping ids not seen for a week).
- Both together: (2) is the automatic version of (1) for a complete network; (1) also drops the links.
- With 5 buttons, a working network is a connected graph, not a full mesh, so "complete" is "every known id is in
  the network's items", not "linked to all of them".

## Open questions

- Do iPhones and the Android phones in use accept a PHY update to Coded from Chrome? Does Windows Chrome?
- Coding for admin links: S=8 (range) or S=2 (about half the airtime)? Is it worth choosing per link by RSSI?
- Is the real busy rate 5 changes/s, or much lower? Measure it once stage 3 logs item counts.
- Does a shortest-path relay tree ever earn its complexity over the simple suppression rule?
- Command expiry defaults per operation (identify and sounds short; config and owner long?).
