# Lines (tug-of-war, forge-a-path) and the timeline

Agreed design, 2026-11-25. Stages 1 and 2 built. The map is out of scope for now.

## Stages

1. **Time base and synced identify** (built): 64-bit network time carried across deep sleep by the LP timer;
   "identify all" starts at network time + 1 s, so how together they flash shows how well the clocks agree.
2. **Lines** (built, not yet tested on buttons): as below. Line id: a CRC of the forming buttons' ids (the same
   on every one of them, and different for different sets), or random from the page. The demo shows lines in the
   page but its holds don't follow the rule.
3. **Timeline.**

## Time

- **One counter:** network ms, 64-bit inside a button, forward-only (everyone moves to the furthest-ahead clock it
  hears). Frames and items carry the low 32 bits. Calendar time, where wanted, is worked out by the page from
  "how long before now".
- **Carried across deep sleep** and every reset but a power-on, by the LP timer (RC oscillator: about a percent).
  A power-on starts from 0 until it hears a button further ahead.
- **Assumption:** at least one button stays on (or in deep sleep, carrying the time) through a game. If every
  button has a power-on reset, time starts again from 0, and older log records are "from the future": shown as
  not tonight.
- **Jumps:** a button that ran alone and then joins a network jumps forward. Only a jump of a minute or more is
  logged (a "time jumped by Δ" record), so the page can shift that boot's earlier records; the small corrections
  from drift aren't logged.
- **Sync quality:** frames carry the time they were built, not sent, so links differ by up to a connection
  interval per hop. To measure with identify all, then improve with a ping round trip before playing music
  together (needs about 1 ms).

## Lines

- **Membership:** each button has a line id (0 = independent) and an index. Several lines and independent buttons
  can share a network. A line of one is independent.
- **Line id:** random (16-bit), so two parts of a split network that join don't collide. Its colour (identity only,
  for the forming flash, "LinE", and the page) is the id mod a palette of 8–16 hues, picked to avoid the team
  colours. Team colours stay as each button has them ("team 1 is team 1").
- **Per line:** type (tug-of-war or forge-a-path); each end's team (1, 2 or none: none at an end means a finished
  line can't be taken back from that side); retake time and advance time (both default 30 s).
- **Rule:** team T can capture node i if T owns a neighbour (an end's team counts as a virtual neighbour beyond it)
  and T doesn't own node i. Forge-a-path: a node once owned isn't captured again (as an infinite retake time).
- **Lockout:** a node just captured can't be taken back for the retake time; a node that just became the next one
  along can't be taken for the advance time. Network time from its neighbour's capture, so every button agrees.
- **Displays:** can't be captured: "--:--" on both. Can be (after its lockout, or counting down to it): the time on
  the display of the team that can capture it, the other blank (counting down while locked out).
- **Denied:** a press that can't capture gets a short denied sound and flash and otherwise nothing: no build-up,
  no capture. The display says "nEEd" and the index of the node that team needs. Holding still goes on to the
  pause, reset and power-off at the usual times. (Check the existing game-pause press behaviour for a conflict.)
- **Mode while in a line:** teams-only (no clocks). Back to its own mode when it leaves.
- **Start owners:** each button's existing start-team setting. A new line defaults to half team 1, half team 2,
  the middle one random when odd; the page can change it.
- **Lenient:** a capture checks the last known owners. A neighbour never heard from, or stale, doesn't block.
  Nothing is undone later. The page warns when the network is out of date. Admins can set an owner the rules
  wouldn't allow, after a yes/no warning.

### Forming a line without the page

- **Hold them together** (a stack pressed at once, or everyone holding): each button sees the others holding in
  their hot items. While two or more hold at once, they flash a shared random colour on a shared beat (network
  time). The ones whose hold-resets land within ±1 s of each other form a new line, numbered in node-id order;
  the admins put them on the field by the number each shows.
- **A hold-reset alone** leaves its line.
- **On joining:** "LinE" / "1  3" (line 1, 3rd) for 20 s, in the line's colour. A quadruple tap shows it again.
- **A check of the order:** a sweep down the line, repeated for 20 s.

### Page

- Lines grouped, with type, end teams, retake and advance times.
- Add and remove buttons, reorder (click to reorder, click to lock).
- Only strict straight lines in the UI. Anything else in the data is shown as independent buttons.

## Timeline

- **Records** describe what happened, not whether it was allowed: a capture is a capture, whether the network was
  out of date or a prop drove it (later: contacts or a potentiometer on a flag or quest item; servos).
- **Every button logs every button's events** that it sees (hot items), in a ring in a raw flash partition, about
  1000–2000 records. So any one button can give the whole night's timeline, without every button being online.
- **Record:** u16 node id, u16 time (0.1 s after the last base record), u8 type, u8 argument: 6 bytes. A base
  record carries the full 64-bit network time; one is written at every start, and whenever the u16 would
  overflow (109 min). Base records are also the delimiter between nights.
- **Types** (a dozen or so): captured (team), owner set by an admin, hold started, hold abandoned (stage), start,
  pause, reset, on, off, auto-off, boot, time jumped, joined line, left line.
- **Transport:** live from the hot items, as now. Catch-up: a page asks the button it's connected to for its log
  (it holds everyone's), and keeps it in localStorage. A latest-wins "log tail" item per button (its last few
  records) covers a button whose changes were missed while partitioned.
- **Display:** a row per button, time across, bars in the owner's colour, hatched while paused, grey while
  unknown; icons at moments for the other events; a strip above for who's winning (buttons held per team, or a
  line's frontier position); a summary table (time held per team, captures, longest hold); zoom and scroll.
  Tonight by default: everything since the last long gap; older nights hidden.
- **Rounds:** not configured; the timeline just shows when things happened.
