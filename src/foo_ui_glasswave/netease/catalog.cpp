#include "pch.h"
#include "netease/catalog.h"
#include "netease/handle.h"
#include "netease/profile.h"

#include <SDK/file_info_impl.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace glasswave::netease {

namespace {

std::string Utf8(std::wstring_view s) {
    if (s.empty()) return {};
    return pfc::stringcvt::string_utf8_from_wide(s.data(), s.size()).get_ptr();
}

std::wstring Wide(const std::string& s) {
    if (s.empty()) return {};
    return pfc::stringcvt::string_wide_from_utf8(s.c_str()).get_ptr();
}

std::string Clean(std::wstring_view s) {
    std::wstring copy(s);
    for (auto& c : copy)
        if (c == L'\t' || c == L'\r' || c == L'\n') c = L' ';
    return Utf8(copy);
}

std::filesystem::path CacheFile() {
    const auto dir = CacheDir();
    return dir.empty() ? dir : dir / L"songs.tsv";
}

}  // namespace

Catalog& Catalog::Get() {
    static Catalog catalog;
    return catalog;
}

void Catalog::Put(const Song& song) {
    if (!song.id) return;
    std::lock_guard lock(m_mutex);
    m_songs[song.id] = song;
}

void Catalog::Put(const std::vector<Song>& songs) {
    std::lock_guard lock(m_mutex);
    for (const auto& song : songs)
        if (song.id) m_songs[song.id] = song;
}

void Catalog::Put(const Playlist& playlist) {
    if (!playlist.id) return;
    std::lock_guard lock(m_mutex);
    m_playlists[playlist.id] = playlist;
}

void Catalog::Put(const Artist& artist) {
    if (!artist.id) return;
    std::lock_guard lock(m_mutex);
    m_artists[artist.id] = artist;
}

void Catalog::Put(const Album& album) {
    if (!album.id) return;
    std::lock_guard lock(m_mutex);
    m_albums[album.id] = album;
}

void Catalog::Put(const std::vector<Album>& albums) {
    std::lock_guard lock(m_mutex);
    for (const auto& album : albums)
        if (album.id) m_albums[album.id] = album;
}

std::optional<Song> Catalog::FindSong(int64_t id) const {
    std::lock_guard lock(m_mutex);
    const auto it = m_songs.find(id);
    if (it == m_songs.end()) return std::nullopt;
    return it->second;
}

std::optional<Playlist> Catalog::FindPlaylist(int64_t id) const {
    std::lock_guard lock(m_mutex);
    const auto it = m_playlists.find(id);
    if (it == m_playlists.end()) return std::nullopt;
    return it->second;
}

std::optional<Artist> Catalog::FindArtist(int64_t id) const {
    std::lock_guard lock(m_mutex);
    const auto it = m_artists.find(id);
    if (it == m_artists.end()) return std::nullopt;
    return it->second;
}

std::optional<Album> Catalog::FindAlbum(int64_t id) const {
    std::lock_guard lock(m_mutex);
    const auto it = m_albums.find(id);
    if (it == m_albums.end()) return std::nullopt;
    return it->second;
}

metadb_handle_ptr Catalog::Handle(int64_t songId) const {
    return metadb::get()->handle_create(SongPath(songId).c_str(), 0);
}

metadb_handle_ptr Catalog::PlaylistHandle(int64_t playlistId) const {
    return metadb::get()->handle_create(CoverPath(CoverKind::Playlist, playlistId).c_str(), 0);
}

metadb_handle_ptr Catalog::ArtistHandle(int64_t artistId) const {
    return metadb::get()->handle_create(CoverPath(CoverKind::Artist, artistId).c_str(), 0);
}

metadb_handle_ptr Catalog::AlbumHandle(int64_t albumId) const {
    return metadb::get()->handle_create(CoverPath(CoverKind::Album, albumId).c_str(), 0);
}

std::wstring Catalog::SongCover(int64_t songId) const {
    if (const auto song = FindSong(songId)) return song->coverUrl;
    return {};
}

std::wstring Catalog::PlaylistCover(int64_t playlistId) const {
    if (const auto playlist = FindPlaylist(playlistId)) return playlist->coverUrl;
    return {};
}

std::wstring Catalog::ArtistCover(int64_t artistId) const {
    if (const auto artist = FindArtist(artistId)) return artist->avatarUrl;
    return {};
}

std::wstring Catalog::AlbumCover(int64_t albumId) const {
    if (const auto album = FindAlbum(albumId)) return album->coverUrl;
    return {};
}

void Catalog::PutAndHint(const std::vector<Song>& songs) {
    Put(songs);
    Hint(songs);
    Save();
}

void Catalog::Hint(const std::vector<Song>& songs) {
    if (songs.empty()) return;
    auto hints = metadb_io_v2::get()->create_hint_list();
    if (!hints.is_valid()) return;
    for (const auto& song : songs) {
        if (!song.id) continue;
        file_info_impl info;
        auto setMeta = [&info](const char* name, const std::string& value) {
            if (!value.empty()) info.meta_set_ex(name, SIZE_MAX, value.c_str(), SIZE_MAX);
        };
        setMeta("title", Utf8(song.title));
        const std::wstring artists = JoinArtists(song.artists);
        setMeta("artist", Utf8(artists));
        setMeta("album artist", Utf8(song.albumArtist));
        setMeta("album", Utf8(song.album));
        if (song.durationMs > 0) info.set_length(static_cast<double>(song.durationMs) / 1000.0);
        if (!song.codec.empty()) info.info_set("codec", Utf8(song.codec).c_str());
        if (song.bitrate > 0) info.info_set_int("bitrate", song.bitrate);
        if (song.sampleRate > 0) info.info_set_int("samplerate", song.sampleRate);
        if (song.bitsPerSample > 0) info.info_set_int("bitspersample", song.bitsPerSample);
        info.info_set_int("channels", 2);
        if (song.codec == L"FLAC") info.info_set("encoding", "lossless");
        t_filestats stats;
        hints->add_hint(Handle(song.id), info, stats, true);
    }
    hints->on_done();
}

void Catalog::Load() {
    const auto path = CacheFile();
    if (path.empty()) return;
    std::ifstream in(path, std::ios::binary);
    if (!in) return;

    std::vector<Song> loaded;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        std::vector<std::string> fields;
        size_t start = 0;
        while (start <= line.size()) {
            const size_t end = line.find('\t', start);
            fields.push_back(line.substr(start, end == std::string::npos ? std::string::npos : end - start));
            if (end == std::string::npos) break;
            start = end + 1;
        }
        if (fields.size() < 12) continue;
        Song song;
        song.id = _strtoi64(fields[0].c_str(), nullptr, 10);
        if (!song.id) continue;
        song.durationMs = static_cast<int>(_strtoi64(fields[1].c_str(), nullptr, 10));
        song.fee = static_cast<int>(_strtoi64(fields[2].c_str(), nullptr, 10));
        song.bitrate = static_cast<int>(_strtoi64(fields[3].c_str(), nullptr, 10));
        song.sampleRate = static_cast<int>(_strtoi64(fields[4].c_str(), nullptr, 10));
        song.bitsPerSample = static_cast<int>(_strtoi64(fields[5].c_str(), nullptr, 10));
        song.title = Wide(fields[6]);
        song.album = Wide(fields[7]);
        song.albumArtist = Wide(fields[8]);
        std::wstring artists = Wide(fields[9]);
        size_t pos = 0;
        while (pos <= artists.size()) {
            const size_t end = artists.find(L'\x1f', pos);
            const std::wstring name = artists.substr(pos, end == std::wstring::npos ? std::wstring::npos : end - pos);
            if (!name.empty()) song.artists.push_back(name);
            if (end == std::wstring::npos) break;
            pos = end + 1;
        }
        if (fields.size() >= 13 && !fields[12].empty()) {  // artist ids added with the artist pages
            std::vector<int64_t> ids;
            const std::wstring values = Wide(fields[12]);
            size_t idPos = 0;
            while (idPos <= values.size()) {
                const size_t idEnd = values.find(L'\x1f', idPos);
                const std::wstring part =
                    values.substr(idPos, idEnd == std::wstring::npos ? std::wstring::npos : idEnd - idPos);
                ids.push_back(_wtoi64(part.c_str()));
                if (idEnd == std::wstring::npos) break;
                idPos = idEnd + 1;
            }
            for (size_t i = 0; i < song.artists.size(); ++i) {
                ArtistRef ref;
                ref.name = song.artists[i];
                if (i < ids.size()) ref.id = ids[i];
                song.artistRefs.push_back(std::move(ref));
            }
        }
        song.coverUrl = Wide(fields[10]);
        song.codec = Wide(fields[11]);
        loaded.push_back(std::move(song));
    }
    if (loaded.empty()) return;
    {
        std::lock_guard lock(m_mutex);
        for (const auto& song : loaded) m_songs[song.id] = song;
    }
    Hint(loaded);
}

void Catalog::Save() const {
    const auto path = CacheFile();
    if (path.empty() || !EnsureDir(path.parent_path())) return;
    std::vector<Song> songs;
    {
        std::lock_guard lock(m_mutex);
        songs.reserve(m_songs.size());
        for (const auto& [id, song] : m_songs) songs.push_back(song);
    }
    if (songs.empty()) return;

    auto tmp = path;
    tmp += L".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return;
        for (const auto& song : songs) {
            std::wstring artists;
            for (size_t i = 0; i < song.artists.size(); ++i) {
                if (i) artists += L'\x1f';
                artists += song.artists[i];
            }
            std::wstring artistIds;
            for (size_t i = 0; i < song.artists.size(); ++i) {
                if (i) artistIds += L'\x1f';
                artistIds += song.artistRefs.size() > i ? std::to_wstring(song.artistRefs[i].id) : L"0";
            }
            out << song.id << '\t' << song.durationMs << '\t' << song.fee << '\t' << song.bitrate << '\t'
                << song.sampleRate << '\t' << song.bitsPerSample << '\t' << Clean(song.title) << '\t'
                << Clean(song.album) << '\t' << Clean(song.albumArtist) << '\t' << Clean(artists) << '\t'
                << Clean(song.coverUrl) << '\t' << Clean(song.codec) << '\t' << Clean(artistIds) << '\n';
        }
    }
    MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
}

}  // namespace glasswave::netease
