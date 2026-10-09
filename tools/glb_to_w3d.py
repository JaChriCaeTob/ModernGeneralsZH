#!/usr/bin/env python3
"""Puts the geometry of an edited .glb back into a W3D file, keeping everything else of the original (names, skeleton, level of detail,
materials, shaders, textures) so the game accepts the result without any naming work.

  glb_to_w3d.py <edited.glb> <original.W3D> <output.W3D> [--list]

How it works
  * Every mesh object of the .glb is matched by name to a mesh of the original W3D ("CONTAINER.MESH" as written by w3d_to_glb.py;
    Blender's ".001" suffixes and upper/lower case are ignored). Meshes of the original that are not in the .glb stay as they were.
  * The new vertices, normals, UVs and triangles replace the old ones. Materials and textures stay those of the original mesh, so
    your texture has to be saved as the DDS with the same name as the original texture (see --list).
  * The object's position in the scene is taken into account (and undone against the bone it belongs to), so you may move, rotate and
    scale objects freely in Blender.
  * Triangulate in Blender (the glTF exporter does that). Flat/smooth shading and UV seams are fine.
Not supported: skinned meshes (characters with vertex weights; they are skipped with a warning) and new meshes that the original does not have.
--list prints the meshes of the original with the texture each one uses, then exits.
"""
import json
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import w3dlib as w3d


def read_glb(path):
    data = open(path, 'rb').read()
    magic, ver, length = struct.unpack_from('<III', data, 0)
    if magic != 0x46546C67:
        raise SystemExit('not a .glb file (export from Blender as "glTF Binary (.glb)")')
    pos, js, binary = 12, None, b''
    while pos < length:
        clen, ctype = struct.unpack_from('<II', data, pos)
        body = data[pos + 8:pos + 8 + clen]
        if ctype == 0x4E4F534A:
            js = json.loads(body)
        elif ctype == 0x004E4942:
            binary = body
        pos += 8 + clen
    return js, binary


COMP = {5120: ('b', 1), 5121: ('B', 1), 5122: ('h', 2), 5123: ('H', 2), 5125: ('I', 4), 5126: ('f', 4)}
NCOMP = {'SCALAR': 1, 'VEC2': 2, 'VEC3': 3, 'VEC4': 4}


def accessor(js, binary, idx):
    a = js['accessors'][idx]
    view = js['bufferViews'][a['bufferView']]
    fmt, size = COMP[a['componentType']]
    n = NCOMP[a['type']]
    stride = view.get('byteStride') or size * n
    base = view.get('byteOffset', 0) + a.get('byteOffset', 0)
    out = []
    for i in range(a['count']):
        row = struct.unpack_from('<%d%s' % (n, fmt), binary, base + i * stride)
        if a.get('normalized'):
            row = tuple(v / {'B': 255.0, 'H': 65535.0}.get(fmt, 1.0) for v in row)
        out.append(row if n > 1 else row[0])
    return out


def node_matrix(node):
    if 'matrix' in node:
        m = node['matrix']          # column major
        return [[m[0], m[4], m[8], m[12]], [m[1], m[5], m[9], m[13]], [m[2], m[6], m[10], m[14]], [0, 0, 0, 1]]
    t = node.get('translation', [0, 0, 0])
    q = node.get('rotation', [0, 0, 0, 1])
    s = node.get('scale', [1, 1, 1])
    return w3d.mat_from_trs(t, q, s)


def world_matrices(js):
    nodes = js.get('nodes', [])
    parent = {}
    for i, n in enumerate(nodes):
        for c in n.get('children', []):
            parent[c] = i
    cache = {}

    def world(i):
        if i not in cache:
            m = node_matrix(nodes[i])
            cache[i] = m if i not in parent else w3d.mat_mul(world(parent[i]), m)
        return cache[i]
    return [world(i) for i in range(len(nodes))]


def norm_name(s):
    return re.sub(r'\.\d{3}$', '', s).lower()


def main():
    args = sys.argv[1:]
    if '--list' in args:
        chunks = w3d.read_file(args[0])
        for c in chunks:
            if c.type == w3d.MESH:
                g = w3d.read_mesh(c)
                print('%-40s tris %5d  verts %5d  texture %s%s' % ((g.container + '.' + g.name) if g.container else g.name, len(g.tris),
                                                                  len(g.positions), g.texture, '  (skinned)' if g.skinned else ''))
        return 0
    if len(args) != 3:
        print(__doc__)
        return 1
    glb, src, dst = args
    js, binary = read_glb(glb)
    chunks = w3d.read_file(src)
    pivots = []
    for c in chunks:
        if c.type == w3d.HIERARCHY:
            pivots = w3d.read_pivots(c)
    bones = w3d.hlod_bones(chunks)
    worlds = world_matrices(js)
    # glTF (x, z, -y) back to the game's axes: the inverse of w3d_to_glb
    to_game = [[1, 0, 0, 0], [0, 0, -1, 0], [0, 1, 0, 0], [0, 0, 0, 1]]

    targets = {}
    for c in chunks:
        if c.type == w3d.MESH:
            g = w3d.read_mesh(c)
            full = (g.container + '.' + g.name) if g.container else g.name
            targets[norm_name(full)] = (c, g, full)
            targets.setdefault(norm_name(g.name), (c, g, full))
    done = set()
    for i, node in enumerate(js.get('nodes', [])):
        if 'mesh' not in node:
            continue
        key = norm_name(node.get('name', ''))
        if key not in targets:
            key = norm_name(js['meshes'][node['mesh']].get('name', ''))
        if key not in targets:
            print('skipped "%s": no mesh with that name in %s' % (node.get('name'), os.path.basename(src)))
            continue
        chunk, g, full = targets[key]
        if g.skinned:
            print('skipped "%s": skinned meshes cannot be written back' % full)
            continue
        bone = bones.get(full)
        if bone is None:
            for pi, pv in enumerate(pivots):
                if pv.name.lower() == g.name.lower():
                    bone = pi
        pivot_inv = w3d.mat_inverse_rigid(pivots[bone].world) if bone is not None and bone < len(pivots) else w3d.mat_identity()
        m = w3d.mat_mul(pivot_inv, w3d.mat_mul(to_game, worlds[i]))
        pos, nrm, uvs, tris = [], [], [], []
        for prim in js['meshes'][node['mesh']]['primitives']:
            if prim.get('mode', 4) != 4:
                print('skipped a non-triangle primitive of "%s"' % full)
                continue
            attrs = prim['attributes']
            p = accessor(js, binary, attrs['POSITION'])
            n = accessor(js, binary, attrs['NORMAL']) if 'NORMAL' in attrs else [(0, 0, 1)] * len(p)
            uv = accessor(js, binary, attrs['TEXCOORD_0']) if 'TEXCOORD_0' in attrs else [(0.0, 0.0)] * len(p)
            idx = accessor(js, binary, prim['indices']) if 'indices' in prim else list(range(len(p)))
            base = len(pos)
            pos += [w3d.xform_point(m, v) for v in p]
            nrm += [w3d.normalize(w3d.xform_dir(m, v)) for v in n]
            uvs += [tuple(v) for v in uv]
            tris += [(base + idx[k], base + idx[k + 1], base + idx[k + 2]) for k in range(0, len(idx) - 2, 3)]
        if not pos or not tris:
            continue
        old = (len(g.tris), len(g.positions))
        notes = w3d.write_mesh_geometry(chunk, pos, nrm, uvs, tris)
        done.add(full)
        print('%-40s %d tris / %d verts  (was %d / %d)  texture %s %s' % (full, len(tris), len(pos), old[0], old[1], g.texture, '; '.join(notes)))
    if not done:
        print('nothing was replaced: no mesh name of the .glb matches a mesh of the W3D (use --list to see the names)')
        return 2
    w3d.write_file(dst, chunks)
    print('wrote %s' % dst)
    return 0


if __name__ == '__main__':
    sys.exit(main())
