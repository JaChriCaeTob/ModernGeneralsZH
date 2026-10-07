// x64 replacement for the subset of d3dx8.lib used by Generals/Zero Hour.
#include <windows.h>
#include <d3d8.h>
#include <d3dx8.h>
#include <cmath>
#include <cstring>
#include <algorithm>

extern "C" {

// ---------------------------------------------------------------- math
D3DXMATRIX* WINAPI D3DXMatrixTranspose(D3DXMATRIX* out, const D3DXMATRIX* m)
{
	D3DXMATRIX t;
	for (int i = 0; i < 4; ++i)
		for (int j = 0; j < 4; ++j)
			t.m[i][j] = m->m[j][i];
	*out = t;
	return out;
}

D3DXMATRIX* WINAPI D3DXMatrixMultiply(D3DXMATRIX* out, const D3DXMATRIX* a, const D3DXMATRIX* b)
{
	D3DXMATRIX t;
	for (int i = 0; i < 4; ++i)
		for (int j = 0; j < 4; ++j)
			t.m[i][j] = a->m[i][0] * b->m[0][j] + a->m[i][1] * b->m[1][j] + a->m[i][2] * b->m[2][j] + a->m[i][3] * b->m[3][j];
	*out = t;
	return out;
}

D3DXMATRIX* WINAPI D3DXMatrixInverse(D3DXMATRIX* out, FLOAT* det, const D3DXMATRIX* in)
{
	// Gauss-Jordan elimination with partial pivoting.
	double a[4][8];
	for (int i = 0; i < 4; ++i)
		for (int j = 0; j < 4; ++j) {
			a[i][j] = in->m[i][j];
			a[i][4 + j] = (i == j) ? 1.0 : 0.0;
		}
	double d = 1.0;
	for (int c = 0; c < 4; ++c) {
		int p = c;
		for (int r = c + 1; r < 4; ++r)
			if (std::fabs(a[r][c]) > std::fabs(a[p][c])) p = r;
		if (a[p][c] == 0.0) {
			if (det) *det = 0.0f;
			return nullptr;
		}
		if (p != c) {
			for (int k = 0; k < 8; ++k) std::swap(a[p][k], a[c][k]);
			d = -d;
		}
		d *= a[c][c];
		const double inv = 1.0 / a[c][c];
		for (int k = 0; k < 8; ++k) a[c][k] *= inv;
		for (int r = 0; r < 4; ++r) {
			if (r == c) continue;
			const double f = a[r][c];
			if (f == 0.0) continue;
			for (int k = 0; k < 8; ++k) a[r][k] -= f * a[c][k];
		}
	}
	if (det) *det = (FLOAT)d;
	for (int i = 0; i < 4; ++i)
		for (int j = 0; j < 4; ++j)
			out->m[i][j] = (FLOAT)a[i][4 + j];
	return out;
}

D3DXMATRIX* WINAPI D3DXMatrixScaling(D3DXMATRIX* out, FLOAT sx, FLOAT sy, FLOAT sz)
{
	D3DXMatrixIdentity(out);
	out->_11 = sx; out->_22 = sy; out->_33 = sz;
	return out;
}

D3DXMATRIX* WINAPI D3DXMatrixTranslation(D3DXMATRIX* out, FLOAT x, FLOAT y, FLOAT z)
{
	D3DXMatrixIdentity(out);
	out->_41 = x; out->_42 = y; out->_43 = z;
	return out;
}

D3DXMATRIX* WINAPI D3DXMatrixRotationZ(D3DXMATRIX* out, FLOAT angle)
{
	D3DXMatrixIdentity(out);
	const FLOAT s = std::sin(angle), c = std::cos(angle);
	out->_11 = c; out->_12 = s; out->_21 = -s; out->_22 = c;
	return out;
}

D3DXVECTOR4* WINAPI D3DXVec4Transform(D3DXVECTOR4* out, const D3DXVECTOR4* v, const D3DXMATRIX* m)
{
	D3DXVECTOR4 t;
	t.x = v->x * m->_11 + v->y * m->_21 + v->z * m->_31 + v->w * m->_41;
	t.y = v->x * m->_12 + v->y * m->_22 + v->z * m->_32 + v->w * m->_42;
	t.z = v->x * m->_13 + v->y * m->_23 + v->z * m->_33 + v->w * m->_43;
	t.w = v->x * m->_14 + v->y * m->_24 + v->z * m->_34 + v->w * m->_44;
	*out = t;
	return out;
}

D3DXVECTOR4* WINAPI D3DXVec3Transform(D3DXVECTOR4* out, const D3DXVECTOR3* v, const D3DXMATRIX* m)
{
	D3DXVECTOR4 in(v->x, v->y, v->z, 1.0f);
	return D3DXVec4Transform(out, &in, m);
}

UINT WINAPI D3DXGetFVFVertexSize(DWORD fvf)
{
	UINT size = 0;
	switch (fvf & D3DFVF_POSITION_MASK) {
		case D3DFVF_XYZ: size += 12; break;
		case D3DFVF_XYZRHW: size += 16; break;
		case D3DFVF_XYZB1: size += 16; break;
		case D3DFVF_XYZB2: size += 20; break;
		case D3DFVF_XYZB3: size += 24; break;
		case D3DFVF_XYZB4: size += 28; break;
		case D3DFVF_XYZB5: size += 32; break;
	}
	if (fvf & D3DFVF_NORMAL) size += 12;
	if (fvf & D3DFVF_DIFFUSE) size += 4;
	if (fvf & D3DFVF_SPECULAR) size += 4;
	const UINT texCount = (fvf & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT;
	for (UINT i = 0; i < texCount; ++i) {
		switch ((fvf >> (16 + i * 2)) & 3) {
			case D3DFVF_TEXTUREFORMAT1: size += 4; break;
			case D3DFVF_TEXTUREFORMAT2: size += 8; break;
			case D3DFVF_TEXTUREFORMAT3: size += 12; break;
			case D3DFVF_TEXTUREFORMAT4: size += 16; break;
		}
	}
	return size;
}

// ---------------------------------------------------------------- unsupported
HRESULT WINAPI D3DXCreateFont(LPDIRECT3DDEVICE8, HFONT, LPD3DXFONT*) { return E_NOTIMPL; }

HRESULT WINAPI D3DXGetErrorStringA(HRESULT hr, LPSTR buf, UINT len)
{
	if (!buf || !len) return D3DERR_INVALIDCALL;
	_snprintf_s(buf, len, _TRUNCATE, "HRESULT 0x%08lX", (unsigned long)hr);
	return D3D_OK;
}

HRESULT WINAPI D3DXCreateTextureFromFileExA(LPDIRECT3DDEVICE8, LPCSTR, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL,
	DWORD, DWORD, D3DCOLOR, D3DXIMAGE_INFO*, PALETTEENTRY*, LPDIRECT3DTEXTURE8*)
{
	return E_NOTIMPL;
}

} // extern "C"
