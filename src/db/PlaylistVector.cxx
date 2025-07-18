// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright The Music Player Daemon Project

#include "PlaylistVector.hxx"
#include "db/DatabaseLock.hxx"

#include <algorithm>
#include <cassert>

PlaylistVector::iterator
PlaylistVector::find(std::string_view name) noexcept
{
	assert(holding_db_lock());

	return std::find_if(begin(), end(),
			    PlaylistInfo::CompareName(name));
}

bool
PlaylistVector::UpdateOrInsert(PlaylistInfo &&pi) noexcept
{
	assert(holding_db_lock());

	auto i = find(pi.name);
	if (i != end()) {
		i->mark = true;

		if (pi.mtime == i->mtime)
			return false;

		i->mtime = pi.mtime;
	} else {
		pi.mark = true;
		push_back(std::move(pi));
	}

	return true;
}

bool
PlaylistVector::erase(std::string_view name) noexcept
{
	assert(holding_db_lock());

	auto i = find(name);
	if (i == end())
		return false;

	erase(i);
	return true;
}

bool
PlaylistVector::exists(std::string_view name) const noexcept
{
	assert(holding_db_lock());

	return std::find_if(begin(), end(),
			    PlaylistInfo::CompareName(name)) != end();
}
