// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright The Music Player Daemon Project

#include "FingerprintCommands.hxx"
#include "Request.hxx"
#include "LocateUri.hxx"
#include "lib/chromaprint/DecoderClient.hxx"
#include "decoder/DecoderAPI.hxx"
#include "decoder/DecoderList.hxx"
#include "storage/StorageInterface.hxx"
#include "client/Client.hxx"
#include "client/Response.hxx"
#include "client/ThreadBackgroundCommand.hxx"
#include "db/Features.hxx" // for ENABLE_DATABASE
#include "input/InputStream.hxx"
#include "input/LocalOpen.hxx"
#include "input/Handler.hxx"
#include "thread/Mutex.hxx"
#include "thread/Cond.hxx"
#include "system/Error.hxx"
#include "util/MimeType.hxx"
#include "util/UriExtract.hxx"

#include <fmt/format.h>

class GetChromaprintCommand final
	: public ThreadBackgroundCommand, ChromaprintDecoderClient, InputStreamHandler
{
	Mutex mutex;
	Cond cond;

	const std::string uri;
	const AllocatedPath path;

	bool cancel = false;

public:
	GetChromaprintCommand(Client &_client, std::string &&_uri,
			      AllocatedPath &&_path)  noexcept
		:ThreadBackgroundCommand(_client),
		 uri(std::move(_uri)), path(std::move(_path))
	{
	}

protected:
	void Run() override;

	void SendResponse(Response &r) noexcept override {
		r.Fmt("chromaprint: {}\n",
		      GetFingerprint());
	}

	void CancelThread() noexcept override {
		const std::scoped_lock lock{mutex};
		cancel = true;
		cond.notify_one();
	}

private:
	void DecodeStream(InputStream &is, const DecoderPlugin &plugin);
	bool DecodeStream(InputStream &is, std::string_view suffix,
			  const DecoderPlugin &plugin);
	void DecodeStream(InputStream &is);
	bool DecodeContainer(std::string_view suffix, const DecoderPlugin &plugin);
	bool DecodeContainer(std::string_view suffix);
	bool DecodeFile(std::string_view suffix, InputStream &is,
			const DecoderPlugin &plugin);
	void DecodeFile();

	/* virtual methods from class DecoderClient */
	InputStreamPtr OpenUri(std::string_view uri) override;
	size_t Read(InputStream &is,
		    std::span<std::byte> dest) noexcept override;

	/* virtual methods from class InputStreamHandler */
	void OnInputStreamReady() noexcept override {
		cond.notify_one();
	}

	void OnInputStreamAvailable() noexcept override {
		cond.notify_one();
	}
};

inline void
GetChromaprintCommand::DecodeStream(InputStream &input_stream,
				    const DecoderPlugin &plugin)
{
	assert(plugin.stream_decode != nullptr);
	assert(input_stream.IsReady());

	if (cancel)
		throw StopDecoder();

	/* rewind the stream, so each plugin gets a fresh start */
	try {
		input_stream.LockRewind();
	} catch (...) {
	}

	plugin.StreamDecode(*this, input_stream);
}

[[gnu::pure]]
static bool
decoder_check_plugin_mime(const DecoderPlugin &plugin,
			  const InputStream &is) noexcept
{
	assert(plugin.stream_decode != nullptr);

	const char *mime_type = is.GetMimeType();
	return mime_type != nullptr &&
		plugin.SupportsMimeType(GetMimeTypeBase(mime_type));
}

[[gnu::pure]]
static bool
decoder_check_plugin_suffix(const DecoderPlugin &plugin,
			    std::string_view suffix) noexcept
{
	assert(plugin.stream_decode != nullptr);

	return !suffix.empty() && plugin.SupportsSuffix(suffix);
}

[[gnu::pure]]
static bool
decoder_check_plugin(const DecoderPlugin &plugin, const InputStream &is,
		     std::string_view suffix) noexcept
{
	return plugin.stream_decode != nullptr &&
		(decoder_check_plugin_mime(plugin, is) ||
		 decoder_check_plugin_suffix(plugin, suffix));
}

inline bool
GetChromaprintCommand::DecodeStream(InputStream &is,
				    std::string_view suffix,
				    const DecoderPlugin &plugin)
{
	if (!decoder_check_plugin(plugin, is, suffix))
		return false;

	ChromaprintDecoderClient::Reset();

	DecodeStream(is, plugin);
	return true;
}

inline void
GetChromaprintCommand::DecodeStream(InputStream &is)
{
	const auto suffix = uri_get_suffix(uri);

	for (const auto &plugin : GetEnabledDecoderPlugins()) {
		if (DecodeStream(is, suffix, plugin))
			break;
	}
}

inline bool
GetChromaprintCommand::DecodeContainer(std::string_view suffix,
				       const DecoderPlugin &plugin)
{
	if (plugin.container_scan == nullptr ||
	    plugin.file_decode == nullptr ||
	    !plugin.SupportsSuffix(suffix))
		return false;

	ChromaprintDecoderClient::Reset();

	plugin.FileDecode(*this, path);
	return IsReady();
}

inline bool
GetChromaprintCommand::DecodeContainer(std::string_view suffix)
{
	for (const auto &plugin : GetEnabledDecoderPlugins()) {
		if (DecodeContainer(suffix, plugin))
			return true;
	}

	return false;
}

inline bool
GetChromaprintCommand::DecodeFile(std::string_view suffix, InputStream &is,
				  const DecoderPlugin &plugin)
{
	if (!plugin.SupportsSuffix(suffix))
		return false;

	{
		const std::scoped_lock protect{mutex};
		if (cancel)
			throw StopDecoder();
	}

	ChromaprintDecoderClient::Reset();

	if (plugin.file_decode != nullptr) {
		plugin.FileDecode(*this, path);
		return IsReady();
	} else if (plugin.stream_decode != nullptr) {
		plugin.StreamDecode(*this, is);
		return IsReady();
	} else
		return false;
}

inline void
GetChromaprintCommand::DecodeFile()
{
	const char *_suffix = PathTraitsUTF8::GetFilenameSuffix(uri.c_str());
	if (_suffix == nullptr)
		return;

	const std::string_view suffix{_suffix};

	InputStreamPtr input_stream;

	try {
		input_stream = OpenLocalInputStream(path, mutex);
	} catch (const std::system_error &e) {
		if (IsPathNotFound(e) &&
		    /* ENOTDIR means this may be a path inside a
		       "container" file */
		    DecodeContainer(suffix))
			return;

		throw;
	}

	assert(input_stream);

	input_stream->SetHandler(this);

	for (const auto &plugin : GetEnabledDecoderPlugins()) {
		if (DecodeFile(suffix, *input_stream, plugin))
			break;
	}
}

void
GetChromaprintCommand::Run()
try {
	if (!path.IsNull())
		DecodeFile();
	else
		DecodeStream(*OpenUri(uri));

	ChromaprintDecoderClient::Finish();
} catch (StopDecoder) {
}

InputStreamPtr
GetChromaprintCommand::OpenUri(std::string_view uri2)
{
	if (cancel)
		throw StopDecoder();

	auto is = InputStream::Open(uri2, mutex);
	is->SetHandler(this);

	std::unique_lock lock{mutex};
	while (true) {
		if (cancel)
			throw StopDecoder();

		is->Update();
		if (is->IsReady()) {
			is->Check();
			return is;
		}

		cond.wait(lock);
	}
}

size_t
GetChromaprintCommand::Read(InputStream &is,
			    std::span<std::byte> dest) noexcept
{
	/* overriding ChromaprintDecoderClient's implementation to
	   make it cancellable */

	if (dest.empty())
		return 0;

	std::unique_lock lock{mutex};

	while (true) {
		if (cancel)
			return 0;

		if (is.IsAvailable())
			break;

		cond.wait(lock);
	}

	try {
		return is.Read(lock, dest);
	} catch (...) {
		ChromaprintDecoderClient::error = std::current_exception();
		return 0;
	}
}

CommandResult
handle_getfingerprint(Client &client, Request args, Response &)
{
	const char *_uri = args.front();

	auto lu = LocateUri(UriPluginKind::INPUT, _uri, &client
#ifdef ENABLE_DATABASE
			    , nullptr
#endif
			    );

	std::string uri = lu.canonical_uri;

	switch (lu.type) {
	case LocatedUri::Type::ABSOLUTE:
		break;

	case LocatedUri::Type::PATH:
		break;

	case LocatedUri::Type::RELATIVE:
#ifdef ENABLE_DATABASE
		{
			const auto *storage = client.GetStorage();
			if (storage == nullptr)
				throw ProtocolError(ACK_ERROR_NO_EXIST, "No database");

			lu.path = storage->MapFS(lu.canonical_uri);
			if (lu.path.IsNull()) {
				uri = storage->MapUTF8(lu.canonical_uri);
				if (!uri_has_scheme(uri))
					throw ProtocolError(ACK_ERROR_NO_EXIST, "No such song");
			}
		}
#else
		throw ProtocolError(ACK_ERROR_NO_EXIST, "No database");
#endif
	}


	auto cmd = std::make_unique<GetChromaprintCommand>(client,
							   std::move(uri),
							   std::move(lu.path));
	cmd->Start();
	client.SetBackgroundCommand(std::move(cmd));
	return CommandResult::BACKGROUND;
}
