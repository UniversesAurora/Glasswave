#include "pch.h"
#include "netease/catalog.h"
#include "netease/handle.h"

#include <SDK/file_info_impl.h>

namespace glasswave::netease {

namespace {

std::string Utf8(std::wstring_view s) {
    if (s.empty()) return {};
    return pfc::stringcvt::string_utf8_from_wide(s.data(), s.size()).get_ptr();
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

metadb_handle_ptr Catalog::Handle(int64_t songId) const {
    return metadb::get()->handle_create(SongPath(songId).c_str(), 0);
}

metadb_handle_ptr Catalog::PlaylistHandle(int64_t playlistId) const {
    return metadb::get()->handle_create(CoverPath(true, playlistId).c_str(), 0);
}

std::wstring Catalog::SongCover(int64_t songId) const {
    if (const auto song = FindSong(songId)) return song->coverUrl;
    return {};
}

std::wstring Catalog::PlaylistCover(int64_t playlistId) const {
    if (const auto playlist = FindPlaylist(playlistId)) return playlist->coverUrl;
    return {};
}

void Catalog::PutAndHint(const std::vector<Song>& songs) {
    Put(songs);
    Hint(songs);
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
        t_filestats stats;
        hints->add_hint(Handle(song.id), info, stats, true);
    }
    hints->on_done();
}

}  // namespace glasswave::netease
