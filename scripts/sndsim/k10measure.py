"""Measure the K10's speaker with a microphone: frequency response, level
linearity, and the level where it starts to distort.

    python scripts/sndsim/k10measure.py 192.168.4.36          # record + analyse
    python scripts/sndsim/k10measure.py --analyse out/k10rec.wav
    python scripts/sndsim/k10measure.py --list-devices
    python scripts/sndsim/k10measure.py 192.168.4.36 --device 9

How it works: GET /sndtest makes the board play a fixed 25 s sequence of exact
digital levels (snd-engine.hpp sndCalBlock -- it bypasses the mix, the limiter
and the volume setting). This records the laptop mic meanwhile, aligns the
recording against the bit-exact reference (scripts/sndsim/out/cal.wav, from
`python scripts/sndsim/sndsim.py cal`), and reports:

  * frequency response (from the 100 Hz..7.5 kHz sweep, cross-checked against
    pink noise), relative to 1 kHz -- speaker + room + laptop mic together;
  * level linearity: each ladder step should be +6.0 dB; a shortfall is the amp
    (or the mic's AGC -- the sync tone at start and end catches that);
  * THD at 1 kHz and 300 Hz per step, which is where crackle begins.

Writes scripts/sndsim/out/k10rec.wav, k10measure.png and k10_response.json
(the curve sndsim's speaker model is fitted from). Put the K10 20-30 cm in
front of the laptop mic, speaker facing it, in a quiet room.
"""
import json, os, sys, time, threading, urllib.request, wave
import numpy as np
from scipy import signal
from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "out")
REF = os.path.join(OUT, "cal.wav")
FS_REC = 48000
SR = 16000


def rd(path):
    with wave.open(path) as w:
        n, ch, fs = w.getnframes(), w.getnchannels(), w.getframerate()
        x = np.frombuffer(w.readframes(n), np.int16).astype(np.float64) / 32768
    if ch > 1:
        x = x.reshape(-1, ch).mean(axis=1)
    return x, fs


def wr(path, x, fs):
    with wave.open(path, "wb") as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(fs)
        w.writeframes(np.clip(x * 32767, -32768, 32767).astype(np.int16).tobytes())


def record(ip, device, seconds=31.0, fmt=None):
    import sounddevice as sd
    kw = {"samplerate": FS_REC, "channels": 1, "dtype": "float32"}
    if device is not None:
        kw["device"] = device
    print("recording %.0f s from %s ..." % (seconds, sd.query_devices(device if device is not None else sd.default.device[0])["name"]))
    buf = sd.rec(int(seconds * FS_REC), **kw)
    time.sleep(1.2)
    try:
        url = "http://%s/sndtest" % ip + ("?fmt=%d" % fmt if fmt is not None else "")
        r = urllib.request.urlopen(url, timeout=4).read().decode(errors="replace").strip()
        print("board:", r)
    except Exception as e:
        print("could not reach http://%s/sndtest: %s" % (ip, e))
    sd.wait()
    x = buf[:, 0].astype(np.float64)
    path = os.path.join(OUT, "k10rec%s.wav" % ("" if fmt is None else "_fmt%d" % fmt))
    wr(path, x, FS_REC)
    print("saved", path, "peak %.1f dBFS" % (20 * np.log10(np.abs(x).max() + 1e-9)))
    return path


def band_env(x, fs, lo, hi, rate=200):
    sos = signal.butter(4, [lo, hi], "bandpass", fs=fs, output="sos")
    e = np.abs(signal.sosfilt(sos, x))
    hop = fs // rate
    n = len(e) // hop
    return e[: n * hop].reshape(n, hop).mean(axis=1)


def align(rec16, ref16):
    """Seconds into the recording where the reference's t=0 lands. Correlates
    broadband log-energy envelopes (10 ms): the sequence's silences and steps
    are distinctive, and unlike a 1 kHz band this survives a distorted board
    (the first measurement locked onto a harmonic with the band method)."""
    def env(v):
        h = SR // 100; n = len(v) // h
        return np.log10(np.sqrt(np.mean(v[: n * h].reshape(n, h) ** 2, axis=1)) + 1e-5)
    er = env(rec16); ef = env(ref16)
    er = er - er.mean(); ef = ef - ef.mean()
    c = signal.correlate(er, ef, mode="full")
    lags = np.arange(-len(ef) + 1, len(er))
    ok = (lags >= 0) & (lags <= max(0, len(er) - len(ef)))
    lag = lags[ok][np.argmax(c[ok])] if ok.any() else 0
    return lag / 100.0


def db(v):
    return 20 * np.log10(v + 1e-12)


def tone_levels(x, fs, f0, nh=6):
    """Fundamental and harmonic amplitudes (dB) of a steady tone segment."""
    w = np.hanning(len(x))
    X = np.abs(np.fft.rfft(x * w)) / (w.sum() / 2)
    f = np.fft.rfftfreq(len(x), 1 / fs)
    def pk(fc):
        m = (f > fc - 25) & (f < fc + 25)
        return X[m].max() if m.any() else 0.0
    fund = pk(f0)
    harm = [pk(f0 * n) for n in range(2, nh + 1) if f0 * n < min(fs / 2 - 100, 12000)]
    band = (f > 120) & (f < 8000)
    notf = band & ~((f > f0 - 40) & (f < f0 + 40))
    rest = np.sqrt(np.sum(X[notf] ** 2) / 1.5)            # Hann power correction, roughly
    return fund, harm, rest


def analyse(path):
    rec, fs = rd(path)
    if fs != FS_REC:
        rec = signal.resample_poly(rec, FS_REC, fs); fs = FS_REC
    if not os.path.isfile(REF):
        sys.exit("missing %s -- run: python scripts/sndsim/sndsim.py cal" % REF)
    ref, _ = rd(REF)
    rec16 = signal.resample_poly(rec, 1, 3)
    off = align(rec16, ref)
    print("aligned: reference t=0 at %.3f s into the recording" % off)
    S = lambda a, b, x=rec, r=fs: x[int((off + a) * r): int((off + b) * r)]
    report = {"offset_s": off}

    noise = np.sqrt(np.mean(S(0.2, 0.9) ** 2))
    s1 = tone_levels(S(1.1, 1.45), fs, 1000)[0]
    s2 = tone_levels(S(24.55, 24.9), fs, 1000)[0]
    report["noise_dbfs"] = db(noise); report["sync_start_db"] = db(s1); report["sync_end_db"] = db(s2)
    print("room/mic noise floor %.1f dBFS;  sync tone (-30 dBFS on the board): start %.1f, end %.1f dB%s"
          % (db(noise), db(s1), db(s2), "  <-- mic gain moved (AGC?)" if abs(db(s1) - db(s2)) > 1.5 else ""))

    # sweep: 25 ms windows, level vs time -> level vs frequency
    t = np.arange(2.05, 9.95, 0.025)
    fr = 100.0 * (75.0 ** ((t - 2.0) / 8.0))
    lv = np.array([db(np.sqrt(np.mean(S(a, a + 0.025) ** 2))) for a in t])
    lv -= np.interp(np.log(1000), np.log(fr), lv)
    # smooth over 1/6 octave in log-f
    lf = np.log2(fr); sm = np.array([lv[np.abs(lf - l) < 1 / 12].mean() for l in lf])
    # pink: 1/3-octave bands, recording minus reference
    cents = [125, 160, 200, 250, 315, 400, 500, 630, 800, 1000, 1250, 1600, 2000, 2500, 3150, 4000, 5000, 6300]
    pr = S(22.1, 24.4); pf = ref[int(22.1 * SR): int(24.4 * SR)]
    Pr = np.abs(np.fft.rfft(pr * np.hanning(len(pr)))) ** 2; fr_ = np.fft.rfftfreq(len(pr), 1 / fs)
    Pf = np.abs(np.fft.rfft(pf * np.hanning(len(pf)))) ** 2; ff_ = np.fft.rfftfreq(len(pf), 1 / SR)
    pink = []
    for c in cents:
        lo, hi = c / 2 ** (1 / 6), c * 2 ** (1 / 6)
        a = Pr[(fr_ >= lo) & (fr_ < hi)].sum() / max(1, ((fr_ >= lo) & (fr_ < hi)).sum())
        b = Pf[(ff_ >= lo) & (ff_ < hi)].sum() / max(1, ((ff_ >= lo) & (ff_ < hi)).sum())
        pink.append(10 * np.log10((a + 1e-20) / (b + 1e-20)))
    pink = np.array(pink); pink -= pink[cents.index(1000)]
    print("\nfrequency response re 1 kHz (speaker + room + laptop mic):")
    print("   Hz   sweep   pink")
    for c, p in zip(cents, pink):
        print("%6d  %6.1f  %6.1f" % (c, np.interp(np.log(c), np.log(fr), sm), p))
    report["response"] = [[float(f), float(v)] for f, v in zip(fr[::4], sm[::4])]
    report["pink_bands"] = [[c, float(p)] for c, p in zip(cents, pink)]

    # ladders
    def ladder(t0, f0, levels):
        rows = []; prev = None
        for k, L in enumerate(levels):
            seg = S(t0 + 0.8 * k + 0.1, t0 + 0.8 * k + 0.5)
            fund, harm, rest = tone_levels(seg, fs, f0)
            thd = np.sqrt(sum(h * h for h in harm)) / max(fund, 1e-12)
            thdn = rest / max(fund, 1e-12)
            step = db(fund) - prev if prev is not None else float("nan")
            prev = db(fund)
            rows.append({"dbfs": L, "fund_db": db(fund), "step_db": step, "thd_pct": 100 * thd, "thdn_pct": 100 * thdn,
                         "h": [db(h) - db(fund) for h in harm]})
        return rows
    l1 = ladder(10.5, 1000, [-46, -40, -34, -28, -22, -16, -10])
    l3 = ladder(16.3, 300, [-40, -34, -28, -22, -16, -10])
    # one number per format: how much of each 1 kHz step's energy is the 1 kHz itself
    pur = []
    for k in range(7):
        seg = S(10.5 + 0.8 * k + 0.1, 10.5 + 0.8 * k + 0.5)
        X = np.abs(np.fft.rfft(seg * np.hanning(len(seg)))) ** 2; f = np.fft.rfftfreq(len(seg), 1 / fs)
        band = (f > 150) & (f < 8000); fund = band & (f > 960) & (f < 1040)
        pur.append(100 * X[fund].sum() / max(X[band].sum(), 1e-20))
    report["purity_1k_pct"] = pur
    for name, rows in (("1 kHz", l1), ("300 Hz", l3)):
        print("\n%s ladder: level on the board -> what the mic got" % name)
        print("  dBFS   fund dB  step(+6)  THD%%   THD+N%%   2nd   3rd   4th (dB re fund)")
        for r in rows:
            h = r["h"] + [float("nan")] * 3
            print("  %4d   %7.1f   %6.1f  %6.2f  %7.2f  %5.1f %5.1f %5.1f" %
                  (r["dbfs"], r["fund_db"], r["step_db"], r["thd_pct"], r["thdn_pct"], h[0], h[1], h[2]))
    report["ladder_1k"] = l1; report["ladder_300"] = l3
    print()
    print("TONE PURITY: share of each 1 kHz step's energy that is 1 kHz (a healthy chain is >90%)")
    print("  " + "  ".join("%d:%3.0f%%" % (L, p) for L, p in zip([-46, -40, -34, -28, -22, -16, -10], pur)))
    tag = os.path.splitext(os.path.basename(path))[0].replace("k10rec", "")
    json.dump(report, open(os.path.join(OUT, "k10_response%s.json" % tag), "w"), indent=1, default=float)

    # picture: response curve + THD vs level
    W, H = 900, 420
    im = Image.new("RGB", (W, H), (11, 8, 6)); d = ImageDraw.Draw(im)
    def px(f, v):
        x = 60 + (np.log10(f) - 2) / (np.log10(8000) - 2) * (W - 100)
        y = 30 + (10 - v) / 50 * 250
        return x, y
    for v in range(10, -41, -10):
        d.line([px(100, v), px(8000, v)], fill=(40, 30, 16)); d.text((8, px(100, v)[1] - 6), "%+d dB" % v, fill=(143, 97, 36))
    for f in (100, 200, 500, 1000, 2000, 5000):
        d.line([px(f, 10), px(f, -40)], fill=(40, 30, 16)); d.text((px(f, -40)[0] - 12, 285), "%g" % f, fill=(143, 97, 36))
    pts = [px(f, max(-40, min(10, v))) for f, v in zip(fr, sm)]
    d.line(pts, fill=(255, 213, 138), width=2)
    for c, p in zip(cents, pink):
        x, y = px(c, max(-40, min(10, p))); d.ellipse([x - 3, y - 3, x + 3, y + 3], outline=(242, 163, 58))
    d.text((60, 8), "K10 speaker + room + mic, re 1 kHz  (line: sweep, dots: pink noise)", fill=(255, 241, 214))
    y0 = 320
    d.text((60, y0 - 16), "THD by level:  1 kHz (bright)   300 Hz (dim)", fill=(255, 241, 214))
    for rows, col in ((l1, (255, 213, 138)), (l3, (143, 97, 36))):
        for r in rows:
            x = 60 + (r["dbfs"] + 50) / 45 * (W - 100); hgt = min(90, r["thd_pct"] * 3)
            d.rectangle([x - 6 if col[0] == 255 else x + 1, y0 + 90 - hgt, x - 1 if col[0] == 255 else x + 6, y0 + 90], fill=col)
            d.text((x - 10, y0 + 92), "%d" % r["dbfs"], fill=(143, 97, 36))
    im.save(os.path.join(OUT, "k10measure%s.png" % tag))
    print("\nwrote out/k10measure%s.png and out/k10_response%s.json" % (tag, tag))


def main(argv):
    if "--list-devices" in argv:
        import sounddevice as sd; print(sd.query_devices()); return
    dev = None; fmt = None
    if "--device" in argv:
        i = argv.index("--device"); dev = int(argv[i + 1]); del argv[i:i + 2]
    if "--fmt" in argv:
        i = argv.index("--fmt"); fmt = int(argv[i + 1]); del argv[i:i + 2]
    if "--analyse" in argv:
        i = argv.index("--analyse"); analyse(argv[i + 1]); return
    ips = [a for a in argv if not a.startswith("-")]
    if not ips:
        sys.exit(__doc__)
    os.makedirs(OUT, exist_ok=True)
    analyse(record(ips[0], dev, fmt=fmt))


if __name__ == "__main__":
    main(sys.argv[1:])
