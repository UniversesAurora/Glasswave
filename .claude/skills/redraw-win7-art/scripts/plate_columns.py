"""Extract a button plate's columns as C++ tables, for a plate that is the same across its
middle (WMP's square plates, split buttons, Media Center's control plates).

    uv run --no-project --with pillow python plate_columns.py IMAGE --cols 0,1,2,12 [--name kPlateHot]

Prints `constexpr PlateColumn NAME[n][rows]` with {0xRRGGBB, alpha} per row: the edge columns
from the outside in, then a middle column that is stretched across the button. Check first
with dump_pixels.py that the columns between the edge ones and the middle are equal, and that
the right edge mirrors the left.
"""
import argparse
from PIL import Image


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('image')
    ap.add_argument('--cols', default='0,1,2,12')
    ap.add_argument('--name', default='kPlate')
    args = ap.parse_args()
    im = Image.open(args.image).convert('RGBA')
    cols = [int(c) for c in args.cols.split(',')]
    print('constexpr PlateColumn %s[%d][%d] = {' % (args.name, len(cols), im.height))
    for i, x in enumerate(cols):
        cells = ['{0x%02X%02X%02X, %d}' % (p[0], p[1], p[2], p[3]) for p in (im.getpixel((x, y)) for y in range(im.height))]
        print('    {' + ', '.join(cells) + '}' + (',' if i + 1 < len(cols) else '};'))


if __name__ == '__main__':
    main()
