// Demo mode (index.html?demo): simulated buttons, so the page can be tried without hardware.
// Each one pretends to be a Bluetooth device and goes through the page's normal code path
// (connect, read, notifications, command writes). The rules are a rough copy of the firmware's,
// enough to look right: clocks count down, holds fill and capture, the controls do what they say.
"use strict";
(() => {
  const DEMOS = [
    { name: "Bridge", owner: 0, running: true, time: [512000, 431000], batt: 87 },
    { name: "Shrine", owner: 1, running: true, time: [598000, 264000], batt: 64 },
    { name: "Gate", owner: -1, running: true, time: [600000, 600000], batt: 92 },
    { name: "Tower", owner: 0, running: false, time: [377000, 489000], batt: 41 },
    { name: "Well", owner: 1, running: true, time: [600000, 75000], batt: 18, mode: MODE_ONE, timerTeam: 1 },
  ];
  const TICK = 100, NOTIFY = 250;

  function makeDevice(d) {
    const cfg = {
      mode: d.mode ?? MODE_TWO, timerTeam: d.timerTeam ?? 0, countUp: 0, show: 1, initial: 0, defCapture: 0,
      volume: 128, ledLevel: 128, holdMs: 3000, dur: [600000, 600000], colour: ["#9600ff", "#ffaa00"], name: d.name,
    };
    const s = {
      running: d.running, won: false, gamePaused: false, wasRunning: false, held: false, identifyUntil: 0,
      owner: d.owner, winner: -1, target: d.owner >= 0 ? 1 - d.owner : Math.round(Math.random()), heldMs: 0,
      holdFor: 0, time: d.time.slice(), batt: d.batt, power: 0, wonAt: 0, start: performance.now(),
    };
    const timed = t => cfg.mode === MODE_TWO || (cfg.mode === MODE_ONE && cfg.timerTeam === t);

    function encodeState() {
      const b = new ArrayBuffer(STATE_LEN), v = new DataView(b), now = performance.now();
      const flags = s.running | s.won << 1 | s.gamePaused << 2 | s.held << 3 | (now < s.identifyUntil) << 4;
      v.setUint8(0, PROTO_VERSION); v.setUint8(1, flags);
      v.setInt8(2, s.owner); v.setInt8(3, s.winner); v.setInt8(4, s.target); v.setUint8(5, 0);
      v.setUint16(6, Math.min(65535, s.heldMs), true);
      v.setInt32(8, Math.round(s.time[0]), true); v.setInt32(12, Math.round(s.time[1]), true);
      const mv = 3500 + s.batt * 7;
      v.setUint16(16, mv, true); v.setInt16(20, s.batt, true); v.setUint8(22, 1); v.setUint8(23, 1);
      v.setUint32(24, Math.round((now - s.start) / 1000), true); v.setUint32(28, 1, true);
      v.setUint8(32, s.power); v.setUint8(33, s.batt < 20 ? 2 : 0); v.setUint16(34, mv, true);
      return new DataView(b);
    }

    const chr = kind => ({
      fns: [],
      addEventListener(_, f) { this.fns.push(f); },
      async readValue() { return kind === "state" ? encodeState() : new DataView(encodeConfig(cfg)); },
      async startNotifications() {},
      notify() { const value = kind === "state" ? encodeState() : new DataView(encodeConfig(cfg)); this.fns.forEach(f => f({ target: { value } })); },
      async writeValueWithResponse(data) { onWrite(kind, new DataView(data.buffer || data, data.byteOffset || 0, data.byteLength)); },
    });
    const chrs = { state: chr("state"), config: chr("config"), cmd: chr("cmd") };
    const device = {
      name: d.name, id: "demo-" + d.name, addEventListener() {},
      gatt: {
        connected: false,
        async connect() { device.gatt.connected = true; return server; },
        disconnect() { device.gatt.connected = false; },
      },
    };
    const server = {
      async getPrimaryService() {
        return { async getCharacteristic(u) { return u === CHR_STATE ? chrs.state : u === CHR_CONFIG ? chrs.config : chrs.cmd; } };
      },
    };

    function reset() {
      Object.assign(s, { running: false, won: false, held: false, heldMs: 0, owner: cfg.initial - 1, winner: -1 });
      s.time = cfg.dur.slice();
      s.target = s.owner >= 0 ? 1 - s.owner : Math.round(Math.random());
    }
    function onWrite(kind, v) {
      if (kind === "config") { Object.assign(cfg, parseConfig(v)); chrs.config.notify(); return; }
      const op = v.getUint8(0);
      switch (op) {
        case OP.START: s.running = true; s.gamePaused = false; break;
        case OP.PAUSE: s.running = false; break;
        case OP.RESET: reset(); break;
        case OP.IDENTIFY: s.identifyUntil = performance.now() + v.getUint16(1, true); break;
        case OP.SET_TIME: s.time[v.getUint8(1)] = v.getInt32(2, true); break;
        case OP.ADD_TIME: { const t = v.getUint8(1); s.time[t] = Math.max(0, s.time[t] + v.getInt32(2, true)); break; }
        case OP.SET_OWNER: s.owner = v.getInt8(1); s.won = false; s.target = s.owner >= 0 ? 1 - s.owner : s.target; break;
        case OP.GAME_PAUSE: if (!s.gamePaused) { s.wasRunning = s.running; s.running = false; s.gamePaused = true; s.held = false; } break;
        case OP.GAME_RESUME: if (s.gamePaused) { s.running = s.wasRunning; s.gamePaused = false; } break;
        case OP.POWER_OFF: s.power = 2; s.running = false; s.held = false; break;
        case OP.POWER_ON: s.power = 0; break;
        case OP.FACTORY_RESET: break;
      }
      chrs.state.notify();
    }

    // The game: clocks run, and now and then someone holds the lid (some give up part way).
    function step(dt) {
      if (s.power) return;
      const now = performance.now();
      if (s.running && !s.won && s.owner >= 0 && timed(s.owner)) {
        s.time[s.owner] -= dt;
        if (s.time[s.owner] <= 0) { s.time[s.owner] = 0; s.won = true; s.winner = s.owner; s.running = false; s.wonAt = now; }
      }
      if (s.won && now - s.wonAt > 15000) { reset(); s.time = [180000 + Math.random() * 400000, 180000 + Math.random() * 400000]; s.running = true; }
      if (s.held) {
        s.heldMs += dt;
        if (s.heldMs >= s.holdFor) {
          if (s.holdFor >= cfg.holdMs) {
            s.owner = s.target;  // a paused button's target is its holder, so it resumes with the same team
            s.running = true;
          }
          s.held = false; s.heldMs = 0;
          s.target = s.owner >= 0 ? 1 - s.owner : 1 - s.target;
        }
      } else if (!s.won && !s.gamePaused && Math.random() < dt / 9000) {
        s.held = true; s.heldMs = 0;
        s.holdFor = Math.random() < 0.3 ? cfg.holdMs * (0.3 + Math.random() * 0.5) : cfg.holdMs;
        if (!s.running && s.owner >= 0) s.target = s.owner;
      }
    }
    let lastStep = performance.now(), lastNotify = 0;
    setInterval(() => {
      const now = performance.now();
      step(now - lastStep); lastStep = now;
      if (device.gatt.connected && now - lastNotify >= NOTIFY) { lastNotify = now; chrs.state.notify(); }
    }, TICK);
    return device;
  }

  document.getElementById("demo-note").hidden = false;
  document.getElementById("demo-link").hidden = true;
  document.getElementById("unsupported").style.display = "none";
  document.getElementById("add").disabled = true;
  for (const d of DEMOS) new Btn(makeDevice(d));
})();
