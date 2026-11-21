#include "pch.h"
#include "applemusic/catalog.h"
#include "applemusic/atmos_renderer.h"
#include "applemusic/client.h"
#include "applemusic/config.h"
#include "applemusic/handle.h"
#include "applemusic/playback_log.h"
#include "applemusic/stream_provider.h"
#include "cache/audio.h"
#include "net/http.h"
#include "netease/json.h"
#include "stream/playback_source.h"
#include "stream/track_snapshot.h"

#include <SDK/input_impl.h>

#include <algorithm>
#include <cmath>
#include <mutex>

namespace glasswave::applemusic {
namespace {

namespace json = netease::json;

constexpr double kFullSongDurationToleranceSeconds = 2.0;

std::string Utf8(std::wstring_view text) {
    return pfc::stringcvt::string_utf8_from_wide(text.data(), text.size()).get_ptr();
}

std::string ActualCodec(file_info& info) {
    if (const char* value = info.info_get("codec"); value && *value) return value;
    pfc::string8 codec;
    if (info.info_get_codec_long(codec)) return codec.get_ptr();
    return {};
}

bool CodecMatches(std::string_view actual, std::wstring_view declared) {
    std::string upper(actual);
    for (char& c : upper) {
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    }
    std::string expected = Utf8(declared);
    for (char& c : expected) {
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    }
    return !expected.empty() && upper.find(expected) != std::string::npos;
}

std::string Lower(std::wstring_view text) {
    std::string value = Utf8(text);
    for (char& c : value)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 'a' - 'A');
    return value;
}

struct SessionCleanup {
    ~SessionCleanup() {
        if (session.empty()) return;
        try {
            const std::wstring cleanupBridge = bridge.empty() ? BridgeUrl() : bridge;
            const std::string cleanupToken = token.empty() ? BridgeToken() : token;
            net::HttpOptions options;
            options.method = L"DELETE";
            options.noProxy = true;
            options.timeoutMs = 1000;
            options.headers.emplace_back(
                L"X-Api-Token", pfc::stringcvt::string_wide_from_utf8(cleanupToken.c_str()).get_ptr());
            if (!transactionTag.empty())
                options.headers.emplace_back(
                    L"X-Glasswave-Probe-Transaction",
                    pfc::stringcvt::string_wide_from_utf8(transactionTag.c_str()).get_ptr());
            abort_callback_dummy abort;
            net::SendHttp(cleanupBridge + L"/api/v1/playback/" + Encode(session), options, abort);
        } catch (...) {
        }
    }

    std::wstring bridge;
    std::string token;
    std::string transactionTag;
    std::wstring session;
};

struct AppleEncodedFetch {
    void operator()(cache::audio::Sink& sink, abort_callback& abort) const {
        auto request = http_client::get()->create_request("GET");
        request->add_header("X-Api-Token", token.c_str());
        if (cleanup && !cleanup->transactionTag.empty())
            request->add_header("X-Glasswave-Probe-Transaction", cleanup->transactionTag.c_str());
        const auto source = request->run(url.c_str(), abort);
        std::array<uint8_t, 64 * 1024> buffer{};
        for (;;) {
            const auto count = source->read(buffer.data(), buffer.size(), abort);
            if (!count) return;
            sink.Write(buffer.data(), count, abort);
        }
    }

    std::string url;
    std::string token;
    std::shared_ptr<SessionCleanup> cleanup;
};

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
    std::string Memo() const {
        std::string out;
        for (const std::string& field :
             {Utf8(asset), Utf8(codec), Utf8(extension), Utf8(spatial), Utf8(mime), sourceVersion,
              std::to_string(durationMs), std::to_string(sampleRate), std::to_string(bitDepth),
              std::to_string(bitRate), std::to_string(channels)}) {
            out += field;
            out += '\n';
        }
        return out;
    }
    bool Parse(const std::string& memo) {
        std::vector<std::string> fields;
        for (size_t start = 0; start < memo.size();) {
            const size_t end = memo.find('\n', start);
            if (end == std::string::npos) return false;
            fields.push_back(memo.substr(start, end - start));
            start = end + 1;
        }
        if (fields.size() != 11) return false;
        const auto wide = [](const std::string& text) {
            return std::wstring(pfc::stringcvt::string_wide_from_utf8(text.c_str()).get_ptr());
        };
        asset = wide(fields[0]);
        codec = wide(fields[1]);
        extension = wide(fields[2]);
        spatial = wide(fields[3]);
        mime = wide(fields[4]);
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
class AppleInput : public input_stubs {
  public:
    ~AppleInput() { ReleaseDecoder(); }

    void open(file::ptr, const char* path, t_input_open_reason reason, abort_callback&) {
        if (reason == input_open_info_write) throw exception_tagging_unsupported();
        if (!ParseSongPath(path, m_id)) throw exception_io_data("Invalid Apple Music track");
        m_path = path;
        m_cacheKey = {};
    }

    void get_info(file_info& out, abort_callback& abort) {
        try {
            if (m_atmos) {
                out.info_set("codec", "E-AC-3 + JOC (Dolby Atmos)");
                out.info_set("encoding", "lossy");
                out.info_set_int("samplerate", m_atmosInfo.sampleRate);
                out.info_set_int("channels", m_atmosInfo.channels);
                out.info_set_int("bitspersample", 32);
                out.info_set("atmos_renderer", "Cavern");
                out.info_set_int("atmos_objects", m_atmosInfo.dynamicObjects);
                if (m_atmosTarget == atmos::OutputTarget::Headphones) out.info_set_int("headroom_db", 6);
                if (m_durationMs > 0) out.set_length(static_cast<double>(m_durationMs) / 1000.0);
                out.info_set("atmos_output", m_atmosTarget == atmos::OutputTarget::Headphones ? "headphones"
                                             : m_atmosInfo.channels == 6                      ? "5.1 speakers"
                                             : m_atmosInfo.channels == 8 ? "7.1 speakers"
                                                                         : "stereo speakers");
            } else if (m_decoder.is_valid()) {
                m_decoder->get_info(0, out, abort);
            }
        } catch (const exception_aborted&) {
            throw;
        } catch (...) {
            InvalidateCache();
            throw;
        }
        if (out.get_length() <= 0 && m_durationMs > 0)
            out.set_length(static_cast<double>(m_durationMs) / 1000.0);
        ApplyMetadata(out);
    }

    t_filestats2 get_stats2(uint32_t, abort_callback&) { return {}; }

    void decode_initialize(unsigned flags, abort_callback& abort) {
        m_flags = flags;
        stream::AnnouncePlaybackOpening(m_path, flags);
        const auto log_failure = [this](std::string_view stage, std::string_view error_class,
                                        const std::string_view error) noexcept {
            playback_log::Failure(m_id, stage, m_positionSeconds, error_class, error);
        };
        const auto cleanup_failed_initialize = [this]() noexcept {
            ReleaseDecoder();
            m_durationMs = 0;
            m_positionSeconds = 0;
            m_report = false;
        };
        if (m_atmos) {
            try {
                SeekAtmos(0, abort);
                playback_log::Ready(m_id, m_positionSeconds);
            } catch (const exception_aborted&) {
                throw;
            } catch (...) {
                InvalidateCache();
                throw;
            }
            return;
        }
        if (m_decoder.is_valid()) {
            try {
                m_decoder->initialize(0, flags, abort);
                m_positionSeconds = 0;
                playback_log::Ready(m_id, m_positionSeconds);
            } catch (const exception_aborted&) {
                throw;
            } catch (const exception_io_data& error) {
                log_failure("decode_initialize", "io_data", error.what());
                InvalidateCache();
                throw;
            } catch (const std::exception& error) {
                log_failure("decode_initialize", "std_exception", error.what());
                InvalidateCache();
                throw;
            } catch (...) {
                playback_log::UnknownFailure(m_id, "decode_initialize", m_positionSeconds);
                InvalidateCache();
                throw;
            }
            return;
        }
        try {
            Object capabilities;
            capabilities.Insert(L"lossless", winrt::Windows::Data::Json::JsonValue::CreateBooleanValue(true));
            capabilities.Insert(L"hiRes", winrt::Windows::Data::Json::JsonValue::CreateBooleanValue(true));
            Object request;
            stream::Id asset = m_id;
            const auto song = Catalog::Get().FindSongRaw(m_id);
            if (song && !song->catalogId.empty()) asset = song->catalogId;
            if (IsLibraryId(asset) && asset[0] != L'i')
                throw exception_io_data("This Apple Music library item is not a song");
            const bool rendererAvailable = atmos::Renderer::Available();
            SetAtmosRendererReady(rendererAvailable);
            const bool preferAtmos = GetAtmosEnabled() && rendererAvailable;
            request.Insert(L"preferAtmos",
                           winrt::Windows::Data::Json::JsonValue::CreateBooleanValue(preferAtmos));
            const auto open_session = [&](const std::wstring& assetId) {
                request.Insert(L"assetId", winrt::Windows::Data::Json::JsonValue::CreateStringValue(assetId));
                request.Insert(L"capabilities", capabilities);
                return Request(L"/api/v1/playback", abort, L"POST", Utf8(json::Stringify(request)));
            };

            std::string transactionTag = ProbeTransactionTag();
            const std::string memoIdentity = Utf8(m_id);
            m_sourceRequest = preferAtmos ? "atmos" : "stereo";
            bool persistent = true;

            // Names the cache entry of one delivered source and opens it: from the cache, by
            // joining its running transfer, or (with a fetcher) by starting the download.
            const auto openSource = [&](const DeliveredSource& delivered, cache::audio::Fetcher fetch) {
                cache::audio::Source source;
                source.provider = "apple";
                persistent = true;
                try {
                    source.accountScope = cache::AccountScope(source.provider);
                } catch (...) {
                    persistent = false;
                    source.accountScope = "ephemeral-" + std::to_string(GetCurrentProcessId());
                }
                const std::string quality =
                    AppleQuality(delivered.codec, delivered.sampleRate, delivered.bitDepth, delivered.bitRate,
                                 delivered.channels, delivered.spatial);
                source.identity = Utf8(delivered.asset);
                source.quality = quality;
                source.codec = Lower(delivered.codec);
                source.sourceVersion = delivered.sourceVersion;
                const cache::Key key = cache::audio::MakeKey(std::move(source));

                cache::Metadata metadata;
                metadata.contentType = delivered.ContentType();
                metadata.validatedAt = cache::Store::Now();
                metadata.extra = "schema=audio-v1;producer=" + std::string(cache::audio::kProducerSalt) +
                                 ";codec=" + Lower(delivered.codec) + ";quality=" + quality;
                cache::audio::Request transfer;
                transfer.key = key;
                transfer.metadata = std::move(metadata);
                transfer.cachedOnly = !fetch;
                transfer.fetch = std::move(fetch);
                transfer.persistent = persistent;
                auto opened = cache::audio::OpenOrStart(std::move(transfer), abort);
                if (!opened.admitted || !opened.stream.is_valid()) return false;
                m_cacheKey = key;
                m_source = std::move(opened.stream);
                m_flight = std::move(opened.flight);
                return true;
            };

            // A song that is already in the cache starts from it at once: a playback session
            // is opened with the service only when there is nothing to play yet.
            DeliveredSource delivered;
            bool fromCache = false;
            if (transactionTag.empty()) {
                if (const auto remembered = cache::audio::RecallSource("apple", memoIdentity, m_sourceRequest)) {
                    DeliveredSource known;
                    if (known.Parse(*remembered) && (known.codec != L"atmos" || rendererAvailable) &&
                        openSource(known, {})) {
                        delivered = std::move(known);
                        fromCache = true;
                    }
                }
            }
            if (!fromCache) {
                Object session;
                try {
                    session = open_session(asset);
                } catch (const BridgeHttpError& error) {
                    if (error.status() != 404 || !song || song->libraryId.empty() ||
                        song->catalogId.empty() || asset != song->catalogId) {
                        throw;
                    }
                    const auto mapping =
                        ResolveLibraryPlaybackMapping(song->libraryId, song->catalogId, abort);
                    if (!mapping) throw;
                    asset = mapping->reportingId;
                    session = open_session(asset);
                }
                const std::wstring sessionId = json::GetString(session, L"sessionId");
                std::wstring bridge;
                std::string token;
                std::shared_ptr<SessionCleanup> cleanup;
                if (!sessionId.empty()) {
                    cleanup = std::make_shared<SessionCleanup>();
                    cleanup->transactionTag = std::move(transactionTag);
                    cleanup->session = sessionId;
                    bridge = BridgeUrl();
                    token = BridgeToken();
                    cleanup->bridge = bridge;
                    cleanup->token = token;
                }
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
                if (song && song->track.durationMs > 0 &&
                    static_cast<double>(delivered.durationMs) / 1000.0 + kFullSongDurationToleranceSeconds <
                        static_cast<double>(song->track.durationMs) / 1000.0)
                    throw exception_io_data("Apple Music returned an incomplete full-song session");
                if (delivered.codec == L"atmos" && !rendererAvailable)
                    throw exception_io_data("The Dolby Atmos renderer is unavailable");
                delivered.sourceVersion =
                    AppleSourceVersion(session, json::GetString(session, L"container"), delivered.extension,
                                       delivered.mime, delivered.spatial, delivered.durationMs);
                if (!openSource(delivered,
                                AppleEncodedFetch{Utf8(bridge + audio +
                                                       (delivered.codec == L"atmos" ? L"?container=ec3" : L"")),
                                                  token, std::move(cleanup)}))
                    throw exception_io_data("Audio prefetch capacity is full");
                if (persistent && ProbeTransactionTag().empty())
                    cache::audio::RememberSource("apple", memoIdentity, m_sourceRequest, delivered.Memo());
            }
            m_durationMs = delivered.durationMs;
            const std::wstring& codec = delivered.codec;
            const int declaredSampleRate = delivered.sampleRate;
            const int declaredBitDepth = delivered.bitDepth;
            const bool spatial = codec == L"atmos";
            m_decodePath = "apple://decode/stream." + AppleExtension(codec, delivered.extension);
            m_flags = flags;
            if (spatial) {
                const auto output = GetAtmosOutput();
                m_atmosTarget = static_cast<atmos::OutputTarget>(output);
                m_atmos = std::make_unique<atmos::Renderer>(m_source, m_atmosTarget);
                m_atmosInfo = m_atmos->WaitForInfo(abort);
                if (!m_atmosInfo.hasJocObjects)
                    throw exception_io_data("The Atmos source has no decoded JOC objects");
                if (declaredSampleRate > 0 &&
                    m_atmosInfo.sampleRate != static_cast<uint32_t>(declaredSampleRate))
                    throw exception_io_data("The Atmos source returned the wrong sample rate");
                m_positionSeconds = 0;
                m_report = true;
                playback_log::Ready(m_id, m_positionSeconds);
                return;
            }
            input_entry::g_open_for_decoding(m_decoder, m_source, m_decodePath.c_str(), abort, true);
            m_decoder->initialize(0, flags, abort);
            m_flags = flags;
            file_info_impl actual;
            m_decoder->get_info(0, actual, abort);
            const std::string actualCodec = ActualCodec(actual);
            if (!CodecMatches(actualCodec, codec))
                throw exception_io_data("Apple Music decoder returned the wrong codec");
            if (declaredSampleRate > 0 && actual.info_get_int("samplerate") != declaredSampleRate)
                throw exception_io_data("Apple Music decoder returned the wrong sample rate");
            if (declaredBitDepth > 0 && actual.info_get_int("bitspersample") != declaredBitDepth)
                throw exception_io_data("Apple Music decoder returned the wrong bit depth");
            const double actualSeconds = actual.get_length();
            if (!std::isfinite(actualSeconds) ||
                (actualSeconds > 0 && actualSeconds + kFullSongDurationToleranceSeconds <
                                          static_cast<double>(m_durationMs) / 1000.0))
                throw exception_io_data("Apple Music decoder did not receive the complete song");
            m_positionSeconds = 0;
            m_report = true;
            playback_log::Ready(m_id, m_positionSeconds);
        } catch (const exception_aborted&) {
            cleanup_failed_initialize();
            throw;
        } catch (const exception_io_data& error) {
            log_failure("decode_initialize", "io_data", error.what());
            InvalidateCache();
            cleanup_failed_initialize();
            throw;
        } catch (const std::exception& error) {
            log_failure("decode_initialize", "std_exception", error.what());
            InvalidateCache();
            cleanup_failed_initialize();
            throw;
        } catch (...) {
            playback_log::UnknownFailure(m_id, "decode_initialize", m_positionSeconds);
            InvalidateCache();
            cleanup_failed_initialize();
            throw;
        }
    }

    bool decode_run(audio_chunk& chunk, abort_callback& abort) {
        if (!m_decoder.is_valid() && !m_atmos) return false;
        try {
            bool decoded = false;
            if (m_atmos) {
                constexpr size_t framesPerChunk = 1024;
                m_atmosBuffer.resize(framesPerChunk * m_atmosInfo.channels);
                const size_t frames = m_atmos->Read(m_atmosBuffer.data(), framesPerChunk, abort);
                if (frames) {
                    chunk.set_data(m_atmosBuffer.data(), frames, m_atmosInfo.channels, m_atmosInfo.sampleRate,
                                   m_atmosInfo.channelMask);
                    decoded = true;
                }
            } else {
                decoded = m_decoder->run(chunk, abort);
            }
            if (decoded) {
                if (chunk.get_sample_rate())
                    m_positionSeconds +=
                        static_cast<double>(chunk.get_sample_count()) / chunk.get_sample_rate();
            } else if (m_durationMs > 0 && m_positionSeconds + kFullSongDurationToleranceSeconds <
                                               static_cast<double>(m_durationMs) / 1000.0) {
                throw exception_io_data("The Apple Music stream ended before the complete song arrived");
            } else {
                playback_log::Complete(m_id, m_positionSeconds);
            }
            return decoded;
        } catch (const exception_aborted&) {
            throw;
        } catch (const exception_io_data& error) {
            playback_log::Failure(m_id, "decode_run", m_positionSeconds, "io_data", error.what());
            InvalidateCache();
            throw;
        } catch (const std::exception& error) {
            playback_log::Failure(m_id, "decode_run", m_positionSeconds, "std_exception", error.what());
            InvalidateCache();
            throw;
        } catch (...) {
            playback_log::UnknownFailure(m_id, "decode_run", m_positionSeconds);
            InvalidateCache();
            throw;
        }
    }
    void decode_seek(double seconds, abort_callback& abort) {
        if (!std::isfinite(seconds) || seconds < 0 ||
            (m_durationMs > 0 && seconds > static_cast<double>(m_durationMs) / 1000.0))
            throw exception_io_seek_out_of_range();
        try {
            if (m_atmos) {
                SeekAtmos(seconds, abort);
                return;
            }
            if (m_flight && !m_source->can_seek()) {
                // A completed cache can be reopened without inventing a remote length.
                // Seeking ahead may wait for transfer; starting playback never does.
                const auto completed = m_flight->Completed(abort);
                service_ptr_t<input_decoder> decoder;
                input_entry::g_open_for_decoding(decoder, completed, m_decodePath.c_str(), abort, true);
                decoder->initialize(0, m_flags, abort);
                file_info_impl actual;
                decoder->get_info(0, actual, abort);
                const double actualSeconds = actual.get_length();
                if (!std::isfinite(actualSeconds) || actualSeconds <= 0 ||
                    actualSeconds + kFullSongDurationToleranceSeconds <
                        static_cast<double>(m_durationMs) / 1000.0)
                    throw exception_io_data("The completed Apple Music source is shorter than the song");
                if (seconds > actualSeconds) throw exception_io_seek_out_of_range();
                decoder->seek(seconds, abort);
                m_decoder = std::move(decoder);
                m_source = std::move(completed);
                m_report = true;
            } else {
                m_decoder->seek(seconds, abort);
            }
            m_positionSeconds = seconds;
        } catch (const exception_aborted&) {
            throw;
        } catch (const exception_io_seek_out_of_range&) {
            throw;
        } catch (...) {
            playback_log::UnknownFailure(m_id, "decode_seek", seconds);
            InvalidateCache();
            throw;
        }
    }
    bool decode_can_seek() {
        return m_atmos ? m_flight || (m_source.is_valid() && m_source->can_seek())
                       : m_decoder.is_valid() && (m_flight || m_decoder->can_seek());
    }
    bool decode_get_dynamic_info(file_info& out, double& delta) {
        if (!m_decoder.is_valid() && !m_atmos) return false;
        try {
            if (m_report) {
                m_report = false;
                abort_callback_dummy abort;
                // The encoded ALAC/AAC container may have no tags. Preserve the
                // provider's metadata when publishing the decoder's technical info.
                get_info(out, abort);
                delta = 0;
                return true;
            }
            const bool changed = m_decoder.is_valid() && m_decoder->get_dynamic_info(out, delta);
            if (changed) ApplyMetadata(out);
            return changed;
        } catch (const exception_aborted&) {
            throw;
        } catch (...) {
            InvalidateCache();
            throw;
        }
    }
    bool decode_get_dynamic_info_track(file_info& out, double& delta) {
        try {
            const bool changed = m_decoder.is_valid() && m_decoder->get_dynamic_info_track(out, delta);
            if (changed) ApplyMetadata(out);
            return changed;
        } catch (const exception_aborted&) {
            throw;
        } catch (...) {
            InvalidateCache();
            throw;
        }
    }
    void decode_on_idle(abort_callback& abort) {
        try {
            if (m_decoder.is_valid()) m_decoder->on_idle(abort);
        } catch (const exception_aborted&) {
            throw;
        } catch (...) {
            InvalidateCache();
            throw;
        }
    }
    void retag(const file_info&, abort_callback&) { throw exception_tagging_unsupported(); }
    void remove_tags(abort_callback&) { throw exception_tagging_unsupported(); }
    static bool g_is_our_content_type(const char*) { return false; }
    static bool g_is_our_path(const char* path, const char*) {
        stream::Id id;
        return ParseSongPath(path, id);
    }
    static const char* g_get_name() { return "Glasswave Apple Music input"; }
    static GUID g_get_guid() {
        return {0x35f5a924, 0x7456, 0x47fa, {0x97, 0xf7, 0xc7, 0x5c, 0xb2, 0x58, 0x16, 0x28}};
    }

  private:
    void SeekAtmos(double seconds, abort_callback& abort) {
        if (!std::isfinite(seconds) || seconds < 0 ||
            seconds > static_cast<double>(m_durationMs) / 1000.0 + kFullSongDurationToleranceSeconds)
            throw exception_io_seek_out_of_range();
        file::ptr completed;
        if (m_flight) {
            completed = m_flight->Completed(abort);
        } else {
            m_source->reopen(abort);
            completed = m_source;
        }
        auto replacement = std::make_unique<atmos::Renderer>(completed, m_atmosTarget);
        const auto info = replacement->WaitForInfo(abort);
        if (!info.hasJocObjects || info.sampleRate != m_atmosInfo.sampleRate ||
            info.channels != m_atmosInfo.channels || info.channelMask != m_atmosInfo.channelMask)
            throw exception_io_data("The Atmos renderer changed format during seeking");
        uint64_t remaining = static_cast<uint64_t>(std::llround(seconds * info.sampleRate));
        std::vector<audio_sample> scratch(1024 * info.channels);
        while (remaining) {
            abort.check();
            const size_t wanted = static_cast<size_t>(std::min<uint64_t>(remaining, 1024));
            const size_t received = replacement->Read(scratch.data(), wanted, abort);
            if (!received) throw exception_io_seek_out_of_range();
            remaining -= received;
        }
        m_atmos = std::move(replacement);
        m_atmosInfo = info;
        m_positionSeconds = seconds;
        m_report = true;
    }

    void ApplyMetadata(file_info& out) const {
        if (const auto song = Catalog::Get().FindSong(m_id)) {
            if (!song->track.title.empty()) out.meta_set("title", Utf8(song->track.title).c_str());
            const auto artists = stream::JoinArtists(song->track.artists);
            if (!artists.empty()) out.meta_set("artist", Utf8(artists).c_str());
            if (!song->track.album.empty()) out.meta_set("album", Utf8(song->track.album).c_str());
            if (!m_decoder.is_valid() && song->track.durationMs > 0)
                out.set_length(static_cast<double>(song->track.durationMs) / 1000.0);
        } else {
            // Read in the background and published to the metadata of every view showing it;
            // meanwhile the song's last known tags (stream/track_snapshot.h), and a stand-in
            // only for a song never seen. Either way the record is reported once it is known.
            StreamProvider::Get().RequestUnknownSong(m_id);
            m_reportWhenKnown = true;
            if (!stream::KnownTags(Catalog::Get().Handle(m_id), out) && !out.meta_get("title", 0)) {
                out.meta_set("title", Utf8(L"Apple Music " + m_id).c_str());
                out.info_set(stream::kPlaceholderInfo, "1");
            }
        }
    }

    void ReleaseDecoder() noexcept {
        m_decoder.release();
        m_atmos.reset();
        m_atmosBuffer.clear();
        m_source.release();
        m_flight.reset();
        m_cacheKey = {};
    }

    void InvalidateCache() noexcept {
        if (m_cacheKey.identity.empty()) return;
        cache::audio::Invalidate(m_cacheKey);
        // The remembered source led to bytes the decoder rejected: open a session next time.
        if (!m_sourceRequest.empty()) {
            try {
                cache::audio::ForgetSource("apple", Utf8(m_id), m_sourceRequest);
            } catch (...) {
            }
        }
    }

    stream::Id m_id;
    std::string m_path;
    cache::Key m_cacheKey;
    std::string m_sourceRequest; // what was asked for (stereo or Atmos) when this opened
    std::string m_decodePath;
    int m_durationMs = 0;
    double m_positionSeconds = 0;
    service_ptr_t<input_decoder> m_decoder;
    file::ptr m_source;
    std::shared_ptr<cache::audio::Flight> m_flight;
    std::unique_ptr<atmos::Renderer> m_atmos;
    atmos::RenderInfo m_atmosInfo;
    atmos::OutputTarget m_atmosTarget = atmos::OutputTarget::StereoSpeakers;
    std::vector<audio_sample> m_atmosBuffer;
    unsigned m_flags = 0;
    bool m_report = false;
};

#pragma warning(push)
#pragma warning(disable : 4100 4702)
static input_singletrack_factory_t<AppleInput, input_entry::flag_redirect> g_input;
#pragma warning(pop)

} // namespace glasswave::applemusic
