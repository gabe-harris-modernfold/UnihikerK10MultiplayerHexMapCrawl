"""Generate snd-vocab.h: the K10's spoken vocabulary as TMS5220 LPC bitstreams.

    python scripts/lpc/gen_vocab.py                    # regenerate everything
    python scripts/lpc/gen_vocab.py --only N1 DOOM32   # re-check a subset (header untouched)
    python scripts/lpc/gen_vocab.py --rate -2          # override the SAPI speaking rate

Pipeline, per phrase of scripts/lpc/phrases.txt (file order = enum order):
  1. TTS. Windows SAPI (System.Speech, driven through PowerShell) speaks the
     text to a 16 kHz / 16-bit / mono WAV. WAVs are cached in out/tts/ under a
     hash of (voice, rate, text), so a re-run only synthesises changed phrases.
  2. Conditioning. Resample to 8 kHz (resample_poly), high-pass at 70 Hz (the
     SAPI voice carries a small DC offset), trim leading/trailing silence to
     20 ms, and scale so the 95th-percentile RMS of the voiced 25 ms frames is
     REF_RMS -- every phrase then comes out equally loud.
  3. Encode with lpc_tms.encode() (see its docstring for the method).
  4. Validate. pack -> unpack must give back the identical frame list, and
     decode() of the packed bits must equal the encoder's own render bit for
     bit. Then measure against the conditioned source: log-spectral distance
     on voiced frames, clipped samples, voicing and the correlation of the two
     RMS envelopes. Writes out/src8k/<ID>.wav (the conditioned source, at the
     level the decoder aims for), out/decoded/<ID>.wav and out/report.txt.
  5. Write snd-vocab.h at the repo root -- full runs only, and only if every
     check passed (round trip, decoder parity, nothing silent, envelope r,
     byte budget).

The metric, for reading report.txt: LSD is the RMS difference in dB between
cepstrally smoothed log power spectra (Hamming, 512-point FFT, lifter 30,
floor 50 dB under the source frame's peak) of each 25 ms source frame and the
same frame of the decoder output, over 100..3800 Hz, averaged over the frames
the encoder sent as voiced. Smoothing removes the harmonic fine structure,
so it scores the spectral envelope: formants and tilt.
"""

from __future__ import annotations

import argparse
import hashlib
import os
import re
import subprocess
import sys
import time

import numpy as np
from scipy import signal
from scipy.io import wavfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
import lpc_tms as lpc  # noqa: E402

PHRASES = os.path.join(HERE, 'phrases.txt')
OUT = os.path.join(HERE, 'out')
HEADER = os.path.join(ROOT, 'snd-vocab.h')

VOICE = 'Microsoft David Desktop'
RATE = -1                  # SAPI rate, -10..10; slightly slow speech encodes better
TTS_FS = 16000             # SAPI output rate; condition() resamples whatever it gets

HPF_HZ = 70.0              # below David's lowest F0
TRIM_GATE_DB = -50.0       # 5 ms frames this far under the loudest count as silence
TRIM_KEEP = 0.020          # seconds of silence kept at each end
REF_RMS = 0.35             # p95 voiced-frame RMS after normalisation (full scale 1.0)

BYTE_BUDGET = 40 * 1024
MIN_ENV_R = 0.80

# LSD metric (see module docstring)
NFFT = 512
LIFTER = 30
LSD_FLOOR_DB = -50.0
LSD_BAND = (100.0, 3800.0)


# ---------------------------------------------------------------------------
# Phrase list
# ---------------------------------------------------------------------------

def read_phrases(path: str = PHRASES) -> list:
    """[(ID, text)] in file order. `#` starts a comment; blank lines are skipped."""
    out, seen = [], set()
    with open(path, encoding='utf-8') as fh:
        for n, line in enumerate(fh, 1):
            line = line.split('#', 1)[0].strip()
            if not line:
                continue
            if '|' not in line:
                sys.exit(f"{path}:{n}: expected 'ID | text'")
            pid, text = (s.strip() for s in line.split('|', 1))
            if not re.fullmatch(r'[A-Za-z][A-Za-z0-9_]*', pid):
                sys.exit(f"{path}:{n}: ID {pid!r} is not a valid C identifier tail")
            if pid in seen:
                sys.exit(f"{path}:{n}: duplicate ID {pid}")
            if not text or not text.isascii():
                sys.exit(f"{path}:{n}: text must be non-empty ASCII")
            seen.add(pid)
            out.append((pid, text))
    return out


# ---------------------------------------------------------------------------
# TTS (SAPI through PowerShell), cached
# ---------------------------------------------------------------------------

_SAPI_PS1 = r"""
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Speech
$s = New-Object System.Speech.Synthesis.SpeechSynthesizer
$s.SelectVoice('@VOICE@')
$s.Rate = @RATE@
$fmt = New-Object System.Speech.AudioFormat.SpeechAudioFormatInfo(@FS@,
    [System.Speech.AudioFormat.AudioBitsPerSample]::Sixteen,
    [System.Speech.AudioFormat.AudioChannel]::Mono)
foreach ($line in [System.IO.File]::ReadAllLines('@JOBS@', [System.Text.Encoding]::UTF8)) {
    if (-not $line) { continue }
    $path, $text = $line -split "`t", 2
    $part = $path + '.part'
    $s.SetOutputToWaveFile($part, $fmt)
    $s.Speak($text)
    $s.SetOutputToNull()
    Move-Item -LiteralPath $part -Destination $path -Force
}
$s.Dispose()
"""


def tts_key(voice: str, rate: int, text: str) -> str:
    return hashlib.sha1(f'{voice}\n{rate}\n{text}'.encode('utf-8')).hexdigest()[:16]


def synthesize(phrases: list, voice: str, rate: int) -> dict:
    """{ID: path of the 16 kHz SAPI WAV}, synthesising only what is not cached."""
    tts_dir = os.path.join(OUT, 'tts')
    os.makedirs(tts_dir, exist_ok=True)
    paths, jobs = {}, []
    for pid, text in phrases:
        p = os.path.join(tts_dir, tts_key(voice, rate, text) + '.wav')
        paths[pid] = p
        if not os.path.exists(p) and (p, text) not in jobs:
            jobs.append((p, text))
    if jobs:
        print(f'TTS: synthesising {len(jobs)} phrase(s) with {voice}, rate {rate} ...')
        jobs_file = os.path.join(tts_dir, 'jobs.tsv')
        with open(jobs_file, 'w', encoding='utf-8', newline='\n') as fh:
            for p, text in jobs:
                fh.write(f'{p}\t{text}\n')
        ps1 = os.path.join(tts_dir, 'sapi.ps1')
        with open(ps1, 'w', encoding='utf-8-sig', newline='\r\n') as fh:
            fh.write(_SAPI_PS1.replace('@VOICE@', voice.replace("'", "''"))
                     .replace('@RATE@', str(int(rate)))
                     .replace('@FS@', str(TTS_FS))
                     .replace('@JOBS@', jobs_file.replace("'", "''")))
        res = subprocess.run(['powershell', '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', ps1],
                             capture_output=True, text=True)
        missing = [p for p, _ in jobs if not os.path.exists(p)]
        if res.returncode != 0 or missing:
            sys.exit(f'SAPI failed (exit {res.returncode}, {len(missing)} missing):\n{res.stdout}\n{res.stderr}')
    return paths


# ---------------------------------------------------------------------------
# Conditioning
# ---------------------------------------------------------------------------

def condition(path: str) -> np.ndarray:
    """SAPI WAV -> 8 kHz float, DC-free, trimmed, loudness-normalised."""
    sr, pcm = wavfile.read(path)
    x = pcm.astype(np.float64) / 32768.0
    if x.ndim > 1:
        x = x.mean(axis=1)
    g = np.gcd(lpc.FS, sr)
    x = signal.resample_poly(x, lpc.FS // g, sr // g)
    x = signal.sosfiltfilt(signal.butter(4, HPF_HZ, 'high', fs=lpc.FS, output='sos'), x)

    hop = lpc.FS // 200                                    # 5 ms
    n = len(x) // hop
    env = np.sqrt((x[:n * hop].reshape(n, hop) ** 2).mean(axis=1))
    if n == 0 or env.max() <= 0.0:
        sys.exit(f'{path}: SAPI produced no audio')
    live = np.flatnonzero(env > env.max() * 10.0 ** (TRIM_GATE_DB / 20.0))
    keep = int(TRIM_KEEP * lpc.FS)
    x = x[max(0, live[0] * hop - keep):min(len(x), (live[-1] + 1) * hop + keep)]

    va = lpc.analyze_voicing(x)
    nf = len(va['voiced'])
    fr = np.zeros(nf * lpc.FRAME_LEN)
    fr[:len(x)] = x
    fr = fr.reshape(nf, lpc.FRAME_LEN)
    rms = np.sqrt(((fr - fr.mean(axis=1, keepdims=True)) ** 2).mean(axis=1))
    pool = rms[va['voiced']] if va['voiced'].any() else rms[va['rel_db'] > -30.0]
    return x * (REF_RMS / np.percentile(pool, 95))


# ---------------------------------------------------------------------------
# Metrics
# ---------------------------------------------------------------------------

def _frames(x: np.ndarray, n: int) -> np.ndarray:
    fr = np.zeros(n * lpc.FRAME_LEN)
    m = min(len(x), len(fr))
    fr[:m] = x[:m]
    return fr.reshape(n, lpc.FRAME_LEN)


def _smooth_db(fr: np.ndarray, floor: np.ndarray) -> np.ndarray:
    """Cepstrally smoothed log power spectrum (dB) of each row."""
    w = np.hamming(fr.shape[1])
    X = np.fft.rfft((fr - fr.mean(axis=1, keepdims=True)) * w, NFFT)
    c = np.fft.irfft(10.0 * np.log10(np.abs(X) ** 2 + floor[:, None]), NFFT)
    c[:, LIFTER + 1:NFFT - LIFTER] = 0.0
    return np.fft.rfft(c, NFFT).real


def lsd_frames(src: np.ndarray, dec: np.ndarray, n: int) -> np.ndarray:
    """Per-frame log-spectral distance (dB) between two signals, first n frames."""
    a, b = _frames(src, n), _frames(dec, n)
    w = np.hamming(lpc.FRAME_LEN)
    pk = (np.abs(np.fft.rfft((a - a.mean(axis=1, keepdims=True)) * w, NFFT)) ** 2).max(axis=1)
    floor = np.maximum(pk, 1e-20) * 10.0 ** (LSD_FLOOR_DB / 10.0)
    f = np.arange(NFFT // 2 + 1) * lpc.FS / NFFT
    band = (f >= LSD_BAND[0]) & (f <= LSD_BAND[1])
    d = _smooth_db(a, floor)[:, band] - _smooth_db(b, floor)[:, band]
    return np.sqrt((d * d).mean(axis=1))


def frame_rms(x: np.ndarray, n: int) -> np.ndarray:
    fr = _frames(x, n)
    return np.sqrt(((fr - fr.mean(axis=1, keepdims=True)) ** 2).mean(axis=1))


def self_test() -> None:
    """Refuse to run if the codec's fundamentals are off (fast, runs every time)."""
    # the decoder lattice must be the all-pole filter 1/A(z), A = step-up of +K
    kidx = (3, 20, 9, 7, 5, 11, 2, 6, 1, 4)
    fr = [lpc.Frame(9, False, 44, kidx)] + [lpc.Frame(9, True, 44, ())] * 3
    dec = lpc.Decoder()
    raw = np.concatenate([dec.render(f) for f in fr])
    per = lpc.PITCH[44]
    exc = np.array([lpc.CHIRP[i % per] if i % per < 52 else 0 for i in range(len(raw))], float)
    k = [lpc.K_TABLES[i][kidx[i]] / 512.0 for i in range(10)]
    ref = signal.lfilter([1.0], lpc.k_to_a(k), lpc.ENERGY[9] * exc / 8.0)
    assert np.abs(ref - raw).max() < 1e-9 * np.abs(ref).max(), 'lattice != 1/A(z) with +K'
    # Levinson's k1 on low-pass data is negative (so K1 of voiced speech sits near -1)
    x = signal.lfilter([1.0], [1.0, -1.6, 0.8], np.random.default_rng(1).standard_normal(8000))
    r = np.array([np.dot(x[:len(x) - i], x[i:]) for i in range(3)])
    assert lpc.levinson(r, 2)[1][0] < -0.8, 'Levinson sign convention'
    # Talkie bit order: a lone STOP (E=15) occupies bits 0..3 of byte 0
    assert lpc.pack([lpc.STOP]) == b'\x0f'
    assert lpc.pack([lpc.Frame(1, False, 0, (0, 0, 0, 0)), lpc.STOP]) == bytes.fromhex('080000e001')


# ---------------------------------------------------------------------------
# Per-phrase processing
# ---------------------------------------------------------------------------

def process(pid: str, text: str, wav16: str, cfg: lpc.EncoderConfig | None = None) -> dict:
    """Condition, encode and validate one phrase. Returns frames, bits and metrics."""
    src = condition(wav16)
    info = {}
    frames = lpc.encode(src, 1.0, cfg, info)
    bits = lpc.pack(frames)
    stats = {}
    out = lpc.decode(bits, stats=stats)
    n = len(frames) - 1                          # speech frames (without STOP)

    roundtrip = lpc.unpack(bits) == frames and lpc.pack(lpc.unpack(bits)) == bits
    parity = np.array_equal(stats['raw'], info['raw'])
    speech = np.array([not f.is_silent for f in frames[:-1]])
    voiced = np.array([f.voiced for f in frames[:-1]])
    lsd = lsd_frames(src, out, n)
    rs, rd = frame_rms(src, n), frame_rms(out, n)
    env_r = float(np.corrcoef(rs, rd)[0, 1]) if n > 2 and rs.std() > 0 and rd.std() > 0 else 0.0
    dc = np.abs(_frames(out, n).mean(axis=1))
    dur = len(frames) * lpc.FRAME_LEN / lpc.FS
    return dict(
        id=pid, text=text, frames=frames, bits=bits, src=src, out=out,
        nframes=len(frames), nbytes=len(bits), dur=dur, bps=len(bits) * 8 / dur,
        lsd=float(lsd[voiced].mean()) if voiced.any() else float('nan'),
        lsd_frames=lsd[voiced], lsd_unvoiced=lsd[speech & ~voiced],
        clip=stats['clipped'] / stats['samples'], clipped=stats['clipped'], samples=stats['samples'],
        voicing=float(voiced.sum() / max(1, speech.sum())), env_r=env_r,
        out_rms=float(np.sqrt(np.mean(out.astype(np.float64) ** 2))),
        dc_ratio=float(dc[voiced].mean() / max(rd[voiced].mean(), 1e-12)) if voiced.any() else 0.0,
        peak_node=stats['peak_node'], roundtrip=roundtrip, parity=parity,
        repeats=sum(1 for f in frames if f.repeat), silences=int((~speech).sum()),
        energy=info['energy'])


# ---------------------------------------------------------------------------
# Outputs
# ---------------------------------------------------------------------------

def write_wav(path: str, x: np.ndarray) -> None:
    os.makedirs(os.path.dirname(path), exist_ok=True)
    wavfile.write(path, lpc.FS, np.round(np.clip(x, -1.0, 32767 / 32768) * 32768).astype(np.int16))


def c_string(text: str) -> str:
    return '"' + text.replace('\\', '\\\\').replace('"', '\\"').replace("'", "\\'") + '"'


def write_header(results: list, voice: str, rate: int, path: str = HEADER) -> None:
    total = sum(r['nbytes'] for r in results)
    L = ['#pragma once',
         '// snd-vocab.h -- GENERATED by scripts/lpc/gen_vocab.py from scripts/lpc/phrases.txt.',
         '// Do not edit by hand; edit phrases.txt and re-run the generator.',
         '// TMS5220 LPC-10 frames, Talkie bitstream order (LSB-first per byte), 8 kHz,',
         f'// 200 samples (25 ms) per frame. Voice: {voice}, SAPI rate {rate}. Decoded by snd-lpc.hpp.',
         f'// {len(results)} phrases, {total} bytes of LPC data.',
         '#include <stdint.h>',
         '',
         'enum SndVocab : uint8_t {']
    names = [f"VOC_{r['id']}" + (' = 0' if i == 0 else '') for i, r in enumerate(results)]
    line = ' '
    for nm in names:
        if len(line) + len(nm) + 2 > 100:
            L.append(line.rstrip())
            line = ' '
        line += f' {nm},'
    L += [line.rstrip(), '  VOC_COUNT', '};', '']
    for r in results:
        comment = r['text'].replace('\\', '/')
        L.append(f"static const uint8_t VOC_BITS_{r['id']}[] = {{   "
                 f"// \"{comment}\"  (frames={r['nframes']}, bytes={r['nbytes']})")
        b = r['bits']
        for i in range(0, len(b), 16):
            L.append('  ' + ', '.join(f'0x{v:02x}' for v in b[i:i + 16]) + ',')
        L.append('};')
    L += ['',
          'struct SndVocabEntry { const uint8_t* bits; uint16_t bytes; uint16_t frames; const char* text; };',
          'static const SndVocabEntry SND_VOCAB[VOC_COUNT] = {']
    for r in results:
        L.append(f"  {{ VOC_BITS_{r['id']}, (uint16_t)sizeof(VOC_BITS_{r['id']}), "
                 f"{r['nframes']}, {c_string(r['text'])} }},")
    L += ['};', '']
    text = '\n'.join(L)
    assert text.isascii()
    with open(path, 'w', encoding='ascii', newline='\r\n') as fh:
        fh.write(text)


def write_report(results: list, voice: str, rate: int, subset: bool, problems: list,
                 path: str = os.path.join(OUT, 'report.txt')) -> str:
    tot_bytes = sum(r['nbytes'] for r in results)
    tot_dur = sum(r['dur'] for r in results)
    all_lsd = np.concatenate([r['lsd_frames'] for r in results])
    all_lsd_u = np.concatenate([r['lsd_unvoiced'] for r in results])
    clipped = sum(r['clipped'] for r in results)
    samples = sum(r['samples'] for r in results)
    L = [f'snd-vocab LPC report -- {time.strftime("%Y-%m-%d %H:%M")}',
         f'voice {voice}, SAPI rate {rate}, REF_RMS {REF_RMS}' + ('   ** SUBSET RUN, header not written **' if subset else ''),
         f'encoder: {lpc.EncoderConfig()}',
         'LSD = cepstrally smoothed log-spectral distance on voiced frames, dB (see gen_vocab.py);',
         'clip% = samples whose raw lattice output left [-2048, 2047]; voi% = voiced share of the',
         'non-silent frames; env_r = correlation of source and decoded 25 ms RMS; rep = repeat frames.',
         '',
         f"{'ID':<14}{'frm':>5}{'bytes':>6}{'dur_s':>7}{'bit/s':>6}{'LSD':>6}{'clip%':>7}"
         f"{'voi%':>6}{'env_r':>7}{'rep':>5}  text"]
    for r in results:
        L.append(f"{r['id']:<14}{r['nframes']:>5}{r['nbytes']:>6}{r['dur']:>7.2f}{r['bps']:>6.0f}"
                 f"{r['lsd']:>6.2f}{100 * r['clip']:>7.2f}{100 * r['voicing']:>6.0f}{r['env_r']:>7.3f}"
                 f"{r['repeats']:>5}  {r['text']}")
    worst = sorted(results, key=lambda r: -np.nan_to_num(r['lsd']))[:5]
    L += ['',
          f'phrases              {len(results)}',
          f'total bytes          {tot_bytes}  (budget {BYTE_BUDGET})',
          f'total duration       {tot_dur:.2f} s',
          f'average bit rate     {tot_bytes * 8 / tot_dur:.0f} bit/s',
          f'clipped samples      {100 * clipped / samples:.3f} %',
          f'mean LSD, voiced     {all_lsd.mean():.2f} dB over {len(all_lsd)} frames '
          f'(per-phrase mean {np.nanmean([r["lsd"] for r in results]):.2f})',
          f'mean LSD, unvoiced   {all_lsd_u.mean():.2f} dB over {len(all_lsd_u)} frames',
          f'mean envelope r      {np.mean([r["env_r"] for r in results]):.3f} '
          f'(min {min(r["env_r"] for r in results):.3f})',
          f'repeat frames        {sum(r["repeats"] for r in results)} of {sum(r["nframes"] for r in results)}',
          f'voiced DC / AC RMS   {np.mean([r["dc_ratio"] for r in results]):.3f} (unipolar chirp)',
          f'peak |lattice node|  {max(r["peak_node"] for r in results):.0f} (u0 clamps at 2048)',
          'worst LSD            ' + ', '.join(f"{r['id']} {r['lsd']:.2f}" for r in worst),
          '']
    L += (['PROBLEMS:'] + ['  ' + p for p in problems]) if problems else ['all checks passed']
    text = '\n'.join(L) + '\n'
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'w', encoding='ascii', errors='replace', newline='\r\n') as fh:
        fh.write(text)
    return text


# ---------------------------------------------------------------------------

def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--only', nargs='+', metavar='ID', help='process just these phrases (header untouched)')
    ap.add_argument('--rate', type=int, default=RATE, help=f'SAPI rate -10..10 (default {RATE})')
    ap.add_argument('--voice', default=VOICE, help=f'SAPI voice (default {VOICE!r})')
    args = ap.parse_args(argv)

    self_test()
    phrases = read_phrases()
    if args.only:
        unknown = set(args.only) - {p for p, _ in phrases}
        if unknown:
            sys.exit(f"unknown ID(s): {' '.join(sorted(unknown))}")
        phrases = [(p, t) for p, t in phrases if p in args.only]
    wavs = synthesize(phrases, args.voice, args.rate)

    results, problems = [], []
    t0 = time.time()
    for pid, text in phrases:
        r = process(pid, text, wavs[pid])
        results.append(r)
        write_wav(os.path.join(OUT, 'src8k', pid + '.wav'), r['src'])
        write_wav(os.path.join(OUT, 'decoded', pid + '.wav'), r['out'])
        if not r['roundtrip']:
            problems.append(f'{pid}: bitstream round trip failed')
        if not r['parity']:
            problems.append(f'{pid}: decode() differs from the encoder render')
        if r['out_rms'] < 0.01:
            problems.append(f"{pid}: decoded output is (nearly) silent, rms {r['out_rms']:.4f}")
    mean_r = float(np.mean([r['env_r'] for r in results]))
    if mean_r < MIN_ENV_R:
        problems.append(f'mean envelope correlation {mean_r:.3f} < {MIN_ENV_R}')
    total = sum(r['nbytes'] for r in results)
    if not args.only and total >= BYTE_BUDGET:
        problems.append(f'total {total} bytes is over the {BYTE_BUDGET}-byte budget')

    report = write_report(results, args.voice, args.rate, bool(args.only), problems)
    print(report)
    print(f'encoded {len(results)} phrase(s) in {time.time() - t0:.1f} s')
    if problems:
        print('NOT writing snd-vocab.h: fix the problems above.')
        return 1
    if args.only:
        print('subset run: snd-vocab.h left untouched.')
    else:
        write_header(results, args.voice, args.rate)
        print(f'wrote {HEADER}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
