#!/usr/bin/env python3
"""Generates ONYX's six theme tracks: original, calm ambient pieces synthesised from scratch
(no samples, nothing borrowed). Each one has its own key, tempo, instruments and mood, and each
loops cleanly (the reverb tail wraps around the loop point).

    python3 tools/make_theme_music.py <out_dir>        # writes <theme>.m4a (AAC) for every theme

Needs numpy, scipy and ffmpeg.
"""
import subprocess
import sys
from pathlib import Path

import numpy as np
from scipy.signal import butter, lfilter

SR = 44100


def mtof(m):
    return 440.0 * 2 ** ((m - 69) / 12.0)


def env(n, a, r, curve=2.0):
    """Attack/release envelope over n samples (seconds in a, r)."""
    e = np.ones(n)
    na, nr = max(1, int(a * SR)), max(1, int(r * SR))
    na, nr = min(na, n // 2), min(nr, n // 2)
    e[:na] = np.linspace(0, 1, na) ** curve
    e[n - nr:] = np.linspace(1, 0, nr) ** curve
    return e


def lowpass(x, fc, order=2):
    b, a = butter(order, fc / (SR / 2), "low")
    return lfilter(b, a, x)


def highpass(x, fc, order=2):
    b, a = butter(order, fc / (SR / 2), "high")
    return lfilter(b, a, x)


class Track:
    def __init__(self, seconds, seed):
        self.n = int(seconds * SR)
        self.rng = np.random.default_rng(seed)
        self.dry = np.zeros((2, self.n))
        self.wet = np.zeros((2, self.n))

    def add(self, sig, t, pan=0.0, wet=0.5, gain=1.0):
        i = int(t * SR)
        if i >= self.n:
            return
        sig = sig[: self.n - i]
        l, r = np.cos((pan + 1) * np.pi / 4), np.sin((pan + 1) * np.pi / 4)
        self.dry[0, i:i + len(sig)] += sig * l * gain * (1 - wet * 0.5)
        self.dry[1, i:i + len(sig)] += sig * r * gain * (1 - wet * 0.5)
        self.wet[0, i:i + len(sig)] += sig * l * gain * wet
        self.wet[1, i:i + len(sig)] += sig * r * gain * wet

    def wrap_add(self, sig, t, **kw):
        """Like add, but a note that runs past the end wraps to the start (seamless loop)."""
        i = int(t * SR)
        if i + len(sig) <= self.n:
            self.add(sig, t, **kw)
        else:
            cut = self.n - i
            self.add(sig[:cut], t, **kw)
            self.add(sig[cut:], 0.0, **kw)

    # ----- instruments -------------------------------------------------------------------------
    def pad(self, midis, dur, bright=0.35, attack=2.5, release=3.0, detune=0.0035):
        n = int(dur * SR)
        t = np.arange(n) / SR
        out = np.zeros(n)
        for m in midis:
            f = mtof(m)
            for d in (-detune, 0.0, detune):
                ph = self.rng.uniform(0, 6.28)
                lfo = 1 + 0.0012 * np.sin(2 * np.pi * 0.17 * t + ph)
                for h, amp in ((1, 1.0), (2, 0.5 * bright * 2), (3, 0.25 * bright * 2), (4, 0.12 * bright * 2)):
                    out += amp * np.sin(2 * np.pi * f * (1 + d) * h * t * lfo + ph * h) / 3.0
        out *= env(n, attack, release)
        return out / max(1, len(midis)) * 0.9

    def bell(self, midi, dur=5.0, ratio=3.5, index=2.2, decay=1.4):
        n = int(dur * SR)
        t = np.arange(n) / SR
        f = mtof(midi)
        mod = np.sin(2 * np.pi * f * ratio * t) * index * np.exp(-t / decay)
        car = np.sin(2 * np.pi * f * t + mod) * np.exp(-t / (decay * 1.8))
        car += 0.3 * np.sin(2 * np.pi * f * 2 * t) * np.exp(-t / (decay * 0.6))
        return car * env(n, 0.004, 0.2)

    def epiano(self, midi, dur=4.0):
        n = int(dur * SR)
        t = np.arange(n) / SR
        f = mtof(midi)
        mod = np.sin(2 * np.pi * f * t) * 1.6 * np.exp(-t / 0.6)
        tine = np.sin(2 * np.pi * f * 14 * t) * 0.15 * np.exp(-t / 0.08)
        car = (np.sin(2 * np.pi * f * t + mod) + tine) * np.exp(-t / 1.8)
        return car * env(n, 0.006, 0.3)

    def pluck(self, midi, dur=3.0, bright=0.5):
        """Karplus-Strong string: warm, woody."""
        f = mtof(midi)
        n = int(dur * SR)
        period = int(round(SR / f))
        burst = self.rng.uniform(-1, 1, period)
        burst = lowpass(burst, 1500 + 4000 * bright, 1)
        x = np.zeros(n)
        x[:period] = burst
        a = np.zeros(period + 2)
        a[0] = 1.0
        a[period] = -0.4985
        a[period + 1] = -0.4985
        y = lfilter([1.0], a, x)
        return y * env(n, 0.002, 0.3) * 0.9

    def sub(self, midi, dur, attack=1.5, release=2.5):
        n = int(dur * SR)
        t = np.arange(n) / SR
        f = mtof(midi)
        out = np.sin(2 * np.pi * f * t) + 0.25 * np.sin(2 * np.pi * f * 2 * t)
        return out * env(n, attack, release) * 0.8

    def air(self, dur, fc=1800, q=0.5, slow=0.05):
        """Breathy filtered noise that swells slowly."""
        n = int(dur * SR)
        x = self.rng.normal(0, 1, n)
        x = lowpass(highpass(x, fc * 0.4, 1), fc, 1)
        t = np.arange(n) / SR
        swell = 0.5 + 0.5 * np.sin(2 * np.pi * slow * t - 1.5)
        return x * swell * q

    # ----- finish ------------------------------------------------------------------------------
    def reverb_ir(self, seconds, damp):
        n = int(seconds * SR)
        t = np.arange(n) / SR
        ir = []
        for _ in range(2):
            x = self.rng.normal(0, 1, n) * np.exp(-t / (seconds / 4.6))
            x = lowpass(x, damp, 1)
            ir.append(x)
        return np.array(ir)

    def render(self, rev_seconds=4.5, damp=3800, rev_mix=0.55, target_peak=0.55):
        ir = self.reverb_ir(rev_seconds, damp)
        # Circular convolution: the tail wraps around, so the loop point is seamless.
        out = np.zeros_like(self.dry)
        nfft = self.n
        for c in range(2):
            h = np.zeros(nfft)
            h[: ir.shape[1]] = ir[c] / np.sqrt(np.sum(ir[c] ** 2)) * 0.5
            wet = np.fft.irfft(np.fft.rfft(self.wet[c]) * np.fft.rfft(h), nfft)
            out[c] = self.dry[c] + wet * rev_mix * 2.0
        # Gentle high roll-off and a hair of tape-ish warmth.
        for c in range(2):
            out[c] = lowpass(out[c], 9500, 1)
            out[c] = np.tanh(out[c] * 1.1) / 1.1
        out -= out.mean(axis=1, keepdims=True)
        out *= target_peak / np.max(np.abs(out))
        return out


def write_m4a(stereo, path):
    pcm = (np.clip(stereo.T, -1, 1) * 32767).astype("<i2").tobytes()
    cmd = ["ffmpeg", "-y", "-loglevel", "error", "-f", "s16le", "-ar", str(SR), "-ac", "2", "-i", "-",
           "-c:a", "aac", "-b:a", "112k", "-movflags", "+faststart", str(path)]
    subprocess.run(cmd, input=pcm, check=True)


# =================================================================================================
# The six pieces. bar = seconds per bar; each is built from a chord loop over `bars` bars.
# =================================================================================================

def aero_channel():
    """Aero Channel: sunlit and airy. D Lydian, glassy bells over a slow bright pad (66 BPM)."""
    bar = 4 * 60 / 66
    bars = 28
    tr = Track(bar * bars, 11)
    prog = [([50, 57, 61, 66], 38), ([47, 54, 59, 62], 35), ([52, 59, 62, 66], 40), ([50, 54, 57, 61], 38)]
    for b in range(bars):
        chord, root = prog[(b // 2) % 4]
        if b % 2 == 0:
            tr.wrap_add(tr.pad(chord, bar * 2.6, bright=0.7, attack=2.0, release=3.2), b * bar, pan=0, wet=0.7, gain=0.55)
            tr.wrap_add(tr.sub(root, bar * 2.2), b * bar, wet=0.2, gain=0.35)
    scale = [74, 76, 78, 79, 81, 83, 85, 86]  # D Lydian, upper register
    rng = np.random.default_rng(5)
    t = 0.0
    while t < bar * bars - 2:
        m = int(rng.choice(scale))
        tr.wrap_add(tr.bell(m, dur=5.5), t, pan=float(rng.uniform(-0.8, 0.8)), wet=0.85, gain=0.30)
        t += float(rng.choice([1.8, 2.3, 2.7, 3.6, 4.4]))
    tr.wrap_add(tr.air(bar * bars, 3200, 0.10, 0.04), 0, wet=0.6, gain=0.5)
    return tr.render(rev_seconds=5.0, damp=6000)


def carbon():
    """Carbon: dark and minimal. D minor drone, sparse low pulses and a thin cold shimmer (54 BPM)."""
    bar = 4 * 60 / 54
    bars = 28
    tr = Track(bar * bars, 23)
    for b in range(0, bars, 4):
        tr.wrap_add(tr.sub(26, bar * 4.5, attack=3, release=4), b * bar, wet=0.3, gain=0.7)
        tr.wrap_add(tr.pad([38, 45, 50, 53], bar * 4.6, bright=0.18, attack=3.5, release=4.0), b * bar, wet=0.6, gain=0.55)
    for b in range(2, bars, 8):
        tr.wrap_add(tr.pad([46, 50, 53, 57], bar * 4.4, bright=0.2, attack=3.5, release=4.0), b * bar, wet=0.6, gain=0.45)
    rng = np.random.default_rng(9)
    for k in range(bars * 2):  # slow, sparse low pulses
        if rng.random() < 0.55:
            m = int(rng.choice([38, 41, 45, 50]))
            tr.wrap_add(tr.pluck(m, dur=3.2, bright=0.1), k * bar / 2 + float(rng.uniform(0, 0.3)),
                        pan=float(rng.uniform(-0.4, 0.4)), wet=0.8, gain=0.42)
    for k in range(10):  # thin cold shimmer far away
        tr.wrap_add(tr.bell(int(rng.choice([86, 89, 93])), dur=6.0, index=1.2, decay=2.2),
                    float(rng.uniform(0, bar * bars - 6)), pan=float(rng.uniform(-1, 1)), wet=0.95, gain=0.12)
    tr.wrap_add(tr.air(bar * bars, 700, 0.14, 0.03), 0, wet=0.5, gain=0.6)
    return tr.render(rev_seconds=6.0, damp=2400)


def dual_screen():
    """Dual Screen: soft electric piano and warm chords, late-night lounge. F major 7 / A minor 7 (72 BPM)."""
    bar = 4 * 60 / 72
    bars = 32
    tr = Track(bar * bars, 31)
    prog = [([53, 57, 60, 64], 41), ([57, 60, 64, 67], 45), ([50, 53, 57, 60], 38), ([55, 59, 62, 65], 43)]
    for b in range(bars):
        chord, root = prog[(b // 2) % 4]
        if b % 2 == 0:
            tr.wrap_add(tr.pad([m + 12 for m in chord[:3]], bar * 2.2, bright=0.3, attack=1.2, release=2.4), b * bar, wet=0.6, gain=0.35)
            tr.wrap_add(tr.sub(root, bar * 1.9, attack=0.4, release=1.6), b * bar, wet=0.15, gain=0.5)
        # gently rolled chord on beats 1 and 3
        for beat, vel in ((0, 1.0), (2, 0.6)):
            for k, m in enumerate(chord):
                tr.wrap_add(tr.epiano(m + 12), b * bar + beat * bar / 4 + k * 0.045, pan=-0.25 + 0.15 * k, wet=0.55, gain=0.20 * vel)
    rng = np.random.default_rng(3)
    mel = [72, 74, 76, 79, 81, 84]
    t = 0.0
    while t < bar * bars - 3:
        tr.wrap_add(tr.epiano(int(rng.choice(mel)), dur=3.0), t, pan=float(rng.uniform(0.1, 0.6)), wet=0.7, gain=0.17)
        t += float(rng.choice([1.25, 2.5, 3.75, 5.0]))
    tr.wrap_add(tr.air(bar * bars, 1400, 0.07, 0.05), 0, wet=0.5, gain=0.5)
    return tr.render(rev_seconds=3.4, damp=4200)


def matcha():
    """Matcha: a quiet tea-house. D major pentatonic, woody plucks over a soft drone (60 BPM)."""
    bar = 4.0
    bars = 28
    tr = Track(bar * bars, 41)
    for b in range(0, bars, 4):
        tr.wrap_add(tr.pad([38, 45, 50, 57], bar * 4.6, bright=0.12, attack=3.0, release=3.5), b * bar, wet=0.6, gain=0.5)
        tr.wrap_add(tr.sub(26, bar * 4.4, attack=3.0, release=3.0), b * bar, wet=0.2, gain=0.35)
    penta = [62, 64, 66, 69, 71, 74, 76, 78, 81]  # D E F# A B
    rng = np.random.default_rng(14)
    t = 0.5
    motif = [0, 2, 4, 2, 3, 1]
    idx = 0
    while t < bar * bars - 3:
        if rng.random() < 0.8:
            step = motif[idx % len(motif)] + int(rng.integers(0, 2))
            tr.wrap_add(tr.pluck(penta[min(step, len(penta) - 1)], dur=3.2, bright=0.45), t,
                        pan=float(rng.uniform(-0.5, 0.5)), wet=0.55, gain=0.5)
            idx += 1
        t += float(rng.choice([1.0, 1.5, 2.0, 2.0, 3.0]))
    for k in range(6):  # a few distant temple-bell tones
        tr.wrap_add(tr.bell(int(rng.choice([50, 57, 62])), dur=7.0, ratio=2.76, index=1.0, decay=3.0),
                    k * bar * 4.5, pan=float(rng.uniform(-0.5, 0.5)), wet=0.9, gain=0.22)
    tr.wrap_add(tr.air(bar * bars, 2600, 0.05, 0.06), 0, wet=0.6, gain=0.5)
    return tr.render(rev_seconds=3.0, damp=5000)


def midnight_onyx():
    """Midnight Onyx: deep, slow and open. C# minor pads, a lone soft piano and long space (48 BPM)."""
    bar = 4 * 60 / 48
    bars = 24
    tr = Track(bar * bars, 53)
    prog = [([49, 56, 61, 64], 25), ([45, 52, 57, 61], 21), ([47, 54, 59, 63], 23), ([44, 51, 56, 59], 20)]
    for b in range(0, bars, 2):
        chord, root = prog[(b // 2) % 4]
        tr.wrap_add(tr.pad(chord, bar * 2.6, bright=0.22, attack=3.2, release=3.8), b * bar, wet=0.75, gain=0.6)
        tr.wrap_add(tr.pad([m + 12 for m in chord[1:]], bar * 2.6, bright=0.4, attack=3.6, release=4.0), b * bar, wet=0.85, gain=0.3)
        tr.wrap_add(tr.sub(root + 12, bar * 2.4, attack=2.0, release=3.0), b * bar, wet=0.3, gain=0.6)
    rng = np.random.default_rng(8)
    notes = [73, 76, 80, 83, 85, 88]  # C# minor high
    t = 3.0
    while t < bar * bars - 6:
        tr.wrap_add(tr.epiano(int(rng.choice(notes)), dur=5.0), t, pan=float(rng.uniform(-0.3, 0.7)), wet=0.9, gain=0.22)
        t += float(rng.choice([4.0, 5.0, 6.5, 8.0]))
    tr.wrap_add(tr.air(bar * bars, 900, 0.10, 0.03), 0, wet=0.6, gain=0.5)
    return tr.render(rev_seconds=7.0, damp=3000)


def sunset_arcade():
    """Sunset Arcade: warm chillwave glow. A major, soft synth arpeggio under slow pads (84 BPM)."""
    bar = 4 * 60 / 84
    bars = 32
    tr = Track(bar * bars, 67)
    prog = [([57, 61, 64, 68], 33), ([54, 61, 64, 66], 30), ([52, 56, 59, 64], 28), ([56, 59, 63, 68], 32)]
    for b in range(0, bars, 2):
        chord, root = prog[(b // 2) % 4]
        tr.wrap_add(tr.pad(chord, bar * 2.5, bright=0.5, attack=1.4, release=2.6, detune=0.006), b * bar, wet=0.6, gain=0.5)
        tr.wrap_add(tr.sub(root, bar * 2.2, attack=0.5, release=1.8), b * bar, wet=0.15, gain=0.55)
    step = 60 / 84 / 2  # eighth notes
    for b in range(bars):
        chord, _ = prog[(b // 2) % 4]
        arp = [chord[0] + 12, chord[1] + 12, chord[2] + 12, chord[3] + 12, chord[2] + 12, chord[1] + 12]
        for k in range(8):
            if k % 2 == 1 and (b + k) % 3 == 0:
                continue
            m = arp[k % len(arp)] + (12 if b % 4 == 3 and k > 5 else 0)
            tr.wrap_add(tr.bell(m, dur=1.6, ratio=2.0, index=0.9, decay=0.55), b * bar + k * step,
                        pan=-0.5 + 0.14 * k, wet=0.5, gain=0.15)
    tr.wrap_add(tr.air(bar * bars, 2200, 0.06, 0.05), 0, wet=0.5, gain=0.5)
    return tr.render(rev_seconds=3.8, damp=4600)


PIECES = {
    "aero-channel": aero_channel,
    "carbon": carbon,
    "dual-screen": dual_screen,
    "matcha": matcha,
    "midnight-onyx": midnight_onyx,
    "sunset-arcade": sunset_arcade,
}

if __name__ == "__main__":
    out = Path(sys.argv[1] if len(sys.argv) > 1 else ".")
    out.mkdir(parents=True, exist_ok=True)
    only = sys.argv[2:] or list(PIECES)
    for name in only:
        audio = PIECES[name]()
        write_m4a(audio, out / f"{name}.m4a")
        print(name, f"{audio.shape[1] / SR:.0f}s", (out / f"{name}.m4a").stat().st_size // 1024, "KiB")
