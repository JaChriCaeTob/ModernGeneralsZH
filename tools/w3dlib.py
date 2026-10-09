"""Reading and writing the parts of Westwood's W3D model format that the model tools need (stdlib only).

A W3D file is a sequence of chunks: u32 type, u32 size (the top bit set means the chunk contains sub chunks), payload.
Only what is needed to move mesh geometry in and out is interpreted; every other chunk is carried through unchanged.
"""
import math
import struct

# chunk ids
MESH = 0x00
VERTICES = 0x02
VERTEX_NORMALS = 0x03
VERTEX_INFLUENCES = 0x0E
MESH_HEADER3 = 0x1F
TRIANGLES = 0x20
VERTEX_SHADE_INDICES = 0x22
VERTEX_MATERIAL_IDS = 0x39
TEXTURES = 0x30
TEXTURE = 0x31
TEXTURE_NAME = 0x32
MATERIAL_PASS = 0x38
DCG = 0x3B
DIG = 0x3C
SCG = 0x3E
TEXTURE_STAGE = 0x48
STAGE_TEXCOORDS = 0x4A
AABTREE = 0x50
HIERARCHY = 0x100
HIERARCHY_HEADER = 0x101
PIVOTS = 0x102
HLOD = 0x700
HLOD_LOD_ARRAY = 0x702
HLOD_SUB_OBJECT = 0x704


class Chunk:
    def __init__(self, ctype, data=None, children=None):
        self.type = ctype
        self.data = data            # bytes for leaf chunks
        self.children = children    # list of Chunk for container chunks (None for leaves)

    def find(self, ctype):
        return [c for c in (self.children or []) if c.type == ctype]

    def first(self, ctype):
        for c in self.children or []:
            if c.type == ctype:
                return c
        return None

    def to_bytes(self):
        if self.children is not None:
            payload = b''.join(c.to_bytes() for c in self.children)
            size = len(payload) | 0x80000000
        else:
            payload = self.data
            size = len(payload)
        return struct.pack('<II', self.type, size) + payload


def parse(data, pos=0, end=None):
    end = len(data) if end is None else end
    out = []
    while pos + 8 <= end:
        t, s = struct.unpack_from('<II', data, pos)
        size = s & 0x7FFFFFFF
        body = pos + 8
        if s & 0x80000000:
            out.append(Chunk(t, children=parse(data, body, body + size)))
        else:
            out.append(Chunk(t, data=data[body:body + size]))
        pos = body + size
    return out


def read_file(path):
    return parse(open(path, 'rb').read())


def write_file(path, chunks):
    open(path, 'wb').write(b''.join(c.to_bytes() for c in chunks))


def cstr(b):
    return b.split(b'\0')[0].decode('latin-1')


def pad_name(s, n):
    b = s.encode('latin-1')[:n - 1]
    return b + b'\0' * (n - len(b))


# ---- matrices (row major, column vectors: p' = M * p) --------------------------------------------------------------

def mat_identity():
    return [[1, 0, 0, 0], [0, 1, 0, 0], [0, 0, 1, 0], [0, 0, 0, 1]]


def mat_mul(a, b):
    return [[sum(a[i][k] * b[k][j] for k in range(4)) for j in range(4)] for i in range(4)]


def mat_from_trs(t, q, s=(1, 1, 1)):
    x, y, z, w = q
    r = [[1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
         [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
         [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)]]
    return [[r[0][0] * s[0], r[0][1] * s[1], r[0][2] * s[2], t[0]],
            [r[1][0] * s[0], r[1][1] * s[1], r[1][2] * s[2], t[1]],
            [r[2][0] * s[0], r[2][1] * s[1], r[2][2] * s[2], t[2]],
            [0, 0, 0, 1]]


def mat_inverse_rigid(m):
    """inverse of a matrix made of rotation and translation (and uniform scale)"""
    sc2 = m[0][0] ** 2 + m[1][0] ** 2 + m[2][0] ** 2
    if sc2 == 0:
        sc2 = 1.0
    r = [[m[j][i] / sc2 for j in range(3)] for i in range(3)]
    t = [-(r[i][0] * m[0][3] + r[i][1] * m[1][3] + r[i][2] * m[2][3]) for i in range(3)]
    return [[r[0][0], r[0][1], r[0][2], t[0]], [r[1][0], r[1][1], r[1][2], t[1]], [r[2][0], r[2][1], r[2][2], t[2]], [0, 0, 0, 1]]


def xform_point(m, p):
    return (m[0][0] * p[0] + m[0][1] * p[1] + m[0][2] * p[2] + m[0][3],
            m[1][0] * p[0] + m[1][1] * p[1] + m[1][2] * p[2] + m[1][3],
            m[2][0] * p[0] + m[2][1] * p[1] + m[2][2] * p[2] + m[2][3])


def xform_dir(m, v):
    return (m[0][0] * v[0] + m[0][1] * v[1] + m[0][2] * v[2],
            m[1][0] * v[0] + m[1][1] * v[1] + m[1][2] * v[2],
            m[2][0] * v[0] + m[2][1] * v[1] + m[2][2] * v[2])


def normalize(v):
    l = math.sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2])
    return (v[0] / l, v[1] / l, v[2] / l) if l > 1e-12 else (0.0, 0.0, 1.0)


# ---- hierarchy ------------------------------------------------------------------------------------------------------

class Pivot:
    def __init__(self, name, parent, t, q):
        self.name, self.parent, self.t, self.q = name, parent, t, q
        self.world = None


def read_pivots(hier_chunk):
    p = hier_chunk.first(PIVOTS)
    out = []
    if not p:
        return out
    for i in range(len(p.data) // 60):
        name = cstr(p.data[i * 60:i * 60 + 16])
        parent, = struct.unpack_from('<i', p.data, i * 60 + 16)
        t = struct.unpack_from('<3f', p.data, i * 60 + 20)
        q = struct.unpack_from('<4f', p.data, i * 60 + 44)
        out.append(Pivot(name, parent, t, q))
    for i, pv in enumerate(out):
        local = mat_from_trs(pv.t, pv.q)
        pv.world = local if pv.parent < 0 else mat_mul(out[pv.parent].world, local)
    return out


def hlod_bones(chunks):
    """sub object name -> bone index of the first (highest detail) level of detail"""
    result = {}
    for h in chunks:
        if h.type != HLOD:
            continue
        arrays = h.find(HLOD_LOD_ARRAY)
        if arrays:
            for s in arrays[-1].find(HLOD_SUB_OBJECT):
                bone, = struct.unpack_from('<I', s.data, 0)
                result[cstr(s.data[4:36])] = bone
    return result


# ---- meshes ---------------------------------------------------------------------------------------------------------

class MeshGeo:
    def __init__(self):
        self.name = ''
        self.container = ''
        self.positions = []
        self.normals = []
        self.uvs = []        # first stage
        self.tris = []       # (i, j, k)
        self.surface = []    # surface type per triangle
        self.skinned = False
        self.texture = None


def read_mesh(mesh):
    g = MeshGeo()
    hdr = mesh.first(MESH_HEADER3)
    g.name = cstr(hdr.data[8:24])
    g.container = cstr(hdr.data[24:40])
    g.skinned = mesh.first(VERTEX_INFLUENCES) is not None
    v = mesh.first(VERTICES)
    n = mesh.first(VERTEX_NORMALS)
    g.positions = [struct.unpack_from('<3f', v.data, 12 * i) for i in range(len(v.data) // 12)]
    g.normals = [struct.unpack_from('<3f', n.data, 12 * i) for i in range(len(n.data) // 12)] if n else [(0, 0, 1)] * len(g.positions)
    t = mesh.first(TRIANGLES)
    for i in range(len(t.data) // 32):
        a, b, c, attr = struct.unpack_from('<4I', t.data, 32 * i)
        g.tris.append((a, b, c))
        g.surface.append(attr)
    for mp in mesh.find(MATERIAL_PASS):
        for st in mp.find(TEXTURE_STAGE):
            tc = st.first(STAGE_TEXCOORDS)
            if tc:
                g.uvs = [struct.unpack_from('<2f', tc.data, 8 * i) for i in range(len(tc.data) // 8)]
                break
        if g.uvs:
            break
    if not g.uvs:
        g.uvs = [(0.0, 0.0)] * len(g.positions)
    tex = mesh.first(TEXTURES)
    if tex:
        for tx in tex.find(TEXTURE):
            nm = tx.first(TEXTURE_NAME)
            if nm:
                g.texture = cstr(nm.data)
                break
    return g


def write_mesh_geometry(mesh, positions, normals, uvs, tris):
    """replaces the geometry of a MESH chunk, keeping its materials, shaders and names; returns a list of notes"""
    notes = []
    nv, nt = len(positions), len(tris)
    old_nv = len(mesh.first(VERTICES).data) // 12
    same_vertices = nv == old_nv
    old_t = mesh.first(TRIANGLES)
    attrs = [struct.unpack_from('<I', old_t.data, 32 * i + 12)[0] for i in range(len(old_t.data) // 32)]
    default_attr = max(set(attrs), key=attrs.count) if attrs else 0

    def set_data(ctype, data):
        for c in mesh.children:
            if c.type == ctype:
                c.data = data
                return c
        return None

    set_data(VERTICES, b''.join(struct.pack('<3f', *p) for p in positions))
    set_data(VERTEX_NORMALS, b''.join(struct.pack('<3f', *n) for n in normals))
    tb = []
    for (a, b, c) in tris:
        pa, pb, pc = positions[a], positions[b], positions[c]
        u = (pb[0] - pa[0], pb[1] - pa[1], pb[2] - pa[2])
        w = (pc[0] - pa[0], pc[1] - pa[1], pc[2] - pa[2])
        nrm = normalize((u[1] * w[2] - u[2] * w[1], u[2] * w[0] - u[0] * w[2], u[0] * w[1] - u[1] * w[0]))
        dist = nrm[0] * pa[0] + nrm[1] * pa[1] + nrm[2] * pa[2]
        tb.append(struct.pack('<4I4f', a, b, c, attrs[len(tb)] if len(attrs) == nt else default_attr, nrm[0], nrm[1], nrm[2], dist))
    set_data(TRIANGLES, b''.join(tb))
    if not same_vertices:
        set_data(VERTEX_SHADE_INDICES, struct.pack('<%dI' % nv, *range(nv)))
    for mp in mesh.find(MATERIAL_PASS):
        for c in mp.children:
            if c.type in (DCG, DIG) and not same_vertices:
                c.data = b'\xff\xff\xff\xff' * nv        # vertex colours of new vertices: white
            elif c.type == SCG and not same_vertices:
                c.data = b'\x00\x00\x00\x00' * nv
            elif c.type == TEXTURE_STAGE:
                for sc in c.children:
                    if sc.type == STAGE_TEXCOORDS:
                        sc.data = b''.join(struct.pack('<2f', *uv) for uv in uvs)
    # header: counts and bounds
    hdr = mesh.first(MESH_HEADER3)
    d = bytearray(hdr.data)
    struct.pack_into('<II', d, 40, nt, nv)
    mn = [min(p[i] for p in positions) for i in range(3)]
    mx = [max(p[i] for p in positions) for i in range(3)]
    ctr = [(mn[i] + mx[i]) * 0.5 for i in range(3)]
    rad = max(math.sqrt(sum((p[i] - ctr[i]) ** 2 for i in range(3))) for p in positions)
    struct.pack_into('<6f', d, 76, *(mn + mx))
    struct.pack_into('<4f', d, 100, ctr[0], ctr[1], ctr[2], rad)
    hdr.data = bytes(d)
    # chunks that depend on the old topology cannot be kept
    keep = []
    for c in mesh.children:
        if c.type == AABTREE:
            notes.append('collision tree (AABTREE) removed')
            continue
        keep.append(c)
    mesh.children = keep
    return notes
