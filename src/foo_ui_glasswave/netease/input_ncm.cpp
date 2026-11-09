#include "pch.h"
#include "netease/catalog.h"
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

// The decoder's view of a song's CDN stream. NetEase signs each URL for a limited time
// (expireSeconds, about 20 minutes): after a long pause a seek or a dropped connection
// would fail with 403. The stream then asks for a fresh URL of the same quality and
// continues at the same byte.
class NcmHttpFile : public file {
public:
    using Resolve = std::function<file::ptr(abort_callback&)>;

    NcmHttpFile(file::ptr inner, std::string contentType, Resolve resolve, int expireSeconds)
        : m_inner(std::move(inner)),
          m_contentType(std::move(contentType)),
          m_resolve(std::move(resolve)),
          m_lifetimeMs(static_cast<uint64_t>(std::max(expireSeconds, 120)) * 1000),
          m_since(GetTickCount64()) {}

    t_size read(void* buffer, t_size bytes, abort_callback& abort) override {
        t_size done = 0;
        try {
            done = m_inner->read(buffer, bytes, abort);
        } catch (const exception_aborted&) {
            throw;
        } catch (const std::exception&) {
            if (!Renew(abort)) throw;
            done = m_inner->read(buffer, bytes, abort);
        }
        m_position += done;
        return done;
    }
    void write(const void* buffer, t_size bytes, abort_callback& abort) override {
        m_inner->write(buffer, bytes, abort);
    }
    t_filesize get_size(abort_callback& abort) override { return m_inner->get_size(abort); }
    t_filesize get_position(abort_callback&) override { return m_position; }
    void resize(t_filesize size, abort_callback& abort) override { m_inner->resize(size, abort); }
    void seek(t_filesize position, abort_callback& abort) override {
        m_position = position;
        if (Expired()) Renew(abort);  // a seek opens a new range request with the URL
        try {
            m_inner->seek(position, abort);
        } catch (const exception_aborted&) {
            throw;
        } catch (const std::exception&) {
            if (!Renew(abort)) throw;
        }
    }
    bool can_seek() override { return m_inner->can_seek(); }
    bool get_content_type(pfc::string_base& out) override {
        out = m_contentType.c_str();
        return true;
    }
    void reopen(abort_callback& abort) override {
        m_position = 0;
        if (Expired()) Renew(abort);
        else m_inner->reopen(abort);
    }
    bool is_remote() override { return m_inner->is_remote(); }
    void on_idle(abort_callback& abort) override { m_inner->on_idle(abort); }
    t_filetimestamp get_timestamp(abort_callback& abort) override { return m_inner->get_timestamp(abort); }

private:
    bool Expired() const { return GetTickCount64() - m_since + 60'000 > m_lifetimeMs; }

    // A fresh URL, positioned where the decoder is. False when none can be had.
    bool Renew(abort_callback& abort) {
        file::ptr fresh;
        try {
            fresh = m_resolve(abort);
            if (m_position) fresh->seek(m_position, abort);
        } catch (const exception_aborted&) {
            throw;
        } catch (const std::exception&) {
            return false;
        }
        m_inner = std::move(fresh);
        m_since = GetTickCount64();
        Log(L"input: renewed the stream URL at byte " + std::to_wstring(m_position));
        return true;
    }

    file::ptr m_inner;
    std::string m_contentType;
    Resolve m_resolve;
    uint64_t m_lifetimeMs;
    uint64_t m_since;
    t_filesize m_position = 0;
};

// Opens the song's CDN stream with the headers the CDN expects.
file::ptr OpenStream(const std::wstring& url, abort_callback& abort) {
    auto request = http_client::get()->create_request("GET");
    request->add_header("User-Agent",
                        "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) "
                        "Glasswave/0.1");
    request->add_header("Referer", "https://music.163.com/");
    return request->run(Utf8(url).c_str(), abort);
}

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
    }

    void get_info(file_info& p_info, abort_callback& p_abort) {
        (void)p_abort;
        const auto song = Catalog::Get().FindSong(m_id);
        if (!song) {
            p_info.meta_set("title", ("NetEase song " + std::to_string(m_id)).c_str());
            return;
        }
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
        Log(L"input: decoding song " + std::to_wstring(m_id));
        auto& provider = Session::Get();  // started by the component's initquit, on the main thread
        if (!provider.LoggedIn())
            throw exception_io_data("Please sign in to NetEase Cloud Music before playing this track.");
        std::wstring error;
        if (!provider.Service().EnsureStarted(p_abort, &error))
            throw exception_io_data(Utf8(error.empty() ? L"NetEase service is unavailable." : error).c_str());

        SongUrl resolved;
        bool ok = false;
        for (const auto& level : LevelsFor(GetQuality())) {
            if (provider.Api().GetSongUrl(m_id, level, p_abort, resolved, error)) {
                ok = true;
                break;
            }
        }
        if (!ok)
            throw exception_io_data(Utf8(error.empty() ? L"No playable source for this song." : error).c_str());
        if (resolved.trial)
            Log(L"input: playing a free trial for song " + std::to_wstring(m_id));
        const std::string extension = ExtensionFor(resolved);
        Log(L"input: song " + std::to_wstring(m_id) + L", level " + resolved.level + L", type " + resolved.type +
            L", ext " + Wide(extension.c_str()));
        file::ptr inner = OpenStream(resolved.url, p_abort);
        const std::string contentType =
            extension == "flac" ? "audio/flac" : extension == "m4a" ? "audio/mp4" : "audio/mpeg";
        // The same quality level again: the stream's format must not change mid-song.
        auto resolve = [id = m_id, level = resolved.level](abort_callback& abort) {
            SongUrl fresh;
            std::wstring urlError;
            if (!Session::Get().Api().GetSongUrl(id, level, abort, fresh, urlError) || fresh.url.empty())
                throw exception_io_data(Utf8(urlError.empty() ? L"Cannot renew the stream URL." : urlError).c_str());
            return OpenStream(fresh.url, abort);
        };
        file::ptr typed;
        typed ^= fb2k::service_new<NcmHttpFile>(inner, contentType, std::move(resolve), resolved.expireSeconds);
        const std::string fakePath = "ncm://decode/" + std::to_string(m_id) + "." + extension;
        input_entry::g_open_for_decoding(m_decoder, typed, fakePath.c_str(), p_abort, true);
        m_decoder->initialize(0, p_flags, p_abort);
        Log(L"input: opened song " + std::to_wstring(m_id) + L" as stream " + Wide(contentType.c_str()));
    }

    bool decode_run(audio_chunk& p_chunk, abort_callback& p_abort) {
        return m_decoder.is_valid() && m_decoder->run(p_chunk, p_abort);
    }

    void decode_seek(double p_seconds, abort_callback& p_abort) {
        if (m_decoder.is_valid()) m_decoder->seek(p_seconds, p_abort);
    }

    bool decode_can_seek() { return m_decoder.is_valid() && m_decoder->can_seek(); }

    bool decode_get_dynamic_info(file_info& p_out, double& p_timestamp_delta) {
        return m_decoder.is_valid() && m_decoder->get_dynamic_info(p_out, p_timestamp_delta);
    }

    bool decode_get_dynamic_info_track(file_info& p_out, double& p_timestamp_delta) {
        return m_decoder.is_valid() && m_decoder->get_dynamic_info_track(p_out, p_timestamp_delta);
    }

    void decode_on_idle(abort_callback& p_abort) {
        if (m_decoder.is_valid()) m_decoder->on_idle(p_abort);
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
        static const GUID guid = {0x7c9f2a51, 0x3b4d, 0x4e88, {0x9a, 0x61, 0x2f, 0x7c, 0x5d, 0x0b, 0x8e, 0x34}};
        return guid;
    }

private:
    int64_t m_id = 0;
    service_ptr_t<input_decoder> m_decoder;
};

#pragma warning(push)
#pragma warning(disable : 4100 4702)  // SDK input templates forward unused parameters
static input_singletrack_factory_t<NcmInput, input_entry::flag_redirect> g_ncm_input;
#pragma warning(pop)

}  // namespace glasswave::netease
