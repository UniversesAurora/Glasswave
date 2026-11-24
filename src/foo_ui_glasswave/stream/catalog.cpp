#include "pch.h"
#include "core/tsv.h"
#include "stream/catalog.h"

#include "core/file.h"
#include "core/paths.h"
#include "core/text.h"
#include "stream/track_snapshot.h"

#include <SDK/file_info_impl.h>

#include <algorithm>
#include <cstdlib>
#include <unordered_set>

namespace glasswave::stream {

namespace {

using core::Utf8;
using core::Wide;

constexpr size_t kCacheFileLimit = 64u * 1024u * 1024u;
constexpr size_t kCacheLineLimit = 200'000;
constexpr size_t kCacheFields = 12;
constexpr char kListSeparator = '\x1f';
// A song whose read failed or is still running is asked for again after this.
constexpr auto kAskAgain = std::chrono::minutes(2);

std::string Clean(std::wstring_view text) {
    std::wstring copy(text);
    for (auto& c : copy)
        if (c == L'\t' || c == L'\r' || c == L'\n') c = L' ';
    return Utf8(copy);
}

template <class T, class Text> std::string Pack(const std::vector<T>& values, Text text) {
    std::string out;
    for (size_t i = 0; i < values.size(); ++i) {
        if (i) out.push_back(kListSeparator);
        out += Clean(text(values[i]));
    }
    return out;
}

int ToInt(std::string_view text) {
    return static_cast<int>(_strtoi64(std::string(text).c_str(), nullptr, 10));
}

const char* KindName(ArtKind kind) {
    switch (kind) {
    case ArtKind::Album:
        return "album";
    case ArtKind::Artist:
        return "artist";
    case ArtKind::Playlist:
        return "playlist";
    case ArtKind::Station:
        return "station";
    case ArtKind::Track:
        break;
    }
    return "song";
}

constexpr ArtKind kKinds[] = {ArtKind::Track, ArtKind::Album, ArtKind::Artist, ArtKind::Playlist,
                              ArtKind::Station};

bool Unnamed(const std::wstring& name) {
    return name.empty() || name == L"Unknown";
}

// A name the service did not know is "Unknown": any real name replaces it.
std::wstring PreferName(const std::wstring& primary, const std::wstring& fallback) {
    return Unnamed(primary) && !Unnamed(fallback) ? fallback : primary;
}

std::wstring PreferText(const std::wstring& primary, const std::wstring& fallback) {
    return primary.empty() ? fallback : primary;
}

int PreferCount(int primary, int fallback) {
    return primary == kUnknownCount ? fallback : primary;
}

bool EqualAscii(std::wstring_view a, std::wstring_view b) {
    return core::LowerAscii(a) == core::LowerAscii(b);
}

void Append(std::vector<std::wstring>& into, const std::vector<std::wstring>& from) {
    for (const auto& value : from)
        if (std::find(into.begin(), into.end(), value) == into.end()) into.push_back(value);
}

// What Hint writes to metadb: a song that differs here is shown differently.
bool SameTags(const Track& a, const Track& b) {
    if (a.title != b.title || a.album != b.album || a.durationMs != b.durationMs ||
        a.artists.size() != b.artists.size())
        return false;
    for (size_t i = 0; i < a.artists.size(); ++i)
        if (a.artists[i].name != b.artists[i].name) return false;
    return true;
}

} // namespace

void WriteTrackTags(const Track& track, file_info& info) {
    const auto set = [&info](const char* name, const std::wstring& value) {
        if (!value.empty()) info.meta_set(name, Utf8(value).c_str());
    };
    set("title", track.title);
    set("artist", JoinArtists(track.artists));
    set("album", track.album);
    if (!track.artists.empty()) set("album artist", track.artists.front().name);
    if (track.durationMs > 0) info.set_length(static_cast<double>(track.durationMs) / 1000.0);
}

std::vector<ArtistRef> MergeArtistRefs(std::vector<ArtistRef> primary,
                                       const std::vector<ArtistRef>& fallback) {
    for (const auto& candidate : fallback) {
        auto found = std::find_if(primary.begin(), primary.end(), [&candidate](const ArtistRef& item) {
            if (!candidate.id.empty() && item.id == candidate.id) return true;
            return !candidate.name.empty() && !item.name.empty() &&
                   (candidate.name == item.name || EqualAscii(candidate.name, item.name));
        });
        if (found == primary.end()) {
            if (!candidate.id.empty() || !candidate.name.empty()) primary.push_back(candidate);
            continue;
        }
        if (found->id.empty()) found->id = candidate.id;
        if (found->name.empty()) found->name = candidate.name;
    }
    return primary;
}

Track MergeTrack(const Track& primary, const Track& fallback) {
    Track merged = primary;
    if (merged.id.empty()) merged.id = fallback.id;
    merged.title = PreferName(merged.title, fallback.title);
    merged.note = PreferText(merged.note, fallback.note);
    merged.album = PreferName(merged.album, fallback.album);
    merged.albumId = PreferText(merged.albumId, fallback.albumId);
    merged.artists = MergeArtistRefs(std::move(merged.artists), fallback.artists);
    if (merged.durationMs <= 0) merged.durationMs = fallback.durationMs;
    if (merged.popularity == 0) merged.popularity = fallback.popularity;
    Append(merged.badges, fallback.badges);
    return merged;
}

Playlist MergePlaylist(const Playlist& primary, const Playlist& fallback, bool mergeFlags) {
    Playlist merged = primary;
    if (merged.id.empty()) merged.id = fallback.id;
    merged.name = PreferName(merged.name, fallback.name);
    merged.coverUrl = PreferText(merged.coverUrl, fallback.coverUrl);
    merged.creator = PreferText(merged.creator, fallback.creator);
    merged.description = PreferText(merged.description, fallback.description);
    merged.trackCount = PreferCount(merged.trackCount, fallback.trackCount);
    merged.tagline = PreferText(merged.tagline, fallback.tagline);
    merged.type = PreferText(merged.type, fallback.type);
    merged.created = PreferText(merged.created, fallback.created);
    merged.updated = PreferText(merged.updated, fallback.updated);
    merged.updates = PreferText(merged.updates, fallback.updates);
    if (merged.tags.empty()) merged.tags = fallback.tags;
    if (merged.aliases.empty()) merged.aliases = fallback.aliases;
    if (merged.traits.empty()) merged.traits = fallback.traits;
    if (mergeFlags) {
        merged.own = merged.own || fallback.own;
        merged.saved = merged.saved || fallback.saved;
    }
    return merged;
}

Album MergeAlbum(const Album& primary, const Album& fallback) {
    Album merged = primary;
    if (merged.id.empty()) merged.id = fallback.id;
    merged.name = PreferName(merged.name, fallback.name);
    merged.coverUrl = PreferText(merged.coverUrl, fallback.coverUrl);
    merged.description = PreferText(merged.description, fallback.description);
    merged.artists = MergeArtistRefs(std::move(merged.artists), fallback.artists);
    merged.trackCount = PreferCount(merged.trackCount, fallback.trackCount);
    if (merged.year == 0) merged.year = fallback.year;
    merged.released = PreferText(merged.released, fallback.released);
    merged.label = PreferText(merged.label, fallback.label);
    merged.copyright = PreferText(merged.copyright, fallback.copyright);
    merged.type = PreferText(merged.type, fallback.type);
    merged.edition = PreferText(merged.edition, fallback.edition);
    merged.upc = PreferText(merged.upc, fallback.upc);
    if (merged.genres.empty()) merged.genres = fallback.genres;
    if (merged.aliases.empty()) merged.aliases = fallback.aliases;
    if (merged.traits.empty()) merged.traits = fallback.traits;
    return merged;
}

Artist MergeArtist(const Artist& primary, const Artist& fallback, bool mergeFollowState) {
    Artist merged = primary;
    if (merged.id.empty()) merged.id = fallback.id;
    merged.name = PreferName(merged.name, fallback.name);
    merged.avatarUrl = PreferText(merged.avatarUrl, fallback.avatarUrl);
    merged.albumCount = PreferCount(merged.albumCount, fallback.albumCount);
    merged.trackCount = PreferCount(merged.trackCount, fallback.trackCount);
    if (fallback.profile && !primary.profile) {
        CopyArtistProfile(fallback, merged); // a list row over the artist's own record
    } else {
        merged.bio = PreferText(merged.bio, fallback.bio);
    }
    if (mergeFollowState || !merged.followKnown) {
        if (fallback.followKnown) {
            merged.followKnown = true;
            merged.followed = fallback.followed;
        }
    }
    return merged;
}

bool LacksLinks(const Track& track) {
    return track.albumId.empty() || track.artists.empty() ||
           std::any_of(track.artists.begin(), track.artists.end(),
                       [](const ArtistRef& artist) { return artist.id.empty(); });
}

Catalog::Catalog(CatalogTraits traits) : m_traits(std::move(traits)) {}

// ---- records -------------------------------------------------------------------------------

// Every song that is new or shown differently goes to metadb as well, whoever put it: a song
// known only here kept, in every list and across restarts, the stand-in title an input gave it
// before (owner, 2026-11-21: "Apple Music 1888098957").
bool Catalog::StoreLocked(const TrackRecord& song) {
    const auto found = m_tracks.find(song.track.id);
    const bool shown = found != m_tracks.end() && !song.retag && SameTags(found->second.track, song.track);
    m_tracks[song.track.id] = {song.track, song.cover};
    if (shown) return false;
    m_hintQueue.push_back(song.track.id);
    return !std::exchange(m_hintPosted, true);
}

void Catalog::PostHints() {
    fb2k::inMainThread([this] { FlushHints(); });
}

void Catalog::FlushHints() {
    std::vector<Track> songs;
    {
        std::lock_guard lock(m_mutex);
        m_hintPosted = false;
        std::unordered_set<Id> seen;
        for (const auto& id : m_hintQueue)
            if (seen.insert(id).second)
                if (const auto found = m_tracks.find(id); found != m_tracks.end())
                    songs.push_back(found->second.track);
        m_hintQueue.clear();
    }
    Hint(songs);
}

void Catalog::Put(const TrackRecord& song) {
    if (song.track.id.empty()) return;
    bool post = false;
    {
        std::lock_guard lock(m_mutex);
        post = StoreLocked(song);
    }
    if (post) PostHints();
}

void Catalog::Put(const std::vector<TrackRecord>& songs) {
    bool post = false;
    {
        std::lock_guard lock(m_mutex);
        for (const auto& song : songs)
            if (!song.track.id.empty()) post = StoreLocked(song) || post;
    }
    if (post) PostHints();
}

void Catalog::Put(const Album& album) {
    if (album.id.empty()) return;
    std::lock_guard lock(m_mutex);
    m_albums[album.id] = album;
}

void Catalog::Put(const Artist& artist) {
    if (artist.id.empty()) return;
    std::lock_guard lock(m_mutex);
    m_artists[artist.id] = artist;
}

void Catalog::Put(const Playlist& playlist) {
    if (playlist.id.empty()) return;
    std::lock_guard lock(m_mutex);
    m_playlists[playlist.id] = playlist;
}

void Catalog::Put(const Station& station) {
    if (station.id.empty()) return;
    std::lock_guard lock(m_mutex);
    m_stations[station.id] = station;
}

void Catalog::Put(const std::vector<Album>& albums) {
    for (const auto& album : albums)
        Put(album);
}

void Catalog::Put(const std::vector<Artist>& artists) {
    for (const auto& artist : artists)
        Put(artist);
}

void Catalog::Put(const std::vector<Playlist>& playlists) {
    for (const auto& playlist : playlists)
        Put(playlist);
}

void Catalog::Restore(const TrackRecord& song) {
    if (song.track.id.empty()) return;
    bool post = false;
    {
        std::lock_guard lock(m_mutex);
        const auto found = m_tracks.find(song.track.id);
        if (found == m_tracks.end()) {
            post = StoreLocked(song);
        } else {
            TrackRecord merged{MergeTrack(found->second.track, song.track),
                               PreferText(found->second.cover, song.cover), song.retag};
            post = StoreLocked(merged);
        }
    }
    if (post) PostHints();
}

void Catalog::Restore(const Album& album) {
    if (album.id.empty()) return;
    std::lock_guard lock(m_mutex);
    const auto [it, inserted] = m_albums.try_emplace(album.id, album);
    if (!inserted) it->second = MergeAlbum(it->second, album);
}

void Catalog::Restore(const Artist& artist) {
    if (artist.id.empty()) return;
    std::lock_guard lock(m_mutex);
    const auto [it, inserted] = m_artists.try_emplace(artist.id, artist);
    if (!inserted) it->second = MergeArtist(it->second, artist, true);
}

void Catalog::Restore(const Playlist& playlist) {
    if (playlist.id.empty()) return;
    std::lock_guard lock(m_mutex);
    const auto [it, inserted] = m_playlists.try_emplace(playlist.id, playlist);
    if (!inserted) it->second = MergePlaylist(it->second, playlist, true);
}

void Catalog::RestoreAndHint(const std::vector<TrackRecord>& songs) {
    for (const auto& song : songs)
        Restore(song);
    FlushHints();
}

std::optional<Track> Catalog::FindTrack(const Id& id) const {
    std::lock_guard lock(m_mutex);
    const auto it = m_tracks.find(id);
    return it == m_tracks.end() ? std::nullopt : std::optional<Track>(it->second.track);
}

std::optional<Album> Catalog::FindAlbum(const Id& id) const {
    std::lock_guard lock(m_mutex);
    const auto it = m_albums.find(id);
    return it == m_albums.end() ? std::nullopt : std::optional<Album>(it->second);
}

std::optional<Artist> Catalog::FindArtist(const Id& id) const {
    std::lock_guard lock(m_mutex);
    const auto it = m_artists.find(id);
    return it == m_artists.end() ? std::nullopt : std::optional<Artist>(it->second);
}

std::optional<Playlist> Catalog::FindPlaylist(const Id& id) const {
    std::lock_guard lock(m_mutex);
    const auto it = m_playlists.find(id);
    return it == m_playlists.end() ? std::nullopt : std::optional<Playlist>(it->second);
}

std::optional<Station> Catalog::FindStation(const Id& id) const {
    std::lock_guard lock(m_mutex);
    const auto it = m_stations.find(id);
    return it == m_stations.end() ? std::nullopt : std::optional<Station>(it->second);
}

void Catalog::ErasePlaylist(const Id& id) {
    if (id.empty()) return;
    std::lock_guard lock(m_mutex);
    m_playlists.erase(id);
}

void Catalog::EraseAlbum(const Id& id) {
    if (id.empty()) return;
    std::lock_guard lock(m_mutex);
    m_albums.erase(id);
}

std::wstring Catalog::PreferredKey(ArtKind kind, const Id& id) {
    return std::wstring(1, static_cast<wchar_t>(L'0' + static_cast<int>(kind))) + id;
}

std::wstring Catalog::CoverUrl(ArtKind kind, const Id& id) const {
    std::lock_guard lock(m_mutex);
    if (const auto preferred = m_preferred.find(PreferredKey(kind, id));
        preferred != m_preferred.end() && !preferred->second.empty())
        return preferred->second;
    switch (kind) {
    case ArtKind::Track:
        if (const auto found = m_tracks.find(id); found != m_tracks.end()) return found->second.cover;
        break;
    case ArtKind::Album:
        if (const auto found = m_albums.find(id); found != m_albums.end()) return found->second.coverUrl;
        break;
    case ArtKind::Artist:
        if (const auto found = m_artists.find(id); found != m_artists.end()) return found->second.avatarUrl;
        break;
    case ArtKind::Playlist:
        if (const auto found = m_playlists.find(id); found != m_playlists.end())
            return found->second.coverUrl;
        break;
    case ArtKind::Station:
        if (const auto found = m_stations.find(id); found != m_stations.end()) return found->second.coverUrl;
        break;
    }
    return {};
}

void Catalog::PreferCover(ArtKind kind, const Id& id, std::wstring coverUrl) {
    if (id.empty() || coverUrl.empty()) return;
    std::lock_guard lock(m_mutex);
    m_preferred[PreferredKey(kind, id)] = std::move(coverUrl);
}

void Catalog::ClearPreferredCovers() {
    std::lock_guard lock(m_mutex);
    m_preferred.clear();
}

// ---- handles and paths ---------------------------------------------------------------------

std::string Catalog::SongPath(const Id& song) const {
    const std::string segment = m_traits.encodeId(song);
    return segment.empty() ? std::string() : m_traits.scheme + "://song/" + segment;
}

bool Catalog::ParseSongPath(const char* path, Id& song) const {
    if (!path) return false;
    const std::string prefix = m_traits.scheme + "://song/";
    const std::string_view text(path);
    if (text.size() <= prefix.size() || text.substr(0, prefix.size()) != prefix) return false;
    Id parsed;
    if (!m_traits.decodeId(text.substr(prefix.size()), parsed)) return false;
    song = std::move(parsed);
    return true;
}

std::string Catalog::CoverPath(ArtKind kind, const Id& id) const {
    const std::string segment = m_traits.encodeId(id);
    return segment.empty() ? std::string() : m_traits.scheme + "://cover/" + KindName(kind) + "/" + segment;
}

bool Catalog::ParseCoverPath(const char* path, ArtKind& kind, Id& id) const {
    if (!path) return false;
    const std::string_view text(path);
    for (const ArtKind candidate : kKinds) {
        const std::string prefix = m_traits.scheme + "://cover/" + KindName(candidate) + "/";
        if (text.size() <= prefix.size() || text.substr(0, prefix.size()) != prefix) continue;
        Id parsed;
        if (!m_traits.decodeId(text.substr(prefix.size()), parsed)) return false;
        kind = candidate;
        id = std::move(parsed);
        return true;
    }
    return false;
}

metadb_handle_ptr Catalog::Handle(const Id& song) const {
    const std::string path = SongPath(song);
    return path.empty() ? metadb_handle_ptr() : metadb::get()->handle_create(path.c_str(), 0);
}

metadb_handle_ptr Catalog::CoverHandle(ArtKind kind, const Id& id) const {
    const std::string path = CoverPath(kind, id);
    return path.empty() ? metadb_handle_ptr() : metadb::get()->handle_create(path.c_str(), 0);
}

// ---- metadb --------------------------------------------------------------------------------

void Catalog::WriteTags(const Track& track, file_info& info) const {
    if (m_traits.tags)
        m_traits.tags(track, info);
    else
        WriteTrackTags(track, info);
}

void Catalog::PutAndHint(const std::vector<TrackRecord>& songs, bool persist) {
    Put(songs);
    FlushHints(); // at once (main thread), with whatever else was waiting
    if (persist) QueueSave();
}

void Catalog::Hint(const std::vector<Track>& tracks) const {
    if (tracks.empty()) return;
    auto hints = metadb_io_v2::get()->create_hint_list();
    if (!hints.is_valid()) return;
    for (const auto& track : tracks) {
        if (track.id.empty()) continue;
        file_info_impl info;
        WriteTags(track, info);
        // Tags only: the technical info playback measured (bit rate, codec...) stays.
        HintTags(hints, Handle(track.id), info);
    }
    hints->on_done();
}

void Catalog::Hint(const std::vector<Id>& ids) const {
    std::vector<Track> tracks;
    {
        std::lock_guard lock(m_mutex);
        for (const auto& id : ids)
            if (const auto found = m_tracks.find(id); found != m_tracks.end())
                tracks.push_back(found->second.track);
    }
    Hint(tracks);
}

void Catalog::HintAll() const {
    std::vector<Track> tracks;
    {
        std::lock_guard lock(m_mutex);
        tracks.reserve(m_tracks.size());
        for (const auto& [id, entry] : m_tracks)
            tracks.push_back(entry.track);
    }
    Hint(tracks);
}

// ---- restart cache -------------------------------------------------------------------------

std::filesystem::path Catalog::CacheFile() const {
    const auto dir = core::ProfileDir(m_traits.dir);
    return dir.empty() ? std::filesystem::path() : dir / L"cache" / L"songs.tsv";
}

std::vector<CachedTrack> Catalog::ReadCache() const {
    const auto path = CacheFile();
    if (path.empty()) return {};
    const auto bytes = core::ReadFileLimited(path, kCacheFileLimit);
    if (!bytes) return {};

    std::vector<CachedTrack> loaded;
    size_t lines = 0;
    bool headerSeen = false;
    for (const auto raw : core::tsv::Split(*bytes, '\n')) {
        std::string_view line = raw;
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (++lines > kCacheLineLimit) return {};
        if (!headerSeen) {
            if (line.empty()) continue;
            if (line != m_traits.cacheHeader) return {};
            headerSeen = true;
            continue;
        }
        if (line.empty() || line[0] == '#') continue;
        const auto fields = core::tsv::Split(line, '\t');
        if (fields.size() != kCacheFields) continue; // one current format; other lines are dropped

        CachedTrack song;
        Track& track = song.track;
        track.id = Wide(fields[0]);
        if (track.id.empty()) continue;
        track.durationMs = ToInt(fields[1]);
        track.popularity = ToInt(fields[2]);
        track.title = Wide(fields[3]);
        track.note = Wide(fields[4]);
        track.album = Wide(fields[5]);
        track.albumId = Wide(fields[6]);
        const auto names = core::tsv::Split(fields[7], kListSeparator);
        const auto ids = core::tsv::Split(fields[8], kListSeparator);
        for (size_t i = 0; i < names.size(); ++i) {
            const std::wstring name = Wide(names[i]);
            if (name.empty()) continue;
            track.artists.push_back({i < ids.size() ? Wide(ids[i]) : Id(), name});
        }
        for (const auto badge : core::tsv::Split(fields[9], kListSeparator))
            if (!badge.empty()) track.badges.push_back(Wide(badge));
        song.cover = Wide(fields[10]);
        song.extra = Wide(fields[11]);
        loaded.push_back(std::move(song));
    }
    return headerSeen ? loaded : std::vector<CachedTrack>{};
}

std::vector<Id> Catalog::InstallMissing(std::vector<CachedTrack> songs) {
    std::vector<Id> installed;
    std::lock_guard lock(m_mutex);
    for (auto& song : songs) {
        if (song.track.id.empty() || m_tracks.contains(song.track.id)) continue;
        installed.push_back(song.track.id);
        m_tracks.emplace(song.track.id, TrackEntry{std::move(song.track), std::move(song.cover)});
    }
    return installed;
}

void Catalog::SaveLocked() const {
    const auto path = CacheFile();
    if (path.empty()) return;
    std::vector<TrackEntry> songs;
    {
        std::lock_guard lock(m_mutex);
        songs.reserve(m_tracks.size());
        for (const auto& [id, entry] : m_tracks)
            songs.push_back(entry);
    }
    if (songs.empty()) return;
    std::sort(songs.begin(), songs.end(),
              [](const TrackEntry& a, const TrackEntry& b) { return a.track.id < b.track.id; });

    std::string out = m_traits.cacheHeader;
    out.push_back('\n');
    for (const auto& song : songs) {
        const Track& track = song.track;
        out += Clean(track.id) + '\t' + std::to_string(track.durationMs) + '\t' +
               std::to_string(track.popularity) + '\t' + Clean(track.title) + '\t' + Clean(track.note) +
               '\t' + Clean(track.album) + '\t' + Clean(track.albumId) + '\t' +
               Pack(track.artists, [](const ArtistRef& artist) { return artist.name; }) + '\t' +
               Pack(track.artists, [](const ArtistRef& artist) { return artist.id; }) + '\t' +
               Pack(track.badges, [](const std::wstring& badge) { return badge; }) + '\t' +
               Clean(song.cover) + '\t' +
               Clean(m_traits.encodeExtra ? m_traits.encodeExtra(track.id) : std::wstring()) + '\n';
    }
    try {
        core::WriteFileAtomic(path, out);
    } catch (const std::exception&) {
        // disk full or similar: the previous cache stays
    }
}

void Catalog::QueueSave() {
    m_saveDirty = true;
    if (m_savePosted.exchange(true)) return;
    // Every page of a long playlist puts songs; the whole file is written once, off the main
    // thread, after the burst.
    fb2k::callLater(2.0, [this] {
        m_savePosted = false;
        if (m_saveClosed) return;
        fb2k::splitTask([this] {
            std::lock_guard saving(m_saveMutex);
            if (!m_saveClosed && m_saveDirty.exchange(false)) SaveLocked();
        });
    });
}

void Catalog::FlushPendingSave() {
    // Waits for a write in flight (it holds m_saveMutex), writes what is still pending, and lets
    // no deferred write start after it: the provider is going away.
    std::lock_guard saving(m_saveMutex);
    m_saveClosed = true;
    if (m_saveDirty.exchange(false)) SaveLocked();
}

// ---- reading from the service --------------------------------------------------------------

void Catalog::Want(const Id& song) {
    if (song.empty()) return;
    const auto now = std::chrono::steady_clock::now();
    {
        std::lock_guard lock(m_readMutex);
        if (m_settled.contains(song)) return;
        if (const auto asked = m_asked.find(song); asked != m_asked.end() && now - asked->second < kAskAgain)
            return;
        m_asked[song] = now;
        m_wanted.push_back(song);
        // Gathered until the main thread is free: a list asks for its rows one by one in a paint.
        if (!m_reader || std::exchange(m_wantedPosted, true)) return;
    }
    fb2k::inMainThread([this] { FlushWanted(); });
}

void Catalog::FlushWanted() {
    Reader read;
    std::vector<Id> wanted;
    {
        std::lock_guard lock(m_readMutex);
        m_wantedPosted = false;
        if (!m_reader) return; // the asks wait for the next reader
        read = m_reader;
        wanted.swap(m_wanted);
    }
    // A list or a page's enrichment may have brought a song in meanwhile.
    std::vector<Id> batch;
    std::vector<Id> checkProvider;
    {
        std::lock_guard lock(m_mutex);
        std::unordered_set<Id> seen;
        for (auto& id : wanted) {
            if (!seen.insert(id).second) continue;
            const auto found = m_tracks.find(id);
            if (found == m_tracks.end() || LacksLinks(found->second.track))
                batch.push_back(std::move(id));
            else if (m_traits.needsRead)
                checkProvider.push_back(std::move(id));
        }
    }
    if (m_traits.needsRead) {
        for (auto& id : checkProvider)
            if (m_traits.needsRead(id)) batch.push_back(std::move(id));
    }
    if (!batch.empty()) read(std::move(batch));
}

void Catalog::Rewant(const Id& song) {
    if (song.empty()) return;
    {
        std::lock_guard lock(m_readMutex);
        m_settled.erase(song);
        m_asked.erase(song);
    }
    Want(song);
}

void Catalog::ClearSettled() {
    std::lock_guard lock(m_readMutex);
    m_settled.clear();
    m_asked.clear();
}

void Catalog::Settle(const std::vector<Id>& songs) {
    std::lock_guard lock(m_readMutex);
    for (const auto& id : songs) {
        m_settled.insert(id);
        m_asked.erase(id);
    }
}

void Catalog::StartReading(Reader read) {
    {
        std::lock_guard lock(m_readMutex);
        m_reader = std::move(read);
        if (m_wanted.empty() || std::exchange(m_wantedPosted, true)) return;
    }
    fb2k::inMainThread([this] { FlushWanted(); });
}

void Catalog::StopReading() {
    std::lock_guard lock(m_readMutex);
    m_reader = nullptr;
}

void Catalog::ForgetReads() {
    std::lock_guard lock(m_readMutex);
    m_wanted.clear();
    m_asked.clear();
    m_settled.clear();
}

// ---- forgetting ----------------------------------------------------------------------------

void Catalog::ClearMemory() {
    {
        std::lock_guard lock(m_mutex);
        m_tracks.clear();
        m_albums.clear();
        m_artists.clear();
        m_playlists.clear();
    }
    ForgetReads(); // what was read is gone with the records
}

void Catalog::Clear() {
    ClearMemory();
    {
        std::lock_guard lock(m_mutex);
        m_stations.clear();
        m_preferred.clear();
    }
    m_saveDirty = false;
    const auto path = CacheFile();
    if (path.empty()) return;
    std::lock_guard saving(m_saveMutex);
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

} // namespace glasswave::stream
