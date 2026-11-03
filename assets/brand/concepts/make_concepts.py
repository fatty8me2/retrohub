"""RetroHub logo concepts (phase 1). Hand-written SVGs, wordmarks built from stroked/pixel shapes (no font needed).

Run:  python make_concepts.py        (needs resvg-py + Pillow)
Writes, per concept N:  logoN-<slug>.svg (lockup), logoN-<slug>-icon.svg, logoN-<slug>-large.png (lockup, transparent),
logoN-<slug>-icon-256.png; plus comparison-sheet.png.
"""
import io, os
import resvg_py
from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
NAVY = "#1d1446"
DARK = (18, 19, 26)       # app sidebar ~ #12131a
LIGHT = (246, 244, 250)

# ---------------------------------------------------------------- stroked wordmark (cap 0..98, x-height top 36)
GLYPH = {
    "R": (56, ['<path d="M0 98V0H28A25 25 0 0 1 28 50H0M26 50L52 98"/>']),
    "e": (62, ['<path d="M0 67H62A31 31 0 1 0 54.7 86.9"/>']),
    "t": (44, ['<path d="M20 10V80Q20 98 42 98M3 36H41"/>']),
    "r": (36, ['<path d="M2 36V98M2 66Q2 36 36 37"/>']),
    "o": (62, ['<circle cx="31" cy="67" r="31"/>']),
    "H": (54, ['<path d="M0 0V98M54 0V98M0 49H54"/>']),
    "u": (56, ['<path d="M2 36V71A27 27 0 0 0 56 71V36M56 36V98"/>']),
    "b": (64, ['<path d="M2 0V98"/><circle cx="33" cy="67" r="31"/>']),
}


def stroke_word(text="RetroHub", sw=16, fills=None, ol=7, ol_col=NAVY, gap=None, bounce=None, shadow=None, sticker=None):
    """Returns (svg_group, width, ytop, ybot) in word units. fills: list of colours (per letter) or one colour/url().
    bounce: list of (dy, rot_deg) per letter. shadow: (dx, dy, colour). sticker: (white ring extra width, outer navy extra)."""
    gap = gap if gap is not None else sw * 0.7 + 10
    n = len(text)
    fills = fills or ["#fff"] * n
    if isinstance(fills, str):
        fills = [fills] * n
    bounce = bounce or [(0, 0)] * n
    xs, x = [], 0.0
    for ch in text:
        xs.append(x)
        x += GLYPH[ch][0] + gap
    width = x - gap

    def layer(width_, colour_fn, dx=0, dy=0):
        parts = [f'<g fill="none" stroke-linecap="round" stroke-linejoin="round" stroke-width="{width_:.1f}" transform="translate({dx} {dy})">']
        for i, ch in enumerate(text):
            w, els = GLYPH[ch]
            d, r = bounce[i]
            parts.append(f'<g stroke="{colour_fn(i)}" transform="translate({xs[i]:.1f} {d}) rotate({r} {w/2} 49)">{"".join(els)}</g>')
        parts.append("</g>")
        return "".join(parts)

    out = []
    if shadow:
        out.append(layer(sw + 2 * ol, lambda i: shadow[2], shadow[0], shadow[1]))
    if sticker:
        out.append(layer(sw + 2 * ol + sticker[0] * 2 + sticker[1] * 2, lambda i: ol_col))
        out.append(layer(sw + 2 * ol + sticker[0] * 2, lambda i: "#fff"))
    out.append(layer(sw + 2 * ol, lambda i: ol_col))
    out.append(layer(sw, lambda i: fills[i]))
    pad = sw / 2 + ol + (sticker[0] + sticker[1] if sticker else 0)
    ext_b = max(abs(b[0]) for b in bounce) + (shadow[1] if shadow else 0)
    return "".join(out), width, -pad - 8, 98 + pad + ext_b + 8


# ---------------------------------------------------------------- pixel font 5x7
PIX = {
    "R": ["XXXX.", "X...X", "X...X", "XXXX.", "X.X..", "X..X.", "X...X"],
    "E": ["XXXXX", "X....", "X....", "XXXX.", "X....", "X....", "XXXXX"],
    "T": ["XXXXX", "..X..", "..X..", "..X..", "..X..", "..X..", "..X.."],
    "O": [".XXX.", "X...X", "X...X", "X...X", "X...X", "X...X", ".XXX."],
    "H": ["X...X", "X...X", "X...X", "XXXXX", "X...X", "X...X", "X...X"],
    "U": ["X...X", "X...X", "X...X", "X...X", "X...X", "X...X", ".XXX."],
    "B": ["XXXX.", "X...X", "X...X", "XXXX.", "X...X", "X...X", "XXXX."],
}
RAINBOW = ["#ff4f8b", "#ff7a3d", "#ffc233", "#8be04e", "#2fd9c5", "#4aa8ff", "#9a6bff"]


def pixel_text(text, c, ox, oy, shadow="#0c0630"):
    """Cells of size c. Letters 5 wide, 1 gap. Rainbow by row, 1-cell hard shadow. Returns svg string, width in cells."""
    cells = []
    x = 0
    for ch in text:
        for ry, row in enumerate(PIX[ch]):
            for rx, v in enumerate(row):
                if v == "X":
                    cells.append((x + rx, ry))
        x += 6
    sh = "".join(f'<rect x="{ox+(cx+1)*c}" y="{oy+(cy+1)*c}" width="{c}" height="{c}"/>' for cx, cy in cells)
    fg = "".join(f'<rect x="{ox+cx*c}" y="{oy+cy*c}" width="{c}" height="{c}" fill="{RAINBOW[cy]}"/>' for cx, cy in cells)
    return f'<g shape-rendering="crispEdges"><g fill="{shadow}">{sh}</g>{fg}</g>', x - 1


# ---------------------------------------------------------------- icons (all 256 x 256)
def icon1():
    """Pad Pal: friendly blob controller with a face."""
    shapes = ('<rect x="28" y="66" width="200" height="104" rx="52"/><circle cx="64" cy="160" r="42"/><circle cx="192" cy="160" r="42"/>')
    return f'''<defs><linearGradient id="b1" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#a98bff"/><stop offset="1" stop-color="#6340f2"/></linearGradient>
<clipPath id="c1">{shapes}</clipPath></defs>
<g transform="translate(0 14)">
<path d="M128 70Q128 42 150 40" fill="none" stroke="{NAVY}" stroke-width="22" stroke-linecap="round"/>
<path d="M128 70Q128 42 150 40" fill="none" stroke="#fff" stroke-width="7" stroke-linecap="round" opacity=".9"/>
<circle cx="154" cy="36" r="17" fill="#ffd23f" stroke="{NAVY}" stroke-width="9"/>
<circle cx="148" cy="30" r="5" fill="#fff" opacity=".85"/>
<g fill="{NAVY}" stroke="{NAVY}" stroke-width="20" stroke-linejoin="round">{shapes}</g>
<g fill="url(#b1)">{shapes}</g>
<g clip-path="url(#c1)"><rect x="28" y="66" width="200" height="30" fill="#fff" opacity=".16"/>
<ellipse cx="128" cy="214" rx="120" ry="30" fill="#2a1a9a" opacity=".28"/></g>
<g fill="#f6f1ff" stroke="{NAVY}" stroke-width="6" stroke-linejoin="round"><path d="M56 82h18v14h14v18H74v14H56v-14H42V96h14z"/></g>
<circle cx="199" cy="92" r="11" fill="#ffd23f" stroke="{NAVY}" stroke-width="6"/>
<circle cx="219" cy="110" r="11" fill="#2fe0ff" stroke="{NAVY}" stroke-width="6"/>
<g><ellipse cx="104" cy="112" rx="19" ry="23" fill="#fff" stroke="{NAVY}" stroke-width="6"/><ellipse cx="152" cy="112" rx="19" ry="23" fill="#fff" stroke="{NAVY}" stroke-width="6"/>
<circle cx="109" cy="118" r="10" fill="{NAVY}"/><circle cx="157" cy="118" r="10" fill="{NAVY}"/>
<circle cx="113" cy="113" r="3.6" fill="#fff"/><circle cx="161" cy="113" r="3.6" fill="#fff"/></g>
<path d="M107 146Q128 170 149 146" fill="none" stroke="{NAVY}" stroke-width="8" stroke-linecap="round"/>
<ellipse cx="84" cy="146" rx="11" ry="7" fill="#ff7ab0" opacity=".9"/><ellipse cx="172" cy="146" rx="11" ry="7" fill="#ff7ab0" opacity=".9"/>
</g>'''


def icon2():
    """Pixel: chunky 8-bit RH tile with a pixel heart."""
    c = 16
    cells = []
    for y in range(16):
        for x in range(16):
            if (x in (0, 15)) and (y in (0, 15)):
                continue
            edge = x in (0, 15) or y in (0, 15)
            corner_in = (x, y) in ((1, 1), (14, 1), (1, 14), (14, 14))
            col = "#8a73ff" if edge else "#4a35b8" if corner_in else ("#2b1c80" if y < 8 else "#24176f")
            cells.append(f'<rect x="{x*c}" y="{y*c}" width="{c}" height="{c}" fill="{col}"/>')
    # letters R(2..6) H(9..13), rows 2..8
    sh, fg = [], []
    for letter, x0 in (("R", 2), ("H", 9)):
        for ry, row in enumerate(PIX[letter]):
            for rx, v in enumerate(row):
                if v == "X":
                    sh.append(f'<rect x="{(x0+rx+1)*c}" y="{(2+ry+1)*c}" width="{c}" height="{c}"/>')
                    fg.append(f'<rect x="{(x0+rx)*c}" y="{(2+ry)*c}" width="{c}" height="{c}" fill="{RAINBOW[ry]}"/>')
    heart = ["XX..XX", "XXXXXX", ".XXXX.", "..XX.."]
    hp = []
    for ry, row in enumerate(heart):
        for rx, v in enumerate(row):
            if v == "X":
                hp.append(f'<rect x="{(5+rx)*c}" y="{(10+ry)*c}" width="{c}" height="{c}" fill="#ff4b6e"/>')
    hp.append(f'<rect x="{5*c}" y="{10*c}" width="{c}" height="{c}" fill="#ffd0da"/>')
    return (f'<g shape-rendering="crispEdges">{"".join(cells)}<g fill="#0c0630">{"".join(sh)}</g>{"".join(fg)}{"".join(hp)}</g>')


def icon3():
    """Sticker: tilted cartridge with a play label, die-cut white edge, sparkles."""
    body = "M72 40H184Q202 40 202 58V196Q202 214 184 214H72Q54 214 54 196V58Q54 40 72 40Z"
    spark = lambda cx, cy, r, f: (f'<path d="M{cx} {cy-r}Q{cx+r*.18} {cy-r*.18} {cx+r} {cy}Q{cx+r*.18} {cy+r*.18} {cx} {cy+r}Q{cx-r*.18} {cy+r*.18} {cx-r} {cy}Q{cx-r*.18} {cy-r*.18} {cx} {cy-r}Z" '
                                  f'fill="{f}" stroke="{NAVY}" stroke-width="6" stroke-linejoin="round"/>')
    return f'''<defs><linearGradient id="b3" x1="0" y1="0" x2="1" y2="1"><stop offset="0" stop-color="#3ff0d0"/><stop offset="1" stop-color="#16a6d9"/></linearGradient>
<linearGradient id="l3" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#ffe45c"/><stop offset="1" stop-color="#ff8a3d"/></linearGradient></defs>
<g transform="rotate(-9 128 128)">
<path d="{body}" fill="{NAVY}" opacity=".35" transform="translate(7 10)" stroke="{NAVY}" stroke-width="34" stroke-linejoin="round"/>
<path d="{body}" fill="{NAVY}" stroke="{NAVY}" stroke-width="34" stroke-linejoin="round"/>
<path d="{body}" fill="#fff" stroke="#fff" stroke-width="24" stroke-linejoin="round"/>
<path d="{body}" fill="url(#b3)" stroke="{NAVY}" stroke-width="7" stroke-linejoin="round"/>
<g stroke="{NAVY}" stroke-width="7" stroke-linecap="round" opacity=".55"><path d="M82 62H174M82 76H174"/></g>
<rect x="72" y="92" width="112" height="84" rx="16" fill="url(#l3)" stroke="{NAVY}" stroke-width="7"/>
<path d="M112 108L156 134L112 160Z" fill="#fff" stroke="{NAVY}" stroke-width="7" stroke-linejoin="round"/>
<rect x="86" y="190" width="84" height="10" rx="5" fill="{NAVY}" opacity=".7"/>
<path d="M62 52Q62 46 72 46H110" fill="none" stroke="#fff" stroke-width="6" stroke-linecap="round" opacity=".55"/>
</g>
{spark(214, 56, 24, "#ffe45c")}{spark(40, 196, 18, "#ff6fb0")}{spark(226, 206, 10, "#fff")}'''


def icon4():
    """Couch crew: three friends on a sofa, on a warm tile."""
    o = f'stroke="{NAVY}" stroke-width="8" stroke-linejoin="round"'
    face = lambda cx, cy, f: (f'<circle cx="{cx}" cy="{cy}" r="27" fill="{f}" {o}/>'
                              f'<circle cx="{cx-9}" cy="{cy-3}" r="4.6" fill="{NAVY}"/><circle cx="{cx+9}" cy="{cy-3}" r="4.6" fill="{NAVY}"/>'
                              f'<path d="M{cx-8} {cy+8}Q{cx} {cy+17} {cx+8} {cy+8}" fill="none" stroke="{NAVY}" stroke-width="4.5" stroke-linecap="round"/>')
    return f'''<defs><linearGradient id="t4" x1="0" y1="0" x2="1" y2="1"><stop offset="0" stop-color="#5b3df5"/><stop offset="1" stop-color="#9b3fe0"/></linearGradient>
<linearGradient id="s4" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#ff9a62"/><stop offset="1" stop-color="#ff6a45"/></linearGradient></defs>
<rect x="8" y="8" width="240" height="240" rx="64" fill="url(#t4)"/>
<rect x="8" y="8" width="240" height="120" rx="64" fill="#fff" opacity=".10"/>
<rect x="12" y="12" width="232" height="232" rx="60" fill="none" stroke="#fff" stroke-opacity=".25" stroke-width="3"/>
{face(86, 100, "#ffd23f")}{face(128, 88, "#33e1ff")}{face(170, 100, "#ff7ab0")}
<rect x="52" y="116" width="152" height="74" rx="28" fill="#f0603a" {o}/>
<rect x="64" y="146" width="128" height="52" rx="20" fill="url(#s4)" {o}/>
<rect x="32" y="130" width="40" height="80" rx="20" fill="#ff7a52" {o}/>
<rect x="184" y="130" width="40" height="80" rx="20" fill="#ff7a52" {o}/>
<rect x="46" y="206" width="14" height="18" rx="5" fill="{NAVY}"/><rect x="196" y="206" width="14" height="18" rx="5" fill="{NAVY}"/>
<path d="M84 160H172" stroke="#fff" stroke-opacity=".35" stroke-width="5" stroke-linecap="round"/>'''


def icon5():
    """Pop D-pad: violet cross with four candy buttons, hard shadow."""
    cross = '<rect x="38" y="100" width="180" height="56" rx="26"/><rect x="100" y="38" width="56" height="180" rx="26"/>'
    btn = lambda cx, cy, f: (f'<circle cx="{cx+5}" cy="{cy+7}" r="27" fill="{NAVY}"/>'
                             f'<circle cx="{cx}" cy="{cy}" r="27" fill="{f}" stroke="{NAVY}" stroke-width="8"/>'
                             f'<circle cx="{cx-9}" cy="{cy-9}" r="6" fill="#fff" opacity=".75"/>')
    return f'''<defs><linearGradient id="d5" x1="0" y1="0" x2="1" y2="1"><stop offset="0" stop-color="#a58bff"/><stop offset="1" stop-color="#6a45f5"/></linearGradient></defs>
<g transform="translate(-2 -4)">
<g fill="{NAVY}" stroke="{NAVY}" stroke-width="18" stroke-linejoin="round" transform="translate(6 9)">{cross}</g>
<g fill="url(#d5)" stroke="{NAVY}" stroke-width="8" stroke-linejoin="round">{cross}</g>
<g fill="url(#d5)">{cross}</g>
<path d="M110 84V60L128 46L146 60V84Z M110 172V196L128 210L146 196V172Z" fill="none"/>
<g fill="{NAVY}" opacity=".28"><path d="M118 62L128 52L138 62Z"/><path d="M118 194L128 204L138 194Z"/><path d="M62 118L52 128L62 138Z"/><path d="M194 118L204 128L194 138Z"/></g>
<path d="M118 112L146 128L118 144Z" fill="#fff" stroke="#fff" stroke-width="8" stroke-linejoin="round"/>
{btn(62, 62, "#2fe0ff")}{btn(194, 62, "#ffd23f")}{btn(194, 194, "#ff6fa8")}{btn(62, 194, "#8be04e")}
</g>'''


# ---------------------------------------------------------------- lockups
def word_for(n):
    if n == 1:
        g, w, y0, y1 = stroke_word(sw=17, ol=7, fills=["#ff6b9a"] * 5 + ["#ffd23f"] * 3, bounce=[(0, 0)] * 8)
    elif n == 3:
        cols = ["#ff6fb0", "#ffb62e", "#3ff0d0", "#7c8bff", "#ff7a3d", "#c26bff", "#8be04e", "#33c8ff"]
        bounce = [(0, -6), (-5, 4), (3, -4), (-4, 5), (4, -5), (-3, 3), (3, -4), (-5, 5)]
        g, w, y0, y1 = stroke_word(sw=15, ol=5, fills=cols, bounce=bounce, sticker=(5, 3), gap=24)
    elif n == 4:
        g, w, y0, y1 = stroke_word(sw=20, ol=7, fills=["#ff8a4d"] * 5 + ["#ffd23f"] * 3, gap=19)
    elif n == 5:
        g, w, y0, y1 = stroke_word(sw=16, ol=6, fills=["#9a7bff"] * 4 + ["#ffd23f"] + ["#9a7bff"] * 3, shadow=(5, 7, "#ff5fa8"), gap=24)
    return g, w, y0, y1


def lockup(n, name):
    if n == 2:
        c = 16
        wd, wc = pixel_text("RETROHUB", c, 0, 0)
        ox = 256 + 40
        icon = globals()[f"icon{n}"]()
        W = ox + wc * c + 40
        # vertical centre 7 rows -> y offset
        wd, _ = pixel_text("RETROHUB", c, ox, (256 - 7 * c) // 2 - 4)
        return f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {W} 256" width="{W}" height="256">{icon}{wd}</svg>\n', W
    g, w, y0, y1 = word_for(n)
    s = 1.18 if n != 4 else 1.15
    ox = 256 + 34
    cy = 128 + (6 if n == 3 else 0)
    ty = cy - (49 * s)
    W = int(ox + w * s + 34)
    icon = globals()[f"icon{n}"]()
    return (f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {W} 256" width="{W}" height="256">{icon}'
            f'<g transform="translate({ox} {ty:.1f}) scale({s})">{g}</g></svg>\n'), W


CONCEPTS = [
    (1, "padpal", "Pad Pal", "A friendly blob controller with a face, antenna bobble and cheeks; pink/yellow stroked wordmark."),
    (2, "pixel", "Pixel Pop", "Chunky 8-bit RH tile with a pixel heart; rainbow pixel-font wordmark with hard shadow."),
    (3, "sticker", "Cart Sticker", "Die-cut sticker of a tilted cartridge with a play label and sparkles; bouncy multicolour letters."),
    (4, "couch", "Couch Crew", "Three blob friends on a coral sofa on a purple tile; warm heavy wordmark."),
    (5, "dpad", "Candy D-pad", "Violet D-pad hub with four candy buttons and a hard pop shadow; violet wordmark with pink shadow."),
]


def render_svg(svg_text, width, height=None):
    kw = dict(svg_string=svg_text, width=width)
    if height:
        kw["height"] = height
    return Image.open(io.BytesIO(bytes(resvg_py.svg_to_bytes(**kw)))).convert("RGBA")


def build():
    items = []
    for n, slug, title, desc in CONCEPTS:
        lk, W = lockup(n, slug)
        icon_svg = f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 256 256" width="256" height="256">{globals()[f"icon{n}"]()}</svg>\n'
        base = f"logo{n}-{slug}"
        for fn, txt in ((base + ".svg", lk), (base + "-icon.svg", icon_svg)):
            open(os.path.join(HERE, fn), "w", encoding="utf-8").write(txt)
        big = render_svg(lk, 1600)
        big.save(os.path.join(HERE, base + "-large.png"))
        render_svg(icon_svg, 256).save(os.path.join(HERE, base + "-icon-256.png"))
        items.append((n, slug, title, desc, lk, icon_svg, W))
    return items


def font(sz, bold=False):
    return ImageFont.truetype(r"C:\Windows\Fonts\arialbd.ttf" if bold else r"C:\Windows\Fonts\arial.ttf", sz)


def sheet(items):
    RH, PADY = 340, 14
    SW = 2000
    img = Image.new("RGBA", (SW, RH * len(items) + 70), (40, 40, 48, 255))
    d = ImageDraw.Draw(img)
    d.text((20, 18), "RetroHub logo concepts - phase 1 (large lockup | on light | icon 256/64/32/16 on dark + light, with 6x zoom | app sidebar at 1x: expanded + collapsed)",
           font=font(20, True), fill=(235, 235, 245))
    for i, (n, slug, title, desc, lk, icon_svg, W) in enumerate(items):
        y0 = 60 + i * RH
        # A: large lockup on dark
        pa = (20, y0, 760, y0 + RH - PADY)
        d.rounded_rectangle(pa, 14, fill=DARK + (255,))
        L = render_svg(lk, 700)
        img.alpha_composite(L, (pa[0] + 20, pa[1] + 22))
        d.text((pa[0] + 16, pa[1] + 6), f"{n}. {title}", font=font(17, True), fill=(210, 210, 225))
        d.text((pa[0] + 16, pa[3] - 60), desc[:96], font=font(13), fill=(150, 150, 170))
        d.text((pa[0] + 16, pa[3] - 40), desc[96:], font=font(13), fill=(150, 150, 170))
        # B: lockup on light
        pb = (770, y0, 1130, y0 + RH - PADY)
        d.rounded_rectangle(pb, 14, fill=LIGHT + (255,))
        L2 = render_svg(lk, 330)
        img.alpha_composite(L2, (pb[0] + 15, pb[1] + 30))
        L3 = render_svg(lk, 190)
        img.alpha_composite(L3, (pb[0] + 15, pb[1] + 130))
        # C: icons
        pc = (1140, y0, 1560, y0 + RH - PADY)
        d.rectangle(pc, fill=DARK + (255,))
        d.rectangle((pc[0] + 210, pc[1], pc[2], pc[3]), fill=LIGHT + (255,))
        d.rounded_rectangle((pc[0] + 0, pc[1], pc[2], pc[3]), 14, outline=(60, 60, 72), width=1)
        i256 = render_svg(icon_svg, 150)
        img.alpha_composite(i256, (pc[0] + 10, pc[1] + 18))
        d.text((pc[0] + 12, pc[1] + 4), "256 (shown at 150)", font=font(11), fill=(150, 150, 170))
        for bi, (bx, bgc) in enumerate(((pc[0] + 10, DARK), (pc[0] + 218, LIGHT))):
            by = pc[1] + 176 if bi == 0 else pc[1] + 18
            x = bx
            for sz in (64, 32, 16):
                img.alpha_composite(render_svg(icon_svg, sz), (x, by))
                x += sz + 14
            zy = by + 76 if bi == 0 else by + 96
            img.alpha_composite(render_svg(icon_svg, 16).resize((80, 80), Image.NEAREST), (bx, zy))
            img.alpha_composite(render_svg(icon_svg, 32).resize((80, 80), Image.NEAREST), (bx + 96, zy))
        d.text((pc[0] + 222, pc[1] + 4), "64 | 32 | 16, then 16 at 5x and 32 at 2.5x", font=font(11), fill=(90, 90, 110))
        # D: sidebar mock at 1x (mark 40px, text area), expanded + collapsed
        pd = (1570, y0, SW - 20, y0 + RH - PADY)
        d.rounded_rectangle(pd, 14, fill=(30, 31, 40, 255))
        sbw = 240
        sb = Image.new("RGBA", (sbw, 230), DARK + (255,))
        sd = ImageDraw.Draw(sb)
        ms = 40
        mark = render_svg(icon_svg, ms)
        sb.alpha_composite(mark, (12, 24))
        # wordmark part of the lockup: crop the lockup at 40px mark height
        lk_h = render_svg(lk, int(W * ms / 256))
        word = lk_h.crop((ms + 4, 0, lk_h.width, lk_h.height))
        sb.alpha_composite(word, (12 + ms + 4, 24))
        for k, lab in enumerate(["Library", "Play Online", "Saves", "Controllers", "Settings"]):
            yy = 84 + k * 28
            if k == 0:
                sd.rounded_rectangle((8, yy - 4, sbw - 8, yy + 20), 8, fill=(40, 42, 62))
            sd.ellipse((16, yy + 2, 28, yy + 14), fill=(110, 112, 140))
            sd.text((38, yy + 1), lab, font=font(13), fill=(200, 202, 220))
        img.alpha_composite(sb, (pd[0] + 14, pd[1] + 14))
        col = Image.new("RGBA", (72, 230), DARK + (255,))
        col.alpha_composite(render_svg(icon_svg, 40), (16, 24))
        cd = ImageDraw.Draw(col)
        for k in range(5):
            cd.ellipse((26, 88 + k * 28, 38, 100 + k * 28), fill=(110, 112, 140))
        img.alpha_composite(col, (pd[0] + 14 + sbw + 14, pd[1] + 14))
        # 2x zoom of sidebar header
        hdr = sb.crop((0, 0, sbw, 76)).resize((sbw * 2 - 0, 152), Image.LANCZOS)
        img.alpha_composite(hdr, (pd[0] + 14, pd[1] + 14 + 236)) if False else None
        d.text((pd[0] + 14, pd[3] - 22), "real size (mark 40px)  |  collapsed", font=font(11), fill=(150, 150, 170))
    img.convert("RGB").save(os.path.join(HERE, "comparison-sheet.png"))


if __name__ == "__main__":
    items = build()
    sheet(items)
    print("ok")
