#include "pch.h"
#include "netease/catalog.h"
#include "cache/audio.h"
#include "download/downloads.h"
#include "netease/config.h"
#include "netease/handle.h"
#include "netease/log.h"
#include "netease/profile.h"
#include "netease/session.h"

#include <SDK/input_impl.h>

#include <cwctype>

namespace glasswave::netease {

namespace {

std::string Utf8(std::wstring_view s) {
    if (s.empty()) return {};
    return pfc::stringcvt::string_utf8_from_wide(s.data(), s.size()).get_ptr();
}

std::wstring Wide(const char* s) {
    return s ? pfc::stringcvt::string_wide_from_utf8(s).get_ptr() : std::wstring();
}

std::vector<std::wstring> LevelsFor(Quality quality) {
    switch (quality) {
    case Quality::Lossless: return {L"lossless"};
    case Quality::Hires: return {L"hires", L"lossless", L"exhigh", L"higher", L"standard"};
    case Quality::Exhigh: return {L"exhigh", L"higher", L"standard"};
    case Quality::Standard: return {L"standard"};
    case Quality::Auto:
    default: return {L"lossless", L"exhigh", L"higher", L"standard"};
    }
}

std::string ExtensionFor(const SongUrl& url) {
    std::wstring type = url.type;
    for (auto& c : type) c = static_cast<wchar_t>(std::towlower(static_cast<wint_t>(c)));
    if (type == L"flac") return "flac";
    if (type == L"mp3") return "mp3";
    if (type == L"m4a" || type == L"mp4") return "m4a";
    if (url.level == L"lossless" || url.level == L"hires") return "flac";
    return "mp3";
}

// Opens the song's CDN stream with the headers the CDN expects.
file::ptr OpenStream(const std::wstring& url, abort_callback& abort) {
    auto request = http_client::get()->create_request("GET");
    request->add_header("User-Agent",
                        "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) "
                        "Glasswave/0.1");
    request->add_header("Referer", "https://music.163.com/");
    return request->run(Utf8(url).c_str(), abort);
}

std::wstring Lower(std::wstring_view text) {
    std::wstring value(text);
    for (wchar_t& c : value)
        if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c + L'a' - L'A');
    return value;
}

bool SameSourceVariant(const SongUrl& expected, const SongUrl& actual) {
    return Lower(expected.level) == Lower(actual.level) && Lower(expected.type) == Lower(actual.type) &&
           expected.bitrate == actual.bitrate && expected.trial == actual.trial;
}

bool CodecMatches(std::wstring_view type, std::string_view actual) {
    std::string upper(actual);
    for (char& c : upper)
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    const std::wstring expected = Lower(type);
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
std::string SourceMemo(const SongUrl& url) {
    return Utf8(Lower(url.level)) + '\t' + Utf8(Lower(url.type)) + '\t' + std::to_string(url.bitrate);
}

bool ParseSourceMemo(const std::string& memo, SongUrl& url) {
    const size_t first = memo.find('\t');
    const size_t second = first == std::string::npos ? first : memo.find('\t', first + 1);
    if (second == std::string::npos) return false;
    url = {};
    url.level = Wide(memo.substr(0, first).c_str());
    url.type = Wide(memo.substr(first + 1, second - first - 1).c_str());
    url.bitrate = std::atoi(memo.c_str() + second + 1);
    return !url.level.empty() && url.bitrate > 0;
}

struct NcmEncodedFetch {
    void operator()(cache::audio::Sink& sink, abort_callback& abort) const {
        uint64_t position = sink.Size();
        uint64_t openedAt = GetTickCount64();
        const uint64_t lifetimeMs = static_cast<uint64_t>(std::max(expiredAfterSeconds, 120)) * 1000;
        bool renewedAtPosition = false;

        file::ptr source;
        auto open = [&](const std::wstring& url) {
            source = OpenStream(url, abort);
            if (position) source->seek(position, abort);
            openedAt = GetTickCount64();
            renewedAtPosition = false;
        };
        auto renew = [&] {
            SongUrl fresh;
            std::wstring error;
            if (Session::Get().Api().GetSongUrl(songId, resolved.level, abort, fresh, error) !=
                stream::SourceStatus::Playable) {
                throw exception_io_data(
                    Utf8(error.empty() ? L"Cannot renew the stream URL." : error).c_str());
            }
            if (!SameSourceVariant(resolved, fresh)) {
                throw exception_io_data("NetEase renewed a different audio source quality or codec");
            }
            source = OpenStream(fresh.url, abort);
            if (position) source->seek(position, abort);
            openedAt = GetTickCount64();
            renewedAtPosition = true;
            Log(L"input: renewed the stream URL at byte " + std::to_wstring(position));
        };

        try {
            open(initialUrl);
        } catch (const exception_aborted&) {
            throw;
        } catch (const std::exception&) {
            renew();
        }

        std::array<uint8_t, 64 * 1024> buffer{};
        for (;;) {
            abort.check();
            if (GetTickCount64() - openedAt + 60'000 > lifetimeMs) renew();
            t_size read = 0;
            try {
                read = source->read(buffer.data(), buffer.size(), abort);
            } catch (const exception_aborted&) {
                throw;
            } catch (const std::exception&) {
                if (renewedAtPosition) throw;
                renew();
                continue;
            }
            if (!read) {
                if (!position && !renewedAtPosition) {
                    renew();
                    continue;
                }
                return;
            }
            sink.Write(buffer.data(), read, abort);
            position += read;
            renewedAtPosition = false;
        }
    }

    std::wstring initialUrl;
    int64_t songId = 0;
    SongUrl resolved;
    int expiredAfterSeconds = 1200;
};

}  // namespace

class NcmInput : public input_stubs {
public:
    void open(service_ptr_t<file> p_filehint, const char* p_path, t_input_open_reason p_reason,
              abort_callback& p_abort) {
        (void)p_filehint;
        (void)p_abort;
        if (p_reason == input_open_info_write) throw exception_tagging_unsupported();
        int64_t id = 0;
        if (!ParseSongPath(p_path, id)) throw exception_io_data("Invalid NetEase track");
        m_id = id;
        m_path = p_path;
        m_cacheKey = {};
    }

    void get_info(file_info& p_info, abort_callback& p_abort) {
        const auto song = Catalog::Get().FindSong(m_id);
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
            if (!stream::KnownTags(Catalog::Get().Handle(m_id), p_info)) {
                p_info.meta_set("title", ("NetEase song " + std::to_string(m_id)).c_str());
                p_info.info_set(stream::kPlaceholderInfo, "1");
            }
            TechInfo(p_info, p_abort);
            return;
        }
        m_reportWhenKnown = false;
        p_info.meta_set("title", Utf8(song->title).c_str());
        const std::wstring artists = JoinArtists(song->artists);
        if (!artists.empty()) p_info.meta_set("artist", Utf8(artists).c_str());
        if (!song->albumArtist.empty()) p_info.meta_set("album artist", Utf8(song->albumArtist).c_str());
        if (!song->album.empty()) p_info.meta_set("album", Utf8(song->album).c_str());
        if (song->durationMs > 0) p_info.set_length(static_cast<double>(song->durationMs) / 1000.0);
        if (!song->codec.empty()) p_info.info_set("codec", Utf8(song->codec).c_str());
        if (song->bitrate > 0) p_info.info_set_int("bitrate", (song->bitrate + 500) / 1000);  // kbps
        if (song->sampleRate > 0) p_info.info_set_int("samplerate", song->sampleRate);
        if (song->bitsPerSample > 0) p_info.info_set_int("bitspersample", song->bitsPerSample);
        p_info.info_set_int("channels", 2);
        if (song->codec == L"FLAC") p_info.info_set("encoding", "lossless");
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
                Log(L"input: song " + std::to_wstring(m_id) + L" from its downloaded file");
                return;
            }
            const auto epoch = stream::SourcePlaybackEpoch();
            Log(L"input: decoding song " + std::to_wstring(m_id));
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
                cache::audio::Source source;
                source.provider = "ncm";
                persistent = true;
                try {
                    source.accountScope = cache::AccountScope(source.provider);
                } catch (...) {
                    persistent = false;
                    source.accountScope = "ephemeral-" + std::to_string(GetCurrentProcessId());
                }
                source.identity = identity;
                source.quality = Utf8(Lower(url.level) + L":" + Lower(url.type) + L":" +
                                      std::to_wstring(url.bitrate) + (url.trial ? L":trial" : L":full"));
                source.codec = Utf8(Lower(url.type));
                source.sourceVersion = std::string(cache::audio::kProducerSalt) + ":" + source.codec + ":" +
                                       std::to_string(url.bitrate) + (url.trial ? ":trial" : ":full");
                const cache::Key key = cache::audio::MakeKey(std::move(source));

                cache::Metadata metadata;
                metadata.contentType =
                    extension == "flac" ? "audio/flac" : extension == "m4a" ? "audio/mp4" : "audio/mpeg";
                metadata.validatedAt = cache::Store::Now();
                metadata.extra = "schema=audio-v1;producer=" + std::string(cache::audio::kProducerSalt) +
                                 ";level=" + Utf8(url.level) + ";type=" + Utf8(url.type) +
                                 ";bitrate=" + std::to_string(url.bitrate) +
                                 (url.trial ? ";trial=1" : ";trial=0");
                cache::audio::Request transfer;
                transfer.key = key;
                transfer.metadata = std::move(metadata);
                if (!cachedOnly) transfer.fetch = NcmEncodedFetch{url.url, m_id, url, url.expireSeconds};
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
                    Log(L"input: song " + std::to_wstring(m_id) + L" from the cache, level " + resolved.level);
                }
            }
            if (!fromCache) {
                if (!provider.Service().EnsureStarted(p_abort, &error))
                    throw exception_io_data(
                        Utf8(error.empty() ? L"NetEase service is unavailable." : error).c_str());

                auto status = stream::SourceStatus::Unavailable;
                std::wstring requestError;
                for (const auto& level : LevelsFor(GetQuality())) {
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
                        Log(L"input: skipped unavailable song " + std::to_wstring(m_id));
                        return;
                    }
                    if (!requestError.empty()) error = requestError;
                    throw exception_io_data(
                        Utf8(error.empty() ? L"No playable source for this song." : error).c_str());
                }
                if (resolved.trial)
                    Log(L"input: playing a free trial for song " + std::to_wstring(m_id));
                Log(L"input: song " + std::to_wstring(m_id) + L", level " + resolved.level + L", type " +
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
            if (const auto song = Catalog::Get().FindSong(m_id); song && song->durationMs > 0)
                m_durationSeconds = static_cast<double>(song->durationMs) / 1000.0;

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
            Log(L"input: opened song " + std::to_wstring(m_id) + L" as stream " +
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
        int64_t id = 0;
        return ParseSongPath(p_path, id);
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
