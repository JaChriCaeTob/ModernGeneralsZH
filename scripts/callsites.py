import os, re, collections

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SCAN = ['Core', os.path.join('GeneralsMD', 'Code')]

# direct calls on a raw device/texture/buffer pointer
DEV = r'(?:_Get_D3D_Device8\(\)|m_pDev|D3DDevice|pDev|m_pD3DDevice|dev|device)'
direct_re = re.compile(DEV + r'\s*->\s*([A-Za-z0-9_]+)\s*\(')
macro_re = re.compile(r'\bDX8CALL\(\s*([A-Za-z0-9_]+)\s*\(')
# calls that already go through the wrapper (still tied to D3D8 enums, but one seam to change)
wrapper_re = re.compile(r'\bDX8Wrapper::(Set_DX8_[A-Za-z_]+|_Get_D3D_Device8|_Get_DX8_[A-Za-z_]+|_Set_DX8_[A-Za-z_]+|Set_DX8_Render_State)\b')
wrapper_states_re = re.compile(r'\bD3DRS_[A-Z0-9_]+|\bD3DTSS_[A-Z0-9_]+')

def is_wrapper_internal(path):
    p = path.replace('\\', '/').lower()
    names = ('dx8wrapper', 'dx8vertexbuffer', 'dx8indexbuffer', 'dx8fvf', 'dx8caps', 'dx8texman', 'dx8renderer',
             'dx8polygonrenderer', 'dx8webbrowser', 'dx8list', 'dx8snapshot', 'shader.', 'vertmaterial', 'texture.', 'textureloader',
             'surfaceclass', 'ddsfile', 'ww3d.', 'render2d', 'dx8mesh', 'dx8tex', 'texturefilter', 'dx8wrapper')
    base = p.split('/')[-1]
    return any(base.startswith(n) or n in base for n in names)

def area_of(path):
    p = path.replace('\\', '/').lower()
    base = p.split('/')[-1]
    rules = [
        ('Water', ('w3dwater', 'water')),
        ('Shadows', ('shadow',)),
        ('Terrain / map buffers', ('heightmap', 'terrain', 'w3droad', 'w3dbridge', 'w3dtree', 'w3dprop', 'w3dbib', 'w3dwaypoint', 'w3dscorch', 'w3dmouse', 'tracks', 'w3dsmudge', 'bridge')),
        ('Fog of war (shroud)', ('shroud',)),
        ('Post effects / shader manager', ('w3dshadermanager', 'shaderman', 'w3dsmudge', 'shockwave')),
        ('Particles, lines, sprites', ('pointgr', 'particle', 'line', 'sphere', 'sortingrenderer', 'dynamesh', 'w3dparticle')),
        ('2D / UI / video', ('render2d', 'w3ddisplay', 'w3dgadget', 'w3dvideo', 'w3dgui', 'w3dgamewindow', 'w3dprogress', 'font3d', 'textdraw', 'w3dstatictext')),
        ('Scene / view / camera', ('w3dview', 'w3dscene', 'w3dcustomscene', 'camera', 'scene.', 'w3dstatus')),
        ('Models / materials / textures', ('mesh', 'w3dmodel', 'texture', 'vertmaterial', 'assetmgr', 'w3dasset', 'ddsfile', 'surfaceclass', 'shader.', 'w3dmodeldraw', 'hlod', 'matinfo')),
        ('Device / window / startup', ('dx8wrapper', 'dx8caps', 'dx8texman', 'winmain', 'w3dgame', 'w3d_')),
    ]
    for name, keys in rules:
        if any(k in base for k in keys):
            return name
    return 'Other'

direct = collections.defaultdict(lambda: collections.Counter())
wrapper = collections.Counter()
states = collections.defaultdict(collections.Counter)
files_all = set()
for scan in SCAN:
    for dp, dn, fn in os.walk(os.path.join(ROOT, scan)):
        if os.sep + 'Tools' + os.sep in dp + os.sep:
            continue
        for f in fn:
            if not f.endswith(('.cpp', '.h', '.inl')):
                continue
            path = os.path.join(dp, f)
            try:
                text = open(path, encoding='latin-1').read()
            except Exception:
                continue
            rel = os.path.relpath(path, ROOT)
            hit = False
            for m in direct_re.finditer(text):
                direct[rel][m.group(1)] += 1; hit = True
            for m in macro_re.finditer(text):
                direct[rel]['DX8CALL:' + m.group(1)] += 1; hit = True
            for m in wrapper_re.finditer(text):
                wrapper[rel] += 1; hit = True
            for m in wrapper_states_re.finditer(text):
                states[rel][m.group(0)] += 1; hit = True
            if hit:
                files_all.add(rel)

def kind(method):
    m = method.replace('DX8CALL:', '')
    if m.startswith(('SetRenderState', 'GetRenderState')): return 'render state'
    if m.startswith(('SetTextureStageState', 'GetTextureStageState')): return 'texture stage state (fixed function)'
    if m.startswith(('SetTexture', 'GetTexture')): return 'bind texture'
    if m.startswith(('SetPixelShader', 'CreatePixelShader', 'DeletePixelShader', 'SetPixelShaderConstant', 'GetPixelShader')): return 'pixel shader'
    if m.startswith(('SetVertexShader', 'CreateVertexShader', 'DeleteVertexShader', 'SetVertexShaderConstant', 'GetVertexShader')): return 'vertex shader / FVF'
    if m.startswith(('SetTransform', 'GetTransform', 'MultiplyTransform')): return 'transforms'
    if m.startswith(('SetLight', 'LightEnable', 'GetLight', 'SetMaterial', 'GetMaterial')): return 'lights / material'
    if m.startswith(('Draw',)): return 'draw calls'
    if m.startswith(('SetStreamSource', 'SetIndices', 'GetStreamSource')): return 'bind vertex/index buffers'
    if m.startswith(('Create', 'CopyRects', 'UpdateTexture', 'GetBackBuffer', 'GetRenderTarget', 'SetRenderTarget', 'GetDepthStencilSurface', 'Clear', 'Present', 'Reset', 'TestCooperativeLevel', 'GetViewport', 'SetViewport', 'SetClipPlane', 'GetDeviceCaps', 'GetDisplayMode', 'BeginScene', 'EndScene', 'SetGammaRamp')): return 'resources / targets / device'
    if m.startswith(('Lock', 'Unlock', 'GetLevelDesc', 'GetSurfaceLevel', 'GetDesc', 'AddRef', 'Release', 'GetDevice')): return 'lock / description / refcount'
    return 'other (' + m + ')'

# split into backend (the wrapper itself) and bypass (everything else)
backend, bypass = {}, {}
for rel, c in direct.items():
    (backend if is_wrapper_internal(rel) else bypass)[rel] = c

def total(d): return sum(sum(c.values()) for c in d.values())

out = []
w = out.append
w('# Render call site inventory\n')
w('Generated by `scripts/callsites.py`, which scans the source tree (regex, so close but not exact). It lists every place the game talks to')
w('Direct3D 8 directly, to plan routing everything through one backend interface. "Wrapper internal" files already *are* the seam; "bypass" files')
w('call the device (or Direct3D types) without going through it and are the real work.\n')
w('## Totals\n')
w('| Measure | Count |')
w('|---|---|')
w('| Source files that reference D3D8 devices, states or enums | %d |' % len(files_all))
w('| Direct device/texture/buffer method calls, wrapper internal files | %d in %d files |' % (total(backend), len(backend)))
w('| Direct device/texture/buffer method calls, **bypass** (outside the wrapper) | %d in %d files |' % (total(bypass), len(bypass)))
w('| Calls through `DX8Wrapper::*DX8*` helpers (already one seam, still D3D8 enums) | %d in %d files |' % (sum(wrapper.values()), len(wrapper)))
w('')

w('## Bypass calls by subsystem (the work list)\n')
bya = collections.defaultdict(lambda: [0, set()])
for rel, c in bypass.items():
    a = area_of(rel)
    bya[a][0] += sum(c.values()); bya[a][1].add(rel)
w('| Subsystem | Direct calls | Files |')
w('|---|---:|---:|')
for a, (n, fs) in sorted(bya.items(), key=lambda kv: -kv[1][0]):
    w('| %s | %d | %d |' % (a, n, len(fs)))
w('')

w('## Bypass calls by kind\n')
kinds = collections.Counter()
for rel, c in bypass.items():
    for meth, n in c.items():
        kinds[kind(meth)] += n
w('| Kind of call | Count |')
w('|---|---:|')
for k, n in kinds.most_common():
    w('| %s | %d |' % (k, n))
w('')

w('## Bypass files, largest first\n')
w('| File | Subsystem | Direct calls | Main kinds |')
w('|---|---|---:|---|')
for rel, c in sorted(bypass.items(), key=lambda kv: -sum(kv[1].values())):
    kk = collections.Counter()
    for meth, n in c.items(): kk[kind(meth)] += n
    top = ', '.join('%s %d' % (k.split(' (')[0], n) for k, n in kk.most_common(3))
    w('| `%s` | %s | %d | %s |' % (rel.replace('\\', '/'), area_of(rel), sum(c.values()), top))
w('')

w('## Wrapper internal files (the seam itself)\n')
w('| File | Direct calls |')
w('|---|---:|')
for rel, c in sorted(backend.items(), key=lambda kv: -sum(kv[1].values())):
    w('| `%s` | %d |' % (rel.replace('\\', '/'), sum(c.values())))
w('')

w('## Files using D3D8 render-state / texture-stage enums outside the wrapper\n')
w('These go through `Set_DX8_*` helpers, so only the enum values must be translated by the new backend, not the call sites.\n')
w('| File | `D3DRS_*` / `D3DTSS_*` uses |')
w('|---|---:|')
for rel, c in sorted(states.items(), key=lambda kv: -sum(kv[1].values())):
    if is_wrapper_internal(rel): continue
    if rel in bypass: pass
    w('| `%s` | %d |' % (rel.replace('\\', '/'), sum(c.values())))
w('')
open(os.path.join(ROOT, 'docs', 'RENDER_CALLSITES.md'), 'w', encoding='utf-8').write('\n'.join(out))

print('files with any D3D8 reference:', len(files_all))
print('backend calls', total(backend), 'in', len(backend), 'files; bypass calls', total(bypass), 'in', len(bypass), 'files; wrapper helper uses', sum(wrapper.values()))
print()
for a, (n, fs) in sorted(bya.items(), key=lambda kv: -kv[1][0]):
    print('%-34s %4d calls %3d files' % (a, n, len(fs)))
print()
for k, n in kinds.most_common():
    print('%-40s %d' % (k, n))
print()
for rel, c in sorted(bypass.items(), key=lambda kv: -sum(kv[1].values()))[:15]:
    print('%4d %s' % (sum(c.values()), rel.replace('\\', '/')))
