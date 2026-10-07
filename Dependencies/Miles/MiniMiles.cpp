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

// MiniMiles: a miniaudio based implementation of the subset of the Miles Sound System API (mss/mss.h)
// that the engine uses. Used on 64-bit builds, where the 32-bit only mss32.dll cannot be loaded.
//
// Mapping:
//  * HSAMPLE / H3DSAMPLE / HSTREAM / HAUDIO are all "voices": a decoder wrapped in a looping data source
//    that is played by a miniaudio engine sound.
//  * Samples play from an in-memory WAV image owned by the engine's audio cache.
//  * Streams (music, speech) are read completely through the engine's file callbacks (so they work from
//    inside .big archives) and decoded from memory.
//  * 3D samples are positioned by this code, not by miniaudio: distance attenuation and left/right pan are
//    recomputed on a worker thread. The same thread fires the end-of-sample callbacks, like the Miles timer thread.
//  * Loop counts follow Miles: 1 = play once, N = play N times, 0 = endless. AIL_stream_loop_count reports the
//    plays that are left, which the engine uses to detect how often a music track completed.

#include "MilesLoader.h"
#include "mss/mss.h"

#include <windows.h>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <vector>
#include <list>

#define MA_NO_ENCODING
#define MA_NO_GENERATION
#define MA_NO_NODE_GRAPH_TESTS
#define MINIAUDIO_IMPLEMENTATION
#include "../miniaudio/miniaudio.h"

namespace
{

// ------------------------------------------------------------------------------------------------
// WAV helpers
// ------------------------------------------------------------------------------------------------
struct WavInfo
{
	unsigned short format;
	unsigned short channels;
	unsigned short bits;
	unsigned short blockAlign;
	unsigned rate;
	const unsigned char* data;
	unsigned dataLen;
	size_t imageSize; // bytes from the start of the image to the end of the data chunk
};

static unsigned short rd16(const unsigned char* p) { return (unsigned short)(p[0] | (p[1] << 8)); }
static unsigned rd32(const unsigned char* p) { return (unsigned)(p[0] | (p[1] << 8) | (p[2] << 16) | ((unsigned)p[3] << 24)); }

static bool parseWav(const void* image, WavInfo& w)
{
	memset(&w, 0, sizeof(w));
	const unsigned char* b = (const unsigned char*)image;
	if (!b || memcmp(b, "RIFF", 4) != 0 || memcmp(b + 8, "WAVE", 4) != 0)
		return false;

	const unsigned char* p = b + 12;
	bool haveFmt = false;
	for (int guard = 0; guard < 64; ++guard)
	{
		const unsigned size = rd32(p + 4);
		if (memcmp(p, "fmt ", 4) == 0)
		{
			w.format = rd16(p + 8);
			w.channels = rd16(p + 10);
			w.rate = rd32(p + 12);
			w.blockAlign = rd16(p + 20);
			w.bits = rd16(p + 22);
			haveFmt = true;
		}
		else if (memcmp(p, "data", 4) == 0)
		{
			w.data = p + 8;
			w.dataLen = size;
			w.imageSize = (size_t)(p + 8 - b) + size;
			return haveFmt;
		}
		p += 8 + size + (size & 1);
	}
	return false;
}

static const int kImaIndexTable[16] = { -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8 };
static const int kImaStepTable[89] = {
	7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118,
	130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963, 1060,
	1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484,
	7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767 };

static short imaDecodeNibble(int nibble, int& predictor, int& index)
{
	int step = kImaStepTable[index];
	int diff = step >> 3;
	if (nibble & 4) diff += step;
	if (nibble & 2) diff += step >> 1;
	if (nibble & 1) diff += step >> 2;
	if (nibble & 8) predictor -= diff; else predictor += diff;
	if (predictor > 32767) predictor = 32767;
	if (predictor < -32768) predictor = -32768;
	index += kImaIndexTable[nibble];
	if (index < 0) index = 0;
	if (index > 88) index = 88;
	return (short)predictor;
}

// Decodes Microsoft IMA ADPCM into a complete 16 bit PCM WAV image (malloc'ed).
static void* decodeImaAdpcm(const WavInfo& w, unsigned& outSize)
{
	const int ch = w.channels;
	const int block = w.blockAlign;
	if (ch < 1 || ch > 2 || block < 4 * ch)
		return nullptr;

	const int samplesPerBlock = (block - 4 * ch) * 2 / ch + 1;
	const unsigned blocks = w.dataLen / block;
	const unsigned totalFrames = blocks * samplesPerBlock;
	const unsigned pcmBytes = totalFrames * ch * 2;

	unsigned char* out = (unsigned char*)malloc(44 + pcmBytes);
	if (!out)
		return nullptr;

	memcpy(out, "RIFF", 4);
	const unsigned riffSize = 36 + pcmBytes;
	memcpy(out + 4, &riffSize, 4);
	memcpy(out + 8, "WAVEfmt ", 8);
	const unsigned fmtSize = 16;
	memcpy(out + 16, &fmtSize, 4);
	const unsigned short fmtTag = 1, channels = (unsigned short)ch, bits = 16;
	const unsigned short align = (unsigned short)(ch * 2);
	const unsigned byteRate = w.rate * ch * 2;
	memcpy(out + 20, &fmtTag, 2);
	memcpy(out + 22, &channels, 2);
	memcpy(out + 24, &w.rate, 4);
	memcpy(out + 28, &byteRate, 4);
	memcpy(out + 32, &align, 2);
	memcpy(out + 34, &bits, 2);
	memcpy(out + 36, "data", 4);
	memcpy(out + 40, &pcmBytes, 4);

	short* pcm = (short*)(out + 44);
	for (unsigned b = 0; b < blocks; ++b)
	{
		const unsigned char* src = w.data + (size_t)b * block;
		short* dst = pcm + (size_t)b * samplesPerBlock * ch;
		int predictor[2] = { 0, 0 }, index[2] = { 0, 0 };
		for (int c = 0; c < ch; ++c)
		{
			predictor[c] = (short)rd16(src + c * 4);
			index[c] = src[c * 4 + 2];
			if (index[c] > 88) index[c] = 88;
			dst[c] = (short)predictor[c];
		}
		const unsigned char* d = src + 4 * ch;
		if (ch == 1)
		{
			for (int i = 1; i < samplesPerBlock; i += 2)
			{
				const unsigned char v = *d++;
				dst[i] = imaDecodeNibble(v & 15, predictor[0], index[0]);
				if (i + 1 < samplesPerBlock)
					dst[i + 1] = imaDecodeNibble(v >> 4, predictor[0], index[0]);
			}
		}
		else
		{
			// Groups of 8 samples: 4 bytes for the left channel followed by 4 bytes for the right channel.
			for (int i = 1; i < samplesPerBlock; i += 8)
			{
				for (int c = 0; c < 2; ++c)
					for (int k = 0; k < 4; ++k)
					{
						const unsigned char v = *d++;
						const int s0 = i + k * 2;
						if (s0 < samplesPerBlock)
							dst[s0 * 2 + c] = imaDecodeNibble(v & 15, predictor[c], index[c]);
						if (s0 + 1 < samplesPerBlock)
							dst[(s0 + 1) * 2 + c] = imaDecodeNibble(v >> 4, predictor[c], index[c]);
					}
			}
		}
	}

	outSize = 44 + pcmBytes;
	return out;
}

// ------------------------------------------------------------------------------------------------
// Looping data source. Plays the decoder N times (Miles loop count semantics), gapless.
// ------------------------------------------------------------------------------------------------
struct LoopSource
{
	ma_data_source_base base;
	ma_decoder* decoder;
	volatile LONG remaining; // plays left including the current one, <= 0 means endless
	volatile LONG loopsDone;
};

static ma_result loopRead(ma_data_source* ds, void* out, ma_uint64 frameCount, ma_uint64* pRead)
{
	LoopSource* s = (LoopSource*)ds;
	ma_format format; ma_uint32 channels;
	ma_data_source_get_data_format(s->decoder, &format, &channels, nullptr, nullptr, 0);
	const size_t bpf = ma_get_bytes_per_frame(format, channels);

	ma_uint64 total = 0;
	int emptyReads = 0;
	while (total < frameCount)
	{
		ma_uint64 got = 0;
		ma_result r = ma_decoder_read_pcm_frames(s->decoder, (char*)out + total * bpf, frameCount - total, &got);
		total += got;
		if (got > 0)
			emptyReads = 0;

		if (r == MA_AT_END || (r == MA_SUCCESS && got == 0))
		{
			const LONG rem = s->remaining;
			if ((rem <= 0 || rem > 1) && ++emptyReads < 3)
			{
				if (rem > 1)
					InterlockedDecrement(&s->remaining);
				InterlockedIncrement(&s->loopsDone);
				if (ma_decoder_seek_to_pcm_frame(s->decoder, 0) != MA_SUCCESS)
					break;
				continue;
			}
			*pRead = total;
			return total > 0 ? MA_SUCCESS : MA_AT_END;
		}
		if (r != MA_SUCCESS)
			break;
	}
	*pRead = total;
	return MA_SUCCESS;
}

static ma_result loopSeek(ma_data_source* ds, ma_uint64 frame)
{
	return ma_decoder_seek_to_pcm_frame(((LoopSource*)ds)->decoder, frame);
}

static ma_result loopGetFormat(ma_data_source* ds, ma_format* f, ma_uint32* c, ma_uint32* r, ma_channel* map, size_t mapCap)
{
	return ma_data_source_get_data_format(((LoopSource*)ds)->decoder, f, c, r, map, mapCap);
}

static ma_result loopGetCursor(ma_data_source* ds, ma_uint64* cursor)
{
	return ma_decoder_get_cursor_in_pcm_frames(((LoopSource*)ds)->decoder, cursor);
}

static ma_result loopGetLength(ma_data_source* ds, ma_uint64* length)
{
	return ma_decoder_get_length_in_pcm_frames(((LoopSource*)ds)->decoder, length);
}

static ma_data_source_vtable g_loopVtable = { loopRead, loopSeek, loopGetFormat, loopGetCursor, loopGetLength, nullptr, 0 };

// ------------------------------------------------------------------------------------------------
// Voices
// ------------------------------------------------------------------------------------------------
enum VoiceKind { VK_SAMPLE, VK_SAMPLE3D, VK_STREAM, VK_QUICK, VK_LISTENER };

struct Voice
{
	VoiceKind kind;
	bool allocated;

	const void* image;               // samples: external WAV image
	std::vector<unsigned char> own;  // streams: complete file data
	ma_decoder decoder;
	bool decoderReady;
	LoopSource source;
	ma_sound sound;
	bool soundReady;

	float volume, pan;
	int sampleRate;                  // native rate of the decoded data
	int rateOverride;                // value set through *_playback_rate, 0 = native
	LONG loopCount;                  // as set by the engine, 0 = endless
	S32 userData[8];

	bool started;                    // between start and stop
	volatile LONG ended;             // set by the audio thread when the sound reached its end
	bool endNotified;
	AIL_sample_callback eosSample;
	AIL_3dsample_callback eos3D;
	AIL_stream_callback eosStream;

	// 3D
	float pos[3], face[3], up[3];
	float minDist, maxDist;
	float occlusion;
};

static CRITICAL_SECTION g_lock;
static bool g_lockInit = false;
static ma_engine g_engine;
static bool g_engineReady = false;
static HANDLE g_thread = nullptr;
static volatile LONG g_threadRun = 0;
static std::list<Voice*> g_voices;      // every voice ever allocated and not yet destroyed
static Voice* g_listener = nullptr;
static DIG_DRIVER g_dig;
static AIL_file_open_callback g_fileOpen = nullptr;
static AIL_file_close_callback g_fileClose = nullptr;
static AIL_file_seek_callback g_fileSeek = nullptr;
static AIL_file_read_callback g_fileRead = nullptr;
static char g_lastError[64] = "";
static const char* kProviderName = "Miles Fast 2D Positional Audio";

struct Lock
{
	Lock() { EnterCriticalSection(&g_lock); }
	~Lock() { LeaveCriticalSection(&g_lock); }
};

static Voice* newVoice(VoiceKind kind)
{
	Lock l;

	// Released voices are recycled rather than freed: the worker thread and the engine may still hold a
	// pointer to one for a short while, and recycling keeps that memory valid.
	Voice* v = nullptr;
	for (std::list<Voice*>::iterator it = g_voices.begin(); it != g_voices.end(); ++it)
	{
		if (!(*it)->allocated && (*it)->kind == kind)
		{
			v = *it;
			break;
		}
	}
	const bool recycled = (v != nullptr);
	if (!recycled)
		v = new Voice();

	memset(&v->decoder, 0, sizeof(v->decoder));
	memset(&v->sound, 0, sizeof(v->sound));
	memset(&v->source, 0, sizeof(v->source));
	v->kind = kind;
	v->allocated = true;
	v->image = nullptr;
	v->decoderReady = false;
	v->soundReady = false;
	v->volume = 1.0f;
	v->pan = 0.5f;
	v->sampleRate = 22050;
	v->rateOverride = 0;
	v->loopCount = 1;
	memset(v->userData, 0, sizeof(v->userData));
	v->started = false;
	v->ended = 0;
	v->endNotified = false;
	v->eosSample = nullptr;
	v->eos3D = nullptr;
	v->eosStream = nullptr;
	v->pos[0] = v->pos[1] = v->pos[2] = 0.0f;
	v->face[0] = 0; v->face[1] = 1; v->face[2] = 0;
	v->up[0] = 0; v->up[1] = 0; v->up[2] = 1;
	v->minDist = 100.0f;
	v->maxDist = 1000.0f;
	v->occlusion = 0.0f;
	if (!recycled)
		g_voices.push_back(v);
	return v;
}

static void soundEnded(void* user, ma_sound*)
{
	Voice* v = (Voice*)user;
	InterlockedExchange(&v->ended, 1);
}

static void teardownPlayback(Voice* v)
{
	if (v->soundReady)
	{
		ma_sound_uninit(&v->sound);
		v->soundReady = false;
	}
	if (v->decoderReady)
	{
		ma_data_source_uninit(&v->source.base);
		ma_decoder_uninit(&v->decoder);
		v->decoderReady = false;
	}
	v->started = false;
	InterlockedExchange(&v->ended, 0);
	v->endNotified = false;
}

static void destroyVoice(Voice* v)
{
	Lock l;
	teardownPlayback(v);
	v->own.clear();
	v->own.shrink_to_fit();
	v->allocated = false;
}

static void applyGain(Voice* v);

// Creates decoder + data source + sound for the voice from a memory image.
static bool prepare(Voice* v, const void* data, size_t size)
{
	if (!g_engineReady || !data || size == 0)
		return false;

	teardownPlayback(v);

	ma_decoder_config cfg = ma_decoder_config_init(ma_format_f32, 0, 0);
	if (ma_decoder_init_memory(data, size, &cfg, &v->decoder) != MA_SUCCESS)
		return false;
	v->decoderReady = true;

	ma_format fmt; ma_uint32 ch = 0, rate = 0;
	ma_data_source_get_data_format(&v->decoder, &fmt, &ch, &rate, nullptr, 0);
	v->sampleRate = (int)rate;

	ma_data_source_config dsc = ma_data_source_config_init();
	dsc.vtable = &g_loopVtable;
	if (ma_data_source_init(&dsc, &v->source.base) != MA_SUCCESS)
	{
		ma_decoder_uninit(&v->decoder);
		v->decoderReady = false;
		return false;
	}
	v->source.decoder = &v->decoder;
	v->source.remaining = v->loopCount == 0 ? -1 : v->loopCount;
	v->source.loopsDone = 0;

	const ma_uint32 flags = MA_SOUND_FLAG_NO_SPATIALIZATION;
	if (ma_sound_init_from_data_source(&g_engine, &v->source.base, flags, nullptr, &v->sound) != MA_SUCCESS)
	{
		ma_data_source_uninit(&v->source.base);
		ma_decoder_uninit(&v->decoder);
		v->decoderReady = false;
		return false;
	}
	v->soundReady = true;
	ma_sound_set_end_callback(&v->sound, soundEnded, v);
	ma_sound_set_pan_mode(&v->sound, ma_pan_mode_balance);
	if (v->rateOverride > 0 && v->sampleRate > 0)
		ma_sound_set_pitch(&v->sound, (float)v->rateOverride / (float)v->sampleRate);
	applyGain(v);
	return true;
}

static void spatialize(Voice* v, float& gain, float& pan)
{
	gain = 1.0f;
	pan = 0.0f;
	if (!g_listener)
		return;

	const float dx = v->pos[0] - g_listener->pos[0];
	const float dy = v->pos[1] - g_listener->pos[1];
	const float dz = v->pos[2] - g_listener->pos[2];
	const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);

	if (dist > v->minDist)
	{
		const float range = v->maxDist - v->minDist;
		gain = range > 0.0f ? 1.0f - (dist - v->minDist) / range : 0.0f;
		if (gain < 0.0f) gain = 0.0f;
	}

	// Right vector of the listener on the ground plane (face x up with up = +Z).
	const float fx = g_listener->face[0], fy = g_listener->face[1];
	const float fl = std::sqrt(fx * fx + fy * fy);
	const float hd = std::sqrt(dx * dx + dy * dy);
	if (fl > 1e-4f && hd > 1.0f)
	{
		const float rx = fy / fl, ry = -fx / fl;
		pan = (dx * rx + dy * ry) / hd;
		pan *= 0.85f;
	}
}

static void applyGain(Voice* v)
{
	if (!v->soundReady)
		return;
	float gain = v->volume;
	float pan = (v->pan - 0.5f) * 2.0f;
	if (v->kind == VK_SAMPLE3D)
	{
		float sg, sp;
		spatialize(v, sg, sp);
		gain *= sg * (1.0f - v->occlusion * 0.5f);
		pan = sp;
	}
	ma_sound_set_volume(&v->sound, gain);
	ma_sound_set_pan(&v->sound, pan < -1.0f ? -1.0f : (pan > 1.0f ? 1.0f : pan));
}

static void startVoice(Voice* v)
{
	if (!v->soundReady)
		return;
	ma_sound_stop(&v->sound);
	ma_sound_seek_to_pcm_frame(&v->sound, 0);
	v->source.remaining = v->loopCount == 0 ? -1 : v->loopCount;
	v->source.loopsDone = 0;
	InterlockedExchange(&v->ended, 0);
	v->endNotified = false;
	applyGain(v);
	ma_sound_start(&v->sound);
	v->started = true;
}

static void stopVoice(Voice* v)
{
	if (v->soundReady)
		ma_sound_stop(&v->sound);
	v->started = false;
}

static void resumeVoice(Voice* v)
{
	if (v->soundReady)
	{
		ma_sound_start(&v->sound);
		v->started = true;
	}
}

static DWORD WINAPI workerMain(LPVOID)
{
	while (g_threadRun)
	{
		struct Pending { Voice* v; };
		std::vector<Pending> fire;
		{
			Lock l;
			for (std::list<Voice*>::iterator it = g_voices.begin(); it != g_voices.end(); ++it)
			{
				Voice* v = *it;
				if (!v->allocated || !v->soundReady)
					continue;
				if (v->kind == VK_SAMPLE3D && v->started)
					applyGain(v);
				if (v->ended && !v->endNotified && v->started)
				{
					v->endNotified = true;
					v->started = false;
					fire.push_back({ v });
				}
			}
		}
		for (size_t i = 0; i < fire.size(); ++i)
		{
			Voice* v = fire[i].v;
			switch (v->kind)
			{
				case VK_SAMPLE: if (v->eosSample) v->eosSample((HSAMPLE)v); break;
				case VK_SAMPLE3D: if (v->eos3D) v->eos3D((H3DPOBJECT)v); break;
				case VK_STREAM: if (v->eosStream) v->eosStream((HSTREAM)v); break;
				default: break;
			}
		}
		Sleep(10);
	}
	return 0;
}

static bool readWholeFile(const char* name, std::vector<unsigned char>& out)
{
	if (!g_fileOpen || !g_fileRead || !g_fileClose)
		return false;

	void* handle = nullptr;
	if (!g_fileOpen(name, &handle) || !handle)
		return false;

	out.clear();
	const U32 chunk = 64 * 1024;
	size_t used = 0;
	for (;;)
	{
		out.resize(used + chunk);
		const U32 got = g_fileRead(handle, &out[used], chunk);
		used += got;
		if (got < chunk)
			break;
	}
	out.resize(used);
	g_fileClose(handle);
	return used > 0;
}

static float msOf(Voice* v, bool total)
{
	if (!v->decoderReady || v->sampleRate <= 0)
		return 0.0f;
	ma_uint64 frames = 0;
	if (total)
		ma_decoder_get_length_in_pcm_frames(&v->decoder, &frames);
	else if (v->soundReady)
		ma_decoder_get_cursor_in_pcm_frames(&v->decoder, &frames);
	return (float)frames * 1000.0f / (float)v->sampleRate;
}

} // namespace

// ================================================================================================
// MilesLoader
// ================================================================================================
bool MilesLoader::isLoaded() { return true; }
bool MilesLoader::isFailed() { return false; }
unsigned long MilesLoader::getLastError() { return 0; }
bool MilesLoader::load() { return true; }
void MilesLoader::unload() {}

// ================================================================================================
// Miles API
// ================================================================================================
extern "C"
{

S32 __stdcall AIL_startup(void)
{
	if (!g_lockInit)
	{
		InitializeCriticalSection(&g_lock);
		g_lockInit = true;
	}
	return 1;
}

void __stdcall AIL_shutdown(void)
{
	if (g_threadRun)
	{
		InterlockedExchange(&g_threadRun, 0);
		WaitForSingleObject(g_thread, 2000);
		CloseHandle(g_thread);
		g_thread = nullptr;
	}
	if (g_lockInit)
	{
		Lock l;
		for (std::list<Voice*>::iterator it = g_voices.begin(); it != g_voices.end(); ++it)
		{
			teardownPlayback(*it);
			delete *it;
		}
		g_voices.clear();
		g_listener = nullptr;
	}
	if (g_engineReady)
	{
		ma_engine_uninit(&g_engine);
		g_engineReady = false;
	}
}

S32 __stdcall AIL_quick_startup(S32, S32, U32, S32, S32)
{
	AIL_startup();
	if (g_engineReady)
		return 1;

	ma_engine_config cfg = ma_engine_config_init();
	cfg.channels = 2;
	if (ma_engine_init(&cfg, &g_engine) != MA_SUCCESS)
	{
		strcpy(g_lastError, "miniaudio engine init failed");
		return 0;
	}
	g_engineReady = true;

	InterlockedExchange(&g_threadRun, 1);
	g_thread = CreateThread(nullptr, 0, workerMain, nullptr, 0, nullptr);
	return 1;
}

void __stdcall AIL_quick_handles(HDIGDRIVER* pdig, HMDIDRIVER* pmdi, HDLSDEVICE* pdls)
{
	if (pdig) *pdig = &g_dig;
	if (pmdi) *pmdi = nullptr;
	if (pdls) *pdls = nullptr;
}

char* __stdcall AIL_last_error(void) { return g_lastError; }
char* __stdcall AIL_set_redist_directory(const char*) { return nullptr; }
S32 __stdcall AIL_set_preference(U32, S32) { return 0; }
S32 __stdcall AIL_waveOutOpen(HDIGDRIVER*, LPHWAVEOUT*, S32, LPWAVEFORMAT) { return 1; }
void __stdcall AIL_waveOutClose(HDIGDRIVER) {}
void __stdcall AIL_stop_timer(HTIMER) {}
void __stdcall AIL_release_timer_handle(HTIMER) {}
U32 __stdcall AIL_get_timer_highest_delay(void) { return 0; }
void __stdcall AIL_lock(void) {}
void __stdcall AIL_unlock(void) {}
void __stdcall AIL_lock_mutex(void) {}
void __stdcall AIL_unlock_mutex(void) {}

void __stdcall AIL_set_file_callbacks(AIL_file_open_callback o, AIL_file_close_callback c, AIL_file_seek_callback s, AIL_file_read_callback r)
{
	g_fileOpen = o; g_fileClose = c; g_fileSeek = s; g_fileRead = r;
}

void __stdcall AIL_get_DirectSound_info(HSAMPLE, AILLPDIRECTSOUND* ds, AILLPDIRECTSOUNDBUFFER* dsb)
{
	if (ds) *ds = nullptr;
	if (dsb) *dsb = nullptr;
}

// ---- providers / filters ------------------------------------------------------------------------
S32 __stdcall AIL_enumerate_3D_providers(HPROENUM* next, HPROVIDER* dest, char** name)
{
	if (!next || *next != 0)
		return 0;
	*next = 1;
	if (dest) *dest = (HPROVIDER)kProviderName;
	if (name) *name = (char*)kProviderName;
	return 1;
}

S32 __stdcall AIL_enumerate_filters(HPROENUM*, HPROVIDER*, char**) { return 0; }
M3DRESULT __stdcall AIL_open_3D_provider(HPROVIDER) { return M3D_NOERR; }
void __stdcall AIL_close_3D_provider(HPROVIDER) {}
void __stdcall AIL_set_3D_speaker_type(HPROVIDER, S32) {}

H3DPOBJECT __stdcall AIL_open_3D_listener(HPROVIDER)
{
	Lock l;
	if (!g_listener)
		g_listener = newVoice(VK_LISTENER);
	return (H3DPOBJECT)g_listener;
}

void __stdcall AIL_close_3D_listener(H3DPOBJECT listener)
{
	Lock l;
	if (listener && (Voice*)listener == g_listener)
		g_listener = nullptr;
	if (listener)
		destroyVoice((Voice*)listener);
}

// ---- file info ----------------------------------------------------------------------------------
S32 __stdcall AIL_WAV_info(const void* data, AILSOUNDINFO* info)
{
	WavInfo w;
	if (!info || !parseWav(data, w))
		return 0;
	memset(info, 0, sizeof(*info));
	info->format = w.format;
	info->data_ptr = w.data;
	info->data_len = w.dataLen;
	info->rate = w.rate;
	info->bits = w.bits;
	info->channels = w.channels;
	info->block_size = w.blockAlign;
	info->initial_ptr = data;
	if (w.channels && w.bits)
		info->samples = w.dataLen / (w.channels * ((w.bits + 7) / 8));
	return 1;
}

S32 __stdcall AIL_decompress_ADPCM(const AILSOUNDINFO* info, void** outdata, U32* outsize)
{
	if (!info || !outdata || !outsize || info->format != WAVE_FORMAT_IMA_ADPCM)
		return 0;
	WavInfo w;
	memset(&w, 0, sizeof(w));
	w.format = (unsigned short)info->format;
	w.channels = (unsigned short)info->channels;
	w.bits = (unsigned short)info->bits;
	w.blockAlign = (unsigned short)info->block_size;
	w.rate = info->rate;
	w.data = (const unsigned char*)info->data_ptr;
	w.dataLen = info->data_len;
	unsigned size = 0;
	void* out = decodeImaAdpcm(w, size);
	if (!out)
		return 0;
	*outdata = out;
	*outsize = size;
	return 1;
}

void __stdcall AIL_mem_free_lock(void* ptr) { free(ptr); }

// ---- sample handles -----------------------------------------------------------------------------
HSAMPLE __stdcall AIL_allocate_sample_handle(HDIGDRIVER) { return (HSAMPLE)newVoice(VK_SAMPLE); }
H3DSAMPLE __stdcall AIL_allocate_3D_sample_handle(HPROVIDER) { return (H3DSAMPLE)newVoice(VK_SAMPLE3D); }
void __stdcall AIL_release_sample_handle(HSAMPLE s) { if (s) destroyVoice((Voice*)s); }
void __stdcall AIL_release_3D_sample_handle(H3DSAMPLE s) { if (s) destroyVoice((Voice*)s); }

void __stdcall AIL_init_sample(HSAMPLE sample)
{
	Voice* v = (Voice*)sample;
	if (!v) return;
	Lock l;
	teardownPlayback(v);
	v->image = nullptr;
	v->volume = 1.0f;
	v->pan = 0.5f;
	v->rateOverride = 0;
	v->loopCount = 1;
	v->eosSample = nullptr;
}

S32 __stdcall AIL_set_sample_file(HSAMPLE sample, const void* file_image, S32)
{
	Voice* v = (Voice*)sample;
	WavInfo w;
	if (!v || !parseWav(file_image, w))
		return 0;
	Lock l;
	v->image = file_image;
	return prepare(v, file_image, w.imageSize) ? 1 : 0;
}

S32 __stdcall AIL_set_3D_sample_file(H3DSAMPLE sample, const void* file_image)
{
	Voice* v = (Voice*)sample;
	WavInfo w;
	if (!v || !parseWav(file_image, w))
		return 0;
	Lock l;
	v->image = file_image;
	return prepare(v, file_image, w.imageSize) ? 1 : 0;
}

S32 __stdcall AIL_set_named_sample_file(HSAMPLE sample, const char*, const void* file_image, S32, S32 block)
{
	return AIL_set_sample_file(sample, file_image, block);
}

HPROVIDER __stdcall AIL_set_sample_processor(HSAMPLE, SAMPLESTAGE, HPROVIDER) { return nullptr; }
void __stdcall AIL_set_filter_sample_preference(HSAMPLE, const char*, const void*) {}

void __stdcall AIL_start_sample(HSAMPLE s) { Voice* v = (Voice*)s; if (v) { Lock l; startVoice(v); } }
void __stdcall AIL_stop_sample(HSAMPLE s) { Voice* v = (Voice*)s; if (v) { Lock l; stopVoice(v); } }
void __stdcall AIL_resume_sample(HSAMPLE s) { Voice* v = (Voice*)s; if (v) { Lock l; resumeVoice(v); } }
void __stdcall AIL_end_sample(HSAMPLE s) { Voice* v = (Voice*)s; if (v) { Lock l; stopVoice(v); } }

void __stdcall AIL_start_3D_sample(H3DSAMPLE s) { AIL_start_sample((HSAMPLE)s); }
void __stdcall AIL_stop_3D_sample(H3DSAMPLE s) { AIL_stop_sample((HSAMPLE)s); }
void __stdcall AIL_resume_3D_sample(H3DSAMPLE s) { AIL_resume_sample((HSAMPLE)s); }
void __stdcall AIL_end_3D_sample(H3DSAMPLE s) { AIL_end_sample((HSAMPLE)s); }

AIL_sample_callback __stdcall AIL_register_EOS_callback(HSAMPLE s, AIL_sample_callback cb)
{
	Voice* v = (Voice*)s; if (!v) return nullptr;
	Lock l; AIL_sample_callback old = v->eosSample; v->eosSample = cb; return old;
}

AIL_3dsample_callback __stdcall AIL_register_3D_EOS_callback(H3DSAMPLE s, AIL_3dsample_callback cb)
{
	Voice* v = (Voice*)s; if (!v) return nullptr;
	Lock l; AIL_3dsample_callback old = v->eos3D; v->eos3D = cb; return old;
}

void __stdcall AIL_set_sample_volume_pan(HSAMPLE s, F32 volume, F32 pan)
{
	Voice* v = (Voice*)s; if (!v) return;
	Lock l; v->volume = volume; v->pan = pan; applyGain(v);
}

void __stdcall AIL_sample_volume_pan(HSAMPLE s, F32* volume, F32* pan)
{
	Voice* v = (Voice*)s; if (!v) return;
	if (volume) *volume = v->volume;
	if (pan) *pan = v->pan;
}

F32 __stdcall AIL_3D_sample_volume(H3DSAMPLE s) { Voice* v = (Voice*)s; return v ? v->volume : 0.0f; }
void __stdcall AIL_set_3D_sample_volume(H3DSAMPLE s, F32 volume)
{
	Voice* v = (Voice*)s; if (!v) return;
	Lock l; v->volume = volume; applyGain(v);
}

void __stdcall AIL_set_sample_playback_rate(HSAMPLE s, S32 rate)
{
	Voice* v = (Voice*)s; if (!v) return;
	Lock l;
	v->rateOverride = rate;
	if (v->soundReady && v->sampleRate > 0 && rate > 0)
		ma_sound_set_pitch(&v->sound, (float)rate / (float)v->sampleRate);
}

S32 __stdcall AIL_sample_playback_rate(HSAMPLE s)
{
	Voice* v = (Voice*)s; if (!v) return 0;
	return v->rateOverride > 0 ? v->rateOverride : v->sampleRate;
}

void __stdcall AIL_set_3D_sample_playback_rate(H3DSAMPLE s, S32 rate) { AIL_set_sample_playback_rate((HSAMPLE)s, rate); }
S32 __stdcall AIL_3D_sample_playback_rate(H3DSAMPLE s) { return AIL_sample_playback_rate((HSAMPLE)s); }

void __stdcall AIL_set_sample_loop_count(HSAMPLE s, S32 count)
{
	Voice* v = (Voice*)s; if (!v) return;
	Lock l;
	v->loopCount = count;
	if (v->decoderReady)
		v->source.remaining = count == 0 ? -1 : count;
}

S32 __stdcall AIL_sample_loop_count(HSAMPLE s)
{
	Voice* v = (Voice*)s; if (!v) return 0;
	return v->decoderReady ? (v->source.remaining < 0 ? 0 : v->source.remaining) : v->loopCount;
}

void __stdcall AIL_set_3D_sample_loop_count(H3DSAMPLE s, U32 count) { AIL_set_sample_loop_count((HSAMPLE)s, (S32)count); }
U32 __stdcall AIL_3D_sample_loop_count(H3DSAMPLE s) { return (U32)AIL_sample_loop_count((HSAMPLE)s); }

void __stdcall AIL_sample_ms_position(HSAMPLE s, S32* total, S32* current)
{
	Voice* v = (Voice*)s; if (!v) return;
	Lock l;
	if (total) *total = (S32)msOf(v, true);
	if (current) *current = (S32)msOf(v, false);
}

void __stdcall AIL_set_sample_ms_position(HSAMPLE s, S32 pos)
{
	Voice* v = (Voice*)s; if (!v || !v->soundReady) return;
	Lock l;
	ma_sound_seek_to_pcm_frame(&v->sound, (ma_uint64)((double)pos * v->sampleRate / 1000.0));
}

void __stdcall AIL_set_3D_sample_offset(H3DSAMPLE s, U32 offset)
{
	Voice* v = (Voice*)s; if (!v || !v->soundReady) return;
	Lock l;
	ma_sound_seek_to_pcm_frame(&v->sound, offset / 2);
}

U32 __stdcall AIL_3D_sample_length(H3DSAMPLE s)
{
	Voice* v = (Voice*)s; if (!v || !v->decoderReady) return 0;
	ma_uint64 frames = 0;
	ma_decoder_get_length_in_pcm_frames(&v->decoder, &frames);
	return (U32)(frames * 2);
}

U32 __stdcall AIL_3D_sample_offset(H3DSAMPLE s)
{
	Voice* v = (Voice*)s; if (!v || !v->decoderReady) return 0;
	ma_uint64 frames = 0;
	ma_decoder_get_cursor_in_pcm_frames(&v->decoder, &frames);
	return (U32)(frames * 2);
}

void __stdcall AIL_set_sample_user_data(HSAMPLE s, U32 index, S32 value) { Voice* v = (Voice*)s; if (v && index < 8) v->userData[index] = value; }
S32 __stdcall AIL_sample_user_data(HSAMPLE s, U32 index) { Voice* v = (Voice*)s; return (v && index < 8) ? v->userData[index] : 0; }
void __stdcall AIL_set_3D_user_data(H3DPOBJECT o, U32 index, S32 value) { Voice* v = (Voice*)o; if (v && index < 8) v->userData[index] = value; }
S32 __stdcall AIL_3D_user_data(H3DPOBJECT o, U32 index) { Voice* v = (Voice*)o; return (v && index < 8) ? v->userData[index] : 0; }

// ---- 3D -----------------------------------------------------------------------------------------
void __stdcall AIL_set_3D_position(H3DPOBJECT obj, F32 x, F32 y, F32 z)
{
	Voice* v = (Voice*)obj; if (!v) return;
	Lock l;
	v->pos[0] = x; v->pos[1] = y; v->pos[2] = z;
	if (v->kind == VK_SAMPLE3D)
		applyGain(v);
}

void __stdcall AIL_set_3D_orientation(H3DPOBJECT obj, F32 xf, F32 yf, F32 zf, F32 xu, F32 yu, F32 zu)
{
	Voice* v = (Voice*)obj; if (!v) return;
	Lock l;
	v->face[0] = xf; v->face[1] = yf; v->face[2] = zf;
	v->up[0] = xu; v->up[1] = yu; v->up[2] = zu;
}

void __stdcall AIL_set_3D_velocity_vector(H3DPOBJECT, F32, F32, F32) {}
void __stdcall AIL_set_3D_sample_effects_level(H3DSAMPLE, F32) {}

void __stdcall AIL_set_3D_sample_distances(H3DSAMPLE s, F32 max_dist, F32 min_dist)
{
	Voice* v = (Voice*)s; if (!v) return;
	Lock l;
	v->maxDist = max_dist;
	v->minDist = min_dist;
}

void __stdcall AIL_set_3D_sample_occlusion(H3DSAMPLE s, F32 occlusion)
{
	Voice* v = (Voice*)s; if (!v) return;
	Lock l; v->occlusion = occlusion; applyGain(v);
}

// ---- streams ------------------------------------------------------------------------------------
HSTREAM __stdcall AIL_open_stream(HDIGDRIVER, const char* filename, S32)
{
	if (!g_engineReady || !filename)
		return nullptr;

	std::vector<unsigned char> bytes;
	if (!readWholeFile(filename, bytes))
		return nullptr;

	Voice* v = newVoice(VK_STREAM);
	{
		Lock l;
		v->own.swap(bytes);
		if (!prepare(v, &v->own[0], v->own.size()))
		{
			destroyVoice(v);
			return nullptr;
		}
	}
	return (HSTREAM)v;
}

void __stdcall AIL_close_stream(HSTREAM s) { Voice* v = (Voice*)s; if (v) destroyVoice(v); }

void __stdcall AIL_start_stream(HSTREAM s) { Voice* v = (Voice*)s; if (v) { Lock l; startVoice(v); } }

void __stdcall AIL_pause_stream(HSTREAM s, S32 onoff)
{
	Voice* v = (Voice*)s; if (!v) return;
	Lock l;
	if (onoff) stopVoice(v); else resumeVoice(v);
}

AIL_stream_callback __stdcall AIL_register_stream_callback(HSTREAM s, AIL_stream_callback cb)
{
	Voice* v = (Voice*)s; if (!v) return nullptr;
	Lock l; AIL_stream_callback old = v->eosStream; v->eosStream = cb; return old;
}

void __stdcall AIL_set_stream_volume_pan(HSTREAM s, F32 volume, F32 pan) { AIL_set_sample_volume_pan((HSAMPLE)s, volume, pan); }
void __stdcall AIL_stream_volume_pan(HSTREAM s, F32* volume, F32* pan) { AIL_sample_volume_pan((HSAMPLE)s, volume, pan); }

void __stdcall AIL_set_stream_loop_count(HSTREAM s, S32 count) { AIL_set_sample_loop_count((HSAMPLE)s, count); }
S32 __stdcall AIL_stream_loop_count(HSTREAM s) { return AIL_sample_loop_count((HSAMPLE)s); }
void __stdcall AIL_set_stream_loop_block(HSTREAM, S32, S32) {}
void __stdcall AIL_set_stream_playback_rate(HSTREAM s, S32 rate) { AIL_set_sample_playback_rate((HSAMPLE)s, rate); }
S32 __stdcall AIL_stream_playback_rate(HSTREAM s) { return AIL_sample_playback_rate((HSAMPLE)s); }
void __stdcall AIL_stream_ms_position(HSTREAM s, S32* total, S32* current) { AIL_sample_ms_position((HSAMPLE)s, total, current); }
void __stdcall AIL_set_stream_ms_position(HSTREAM s, S32 pos) { AIL_set_sample_ms_position((HSAMPLE)s, pos); }

// ---- quick play (used for forced one-shot audio such as mission briefings) ----------------------------
HAUDIO __stdcall AIL_quick_load_and_play(const char* filename, U32 loop_count, S32)
{
	if (!g_engineReady || !filename)
		return nullptr;

	std::vector<unsigned char> bytes;
	if (!readWholeFile(filename, bytes))
		return nullptr;

	Voice* v = newVoice(VK_QUICK);
	{
		Lock l;
		v->loopCount = (LONG)loop_count;
		v->own.swap(bytes);
		if (!prepare(v, &v->own[0], v->own.size()))
		{
			destroyVoice(v);
			return nullptr;
		}
		startVoice(v);
	}
	return (HAUDIO)v;
}

void __stdcall AIL_quick_set_volume(HAUDIO a, F32 volume, F32 extravol)
{
	Voice* v = (Voice*)a; if (!v) return;
	Lock l; v->volume = volume; v->pan = extravol; applyGain(v);
}

void __stdcall AIL_quick_unload(HAUDIO a) { Voice* v = (Voice*)a; if (v) destroyVoice(v); }

} // extern "C"
