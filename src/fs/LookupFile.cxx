// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright The Music Player Daemon Project

#include "LookupFile.hxx"
#include "FileInfo.hxx"
#include "system/Error.hxx"

[[gnu::pure]]
static PathTraitsFS::pointer
FindSlash(PathTraitsFS::pointer p, size_t i) noexcept
{
	for (; i > 0; --i)
		if (p[i] == '/')
			return p + i;

	return nullptr;
}

ArchiveLookupResult
LookupFile(Path pathname)
{
	PathTraitsFS::string buffer(pathname.c_str());
	size_t idx = buffer.size();

	PathTraitsFS::pointer slash = nullptr;

	while (true) {
		try {
			//try to stat if its real directory
			const FileInfo file_info(Path::FromFS(buffer.c_str()));

			//is something found ins original path (is not an archive)
			if (slash == nullptr)
				return {};

			//its a file ?
			if (file_info.IsRegular()) {
				//so the upper should be file
				return {AllocatedPath::FromFS(buffer), AllocatedPath::FromFS(slash + 1)};
			} else {
				return {};
			}
		} catch (const std::system_error &e) {
			if (!IsPathNotFound(e))
				throw;

#ifdef _WIN32
			if (idx == 0)
				/* on Windows, the semantics are
				   different for empty strings:
				   GetFileAttributesExA() fails with
				   ERROR_PATH_NOT_FOUND, and the
				   IsPathNotFound() check above would
				   not rethrow the exception, but the
				   empty string would lead to an
				   integer overflow in the code below,
				   so we need to make this a special
				   case on Windows */
				throw;
#endif

		}

		//find one dir up
		if (slash != nullptr)
			*slash = '/';

		assert(idx > 0);

		slash = FindSlash(&buffer.front(), idx - 1);
		if (slash == nullptr)
			return {};

		*slash = 0;
		idx = slash - buffer.c_str();
	}
}

