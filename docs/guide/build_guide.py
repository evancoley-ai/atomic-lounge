# SPDX-License-Identifier: AGPL-3.0-only
# Vintage Chonk user guide: pictures + PDF (needs Pillow and PyMuPDF, so no -I: they live in the user site).
#   python docs/guide/build_guide.py            screens, PDF, page previews
#   python docs/guide/build_guide.py --no-shots  reuse docs/guide/img as it is
# 1. builds tools/guide_shots (the real panel UI on the desktop emulation, MSYS2 UCRT64 gcc) and
#    writes every FM-1 screen 3x (nearest neighbour) to docs/guide/img/fm1/
# 2. copies the tablet screenshots (build/android_ui_shots, from tests/test_panel.mjs)
# 3. prints guide.html to PDF with headless Chrome twice: the first pass finds the page of every
#    contents entry (the PDF's own link targets), the second prints the numbers
# 4. renders every PDF page to build/guide_preview/ (PyMuPDF) for review
import os
import re
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
CHROME = r"C:\Program Files\Google\Chrome\Application\chrome.exe"
GCC = r"C:\msys64\ucrt64\bin\gcc.exe"
PDF = os.path.join(HERE, "Vintage-Chonk-User-Guide.pdf")
SRC = os.path.join(HERE, "guide.html")
TMP = os.path.join(ROOT, "build", "guide")


def shots():
    from PIL import Image
    os.makedirs(TMP, exist_ok=True)
    exe = os.path.join(TMP, "guide_shots.exe")
    # every engine source (the engine grows: e.g. dsp/sequencer); chroma effects are #included by mfe_fx.c
    dsp = sorted(os.path.relpath(os.path.join(d, f), ROOT).replace("\\", "/")
                 for d, _, fs in os.walk(os.path.join(ROOT, "dsp")) for f in fs
                 if f.endswith(".c") and "chroma" not in d)
    src = ["tools/guide_shots/guide_shots.c", "firmware/ui/mfm1_ui.c", "firmware/ui/mfm1_gfx.c",
           "firmware/ui/assets/mfm1_assets.c", "firmware/ui/assets/mfm1_img.c", "firmware/ui/assets/mfm1_font5x7.c",
           "firmware/src/mfm1_app.c", "firmware/src/mfm1_factory.c", "firmware/platform/host/mfm1_host.c"] + dsp
    # the FM-1 device build's effect buffer sizes, as build.ps1 uses for the UI tests
    flags = ["-O2", "-std=c11", "-Idsp/include", "-Ifirmware/include", "-Ifirmware/platform/host",
             "-DMFE_DELAY_LEN=12288", "-DMFE_OILCAN_RING=8192"]
    env = dict(os.environ, PATH=os.path.dirname(GCC) + os.pathsep + os.environ["PATH"])
    subprocess.run([GCC] + flags + ["-o", exe] + src + ["-lm"], cwd=ROOT, check=True, env=env)
    raw = os.path.join(TMP, "shots")
    shutil.rmtree(raw, ignore_errors=True)
    os.makedirs(raw)
    subprocess.run([exe, raw], cwd=ROOT, check=True, env=env, stdout=subprocess.DEVNULL)
    out = os.path.join(HERE, "img", "fm1")
    os.makedirs(out, exist_ok=True)
    for f in sorted(os.listdir(raw)):
        im = Image.open(os.path.join(raw, f)).convert("RGB")
        im.resize((im.width * 3, im.height * 3), Image.NEAREST).save(os.path.join(out, f), optimize=True)
    tab = os.path.join(HERE, "img", "tablet")
    os.makedirs(tab, exist_ok=True)
    shots_dir = os.path.join(ROOT, "build", "android_ui_shots")
    for f in ["panel", "panel_env", "panel_perf", "panel_stepper", "effects", "matrix"]:
        p = os.path.join(shots_dir, f + ".png")
        if os.path.exists(p):
            im = Image.open(p).convert("RGB")
            im.resize((1600, int(im.height * 1600 / im.width)), Image.LANCZOS).save(
                os.path.join(tab, f + ".jpg"), quality=86)
        else:
            print("missing tablet screenshot", p, "(run: node tests/test_panel.mjs)")


KW = 56          # white key pitch in the SVGs
KX = 50          # left edge of the first white key


def svg_panel():
    """The FM-1 panel under Vintage Chonk, drawn from a photo of the unit (docs/FM1_UI_DESIGN.md, verified layout)."""
    blk = [0, 1, 2, 4, 5, 7, 8, 9, 11, 12, 14]          # black key i sits between white keys i and i+1
    leg = ["OP1", "OP2", "OP3", "OP4", "OP5", "OP6", "PIT", "GLO", "MONO", "POLY", ""]
    mean = ["OSC 1", "OSC 2", "OSC 3", "MOD OSC", "NOISE", "FILTERS", "PITCH", "VOICE", "MONO", "POLY", "MACROS"]
    s = ['<svg viewBox="0 0 1000 560" xmlns="http://www.w3.org/2000/svg" style="width:100%">',
         '<rect x="8" y="8" width="984" height="544" rx="30" fill="#e2622a" stroke="#a8401a" stroke-width="3"/>']

    def knob(x, y, label):
        s.append(f'<text x="{x}" y="{y - 36}" text-anchor="middle" font-size="15" font-weight="800" fill="#fff1dc">{label}</text>')
        s.append(f'<circle cx="{x}" cy="{y}" r="25" fill="#231a1d" stroke="#000" stroke-width="2"/>'
                 f'<line x1="{x}" y1="{y}" x2="{x}" y2="{y - 20}" stroke="#ddd" stroke-width="3" stroke-linecap="round"/>')
    for x, y, label in [(95, 95, "MASTER"), (205, 95, "SELECT"), (95, 205, "PRESETS"), (205, 205, "ALGORITHM")]:
        knob(x, y, label)
    for x, label in [(52, "OCT-"), (150, "OCT+")]:
        s.append(f'<rect x="{x}" y="252" width="88" height="38" rx="9" fill="#b9471f" stroke="#8a3412" stroke-width="2"/>'
                 f'<text x="{x + 44}" y="277" text-anchor="middle" font-size="15" font-weight="800" fill="#3a1a10">{label}</text>')
    s.append('<rect x="290" y="30" width="252" height="252" rx="26" fill="#121014"/>'
             '<image href="img/fm1/home.png" x="306" y="46" width="220" height="220"/>')
    for i, x in enumerate([612, 702, 792, 882]):
        knob(x, 82, f"KNOB{i + 1}")
    rows = [(140, ["FX", "SEL", "ENV", "LFO", "EDIT", "GLO"]), (198, ["HOME", "SAVE", "ARP", "SEQ", "PLAY/STOP", "REC"])]
    for y, labels in rows:
        for c, label in enumerate(labels):
            x = 565 + c * 69
            tc = "#7a3a20" if label == "ARP" else "#2a1208"
            fs = 15 if len(label) < 6 else 11
            s.append(f'<rect x="{x}" y="{y}" width="62" height="48" rx="9" fill="#b9471f" stroke="#8a3412" stroke-width="2"/>'
                     f'<text x="{x + 31}" y="{y + 29}" text-anchor="middle" font-size="{fs}" font-weight="800" fill="{tc}">{label}</text>')
    s.append('<rect x="30" y="300" width="940" height="236" rx="20" fill="#b2421b"/>')
    for i in range(16):
        x = KX + i * KW
        s.append(f'<rect x="{x + 2}" y="418" width="{KW - 4}" height="104" rx="12" fill="#c95427" stroke="#8a3412" stroke-width="2"/>'
                 f'<rect x="{x + KW / 2 - 3}" y="440" width="6" height="44" rx="3" fill="#4a2a2a"/>'
                 f'<text x="{x + KW / 2}" y="512" text-anchor="middle" font-size="13" font-weight="800" fill="#fff1dc">{i + 1}</text>')
    for j, i in enumerate(blk):
        cx = KX + (i + 1) * KW
        s.append(f'<rect x="{cx - 23}" y="334" width="46" height="74" rx="12" fill="#c95427" stroke="#8a3412" stroke-width="2"/>'
                 f'<rect x="{cx - 3}" y="344" width="6" height="30" rx="3" fill="#4a2a2a"/>')
        if leg[j]:
            s.append(f'<text x="{cx}" y="398" text-anchor="middle" font-size="11" font-weight="800" fill="#3a1a10">{leg[j]}</text>')
        s.append(f'<text x="{cx}" y="324" text-anchor="middle" font-size="10.5" font-weight="800" fill="#ffe08a">{mean[j]}</text>')
    s.append('</svg>')
    return "".join(s)


def svg_keys():
    """Which white key picks which modulation source / step (firmware/ui/mfm1_ui.c key_down)."""
    k = ['<svg viewBox="0 0 1000 250" xmlns="http://www.w3.org/2000/svg" style="width:100%">']
    for i in range(16):
        x = KX + i * KW
        k.append(f'<rect x="{x + 2}" y="6" width="{KW - 4}" height="62" rx="10" fill="#fff8ec" stroke="#8a6a60" stroke-width="2"/>'
                 f'<text x="{x + KW / 2}" y="46" text-anchor="middle" font-size="18" font-weight="800" fill="#2a1630">{i + 1}</text>')

    def band(y, a, b, col, label, tc="#fff", fs=17):
        x = KX + a * KW + 2
        w = (b - a) * KW - 4
        k.append(f'<rect x="{x}" y="{y}" width="{w}" height="36" rx="8" fill="{col}"/>'
                 f'<text x="{x + w / 2}" y="{y + 24}" text-anchor="middle" font-size="{fs}" font-weight="800" fill="{tc}">{label}</text>')
    band(80, 0, 4, "#1d7d73", "hold ENV: ENV 1-4")
    band(80, 4, 8, "#5f7a14", "hold LFO: slots 5-8")
    for j, label in enumerate(["VEL", "KEY", "MODW", "AFTT", "BEND", "RAND"]):
        band(80, 8 + j, 9 + j, "#b02a6c", label, fs=12.5)
    k.append(f'<text x="{KX + 11 * KW}" y="140" text-anchor="middle" font-size="16" font-weight="800" fill="#b02a6c">hold GLO: performance sources</text>')
    band(120, 0, 8, "#b85a0c", "hold HOME: macros 1-8")
    band(164, 0, 16, "#c79a12", "hold SEQ on a Performer or Stepper page: select steps 1-16", "#2a1630")
    k.append(f'<text x="{KX}" y="238" font-size="15" fill="#5a3a4e">The 16 lower (white) keys, counted from the left. '
             'While a button is held the keys do not play notes.</text>')
    k.append('</svg>')
    return "".join(k)


def svg_flow():
    """Signal path (dsp/include/mfe.h)."""
    f = ['<svg viewBox="0 0 1000 330" xmlns="http://www.w3.org/2000/svg" style="width:100%">',
         '<defs><marker id="ar" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="7" markerHeight="7" orient="auto">'
         '<path d="M0,0 L10,5 L0,10 z" fill="#5a3a4e"/></marker></defs>']

    def box(x, y, w, h, label, fill, tc="#fff", fs=15, dash=False):
        st = ' stroke="#5a3a4e" stroke-width="2" stroke-dasharray="6 4"' if dash else ''
        f.append(f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="9" fill="{fill}"{st}/>'
                 f'<text x="{x + w / 2}" y="{y + h / 2 + fs * 0.36}" text-anchor="middle" font-size="{fs}" font-weight="800" fill="{tc}">{label}</text>')

    def line(x1, y1, x2, y2, dash=False):
        d = ' stroke-dasharray="5 4"' if dash else ''
        f.append(f'<line x1="{x1}" y1="{y1}" x2="{x2}" y2="{y2}" stroke="#5a3a4e" stroke-width="2"{d} marker-end="url(#ar)"/>')
    srcs = [("OSC 1", 20), ("OSC 2", 70), ("OSC 3", 120), ("NOISE", 170)]
    for label, y in srcs:
        box(20, y, 120, 38, label, "#542c3c", "#faeac8")
        line(140, y + 19, 268, 66)
        line(140, y + 19, 268, 158)
    box(20, 245, 120, 38, "MOD OSC", "#f6ead0", "#542c3c", dash=True)
    line(80, 245, 80, 210, dash=True)
    box(270, 44, 140, 44, "FILTER 1", "#c79a12", "#2a1630")
    box(270, 136, 140, 44, "FILTER 2", "#c79a12", "#2a1630")
    line(340, 88, 340, 134, dash=True)
    f.append('<text x="350" y="117" font-size="12" fill="#5a3a4e">serial</text>')
    line(410, 66, 466, 104)
    line(410, 158, 466, 122)
    f.append('<text x="405" y="210" font-size="12" fill="#5a3a4e">SER / PAR, F1 MIX, F2 MIX</text>')
    box(468, 90, 130, 46, "AMP (ENV 4)", "#1d7d73")
    fx = ["OCT FUZZ", "TAPE", "ENSEMBLE", "BBD ECHO", "OIL CAN", "REVERB"]
    for i, label in enumerate(fx):
        y = 14 + i * 46
        box(660, y, 150, 34, label, "#b02a6c", fs=13)
        if i:
            line(735, y - 12, 735, y)
    line(598, 113, 658, 31)
    line(810, 14 + 5 * 46 + 17, 862, 14 + 5 * 46 + 17)
    box(864, 248, 100, 34, "OUT", "#2a1630", "#faeac8", fs=14)
    f.append('<rect x="190" y="248" width="420" height="72" rx="10" fill="#eef3d8" stroke="#5f7a14" stroke-width="2"/>')
    f.append('<text x="400" y="272" text-anchor="middle" font-size="13" font-weight="800" fill="#2a1630">MODULATION: 16 routings</text>')
    f.append('<text x="400" y="292" text-anchor="middle" font-size="12" fill="#2a1630">ENV 1-4 &#183; slots 5-8 (LFO / Performer / Stepper) &#183; macros 1-8</text>')
    f.append('<text x="400" y="310" text-anchor="middle" font-size="12" fill="#2a1630">velocity &#183; key &#183; mod wheel &#183; bend &#183; aftertouch &#183; random</text>')
    f.append('</svg>')
    return "".join(f)


def expand(html):
    parts = {"panel": svg_panel(), "keys": svg_keys(), "flow": svg_flow()}
    return re.sub(r"<!--INCLUDE (\w+)-->", lambda m: parts[m.group(1)], html)


def print_pdf(html, pdf):
    url = "file:///" + html.replace("\\", "/")
    subprocess.run([CHROME, "--headless=new", "--disable-gpu", "--no-pdf-header-footer",
                    "--allow-file-access-from-files", "--virtual-time-budget=8000",
                    "--print-to-pdf=" + pdf, url], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def main():
    import fitz
    if "--no-shots" not in sys.argv:
        shots()
    html = expand(open(SRC, encoding="utf-8").read())
    pass1 = os.path.join(HERE, "_pass.html")
    open(pass1, "w", encoding="utf-8").write(html)
    tmp_pdf = os.path.join(TMP, "pass1.pdf")
    os.makedirs(TMP, exist_ok=True)
    print_pdf(pass1, tmp_pdf)
    # contents entries are links to #ids; the PDF keeps each link's target page
    doc = fitz.open(tmp_pdf)
    ids = re.findall(r'<a class="toc-row[^"]*" href="#([\w-]+)"', html)
    names = doc.resolve_names()                  # Chrome writes each #id as a named destination
    pages = {i: names[i]["page"] + 1 for i in ids if i in names}
    doc.close()
    if len(pages) < len(ids):
        print("warning: contents entries without a target:", sorted(set(ids) - set(pages)))
    for i, p in pages.items():
        html = re.sub(r'(<span class="pg" data-for="%s">)[^<]*(</span>)' % re.escape(i), r"\g<1>%d\g<2>" % p, html)
    open(pass1, "w", encoding="utf-8").write(html)
    print_pdf(pass1, PDF)
    os.remove(pass1)
    doc = fitz.open(PDF)
    prev = os.path.join(ROOT, "build", "guide_preview")
    shutil.rmtree(prev, ignore_errors=True)
    os.makedirs(prev)
    for i, page in enumerate(doc):
        page.get_pixmap(dpi=80).save(os.path.join(prev, "p%02d.png" % (i + 1)))
    print(f"{PDF}: {len(doc)} pages; previews in {prev}")


if __name__ == "__main__":
    main()
