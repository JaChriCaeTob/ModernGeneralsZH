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

// Client-side only list of expanding explosion shockwaves. It holds no game logic state: it is fed by the FX
// system (see FXList.cpp) and read by the renderer (W3DShockwave.cpp), which draws a screen space refraction ring.

#pragma once

#include "Lib/BaseType.h"

struct ShockwaveInstance
{
	Coord3D pos;               ///< world position of the explosion
	Real maxRadius;            ///< world radius the ring reaches at the end of its life
	Real strength;             ///< 0..1, scales the distortion
	UnsignedInt startMs;       ///< wall clock start time (timeGetTime)
	UnsignedInt durationMs;
};

class ShockwaveList
{
public:
	static ShockwaveList& instance();

	/// Adds a shockwave. Near duplicates (several FX of one explosion) are merged.
	void spawn(const Coord3D &pos, Real maxRadius, Real strength);

	/// Convenience for the camera shake types of View::CameraShakeType.
	void spawnFromShake(const Coord3D &pos, Int shakeType);

	/// Drops finished waves. Returns whether any wave is still active.
	Bool update();

	Int count() const { return m_count; }
	const ShockwaveInstance& get(Int index) const { return m_waves[index]; }

	/// Animation progress 0..1 of a wave.
	static Real progress(const ShockwaveInstance &wave, UnsignedInt nowMs);

	Bool isEnabled();
	void setEnabled(Bool enabled) { m_enabled = enabled ? 1 : 0; }

private:
	ShockwaveList() : m_count(0), m_enabled(-1) {}

	enum { MAX_WAVES = 8 };
	ShockwaveInstance m_waves[MAX_WAVES];
	Int m_count;
	Int m_enabled; ///< -1 = not read from the options yet
};

#define TheShockwaves (ShockwaveList::instance())
