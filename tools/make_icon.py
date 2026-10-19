"""Draws Halo Settings' icon and writes it as res/halo-settings.ico, res/halo-settings.res and the favicon in
ui/settings.html.

The toolchain has no resource compiler, so the .res is written here (lld links .res files as they are):
one RT_ICON per size, PNG-compressed, and an RT_GROUP_ICON with id 1, which makes it the exe's icon.
Run from the repo root: python tools/make_icon.py
"""
import base64
import io
import re
import struct

from PIL import Image, ImageDraw, ImageFilter

N = 1024  # drawn large, then scaled down
SIZES = [16, 20, 24, 32, 40, 48, 64, 96, 128, 256]
TEAL, TEAL_HI = (0x14, 0xA3, 0x94), (0x5F, 0xE0, 0xCF)


def lerp(a, b, t):
    return tuple(round(x + (y - x) * t) for x, y in zip(a, b))


def draw():
    # A smoked-glass squircle with a teal halo ring and a white knob on it: a dial, in Halo's colours.
    bg = Image.new("RGBA", (N, N))
    top, bottom = (0x2A, 0x2D, 0x34), (0x0F, 0x10, 0x13)
    px = bg.load()
    for y in range(N):
        c = lerp(top, bottom, y / (N - 1))
        for x in range(N):
            px[x, y] = c + (255,)
    mask = Image.new("L", (N, N))
    ImageDraw.Draw(mask).rounded_rectangle([40, 40, N - 41, N - 41], radius=230, fill=255)
    icon = Image.new("RGBA", (N, N))
    icon.paste(bg, (0, 0), mask)
    # a hairline highlight along the top edge
    rim = Image.new("RGBA", (N, N))
    ImageDraw.Draw(rim).rounded_rectangle([40, 40, N - 41, N - 41], radius=230, outline=(255, 255, 255, 46), width=8)
    icon.alpha_composite(rim)

    c, r, w = N / 2, 300, 92
    ring = Image.new("RGBA", (N, N))
    d = ImageDraw.Draw(ring)
    for i in range(w):  # the ring brightens toward its outer edge
        col = lerp(TEAL, TEAL_HI, i / w)
        rr = r - w / 2 + i
        d.ellipse([c - rr, c - rr, c + rr, c + rr], outline=col + (255,), width=2)
    glow = ring.filter(ImageFilter.GaussianBlur(40))
    icon.alpha_composite(Image.composite(glow, Image.new("RGBA", (N, N)), mask))
    icon.alpha_composite(ring)

    k = 78  # the knob, at one o'clock
    kx, ky = c + r * 0.5, c - r * 0.866
    shadow = Image.new("RGBA", (N, N))
    ImageDraw.Draw(shadow).ellipse([kx - k, ky - k + 14, kx + k, ky + k + 14], fill=(0, 0, 0, 150))
    icon.alpha_composite(shadow.filter(ImageFilter.GaussianBlur(14)))
    ImageDraw.Draw(icon).ellipse([kx - k, ky - k, kx + k, ky + k], fill=(250, 250, 252, 255))
    return icon


def png(img):
    b = io.BytesIO()
    img.save(b, "PNG", optimize=True)
    return b.getvalue()


def res_entry(rtype, rid, data, flags):
    header = struct.pack("<IIHHHHIHHII", len(data), 32, 0xFFFF, rtype, 0xFFFF, rid, 0, flags, 0x0409, 0, 0)
    return header + data + b"\0" * (-len(data) % 4)


def main():
    big = draw()
    images = [big.resize((s, s), Image.LANCZOS) for s in SIZES]
    pngs = [png(i) for i in images]

    big.resize((256, 256), Image.LANCZOS).save("res/halo-settings.ico", sizes=[(s, s) for s in SIZES])
    # the dashboard's favicon, which the Edge app window shows as its icon
    fav = '<link rel="icon" href="data:image/png;base64,' + base64.b64encode(png(images[-2])).decode() + '">'
    html = open("ui/settings.html", encoding="utf-8").read()
    html, n = re.subn(r'<link rel="icon"[^>]*>', lambda m: fav, html)
    if not n:
        html = html.replace("<title>Halo Settings</title>", "<title>Halo Settings</title>\n" + fav, 1)
    open("ui/settings.html", "w", encoding="utf-8", newline="").write(html)

    # the empty entry every .res starts with
    out = struct.pack("<IIHHHHIHHII", 0, 32, 0xFFFF, 0, 0xFFFF, 0, 0, 0, 0, 0, 0)
    group = struct.pack("<HHH", 0, 1, len(SIZES))
    for i, (s, data) in enumerate(zip(SIZES, pngs)):
        out += res_entry(3, i + 1, data, 0x1010)  # RT_ICON
        group += struct.pack("<BBBBHHIH", s % 256, s % 256, 0, 0, 1, 32, len(data), i + 1)
    out += res_entry(14, 1, group, 0x1030)  # RT_GROUP_ICON
    open("res/halo-settings.res", "wb").write(out)
    print("wrote res/halo-settings.ico, res/halo-settings.res and the favicon")


if __name__ == "__main__":
    main()
