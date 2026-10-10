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
	// volumetric clouds (Options.ini keys Clouds, CloudShadows, CloudBase, CloudThickness, CloudCoverage, CloudDensity, CloudSpeed, CloudShadowStrength)
	bool clouds = false;				// visible clouds (off by default: cloud shadows are always on) when the camera is above the cloud layer
	bool cloudShadows = true;		// moving cloud shadows on the ground (replace the game's cloud texture)
	float cloudBase = 350.0f;		// height of the underside of the cloud layer (world units, the ground is mostly between 0 and 100)
	float cloudThickness = 120.0f;
	float cloudCoverage = 0.20f;		// 0 clear sky .. 1 overcast
	float cloudDensity = 0.85f;		// opacity of the clouds
	float cloudSpeed = 1.0f;			// drift speed
	float cloudShadowStrength = 0.85f;
};
void VkGfx_SetEffectToggles(bool softShadows, bool ambientOcclusion, bool bloom, bool fxaa);		// the options menu check boxes, applied live
bool VkGfx_CloudShadowsReplaceGameClouds();		// the game's own cloud texture on the terrain is switched off
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
