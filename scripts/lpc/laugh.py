"""Synthesise the Creeping Doom's laughs for the K10's speech vocabulary.

    python scripts/lpc/laugh.py            # write out/laugh/laugh<N>.wav (16 kHz) to listen to

SAPI cannot laugh -- asked to say "ha ha ha" it reads the letters out -- so a
laugh is built here instead, as a voice: a Rosenberg glottal pulse train with
jitter, shimmer and breath, through a cascade of five formant resonators
(Klatt's), shaped syllable by syllable to the score in doom-laugh.h. That score
is the one the LCD and the TV move the skull's jaw to, so it is scraped out of
the header rather than copied here.

gen_vocab.py calls render() for any phrase whose text starts with "@laugh N"
and then treats the WAV exactly like a SAPI one: conditioned, encoded to
TMS5220 frames, validated. Each syllable is
  - a breath ('h'): aspiration noise through the vowel's own formants (that is
    what an h is), rising into the vowel;
  - or a hum ('m'): voiced through a nasal murmur, mouth shut, gliding open
    through a w into the vowel -- the "mwa";
  - or a hard breath ('H'): shorter and stronger, the vowel bitten off by a
    glottal stop and breathier all the way -- what makes a cackle a cackle;
  - or a click ('k'): a short burst and a breath;
  - then the vowel: voiced, pitch gliding f0a -> f0b, loud at once and decaying,
    breathier at the onset, a vibrato and an arch on the long ones.
"""

from __future__ import annotations

import math
import os
import re
import sys

import numpy as np
from scipy.io import wavfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
HEADER = os.path.join(ROOT, 'doom-laugh.h')

FS = 16000
LEAD_S = 0.02               # silence before the first breath (condition() keeps 20 ms)
TAIL_S = 0.08

# Formants (Hz) and bandwidths, an adult male, F1..F5. 'n' is the hum's nasal
# murmur: the energy sits low and the upper formants are damped.
FORMANTS = {
    'a': ((730, 1090, 2440, 3400, 4000), (90, 110, 150, 250, 300)),
    'o': ((570, 860, 2410, 3400, 4000), (80, 100, 150, 250, 300)),
    'e': ((530, 1840, 2480, 3400, 4000), (80, 110, 150, 250, 300)),
    'u': ((640, 1190, 2390, 3400, 4000), (90, 110, 150, 250, 300)),
    'x': ((690, 1660, 2490, 3400, 4000), (90, 120, 160, 250, 300)),
    'n': ((260, 1050, 2250, 3300, 4000), (90, 300, 400, 500, 600)),
    'w': ((320, 640, 2250, 3300, 4000), (80, 100, 150, 250, 300)),
}


def read_laughs(path: str = HEADER) -> list:
    """[[syllable dict, ...] per laugh] from doom-laugh.h, in DOOM_LAUGH_<n> order."""
    with open(path, encoding='utf-8') as fh:
        text = fh.read()
    m = re.search(r'DOOM_LAUGH_N\s*=\s*(\d+)', text)
    if not m:
        sys.exit(f'{path}: DOOM_LAUGH_N not found')
    out = []
    for i in range(int(m.group(1))):
        t = re.search(r'DOOM_LAUGH_%d\[\]\s*=\s*\{(.*?)\n\};' % i, text, re.S)
        if not t:
            sys.exit(f'{path}: DOOM_LAUGH_{i}[] not found')
        rows = re.findall(r"\{\s*(\d+),\s*(\d+),\s*(\d+),\s*'(.)',\s*'(.)',\s*(\d+),\s*(\d+)\s*\}", t.group(1))
        if not rows:
            sys.exit(f'{path}: DOOM_LAUGH_{i}[] has no syllables')
        out.append([dict(at=int(a), len=int(n), amp=int(p), cons=c, vowel=v, f0a=int(fa), f0b=int(fb))
                    for a, n, p, c, v, fa, fb in rows])
    return out


def laugh_text_hash(path: str = HEADER) -> str:
    """What a laugh's cache key depends on: the score and this synthesiser."""
    import hashlib
    h = hashlib.sha1()
    for p in (path, os.path.abspath(__file__)):
        with open(p, 'rb') as fh:
            h.update(fh.read())
    return h.hexdigest()[:12]


def _rosenberg(p: float) -> float:
    """Glottal flow over one period, phase 0..1: opens, closes, stays shut."""
    tp, tn = 0.42, 0.16
    if p < tp:
        return 0.5 * (1.0 - math.cos(math.pi * p / tp))
    if p < tp + tn:
        return math.cos(0.5 * math.pi * (p - tp) / tn)
    return 0.0


def _tracks(index: int) -> tuple:
    """Per-sample control tracks for laugh `index`: formants, bandwidths, pitch,
    and the gains of the glottal source, the breath and the click."""
    syls = read_laughs()[index]
    end = max(s['at'] + s['len'] for s in syls) / 1000.0
    n = int((LEAD_S + end + TAIL_S) * FS)

    # Per-sample control tracks, filled syllable by syllable.
    F = np.zeros((n, 5)); B = np.ones((n, 5)) * 200.0
    F[:] = FORMANTS['a'][0]; B[:] = FORMANTS['a'][1]
    f0 = np.full(n, 120.0)
    voice = np.zeros(n)          # glottal source gain
    breath = np.zeros(n)         # aspiration gain
    burst = np.zeros(n)          # a click's broadband burst
    for s in syls:
        a = (s['amp'] / 255.0) ** 1.3
        i0 = int((LEAD_S + s['at'] / 1000.0) * FS)
        L = int(s['len'] / 1000.0 * FS)
        vf, vb = (np.array(x, float) for x in FORMANTS[s['vowel']])
        long_ = s['len'] > 300
        hard = s['cons'] == 'H'                       # a cackle: a short hard breath, no lingering
        if s['cons'] == 'm':
            hum = int(0.38 * L)
            glide = int(0.080 * FS)                   # out of the hum, through a w, into the vowel
            nf, nb = (np.array(x, float) for x in FORMANTS['n'])
            wf, wb = (np.array(x, float) for x in FORMANTS['w'])
            for k in range(hum):
                j = i0 + k
                F[j], B[j] = nf, nb
                f0[j] = s['f0a'] * (0.92 + 0.08 * k / max(1, hum))
                voice[j] = a * 0.55 * min(1.0, k / (0.03 * FS))
                breath[j] = a * 0.03
            for k in range(glide):
                j = i0 + hum + k
                u = k / glide
                if u < 0.35:
                    F[j], B[j] = nf + (wf - nf) * (u / 0.35), nb + (wb - nb) * (u / 0.35)
                else:
                    F[j], B[j] = wf + (vf - wf) * ((u - 0.35) / 0.65), wb + (vb - wb) * ((u - 0.35) / 0.65)
                f0[j] = s['f0a']
                voice[j] = a * (0.55 + 0.45 * u)
                breath[j] = a * 0.05
            v0 = hum + glide
        else:
            v0 = int(min(0.040 if hard else 0.055, (0.26 if hard else 0.35) * s['len'] / 1000.0) * FS)
            if s['cons'] == 'k':
                for k in range(int(0.008 * FS)):
                    burst[i0 + k] = a * 1.4 * (1.0 - k / (0.008 * FS))
            for k in range(v0):
                j = i0 + k
                F[j], B[j] = vf, vb * 1.6               # an h is the vowel, breathed, a little damped
                breath[j] = a * (1.0 if hard else 0.85) * min(1.0, k / ((0.015 if hard else 0.03) * FS))
        vl = L - v0
        rel = int((0.014 if hard else 0.035) * FS)       # 'H' is bitten off: a glottal stop
        for k in range(vl):
            j = i0 + v0 + k
            if j >= n:
                break
            u = k / max(1, vl)
            F[j], B[j] = vf, vb
            g = s['f0a'] + (s['f0b'] - s['f0a']) * u
            if long_:
                g *= 1.0 + 0.07 * math.sin(math.pi * u) + 0.03 * math.sin(2 * math.pi * 5.5 * k / FS)
            f0[j] = g
            env = (1.0 - 0.5 * u ** 1.5) * min(1.0, k / (0.012 * FS)) * min(1.0, (vl - k) / rel)
            voice[j] = a * env
            breath[j] = a * env * ((0.45 - 0.2 * min(1.0, u * 3.0)) if hard else (0.35 - 0.25 * min(1.0, u * 3.0)))
    return F, B, f0, voice, breath, burst, n


def frame_hint(index: int, n_frames: int, frame_len: int, fs: int) -> np.ndarray:
    """What the encoder should hear in each of its frames (lpc_tms.apply_lag_hint):
    the period in samples at `fs` where the source is voicing for a third of
    the frame or more (at its median pitch there), -1 where it is otherwise
    breathing or clicking, 0 where it is silent. A whole frame rather than its
    middle: a frame that only catches the start of a breath is still a breath,
    and left to the tracker it reads the onset as a pitch."""
    _, _, f0, voice, breath, burst, n = _tracks(index)
    out = np.zeros(n_frames)
    vmax = max(1e-9, voice.max())
    for f in range(n_frames):
        a, b = int(f * frame_len * FS / fs), int((f + 1) * frame_len * FS / fs)
        if a >= n:
            break
        on = voice[a:b] > 0.04 * vmax
        if on.sum() * 3 >= b - a:
            out[f] = fs / float(np.median(f0[a:b][on]))
        elif breath[a:b].max() > 0.0 or burst[a:b].max() > 0.0 or on.any():
            out[f] = -1.0
    return out


def render(index: int, path: str | None = None, seed: int = 7) -> np.ndarray:
    """Laugh `index` as float audio at FS; also written to `path` (16-bit) if given."""
    F, B, f0, voice, breath, burst, n = _tracks(index)
    rng = np.random.default_rng(seed + 101 * index)

    # Source: a Rosenberg pulse train (differentiated: the lips radiate the
    # flow's derivative), with per-period jitter and shimmer, plus aspiration
    # that pulses with the flow, plus the clicks.
    src = np.zeros(n)
    ph, flow_prev, per_f0, per_amp = 0.0, 0.0, f0[0], 1.0
    noise = rng.standard_normal(n)
    # aspiration is noise with the top taken off (a breath is not hiss)
    asp = np.convolve(noise, np.ones(3) / 3.0, mode='same')
    for j in range(n):
        if voice[j] > 0.0:
            ph += per_f0 / FS
            if ph >= 1.0:
                ph -= 1.0
                per_f0 = f0[j] * (1.0 + 0.012 * rng.standard_normal())
                per_amp = 1.0 + 0.08 * rng.standard_normal()
            flow = _rosenberg(ph) * per_amp
        else:
            flow, ph = 0.0, 0.0
        d = (flow - flow_prev) * voice[j]
        flow_prev = flow
        src[j] = d * 6.0 + asp[j] * (breath[j] * 0.22 + voice[j] * 0.03 * flow) + noise[j] * burst[j] * 0.3

    # Tract: five resonators in cascade, coefficients per sample.
    y = src
    T = 1.0 / FS
    for f in range(5):
        Ff, Bf = F[:, f], B[:, f]
        C = -np.exp(-2.0 * math.pi * Bf * T)
        Bc = 2.0 * np.exp(-math.pi * Bf * T) * np.cos(2.0 * math.pi * Ff * T)
        A = 1.0 - Bc - C
        out = np.zeros(n)
        y1 = y2 = 0.0
        for j in range(n):
            o = A[j] * y[j] + Bc[j] * y1 + C[j] * y2
            out[j] = o
            y2, y1 = y1, o
        y = out
    y = y / max(1e-9, np.max(np.abs(y))) * 0.7
    if path:
        os.makedirs(os.path.dirname(path), exist_ok=True)
        wavfile.write(path, FS, np.round(y * 32767).astype(np.int16))
    return y


def main() -> int:
    out = os.path.join(HERE, 'out', 'laugh')
    for i in range(len(read_laughs())):
        p = os.path.join(out, f'laugh{i}.wav')
        render(i, p)
        print(p)
    return 0


if __name__ == '__main__':
    sys.exit(main())
