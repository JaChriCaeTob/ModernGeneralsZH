/*
**	Command & Conquer Generals Zero Hour(tm)
**
**	This program is free software: you can redistribute it and/or modify
**	it under the terms of the GNU General Public License as published by
**	the Free Software Foundation, either version 3 of the License, or
**	(at your option) any later version.
**
**	This program is distributed in the hope that it will be useful,
**	but WITHOUT ANY WARRANTY; without even the implied warranty of
**	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
**	GNU General Public License for more details.
**
**	You should have received a copy of the GNU General Public License
**	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "WW3D2/dx8wrapper.h"
#include "WW3D2/texture.h"
#include "Lib/BaseType.h"
#include "W3DDevice/GameClient/W3DShockwave.h"
#include "GameClient/Shockwave.h"
#include "GameClient/View.h"
#include "GameClient/Display.h"

#include <mmsystem.h>
#include <cmath>
#include <vector>

namespace
{

struct ScreenVertex
{
	float x, y, z, rhw;
	DWORD color;
	float u, v;
};

const DWORD kFvf = D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1;
const float kPi = 3.14159265f;

inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

struct ViewRect
{
	float x0, y0, x1, y1;   // drawable area in pixels (the tactical view)
	float texW, texH;       // size of the captured back buffer texture
};

ScreenVertex makeVertex(const ViewRect &r, float px, float py, float sx, float sy, float brightness)
{
	ScreenVertex v;
	v.x = clampf(px, r.x0, r.x1) - 0.5f;
	v.y = clampf(py, r.y0, r.y1) - 0.5f;
	v.z = 0.0f;
	v.rhw = 1.0f;
	const int c = (int)clampf(brightness * 255.0f, 0.0f, 255.0f);
	v.color = 0xff000000 | (c << 16) | (c << 8) | c;
	v.u = clampf(sx, r.x0, r.x1) / r.texW;
	v.v = clampf(sy, r.y0, r.y1) / r.texH;
	return v;
}

// A ground circle of the given world radius is an ellipse on screen. Its two basis vectors are the projected
// east and north offsets (in pixels), which also covers a rotated camera.
bool projectedBasis(const ShockwaveInstance &wave, float worldRadius, ICoord2D &center, float e1[2], float e2[2])
{
	if (!TheTacticalView->worldToScreen(&wave.pos, &center))
		return false;

	Coord3D east = wave.pos, north = wave.pos;
	east.x += worldRadius;
	north.y += worldRadius;
	ICoord2D se, sn;
	TheTacticalView->worldToScreenTriReturn(&east, &se);
	TheTacticalView->worldToScreenTriReturn(&north, &sn);

	e1[0] = (float)(se.x - center.x);
	e1[1] = (float)(se.y - center.y);
	e2[0] = (float)(sn.x - center.x);
	e2[1] = (float)(sn.y - center.y);
	return std::sqrt(e1[0] * e1[0] + e1[1] * e1[1]) > 2.0f || std::sqrt(e2[0] * e2[0] + e2[1] * e2[1]) > 2.0f;
}

TextureClass *g_capture = nullptr;
UnsignedInt g_captureW = 0, g_captureH = 0;

// (Re)creates the texture that receives a copy of the back buffer.
bool ensureCapture(UnsignedInt w, UnsignedInt h, WW3DFormat format)
{
	if (g_capture && g_captureW == w && g_captureH == h)
		return true;

	REF_PTR_RELEASE(g_capture);
	g_capture = MSGNEW("TextureClass") TextureClass(w, h, format, MIP_LEVELS_1, TextureClass::POOL_DEFAULT, true);
	g_captureW = w;
	g_captureH = h;
	return g_capture != nullptr;
}

} // namespace

Bool W3DShockwave::isActive()
{
	return TheShockwaves.isEnabled() && TheShockwaves.update();
}

void W3DShockwave::releaseResources()
{
	REF_PTR_RELEASE(g_capture);
	g_captureW = g_captureH = 0;
}

void W3DShockwave::render()
{
	SurfaceClass *backBuffer = DX8Wrapper::_Get_DX8_Back_Buffer();
	if (!backBuffer)
		return;

	SurfaceClass::SurfaceDescription bbDesc;
	backBuffer->Get_Description(bbDesc);

	if (!ensureCapture(bbDesc.Width, bbDesc.Height, bbDesc.Format))
	{
		REF_PTR_RELEASE(backBuffer);
		return;
	}

	SurfaceClass *captureSurface = g_capture->Get_Surface_Level();
	if (!captureSurface)
	{
		REF_PTR_RELEASE(backBuffer);
		return;
	}
	captureSurface->Copy(0, 0, 0, 0, bbDesc.Width, bbDesc.Height, backBuffer);
	REF_PTR_RELEASE(captureSurface);
	REF_PTR_RELEASE(backBuffer);


	Int ox, oy;
	TheTacticalView->getOrigin(&ox, &oy);
	ViewRect r;
	r.x0 = (float)ox;
	r.y0 = (float)oy;
	r.x1 = (float)(ox + TheTacticalView->getWidth());
	r.y1 = (float)(oy + TheTacticalView->getHeight());
	r.texW = (float)bbDesc.Width;
	r.texH = (float)bbDesc.Height;
	const float scale = r.texH / 1080.0f;

	// Put the W3D state tracking into a known state before drawing with the raw device.
	VertexMaterialClass *vmat = VertexMaterialClass::Get_Preset(VertexMaterialClass::PRELIT_DIFFUSE);
	DX8Wrapper::Set_Material(vmat);
	REF_PTR_RELEASE(vmat);
	DX8Wrapper::Set_Shader(ShaderClass::_PresetOpaqueShader);
	DX8Wrapper::Set_Texture(0, nullptr);
	DX8Wrapper::Apply_Render_State_Changes();
	DX8Wrapper::Set_DX8_Render_State(D3DRS_ZFUNC, D3DCMP_ALWAYS);
	DX8Wrapper::Set_DX8_Render_State(D3DRS_ZWRITEENABLE, FALSE);
	DX8Wrapper::Set_DX8_Render_State(D3DRS_ALPHABLENDENABLE, FALSE);
	DX8Wrapper::Set_DX8_Render_State(D3DRS_CULLMODE, D3DCULL_NONE);
	DX8Wrapper::Apply_Render_State_Changes();

	// Texture * diffuse * 2, so a diffuse of 0.5 grey leaves the scene unchanged.
	DX8Wrapper::Set_DX8_Texture_Stage_State(0, D3DTSS_COLOROP, D3DTOP_MODULATE2X);
	DX8Wrapper::Set_DX8_Texture_Stage_State(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
	DX8Wrapper::Set_DX8_Texture_Stage_State(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
	DX8Wrapper::Set_DX8_Texture_Stage_State(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2);
	DX8Wrapper::Set_DX8_Texture_Stage_State(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
	DX8Wrapper::Set_DX8_Texture_Stage_State(0, D3DTSS_ADDRESSU, D3DTADDRESS_CLAMP);
	DX8Wrapper::Set_DX8_Texture_Stage_State(0, D3DTSS_ADDRESSV, D3DTADDRESS_CLAMP);
	DX8Wrapper::Set_DX8_Texture_Stage_State(0, D3DTSS_MINFILTER, D3DTEXF_LINEAR);
	DX8Wrapper::Set_DX8_Texture_Stage_State(0, D3DTSS_MAGFILTER, D3DTEXF_LINEAR);
	DX8Wrapper::Set_DX8_Texture_Stage_State(0, D3DTSS_MIPFILTER, D3DTEXF_NONE);
	DX8Wrapper::Set_DX8_Texture_Stage_State(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
	DX8Wrapper::Set_DX8_Texture_Stage_State(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);

	DX8Wrapper::Raw_Set_Texture(0, g_capture->Peek_D3D_Texture());
	DX8Wrapper::Raw_Set_Vertex_Shader(kFvf);

	const UnsignedInt now = timeGetTime();
	static std::vector<ScreenVertex> verts;
	static std::vector<WORD> indices;

	for (Int i = 0; i < TheShockwaves.count(); ++i)
	{
		const ShockwaveInstance &wave = TheShockwaves.get(i);
		const float t = ShockwaveList::progress(wave, now);

		// The front decelerates like a real blast wave.
		const float front = 1.0f - std::pow(1.0f - t, 2.2f);
		const float worldRadius = wave.maxRadius * front;
		if (worldRadius < 1.0f)
			continue;

		ICoord2D center;
		float e1[2], e2[2];
		if (!projectedBasis(wave, worldRadius, center, e1, e2))
			continue;

		const float len1 = std::sqrt(e1[0] * e1[0] + e1[1] * e1[1]);
		const float len2 = std::sqrt(e2[0] * e2[0] + e2[1] * e2[1]);
		const float meanRadius = 0.5f * (len1 + len2);
		if (meanRadius < 2.0f)
			continue;

		const float fade = std::pow(1.0f - t, 1.3f);
		const float rampIn = clampf(t * 8.0f, 0.0f, 1.0f);
		const float life = fade * rampIn;
		const float amplitude = wave.strength * life * 18.0f * scale;
		if (amplitude < 0.25f)
			continue;

		// Band half width relative to the ring radius.
		const float relWidth = std::max(0.22f, 12.0f * scale / meanRadius);

		const int angular = (int)clampf(meanRadius * 2.0f, 48.0f, 200.0f);
		const int radial = 24;
		const float range = 2.4f;

		verts.clear();
		verts.reserve((size_t)(angular + 1) * (radial + 1));
		for (int a = 0; a <= angular; ++a)
		{
			const float theta = 2.0f * kPi * (float)a / (float)angular;
			const float c = std::cos(theta), s = std::sin(theta);
			const float dirX = c * e1[0] + s * e2[0];
			const float dirY = c * e1[1] + s * e2[1];
			const float dirLen = std::sqrt(dirX * dirX + dirY * dirY);
			const float ux = dirLen > 1e-3f ? dirX / dirLen : 0.0f;
			const float uy = dirLen > 1e-3f ? dirY / dirLen : 0.0f;

			for (int j = 0; j <= radial; ++j)
			{
				const float x = -range + 2.0f * range * (float)j / (float)radial;
				const float k = std::max(0.0f, 1.0f + x * relWidth);
				const float px = (float)center.x + k * dirX;
				const float py = (float)center.y + k * dirY;

				// Odd profile: pushes the picture outward in front of the wave, pulls it back behind it.
				const float g = 2.95f * x * std::exp(-1.6f * x * x);
				const float dx = ux * amplitude * g;
				const float dy = uy * amplitude * g;

				const float highlight = std::exp(-3.0f * x * x);
				const float brightness = 0.5f * (1.0f + 0.30f * highlight * life);

				verts.push_back(makeVertex(r, px, py, px - dx, py - dy, brightness));
			}
		}

		indices.clear();
		indices.reserve((size_t)angular * radial * 6);
		const int stride = radial + 1;
		for (int a = 0; a < angular; ++a)
			for (int j = 0; j < radial; ++j)
			{
				const WORD i0 = (WORD)(a * stride + j), i1 = (WORD)(i0 + 1);
				const WORD i2 = (WORD)(i0 + stride), i3 = (WORD)(i2 + 1);
				indices.push_back(i0); indices.push_back(i2); indices.push_back(i1);
				indices.push_back(i1); indices.push_back(i2); indices.push_back(i3);
			}

		DX8Wrapper::Raw_Draw_Indexed_Primitive_UP(D3DPT_TRIANGLELIST, 0, (UINT)verts.size(), (UINT)(indices.size() / 3),
			&indices[0], D3DFMT_INDEX16, &verts[0], sizeof(ScreenVertex));
	}

	DX8Wrapper::Raw_Set_Texture(0, nullptr);
	DX8Wrapper::Invalidate_Cached_Render_States();
}
