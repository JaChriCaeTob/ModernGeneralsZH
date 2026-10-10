#!/usr/bin/env python3
"""Converts the updated_*.bmp screenshots to PNG and builds compare_<frame>.png (original left, updated right) in the given folder."""
import glob, os, sys
from PIL import Image, ImageDraw

folder = sys.argv[1]
for bmp in sorted(glob.glob(os.path.join(folder, 'updated_*.bmp'))):
    png = bmp[:-4] + '.png'
    Image.open(bmp).convert('RGB').save(png)
    os.remove(bmp)
for up in sorted(glob.glob(os.path.join(folder, 'updated_*.png'))):
    frame = os.path.basename(up)[len('updated_'):-4]
    orig = os.path.join(folder, 'original_%s.png' % frame)
    if not os.path.exists(orig):
        continue
    a, b = Image.open(orig).convert('RGB'), Image.open(up).convert('RGB')
    if a.size != b.size:
        a = a.resize(b.size, Image.LANCZOS)
    half = b.size[0] // 2
    sheet = Image.new('RGB', (b.size[0] * 2, b.size[1]))
    sheet.paste(a, (0, 0)); sheet.paste(b, (b.size[0], 0))
    d = ImageDraw.Draw(sheet)
    d.text((16, b.size[1] - 28), 'original', fill=(255, 255, 255)); d.text((b.size[0] + 16, b.size[1] - 28), 'updated', fill=(255, 255, 255))
    sheet.save(os.path.join(folder, 'compare_%s.png' % frame))
    print('compare_%s.png' % frame)
