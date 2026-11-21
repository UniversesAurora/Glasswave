#include "pch.h"
#include "netease/catalog.h"
#include "netease/handle.h"
#include "core/paths.h"
#include "stream/track_snapshot.h"

#include <SDK/file_info_impl.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unordered_set>

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
    const auto dir = core::ProfileDir(L"netease");
    return dir.empty() ? dir : dir / L"cache" / L"songs.tsv";
}

void Fill(std::wstring& target, const std::wstring& value) {
    if (!value.empty() && (target.empty() || target == L"Unknown")) target = value;
}

void MergeRestored(Song& target, const Song& source) {
    Fill(target.title, source.title);
    if (target.note.empty()) target.note = source.note;
    target.original = target.original || source.original;
    Fill(target.album, source.album);
    Fill(target.albumArtist, source.albumArtist);
    if (target.artists.empty()) target.artists = source.artists;
    if (target.artistRefs.empty()) target.artistRefs = source.artistRefs;
    if (!target.albumId) target.albumId = source.albumId;
    if (target.durationMs <= 0) target.durationMs = source.durationMs;
    if (target.fee == 0) target.fee = source.fee;
    if (target.popularity == 0) target.popularity = source.popularity;
    if (target.coverUrl.empty()) target.coverUrl = source.coverUrl;
    if (target.codec.empty()) target.codec = source.codec;
    if (target.bitrate == 0) target.bitrate = source.bitrate;
    if (target.sampleRate == 0) target.sampleRate = source.sampleRate;
    if (target.bitsPerSample == 0) target.bitsPerSample = source.bitsPerSample;
}

void MergeRestored(Playlist& target, const Playlist& source) {
    Fill(target.name, source.name);
    if (target.coverUrl.empty()) target.coverUrl = source.coverUrl;
    if (target.trackCount == 0) target.trackCount = source.trackCount;
    if (!target.creatorId) target.creatorId = source.creatorId;
    Fill(target.creatorName, source.creatorName);
    Fill(target.description, source.description);
    target.subscribed = target.subscribed || source.subscribed;
    if (!target.createTime) target.createTime = source.createTime;
    if (!target.updateTime) target.updateTime = source.updateTime;
    if (target.tags.empty()) target.tags = source.tags;
    Fill(target.updateFrequency, source.updateFrequency);
    Fill(target.englishTitle, source.englishTitle);
    target.chart = target.chart || source.chart;
}

void MergeRestored(Artist& target, const Artist& source) {
    Fill(target.name, source.name);
    if (target.avatarUrl.empty()) target.avatarUrl = source.avatarUrl;
    Fill(target.brief, source.brief);
    if (target.albumCount == 0) target.albumCount = source.albumCount;
    if (target.songCount == 0) target.songCount = source.songCount;
    if (source.followKnown) {
        target.followKnown = true;
        target.followed = target.followed || source.followed;
    }
}

void MergeRestored(Album& target, const Album& source) {
    Fill(target.name, source.name);
    if (target.coverUrl.empty()) target.coverUrl = source.coverUrl;
    if (target.artistRefs.empty()) target.artistRefs = source.artistRefs;
    Fill(target.description, source.description);
    Fill(target.company, source.company);
    Fill(target.type, source.type);
    Fill(target.subType, source.subType);
    if (target.aliases.empty()) target.aliases = source.aliases;
    if (target.trackCount == 0) target.trackCount = source.trackCount;
    if (target.publishTime == 0) target.publishTime = source.publishTime;
}

} // namespace

Catalog& Catalog::Get() {
    static Catalog catalog;
    return catalog;
}

namespace {
// What Hint writes to metadb: a record that differs here is shown differently.
bool SameTags(const Song& a, const Song& b) {
    if (a.title != b.title || a.album != b.album || a.albumArtist != b.albumArtist ||
        a.durationMs != b.durationMs || a.codec != b.codec || a.bitrate != b.bitrate ||
        a.sampleRate != b.sampleRate || a.bitsPerSample != b.bitsPerSample)
        return false;
    return a.artists == b.artists;
}
} // namespace

// Every record that is new or shown differently goes to metadb as well, whoever put it (as in
// the Apple Music catalog): a song known only here must not keep another title in the lists.
bool Catalog::StoreLocked(const Song& song) {
    const auto found = m_songs.find(song.id);
    const bool shown = found != m_songs.end() && SameTags(found->second, song);
    m_songs[song.id] = song;
    if (shown) return false;
    m_hintQueue.push_back(song.id);
    return !std::exchange(m_hintPosted, true);
}

void Catalog::FlushHints() {
    std::vector<Song> songs;
    {
        std::lock_guard lock(m_mutex);
        m_hintPosted = false;
        std::unordered_set<int64_t> seen;
        for (const int64_t id : m_hintQueue)
            if (seen.insert(id).second)
                if (const auto found = m_songs.find(id); found != m_songs.end()) songs.push_back(found->second);
        m_hintQueue.clear();
    }
    Hint(songs);
}

void Catalog::Put(const Song& song) {
    if (!song.id) return;
    bool post = false;
    {
        std::lock_guard lock(m_mutex);
        post = StoreLocked(song);
    }
    if (post) fb2k::inMainThread([] { Catalog::Get().FlushHints(); });
}

void Catalog::Put(const std::vector<Song>& songs) {
    bool post = false;
    {
        std::lock_guard lock(m_mutex);
        for (const auto& song : songs)
            if (song.id) post = StoreLocked(song) || post;
    }
    if (post) fb2k::inMainThread([] { Catalog::Get().FlushHints(); });
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

void Catalog::Restore(const Song& song) {
    if (!song.id) return;
    std::lock_guard lock(m_mutex);
    auto [it, inserted] = m_songs.try_emplace(song.id, song);
    if (!inserted) MergeRestored(it->second, song);
}

void Catalog::Restore(const Playlist& playlist) {
    if (!playlist.id) return;
    std::lock_guard lock(m_mutex);
    auto [it, inserted] = m_playlists.try_emplace(playlist.id, playlist);
    if (!inserted) MergeRestored(it->second, playlist);
}

void Catalog::Restore(const Artist& artist) {
    if (!artist.id) return;
    std::lock_guard lock(m_mutex);
    auto [it, inserted] = m_artists.try_emplace(artist.id, artist);
    if (!inserted) MergeRestored(it->second, artist);
}

void Catalog::Restore(const Album& album) {
    if (!album.id) return;
    std::lock_guard lock(m_mutex);
    auto [it, inserted] = m_albums.try_emplace(album.id, album);
    if (!inserted) MergeRestored(it->second, album);
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
    std::lock_guard lock(m_mutex);
    if (auto it = m_preferredCovers.find(playlistId); it != m_preferredCovers.end() && !it->second.empty())
        return it->second;
    if (auto it = m_playlists.find(playlistId); it != m_playlists.end()) return it->second.coverUrl;
    return {};
}

void Catalog::PreferCover(int64_t playlistId, std::wstring coverUrl) {
    if (!playlistId || coverUrl.empty()) return;
    std::lock_guard lock(m_mutex);
    m_preferredCovers[playlistId] = std::move(coverUrl);
}

void Catalog::ClearPreferredCovers() {
    std::lock_guard lock(m_mutex);
    m_preferredCovers.clear();
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
    FlushHints(); // at once (main thread), with whatever else was waiting
    Save();
}

void Catalog::RestoreAndHint(const std::vector<Song>& songs) {
    if (songs.empty()) return;
    std::vector<Song> restored;
    restored.reserve(songs.size());
    {
        std::lock_guard lock(m_mutex);
        for (const auto& song : songs) {
            if (!song.id) continue;
            auto [it, inserted] = m_songs.try_emplace(song.id, song);
            if (!inserted) MergeRestored(it->second, song);
            restored.push_back(it->second);
        }
    }
    Hint(restored);
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
        if (song.bitrate > 0) info.info_set_int("bitrate", (song.bitrate + 500) / 1000); // kbps
        if (song.sampleRate > 0) info.info_set_int("samplerate", song.sampleRate);
        if (song.bitsPerSample > 0) info.info_set_int("bitspersample", song.bitsPerSample);
        info.info_set_int("channels", 2);
        if (song.codec == L"FLAC") info.info_set("encoding", "lossless");
        stream::HintTags(hints, Handle(song.id), info); // keeps what playback measured
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
        if (fields.size() != 16) continue; // one current format; other lines are dropped
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
            const std::wstring name =
                artists.substr(pos, end == std::wstring::npos ? std::wstring::npos : end - pos);
            if (!name.empty()) song.artists.push_back(name);
            if (end == std::wstring::npos) break;
            pos = end + 1;
        }
        if (!fields[12].empty()) {
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
        song.albumId = _strtoi64(fields[13].c_str(), nullptr, 10);
        song.note = Wide(fields[14]);
        song.original = fields[15] == "1";
        loaded.push_back(std::move(song));
    }
    if (loaded.empty()) return;
    {
        std::lock_guard lock(m_mutex);
        for (const auto& song : loaded)
            m_songs[song.id] = song;
    }
    Hint(loaded);
}

void Catalog::Save() const {
    const auto path = CacheFile();
    if (path.empty() || !core::EnsureDir(path.parent_path())) return;
    std::vector<Song> songs;
    {
        std::lock_guard lock(m_mutex);
        songs.reserve(m_songs.size());
        for (const auto& [id, song] : m_songs)
            songs.push_back(song);
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
                << Clean(song.coverUrl) << '\t' << Clean(song.codec) << '\t' << Clean(artistIds) << '\t'
                << song.albumId << '\t' << Clean(song.note) << '\t' << (song.original ? '1' : '0') << '\n';
        }
        out.close();
        if (!out) { // disk full or similar: keep the previous cache
            DeleteFileW(tmp.c_str());
            return;
        }
    }
    MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
}

} // namespace glasswave::netease
