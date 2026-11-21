#include "pch.h"
#include "applemusic/catalog.h"
#include "applemusic/display_metadata.h"
#include "netease/profile.h"
#include "stream/track_snapshot.h"

#include <SDK/file_info_impl.h>

#include <filesystem>
#include <fstream>
#include <sstream>

namespace glasswave::applemusic {

namespace {

constexpr uintmax_t kCacheFileLimit = 64u * 1024u * 1024u;
constexpr size_t kCacheLineLimit = 200'000;
constexpr std::string_view kCacheHeader = "# glasswave apple music songs v6";

std::string Utf8(std::wstring_view text) {
    if (text.empty()) return {};
    return pfc::stringcvt::string_utf8_from_wide(text.data(), text.size()).get_ptr();
}

std::wstring Wide(std::string_view text) {
    if (text.empty()) return {};
    const std::string copy(text);
    return pfc::stringcvt::string_wide_from_utf8(copy.c_str()).get_ptr();
}

bool IsCatalogId(std::wstring_view id) {
    return !id.empty() && !IsLibraryId(id) &&
           std::all_of(id.begin(), id.end(), [](wchar_t c) { return c >= L'0' && c <= L'9'; });
}

std::string Clean(std::wstring_view text) {
    std::wstring copy(text);
    for (auto& c : copy)
        if (c == L'\t' || c == L'\r' || c == L'\n') c = L' ';
    return Utf8(copy);
}

std::string Pack(const std::vector<std::wstring>& values) {
    std::string out;
    for (size_t i = 0; i < values.size(); ++i) {
        if (i) out.push_back('\x1f');
        out += Clean(values[i]);
    }
    return out;
}

std::vector<std::string> Split(std::string_view text) {
    std::vector<std::string> values;
    size_t start = 0;
    while (start <= text.size()) {
        const size_t end = text.find('\x1f', start);
        values.emplace_back(
            text.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start));
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    return values;
}

std::filesystem::path CacheFile() {
    const auto profile = netease::ProfileDir();
    return profile.empty() ? std::filesystem::path()
                           : profile.parent_path() / L"applemusic" / L"cache" / L"songs.tsv";
}

} // namespace

Catalog& Catalog::Get() {
    static Catalog catalog;
    return catalog;
}

void Catalog::LoadMeasuredLocked() const {
    if (m_measuredLoaded) return;
    m_measuredLoaded = true;
    const auto path = MeasuredFile();
    if (path.empty()) return;
    std::ifstream input(path, std::ios::binary);
    std::string line;
    if (!input || !std::getline(input, line) || line != kMeasuredHeader) return;
    while (m_measured.size() < kMeasuredLimit && std::getline(input, line)) {
        const size_t tab = line.find('\t');
        if (tab == std::string::npos || tab == 0) continue;
        const int length = static_cast<int>(_strtoi64(line.c_str() + tab + 1, nullptr, 10));
        if (length > 0) m_measured[Wide(std::string_view(line).substr(0, tab))] = length;
    }
}

void Catalog::ApplyMeasuredLocked(Song& song) const {
    LoadMeasuredLocked();
    if (const auto found = m_measured.find(song.track.id); found != m_measured.end())
        song.track.durationMs = found->second;
}

void Catalog::SaveMeasuredLocked() const {
    const auto path = MeasuredFile();
    if (path.empty() || !netease::EnsureDir(path.parent_path())) return;
    auto temp = path;
    temp += L".tmp";
    {
        std::ofstream output(temp, std::ios::binary | std::ios::trunc);
        if (!output) return;
        output << kMeasuredHeader << '\n';
        for (const auto& [id, length] : m_measured)
            output << Clean(id) << '\t' << length << '\n';
        output.close();
        if (!output) {
            std::error_code removeError;
            std::filesystem::remove(temp, removeError);
            return;
        }
    }
    MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
}

int Catalog::MeasuredLength(const stream::Id& songId) const {
    std::lock_guard lock(m_mutex);
    LoadMeasuredLocked();
    const auto found = m_measured.find(songId);
    return found == m_measured.end() ? 0 : found->second;
}

void Catalog::PutMeasuredLength(const stream::Id& songId, int durationMs) {
    if (songId.empty() || durationMs <= 0) return;
    std::optional<Song> song;
    {
        std::lock_guard lock(m_mutex);
        LoadMeasuredLocked();
        if (const auto found = m_measured.find(songId); found != m_measured.end() && found->second == durationMs)
            return;
        m_measured[songId] = durationMs;
        SaveMeasuredLocked();
        if (const auto found = m_songs.find(songId); found != m_songs.end()) {
            found->second.track.durationMs = durationMs;
            song = found->second;
        }
    }
    if (song) {
        Hint({*song});
        Save();
    }
}

namespace {
// What Hint writes to metadb: a record that differs here is shown differently.
bool SameTags(const Song& a, const Song& b) {
    if (a.track.title != b.track.title || a.track.album != b.track.album ||
        a.track.durationMs != b.track.durationMs || a.composerName != b.composerName ||
        a.composerNames != b.composerNames || a.genreNames != b.genreNames ||
        a.track.artists.size() != b.track.artists.size())
        return false;
    for (size_t i = 0; i < a.track.artists.size(); ++i)
        if (a.track.artists[i].name != b.track.artists[i].name) return false;
    return true;
}
} // namespace

// Every record that is new or shown differently goes to metadb as well, whoever put it: a
// song known only here kept, in every list and across restarts, the stand-in title an input
// gave it before (owner, 2026-11-21: "Apple Music 1888098957", the catalog's restart cache
// skipping songs already put, unknown-song reads skipping songs already known).
bool Catalog::StoreLocked(const Song& song) {
    const auto found = m_songs.find(song.track.id);
    const bool shown = found != m_songs.end() && SameTags(found->second, song);
    Song& stored = m_songs[song.track.id] = song;
    ApplyMeasuredLocked(stored);
    if (!song.track.albumId.empty()) m_songAlbums[song.track.id] = song.track.albumId;
    if (shown) return false;
    m_hintQueue.push_back(song.track.id);
    return !std::exchange(m_hintPosted, true);
}

void Catalog::PostHints() {
    fb2k::inMainThread([] { Catalog::Get().FlushHints(); });
}

void Catalog::FlushHints() {
    std::vector<Song> songs;
    {
        std::lock_guard lock(m_mutex);
        m_hintPosted = false;
        std::unordered_set<stream::Id> seen;
        for (const auto& id : m_hintQueue)
            if (seen.insert(id).second)
                if (const auto found = m_songs.find(id); found != m_songs.end()) songs.push_back(found->second);
        m_hintQueue.clear();
    }
    Hint(songs);
}

void Catalog::Put(const Song& song) {
    if (song.track.id.empty()) return;
    bool post = false;
    {
        std::lock_guard lock(m_mutex);
        post = StoreLocked(song);
    }
    if (post) PostHints();
}

void Catalog::Put(const std::vector<Song>& songs) {
    bool post = false;
    {
        std::lock_guard lock(m_mutex);
        for (const auto& song : songs)
            if (!song.track.id.empty()) post = StoreLocked(song) || post;
    }
    if (post) PostHints();
}

void Catalog::Put(const stream::Station& station) {
    if (station.id.empty()) return;
    std::lock_guard lock(m_mutex);
    m_stations[station.id] = station;
}

void Catalog::Put(const stream::Playlist& playlist) {
    if (playlist.id.empty()) return;
    std::lock_guard lock(m_mutex);
    m_playlists[playlist.id] = playlist;
}

void Catalog::Put(const std::vector<stream::Playlist>& playlists) {
    for (const auto& playlist : playlists)
        Put(playlist);
}

void Catalog::Put(const stream::Album& album) {
    if (album.id.empty()) return;
    std::lock_guard lock(m_mutex);
    m_albums[album.id] = album;
}

void Catalog::Put(const std::vector<stream::Album>& albums) {
    for (const auto& album : albums)
        Put(album);
}

void Catalog::Put(const stream::Artist& artist) {
    if (artist.id.empty()) return;
    std::lock_guard lock(m_mutex);
    m_artists[artist.id] = artist;
}

void Catalog::Put(const std::vector<stream::Artist>& artists) {
    for (const auto& artist : artists)
        Put(artist);
}

void Catalog::PutSongAlbum(const stream::Id& songId, const stream::Id& albumId) {
    if (songId.empty() || albumId.empty()) return;
    std::lock_guard lock(m_mutex);
    m_songAlbums[songId] = albumId;
}

stream::Id Catalog::SongAlbum(const stream::Id& songId) const {
    std::lock_guard lock(m_mutex);
    if (const auto song = m_songs.find(songId); song != m_songs.end() && !song->second.track.albumId.empty())
        return song->second.track.albumId;
    const auto found = m_songAlbums.find(songId);
    return found == m_songAlbums.end() ? stream::Id() : found->second;
}

std::optional<Song> Catalog::FindSongRaw(const stream::Id& id) const {
    std::lock_guard lock(m_mutex);
    const auto it = m_songs.find(id);
    return it == m_songs.end() ? std::nullopt : std::optional<Song>(it->second);
}

std::optional<Song> Catalog::FindSong(const stream::Id& id) const {
    auto song = FindSongRaw(id);
    if (song) DisplayMetadata::Get().Apply(*song, DisplayMetadata::Get().SourceStorefront());
    return song;
}

std::optional<stream::Playlist> Catalog::FindPlaylistRaw(const stream::Id& id) const {
    std::lock_guard lock(m_mutex);
    const auto it = m_playlists.find(id);
    return it == m_playlists.end() ? std::nullopt : std::optional<stream::Playlist>(it->second);
}

std::optional<stream::Playlist> Catalog::FindPlaylist(const stream::Id& id) const {
    auto playlist = FindPlaylistRaw(id);
    if (playlist) DisplayMetadata::Get().Apply(*playlist, DisplayMetadata::Get().SourceStorefront());
    return playlist;
}

std::optional<stream::Album> Catalog::FindAlbumRaw(const stream::Id& id) const {
    std::lock_guard lock(m_mutex);
    const auto it = m_albums.find(id);
    return it == m_albums.end() ? std::nullopt : std::optional<stream::Album>(it->second);
}

std::optional<stream::Album> Catalog::FindAlbum(const stream::Id& id) const {
    auto album = FindAlbumRaw(id);
    if (album) DisplayMetadata::Get().Apply(*album, DisplayMetadata::Get().SourceStorefront());
    return album;
}

std::optional<stream::Artist> Catalog::FindArtistRaw(const stream::Id& id) const {
    std::lock_guard lock(m_mutex);
    const auto it = m_artists.find(id);
    return it == m_artists.end() ? std::nullopt : std::optional<stream::Artist>(it->second);
}

std::optional<stream::Artist> Catalog::FindArtist(const stream::Id& id) const {
    auto artist = FindArtistRaw(id);
    if (artist) DisplayMetadata::Get().Apply(*artist, DisplayMetadata::Get().SourceStorefront());
    return artist;
}

void Catalog::PutAlbumLibraryId(const stream::Id& catalogId, const stream::Id& libraryId) {
    if (!IsCatalogId(catalogId) || !IsLibraryId(libraryId) || libraryId.empty() || libraryId[0] != L'l') {
        return;
    }
    std::lock_guard lock(m_mutex);
    m_albumLibraryIds[catalogId] = libraryId;
}

stream::Id Catalog::FindAlbumCatalogId(const stream::Id& libraryId) const {
    std::lock_guard lock(m_mutex);
    const auto found = std::find_if(m_albumLibraryIds.begin(), m_albumLibraryIds.end(),
                                    [&libraryId](const auto& item) { return item.second == libraryId; });
    return found == m_albumLibraryIds.end() ? stream::Id() : found->first;
}

void Catalog::PutArtistCatalogId(const stream::Id& libraryId, const stream::Id& catalogId) {
    if (!IsLibraryId(libraryId) || libraryId.empty() || libraryId[0] != L'r' || !IsCatalogId(catalogId)) {
        return;
    }
    std::lock_guard lock(m_mutex);
    m_artistCatalogIds[libraryId] = catalogId;
}

stream::Id Catalog::FindArtistCatalogId(const stream::Id& libraryId) const {
    std::lock_guard lock(m_mutex);
    const auto found = m_artistCatalogIds.find(libraryId);
    return found == m_artistCatalogIds.end() ? stream::Id() : found->second;
}

void Catalog::RemovePlaylist(const stream::Id& id) {
    if (id.empty()) return;
    std::lock_guard lock(m_mutex);
    m_playlists.erase(id);
}

void Catalog::RemoveAlbum(const stream::Id& id) {
    if (id.empty()) return;
    std::lock_guard lock(m_mutex);
    m_albums.erase(id);
}

metadb_handle_ptr Catalog::Handle(const stream::Id& songId) const {
    const std::string path = SongPath(songId);
    return path.empty() ? metadb_handle_ptr() : metadb::get()->handle_create(path.c_str(), 0);
}

metadb_handle_ptr Catalog::CoverHandle(stream::ArtKind kind, const stream::Id& id) const {
    const CoverKind coverKind = kind == stream::ArtKind::Album      ? CoverKind::Album
                                : kind == stream::ArtKind::Artist   ? CoverKind::Artist
                                : kind == stream::ArtKind::Playlist ? CoverKind::Playlist
                                : kind == stream::ArtKind::Station  ? CoverKind::Station
                                                                    : CoverKind::Track;
    const std::string path = CoverPath(coverKind, id);
    return path.empty() ? metadb_handle_ptr() : metadb::get()->handle_create(path.c_str(), 0);
}

std::wstring Catalog::CoverUrl(stream::ArtKind kind, const stream::Id& id) const {
    switch (kind) {
    case stream::ArtKind::Track:
        if (const auto song = FindSong(id)) return song->coverUrl;
        break;
    case stream::ArtKind::Album:
        if (const auto album = FindAlbum(id)) return album->coverUrl;
        break;
    case stream::ArtKind::Artist:
        if (const auto artist = FindArtist(id)) return artist->avatarUrl;
        break;
    case stream::ArtKind::Playlist:
        if (const auto playlist = FindPlaylist(id)) return playlist->coverUrl;
        break;
    case stream::ArtKind::Station: {
        std::lock_guard lock(m_mutex);
        if (const auto found = m_stations.find(id); found != m_stations.end()) return found->second.coverUrl;
        break;
    }
    }
    return {};
}

void Catalog::PutAndHint(const std::vector<Song>& songs, bool persist) {
    Put(songs);
    FlushHints(); // at once (main thread), with whatever else was waiting
    if (persist) Save();
}

void Catalog::Hint(const std::vector<Song>& songs) const {
    if (songs.empty()) return;
    auto hints = metadb_io_v2::get()->create_hint_list();
    if (!hints.is_valid()) return;
    for (const auto& song : songs) {
        if (song.track.id.empty()) continue;
        Song display = song;
        DisplayMetadata::Get().Apply(display, DisplayMetadata::Get().SourceStorefront());
        file_info_impl info;
        info.meta_set("title", Utf8(display.track.title).c_str());
        info.meta_set("artist", Utf8(stream::JoinArtists(display.track.artists)).c_str());
        info.meta_set("album", Utf8(display.track.album).c_str());
        if (!display.track.artists.empty())
            info.meta_set("album artist", Utf8(display.track.artists.front().name).c_str());
        if (!display.composerNames.empty() || !display.composerName.empty()) {
            std::wstring composers;
            if (!display.composerNames.empty()) {
                for (size_t i = 0; i < display.composerNames.size(); ++i) {
                    if (i) composers += L"; ";
                    composers += display.composerNames[i];
                }
            } else {
                composers = display.composerName;
            }
            info.meta_set("composer", Utf8(composers).c_str());
        }
        if (!display.genreNames.empty()) {
            std::wstring genres;
            for (size_t i = 0; i < display.genreNames.size(); ++i) {
                if (i) genres += L"; ";
                genres += display.genreNames[i];
            }
            info.meta_set("genre", Utf8(genres).c_str());
        }
        if (display.track.durationMs > 0)
            info.set_length(static_cast<double>(display.track.durationMs) / 1000.0);
        // Tags only: the technical info playback measured (bit rate, codec...) stays.
        stream::HintTags(hints, Handle(display.track.id), info);
    }
    hints->on_done();
}

std::vector<Song> Catalog::ReadCacheFile() const {
    const auto path = CacheFile();
    if (path.empty()) return {};
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec) || ec) return {};
    const uintmax_t bytes = std::filesystem::file_size(path, ec);
    if (ec || bytes == 0 || bytes > kCacheFileLimit) return {};

    std::ifstream input(path, std::ios::binary);
    if (!input) return {};
    std::vector<Song> loaded;
    std::string line;
    size_t lines = 0;
    bool headerSeen = false;
    while (std::getline(input, line)) {
        if (++lines > kCacheLineLimit) return {};
        if (!headerSeen) {
            if (line.empty()) continue;
            if (line != kCacheHeader) return {};
            headerSeen = true;
            continue;
        }
        if (line.empty() || line[0] == '#') continue;
        std::vector<std::string> fields;
        size_t start = 0;
        bool extraFields = false;
        while (start <= line.size()) {
            const size_t end = line.find('\t', start);
            if (fields.size() == 20) {
                extraFields = true;
                break;
            }
            fields.emplace_back(
                line.substr(start, end == std::string::npos ? std::string::npos : end - start));
            if (end == std::string::npos) break;
            start = end + 1;
        }
        if (extraFields || fields.size() != 20) continue;

        Song song;
        song.track.id = Wide(fields[0]);
        song.catalogId = Wide(fields[1]);
        song.libraryId = Wide(fields[2]);
        song.libraryAlbumId = Wide(fields[18]);
        song.reportingId = Wide(fields[19]);
        song.isrc = Wide(fields[3]);
        song.contentRating = Wide(fields[4]);
        song.customTitle = fields[5] == "1";
        song.customAlbum = fields[6] == "1";
        if (song.track.id.empty()) continue;
        song.track.durationMs = static_cast<int>(_strtoi64(fields[7].c_str(), nullptr, 10));
        song.track.popularity = static_cast<int>(_strtoi64(fields[8].c_str(), nullptr, 10));
        song.track.title = Wide(fields[9]);
        song.track.album = Wide(fields[10]);
        song.track.albumId = Wide(fields[11]);
        if (song.libraryAlbumId.empty() && IsLibraryId(song.track.albumId) && song.track.albumId[0] == L'l')
            song.libraryAlbumId = song.track.albumId;
        const auto artistNames = Split(fields[12]);
        const auto artistIds = Split(fields[13]);
        for (size_t i = 0; i < artistNames.size(); ++i) {
            const std::wstring name = Wide(artistNames[i]);
            if (name.empty()) continue;
            song.track.artists.push_back({i < artistIds.size() ? Wide(artistIds[i]) : std::wstring(), name});
        }
        song.coverUrl = Wide(fields[14]);
        {
            for (const auto& genre : Split(fields[15])) {
                const std::wstring value = Wide(genre);
                if (!value.empty()) song.genreNames.push_back(value);
            }
            song.composerName = Wide(fields[16]);
            {
                for (const auto& composer : Split(fields[17])) {
                    const std::wstring value = Wide(composer);
                    if (!value.empty()) song.composerNames.push_back(value);
                }
            }
        }
        loaded.push_back(std::move(song));
    }
    return headerSeen ? loaded : std::vector<Song>{};
}

void Catalog::InstallCached(std::vector<Song> songs) {
    std::vector<Song> installed;
    {
        std::lock_guard lock(m_mutex);
        for (auto& song : songs) {
            if (song.track.id.empty()) continue;
            if (!song.track.albumId.empty() && !song.libraryAlbumId.empty() &&
                IsLibraryId(song.libraryAlbumId) && song.libraryAlbumId[0] == L'l') {
                m_albumLibraryIds[song.track.albumId] = song.libraryAlbumId;
            }
            if (!song.track.albumId.empty()) m_songAlbums.try_emplace(song.track.id, song.track.albumId);
            if (m_songs.contains(song.track.id)) continue;
            const stream::Id id = song.track.id;
            installed.push_back(song);
            m_songs.emplace(id, std::move(song));
        }
    }
    Hint(installed);
}

void Catalog::Save() const {
    const auto path = CacheFile();
    if (path.empty() || !netease::EnsureDir(path.parent_path())) return;
    std::vector<Song> songs;
    {
        std::lock_guard lock(m_mutex);
        songs.reserve(m_songs.size());
        for (const auto& [id, song] : m_songs)
            songs.push_back(song);
    }
    std::sort(songs.begin(), songs.end(),
              [](const Song& a, const Song& b) { return a.track.id < b.track.id; });

    auto temp = path;
    temp += L".tmp";
    {
        std::ofstream output(temp, std::ios::binary | std::ios::trunc);
        if (!output) return;
        output << kCacheHeader << '\n';
        for (const auto& song : songs) {
            std::vector<std::wstring> names;
            std::vector<std::wstring> ids;
            for (const auto& artist : song.track.artists) {
                names.push_back(artist.name);
                ids.push_back(artist.id);
            }
            output << Clean(song.track.id) << '\t' << Clean(song.catalogId) << '\t' << Clean(song.libraryId)
                   << '\t' << Clean(song.isrc) << '\t' << Clean(song.contentRating) << '\t'
                   << (song.customTitle ? '1' : '0') << '\t' << (song.customAlbum ? '1' : '0') << '\t'
                   << song.track.durationMs << '\t' << song.track.popularity << '\t'
                   << Clean(song.track.title) << '\t' << Clean(song.track.album) << '\t'
                   << Clean(song.track.albumId) << '\t' << Pack(names) << '\t' << Pack(ids) << '\t'
                   << Clean(song.coverUrl) << '\t' << Pack(song.genreNames) << '\t'
                   << Clean(song.composerName) << '\t' << Pack(song.composerNames) << '\t'
                   << Clean(song.libraryAlbumId) << '\t' << Clean(song.reportingId) << '\n';
        }
        output.close();
        if (!output) {
            std::error_code removeError;
            std::filesystem::remove(temp, removeError);
            return;
        }
    }
    MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
}

void Catalog::RefreshHints() const {
    std::vector<Song> songs;
    {
        std::lock_guard lock(m_mutex);
        songs.reserve(m_songs.size());
        for (const auto& [id, song] : m_songs) {
            (void)id;
            songs.push_back(song);
        }
    }
    Hint(songs);
}

void Catalog::ClearMemory() {
    std::lock_guard lock(m_mutex);
    m_songs.clear();
    m_playlists.clear();
    m_albums.clear();
    m_artists.clear();
    m_songAlbums.clear();
}

void Catalog::Clear() {
    ClearMemory();
    {
        std::lock_guard lock(m_mutex);
        m_albumLibraryIds.clear();
        m_artistCatalogIds.clear();
    }
    const auto path = CacheFile();
    if (!path.empty()) {
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }
}

} // namespace glasswave::applemusic
