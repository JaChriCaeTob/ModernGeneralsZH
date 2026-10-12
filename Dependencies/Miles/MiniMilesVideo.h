// Not part of the Miles API: lets the video player play the sound of a movie through the miniaudio engine of MiniMiles.
// The player decodes the sound to interleaved 16 bit samples and pushes them in; they are played as soon as they arrive.
#pragma once

// returns a handle, or nullptr when the audio engine is not running (no sound card, audio off)
void* MiniMiles_VideoAudioOpen(unsigned channels, unsigned sampleRate);
void MiniMiles_VideoAudioWrite(void* handle, const short* interleaved, unsigned frames);
void MiniMiles_VideoAudioSetVolume(void* handle, float volume);	// 0..1
void MiniMiles_VideoAudioClose(void* handle);
