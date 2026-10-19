#!/usr/bin/env python3
"""Generates the scorch mark textures of this fork (own work, procedural, nothing taken from the game).

  gen_scorch.py <out_dir> [--preview <png>]

Writes <out_dir>/Art/Textures/exscorch01.dds     colour (RGB) and coverage (alpha) of the marks, DXT5, 2048x2048, mip maps
       <out_dir>/Art/Textures/exscorch01_h.dds   height of the ground (alpha, 0.5 = flat), DXT5, 2048x2048, mip maps

Both use the layout of the original atlas: 3x3 marks, each mark a square of a quarter of the texture, centres at 1/8, 1/2 and 7/8
(the engine places a mark on the terrain with its own uv mapping, see W3DScorch.cpp). Types used by the game: 0 to 3 scorch marks,
4 soft shadow blob. The remaining cells hold a cracked ground and are empty otherwise.
The height texture is used by the native renderer to light the marks as shallow craters (rim and bowl).
"""
import os, struct, sys
import numpy as np
from PIL import Image

ATLAS = 2048
MARK = ATLAS // 4          # 512 px: the mark fills the square, radius 1 = half of it
CELL_CENTERS = [0.125, 0.5, 0.875]


def pink(n, beta, seed, lo=1.0):
    """smooth random field with a 1/f^beta spectrum, roughly in [-1, 1]"""
    rng = np.random.default_rng(seed)
    f = np.fft.fftfreq(n)[:, None] ** 2 + np.fft.fftfreq(n)[None, :] ** 2
    f = np.sqrt(f)
    f[0, 0] = 1.0
    spec = (rng.normal(size=(n, n)) + 1j * rng.normal(size=(n, n))) / np.maximum(f * n / lo, 1.0) ** beta
    spec[0, 0] = 0
    a = np.real(np.fft.ifft2(spec))
    a -= a.mean()
    return a / (np.abs(a).max() + 1e-9)


def smoothstep(a, b, x):
    t = np.clip((x - a) / (b - a), 0.0, 1.0)
    return t * t * (3 - 2 * t)


def mark(kind, seed):
    """returns (rgb float HxWx3, alpha HxW, height HxW with 0.5 = flat) of one mark, n x n pixels"""
    n = MARK
    y, x = np.mgrid[0:n, 0:n].astype(np.float64)
    x = (x + 0.5) / n * 2 - 1
    y = (y + 0.5) / n * 2 - 1
    rng = np.random.default_rng(seed)
    big = pink(n, 1.6, seed * 7 + 1)
    mid = pink(n, 1.2, seed * 7 + 2, lo=4.0)
    fine = pink(n, 0.7, seed * 7 + 3, lo=8.0)
    th = np.arctan2(y, x)

    if kind == 4:  # soft round shadow under things that burn: no height
        r = np.sqrt(x * x + y * y)
        a = smoothstep(0.95, 0.15, r) * (0.55 + 0.25 * big)
        rgb = np.zeros((n, n, 3)) + np.array([0.05, 0.045, 0.04])
        return rgb, np.clip(a, 0, 1) * 0.55, np.full((n, n), 0.5)

    # outline: radius depends on the angle, a few blast streaks reach further
    stretch = [1.0, 1.35, 1.0, 1.1][kind]
    ang = rng.uniform(0, np.pi)
    xr = x * np.cos(ang) + y * np.sin(ang)
    yr = -x * np.sin(ang) + y * np.cos(ang)
    r = np.sqrt((xr / stretch) ** 2 + (yr * (1.0 if kind != 1 else 1.25)) ** 2)
    rays = np.zeros_like(th)
    n_rays = [9, 7, 14, 22][kind]
    for k in range(n_rays):
        a0 = rng.uniform(-np.pi, np.pi)
        w = rng.uniform(0.04, 0.12)
        length = rng.uniform(0.1, 0.38 if kind != 3 else 0.5)
        d = np.angle(np.exp(1j * (th - a0)))
        rays = np.maximum(rays, length * np.exp(-(d / w) ** 2))
    base_r = [0.50, 0.52, 0.42, 0.44][kind]
    outline_round = base_r * (1.0 + 0.22 * big + 0.10 * mid)
    outline = outline_round + rays * (0.55 + 0.45 * mid)
    if kind == 2:  # a cluster: two more blotches next to the first
        m = np.full_like(r, -1.0)
        for k in range(2):
            cx, cy = rng.uniform(-0.38, 0.38, 2)
            rr2 = np.sqrt((x - cx) ** 2 + (y - cy) ** 2)
            m = np.maximum(m, 0.25 * (1 + 0.35 * mid) - rr2)
        d = np.minimum(r - outline, -m)
    else:
        d = r - outline

    core = smoothstep(0.045, -0.045, d + 0.03 * fine)                       # solid burnt area
    inner = smoothstep(0.0, -0.22, d + 0.05 * mid)                          # darker towards the middle
    halo = np.exp(-np.maximum(d, 0) / 0.10) * (0.55 + 0.45 * mid) * (r < 0.98)
    soot = np.exp(-np.maximum(d, 0) / 0.30) * 0.16 * (0.6 + 0.4 * big)

    # speckles of debris around the mark
    speck = np.zeros((n, n))
    for k in range(260):
        ang2 = rng.uniform(-np.pi, np.pi)
        rad = base_r * rng.uniform(0.9, 1.9)
        px, py = np.cos(ang2) * rad, np.sin(ang2) * rad
        if abs(px) > 0.93 or abs(py) > 0.93:
            continue
        s = rng.uniform(0.006, 0.02)
        speck = np.maximum(speck, np.exp(-((x - px) ** 2 + (y - py) ** 2) / (2 * s * s)) * rng.uniform(0.5, 1.0) * np.exp(-(rad - base_r) * 2.2))
    far = smoothstep(0.95, 0.55, np.sqrt(x * x + y * y))
    halo = halo * far
    soot = soot * far
    alpha = core * (0.70 + 0.10 * fine + 0.12 * inner) + (1 - core) * (halo * 0.42 + soot) + speck * 0.8
    alpha = np.clip(alpha, 0, 1)
    edge = smoothstep(1.0, 0.86, np.maximum(np.abs(x), np.abs(y)))
    alpha *= edge

    # colour: black soot in the middle, brown-grey ash at the edge, lighter dust in the halo
    dark = np.array([0.055, 0.047, 0.04])
    ash = np.array([0.24, 0.20, 0.16])
    dust = np.array([0.34, 0.30, 0.25])
    t_core = np.clip(inner * 0.9 + 0.1 * fine, 0, 1)[..., None]
    rgb = ash * (1 - t_core) + dark * t_core
    t_halo = np.clip(halo * 1.5, 0, 1)[..., None] * (1 - core[..., None])
    rgb = rgb * (1 - t_halo) + dust * t_halo
    rgb = rgb * (0.85 + 0.3 * (0.5 + 0.5 * fine))[..., None]

    # height: a shallow bowl with a rim, scattered by the noise; flat (0.5) far away
    rc = np.maximum(outline_round * 0.85, 0.08)
    rr = r
    bowl = -np.clip(1.0 - (rr / rc) ** 2, 0, 1) ** 1.3
    rim = 0.38 * np.exp(-(((rr - rc * 1.06) / (0.10 + 0.05 * mid)) ** 2))
    ejecta = 0.10 * np.exp(-np.maximum(rr - rc, 0) / 0.22) * (0.5 + 0.5 * big) * (rr > rc)
    clods = 0.05 * fine * np.clip(1 - rr / (rc * 1.6), 0, 1)
    if kind == 1:  # a blast from the side digs deeper on one end
        bowl = bowl * (1.0 + 0.35 * (xr / 1.2))
    grooves = -0.10 * np.clip(rays / 0.3, 0, 1) * (rr > 0.05) * np.exp(-np.maximum(rr - rc, 0) / 0.25)
    hgt = bowl * 0.95 + rim + ejecta + clods + grooves
    hgt *= smoothstep(1.0, 0.8, np.maximum(np.abs(x), np.abs(y)))
    h = 0.5 + 0.5 * np.clip(hgt, -1, 1)
    return rgb, alpha, h


def cracks(seed):
    """dry cracked ground: dark thin lines on a transparent background"""
    n = MARK
    rng = np.random.default_rng(seed)
    pts = rng.uniform(-0.9, 0.9, (46, 2))
    y, x = np.mgrid[0:n, 0:n].astype(np.float64)
    x = (x + 0.5) / n * 2 - 1
    y = (y + 0.5) / n * 2 - 1
    d1 = np.full((n, n), 9.0)
    d2 = np.full((n, n), 9.0)
    for px, py in pts:
        d = np.sqrt((x - px) ** 2 + (y - py) ** 2)
        d2 = np.minimum(d2, np.maximum(d, d1))
        d1 = np.minimum(d1, d)
    edge = d2 - d1
    jitter = pink(n, 1.0, seed + 5, lo=6.0) * 0.01
    line = np.exp(-((edge + jitter) / 0.012) ** 2)
    fade = smoothstep(0.95, 0.5, np.sqrt(x * x + y * y))
    a = np.clip(line * fade * 0.9, 0, 1)
    rgb = np.zeros((n, n, 3)) + np.array([0.03, 0.025, 0.02])
    return rgb, a, np.full((n, n), 0.5) - 0.12 * line * fade


def build():
    rgb = np.zeros((ATLAS, ATLAS, 3)) + np.array([0.2, 0.17, 0.14])
    alpha = np.zeros((ATLAS, ATLAS))
    height = np.full((ATLAS, ATLAS), 0.5)
    for idx in range(9):
        row, col = divmod(idx, 3)
        cx = int(round(CELL_CENTERS[col] * ATLAS))
        cy = int(round(CELL_CENTERS[row] * ATLAS))
        if idx < 5:
            m = mark(idx, 11 + idx * 5)
        elif idx == 5:
            m = cracks(77)
        else:
            continue
        x0, y0 = cx - MARK // 2, cy - MARK // 2
        rgb[y0:y0 + MARK, x0:x0 + MARK] = m[0]
        alpha[y0:y0 + MARK, x0:x0 + MARK] = m[1]
        height[y0:y0 + MARK, x0:x0 + MARK] = m[2]
    return rgb, alpha, height


def dxt5_levels(img_rgba):
    """DXT5 data of all mip levels of an RGBA uint8 image (through Pillow's encoder)"""
    data = []
    im = Image.fromarray(img_rgba, 'RGBA')
    levels = 0
    while True:
        tmp = os.path.join(os.environ.get('TEMP', '.'), '_scorch_tmp.dds')
        im.save(tmp, pixel_format='DXT5')
        raw = open(tmp, 'rb').read()
        os.remove(tmp)
        hdr = 128 + (20 if raw[84:88] == b'DX10' else 0)
        data.append(raw[hdr:])
        levels += 1
        if im.width <= 4:
            break
        im = im.resize((im.width // 2, im.height // 2), Image.BOX)
    return data, levels


def write_dds(path, img_rgba):
    data, levels = dxt5_levels(img_rgba)
    h, w = img_rgba.shape[:2]
    flags = 0x1 | 0x2 | 0x4 | 0x1000 | 0x20000 | 0x80000       # caps, height, width, pixel format, mipmap count, linear size
    hdr = b'DDS ' + struct.pack('<7I', 124, flags, h, w, len(data[0]), 0, levels) + b'\0' * 44
    hdr += struct.pack('<2I4s5I', 32, 0x4, b'DXT5', 0, 0, 0, 0, 0)
    hdr += struct.pack('<5I', 0x1000 | 0x400000, 0, 0, 0, 0)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'wb') as f:
        f.write(hdr)
        for d in data:
            f.write(d)


def preview(rgb, alpha, height, path):
    """a rough picture of the result on grass, lit from the upper left like the in-game relief shading"""
    n = ATLAS
    ground = np.zeros((n, n, 3)) + np.array([0.28, 0.36, 0.17])
    gy, gx = np.gradient(height * 0.9)
    gx *= n / 4 * 0.25 / 4
    gy *= n / 4 * 0.25 / 4
    nx, ny, nz = -gx * 6, -gy * 6, np.ones_like(gx)
    inv = 1 / np.sqrt(nx * nx + ny * ny + nz * nz)
    nx, ny, nz = nx * inv, ny * inv, nz * inv
    L = np.array([0.55, -0.45, 0.70]); L /= np.linalg.norm(L)
    amb, dif = 0.45, 0.75
    gain = (amb + dif * np.maximum(nx * L[0] + ny * L[1] + nz * L[2], 0)) / (amb + dif * L[2])
    lit = np.clip(ground * gain[..., None], 0, 1)
    out = lit * (1 - alpha[..., None]) + rgb * alpha[..., None]
    Image.fromarray((np.clip(out, 0, 1) * 255).astype(np.uint8)).save(path)


def main():
    out = sys.argv[1]
    rgb, alpha, height = build()
    tex = np.zeros((ATLAS, ATLAS, 4), np.uint8)
    tex[..., :3] = (np.clip(rgb, 0, 1) * 255 + 0.5).astype(np.uint8)
    tex[..., 3] = (np.clip(alpha, 0, 1) * 255 + 0.5).astype(np.uint8)
    # colour of the transparent parts: the ash colour, so filtering does not pull dark fringes into the edges
    mask = alpha < 0.02
    tex[mask, :3] = (np.array([0.20, 0.17, 0.14]) * 255).astype(np.uint8)
    write_dds(os.path.join(out, 'Art', 'Textures', 'exscorch01.dds'), tex)
    ht = np.zeros((ATLAS, ATLAS, 4), np.uint8)
    hv = (np.clip(height, 0, 1) * 255 + 0.5).astype(np.uint8)
    ht[..., 0] = ht[..., 1] = ht[..., 2] = hv
    ht[..., 3] = hv
    write_dds(os.path.join(out, 'Art', 'Textures', 'exscorch01_h.dds'), ht)
    if '--preview' in sys.argv:
        preview(rgb, alpha, height, sys.argv[sys.argv.index('--preview') + 1])
    print('written to', out)


if __name__ == '__main__':
    main()
