#include "pch.h"
#include "netease/catalog.h"
#include "netease/handle.h"
#include "netease/http.h"
#include "netease/log.h"
#include "netease/profile.h"
#include "netease/provider.h"

#include <SDK/album_art_helpers.h>

#include <filesystem>
#include <fstream>

namespace glasswave::netease {

namespace {

std::wstring CoverUrl(std::wstring url) {
    if (url.empty()) return {};
    url += url.find(L'?') == std::wstring::npos ? L"?param=500y500" : L"&param=500y500";
    return url;
}

std::filesystem::path CacheFile(const std::wstring& url) {
    const std::wstring key = url + std::to_wstring(std::hash<std::wstring>{}(url));
    const auto dir = CoverDir();
    if (dir.empty()) return {};
    return dir / (std::to_wstring(std::hash<std::wstring>{}(key)) + L".img");
}

bool ReadCache(const std::filesystem::path& path, std::vector<BYTE>& bytes) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return !bytes.empty();
}

void WriteCache(const std::filesystem::path& path, const std::string& bytes) {
    if (!EnsureDir(path.parent_path())) return;
    auto tmp = path;
    tmp += L".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return;
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }
    MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
}

}  // namespace

class NcmArtInstance : public album_art_extractor_instance {
public:
    explicit NcmArtInstance(std::string path) : m_path(std::move(path)) {}

    album_art_data_ptr query(const GUID& what, abort_callback& abort) override {
        if (what != album_art_ids::cover_front) throw exception_album_art_not_found();
        bool playlist = false;
        int64_t id = 0;
        if (!ParseCoverPath(m_path.c_str(), playlist, id)) {
            playlist = false;
            if (!ParseSongPath(m_path.c_str(), id)) throw exception_album_art_not_found();
        }

        std::wstring url;
        if (playlist) {
            url = Catalog::Get().PlaylistCover(id);
        } else {
            url = Catalog::Get().SongCover(id);
            if (url.empty()) {
                auto& provider = Provider::Get();
                std::wstring error;
                if (provider.Service().EnsureStarted(abort, &error)) {
                    std::vector<Song> songs;
                    if (provider.Api().SongDetails({id}, abort, songs, error)) {
                        Catalog::Get().Put(songs);
                        if (!songs.empty()) url = songs.front().coverUrl;
                    }
                }
            }
        }
        url = CoverUrl(url);
        if (url.empty()) throw exception_album_art_not_found();

        const auto cache = CacheFile(url);
        std::vector<BYTE> bytes;
        if (!cache.empty() && ReadCache(cache, bytes))
            return album_art_data_impl::g_create(bytes.data(), bytes.size());

        HttpOptions options;
        options.noProxy = false;
        options.timeoutMs = 15000;
        options.maxBodyBytes = 20u * 1024u * 1024u;
        options.headers = {
            {L"User-Agent",
             L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Glasswave/0.1"},
            {L"Referer", L"https://music.163.com/"}};
        const HttpResponse response = SendHttp(url, options, abort);
        if (!response.ok() || response.body.empty()) {
            Log(L"art: cover download failed");
            throw exception_album_art_not_found();
        }
        if (!cache.empty()) WriteCache(cache, response.body);
        return album_art_data_impl::g_create(response.body.data(), response.body.size());
    }

private:
    std::string m_path;
};

class NcmArtExtractor : public album_art_extractor_v2 {
public:
    bool is_our_path(const char* path, const char* extension) override {
        (void)extension;
        bool playlist = false;
        int64_t id = 0;
        if (ParseCoverPath(path, playlist, id)) return true;
        return ParseSongPath(path, id);
    }

    album_art_extractor_instance_ptr open(file_ptr filehint, const char* path, abort_callback& abort) override {
        (void)filehint;
        (void)abort;
        return fb2k::service_new<NcmArtInstance>(std::string(path ? path : ""));
    }

    GUID get_guid() override {
        static const GUID guid = {0x2d61b0e4, 0x7a3c, 0x4d95, {0x8f, 0x12, 0x6b, 0x5e, 0x0c, 0x74, 0x9a, 0x21}};
        return guid;
    }
};

static service_factory_single_t<NcmArtExtractor> g_ncm_art;

}  // namespace glasswave::netease
