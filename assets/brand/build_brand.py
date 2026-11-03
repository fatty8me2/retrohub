"""RetroHub brand asset generator: the "Candy D-pad" logo.

Writes the hand-built SVGs (mark in three size-hinted variants, wordmarks, lockup), renders PNGs with resvg
(pip install resvg-py) and assembles retrohub.ico. Run:  python build_brand.py

The in-app logo (ui_draw_brand_logo / ui_draw_brand_wordmark in src/hub/ui_kit.cpp) mirrors this geometry in a 256 box:
keep the two in step when changing either.
"""
import io, os, struct
import resvg_py
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
PNG_DIR = os.path.join(HERE, "png")
os.makedirs(PNG_DIR, exist_ok=True)

SIZES = [16, 20, 24, 32, 40, 48, 64, 96, 128, 256, 512, 1024]
ICO_SIZES = [16, 20, 24, 32, 40, 48, 64, 96, 128, 256]
NAVY = "#1d1446"
VIOLET_DARK_UI = "#9a7bff"     # wordmark on dark backgrounds
VIOLET_LIGHT_UI = "#6a45f5"    # wordmark on light backgrounds
PINK = "#ff5fa8"
YELLOW = "#ffd23f"


# ------------------------------------------------------------------ the mark (256 box, transparent background)
def mark_group(variant, uid="m"):
    """variant: full | mid | tiny. Cross + four candy buttons + play glyph, navy outline, hard shadow (not at tiny)."""
    if variant == "tiny":
        th, rr, ol, br, bo, sh = 66, 30, 12, 32, 66, None
        span = 180
    elif variant == "mid":
        th, rr, ol, br, bo, sh = 60, 27, 12, 28, 66, (5, 7)
        span = 180
    else:
        th, rr, ol, br, bo, sh = 56, 26, 8, 27, 66, (6, 9)
        span = 180
    c = 128
    lo, hi = c - span / 2, c + span / 2
    arm = lambda: (f'<rect x="{lo:.0f}" y="{c-th/2:.0f}" width="{span}" height="{th}" rx="{rr}"/>'
                   f'<rect x="{c-th/2:.0f}" y="{lo:.0f}" width="{th}" height="{span}" rx="{rr}"/>')
    g = f'<linearGradient id="{uid}g" x1="0" y1="0" x2="1" y2="1"><stop offset="0" stop-color="#a58bff"/><stop offset="1" stop-color="#6a45f5"/></linearGradient>'
    out = [f"<defs>{g}</defs>"]
    cols = [("#2fe0ff", c - bo, c - bo), (YELLOW, c + bo, c - bo), ("#ff6fa8", c + bo, c + bo), ("#8be04e", c - bo, c + bo)]
    if sh:
        out.append(f'<g fill="{NAVY}" stroke="{NAVY}" stroke-width="{ol*2}" stroke-linejoin="round" transform="translate({sh[0]} {sh[1]})">{arm()}</g>')
    out.append(f'<g fill="{NAVY}" stroke="{NAVY}" stroke-width="{ol}" stroke-linejoin="round">{arm()}</g>')
    out.append(f'<g fill="url(#{uid}g)">{arm()}</g>')
    if variant != "tiny":
        t = 12 if variant == "full" else 14
        out.append(f'<g fill="{NAVY}" opacity=".28"><path d="M{c-t} {lo+22:.0f}L{c} {lo+12:.0f}L{c+t} {lo+22:.0f}Z"/><path d="M{c-t} {hi-22:.0f}L{c} {hi-12:.0f}L{c+t} {hi-22:.0f}Z"/>'
                   f'<path d="M{lo+22:.0f} {c-t}L{lo+12:.0f} {c}L{lo+22:.0f} {c+t}Z"/><path d="M{hi-22:.0f} {c-t}L{hi-12:.0f} {c}L{hi-22:.0f} {c+t}Z"/></g>')
    pw = 8 if variant != "tiny" else 12
    out.append(f'<path d="M{c-10} {c-16}L{c+18} {c}L{c-10} {c+16}Z" fill="#fff" stroke="#fff" stroke-width="{pw}" stroke-linejoin="round"/>')
    for col, x, y in cols:
        if sh:
            out.append(f'<circle cx="{x+sh[0]*.8:.0f}" cy="{y+sh[1]*.8:.0f}" r="{br+ol/2}" fill="{NAVY}"/>')
        out.append(f'<circle cx="{x}" cy="{y}" r="{br}" fill="{col}" stroke="{NAVY}" stroke-width="{ol}"/>')
        if variant != "tiny":
            out.append(f'<circle cx="{x-9}" cy="{y-9}" r="6" fill="#fff" opacity=".75"/>')
    return "".join(out)


def mark_svg(variant="full"):
    return f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 256 256">{mark_group(variant)}</svg>\n'


# ------------------------------------------------------------------ the wordmark (stroked letters, cap 0..98)
GLYPH = {
    "R": (56, '<path d="M0 98V0H28A25 25 0 0 1 28 50H0M26 50L52 98"/>'),
    "e": (62, '<path d="M0 67H62A31 31 0 1 0 54.7 86.9"/>'),
    "t": (44, '<path d="M20 10V80Q20 98 42 98M3 36H41"/>'),
    "r": (36, '<path d="M2 36V98M2 66Q2 36 36 37"/>'),
    "o": (62, '<circle cx="31" cy="67" r="31"/>'),
    "H": (54, '<path d="M0 0V98M54 0V98M0 49H54"/>'),
    "u": (56, '<path d="M2 36V71A27 27 0 0 0 56 71V36M56 36V98"/>'),
    "b": (64, '<path d="M2 0V98"/><circle cx="33" cy="67" r="31"/>'),
}
SW, GAP = 17, 22


def word_group(violet, outline=True):
    xs, x = [], 0
    for ch in "RetroHub":
        xs.append(x)
        x += GLYPH[ch][0] + GAP
    width = x - GAP

    def layer(w, col_fn, dx=0, dy=0):
        s = [f'<g fill="none" stroke-linecap="round" stroke-linejoin="round" stroke-width="{w}" transform="translate({dx} {dy})">']
        for i, ch in enumerate("RetroHub"):
            s.append(f'<g stroke="{col_fn(i)}" transform="translate({xs[i]} 0)">{GLYPH[ch][1]}</g>')
        s.append("</g>")
        return "".join(s)
    ol = 6 if outline else 0
    parts = [layer(SW + 2 * ol, lambda i: PINK, 5, 7)]
    if outline:
        parts.append(layer(SW + 2 * ol, lambda i: NAVY))
    parts.append(layer(SW, lambda i: YELLOW if i == 4 else violet))
    return "".join(parts), width


def wordmark_svg(violet, outline):
    g, w = word_group(violet, outline)
    pad = 24
    W, H = w + 2 * pad + 8, 98 + 2 * pad + 8 + 6
    return f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {W:.0f} {H}"><g transform="translate({pad+SW/2} {pad+SW/2})">{g}</g></svg>\n'


def lockup_svg(violet, outline=True):
    g, w = word_group(violet, outline)
    s = 1.18
    ox = 256 + 28
    W = ox + w * s + 40
    return (f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {W:.0f} 256">{mark_group("full")}'
            f'<g transform="translate({ox} {128 - 49 * s:.1f}) scale({s})">{g}</g></svg>\n')


# ------------------------------------------------------------------ output
def write(path, text):
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)


def render(svg_text, w, h=None):
    return bytes(resvg_py.svg_to_bytes(svg_string=svg_text, width=w, **({"height": h} if h else {})))


def variant_for(size):
    return "tiny" if size <= 24 else "mid" if size <= 48 else "full"


def main():
    write(os.path.join(HERE, "retrohub-logo.svg"), mark_svg("full"))
    write(os.path.join(HERE, "retrohub-logo-mid.svg"), mark_svg("mid"))
    write(os.path.join(HERE, "retrohub-logo-tiny.svg"), mark_svg("tiny"))
    write(os.path.join(HERE, "retrohub-mark.svg"), mark_svg("full"))
    write(os.path.join(HERE, "retrohub-wordmark.svg"), wordmark_svg(VIOLET_DARK_UI, False))
    write(os.path.join(HERE, "retrohub-wordmark-light.svg"), wordmark_svg(VIOLET_LIGHT_UI, True))
    write(os.path.join(HERE, "retrohub-lockup.svg"), lockup_svg(VIOLET_DARK_UI, True))
    imgs = {}
    for s in SIZES:
        # hinted variant, rendered at 4x and downsampled for the small sizes so they stay crisp but anti-aliased
        ss = s * 4 if s <= 48 else s
        big = Image.open(io.BytesIO(render(mark_svg(variant_for(s)), ss))).convert("RGBA")
        if ss != s:
            big = big.resize((s, s), Image.LANCZOS)
        p = os.path.join(PNG_DIR, f"retrohub-{s}.png")
        big.save(p, optimize=True)
        imgs[s] = p
    blobs = [open(imgs[s], "rb").read() for s in ICO_SIZES]
    hdr = struct.pack("<HHH", 0, 1, len(ICO_SIZES))
    off = 6 + 16 * len(ICO_SIZES)
    ents = b""
    for s, b in zip(ICO_SIZES, blobs):
        ents += struct.pack("<BBBBHHII", 0 if s >= 256 else s, 0 if s >= 256 else s, 0, 0, 1, 32, len(b), off)
        off += len(b)
    with open(os.path.join(HERE, "retrohub.ico"), "wb") as f:
        f.write(hdr + ents + b"".join(blobs))
    for name in ("retrohub-wordmark", "retrohub-wordmark-light", "retrohub-lockup"):
        open(os.path.join(PNG_DIR, name + ".png"), "wb").write(render(open(os.path.join(HERE, name + ".svg"), encoding="utf-8").read(), 1600))
    print("ok")


if __name__ == "__main__":
    main()
