"""The K10 measuring itself: play a known sequence, record it through the
board's own microphones, download the capture, score it.

    python scripts/sndsim/k10loop.py 192.168.4.36                 # tone sequence (seq 0)
    python scripts/sndsim/k10loop.py 192.168.4.36 --seq 1         # real content through the mix
    python scripts/sndsim/k10loop.py 192.168.4.36 --seq 1 --set lp=3800 dac8=0 vol=3
    python scripts/sndsim/k10loop.py 192.168.4.36 --fmt 2         # I2S format for the test (duplex only)

Why the board's mics: a laptop "Microphone Array" runs Windows voice
processing (noise suppression, echo cancellation) that deletes steady tones,
so it cannot measure a speaker. The K10's MEMS mics are raw I2S, sit next to
its speaker, and record on the same pass the sequence starts (GET
/sndtest?rec=1, ui-audio.hpp), so every run is the same geometry and timing.

--set k=v applies GET /snddbg knobs first (lp, noise, wind, dac8, vol).
Writes scripts/sndsim/out/loop_seq<N>[_tag].wav and prints the scores:
  seq 0: per 1 kHz / 300 Hz ladder step, tone purity (share of the step's
         energy that is the tone; a clean chain is >90%) and THD.
  seq 1: per segment (narrator, whisper, calliope, music box, bell,
         harmonium), "fizz" = energy above 4 kHz relative to 250 Hz-4 kHz,
         and for held notes the harmonic-to-noise ratio; plus the gap floor.
"""
import json, os, sys, time, urllib.request, wave
import numpy as np
from scipy import signal

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "out")
SR = 16000


def get(ip, path, timeout=5):
    return urllib.request.urlopen("http://%s%s" % (ip, path), timeout=timeout).read()


def rd_bytes(b):
    import io
    with wave.open(io.BytesIO(b)) as w:
        ch = w.getnchannels(); x = np.frombuffer(w.readframes(w.getnframes()), np.int16).astype(np.float64) / 32768
    return x.reshape(-1, ch) if ch > 1 else x[:, None]


def db(v):
    return 20 * np.log10(v + 1e-12)


def pick_channel(x):
    """The channel carrying the most signal (the other mic may be idle)."""
    r = np.sqrt(np.mean(x ** 2, axis=0))
    return x[:, int(np.argmax(r))], r


def spectrum(seg):
    X = np.abs(np.fft.rfft(seg * np.hanning(len(seg)))) ** 2
    return X, np.fft.rfftfreq(len(seg), 1 / SR)


def purity(seg, f0):
    X, f = spectrum(seg)
    band = (f > 150) & (f < 7800); fund = band & (np.abs(f - f0) < max(25, f0 * 0.03))
    return 100 * X[fund].sum() / max(X[band].sum(), 1e-20)


def thd(seg, f0):
    X, f = spectrum(seg)
    pk = lambda fc: X[(np.abs(f - fc) < 25)].max() if fc < 7800 else 0.0
    h = [pk(f0 * n) for n in range(2, 8)]
    return 100 * np.sqrt(sum(h)) / max(np.sqrt(pk(f0)), 1e-12)


def fizz(seg):
    X, f = spectrum(seg)
    lo = X[(f > 250) & (f < 4000)].sum(); hi = X[(f >= 4000) & (f < 7800)].sum()
    return 10 * np.log10((hi + 1e-20) / (lo + 1e-20))


def hnr(seg, f0s):
    """Energy within +-3% of the notes' harmonics vs everything else (250 Hz-7.8 kHz)."""
    X, f = spectrum(seg)
    band = (f > 250) & (f < 7800); harm = np.zeros_like(band)
    for f0 in f0s:
        for n in range(1, 40):
            if f0 * n > 7800: break
            harm |= np.abs(f - f0 * n) < max(12, f0 * n * 0.03)
    harm &= band
    return 10 * np.log10((X[harm].sum() + 1e-20) / (X[band & ~harm].sum() + 1e-20))


def align_ref(x, ref):
    def env(v):
        h = SR // 100; n = len(v) // h
        return np.log10(np.sqrt(np.mean(v[: n * h].reshape(n, h) ** 2, axis=1)) + 1e-5)
    er, ef = env(x), env(ref); er -= er.mean(); ef -= ef.mean()
    c = signal.correlate(er, ef, mode="full"); lags = np.arange(-len(ef) + 1, len(er))
    ok = (lags >= 0) & (lags <= 300)
    return lags[ok][np.argmax(c[ok])] / 100.0 if ok.any() else 0.0


def run(ip, seq, fmt, sets, tag):
    if sets:
        q = "&".join("%s=%s" % kv for kv in sets.items())
        print("snddbg:", get(ip, "/snddbg?" + q).decode().strip())
        time.sleep(0.3)
    q = "/sndtest?rec=1&seq=%d" % seq + ("&fmt=%d" % fmt if fmt is not None else "")
    print("board:", get(ip, q).decode().strip())
    time.sleep(22.5 if seq == 1 else 27.0)
    for _ in range(20):
        try:
            b = get(ip, "/sndrec.wav", timeout=20); break
        except urllib.error.HTTPError as e:
            time.sleep(1.0)
    else:
        sys.exit("capture never became ready")
    path = os.path.join(OUT, "loop_seq%d%s.wav" % (seq, ("_" + tag) if tag else ""))
    open(path, "wb").write(b)
    x2 = rd_bytes(b); x, r = pick_channel(x2)
    print("captured %.1f s, channel rms %s dBFS, peak %.1f dBFS -> %s" %
          (len(x) / SR, " / ".join("%.1f" % db(v) for v in r), db(np.abs(x).max()), path))
    res = {"seq": seq, "sets": sets}
    if seq == 0:
        ref_path = os.path.join(OUT, "cal.wav")
        with wave.open(ref_path) as w:
            ref = np.frombuffer(w.readframes(w.getnframes()), np.int16).astype(np.float64) / 32768
        off = align_ref(x, ref)
        S = lambda a, b: x[int((off + a) * SR): int((off + b) * SR)]
        print("aligned %.2f s; floor %.1f dBFS" % (off, db(np.sqrt(np.mean(S(0.2, 0.9) ** 2)))))
        p1 = [purity(S(10.5 + 0.8 * k + 0.1, 10.5 + 0.8 * k + 0.5), 1000) for k in range(7)]
        t1 = [thd(S(10.5 + 0.8 * k + 0.1, 10.5 + 0.8 * k + 0.5), 1000) for k in range(7)]
        l1 = [db(np.sqrt(np.mean(S(10.5 + 0.8 * k + 0.1, 10.5 + 0.8 * k + 0.5) ** 2))) for k in range(7)]
        p3 = [purity(S(16.3 + 0.8 * k + 0.1, 16.3 + 0.8 * k + 0.5), 300) for k in range(6)]
        print("1 kHz  dBFS : " + "  ".join("%6d" % L for L in (-46, -40, -34, -28, -22, -16, -10)))
        print("  level dB  : " + "  ".join("%6.1f" % v for v in l1))
        print("  purity %% : " + "  ".join("%6.0f" % v for v in p1))
        print("  THD %%    : " + "  ".join("%6.1f" % v for v in t1))
        print("300 Hz purity: " + "  ".join("%5.0f" % v for v in p3))
        res.update(purity1k=p1, thd1k=t1, level1k=l1, purity300=p3)
    else:
        # content: find the start from the narrator onset (first sustained energy)
        e = np.sqrt(np.convolve(x ** 2, np.ones(160) / 160, mode="same"))
        floor = np.percentile(e, 10); on = np.argmax(e > floor * 6)
        t0 = on / SR - 0.5 - 0.05
        S = lambda a, b: x[int(max(0, (t0 + a)) * SR): int((t0 + b) * SR)]
        segs = [("narrator", 0.6, 3.4, None), ("whisper", 4.1, 7.9, None), ("calliope call", 8.5, 9.6, None),
                ("calliope A4", 10.2, 11.4, [440.0]), ("music box D6", 12.05, 12.9, [1174.7]),
                ("bell D4", 14.05, 15.5, None), ("harmonium Dm", 17.3, 18.9, [293.7, 349.2, 440.0])]
        print("start %.2f s; gap floor %.1f dBFS (16.0-16.8 s)" % (t0, db(np.sqrt(np.mean(S(16.0, 16.8) ** 2)))))
        print("  %-14s %8s %8s %8s" % ("segment", "level", "fizz dB", "HNR dB"))
        rows = []
        for name, a, b, f0s in segs:
            s = S(a, b)
            lv = db(np.sqrt(np.mean(s ** 2))); fz = fizz(s); hn = hnr(s, f0s) if f0s else float("nan")
            rows.append((name, lv, fz, hn))
            print("  %-14s %8.1f %8.1f %8.1f" % (name, lv, fz, hn))
        res["segments"] = rows
    json.dump(res, open(path[:-4] + ".json", "w"), indent=1, default=float)
    return res


def main(argv):
    seq, fmt, tag, sets = 0, None, "", {}
    if "--seq" in argv: i = argv.index("--seq"); seq = int(argv[i + 1]); del argv[i:i + 2]
    if "--fmt" in argv: i = argv.index("--fmt"); fmt = int(argv[i + 1]); del argv[i:i + 2]
    if "--tag" in argv: i = argv.index("--tag"); tag = argv[i + 1]; del argv[i:i + 2]
    if "--set" in argv:
        i = argv.index("--set"); j = i + 1
        while j < len(argv) and "=" in argv[j]:
            k, v = argv[j].split("=", 1); sets[k] = v; j += 1
        del argv[i:j]
    ips = [a for a in argv if not a.startswith("-")]
    if not ips: sys.exit(__doc__)
    os.makedirs(OUT, exist_ok=True)
    run(ips[0], seq, fmt, sets, tag)


if __name__ == "__main__":
    main(sys.argv[1:])
