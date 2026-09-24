"""Render the LCD FX layer to GIF on the desktop, from the real firmware C++.

    python scripts/fxsim/fxsim.py                 # every scene
    python scripts/fxsim/fxsim.py quake eye       # just these
    python scripts/fxsim/fxsim.py --list          # what scenes exist

Compiles scripts/fxsim/fxsim.cpp -- which includes ui-fx.hpp unchanged under
FX_NATIVE -- with MSVC, plays each scene's cues over a real screen, and writes
scripts/fxsim/out/<scene>.gif (2x, real frame timing) and <scene>_sheet.png
(ten frames side by side, for a quick look without a GIF viewer). Nothing is
ported: what renders here is what would be flashed.

Needs MSVC (any Visual Studio / Build Tools with vcvars64.bat; set VCVARS64
to override the search), LovyanGFX for its font headers (first argument
`--lgfx DIR`, or LOVYANGFX_DIR, else the Arduino sketchbook), Pillow and
numpy. The base screens are fixtures/*.png, 240x320, drawn by the offline PIL
previewers that port ui-screens.hpp; any 240x320 PNG will do.
"""
import glob, os, subprocess, sys, tempfile
import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
OUT = os.path.join(HERE, "out")
FRAMES = os.path.join(OUT, "frames")
EXE = os.path.join(OUT, "fxsim.exe")
W, H = 240, 320
SCENES = ["quake", "strike", "dawn", "eye", "hunt", "switch", "madness", "fire", "threat",
          "chem", "storm", "join", "thrown", "act", "odds", "fog", "reprint", "below", "crafted", "crawl"]


def find_vcvars():
    if os.environ.get("VCVARS64") and os.path.isfile(os.environ["VCVARS64"]):
        return os.environ["VCVARS64"]
    for base in ("C:/Program Files/Microsoft Visual Studio", "C:/Program Files (x86)/Microsoft Visual Studio"):
        hits = sorted(glob.glob(base + "/*/*/VC/Auxiliary/Build/vcvars64.bat"), reverse=True)
        if hits:
            return hits[0]
    sys.exit("vcvars64.bat not found -- install VS Build Tools (C++) or set VCVARS64")


def find_fonts(explicit):
    cands = [explicit, os.environ.get("LOVYANGFX_DIR", ""),
             os.path.expanduser("~/Documents/Arduino/libraries/LovyanGFX")]
    cands += glob.glob(os.path.expanduser("~/OneDrive*/Documents/Arduino/libraries/LovyanGFX"))
    for c in cands:
        if c and os.path.isdir(os.path.join(c, "src", "lgfx", "Fonts")):
            return os.path.join(c, "src", "lgfx", "Fonts")
    sys.exit("LovyanGFX not found -- pass --lgfx DIR or set LOVYANGFX_DIR")


def build(fonts):
    src = os.path.join(HERE, "fxsim.cpp")
    deps = [src, os.path.join(ROOT, "ui-fx.hpp")]
    if os.path.isfile(EXE) and all(os.path.getmtime(EXE) > os.path.getmtime(d) for d in deps):
        return
    bat = os.path.join(tempfile.gettempdir(), "fxsim_build.bat")
    with open(bat, "w") as f:
        f.write('@echo off\r\ncall "%s" >nul\r\ncd /d "%s"\r\n' % (find_vcvars(), OUT))
        # 4244/4305/4267/4838: the header is written for a 32-bit target and
        # narrows float->int and size_t->int on purpose.
        f.write('cl /nologo /O2 /std:c++14 /EHsc /W3 /wd4244 /wd4305 /wd4267 /wd4838 '
                '/I "%s" "%s" /Fe:"%s"\r\n' % (fonts, src, EXE))
    r = subprocess.run(["cmd", "/c", bat], capture_output=True, text=True)
    if r.returncode or not os.path.isfile(EXE):
        sys.exit("build failed:\n" + r.stdout + r.stderr)


def fixtures():
    for png in glob.glob(os.path.join(HERE, "fixtures", "*.png")):
        a = np.asarray(Image.open(png).convert("RGB").resize((W, H)), dtype=np.uint16)
        c = ((a[..., 0] >> 3) << 11) | ((a[..., 1] >> 2) << 5) | (a[..., 2] >> 3)
        c.astype(">u2").tofile(os.path.join(OUT, os.path.basename(png)[:-4] + ".raw"))


def frame_img(path):
    a = np.fromfile(path, dtype=">u2").reshape(H, W).astype(np.uint32)
    r, g, b = (a >> 11) & 31, (a >> 5) & 63, a & 31
    return Image.fromarray(np.dstack(((r << 3) | (r >> 2), (g << 2) | (g >> 4),
                                      (b << 3) | (b >> 2))).astype(np.uint8), "RGB")


def assemble(name):
    rows = [l.split() for l in open(os.path.join(FRAMES, name + ".txt"))]
    frames = [(int(r[0]), int(r[1]), int(r[2])) for r in rows]
    imgs = [frame_img(os.path.join(FRAMES, "%s_%04d.raw" % (name, i))) for i, _, _ in frames]
    big = [im.resize((W * 2, H * 2), Image.NEAREST) for im in imgs]
    big[0].save(os.path.join(OUT, name + ".gif"), save_all=True, append_images=big[1:],
                duration=[p for _, _, p in frames], loop=0, disposal=1)
    idx = np.linspace(0, len(imgs) - 1, min(10, len(imgs))).astype(int)
    sheet = Image.new("RGB", (len(idx) * (W + 4) - 4, H), (50, 50, 50))
    for k, i in enumerate(idx):
        sheet.paste(imgs[i], (k * (W + 4), 0))
    sheet.save(os.path.join(OUT, name + "_sheet.png"))
    for f in glob.glob(os.path.join(FRAMES, name + "_*.raw")):
        os.remove(f)
    return len(frames)


def main(argv):
    if "--list" in argv:
        print("\n".join(SCENES)); return
    lgfx = ""
    if "--lgfx" in argv:
        i = argv.index("--lgfx"); lgfx = argv[i + 1]; del argv[i:i + 2]
    names = [a for a in argv if not a.startswith("-")] or SCENES
    bad = [n for n in names if n not in SCENES]
    if bad:
        sys.exit("unknown scene(s): %s (see --list)" % " ".join(bad))
    os.makedirs(FRAMES, exist_ok=True)
    build(find_fonts(lgfx))
    fixtures()
    for n in names:
        r = subprocess.run([EXE, OUT, FRAMES, n], capture_output=True, text=True)
        if r.returncode:
            sys.exit(r.stdout + r.stderr)
        print("%-10s %3d frames -> %s" % (n, assemble(n), os.path.join(OUT, n + ".gif")))


if __name__ == "__main__":
    main(sys.argv[1:])
