// Demo mode (index.html?demo): simulated buttons, so the page can be tried without hardware.
// Five buttons run a rough copy of the firmware's game rules (clocks count down, holds fill and
// capture, the controls do what they say) and form a simulated network. The page "connects" to two
// of them, and goes through its normal code path: network frames in, commands out, active/passive
// links. The other three are only heard of through the network, as with real buttons out of range.
"use strict";
(() => {
  // fav: the team that mostly owns this button. steal: how often the other team's hold succeeds
  // (their other holds give up part way), so each button is mostly one colour over time.
  // Shrine starts near zero and its owner never loses it, so a win shows up within a minute.
  // Well is in one-timer mode: one shared clock, shown in the owner's box.
  // links: which other buttons each one hears directly (Well is out at the edge).
  const DEMOS = [
    { id: 0xB001, name: "Bridge", owner: 0, running: true, time: [512000, 431000], batt: 87, fav: 0, steal: 0.25, direct: true },
    { id: 0xB002, name: "Shrine", owner: 1, running: true, time: [598000, 40000], batt: 64, fav: 1, steal: 0 },
    { id: 0xB003, name: "Gate", owner: -1, running: true, time: [600000, 600000], batt: 92, fav: 1, steal: 0.3, direct: true },
    { id: 0xB004, name: "Tower", owner: 0, running: false, time: [377000, 489000], batt: 41, fav: 0, steal: 0.2 },
    { id: 0xB005, name: "Well", owner: 1, running: true, time: [375000, 375000], batt: 18, fav: 1, steal: 0.35, mode: MODE_ONE },
  ];
  const LINKS = { 0xB001: [0xB002, 0xB003, 0xB004], 0xB002: [0xB001, 0xB003, 0xB004], 0xB003: [0xB001, 0xB002, 0xB004, 0xB005],
                  0xB004: [0xB001, 0xB002, 0xB003, 0xB005], 0xB005: [0xB003, 0xB004] };
  const TICK = 100, HOT_EVERY = 10000, SLOW_EVERY = 30000;
  const HOLD_EVERY = 9000;   // average ms between hold attempts on a button
  const netMs = () => (performance.now() + 500000) >>> 0;
  const sims = [];
  const seenCmd = new Map();  // origin -> highest seq acted on (the network's exactly-once, roughly)

  function makeSim(d) {
    const cfg = {
      mode: d.mode ?? MODE_TWO, timerTeam: 0, countUp: 0, show: 1, initial: 0, defCapture: 0,
      volume: 128, ledLevel: 128, holdMs: 3000, dur: [600000, 600000], colour: ["#9600ff", "#ffaa00"], name: d.name,
    };
    const s = {
      running: d.running, won: false, gamePaused: false, wasRunning: false, held: false, identifyUntil: 0,
      owner: d.owner, winner: -1, target: d.owner >= 0 ? 1 - d.owner : Math.round(Math.random()), heldMs: 0,
      holdFor: 0, time: d.time.slice(), batt: d.batt, power: 0, wonAt: 0, start: performance.now(),
    };
    const sim = { id: d.id, d, cfg, s, ver: [0, 0, 0], data: [null, null, null], shape: "", hotAt: -1e9, slowAt: -1e9 };
    const timed = () => cfg.mode !== MODE_TEAMS;
    const share = t => { if (cfg.mode === MODE_ONE) s.time[1 - t] = s.time[t]; };  // one timer: one shared clock
    let seq = 0;
    const bump = (k, bytes) => { sim.ver[k] = (0x01000000 | ++seq) >>> 0; sim.data[k] = bytes; };

    function encodeHot() {
      const b = new Uint8Array(21), v = new DataView(b.buffer), now = performance.now();
      v.setUint32(0, netMs(), true);
      v.setUint8(4, s.running | s.won << 1 | s.gamePaused << 2 | s.held << 3 | (now < s.identifyUntil) << 4);
      v.setInt8(5, s.owner); v.setInt8(6, s.winner); v.setInt8(7, s.target); v.setUint8(8, 0);
      v.setUint16(9, Math.min(65535, s.heldMs), true);
      v.setInt32(11, Math.round(s.time[0]), true); v.setInt32(15, Math.round(s.time[1]), true);
      v.setUint8(19, s.power); v.setUint8(20, s.batt < 20 ? 2 : 0);
      return b;
    }
    function encodeSlow() {
      const nb = LINKS[d.id], b = new Uint8Array(23 + nb.length * 4), v = new DataView(b.buffer), mv = 3500 + s.batt * 7;
      v.setUint32(0, netMs(), true); v.setUint16(4, mv, true); v.setInt16(8, s.batt, true); v.setUint16(10, mv, true);
      v.setUint8(12, d.direct ? 1 : 0); v.setUint8(13, 1);
      v.setUint32(14, Math.round((performance.now() - s.start) / 1000), true); v.setUint32(18, 1, true);
      v.setUint8(22, nb.length);
      nb.forEach((id, k) => { v.setUint16(23 + k * 4, id, true); v.setInt8(25 + k * 4, -55 - ((id * 7 + d.id) % 30)); v.setUint8(26 + k * 4, 3); });
      return b;
    }
    // Like the firmware: a new version when something other than the running clocks changed, and every 10 s.
    sim.publish = force => {
      const now = performance.now(), h = encodeHot();
      const shape = [s.running, s.won, s.gamePaused, s.held, now < s.identifyUntil, s.owner, s.winner, s.target, s.power].join();
      if (force || shape !== sim.shape || now - sim.hotAt > HOT_EVERY) { sim.shape = shape; sim.hotAt = now; bump(0, h); }
      if (now - sim.slowAt > SLOW_EVERY) { sim.slowAt = now; bump(1, encodeSlow()); }
      if (!sim.data[2]) bump(2, new Uint8Array(encodeConfig(cfg)));
    };

    function reset() {
      Object.assign(s, { running: false, won: false, held: false, heldMs: 0, owner: cfg.initial - 1, winner: -1 });
      s.time = cfg.mode === MODE_ONE ? [cfg.dur[0], cfg.dur[0]] : cfg.dur.slice();
      s.target = s.owner >= 0 ? 1 - s.owner : Math.round(Math.random());
    }
    sim.apply = (kind, v) => {
      if (kind === CMD_KIND_CONFIG) { Object.assign(cfg, parseConfig(v)); bump(2, new Uint8Array(encodeConfig(cfg))); return; }
      switch (v.getUint8(0)) {
        case OP.START: s.running = true; s.gamePaused = false; break;
        case OP.PAUSE: s.running = false; break;
        case OP.RESET: reset(); break;
        case OP.IDENTIFY: s.identifyUntil = performance.now() + v.getUint16(1, true); break;
        case OP.SET_TIME: s.time[v.getUint8(1)] = v.getInt32(2, true); share(v.getUint8(1)); break;
        case OP.ADD_TIME: { const t = v.getUint8(1); s.time[t] = Math.max(0, s.time[t] + v.getInt32(2, true)); share(t); break; }
        case OP.SET_OWNER: s.owner = v.getInt8(1); s.won = false; s.target = s.owner >= 0 ? 1 - s.owner : s.target; break;
        case OP.GAME_PAUSE: if (!s.gamePaused) { s.wasRunning = s.running; s.running = false; s.gamePaused = true; s.held = false; } break;
        case OP.GAME_RESUME: if (s.gamePaused) { s.running = s.wasRunning; s.gamePaused = false; } break;
        case OP.POWER_OFF: s.power = 2; s.running = false; s.held = false; break;
        case OP.POWER_ON: s.power = 0; break;
        case OP.FACTORY_RESET: break;
      }
      sim.publish(true);
    };

    // The game: clocks run, and now and then someone holds the lid down (some give up part way).
    sim.step = dt => {
      if (s.power) return;
      const now = performance.now();
      if (s.running && !s.won && s.owner >= 0 && timed(s.owner)) {
        s.time[s.owner] -= dt;
        if (s.time[s.owner] <= 0) { s.time[s.owner] = 0; s.won = true; s.winner = s.owner; s.running = false; s.wonAt = now; }
        share(s.owner);
      }
      if (s.held) {
        s.heldMs += dt;
        if (s.heldMs >= s.holdFor) {
          if (s.holdFor >= cfg.holdMs) {
            s.owner = s.target;  // a paused button's target is its owner, so it resumes with the same team
            s.running = true;
          }
          s.held = false; s.heldMs = 0;
          s.target = s.owner >= 0 ? 1 - s.owner : 1 - s.target;
        }
      } else if (!s.won && !s.gamePaused && !(s.owner >= 0 && !s.running) && Math.random() < dt / HOLD_EVERY) {
        // (a paused button is left alone, so the demo always shows one; Start resumes it)
        s.held = true; s.heldMs = 0;
        const succeeds = s.target === d.fav ? Math.random() < 0.85 : Math.random() < d.steal;
        s.holdFor = succeeds ? cfg.holdMs : cfg.holdMs * (0.3 + Math.random() * 0.5);
      }
    };
    sim.newGame = () => {  // back to how the demo opened
      reset();
      Object.assign(s, { owner: d.owner, running: d.running, time: d.time.slice(), gamePaused: false });
      s.target = d.owner >= 0 ? 1 - d.owner : d.fav;
    };
    sims.push(sim);
    return sim;
  }

  // A simulated direct connection to one of the buttons: its network characteristic sends
  // everything the (simulated) network knows to an active page, heartbeats to a passive one.
  function makeDevice(sim) {
    const sent = new Map();  // `${id}/${stream}` -> version sent to the page
    let active = false;
    const net = {
      fns: [],
      addEventListener(_, f) { this.fns.push(f); },
      async startNotifications() {},
      notify(bytes) { const value = new DataView(bytes.buffer); this.fns.forEach(f => f({ target: { value } })); },
      async writeValueWithResponse(data) {
        if (!device.gatt.connected) throw new Error("not connected");
        const v = new DataView(data.buffer || data, data.byteOffset || 0, data.byteLength);
        const type = v.getUint8(1);
        if (type === F_HELLO) {
          active = !!(v.getUint8(9) & 1);
          const [f, dv] = frame(F_HELLO, 4);
          dv.setUint32(2, netMs(), true); dv.setUint16(6, sim.id, true); dv.setUint8(8, K_BUTTON);
          setTimeout(() => net.notify(f), 20);
        } else if (type === F_SUMMARY) {
          if (v.getUint8(6) & 1) sent.clear();  // simplification: resend everything newer than the page's list
          for (let o = 7; o + 17 <= v.byteLength; o += 17)
            for (let k = 0; k < 3; k++) sent.set(`${v.getUint16(o, true)}/${k}`, v.getUint32(o + 3 + k * 4, true));
        } else if (type === F_ITEMS) {
          for (let o = 6; o + 8 <= v.byteLength;) {
            const origin = v.getUint16(o, true), stream = v.getUint8(o + 2), seq = v.getUint32(o + 3, true), len = v.getUint8(o + 7);
            const p = new DataView(v.buffer, v.byteOffset + o + 8, len);
            o += 8 + len;
            if (stream !== S_CMD || (seenCmd.get(origin) ?? -1) >= seq) continue;  // a repeat through another link
            seenCmd.set(origin, seq);
            if (newer32(netMs(), p.getUint32(2, true))) continue;  // expired
            const target = p.getUint16(6, true), kind = p.getUint8(8), body = new DataView(p.buffer, p.byteOffset + 9, len - 9);
            for (const x of sims) if (target === ALL || target === x.id) x.apply(kind, body);
          }
        }
      },
    };
    const link = { async readValue() {  // Coded both ways, 100 ms interval, 20 s timeout, some RSSI
      const v = new DataView(new ArrayBuffer(9));
      v.setUint8(0, 3); v.setUint8(1, 3); v.setUint16(2, 80, true); v.setUint16(6, 2000, true);
      v.setInt8(8, -60 - Math.round(Math.random() * 25));
      return v;
    } };
    const device = {
      name: sim.d.name, id: "demo-" + sim.d.name, addEventListener() {},
      gatt: {
        connected: false,
        async connect() { device.gatt.connected = true; return server; },
        disconnect() { device.gatt.connected = false; active = false; },
      },
    };
    const server = {
      async getPrimaryService() {
        return { async getCharacteristic(u) {
          if (u === CHR_NET) return net;
          if (u === CHR_LINK) return link;
          throw new Error("no such characteristic in the demo");
        } };
      },
    };
    let lastHb = 0;
    setInterval(() => {
      if (!device.gatt.connected) return;
      const now = performance.now();
      if (active) {
        const items = [];
        for (const x of sims) for (let k = 0; k < 3; k++) {
          const key = `${x.id}/${k}`;
          if (x.data[k] && (!sent.has(key) || newer32(x.ver[k], sent.get(key)))) { items.push([x, k]); sent.set(key, x.ver[k]); }
        }
        if (items.length) {
          const len = items.reduce((n, [x, k]) => n + 8 + x.data[k].length, 0), [f, dv] = frame(F_ITEMS, len);
          dv.setUint32(2, netMs(), true);
          let o = 6;
          for (const [x, k] of items) {
            dv.setUint16(o, x.id, true); dv.setUint8(o + 2, k); dv.setUint32(o + 3, x.ver[k], true); dv.setUint8(o + 7, x.data[k].length);
            f.set(x.data[k], o + 8);
            o += 8 + x.data[k].length;
          }
          net.notify(f);
        }
      }
      if (now - lastHb > 1000) {
        lastHb = now;
        const [f, dv] = frame(F_HEARTBEAT, 4);
        dv.setUint32(2, netMs(), true);
        net.notify(f);
      }
    }, TICK);
    return device;
  }

  for (const d of DEMOS) makeSim(d);
  let lastStep = performance.now();
  setInterval(() => {
    const now = performance.now();
    for (const x of sims) { x.step(now - lastStep); x.publish(false); }
    lastStep = now;
  }, TICK);

  document.getElementById("demo-note").hidden = false;
  document.getElementById("demo-link").hidden = true;
  document.getElementById("unsupported").style.display = "none";
  document.getElementById("add").disabled = true;
  for (const x of sims) if (x.d.direct) new Link(makeDevice(x));
  // Like a real game, a won button stays won (nobody resets mid-game). The demo starts a new game on all
  // of them together once one has been won for a minute, or once 3 have stopped (won or paused) for 20 s.
  // Not while game-paused, so the game pause can be tried.
  let stoppedSince = 0;
  setInterval(() => {
    const now = performance.now();
    if (sims.some(x => x.s.gamePaused)) { stoppedSince = 0; return; }
    const stopped = sims.filter(x => x.s.won || !x.s.running).length >= 3;
    stoppedSince = stopped ? stoppedSince || now : 0;
    if (sims.some(x => x.s.won && now - x.s.wonAt > 60000) || (stoppedSince && now - stoppedSince > 20000)) {
      sims.forEach(x => { x.newGame(); x.publish(true); });
      stoppedSince = 0;
    }
  }, 1000);
})();
