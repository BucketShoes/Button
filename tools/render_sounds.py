#!/usr/bin/env python3
"""Render the button's synth sounds to WAV files, to listen on a PC.

A port of the synth in src/audio.c (the instruments and the mixer) and of the themed sounds in src/main.c
(the build-up, the capture calls, the countdown and the win). It follows the C step by step, integer
maths included, so the files are what the button sends to its amp, before the speaker. Keep it in step
with those files when the sounds change.

    py -3 tools/render_sounds.py            # writes audio/preview/*.wav
    py -3 tools/render_sounds.py OUTDIR

Full volume (the button's master gain at 255). The page's volume setting only scales this.
"""
import math
import os
import struct
import sys
import wave

SR = 48000
BLOCK = SR // 100
SQ_AMP = 28000
ATTACK = SR // 500
RELEASE = SR // 125
CTRL = 32
SINE_BITS = 10
M32 = 0xFFFFFFFF

(W_SQUARE, W_TRI, W_SINE, W_NOISE, W_BELL, W_TRUMPET, W_HORN, W_FLUTE, W_DRUM, W_SNARE, W_TIMPANI,
 W_SWELL) = range(12)

SINE_TAB = [int(math.sin(i * 2 * math.pi / (1 << SINE_BITS)) * 32767.0) for i in range(1 << SINE_BITS)]


def sine(ph):
    return SINE_TAB[(ph & M32) >> (32 - SINE_BITS)]


def cdiv(a, b):  # C integer division (truncates towards zero)
    q = abs(a) // abs(b)
    return q if (a >= 0) == (b >= 0) else -q


def s16(v):
    v &= 0xFFFF
    return v - 0x10000 if v & 0x8000 else v


def N(f0, f1, ms, wave_=W_SQUARE, vol=0):
    return (f0, f1, ms, wave_, vol)


def decay_k(ms, to):
    steps = ms * (SR / 1000.0) / CTRL
    return to if steps < 1 else math.exp(math.log(to) / steps)


class Chan:
    def __init__(self, notes, slot):
        self.notes, self.n, self.idx = notes, len(notes), 0
        self.pos = self.len = 0
        self.phase = self.inc = 0
        self.lfsr = 0x12345 + slot
        self.active = self.n > 0
        self.phm = self.incm = self.inc_eff = self.vib = 0
        self.a = self.ak = self.q = self.qk = self.nz = self.nzk = 1.0
        self.amp_t = self.amp_n = self.dev = 0
        if self.active:
            self.note_begin()

    def nt(self):
        return self.notes[self.idx]

    def note_begin(self):
        nt = self.nt()
        self.pos = 0
        self.len = max(1, nt[2] * SR // 1000)
        if nt[3] >= W_BELL:
            self.inst_begin(nt)

    def update_inc(self):
        f0, f1 = self.nt()[0], self.nt()[1]
        f = f0 + cdiv((f1 - f0) * self.pos, self.len)
        self.inc = ((max(f, 0)) << 32) // SR

    def inst_begin(self, nt):
        ms = nt[2] or 1
        self.a = self.q = self.nz = 1.0
        self.ak = self.qk = self.nzk = 1.0
        self.vib = 0
        self.phase = self.phm = 0
        w = nt[3]
        if w == W_BELL:
            self.ak, self.qk = decay_k(ms, 0.006), decay_k(ms * 0.35, 0.006)
        elif w == W_DRUM:
            self.ak, self.qk, self.nzk = decay_k(ms, 0.006), decay_k(25, 0.05), decay_k(30, 0.01)
        elif w == W_SNARE:
            self.ak, self.qk, self.nzk = decay_k(ms * 0.5, 0.006), decay_k(15, 0.05), decay_k(ms, 0.006)
        elif w == W_TIMPANI:
            self.ak, self.qk, self.nzk = decay_k(ms, 0.006), decay_k(80, 0.05), decay_k(12, 0.01)

    def sus_env(self, attack_ms, release_ms):
        t = self.pos * (1000.0 / SR)
        rem = (self.len - self.pos) * (1000.0 / SR)
        e = t / attack_ms if t < attack_ms else 1.0 - 0.15 * (t - attack_ms) / (t - attack_ms + 600.0)
        if rem < release_ms:
            e *= rem / release_ms
        return max(e, 0.0)

    def inst_ctrl(self, nt):
        t = self.pos * (1000.0 / SR)
        pitch, index, tone, noise, ratio, vib_depth = 1.0, 0.0, 0.0, 0.0, 1.0, 0.0
        w = nt[3]
        if w == W_BELL:
            ratio = 1.4
            tone = self.a * (t / 1.5 if t < 1.5 else 1.0)
            index = 0.8 + 7.0 * self.q
        elif w == W_TRUMPET:
            e = self.sus_env(35, 50)
            tone, index = e, 0.7 + 4.3 * e * e
            pitch = 0.97 + 0.03 * t / 35 if t < 35 else 1.0
            vib_depth = 0.004 if t > 300 else 0.0
            noise = 0.03 * e
        elif w == W_HORN:
            e = self.sus_env(90, 90)
            tone, index = e, 0.3 + 2.0 * e * e
            pitch = 0.94 + 0.06 * t / 90 if t < 90 else 1.0
            vib_depth = 0.003 if t > 400 else 0.0
            noise = 0.08 * e
        elif w == W_SWELL:
            e = self.sus_env(self.len * (1000.0 / SR), 1)
            e = e * e
            tone, index, noise = e, 0.3 + 3.0 * e, 0.08 * e
        elif w == W_FLUTE:
            e = self.sus_env(70, 60)
            tone, index, ratio = e, 0.35 * e, 2.0
            vib_depth = 0.006 if t > 180 else 0.0
            noise = 0.10 * e
        elif w == W_DRUM:
            tone, pitch, index, noise = self.a, 1.0 + 0.9 * self.q, 0.6 * self.q, 0.7 * self.nz
        elif w == W_SNARE:
            tone, pitch, noise = 0.5 * self.a, 1.0 + 0.6 * self.q, 1.0 * self.nz
        elif w == W_TIMPANI:
            tone, pitch = self.a, 1.0 + 0.04 * self.q
            index, ratio, noise = 0.9 * self.q + 0.2 * self.a, 1.5, 0.25 * self.nz
        self.a *= self.ak
        self.q *= self.qk
        self.nz *= self.nzk
        if vib_depth > 0:
            self.vib = (self.vib + int(5.2 * 4294967296.0 / (SR / CTRL))) & M32
            pitch *= 1.0 + vib_depth * sine(self.vib) / 32767.0
        rem = self.len - self.pos
        rel = rem / (SR / 200.0) if rem < SR // 200 else 1.0
        self.inc_eff = int(self.inc * pitch) & M32
        self.incm = int(self.inc_eff * ratio) & M32
        self.dev = int(index * 20861.0) & M32
        self.amp_t = int(tone * rel * 30000.0)
        self.amp_n = int(noise * rel * 30000.0)

    def noise(self):
        x = self.lfsr
        x ^= (x << 13) & M32
        x ^= x >> 17
        x ^= (x << 5) & M32
        self.lfsr = x
        return s16(x)

    def inst_sample(self):
        m = sine(self.phm)
        self.phm = (self.phm + self.incm) & M32
        v = (sine(self.phase + ((m * self.dev) & M32)) * self.amp_t) >> 15
        self.phase = (self.phase + self.inc_eff) & M32
        if self.amp_n:
            v += (self.noise() * self.amp_n) >> 15
        return v

    def wave_sample(self, w):
        if w == W_SQUARE:
            return SQ_AMP if self.phase & 0x80000000 else -SQ_AMP
        if w == W_TRI:
            p = self.phase >> 16
            return 2 * p - 32768 if p < 32768 else 98303 - 2 * p
        if w == W_SINE:
            return cdiv(sine(self.phase) * 3, 4)
        return cdiv(self.noise() * 3, 4)

    def render(self, acc):  # one block, as render_block does for one slot
        if not self.active:
            return
        self.update_inc()
        for i in range(BLOCK):
            nt = self.nt()
            if nt[0] and nt[3] >= W_BELL:
                if self.pos % CTRL == 0:
                    self.inst_ctrl(nt)
                acc[i] += (self.inst_sample() * nt[4]) >> 8
            elif nt[0]:
                rem = self.len - self.pos
                env = 256
                if self.pos < ATTACK:
                    env = self.pos * 256 // ATTACK
                if rem < RELEASE and rem * 256 // RELEASE < env:
                    env = rem * 256 // RELEASE
                acc[i] += (self.wave_sample(nt[3]) * nt[4] * env) >> 16
                self.phase = (self.phase + self.inc) & M32
            self.pos += 1
            if self.pos >= self.len:
                self.idx += 1
                if self.idx >= self.n:
                    self.active = False
                    break
                self.note_begin()
                self.update_inc()
            elif self.pos & 127 == 0:
                self.update_inc()


LIM_T, LIM_R = 20000, 32767 - 20000


def limit(v):  # the soft limiter at the end of render_block
    a = abs(v)
    if a > LIM_T:
        d = a - LIM_T
        a = LIM_T + LIM_R * d // (LIM_R + d)
        v = -a if v < 0 else a
    return v


def render(events, total_ms=None):
    """events: (start_ms, slot, notes). A later event on the same slot replaces what it was playing."""
    events = sorted(events, key=lambda e: e[0])
    chans, out, t, k = {}, [], 0, 0
    end = total_ms or max(s + sum(n[2] for n in notes) for s, _, notes in events) + 300
    while t < end:
        while k < len(events) and events[k][0] <= t:
            _, slot, notes = events[k]
            chans[slot] = Chan(notes, slot)
            k += 1
        acc = [0] * BLOCK
        for c in chans.values():
            c.render(acc)
        out.extend(limit(v) for v in acc)
        t += 10
    return out


def write_wav(path, samples):
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(SR)
        w.writeframes(struct.pack("<%dh" % len(samples), *samples))


# ------------------------------------------------------------------ the sounds (src/main.c)
# Slots (the order in audio.h)
SLOT_CLICK, SLOT_SW1, SLOT_SW2, SLOT_SW3, SLOT_BUILD, SLOT_BUILD2, SLOT_EVENT, SLOT_EVENT2, SLOT_DIAG = range(9)

SND_TEAM = [
    [N(1047, 1047, 100, W_SQUARE, 255), N(1319, 1319, 100, W_SQUARE, 255), N(1568, 1568, 100, W_SQUARE, 255),
     N(2093, 2093, 350, W_SQUARE, 255), N(0, 0, 1)],
    [N(880, 880, 90, W_SQUARE, 255), N(0, 0, 40), N(880, 880, 90, W_SQUARE, 255), N(0, 0, 40),
     N(1319, 1319, 400, W_SQUARE, 255)],
]
SND_WIN = [N(784, 784, 150, W_SQUARE, 255), N(1047, 1047, 150, W_SQUARE, 255), N(1319, 1319, 150, W_SQUARE, 255),
           N(1568, 1568, 500, W_SQUARE, 255), N(0, 0, 100), N(1319, 1319, 150, W_SQUARE, 255),
           N(1568, 1568, 800, W_SQUARE, 255)]
BLIP_AT = [60, 30, 20, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1]


def scifi_blip(i):
    pulses = 1 if i == 0 else 2 if i < 3 else 3 if i < 8 else 4
    f, on, gap = 880 + i * 130, 90 - i * 5, 60 - i * 3
    b = []
    for p in range(pulses):
        b.append(N(f, f, on, W_SQUARE, 255))
        if p < pulses - 1:
            b.append(N(0, 0, gap))
    return b


def scifi_build(ms):
    return [(0, SLOT_BUILD, [N(250, 1200, ms, W_SQUARE, 140)])]


MED_TEAM = [
    [N(392, 392, 120, W_TRUMPET, 255), N(0, 0, 25), N(392, 392, 120, W_TRUMPET, 255), N(0, 0, 25),
     N(523, 523, 140, W_TRUMPET, 255), N(659, 659, 140, W_TRUMPET, 255), N(784, 784, 650, W_TRUMPET, 255)],
    [N(440, 440, 300, W_HORN, 255), N(0, 0, 40), N(659, 659, 300, W_HORN, 255), N(0, 0, 40),
     N(880, 880, 800, W_HORN, 255)],
]
MED_TEAM_DRUM = [N(196, 196, 500, W_TIMPANI, 255)]
MED_WIN = [N(523, 523, 130, W_TRUMPET, 255), N(0, 0, 20), N(523, 523, 130, W_TRUMPET, 255), N(0, 0, 20),
           N(523, 523, 130, W_TRUMPET, 255), N(659, 659, 450, W_TRUMPET, 255), N(0, 0, 60),
           N(587, 587, 150, W_TRUMPET, 255), N(659, 659, 150, W_TRUMPET, 255), N(784, 784, 1100, W_TRUMPET, 255)]
MED_WIN_BELLS = [N(196, 196, 300, W_TIMPANI, 255), N(196, 196, 300, W_TIMPANI, 200), N(262, 262, 520, W_TIMPANI, 255),
                 N(1047, 1047, 500, W_BELL, 200), N(784, 784, 500, W_BELL, 200), N(1047, 1047, 2200, W_BELL, 230)]
MED_BLIP_SECOND = [N(0, 0, 170), N(1047, 1047, 750, W_BELL, 255)]
MED_CANCEL = [N(440, 311, 380, W_HORN, 150)]


def med_blip(sec):
    if sec == 60:
        return [N(440, 440, 700, W_HORN, 255)]
    if sec >= 20:
        n = 2 if sec == 30 else 3
        return [N(659, 659, 260 if p < n - 1 else 900, W_BELL, 255) for p in range(n)]
    if sec > 3:
        f = 784 + (10 - sec) * 40
        return [N(f, f, 700, W_BELL, 255)]
    return [N(1047, 1047, 800, W_BELL, 255)]


def med_build(ms):
    scale = [440, 494, 523, 587, 659, 698, 831]
    opening = min(max(ms * 3 // 10, 400), 1000, ms)
    drums, t, roll_at = [], 0, ms * 4 // 5
    while t < opening:
        r = min(opening - t, 45)
        drums.append(N(330, 330, r, W_SNARE, 25 + 175 * t // opening))
        t += r
    beat = 0
    while t < ms and len(drums) < 398:
        step = 420 - 220 * t // ms
        vol = 190 + 65 * t // ms
        if t >= roll_at:
            r = max(step // 4, 35)
            r = min(r, ms - t)
            drums.append(N(330, 330, r, W_SNARE, vol))
            t += r
            beat += 1
            continue
        bar, half = beat % 4 == 0, t >= ms // 2
        ln = min(step // 2 if half else step, ms - t)
        f = 165 if bar else 220
        drums.append(N(f, f, ln, W_DRUM, vol if bar else vol * 4 // 5))
        t += ln
        if half and t < ms:
            l2 = min(step - ln, ms - t)
            drums.append(N(300, 300, l2, W_SNARE, vol * 3 // 5))
            t += l2
        beat += 1
    if t < ms:
        drums.append(N(0, 0, ms - t))
    climb = [N(370, 440, opening, W_SWELL, 120)]
    rest = ms - opening
    if rest > 0:
        k = min(max(rest // 800, 1), 6)
        done = 0
        for i in range(k):
            e = rest * (i + 1) // k
            f = scale[7 - k + i]
            climb.append(N(f, f, e - done, W_HORN, 50 + 60 * (i + 1) // k))
            done = e
    return [(0, SLOT_BUILD, climb), (0, SLOT_BUILD2, drums)]


def countdown(blip_fn, extra_fn, secs):
    ev, t = [], 0
    for s in secs:
        ev.append((t, SLOT_DIAG, blip_fn(s)))
        ev += [(t, slot, notes) for slot, notes in extra_fn(s)]
        t += 1000
    return ev


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), "..", "audio", "preview")
    os.makedirs(out, exist_ok=True)
    files = {}
    for team in (0, 1):
        files[f"medieval_team{team + 1}_capture"] = [(0, SLOT_EVENT, MED_TEAM[team]), (0, SLOT_EVENT2, MED_TEAM_DRUM)]
        files[f"scifi_team{team + 1}_capture"] = [(0, SLOT_EVENT, SND_TEAM[team])]
    for hold in (3000, 8000, 20000):
        s = hold // 1000
        # The build-up and the capture call that ends it, as in a game (team 1)
        files[f"medieval_hold_{s}s"] = med_build(hold) + [(hold, SLOT_EVENT, MED_TEAM[0]), (hold, SLOT_EVENT2, MED_TEAM_DRUM)]
        files[f"scifi_hold_{s}s"] = scifi_build(hold) + [(hold, SLOT_EVENT, SND_TEAM[0])]
    files["medieval_let_go_at_1.2s"] = med_build(3000) + [(1200, SLOT_BUILD, MED_CANCEL), (1200, SLOT_BUILD2, [])]
    files["medieval_win"] = [(0, SLOT_EVENT, MED_WIN), (0, SLOT_EVENT2, MED_WIN_BELLS)]
    files["scifi_win"] = [(0, SLOT_EVENT, SND_WIN)]
    secs = [60, 30, 20, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1]
    second = lambda s: [(SLOT_EVENT2, MED_BLIP_SECOND)] if s <= 3 else []
    files["medieval_countdown_60_30_20"] = countdown(med_blip, second, [60, 30, 20])
    files["medieval_countdown_10_to_1"] = countdown(med_blip, second, secs[3:])
    files["scifi_countdown_60_30_20"] = countdown(lambda s: scifi_blip(secs.index(s)), lambda s: [], [60, 30, 20])
    files["scifi_countdown_10_to_1"] = countdown(lambda s: scifi_blip(secs.index(s)), lambda s: [], secs[3:])
    for name, ev in files.items():
        path = os.path.join(out, name + ".wav")
        write_wav(path, render(ev))
        print(path)


if __name__ == "__main__":
    main()
