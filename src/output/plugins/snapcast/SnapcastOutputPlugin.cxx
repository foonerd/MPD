// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright The Music Player Daemon Project

#include "SnapcastOutputPlugin.hxx"
#include "Internal.hxx"
#include "Client.hxx"
#include "output/OutputAPI.hxx"
#include "output/Features.h"
#include "encoder/EncoderInterface.hxx"
#include "encoder/EncoderPlugin.hxx"
#include "encoder/Configured.hxx"
#include "encoder/plugins/WaveEncoderPlugin.hxx"
#include "net/UniqueSocketDescriptor.hxx"
#include "net/SocketAddress.hxx"
#include "event/Call.hxx"
#include "util/Domain.hxx"
#include "util/DeleteDisposer.hxx"
#include "util/SpanCast.hxx"
#include "config/Net.hxx"

#ifdef HAVE_ZEROCONF
#include "zeroconf/Helper.hxx"
#endif

#ifdef HAVE_NLOHMANN_JSON
#include <nlohmann/json.hpp>
#endif

#include <cassert>

#include <string.h>

inline
SnapcastOutput::SnapcastOutput(EventLoop &_loop, const ConfigBlock &block)
	:AudioOutput(FLAG_ENABLE_DISABLE|FLAG_PAUSE|
		     FLAG_NEED_FULLY_DEFINED_AUDIO_FORMAT),
	 ServerSocket(_loop),
	 inject_event(_loop, BIND_THIS_METHOD(OnInject)),
	 // TODO: support other encoder plugins?
	 prepared_encoder(encoder_init(wave_encoder_plugin, block))
{
	const unsigned port = block.GetBlockValue("port", 1704U);
	ServerSocketAddGeneric(*this, block.GetBlockValue("bind_to_address"),
			       port);

#ifdef HAVE_ZEROCONF
	if (block.GetBlockValue("zeroconf", true))
		zeroconf_port = port;
#endif
}

SnapcastOutput::~SnapcastOutput() noexcept = default;

inline void
SnapcastOutput::Bind()
{
	open = false;

	BlockingCall(GetEventLoop(), [this](){
		ServerSocket::Open();

#ifdef HAVE_ZEROCONF
		if (zeroconf_port > 0)
			zeroconf_helper = std::make_unique<ZeroconfHelper>
				(GetEventLoop(), "Music Player Daemon",
				 "_snapcast._tcp", zeroconf_port);
#endif
	});
}

inline void
SnapcastOutput::Unbind() noexcept
{
	assert(!open);

	BlockingCall(GetEventLoop(), [this](){
#ifdef HAVE_ZEROCONF
		zeroconf_helper.reset();
#endif

		ServerSocket::Close();
	});
}

/**
 * Creates a new #SnapcastClient object and adds it into the
 * SnapcastOutput.clients linked list.
 */
inline void
SnapcastOutput::AddClient(UniqueSocketDescriptor fd) noexcept
{
	auto *client = new SnapcastClient(*this, std::move(fd));
	clients.push_front(*client);
}

void
SnapcastOutput::OnAccept(UniqueSocketDescriptor fd,
			 SocketAddress) noexcept
{
	/* the listener socket has become readable - a client has
	   connected */

	const std::scoped_lock protect{mutex};

	/* can we allow additional client */
	if (open)
		AddClient(std::move(fd));
}

static AllocatedArray<std::byte>
ReadEncoder(Encoder &encoder) noexcept
{
	std::byte buffer[4096];

	return AllocatedArray<std::byte>{encoder.Read(std::span{buffer})};
}

inline void
SnapcastOutput::OpenEncoder(AudioFormat &audio_format)
{
	encoder = prepared_encoder->Open(audio_format);
	codec_header = ReadEncoder(*encoder);

	unflushed_input = 0;
}

void
SnapcastOutput::Open(AudioFormat &audio_format)
{
	assert(!open);
	assert(clients.empty());

	const std::scoped_lock protect{mutex};

	OpenEncoder(audio_format);

	/* initialize other attributes */

	timer = new Timer(audio_format);

	open = true;
	pause = false;
}

void
SnapcastOutput::Close() noexcept
{
	assert(open);

	delete timer;

	BlockingCall(GetEventLoop(), [this](){
		inject_event.Cancel();

		const std::scoped_lock protect{mutex};
		open = false;
		clients.clear_and_dispose(DeleteDisposer{});
	});

	ClearQueue(chunks);

	codec_header = std::span<const std::byte>{};
	delete encoder;
}

void
SnapcastOutput::OnInject() noexcept
{
	const std::scoped_lock protect{mutex};

	while (!chunks.empty()) {
		const auto chunk = std::move(chunks.front());
		chunks.pop();

		for (auto &client : clients)
			client.Push(chunk);
	}
}

void
SnapcastOutput::RemoveClient(SnapcastClient &client) noexcept
{
	assert(!clients.empty());

	client.unlink();
	delete &client;

	if (clients.empty())
		drain_cond.notify_one();
}

std::chrono::steady_clock::duration
SnapcastOutput::Delay() const noexcept
{
	if (pause) {
		/* Pause() will not do anything, it will not fill
		   the buffer and it will not update the timer;
		   therefore, we reset the timer here */
		timer->Reset();

		/* some arbitrary delay that is long enough to avoid
		   consuming too much CPU, and short enough to notice
		   new clients quickly enough */
		return std::chrono::seconds(1);
	}

	return timer->IsStarted()
		? timer->GetDelay()
		: std::chrono::steady_clock::duration::zero();
}

#ifdef HAVE_NLOHMANN_JSON

static constexpr struct {
	TagType type;
	const char *name;
} snapcast_tags[] = {
	/* these tags are mentioned in an example in
	   snapcast/common/message/stream_tags.hpp */
	{ TAG_ARTIST, "artist" },
	{ TAG_ALBUM, "album" },
	{ TAG_TITLE, "track" },
	{ TAG_MUSICBRAINZ_TRACKID, "musicbrainzid" },
};

static bool
TranslateTagType(nlohmann::json &json, const Tag &tag, TagType type,
		 const char *name) noexcept
{
	// TODO: support multiple values?
	const char *value = tag.GetValue(type);
	if (value == nullptr)
		return false;

	json.emplace(name, value);
	return true;
}

static nlohmann::json
ToJson(const Tag &tag) noexcept
{
	auto json = nlohmann::json::object();

	for (const auto [type, name] : snapcast_tags)
		TranslateTagType(json, tag, type, name);

	return json;
}

#endif

void
SnapcastOutput::SendTag(const Tag &tag)
{
#ifdef HAVE_NLOHMANN_JSON
	if (!LockHasClients())
		return;

	const auto json = ToJson(tag);
	if (json.empty())
		return;

	const auto payload = json.dump();

	const std::scoped_lock protect{mutex};
	// TODO: enqueue StreamTags, don't send directly
	for (auto &client : clients)
		client.SendStreamTags(AsBytes(payload));
#else
	(void)tag;
#endif
}

std::size_t
SnapcastOutput::Play(std::span<const std::byte> src)
{
	pause = false;

	const auto now = std::chrono::steady_clock::now();

	if (!timer->IsStarted())
		timer->Start();
	timer->Add(src.size());

	if (!LockHasClients())
		return src.size();

	encoder->Write(src);
	unflushed_input += src.size();

	if (unflushed_input >= 65536) {
		/* we have fed a lot of input into the encoder, but it
		   didn't give anything back yet - flush now to avoid
		   buffer underruns */
		try {
			encoder->Flush();
		} catch (...) {
			/* ignore */
		}

		unflushed_input = 0;
	}

	while (true) {
		std::byte buffer[32768];

		const auto payload = encoder->Read(std::span{buffer});
		if (payload.empty())
			break;

		unflushed_input = 0;

		const std::scoped_lock protect{mutex};
		if (chunks.empty())
			inject_event.Schedule();

		chunks.push(std::make_shared<SnapcastChunk>(now, AllocatedArray{payload}));
	}

	return src.size();
}

bool
SnapcastOutput::Pause()
{
	pause = true;

	return true;
}

inline bool
SnapcastOutput::IsDrained() const noexcept
{
	if (!chunks.empty())
		return false;

	return std::all_of(clients.begin(), clients.end(), [](auto&& c){ return c.IsDrained(); });
}

void
SnapcastOutput::Drain()
{
	std::unique_lock protect{mutex};
	drain_cond.wait(protect, [this]{ return IsDrained(); });
}

void
SnapcastOutput::Cancel() noexcept
{
	const std::scoped_lock protect{mutex};

	ClearQueue(chunks);

	for (auto &client : clients)
		client.Cancel();
}

const struct AudioOutputPlugin snapcast_output_plugin = {
	"snapcast",
	nullptr,
	&SnapcastOutput::Create,
	nullptr,
};
