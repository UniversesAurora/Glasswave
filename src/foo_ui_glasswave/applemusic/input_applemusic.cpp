#include "pch.h"
#include "applemusic/catalog.h"
#include "core/text.h"
#include "applemusic/client.h"
#include "applemusic/config.h"
#include "applemusic/playback_log.h"
#include "applemusic/stream_provider.h"
#include "cache/audio.h"
#include "net/http.h"
#include "net/resumable_fetch.h"
#include "core/json.h"
#include "core/task_queue.h"
#include "stream/encoded_input.h"
#include "stream/playback_source.h"
#include "stream/track_snapshot.h"

#include <SDK/initquit.h>
#include <SDK/input_impl.h>

#include <algorithm>
#include <cmath>
#include <mutex>

namespace glasswave::applemusic {
namespace {

namespace json = core::json;
namespace atmos = stream::atmos;

// Songs skipped as unavailable since a song last played. A run that meets one of them again,
// or grows long, fails as before, so repeat modes and lists of nothing else cannot spin.
std::mutex g_skipMutex;
std::vector<stream::Id> g_skipRun;
constexpr size_t kSkipRunLimit = 20;

bool AdmitSkip(const stream::Id& id) {
    std::lock_guard lock(g_skipMutex);
    if (g_skipRun.size() >= kSkipRunLimit || std::find(g_skipRun.begin(), g_skipRun.end(), id) != g_skipRun.end())
        return false;
    g_skipRun.push_back(id);
    return true;
}

void EndSkipRun() {
    std::lock_guard lock(g_skipMutex);
    g_skipRun.clear();
}

using core::Utf8;
using core::Wide;

std::string Lower(std::wstring_view text) {
    return core::LowerAscii(Utf8(text));
}

// Ends playback sessions on a worker. A session is released by whichever thread drops the last
// reference to its fetcher (a cache worker, the decoder, the main thread), and the DELETE is a
// blocking request of up to a second, so none of them waits for it. After quit nothing is sent:
// the bridge drops a session that is not asked for anyway.
class SessionCleanupQueue : public initquit {
  public:
    static void Post(std::function<void()> task) {
        State& state = Shared();
        std::lock_guard lock(state.mutex);
        if (state.stopped) return;
        if (!state.queue) state.queue = std::make_unique<core::TaskQueue>(1);
        // A queue that stops still runs what it holds: a DELETE still waiting at quit is skipped
        // rather than delaying it by up to a second each.
        state.queue->Post([&state, task = std::move(task)] {
            {
                std::lock_guard lock(state.mutex);
                if (state.stopped) return;
            }
            task();
        });
    }

    void on_quit() override {
        State& state = Shared();
        std::unique_ptr<core::TaskQueue> queue;
        {
            std::lock_guard lock(state.mutex);
            state.stopped = true;
            queue = std::move(state.queue);
        }
        if (queue) queue->Stop();
    }

  private:
    struct State {
        std::mutex mutex;
        std::unique_ptr<core::TaskQueue> queue;
        bool stopped = false;
    };
    static State& Shared() {
        static State* state = new State; // never destroyed: a late session may still be dropped
        return *state;
    }
};
static initquit_factory_t<SessionCleanupQueue> g_sessionCleanupQueue;

struct SessionCleanup {
    ~SessionCleanup() {
        if (session.empty()) return;
        SessionCleanupQueue::Post([bridge = std::move(bridge), token = std::move(token),
                                   transactionTag = std::move(transactionTag), session = std::move(session)] {
            try {
                const std::wstring cleanupBridge = bridge.empty() ? BridgeUrl() : bridge;
                const std::string cleanupToken = token.empty() ? BridgeToken() : token;
                net::HttpOptions options;
                options.method = L"DELETE";
                options.noProxy = true;
                options.timeoutMs = 1000;
                options.headers.emplace_back(L"X-Api-Token", Wide(cleanupToken));
                if (!transactionTag.empty())
                    options.headers.emplace_back(L"X-Glasswave-Probe-Transaction", Wide(transactionTag));
                abort_callback_dummy abort;
                net::SendHttp(cleanupBridge + L"/api/v1/playback/" + Encode(session), options, abort);
            } catch (...) {
                // The bridge drops a session nobody uses; nothing depends on this call.
            }
        });
    }

    std::wstring bridge;
    std::string token;
    std::string transactionTag;
    std::wstring session;
};

// The bridge keeps downloading the song into its disk cache when a connection drops, and serves a
// byte range of it once committed; until then it answers a range with 503 and Retry-After.
cache::audio::Fetcher AppleEncodedFetch(std::string url, std::string token,
                                        std::shared_ptr<SessionCleanup> cleanup) {
    net::ResumableSource source;
    source.label = "Apple Music";
    source.target.url = std::move(url);
    source.target.headers.emplace_back("X-Api-Token", std::move(token));
    if (cleanup && !cleanup->transactionTag.empty())
        source.target.headers.emplace_back("X-Glasswave-Probe-Transaction", cleanup->transactionTag);
    // The cleanup rides with the fetcher: the session ends when the transfer is dropped.
    return [source = std::move(source), cleanup = std::move(cleanup)](cache::audio::Sink& sink,
                                                                      abort_callback& abort) {
        net::FetchResumable(source, net::WriteTo(sink), abort);
    };
}

std::string AppleExtension(std::wstring_view codec, std::wstring_view declared) {
    std::string extension = Lower(declared);
    if (codec == L"atmos") return "ec3";
    if (codec == L"mp3") return "mp3";
    if (extension == "m4a" || extension == "mp4") return extension;
    return "m4a";
}

std::string AppleQuality(std::wstring_view codec, int sampleRate, int bitDepth, int bitRate, int channels,
                         std::wstring_view spatial) {
    std::ostringstream value;
    value << Lower(codec);
    if (sampleRate > 0) value << ":" << sampleRate << "Hz";
    if (bitDepth > 0) value << ":" << bitDepth << "bit";
    if (bitRate > 0) value << ":" << bitRate << "bps";
    if (channels > 0) value << ":" << channels << "ch";
    if (!spatial.empty()) value << ":" << Lower(spatial);
    return value.str();
}

std::string AppleSourceVersion(const Object& session, std::wstring_view container,
                               std::wstring_view extension, std::wstring_view mime, std::wstring_view spatial,
                               int durationMs) {
    std::wstring declared = json::GetString(session, L"sourceVersion");
    if (declared.empty()) declared = json::GetString(session, L"assetVersion");
    std::ostringstream value;
    if (!declared.empty()) value << Lower(declared);
    value << "|" << durationMs << "|" << Lower(container) << "|" << Lower(extension) << "|" << Lower(mime)
          << "|" << Lower(spatial);
    return value.str();
}

// What a playback session said it delivers: everything that names the song's cache entry and
// that the decoder is checked against. Remembered per song (cache::audio::RememberSource) so
// a cached song opens without a new session.
struct DeliveredSource {
    std::wstring asset, codec, extension, spatial, mime;
    std::string sourceVersion;
    int durationMs = 0, sampleRate = 0, bitDepth = 0, bitRate = 0, channels = 0;

    bool ValidCodec() const {
        return codec == L"alac" || codec == L"aac" || codec == L"mp3" || codec == L"atmos";
    }
    std::string ContentType() const {
        if (!mime.empty() && mime.starts_with(L"audio/")) return Utf8(mime);
        return codec == L"atmos" ? "audio/eac3" : codec == L"mp3" ? "audio/mpeg" : "audio/mp4";
    }
    std::vector<std::string> Memo() const {
        return {Utf8(asset),
                Utf8(codec),
                Utf8(extension),
                Utf8(spatial),
                Utf8(mime),
                sourceVersion,
                std::to_string(durationMs),
                std::to_string(sampleRate),
                std::to_string(bitDepth),
                std::to_string(bitRate),
                std::to_string(channels)};
    }
    bool Parse(const std::vector<std::string>& fields) {
        if (fields.size() != 11) return false;
        asset = Wide(fields[0]);
        codec = Wide(fields[1]);
        extension = Wide(fields[2]);
        spatial = Wide(fields[3]);
        mime = Wide(fields[4]);
        sourceVersion = fields[5];
        durationMs = std::atoi(fields[6].c_str());
        sampleRate = std::atoi(fields[7].c_str());
        bitDepth = std::atoi(fields[8].c_str());
        bitRate = std::atoi(fields[9].c_str());
        channels = std::atoi(fields[10].c_str());
        return !asset.empty() && ValidCodec() && durationMs > 0;
    }
};

} // namespace

// A session owns encoded media only. Its consumer is always the core's input decoder.
class AppleInput : public stream::EncodedInputBase {
  public:
    void open(file::ptr, const char* path, t_input_open_reason reason, abort_callback&) {
        if (reason == input_open_info_write) throw exception_tagging_unsupported();
        if (!Catalog::Get().Records().ParseSongPath(path, m_id))
            throw exception_io_data("Invalid Apple Music track");
        m_path = path;
        m_cacheKey = {};
    }

    void get_info(file_info& out, abort_callback& abort) {
        FillDecoderInfo(out, abort);
        if (out.get_length() <= 0 && m_durationSeconds > 0) out.set_length(m_durationSeconds);
        // What plays is kept for later runs; before it decodes, the last one kept.
        if (HasOutput())
            stream::NoteTechInfo(Catalog::Get().Handle(m_id), out);
        else
            stream::KnownTechInfo(Catalog::Get().Handle(m_id), out);
        ApplyMetadata(out);
    }

    t_filestats2 get_stats2(uint32_t, abort_callback&) { return {}; }

    static bool g_is_our_path(const char* path, const char*) {
        stream::Id id;
        return Catalog::Get().Records().ParseSongPath(path, id);
    }
    static const char* g_get_name() { return "Glasswave Apple Music input"; }
    static GUID g_get_guid() {
        return {0x35f5a924, 0x7456, 0x47fa, {0x97, 0xf7, 0xc7, 0x5c, 0xb2, 0x58, 0x16, 0x28}};
    }

  protected:
    const char* ProviderKey() const override { return "apple"; }
    std::string_view Label() const override { return "Apple Music"; }
    std::string SongIdentity() const override { return Utf8(m_id); }
    bool SongKnown() const override { return Catalog::Get().FindSong(m_id).has_value(); }
    void PublishInfo(file_info& out, abort_callback& abort) override { get_info(out, abort); }

    std::string BeginOpen(abort_callback&) override {
        m_song = Catalog::Get().FindSongRaw(m_id);
        m_asset = m_id;
        if (m_song && !m_song->catalogId.empty()) m_asset = m_song->catalogId;
        if (IsLibraryId(m_asset) && m_asset[0] != L'i')
            throw exception_io_data("This Apple Music library item is not a song");
        m_quality = GetAudioQuality();
        m_rendererAvailable = atmos::Renderer::Available();
        SetAtmosRendererReady(m_rendererAvailable);
        m_preferAtmos = GetAtmosEnabled() && m_rendererAvailable;
        m_transactionTag = ProbeTransactionTag();
        // The remembered source of a song is per request kind: a tier never replays another's.
        return m_preferAtmos                         ? "atmos"
               : m_quality == AudioQuality::High     ? "aac"
               : m_quality == AudioQuality::Lossless ? "lossless"
                                                     : "stereo";
    }

    std::optional<stream::EncodedSource> Recall(const std::vector<std::string>& memo,
                                                abort_callback&) override {
        if (!m_transactionTag.empty()) return std::nullopt;
        DeliveredSource known;
        if (!known.Parse(memo) || (known.codec == L"atmos" && !m_rendererAvailable)) return std::nullopt;
        m_delivered = known;
        return Describe(known);
    }

    std::optional<stream::EncodedSource> Resolve(abort_callback& abort) override {
        try {
            return OpenSession(abort);
        } catch (const BridgeHttpError& error) {
            // Apple will not play this song here: it opens as an empty track, so the list plays
            // on to the next one without an error every time it comes round.
            // Not from a prefetch thread: that is no playback, and must not spend the skip budget
            // of the real one (a refused prefetch fails and is tried again when it plays).
            if (error.refused() && !cache::audio::IsPrefetchThread() && AdmitSkip(m_id)) {
                playback_log::Skipped(m_id, error.what());
                return std::nullopt;
            }
            throw;
        }
    }

    bool CodecMatches(std::string_view declared, std::string_view actual) const override {
        return stream::ContainsNoCase(actual, declared);
    }

    void OnDelivered(const stream::EncodedSource&) override {
        // An uploaded song's library length is Apple's reading of the upload; the delivered
        // length replaces it in the catalog, the lists and the playlist.
        if (IsLibraryId(m_id) && m_song &&
            std::abs(m_song->track.durationMs - m_delivered.durationMs) >
                static_cast<int>(stream::kFullSongToleranceSeconds * 1000))
            fb2k::inMainThread([id = m_id, length = m_delivered.durationMs] {
                Catalog::Get().PutMeasuredLength(id, length);
            });
    }

    void OnReady() override {
        if (!cache::audio::IsPrefetchThread()) EndSkipRun();
        playback_log::Ready(m_id, m_positionSeconds);
    }
    void OnComplete() override { playback_log::Complete(m_id, m_positionSeconds); }
    void ApplyDynamicMetadata(file_info& out) override { ApplyMetadata(out); }

    void ReportFailure(std::string_view stage, std::string_view errorClass,
                       std::string_view what) noexcept override {
        if (errorClass == "non_std_exception")
            playback_log::UnknownFailure(m_id, stage, m_positionSeconds);
        else
            playback_log::Failure(m_id, stage, m_positionSeconds, errorClass, what);
    }

    stream::atmos::OutputTarget AtmosTarget() const override {
        return static_cast<stream::atmos::OutputTarget>(GetAtmosOutput());
    }

  private:
    // The source a delivered description names; its bytes come from the bridge.
    stream::EncodedSource Describe(const DeliveredSource& delivered) const {
        const std::string quality = AppleQuality(delivered.codec, delivered.sampleRate, delivered.bitDepth,
                                                 delivered.bitRate, delivered.channels, delivered.spatial);
        stream::EncodedSource source;
        source.name.identity = Utf8(delivered.asset);
        source.name.quality = quality;
        source.name.codec = Lower(delivered.codec);
        source.name.sourceVersion = delivered.sourceVersion;
        source.name.contentType = delivered.ContentType();
        source.name.extra = "codec=" + Lower(delivered.codec) + ";quality=" + quality;
        source.decodePath = "apple://decode/stream." + AppleExtension(delivered.codec, delivered.extension);
        source.durationSeconds = static_cast<double>(delivered.durationMs) / 1000.0;
        source.dolbyAtmos = delivered.codec == L"atmos";
        source.sampleRate = delivered.sampleRate;
        source.bitDepth = delivered.bitDepth;
        if (m_transactionTag.empty()) source.memo = delivered.Memo();
        return source;
    }

    Object OpenPlayback(const std::wstring& assetId, abort_callback& abort) const {
        Object capabilities;
        // The owner's quality: AAC, ALAC up to 48 kHz, or the best ALAC the song has.
        capabilities.Insert(L"lossless", winrt::Windows::Data::Json::JsonValue::CreateBooleanValue(
                                             m_quality != AudioQuality::High));
        capabilities.Insert(L"hiRes", winrt::Windows::Data::Json::JsonValue::CreateBooleanValue(
                                          m_quality == AudioQuality::HiResLossless));
        Object request;
        request.Insert(L"preferAtmos",
                       winrt::Windows::Data::Json::JsonValue::CreateBooleanValue(m_preferAtmos));
        request.Insert(L"assetId", winrt::Windows::Data::Json::JsonValue::CreateStringValue(assetId));
        request.Insert(L"capabilities", capabilities);
        return Request(L"/api/v1/playback", abort, L"POST", Utf8(json::Stringify(request)));
    }

    // Opens a playback session with the service and describes what it delivers.
    stream::EncodedSource OpenSession(abort_callback& abort) {
        stream::Id asset = m_asset;
        Object session;
        try {
            session = OpenPlayback(asset, abort);
        } catch (const BridgeHttpError& error) {
            if (error.status() != 404 || !m_song || m_song->libraryId.empty() || m_song->catalogId.empty() ||
                asset != m_song->catalogId) {
                throw;
            }
            const auto mapping = ResolveLibraryPlaybackMapping(m_song->libraryId, m_song->catalogId, abort);
            if (!mapping) throw;
            asset = mapping->reportingId;
            session = OpenPlayback(asset, abort);
        }
        const std::wstring sessionId = json::GetString(session, L"sessionId");
        std::wstring bridge;
        std::string token;
        std::shared_ptr<SessionCleanup> cleanup;
        if (!sessionId.empty()) {
            cleanup = std::make_shared<SessionCleanup>();
            cleanup->transactionTag = m_transactionTag;
            cleanup->session = sessionId;
            bridge = BridgeUrl();
            token = BridgeToken();
            cleanup->bridge = bridge;
            cleanup->token = token;
        }
        DeliveredSource delivered;
        delivered.asset = asset;
        delivered.durationMs = json::GetInt(session, L"durationMs");
        delivered.sampleRate = json::GetInt(session, L"sampleRate");
        delivered.bitDepth = json::GetInt(session, L"bitDepth");
        delivered.bitRate = json::GetInt(session, L"bitRate");
        delivered.channels = json::GetInt(session, L"channelCount");
        delivered.codec = json::GetString(session, L"codec");
        delivered.extension = json::GetString(session, L"extension");
        delivered.spatial = json::GetString(session, L"spatialAudio");
        delivered.mime = json::GetString(session, L"codecMimeType");
        const auto streams = json::GetObject(session, L"streams");
        const std::wstring audio = streams ? json::GetString(*streams, L"audio") : std::wstring();
        const std::wstring expected = L"/api/v1/playback/" + Encode(sessionId) + L"/audio";
        if (sessionId.empty() || delivered.durationMs <= 0 || audio != expected || !delivered.ValidCodec())
            throw exception_io_data("Apple Music returned an invalid full-song session");
        // A short session is a preview of a catalog song. An uploaded library song has no
        // preview, and its library length is computed from the upload and can be wrong
        // (an MP3 measured at twice its length), so its session length stands.
        if (m_song && m_song->track.durationMs > 0 && !IsLibraryId(asset) &&
            static_cast<double>(delivered.durationMs) / 1000.0 + stream::kFullSongToleranceSeconds <
                static_cast<double>(m_song->track.durationMs) / 1000.0)
            throw exception_io_data("Apple Music returned an incomplete full-song session");
        if (delivered.codec == L"atmos" && !m_rendererAvailable)
            throw exception_io_data("The Dolby Atmos renderer is unavailable");
        delivered.sourceVersion =
            AppleSourceVersion(session, json::GetString(session, L"container"), delivered.extension,
                               delivered.mime, delivered.spatial, delivered.durationMs);
        m_delivered = delivered;
        stream::EncodedSource source = Describe(delivered);
        source.fetch =
            AppleEncodedFetch(Utf8(bridge + audio + (delivered.codec == L"atmos" ? L"?container=ec3" : L"")),
                              std::move(token), std::move(cleanup));
        return source;
    }

    void ApplyMetadata(file_info& out) {
        if (const auto song = Catalog::Get().FindSong(m_id)) {
            if (!song->track.title.empty()) out.meta_set("title", Utf8(song->track.title).c_str());
            const auto artists = stream::JoinArtists(song->track.artists);
            if (!artists.empty()) out.meta_set("artist", Utf8(artists).c_str());
            if (!song->track.album.empty()) out.meta_set("album", Utf8(song->track.album).c_str());
            if (!m_decoder.is_valid() && song->track.durationMs > 0)
                out.set_length(static_cast<double>(song->track.durationMs) / 1000.0);
            // A station's song arrives without its album and artists: read them while it plays.
            if (stream::LacksLinks(song->track)) Catalog::Get().Records().Want(m_id);
        } else {
            // Read in the background and published to the metadata of every view showing it;
            // meanwhile the song's last known tags (stream/track_snapshot.h), and a stand-in
            // only for a song never seen. Either way the record is reported once it is known.
            Catalog::Get().Records().Want(m_id);
            m_reportWhenKnown = true;
            if (!stream::KnownTags(Catalog::Get().Handle(m_id), out) && !out.meta_get("title", 0)) {
                out.meta_set("title", Utf8(L"Apple Music " + m_id).c_str());
                out.info_set(stream::kPlaceholderInfo, "1");
            }
        }
    }

    stream::Id m_id;
    // What this opening read at its start (BeginOpen) and the session said.
    std::optional<Song> m_song;
    stream::Id m_asset;
    AudioQuality m_quality = AudioQuality::Lossless;
    bool m_rendererAvailable = false;
    bool m_preferAtmos = false;
    std::string m_transactionTag;
    DeliveredSource m_delivered;
};

#pragma warning(push)
#pragma warning(disable : 4100 4702)
static input_singletrack_factory_t<AppleInput, input_entry::flag_redirect> g_input;
#pragma warning(pop)

} // namespace glasswave::applemusic
