#include "pch.h"
#include "stream/encoded_input.h"

#include "stream/playback_source.h"

#include <cmath>

namespace glasswave::stream {

std::string ActualCodec(file_info& info) {
    if (const char* value = info.info_get("codec"); value && *value) return value;
    pfc::string8 codec;
    if (info.info_get_codec_long(codec)) return codec.get_ptr();
    return {};
}

bool ContainsNoCase(std::string_view text, std::string_view part) {
    if (part.empty()) return false;
    const auto upper = [](char c) { return c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c; };
    if (part.size() > text.size()) return false;
    for (size_t start = 0; start + part.size() <= text.size(); ++start) {
        size_t i = 0;
        while (i < part.size() && upper(text[start + i]) == upper(part[i]))
            ++i;
        if (i == part.size()) return true;
    }
    return false;
}

cache::audio::Fetcher ResumableFetcher(net::ResumableSource source) {
    return [source = std::move(source)](cache::audio::Sink& sink, abort_callback& abort) {
        net::FetchResumable(source, net::WriteTo(sink), abort);
    };
}

namespace {
std::string Message(std::string_view label, std::string_view text) {
    return "The " + std::string(label) + " " + std::string(text);
}
} // namespace

// ---- opening ----------------------------------------------------------------------------

bool EncodedInputBase::OpenOffline(unsigned, abort_callback&) {
    return false;
}

void EncodedInputBase::decode_initialize(unsigned flags, abort_callback& abort) {
    m_flags = flags;
    AnnouncePlaybackOpening(m_path, flags);
    if (HasOutput()) {
        // Already open: the same song starts over.
        Guarded("decode_initialize", [&] {
            if (m_atmos) {
                SeekAtmos(0, abort);
            } else {
                m_decoder->initialize(0, flags, abort);
                m_positionSeconds = 0;
            }
            MarkReady();
        });
        return;
    }
    try {
        Guarded("decode_initialize", [&] { OpenFresh(flags, abort); });
    } catch (...) {
        ReleaseDecoder();
        throw;
    }
}

void EncodedInputBase::OpenFresh(unsigned flags, abort_callback& abort) {
    if (OpenOffline(flags, abort)) return;
    m_sourceRequest = BeginOpen(abort);

    // A song that is already in the cache starts from it at once: the service is asked which
    // source it would deliver only when there is nothing to play yet.
    std::optional<EncodedSource> source;
    if (const auto memo = cache::audio::RecallSource(ProviderKey(), SongIdentity(), m_sourceRequest)) {
        if (auto known = Recall(*memo, abort); known && OpenSource(*known, abort)) source = std::move(known);
    }
    if (!source) {
        source = Resolve(abort);
        if (!source) return;
        if (!OpenSource(*source, abort)) throw exception_io_data("Audio prefetch capacity is full");
        if (m_persistent && !source->memo.empty())
            cache::audio::RememberSource(ProviderKey(), SongIdentity(), m_sourceRequest, source->memo);
    }

    m_decodePath = source->decodePath;
    m_durationSeconds = source->durationSeconds;
    m_complete = source->complete;
    OnDelivered(*source);
    if (source->dolbyAtmos) {
        OpenRenderer(*source, abort);
        m_positionSeconds = 0;
        MarkReady();
        return;
    }
    input_entry::g_open_for_decoding(m_decoder, m_source, m_decodePath.c_str(), abort, true);
    m_decoder->initialize(0, flags, abort);
    file_info_impl actual;
    m_decoder->get_info(0, actual, abort);
    Validate(*source, actual);
    m_positionSeconds = 0;
    MarkReady();
}

// Names the cache entry of one source and opens it: from the cache, by joining its running
// transfer, or (with a fetcher) by starting the download.
bool EncodedInputBase::OpenSource(EncodedSource& source, abort_callback& abort) {
    source.name.provider = ProviderKey();
    cache::audio::NamedSource named = cache::audio::NameSource(source.name);
    cache::audio::Request transfer;
    transfer.key = named.key;
    transfer.metadata = std::move(named.metadata);
    transfer.cachedOnly = !source.fetch;
    transfer.fetch = std::move(source.fetch);
    transfer.persistent = named.persistent;
    auto opened = cache::audio::OpenOrStart(std::move(transfer), abort);
    if (!opened.admitted || !opened.stream.is_valid()) return false;
    m_cacheKey = named.key;
    m_persistent = named.persistent;
    m_source = std::move(opened.stream);
    m_flight = std::move(opened.flight);
    return true;
}

void EncodedInputBase::OpenRenderer(const EncodedSource& source, abort_callback& abort) {
    m_atmosTarget = AtmosTarget();
    m_atmos = std::make_unique<stream::atmos::Renderer>(m_source, m_atmosTarget);
    m_atmosInfo = m_atmos->WaitForInfo(abort);
    if (!m_atmosInfo.hasJocObjects) throw exception_io_data("The Atmos source has no decoded JOC objects");
    if (source.sampleRate > 0 && m_atmosInfo.sampleRate != static_cast<uint32_t>(source.sampleRate))
        throw exception_io_data("The Atmos source returned the wrong sample rate");
}

// What the decoder reports must be what the source declared, and the whole song.
void EncodedInputBase::Validate(const EncodedSource& source, file_info& actual) {
    if (!CodecMatches(source.name.codec, ActualCodec(actual)))
        throw exception_io_data(Message(Label(), "decoder returned the wrong codec").c_str());
    if (source.sampleRate > 0 && actual.info_get_int("samplerate") != source.sampleRate)
        throw exception_io_data(Message(Label(), "decoder returned the wrong sample rate").c_str());
    if (source.bitDepth > 0 && actual.info_get_int("bitspersample") != source.bitDepth)
        throw exception_io_data(Message(Label(), "decoder returned the wrong bit depth").c_str());
    const double actualSeconds = actual.get_length();
    if (!std::isfinite(actualSeconds) || (m_complete && actualSeconds > 0 && m_durationSeconds > 0 &&
                                          actualSeconds + kFullSongToleranceSeconds < m_durationSeconds))
        throw exception_io_data(Message(Label(), "decoder did not receive the complete song").c_str());
    if (m_durationSeconds <= 0 && actualSeconds > 0) m_durationSeconds = actualSeconds;
}

void EncodedInputBase::MarkReady() {
    m_report = true;
    OnReady();
}

// ---- decoding ---------------------------------------------------------------------------

bool EncodedInputBase::decode_run(audio_chunk& chunk, abort_callback& abort) {
    if (!HasOutput()) return false;
    return Guarded("decode_run", [&] {
        bool decoded = false;
        if (m_atmos) {
            m_atmosBuffer.resize(atmos::kFramesPerRead * m_atmosInfo.channels);
            const size_t frames = m_atmos->Read(m_atmosBuffer.data(), atmos::kFramesPerRead, abort);
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
                m_positionSeconds += static_cast<double>(chunk.get_sample_count()) / chunk.get_sample_rate();
        } else if (m_complete && m_durationSeconds > 0 &&
                   m_positionSeconds + kFullSongToleranceSeconds < m_durationSeconds) {
            throw exception_io_data(
                Message(Label(), "stream ended before the complete song arrived").c_str());
        } else {
            OnComplete();
        }
        return decoded;
    });
}

void EncodedInputBase::decode_seek(double seconds, abort_callback& abort) {
    // A skipped song has nothing to seek in.
    if (!HasOutput()) throw exception_io_seek_out_of_range();
    if (!std::isfinite(seconds) || seconds < 0 || (m_durationSeconds > 0 && seconds > m_durationSeconds))
        throw exception_io_seek_out_of_range();
    Guarded("decode_seek", [&] {
        if (m_atmos) {
            SeekAtmos(seconds, abort);
            return;
        }
        if (m_flight && !m_source->can_seek())
            ReopenCompleted(seconds, abort);
        else
            m_decoder->seek(seconds, abort);
        m_positionSeconds = seconds;
    });
}

// A completed cache can be reopened without inventing a remote length. Seeking ahead may wait
// for the transfer; starting playback never does.
void EncodedInputBase::ReopenCompleted(double seconds, abort_callback& abort) {
    file::ptr completed = m_flight->Completed(abort);
    service_ptr_t<input_decoder> decoder;
    input_entry::g_open_for_decoding(decoder, completed, m_decodePath.c_str(), abort, true);
    decoder->initialize(0, m_flags, abort);
    file_info_impl actual;
    decoder->get_info(0, actual, abort);
    const double actualSeconds = actual.get_length();
    if (!std::isfinite(actualSeconds) || actualSeconds <= 0 ||
        (m_complete && actualSeconds + kFullSongToleranceSeconds < m_durationSeconds))
        throw exception_io_data(Message(Label(), "completed source is shorter than the song").c_str());
    if (seconds > actualSeconds) throw exception_io_seek_out_of_range();
    decoder->seek(seconds, abort);
    m_decoder = std::move(decoder);
    m_source = std::move(completed);
    m_report = true;
}

// The renderer cannot seek: a new one decodes and discards up to the position, which needs the
// whole song.
void EncodedInputBase::SeekAtmos(double seconds, abort_callback& abort) {
    if (!std::isfinite(seconds) || seconds < 0 || seconds > m_durationSeconds + kFullSongToleranceSeconds)
        throw exception_io_seek_out_of_range();
    file::ptr completed;
    if (m_flight) {
        completed = m_flight->Completed(abort);
    } else {
        m_source->reopen(abort);
        completed = m_source;
    }
    auto replacement = std::make_unique<stream::atmos::Renderer>(completed, m_atmosTarget);
    const auto info = replacement->WaitForInfo(abort);
    if (!info.hasJocObjects || info.sampleRate != m_atmosInfo.sampleRate ||
        info.channels != m_atmosInfo.channels || info.channelMask != m_atmosInfo.channelMask)
        throw exception_io_data("The Atmos renderer changed format during seeking");
    uint64_t remaining = static_cast<uint64_t>(std::llround(seconds * info.sampleRate));
    std::vector<audio_sample> scratch(atmos::kFramesPerRead * info.channels);
    while (remaining) {
        abort.check();
        const size_t wanted = static_cast<size_t>(std::min<uint64_t>(remaining, atmos::kFramesPerRead));
        const size_t received = replacement->Read(scratch.data(), wanted, abort);
        if (!received) throw exception_io_seek_out_of_range();
        remaining -= received;
    }
    m_atmos = std::move(replacement);
    m_atmosInfo = info;
    m_positionSeconds = seconds;
    m_report = true;
}

bool EncodedInputBase::decode_can_seek() {
    if (m_atmos) return m_flight || (m_source.is_valid() && m_source->can_seek());
    return m_decoder.is_valid() && (m_flight || m_decoder->can_seek());
}

bool EncodedInputBase::decode_get_dynamic_info(file_info& out, double& delta) {
    if (!HasOutput()) return false;
    return Guarded("decode_get_dynamic_info", [&] {
        // The song's record arrived after it opened: the playing info takes it now.
        if (m_reportWhenKnown && SongKnown()) {
            m_reportWhenKnown = false;
            m_report = true;
        }
        if (m_report) {
            m_report = false;
            abort_callback_dummy abort;
            // The encoded container may have no tags: the source's are kept when the decoder's
            // technical info is published.
            PublishInfo(out, abort);
            delta = 0;
            return true;
        }
        const bool changed = m_decoder.is_valid() && m_decoder->get_dynamic_info(out, delta);
        if (changed) ApplyDynamicMetadata(out);
        return changed;
    });
}

bool EncodedInputBase::decode_get_dynamic_info_track(file_info& out, double& delta) {
    return Guarded("decode_get_dynamic_info_track", [&] {
        const bool changed = m_decoder.is_valid() && m_decoder->get_dynamic_info_track(out, delta);
        if (changed) ApplyDynamicMetadata(out);
        return changed;
    });
}

void EncodedInputBase::decode_on_idle(abort_callback& abort) {
    Guarded("decode_on_idle", [&] {
        if (m_decoder.is_valid()) m_decoder->on_idle(abort);
    });
}

// ---- info ---------------------------------------------------------------------------------

void EncodedInputBase::FillDecoderInfo(file_info& out, abort_callback& abort) {
    Guarded("get_info", [&] {
        if (m_atmos) {
            DescribeAtmos(out);
        } else if (m_decoder.is_valid()) {
            m_decoder->get_info(0, out, abort);
            // The delivered length is the audio's. A decoder can estimate an MP3 from a misleading
            // first frame (Build 2021 BGM1: 7:51 for 3:55 of audio), and the player's seek bar and
            // seeking follow the length reported here.
            if (m_durationSeconds > 0 &&
                std::abs(out.get_length() - m_durationSeconds) > kFullSongToleranceSeconds)
                out.set_length(m_durationSeconds);
        }
    });
}

void EncodedInputBase::DescribeAtmos(file_info& out) const {
    out.info_set("codec", "E-AC-3 + JOC (Dolby Atmos)");
    out.info_set("encoding", "lossy");
    out.info_set_int("samplerate", m_atmosInfo.sampleRate);
    out.info_set_int("channels", m_atmosInfo.channels);
    out.info_set_int("bitspersample", 32);
    out.info_set("atmos_renderer", "Cavern");
    out.info_set_int("atmos_objects", m_atmosInfo.dynamicObjects);
    if (m_atmosTarget == stream::atmos::OutputTarget::Headphones) out.info_set_int("headroom_db", 6);
    if (m_durationSeconds > 0) out.set_length(m_durationSeconds);
    out.info_set("atmos_output", m_atmosTarget == stream::atmos::OutputTarget::Headphones ? "headphones"
                                 : m_atmosInfo.channels == 6                              ? "5.1 speakers"
                                 : m_atmosInfo.channels == 8 ? "7.1 speakers"
                                                             : "stereo speakers");
}

// ---- release ------------------------------------------------------------------------------

void EncodedInputBase::ReleaseDecoder() noexcept {
    m_decoder.release();
    m_atmos.reset();
    m_atmosBuffer.clear();
    m_source.release();
    m_flight.reset();
    m_cacheKey = {};
    m_positionSeconds = 0;
    m_durationSeconds = 0;
    m_complete = true;
    m_decodePath.clear();
    m_report = false;
}

void EncodedInputBase::InvalidateCache() noexcept {
    if (m_cacheKey.identity.empty()) return;
    cache::audio::Invalidate(m_cacheKey);
    // The remembered source led to bytes the decoder rejected: ask the service next time.
    if (m_sourceRequest.empty()) return;
    try {
        cache::audio::ForgetSource(ProviderKey(), SongIdentity(), m_sourceRequest);
    } catch (...) {
    }
}

} // namespace glasswave::stream
