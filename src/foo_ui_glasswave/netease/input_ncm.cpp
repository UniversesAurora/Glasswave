#include "pch.h"
#include "netease/catalog.h"
#include "core/text.h"
#include "cache/audio.h"
#include "download/downloads.h"
#include "netease/config.h"
#include "core/log.h"
#include "core/paths.h"
#include "net/resumable_fetch.h"
#include "netease/session.h"
#include "stream/encoded_input.h"
#include "stream/playback_source.h"
#include "stream/track_snapshot.h"

#include <SDK/input_impl.h>

namespace glasswave::netease {

namespace {

using core::LowerAscii;
using core::Utf8;
using core::Wide;

std::string ExtensionFor(const SongUrl& url) {
    const std::wstring type = LowerAscii(url.type);
    const std::wstring level = LowerAscii(url.level);
    if (type == L"flac") return "flac";
    if (type == L"mp3") return "mp3";
    if (type == L"m4a" || type == L"mp4") return "m4a";
    if (level == L"lossless" || level == L"hires") return "flac";
    return "mp3";
}

const char* ContentTypeFor(const std::string& extension) {
    return extension == "flac" ? "audio/flac" : extension == "m4a" ? "audio/mp4" : "audio/mpeg";
}

bool SameSourceVariant(const SongUrl& expected, const SongUrl& actual) {
    return LowerAscii(expected.level) == LowerAscii(actual.level) &&
           LowerAscii(expected.type) == LowerAscii(actual.type) && expected.bitrate == actual.bitrate &&
           expected.trial == actual.trial;
}

// What cache::audio::RememberSource keeps for a song: the fields that name its cache entry.
std::vector<std::string> SourceMemo(const SongUrl& url) {
    return {Utf8(LowerAscii(url.level)), Utf8(LowerAscii(url.type)), std::to_string(url.bitrate)};
}

bool ParseSourceMemo(const std::vector<std::string>& fields, SongUrl& url) {
    if (fields.size() != 3) return false;
    url = {};
    url.level = Wide(fields[0]);
    url.type = Wide(fields[1]);
    url.bitrate = std::atoi(fields[2].c_str());
    return !url.level.empty() && url.bitrate > 0;
}

// A signed URL lives expireSeconds (1200 for the CDN); one is renewed this long before it
// expires, and never lives for less than the minimum, whatever the service says.
constexpr int kMinUrlLifetimeSeconds = 120;
constexpr uint64_t kUrlRenewMarginMs = 60'000;

// The CDN answers Range requests (downloads rely on it too), so a transfer cut short or a URL
// renewed mid-song continues from the bytes already received.
net::ResumableSource FetchSource(int64_t songId, const SongUrl& resolved) {
    net::ResumableSource source;
    source.label = "NetEase";
    const std::vector<std::pair<std::string, std::string>> headers = {
        {"User-Agent",
         "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Glasswave/0.1"},
        {"Referer", "https://music.163.com/"}};
    source.target = {Utf8(resolved.url), headers};
    const uint64_t lifetimeMs =
        static_cast<uint64_t>(std::max(resolved.expireSeconds, kMinUrlLifetimeSeconds)) * 1000;
    source.renewAfterMs = lifetimeMs > kUrlRenewMarginMs ? lifetimeMs - kUrlRenewMarginMs : 0;
    source.renew = [songId, resolved, headers](abort_callback& abort) {
        SongUrl fresh;
        std::wstring error;
        if (Session::Get().Api().GetSongUrl(songId, resolved.level, abort, fresh, error) !=
            stream::SourceStatus::Playable)
            throw exception_io_data(Utf8(error.empty() ? L"Cannot renew the stream URL." : error).c_str());
        if (!SameSourceVariant(resolved, fresh))
            throw exception_io_data("NetEase renewed a different audio source quality or codec");
        return net::FetchTarget{Utf8(fresh.url), headers};
    };
    return source;
}

} // namespace

class NcmInput : public stream::EncodedInputBase {
  public:
    void open(service_ptr_t<file>, const char* path, t_input_open_reason reason, abort_callback&) {
        if (reason == input_open_info_write) throw exception_tagging_unsupported();
        stream::Id id;
        if (!Catalog::Get().Records().ParseSongPath(path, id))
            throw exception_io_data("Invalid NetEase track");
        m_id = Num(id);
        m_path = path;
        m_cacheKey = {};
    }

    void get_info(file_info& info, abort_callback& abort) {
        const auto song = Catalog::Get().FindSong(IdText(m_id));
        if (!song) {
            // A downloaded song the catalog has not seen this run: its file's tags.
            if (const auto local = download::LocalFile(L"ncm", std::to_wstring(m_id))) {
                try {
                    service_ptr_t<input_info_reader> reader;
                    input_entry::g_open_for_info_read(reader, nullptr, ("file://" + Utf8(*local)).c_str(),
                                                      abort, true);
                    reader->get_info(0, info, abort);
                    return;
                } catch (const exception_aborted&) {
                    throw;
                } catch (...) {
                }
            }
            // Its last known tags (stream/track_snapshot.h); a stand-in only for a song never
            // seen. Either way the record is reported once the catalog has it.
            m_reportWhenKnown = true;
            if (!stream::KnownTags(Catalog::Get().Records().Handle(IdText(m_id)), info)) {
                info.meta_set("title", ("NetEase song " + std::to_string(m_id)).c_str());
                info.info_set(stream::kPlaceholderInfo, "1");
            }
            TechInfo(info, abort);
            return;
        }
        m_reportWhenKnown = false;
        Catalog::Get().Records().WriteTags(song->track, info); // names, length, quality
        TechInfo(info, abort);
    }

    t_filestats2 get_stats2(uint32_t, abort_callback&) { return t_filestats2(); }

    static bool g_is_our_path(const char* path, const char*) {
        stream::Id id;
        return Catalog::Get().Records().ParseSongPath(path, id);
    }
    static const char* g_get_name() { return "Glasswave NetEase input"; }
    static GUID g_get_guid() {
        return {0x7c9f2a51, 0x3b4d, 0x4e88, {0x9a, 0x61, 0x2f, 0x7c, 0x5d, 0x0b, 0x8e, 0x34}};
    }

  protected:
    const char* ProviderKey() const override { return "ncm"; }
    std::string_view Label() const override { return "NetEase"; }
    std::string SongIdentity() const override { return std::to_string(m_id); }
    bool SongKnown() const override { return Catalog::Get().FindSong(IdText(m_id)).has_value(); }
    void PublishInfo(file_info& out, abort_callback& abort) override { get_info(out, abort); }

    // A downloaded song plays from its file while the file exists (download::Service).
    bool OpenOffline(unsigned flags, abort_callback& abort) override {
        const auto local = download::LocalFile(L"ncm", std::to_wstring(m_id));
        if (!local) return false;
        if (cache::audio::IsPrefetchThread()) return true; // nothing to fetch ahead
        const std::string location = "file://" + Utf8(*local);
        input_entry::g_open_for_decoding(m_decoder, nullptr, location.c_str(), abort, true);
        m_decoder->initialize(0, flags, abort);
        file_info_impl actual;
        m_decoder->get_info(0, actual, abort);
        m_decodePath = location;
        m_positionSeconds = 0;
        m_durationSeconds = actual.get_length();
        core::Log(L"input: song " + std::to_wstring(m_id) + L" from its downloaded file");
        MarkReady();
        return true;
    }

    std::string BeginOpen(abort_callback&) override {
        m_epoch = stream::SourcePlaybackEpoch();
        core::Log(L"input: decoding song " + std::to_wstring(m_id));
        if (!Session::Get().LoggedIn()) // started by the component's initquit, on the main thread
            throw exception_io_data("Please sign in to NetEase Cloud Music before playing this track.");
        return "quality-" + std::to_string(static_cast<int>(GetQuality()));
    }

    std::optional<stream::EncodedSource> Recall(const std::vector<std::string>& memo,
                                                abort_callback&) override {
        SongUrl known;
        if (!ParseSourceMemo(memo, known)) return std::nullopt;
        core::Log(L"input: song " + std::to_wstring(m_id) + L" from the cache, level " + known.level);
        return Describe(known);
    }

    std::optional<stream::EncodedSource> Resolve(abort_callback& abort) override {
        auto& provider = Session::Get();
        std::wstring error;
        if (!provider.Service().EnsureStarted(abort, &error))
            throw exception_io_data(Utf8(error.empty() ? L"NetEase service is unavailable." : error).c_str());

        SongUrl resolved;
        auto status = stream::SourceStatus::Unavailable;
        std::wstring requestError;
        for (const auto& level : QualityLevels(GetQuality())) {
            const auto result = provider.Api().GetSongUrl(m_id, level, abort, resolved, error);
            status = stream::CombineSourceStatus(status, result);
            if (result == stream::SourceStatus::Playable) break;
            if (result == stream::SourceStatus::Failed && requestError.empty()) requestError = error;
        }
        if (status != stream::SourceStatus::Playable) {
            if (status == stream::SourceStatus::Unavailable && !cache::audio::IsPrefetchThread() &&
                stream::SkipUnavailableSource(m_epoch, m_path, error, m_flags, abort)) {
                // No CDN request or audio decoder is created for an unavailable source. EOF lets
                // the core choose the next item in its native playback order.
                core::Log(L"input: skipped unavailable song " + std::to_wstring(m_id));
                return std::nullopt;
            }
            if (!requestError.empty()) error = requestError;
            throw exception_io_data(
                Utf8(error.empty() ? L"No playable source for this song." : error).c_str());
        }
        if (resolved.trial) core::Log(L"input: playing a free trial for song " + std::to_wstring(m_id));
        core::Log(L"input: song " + std::to_wstring(m_id) + L", level " + resolved.level + L", type " +
                  resolved.type + L", ext " + Wide(ExtensionFor(resolved)));
        stream::EncodedSource source = Describe(resolved);
        source.fetch = stream::ResumableFetcher(FetchSource(m_id, resolved));
        // A trial clip is never remembered: the full song must be asked for again.
        if (!resolved.trial) source.memo = SourceMemo(resolved);
        return source;
    }

    bool CodecMatches(std::string_view declared, std::string_view actual) const override {
        using stream::ContainsNoCase;
        if (actual.empty()) return true;
        if (declared == "flac") return ContainsNoCase(actual, "FLAC");
        if (declared == "mp3") return ContainsNoCase(actual, "MP3") || ContainsNoCase(actual, "MPEG");
        if (declared == "m4a" || declared == "mp4")
            return ContainsNoCase(actual, "AAC") || ContainsNoCase(actual, "ALAC");
        return declared.empty();
    }

    void OnDelivered(const stream::EncodedSource& source) override {
        core::Log(L"input: opened song " + std::to_wstring(m_id) + L" as stream " +
                  Wide(source.name.contentType));
    }

  private:
    // The source a resolved URL level names; its bytes come from the CDN.
    stream::EncodedSource Describe(const SongUrl& url) const {
        const std::string extension = ExtensionFor(url);
        stream::EncodedSource source;
        source.name.identity = std::to_string(m_id);
        source.name.quality = Utf8(LowerAscii(url.level) + L":" + LowerAscii(url.type) + L":" +
                                   std::to_wstring(url.bitrate) + (url.trial ? L":trial" : L":full"));
        source.name.codec = Utf8(LowerAscii(url.type));
        source.name.sourceVersion = std::string(cache::audio::kProducerSalt) + ":" + source.name.codec + ":" +
                                    std::to_string(url.bitrate) + (url.trial ? ":trial" : ":full");
        source.name.contentType = ContentTypeFor(extension);
        source.name.extra = "level=" + Utf8(url.level) + ";type=" + Utf8(url.type) +
                            ";bitrate=" + std::to_string(url.bitrate) + (url.trial ? ";trial=1" : ";trial=0");
        source.decodePath = "ncm://decode/" + std::to_string(m_id) + "." + extension;
        source.complete = !url.trial;
        if (const auto song = Catalog::Get().FindSong(IdText(m_id)); song && song->track.durationMs > 0)
            source.durationSeconds = static_cast<double>(song->track.durationMs) / 1000.0;
        return source;
    }

    // What plays: the decoder's own technical info while decoding (kept for later runs, as the
    // catalog record has it only for songs whose stream was asked for), else the last one kept.
    void TechInfo(file_info& info, abort_callback& abort) {
        const auto item = Catalog::Get().Records().Handle(IdText(m_id));
        if (m_decoder.is_valid()) {
            try {
                file_info_impl actual;
                m_decoder->get_info(0, actual, abort);
                stream::CopyTechInfo(actual, info);
            } catch (const exception_aborted&) {
                throw;
            } catch (...) {
            }
            stream::NoteTechInfo(item, info);
        } else {
            stream::KnownTechInfo(item, info);
        }
    }

    int64_t m_id = 0;
    uint64_t m_epoch = 0; // the playback attempt this opened in (stream::SourceAvailability)
};

#pragma warning(push)
#pragma warning(disable : 4100 4702) // SDK input templates forward unused parameters
static input_singletrack_factory_t<NcmInput, input_entry::flag_redirect> g_ncm_input;
#pragma warning(pop)

} // namespace glasswave::netease
