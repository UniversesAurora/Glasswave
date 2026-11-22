#pragma once

#include "cache/audio.h"
#include "net/resumable_fetch.h"
#include "stream/atmos_renderer.h"

#include <SDK/input_impl.h>

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The foobar2000 input of a streaming source whose songs are encoded files that the core's own
// decoders play (ALAC/AAC/MP3/FLAC): everything but what the source knows. A source supplies how
// to resolve a song to the bytes it delivers (URL or session, headers, codec, length) and its
// tags; the base names the cache entry, opens or joins the shared transfer, remembers what the
// service said, opens the decoder over the cache's stream, checks what it got against what was
// promised and reopens the finished cache file to seek.
namespace glasswave::stream {

// A song's length is the delivered audio's within this much; a decoder that finds less received
// an incomplete song.
inline constexpr double kFullSongToleranceSeconds = 2.0;

// What a source resolved a song to: the bytes it delivers and what is checked about them.
struct EncodedSource {
    // Names the cache entry (never a URL or a credential).
    cache::audio::SourceName name;
    // The path the core's decoder is opened on; only its extension selects the decoder.
    std::string decodePath;
    // The song's length in seconds, 0 when unknown (the decoder's is taken then).
    double durationSeconds = 0;
    // False for a clip shorter than the song (a free trial): its length is not checked.
    bool complete = true;
    // E-AC-3 with JOC objects: rendered by stream::atmos::Renderer instead of a core decoder.
    bool dolbyAtmos = false;
    // What the decoder (or renderer) must report; 0 checks nothing.
    int sampleRate = 0;
    int bitDepth = 0;
    // What cache::audio::RememberSource keeps to name this entry again without asking the
    // service; empty remembers nothing (a trial clip, a test fixture).
    std::vector<std::string> memo;
    // Starts the download. Empty: the bytes are expected to be in the cache already.
    cache::audio::Fetcher fetch;
};

// "codec" of a decoder's info, else its long codec name; empty when it says none.
std::string ActualCodec(file_info& info);
// ASCII case-insensitive substring test, for matching a decoder's codec name.
bool ContainsNoCase(std::string_view text, std::string_view part);
// A cache transfer that reads `source` with net::FetchResumable.
cache::audio::Fetcher ResumableFetcher(net::ResumableSource source);

class EncodedInputBase : public input_stubs {
  public:
    EncodedInputBase() = default;
    EncodedInputBase(const EncodedInputBase&) = delete;
    EncodedInputBase& operator=(const EncodedInputBase&) = delete;
    ~EncodedInputBase() { ReleaseDecoder(); }

    void decode_initialize(unsigned flags, abort_callback& abort);
    bool decode_run(audio_chunk& chunk, abort_callback& abort);
    void decode_seek(double seconds, abort_callback& abort);
    bool decode_can_seek();
    bool decode_get_dynamic_info(file_info& out, double& delta);
    bool decode_get_dynamic_info_track(file_info& out, double& delta);
    void decode_on_idle(abort_callback& abort);
    void retag(const file_info&, abort_callback&) { throw exception_tagging_unsupported(); }
    void remove_tags(abort_callback&) { throw exception_tagging_unsupported(); }
    static bool g_is_our_content_type(const char*) { return false; }

  protected:
    // ---- what a source supplies ----------------------------------------------------------
    // Names the cache entries and remembered sources ("apple", "ncm").
    virtual const char* ProviderKey() const = 0;
    // Names the source in messages ("Apple Music").
    virtual std::string_view Label() const = 0;
    // The song as the remembered source is keyed.
    virtual std::string SongIdentity() const = 0;
    // True when the source's record of the song is known (what the tags are written from).
    virtual bool SongKnown() const = 0;
    // The core asks for the playing info again: call get_info.
    virtual void PublishInfo(file_info& out, abort_callback& abort) = 0;

    // A song that needs no source (a downloaded file): opens the decoder itself and returns
    // true, or returns true with no decoder when there is nothing to do (a prefetch).
    virtual bool OpenOffline(unsigned flags, abort_callback& abort);
    // Reads the settings this opening depends on and returns what was asked for (the quality
    // tier): a remembered source is only reused for the same request.
    virtual std::string BeginOpen(abort_callback& abort) = 0;
    // The source a remembered memo names, without a fetcher; nullopt when it cannot be used.
    virtual std::optional<EncodedSource> Recall(const std::vector<std::string>& memo,
                                                abort_callback& abort) = 0;
    // Asks the service. nullopt: the song is skipped as unavailable (the source has said so);
    // there is nothing to decode and the core goes on to the next one.
    virtual std::optional<EncodedSource> Resolve(abort_callback& abort) = 0;
    // Whether a decoder that reports `actual` plays what the source declared (EncodedSource::
    // name.codec). Not called for Atmos.
    virtual bool CodecMatches(std::string_view declared, std::string_view actual) const = 0;

    virtual void OnDelivered(const EncodedSource&) {}
    virtual void OnReady() {}
    virtual void OnComplete() {}
    // Tags of the decoder's own dynamic info are replaced by the source's.
    virtual void ApplyDynamicMetadata(file_info&) {}
    virtual void ReportFailure(std::string_view /*stage*/, std::string_view /*errorClass*/,
                               std::string_view /*what*/) noexcept {}
    virtual stream::atmos::OutputTarget AtmosTarget() const {
        return stream::atmos::OutputTarget::StereoSpeakers;
    }

    // ---- for the source ------------------------------------------------------------------
    bool HasOutput() const noexcept { return m_decoder.is_valid() || m_atmos; }
    bool Atmos() const noexcept { return m_atmos != nullptr; }
    // The decoder's (or renderer's) info with the delivered length.
    void FillDecoderInfo(file_info& out, abort_callback& abort);
    void DescribeAtmos(file_info& out) const;
    void MarkReady();
    // Releases the decoder, the source and the transfer; forgets the song's length.
    void ReleaseDecoder() noexcept;
    // The bytes were rejected by a decoder: they are never offered again and the remembered
    // source that led to them is forgotten.
    void InvalidateCache() noexcept;

    // Runs `fn`; a failure other than an abort invalidates the cache entry and is reported.
    template <class Fn> decltype(auto) Guarded(std::string_view stage, Fn&& fn) {
        try {
            return fn();
        } catch (const exception_aborted&) {
            throw;
        } catch (const exception_io_seek_out_of_range&) {
            throw;
        } catch (const exception_io_data& error) {
            ReportFailure(stage, "io_data", error.what());
            InvalidateCache();
            throw;
        } catch (const std::exception& error) {
            ReportFailure(stage, "std_exception", error.what());
            InvalidateCache();
            throw;
        } catch (...) {
            ReportFailure(stage, "non_std_exception", "unknown exception");
            InvalidateCache();
            throw;
        }
    }

    std::string m_path;
    std::string m_sourceRequest; // what was asked for (a quality tier) when this opened
    std::string m_decodePath;
    cache::Key m_cacheKey;
    service_ptr_t<input_decoder> m_decoder;
    file::ptr m_source;
    std::shared_ptr<cache::audio::Flight> m_flight;
    double m_positionSeconds = 0;
    double m_durationSeconds = 0;
    bool m_complete = true;
    bool m_persistent = true;
    unsigned m_flags = 0;
    bool m_report = false;          // the playing info is published again (the decoder is new)
    bool m_reportWhenKnown = false; // it was published without the song's record

  private:
    void OpenFresh(unsigned flags, abort_callback& abort);
    bool OpenSource(EncodedSource& source, abort_callback& abort);
    void OpenRenderer(const EncodedSource& source, abort_callback& abort);
    void Validate(const EncodedSource& source, file_info& actual);
    void ReopenCompleted(double seconds, abort_callback& abort);
    void SeekAtmos(double seconds, abort_callback& abort);

    struct AtmosFrameEntry {
        uint64_t byteOffset = 0;
        uint64_t sampleOffset = 0;
        uint32_t sampleCount = 0;
    };

    std::unique_ptr<stream::atmos::Renderer> m_atmos;
    stream::atmos::RenderInfo m_atmosInfo;
    stream::atmos::OutputTarget m_atmosTarget = stream::atmos::OutputTarget::StereoSpeakers;
    std::vector<audio_sample> m_atmosBuffer;
    std::vector<AtmosFrameEntry> m_atmosFrames;
    bool m_atmosFramesIndexed = false;
};

} // namespace glasswave::stream
