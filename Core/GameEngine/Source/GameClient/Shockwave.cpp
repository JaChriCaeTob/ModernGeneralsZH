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

#include "PreRTS.h"

#include "GameClient/Shockwave.h"
#include "GameClient/View.h"
#include "Common/OptionPreferences.h"

#include <mmsystem.h>

ShockwaveList& ShockwaveList::instance()
{
	static ShockwaveList s_instance;
	return s_instance;
}

Bool ShockwaveList::isEnabled()
{
	if (m_enabled < 0)
	{
		OptionPreferences prefs;
		m_enabled = prefs.getShockwavesEnabled() ? 1 : 0;
	}
	return m_enabled != 0;
}

void ShockwaveList::spawn(const Coord3D &pos, Real maxRadius, Real strength)
{
	if (!isEnabled() || maxRadius <= 0.0f || strength <= 0.0f)
		return;

	const UnsignedInt now = timeGetTime();

	// Merge with a wave of the same explosion: close in space and just started.
	for (Int i = 0; i < m_count; ++i)
	{
		ShockwaveInstance &w = m_waves[i];
		const Real dx = w.pos.x - pos.x, dy = w.pos.y - pos.y;
		if (now - w.startMs < 150 && dx * dx + dy * dy < (w.maxRadius * 0.5f) * (w.maxRadius * 0.5f))
		{
			if (maxRadius > w.maxRadius)
			{
				w.maxRadius = maxRadius;
				w.durationMs = (UnsignedInt)(450.0f + maxRadius * 2.0f);
				if (w.durationMs > 1400) w.durationMs = 1400;
			}
			if (strength > w.strength)
				w.strength = strength;
			return;
		}
	}

	if (m_count == MAX_WAVES)
	{
		// Replace the oldest wave.
		for (Int i = 1; i < m_count; ++i)
			m_waves[i - 1] = m_waves[i];
		--m_count;
	}

	ShockwaveInstance &w = m_waves[m_count++];
	w.pos = pos;
	w.maxRadius = maxRadius;
	w.strength = strength > 1.0f ? 1.0f : strength;
	w.startMs = now;
	w.durationMs = (UnsignedInt)(450.0f + maxRadius * 2.0f);
	if (w.durationMs > 1400)
		w.durationMs = 1400;
}

void ShockwaveList::spawnFromShake(const Coord3D &pos, Int shakeType)
{
	// Radius in world units, strength 0..1, indexed by View::CameraShakeType.
	static const Real radius[]   = { 45.0f, 100.0f, 180.0f, 300.0f, 450.0f, 650.0f };
	static const Real strength[] = { 0.30f, 0.50f,  0.70f,  0.90f,  1.00f,  1.00f };
	static_assert(ARRAY_SIZE(radius) == View::SHAKE_COUNT, "Shockwave table must cover every shake type");

	if (shakeType < 0 || shakeType >= View::SHAKE_COUNT)
		return;
	spawn(pos, radius[shakeType], strength[shakeType]);
}

Bool ShockwaveList::update()
{
	const UnsignedInt now = timeGetTime();
	Int out = 0;
	for (Int i = 0; i < m_count; ++i)
	{
		if (now - m_waves[i].startMs < m_waves[i].durationMs)
			m_waves[out++] = m_waves[i];
	}
	m_count = out;
	return m_count > 0;
}

Real ShockwaveList::progress(const ShockwaveInstance &wave, UnsignedInt nowMs)
{
	const Real t = (Real)(nowMs - wave.startMs) / (Real)wave.durationMs;
	return t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
}
