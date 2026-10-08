#pragma once

// In-memory Direct3D 8 resources shared by the null backend and the Vulkan backend: surfaces, textures and buffers that keep their
// content in system memory. A GPU backend uploads from these; the null backend just stores them.

#include "WWLib/always.h"
#include "gfx_d3d8_map.h"
#include "gfx_null.h"
#include <cstdio>
#include <vector>
#include <algorithm>
#include <cstring>

#define NULLGFX_UNUSED_PARAMS
#include "gfx_null_stubs.inl"

#define NULLGFX_REFCOUNT \
	STDMETHOD(QueryInterface)(THIS_ REFIID, void** ppv) override { if (ppv) *ppv = nullptr; return E_NOINTERFACE; } \
	STDMETHOD_(ULONG, AddRef)(THIS) override { return ++m_refs; } \
	STDMETHOD_(ULONG, Release)(THIS) override { ULONG r = --m_refs; if (r == 0) delete this; return r; } \
	ULONG m_refs = 1;

namespace gfxmem
{

// ---- format helpers -------------------------------------------------------------------------------------------------------

inline bool IsBlockFormat(D3DFORMAT f)
{
	return f == D3DFMT_DXT1 || f == D3DFMT_DXT2 || f == D3DFMT_DXT3 || f == D3DFMT_DXT4 || f == D3DFMT_DXT5;
}

inline UINT BytesPerPixel(D3DFORMAT f)
{
	switch (f)
	{
	case D3DFMT_A8R8G8B8: case D3DFMT_X8R8G8B8: case D3DFMT_D24S8: case D3DFMT_D24X8: case D3DFMT_D32:
	case D3DFMT_X8L8V8U8: case D3DFMT_Q8W8V8U8: case D3DFMT_V16U16: case D3DFMT_W11V11U10:
		return 4;
	case D3DFMT_R8G8B8:
		return 3;
	case D3DFMT_R5G6B5: case D3DFMT_X1R5G5B5: case D3DFMT_A1R5G5B5: case D3DFMT_A4R4G4B4: case D3DFMT_A8L8:
	case D3DFMT_V8U8: case D3DFMT_L6V5U5: case D3DFMT_D16: case D3DFMT_D16_LOCKABLE: case D3DFMT_D15S1: case D3DFMT_A8P8:
	case D3DFMT_X4R4G4B4: case D3DFMT_A8R3G3B2:
		return 2;
	case D3DFMT_R3G3B2: case D3DFMT_A8: case D3DFMT_P8: case D3DFMT_L8: case D3DFMT_A4L4:
		return 1;
	default:
		return 4;
	}
}

// bytes of one row of pixels (or of one row of 4x4 blocks) and the number of rows
inline UINT RowPitch(D3DFORMAT f, UINT width)
{
	if (IsBlockFormat(f))
		return ((width + 3) / 4) * (f == D3DFMT_DXT1 ? 8 : 16);
	return width * BytesPerPixel(f);
}

inline UINT RowCount(D3DFORMAT f, UINT height)
{
	return IsBlockFormat(f) ? (height + 3) / 4 : height;
}

// ---- resources ------------------------------------------------------------------------------------------------------------

class NullSurface : public IDirect3DSurface8NullBase
{
public:
	NullSurface(UINT width, UINT height, D3DFORMAT format, DWORD usage, D3DPOOL pool, IDirect3DDevice8* device)
		: m_width(width), m_height(height), m_format(format), m_usage(usage), m_pool(pool), m_device(device), m_ownerDirty(nullptr)
	{
		m_pitch = RowPitch(format, width);
		m_data.assign((size_t)m_pitch * RowCount(format, height), 0);
	}
	NULLGFX_REFCOUNT
	STDMETHOD(GetDevice)(THIS_ IDirect3DDevice8** ppDevice) override { if (ppDevice) { *ppDevice = m_device; if (m_device) m_device->AddRef(); } return D3D_OK; }
	STDMETHOD(GetContainer)(THIS_ REFIID, void** ppContainer) override { if (ppContainer) *ppContainer = nullptr; return E_NOINTERFACE; }
	STDMETHOD(GetDesc)(THIS_ D3DSURFACE_DESC* pDesc) override
	{
		pDesc->Format = m_format; pDesc->Type = D3DRTYPE_SURFACE; pDesc->Usage = m_usage; pDesc->Pool = m_pool;
		pDesc->Size = (UINT)m_data.size(); pDesc->MultiSampleType = D3DMULTISAMPLE_NONE; pDesc->Width = m_width; pDesc->Height = m_height;
		return D3D_OK;
	}
	STDMETHOD(LockRect)(THIS_ D3DLOCKED_RECT* pLockedRect, CONST RECT* pRect, DWORD Flags) override
	{
		if (m_ownerDirty && !(Flags & GFX_LOCK_READONLY))
			*m_ownerDirty = true;
		BYTE* base = m_data.data();
		if (pRect && !IsBlockFormat(m_format))
			base += (size_t)pRect->top * m_pitch + (size_t)pRect->left * BytesPerPixel(m_format);
		pLockedRect->Pitch = (INT)m_pitch;
		pLockedRect->pBits = base;
		return D3D_OK;
	}
	STDMETHOD(UnlockRect)(THIS) override { return D3D_OK; }
	UINT m_width, m_height, m_pitch;
	D3DFORMAT m_format;
	DWORD m_usage;
	D3DPOOL m_pool;
	IDirect3DDevice8* m_device;
	std::vector<BYTE> m_data;
	bool* m_ownerDirty;		// set when the surface content may have changed (a texture level or a render target copy)
	bool m_dirty = true;
};

class NullTexture : public IDirect3DTexture8NullBase
{
public:
	NullTexture(UINT width, UINT height, UINT levels, DWORD usage, D3DFORMAT format, D3DPOOL pool, IDirect3DDevice8* device)
		: m_device(device)
	{
		if (levels == 0)
		{
			levels = 1;
			for (UINT w = width, h = height; w > 1 || h > 1; w = (w > 1 ? w / 2 : 1), h = (h > 1 ? h / 2 : 1))
				++levels;
		}
		UINT w = width, h = height;
		for (UINT i = 0; i < levels; ++i)
		{
			NullSurface* level = new NullSurface(w, h, format, usage, pool, device);
			level->m_ownerDirty = &m_dirty;
			m_levels.push_back(level);
			w = (w > 1 ? w / 2 : 1); h = (h > 1 ? h / 2 : 1);
		}
	}
	~NullTexture() { if (s_destroyHook) s_destroyHook(this); for (NullSurface* s : m_levels) s->Release(); }
	inline static void (*s_destroyHook)(NullTexture*) = nullptr;		// lets a GPU backend free what it created for this texture
	bool m_dirty = true;								// CPU data changed since the last upload
	void* m_gpu = nullptr;							// owned by the GPU backend
	NULLGFX_REFCOUNT
	STDMETHOD(GetDevice)(THIS_ IDirect3DDevice8** ppDevice) override { if (ppDevice) { *ppDevice = m_device; if (m_device) m_device->AddRef(); } return D3D_OK; }
	STDMETHOD_(D3DRESOURCETYPE, GetType)(THIS) override { return D3DRTYPE_TEXTURE; }
	STDMETHOD_(DWORD, GetLevelCount)(THIS) override { return (DWORD)m_levels.size(); }
	STDMETHOD(GetLevelDesc)(THIS_ UINT Level, D3DSURFACE_DESC* pDesc) override
	{
		if (Level >= m_levels.size()) return D3DERR_INVALIDCALL;
		m_levels[Level]->GetDesc(pDesc); pDesc->Type = D3DRTYPE_TEXTURE; return D3D_OK;
	}
	STDMETHOD(GetSurfaceLevel)(THIS_ UINT Level, IDirect3DSurface8** ppSurfaceLevel) override
	{
		if (Level >= m_levels.size()) return D3DERR_INVALIDCALL;
		m_levels[Level]->AddRef(); *ppSurfaceLevel = m_levels[Level]; return D3D_OK;
	}
	STDMETHOD(LockRect)(THIS_ UINT Level, D3DLOCKED_RECT* pLockedRect, CONST RECT* pRect, DWORD Flags) override
	{
		if (Level >= m_levels.size()) return D3DERR_INVALIDCALL;
		return m_levels[Level]->LockRect(pLockedRect, pRect, Flags);
	}
	STDMETHOD(UnlockRect)(THIS_ UINT) override { return D3D_OK; }
	IDirect3DDevice8* m_device;
	std::vector<NullSurface*> m_levels;
};

class NullVertexBuffer : public IDirect3DVertexBuffer8NullBase
{
public:
	NullVertexBuffer(UINT length, DWORD usage, DWORD fvf, D3DPOOL pool, IDirect3DDevice8* device)
		: m_length(length), m_usage(usage), m_fvf(fvf), m_pool(pool), m_device(device), m_data(length, 0) {}
	NULLGFX_REFCOUNT
	STDMETHOD(GetDevice)(THIS_ IDirect3DDevice8** ppDevice) override { if (ppDevice) { *ppDevice = m_device; if (m_device) m_device->AddRef(); } return D3D_OK; }
	STDMETHOD_(D3DRESOURCETYPE, GetType)(THIS) override { return D3DRTYPE_VERTEXBUFFER; }
	STDMETHOD(Lock)(THIS_ UINT OffsetToLock, UINT, BYTE** ppbData, DWORD) override { *ppbData = m_data.data() + OffsetToLock; return D3D_OK; }
	STDMETHOD(Unlock)(THIS) override { return D3D_OK; }
	STDMETHOD(GetDesc)(THIS_ D3DVERTEXBUFFER_DESC* pDesc) override
	{
		pDesc->Format = D3DFMT_VERTEXDATA; pDesc->Type = D3DRTYPE_VERTEXBUFFER; pDesc->Usage = m_usage; pDesc->Pool = m_pool;
		pDesc->Size = m_length; pDesc->FVF = m_fvf; return D3D_OK;
	}
	UINT m_length; DWORD m_usage, m_fvf; D3DPOOL m_pool; IDirect3DDevice8* m_device; std::vector<BYTE> m_data;
};

class NullIndexBuffer : public IDirect3DIndexBuffer8NullBase
{
public:
	NullIndexBuffer(UINT length, DWORD usage, D3DFORMAT format, D3DPOOL pool, IDirect3DDevice8* device)
		: m_length(length), m_usage(usage), m_format(format), m_pool(pool), m_device(device), m_data(length, 0) {}
	NULLGFX_REFCOUNT
	STDMETHOD(GetDevice)(THIS_ IDirect3DDevice8** ppDevice) override { if (ppDevice) { *ppDevice = m_device; if (m_device) m_device->AddRef(); } return D3D_OK; }
	STDMETHOD_(D3DRESOURCETYPE, GetType)(THIS) override { return D3DRTYPE_INDEXBUFFER; }
	STDMETHOD(Lock)(THIS_ UINT OffsetToLock, UINT, BYTE** ppbData, DWORD) override { *ppbData = m_data.data() + OffsetToLock; return D3D_OK; }
	STDMETHOD(Unlock)(THIS) override { return D3D_OK; }
	STDMETHOD(GetDesc)(THIS_ D3DINDEXBUFFER_DESC* pDesc) override
	{
		pDesc->Format = m_format; pDesc->Type = D3DRTYPE_INDEXBUFFER; pDesc->Usage = m_usage; pDesc->Pool = m_pool; pDesc->Size = m_length;
		return D3D_OK;
	}
	UINT m_length; DWORD m_usage; D3DFORMAT m_format; D3DPOOL m_pool; IDirect3DDevice8* m_device; std::vector<BYTE> m_data;
};

// ---- the device (state tracking and memory resources; draws nothing) -----------------------------------------------------------------------------------------------------------

class NullDevice : public IDirect3DDevice8NullBase
{
public:
	NullDevice(IDirect3D8* d3d, const D3DPRESENT_PARAMETERS& pp, HWND focus, UINT adapter, D3DDEVTYPE type, DWORD behavior)
		: m_d3d(d3d), m_pp(pp), m_focus(focus), m_adapter(adapter), m_type(type), m_behavior(behavior)
	{
		memset(m_renderStates, 0, sizeof(m_renderStates));
		memset(m_stageStates, 0, sizeof(m_stageStates));
		memset(&m_viewport, 0, sizeof(m_viewport));
		CreateTargets();
	}
	~NullDevice() { ReleaseTargets(); }
	NULLGFX_REFCOUNT

	void CreateTargets()
	{
		UINT w = m_pp.BackBufferWidth ? m_pp.BackBufferWidth : 800, h = m_pp.BackBufferHeight ? m_pp.BackBufferHeight : 600;
		D3DFORMAT fmt = m_pp.BackBufferFormat == D3DFMT_UNKNOWN ? D3DFMT_X8R8G8B8 : m_pp.BackBufferFormat;
		m_back = new NullSurface(w, h, fmt, D3DUSAGE_RENDERTARGET, D3DPOOL_DEFAULT, this);
		m_depth = new NullSurface(w, h, m_pp.AutoDepthStencilFormat == D3DFMT_UNKNOWN ? D3DFMT_D24S8 : m_pp.AutoDepthStencilFormat, 0, D3DPOOL_DEFAULT, this);
		m_target = m_back; m_back->AddRef();
		m_viewport.X = 0; m_viewport.Y = 0; m_viewport.Width = w; m_viewport.Height = h; m_viewport.MinZ = 0; m_viewport.MaxZ = 1;
	}
	void ReleaseTargets()
	{
		if (m_target) { m_target->Release(); m_target = nullptr; }
		if (m_back) { m_back->Release(); m_back = nullptr; }
		if (m_depth) { m_depth->Release(); m_depth = nullptr; }
	}

	STDMETHOD(TestCooperativeLevel)(THIS) override { return D3D_OK; }
	STDMETHOD_(UINT, GetAvailableTextureMem)(THIS) override { return 1024u * 1024u * 1024u; }
	STDMETHOD(GetDirect3D)(THIS_ IDirect3D8** ppD3D8) override { *ppD3D8 = m_d3d; m_d3d->AddRef(); return D3D_OK; }
	STDMETHOD(GetDeviceCaps)(THIS_ D3DCAPS8* pCaps) override { return NullGfx_FillCaps(m_adapter, pCaps); }
	STDMETHOD(GetDisplayMode)(THIS_ D3DDISPLAYMODE* pMode) override { return NullGfx_GetDesktopMode(pMode); }
	STDMETHOD(GetCreationParameters)(THIS_ D3DDEVICE_CREATION_PARAMETERS* p) override
	{
		p->AdapterOrdinal = m_adapter; p->DeviceType = m_type; p->hFocusWindow = m_focus; p->BehaviorFlags = m_behavior; return D3D_OK;
	}
	STDMETHOD(Reset)(THIS_ D3DPRESENT_PARAMETERS* pPresentationParameters) override
	{
		m_pp = *pPresentationParameters; ReleaseTargets(); CreateTargets(); return D3D_OK;
	}
	STDMETHOD(Present)(THIS_ CONST RECT*, CONST RECT*, HWND, CONST RGNDATA*) override
	{
		LogFrame();
		return D3D_OK;
	}
	void LogFrame()
	{
		// frame rate log, one line every 5 seconds: the speed of the game itself with no rendering at all
		++m_frames;
		const DWORD now = GetTickCount();
		if (m_lastLog == 0) m_lastLog = now;
		if (now - m_lastLog >= 5000)
		{
			if (FILE* f = fopen("gfx_null_fps.txt", "a"))
			{
				fprintf(f, "%.0f fps over %.1f s\n", m_frames * 1000.0 / (now - m_lastLog), (now - m_lastLog) / 1000.0);
				fclose(f);
			}
			m_frames = 0; m_lastLog = now;
		}
	}
	DWORD m_frames = 0, m_lastLog = 0;
	STDMETHOD(GetBackBuffer)(THIS_ UINT, D3DBACKBUFFER_TYPE, IDirect3DSurface8** pp) override { m_back->AddRef(); *pp = m_back; return D3D_OK; }
	STDMETHOD(CreateTexture)(THIS_ UINT w, UINT h, UINT levels, DWORD usage, D3DFORMAT fmt, D3DPOOL pool, IDirect3DTexture8** pp) override
	{
		*pp = new NullTexture(w, h, levels, usage, fmt, pool, this); return D3D_OK;
	}
	STDMETHOD(CreateVolumeTexture)(THIS_ UINT, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DVolumeTexture8** pp) override { *pp = nullptr; return D3DERR_NOTAVAILABLE; }
	STDMETHOD(CreateCubeTexture)(THIS_ UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DCubeTexture8** pp) override { *pp = nullptr; return D3DERR_NOTAVAILABLE; }
	STDMETHOD(CreateVertexBuffer)(THIS_ UINT len, DWORD usage, DWORD fvf, D3DPOOL pool, IDirect3DVertexBuffer8** pp) override
	{
		*pp = new NullVertexBuffer(len, usage, fvf, pool, this); return D3D_OK;
	}
	STDMETHOD(CreateIndexBuffer)(THIS_ UINT len, DWORD usage, D3DFORMAT fmt, D3DPOOL pool, IDirect3DIndexBuffer8** pp) override
	{
		*pp = new NullIndexBuffer(len, usage, fmt, pool, this); return D3D_OK;
	}
	STDMETHOD(CreateRenderTarget)(THIS_ UINT w, UINT h, D3DFORMAT fmt, D3DMULTISAMPLE_TYPE, BOOL, IDirect3DSurface8** pp) override
	{
		*pp = new NullSurface(w, h, fmt, D3DUSAGE_RENDERTARGET, D3DPOOL_DEFAULT, this); return D3D_OK;
	}
	STDMETHOD(CreateDepthStencilSurface)(THIS_ UINT w, UINT h, D3DFORMAT fmt, D3DMULTISAMPLE_TYPE, IDirect3DSurface8** pp) override
	{
		*pp = new NullSurface(w, h, fmt, 0, D3DPOOL_DEFAULT, this); return D3D_OK;
	}
	STDMETHOD(CreateImageSurface)(THIS_ UINT w, UINT h, D3DFORMAT fmt, IDirect3DSurface8** pp) override
	{
		*pp = new NullSurface(w, h, fmt, 0, D3DPOOL_SYSTEMMEM, this); return D3D_OK;
	}
	// copies a rectangle between two surfaces of the same format (block compressed formats must be block aligned)
	static void CopySurfaceRect(NullSurface* s, NullSurface* d, RECT src, int dx, int dy)
	{
		if (!s || !d || s->m_format != d->m_format)
			return;
		src.left = std::max<LONG>(src.left, 0); src.top = std::max<LONG>(src.top, 0);
		src.right = std::min<LONG>(src.right, (LONG)s->m_width); src.bottom = std::min<LONG>(src.bottom, (LONG)s->m_height);
		const int w = (int)(src.right - src.left), h = (int)(src.bottom - src.top);
		if (w <= 0 || h <= 0 || dx < 0 || dy < 0 || dx + w > (int)d->m_width || dy + h > (int)d->m_height)
			return;
		if (IsBlockFormat(s->m_format))
		{
			const UINT block = s->m_format == D3DFMT_DXT1 ? 8 : 16;
			for (int by = 0; by < (h + 3) / 4; ++by)
				memcpy(d->m_data.data() + (size_t)(dy / 4 + by) * d->m_pitch + (size_t)(dx / 4) * block,
					s->m_data.data() + (size_t)(src.top / 4 + by) * s->m_pitch + (size_t)(src.left / 4) * block, (size_t)((w + 3) / 4) * block);
		}
		else
		{
			const UINT bpp = BytesPerPixel(s->m_format);
			for (int y = 0; y < h; ++y)
				memcpy(d->m_data.data() + (size_t)(dy + y) * d->m_pitch + (size_t)dx * bpp,
					s->m_data.data() + (size_t)(src.top + y) * s->m_pitch + (size_t)src.left * bpp, (size_t)w * bpp);
		}
		if (d->m_ownerDirty)
			*d->m_ownerDirty = true;
	}
	STDMETHOD(CopyRects)(THIS_ IDirect3DSurface8* src, CONST RECT* rects, UINT count, IDirect3DSurface8* dst, CONST POINT* points) override
	{
		NullSurface* s = static_cast<NullSurface*>(src); NullSurface* d = static_cast<NullSurface*>(dst);
		if (!s || !d)
			return D3DERR_INVALIDCALL;
		if (!rects || count == 0)
		{
			RECT all = { 0, 0, (LONG)s->m_width, (LONG)s->m_height };
			CopySurfaceRect(s, d, all, 0, 0);
			return D3D_OK;
		}
		for (UINT i = 0; i < count; ++i)
			CopySurfaceRect(s, d, rects[i], points ? points[i].x : rects[i].left, points ? points[i].y : rects[i].top);
		return D3D_OK;
	}
	STDMETHOD(UpdateTexture)(THIS_ IDirect3DBaseTexture8* src, IDirect3DBaseTexture8* dst) override
	{
		NullTexture* s = static_cast<NullTexture*>(static_cast<IDirect3DTexture8*>(src));
		NullTexture* d = static_cast<NullTexture*>(static_cast<IDirect3DTexture8*>(dst));
		if (!s || !d)
			return D3DERR_INVALIDCALL;
		// the destination may have fewer levels than the source: match levels by size
		for (NullSurface* dl : d->m_levels)
			for (NullSurface* sl : s->m_levels)
				if (sl->m_width == dl->m_width && sl->m_height == dl->m_height)
				{
					CopySurfaceRect(sl, dl, RECT{ 0, 0, (LONG)sl->m_width, (LONG)sl->m_height }, 0, 0);
					break;
				}
		d->m_dirty = true;
		return D3D_OK;
	}
	STDMETHOD(GetFrontBuffer)(THIS_ IDirect3DSurface8*) override { return D3D_OK; }
	STDMETHOD(SetRenderTarget)(THIS_ IDirect3DSurface8* rt, IDirect3DSurface8*) override
	{
		IDirect3DSurface8* n = rt ? rt : m_back;
		n->AddRef(); if (m_target) m_target->Release(); m_target = n;
		return D3D_OK;
	}
	STDMETHOD(GetRenderTarget)(THIS_ IDirect3DSurface8** pp) override { m_target->AddRef(); *pp = m_target; return D3D_OK; }
	STDMETHOD(GetDepthStencilSurface)(THIS_ IDirect3DSurface8** pp) override { m_depth->AddRef(); *pp = m_depth; return D3D_OK; }
	STDMETHOD(SetViewport)(THIS_ CONST D3DVIEWPORT8* v) override { m_viewport = *v; return D3D_OK; }
	STDMETHOD(GetViewport)(THIS_ D3DVIEWPORT8* v) override { *v = m_viewport; return D3D_OK; }
	STDMETHOD(SetRenderState)(THIS_ D3DRENDERSTATETYPE s, DWORD v) override { if ((UINT)s < 256) m_renderStates[s] = v; return D3D_OK; }
	STDMETHOD(GetRenderState)(THIS_ D3DRENDERSTATETYPE s, DWORD* v) override { *v = (UINT)s < 256 ? m_renderStates[s] : 0; return D3D_OK; }
	STDMETHOD(SetTextureStageState)(THIS_ DWORD st, D3DTEXTURESTAGESTATETYPE t, DWORD v) override { if (st < 8 && (UINT)t < 64) m_stageStates[st][t] = v; return D3D_OK; }
	STDMETHOD(GetTextureStageState)(THIS_ DWORD st, D3DTEXTURESTAGESTATETYPE t, DWORD* v) override { *v = (st < 8 && (UINT)t < 64) ? m_stageStates[st][t] : 0; return D3D_OK; }
	STDMETHOD(ValidateDevice)(THIS_ DWORD* passes) override { if (passes) *passes = 1; return D3D_OK; }
	STDMETHOD(CreateVertexShader)(THIS_ CONST DWORD*, CONST DWORD*, DWORD* h, DWORD) override { *h = 0x80000000u | ++m_shaderCounter; return D3D_OK; }
	STDMETHOD(CreatePixelShader)(THIS_ CONST DWORD*, DWORD* h) override { *h = 0x80000000u | ++m_shaderCounter; return D3D_OK; }
	STDMETHOD(CreateStateBlock)(THIS_ D3DSTATEBLOCKTYPE, DWORD* t) override { *t = ++m_shaderCounter; return D3D_OK; }
	STDMETHOD(CreateAdditionalSwapChain)(THIS_ D3DPRESENT_PARAMETERS*, IDirect3DSwapChain8** pp) override { *pp = nullptr; return D3DERR_NOTAVAILABLE; }

	IDirect3D8* m_d3d;
	D3DPRESENT_PARAMETERS m_pp;
	HWND m_focus; UINT m_adapter; D3DDEVTYPE m_type; DWORD m_behavior;
	NullSurface* m_back = nullptr; NullSurface* m_depth = nullptr; IDirect3DSurface8* m_target = nullptr;
	D3DVIEWPORT8 m_viewport;
	DWORD m_renderStates[256];
	DWORD m_stageStates[8][64];
	DWORD m_shaderCounter = 0;
};


} // namespace gfxmem
