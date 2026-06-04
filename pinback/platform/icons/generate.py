#!/usr/bin/env python3
"""Generate every platform's icon assets from the brand source art.

Source of truth: platform/icons/src/{Default,Dark,TintedDark}-1024.png
(exported from Apple Icon Composer — full-bleed rounded badges, transparent corners).

Outputs, wired into each project:
  iOS      Assets.xcassets/AppIcon.appiconset  (Any/Dark/Tinted appearances, 1024)
  macOS    Resources/AppIcon.icns
  Windows  app.ico  (16..256)
  Linux    icons/hicolor/<size>/apps/dev.pinback.shell.png + .desktop
  Android  mipmap-* legacy + round + adaptive foreground + monochrome + adaptive XML + bg color

Run:  python platform/icons/generate.py
"""
import os, json
from PIL import Image, ImageDraw, ImageFilter

HERE = os.path.dirname(os.path.abspath(__file__))
PLAT = os.path.dirname(HERE)
SRC  = os.path.join(HERE, "src")

default = Image.open(os.path.join(SRC, "Default-1024.png")).convert("RGBA")
dark    = Image.open(os.path.join(SRC, "Dark-1024.png")).convert("RGBA")
tinted  = Image.open(os.path.join(SRC, "TintedDark-1024.png")).convert("RGBA")

def ensure(d): os.makedirs(d, exist_ok=True)

def resized(img, n): return img.resize((n, n), Image.LANCZOS)

def circle_mask(n):
    m = Image.new("L", (n, n), 0)
    ImageDraw.Draw(m).ellipse((0, 0, n - 1, n - 1), fill=255)
    return m

def on_bg(badge, n, bg, scale=1.0):
    """Opaque square canvas of color `bg` with `badge` scaled by `scale`, centered."""
    canvas = Image.new("RGBA", (n, n), bg + (255,))
    s = int(n * scale)
    b = badge.resize((s, s), Image.LANCZOS)
    canvas.alpha_composite(b, ((n - s) // 2, (n - s) // 2))
    return canvas

def transparent_centered(badge, n, scale):
    """Transparent square canvas with `badge` scaled by `scale`, centered."""
    canvas = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    s = int(n * scale)
    b = badge.resize((s, s), Image.LANCZOS)
    canvas.alpha_composite(b, ((n - s) // 2, (n - s) // 2))
    return canvas

# --- sample the badge's dark background colour (for Android layers) ----------
px = default.load()
acc = [0, 0, 0]; cnt = 0
for y in range(0, 1024, 5):
    for x in range(0, 1024, 5):
        r, g, b, a = px[x, y]
        if a > 200 and (0.299 * r + 0.587 * g + 0.114 * b) < 70:
            acc[0] += r; acc[1] += g; acc[2] += b; cnt += 1
NAVY = tuple(c // cnt for c in acc) if cnt else (10, 18, 40)
NAVY_HEX = "#%02X%02X%02X" % NAVY
print("sampled background navy:", NAVY_HEX)

# --- monochrome silhouette: solid sun keyed off colour-distance from the navy
#     background (metallic shading defeats a plain luminance threshold) --------
def monochrome_1024():
    d = default.load()
    mask = Image.new("L", (1024, 1024), 0)
    m = mask.load()
    nr, ng, nb = NAVY
    for y in range(1024):
        for x in range(1024):
            r, g, b, a = d[x, y]
            if a < 40:
                continue
            if ((r - nr) ** 2 + (g - ng) ** 2 + (b - nb) ** 2) ** 0.5 > 72:
                m[x, y] = 255
    # morphological close to fill the shaded gaps, then soften the edge
    mask = mask.filter(ImageFilter.MaxFilter(7)).filter(ImageFilter.MinFilter(5))
    mask = mask.filter(ImageFilter.GaussianBlur(1.2))
    out = Image.new("RGBA", (1024, 1024), (255, 255, 255, 0))
    out.putalpha(mask)
    return out

MONO = monochrome_1024()

# ============================ iOS ===========================================
def gen_ios():
    base = os.path.join(PLAT, "ios", "Pinback", "Assets.xcassets")
    appicon = os.path.join(base, "AppIcon.appiconset")
    ensure(appicon)
    json.dump({"info": {"author": "xcode", "version": 1}},
              open(os.path.join(base, "Contents.json"), "w"), indent=2)
    default.save(os.path.join(appicon, "icon-default-1024.png"))
    dark.save(os.path.join(appicon, "icon-dark-1024.png"))
    tinted.save(os.path.join(appicon, "icon-tinted-1024.png"))
    contents = {"images": [
        {"filename": "icon-default-1024.png", "idiom": "universal", "platform": "ios", "size": "1024x1024"},
        {"appearances": [{"appearance": "luminosity", "value": "dark"}],
         "filename": "icon-dark-1024.png", "idiom": "universal", "platform": "ios", "size": "1024x1024"},
        {"appearances": [{"appearance": "luminosity", "value": "tinted"}],
         "filename": "icon-tinted-1024.png", "idiom": "universal", "platform": "ios", "size": "1024x1024"},
    ], "info": {"author": "xcode", "version": 1}}
    json.dump(contents, open(os.path.join(appicon, "Contents.json"), "w"), indent=2)
    print("iOS: AppIcon.appiconset (Any/Dark/Tinted)")

# ============================ macOS =========================================
def gen_macos():
    out = os.path.join(PLAT, "macos", "Resources")
    ensure(out)
    # macOS icons sit on a ~824/1024 grid with transparent padding (Big Sur style).
    canvas = Image.new("RGBA", (1024, 1024), (0, 0, 0, 0))
    s = 824
    canvas.alpha_composite(default.resize((s, s), Image.LANCZOS), ((1024 - s) // 2, (1024 - s) // 2))
    canvas.save(os.path.join(out, "AppIcon.icns"),
                sizes=[(16, 16), (32, 32), (64, 64), (128, 128), (256, 256), (512, 512), (1024, 1024)])
    print("macOS: AppIcon.icns (padded squircle)")

# ============================ Windows =======================================
def gen_windows():
    out = os.path.join(PLAT, "windows")
    default.save(os.path.join(out, "app.ico"),
                 sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)])
    print("Windows: app.ico (16..256)")

# ============================ Linux =========================================
def gen_linux():
    root = os.path.join(PLAT, "linux", "icons", "hicolor")
    for n in (16, 32, 48, 64, 128, 256, 512):
        d = os.path.join(root, f"{n}x{n}", "apps")
        ensure(d)
        resized(default, n).save(os.path.join(d, "dev.pinback.shell.png"))
    print("Linux: hicolor PNGs 16..512")

# ============================ Android =======================================
def gen_android():
    res = os.path.join(PLAT, "android", "app", "src", "main", "res")
    legacy = {"mdpi": 48, "hdpi": 72, "xhdpi": 96, "xxhdpi": 144, "xxxhdpi": 192}
    adaptive = {"mdpi": 108, "hdpi": 162, "xhdpi": 216, "xxhdpi": 324, "xxxhdpi": 432}

    for dens, n in legacy.items():
        d = os.path.join(res, f"mipmap-{dens}"); ensure(d)
        sq = on_bg(default, n, NAVY, scale=0.92)
        sq.save(os.path.join(d, "ic_launcher.png"))
        rnd = sq.copy(); rnd.putalpha(circle_mask(n))
        rnd.save(os.path.join(d, "ic_launcher_round.png"))

    for dens, n in adaptive.items():
        d = os.path.join(res, f"mipmap-{dens}"); ensure(d)
        # foreground/monochrome keep the sun inside the ~66% safe zone
        transparent_centered(default, n, 0.80).save(os.path.join(d, "ic_launcher_foreground.png"))
        mono = Image.new("RGBA", (n, n), (0, 0, 0, 0))
        s = int(n * 0.80)
        mono.alpha_composite(MONO.resize((s, s), Image.LANCZOS), ((n - s) // 2, (n - s) // 2))
        mono.save(os.path.join(d, "ic_launcher_monochrome.png"))

    anydpi = os.path.join(res, "mipmap-anydpi-v26"); ensure(anydpi)
    adaptive_xml = (
        '<?xml version="1.0" encoding="utf-8"?>\n'
        '<adaptive-icon xmlns:android="http://schemas.android.com/apk/res/android">\n'
        '    <background android:drawable="@color/ic_launcher_background" />\n'
        '    <foreground android:drawable="@mipmap/ic_launcher_foreground" />\n'
        '    <monochrome android:drawable="@mipmap/ic_launcher_monochrome" />\n'
        '</adaptive-icon>\n'
    )
    open(os.path.join(anydpi, "ic_launcher.xml"), "w").write(adaptive_xml)
    open(os.path.join(anydpi, "ic_launcher_round.xml"), "w").write(adaptive_xml)

    values = os.path.join(res, "values"); ensure(values)
    open(os.path.join(values, "ic_launcher_background.xml"), "w").write(
        '<?xml version="1.0" encoding="utf-8"?>\n<resources>\n'
        f'    <color name="ic_launcher_background">{NAVY_HEX}</color>\n</resources>\n')
    print("Android: mipmaps (legacy+round+foreground+monochrome) + adaptive XML + bg color")

if __name__ == "__main__":
    gen_ios(); gen_macos(); gen_windows(); gen_linux(); gen_android()
    print("done.")
