"""Turn a 16-bit PCM WAV into the speaker-test flash image (audio/test_audio.bin).

Picks the loudest SECONDS-long window, mixes to mono, resamples to 24 kHz,
high-passes (the FS-1540 has nothing below a few hundred Hz), compresses and
normalises for loudness, fades the ends, and writes:

    "PCM1" | u32 sample_rate | u32 sample_count | u32 reserved | int16 LE samples

Pure standard library (no numpy). The WAV can come from tools/transcode.ps1,
which uses Windows' own decoder to turn an MP3 into a WAV.

    python tools/make_test_audio.py music.wav audio/test_audio.bin [seconds]
"""
import array
import math
import struct
import sys
import wave

OUT_RATE = 24000
HP_HZ = 250.0
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


def compress(x, rate):
    # Slow RMS levelling towards a loud target, then a soft (tanh) limiter.
    target = 0.50
    att = math.exp(-1 / (0.010 * rate))
    rel = math.exp(-1 / (0.300 * rate))
    peak = max(abs(v) for v in x) or 1.0
    env = 0.0
    for i in range(len(x)):
        v = x[i] / peak
        e = v * v
        env = (att if e > env else rel) * env + (1 - (att if e > env else rel)) * e
        g = min(8.0, target / math.sqrt(env + 1e-6))
        x[i] = math.tanh(v * g * 1.5) * 0.97
    return x


def main():
    src, dst = sys.argv[1], sys.argv[2]
    seconds = float(sys.argv[3]) if len(sys.argv) > 3 else 45.0
    seg, rate = read_window(src, seconds)
    y = resample(seg, rate, OUT_RATE)
    y = highpass(y, OUT_RATE, HP_HZ)
    y = compress(y, OUT_RATE)
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
    print(f"wrote {dst}: {len(pcm) / OUT_RATE:.1f}s at {OUT_RATE} Hz, {16 + 2 * len(pcm)} bytes")


if __name__ == "__main__":
    main()
