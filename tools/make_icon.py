"""Draws windows/gui/vcam.ico: a lens seen head-on with a red tally light.

Run: python3 tools/make_icon.py  (needs Pillow)
"""
import os
from PIL import Image, ImageDraw, ImageFilter

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "..", "windows", "gui", "vcam.ico")
SIZES = [16, 20, 24, 32, 40, 48, 64, 96, 256]


def radial(size, center, radius, inner, outer):
    """A radial gradient disc image (RGBA) from inner to outer colour."""
    img = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    px = img.load()
    cx, cy = center
    for y in range(size):
        for x in range(size):
            d = ((x - cx) ** 2 + (y - cy) ** 2) ** 0.5 / radius
            if d <= 1.0:
                t = d ** 1.2
                px[x, y] = tuple(int(inner[i] + (outer[i] - inner[i]) * t) for i in range(3)) + (255,)
    return img


def draw(size):
    s = size * 4  # supersample
    img = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    c = s / 2
    small = size <= 24

    # Lens barrel: graphite with a lighter rim.
    r_body = s * 0.46
    body = radial(s, (c - s * 0.08, c - s * 0.1), r_body * 1.25, (92, 98, 110), (24, 26, 32))
    mask = Image.new("L", (s, s), 0)
    ImageDraw.Draw(mask).ellipse([c - r_body, c - r_body, c + r_body, c + r_body], fill=255)
    img.paste(body, (0, 0), mask)
    d = ImageDraw.Draw(img)
    rim = max(1, int(s * (0.035 if small else 0.018)))
    d.ellipse([c - r_body, c - r_body, c + r_body, c + r_body], outline=(150, 156, 168, 255), width=rim)

    # Mount ring.
    r_mount = s * 0.335
    d.ellipse([c - r_mount, c - r_mount, c + r_mount, c + r_mount], fill=(14, 15, 19, 255))
    if not small:
        d.ellipse([c - r_mount, c - r_mount, c + r_mount, c + r_mount], outline=(70, 74, 84, 255), width=max(1, int(s * 0.012)))

    # Glass: deep blue to violet.
    r_glass = s * (0.27 if not small else 0.29)
    glass = radial(s, (c + s * 0.05, c + s * 0.06), r_glass * 1.2, (88, 120, 255), (22, 18, 70))
    gmask = Image.new("L", (s, s), 0)
    ImageDraw.Draw(gmask).ellipse([c - r_glass, c - r_glass, c + r_glass, c + r_glass], fill=255)
    img.paste(glass, (0, 0), gmask)
    if not small:
        r_inner = r_glass * 0.55
        d.ellipse([c - r_inner, c - r_inner, c + r_inner, c + r_inner], outline=(140, 160, 255, 120), width=max(1, int(s * 0.008)))

    # Reflection: a soft crescent upper left.
    hl = Image.new("L", (s, s), 0)
    hd = ImageDraw.Draw(hl)
    hd.ellipse([c - r_glass * 0.78, c - r_glass * 0.82, c + r_glass * 0.1, c - r_glass * 0.05], fill=200)
    hd.ellipse([c - r_glass * 0.6, c - r_glass * 0.62, c + r_glass * 0.35, c + r_glass * 0.2], fill=0)
    hl = hl.filter(ImageFilter.GaussianBlur(s * 0.01))
    white = Image.new("RGBA", (s, s), (255, 255, 255, 255))
    img.paste(white, (0, 0), Image.composite(hl, Image.new("L", (s, s), 0), gmask))

    # Tally light, separated from the barrel by a transparent gap.
    r_tally = s * (0.13 if small else 0.105)
    tx, ty = s * 0.80, s * 0.20
    gap = s * (0.05 if small else 0.035)
    cut = Image.new("L", (s, s), 255)
    ImageDraw.Draw(cut).ellipse([tx - r_tally - gap, ty - r_tally - gap, tx + r_tally + gap, ty + r_tally + gap], fill=0)
    alpha = img.getchannel("A")
    img.putalpha(Image.composite(alpha, Image.new("L", (s, s), 0), cut))
    tally = radial(s, (tx - r_tally * 0.3, ty - r_tally * 0.35), r_tally * 1.3, (255, 120, 104), (214, 32, 22))
    tmask = Image.new("L", (s, s), 0)
    ImageDraw.Draw(tmask).ellipse([tx - r_tally, ty - r_tally, tx + r_tally, ty + r_tally], fill=255)
    img.paste(tally, (0, 0), tmask)

    return img.resize((size, size), Image.LANCZOS)


def main():
    images = [draw(n) for n in SIZES]
    largest = images[-1]
    largest.save(OUT, format="ICO", sizes=[(n, n) for n in SIZES], append_images=images[:-1])
    if os.environ.get("ICON_PREVIEW"):  # a strip of every size, to look at
        preview = Image.new("RGBA", (sum(SIZES) + 10 * len(SIZES), 256), (240, 240, 240, 255))
        x = 0
        for img in images:
            preview.paste(img, (x, 256 - img.height), img)
            x += img.width + 10
        preview.save(os.environ["ICON_PREVIEW"])


if __name__ == "__main__":
    main()
