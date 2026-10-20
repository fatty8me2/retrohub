"""RetroHub brand asset generator.

Writes the hand-built SVGs (logo, mark, wordmark, per-size hinted variants), renders PNGs with resvg
(pip install resvg-py) and assembles retrohub.ico. Run:  python build_brand.py
"""
import math, os, struct, io
import resvg_py
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
PNG_DIR = os.path.join(HERE, "png")
os.makedirs(PNG_DIR, exist_ok=True)

SIZES = [16, 20, 24, 32, 40, 48, 64, 96, 128, 256, 512, 1024]

DEFS = """<defs>
<linearGradient id="bg" x1="0" y1="0" x2="1" y2="1"><stop offset="0" stop-color="#2b2670"/><stop offset=".5" stop-color="#16163a"/><stop offset="1" stop-color="#0b0c1b"/></linearGradient>
<linearGradient id="g" x1="0" y1="0" x2="1" y2="1"><stop offset="0" stop-color="#7c8bff"/><stop offset=".5" stop-color="#a064ff"/><stop offset="1" stop-color="#ff5fb0"/></linearGradient>
<linearGradient id="gp" gradientUnits="userSpaceOnUse" x1="300" y1="200" x2="760" y2="850"><stop offset="0" stop-color="#7c8bff"/><stop offset=".5" stop-color="#a064ff"/><stop offset="1" stop-color="#ff5fb0"/></linearGradient>
<linearGradient id="sh" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#fff" stop-opacity=".20"/><stop offset=".5" stop-color="#fff" stop-opacity="0"/></linearGradient>
<radialGradient id="glow" cx=".5" cy=".5" r=".5"><stop offset="0" stop-color="#8a6bff" stop-opacity=".55"/><stop offset="1" stop-color="#8a6bff" stop-opacity="0"/></radialGradient>
</defs>"""


def hex_pts(cx, cy, r):
    return [(cx + r * math.cos(math.radians(a)), cy + r * math.sin(math.radians(a))) for a in (-90, -30, 30, 90, 150, 210)]


def poly(pts):
    return "M" + "L".join(f"{x:.1f} {y:.1f}" for x, y in pts) + "Z"


def mark_group(variant, ink_gradient="url(#gp)", cut="#0f1026"):
    """The hub mark centred on (512,512) in a 1024 box. variant: full | mid | tiny."""
    if variant == "tiny":
        R, sw = 300, 120   # fat hexagon, nothing but hex + play
        tri = "M440 350L690 512L440 674Z"
        return (f'<path d="{poly(hex_pts(512,512,R))}" fill="{ink_gradient}" stroke="{ink_gradient}" stroke-width="{sw}" stroke-linejoin="round"/>'
                f'<path d="{tri}" fill="#fff" stroke="#fff" stroke-width="44" stroke-linejoin="round"/>')
    R, sw = 300, 84
    hexp = poly(hex_pts(512, 512, R))
    if variant == "mid":
        tri = "M450 352L676 482L450 612Z"
        mask = ('<mask id="m"><rect width="1024" height="1024" fill="#fff"/>'
                '<rect x="0" y="716" width="1024" height="44" fill="#000"/></mask>')
        return (f'{mask}<g mask="url(#m)"><path d="{hexp}" fill="{ink_gradient}" stroke="{ink_gradient}" stroke-width="{sw}" stroke-linejoin="round"/></g>'
                f'<path d="{tri}" fill="#fff" stroke="#fff" stroke-width="40" stroke-linejoin="round"/>')
    tri = "M452 360L664 482L452 604Z"
    mask = ('<mask id="m"><rect width="1024" height="1024" fill="#fff"/>'
            '<rect x="0" y="690" width="1024" height="20" fill="#000"/>'
            '<rect x="0" y="764" width="1024" height="34" fill="#000"/></mask>')
    vs = hex_pts(512, 512, R)
    nodes = "".join(f'<circle cx="{vs[i][0]:.1f}" cy="{vs[i][1]:.1f}" r="46" fill="#34defc" stroke="{cut}" stroke-width="14"/>' for i in (0, 2, 4))
    return (f'{mask}<g mask="url(#m)"><path d="{hexp}" fill="{ink_gradient}" stroke="{ink_gradient}" stroke-width="{sw}" stroke-linejoin="round"/></g>'
            f'<path d="{tri}" fill="#fff" stroke="#fff" stroke-width="52" stroke-linejoin="round"/>'
            f'{nodes}')


def tile(variant):
    m = 48 if variant == "full" else 16      # small sizes: use more of the canvas
    w = 1024 - 2 * m
    rx = {"full": 224, "mid": 232, "tiny": 210}[variant]
    s = ('<ellipse cx="512" cy="520" rx="420" ry="420" fill="url(#glow)"/>' if variant == "full" else "")
    rim = f'<rect x="{m+4}" y="{m+4}" width="{w-8}" height="{w-8}" rx="{rx-4}" fill="none" stroke="#9c8cff" stroke-opacity=".38" stroke-width="{8 if variant=="full" else 16}"/>'
    return (f'<rect x="{m}" y="{m}" width="{w}" height="{w}" rx="{rx}" fill="url(#bg)"/>{s}'
            f'<rect x="{m}" y="{m}" width="{w}" height="{w}" rx="{rx}" fill="url(#sh)"/>{rim}')


def svg_doc(inner, vb="0 0 1024 1024", w=None, h=None):
    wh = f' width="{w}" height="{h}"' if w else ""
    return f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="{vb}"{wh}>{DEFS}{inner}</svg>\n'


def logo_svg(variant):
    return svg_doc(tile(variant) + mark_group(variant))


def mark_svg():
    # icon-only (no tile): works on dark and light; the node cut-outs use a transparent ring via mask-free stroke colour
    return svg_doc(f'<g transform="translate(0 0)">{mark_group("full", cut="#ffffff00")}</g>', vb="150 150 724 724")


# ---- monoline wordmark glyphs (geometric strokes; baseline y=400, x-height top y=120, cap y=0) ----
GLYPHS = {
    "R": (250, '<path d="M0 400V0H110a100 100 0 0 1 0 200H0M110 200L230 400"/>'),
    "e": (280, '<path d="M0 260H280A140 140 0 1 0 239 359"/>'),
    "t": (190, '<path d="M95 40V300a100 100 0 0 0 95 100M10 130H190"/>'),
    "r": (160, '<path d="M0 120V400M0 270Q10 130 160 130"/>'),
    "o": (280, '<circle cx="140" cy="260" r="140"/>'),
    "H": (250, '<path d="M0 0V400M0 200H250M250 0V400"/>'),
    "u": (250, '<path d="M0 120V270a125 125 0 0 0 250 0V120M250 270V400"/>'),
    "b": (280, '<path d="M0 0V400"/><circle cx="140" cy="260" r="140"/>'),
}


def wordmark_svg(dark_bg=True, with_mark=True):
    txt = "#eef0f6" if dark_bg else "#171a2e"
    x = 0
    parts = []
    grads = []
    gap = 84
    hub0 = sum(GLYPHS[c][0] + gap for c in "Retro")
    hubw = sum(GLYPHS[c][0] + gap for c in "Hub") - gap
    for i, ch in enumerate("RetroHub"):
        w, d = GLYPHS[ch]
        stroke = txt if i < 5 else f"url(#wg{i})"
        if i >= 5:
            grads.append(f'<linearGradient id="wg{i}" gradientUnits="userSpaceOnUse" x1="{hub0-x:.0f}" y1="0" x2="{hub0-x+hubw:.0f}" y2="400"><stop offset="0" stop-color="#8b94ff"/><stop offset=".55" stop-color="#a064ff"/><stop offset="1" stop-color="#ff5fb0"/></linearGradient>')
        parts.append(f'<g transform="translate({x} 0)" stroke="{stroke}">{d}</g>')
        x += w + gap
    total = x - gap
    pad = 40
    sw = 62
    body = f'<g fill="none" stroke-width="{sw}" stroke-linecap="round" stroke-linejoin="round" transform="translate({pad+sw/2} {pad+sw/2})">{"".join(parts)}</g>'
    wgrad = "<defs>" + "".join(grads) + "</defs>"
    textw = total + 2 * pad + sw
    textx = 0
    h = 400 + 2 * pad + sw
    inner = ""
    if with_mark:
        ms = h * 1.0
        # tile logo to the left, scaled to height h
        scale = ms / 1024
        inner += f'<g transform="scale({scale:.5f})">{tile("full")}{mark_group("full")}</g>'
        textx = ms + 70
    inner += f'<g transform="translate({textx:.1f} 0)">{body}</g>'
    W = textx + textw
    return f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {W:.0f} {h:.0f}">{DEFS}{wgrad}{inner}</svg>\n'


def write(path, text):
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)


def render(svg_text, size):
    return bytes(resvg_py.svg_to_bytes(svg_string=svg_text, width=size, height=size))


def variant_for(size):
    return "tiny" if size <= 24 else "mid" if size <= 48 else "full"


def main():
    write(os.path.join(HERE, "retrohub-logo.svg"), logo_svg("full"))
    write(os.path.join(HERE, "retrohub-logo-mid.svg"), logo_svg("mid"))
    write(os.path.join(HERE, "retrohub-logo-tiny.svg"), logo_svg("tiny"))
    write(os.path.join(HERE, "retrohub-mark.svg"), mark_svg())
    write(os.path.join(HERE, "retrohub-wordmark.svg"), wordmark_svg(True))
    write(os.path.join(HERE, "retrohub-wordmark-light.svg"), wordmark_svg(False))
    imgs = {}
    for s in SIZES:
        # render the hinted variant at 2x-4x then downsample, so small sizes are crisp but anti-aliased
        ss = max(s * 4 if s <= 48 else s, s)
        big = Image.open(io.BytesIO(render(logo_svg(variant_for(s)), ss))).convert("RGBA")
        if ss != s:
            big = big.resize((s, s), Image.LANCZOS)
        p = os.path.join(PNG_DIR, f"retrohub-{s}.png")
        big.save(p, optimize=True)
        imgs[s] = p
    # ICO: PNG-compressed entries, all sizes <= 256
    ico_sizes = [16, 20, 24, 32, 40, 48, 64, 96, 128, 256]
    blobs = [open(imgs[s], "rb").read() for s in ico_sizes]
    hdr = struct.pack("<HHH", 0, 1, len(ico_sizes))
    off = 6 + 16 * len(ico_sizes)
    ents = b""
    for s, b in zip(ico_sizes, blobs):
        ents += struct.pack("<BBBBHHII", 0 if s >= 256 else s, 0 if s >= 256 else s, 0, 0, 1, 32, len(b), off)
        off += len(b)
    with open(os.path.join(HERE, "retrohub.ico"), "wb") as f:
        f.write(hdr + ents + b"".join(blobs))
    # wordmark PNG preview
    for name in ("retrohub-wordmark", "retrohub-wordmark-light"):
        svg = open(os.path.join(HERE, name + ".svg"), encoding="utf-8").read()
        png = resvg_py.svg_to_bytes(svg_string=svg, width=1600)
        open(os.path.join(PNG_DIR, name + ".png"), "wb").write(bytes(png))
    print("ok")


if __name__ == "__main__":
    main()
