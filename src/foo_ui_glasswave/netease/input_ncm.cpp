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
#include "stream/track_snapshot.h"

#include <SDK/input_impl.h>

#include <cwctype>

namespace glasswave::netease {

namespace {

using core::Utf8;
using core::Wide;
using core::LowerAscii;

std::string ExtensionFor(const SongUrl& url) {
    std::wstring type = url.type;
    for (auto& c : type) c = static_cast<wchar_t>(std::towlower(static_cast<wint_t>(c)));
    if (type == L"flac") return "flac";
    if (type == L"mp3") return "mp3";
    if (type == L"m4a" || type == L"mp4") return "m4a";
    if (url.level == L"lossless" || url.level == L"hires") return "flac";
    return "mp3";
}

bool SameSourceVariant(const SongUrl& expected, const SongUrl& actual) {
    return LowerAscii(expected.level) == LowerAscii(actual.level) && LowerAscii(expected.type) == LowerAscii(actual.type) &&
           expected.bitrate == actual.bitrate && expected.trial == actual.trial;
}

bool CodecMatches(std::wstring_view type, std::string_view actual) {
    std::string upper(actual);
    for (char& c : upper)
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    const std::wstring expected = LowerAscii(type);
    if (expected == L"flac") return upper.find("FLAC") != std::string::npos;
    if (expected == L"mp3") return upper.find("MP3") != std::string::npos ||
                                  upper.find("MPEG") != std::string::npos;
    if (expected == L"m4a" || expected == L"mp4")
        return upper.find("AAC") != std::string::npos || upper.find("ALAC") != std::string::npos;
    return expected.empty();
}

std::string ActualCodec(file_info& info) {
    if (const char* value = info.info_get("codec"); value && *value) return value;
    pfc::string8 codec;
    if (info.info_get_codec_long(codec)) return codec.get_ptr();
    return {};
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
cache::audio::Fetcher NcmEncodedFetch(int64_t songId, const SongUrl& resolved) {
    net::ResumableSource source;
    source.label = "NetEase";
    const std::vector<std::pair<std::string, std::string>> headers = {
        {"User-Agent",
         "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Glasswave/0.1"},
        {"Referer", "https://music.163.com/"}};
    source.target = {Utf8(resolved.url), headers};
    const uint64_t lifetimeMs = static_cast<uint64_t>(std::max(resolved.expireSeconds, kMinUrlLifetimeSeconds)) * 1000;
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
    return [source = std::move(source)](cache::audio::Sink& sink, abort_callback& abort) {
        net::FetchResumable(source, net::WriteTo(sink), abort);
    };
}

}  // namespace

class NcmInput : public input_stubs {
public:
    void open(service_ptr_t<file> p_filehint, const char* p_path, t_input_open_reason p_reason,
              abort_callback& p_abort) {
        (void)p_filehint;
        (void)p_abort;
        if (p_reason == input_open_info_write) throw exception_tagging_unsupported();
        stream::Id id;
        if (!Catalog::Get().Records().ParseSongPath(p_path, id)) throw exception_io_data("Invalid NetEase track");
        m_id = Num(id);
        m_path = p_path;
        m_cacheKey = {};
    }

    void get_info(file_info& p_info, abort_callback& p_abort) {
        const auto song = Catalog::Get().FindSong(IdText(m_id));
        if (!song) {
            // A downloaded song the catalog has not seen this run: its file's tags.
            if (const auto local = download::LocalFile(L"ncm", std::to_wstring(m_id))) {
                try {
                    service_ptr_t<input_info_reader> reader;
                    input_entry::g_open_for_info_read(reader, nullptr, ("file://" + Utf8(*local)).c_str(), p_abort,
                                                      true);
                    reader->get_info(0, p_info, p_abort);
                    return;
                } catch (const exception_aborted&) {
                    throw;
                } catch (...) {
                }
            }
            // Its last known tags (stream/track_snapshot.h); a stand-in only for a song never
            // seen. Either way the record is reported once the catalog has it.
            m_reportWhenKnown = true;
            if (!stream::KnownTags(Catalog::Get().Records().Handle(IdText(m_id)), p_info)) {
                p_info.meta_set("title", ("NetEase song " + std::to_string(m_id)).c_str());
                p_info.info_set(stream::kPlaceholderInfo, "1");
            }
            TechInfo(p_info, p_abort);
            return;
        }
        m_reportWhenKnown = false;
        Catalog::Get().Records().WriteTags(song->track, p_info); // names, length, quality
        TechInfo(p_info, p_abort);
    }

    // What plays: the decoder's own technical info while decoding (kept for later runs, as the
    // catalog record has it only for songs whose stream was asked for), else the last one kept.
    void TechInfo(file_info& p_info, abort_callback& p_abort) {
        const auto item = Catalog::Get().Records().Handle(IdText(m_id));
        if (m_decoder.is_valid()) {
            try {
                file_info_impl actual;
                m_decoder->get_info(0, actual, p_abort);
                stream::CopyTechInfo(actual, p_info);
            } catch (const exception_aborted&) {
                throw;
            } catch (...) {
            }
            stream::NoteTechInfo(item, p_info);
        } else {
            stream::KnownTechInfo(item, p_info);
        }
    }

    t_filestats2 get_stats2(uint32_t, abort_callback&) { return t_filestats2(); }

    void decode_initialize(unsigned p_flags, abort_callback& p_abort) {
        if (m_decoder.is_valid()) return;
        m_techReported = false;
        stream::AnnouncePlaybackOpening(m_path, p_flags);
        try {
            // A downloaded song plays from its file while the file exists (download::Service).
            if (const auto local = download::LocalFile(L"ncm", std::to_wstring(m_id))) {
                if (cache::audio::IsPrefetchThread()) return; // nothing to fetch ahead
                const std::string location = "file://" + Utf8(*local);
                input_entry::g_open_for_decoding(m_decoder, nullptr, location.c_str(), p_abort, true);
                m_decoder->initialize(0, p_flags, p_abort);
                file_info_impl actual;
                m_decoder->get_info(0, actual, p_abort);
                m_flags = p_flags;
                m_decodePath = location;
                m_positionSeconds = 0;
                m_durationSeconds = actual.get_length();
                core::Log(L"input: song " + std::to_wstring(m_id) + L" from its downloaded file");
                return;
            }
            const auto epoch = stream::SourcePlaybackEpoch();
            core::Log(L"input: decoding song " + std::to_wstring(m_id));
            auto& provider = Session::Get();  // started by the component's initquit, on the main thread
            if (!provider.LoggedIn())
                throw exception_io_data("Please sign in to NetEase Cloud Music before playing this track.");
            std::wstring error;
            const std::string identity = std::to_string(m_id);
            m_sourceRequest = "quality-" + std::to_string(static_cast<int>(GetQuality()));
            bool persistent = true;

            // Names the cache entry of one resolved source and opens it: from the cache, by
            // joining its running transfer, or (unless cachedOnly) by starting the download.
            const auto openSource = [&](const SongUrl& url, bool cachedOnly) {
                const std::string extension = ExtensionFor(url);
                cache::audio::SourceName name;
                name.provider = "ncm";
                name.identity = identity;
                name.quality = Utf8(LowerAscii(url.level) + L":" + LowerAscii(url.type) + L":" +
                                    std::to_wstring(url.bitrate) + (url.trial ? L":trial" : L":full"));
                name.codec = Utf8(LowerAscii(url.type));
                name.sourceVersion = std::string(cache::audio::kProducerSalt) + ":" + name.codec + ":" +
                                     std::to_string(url.bitrate) + (url.trial ? ":trial" : ":full");
                name.contentType =
                    extension == "flac" ? "audio/flac" : extension == "m4a" ? "audio/mp4" : "audio/mpeg";
                name.extra = "level=" + Utf8(url.level) + ";type=" + Utf8(url.type) +
                             ";bitrate=" + std::to_string(url.bitrate) + (url.trial ? ";trial=1" : ";trial=0");
                cache::audio::NamedSource named = cache::audio::NameSource(std::move(name));
                persistent = named.persistent;
                const cache::Key key = named.key;
                cache::audio::Request transfer;
                transfer.key = key;
                transfer.metadata = std::move(named.metadata);
                if (!cachedOnly) transfer.fetch = NcmEncodedFetch(m_id, url);
                transfer.persistent = persistent;
                transfer.cachedOnly = cachedOnly;
                auto opened = cache::audio::OpenOrStart(std::move(transfer), p_abort);
                if (!opened.admitted || !opened.stream.is_valid()) return false;
                m_cacheKey = key;
                m_source = std::move(opened.stream);
                m_flight = std::move(opened.flight);
                m_trial = url.trial;
                return true;
            };

            // A song that is already in the cache starts from it at once: the service is
            // asked which source it would deliver only when there is nothing to play yet.
            SongUrl resolved;
            bool fromCache = false;
            if (const auto remembered = cache::audio::RecallSource("ncm", identity, m_sourceRequest)) {
                SongUrl known;
                if (ParseSourceMemo(*remembered, known) && openSource(known, true)) {
                    resolved = std::move(known);
                    fromCache = true;
                    core::Log(L"input: song " + std::to_wstring(m_id) + L" from the cache, level " + resolved.level);
                }
            }
            if (!fromCache) {
                if (!provider.Service().EnsureStarted(p_abort, &error))
                    throw exception_io_data(
                        Utf8(error.empty() ? L"NetEase service is unavailable." : error).c_str());

                auto status = stream::SourceStatus::Unavailable;
                std::wstring requestError;
                for (const auto& level : QualityLevels(GetQuality())) {
                    const auto result = provider.Api().GetSongUrl(m_id, level, p_abort, resolved, error);
                    status = stream::CombineSourceStatus(status, result);
                    if (result == stream::SourceStatus::Playable) break;
                    if (result == stream::SourceStatus::Failed && requestError.empty()) requestError = error;
                }
                if (status != stream::SourceStatus::Playable) {
                    if (status == stream::SourceStatus::Unavailable && !cache::audio::IsPrefetchThread() &&
                        stream::SkipUnavailableSource(epoch, m_path, error, p_flags, p_abort)) {
                        // No CDN request or audio decoder is created for an unavailable source.
                        // EOF lets the core choose the next item in its native playback order.
                        core::Log(L"input: skipped unavailable song " + std::to_wstring(m_id));
                        return;
                    }
                    if (!requestError.empty()) error = requestError;
                    throw exception_io_data(
                        Utf8(error.empty() ? L"No playable source for this song." : error).c_str());
                }
                if (resolved.trial)
                    core::Log(L"input: playing a free trial for song " + std::to_wstring(m_id));
                core::Log(L"input: song " + std::to_wstring(m_id) + L", level " + resolved.level + L", type " +
                    resolved.type + L", ext " + Wide(ExtensionFor(resolved).c_str()));
                if (!openSource(resolved, false)) throw exception_io_data("Audio prefetch capacity is full");
                // A trial clip is never remembered: the full song must be asked for again.
                if (persistent && !resolved.trial)
                    cache::audio::RememberSource("ncm", identity, m_sourceRequest, SourceMemo(resolved));
            }
            const std::string extension = ExtensionFor(resolved);
            const std::string contentType =
                extension == "flac" ? "audio/flac" : extension == "m4a" ? "audio/mp4" : "audio/mpeg";
            m_flags = p_flags;
            m_durationSeconds = 0;
            if (const auto song = Catalog::Get().FindSong(IdText(m_id)); song && song->track.durationMs > 0)
                m_durationSeconds = static_cast<double>(song->track.durationMs) / 1000.0;

            const std::string fakePath = "ncm://decode/" + std::to_string(m_id) + "." + extension;
            m_decodePath = fakePath;
            input_entry::g_open_for_decoding(m_decoder, m_source, fakePath.c_str(), p_abort, true);
            m_decoder->initialize(0, p_flags, p_abort);
            file_info_impl actual;
            m_decoder->get_info(0, actual, p_abort);
            const std::string actualCodec = ActualCodec(actual);
            if (!actualCodec.empty() && !CodecMatches(resolved.type, actualCodec))
                throw exception_io_data("NetEase decoder returned a different encoded codec");
            const double actualSeconds = actual.get_length();
            if (!m_trial && m_durationSeconds > 0 && actualSeconds > 0 &&
                actualSeconds + 2.0 < m_durationSeconds)
                throw exception_io_data("NetEase decoder received an incomplete song");
            if (m_durationSeconds <= 0 && actualSeconds > 0) m_durationSeconds = actualSeconds;
            m_positionSeconds = 0;
            core::Log(L"input: opened song " + std::to_wstring(m_id) + L" as stream " +
                Wide(contentType.c_str()));
        } catch (const exception_aborted&) {
            ReleaseDecoder();
            throw;
        } catch (...) {
            InvalidateCache();
            ReleaseDecoder();
            throw;
        }
    }

    bool decode_run(audio_chunk& p_chunk, abort_callback& p_abort) {
        if (!m_decoder.is_valid()) return false;
        try {
            const bool decoded = m_decoder->run(p_chunk, p_abort);
            if (decoded) {
                if (p_chunk.get_sample_rate())
                    m_positionSeconds +=
                        static_cast<double>(p_chunk.get_sample_count()) / p_chunk.get_sample_rate();
            } else if (!m_trial && m_durationSeconds > 2.0 && m_positionSeconds + 2.0 < m_durationSeconds) {
                throw exception_io_data("The NetEase stream ended before the complete song arrived");
            }
            return decoded;
        } catch (const exception_aborted&) {
            throw;
        } catch (...) {
            InvalidateCache();
            throw;
        }
    }

    void decode_seek(double p_seconds, abort_callback& p_abort) {
        if (!m_decoder.is_valid()) return;
        try {
            if (!m_decoder->can_seek() && m_flight) {
                const auto completed = m_flight->Completed(p_abort);
                service_ptr_t<input_decoder> decoder;
                input_entry::g_open_for_decoding(decoder, completed, m_decodePath.c_str(), p_abort, true);
                decoder->initialize(0, m_flags, p_abort);
                decoder->seek(p_seconds, p_abort);
                m_decoder = std::move(decoder);
            } else {
                m_decoder->seek(p_seconds, p_abort);
            }
            m_positionSeconds = p_seconds;
        } catch (const exception_aborted&) {
            throw;
        } catch (const exception_io_seek_out_of_range&) {
            throw;
        } catch (...) {
            InvalidateCache();
            throw;
        }
    }

    bool decode_can_seek() {
        return m_decoder.is_valid() && (m_flight || m_decoder->can_seek());
    }

    bool decode_get_dynamic_info(file_info& p_out, double& p_timestamp_delta) {
        try {
            // The song's record arrived after it opened, or decoding has just begun (its real
            // technical info): the playing info takes it now.
            if ((m_reportWhenKnown && Catalog::Get().FindSong(IdText(m_id))) || (m_decoder.is_valid() && !m_techReported)) {
                m_techReported = true;
                abort_callback_dummy abort;
                get_info(p_out, abort);
                p_timestamp_delta = 0;
                return true;
            }
            return m_decoder.is_valid() && m_decoder->get_dynamic_info(p_out, p_timestamp_delta);
        } catch (const exception_aborted&) {
            throw;
        } catch (...) {
            InvalidateCache();
            throw;
        }
    }

    bool decode_get_dynamic_info_track(file_info& p_out, double& p_timestamp_delta) {
        try {
            return m_decoder.is_valid() && m_decoder->get_dynamic_info_track(p_out, p_timestamp_delta);
        } catch (const exception_aborted&) {
            throw;
        } catch (...) {
            InvalidateCache();
            throw;
        }
    }

    void decode_on_idle(abort_callback& p_abort) {
        try {
            if (m_decoder.is_valid()) m_decoder->on_idle(p_abort);
        } catch (const exception_aborted&) {
            throw;
        } catch (...) {
            InvalidateCache();
            throw;
        }
    }

    void retag(const file_info&, abort_callback&) { throw exception_tagging_unsupported(); }
    void remove_tags(abort_callback&) { throw exception_tagging_unsupported(); }

    static bool g_is_our_content_type(const char* p_content_type) {
        (void)p_content_type;
        return false;
    }

    static bool g_is_our_path(const char* p_path, const char* p_extension) {
        (void)p_extension;
        stream::Id id;
        return Catalog::Get().Records().ParseSongPath(p_path, id);
    }

    static const char* g_get_name() { return "Glasswave NetEase input"; }

    static const GUID g_get_guid() {
        static const GUID guid = {
            0x7c9f2a51, 0x3b4d, 0x4e88, {0x9a, 0x61, 0x2f, 0x7c, 0x5d, 0x0b, 0x8e, 0x34}};
        return guid;
    }

private:
    void InvalidateCache() noexcept {
        if (m_cacheKey.identity.empty()) return;
        cache::audio::Invalidate(m_cacheKey);
        // The remembered source led to bytes the decoder rejected: resolve it again next time.
        if (!m_sourceRequest.empty()) cache::audio::ForgetSource("ncm", m_cacheKey.identity, m_sourceRequest);
    }

    void ReleaseDecoder() noexcept {
        m_decoder.release();
        m_source.release();
        m_flight.reset();
        m_cacheKey = {};
        m_positionSeconds = 0;
        m_durationSeconds = 0;
        m_trial = false;
        m_decodePath.clear();
    }

    int64_t m_id = 0;
    bool m_reportWhenKnown = false; // reported without the song's record
    bool m_techReported = false;    // the decoder's technical info went out since it opened
    std::string m_path;
    service_ptr_t<input_decoder> m_decoder;
    file::ptr m_source;
    std::shared_ptr<cache::audio::Flight> m_flight;
    cache::Key m_cacheKey;
    std::string m_sourceRequest; // what was asked for (the quality setting) when this opened
    std::string m_decodePath;
    double m_positionSeconds = 0;
    double m_durationSeconds = 0;
    unsigned m_flags = 0;
    bool m_trial = false;
};

#pragma warning(push)
#pragma warning(disable : 4100 4702)  // SDK input templates forward unused parameters
static input_singletrack_factory_t<NcmInput, input_entry::flag_redirect> g_ncm_input;
#pragma warning(pop)

}  // namespace glasswave::netease
