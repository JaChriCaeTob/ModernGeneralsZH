// Surface/texture helpers of the x64 d3dx8 replacement: format conversion, resampling and
// DXT1/DXT3/DXT5 encoding + decoding. Pixels are processed as float ARGB; quality is
// "good enough for game textures", not a reference compressor.
#include <windows.h>
#include <d3d8.h>
#include <d3dx8.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace {

struct Px { float a, r, g, b; };

bool IsDXT(D3DFORMAT f)
{
	return f == D3DFMT_DXT1 || f == D3DFMT_DXT2 || f == D3DFMT_DXT3 || f == D3DFMT_DXT4 || f == D3DFMT_DXT5;
}

bool BytesPerPixel(D3DFORMAT f, UINT& bpp)
{
	switch (f) {
		case D3DFMT_A8R8G8B8: case D3DFMT_X8R8G8B8: bpp = 4; return true;
		case D3DFMT_R5G6B5: case D3DFMT_A1R5G5B5: case D3DFMT_X1R5G5B5: case D3DFMT_A4R4G4B4: case D3DFMT_A8L8: bpp = 2; return true;
		case D3DFMT_R8G8B8: bpp = 3; return true;
		case D3DFMT_L8: case D3DFMT_A8: bpp = 1; return true;
		default: return false;
	}
}

float Norm(unsigned v, unsigned bits) { return float(v) / float((1u << bits) - 1); }

unsigned Quant(float v, unsigned bits)
{
	v = std::min(1.0f, std::max(0.0f, v));
	return unsigned(v * float((1u << bits) - 1) + 0.5f);
}

Px DecodePx(D3DFORMAT f, const BYTE* p)
{
	switch (f) {
		case D3DFMT_A8R8G8B8: case D3DFMT_X8R8G8B8: {
			DWORD v; memcpy(&v, p, 4);
			return { f == D3DFMT_X8R8G8B8 ? 1.0f : Norm(v >> 24, 8), Norm((v >> 16) & 255, 8), Norm((v >> 8) & 255, 8), Norm(v & 255, 8) };
		}
		case D3DFMT_R8G8B8: return { 1.0f, Norm(p[2], 8), Norm(p[1], 8), Norm(p[0], 8) };
		case D3DFMT_R5G6B5: { WORD v; memcpy(&v, p, 2); return { 1.0f, Norm(v >> 11, 5), Norm((v >> 5) & 63, 6), Norm(v & 31, 5) }; }
		case D3DFMT_A1R5G5B5: case D3DFMT_X1R5G5B5: {
			WORD v; memcpy(&v, p, 2);
			return { f == D3DFMT_X1R5G5B5 ? 1.0f : float(v >> 15), Norm((v >> 10) & 31, 5), Norm((v >> 5) & 31, 5), Norm(v & 31, 5) };
		}
		case D3DFMT_A4R4G4B4: { WORD v; memcpy(&v, p, 2); return { Norm(v >> 12, 4), Norm((v >> 8) & 15, 4), Norm((v >> 4) & 15, 4), Norm(v & 15, 4) }; }
		case D3DFMT_A8L8: { WORD v; memcpy(&v, p, 2); const float l = Norm(v & 255, 8); return { Norm(v >> 8, 8), l, l, l }; }
		case D3DFMT_L8: { const float l = Norm(*p, 8); return { 1.0f, l, l, l }; }
		case D3DFMT_A8: return { Norm(*p, 8), 0, 0, 0 };
		default: return { 1, 0, 0, 0 };
	}
}

void EncodePx(D3DFORMAT f, BYTE* p, const Px& c)
{
	switch (f) {
		case D3DFMT_A8R8G8B8: case D3DFMT_X8R8G8B8: {
			const DWORD v = ((f == D3DFMT_X8R8G8B8 ? 255u : Quant(c.a, 8)) << 24) | (Quant(c.r, 8) << 16) | (Quant(c.g, 8) << 8) | Quant(c.b, 8);
			memcpy(p, &v, 4); break;
		}
		case D3DFMT_R8G8B8: p[2] = BYTE(Quant(c.r, 8)); p[1] = BYTE(Quant(c.g, 8)); p[0] = BYTE(Quant(c.b, 8)); break;
		case D3DFMT_R5G6B5: { const WORD v = WORD((Quant(c.r, 5) << 11) | (Quant(c.g, 6) << 5) | Quant(c.b, 5)); memcpy(p, &v, 2); break; }
		case D3DFMT_A1R5G5B5: case D3DFMT_X1R5G5B5: {
			const WORD v = WORD(((f == D3DFMT_X1R5G5B5 ? 1u : Quant(c.a, 1)) << 15) | (Quant(c.r, 5) << 10) | (Quant(c.g, 5) << 5) | Quant(c.b, 5));
			memcpy(p, &v, 2); break;
		}
		case D3DFMT_A4R4G4B4: { const WORD v = WORD((Quant(c.a, 4) << 12) | (Quant(c.r, 4) << 8) | (Quant(c.g, 4) << 4) | Quant(c.b, 4)); memcpy(p, &v, 2); break; }
		case D3DFMT_A8L8: { const WORD v = WORD((Quant(c.a, 8) << 8) | Quant((c.r + c.g + c.b) / 3.0f, 8)); memcpy(p, &v, 2); break; }
		case D3DFMT_L8: *p = BYTE(Quant((c.r + c.g + c.b) / 3.0f, 8)); break;
		case D3DFMT_A8: *p = BYTE(Quant(c.a, 8)); break;
		default: break;
	}
}

// ------------------------------------------------------------------ DXT
Px From565(unsigned c) { return { 1.0f, Norm(c >> 11, 5), Norm((c >> 5) & 63, 6), Norm(c & 31, 5) }; }

void DecodeColorBlock(const BYTE* b, Px out[16], bool dxt1)
{
	WORD c0, c1; memcpy(&c0, b, 2); memcpy(&c1, b + 2, 2);
	DWORD bits; memcpy(&bits, b + 4, 4);
	Px pal[4] = { From565(c0), From565(c1), {}, {} };
	if (c0 > c1 || !dxt1) {
		const Px &a = pal[0], &b1 = pal[1];
		pal[2] = { 1, (2 * a.r + b1.r) / 3, (2 * a.g + b1.g) / 3, (2 * a.b + b1.b) / 3 };
		pal[3] = { 1, (a.r + 2 * b1.r) / 3, (a.g + 2 * b1.g) / 3, (a.b + 2 * b1.b) / 3 };
	} else {
		pal[2] = { 1, (pal[0].r + pal[1].r) * 0.5f, (pal[0].g + pal[1].g) * 0.5f, (pal[0].b + pal[1].b) * 0.5f };
		pal[3] = { 0, 0, 0, 0 };
	}
	for (int i = 0; i < 16; ++i) out[i] = pal[(bits >> (2 * i)) & 3];
}

void DecodeBlock(D3DFORMAT f, const BYTE* b, Px out[16])
{
	if (f == D3DFMT_DXT1) { DecodeColorBlock(b, out, true); return; }
	DecodeColorBlock(b + 8, out, false);
	if (f == D3DFMT_DXT2 || f == D3DFMT_DXT3) {
		for (int i = 0; i < 16; ++i) {
			const unsigned v = (b[i / 2] >> ((i & 1) * 4)) & 15;
			out[i].a = Norm(v, 4);
		}
	} else {
		float a[8]; a[0] = b[0] / 255.0f; a[1] = b[1] / 255.0f;
		if (b[0] > b[1]) for (int k = 1; k < 7; ++k) a[1 + k] = ((7 - k) * a[0] + k * a[1]) / 7.0f;
		else { for (int k = 1; k < 5; ++k) a[1 + k] = ((5 - k) * a[0] + k * a[1]) / 5.0f; a[6] = 0; a[7] = 1; }
		uint64_t bits = 0; memcpy(&bits, b + 2, 6);
		for (int i = 0; i < 16; ++i) out[i].a = a[(bits >> (3 * i)) & 7];
	}
}

void EncodeColorBlock(const Px in[16], BYTE* b, bool dxt1)
{
	bool hasAlpha = false;
	float lo[3] = { 1, 1, 1 }, hi[3] = { 0, 0, 0 };
	for (int i = 0; i < 16; ++i) {
		if (dxt1 && in[i].a < 0.5f) { hasAlpha = true; continue; }
		const float v[3] = { in[i].r, in[i].g, in[i].b };
		for (int c = 0; c < 3; ++c) { lo[c] = std::min(lo[c], v[c]); hi[c] = std::max(hi[c], v[c]); }
	}
	if (lo[0] > hi[0]) { lo[0] = lo[1] = lo[2] = hi[0] = hi[1] = hi[2] = 0; } // fully transparent block
	// Slightly inset the box to reduce the error of the min/max endpoints.
	for (int c = 0; c < 3; ++c) { const float d = (hi[c] - lo[c]) * (1.0f / 16); lo[c] += d; hi[c] -= d; }
	auto pack = [](const float* v) { return unsigned((Quant(v[0], 5) << 11) | (Quant(v[1], 6) << 5) | Quant(v[2], 5)); };
	unsigned c0 = pack(hi), c1 = pack(lo);
	Px p0 = From565(c0), p1 = From565(c1);
	bool fourColor = true;
	if (hasAlpha) { // 3-colour + transparent mode requires c0 <= c1
		if (c0 > c1) std::swap(c0, c1);
		fourColor = false;
	} else if (c0 < c1) {
		std::swap(c0, c1);
	}
	if (c0 == c1 && fourColor) { /* flat block: any index works */ }
	p0 = From565(c0); p1 = From565(c1);
	Px pal[4] = { p0, p1, {}, {} };
	if (fourColor) {
		pal[2] = { 1, (2 * p0.r + p1.r) / 3, (2 * p0.g + p1.g) / 3, (2 * p0.b + p1.b) / 3 };
		pal[3] = { 1, (p0.r + 2 * p1.r) / 3, (p0.g + 2 * p1.g) / 3, (p0.b + 2 * p1.b) / 3 };
	} else {
		pal[2] = { 1, (p0.r + p1.r) / 2, (p0.g + p1.g) / 2, (p0.b + p1.b) / 2 };
	}
	DWORD idx = 0;
	for (int i = 0; i < 16; ++i) {
		int best = 0;
		if (!fourColor && in[i].a < 0.5f) best = 3;
		else {
			float bd = 1e9f;
			for (int k = 0; k < (fourColor ? 4 : 3); ++k) {
				const float dr = in[i].r - pal[k].r, dg = in[i].g - pal[k].g, db = in[i].b - pal[k].b;
				const float d = dr * dr + dg * dg + db * db;
				if (d < bd) { bd = d; best = k; }
			}
		}
		idx |= DWORD(best) << (2 * i);
	}
	const WORD w0 = WORD(c0), w1 = WORD(c1);
	memcpy(b, &w0, 2); memcpy(b + 2, &w1, 2); memcpy(b + 4, &idx, 4);
}

void EncodeBlock(D3DFORMAT f, const Px in[16], BYTE* b)
{
	if (f == D3DFMT_DXT1) { EncodeColorBlock(in, b, true); return; }
	if (f == D3DFMT_DXT2 || f == D3DFMT_DXT3) {
		for (int i = 0; i < 8; ++i) b[i] = BYTE(Quant(in[2 * i].a, 4) | (Quant(in[2 * i + 1].a, 4) << 4));
	} else {
		float lo = 1, hi = 0;
		for (int i = 0; i < 16; ++i) { lo = std::min(lo, in[i].a); hi = std::max(hi, in[i].a); }
		const unsigned a0 = Quant(hi, 8), a1 = Quant(lo, 8);
		b[0] = BYTE(a0); b[1] = BYTE(a1);
		float pal[8]; pal[0] = a0 / 255.0f; pal[1] = a1 / 255.0f;
		if (a0 > a1) for (int k = 1; k < 7; ++k) pal[1 + k] = ((7 - k) * pal[0] + k * pal[1]) / 7.0f;
		else for (int k = 2; k < 8; ++k) pal[k] = pal[0]; // flat alpha: every index maps to a0
		uint64_t bits = 0;
		for (int i = 0; i < 16; ++i) {
			int best = 0; float bd = 1e9f;
			for (int k = 0; k < 8; ++k) { const float d = std::fabs(in[i].a - pal[k]); if (d < bd) { bd = d; best = k; } }
			bits |= uint64_t(best) << (3 * i);
		}
		memcpy(b + 2, &bits, 6);
	}
	EncodeColorBlock(in, b + 8, false);
}

// ------------------------------------------------------------------ surface IO
struct Image { int w = 0, h = 0; std::vector<Px> px; };

bool ReadSurface(LPDIRECT3DSURFACE8 s, const D3DSURFACE_DESC& d, const RECT& r, Image& out)
{
	D3DLOCKED_RECT lr;
	if (FAILED(s->LockRect(&lr, nullptr, D3DLOCK_READONLY))) return false;
	out.w = r.right - r.left; out.h = r.bottom - r.top;
	out.px.resize(size_t(out.w) * out.h);
	const BYTE* base = (const BYTE*)lr.pBits;
	if (IsDXT(d.Format)) {
		const int bsz = d.Format == D3DFMT_DXT1 ? 8 : 16;
		for (int y = 0; y < out.h; ++y)
			for (int x = 0; x < out.w; ++x) {
				const int sx = r.left + x, sy = r.top + y;
				Px blk[16];
				DecodeBlock(d.Format, base + (sy / 4) * lr.Pitch + (sx / 4) * bsz, blk);
				out.px[size_t(y) * out.w + x] = blk[(sy & 3) * 4 + (sx & 3)];
			}
	} else {
		UINT bpp = 0; BytesPerPixel(d.Format, bpp);
		for (int y = 0; y < out.h; ++y)
			for (int x = 0; x < out.w; ++x)
				out.px[size_t(y) * out.w + x] = DecodePx(d.Format, base + (r.top + y) * lr.Pitch + (r.left + x) * bpp);
	}
	s->UnlockRect();
	return true;
}

bool WriteSurface(LPDIRECT3DSURFACE8 s, const D3DSURFACE_DESC& d, const RECT& r, const Image& in)
{
	D3DLOCKED_RECT lr;
	if (FAILED(s->LockRect(&lr, nullptr, 0))) return false;
	BYTE* base = (BYTE*)lr.pBits;
	if (IsDXT(d.Format)) {
		const int bsz = d.Format == D3DFMT_DXT1 ? 8 : 16;
		for (int by = r.top / 4; by < (r.bottom + 3) / 4; ++by)
			for (int bx = r.left / 4; bx < (r.right + 3) / 4; ++bx) {
				Px blk[16];
				for (int j = 0; j < 4; ++j)
					for (int i = 0; i < 4; ++i) {
						const int x = std::min(std::max<int>(bx * 4 + i - r.left, 0), in.w - 1);
						const int y = std::min(std::max<int>(by * 4 + j - r.top, 0), in.h - 1);
						blk[j * 4 + i] = in.px[size_t(y) * in.w + x];
					}
				EncodeBlock(d.Format, blk, base + by * lr.Pitch + bx * bsz);
			}
	} else {
		UINT bpp = 0; BytesPerPixel(d.Format, bpp);
		for (int y = 0; y < in.h; ++y)
			for (int x = 0; x < in.w; ++x)
				EncodePx(d.Format, base + (r.top + y) * lr.Pitch + (r.left + x) * bpp, in.px[size_t(y) * in.w + x]);
	}
	s->UnlockRect();
	return true;
}

// Box filter when shrinking, bilinear when enlarging.
Image Resample(const Image& src, int dw, int dh)
{
	if (src.w == dw && src.h == dh) return src;
	Image out; out.w = dw; out.h = dh; out.px.resize(size_t(dw) * dh);
	for (int y = 0; y < dh; ++y)
		for (int x = 0; x < dw; ++x) {
			Px acc = { 0, 0, 0, 0 };
			if (dw <= src.w && dh <= src.h) {
				const int x0 = x * src.w / dw, x1 = std::max(x0 + 1, (x + 1) * src.w / dw);
				const int y0 = y * src.h / dh, y1 = std::max(y0 + 1, (y + 1) * src.h / dh);
				int n = 0;
				for (int yy = y0; yy < y1; ++yy)
					for (int xx = x0; xx < x1; ++xx) {
						const Px& p = src.px[size_t(yy) * src.w + xx];
						acc.a += p.a; acc.r += p.r; acc.g += p.g; acc.b += p.b; ++n;
					}
				const float inv = 1.0f / n; acc = { acc.a * inv, acc.r * inv, acc.g * inv, acc.b * inv };
			} else {
				const float fx = (x + 0.5f) * src.w / dw - 0.5f, fy = (y + 0.5f) * src.h / dh - 0.5f;
				const int x0 = (int)std::floor(fx), y0 = (int)std::floor(fy);
				const float tx = fx - x0, ty = fy - y0;
				auto at = [&](int xx, int yy) -> const Px& {
					return src.px[size_t(std::min(std::max(yy, 0), src.h - 1)) * src.w + std::min(std::max(xx, 0), src.w - 1)];
				};
				const Px &p00 = at(x0, y0), &p10 = at(x0 + 1, y0), &p01 = at(x0, y0 + 1), &p11 = at(x0 + 1, y0 + 1);
				auto lerp = [&](float a, float b, float c, float d) { return (a * (1 - tx) + b * tx) * (1 - ty) + (c * (1 - tx) + d * tx) * ty; };
				acc = { lerp(p00.a, p10.a, p01.a, p11.a), lerp(p00.r, p10.r, p01.r, p11.r), lerp(p00.g, p10.g, p01.g, p11.g), lerp(p00.b, p10.b, p01.b, p11.b) };
			}
			out.px[size_t(y) * dw + x] = acc;
		}
	return out;
}

bool Supported(D3DFORMAT f) { UINT b; return IsDXT(f) || BytesPerPixel(f, b); }

} // namespace

extern "C" {

HRESULT WINAPI D3DXLoadSurfaceFromSurface(LPDIRECT3DSURFACE8 dst, const PALETTEENTRY*, const RECT* dstRect,
	LPDIRECT3DSURFACE8 src, const PALETTEENTRY*, const RECT* srcRect, DWORD, D3DCOLOR)
{
	if (!dst || !src) return D3DERR_INVALIDCALL;
	D3DSURFACE_DESC sd, dd;
	src->GetDesc(&sd);
	dst->GetDesc(&dd);
	if (!Supported(sd.Format) || !Supported(dd.Format)) return E_NOTIMPL;

	const RECT sr = srcRect ? *srcRect : RECT{ 0, 0, (LONG)sd.Width, (LONG)sd.Height };
	const RECT dr = dstRect ? *dstRect : RECT{ 0, 0, (LONG)dd.Width, (LONG)dd.Height };

	// Same compressed format and size: copy the blocks untouched.
	if (sd.Format == dd.Format && IsDXT(sd.Format) && !srcRect && !dstRect && sd.Width == dd.Width && sd.Height == dd.Height) {
		D3DLOCKED_RECT sl, dl;
		if (FAILED(src->LockRect(&sl, nullptr, D3DLOCK_READONLY))) return D3DERR_INVALIDCALL;
		if (FAILED(dst->LockRect(&dl, nullptr, 0))) { src->UnlockRect(); return D3DERR_INVALIDCALL; }
		const UINT bsz = sd.Format == D3DFMT_DXT1 ? 8 : 16;
		const UINT rows = (sd.Height + 3) / 4, rowBytes = ((sd.Width + 3) / 4) * bsz;
		for (UINT y = 0; y < rows; ++y)
			memcpy((BYTE*)dl.pBits + y * dl.Pitch, (const BYTE*)sl.pBits + y * sl.Pitch, rowBytes);
		dst->UnlockRect();
		src->UnlockRect();
		return D3D_OK;
	}

	Image img;
	if (!ReadSurface(src, sd, sr, img)) return D3DERR_INVALIDCALL;
	const Image scaled = Resample(img, dr.right - dr.left, dr.bottom - dr.top);
	return WriteSurface(dst, dd, dr, scaled) ? D3D_OK : D3DERR_INVALIDCALL;
}

HRESULT WINAPI D3DXCreateTexture(LPDIRECT3DDEVICE8 dev, UINT w, UINT h, UINT levels, DWORD usage, D3DFORMAT fmt,
	D3DPOOL pool, LPDIRECT3DTEXTURE8* out)
{
	if (!dev || !out) return D3DERR_INVALIDCALL;
	if (levels == D3DX_DEFAULT) levels = 0; // 0 = full chain in D3D8
	return dev->CreateTexture(w, h, levels, usage, fmt, pool, out);
}

HRESULT WINAPI D3DXCreateCubeTexture(LPDIRECT3DDEVICE8 dev, UINT size, UINT levels, DWORD usage, D3DFORMAT fmt,
	D3DPOOL pool, LPDIRECT3DCUBETEXTURE8* out)
{
	if (!dev || !out) return D3DERR_INVALIDCALL;
	if (levels == D3DX_DEFAULT) levels = 0;
	return dev->CreateCubeTexture(size, levels, usage, fmt, pool, out);
}

HRESULT WINAPI D3DXCreateVolumeTexture(LPDIRECT3DDEVICE8 dev, UINT w, UINT h, UINT d, UINT levels, DWORD usage,
	D3DFORMAT fmt, D3DPOOL pool, LPDIRECT3DVOLUMETEXTURE8* out)
{
	if (!dev || !out) return D3DERR_INVALIDCALL;
	if (levels == D3DX_DEFAULT) levels = 0;
	return dev->CreateVolumeTexture(w, h, d, levels, usage, fmt, pool, out);
}

HRESULT WINAPI D3DXFilterTexture(LPDIRECT3DBASETEXTURE8 base, const PALETTEENTRY*, UINT srcLevel, DWORD)
{
	if (!base || base->GetType() != D3DRTYPE_TEXTURE) return D3DERR_INVALIDCALL;
	LPDIRECT3DTEXTURE8 tex = (LPDIRECT3DTEXTURE8)base;
	const DWORD levels = tex->GetLevelCount();
	for (DWORD l = srcLevel; l + 1 < levels; ++l) {
		LPDIRECT3DSURFACE8 s = nullptr, d = nullptr;
		if (FAILED(tex->GetSurfaceLevel(l, &s))) return D3DERR_INVALIDCALL;
		if (FAILED(tex->GetSurfaceLevel(l + 1, &d))) { s->Release(); return D3DERR_INVALIDCALL; }
		const HRESULT hr = D3DXLoadSurfaceFromSurface(d, nullptr, nullptr, s, nullptr, nullptr, D3DX_FILTER_BOX, 0);
		s->Release();
		d->Release();
		if (FAILED(hr)) return hr;
	}
	return D3D_OK;
}

} // extern "C"
