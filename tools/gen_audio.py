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

def song_aero():
    """Breezy 92 BPM bossa-ish loop in F major: EP comping, round bass,
    music-box melody and soft brushes."""
    bpm, beats = 92, 4
    prog = [
        [53, 57, 60, 64], [52, 55, 59, 62], [50, 53, 57, 60], [52, 55, 60, 64],
        [46, 50, 53, 57], [45, 48, 52, 55], [43, 46, 50, 53], [48, 53, 55, 58],
    ] * 4  # 32 bars
    roots = [53, 52, 50, 52, 46, 45, 43, 48] * 4
    spb = 60 / bpm
    total = len(prog) * beats * spb
    L = int(total * SR)
    ep, bass, mel, perc = (np.zeros(L) for _ in range(4))
    rnd = random.Random(7)
    # comping rhythm (in beats) with a bossa push on the "and" of 2
    comp = [(0.0, 1.2), (1.5, 0.9), (2.5, 1.0), (3.5, 0.6)]
    for bar, chord in enumerate(prog):
        b0 = bar * beats * spb
        for k, (pos, length) in enumerate(comp):
            for i, note in enumerate(chord):
                vel = 0.20 if k else 0.26
                add(ep, b0 + pos * spb + i * 0.012, ep_note(midi_hz(note + 12), length * spb, vel))
        r = roots[bar]
        for pos, n, v in ((0, r, 0.55), (1.5, r + 7, 0.40), (2, r + 12, 0.30), (3, r + 7, 0.40)):
            add(bass, b0 + pos * spb, bass_note(midi_hz(n - 12), 0.9 * spb, v))
        # brushes: swung eighths, accent on 2 and 4
        for e in range(8):
            swing = 0.08 if e % 2 else 0.0
            vel = 0.10 if e % 2 else 0.07
            add(perc, b0 + (e * 0.5 + swing) * spb, noise_hit(0.12, 6500, 2500, vel, bar * 8 + e))
        for beat in (1, 3):
            add(perc, b0 + beat * spb, noise_hit(0.2, 2500, 1800, 0.12, 999 + bar))
        add(perc, b0, thump(0.35, 0.22))
        add(perc, b0 + 2.5 * spb, thump(0.3, 0.14))
    # melody: phrases of 2 bars drawn from chord tones + major pentatonic
    scale = [65, 67, 69, 72, 74, 77, 79, 81]
    for phrase in range(0, len(prog), 2):
        if phrase % 8 == 6:
            continue  # breathe
        b0 = phrase * beats * spb
        t = 0.0
        while t < 7.0:
            step = rnd.choice([0.5, 0.5, 1.0, 1.0, 1.5])
            note = rnd.choice(scale + [n + 12 for n in prog[phrase][1:3]])
            add(mel, b0 + t * spb, bell_note(midi_hz(note), 1.6, 0.20 + rnd.random() * 0.08))
            t += step
    mix = np.zeros((L, 2))
    mix += pan(ep, -0.15)
    mix += pan(bass, 0.0)
    mix += pan(mel, 0.25)
    mix += pan(perc, 0.05) * 0.9
    return master(reverb_loop(mix, 2.2, 0.26))


def song_midnight():
    """Slow, dark lo-fi loop in D minor: warm pad, sub bass, glassy arps."""
    bpm, beats = 78, 4
    prog = [[50, 53, 57, 60], [46, 50, 53, 57], [43, 46, 50, 53], [45, 49, 52, 55]] * 6
    spb = 60 / bpm
    L = int(len(prog) * beats * spb * SR)
    pad, bass, arp, perc = (np.zeros(L) for _ in range(4))
    rnd = random.Random(11)
    for bar, chord in enumerate(prog):
        b0 = bar * beats * spb
        for note in chord:
            add(pad, b0, pad_note(midi_hz(note), beats * spb * 1.05, 0.16, 1200))
        add(bass, b0, bass_note(midi_hz(chord[0] - 12), 2.5 * spb, 0.6))
        add(bass, b0 + 2.5 * spb, bass_note(midi_hz(chord[0] - 12), 1.4 * spb, 0.4))
        order = chord[1:] + [chord[0] + 12, chord[2] + 12]
        for s in range(8):
            if rnd.random() < 0.18:
                continue
            note = order[(s * 3 + bar) % len(order)] + 12
            add(arp, b0 + s * 0.5 * spb, bell_note(midi_hz(note), 1.2, 0.12))
        add(perc, b0, thump(0.4, 0.3))
        add(perc, b0 + 2 * spb, noise_hit(0.25, 1900, 1200, 0.16, bar))
        for e in range(8):
            add(perc, b0 + e * 0.5 * spb + (0.06 if e % 2 else 0), noise_hit(0.08, 8000, 2000, 0.05, 50 + e))
    crackle = (np.random.default_rng(5).random(L) > 0.9993) * np.random.default_rng(6).uniform(-0.2, 0.2, L)
    mix = pan(pad, 0) + pan(bass, 0) + pan(arp, 0.3) + pan(perc, -0.05) + pan(crackle, 0) * 0.6
    return master(reverb_loop(mix, 2.8, 0.32))


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
    for name, fn in (("aero-channel", song_aero), ("midnight-onyx", song_midnight),
                     ("sunset-arcade", song_sunset)):
        data = fn()
        write_wav(tmp, data)
        encode_m4a(tmp, os.path.join(OUT, name + ".m4a"))
        print(f"{name}: {len(data) / SR:.1f}s")
    os.remove(tmp)
    for name, data in sfx().items():
        write_wav(os.path.join(OUT, name + ".wav"), data)
        print("sfx", name)


if __name__ == "__main__":
    main()
