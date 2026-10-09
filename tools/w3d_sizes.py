#!/usr/bin/env python3
"""Lists the size and triangle budget of W3D models, to use the original models as a scale reference.

  w3d_sizes.py <folder with .W3D files> [output.csv]

Per mesh: file, mesh name, triangles, vertices, bounding box size (x, y, z in game units; the game's Z axis is up, one unit is roughly 1 foot).
"""
import csv, os, struct, sys

def chunks(data, pos, end):
    while pos + 8 <= end:
        t, s = struct.unpack_from('<II', data, pos)
        size = s & 0x7FFFFFFF
        yield t, s >> 31, pos + 8, pos + 8 + size
        pos += 8 + size

def scan(path):
    data = open(path, 'rb').read()
    for t, sub, a, b in chunks(data, 0, len(data)):
        if t != 0x00:                       # W3D_CHUNK_MESH
            continue
        for t2, _, a2, b2 in chunks(data, a, b):
            if t2 == 0x1F:                  # W3D_CHUNK_MESH_HEADER3
                name = data[a2 + 8:a2 + 24].split(b'\0')[0].decode('latin-1')
                tris, verts = struct.unpack_from('<II', data, a2 + 40)
                mn = struct.unpack_from('<3f', data, a2 + 76)
                mx = struct.unpack_from('<3f', data, a2 + 88)
                yield name, tris, verts, [mx[i] - mn[i] for i in range(3)]

def main():
    if len(sys.argv) < 2:
        print(__doc__); return 1
    rows = []
    for root, _, files in os.walk(sys.argv[1]):
        for f in sorted(files):
            if f.lower().endswith('.w3d'):
                try:
                    for name, tris, verts, size in scan(os.path.join(root, f)):
                        rows.append([f, name, tris, verts] + ['%.1f' % v for v in size])
                except Exception as e:
                    print('skipped', f, e)
    out = sys.argv[2] if len(sys.argv) > 2 else None
    w = csv.writer(open(out, 'w', newline='') if out else sys.stdout)
    w.writerow(['file', 'mesh', 'triangles', 'vertices', 'size_x', 'size_y', 'size_z'])
    w.writerows(rows)
    return 0

sys.exit(main())
