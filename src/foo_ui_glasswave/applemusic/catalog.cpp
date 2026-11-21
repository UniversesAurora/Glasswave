#include "pch.h"
#include "applemusic/catalog.h"
#include "applemusic/client.h"
#include "applemusic/display_metadata.h"
#include "core/file.h"
#include "core/paths.h"
#include "core/text.h"

#include <SDK/file_info_impl.h>

#include <filesystem>
#include <fstream>

namespace glasswave::applemusic {

namespace {

using core::Utf8;
using core::Wide;

constexpr std::string_view kMeasuredHeader = "# glasswave apple music delivered lengths v1";
constexpr size_t kMeasuredLimit = 100'000;
constexpr size_t kMaxIdBytes = 1024;
constexpr char kFieldSeparator = '\x1e';
constexpr char kListSeparator = '\x1f';
constexpr size_t kExtraFields = 11;

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
        if (i) out.push_back(kListSeparator);
        out += Clean(values[i]);
    }
    return out;
}

std::vector<std::string_view> Split(std::string_view text, char separator) {
    std::vector<std::string_view> values;
    size_t start = 0;
    while (start <= text.size()) {
        const size_t end = text.find(separator, start);
        values.push_back(
            text.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start));
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    return values;
}

std::vector<std::wstring> Unpack(std::string_view text) {
    std::vector<std::wstring> values;
    for (const auto part : Split(text, kListSeparator)) {
        std::wstring value = Wide(part);
        if (!value.empty()) values.push_back(std::move(value));
    }
    return values;
}

std::filesystem::path MeasuredFile() {
    const auto dir = core::ProfileDir(L"applemusic");
    return dir.empty() ? std::filesystem::path() : dir / L"cache" / L"lengths.tsv";
}

// ---- metadb paths: apple://song/<id>, apple://cover/<kind>/<id> -----------------------------
// Ids are percent-encoded UTF-8 path segments so a catalog id and a library id cannot be
// confused by punctuation or a second slash.

int HexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool SafeByte(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' ||
           c == '_' || c == '.' || c == '~';
}

std::string EncodeId(const stream::Id& id) {
    return id.empty() ? std::string() : Utf8(Encode(id));
}

bool DecodeId(std::string_view segment, stream::Id& id) {
    if (segment.empty() || segment.size() > kMaxIdBytes * 3) return false;
    std::string bytes;
    bytes.reserve(segment.size());
    for (size_t i = 0; i < segment.size();) {
        const unsigned char c = static_cast<unsigned char>(segment[i]);
        if (c == '%') {
            if (i + 2 >= segment.size()) return false;
            const int hi = HexValue(segment[i + 1]);
            const int lo = HexValue(segment[i + 2]);
            if (hi < 0 || lo < 0) return false;
            bytes.push_back(static_cast<char>((hi << 4) | lo));
            i += 3;
        } else {
            if (!SafeByte(c)) return false;
            bytes.push_back(static_cast<char>(c));
            ++i;
        }
    }
    if (bytes.empty() || bytes.size() > kMaxIdBytes) return false;
    if (std::any_of(bytes.begin(), bytes.end(), [](unsigned char c) { return c < 0x20 || c == 0x7f; })) {
        return false;
    }
    try {
        id = core::WideStrict(bytes);
    } catch (const std::exception&) {
        return false;
    }
    return true;
}

// What the metadb tags show of a song beyond its track: a change here re-tags it.
bool SameExtraTags(const SongExtra& a, const SongExtra& b) {
    return a.composerName == b.composerName && a.composerNames == b.composerNames &&
           a.genreNames == b.genreNames;
}

std::wstring JoinNames(const std::vector<std::wstring>& names) {
    std::wstring out;
    for (size_t i = 0; i < names.size(); ++i) {
        if (i) out += L"; ";
        out += names[i];
    }
    return out;
}

} // namespace

Catalog::Catalog() : m_records(Traits(*this)) {}

stream::CatalogTraits Catalog::Traits(Catalog& self) {
    stream::CatalogTraits traits;
    traits.dir = L"applemusic";
    traits.scheme = "apple";
    traits.encodeId = EncodeId;
    traits.decodeId = DecodeId;
    traits.cacheHeader = "# glasswave apple music songs v7";
    traits.tags = [&self](const stream::Track& stored, file_info& info) { self.WriteTags(stored, info); };
    traits.encodeExtra = [&self](const stream::Id& id) { return self.EncodeExtra(id); };
    return traits;
}

Catalog& Catalog::Get() {
    static Catalog catalog;
    return catalog;
}

// ---- delivered lengths ---------------------------------------------------------------------

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
    if (path.empty()) return;
    std::string out(kMeasuredHeader);
    out.push_back('\n');
    for (const auto& [id, length] : m_measured)
        out += Clean(id) + '\t' + std::to_string(length) + '\n';
    try {
        core::WriteFileAtomic(path, out);
    } catch (const std::exception&) {
        // the previous file stays
    }
}

int Catalog::MeasuredLength(const stream::Id& songId) const {
    std::lock_guard lock(m_mutex);
    LoadMeasuredLocked();
    const auto found = m_measured.find(songId);
    return found == m_measured.end() ? 0 : found->second;
}

void Catalog::PutMeasuredLength(const stream::Id& songId, int durationMs) {
    if (songId.empty() || durationMs <= 0) return;
    {
        std::lock_guard lock(m_mutex);
        LoadMeasuredLocked();
        if (const auto found = m_measured.find(songId); found != m_measured.end() && found->second == durationMs)
            return;
        m_measured[songId] = durationMs;
        SaveMeasuredLocked();
    }
    if (auto track = m_records.FindTrack(songId)) {
        track->durationMs = durationMs;
        m_records.Put({*track, m_records.CoverUrl(stream::ArtKind::Track, songId), false});
        m_records.QueueSave();
    }
}

// ---- records -------------------------------------------------------------------------------

void Catalog::Put(const Song& song) {
    Put(std::vector<Song>{song});
}

void Catalog::Put(const std::vector<Song>& songs) {
    std::vector<stream::TrackRecord> records;
    records.reserve(songs.size());
    std::lock_guard lock(m_mutex);
    for (const auto& incoming : songs) {
        if (incoming.track.id.empty()) continue;
        Song song = incoming;
        ApplyMeasuredLocked(song);
        const auto known = m_extras.find(song.track.id);
        const bool retag = known == m_extras.end() || !SameExtraTags(known->second, song);
        m_extras[song.track.id] = song;
        if (!song.track.albumId.empty()) m_songAlbums[song.track.id] = song.track.albumId;
        records.push_back({std::move(song.track), std::move(song.coverUrl), retag});
    }
    m_records.Put(records);
}

void Catalog::Put(const stream::Station& station) {
    m_records.Put(station);
}

void Catalog::Put(const stream::Playlist& playlist) {
    m_records.Put(playlist);
}

void Catalog::Put(const std::vector<stream::Playlist>& playlists) {
    for (const auto& playlist : playlists)
        Put(playlist);
}

void Catalog::Put(const stream::Album& album) {
    m_records.Put(album);
}

void Catalog::Put(const std::vector<stream::Album>& albums) {
    for (const auto& album : albums)
        Put(album);
}

void Catalog::Put(const stream::Artist& artist) {
    m_records.Put(artist);
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
    if (const auto track = m_records.FindTrack(songId); track && !track->albumId.empty())
        return track->albumId;
    std::lock_guard lock(m_mutex);
    const auto found = m_songAlbums.find(songId);
    return found == m_songAlbums.end() ? stream::Id() : found->second;
}

std::optional<Song> Catalog::FindSongRaw(const stream::Id& id) const {
    auto track = m_records.FindTrack(id);
    if (!track) return std::nullopt;
    Song song;
    song.track = std::move(*track);
    song.coverUrl = m_records.CoverUrl(stream::ArtKind::Track, id);
    std::lock_guard lock(m_mutex);
    if (const auto extra = m_extras.find(id); extra != m_extras.end())
        static_cast<SongExtra&>(song) = extra->second;
    return song;
}

std::optional<Song> Catalog::FindSong(const stream::Id& id) const {
    auto song = FindSongRaw(id);
    if (song) DisplayMetadata::Get().Apply(*song, DisplayMetadata::Get().SourceStorefront());
    return song;
}

std::optional<stream::Playlist> Catalog::FindPlaylistRaw(const stream::Id& id) const {
    return m_records.FindPlaylist(id);
}

std::optional<stream::Playlist> Catalog::FindPlaylist(const stream::Id& id) const {
    auto playlist = FindPlaylistRaw(id);
    if (playlist) DisplayMetadata::Get().Apply(*playlist, DisplayMetadata::Get().SourceStorefront());
    return playlist;
}

std::optional<stream::Album> Catalog::FindAlbumRaw(const stream::Id& id) const {
    return m_records.FindAlbum(id);
}

std::optional<stream::Album> Catalog::FindAlbum(const stream::Id& id) const {
    auto album = FindAlbumRaw(id);
    if (album) DisplayMetadata::Get().Apply(*album, DisplayMetadata::Get().SourceStorefront());
    return album;
}

std::optional<stream::Artist> Catalog::FindArtistRaw(const stream::Id& id) const {
    return m_records.FindArtist(id);
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
    m_records.ErasePlaylist(id);
}

void Catalog::RemoveAlbum(const stream::Id& id) {
    m_records.EraseAlbum(id);
}

// The cover goes through the display overlay: another region's name for an album can bring its
// own picture.
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
    case stream::ArtKind::Station:
        if (const auto station = m_records.FindStation(id)) return station->coverUrl;
        break;
    }
    return {};
}

void Catalog::PutAndHint(const std::vector<Song>& songs, bool persist) {
    Put(songs);
    m_records.FlushHints(); // at once (main thread), with whatever else was waiting
    if (persist) m_records.QueueSave();
}

// ---- metadb tags and the restart cache -----------------------------------------------------

void Catalog::WriteTags(const stream::Track& stored, file_info& info) const {
    Song display;
    display.track = stored;
    display.coverUrl = m_records.CoverUrl(stream::ArtKind::Track, stored.id);
    {
        std::lock_guard lock(m_mutex);
        if (const auto extra = m_extras.find(stored.id); extra != m_extras.end())
            static_cast<SongExtra&>(display) = extra->second;
    }
    DisplayMetadata::Get().Apply(display, DisplayMetadata::Get().SourceStorefront());
    stream::WriteTrackTags(display.track, info);
    if (!display.composerNames.empty() || !display.composerName.empty())
        info.meta_set("composer", Utf8(display.composerNames.empty() ? display.composerName
                                                                     : JoinNames(display.composerNames))
                                      .c_str());
    if (!display.genreNames.empty()) info.meta_set("genre", Utf8(JoinNames(display.genreNames)).c_str());
}

std::wstring Catalog::EncodeExtra(const stream::Id& id) const {
    SongExtra extra;
    {
        std::lock_guard lock(m_mutex);
        const auto found = m_extras.find(id);
        if (found == m_extras.end()) return {};
        extra = found->second;
    }
    std::string text;
    const auto field = [&text](const std::string& value, bool last = false) {
        text += value;
        if (!last) text.push_back(kFieldSeparator);
    };
    field(Clean(extra.catalogId));
    field(Clean(extra.libraryId));
    field(Clean(extra.libraryAlbumId));
    field(Clean(extra.reportingId));
    field(Clean(extra.isrc));
    field(Clean(extra.contentRating));
    field(extra.customTitle ? "1" : "0");
    field(extra.customAlbum ? "1" : "0");
    field(Pack(extra.genreNames));
    field(Clean(extra.composerName));
    field(Pack(extra.composerNames), true);
    return Wide(text);
}

std::vector<Song> Catalog::ReadCacheFile() const {
    std::vector<Song> songs;
    for (auto& cached : m_records.ReadCache()) {
        const std::string extra = Utf8(cached.extra);
        const auto fields = Split(extra, kFieldSeparator);
        if (fields.size() != kExtraFields) continue;
        Song song;
        song.track = std::move(cached.track);
        song.coverUrl = std::move(cached.cover);
        song.catalogId = Wide(fields[0]);
        song.libraryId = Wide(fields[1]);
        song.libraryAlbumId = Wide(fields[2]);
        song.reportingId = Wide(fields[3]);
        song.isrc = Wide(fields[4]);
        song.contentRating = Wide(fields[5]);
        song.customTitle = fields[6] == "1";
        song.customAlbum = fields[7] == "1";
        song.genreNames = Unpack(fields[8]);
        song.composerName = Wide(fields[9]);
        song.composerNames = Unpack(fields[10]);
        if (song.libraryAlbumId.empty() && IsLibraryId(song.track.albumId) && song.track.albumId[0] == L'l')
            song.libraryAlbumId = song.track.albumId;
        songs.push_back(std::move(song));
    }
    return songs;
}

void Catalog::InstallCached(std::vector<Song> songs) {
    std::vector<stream::Id> installed;
    {
        std::lock_guard lock(m_mutex);
        std::vector<stream::CachedTrack> entries;
        std::unordered_map<stream::Id, const Song*> byId;
        entries.reserve(songs.size());
        for (auto& song : songs) {
            if (song.track.id.empty()) continue;
            if (!song.track.albumId.empty() && !song.libraryAlbumId.empty() &&
                IsLibraryId(song.libraryAlbumId) && song.libraryAlbumId[0] == L'l') {
                m_albumLibraryIds[song.track.albumId] = song.libraryAlbumId;
            }
            if (!song.track.albumId.empty()) m_songAlbums.try_emplace(song.track.id, song.track.albumId);
            ApplyMeasuredLocked(song);
            byId.emplace(song.track.id, &song);
            entries.push_back({song.track, song.coverUrl, {}});
        }
        installed = m_records.InstallMissing(std::move(entries));
        for (const auto& id : installed)
            m_extras[id] = *byId.at(id);
    }
    m_records.Hint(installed);
}

void Catalog::ClearMemory() {
    m_records.ClearMemory();
    std::lock_guard lock(m_mutex);
    m_extras.clear();
    m_songAlbums.clear();
}

void Catalog::Clear() {
    ClearMemory();
    m_records.Clear();
    {
        std::lock_guard lock(m_mutex);
        m_albumLibraryIds.clear();
        m_artistCatalogIds.clear();
        m_measured.clear();
        m_measuredLoaded = true;
    }
    if (const auto path = MeasuredFile(); !path.empty()) {
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }
}

} // namespace glasswave::applemusic
