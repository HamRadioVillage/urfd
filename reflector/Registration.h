/*
 *   Copyright (c) 2026 by the HamRadioVillage urfd contributors
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#pragma once

#include <ctime>
#include <string>

// Registers this reflector with the reflector list, on a schedule, from the
// service itself.
//
// The dashboard has done this from PHP on page load, which means a reflector
// nobody browses stops being listed, any visitor can force a push with
// ?callhome, and the identity hash lives in a world-writable file that the web
// server executes. None of that is a property of a web page. The payload here
// is deliberately identical to what the dashboard posts, because the receiving
// service is not ours to change.
class CRegistration
{
public:
	// Reads the config and loads or creates the identity hash. Returns false
	// when registration is switched off or cannot work, having said why.
	bool Init(void);

	bool IsEnabled(void) const { return m_Enabled; }

	// Registers if the interval has elapsed. Cheap enough to call often; the
	// maintenance thread calls it on its 100 ms tick.
	void Tick(void);

private:
	bool LoadOrCreateHash(void);
	std::string MakeHash(void) const;
	std::string BuildPayload(void) const;
	void Register(void);

	bool m_Enabled = false;
	std::string m_Url, m_HashFile, m_Hash, m_Comment, m_OverrideIp;
	unsigned m_Interval = 3600;

	std::time_t m_NextDue = 0;
	unsigned m_Consecutive = 0;	// consecutive failures
	bool m_EverSucceeded = false;
};
