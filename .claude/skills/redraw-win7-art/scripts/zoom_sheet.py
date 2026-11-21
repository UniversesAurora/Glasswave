"""Stack bitmaps vertically, enlarged with nearest neighbour on a checkerboard, labelled.

    uv run --no-project --with pillow python zoom_sheet.py OUT.png SCALE IMAGE[@x0,y0,x1,y1] ...

Nearest neighbour keeps every source pixel a visible square, so rows, bands, sharp breaks and
antialiased edges can be read; the checkerboard shows what is translucent.
"""
import sys
from PIL import Image, ImageDraw


def main():
    out, scale, files = sys.argv[1], int(sys.argv[2]), sys.argv[3:]
    tiles = []
    for f in files:
        crop = None
        if '@' in f:
            f, c = f.split('@')
            crop = tuple(int(v) for v in c.split(','))
        im = Image.open(f).convert('RGBA')
        if crop:
            im = im.crop(crop)
        im = im.resize((im.width * scale, im.height * scale), Image.NEAREST)
        bg = Image.new('RGBA', im.size, (200, 200, 200, 255))
        d = ImageDraw.Draw(bg)
        s = 8
        for y in range(0, im.height, s):
            for x in range(0, im.width, s):
                if (x // s + y // s) % 2:
                    d.rectangle([x, y, x + s - 1, y + s - 1], fill=(160, 160, 160, 255))
        bg.alpha_composite(im)
        label = f.replace('\\', '/').split('/')[-1] + (f' {crop}' if crop else '') + f' {im.width // scale}x{im.height // scale}'
        tiles.append((label, bg))
    width = max(t[1].width for t in tiles) + 4
    height = sum(t[1].height + 14 for t in tiles)
    canvas = Image.new('RGBA', (max(width, 300), height), (255, 255, 255, 255))
    d = ImageDraw.Draw(canvas)
    y = 0
    for label, t in tiles:
        d.text((2, y), label, fill=(0, 0, 0, 255))
        y += 13
        canvas.paste(t, (2, y))
        y += t.height + 1
    canvas.save(out)


if __name__ == '__main__':
    main()
