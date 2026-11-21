"""Fit a layered glass model to a glossy bitmap (a slider thumb, an orb, a pill) so that it can
be drawn in vectors with Direct2D at any resolution. Matches PaintGlass in
src/foo_ui_glasswave/ui/player_window.cpp; keep the two in step.

    uv run --no-project --with pillow --with numpy --with scipy python fit_glass.py IMAGE \
        --shape X0,X1,CY,R,HORIZON [--out DIR] [--name NAME] [--starts 60] [--preview 12]

Units are bitmap pixels. The shape is a stadium: the centre line from (X0, CY) to (X1, CY),
radius R (a circle when X0 == X1); HORIZON is the y of the sharp break between the light upper
half and the dark lower half (read it from the pixel dump: the row where the colours jump).

Each half is drawn as:
  base   a vertical gradient over the half, three stops (top, middle, foot)
  A      a "crescent": radial about the centre line (centre y + ay, radii R*ax, R*ayr), alpha 0
         up to sA, rising to aA at 1 and held beyond, colour CA - light gathering under the rim
  B      a "glow": radial (centre y + by, radii R*bx, R*byr), alpha aB at its centre, aB*mB
         halfway, 0 at 1, colour CB - a reflection or a light from below
then a rim: a one-unit stroke inside the outline in the bitmap's outermost colour per row.

The fit: least squares over 4x4 subsamples of every opaque pixel, the outermost ring (within
1.3 px of the outline) left out because the rim covers it, many random starts within bounds
that keep the layers physical. Prints the rms error per half (in 0-255 levels) and a C++
GlassLook initializer, and writes NAME_model.png (the fit, enlarged) and NAME_compare.png (the
bitmap and the fit side by side, both enlarged) for checking by eye.
"""
import argparse, json, os
import numpy as np
from PIL import Image
from scipy.optimize import least_squares

SUB = 4
KEYS = [('base', 9, 0, 255), ('ay', 1, -3, 3), ('ax', 1, 0.6, 1.6), ('ayr', 1, 0.6, 1.6), ('sA', 1, 0.2, 0.9),
        ('CA', 3, 0, 255), ('aA', 1, 0, 1), ('by', 1, -3, 4), ('bx', 1, 0.3, 1.6), ('byr', 1, 0.3, 1.6),
        ('CB', 3, 0, 255), ('aB', 1, 0, 1), ('mB', 1, 0, 1)]


def st(px, py, x0, x1, cy, rx, ry):
    qx = np.where(px < x0, px - x0, np.where(px > x1, px - x1, 0.0)) / rx
    return np.sqrt(qx * qx + ((py - cy) / ry) ** 2)


def unpack(v):
    p, i = {}, 0
    for k, n, lo, hi in KEYS:
        p[k] = v[i] if n == 1 else np.array(v[i:i + n])
        i += n
    return p


def half_colour(p, SX, SY, shape, ytop, ybot):
    x0, x1, cy, R, h = shape
    f = np.clip((SY - ytop) / (ybot - ytop), 0, 1)[..., None]
    b = p['base'].reshape(3, 3)
    base = np.where(f < 0.5, b[0] + (b[1] - b[0]) * (f / 0.5), b[1] + (b[2] - b[1]) * ((f - 0.5) / 0.5))
    tA = st(SX, SY, x0, x1, cy + p['ay'], R * p['ax'], R * p['ayr'])
    aA = np.interp(tA, [0, p['sA'], 1], [0, 0, p['aA']])[..., None]
    c = base * (1 - aA) + p['CA'] * aA
    tB = st(SX, SY, x0, x1, cy + p['by'], R * p['bx'], R * p['byr'])
    aB = np.interp(tB, [0, 0.5, 1], [p['aB'], p['aB'] * p['mB'], 0])[..., None]
    return c * (1 - aB) + p['CB'] * aB


def fit_half(a, shape, upper, starts, seed=1):
    x0, x1, cy, R, h = shape
    pts, cols = [], []
    for y in range(a.shape[0]):
        for x in range(a.shape[1]):
            if a[y, x, 3] < 250 or (y + 0.5 < h) != upper:
                continue
            if st(np.array(x + 0.5), np.array(y + 0.5), x0, x1, cy, R, R) * R > R - 1.3:
                continue
            pts.append((x, y))
            cols.append(a[y, x, :3])
    cols = np.array(cols)
    off = (np.arange(SUB) + 0.5) / SUB
    ox, oy = np.meshgrid(off, off)
    SX = np.array([x + ox.ravel() for x, y in pts])
    SY = np.array([y + oy.ravel() for x, y in pts])
    yt, yb = (cy - R, h) if upper else (h, cy + R)
    lo = np.concatenate([[l] * n for k, n, l, hi in KEYS])
    hi = np.concatenate([[hh] * n for k, n, l, hh in KEYS])
    mean = np.tile(cols.mean(0), 3)
    rng = np.random.default_rng(seed)
    best = None
    for _ in range(starts):
        v0 = lo + (hi - lo) * rng.random(len(lo))
        r = least_squares(lambda v: np.concatenate([(half_colour(unpack(v), SX, SY, shape, yt, yb).mean(1) - cols).ravel(),
                                                    0.02 * (v[:9] - mean)]),
                          v0, bounds=(lo, hi), max_nfev=2000)
        err = np.sqrt(((half_colour(unpack(r.x), SX, SY, shape, yt, yb).mean(1) - cols) ** 2).mean())
        if best is None or err < best[0]:
            best = (err, r.x)
    return best, len(pts)


def rim_colours(a):
    out = []
    for y in range(a.shape[0]):
        xs = [x for x in range(a.shape[1]) if a[y, x, 3] > 40]
        if xs:
            out.append((y, a[y, xs[0], :3]))
    return out


def render(shape, size, up, lo_, rim, scale):
    x0, x1, cy, R, h = shape
    W, H = size
    ss = 3
    xs = (np.arange(W * scale * ss) + 0.5) / (scale * ss)
    ys = (np.arange(H * scale * ss) + 0.5) / (scale * ss)
    SX, SY = np.meshgrid(xs, ys)
    d = st(SX, SY, x0, x1, cy, R, R) * R
    inside = d <= R
    col = np.where((SY < h)[..., None], half_colour(unpack(up), SX, SY, shape, cy - R, h),
                   half_colour(unpack(lo_), SX, SY, shape, h, cy + R))
    rys = np.array([y + 0.5 for y, c in rim])
    rc = np.array([c for y, c in rim])
    rimc = np.stack([np.interp(SY, rys, rc[:, c]) for c in range(3)], -1)
    ring = (d > R - 1.0) & inside
    col = np.where(ring[..., None], col * 0.15 + rimc * 0.85, col)
    img = np.concatenate([np.clip(col, 0, 255), inside[..., None] * 255.0], -1)
    img = img.reshape(H * scale, ss, W * scale, ss, 4).mean((1, 3))
    return Image.fromarray(img.astype(np.uint8), 'RGBA')


def hexc(c):
    return '0x%02X%02X%02X' % tuple(int(round(min(255, max(0, x)))) for x in c)


def cpp_half(v):
    p = unpack(v)
    b = p['base']
    return '{{%s, %s, %s}, %.3ff, %.3ff, %.3ff, %.3ff, %s, %.3ff, %.3ff, %.3ff, %.3ff, %s, %.3ff, %.3ff}' % (
        hexc(b[0:3]), hexc(b[3:6]), hexc(b[6:9]), p['ay'], p['ax'], p['ayr'], p['sA'], hexc(p['CA']), p['aA'],
        p['by'], p['bx'], p['byr'], hexc(p['CB']), p['aB'], p['mB'])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('image')
    ap.add_argument('--shape', required=True, help='X0,X1,CY,R,HORIZON in bitmap pixels')
    ap.add_argument('--out', default='.')
    ap.add_argument('--name')
    ap.add_argument('--starts', type=int, default=60)
    ap.add_argument('--preview', type=int, default=12)
    args = ap.parse_args()
    shape = tuple(float(v) for v in args.shape.split(','))
    name = args.name or os.path.splitext(os.path.basename(args.image))[0]
    src = Image.open(args.image).convert('RGBA')
    a = np.asarray(src).astype(float)
    (eu, vu), nu = fit_half(a, shape, True, args.starts)
    (el, vl), nl = fit_half(a, shape, False, args.starts)
    rim = rim_colours(a)
    print('%s: upper rms %.2f over %d px, lower rms %.2f over %d px' % (name, eu, nu, el, nl))
    rims = ', '.join('{%.1ff, %s}' % (y + 0.5, hexc(c)) for y, c in rim)
    print('    {%s,\n     %s,\n     {%s},\n     %d}' % (cpp_half(vu), cpp_half(vl), rims, len(rim)))
    os.makedirs(args.out, exist_ok=True)
    model = render(shape, src.size, vu, vl, rim, args.preview)
    model.save(os.path.join(args.out, name + '_model.png'))
    big = src.resize((src.width * args.preview, src.height * args.preview), Image.NEAREST)
    sheet = Image.new('RGBA', (big.width * 2 + 8, big.height), (128, 128, 128, 255))
    sheet.alpha_composite(big, (0, 0))
    sheet.alpha_composite(model, (big.width + 8, 0))
    sheet.save(os.path.join(args.out, name + '_compare.png'))
    with open(os.path.join(args.out, name + '_fit.json'), 'w') as f:
        json.dump({'shape': shape, 'upper': [float(x) for x in vu], 'lower': [float(x) for x in vl],
                   'rms': [eu, el], 'rim': [[y, [float(c) for c in col]] for y, col in rim]}, f, indent=1)


if __name__ == '__main__':
    main()
