/*
**	Null graphics backend.
**
**	A complete implementation of the Direct3D 8 interfaces the engine talks to that keeps all state and resource data in system
**	memory and draws nothing. Selected with the environment variable GENERALS_GFX=null. Its purposes:
**	  * it proves that the interfaces behind DX8Wrapper are the whole boundary to the graphics API: the game has to start, load
**	    maps and run with only this code behind them;
**	  * it measures how fast the game itself (simulation, scene traversal, draw call generation) runs without any rendering;
**	  * it is the template for a native backend: another implementation of the same interfaces, for example on Vulkan.
**
**	The boilerplate base classes in gfx_null_stubs.inl are generated from d3d8.h by scripts/gen_null_stubs.py.
*/
#include "WWLib/always.h"
#include "gfx_null.h"
#include "gfx_mem.h"
#include <cstdio>

using namespace gfxmem;

namespace
{

// ---- the Direct3D object --------------------------------------------------------------------------------------------------

class NullD3D : public IDirect3D8NullBase
{
public:
	NULLGFX_REFCOUNT
	// 24 bit and palette textures are not offered: the engine falls back to a 32 bit format
	STDMETHOD(CheckDeviceFormat)(THIS_ UINT, D3DDEVTYPE, D3DFORMAT, DWORD, D3DRESOURCETYPE rtype, D3DFORMAT fmt) override
	{
		if (rtype == D3DRTYPE_TEXTURE && (fmt == D3DFMT_R8G8B8 || fmt == D3DFMT_P8 || fmt == D3DFMT_A8P8)) return D3DERR_NOTAVAILABLE;
		return D3D_OK;
	}
	STDMETHOD_(UINT, GetAdapterCount)(THIS) override { return 1; }
	STDMETHOD(GetAdapterIdentifier)(THIS_ UINT, DWORD, D3DADAPTER_IDENTIFIER8* id) override
	{
		memset(id, 0, sizeof(*id));
		strcpy(id->Driver, "null"); strcpy(id->Description, "Null graphics backend (no rendering)");
		return D3D_OK;
	}
	STDMETHOD_(UINT, GetAdapterModeCount)(THIS_ UINT) override { return NullGfx_ModeCount(); }
	STDMETHOD(EnumAdapterModes)(THIS_ UINT, UINT mode, D3DDISPLAYMODE* m) override { return NullGfx_Mode(mode, m); }
	STDMETHOD(GetAdapterDisplayMode)(THIS_ UINT, D3DDISPLAYMODE* m) override { return NullGfx_GetDesktopMode(m); }
	STDMETHOD(GetDeviceCaps)(THIS_ UINT adapter, D3DDEVTYPE, D3DCAPS8* caps) override { return NullGfx_FillCaps(adapter, caps); }
	STDMETHOD(CreateDevice)(THIS_ UINT adapter, D3DDEVTYPE type, HWND focus, DWORD behavior, D3DPRESENT_PARAMETERS* pp, IDirect3DDevice8** out) override
	{
		*out = new NullDevice(this, *pp, focus, adapter, type, behavior); return D3D_OK;
	}
};

} // namespace

// ---- shared helpers (declared in gfx_null.h) ------------------------------------------------------------------------------

namespace
{
const struct { UINT w, h; } kModes[] = { {640, 480}, {800, 600}, {1024, 768}, {1280, 720}, {1280, 1024}, {1600, 900}, {1600, 1200},
	{1920, 1080}, {1920, 1200}, {1920, 1440}, {2560, 1440}, {3840, 2160} };
const D3DFORMAT kModeFormats[] = { D3DFMT_X8R8G8B8, D3DFMT_R5G6B5 };
}

bool g_nullGfxAdvertiseShaders = true;

UINT NullGfx_ModeCount()
{
	return (UINT)(sizeof(kModes) / sizeof(kModes[0])) * 2;
}

HRESULT NullGfx_Mode(UINT index, D3DDISPLAYMODE* mode)
{
	if (index >= NullGfx_ModeCount()) return D3DERR_INVALIDCALL;
	mode->Width = kModes[index / 2].w; mode->Height = kModes[index / 2].h; mode->RefreshRate = 60; mode->Format = kModeFormats[index % 2];
	return D3D_OK;
}

HRESULT NullGfx_GetDesktopMode(D3DDISPLAYMODE* mode)
{
	mode->Width = (UINT)GetSystemMetrics(SM_CXSCREEN); mode->Height = (UINT)GetSystemMetrics(SM_CYSCREEN);
	mode->RefreshRate = 60; mode->Format = D3DFMT_X8R8G8B8;
	return D3D_OK;
}

HRESULT NullGfx_FillCaps(UINT adapter, D3DCAPS8* c)
{
	memset(c, 0, sizeof(*c));
	c->DeviceType = D3DDEVTYPE_HAL; c->AdapterOrdinal = adapter;
	c->Caps = 0; c->Caps2 = D3DCAPS2_CANRENDERWINDOWED | D3DCAPS2_DYNAMICTEXTURES; c->Caps3 = 0;
	c->PresentationIntervals = D3DPRESENT_INTERVAL_IMMEDIATE | D3DPRESENT_INTERVAL_ONE;
	c->CursorCaps = D3DCURSORCAPS_COLOR;
	c->DevCaps = 0xFFFFFFFFu & ~(D3DDEVCAPS_SEPARATETEXTUREMEMORIES);
	c->PrimitiveMiscCaps = 0xFFFFFFFFu; c->RasterCaps = 0xFFFFFFFFu; c->ZCmpCaps = 0xFFFFFFFFu;
	c->SrcBlendCaps = 0xFFFFFFFFu; c->DestBlendCaps = 0xFFFFFFFFu; c->AlphaCmpCaps = 0xFFFFFFFFu; c->ShadeCaps = 0xFFFFFFFFu;
	c->TextureCaps = D3DPTEXTURECAPS_ALPHA | D3DPTEXTURECAPS_PERSPECTIVE | D3DPTEXTURECAPS_MIPMAP | D3DPTEXTURECAPS_PROJECTED;
	c->TextureFilterCaps = 0xFFFFFFFFu; c->CubeTextureFilterCaps = 0; c->VolumeTextureFilterCaps = 0;
	c->TextureAddressCaps = 0xFFFFFFFFu; c->VolumeTextureAddressCaps = 0; c->LineCaps = 0xFFFFFFFFu;
	c->MaxTextureWidth = 4096; c->MaxTextureHeight = 4096; c->MaxVolumeExtent = 0; c->MaxTextureRepeat = 8192; c->MaxTextureAspectRatio = 4096;
	c->MaxAnisotropy = 16; c->MaxVertexW = 1e10f;
	c->GuardBandLeft = -8192.f; c->GuardBandTop = -8192.f; c->GuardBandRight = 8192.f; c->GuardBandBottom = 8192.f;
	c->ExtentsAdjust = 0; c->StencilCaps = 0xFFFFFFFFu; c->FVFCaps = 8 | D3DFVFCAPS_DONOTSTRIPELEMENTS; c->TextureOpCaps = 0xFFFFFFFFu;
	c->MaxTextureBlendStages = 8; c->MaxSimultaneousTextures = 8;
	c->VertexProcessingCaps = 0xFFFFFFFFu; c->MaxActiveLights = 8; c->MaxUserClipPlanes = 6; c->MaxVertexBlendMatrices = 4;
	c->MaxStreams = 16; c->MaxStreamStride = 255;
	if (g_nullGfxAdvertiseShaders)
	{
		c->VertexShaderVersion = D3DVS_VERSION(1, 1); c->MaxVertexShaderConst = 96;
		c->PixelShaderVersion = D3DPS_VERSION(1, 4); c->MaxPixelShaderValue = 8.0f;
	}
	else
	{
		c->VertexShaderVersion = D3DVS_VERSION(0, 0); c->MaxVertexShaderConst = 0;
		c->PixelShaderVersion = D3DPS_VERSION(0, 0); c->MaxPixelShaderValue = 0.0f;
	}
	c->MaxPrimitiveCount = 0xFFFF; c->MaxVertexIndex = 0xFFFFF;
	return D3D_OK;
}

IDirect3D8* WINAPI NullGfx_Direct3DCreate8(UINT)
{
	return new NullD3D();
}

bool NullGfx_Requested()
{
	char value[32] = {};
	DWORD n = GetEnvironmentVariableA("GENERALS_GFX", value, sizeof(value));
	return n > 0 && n < sizeof(value) && _stricmp(value, "null") == 0;
}
