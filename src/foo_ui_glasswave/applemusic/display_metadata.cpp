#include "pch.h"
#include "applemusic/display_metadata.h"
#include "core/text.h"
#include "applemusic/config.h"

#include <algorithm>
#include <iterator>
#include <limits>

namespace glasswave::applemusic {

std::optional<DisplayTarget> DisplayTargetFor(AppleMusicDisplayRegion region) {
    switch (region) {
    case AppleMusicDisplayRegion::Japan:
        return DisplayTarget{region, L"jp", L"ja-JP"};
    case AppleMusicDisplayRegion::China:
        return DisplayTarget{region, L"cn", L"zh-Hans-CN"};
    case AppleMusicDisplayRegion::Account:
        return std::nullopt;
    }
    return std::nullopt;
}

namespace {

constexpr auto kDisplayMetadataFreshness = std::chrono::minutes(15);

constexpr size_t kMaxSongEntries = 16'000;
constexpr size_t kMaxAlbumEntries = 8'000;
constexpr size_t kMaxArtistEntries = 8'000;
constexpr size_t kMaxPlaylistEntries = 4'000;
constexpr uint64_t kAnyEpoch = std::numeric_limits<uint64_t>::max();

void Part(std::string& key, std::string_view value) {
    key += std::to_string(value.size());
    key.push_back(':');
    key.append(value);
}

using core::Utf8;

std::wstring UpperAscii(std::wstring_view value) {
    std::wstring out(value);
    for (auto& c : out)
        if (c >= L'a' && c <= L'z') c = static_cast<wchar_t>(c + L'A' - L'a');
    return out;
}

void ApplyArtists(std::vector<stream::ArtistRef>& target, const std::vector<stream::ArtistRef>& names) {
    for (auto& artist : target) {
        const auto found =
            std::find_if(names.begin(), names.end(), [&artist](const stream::ArtistRef& candidate) {
                return !artist.id.empty() && candidate.id == artist.id;
            });
        if (found != names.end() && !found->name.empty()) artist.name = found->name;
    }
}

bool HasSongDisplayNames(const SongDisplayNames& names) {
    return !names.title.empty() || !names.album.empty() || !names.albumId.empty() || !names.coverUrl.empty() ||
           !names.artists.empty() ||
           !names.composerName.empty() || !names.composerNames.empty() || !names.genreNames.empty();
}

template <typename Entry> bool IsFresh(const Entry& entry) {
    const auto now = std::chrono::steady_clock::now();
    return entry.complete && now >= entry.validatedAt && now - entry.validatedAt <= kDisplayMetadataFreshness;
}

template <typename Entries> void TrimEntries(Entries& entries, size_t limit) {
    while (entries.size() > limit) {
        auto oldest = entries.begin();
        for (auto it = std::next(entries.begin()); it != entries.end(); ++it)
            if (it->second.used < oldest->second.used) oldest = it;
        entries.erase(oldest);
    }
}

std::optional<DisplayTarget> CurrentTarget() {
    return DisplayTargetFor(GetAppleMusicDisplayRegion());
}

} // namespace

DisplayMetadata& DisplayMetadata::Get() {
    static DisplayMetadata cache;
    return cache;
}

void DisplayMetadata::Clear() {
    std::lock_guard lock(m_mutex);
    ++m_epoch;
    m_songs.clear();
    m_albums.clear();
    m_artists.clear();
    m_playlists.clear();
}

void DisplayMetadata::BeginTransition() {
    std::lock_guard lock(m_mutex);
    ++m_epoch;
}

void DisplayMetadata::InvalidateFreshness() {
    std::lock_guard lock(m_mutex);
    for (auto& [key, entry] : m_songs) {
        (void)key;
        entry.validatedAt = {};
    }
    for (auto& [key, entry] : m_albums) {
        (void)key;
        entry.validatedAt = {};
    }
    for (auto& [key, entry] : m_artists) {
        (void)key;
        entry.validatedAt = {};
    }
    for (auto& [key, entry] : m_playlists) {
        (void)key;
        entry.validatedAt = {};
    }
}

uint64_t DisplayMetadata::Epoch() const {
    std::lock_guard lock(m_mutex);
    return m_epoch;
}

void DisplayMetadata::SetSourceStorefront(std::wstring storefront) {
    std::lock_guard lock(m_mutex);
    m_sourceStorefront = UpperAscii(storefront);
}

std::wstring DisplayMetadata::SourceStorefront() const {
    std::lock_guard lock(m_mutex);
    return m_sourceStorefront;
}

std::string DisplayMetadata::Key(const DisplayTarget& target, std::wstring_view kind,
                                 std::wstring_view sourceStorefront, const stream::Id& id) const {
    std::string key = "apple-display-v2;";
    Part(key, std::to_string(static_cast<int>(target.region)));
    Part(key, Utf8(kind));
    Part(key, Utf8(target.storefront));
    Part(key, Utf8(target.locale));
    Part(key, Utf8(UpperAscii(sourceStorefront)));
    Part(key, Utf8(id));
    return key;
}

bool DisplayMetadata::HasSong(std::wstring_view sourceStorefront, const stream::Id& id) const {
    return HasSong(GetAppleMusicDisplayRegion(), sourceStorefront, id);
}

bool DisplayMetadata::HasSong(AppleMusicDisplayRegion region, std::wstring_view sourceStorefront,
                              const stream::Id& id) const {
    if (region != GetAppleMusicDisplayRegion()) return false;
    const auto target = DisplayTargetFor(region);
    if (!target) return false;
    std::lock_guard lock(m_mutex);
    const auto found = m_songs.find(Key(*target, L"song", sourceStorefront, id));
    if (found == m_songs.end()) return false;
    found->second.used = ++m_clock;
    return true;
}

bool DisplayMetadata::HasAlbum(std::wstring_view sourceStorefront, const stream::Id& id) const {
    return HasAlbum(GetAppleMusicDisplayRegion(), sourceStorefront, id);
}

bool DisplayMetadata::HasAlbum(AppleMusicDisplayRegion region, std::wstring_view sourceStorefront,
                               const stream::Id& id) const {
    if (region != GetAppleMusicDisplayRegion()) return false;
    const auto target = DisplayTargetFor(region);
    if (!target) return false;
    std::lock_guard lock(m_mutex);
    const auto found = m_albums.find(Key(*target, L"album", sourceStorefront, id));
    if (found == m_albums.end()) return false;
    found->second.used = ++m_clock;
    return true;
}

bool DisplayMetadata::HasArtist(std::wstring_view sourceStorefront, const stream::Id& id) const {
    return HasArtist(GetAppleMusicDisplayRegion(), sourceStorefront, id);
}

bool DisplayMetadata::HasArtist(AppleMusicDisplayRegion region, std::wstring_view sourceStorefront,
                                const stream::Id& id) const {
    if (region != GetAppleMusicDisplayRegion()) return false;
    const auto target = DisplayTargetFor(region);
    if (!target) return false;
    std::lock_guard lock(m_mutex);
    const auto found = m_artists.find(Key(*target, L"artist", sourceStorefront, id));
    if (found == m_artists.end()) return false;
    found->second.used = ++m_clock;
    return true;
}

bool DisplayMetadata::HasPlaylist(std::wstring_view sourceStorefront, const stream::Id& id) const {
    return HasPlaylist(GetAppleMusicDisplayRegion(), sourceStorefront, id);
}

bool DisplayMetadata::HasPlaylist(AppleMusicDisplayRegion region, std::wstring_view sourceStorefront,
                                  const stream::Id& id) const {
    if (region != GetAppleMusicDisplayRegion()) return false;
    const auto target = DisplayTargetFor(region);
    if (!target) return false;
    std::lock_guard lock(m_mutex);
    const auto found = m_playlists.find(Key(*target, L"playlist", sourceStorefront, id));
    if (found == m_playlists.end()) return false;
    found->second.used = ++m_clock;
    return true;
}

bool DisplayMetadata::IsSongComplete(AppleMusicDisplayRegion region, std::wstring_view sourceStorefront,
                                     const stream::Id& id) const {
    if (region != GetAppleMusicDisplayRegion()) return false;
    const auto target = DisplayTargetFor(region);
    if (!target) return false;
    std::lock_guard lock(m_mutex);
    const auto found = m_songs.find(Key(*target, L"song", sourceStorefront, id));
    if (found == m_songs.end()) return false;
    found->second.used = ++m_clock;
    return IsFresh(found->second);
}

bool DisplayMetadata::IsAlbumComplete(AppleMusicDisplayRegion region, std::wstring_view sourceStorefront,
                                      const stream::Id& id) const {
    if (region != GetAppleMusicDisplayRegion()) return false;
    const auto target = DisplayTargetFor(region);
    if (!target) return false;
    std::lock_guard lock(m_mutex);
    const auto found = m_albums.find(Key(*target, L"album", sourceStorefront, id));
    if (found == m_albums.end()) return false;
    found->second.used = ++m_clock;
    return IsFresh(found->second);
}

bool DisplayMetadata::IsArtistComplete(AppleMusicDisplayRegion region, std::wstring_view sourceStorefront,
                                       const stream::Id& id) const {
    if (region != GetAppleMusicDisplayRegion()) return false;
    const auto target = DisplayTargetFor(region);
    if (!target) return false;
    std::lock_guard lock(m_mutex);
    const auto found = m_artists.find(Key(*target, L"artist", sourceStorefront, id));
    if (found == m_artists.end()) return false;
    found->second.used = ++m_clock;
    return IsFresh(found->second);
}

bool DisplayMetadata::IsPlaylistComplete(AppleMusicDisplayRegion region, std::wstring_view sourceStorefront,
                                         const stream::Id& id) const {
    if (region != GetAppleMusicDisplayRegion()) return false;
    const auto target = DisplayTargetFor(region);
    if (!target) return false;
    std::lock_guard lock(m_mutex);
    const auto found = m_playlists.find(Key(*target, L"playlist", sourceStorefront, id));
    if (found == m_playlists.end()) return false;
    found->second.used = ++m_clock;
    return IsFresh(found->second);
}

void DisplayMetadata::PutSong(std::wstring_view sourceStorefront, const stream::Id& id,
                              SongDisplayNames names) {
    PutSongFor(GetAppleMusicDisplayRegion(), sourceStorefront, id, std::move(names), true);
}

void DisplayMetadata::PutSongFor(AppleMusicDisplayRegion region, std::wstring_view sourceStorefront,
                                 const stream::Id& id, SongDisplayNames names) {
    PutSongFor(region, sourceStorefront, id, std::move(names), false);
}

void DisplayMetadata::PutSongFor(AppleMusicDisplayRegion region, std::wstring_view sourceStorefront,
                                 const stream::Id& id, SongDisplayNames names, bool complete) {
    PutSongFor(region, sourceStorefront, id, std::move(names), complete, kAnyEpoch);
}

void DisplayMetadata::PutSongFor(AppleMusicDisplayRegion region, std::wstring_view sourceStorefront,
                                 const stream::Id& id, SongDisplayNames names, bool complete,
                                 uint64_t expectedEpoch) {
    if (id.empty() || region == AppleMusicDisplayRegion::Account || region != GetAppleMusicDisplayRegion())
        return;
    const auto target = DisplayTargetFor(region);
    if (!target) return;
    std::lock_guard lock(m_mutex);
    if (expectedEpoch != kAnyEpoch && m_epoch != expectedEpoch) return;
    auto& entry = m_songs[Key(*target, L"song", sourceStorefront, id)];
    if (!names.title.empty()) entry.names.title = std::move(names.title);
    if (!names.album.empty()) entry.names.album = std::move(names.album);
    if (!names.albumId.empty()) entry.names.albumId = std::move(names.albumId);
    if (!names.coverUrl.empty()) entry.names.coverUrl = std::move(names.coverUrl);
    if (!names.artists.empty()) {
        entry.names.artists = std::move(names.artists);
        entry.names.artistsAuthoritative = names.artistsAuthoritative;
    }
    if (!names.composerName.empty()) entry.names.composerName = std::move(names.composerName);
    if (!names.composerNames.empty()) entry.names.composerNames = std::move(names.composerNames);
    if (!names.genreNames.empty()) entry.names.genreNames = std::move(names.genreNames);
    entry.complete = entry.complete || complete;
    if (complete) entry.validatedAt = std::chrono::steady_clock::now();
    entry.used = ++m_clock;
    TrimEntries(m_songs, kMaxSongEntries);
}

void DisplayMetadata::PutAlbum(std::wstring_view sourceStorefront, const stream::Id& id,
                               AlbumDisplayNames names) {
    PutAlbumFor(GetAppleMusicDisplayRegion(), sourceStorefront, id, std::move(names), true);
}

void DisplayMetadata::PutAlbumFor(AppleMusicDisplayRegion region, std::wstring_view sourceStorefront,
                                  const stream::Id& id, AlbumDisplayNames names) {
    PutAlbumFor(region, sourceStorefront, id, std::move(names), false);
}

void DisplayMetadata::PutAlbumFor(AppleMusicDisplayRegion region, std::wstring_view sourceStorefront,
                                  const stream::Id& id, AlbumDisplayNames names, bool complete) {
    PutAlbumFor(region, sourceStorefront, id, std::move(names), complete, kAnyEpoch);
}

void DisplayMetadata::PutAlbumFor(AppleMusicDisplayRegion region, std::wstring_view sourceStorefront,
                                  const stream::Id& id, AlbumDisplayNames names, bool complete,
                                  uint64_t expectedEpoch) {
    if (id.empty() || region == AppleMusicDisplayRegion::Account || region != GetAppleMusicDisplayRegion())
        return;
    const auto target = DisplayTargetFor(region);
    if (!target) return;
    std::lock_guard lock(m_mutex);
    if (expectedEpoch != kAnyEpoch && m_epoch != expectedEpoch) return;
    auto& entry = m_albums[Key(*target, L"album", sourceStorefront, id)];
    if (!names.name.empty()) entry.names.name = std::move(names.name);
    if (!names.description.empty()) entry.names.description = std::move(names.description);
    if (!names.coverUrl.empty()) entry.names.coverUrl = std::move(names.coverUrl);
    if (!names.artists.empty()) {
        entry.names.artists = std::move(names.artists);
        entry.names.artistsAuthoritative = names.artistsAuthoritative;
    }
    entry.complete = entry.complete || complete;
    if (complete) entry.validatedAt = std::chrono::steady_clock::now();
    entry.used = ++m_clock;
    TrimEntries(m_albums, kMaxAlbumEntries);
}

void DisplayMetadata::PutArtist(std::wstring_view sourceStorefront, const stream::Id& id,
                                ArtistDisplayNames names) {
    PutArtistFor(GetAppleMusicDisplayRegion(), sourceStorefront, id, std::move(names), true);
}

void DisplayMetadata::PutArtistFor(AppleMusicDisplayRegion region, std::wstring_view sourceStorefront,
                                   const stream::Id& id, ArtistDisplayNames names) {
    PutArtistFor(region, sourceStorefront, id, std::move(names), false);
}

void DisplayMetadata::PutArtistFor(AppleMusicDisplayRegion region, std::wstring_view sourceStorefront,
                                   const stream::Id& id, ArtistDisplayNames names, bool complete) {
    PutArtistFor(region, sourceStorefront, id, std::move(names), complete, kAnyEpoch);
}

void DisplayMetadata::PutArtistFor(AppleMusicDisplayRegion region, std::wstring_view sourceStorefront,
                                   const stream::Id& id, ArtistDisplayNames names, bool complete,
                                   uint64_t expectedEpoch) {
    if (id.empty() || region == AppleMusicDisplayRegion::Account || region != GetAppleMusicDisplayRegion())
        return;
    const auto target = DisplayTargetFor(region);
    if (!target) return;
    std::lock_guard lock(m_mutex);
    if (expectedEpoch != kAnyEpoch && m_epoch != expectedEpoch) return;
    auto& entry = m_artists[Key(*target, L"artist", sourceStorefront, id)];
    if (!names.name.empty()) entry.names.name = std::move(names.name);
    if (!names.bio.empty()) entry.names.bio = std::move(names.bio);
    if (!names.avatarUrl.empty()) entry.names.avatarUrl = std::move(names.avatarUrl);
    entry.complete = entry.complete || complete;
    if (complete) entry.validatedAt = std::chrono::steady_clock::now();
    entry.used = ++m_clock;
    TrimEntries(m_artists, kMaxArtistEntries);
}

void DisplayMetadata::PutPlaylist(std::wstring_view sourceStorefront, const stream::Id& id,
                                  PlaylistDisplayNames names) {
    PutPlaylistFor(GetAppleMusicDisplayRegion(), sourceStorefront, id, std::move(names), true);
}

void DisplayMetadata::PutPlaylistFor(AppleMusicDisplayRegion region, std::wstring_view sourceStorefront,
                                     const stream::Id& id, PlaylistDisplayNames names) {
    PutPlaylistFor(region, sourceStorefront, id, std::move(names), false);
}

void DisplayMetadata::PutPlaylistFor(AppleMusicDisplayRegion region, std::wstring_view sourceStorefront,
                                     const stream::Id& id, PlaylistDisplayNames names, bool complete) {
    PutPlaylistFor(region, sourceStorefront, id, std::move(names), complete, kAnyEpoch);
}

void DisplayMetadata::PutPlaylistFor(AppleMusicDisplayRegion region, std::wstring_view sourceStorefront,
                                     const stream::Id& id, PlaylistDisplayNames names, bool complete,
                                     uint64_t expectedEpoch) {
    if (id.empty() || region == AppleMusicDisplayRegion::Account || region != GetAppleMusicDisplayRegion())
        return;
    const auto target = DisplayTargetFor(region);
    if (!target) return;
    std::lock_guard lock(m_mutex);
    if (expectedEpoch != kAnyEpoch && m_epoch != expectedEpoch) return;
    auto& entry = m_playlists[Key(*target, L"playlist", sourceStorefront, id)];
    if (!names.name.empty()) entry.names.name = std::move(names.name);
    if (!names.description.empty()) entry.names.description = std::move(names.description);
    if (!names.coverUrl.empty()) entry.names.coverUrl = std::move(names.coverUrl);
    entry.complete = entry.complete || complete;
    if (complete) entry.validatedAt = std::chrono::steady_clock::now();
    entry.used = ++m_clock;
    TrimEntries(m_playlists, kMaxPlaylistEntries);
}

void DisplayMetadata::Apply(Song& song, std::wstring_view sourceStorefront) const {
    const auto target = CurrentTarget();
    if (!target) return;
    SongDisplayNames names;
    bool foundSong = false;
    {
        std::lock_guard lock(m_mutex);
        const auto found = m_songs.find(Key(*target, L"song", sourceStorefront, song.track.id));
        if (found != m_songs.end()) {
            found->second.used = ++m_clock;
            names = found->second.names;
            foundSong = true;
        }
    }
    if (!foundSong || !HasSongDisplayNames(names)) {
        Apply(song.track, sourceStorefront);
        return;
    }
    if (!song.customTitle && !names.title.empty()) song.track.title = names.title;
    if (!song.customAlbum) {
        if (!names.album.empty()) song.track.album = names.album;
        if (!names.albumId.empty()) song.track.albumId = names.albumId;
    }
    if (!names.coverUrl.empty()) song.coverUrl = names.coverUrl;
    if (names.artistsAuthoritative)
        song.track.artists = names.artists;
    else
        ApplyArtists(song.track.artists, names.artists);
    if (!names.composerNames.empty()) {
        song.composerNames = std::move(names.composerNames);
        song.composerName.clear();
    } else if (!names.composerName.empty()) {
        song.composerName = std::move(names.composerName);
    }
    if (!names.genreNames.empty()) song.genreNames = std::move(names.genreNames);
}

void DisplayMetadata::Apply(stream::Track& track, std::wstring_view sourceStorefront) const {
    const auto target = CurrentTarget();
    if (!target) return;
    SongDisplayNames names;
    bool foundSong = false;
    {
        std::lock_guard lock(m_mutex);
        const auto found = m_songs.find(Key(*target, L"song", sourceStorefront, track.id));
        if (found != m_songs.end()) {
            found->second.used = ++m_clock;
            names = found->second.names;
            foundSong = true;
        }
    }
    if (foundSong) {
        if (!names.title.empty()) track.title = names.title;
        if (!names.album.empty()) track.album = names.album;
        if (!names.albumId.empty()) track.albumId = names.albumId;
        if (names.artistsAuthoritative)
            track.artists = names.artists;
        else
            ApplyArtists(track.artists, names.artists);
    }
    if (!track.albumId.empty()) {
        stream::Album album;
        album.id = track.albumId;
        album.name = track.album;
        album.artists = track.artists;
        Apply(album, sourceStorefront);
        if (names.album.empty() && !album.name.empty()) track.album = album.name;
        if (names.artists.empty()) ApplyArtists(track.artists, album.artists);
    }
    for (auto& artist : track.artists) {
        const bool songNamed = std::any_of(
            names.artists.begin(), names.artists.end(), [&artist](const stream::ArtistRef& candidate) {
                return !artist.id.empty() && candidate.id == artist.id && !candidate.name.empty();
            });
        if (songNamed) continue;
        stream::Artist display;
        display.id = artist.id;
        display.name = artist.name;
        Apply(display, sourceStorefront);
        if (!display.name.empty()) artist.name = display.name;
    }
}

void DisplayMetadata::Apply(stream::Album& album, std::wstring_view sourceStorefront) const {
    const auto target = CurrentTarget();
    if (!target) return;
    AlbumDisplayNames names;
    {
        std::lock_guard lock(m_mutex);
        const auto found = m_albums.find(Key(*target, L"album", sourceStorefront, album.id));
        if (found == m_albums.end()) return;
        found->second.used = ++m_clock;
        names = found->second.names;
    }
    if (!names.name.empty()) album.name = names.name;
    if (!names.description.empty()) album.description = names.description;
    if (!names.coverUrl.empty()) album.coverUrl = names.coverUrl;
    if (names.artistsAuthoritative)
        album.artists = names.artists;
    else
        ApplyArtists(album.artists, names.artists);
}

void DisplayMetadata::Apply(stream::Artist& artist, std::wstring_view sourceStorefront) const {
    const auto target = CurrentTarget();
    if (!target) return;
    ArtistDisplayNames names;
    {
        std::lock_guard lock(m_mutex);
        const auto found = m_artists.find(Key(*target, L"artist", sourceStorefront, artist.id));
        if (found == m_artists.end()) return;
        found->second.used = ++m_clock;
        names = found->second.names;
    }
    if (!names.name.empty()) artist.name = names.name;
    if (!names.bio.empty()) artist.bio = names.bio;
    if (!names.avatarUrl.empty()) artist.avatarUrl = names.avatarUrl;
}

void DisplayMetadata::Apply(stream::Playlist& playlist, std::wstring_view sourceStorefront) const {
    const auto target = CurrentTarget();
    if (!target || playlist.own) return;
    PlaylistDisplayNames names;
    {
        std::lock_guard lock(m_mutex);
        const auto found = m_playlists.find(Key(*target, L"playlist", sourceStorefront, playlist.id));
        if (found == m_playlists.end()) return;
        found->second.used = ++m_clock;
        names = found->second.names;
    }
    if (!names.name.empty()) playlist.name = names.name;
    if (!names.description.empty()) playlist.description = names.description;
    if (!names.coverUrl.empty()) playlist.coverUrl = names.coverUrl;
}

} // namespace glasswave::applemusic
