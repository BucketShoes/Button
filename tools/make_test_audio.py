"""Turn a 16-bit PCM WAV into the speaker-test flash image (audio/test_audio.bin).

Picks the loudest window, mixes to mono, resamples to 48 kHz if needed, optionally
high-passes and compresses, fades the ends, and writes:

    "PCM1" | u32 sample_rate | u32 sample_count | u32 reserved | int16 LE samples

It also writes a .wav preview next to the .bin with exactly the same samples,
so you can listen on a PC to what the ESP plays (before the RC filter, amp and
speaker).

Pure standard library (no numpy). The WAV can come from tools/transcode.ps1,
which uses Windows' own decoder to turn an MP3 into a WAV.

    python tools/make_test_audio.py music.wav audio/test_audio.bin
        [--seconds 40] [--hp 0] [--compress none|light|heavy]

Defaults (no high-pass, no compression, 48 kHz) give a clean speaker test:
the preview .wav should sound like a mono copy of the source.

--hp is a 4th-order high-pass in Hz (0 = off). Note the firmware's PDM output
also has its own ~35 Hz high-pass, and the amp input coupling is ~16 Hz.
--compress: none = peak-normalise only; light = gentle levelling (max +6 dB)
and a soft limiter; heavy = strong levelling (max +18 dB), driven limiter.
"""
import argparse
import array
import math
import struct
import sys
import wave

OUT_RATE = 48000  # must match SR in src/audio.c; 40 s fits the 4 MB partition
TAPS = 24  # resampler half-width in input samples


def read_window(path, seconds):
    w = wave.open(path, "rb")
    ch, width, rate, n = w.getnchannels(), w.getsampwidth(), w.getframerate(), w.getnframes()
    if width != 2:
        raise SystemExit("need 16-bit WAV")
    raw = w.readframes(n)
    w.close()
    data = array.array("h")
    data.frombytes(raw)
    if sys.byteorder != "little":
        data.byteswap()
    # Mono.
    if ch > 1:
        mono = array.array("f", (sum(data[i:i + ch]) / ch for i in range(0, len(data), ch)))
    else:
        mono = array.array("f", data)
    # Loudest window, scored on 1 s blocks of mean |x| (sampled every 8th).
    blk = rate
    scores = [sum(abs(mono[j]) for j in range(i, min(i + blk, len(mono)), 8)) for i in range(0, len(mono), blk)]
    win = int(seconds)
    best = max(range(max(1, len(scores) - win)), key=lambda s: sum(scores[s:s + win]))
    start = best * blk
    seg = mono[start:start + int(seconds * rate)]
    print(f"source {n / rate:.1f}s {rate} Hz x{ch}; using {start / rate:.1f}s..{(start + len(seg)) / rate:.1f}s")
    return seg, rate


def resample(x, rate_in, rate_out):
    ratio = rate_in / rate_out
    cutoff = min(1.0, rate_out / rate_in) * 0.9  # of input Nyquist
    n_out = int(len(x) / ratio)
    out = array.array("f", bytes(4 * n_out))
    for k in range(n_out):
        t = k * ratio
        c = int(t)
        acc = 0.0
        norm = 0.0
        for j in range(c - TAPS + 1, c + TAPS + 1):
            if 0 <= j < len(x):
                d = t - j
                s = cutoff if d == 0 else math.sin(math.pi * cutoff * d) / (math.pi * d)
                h = s * (0.5 + 0.5 * math.cos(math.pi * d / TAPS))  # Hann
                acc += x[j] * h
                norm += h
        out[k] = acc / norm if norm else 0.0
    return out


def highpass(x, rate, hz):
    # 2nd-order Butterworth biquad, applied twice (4th order).
    w0 = 2 * math.pi * hz / rate
    alpha = math.sin(w0) / math.sqrt(2)
    cw = math.cos(w0)
    b0, b1, b2 = (1 + cw) / 2, -(1 + cw), (1 + cw) / 2
    a0, a1, a2 = 1 + alpha, -2 * cw, 1 - alpha
    b0, b1, b2, a1, a2 = b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0
    for _ in range(2):
        x1 = x2 = y1 = y2 = 0.0
        for i in range(len(x)):
            v = x[i]
            y = b0 * v + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2
            x2, x1, y2, y1 = x1, v, y1, y
            x[i] = y
    return x


COMPRESS = {  # levelling target RMS, max gain, limiter drive
    "light": (0.25, 2.0, 1.0),
    "heavy": (0.50, 8.0, 1.5),
}


def compress(x, rate, mode):
    peak = max(abs(v) for v in x) or 1.0
    if mode == "none":
        for i in range(len(x)):
            x[i] = x[i] / peak * 0.97
        return x
    # RMS levelling towards a target, then a soft (tanh) limiter. The RMS
    # window and the gain smoothing are both much longer than a bass cycle, so
    # the gain doesn't wobble within a waveform (that is audible distortion).
    target, max_gain, drive = COMPRESS[mode]
    att = math.exp(-1 / (0.080 * rate))
    rel = math.exp(-1 / (0.600 * rate))
    gs = math.exp(-1 / (0.050 * rate))
    env = target * target
    g_s = 1.0
    for i in range(len(x)):
        v = x[i] / peak
        e = v * v
        k = att if e > env else rel
        env = k * env + (1 - k) * e
        g = min(max_gain, target / math.sqrt(env + 1e-6))
        g_s = gs * g_s + (1 - gs) * g
        x[i] = math.tanh(v * g_s * drive) / math.tanh(drive) * 0.97
    return x


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("src")
    ap.add_argument("dst")
    ap.add_argument("--seconds", type=float, default=40.0)
    ap.add_argument("--hp", type=float, default=0.0, help="high-pass Hz, 0 = off")
    ap.add_argument("--compress", choices=["none", "light", "heavy"], default="none")
    a = ap.parse_args()
    dst = a.dst
    seg, rate = read_window(a.src, a.seconds)
    y = seg if rate == OUT_RATE else resample(seg, rate, OUT_RATE)
    if a.hp > 0:
        y = highpass(y, OUT_RATE, a.hp)
    y = compress(y, OUT_RATE, a.compress)
    fi, fo = int(0.3 * OUT_RATE), int(2.0 * OUT_RATE)
    for i in range(fi):
        y[i] *= i / fi
    for i in range(fo):
        y[len(y) - 1 - i] *= i / fo
    pcm = array.array("h", (int(max(-32767, min(32767, v * 32767))) for v in y))
    if sys.byteorder != "little":
        pcm.byteswap()
    with open(dst, "wb") as f:
        f.write(b"PCM1" + struct.pack("<III", OUT_RATE, len(pcm), 0))
        f.write(pcm.tobytes())
    preview = dst.rsplit(".", 1)[0] + ".wav"
    w = wave.open(preview, "wb")
    w.setnchannels(1)
    w.setsampwidth(2)
    w.setframerate(OUT_RATE)
    w.writeframes(pcm.tobytes())
    w.close()
    rms = math.sqrt(sum(v * v for v in pcm[::7]) / len(pcm[::7]))
    peak = max(abs(v) for v in pcm)
    print(f"wrote {dst}: {len(pcm) / OUT_RATE:.1f}s at {OUT_RATE} Hz, {16 + 2 * len(pcm)} bytes; "
          f"hp {a.hp:g} Hz, compress {a.compress}; peak {peak}, RMS {rms:.0f} "
          f"(crest {20 * math.log10(peak / rms):.1f} dB)")
    print(f"preview {preview}")


if __name__ == "__main__":
    main()
