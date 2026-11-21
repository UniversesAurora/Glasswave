"""Print a bitmap as a grid of colours, one row per line.

    uv run --no-project --with pillow python dump_pixels.py IMAGE [IMAGE ...] [--crop x0,y0,x1,y1]

Each cell is RRGGBB when opaque (alpha >= 250), RRGGBB/aa (alpha in hex) when partly
transparent, and '.' when fully transparent. Read it with the zoom sheet open beside it.
"""
import argparse
from PIL import Image


def cell(p):
    r, g, b, a = p
    if a == 0:
        return '   .   '
    if a >= 250:
        return '%02X%02X%02X ' % (r, g, b)
    return '%02X%02X%02X/%02x' % (r, g, b, a)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('images', nargs='+')
    ap.add_argument('--crop', help='x0,y0,x1,y1')
    args = ap.parse_args()
    for path in args.images:
        im = Image.open(path).convert('RGBA')
        if args.crop:
            im = im.crop(tuple(int(v) for v in args.crop.split(',')))
        print(path, im.size)
        for y in range(im.height):
            print('%2d ' % y + ' '.join(cell(im.getpixel((x, y))) for x in range(im.width)))


if __name__ == '__main__':
    main()
