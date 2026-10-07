// Minimal Direct3D 8 pixel shader assembler (ps.1.0 - ps.1.3 instruction set) for the 64-bit d3dx8 replacement.
// The game assembles a few small pixel shaders from source text at run time (water); Microsoft's d3dx8.lib is not
// available for x64, so this produces the same token stream. Vertex shaders are loaded precompiled by the game.

#include <windows.h>
#include <d3d8.h>
#include <d3dx8.h>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#ifndef D3DXERR_INVALIDDATA
#define D3DXERR_INVALIDDATA ((HRESULT)0x88760b59L)
#endif

namespace
{

class CompatBuffer : public ID3DXBuffer
{
public:
	explicit CompatBuffer(const std::vector<DWORD>& data) : m_refs(1), m_data(data) {}

	STDMETHOD(QueryInterface)(REFIID iid, LPVOID* out) override
	{
		if (!out) return E_POINTER;
		*out = this;
		AddRef();
		(void)iid;
		return S_OK;
	}
	STDMETHOD_(ULONG, AddRef)() override { return (ULONG)InterlockedIncrement(&m_refs); }
	STDMETHOD_(ULONG, Release)() override
	{
		const ULONG r = (ULONG)InterlockedDecrement(&m_refs);
		if (r == 0) delete this;
		return r;
	}
	STDMETHOD_(LPVOID, GetBufferPointer)() override { return m_data.empty() ? nullptr : &m_data[0]; }
	STDMETHOD_(DWORD, GetBufferSize)() override { return (DWORD)(m_data.size() * sizeof(DWORD)); }

private:
	LONG m_refs;
	std::vector<DWORD> m_data;
};

enum RegType { RT_TEMP = 0, RT_INPUT = 1, RT_CONST = 2, RT_TEXTURE = 3 };

struct OpInfo { const char* name; DWORD code; int operands; };

const OpInfo kOps[] = {
	{ "nop", 0, 0 },   { "mov", 1, 2 },    { "add", 2, 3 },     { "sub", 3, 3 },    { "mad", 4, 4 },
	{ "mul", 5, 3 },   { "dp3", 8, 3 },    { "dp4", 9, 3 },     { "lrp", 18, 4 },   { "cnd", 80, 4 },
	{ "cmp", 88, 4 },  { "tex", 66, 1 },   { "texbem", 67, 2 }, { "texbeml", 68, 2 }, { "texcoord", 64, 1 },
	{ "texkill", 65, 1 }, { "texreg2ar", 69, 2 }, { "texreg2gb", 70, 2 }, { "texm3x2pad", 71, 2 },
	{ "texm3x2tex", 72, 2 }, { "texm3x3pad", 73, 2 }, { "texm3x3tex", 74, 2 }, { "texm3x3spec", 76, 3 },
	{ "texm3x3vspec", 77, 2 }, { "texreg2rgb", 82, 2 }, { "texdp3tex", 83, 2 }, { "texm3x2depth", 84, 2 },
	{ "texdp3", 85, 2 }, { "texm3x3", 86, 2 }, { "texdepth", 87, 1 }, { "bem", 89, 3 },
};

struct Parser
{
	std::string error;

	static std::string lower(std::string s)
	{
		for (size_t i = 0; i < s.size(); ++i) s[i] = (char)tolower((unsigned char)s[i]);
		return s;
	}

	static std::string trim(const std::string& s)
	{
		size_t a = 0, b = s.size();
		while (a < b && isspace((unsigned char)s[a])) ++a;
		while (b > a && isspace((unsigned char)s[b - 1])) --b;
		return s.substr(a, b - a);
	}

	static int component(char c)
	{
		switch (c) {
			case 'r': case 'x': return 0;
			case 'g': case 'y': return 1;
			case 'b': case 'z': return 2;
			case 'a': case 'w': return 3;
		}
		return -1;
	}

	static DWORD regToken(RegType type, int num)
	{
		return 0x80000000u | ((DWORD)(type & 7) << 28) | (((DWORD)(type & 0x18) >> 3) << 11) | (DWORD)(num & 0x7FF);
	}

	bool parseReg(const std::string& text, RegType& type, int& num, std::string& suffix)
	{
		if (text.empty()) return false;
		switch (text[0]) {
			case 'r': type = RT_TEMP; break;
			case 'v': type = RT_INPUT; break;
			case 'c': type = RT_CONST; break;
			case 't': type = RT_TEXTURE; break;
			default: return false;
		}
		size_t i = 1;
		if (i >= text.size() || !isdigit((unsigned char)text[i])) return false;
		num = 0;
		while (i < text.size() && isdigit((unsigned char)text[i])) num = num * 10 + (text[i++] - '0');
		suffix = text.substr(i);
		return true;
	}

	// destination: reg[.mask]
	bool parseDest(std::string text, DWORD& token, DWORD modifiers)
	{
		RegType type; int num; std::string suffix;
		if (!parseReg(text, type, num, suffix)) { error = "bad destination '" + text + "'"; return false; }
		DWORD mask = 0xF;
		if (!suffix.empty()) {
			if (suffix[0] != '.') { error = "bad destination '" + text + "'"; return false; }
			mask = 0;
			for (size_t i = 1; i < suffix.size(); ++i) {
				const int c = component(suffix[i]);
				if (c < 0) { error = "bad write mask '" + text + "'"; return false; }
				mask |= 1u << c;
			}
		}
		token = regToken(type, num) | (mask << 16) | modifiers;
		return true;
	}

	// source: [-|1-]reg[.swizzle][_bx2|_bias|_x2|_dz|_dw]
	bool parseSource(std::string text, DWORD& token)
	{
		DWORD mod = 0;
		bool negate = false, complement = false;
		if (text.compare(0, 2, "1-") == 0) { complement = true; text = text.substr(2); }
		else if (!text.empty() && text[0] == '-') { negate = true; text = text.substr(1); }

		std::string suffixMod;
		const size_t us = text.find('_');
		if (us != std::string::npos) { suffixMod = text.substr(us); text = text.substr(0, us); }

		RegType type; int num; std::string swz;
		if (!parseReg(text, type, num, swz)) { error = "bad source '" + text + "'"; return false; }

		DWORD swizzle = 0xE4;
		if (!swz.empty()) {
			if (swz[0] != '.') { error = "bad swizzle '" + text + "'"; return false; }
			std::string comps = swz.substr(1);
			if (comps.size() == 1) {
				const int c = component(comps[0]);
				if (c < 0) { error = "bad swizzle '" + text + "'"; return false; }
				swizzle = (DWORD)(c | (c << 2) | (c << 4) | (c << 6));
			} else if (comps == "rgb" || comps == "xyz") {
				swizzle = 0xE4; // alpha is handled by the separate alpha pipeline
			} else if (comps.size() == 4) {
				swizzle = 0;
				for (int j = 0; j < 4; ++j) {
					const int c = component(comps[j]);
					if (c < 0) { error = "bad swizzle '" + text + "'"; return false; }
					swizzle |= (DWORD)c << (2 * j);
				}
			} else {
				error = "unsupported swizzle '" + text + "'";
				return false;
			}
		}

		if (complement) mod = 6;                       // 1-x
		else if (suffixMod == "_bx2") mod = negate ? 5 : 4;   // signed scale
		else if (suffixMod == "_bias") mod = negate ? 3 : 2;
		else if (suffixMod == "_x2") mod = negate ? 8 : 7;
		else if (suffixMod == "_dz") mod = 9;
		else if (suffixMod == "_dw") mod = 10;
		else if (!suffixMod.empty()) { error = "unknown source modifier '" + suffixMod + "'"; return false; }
		else if (negate) mod = 1;

		token = regToken(type, num) | (swizzle << 16) | (mod << 24);
		return true;
	}

	bool assemble(const char* src, size_t len, std::vector<DWORD>& out)
	{
		std::string text(src, len);
		size_t pos = 0;
		bool haveVersion = false;
		int lineNo = 0;

		while (pos <= text.size()) {
			size_t eol = text.find_first_of("\r\n", pos);
			if (eol == std::string::npos) eol = text.size();
			std::string line = text.substr(pos, eol - pos);
			pos = eol + 1;
			++lineNo;

			const size_t semi = line.find(';');
			if (semi != std::string::npos) line = line.substr(0, semi);
			const size_t slash = line.find("//");
			if (slash != std::string::npos) line = line.substr(0, slash);
			// line continuation backslash used inside C string literals
			for (size_t k = 0; k < line.size(); ++k) if (line[k] == '\\') line[k] = ' ';
			line = lower(trim(line));
			if (line.empty()) continue;

			if (!haveVersion) {
				int major = 0, minor = 0;
				if (sscanf(line.c_str(), "ps.%d.%d", &major, &minor) != 2 || major != 1 || minor > 3) {
					error = "only ps.1.0 - ps.1.3 is supported";
					return false;
				}
				out.push_back(0xFFFF0000u | ((DWORD)major << 8) | (DWORD)minor);
				haveVersion = true;
				continue;
			}

			bool coissue = false;
			if (line[0] == '+') { coissue = true; line = trim(line.substr(1)); }

			// mnemonic with optional modifiers: mul_x2_sat
			size_t sp = line.find_first_of(" \t");
			std::string mnemonic = line.substr(0, sp);
			std::string operands = sp == std::string::npos ? "" : trim(line.substr(sp));

			DWORD destMods = 0;
			const size_t us = mnemonic.find('_');
			if (us != std::string::npos) {
				std::string mods = mnemonic.substr(us);
				mnemonic = mnemonic.substr(0, us);
				size_t m = 0;
				while (m < mods.size()) {
					const size_t next = mods.find('_', m + 1);
					const std::string one = mods.substr(m, next == std::string::npos ? std::string::npos : next - m);
					if (one == "_sat") destMods |= 1u << 20;
					else if (one == "_x2") destMods |= 1u << 24;
					else if (one == "_x4") destMods |= 2u << 24;
					else if (one == "_d2") destMods |= 15u << 24;
					else if (one == "_d4") destMods |= 14u << 24;
					else { error = "unknown instruction modifier '" + one + "'"; return false; }
					if (next == std::string::npos) break;
					m = next;
				}
			}

			const OpInfo* op = nullptr;
			for (size_t k = 0; k < sizeof(kOps) / sizeof(kOps[0]); ++k)
				if (mnemonic == kOps[k].name) { op = &kOps[k]; break; }
			if (!op) { error = "unknown instruction '" + mnemonic + "'"; return false; }

			std::vector<std::string> args;
			if (!operands.empty()) {
				size_t a = 0;
				for (;;) {
					const size_t comma = operands.find(',', a);
					args.push_back(trim(operands.substr(a, comma == std::string::npos ? std::string::npos : comma - a)));
					if (comma == std::string::npos) break;
					a = comma + 1;
				}
			}
			if ((int)args.size() != op->operands) { error = "wrong operand count for '" + mnemonic + "'"; return false; }

			out.push_back(op->code | (coissue ? 0x40000000u : 0u));
			for (int k = 0; k < op->operands; ++k) {
				DWORD tok = 0;
				if (k == 0) {
					if (!parseDest(args[k], tok, destMods)) return false;
				} else if (!parseSource(args[k], tok)) {
					return false;
				}
				out.push_back(tok);
			}
		}

		if (!haveVersion) { error = "missing version"; return false; }
		out.push_back(0x0000FFFFu); // end
		return true;
	}
};

} // namespace

extern "C" HRESULT WINAPI D3DXAssembleShader(LPCVOID pSrcData, UINT SrcDataLen, DWORD, LPD3DXBUFFER* ppConstants,
	LPD3DXBUFFER* ppCompiledShader, LPD3DXBUFFER* ppCompilationErrors)
{
	if (ppConstants) *ppConstants = nullptr;
	if (ppCompiledShader) *ppCompiledShader = nullptr;
	if (ppCompilationErrors) *ppCompilationErrors = nullptr;
	if (!pSrcData || !ppCompiledShader)
		return D3DERR_INVALIDCALL;

	Parser parser;
	std::vector<DWORD> tokens;
	if (!parser.assemble((const char*)pSrcData, SrcDataLen, tokens)) {
		if (ppCompilationErrors) {
			const std::string& e = parser.error;
			std::vector<DWORD> msg((e.size() + 4) / 4 + 1, 0);
			memcpy(&msg[0], e.c_str(), e.size());
			*ppCompilationErrors = new CompatBuffer(msg);
		}
		return D3DXERR_INVALIDDATA;
	}

	*ppCompiledShader = new CompatBuffer(tokens);
	return D3D_OK;
}
