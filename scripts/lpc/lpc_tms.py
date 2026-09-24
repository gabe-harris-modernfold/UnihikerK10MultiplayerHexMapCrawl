"""TMS5220 LPC-10 speech for the K10: tables, bitstream, reference decoder, encoder.

snd-lpc.hpp on the K10 is a TMS5220-compatible ("Speak & Spell") synthesiser.
This module is its offline twin: a float reference decoder that follows the
same spec line for line, and an encoder that turns 8 kHz audio into frames for
it. gen_vocab.py drives both. Importing this module has no side effects.

Everything here is written from the chip's documented behaviour. The coefficient
tables are the TMS5220 set from MAME's tms5110r.hxx (BSD-3-Clause). No Talkie
code or data is used.

Bitstream ("Talkie order")
  Bytes are consumed in array order and the bits of each byte LSB (bit 0)
  first. An n-bit field is assembled MSB-first from the consumed bits:
  v = 0; repeat n: v = (v << 1) | nextbit().

Frame layout (bits in stream order)
  E:4   0 = silence (frame ends here), 15 = STOP (utterance ends), else energy
  R:1   repeat -- no K bits follow; the previous K indices are reused
  P:6   pitch index, 0 = unvoiced
  R = 0 only:  K1:5 K2:5 K3:4 K4:4, then for voiced frames (P != 0) only
               K5:4 K6:4 K7:4 K8:3 K9:3 K10:3
  => silence 4 bits, repeat 11, unvoiced 29, voiced 50. The final byte of an
  utterance is padded with zero bits after the STOP frame.

Sign convention (verified numerically in gen_vocab.py's self-test)
  The decoder lattice's K_i is the ordinary reflection coefficient of the
  Levinson recursion with A(z) = 1 + sum a_j z^-j, a_i^(i) = k_i and
  k_1 = -r1/r0. Low-pass (voiced) speech therefore has K1 near -0.9, which is
  why the K1 table is dense near -1.
"""

from __future__ import annotations

import math
from dataclasses import dataclass, replace

import numpy as np
from scipy import signal

# ---------------------------------------------------------------------------
# Chip constants and tables
# ---------------------------------------------------------------------------

FS = 8000                 # output sample rate, Hz
FRAME_LEN = 200           # samples per frame (25 ms)
SUBSTEPS = 8              # interpolation sub-steps per frame (25 samples each)

E_SILENCE = 0             # energy index of a silence frame
E_STOP = 15               # energy index of the STOP frame
N_K_UNVOICED = 4          # K1..K4 are sent for unvoiced frames
N_K_VOICED = 10           # K1..K10 for voiced frames

ENERGY = (0, 1, 2, 3, 4, 6, 8, 11, 16, 23, 33, 47, 63, 85, 114, 0)

# pitch period in samples at 8 kHz; index 0 = unvoiced
PITCH = (0, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29,
         30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 44, 46, 48,
         50, 52, 53, 56, 58, 60, 62, 65, 68, 70, 72, 76, 78, 80, 84, 86,
         91, 94, 98, 101, 105, 109, 114, 118, 122, 127, 132, 137, 142, 148, 153, 159)

# reflection coefficients, x/512
K1 = (-501, -498, -497, -495, -493, -491, -488, -482, -478, -474, -469, -464, -459, -452, -445, -437,
      -412, -380, -339, -288, -227, -158, -81, -1, 80, 157, 226, 287, 337, 379, 411, 436)
K2 = (-328, -303, -274, -244, -211, -175, -138, -99, -59, -18, 24, 64, 105, 143, 180, 215,
      248, 278, 306, 331, 354, 374, 392, 408, 422, 435, 445, 455, 463, 470, 476, 506)
K3 = (-441, -387, -333, -279, -225, -171, -117, -63, -9, 45, 98, 152, 206, 260, 314, 368)
K4 = (-328, -273, -217, -161, -106, -50, 5, 61, 116, 172, 228, 283, 339, 394, 450, 506)
K5 = (-328, -282, -235, -189, -142, -96, -50, -3, 43, 90, 136, 182, 229, 275, 322, 368)
K6 = (-256, -212, -168, -123, -79, -35, 10, 54, 98, 143, 187, 232, 276, 320, 365, 409)
K7 = (-308, -260, -212, -164, -117, -69, -21, 27, 75, 122, 170, 218, 266, 314, 361, 409)
K8 = (-256, -161, -66, 29, 124, 219, 314, 409)
K9 = (-256, -176, -96, -15, 65, 146, 226, 307)
K10 = (-205, -132, -59, 14, 87, 160, 234, 307)
K_TABLES = (K1, K2, K3, K4, K5, K6, K7, K8, K9, K10)
K_BITS = (5, 5, 4, 4, 4, 4, 4, 3, 3, 3)

# glottal excitation for voiced frames (signed int8), indexed by the pitch counter
CHIRP = (0x00, 0x03, 0x0f, 0x28, 0x4c, 0x6c, 0x71, 0x50, 0x25, 0x26, 0x4c, 0x44,
         0x1a, 0x32, 0x3b, 0x13, 0x37, 0x1a, 0x25, 0x1f, 0x1d) + (0,) * 31

assert len(ENERGY) == 16 and len(PITCH) == 64 and len(CHIRP) == 52
assert all(len(t) == 1 << b for t, b in zip(K_TABLES, K_BITS))

# float views used by the encoder
_KF = tuple(np.array(t, dtype=np.float64) / 512.0 for t in K_TABLES)
_LOG_PITCH = np.log(np.array(PITCH[1:], dtype=np.float64))


# ---------------------------------------------------------------------------
# Frames and the Talkie-order bitstream
# ---------------------------------------------------------------------------

@dataclass(frozen=True)
class Frame:
    """One 25 ms frame, in the canonical form the parser produces.

    energy  0 = silence, 15 = STOP, 1..14 = speech
    repeat  True: reuse the previous K indices (k is then empty)
    pitch   PITCH index, 0 = unvoiced
    k       K indices as sent: () for silence/STOP/repeat, 4 for unvoiced, 10 for voiced
    """
    energy: int
    repeat: bool = False
    pitch: int = 0
    k: tuple = ()

    @property
    def is_silent(self) -> bool:
        return self.energy in (E_SILENCE, E_STOP)

    @property
    def voiced(self) -> bool:
        return not self.is_silent and self.pitch != 0

    def nbits(self) -> int:
        if self.is_silent:
            return 4
        return 11 + sum(K_BITS[:len(self.k)])

    def check(self) -> None:
        """Raise ValueError unless this frame is in canonical, encodable form."""
        if not 0 <= self.energy <= 15:
            raise ValueError(f"energy index out of range: {self}")
        if self.is_silent:
            if self.repeat or self.pitch or self.k:
                raise ValueError(f"silence/STOP frame carries fields: {self}")
            return
        if not 0 <= self.pitch <= 63:
            raise ValueError(f"pitch index out of range: {self}")
        want = 0 if self.repeat else (N_K_VOICED if self.pitch else N_K_UNVOICED)
        if len(self.k) != want:
            raise ValueError(f"expected {want} K indices: {self}")
        for i, v in enumerate(self.k):
            if not 0 <= v < len(K_TABLES[i]):
                raise ValueError(f"K{i + 1} index out of range: {self}")


SILENCE = Frame(E_SILENCE)
STOP = Frame(E_STOP)


class _BitWriter:
    """Appends MSB-first fields into LSB-first bytes (Talkie order)."""

    def __init__(self) -> None:
        self.buf = bytearray()
        self.nbits = 0

    def put(self, value: int, n: int) -> None:
        for i in range(n - 1, -1, -1):
            pos = self.nbits & 7
            if pos == 0:
                self.buf.append(0)
            if (value >> i) & 1:
                self.buf[-1] |= 1 << pos
            self.nbits += 1


class _BitReader:
    """Reads MSB-first fields out of LSB-first bytes (Talkie order)."""

    def __init__(self, data: bytes) -> None:
        self.data = data
        self.pos = 0

    def get(self, n: int) -> int:
        v = 0
        for _ in range(n):
            byte = self.pos >> 3
            if byte >= len(self.data):
                raise ValueError("bitstream ended before the STOP frame")
            v = (v << 1) | ((self.data[byte] >> (self.pos & 7)) & 1)
            self.pos += 1
        return v


def pack(frames) -> bytes:
    """Serialise frames (which must end with STOP) into a Talkie-order bitstream."""
    frames = list(frames)
    if not frames or frames[-1].energy != E_STOP:
        raise ValueError("an utterance must end with a STOP frame")
    w = _BitWriter()
    for fr in frames:
        fr.check()
        w.put(fr.energy, 4)
        if fr.is_silent:
            if fr.energy == E_STOP:
                break
            continue
        w.put(1 if fr.repeat else 0, 1)
        w.put(fr.pitch, 6)
        for i, v in enumerate(fr.k):
            w.put(v, K_BITS[i])
    return bytes(w.buf)


def unpack(data: bytes) -> list:
    """Parse a Talkie-order bitstream into frames, up to and including STOP."""
    r = _BitReader(data)
    frames = []
    while True:
        e = r.get(4)
        if e in (E_SILENCE, E_STOP):
            frames.append(Frame(e))
            if e == E_STOP:
                return frames
            continue
        rep = r.get(1) == 1
        p = r.get(6)
        if rep:
            k = ()
        else:
            n = N_K_VOICED if p else N_K_UNVOICED
            k = tuple(r.get(K_BITS[i]) for i in range(n))
        frames.append(Frame(e, rep, p, k))


# ---------------------------------------------------------------------------
# Reference decoder
# ---------------------------------------------------------------------------

class Decoder:
    """Float model of the TMS5220 synthesiser -- the spec snd-lpc.hpp implements.

    Per frame: targets are set from the frame (silence keeps the previous K and
    pitch targets with energy 0; repeat keeps the previous K indices; unvoiced
    frames zero the K5..K10 targets). The frame is rendered in 8 sub-steps of
    25 samples. The values interpolate linearly from the previous frame's
    targets, reaching cur = prev + (tgt - prev) * s/8 in sub-step s = 1..8,
    except when the previous frame was silent (or this is the first frame),
    this frame is silent/STOP, or voicing flips -- then cur = tgt for the whole
    frame.

    Per sample: voiced excitation is CHIRP[int(pc)] (0 past the table) with a
    float pitch counter wrapping at the current period; unvoiced excitation is
    +-64 from a 16-bit Galois LFSR (taps 0xB800, seed 1). u10 = E * exc / 8
    drives a 10-stage all-pole lattice; the output u0 is clamped to
    [-2048, 2047], truncated to 8 bits (floor to a multiple of 16) and scaled
    by 1/2048. Clamping is output-only: the lattice state keeps the raw u0.
    """

    def __init__(self, speed: float = 1.0, track_peaks: bool = False) -> None:
        if not speed > 0:
            raise ValueError("speed must be > 0")
        self.frame_len = max(SUBSTEPS, int(round(FRAME_LEN / speed)))
        # end sample (exclusive) of each sub-step; 25, 50, ... 200 at speed 1
        self._sub_end = [int(round(self.frame_len * s / SUBSTEPS)) for s in range(1, SUBSTEPS + 1)]
        self.track_peaks = track_peaks
        self.reset()

    def reset(self) -> None:
        self.x = [0.0] * 10          # lattice backward state, from the previous sample
        self.pc = 0.0                # pitch counter
        self.rng = 1                 # LFSR
        self.e = 0.0                 # targets of the last frame == values at its end
        self.p = 0.0
        self.k = [0.0] * 10
        self.kidx = [None] * 10      # last transmitted K indices (None = never sent)
        self.prev_silent = True      # the first frame never interpolates
        self.prev_voiced = False
        self.peak = 0.0              # max |node| over the lattice (track_peaks only)

    # The encoder snapshots the decoder to try candidate frames.
    def state(self):
        return (list(self.x), self.pc, self.rng, self.e, self.p, list(self.k),
                list(self.kidx), self.prev_silent, self.prev_voiced, self.peak)

    def restore(self, st) -> None:
        (x, self.pc, self.rng, self.e, self.p, k, kidx,
         self.prev_silent, self.prev_voiced, self.peak) = st
        self.x, self.k, self.kidx = list(x), list(k), list(kidx)

    def render(self, fr: Frame, energy_value: float | None = None) -> list:
        """Render one frame; return its raw lattice outputs u0 (before the DAC).

        A STOP frame renders as one frame of silence (the lattice rings down).
        energy_value overrides the target energy of a speech frame with an
        arbitrary float; the encoder uses it for analysis-by-synthesis.
        """
        silent = fr.is_silent
        pe, pp, pk = self.e, self.p, self.k
        if silent:
            te, tp, tk = 0.0, pp, pk
            voiced = self.prev_voiced
        else:
            te = float(ENERGY[fr.energy]) if energy_value is None else float(energy_value)
            tp = float(PITCH[fr.pitch])
            voiced = fr.pitch != 0
            if not fr.repeat:
                for i, v in enumerate(fr.k):
                    self.kidx[i] = v
            tk = [K_TABLES[i][v] / 512.0 if v is not None else 0.0
                  for i, v in enumerate(self.kidx)]
            if not voiced:
                tk[4:] = [0.0] * 6
        interp = not (self.prev_silent or silent or voiced != self.prev_voiced)

        x0, x1, x2, x3, x4, x5, x6, x7, x8, x9 = self.x
        pc, rng, track, peak = self.pc, self.rng, self.track_peaks, self.peak
        out = []
        n = 0
        for s in range(SUBSTEPS):
            if interp:
                f = (s + 1) / SUBSTEPS
                ce = pe + (te - pe) * f
                cp = pp + (tp - pp) * f
                k0, k1, k2, k3, k4, k5, k6, k7, k8, k9 = [a + (b - a) * f for a, b in zip(pk, tk)]
            else:
                ce, cp = te, tp
                k0, k1, k2, k3, k4, k5, k6, k7, k8, k9 = tk
            end = self._sub_end[s]
            while n < end:
                # excitation
                if cp > 0.0:
                    ip = int(pc)
                    exc = CHIRP[ip] if ip < 52 else 0
                    pc += 1.0
                    if pc >= cp:
                        pc -= cp
                else:
                    rng = ((rng >> 1) ^ 0xB800) if rng & 1 else (rng >> 1)
                    exc = -64 if rng & 1 else 64
                u10 = ce * exc / 8.0
                # lattice: every u uses the x values of the previous sample
                u9 = u10 - k9 * x9
                u8 = u9 - k8 * x8
                u7 = u8 - k7 * x7
                u6 = u7 - k6 * x6
                u5 = u6 - k5 * x5
                u4 = u5 - k4 * x4
                u3 = u4 - k3 * x3
                u2 = u3 - k2 * x2
                u1 = u2 - k1 * x1
                u0 = u1 - k0 * x0
                x9 = x8 + k8 * u8
                x8 = x7 + k7 * u7
                x7 = x6 + k6 * u6
                x6 = x5 + k5 * u5
                x5 = x4 + k4 * u4
                x4 = x3 + k3 * u3
                x3 = x2 + k2 * u2
                x2 = x1 + k1 * u1
                x1 = x0 + k0 * u0
                x0 = u0
                if track:
                    peak = max(peak, abs(u10), abs(u9), abs(u8), abs(u7), abs(u6), abs(u5),
                               abs(u4), abs(u3), abs(u2), abs(u1), abs(u0), abs(x1), abs(x2),
                               abs(x3), abs(x4), abs(x5), abs(x6), abs(x7), abs(x8), abs(x9))
                out.append(u0)
                n += 1

        self.x = [x0, x1, x2, x3, x4, x5, x6, x7, x8, x9]
        self.pc, self.rng, self.peak = pc, rng, peak
        self.e, self.p, self.k = te, tp, list(tk)
        self.prev_silent = silent
        self.prev_voiced = voiced
        return out


def dac(u0) -> np.ndarray:
    """The chip's output stage: clamp to 12 bits, keep the top 8, scale to [-1, 1)."""
    y = np.clip(np.asarray(u0, dtype=np.float64), -2048.0, 2047.0)
    return np.floor(y / 16.0) * 16.0 / 2048.0


def decode(bits: bytes, chip: str = '5220', speed: float = 1.0, stats: dict | None = None) -> np.ndarray:
    """Decode one utterance to float32 samples at 8 kHz (the STOP frame renders as silence).

    speed scales the frame length (2.0 = 100 samples per frame) without
    touching pitch. If `stats` is a dict it receives: frames, samples, clipped
    (samples whose raw u0 fell outside [-2048, 2047]), peak_out (max |u0|),
    peak_node (max |u|/|x| anywhere in the lattice) and raw (the u0 array).
    """
    if chip != '5220':
        raise ValueError(f"only the TMS5220 tables are implemented, not {chip!r}")
    frames = unpack(bits)
    dec = Decoder(speed, track_peaks=stats is not None)
    raw = []
    for fr in frames:
        raw.extend(dec.render(fr))
    u = np.asarray(raw, dtype=np.float64)
    if stats is not None:
        stats.update(frames=len(frames), samples=len(u),
                     clipped=int(np.count_nonzero((u > 2047.0) | (u < -2048.0))),
                     peak_out=float(np.abs(u).max(initial=0.0)), peak_node=float(dec.peak), raw=u)
    return dac(u).astype(np.float32)


# ---------------------------------------------------------------------------
# Encoder
# ---------------------------------------------------------------------------

@dataclass(frozen=True)
class EncoderConfig:
    """Encoder tunables. The defaults are what gen_vocab.py ships with."""
    # spectral analysis
    lpc_window: int = 240           # Hamming analysis window, samples (30 ms)
    k_offset: int = 160             # window centre relative to frame start when the
                                    # decoder interpolates into the frame (it reaches
                                    # the targets only at the frame's end) ...
    k_offset_jump: int = 125        # ... and when it jumps straight to them (after
                                    # silence, or where voicing flips)
    preemph_voiced: float = 0.85    # voiced frames are chirp-excited (low-pass)
    preemph_unvoiced: float = 0.0   # unvoiced frames get white LFSR noise
    lag_window_hz: float = 0.0      # Gaussian lag window (bandwidth expansion)
    noise_floor: float = 1.0001     # white-noise correction, r0 *= this
    beam: int = 4                   # K quantiser: best partial paths kept per stage
    cands: int = 3                  # K quantiser: table entries tried per path and stage
    # pitch and voicing. A frame within voice_gate_db of the loudest is voiced if
    # any of three tests passes: periodic enough at a low zero-crossing rate
    # (the main test); very periodic at a moderate rate (open vowels, voiced
    # fricatives); or a loud low-rate sonorant whose correlation dipped (the
    # TTS pitch glides and jumps at unit joins, e.g. mid-diphthong in "five").
    # Then 1-2 frame holes in a voiced run are filled if they are loud and not
    # fricative-like: real unvoiced consonants between vowels are much quieter
    # (f, h, stop closures) or cross zero far more often (s, sh)
    p_offset: int = 100             # pitch/voicing analysis centre (mid-frame)
    pitch_lowpass_hz: float = 1000.0
    nccf_window: int = 240          # correlation window, samples
    lag_min: int = 20
    lag_max: int = 160
    lag_bias: float = 0.10          # prefer short lags by up to this much (kills sub-octaves)
    voice_nccf: float = 0.40        # main test: normalised correlation peak >= this
    voice_zcr: float = 0.30         #   and zero crossings per sample <= this
    strong_nccf: float = 0.70       # very periodic test
    strong_zcr: float = 0.45
    sonorant_nccf: float = 0.25     # sonorant test
    sonorant_zcr: float = 0.30
    sonorant_db: float = -10.0      #   and within this of the loudest frame
    fill_zcr: float = 0.45          # hole filling: every hole frame at most this rate
    fill_db: float = -15.0          #   and within this of the loudest frame
    voice_gate_db: float = -30.0    # nothing quieter than this is voiced
    confident_nccf: float = 0.50    # voiced frames below this take their neighbours' pitch
    # energy
    silence_gate_db: float = -40.0  # frames this far under the loudest become silence
    energy_ac: bool = True          # match AC (mean-removed) RMS: the chirp is unipolar
    energy_search: str = 'viterbi'  # 'viterbi' (joint over frames) or 'greedy' (frame by frame)
    clip_weight: float = 64.0       # viterbi: cost of clipped-excess energy relative to the target
    tail_frames: int = 6            # viterbi: frames of lattice ringing tracked per basis response
    repeat: bool = True             # R=1 when the K indices equal the previous frame's


def _preemphasis(x: np.ndarray, mu: float) -> np.ndarray:
    if mu == 0.0:
        return x
    y = np.empty_like(x)
    y[0] = x[0]
    y[1:] = x[1:] - mu * x[:-1]
    return y


def levinson(r: np.ndarray, order: int):
    """Levinson-Durbin. Returns (a, k, err) with A(z) = 1 + sum a[j] z^-j, a[i] = k_i."""
    a = np.zeros(order + 1)
    a[0] = 1.0
    ks = np.zeros(order)
    err = float(r[0])
    for i in range(1, order + 1):
        acc = r[i] + np.dot(a[1:i], r[i - 1:0:-1])
        k = -acc / err
        a[1:i] = a[1:i] + k * a[i - 1:0:-1]
        a[i] = k
        ks[i - 1] = k
        err *= 1.0 - k * k
    return a, ks, err


def k_to_a(k) -> np.ndarray:
    """Step-up recursion: reflection coefficients -> A(z) = 1 + sum a[j] z^-j."""
    a = np.array([1.0])
    for ki in k:
        ap = np.append(a, 0.0)
        a = ap + ki * ap[::-1]
    return a


def quantize_k(r: np.ndarray, order: int, beam: int = 4, cands: int = 3) -> tuple:
    """Quantise reflection coefficients to the chip tables, closed-loop.

    Stage i extends each surviving order-(i-1) filter a by candidate table
    values k: A_i = [a, 0] + k [0, reversed a]. Its residual energy on the
    windowed signal, E(k) = A_i' R A_i = alpha + 2 beta k + alpha k^2, is exact
    for the *quantised* lower stages, so later coefficients compensate earlier
    rounding. The `cands` entries nearest the per-path optimum -beta/alpha are
    tried and the `beam` lowest-error paths survive; beam=cands=1 is plain
    sequential nearest-entry quantisation. Every table entry has |k| < 1, so
    any result is stable.
    """
    rr = np.asarray(r[:order + 1], dtype=np.float64)
    R = rr[np.abs(np.subtract.outer(np.arange(order + 1), np.arange(order + 1)))]
    paths = [(float(rr[0]), np.array([1.0]), ())]
    for i in range(1, order + 1):
        tab = _KF[i - 1]
        Ri = R[:i + 1, :i + 1]
        grown = []
        for _, a, idx in paths:
            ap = np.append(a, 0.0)
            bp = ap[::-1]
            Ra = Ri @ ap
            alpha = float(ap @ Ra)
            beta = float(bp @ Ra)
            kopt = -beta / alpha if alpha > 0.0 else 0.0
            for j in np.argsort(np.abs(tab - kopt), kind='stable')[:cands]:
                kq = tab[j]
                grown.append((alpha + 2.0 * beta * kq + alpha * kq * kq, ap + kq * bp, idx + (int(j),)))
        grown.sort(key=lambda t: t[0])
        paths = grown[:beam]
    return paths[0][2]


def _autocorr(seg: np.ndarray, order: int, lagwin: np.ndarray, noise_floor: float) -> np.ndarray:
    n = len(seg)
    r = np.array([np.dot(seg[:n - k], seg[k:]) for k in range(order + 1)])
    r *= lagwin[:order + 1]
    r[0] *= noise_floor
    return r


def _frame_rms(x: np.ndarray, n_frames: int, ac: bool) -> np.ndarray:
    fr = np.zeros(n_frames * FRAME_LEN)
    fr[:len(x)] = x
    fr = fr.reshape(n_frames, FRAME_LEN)
    if ac:
        fr = fr - fr.mean(axis=1, keepdims=True)
    return np.sqrt((fr * fr).mean(axis=1))


def _zcr(x: np.ndarray, n_frames: int) -> np.ndarray:
    fr = np.zeros(n_frames * FRAME_LEN)
    fr[:len(x)] = x
    sgn = np.signbit(fr.reshape(n_frames, FRAME_LEN))
    return np.count_nonzero(sgn[:, 1:] != sgn[:, :-1], axis=1) / (FRAME_LEN - 1)


def _nccf(x: np.ndarray, n_frames: int, cfg: EncoderConfig):
    """Normalised cross-correlation per frame over lags lag_min..lag_max.

    For lag t the two correlated windows are centred at c - t/2 and c + t/2,
    so every lag is measured symmetrically about the analysis centre c.
    Computed on a low-passed copy (fundamental plus first few harmonics).
    """
    sos = signal.butter(4, cfg.pitch_lowpass_hz, 'low', fs=FS, output='sos')
    s = signal.sosfiltfilt(sos, x)
    W = cfg.nccf_window
    lags = np.arange(cfg.lag_min, cfg.lag_max + 1)
    pad = W + cfg.lag_max
    sp = np.concatenate([np.zeros(pad), s, np.zeros(pad + FRAME_LEN)])
    i1 = (-(lags + W) // 2)[:, None] + np.arange(W)[None, :]
    i2 = i1 + lags[:, None]
    out = np.zeros((n_frames, len(lags)))
    for f in range(n_frames):
        c = pad + f * FRAME_LEN + cfg.p_offset
        a, b = sp[c + i1], sp[c + i2]
        den = np.sqrt((a * a).sum(axis=1) * (b * b).sum(axis=1))
        out[f] = (a * b).sum(axis=1) / np.maximum(den, 1e-20)
    return out, lags


def _pick_lag(row: np.ndarray, lags: np.ndarray, bias: float):
    """Best lag (fractional, parabolic) and its correlation for one frame."""
    score = row * (1.0 - bias * (lags - lags[0]) / (lags[-1] - lags[0]))
    # only local maxima are candidates (a rising edge at lag_min is not a period)
    peak = np.zeros(len(row), dtype=bool)
    peak[1:-1] = (row[1:-1] >= row[:-2]) & (row[1:-1] >= row[2:])
    if not peak.any():
        return float(lags[int(np.argmax(row))]), float(row.max())
    j = int(np.argmax(np.where(peak, score, -np.inf)))
    frac = 0.0
    if 0 < j < len(row) - 1:
        den = row[j - 1] - 2.0 * row[j] + row[j + 1]
        if den < 0.0:
            frac = 0.5 * (row[j - 1] - row[j + 1]) / den
    return float(lags[j] + frac), float(row[j])


def _corr_at(row: np.ndarray, lags: np.ndarray, lag: float) -> float:
    """Correlation of a frame at an arbitrary lag (best of the two nearest bins)."""
    j = int(round(lag)) - int(lags[0])
    if j < 0 or j >= len(row):
        return -1.0
    lo, hi = max(j - 1, 0), min(j + 2, len(row))
    return float(row[lo:hi].max())


def analyze_voicing(x: np.ndarray, cfg: EncoderConfig | None = None) -> dict:
    """Pitch track and voicing decisions per frame (scale-independent).

    Returns a dict of per-frame arrays: voiced (bool), lag (float period in
    samples, 0 when unvoiced), nccf, zcr, rel_db (frame RMS re the loudest).
    """
    cfg = cfg or EncoderConfig()
    x = np.asarray(x, dtype=np.float64)
    n = max(1, -(-len(x) // FRAME_LEN))
    rms = _frame_rms(x, n, ac=True)
    rel_db = 20.0 * np.log10(np.maximum(rms, 1e-12) / max(rms.max(), 1e-12))
    zcr = _zcr(x, n)
    corr, lags = _nccf(x, n, cfg)
    lag = np.zeros(n)
    peak = np.zeros(n)
    for f in range(n):
        lag[f], peak[f] = _pick_lag(corr[f], lags, cfg.lag_bias)

    main = (peak >= cfg.voice_nccf) & (zcr <= cfg.voice_zcr)
    strong = (peak >= cfg.strong_nccf) & (zcr <= cfg.strong_zcr)
    sonorant = (peak >= cfg.sonorant_nccf) & (zcr <= cfg.sonorant_zcr) & (rel_db >= cfg.sonorant_db)
    voiced = (main | strong | sonorant) & (rel_db >= cfg.voice_gate_db)

    # a lone voiced frame between unvoiced ones is a glitch unless strongly periodic
    v = voiced.copy()
    for f in range(n):
        if v[f] and not (f > 0 and v[f - 1]) and not (f + 1 < n and v[f + 1]) and peak[f] < 0.75:
            voiced[f] = False
    # a one- or two-frame unvoiced hole inside a voiced run is a dropout if it is loud and
    # not fricative-like
    f = 1
    while f < n:
        if voiced[f - 1] and not voiced[f]:
            g = f
            while g < n and not voiced[g]:
                g += 1
            if g < n and g - f <= 2 and all(zcr[h] <= cfg.fill_zcr and rel_db[h] >= cfg.fill_db
                                             for h in range(f, g)):
                voiced[f:g] = True
            f = g
        f += 1

    # low-confidence voiced frames borrow the pitch of confident voiced neighbours
    conf = voiced & (peak >= cfg.confident_nccf)
    for f in np.flatnonzero(voiced & ~conf):
        nb = [lag[g] for g in range(max(0, f - 3), min(n, f + 4)) if conf[g]]
        if nb:
            lag[f] = float(np.median(nb))

    # octave clean-up against the local median period of the voiced neighbourhood
    for f in np.flatnonzero(voiced):
        lo, hi = max(0, f - 3), min(n, f + 4)
        nb = [lag[g] for g in range(lo, hi) if voiced[g] and g != f]
        if not nb:
            continue
        med = float(np.median(nb))
        for alt in (lag[f] / 2.0, lag[f] * 2.0, lag[f] / 3.0):
            if (cfg.lag_min <= alt <= cfg.lag_max and abs(math.log(alt / med)) < abs(math.log(lag[f] / med))
                    and abs(math.log(alt / med)) < 0.2 and _corr_at(corr[f], lags, alt) >= 0.7 * peak[f]):
                lag[f] = alt
                break

    # 3-point median along each voiced run
    sm = lag.copy()
    for f in np.flatnonzero(voiced):
        win = [lag[g] for g in (f - 1, f, f + 1) if 0 <= g < n and voiced[g]]
        sm[f] = float(np.median(win))
    sm[~voiced] = 0.0
    return dict(voiced=voiced, lag=sm, nccf=peak, zcr=zcr, rel_db=rel_db)


def pitch_index(lag: float) -> int:
    """Nearest PITCH index (1..63) to a period in samples, in the log domain."""
    return 1 + int(np.argmin(np.abs(_LOG_PITCH - math.log(max(lag, 1.0)))))


def _energy_greedy(protos: list, target: np.ndarray) -> list:
    """Frame-by-frame analysis-by-synthesis (the simple method, kept for comparison).

    With the lattice state carried from the frames already chosen, the output
    of this frame is affine in its target energy, y = a + E b, so two renders
    (E = 0 and E = 1) price all 14 energy indices exactly; the index nearest
    the target RMS in the log domain wins.
    """
    energies = np.array(ENERGY[1:15], dtype=np.float64)
    dec = Decoder()
    out = []
    for fr, t in zip(protos, target):
        if fr.is_silent:
            out.append(E_SILENCE)
            dec.render(fr)
            continue
        st = dec.state()
        a = np.asarray(dec.render(fr, energy_value=0.0))
        dec.restore(st)
        b = np.asarray(dec.render(fr, energy_value=1.0)) - a
        dec.restore(st)
        a, b = a - a.mean(), b - b.mean()
        # RMS(a + E b)^2 = A + 2 E B + E^2 C for every candidate at once
        A, B, C = np.dot(a, a) / len(a), np.dot(a, b) / len(a), np.dot(b, b) / len(a)
        ms = np.maximum(A + 2.0 * energies * B + energies * energies * C, 1e-30)
        j = 1 + int(np.argmin(np.abs(0.5 * np.log(ms) - math.log(t))))
        out.append(j)
        dec.render(replace(fr, energy=j))
    return out


def _energy_viterbi(protos: list, target: np.ndarray, gate: float, cfg: EncoderConfig) -> list:
    """Choose all energy indices jointly (Viterbi) -- see encode() step 3.

    protos are the frames with every non-silent energy still unchosen; target
    and gate are in lattice units (u0 full scale 2048).
    """
    n = len(protos)
    span = cfg.tail_frames + 1
    energies = np.array(ENERGY[1:15], dtype=np.float64)

    # Decoder state at every frame start with all energies 0: the lattice stays
    # exactly zero while the pitch counter, LFSR and K/pitch targets advance as
    # they will in the real render.
    dec = Decoder()
    snaps = []
    for fr in protos:
        snaps.append(dec.state())
        dec.render(fr, energy_value=None if fr.is_silent else 0.0)

    # basis[j]: the output (span frames) of a unit target energy in frame j alone,
    # including its ramp-down into frame j+1 and the lattice ringing after it
    basis = [None] * n
    for j, fr in enumerate(protos):
        if fr.is_silent:
            continue
        dec.restore(snaps[j])
        g = np.zeros((span, FRAME_LEN))
        g[0] = dec.render(fr, energy_value=1.0)
        peak = np.abs(g[0]).max()
        for m in range(1, min(span, n - j)):
            nxt = protos[j + m]
            g[m] = dec.render(nxt, energy_value=None if nxt.is_silent else 0.0)
            if np.abs(g[m]).max() < 1e-6 * peak:
                break
        basis[j] = g

    def frame_cost(y, t):
        """Squared log-RMS error plus the clipped excess, relative to the target."""
        yc = y - y.mean(axis=-1, keepdims=True) if cfg.energy_ac else y
        rms = np.sqrt((yc * yc).mean(axis=-1))
        over = np.maximum(np.abs(y) - 2047.0, 0.0)
        return ((np.log(np.maximum(rms, 1e-9)) - math.log(t)) ** 2
                + cfg.clip_weight * (over * over).mean(axis=-1) / (t * t))

    # carry[s] = what the survivor path ending in state s contributes to frames i .. i+span-1
    acc = np.zeros(1)
    carry = np.zeros((1, span, FRAME_LEN))
    back = []
    for i, fr in enumerate(protos):
        if fr.is_silent:
            # only the ringing is heard; penalise it where it rises above the gate
            yc = carry[:, 0] - carry[:, 0].mean(axis=-1, keepdims=True)
            rms = np.sqrt((yc * yc).mean(axis=-1))
            cost = acc + np.maximum(np.log(np.maximum(rms, 1e-9) / gate), 0.0) ** 2
            s = int(np.argmin(cost))
            back.append(np.array([s]))
            acc = cost[s:s + 1]
            nxt = np.zeros((1, span, FRAME_LEN))
            nxt[0, :-1] = carry[s, 1:]
            carry = nxt
            continue
        g = basis[i]
        y = carry[:, None, 0, :] + energies[None, :, None] * g[None, None, 0, :]   # (S, 14, 200)
        cost = acc[:, None] + frame_cost(y, target[i])
        best = np.argmin(cost, axis=0)                                             # per energy
        back.append(best)
        acc = cost[best, np.arange(len(energies))]
        nxt = np.zeros((len(energies), span, FRAME_LEN))
        nxt[:, :-1] = carry[best, 1:] + energies[:, None, None] * g[None, 1:]
        carry = nxt

    # trace back
    out = [0] * n
    s = int(np.argmin(acc))
    for i in range(n - 1, -1, -1):
        out[i] = E_SILENCE if protos[i].is_silent else 1 + s
        s = int(back[i][s])
    return out


def encode(wav8k: np.ndarray, gain: float = 1.0, cfg: EncoderConfig | None = None,
           info: dict | None = None) -> list:
    """Encode 8 kHz float audio into TMS5220 frames, ending with STOP.

    The decoded output (float, full scale 1.0) is aimed at gain * wav8k, frame
    by frame in RMS. Steps:
      1. Voicing and pitch per frame (analyze_voicing). Frames under the
         silence gate become silence frames.
      2. Spectrum per frame: LPC-10 (voiced, pre-emphasised to undo the
         chirp's low-pass tilt) or LPC-4 (unvoiced, white LFSR excitation) on
         a Hamming window, quantised with the closed-loop beam search
         (quantize_k). The window is centred k_offset into the frame where the
         decoder interpolates into it, k_offset_jump where it jumps. A frame
         whose K indices equal the ones in force (same voicing, not after
         silence) is sent as a repeat frame.
      3. Energy by analysis-by-synthesis through the reference Decoder. With
         the K, pitch and silence decisions fixed, the decoder output is
         exactly linear in the vector of target energies (the excitation does
         not depend on them and the clamp is output-only): y = sum_j E_j g_j,
         where g_j is frame j's unit-energy response -- its own frame, its
         ramp-down into the next, and the lattice ringing after. A Viterbi
         search over the 14 energy indices (state = the previous frame's
         index, each survivor carrying its exact ringing forward) minimises
         the squared log error between decoded and target frame RMS, plus a
         penalty on clipped samples. It matters where the filter gain moves
         fast: the energy of frame i still sounds, ramping down, through
         frame i+1, so frame-by-frame choices overshoot there by 10-17 dB.
    If `info` is a dict it receives the per-frame analysis plus `raw`, the
    encoder's own render (bit-identical to decode() of the packed frames).
    """
    cfg = cfg or EncoderConfig()
    x = np.asarray(wav8k, dtype=np.float64)
    n = max(1, -(-len(x) // FRAME_LEN))
    va = analyze_voicing(x, cfg)
    voiced = va['voiced']

    # --- which frames will be silent, and which ones the decoder interpolates into
    target = gain * _frame_rms(x, n, ac=cfg.energy_ac)
    gate = target.max() * 10.0 ** (cfg.silence_gate_db / 20.0)
    silent = (target <= gate) | (target <= 1e-9)
    interp = np.zeros(n, dtype=bool)
    interp[1:] = ~silent[1:] & ~silent[:-1] & (voiced[1:] == voiced[:-1])

    # --- spectral analysis per frame
    win = np.hamming(cfg.lpc_window)
    lagwin = np.exp(-0.5 * (2.0 * np.pi * cfg.lag_window_hz * np.arange(N_K_VOICED + 1) / FS) ** 2)
    pad = cfg.lpc_window
    ext = np.concatenate([np.zeros(pad), x, np.zeros(pad + FRAME_LEN)])
    sig_v = _preemphasis(ext, cfg.preemph_voiced)
    sig_u = _preemphasis(ext, cfg.preemph_unvoiced)
    kq = []
    for f in range(n):
        centre = cfg.k_offset if interp[f] else cfg.k_offset_jump
        start = pad + f * FRAME_LEN + centre - cfg.lpc_window // 2
        if voiced[f]:
            seg = sig_v[start:start + cfg.lpc_window] * win
            order = N_K_VOICED
        else:
            seg = sig_u[start:start + cfg.lpc_window] * win
            order = N_K_UNVOICED
        r = _autocorr(seg, order, lagwin, cfg.noise_floor)
        if r[0] <= 1e-20:           # digital silence: any valid filter will do
            r = np.zeros(order + 1)
            r[0] = 1.0
        kq.append(quantize_k(r, order, cfg.beam, cfg.cands))

    # --- frame prototypes (energy still open) and repeat flags
    protos = []
    kidx = [None] * N_K_VOICED            # the K indices in force at the decoder
    for f in range(n):
        if silent[f]:
            protos.append(SILENCE)
            continue
        p = pitch_index(va['lag'][f]) if voiced[f] else 0
        k = kq[f]
        prev = protos[-1] if protos else SILENCE
        rep = (cfg.repeat and not prev.is_silent and prev.voiced == (p != 0)
               and tuple(kidx[:len(k)]) == k)
        if not rep:
            kidx[:len(k)] = k
        protos.append(Frame(1, rep, p, () if rep else k))

    # --- energy
    t_lat = target * 2048.0
    if cfg.energy_search == 'greedy':
        e_idx = _energy_greedy(protos, t_lat)
    elif cfg.energy_search == 'viterbi':
        e_idx = _energy_viterbi(protos, t_lat, gate * 2048.0, cfg)
    else:
        raise ValueError(f'unknown energy_search {cfg.energy_search!r}')
    frames = [fr if fr.is_silent else replace(fr, energy=e) for fr, e in zip(protos, e_idx)]
    frames.append(STOP)

    if info is not None:
        dec = Decoder()
        raw = np.concatenate([dec.render(fr) for fr in frames])
        info.update(va)
        info.update(target=target, energy=np.array(e_idx), k=kq, interp=interp, raw=raw)
    return frames
