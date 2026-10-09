#!/usr/bin/env python3
"""Converts W3D models to binary glTF (.glb) so Blender can open them without a W3D plugin. Names are kept exactly as the game has them.

  w3d_to_glb.py <file.W3D | folder of .W3D files> <output folder> [--textures <folder with the DDS/TGA files>]

For every W3D file one .glb is written:
  * one node per skeleton pivot (bone), named as in the game, in the game's hierarchy
  * one node per mesh, named "CONTAINER.MESH" as the game names it, below the pivot it is attached to
  * the base texture of each mesh (converted once to PNG into <output folder>/textures_png, needs Pillow) as the material; material and image keep the game's texture name
  * node "extras" keep the source file and mesh name that glb_to_w3d.py uses to put an edited mesh back
The game's Z-up axes are converted to glTF's Y-up (Blender converts back on import, so models look upright in Blender).
Skinned meshes (characters with vertex weights) are exported for reference; glb_to_w3d.py cannot write them back.
"""
import io
import json
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import w3dlib as w3d

try:
    from PIL import Image
except ImportError:
    Image = None


def to_gltf(v):             # W3D (x, y, z up) -> glTF (x, z, -y)
    return (v[0], v[2], -v[1])


class GlbBuilder:
    def __init__(self):
        self.bin = bytearray()
        self.views, self.accessors, self.images, self.textures, self.materials = [], [], [], [], []
        self.mat_by_name = {}

    def _view(self, data, target=None):
        while len(self.bin) % 4:
            self.bin.append(0)
        v = {'buffer': 0, 'byteOffset': len(self.bin), 'byteLength': len(data)}
        if target:
            v['target'] = target
        self.bin += data
        self.views.append(v)
        return len(self.views) - 1

    def accessor(self, data, comp, ctype, count, target, mn=None, mx=None):
        a = {'bufferView': self._view(data, target), 'componentType': comp, 'type': ctype, 'count': count}
        if mn is not None:
            a['min'], a['max'] = mn, mx
        self.accessors.append(a)
        return len(self.accessors) - 1

    def material(self, name, png_uri):
        if name in self.mat_by_name:
            return self.mat_by_name[name]
        m = {'name': name, 'pbrMetallicRoughness': {'metallicFactor': 0.0, 'roughnessFactor': 1.0}}
        if png_uri:
            self.images.append({'name': name, 'uri': png_uri})
            self.textures.append({'source': len(self.images) - 1})
            m['pbrMetallicRoughness']['baseColorTexture'] = {'index': len(self.textures) - 1}
        self.materials.append(m)
        self.mat_by_name[name] = len(self.materials) - 1
        return self.mat_by_name[name]


def find_texture(folder, name):
    if not folder or not name:
        return None
    base = os.path.splitext(name)[0]
    for ext in ('.dds', '.tga', '.DDS', '.TGA'):
        for cand in (base + ext, base.lower() + ext.lower()):
            p = os.path.join(folder, cand)
            if os.path.exists(p):
                return p
    return None


def texture_png(path, out_dir):
    """converts a DDS/TGA to PNG once into <out_dir>/textures_png and returns the relative uri"""
    if not path or Image is None:
        return None
    name = os.path.splitext(os.path.basename(path))[0] + '.png'
    dst = os.path.join(out_dir, 'textures_png', name)
    if not os.path.exists(dst):
        try:
            im = Image.open(path).convert('RGBA')
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            im.save(dst, 'PNG')
        except Exception:
            return None
    return 'textures_png/' + name


def convert(src, out_dir, tex_dir):
    chunks = w3d.read_file(src)
    pivots = []
    for c in chunks:
        if c.type == w3d.HIERARCHY:
            pivots = w3d.read_pivots(c)
    bones = w3d.hlod_bones(chunks)
    b = GlbBuilder()
    nodes, children = [], {}

    pivot_node = {}
    for i, pv in enumerate(pivots):
        q = pv.q
        qv = to_gltf(q[:3])
        n = {'name': pv.name, 'translation': list(to_gltf(pv.t)), 'rotation': [qv[0], qv[1], qv[2], q[3]],
             'extras': {'w3d_pivot': i}}
        nodes.append(n)
        pivot_node[i] = len(nodes) - 1
    for i, pv in enumerate(pivots):
        if pv.parent >= 0:
            children.setdefault(pivot_node[pv.parent], []).append(pivot_node[i])
    roots = [pivot_node[i] for i, pv in enumerate(pivots) if pv.parent < 0]

    meshes = []
    for c in chunks:
        if c.type != w3d.MESH:
            continue
        g = w3d.read_mesh(c)
        full = (g.container + '.' + g.name) if g.container else g.name
        bone = bones.get(full)
        if bone is None:
            for i, pv in enumerate(pivots):
                if pv.name.lower() == g.name.lower():
                    bone = i
                    break
        png = texture_png(find_texture(tex_dir, g.texture), out_dir)
        mat = b.material(g.texture or 'untextured', png)
        pos = [to_gltf(p) for p in g.positions]
        nrm = [to_gltf(n) for n in g.normals]
        if g.skinned:
            bone = None            # skinned vertices already live in model space
        a_pos = b.accessor(b''.join(struct.pack('<3f', *p) for p in pos), 5126, 'VEC3', len(pos), 34962,
                           [min(p[i] for p in pos) for i in range(3)], [max(p[i] for p in pos) for i in range(3)])
        a_nrm = b.accessor(b''.join(struct.pack('<3f', *n) for n in nrm), 5126, 'VEC3', len(nrm), 34962)
        a_uv = b.accessor(b''.join(struct.pack('<2f', *uv) for uv in g.uvs), 5126, 'VEC2', len(g.uvs), 34962)
        idx = [i for t in g.tris for i in t]
        a_idx = b.accessor(struct.pack('<%dI' % len(idx), *idx), 5125, 'SCALAR', len(idx), 34963)
        meshes.append({'name': full, 'primitives': [{'attributes': {'POSITION': a_pos, 'NORMAL': a_nrm, 'TEXCOORD_0': a_uv},
                                                      'indices': a_idx, 'material': mat}]})
        n = {'name': full, 'mesh': len(meshes) - 1,
             'extras': {'w3d_file': os.path.basename(src), 'w3d_mesh': g.name, 'w3d_container': g.container,
                        'w3d_bone': bone if bone is not None else -1, 'skinned': g.skinned}}
        nodes.append(n)
        ni = len(nodes) - 1
        if bone is not None and bone in pivot_node:
            children.setdefault(pivot_node[bone], []).append(ni)
        else:
            roots.append(ni)
    for pi, ch in children.items():
        nodes[pi]['children'] = ch
    gltf = {'asset': {'version': '2.0', 'generator': 'GeneralsZH tools/w3d_to_glb.py'},
            'scene': 0, 'scenes': [{'name': os.path.basename(src), 'nodes': roots}], 'nodes': nodes,
            'meshes': meshes, 'materials': b.materials, 'accessors': b.accessors, 'bufferViews': b.views,
            'buffers': [{'byteLength': len(b.bin)}]}
    if b.images:
        gltf['images'], gltf['textures'] = b.images, b.textures
    js = json.dumps(gltf, separators=(',', ':')).encode()
    js += b' ' * (-len(js) % 4)
    binary = bytes(b.bin) + b'\0' * (-len(b.bin) % 4)
    out = struct.pack('<III', 0x46546C67, 2, 12 + 8 + len(js) + 8 + len(binary))
    out += struct.pack('<II', len(js), 0x4E4F534A) + js + struct.pack('<II', len(binary), 0x004E4942) + binary
    os.makedirs(out_dir, exist_ok=True)
    dst = os.path.join(out_dir, os.path.splitext(os.path.basename(src))[0] + '.glb')
    open(dst, 'wb').write(out)
    return dst, len(meshes)


def main():
    args = sys.argv[1:]
    tex = None
    if '--textures' in args:
        i = args.index('--textures')
        tex = args[i + 1]
        del args[i:i + 2]
    if len(args) != 2:
        print(__doc__)
        return 1
    src, out = args
    files = [src] if os.path.isfile(src) else [os.path.join(r, f) for r, _, fs in os.walk(src) for f in sorted(fs) if f.lower().endswith('.w3d')]
    if Image is None:
        print('Pillow is not installed: models are exported without textures (pip install pillow).')
    for f in files:
        try:
            dst, n = convert(f, out, tex)
            print('%s: %d meshes' % (dst, n))
        except Exception as e:
            print('skipped %s: %s' % (f, e))
    return 0


if __name__ == '__main__':
    sys.exit(main())
