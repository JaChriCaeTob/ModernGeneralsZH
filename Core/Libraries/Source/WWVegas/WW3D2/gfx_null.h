#pragma once

// Null graphics backend: see gfx_null.cpp. Selected by setting the environment variable GENERALS_GFX=null.

#include "gfx_d3d8_map.h"

bool NullGfx_Requested();
IDirect3D8* WINAPI NullGfx_Direct3DCreate8(UINT sdkVersion);

// shared by the Direct3D object and the device
UINT NullGfx_ModeCount();
HRESULT NullGfx_Mode(UINT index, D3DDISPLAYMODE* mode);
HRESULT NullGfx_GetDesktopMode(D3DDISPLAYMODE* mode);
HRESULT NullGfx_FillCaps(UINT adapter, D3DCAPS8* caps);
