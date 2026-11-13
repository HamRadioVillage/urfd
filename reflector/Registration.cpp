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

#include <cctype>
#include <fstream>
#include <iostream>
#include <random>
#include <sstream>

#include "CurlGet.h"
#include "Global.h"
#include "Registration.h"

#define HASH_LENGTH 16

bool CRegistration::Init(void)
{
	m_Enabled = g_Configure.GetBoolean(g_Keys.registration.enable);
	if (! m_Enabled)
		return false;

	m_Url.assign(g_Configure.GetString(g_Keys.registration.url));
	m_HashFile.assign(g_Configure.GetString(g_Keys.registration.hashfile));
	m_Interval = g_Configure.GetUnsigned(g_Keys.registration.interval);
	m_OverrideIp.assign(g_Configure.GetString(g_Keys.registration.overrideip));

	// An empty comment falls back to the sponsor, which is the same thing said
	// to a different audience.
	m_Comment.assign(g_Configure.GetString(g_Keys.registration.comment));
	if (m_Comment.empty())
		m_Comment.assign(g_Configure.GetString(g_Keys.names.sponsor));
	if (m_Comment.size() > 100)
		m_Comment.resize(100);	// the registry's documented limit

	if (m_Url.empty())
	{
		std::cerr << "WARNING: [Registration]Enable is true but no Url is set: not registering" << std::endl;
		m_Enabled = false;
		return false;
	}

	if (! LoadOrCreateHash())
	{
		// Registering without a stable hash would create a new listing every
		// time the reflector restarts, which is worse than not registering.
		std::cerr << "WARNING: registration is switched off because its hash file is unusable" << std::endl;
		m_Enabled = false;
		return false;
	}

	// Register promptly on startup so a restart refreshes the listing, but not
	// instantly: let the protocols and the interlink map settle first.
	m_NextDue = std::time(nullptr) + 30;
	std::cout << "Registration: " << m_Url << " every " << m_Interval << "s, first push in 30s" << std::endl;
	if (m_Interval > 600)
	{
		// Worth saying once rather than leaving someone to wonder why a healthy
		// reflector shows as down on the list most of the time.
		std::cout << "Registration: note that the reflector list's dashboard marks a reflector red" << std::endl;
		std::cout << "Registration: once its last contact is over 600s old, so an interval of "
				  << m_Interval << "s will" << std::endl;
		std::cout << "Registration: show this reflector as down for most of every cycle" << std::endl;
	}
	return true;
}

// The registry identifies a reflector by this hash, so it has to survive a
// restart. A sysop migrating from the dashboard copies the existing hash out of
// callinghome.php into this file to keep the same listing.
bool CRegistration::LoadOrCreateHash(void)
{
	std::ifstream in(m_HashFile);
	if (in.is_open())
	{
		std::getline(in, m_Hash);
		in.close();
		// trim whitespace a hand-edited file is likely to carry
		while (! m_Hash.empty() && std::isspace(static_cast<unsigned char>(m_Hash.back())))
			m_Hash.pop_back();
		auto first = m_Hash.find_first_not_of(" \t");
		if (std::string::npos != first)
			m_Hash.erase(0, first);

		if (m_Hash.empty())
		{
			std::cerr << "ERROR: " << m_HashFile << " is empty; delete it and urfd will make a new hash" << std::endl;
			return false;
		}
		for (const auto c : m_Hash)
		{
			if (! std::isalnum(static_cast<unsigned char>(c)))
			{
				std::cerr << "ERROR: the hash in " << m_HashFile << " is not alphanumeric." << std::endl;
				std::cerr << "ERROR: if you copied it from the dashboard's callinghome.php, copy only the" << std::endl;
				std::cerr << "ERROR: value of $Hash, without the quotes or the PHP around it." << std::endl;
				return false;
			}
		}
		std::cout << "Registration: using the existing hash in " << m_HashFile << std::endl;
		return true;
	}

	m_Hash = MakeHash();
	std::ofstream out(m_HashFile, std::ios::out | std::ios::trunc);
	if (! out.is_open())
	{
		std::cerr << "ERROR: cannot write " << m_HashFile << ": check the path and that urfd may write there" << std::endl;
		return false;
	}
	out << m_Hash << std::endl;
	out.close();
	std::cout << "Registration: wrote a new hash to " << m_HashFile << std::endl;
	std::cout << "Registration: keep that file; the reflector list identifies this reflector by it" << std::endl;
	return true;
}

// Same shape as the dashboard's CreateCode(16): alphanumeric, 16 characters.
// Unlike the dashboard's mt_srand(microtime()), this is seeded from the system
// random device -- an identity worth keeping deserves better than the clock.
std::string CRegistration::MakeHash(void) const
{
	static const std::string alphabet("0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ");
	std::random_device rd;
	std::uniform_int_distribution<size_t> pick(0, alphabet.size() - 1);
	std::string hash;
	hash.reserve(HASH_LENGTH);
	for (unsigned i = 0; i < HASH_LENGTH; i++)
		hash.append(1, alphabet[pick(rd)]);
	return hash;
}

void CRegistration::Tick(void)
{
	if (! m_Enabled)
		return;
	const auto now = std::time(nullptr);
	if (now < m_NextDue)
		return;
	m_NextDue = now + m_Interval;
	Register();
}

// The payload the dashboard posts today, built from what the process already
// knows rather than inferred from a pid file's ctime and a re-read of the
// interlink file.
std::string CRegistration::BuildPayload(void) const
{
	// The reflector list knows this reflector by its XLX name: the dashboard
	// registers "XLX" plus the three characters it finds after "<XLX" in the
	// XML export, which is this same patch applied to the callsign. Sending the
	// URF name instead would create a second, unrelated listing.
	CCallsign cs = g_Reflector.GetCallsign();
	cs.PatchCallsign(0, "XLX", 3);
	std::string name(cs.GetCS());
	while (! name.empty() && ' ' == name.back())
		name.pop_back();

	std::stringstream ss;
	ss << "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>" << std::endl;
	ss << "<query>CallingHome</query>" << std::endl;
	ss << "<reflector>" << std::endl;
	ss << "   <name>" << name << "</name>" << std::endl;
	ss << "   <uptime>" << (std::time(nullptr) - g_Reflector.GetStartTime()) << "</uptime>" << std::endl;
	ss << "   <hash>" << m_Hash << "</hash>" << std::endl;
	ss << "   <url>" << g_Configure.GetString(g_Keys.names.url) << "</url>" << std::endl;
	ss << "   <country>" << g_Configure.GetString(g_Keys.names.country) << "</country>" << std::endl;
	ss << "   <comment>" << m_Comment << "</comment>" << std::endl;
	ss << "   <ip>" << m_OverrideIp << "</ip>" << std::endl;
	ss << "   <reflectorversion>" << g_Version << "</reflectorversion>" << std::endl;
	ss << "</reflector>" << std::endl;

	ss << "<interlinks>" << std::endl;
	// GetInterlinkMap() takes the lock; ReleaseInterlinkMap() gives it back.
	auto interlinks = g_GateKeeper.GetInterlinkMap();
	for (auto it = interlinks->begin(); it != interlinks->end(); it++)
	{
		ss << "   <interlink>" << std::endl;
		ss << "      <name>" << it->first << "</name>" << std::endl;
		// The address only: streaming a CIp would append ":port", which the
		// dashboard's rendering of the interlink file never carried.
		ss << "      <address>" << it->second.GetIp().GetAddress() << "</address>" << std::endl;
		ss << "      <modules>" << it->second.GetModules() << "</modules>" << std::endl;
		ss << "   </interlink>" << std::endl;
	}
	g_GateKeeper.ReleaseInterlinkMap();
	// No trailing newline: this makes the payload byte-for-byte what the
	// dashboard posts, so the receiving service cannot tell the difference.
	ss << "</interlinks>";

	return ss.str();
}

void CRegistration::Register(void)
{
	const auto payload = BuildPayload();
	std::stringstream reply;
	CCurlGet curl;
	const auto code = curl.PostForm(m_Url, "xml", payload, reply, 20);

	if (CURLE_OK == code)
	{
		if (m_Consecutive > 0)
			std::cout << "Registration: recovered after " << m_Consecutive << " failed attempt(s)" << std::endl;
		else if (! m_EverSucceeded)
			std::cout << "Registration: registered with " << m_Url << std::endl;
		// Otherwise stay quiet: this runs every interval forever, and a log
		// line per push buys nothing.
		m_Consecutive = 0;
		m_EverSucceeded = true;
		return;
	}

	// Loud, but never fatal: a reflector that cannot reach the list is still a
	// working reflector, and the dashboard used to die() here.
	m_Consecutive++;
	std::cerr << "WARNING: registration with " << m_Url << " failed (curl " << code << "): "
			  << curl_easy_strerror(code) << std::endl;
	std::cerr << "WARNING: retrying in " << m_Interval << "s; this reflector may drop off the list"
			  << (m_Consecutive > 1 ? " (consecutive failures: " : " (first failure") ;
	if (m_Consecutive > 1)
		std::cerr << m_Consecutive;
	std::cerr << ")" << std::endl;
}
