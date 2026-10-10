#pragma once

// Native Vulkan backend (experimental): see gfx_vulkan.cpp. Selected by setting the environment variable GENERALS_GFX=vulkan.

#include "gfx_d3d8_map.h"

// Settings from Options.ini, set by the engine before the renderer starts. Environment variables (GENERALS_GFX, GENERALS_POST, GENERALS_SHADOWS, ...) still override them.
struct VkGfx_Settings
{
	bool classic = false;			// GraphicsMode = classic: original rendering through Direct3D 8 / DXVK
	bool postProcessing = true;		// bloom, colour grading, ambient occlusion
	bool bloom = true, ambientOcclusion = true, fxaa = true, softShadows = true;
	float sunAzimuth = -1.0f, sunElevation = -1.0f;		// degrees; negative = random for every launch
};
void VkGfx_Configure(const VkGfx_Settings& s);		// may also be called while the game runs: the effect settings are applied live
bool VkGfx_NativeActive();				// true when the native renderer is the one in use
bool VkGfx_Requested();
IDirect3D8* WINAPI VkGfx_Direct3DCreate8(UINT sdkVersion);
void VkGfx_BeginScene3D(float sunX, float sunY, float sunZ);
bool VkGfx_ShadowMapsActive();
void VkGfx_SoftDraws(bool soft);			// following draws are particles: the native renderer fades them where they meet geometry
bool VkGfx_SoftDrawsActive();
void VkGfx_SuppressSceneDraws(bool suppress);		// the engine's own shadow draws to the back buffer are dropped while set
void VkGfx_EndScene3D();
