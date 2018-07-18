/*
 * Copyright 2003-2018 The Music Player Daemon Project
 * http://www.musicpd.org
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "config.h"
#include "Data.hxx"
#include "Param.hxx"
#include "Block.hxx"
#include "Parser.hxx"
#include "fs/AllocatedPath.hxx"
#include "util/RuntimeError.hxx"
#include "util/StringAPI.hxx"

#include <stdlib.h>

void
ConfigData::Clear()
{
	for (auto &i : params) {
		delete i;
		i = nullptr;
	}

	for (auto &i : blocks) {
		delete i;
		i = nullptr;
	}
}

gcc_nonnull_all
static void
Append(ConfigParam *&head, ConfigParam *p)
{
	assert(p->next == nullptr);

	auto **i = &head;
	while (*i != nullptr)
		i = &(*i)->next;

	*i = p;
}

void
ConfigData::AddParam(ConfigOption option,
		     std::unique_ptr<ConfigParam> param) noexcept
{
	Append(params[size_t(option)], param.release());
}

const char *
ConfigData::GetString(ConfigOption option,
		      const char *default_value) const noexcept
{
	const auto *param = GetParam(option);
	if (param == nullptr)
		return default_value;

	return param->value.c_str();
}

AllocatedPath
ConfigData::GetPath(ConfigOption option) const
{
	const auto *param = GetParam(option);
	if (param == nullptr)
		return nullptr;

	return param->GetPath();
}

unsigned
ConfigData::GetUnsigned(ConfigOption option, unsigned default_value) const
{
	const auto *param = GetParam(option);
	long value;
	char *endptr;

	if (param == nullptr)
		return default_value;

	const char *const s = param->value.c_str();
	value = strtol(s, &endptr, 0);
	if (endptr == s || *endptr != 0 || value < 0)
		throw FormatRuntimeError("Not a valid non-negative number in line %i",
					 param->line);

	return (unsigned)value;
}

unsigned
ConfigData::GetPositive(ConfigOption option, unsigned default_value) const
{
	const auto *param = GetParam(option);
	long value;
	char *endptr;

	if (param == nullptr)
		return default_value;

	const char *const s = param->value.c_str();
	value = strtol(s, &endptr, 0);
	if (endptr == s || *endptr != 0)
		throw FormatRuntimeError("Not a valid number in line %i",
					 param->line);

	if (value <= 0)
		throw FormatRuntimeError("Not a positive number in line %i",
					 param->line);

	return (unsigned)value;
}

bool
ConfigData::GetBool(ConfigOption option, bool default_value) const
{
	const auto *param = GetParam(option);
	bool success, value;

	if (param == nullptr)
		return default_value;

	success = get_bool(param->value.c_str(), &value);
	if (!success)
		throw FormatRuntimeError("Expected boolean value (yes, true, 1) or "
					 "(no, false, 0) on line %i\n",
					 param->line);

	return value;
}

gcc_nonnull_all
static void
Append(ConfigBlock *&head, ConfigBlock *p)
{
	assert(p->next == nullptr);

	auto **i = &head;
	while (*i != nullptr)
		i = &(*i)->next;

	*i = p;
}

void
ConfigData::AddBlock(ConfigBlockOption option,
		     std::unique_ptr<ConfigBlock> block) noexcept
{
	Append(blocks[size_t(option)], block.release());
}

const ConfigBlock *
ConfigData::FindBlock(ConfigBlockOption option,
		      const char *key, const char *value) const
{
	for (const auto *block = GetBlock(option);
	     block != nullptr; block = block->next) {
		const char *value2 = block->GetBlockValue(key);
		if (value2 == nullptr)
			throw FormatRuntimeError("block without '%s' in line %d",
						 key, block->line);

		if (StringIsEqual(value2, value))
			return block;
	}

	return nullptr;
}

ConfigBlock &
ConfigData::MakeBlock(ConfigBlockOption option,
		      const char *key, const char *value)
{
	auto *block = const_cast<ConfigBlock *>(FindBlock(option, key, value));
	if (block == nullptr) {
		auto new_block = std::make_unique<ConfigBlock>();
		new_block->AddBlockParam(key, value);
		block = new_block.get();
		AddBlock(option, std::move(new_block));
	}

	return *block;
}
