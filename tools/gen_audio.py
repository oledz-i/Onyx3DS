#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Synthesises the original menu music loops and UI sounds for ONYX 3DS.

Everything is generated from scratch (FM electric piano, soft bass, bells,
brushes, pads), so the audio is original and freely redistributable with the
app. Requires numpy and ffmpeg (for the AAC .m4a files).
Run from the repo root:  python3 tools/gen_audio.py
"""
import os
import random
import subprocess
import wave

import numpy as np

SR = 44100
OUT = os.path.join(os.path.dirname(__file__), "..", "app", "ONYX3DS", "Assets", "Audio")


def midi_hz(n):
    return 440.0 * 2 ** ((n - 69) / 12)


def env_adsr(n, a, d, s, r, sr=SR):
    t = np.arange(n) / sr
    e = np.ones(n) * s
    a_n, d_n = int(a * sr), int(d * sr)
    if a_n:
        e[:a_n] = np.linspace(0, 1, a_n)
    if d_n:
        e[a_n:a_n + d_n] = np.linspace(1, s, min(d_n, max(0, n - a_n)))[: max(0, min(d_n, n - a_n))]
    r_n = int(r * sr)
    if r_n and n > r_n:
        e[-r_n:] *= np.linspace(1, 0, r_n)
    return e


def ep_note(freq, dur, vel=0.6):
    """FM electric piano: bright attack mellowing into a sine with tremolo."""
    n = int(dur * SR)
    t = np.arange(n) / SR
    idx = 2.2 * np.exp(-t * 6.0) + 0.25
    mod = np.sin(2 * np.pi * freq * 1.0 * t)
    car = np.sin(2 * np.pi * freq * t + idx * mod)
    tine = np.sin(2 * np.pi * freq * 14.0 * t) * np.exp(-t * 40) * 0.08
    amp = np.exp(-t * 1.6) * (1 - np.exp(-t * 400))
    trem = 1 + 0.06 * np.sin(2 * np.pi * 4.8 * t)
    out = (car + tine) * amp * trem * vel
    out[-min(n, 2000):] *= np.linspace(1, 0, min(n, 2000))
    return out


def bass_note(freq, dur, vel=0.7):
    n = int(dur * SR)
    t = np.arange(n) / SR
    s = np.sin(2 * np.pi * freq * t) + 0.25 * np.sin(2 * np.pi * freq * 2 * t)
    amp = np.exp(-t * 2.2) * (1 - np.exp(-t * 300))
    out = s * amp * vel
    out[-min(n, 1500):] *= np.linspace(1, 0, min(n, 1500))
    return out


def bell_note(freq, dur, vel=0.35):
    n = int(dur * SR)
    t = np.arange(n) / SR
    partials = [(1.0, 1.0, 3.5), (2.76, 0.45, 6), (5.40, 0.22, 9), (8.93, 0.10, 14)]
    s = sum(a * np.sin(2 * np.pi * freq * m * t) * np.exp(-t * d) for m, a, d in partials)
    return s * vel * (1 - np.exp(-t * 900))


def pad_note(freq, dur, vel=0.18, cutoff=1800.0):
    n = int(dur * SR)
    t = np.arange(n) / SR
    s = np.zeros(n)
    for det in (-0.12, 0.0, 0.11):
        f = freq * 2 ** (det / 12)
        s += 2 * ((t * f) % 1.0) - 1  # saw
    # one-pole low-pass
    a = np.exp(-2 * np.pi * cutoff / SR)
    y = np.empty_like(s)
    acc = 0.0
    for i in range(n):
        acc = (1 - a) * s[i] + a * acc
        y[i] = acc
    return y * env_adsr(n, 0.6, 0.4, 0.8, 0.9) * vel / 3


def noise_hit(dur, center, width, vel, seed):
    rng = np.random.default_rng(seed)
    n = int(dur * SR)
    x = rng.standard_normal(n)
    spec = np.fft.rfft(x)
    f = np.fft.rfftfreq(n, 1 / SR)
    spec *= np.exp(-((f - center) / width) ** 2)
    y = np.fft.irfft(spec, n)
    y /= np.max(np.abs(y)) + 1e-9
    t = np.arange(n) / SR
    return y * np.exp(-t * 28) * vel


def thump(dur, vel):
    n = int(dur * SR)
    t = np.arange(n) / SR
    f = 55 + 70 * np.exp(-t * 30)
    return np.sin(2 * np.pi * np.cumsum(f) / SR) * np.exp(-t * 9) * vel


def add(buf, start_s, sig):
    i = int(start_s * SR)
    j = i + len(sig)
    if j <= len(buf):
        buf[i:j] += sig
    else:  # wrap around: keeps the loop seamless
        k = len(buf) - i
        buf[i:] += sig[:k]
        buf[: j - len(buf)] += sig[k:]


def reverb_loop(x, decay=1.8, mix=0.22, seed=3):
    """Circular convolution with a noise tail, so the reverb wraps across the
    loop point instead of cutting off."""
    rng = np.random.default_rng(seed)
    n = int(decay * SR)
    t = np.arange(n) / SR
    ir = rng.standard_normal(n) * np.exp(-t * 6.9 / decay)
    ir /= np.sqrt(np.sum(ir ** 2))
    L = len(x)
    out = np.zeros_like(x)
    for ch in range(x.shape[1]):
        ir_pad = np.zeros(L)
        ir_pad[: min(n, L)] = ir[: min(n, L)]
        out[:, ch] = np.real(np.fft.ifft(np.fft.fft(x[:, ch]) * np.fft.fft(ir_pad)))
    return x * (1 - mix) + out * mix * 2.2


def pan(sig, p):
    left = np.cos((p + 1) * np.pi / 4)
    right = np.sin((p + 1) * np.pi / 4)
    return np.stack([sig * left, sig * right], axis=1)


def master(stereo, peak=0.82):
    # gentle soft clip + normalise
    stereo = np.tanh(stereo * 1.2) / np.tanh(1.2)
    return stereo / (np.max(np.abs(stereo)) + 1e-9) * peak


def write_wav(path, stereo, sr=SR):
    data = (np.clip(stereo, -1, 1) * 32767).astype(np.int16)
    with wave.open(path, "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(sr)
        w.writeframes(data.tobytes())


def encode_m4a(wav_path, m4a_path):
    subprocess.run(["ffmpeg", "-y", "-loglevel", "error", "-i", wav_path, "-c:a", "aac", "-b:a", "128k",
                    "-movflags", "+faststart", m4a_path], check=True)


# ---------------------------------------------------------------------------
# Songs. Chords are lists of MIDI notes; each chord lasts one bar.

def lowpass(x, cutoff):
    """One-pole low-pass (vectorised)."""
    from scipy.signal import lfilter
    a = np.exp(-2 * np.pi * cutoff / SR)
    return lfilter([1 - a], [1, -a], x)


def piano_note(freq, dur, vel=0.3, release=0.25):
    """Soft felt piano: slightly inharmonic partials whose highs fade first,
    a quiet hammer knock and a short damper release."""
    n = int((dur + release) * SR)
    t = np.arange(n) / SR
    out = np.zeros(n)
    B = 0.0004
    for k in range(1, 9):
        fk = freq * k * np.sqrt(1 + B * k * k)
        if fk > 16000:
            break
        amp = (0.9 ** (k - 1)) / k ** 0.7
        decay = 0.9 + 0.55 * k + freq / 900
        out += amp * np.sin(2 * np.pi * fk * t + k * 0.3) * np.exp(-t * decay)
    out *= (1 - np.exp(-t * 900))
    knock = np.random.default_rng(int(freq)).standard_normal(n) * np.exp(-t * 120) * 0.02
    out = out + lowpass(knock, 2500)
    gate = np.ones(n)
    off = int(dur * SR)
    if off < n:
        gate[off:] = np.exp(-np.arange(n - off) / SR * (4.6 / release))
    return out * gate * vel


def marimba_note(freq, dur, vel=0.3):
    n = int(dur * SR)
    t = np.arange(n) / SR
    s = (np.sin(2 * np.pi * freq * t) * np.exp(-t * 7)
         + 0.35 * np.sin(2 * np.pi * freq * 3.93 * t) * np.exp(-t * 22)
         + 0.12 * np.sin(2 * np.pi * freq * 9.2 * t) * np.exp(-t * 45))
    return s * (1 - np.exp(-t * 1500)) * vel


def soft_pad(freq, dur, vel=0.12, cutoff=1100.0, attack=1.2, release=1.6):
    n = int((dur + release) * SR)
    t = np.arange(n) / SR
    s = np.zeros(n)
    for det, ph in ((-0.09, 0.0), (0.0, 1.3), (0.08, 2.1)):
        f = freq * 2 ** (det / 12)
        s += np.sin(2 * np.pi * f * t + ph) + 0.35 * np.sin(4 * np.pi * f * t + ph) + 0.15 * (2 * ((t * f + ph) % 1.0) - 1)
    s = lowpass(s, cutoff)
    e = np.minimum(1, t / attack)
    off = int(dur * SR)
    e[off:] *= np.exp(-np.arange(n - off) / SR * (4.6 / release))
    swell = 1 + 0.08 * np.sin(2 * np.pi * 0.11 * t)
    return s * e * swell * vel / 3


def echo_loop(x, delay_s, feedback=0.35, taps=4):
    """Circular feedback delay (wraps across the loop point)."""
    out = x.copy()
    d = int(delay_s * SR)
    g = 1.0
    for i in range(1, taps + 1):
        g *= feedback
        out += np.roll(x, d * i, axis=0) * g
    return out


def play_melody(buf, notes, spb, beats, voice, transpose=0, vel=0.3, swing=0.0):
    for bar, pos, note, length in notes:
        sw = swing if (pos * 2) % 2 == 1 else 0.0
        add(buf, (bar * beats + pos + sw) * spb, voice(midi_hz(note + transpose), length * spb, vel))


def song_aero():
    """'Plaza': calm, bouncy home-menu loop in F major, 104 BPM, in the spirit
    of the classic console menus: felt piano melody, staccato off-beat
    chords, plucked bass, a whisper of pad and shaker. Original composition."""
    bpm, beats = 104, 4
    spb = 60 / bpm
    # one chord per half bar
    A = [("F", [53, 57, 60, 64])] * 2 + [("Am", [57, 60, 64, 67])] * 2 + \
        [("Bb", [58, 62, 65, 69])] * 2 + [("C6", [60, 64, 67, 69])] * 2 + \
        [("F", [53, 57, 60, 64])] * 2 + [("Dm", [50, 53, 57, 60])] * 2 + \
        [("Gm", [55, 58, 62, 65])] * 2 + [("Csus", [48, 53, 55, 58])] * 2
    B = [("Bb", [58, 62, 65, 69])] * 2 + [("Am", [57, 60, 64, 67])] * 2 + \
        [("Gm", [55, 58, 62, 65])] * 2 + [("F", [53, 57, 60, 64])] * 2 + \
        [("Bb", [58, 62, 65, 69])] * 2 + [("A7", [57, 61, 64, 67])] * 2 + \
        [("Dm", [50, 53, 57, 60]), ("G7", [55, 59, 62, 65])] + [("Csus", [48, 53, 55, 58]), ("C7", [48, 52, 55, 58])]
    halves = A + A + B + A
    bars = len(halves) // 2
    L = int(bars * beats * spb * SR)
    piano, chords, bass, pad, shaker, sparkle = (np.zeros(L) for _ in range(6))
    melA = [(0, 0, 69, .5), (0, .5, 72, .5), (0, 1, 76, 1), (0, 2.5, 74, .5), (0, 3, 72, 1),
            (1, .5, 76, .5), (1, 1, 76, .5), (1, 1.5, 72, .5), (1, 2, 69, 1.5),
            (2, 0, 74, .5), (2, .5, 77, .5), (2, 1, 81, 1), (2, 2.5, 79, .5), (2, 3, 77, 1),
            (3, 0, 76, 1.5), (3, 1.5, 74, .5), (3, 2, 72, 2),
            (4, 0, 69, .5), (4, .5, 72, .5), (4, 1, 77, 1), (4, 2.5, 76, .5), (4, 3, 77, .5), (4, 3.5, 79, .5),
            (5, 0, 81, 1.5), (5, 1.5, 77, .5), (5, 2, 74, 2),
            (6, .5, 70, .5), (6, 1, 74, .5), (6, 1.5, 77, .5), (6, 2, 76, 1), (6, 3, 74, 1),
            (7, 0, 72, 3)]
    melB = [(0, 0, 74, 1), (0, 1, 72, .5), (0, 1.5, 74, .5), (0, 2, 77, 1.5),
            (1, 0, 76, 1), (1, 1, 74, .5), (1, 1.5, 76, .5), (1, 2, 72, 1.5),
            (2, 0, 70, .5), (2, .5, 74, .5), (2, 1, 79, 1), (2, 2, 77, .5), (2, 2.5, 74, .5), (2, 3, 70, 1),
            (3, 0, 69, 3),
            (4, 0, 77, .5), (4, .5, 79, .5), (4, 1, 81, 1), (4, 2, 79, .5), (4, 2.5, 77, .5), (4, 3, 74, 1),
            (5, 0, 73, 1), (5, 1, 76, 1), (5, 2, 79, 1.5),
            (6, 0, 77, 1), (6, 1, 74, 1), (6, 2, 71, 1), (6, 3, 74, 1),
            (7, 0, 76, 2)]
    mel = []
    for section, start in ((melA, 0), (melA, 8), (melB, 16), (melA, 24)):
        mel += [(b + start, p, n, d) for b, p, n, d in section]
    play_melody(piano, mel, spb, beats, lambda f, d, v: piano_note(f, d * 0.92, v), vel=0.34, swing=0.06)
    play_melody(sparkle, [m for m in mel if m[3] >= 1], spb, beats,
                lambda f, d, v: bell_note(f, 1.4, v), transpose=12, vel=0.05)
    for h, (_, chord) in enumerate(halves):
        t0 = h * 2 * spb
        # staccato off-beat chord on the half bar's second beat, root-position voicing
        for i, note in enumerate(chord):
            add(chords, t0 + (1 + 0.06) * spb + i * 0.008, piano_note(midi_hz(note), 0.32 * spb, 0.10, 0.12))
        add(pad, t0, np.concatenate([soft_pad(midi_hz(chord[1]), 2 * spb, 0.05, 900, 0.4, 0.6)]))
        root = chord[0]
        while root > 50:
            root -= 12
        add(bass, t0, bass_note(midi_hz(root - 12 + (7 if h % 4 == 3 else 0)), 0.8 * spb, 0.42))
        for e in range(4):
            add(shaker, t0 + (e * 0.5 + (0.06 if e % 2 else 0)) * spb,
                noise_hit(0.07, 7600, 1800, 0.035 if e % 2 else 0.022, 200 + h * 4 + e))
    mix = pan(piano, -0.08) + pan(chords, 0.18) * 0.9 + pan(bass, 0) + pan(pad, -0.2) + \
        pan(shaker, 0.3) + pan(sparkle, 0.35)
    return master(reverb_loop(mix, 2.4, 0.24), 0.78)


def song_midnight():
    """'Afterglow': slow ambient console-dashboard loop, 60 BPM. Swelling
    pads, sparse felt-piano notes with echoes, glassy shimmer, sub bass.
    Original composition."""
    bpm, beats = 60, 4
    spb = 60 / bpm
    prog = [  # two bars each
        [47, 54, 57, 62, 66],   # Bm9
        [43, 50, 54, 59, 62],   # Gmaj7
        [42, 50, 54, 57, 61],   # D/F#
        [40, 47, 54, 55, 59],   # Em9
        [47, 54, 57, 61, 62],   # Bm(add9)
        [43, 50, 55, 57, 62],   # Gmaj9-ish
        [45, 52, 57, 59, 64],   # Aadd9
        [42, 49, 54, 57, 61],   # F#m7
    ]
    bars = len(prog) * 2
    L = int(bars * beats * spb * SR)
    pad, keys, sub, glass = (np.zeros(L) for _ in range(4))
    piano_lines = [  # (beat within the 8-beat chord, chord-tone index, octave shift)
        [(0, 4, 12), (1.5, 3, 12), (3, 2, 12), (5, 4, 12), (6.5, 3, 24)],
        [(0.5, 3, 12), (2, 4, 12), (4, 2, 12), (5.5, 3, 12)],
    ]
    for c, chord in enumerate(prog):
        t0 = c * 2 * beats * spb
        for note in chord[1:]:
            add(pad, t0, soft_pad(midi_hz(note), 2 * beats * spb, 0.10, 800, 2.6, 2.8))
        add(sub, t0, soft_pad(midi_hz(chord[0] - 12), 2 * beats * spb, 0.16, 300, 1.5, 2.0))
        for beat, idx, octv in piano_lines[c % 2]:
            add(keys, t0 + beat * spb, piano_note(midi_hz(chord[idx] + octv), 1.6 * spb, 0.17, 1.2))
        for k in range(4):
            add(glass, t0 + (k * 2 + 1) * spb,
                bell_note(midi_hz(chord[1 + (k + c) % 4] + 36), 3.0, 0.035))
    keys = echo_loop(keys[:, None], 0.75 * spb, 0.38, 4)[:, 0]
    glass = echo_loop(glass[:, None], 1.5 * spb, 0.45, 5)[:, 0]
    mix = pan(pad, 0) + pan(sub, 0) + pan(keys, -0.12) + pan(glass, 0.3)
    return master(reverb_loop(mix, 4.0, 0.42), 0.74)


def song_dual():
    """'Pocket Plaza': bright, bouncy handheld home-menu loop in G major,
    120 BPM: marimba arpeggios, kalimba-like melody, walking bass, brushes
    and soft snaps. Original composition."""
    bpm, beats = 120, 4
    spb = 60 / bpm
    A = [[55, 59, 62, 66], [52, 55, 59, 62], [57, 60, 64, 67], [50, 54, 57, 60],
         [59, 62, 66, 69], [52, 55, 59, 62], [48, 52, 55, 59], [50, 55, 57, 60]]
    Bp = [[48, 52, 55, 59], [59, 62, 66, 69], [57, 60, 64, 67], [55, 59, 62, 66],
          [48, 52, 55, 59], [59, 63, 66, 69], [52, 55, 59, 62], [57, 60, 64, 67]]
    prog = A + A + Bp + A
    L = int(len(prog) * beats * spb * SR)
    mar, mel, bass, perc, ep = (np.zeros(L) for _ in range(5))
    melA = [(0, 0, 71, .5), (0, .5, 74, .5), (0, 1, 78, 1), (0, 2, 76, .5), (0, 2.5, 74, .5), (0, 3, 71, 1),
            (1, 0, 79, 1.5), (1, 1.5, 78, .5), (1, 2, 76, 1), (1, 3, 74, 1),
            (2, 0, 72, .5), (2, .5, 76, .5), (2, 1, 81, 1), (2, 2, 79, .5), (2, 2.5, 76, .5), (2, 3, 72, 1),
            (3, 0, 78, 1), (3, 1, 76, .5), (3, 1.5, 74, .5), (3, 2, 72, 1), (3, 3, 69, 1),
            (4, 0, 74, .5), (4, .5, 78, .5), (4, 1, 81, 1.5), (4, 2.5, 78, .5), (4, 3, 74, 1),
            (5, 0, 71, 1), (5, 1, 74, .5), (5, 1.5, 76, .5), (5, 2, 79, 2),
            (6, 0, 76, .5), (6, .5, 79, .5), (6, 1, 76, .5), (6, 1.5, 74, .5), (6, 2, 72, .5), (6, 2.5, 71, .5), (6, 3, 72, 1),
            (7, 0, 74, 3)]
    melB = [(0, 0, 76, 1), (0, 1, 79, 1), (0, 2, 72, .5), (0, 2.5, 76, .5), (0, 3, 79, 1),
            (1, 0, 78, 1.5), (1, 1.5, 74, .5), (1, 2, 71, 2),
            (2, 0, 69, .5), (2, .5, 72, .5), (2, 1, 76, 1), (2, 2, 79, .5), (2, 2.5, 76, .5), (2, 3, 72, 1),
            (3, 0, 74, 3),
            (4, 0, 79, .5), (4, .5, 81, .5), (4, 1, 79, 1), (4, 2, 76, .5), (4, 2.5, 72, .5), (4, 3, 76, 1),
            (5, 0, 75, 1), (5, 1, 78, 1), (5, 2, 81, 2),
            (6, 0, 79, 1), (6, 1, 76, 1), (6, 2, 73, 1), (6, 3, 76, 1),
            (7, 0, 72, 1.5), (7, 1.5, 69, .5), (7, 2, 78, 2)]
    notes = []
    for section, start in ((melA, 0), (melA, 8), (melB, 16), (melA, 24)):
        notes += [(b + start, p, n, d) for b, p, n, d in section]
    play_melody(mel, notes, spb, beats, lambda f, d, v: bell_note(f, max(0.6, d * 1.2), v), vel=0.26)
    play_melody(mel, notes, spb, beats, lambda f, d, v: marimba_note(f, max(0.4, d), v), vel=0.12)
    arp = [0, 2, 1, 3, 2, 1, 3, 2]
    for bar, chord in enumerate(prog):
        b0 = bar * beats * spb
        for e, idx in enumerate(arp):
            add(mar, b0 + e * 0.5 * spb, marimba_note(midi_hz(chord[idx]), 0.5, 0.13 if e % 2 else 0.17))
        for i, note in enumerate(chord):
            add(ep, b0 + i * 0.01, ep_note(midi_hz(note), 3.6 * spb, 0.06))
        root = chord[0]
        while root > 52:
            root -= 12
        walk = [root, root + 7, root + 12, root + 7] if bar % 2 == 0 else [root, root + 4, root + 7, root + 9]
        for q, n in enumerate(walk):
            add(bass, b0 + q * spb, bass_note(midi_hz(n - 12), 0.85 * spb, 0.42 if q == 0 else 0.32))
        for e in range(8):
            add(perc, b0 + e * 0.5 * spb, noise_hit(0.09, 6800, 2200, 0.05 if e % 2 else 0.035, 400 + bar * 8 + e))
        for q in (1, 3):
            snap = noise_hit(0.12, 2600, 900, 0.10, 900 + bar * 2 + q)
            add(perc, b0 + q * spb, snap)
        add(perc, b0, thump(0.25, 0.12))
    mix = pan(mar, 0.2) + pan(mel, -0.1) + pan(bass, 0) + pan(perc, 0.05) * 0.8 + pan(ep, -0.25)
    return master(reverb_loop(mix, 1.8, 0.22), 0.78)


def song_sunset():
    """Driving 104 BPM synthwave loop in A minor: pad, octave bass, arpeggio."""
    bpm, beats = 104, 4
    prog = [[57, 60, 64], [53, 57, 60], [48, 52, 55], [55, 59, 62]] * 8
    spb = 60 / bpm
    L = int(len(prog) * beats * spb * SR)
    pad, bass, arp, perc = (np.zeros(L) for _ in range(4))
    for bar, chord in enumerate(prog):
        b0 = bar * beats * spb
        for note in chord:
            add(pad, b0, pad_note(midi_hz(note), beats * spb * 1.02, 0.14, 2600))
        for e in range(8):
            n = chord[0] - 24 + (12 if e % 2 else 0)
            add(bass, b0 + e * 0.5 * spb, bass_note(midi_hz(n), 0.45 * spb, 0.45))
        pattern = [0, 1, 2, 1, 0, 2, 1, 2] * 2
        for s, p in enumerate(pattern):
            add(arp, b0 + s * 0.25 * spb, ep_note(midi_hz(chord[p] + 24), 0.22 * spb, 0.12))
        for beat in range(4):
            add(perc, b0 + beat * spb, thump(0.3, 0.35))
        for beat in (1, 3):
            add(perc, b0 + beat * spb, noise_hit(0.3, 2200, 1500, 0.22, bar * 2 + beat))
        for e in range(8):
            add(perc, b0 + (e + 0.5) * 0.5 * spb, noise_hit(0.06, 9000, 2500, 0.05, 300 + e))
    mix = pan(pad, 0) + pan(bass, 0) + pan(arp, 0.2) + pan(perc, 0)
    return master(reverb_loop(mix, 2.0, 0.24))


# ---------------------------------------------------------------------------
# UI sounds

def sfx():
    def mono(sig):
        sig = sig / (np.max(np.abs(sig)) + 1e-9) * 0.7
        return np.stack([sig, sig], axis=1)

    move = bell_note(midi_hz(88), 0.12, 1.0) * np.exp(-np.arange(int(0.12 * SR)) / SR * 30)
    select = np.zeros(int(0.35 * SR))
    add_local = lambda buf, t, s: buf.__setitem__(slice(int(t * SR), int(t * SR) + len(s)),
                                                  buf[int(t * SR):int(t * SR) + len(s)] + s[: len(buf) - int(t * SR)])
    add_local(select, 0.0, bell_note(midi_hz(84), 0.3, 1.0))
    add_local(select, 0.07, bell_note(midi_hz(91), 0.28, 0.9))
    back = np.zeros(int(0.3 * SR))
    add_local(back, 0.0, bell_note(midi_hz(86), 0.25, 0.9))
    add_local(back, 0.06, bell_note(midi_hz(79), 0.24, 0.8))
    launch = np.zeros(int(1.1 * SR))
    for i, n in enumerate([72, 76, 79, 84, 88, 91]):
        add_local(launch, i * 0.06, bell_note(midi_hz(n), 0.9, 0.8 - i * 0.07))
    sweep = noise_hit(1.0, 3000, 2500, 0.25, 42)
    sweep *= np.linspace(0, 1, len(sweep)) ** 0.5
    add_local(launch, 0.0, sweep)
    return {"move": mono(move), "select": mono(select), "back": mono(back), "launch": mono(launch)}


def main():
    os.makedirs(OUT, exist_ok=True)
    tmp = os.path.join(OUT, "_tmp.wav")
    import sys
    songs = (("aero-channel", song_aero), ("midnight-onyx", song_midnight),
             ("sunset-arcade", song_sunset), ("dual-screen", song_dual))
    only = set(sys.argv[1:])
    for name, fn in songs:
        if only and name not in only:
            continue
        data = fn()
        write_wav(tmp, data)
        encode_m4a(tmp, os.path.join(OUT, name + ".m4a"))
        print(f"{name}: {len(data) / SR:.1f}s")
    os.remove(tmp)
    if only and "sfx" not in only:
        return
    for name, data in sfx().items():
        write_wav(os.path.join(OUT, name + ".wav"), data)
        print("sfx", name)


if __name__ == "__main__":
    main()
