"""Generates the DisplaySwitch icon: assets/icon.png (1024 px), assets/logo-256.png
and assets/DisplaySwitch.ico (16-256 px).

Style: dark terminal. A near-black tile with a neon green monitor outline, a
">_" shell prompt on the screen, a switch badge and faint CRT scanlines.
Each ICO size is drawn separately (not just downscaled), with bolder strokes and
fewer details at small sizes so it stays readable at 16-32 px. Requires Pillow.

    python tools/make_icon.py
"""
from pathlib import Path
from PIL import Image, ImageDraw, ImageFilter

ROOT = Path(__file__).resolve().parent.parent
ASSETS = ROOT / "assets"

BG_TOP = (16, 20, 26)
BG_BOTTOM = (6, 8, 11)
NEON = (0, 255, 156)        # terminal green
NEON_DIM = (0, 140, 90)
CYAN = (0, 229, 255)
SCREEN = (3, 12, 9)


def vertical_gradient(size, top, bottom):
    w, h = size
    col = Image.new("RGB", (1, 256))
    for y in range(256):
        t = y / 255
        col.putpixel((0, y), tuple(int(top[i] + (bottom[i] - top[i]) * t) for i in range(3)))
    return col.resize((w, h), Image.BICUBIC)


def mask_rounded(size, box, radius):
    m = Image.new("L", size, 0)
    ImageDraw.Draw(m).rounded_rectangle(box, radius=radius, fill=255)
    return m


def neon(canvas, shape_mask, color, blur, strength, core=True):
    """Glow around a shape: blurred halo plus the sharp shape on top."""
    halo = shape_mask.filter(ImageFilter.GaussianBlur(blur))
    layer = Image.new("RGBA", canvas.size, color + (0,))
    layer.putalpha(halo.point(lambda v: min(255, int(v * strength))))
    canvas.alpha_composite(layer)
    if core:
        solid = Image.new("RGBA", canvas.size, color + (0,))
        solid.putalpha(shape_mask)
        canvas.alpha_composite(solid)


def render(px):
    ss = 4
    S = px * ss
    small = px <= 32

    canvas = Image.new("RGBA", (S, S), (0, 0, 0, 0))

    # --- tile: dark rounded square with a thin neon rim ---
    margin = int(S * (0.02 if small else 0.06))
    radius = int(S * 0.22)
    tile_box = (margin, margin, S - margin, S - margin)
    tile_mask = mask_rounded((S, S), tile_box, radius)

    if not small:
        shadow = Image.new("RGBA", (S, S), (0, 0, 0, 0))
        sh = mask_rounded((S, S), (margin, margin + int(S * 0.025), S - margin, S - margin + int(S * 0.025)), radius)
        shadow.putalpha(sh.point(lambda v: int(v * 0.55)))
        canvas.alpha_composite(shadow.filter(ImageFilter.GaussianBlur(S * 0.02)))

    tile = vertical_gradient((S, S), BG_TOP, BG_BOTTOM).convert("RGBA")
    tile.putalpha(tile_mask)
    canvas.alpha_composite(tile)

    rim = Image.new("L", (S, S), 0)
    rw = max(int(S * (0.018 if small else 0.008)), 1)
    ImageDraw.Draw(rim).rounded_rectangle(tile_box, radius=radius, outline=255, width=rw)
    rim_layer = Image.new("RGBA", (S, S), NEON_DIM + (0,))
    rim_layer.putalpha(rim.point(lambda v: int(v * (0.9 if small else 0.6))))
    canvas.alpha_composite(rim_layer)

    # --- monitor outline (neon) ---
    mx0, mx1 = S * (0.16 if small else 0.20), S * (0.84 if small else 0.80)
    my0, my1 = S * (0.20 if small else 0.24), S * (0.70 if small else 0.66)
    lw = int(S * (0.065 if small else 0.034))
    screen_box = (mx0, my0, mx1, my1)

    # screen fill, slightly green, with CRT scanlines
    scr = Image.new("RGBA", (S, S), SCREEN + (255,))
    if not small:
        sl = ImageDraw.Draw(scr)
        step = max(int(S * 0.012), 2)
        for y in range(int(my0), int(my1), step * 2):
            sl.rectangle((mx0, y, mx1, y + step - 1), fill=(6, 22, 16, 255))
    scr.putalpha(mask_rounded((S, S), tuple(map(int, screen_box)), int(S * 0.045)))
    canvas.alpha_composite(scr)

    outline = Image.new("L", (S, S), 0)
    od = ImageDraw.Draw(outline)
    od.rounded_rectangle(screen_box, radius=S * 0.05, outline=255, width=lw)
    # stand: neck + base
    cx = S / 2
    od.rectangle((cx - lw * 0.5, my1, cx + lw * 0.5, my1 + S * 0.085), fill=255)
    od.rounded_rectangle((cx - S * 0.15, my1 + S * 0.08, cx + S * 0.15, my1 + S * 0.08 + lw),
                         radius=lw / 2, fill=255)
    neon(canvas, outline, NEON, S * 0.02, 1.1 if not small else 0.6)

    # --- shell prompt ">_" on the screen ---
    prompt = Image.new("L", (S, S), 0)
    pd = ImageDraw.Draw(prompt)
    sx0, sy0, sx1, sy1 = screen_box
    sw, sh = sx1 - sx0, sy1 - sy0
    pw = int(S * (0.075 if small else 0.05))
    # chevron
    cx0, cy = sx0 + sw * (0.20 if small else 0.18), sy0 + sh * 0.50
    ch = sh * (0.26 if small else 0.22)
    cwid = sw * (0.17 if small else 0.14)
    pd.line([(cx0, cy - ch), (cx0 + cwid, cy), (cx0, cy + ch)], fill=255, width=pw, joint="curve")
    for x, y in [(cx0, cy - ch), (cx0, cy + ch), (cx0 + cwid, cy)]:
        pd.ellipse((x - pw / 2, y - pw / 2, x + pw / 2, y + pw / 2), fill=255)
    # block cursor
    ux0 = cx0 + cwid + sw * (0.10 if small else 0.08)
    uw = sw * (0.30 if small else 0.24)
    pd.rectangle((ux0, cy + ch - pw * 0.9, ux0 + uw, cy + ch + pw * 0.4), fill=255)
    neon(canvas, prompt, NEON, S * 0.015, 1.0 if not small else 0.5)

    # --- switch badge: round neon badge with two swap arrows (bottom-right) ---
    if not small:  # too busy below 48 px: the prompt alone reads better
        br = S * 0.135
        bx, by = S * (0.775 if small else 0.765), S * (0.765 if small else 0.75)
        badge = Image.new("RGBA", (S, S), (0, 0, 0, 0))
        bd = ImageDraw.Draw(badge)
        bd.ellipse((bx - br, by - br, bx + br, by + br), fill=BG_BOTTOM + (255,))
        canvas.alpha_composite(badge)

        ring = Image.new("L", (S, S), 0)
        rd = ImageDraw.Draw(ring)
        rd.ellipse((bx - br, by - br, bx + br, by + br), outline=255, width=int(lw * 0.8))
        neon(canvas, ring, CYAN, S * 0.015, 0.9)

        arr = Image.new("L", (S, S), 0)
        ad = ImageDraw.Draw(arr)
        aw = int(lw * (0.75 if small else 0.7))
        alen, head, gap = br * 0.95, br * 0.38, br * 0.32
        for dy, right in ((-gap, True), (gap, False)):
            y = by + dy
            x_tail, x_tip = (bx - alen / 2, bx + alen / 2) if right else (bx + alen / 2, bx - alen / 2)
            back = x_tip - head if right else x_tip + head
            ad.line((x_tail, y, back, y), fill=255, width=aw)
            ad.polygon([(x_tip, y), (back, y - head * 0.75), (back, y + head * 0.75)], fill=255)
        neon(canvas, arr, CYAN, S * 0.01, 0.8)

    # clip to the tile
    alpha = canvas.getchannel("A")
    clip = tile_mask if small else Image.new("L", (S, S), 255)
    canvas.putalpha(Image.composite(alpha, Image.new("L", (S, S), 0), clip))
    return canvas.resize((px, px), Image.LANCZOS)


def main():
    ASSETS.mkdir(exist_ok=True)
    render(1024).save(ASSETS / "icon.png")
    render(256).save(ASSETS / "logo-256.png")

    sizes = [256, 128, 64, 48, 32, 24, 16]
    images = [render(s) for s in sizes]
    images[0].save(ASSETS / "DisplaySwitch.ico", format="ICO",
                   sizes=[(s, s) for s in sizes], append_images=images[1:])
    print("Wrote", ASSETS / "icon.png", ASSETS / "logo-256.png", "and", ASSETS / "DisplaySwitch.ico")


if __name__ == "__main__":
    main()
