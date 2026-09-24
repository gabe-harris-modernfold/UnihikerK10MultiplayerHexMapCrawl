"""Render the K10 sound engine to WAV on the desktop, from the real firmware C++.

    python scripts/sndsim/sndsim.py                  # every scene
    python scripts/sndsim/sndsim.py carnival hunted  # just these
    python scripts/sndsim/sndsim.py --list           # what scenes exist
    python scripts/sndsim/sndsim.py --seed 7 day     # a different roll of the dice

Compiles scripts/sndsim/sndsim.cpp -- which includes snd-engine.hpp (and so
snd-core, snd-lpc, snd-music, snd-sfx and snd-vocab.h) unchanged under
SND_NATIVE -- with MSVC, runs each scene, and writes to scripts/sndsim/out/:

    <scene>.wav       what the engine hands the I2S amp (16 kHz, 16-bit)
    <scene>_k10.wav   the same through a rough model of the K10's 2 W speaker
                      (resonant high-pass ~350 Hz, a presence bump, top roll-off),
                      for auditioning on headphones
    <scene>.png       spectrogram + level strip, for eyeballing
    summary.json      per scene: render speed, voices, peak, clipping, style

Nothing is ported: what renders here is what would be flashed. It cannot tell
you the speed on the board -- that is the `SND:` serial line.
"""
import glob, json, os, subprocess, sys, tempfile, wave
import numpy as np
from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
OUT = os.path.join(HERE, "out")
EXE = os.path.join(OUT, "sndsim.exe")
SR = 16000
DEPS = ["snd-engine.hpp", "snd-core.hpp", "snd-lpc.hpp", "snd-music.hpp", "snd-sfx.hpp", "snd-vocab.h"]


def find_vcvars():
    if os.environ.get("VCVARS64") and os.path.isfile(os.environ["VCVARS64"]):
        return os.environ["VCVARS64"]
    for base in ("C:/Program Files/Microsoft Visual Studio", "C:/Program Files (x86)/Microsoft Visual Studio"):
        hits = sorted(glob.glob(base + "/*/*/VC/Auxiliary/Build/vcvars64.bat"), reverse=True)
        if hits:
            return hits[0]
    sys.exit("vcvars64.bat not found -- install VS Build Tools (C++) or set VCVARS64")


def build(force=False):
    src = os.path.join(HERE, "sndsim.cpp")
    deps = [src] + [os.path.join(ROOT, d) for d in DEPS if os.path.isfile(os.path.join(ROOT, d))]
    if not force and os.path.isfile(EXE) and all(os.path.getmtime(EXE) > os.path.getmtime(d) for d in deps):
        return
    os.makedirs(OUT, exist_ok=True)
    bat = os.path.join(tempfile.gettempdir(), "sndsim_build.bat")
    with open(bat, "w") as f:
        f.write('@echo off\r\ncall "%s" >nul\r\ncd /d "%s"\r\n' % (find_vcvars(), OUT))
        # 4244/4305/4267/4838: written for a 32-bit target; narrows on purpose.
        f.write('cl /nologo /O2 /fp:fast /std:c++14 /EHsc /W3 /wd4244 /wd4305 /wd4267 /wd4838 /wd4996 '
                '"%s" /Fe:"%s"\r\n' % (src, EXE))
    r = subprocess.run(["cmd", "/c", bat], capture_output=True, text=True)
    if r.returncode or not os.path.isfile(EXE):
        sys.exit("build failed:\n" + r.stdout + r.stderr)


def read_wav(path):
    with wave.open(path, "rb") as w:
        return np.frombuffer(w.readframes(w.getnframes()), dtype=np.int16).astype(np.float32) / 32768.0


def write_wav(path, x, sr):
    y = np.clip(x * 32767.0, -32768, 32767).astype(np.int16)
    with wave.open(path, "wb") as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(sr); w.writeframes(y.tobytes())


def speaker_model(x):
    """A rough 2 W micro-speaker in a small plastic case. Not a measurement.
    The first model (2nd-order at 350 Hz, +4 dB at 1.4 kHz) flattered the mix
    badly: on the real K10 the music was "barely perceivable" while the voice
    was too loud. This one loses the low-mids the way the board does and
    honks at 2 kHz, so a headphone listen errs on the side of the board."""
    from scipy import signal
    y = np.asarray(x, dtype=np.float64)
    fs = SR
    sos = signal.butter(4, 420, "highpass", fs=fs, output="sos")         # below its resonance the cone just rattles
    y = signal.sosfilt(sos, y)
    b, a = _peak(2200, 5.0, 1.0, fs)                                     # small cone + box: a 2 kHz honk
    y = signal.lfilter(b, a, y)
    sos = signal.butter(2, 6500, "lowpass", fs=fs, output="sos")
    y = signal.sosfilt(sos, y)
    y = np.tanh(y * 1.2) / np.tanh(1.2)                                  # a little cone breakup
    return y, fs


def _peak(f0, gain_db, q, fs):
    a = 10 ** (gain_db / 40.0); w0 = 2 * np.pi * f0 / fs; al = np.sin(w0) / (2 * q)
    b = np.array([1 + al * a, -2 * np.cos(w0), 1 - al * a]); aa = np.array([1 + al / a, -2 * np.cos(w0), 1 - al / a])
    return b / aa[0], aa / aa[0]


def spectrogram_png(x, path, title):
    n, hop = 512, 256
    if len(x) < n:
        return
    frames = np.lib.stride_tricks.sliding_window_view(x, n)[::hop] * np.hanning(n)
    spec = 20 * np.log10(np.abs(np.fft.rfft(frames, axis=1)) + 1e-6)
    spec = np.clip((spec + 90) / 90, 0, 1).T[::-1]                      # low freqs at the bottom
    h, w = spec.shape
    img = (np.stack([spec ** 0.8 * 255, spec ** 1.6 * 200, spec ** 3 * 120], -1)).astype(np.uint8)
    im = Image.fromarray(img, "RGB").resize((min(1800, w), 257))
    # level strip
    rms = np.sqrt(np.convolve(x ** 2, np.ones(800) / 800, mode="same"))[::hop][:w]
    strip = Image.new("RGB", (im.width, 60), (12, 12, 12)); d = ImageDraw.Draw(strip)
    xs = np.linspace(0, len(rms) - 1, im.width).astype(int)
    for i, k in enumerate(xs):
        v = 20 * np.log10(rms[k] + 1e-6); hgt = int(np.clip((v + 60) / 60, 0, 1) * 58)
        d.line([(i, 59), (i, 59 - hgt)], fill=(220, 150, 40))
    out = Image.new("RGB", (im.width, im.height + 80), (0, 0, 0))
    out.paste(im, (0, 20)); out.paste(strip, (0, im.height + 20))
    ImageDraw.Draw(out).text((4, 4), title, fill=(230, 180, 60))
    out.save(path)


PART_COL = {0: (120, 80, 40), 1: (70, 130, 170), 2: (170, 90, 50), 3: (240, 200, 90), 4: (150, 200, 120),
            5: (200, 70, 70), 6: (90, 90, 90), 7: (255, 120, 220)}
PART_NAME = ["drone", "pad", "bass", "lead", "counter", "perc", "tex", "motif"]


def pianoroll_png(csv_path, path, title, px_per_s=24, lo=36, hi=100):
    """Notes as bars: time across, pitch up. One colour per part; effects grey."""
    rows = [l.strip().split(",") for l in open(csv_path)][1:]
    if not rows:
        return
    ev = [(float(r[0]), int(r[1]), int(r[2]), float(r[4]), float(r[5]), float(r[6])) for r in rows]
    end = max(t for t, *_ in ev) + 3
    W = int(min(4000, end * px_per_s)); H = (hi - lo) * 4 + 40
    scale = W / end
    im = Image.new("RGB", (W, H), (10, 10, 12)); d = ImageDraw.Draw(im)
    for m in range(lo, hi):
        if m % 12 == 2:                               # the D lines: the home key
            y = H - 20 - (m - lo) * 4; d.line([(0, y), (W, y)], fill=(40, 34, 24))
    for s in range(0, int(end), 10):
        d.line([(s * scale, 18), (s * scale, H - 20)], fill=(28, 28, 32))
        d.text((s * scale + 2, H - 16), "%ds" % s, fill=(120, 110, 90))
    for t, bus, part, midi, vel, dur in ev:
        if midi < lo or midi >= hi:
            continue
        x0 = t * scale; x1 = x0 + max(2, (dur if dur > 0 else 2.0) * scale)
        y = H - 20 - (midi - lo) * 4
        col = (110, 110, 110) if bus == 1 else PART_COL.get(part, (200, 200, 200))
        k = 0.45 + 0.55 * min(1.0, vel)
        d.rectangle([x0, y - 1, x1, y + 1], fill=tuple(int(c * k) for c in col))
    x = 4
    d.text((x, 2), title, fill=(230, 180, 60)); x += 8 * len(title) + 20
    for i, n in enumerate(PART_NAME):
        d.rectangle([x, 4, x + 10, 12], fill=PART_COL[i]); d.text((x + 14, 2), n, fill=(200, 200, 200)); x += 14 + 8 * len(n) + 10
    im.save(path)


def scenes():
    r = subprocess.run([EXE, "--list"], capture_output=True, text=True)
    return [l.split()[0] for l in r.stdout.splitlines() if l.strip()]


def main(argv):
    force = "--rebuild" in argv
    seed = "12345"
    if "--seed" in argv:
        i = argv.index("--seed"); seed = argv[i + 1]; del argv[i:i + 2]
    build(force)
    if "--list" in argv:
        print(subprocess.run([EXE, "--list"], capture_output=True, text=True).stdout); return
    names = [a for a in argv if not a.startswith("-")] or scenes()
    summary = {}
    sp = os.path.join(OUT, "summary.json")
    if os.path.isfile(sp):
        try: summary = json.load(open(sp))
        except Exception: summary = {}
    for n in names:
        r = subprocess.run([EXE, OUT, n, seed], capture_output=True, text=True)
        if r.returncode:
            sys.exit(r.stdout + r.stderr)
        info = json.loads(r.stdout.strip().splitlines()[-1])
        x = read_wav(os.path.join(OUT, n + ".wav"))
        y, fs = speaker_model(x)
        write_wav(os.path.join(OUT, n + "_k10.wav"), y * 0.9, fs)
        spectrogram_png(x, os.path.join(OUT, n + ".png"), "%s  (%.0fs)" % (n, len(x) / SR))
        csv = os.path.join(OUT, n + "_notes.csv")
        if os.path.isfile(csv):
            pianoroll_png(csv, os.path.join(OUT, n + "_roll.png"), n)
        rms = float(np.sqrt(np.mean(x ** 2)))
        info["rms_db"] = round(20 * np.log10(rms + 1e-9), 1)
        info["log"] = [l for l in r.stderr.splitlines()][-60:]
        summary[n] = info
        print("%-15s %5.1fs  x%.3f rt  voices avg %.1f max %2d  peak %.2f  rms %5.1f dB  clip %d  -> %s"
              % (n, info["seconds"], info["render_x_realtime"], info["avg_voices"], info["max_voices"],
                 info["peak"], info["rms_db"], info["clipped"], info["final_style"]))
    json.dump(summary, open(sp, "w"), indent=1)


if __name__ == "__main__":
    main(sys.argv[1:])
