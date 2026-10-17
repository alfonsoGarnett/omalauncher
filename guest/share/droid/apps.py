"""Build ~/.cache/droid/apps.json and a neon pixel-art icon per app.

The icon layers come from the launcher's export (android-apps.json lists each app's icon.png and its adaptive
fg/bg/mono layers); for an app the launcher has no icon for, they're pulled over adb (through `droid`) when it's
reachable. Every app's layers are cached, so `droid-apps --rerender` redraws all icons without either.
Run as `droid-apps` (new and updated apps), `droid-apps --force`, `droid-apps --rerender` or `droid-apps <pkg>...`.
Needs the packages in requirements.txt, in ~/.local/share/droid/venv. Optional: names.json next to this file maps
packages to display names.
"""
import colorsys, gc, io, json, os, re, subprocess, sys
from loguru import logger; logger.remove()
from androguard.core.axml import ARSCParser, AXMLPrinter
from PIL import Image
import cairosvg

# The device `droid` connected to (droid-apps passes it in); its port changes when
# wireless debugging restarts, so it's never hard-coded.
DEV = os.environ.get("DROID_DEV") or (subprocess.run(["droid"], capture_output=True, text=True).stdout.split() or ["127.0.0.1:5555"])[0]
OUT = os.path.expanduser("~/.cache/droid")
ICONS = os.path.join(OUT, "icons")
NS = "{http://schemas.android.com/apk/res/android}"
AAPT = "{http://schemas.android.com/aapt}"
S = 432          # layer render size: 108dp at 4x
ART = int(os.environ.get("DROID_ART", 20))   # glyph size in art pixels (20 keeps faces and knots; 16 is chunkier)
TILE = ART + 4   # tile size in art pixels (glyph + border)
SCALE = 120 // TILE   # output pixel size: 120px PNGs
LAYERS = os.path.join(OUT, "layers")
LAYER_NAMES = ("bg", "fg", "mono", "legacy")

# Theme color names by nominal hue: the widget colors the layers with these.
THEME_HUES = {"red": 355, "orange": 25, "yellow": 45, "green": 100, "cyan": 190, "blue": 222, "magenta": 275}
# Tokyo Night values of the same names, for the pre-colored icon.
TOKYO = {"red": (0xf7, 0x76, 0x8e), "orange": (0xff, 0x9e, 0x64), "yellow": (0xe0, 0xaf, 0x68), "green": (0x9e, 0xce, 0x6a),
         "cyan": (0x7d, 0xcf, 0xff), "blue": (0x7a, 0xa2, 0xf7), "magenta": (0xbb, 0x9a, 0xf7), "foreground": (0xc0, 0xca, 0xf5)}


def A(el, name):
    """An android: attribute, also when a shrunk APK stripped its namespace."""
    if el is None: return None
    v = el.get(NS + name)
    return v if v is not None else el.get(name)


def sh(cmd):
    return subprocess.run(["adb", "-s", DEV, "exec-out", cmd], capture_output=True).stdout


class App:
    def __init__(self, pkg):
        self.pkg = pkg
        paths = [l.replace("package:", "").strip() for l in sh(f"pm path --user 0 {pkg}").decode().splitlines() if l.strip()]
        self.apk = next((x for x in paths if x.endswith("/base.apk")), paths[0])
        self.splits = [x for x in paths if x != self.apk]
        self.arsc = ARSCParser(sh(f"unzip -p '{self.apk}' resources.arsc"))
        self._split_arscs = None
        self.manifest = AXMLPrinter(sh(f"unzip -p '{self.apk}' AndroidManifest.xml")).get_xml_obj()

    def file(self, path):
        for apk in [self.apk] + self.splits:
            data = sh(f"unzip -p '{apk}' '{path}'")
            if data:
                return data
        return b""

    def split_arscs(self):
        if self._split_arscs is None:
            self._split_arscs = []
            for apk in self.splits:
                data = sh(f"unzip -p '{apk}' resources.arsc")
                if data:
                    try: self._split_arscs.append(ARSCParser(data))
                    except Exception: pass
        return self._split_arscs

    def values(self, ref):
        """All (config, value) pairs for a @ref."""
        if not ref or not ref.startswith("@"):
            return []
        try:
            rid = int(ref[1:].split(":")[-1], 16)
        except ValueError:
            return []
        out = []
        for arsc in [self.arsc] + (self.split_arscs() if self.splits else []):
            try:
                out += [(c, v) for c, v in arsc.get_resolved_res_configs(rid) if v]
            except Exception:
                pass
            if out and arsc is self.arsc:
                # Density rasters often live in config splits; keep looking only if base has no raster.
                if any(not v.endswith(".xml") for c, v in out) or not self.splits:
                    break
        return out

    def best(self, ref):
        """The value to use: English/default config first, the anydpi xml or the densest raster."""
        vals = self.values(ref)
        if not vals:
            return None
        def score(cv):
            c, v = cv
            q = (c.get_qualifier() if c else "") or ""
            s = 0
            if "anydpi" in q: s += 100
            for i, d in enumerate(["ldpi", "mdpi", "hdpi", "xhdpi", "xxhdpi", "xxxhdpi"]):
                if re.search(rf"(^|-){d}(-|$)", q): s = max(s, 10 + i)
            if re.match(r"^[a-z]{2}(-|$)", q) and not q.startswith("en"): s -= 1000
            return s
        return max(vals, key=score)[1]

    def label(self, act):
        def lab(el):
            v = A(el, "label") if el is not None else None
            if not v: return None
            if not v.startswith("@"): return v
            vals = self.values(v)
            for c, s in vals:
                lang = c.get_language() if c else ""
                if lang in ("en", "\x00\x00", ""): return s
            return vals[0][1] if vals else None
        el = self.activity(act)
        return lab(el) or lab(self.manifest.find("application"))

    def activity(self, act):
        for tag in ("activity", "activity-alias"):
            for el in self.manifest.iter(tag):
                n = A(el, "name") or ""
                if (self.pkg + n if n.startswith(".") else n) == act:
                    return el
        return None

    def icon_ref(self, act):
        el = self.activity(act)
        return (A(el, "icon") if el is not None else None) or A(self.manifest.find("application"), "icon") or A(self.manifest.find("application"), "roundIcon")

    # ---- drawables -> RGBA images at S x S ----

    def color(self, v, depth=0):
        if not v: return None
        if v.startswith("@") and depth < 5:
            return self.color(self.best(v), depth + 1)
        if v.endswith(".xml") and depth < 5:
            try:
                x = AXMLPrinter(self.file(v)).get_xml_obj()
            except Exception:
                return None
            if x.tag == "gradient":
                c = A(x, "startColor") or (A(x[0], "color") if len(x) else None)
                return self.color(c, depth + 1)
            if x.tag == "selector" and len(x):
                return self.color(A(x[-1], "color"), depth + 1)
            return None
        m = re.fullmatch(r"#([0-9a-fA-F]{3,8})", v.strip())
        if not m: return None
        h = m.group(1)
        if len(h) in (3, 4): h = "".join(ch * 2 for ch in h)
        if len(h) == 6: h = "ff" + h
        return (int(h[2:4], 16), int(h[4:6], 16), int(h[6:8], 16), int(h[0:2], 16))

    def render(self, ref, depth=0):
        if depth > 6 or not ref:
            return None
        v = ref if not ref.startswith("@") else self.best(ref)
        if not v: return None
        c = self.color(v)
        if c: return Image.new("RGBA", (S, S), c)
        if v.startswith("res/") and not v.endswith(".xml"):
            try:
                return Image.open(io.BytesIO(self.file(v))).convert("RGBA").resize((S, S), Image.LANCZOS)
            except Exception:
                return None
        if v.endswith(".xml"):
            try:
                x = AXMLPrinter(self.file(v)).get_xml_obj()
            except Exception:
                return None
            return self.render_xml(x, depth)
        return None

    def render_xml(self, x, depth):
        tag = x.tag
        if tag == "vector":
            return self.vector(x)
        if tag == "inset":
            img = self.render(A(x, "drawable"), depth + 1) or (self.render_xml(x[0], depth + 1) if len(x) else None)
            if img is None: return None
            ins = A(x, "inset") or "0"
            f = float(ins.rstrip("%")) / 100 if ins.endswith("%") else 0
            if f <= 0 or f >= 0.5: return img
            inner = int(S * (1 - 2 * f))
            out = Image.new("RGBA", (S, S)); out.paste(img.resize((inner, inner), Image.LANCZOS), ((S - inner) // 2,) * 2)
            return out
        if tag in ("bitmap", "nine-patch"):
            return self.render(A(x, "src"), depth + 1)
        if tag == "layer-list":
            out = Image.new("RGBA", (S, S))
            for item in x:
                img = self.render(A(item, "drawable"), depth + 1) or (self.render_xml(item[0], depth + 1) if len(item) else None)
                if img is not None: out.alpha_composite(img)
            return out
        if tag == "shape":
            for ch in x:
                if ch.tag == "solid":
                    c = self.color(A(ch, "color"))
                    if c: return Image.new("RGBA", (S, S), c)
                if ch.tag == "gradient":
                    c = self.color(A(ch, "startColor"))
                    if c: return Image.new("RGBA", (S, S), c)
            return None
        if tag == "color":
            c = self.color(A(x, "color"))
            return Image.new("RGBA", (S, S), c) if c else None
        if tag == "selector" and len(x):
            return self.render(A(x[0], "drawable"), depth + 1)
        return None

    def vector(self, x):
        vw = float(A(x, "viewportWidth") or 108); vh = float(A(x, "viewportHeight") or 108)
        defs = []
        def css(c, alpha=1.0):
            if not c: return "none", 0
            return f"rgb({c[0]},{c[1]},{c[2]})", c[3] / 255 * alpha
        def paint(el, attr):
            v = A(el, attr)
            if v is None:
                for a in el:
                    if a.tag == AAPT + "attr" and a.get("name") == "android:" + attr and len(a):
                        g = a[0]
                        v = A(g, "startColor") or (A(g[0], "color") if len(g) else None)
            return self.color(v)
        def walk(el, n=[0]):
            parts = []; i = 0; kids = list(el)
            while i < len(kids):
                k = kids[i]
                if k.tag == "group":
                    px, py = float(A(k, "pivotX") or 0), float(A(k, "pivotY") or 0)
                    t = (f"translate({float(A(k, 'translateX') or 0) + px},{float(A(k, 'translateY') or 0) + py}) "
                         f"rotate({float(A(k, 'rotation') or 0)}) scale({float(A(k, 'scaleX') or 1)},{float(A(k, 'scaleY') or 1)}) "
                         f"translate({-px},{-py})")
                    parts.append(f'<g transform="{t}">{walk(k)}</g>')
                elif k.tag == "clip-path":
                    n[0] += 1; cid = f"c{n[0]}"
                    defs.append(f'<clipPath id="{cid}"><path d="{A(k, "pathData")}"/></clipPath>')
                    inner = "".join(self_path(kk) if kk.tag == "path" else "" for kk in kids[i + 1:])
                    grp = "".join(f'<g>{walk(kk)}</g>' for kk in kids[i + 1:] if kk.tag == "group")
                    parts.append(f'<g clip-path="url(#{cid})">{inner}{grp}</g>')
                    break
                elif k.tag == "path":
                    parts.append(self_path(k))
                i += 1
            return "".join(parts)
        def self_path(k):
            d = A(k, "pathData")
            if not d: return ""
            fc, fo = css(paint(k, "fillColor"), float(A(k, "fillAlpha") or 1))
            sc, so = css(paint(k, "strokeColor"), float(A(k, "strokeAlpha") or 1))
            sw = A(k, "strokeWidth") or "0"
            rule = "evenodd" if (A(k, "fillType") or "").lower() in ("evenodd", "1") else "nonzero"
            return (f'<path d="{d}" fill="{fc}" fill-opacity="{fo}" fill-rule="{rule}" stroke="{sc}" '
                    f'stroke-opacity="{so}" stroke-width="{sw}" stroke-linecap="round" stroke-linejoin="round"/>')
        body = walk(x)
        svg = (f'<svg xmlns="http://www.w3.org/2000/svg" width="{S}" height="{S}" viewBox="0 0 {vw} {vh}">'
               f'<defs>{"".join(defs)}</defs>{body}</svg>')
        try:
            return Image.open(io.BytesIO(cairosvg.svg2png(bytestring=svg.encode()))).convert("RGBA").resize((S, S))
        except Exception:
            return None

    def layers(self, act):
        """(background, foreground, monochrome, legacy) images; any may be None."""
        ref = self.icon_ref(act)
        bg = fg = mono = None
        for c, v in self.values(ref):
            if v.endswith(".xml"):
                try: x = AXMLPrinter(self.file(v)).get_xml_obj()
                except Exception: continue
                if x.tag == "adaptive-icon":
                    for ch in x:
                        img = self.render(A(ch, "drawable"), 1) or (self.render_xml(ch[0], 1) if len(ch) else None)
                        if ch.tag == "background": bg = img
                        elif ch.tag == "foreground": fg = img
                        elif ch.tag == "monochrome": mono = img
                    break
        legacy = None
        rasters = [v for c, v in self.values(ref) if v.startswith("res/") and not v.endswith(".xml")]
        if rasters:
            b = self.best(ref) or ""
            legacy = self.render(b if not b.endswith(".xml") else rasters[-1])
        if legacy is None and bg is None and fg is None:
            legacy = self.render(ref)
        return bg, fg, mono, legacy



# ---- pixel art ----
#
# Each icon comes out as two white layers that the widget colors with the current theme:
# `main` (outline, dim fill, HUD corners) and `detail` (inner lines where the art's colors
# change), plus the theme color names for each (`tint`, `second`). `icon` is the same art
# pre-colored in Tokyo Night, for anything that can't tint.

def hsv(c): return colorsys.rgb_to_hsv(*[v / 255 for v in c[:3]])

def theme_name(c):
    """The theme color name nearest a saturated color's hue."""
    h = hsv(c)[0] * 360
    return min(THEME_HUES, key=lambda n: min(abs(h - THEME_HUES[n]), 360 - abs(h - THEME_HUES[n])))

def cls(p):
    """A pixel's color class: a theme color name, or light / grey / dark for greys."""
    h, s, v = hsv(p)
    if s < 0.25 or v < 0.2:
        return "light" if v > 0.7 else "dark" if v < 0.3 else "grey"
    return theme_name(p)

def brand(img):
    """The image's most common saturated color, as a theme name; None if it's grey."""
    if img is None: return None
    small = img.resize((24, 24), Image.BOX)
    counts = {}
    for p in (small.get_flattened_data() if hasattr(small, "get_flattened_data") else small.getdata()):
        if p[3] < 128: continue
        c = cls(p)
        if c in THEME_HUES: counts[c] = counts.get(c, 0) + 1
    return max(counts, key=counts.get) if counts else None

def near(a, b, t): return abs(a[0] - b[0]) + abs(a[1] - b[1]) + abs(a[2] - b[2]) < t

def drop_background(px, W):
    """For a full-bleed glyph (one that carries its own background), clear the background:
    only the region connected to the glyph's edge in the edge's own color, so a logo in a
    similar color inside it survives. Returns the opaque set, unchanged if clearing would
    leave almost nothing (the shape then stays as a framed badge)."""
    solid = {(x, y) for y in range(W) for x in range(W) if px[x, y][3] >= 128}
    if len(solid) < W * W * 0.6:
        return solid
    edge = [(x, y) for (x, y) in solid if x in (0, W - 1) or y in (0, W - 1)
            or any(n not in solid for n in ((x + 1, y), (x - 1, y), (x, y + 1), (x, y - 1)))]
    if not edge:
        return solid
    q = lambda p: (p[0] >> 4, p[1] >> 4, p[2] >> 4)
    counts = {}
    for e in edge: counts[q(px[e])] = counts.get(q(px[e]), 0) + 1
    top = max(counts, key=counts.get)
    base = next(px[e] for e in edge if q(px[e]) == top)
    seen = {e for e in edge if near(px[e], base, 90)}
    stack = list(seen)
    while stack:
        x, y = stack.pop()
        for n in ((x + 1, y), (x - 1, y), (x, y + 1), (x, y - 1)):
            # Step to neighbours of a similar color that stay near the base: follows gentle
            # gradients without leaking into the logo.
            if n in solid and n not in seen and near(px[n], px[x, y], 48) and near(px[n], base, 160):
                seen.add(n); stack.append(n)
    rest = solid - seen
    return rest if len(rest) >= W * W * 0.04 else solid

def cleanup(q):
    """Pixel-art cleanup on {cell: class}: drop stray single pixels and remove the elbow
    pixel of staircase lines so diagonals read as clean steps. (Holes are left alone: a
    one-pixel hole is as often an eye or a gap between petals as noise.)"""
    N4 = lambda x, y: ((x + 1, y), (x - 1, y), (x, y + 1), (x, y - 1))
    N8 = lambda x, y: N4(x, y) + ((x + 1, y + 1), (x - 1, y - 1), (x + 1, y - 1), (x - 1, y + 1))
    q = {k: v for k, v in q.items() if any(n in q for n in N8(*k))}
    for k in sorted(q):
        x, y = k
        n4 = [n for n in N4(x, y) if n in q]
        n8 = [n for n in N8(x, y) if n in q]
        if len(n4) == 2 and len(n8) == 2 and n4[0][0] != n4[1][0] and n4[0][1] != n4[1][1]:
            del q[k]
    return q

def pixelate(glyph, tint, mode):
    """glyph is RGBA S x S on transparent; mode 'mono' is a one-color silhouette, 'color'
    keeps the art's own colors as inner detail. Returns (main, detail, second) or None."""
    crop = int(S * 18 / 108)                        # keep the 72dp safe zone
    W = 144                                         # working resolution
    g = glyph.crop((crop, crop, S - crop, S - crop)).resize((W, W), Image.BOX)
    px = g.load()
    keep = drop_background(px, W)
    # Each art pixel takes the majority color of the opaque pixels in its cell, and is kept
    # when they cover enough of it: majority, not an average, so thin strokes keep their
    # color instead of blurring into the background. Line art keeps cells a third covered;
    # dense shapes need half, so the holes cut into them (Spotify's bars) stay open.
    c = W / ART
    need = 0.5 if len(keep) > W * W * 0.3 else 0.35
    q = {}
    for y in range(ART):
        for x in range(ART):
            cells = [(i, j) for j in range(int(y * c), int((y + 1) * c)) for i in range(int(x * c), int((x + 1) * c))]
            mine = [px[k] for k in cells if k in keep]
            if len(mine) >= len(cells) * need:
                if mode == "mono":
                    q[(x, y)] = tint
                else:
                    cl = [cls(p) for p in mine]
                    v = max(set(cl), key=cl.count)
                    q[(x, y)] = tint if v == "light" else v
    q = cleanup(q)
    if len(q) < ART * ART * 0.06:
        return None
    hues = [v for v in q.values() if v != tint and v in THEME_HUES]
    second = max(set(hues), key=hues.count) if hues else "foreground"
    main = Image.new("RGBA", (TILE, TILE), (0, 0, 0, 0))
    detail = Image.new("RGBA", (TILE, TILE), (0, 0, 0, 0))
    mp, dp = main.load(), detail.load()
    o = (TILE - ART) // 2
    for (x, y), v in q.items():
        nb = ((x + 1, y), (x - 1, y), (x, y + 1), (x, y - 1))
        if any(n not in q for n in nb):
            mp[x + o, y + o] = (255, 255, 255, 255)     # outline
        elif any(q[n] != v for n in nb):
            dp[x + o, y + o] = (255, 255, 255, 255)     # inner detail
        else:
            mp[x + o, y + o] = (255, 255, 255, 60)      # dim fill
    for cx, cy, dx, dy in [(0, 0, 1, 1), (TILE - 1, 0, -1, 1), (0, TILE - 1, 1, -1), (TILE - 1, TILE - 1, -1, -1)]:
        for i in range(3):                              # HUD corner brackets
            mp[cx + dx * i, cy] = (255, 255, 255, 150)
            mp[cx, cy + dy * i] = (255, 255, 255, 150)
    up = lambda im: im.resize((TILE * SCALE, TILE * SCALE), Image.NEAREST)
    return up(main), up(detail), second

def colored(main, detail, tint, second):
    """The two layers pre-colored in Tokyo Night."""
    out = Image.new("RGBA", main.size, (0, 0, 0, 0))
    for layer, name in ((main, tint), (detail, second)):
        solid = Image.new("RGBA", main.size, TOKYO[name] + (255,))
        solid.putalpha(layer.getchannel("A"))
        out = Image.alpha_composite(out, solid)
    return out

def coverage(img):
    """How much of the safe zone an image's opaque pixels fill, 0..1."""
    crop = int(S * 18 / 108)
    a = img.crop((crop, crop, S - crop, S - crop)).resize((72, 72), Image.BOX).getchannel("A")
    data = a.get_flattened_data() if hasattr(a, "get_flattened_data") else a.getdata()
    return sum(1 for v in data if v >= 128) / (72 * 72)

def neon_icon(bg, fg, mono, legacy):
    """(main, detail, tint, second) for an app's icon layers, or None."""
    tint = brand(bg) or brand(fg) or brand(legacy) or "cyan"
    if mono is not None and coverage(mono) < 0.55:
        # Some apps ship a monochrome layer that's a filled block (Walmart): skip those.
        art = pixelate(mono, tint, "mono")
    elif fg is not None:
        art = pixelate(fg, tint, "color")
    elif legacy is not None:
        # Legacy art has no safe-zone padding: scale it into the safe zone.
        out = Image.new("RGBA", (S, S)); inner = int(S * 72 / 108)
        out.paste(legacy.convert("RGBA").resize((inner, inner), Image.LANCZOS), ((S - inner) // 2,) * 2)
        art = pixelate(out, tint, "color")
    else:
        return None
    return (art[0], art[1], tint, art[2]) if art else None

def letter_icon(name):
    """Fallback: the app's first letter, as a neon pixel glyph."""
    from PIL import ImageDraw, ImageFont
    img = Image.new("RGBA", (S, S))
    d = ImageDraw.Draw(img)
    font = ImageFont.load_default(size=int(S * 0.5))
    ch = (name or "?")[:1].upper()
    box = d.textbbox((0, 0), ch, font=font)
    d.text(((S - (box[2] - box[0])) / 2 - box[0], (S - (box[3] - box[1])) / 2 - box[1]), ch, font=font, fill=(255, 255, 255, 255))
    tint = list(THEME_HUES)[sum(map(ord, ch)) % len(THEME_HUES)]
    art = pixelate(img, tint, "mono")
    return (art[0], art[1], tint, art[2]) if art else None


# ---- layer cache: the icon layers pulled over adb, so re-rendering needs no adb ----

def save_layers(pkg, layers):
    d = os.path.join(LAYERS, pkg)
    os.makedirs(d, exist_ok=True)
    for name, img in zip(LAYER_NAMES, layers):
        f = os.path.join(d, name + ".png")
        if img is not None: img.save(f)
        elif os.path.exists(f): os.remove(f)
    open(os.path.join(d, "done"), "w").close()

def load_layers(pkg):
    d = os.path.join(LAYERS, pkg)
    if not os.path.exists(os.path.join(d, "done")): return None
    return tuple(Image.open(os.path.join(d, n + ".png")).convert("RGBA") if os.path.exists(os.path.join(d, n + ".png")) else None for n in LAYER_NAMES)

def exported_layers(item):
    """The layers the launcher exports with its app list (no adb), or None if it has none:
    an adaptive icon's fg/bg/mono, or a plain icon as the legacy layer."""
    files = item.get("layers") or {}
    load = lambda f: Image.open(f).convert("RGBA") if f and os.path.exists(f) else None
    if files:
        return load(files.get("bg")), load(files.get("fg")), load(files.get("mono")), None
    if item.get("icon"):
        return None, None, None, load(item["icon"])
    return None

def exported_stamp(item):
    """What the launcher drew the icon from (its APK path), so an app update redraws the art."""
    f = item.get("icon")
    s = os.path.join(os.path.dirname(f), "stamp") if f else None
    return open(s).read() if s and os.path.exists(s) else None

def get_layers(pkg, act, fetch):
    """The cached layers, or pulled over adb (and cached) when fetch is set."""
    cached = load_layers(pkg)
    if cached is not None and not fetch: return cached, None
    app = App(pkg)
    layers = app.layers(act)
    save_layers(pkg, layers)
    return layers, app

def write_png(img, pkg, kind):
    """Save with a content hash in the name, so a changed icon is a new path to image caches."""
    import hashlib, glob
    buf = io.BytesIO(); img.save(buf, "PNG"); data = buf.getvalue()
    suffix = "" if kind == "icon" else "." + kind
    f = os.path.join(ICONS, f"{pkg}{suffix}-{hashlib.md5(data).hexdigest()[:8]}.png")
    for old in glob.glob(os.path.join(ICONS, f"{pkg}{suffix}-*.png")):
        if old != f: os.remove(old)
    open(f, "wb").write(data)
    return f


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("-")]
    force = "--force" in sys.argv           # pull every app's layers over adb again
    rerender = "--rerender" in sys.argv     # redraw every icon from the cached layers, no adb
    os.makedirs(ICONS, exist_ok=True)
    apps_json = "/run/user/0/android-apps.json"
    if not os.path.exists(apps_json):
        apps_json = os.path.expanduser("~/.local/share/omarchy/android-apps.json")
    acts = {}
    names = {}
    items = {}       # the launcher's entry per package, with its exported icon files
    if os.path.exists(apps_json):
        try:
            for item in json.load(open(apps_json)):
                p = item.get("package")
                # The owner's copy of an app; work or clone profile copies share its icon.
                if p and (item.get("profile") is None or p not in acts):
                    items[p] = item
                    acts[p] = item.get("activity") or ""
                    if item.get("name"):
                        names[p] = item["name"]
        except Exception as e:
            print(f"Error loading {apps_json}: {e}")
    if not acts:
        listing = sh("cmd package query-activities --brief --user 0 -a android.intent.action.MAIN -c android.intent.category.LAUNCHER").decode()
        for l in listing.splitlines():
            l = l.strip()
            if "/" not in l: continue
            p, a = l.split("/", 1)
            acts.setdefault(p, p + a if a.startswith(".") else a)
    if not acts:
        # The launcher hasn't written its app list yet (it's starting) and adb didn't list any either:
        # keep the icons we have rather than replacing them with an empty list.
        print("no app list yet (the launcher may still be starting); apps.json left as it was", flush=True)
        return
    npath = os.path.join(os.path.dirname(os.path.abspath(__file__)), "names.json")
    if os.path.exists(npath):
        names.update(json.load(open(npath)))
    old = {}
    path = os.path.join(OUT, "apps.json")
    if os.path.exists(path):
        old = {a["pkg"]: a for a in json.load(open(path))}

    def done(pkg):
        """Already drawn by this version: every file it lists is still there."""
        a = old.get(pkg) or {}
        return all(a.get(k) and os.path.exists(a[k]) for k in ("icon", "main", "detail"))

    def save(apps):
        apps = sorted(apps, key=lambda a: (a.get("name") or "").lower())
        with open(path + ".tmp", "w", encoding="utf-8") as f:
            json.dump(apps, f, ensure_ascii=False)
        os.replace(path + ".tmp", path)
        return apps

    apps = []
    adb = DEV != "none"      # droid-apps passes "none" when adb isn't reachable
    for i, (pkg, act) in enumerate(sorted(acts.items())):
        item = items.get(pkg) or {}
        stamp = exported_stamp(item)
        # The launcher drew the icon from a new APK since: the app was updated.
        updated = stamp is not None and (old.get(pkg) or {}).get("src") != stamp
        mine = not args or pkg in args
        if not mine or (done(pkg) and not updated and not force and not rerender and not args):
            if pkg in old: apps.append(old[pkg])
            continue
        # Where the layers come from: the launcher's export, else the cache, else adb.
        exported = bool(item.get("icon"))
        cached = load_layers(pkg) is not None
        if not exported and not (cached and not force) and (rerender or not adb):
            if pkg in old: apps.append(old[pkg])   # nothing to draw from without adb
            continue
        print(f"[{i + 1}/{len(acts)}] {pkg}", flush=True)
        entry = {"name": (old.get(pkg) or {}).get("name"), "pkg": pkg, "icon": None}
        try:
            app = None
            if exported:
                layers = exported_layers(item)
                entry["src"] = stamp
            else:
                layers, app = get_layers(pkg, act, force or not cached)
            if app is not None:
                entry["name"] = app.label(act) or entry["name"]
            art = neon_icon(*layers) or letter_icon(names.get(pkg) or entry["name"] or pkg)
            if art:
                main_img, detail_img, tint, second = art
                entry.update(icon=write_png(colored(main_img, detail_img, tint, second), pkg, "icon"),
                             main=write_png(main_img, pkg, "main"), detail=write_png(detail_img, pkg, "detail"),
                             tint=tint, second=second)
        except Exception as e:
            print(f"  {pkg}: {e}", flush=True)
            if pkg in old: entry = old[pkg]
        entry["name"] = names.get(pkg) or entry["name"] or pkg
        apps.append(entry)
        app = None; gc.collect()
        save(apps + [old[p] for p in old if p not in {a["pkg"] for a in apps} and p in acts])
    apps = save(apps)
    print(f"{len(apps)} apps, {sum(1 for a in apps if a.get('icon'))} icons -> {path}")

if __name__ == "__main__":
    main()
