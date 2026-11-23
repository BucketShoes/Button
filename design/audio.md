# Audio: how the sound is made

How the pieces fit, and why. The user-facing summary is the README's "Sound sets"; the code is `src/synth.c`, `src/sounds.c`, `src/audio.c` and the sound section of `src/main.c`.

## Signal path

```
main.c (when) ──audio_play(slot, notes)──► queue ──► audio task (audio.c)
                                                      │ every 10 ms block:
sounds.c (what: the player sounds per set)            │   PCM clip (flash partition) ─┐
main.c tables (the admin sounds)                      │   synth_render (synth.c) ─────┤ sum (int32)
                                                      │   synth_out: master gain, soft knee above -2 dBFS → int16
                                                      ▼
                                       I2S PDM TX, 48 kHz, IO18 → RC filter → NS4150C amp → FS-1540
```

- **48 kHz, 16-bit, mono.** That's the highest rate IDF recommends for PDM TX. The PDM bitstream goes through an RC low-pass into the class-D amp's analogue input.
- **Blocks:** the audio task makes one 10 ms block (480 samples) at a time and writes it to the I2S DMA. That's 6 × 5 ms DMA buffers, so there's 30 ms of slack.
- **Thread safety:** `audio_play` only queues a pointer. The task starts it at the next block, so it's safe from any task. The notes are read in place while they play, so they must be static (or rebuilt only when the slot's done).

## Slots (synth channels)

Each slot plays one note sequence at a time, and starting a new one on a slot replaces the old. All the slots are mixed together, so sounds on different slots overlap. The slot is how sounds avoid cutting each other off:

| Slot | Used for |
|---|---|
| CLICK | the press click, dings and dooms during a hold |
| SW1–3 | the per-switch blips (sci-fi only; debug really) |
| BUILD, BUILD2 | the capture build-up: the horn on BUILD, the drums on BUILD2 (sci-fi: the whoop on BUILD) |
| EVENT, EVENT2 | captures, win, pause, reset, page sounds; EVENT2 is the second voice (timpani, bells) |
| DIAG | countdown steps, the BOOT power-off tone |

## Notes and instruments

- **`note_t`:** f0 → f1 Hz (a linear sweep), length in ms, wave, volume 0–255. f0 = 0 is a rest.
- **Plain waves** (square, triangle, sine, noise): a fixed 2 ms attack and 8 ms release, nothing else. These are the sci-fi and admin sounds. Squares carry best on this speaker, whose resonance is about 850 Hz.
- **Instruments** (bell, trumpet, horn, swell, flute, drum, snare, timpani): two-operator FM, a carrier phase-modulated by a sine.
  - The modulator ratio sets the timbre: 1:1 is harmonic (brass), 1.4 inharmonic (bell), 1.5 or 2 for the others.
  - The modulation index sets the brightness, and it follows the envelope. That's what makes brass "blat" as it gets louder and bells clang then go pure.
  - Each instrument has its own envelope, pitch scoop or drop, vibrato and noise (breath, rattle).
- **Struck instruments** (bell, drums, timpani): the note's ms is how long it rings, to about −45 dB. So two quick strikes in one slot make the first die early. Put the strikes on two slots if they need to ring over each other.
- **Integer and float split:**
  - Per sample, everything is integer: 32-bit phase accumulators, a 1024-entry sine table, the FM product left to wrap mod 2³² (which is phase arithmetic), and xorshift noise.
  - The envelopes, index and pitch are floats, updated every 32 samples (0.67 ms).
  - The C6 has no FPU, so those floats are soft float. Per voice that's about 1,500 updates a second, roughly 2–3 % of the CPU per instrument voice.
- **Master stage:** the gain (Q16, set by the volume in dB steps), then a soft knee above −2 dBFS (26028) that runs smoothly towards full scale, and nothing at all below it. Voices that overlap past it bend instead of hard-clipping.

## Where the sounds are defined

- **`sounds.c`, the player-facing sounds per set:** the build-up, let-go, team calls, win and countdown. `main.c` asks for a `sound_t` (one or two voices, each a slot and notes) and plays its voices together.
  - The build-up is generated per hold, so it lasts exactly until the capture. In medieval that's a swell from silence, then an accelerating march under a climbing horn, then a snare roll for the last fifth. In sci-fi it's one sweep stretched to fit.
- **`main.c`, the admin sounds:** pause, reset, ding, doom, power, battery, identify, whistle. They're the same in every set on purpose: plain beeps say "not part of the game".
- **Plain C:** `synth.c` and `sounds.c` don't include ESP-IDF. That's what lets `tools/render_sounds.c` link the very same code on a PC (see its header for the gcc line) and write WAVs of everything in `sounds.c`.

## Power

- **The amp and PDM switch off** 1.1 s after the last sound. A running PDM holds a power-management lock that stops light sleep.
- **Waking them** costs a fixed 120 ms of queued silence, for the amp's turn-on, so every sound after a quiet spell is late by the same amount and none is clipped.
- **Kept awake** in the last minute of a countdown, so the per-second steps have no delay.
- **On battery power down** (VSYS off): sounds are dropped, not queued.

## Not done yet

- **Recorded samples, music and voice:** the PCM partition (4 MB) plays one raw 48 kHz clip. A sound bank (several clips, a lower rate, ADPCM) would allow recorded horns, background music or voice lines.
- **More teams:** sounds.c has two team calls. More teams need more calls, or one call per team chosen from a list.
