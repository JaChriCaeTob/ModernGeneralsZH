#pragma once

// Native Vulkan backend (experimental): see gfx_vulkan.cpp. Selected by setting the environment variable GENERALS_GFX=vulkan.

#include "gfx_d3d8_map.h"

bool VkGfx_Requested();
IDirect3D8* WINAPI VkGfx_Direct3DCreate8(UINT sdkVersion);
void VkGfx_BeginScene3D(float sunX, float sunY, float sunZ);
bool VkGfx_ShadowMapsActive();
void VkGfx_EndScene3D();
