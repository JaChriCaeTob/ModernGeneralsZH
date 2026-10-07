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

// Screen space explosion shockwave. After the 3D scene is drawn, the back buffer is copied into a texture (the same
// approach the smudge/heat haze system uses, no render target switching) and a refraction ring is drawn back for every
// active ShockwaveList entry. Fixed function only.

#pragma once

#include "Lib/BaseType.h"

namespace W3DShockwave
{
	/// True when at least one shockwave is active. Also removes finished shockwaves.
	Bool isActive();

	/// Copies the back buffer and draws the rings. Call right after the 3D scene was rendered into the back buffer.
	void render();

	/// Frees the capture texture, e.g. before a device reset. It is recreated on demand.
	void releaseResources();
}
