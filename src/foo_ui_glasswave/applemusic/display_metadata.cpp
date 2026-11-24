#include "pch.h"
#include "applemusic/display_metadata.h"
#include "applemusic/config.h"
#include "core/file.h"
#include "core/paths.h"
#include "core/text.h"
#include "core/tsv.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
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

constexpr int64_t kDisplayMetadataFreshnessSec = 7ll * 24 * 3600;
constexpr std::string_view kCacheHeader = "# glasswave apple music display v1";
constexpr size_t kCacheFileLimit = 32u * 1024u * 1024u;
constexpr size_t kCacheLineLimit = 64'000;
constexpr char kListSep = '\x1f';

constexpr size_t kMaxSongEntries = 16'000;
constexpr size_t kMaxAlbumEntries = 8'000;
constexpr size_t kMaxArtistEntries = 8'000;
constexpr size_t kMaxPlaylistEntries = 4'000;
constexpr uint64_t kAnyEpoch = std::numeric_limits<uint64_t>::max();

using core::Utf8;
using core::Wide;

int64_t NowEpochSec() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

void Part(std::string& key, std::string_view value) {
    key += std::to_string(value.size());
    key.push_back(':');
    key.append(value);
}

std::wstring UpperAscii(std::wstring_view value) {
    std::wstring out(value);
    for (auto& c : out)
        if (c >= L'a' && c <= L'z') c = static_cast<wchar_t>(c + L'A' - L'a');
    return out;
}

std::string EscapeField(std::wstring_view text) {
    const std::string utf8 = Utf8(text);
    std::string out;
    out.reserve(utf8.size());
    for (char c : utf8) {
        switch (c) {
        case '\\': out += "\\\\"; break;
        case '\t': out += "\\t"; break;
        case '\r': out += "\\r"; break;
        case '\n': out += "\\n"; break;
        default: out.push_back(c); break;
        }
    }
    return out;
}

std::wstring UnescapeField(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\\' && i + 1 < text.size()) {
            switch (text[++i]) {
            case '\\': out.push_back('\\'); break;
            case 't': out.push_back('\t'); break;
            case 'r': out.push_back('\r'); break;
            case 'n': out.push_back('\n'); break;
            default: out.push_back(text[i]); break;
            }
        } else {
            out.push_back(text[i]);
        }
    }
    return Wide(out);
}

std::string PackStrings(const std::vector<std::wstring>& items) {
    std::string out;
    for (size_t i = 0; i < items.size(); ++i) {
        if (i) out.push_back(kListSep);
        out += EscapeField(items[i]);
    }
    return out;
}

std::vector<std::wstring> UnpackStrings(std::string_view text) {
    std::vector<std::wstring> out;
    if (text.empty()) return out;
    for (const auto part : core::tsv::Split(text, kListSep)) {
        std::wstring value = UnescapeField(part);
        if (!value.empty()) out.push_back(std::move(value));
    }
    return out;
}

void PackArtists(const std::vector<stream::ArtistRef>& artists, std::string& namesOut, std::string& idsOut) {
    namesOut.clear();
    idsOut.clear();
    for (size_t i = 0; i < artists.size(); ++i) {
        if (i) {
            namesOut.push_back(kListSep);
            idsOut.push_back(kListSep);
        }
        namesOut += EscapeField(artists[i].name);
        idsOut += EscapeField(artists[i].id);
    }
}

std::vector<stream::ArtistRef> UnpackArtists(std::string_view namesField, std::string_view idsField) {
    std::vector<stream::ArtistRef> out;
    if (namesField.empty()) return out;
    const auto names = core::tsv::Split(namesField, kListSep);
    const auto ids = core::tsv::Split(idsField, kListSep);
    out.reserve(names.size());
    for (size_t i = 0; i < names.size(); ++i) {
        std::wstring name = UnescapeField(names[i]);
        if (name.empty()) continue;
        std::wstring id = i < ids.size() ? UnescapeField(ids[i]) : std::wstring();
        out.push_back({std::move(id), std::move(name)});
    }
    return out;
}

int64_t ParseInt64(std::string_view text) {
    if (text.empty()) return 0;
    std::string copy(text);
    return _strtoi64(copy.c_str(), nullptr, 10);
}

std::optional<AppleMusicDisplayRegion> ParseRegion(std::string_view text) {
    const int64_t raw = ParseInt64(text);
    const auto region = static_cast<AppleMusicDisplayRegion>(raw);
    return DisplayTargetFor(region) ? std::optional<AppleMusicDisplayRegion>(region) : std::nullopt;
}

std::filesystem::path CacheFile() {
    const auto dir = core::ProfileDir(L"applemusic");
    return dir.empty() ? std::filesystem::path() : dir / L"cache" / L"display.tsv";
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

template <typename Entry> bool IsFresh(const Entry& entry) {
    if (!entry.complete || entry.validatedAt <= 0) return false;
    const int64_t now = NowEpochSec();
    return now >= entry.validatedAt && (now - entry.validatedAt) <= kDisplayMetadataFreshnessSec;
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
    {
        std::lock_guard lock(m_mutex);
        ++m_epoch;
        m_songs.clear();
        m_albums.clear();
        m_artists.clear();
        m_playlists.clear();
        m_saveDirty = false;
    }
    std::lock_guard saving(m_saveMutex);
    if (const auto path = CacheFile(); !path.empty()) {
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }
}

void DisplayMetadata::BeginTransition() {
    std::lock_guard lock(m_mutex);
    ++m_epoch;
}

void DisplayMetadata::InvalidateFreshness() {
    std::lock_guard lock(m_mutex);
    for (auto& [key, entry] : m_songs) {
        (void)key;
        entry.validatedAt = 0;
    }
    for (auto& [key, entry] : m_albums) {
        (void)key;
        entry.validatedAt = 0;
    }
    for (auto& [key, entry] : m_artists) {
        (void)key;
        entry.validatedAt = 0;
    }
    for (auto& [key, entry] : m_playlists) {
        (void)key;
        entry.validatedAt = 0;
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

std::string DisplayMetadata::Key(const DisplayTarget& target, std::wstring_view kind, const stream::Id& id) {
    std::string key = "apple-display-v3;";
    Part(key, std::to_string(static_cast<int>(target.region)));
    Part(key, Utf8(kind));
    Part(key, Utf8(target.storefront));
    Part(key, Utf8(target.locale));
    Part(key, Utf8(id));
    return key;
}

void DisplayMetadata::LoadCache() {
    m_saveClosed = false;
    const auto path = CacheFile();
    if (path.empty()) return;
    const auto bytes = core::ReadFileLimited(path, kCacheFileLimit);
    if (!bytes) return;
    Deserialize(*bytes);
}

void DisplayMetadata::QueueSave() {
    if (m_saveClosed) return;
    m_saveDirty = true;
#ifdef GLASSWAVE_DISPLAY_METADATA_STANDALONE
    return;
#else
    if (m_savePosted.exchange(true)) return;
    const auto schedule = [this] {
        fb2k::callLater(2.0, [this] {
            m_savePosted = false;
            if (m_saveClosed) return;
            fb2k::splitTask([this] {
                std::lock_guard saving(m_saveMutex);
                if (!m_saveClosed && m_saveDirty.exchange(false)) SaveLocked();
            });
        });
    };
    if (core_api::is_main_thread())
        schedule();
    else
        fb2k::inMainThread(schedule);
#endif
}

void DisplayMetadata::FlushPendingSave() {
    std::lock_guard saving(m_saveMutex);
    m_saveClosed = true;
    if (m_saveDirty.exchange(false)) SaveLocked();
}

void DisplayMetadata::SaveLocked() const {
    const auto path = CacheFile();
    if (path.empty()) return;
    const std::string data = Serialize();
    if (data.empty()) return;
    try {
        core::WriteFileAtomic(path, data);
    } catch (const std::exception&) {
    }
}

std::string DisplayMetadata::Serialize() const {
    std::vector<SongEntry> songs;
    std::vector<AlbumEntry> albums;
    std::vector<ArtistEntry> artists;
    std::vector<PlaylistEntry> playlists;
    {
        std::lock_guard lock(m_mutex);
        songs.reserve(m_songs.size());
        for (const auto& [k, v] : m_songs) songs.push_back(v);
        albums.reserve(m_albums.size());
        for (const auto& [k, v] : m_albums) albums.push_back(v);
        artists.reserve(m_artists.size());
        for (const auto& [k, v] : m_artists) artists.push_back(v);
        playlists.reserve(m_playlists.size());
        for (const auto& [k, v] : m_playlists) playlists.push_back(v);
    }
    if (songs.empty() && albums.empty() && artists.empty() && playlists.empty()) return {};

    std::string out(kCacheHeader);
    out.push_back('\n');
    std::string artistNames, artistIds;

    for (const auto& e : songs) {
        if (e.id.empty() || !DisplayTargetFor(e.region)) continue;
        PackArtists(e.names.artists, artistNames, artistIds);
        out += "S\t" + std::to_string(static_cast<int>(e.region)) + '\t' + EscapeField(e.id) + '\t' +
               (e.complete ? "1" : "0") + '\t' + std::to_string(e.validatedAt) + '\t' +
               EscapeField(e.names.title) + '\t' + EscapeField(e.names.album) + '\t' +
               EscapeField(e.names.albumId) + '\t' + EscapeField(e.names.coverUrl) + '\t' + artistNames +
               '\t' + artistIds + '\t' + (e.names.artistsAuthoritative ? "1" : "0") + '\t' +
               EscapeField(e.names.composerName) + '\t' + PackStrings(e.names.composerNames) + '\t' +
               PackStrings(e.names.genreNames) + '\n';
    }
    for (const auto& e : albums) {
        if (e.id.empty() || !DisplayTargetFor(e.region)) continue;
        PackArtists(e.names.artists, artistNames, artistIds);
        out += "A\t" + std::to_string(static_cast<int>(e.region)) + '\t' + EscapeField(e.id) + '\t' +
               (e.complete ? "1" : "0") + '\t' + std::to_string(e.validatedAt) + '\t' +
               EscapeField(e.names.name) + '\t' + EscapeField(e.names.description) + '\t' +
               EscapeField(e.names.coverUrl) + '\t' + artistNames + '\t' + artistIds + '\t' +
               (e.names.artistsAuthoritative ? "1" : "0") + '\n';
    }
    for (const auto& e : artists) {
        if (e.id.empty() || !DisplayTargetFor(e.region)) continue;
        out += "R\t" + std::to_string(static_cast<int>(e.region)) + '\t' + EscapeField(e.id) + '\t' +
               (e.complete ? "1" : "0") + '\t' + std::to_string(e.validatedAt) + '\t' +
               EscapeField(e.names.name) + '\t' + EscapeField(e.names.bio) + '\t' +
               EscapeField(e.names.avatarUrl) + '\n';
    }
    for (const auto& e : playlists) {
        if (e.id.empty() || !DisplayTargetFor(e.region)) continue;
        out += "P\t" + std::to_string(static_cast<int>(e.region)) + '\t' + EscapeField(e.id) + '\t' +
               (e.complete ? "1" : "0") + '\t' + std::to_string(e.validatedAt) + '\t' +
               EscapeField(e.names.name) + '\t' + EscapeField(e.names.description) + '\t' +
               EscapeField(e.names.coverUrl) + '\n';
    }
    return out;
}

size_t DisplayMetadata::Deserialize(std::string_view bytes) {
    size_t lines = 0;
    size_t loaded = 0;
    bool headerSeen = false;
    std::lock_guard lock(m_mutex);
    for (const auto raw : core::tsv::Split(bytes, '\n')) {
        std::string_view line = raw;
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (++lines > kCacheLineLimit) break;
        if (!headerSeen) {
            if (line.empty()) continue;
            if (line != kCacheHeader) return 0;
            headerSeen = true;
            continue;
        }
        if (line.empty() || line[0] == '#') continue;
        const auto f = core::tsv::Split(line, '\t');
        if (f.size() < 5) continue;
        const auto region = ParseRegion(f[1]);
        if (!region) continue;
        const auto target = DisplayTargetFor(*region);
        if (!target) continue;
        const std::wstring id = UnescapeField(f[2]);
        if (id.empty()) continue;
        const bool complete = f[3] == "1";
        const int64_t validatedAt = ParseInt64(f[4]);

        if (f[0] == "S" && f.size() == 15) {
            auto& entry = m_songs[Key(*target, L"song", id)];
            if (entry.complete && !complete) continue;
            entry.region = *region;
            entry.id = id;
            entry.complete = complete;
            entry.validatedAt = validatedAt;
            entry.used = ++m_clock;
            entry.names.title = UnescapeField(f[5]);
            entry.names.album = UnescapeField(f[6]);
            entry.names.albumId = UnescapeField(f[7]);
            entry.names.coverUrl = UnescapeField(f[8]);
            entry.names.artists = UnpackArtists(f[9], f[10]);
            entry.names.artistsAuthoritative = f[11] == "1";
            entry.names.composerName = UnescapeField(f[12]);
            entry.names.composerNames = UnpackStrings(f[13]);
            entry.names.genreNames = UnpackStrings(f[14]);
            ++loaded;
        } else if (f[0] == "A" && f.size() == 11) {
            auto& entry = m_albums[Key(*target, L"album", id)];
            if (entry.complete && !complete) continue;
            entry.region = *region;
            entry.id = id;
            entry.complete = complete;
            entry.validatedAt = validatedAt;
            entry.used = ++m_clock;
            entry.names.name = UnescapeField(f[5]);
            entry.names.description = UnescapeField(f[6]);
            entry.names.coverUrl = UnescapeField(f[7]);
            entry.names.artists = UnpackArtists(f[8], f[9]);
            entry.names.artistsAuthoritative = f[10] == "1";
            ++loaded;
        } else if (f[0] == "R" && f.size() == 8) {
            auto& entry = m_artists[Key(*target, L"artist", id)];
            if (entry.complete && !complete) continue;
            entry.region = *region;
            entry.id = id;
            entry.complete = complete;
            entry.validatedAt = validatedAt;
            entry.used = ++m_clock;
            entry.names.name = UnescapeField(f[5]);
            entry.names.bio = UnescapeField(f[6]);
            entry.names.avatarUrl = UnescapeField(f[7]);
            ++loaded;
        } else if (f[0] == "P" && f.size() == 8) {
            auto& entry = m_playlists[Key(*target, L"playlist", id)];
            if (entry.complete && !complete) continue;
            entry.region = *region;
            entry.id = id;
            entry.complete = complete;
            entry.validatedAt = validatedAt;
            entry.used = ++m_clock;
            entry.names.name = UnescapeField(f[5]);
            entry.names.description = UnescapeField(f[6]);
            entry.names.coverUrl = UnescapeField(f[7]);
            ++loaded;
        }
    }
    TrimEntries(m_songs, kMaxSongEntries);
    TrimEntries(m_albums, kMaxAlbumEntries);
    TrimEntries(m_artists, kMaxArtistEntries);
    TrimEntries(m_playlists, kMaxPlaylistEntries);
    return loaded;
}

bool DisplayMetadata::HasSong(std::wstring_view sourceStorefront, const stream::Id& id) const {
    return HasSong(GetAppleMusicDisplayRegion(), sourceStorefront, id);
}

bool DisplayMetadata::HasSong(AppleMusicDisplayRegion region, std::wstring_view, const stream::Id& id) const {
    if (region != GetAppleMusicDisplayRegion()) return false;
    const auto target = DisplayTargetFor(region);
    if (!target) return false;
    std::lock_guard lock(m_mutex);
    const auto found = m_songs.find(Key(*target, L"song", id));
    if (found == m_songs.end()) return false;
    found->second.used = ++m_clock;
    return true;
}

bool DisplayMetadata::HasAlbum(std::wstring_view sourceStorefront, const stream::Id& id) const {
    return HasAlbum(GetAppleMusicDisplayRegion(), sourceStorefront, id);
}

bool DisplayMetadata::HasAlbum(AppleMusicDisplayRegion region, std::wstring_view,
                               const stream::Id& id) const {
    if (region != GetAppleMusicDisplayRegion()) return false;
    const auto target = DisplayTargetFor(region);
    if (!target) return false;
    std::lock_guard lock(m_mutex);
    const auto found = m_albums.find(Key(*target, L"album", id));
    if (found == m_albums.end()) return false;
    found->second.used = ++m_clock;
    return true;
}

bool DisplayMetadata::HasArtist(std::wstring_view sourceStorefront, const stream::Id& id) const {
    return HasArtist(GetAppleMusicDisplayRegion(), sourceStorefront, id);
}

bool DisplayMetadata::HasArtist(AppleMusicDisplayRegion region, std::wstring_view,
                                const stream::Id& id) const {
    if (region != GetAppleMusicDisplayRegion()) return false;
    const auto target = DisplayTargetFor(region);
    if (!target) return false;
    std::lock_guard lock(m_mutex);
    const auto found = m_artists.find(Key(*target, L"artist", id));
    if (found == m_artists.end()) return false;
    found->second.used = ++m_clock;
    return true;
}

bool DisplayMetadata::HasPlaylist(std::wstring_view sourceStorefront, const stream::Id& id) const {
    return HasPlaylist(GetAppleMusicDisplayRegion(), sourceStorefront, id);
}

bool DisplayMetadata::HasPlaylist(AppleMusicDisplayRegion region, std::wstring_view,
                                  const stream::Id& id) const {
    if (region != GetAppleMusicDisplayRegion()) return false;
    const auto target = DisplayTargetFor(region);
    if (!target) return false;
    std::lock_guard lock(m_mutex);
    const auto found = m_playlists.find(Key(*target, L"playlist", id));
    if (found == m_playlists.end()) return false;
    found->second.used = ++m_clock;
    return true;
}

bool DisplayMetadata::IsSongComplete(AppleMusicDisplayRegion region, std::wstring_view,
                                     const stream::Id& id) const {
    if (region != GetAppleMusicDisplayRegion()) return false;
    const auto target = DisplayTargetFor(region);
    if (!target) return false;
    std::lock_guard lock(m_mutex);
    const auto found = m_songs.find(Key(*target, L"song", id));
    if (found == m_songs.end()) return false;
    found->second.used = ++m_clock;
    return IsFresh(found->second);
}

bool DisplayMetadata::IsAlbumComplete(AppleMusicDisplayRegion region, std::wstring_view,
                                      const stream::Id& id) const {
    if (region != GetAppleMusicDisplayRegion()) return false;
    const auto target = DisplayTargetFor(region);
    if (!target) return false;
    std::lock_guard lock(m_mutex);
    const auto found = m_albums.find(Key(*target, L"album", id));
    if (found == m_albums.end()) return false;
    found->second.used = ++m_clock;
    return IsFresh(found->second);
}

bool DisplayMetadata::IsArtistComplete(AppleMusicDisplayRegion region, std::wstring_view,
                                       const stream::Id& id) const {
    if (region != GetAppleMusicDisplayRegion()) return false;
    const auto target = DisplayTargetFor(region);
    if (!target) return false;
    std::lock_guard lock(m_mutex);
    const auto found = m_artists.find(Key(*target, L"artist", id));
    if (found == m_artists.end()) return false;
    found->second.used = ++m_clock;
    return IsFresh(found->second);
}

bool DisplayMetadata::IsPlaylistComplete(AppleMusicDisplayRegion region, std::wstring_view,
                                         const stream::Id& id) const {
    if (region != GetAppleMusicDisplayRegion()) return false;
    const auto target = DisplayTargetFor(region);
    if (!target) return false;
    std::lock_guard lock(m_mutex);
    const auto found = m_playlists.find(Key(*target, L"playlist", id));
    if (found == m_playlists.end()) return false;
    found->second.used = ++m_clock;
    return IsFresh(found->second);
}

bool DisplayMetadata::PutSong(std::wstring_view sourceStorefront, const stream::Id& id,
                              SongDisplayNames names) {
    return PutSongFor(GetAppleMusicDisplayRegion(), sourceStorefront, id, std::move(names), true);
}

bool DisplayMetadata::PutSongFor(AppleMusicDisplayRegion region, std::wstring_view sourceStorefront,
                                 const stream::Id& id, SongDisplayNames names) {
    return PutSongFor(region, sourceStorefront, id, std::move(names), false);
}

bool DisplayMetadata::PutSongFor(AppleMusicDisplayRegion region, std::wstring_view sourceStorefront,
                                 const stream::Id& id, SongDisplayNames names, bool complete) {
    return PutSongFor(region, sourceStorefront, id, std::move(names), complete, kAnyEpoch);
}

bool DisplayMetadata::PutSongFor(AppleMusicDisplayRegion region, std::wstring_view, const stream::Id& id,
                                 SongDisplayNames names, bool complete, uint64_t expectedEpoch) {
    if (id.empty() || region == AppleMusicDisplayRegion::Account || region != GetAppleMusicDisplayRegion())
        return false;
    const auto target = DisplayTargetFor(region);
    if (!target) return false;
    bool changed = false;
    bool persist = false;
    {
        std::lock_guard lock(m_mutex);
        if (expectedEpoch != kAnyEpoch && m_epoch != expectedEpoch) return false;
        auto& entry = m_songs[Key(*target, L"song", id)];
        if (!complete && entry.complete) {
            entry.used = ++m_clock;
            return false;
        }
        const bool wasComplete = entry.complete;
        entry.region = region;
        entry.id = id;
        if (!names.title.empty() && entry.names.title != names.title) {
            entry.names.title = std::move(names.title);
            changed = true;
        }
        if (!names.album.empty() && entry.names.album != names.album) {
            entry.names.album = std::move(names.album);
            changed = true;
        }
        if (!names.albumId.empty() && entry.names.albumId != names.albumId) {
            entry.names.albumId = std::move(names.albumId);
            changed = true;
        }
        if (!names.coverUrl.empty() && entry.names.coverUrl != names.coverUrl) {
            entry.names.coverUrl = std::move(names.coverUrl);
            changed = true;
        }
        if (!names.artists.empty() &&
            (entry.names.artists != names.artists ||
             entry.names.artistsAuthoritative != names.artistsAuthoritative)) {
            entry.names.artists = std::move(names.artists);
            entry.names.artistsAuthoritative = names.artistsAuthoritative;
            changed = true;
        }
        if (!names.composerName.empty() && entry.names.composerName != names.composerName) {
            entry.names.composerName = std::move(names.composerName);
            changed = true;
        }
        if (!names.composerNames.empty() && entry.names.composerNames != names.composerNames) {
            entry.names.composerNames = std::move(names.composerNames);
            changed = true;
        }
        if (!names.genreNames.empty() && entry.names.genreNames != names.genreNames) {
            entry.names.genreNames = std::move(names.genreNames);
            changed = true;
        }
        entry.complete = entry.complete || complete;
        if (complete) entry.validatedAt = NowEpochSec();
        entry.used = ++m_clock;
        persist = changed || (complete && !wasComplete);
        TrimEntries(m_songs, kMaxSongEntries);
    }
    if (persist) QueueSave();
    return changed;
}

bool DisplayMetadata::PutAlbum(std::wstring_view sourceStorefront, const stream::Id& id,
                               AlbumDisplayNames names) {
    return PutAlbumFor(GetAppleMusicDisplayRegion(), sourceStorefront, id, std::move(names), true);
}

bool DisplayMetadata::PutAlbumFor(AppleMusicDisplayRegion region, std::wstring_view sourceStorefront,
                                  const stream::Id& id, AlbumDisplayNames names) {
    return PutAlbumFor(region, sourceStorefront, id, std::move(names), false);
}

bool DisplayMetadata::PutAlbumFor(AppleMusicDisplayRegion region, std::wstring_view sourceStorefront,
                                  const stream::Id& id, AlbumDisplayNames names, bool complete) {
    return PutAlbumFor(region, sourceStorefront, id, std::move(names), complete, kAnyEpoch);
}

bool DisplayMetadata::PutAlbumFor(AppleMusicDisplayRegion region, std::wstring_view, const stream::Id& id,
                                  AlbumDisplayNames names, bool complete, uint64_t expectedEpoch) {
    if (id.empty() || region == AppleMusicDisplayRegion::Account || region != GetAppleMusicDisplayRegion())
        return false;
    const auto target = DisplayTargetFor(region);
    if (!target) return false;
    bool changed = false;
    bool persist = false;
    {
        std::lock_guard lock(m_mutex);
        if (expectedEpoch != kAnyEpoch && m_epoch != expectedEpoch) return false;
        auto& entry = m_albums[Key(*target, L"album", id)];
        if (!complete && entry.complete) {
            entry.used = ++m_clock;
            return false;
        }
        const bool wasComplete = entry.complete;
        entry.region = region;
        entry.id = id;
        if (!names.name.empty() && entry.names.name != names.name) {
            entry.names.name = std::move(names.name);
            changed = true;
        }
        if (!names.description.empty() && entry.names.description != names.description) {
            entry.names.description = std::move(names.description);
            changed = true;
        }
        if (!names.coverUrl.empty() && entry.names.coverUrl != names.coverUrl) {
            entry.names.coverUrl = std::move(names.coverUrl);
            changed = true;
        }
        if (!names.artists.empty() &&
            (entry.names.artists != names.artists ||
             entry.names.artistsAuthoritative != names.artistsAuthoritative)) {
            entry.names.artists = std::move(names.artists);
            entry.names.artistsAuthoritative = names.artistsAuthoritative;
            changed = true;
        }
        entry.complete = entry.complete || complete;
        if (complete) entry.validatedAt = NowEpochSec();
        entry.used = ++m_clock;
        persist = changed || (complete && !wasComplete);
        TrimEntries(m_albums, kMaxAlbumEntries);
    }
    if (persist) QueueSave();
    return changed;
}

bool DisplayMetadata::PutArtist(std::wstring_view sourceStorefront, const stream::Id& id,
                                ArtistDisplayNames names) {
    return PutArtistFor(GetAppleMusicDisplayRegion(), sourceStorefront, id, std::move(names), true);
}

bool DisplayMetadata::PutArtistFor(AppleMusicDisplayRegion region, std::wstring_view sourceStorefront,
                                   const stream::Id& id, ArtistDisplayNames names) {
    return PutArtistFor(region, sourceStorefront, id, std::move(names), false);
}

bool DisplayMetadata::PutArtistFor(AppleMusicDisplayRegion region, std::wstring_view sourceStorefront,
                                   const stream::Id& id, ArtistDisplayNames names, bool complete) {
    return PutArtistFor(region, sourceStorefront, id, std::move(names), complete, kAnyEpoch);
}

bool DisplayMetadata::PutArtistFor(AppleMusicDisplayRegion region, std::wstring_view, const stream::Id& id,
                                   ArtistDisplayNames names, bool complete, uint64_t expectedEpoch) {
    if (id.empty() || region == AppleMusicDisplayRegion::Account || region != GetAppleMusicDisplayRegion())
        return false;
    const auto target = DisplayTargetFor(region);
    if (!target) return false;
    bool changed = false;
    bool persist = false;
    {
        std::lock_guard lock(m_mutex);
        if (expectedEpoch != kAnyEpoch && m_epoch != expectedEpoch) return false;
        auto& entry = m_artists[Key(*target, L"artist", id)];
        if (!complete && entry.complete) {
            entry.used = ++m_clock;
            return false;
        }
        const bool wasComplete = entry.complete;
        entry.region = region;
        entry.id = id;
        if (!names.name.empty() && entry.names.name != names.name) {
            entry.names.name = std::move(names.name);
            changed = true;
        }
        if (!names.bio.empty() && entry.names.bio != names.bio) {
            entry.names.bio = std::move(names.bio);
            changed = true;
        }
        if (!names.avatarUrl.empty() && entry.names.avatarUrl != names.avatarUrl) {
            entry.names.avatarUrl = std::move(names.avatarUrl);
            changed = true;
        }
        entry.complete = entry.complete || complete;
        if (complete) entry.validatedAt = NowEpochSec();
        entry.used = ++m_clock;
        persist = changed || (complete && !wasComplete);
        TrimEntries(m_artists, kMaxArtistEntries);
    }
    if (persist) QueueSave();
    return changed;
}

bool DisplayMetadata::PutPlaylist(std::wstring_view sourceStorefront, const stream::Id& id,
                                  PlaylistDisplayNames names) {
    return PutPlaylistFor(GetAppleMusicDisplayRegion(), sourceStorefront, id, std::move(names), true);
}

bool DisplayMetadata::PutPlaylistFor(AppleMusicDisplayRegion region, std::wstring_view sourceStorefront,
                                     const stream::Id& id, PlaylistDisplayNames names) {
    return PutPlaylistFor(region, sourceStorefront, id, std::move(names), false);
}

bool DisplayMetadata::PutPlaylistFor(AppleMusicDisplayRegion region, std::wstring_view sourceStorefront,
                                     const stream::Id& id, PlaylistDisplayNames names, bool complete) {
    return PutPlaylistFor(region, sourceStorefront, id, std::move(names), complete, kAnyEpoch);
}

bool DisplayMetadata::PutPlaylistFor(AppleMusicDisplayRegion region, std::wstring_view, const stream::Id& id,
                                     PlaylistDisplayNames names, bool complete, uint64_t expectedEpoch) {
    if (id.empty() || region == AppleMusicDisplayRegion::Account || region != GetAppleMusicDisplayRegion())
        return false;
    const auto target = DisplayTargetFor(region);
    if (!target) return false;
    bool changed = false;
    bool persist = false;
    {
        std::lock_guard lock(m_mutex);
        if (expectedEpoch != kAnyEpoch && m_epoch != expectedEpoch) return false;
        auto& entry = m_playlists[Key(*target, L"playlist", id)];
        if (!complete && entry.complete) {
            entry.used = ++m_clock;
            return false;
        }
        const bool wasComplete = entry.complete;
        entry.region = region;
        entry.id = id;
        if (!names.name.empty() && entry.names.name != names.name) {
            entry.names.name = std::move(names.name);
            changed = true;
        }
        if (!names.description.empty() && entry.names.description != names.description) {
            entry.names.description = std::move(names.description);
            changed = true;
        }
        if (!names.coverUrl.empty() && entry.names.coverUrl != names.coverUrl) {
            entry.names.coverUrl = std::move(names.coverUrl);
            changed = true;
        }
        entry.complete = entry.complete || complete;
        if (complete) entry.validatedAt = NowEpochSec();
        entry.used = ++m_clock;
        persist = changed || (complete && !wasComplete);
        TrimEntries(m_playlists, kMaxPlaylistEntries);
    }
    if (persist) QueueSave();
    return changed;
}

void DisplayMetadata::ApplyArtistFallback(std::vector<stream::ArtistRef>& artists,
                                          const std::vector<stream::ArtistRef>& explicitNames,
                                          std::wstring_view sourceStorefront) const {
    for (auto& artist : artists) {
        const bool namedExplicitly = std::any_of(
            explicitNames.begin(), explicitNames.end(), [&artist](const stream::ArtistRef& candidate) {
                return !artist.id.empty() && candidate.id == artist.id && !candidate.name.empty();
            });
        if (namedExplicitly) continue;
        stream::Artist display;
        display.id = artist.id;
        display.name = artist.name;
        Apply(display, sourceStorefront);
        if (!display.name.empty()) artist.name = display.name;
    }
}

void DisplayMetadata::Apply(Song& song, std::wstring_view sourceStorefront) const {
    const auto target = CurrentTarget();
    if (!target) return;
    const bool applyNames = HasAppleMusicDisplayScope(AppleMusicDisplayScope::Names);
    const bool applyEditorial = HasAppleMusicDisplayScope(AppleMusicDisplayScope::Editorial);
    if (!applyNames && !applyEditorial) return;
    SongDisplayNames names;
    bool foundSong = false;
    {
        std::lock_guard lock(m_mutex);
        const auto found = m_songs.find(Key(*target, L"song", song.track.id));
        if (found != m_songs.end()) {
            found->second.used = ++m_clock;
            names = found->second.names;
            foundSong = true;
        }
    }
    if (foundSong) {
        if (applyNames) {
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
        }
        if (applyEditorial) {
            if (!names.composerNames.empty()) song.composerNames = names.composerNames;
            if (!names.composerName.empty())
                song.composerName = names.composerName;
            else if (!names.composerNames.empty())
                song.composerName.clear();
            if (!names.genreNames.empty()) song.genreNames = names.genreNames;
        }
    }
    if (applyNames && !song.customAlbum && !song.track.albumId.empty() &&
        (names.album.empty() || names.artists.empty() || names.coverUrl.empty())) {
        stream::Album album;
        album.id = song.track.albumId;
        album.name = song.track.album;
        album.artists = song.track.artists;
        album.coverUrl = song.coverUrl;
        Apply(album, sourceStorefront);
        if (names.album.empty() && !album.name.empty()) song.track.album = album.name;
        if (names.artists.empty()) ApplyArtists(song.track.artists, album.artists);
        if (names.coverUrl.empty() && !album.coverUrl.empty()) song.coverUrl = album.coverUrl;
    }
    if (applyNames && !names.artistsAuthoritative)
        ApplyArtistFallback(song.track.artists, names.artists, sourceStorefront);
}

void DisplayMetadata::Apply(stream::Track& track, std::wstring_view sourceStorefront) const {
    const auto target = CurrentTarget();
    if (!target || !HasAppleMusicDisplayScope(AppleMusicDisplayScope::Names)) return;
    SongDisplayNames names;
    bool foundSong = false;
    {
        std::lock_guard lock(m_mutex);
        const auto found = m_songs.find(Key(*target, L"song", track.id));
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
    if (!names.artistsAuthoritative) ApplyArtistFallback(track.artists, names.artists, sourceStorefront);
}

void DisplayMetadata::Apply(stream::Album& album, std::wstring_view sourceStorefront) const {
    const auto target = CurrentTarget();
    if (!target) return;
    const bool applyNames = HasAppleMusicDisplayScope(AppleMusicDisplayScope::Names);
    const bool applyEditorial = HasAppleMusicDisplayScope(AppleMusicDisplayScope::Editorial);
    if (!applyNames && !applyEditorial) return;
    AlbumDisplayNames names;
    bool foundAlbum = false;
    {
        std::lock_guard lock(m_mutex);
        const auto found = m_albums.find(Key(*target, L"album", album.id));
        if (found != m_albums.end()) {
            found->second.used = ++m_clock;
            names = found->second.names;
            foundAlbum = true;
        }
    }
    if (foundAlbum) {
        if (applyNames) {
            if (!names.name.empty()) album.name = names.name;
            if (!names.coverUrl.empty()) album.coverUrl = names.coverUrl;
            if (names.artistsAuthoritative)
                album.artists = names.artists;
            else
                ApplyArtists(album.artists, names.artists);
        }
        if (applyEditorial) {
            if (!names.description.empty()) album.description = names.description;
        }
    }
    if (applyNames && !names.artistsAuthoritative)
        ApplyArtistFallback(album.artists, names.artists, sourceStorefront);
}

void DisplayMetadata::Apply(stream::Artist& artist, std::wstring_view) const {
    const auto target = CurrentTarget();
    if (!target) return;
    const bool applyNames = HasAppleMusicDisplayScope(AppleMusicDisplayScope::Names);
    const bool applyEditorial = HasAppleMusicDisplayScope(AppleMusicDisplayScope::Editorial);
    if (!applyNames && !applyEditorial) return;
    ArtistDisplayNames names;
    {
        std::lock_guard lock(m_mutex);
        const auto found = m_artists.find(Key(*target, L"artist", artist.id));
        if (found == m_artists.end()) return;
        found->second.used = ++m_clock;
        names = found->second.names;
    }
    if (applyNames) {
        if (!names.name.empty()) artist.name = names.name;
        if (!names.avatarUrl.empty()) artist.avatarUrl = names.avatarUrl;
    }
    if (applyEditorial) {
        if (!names.bio.empty()) artist.bio = names.bio;
    }
}

void DisplayMetadata::Apply(stream::Playlist& playlist, std::wstring_view) const {
    const auto target = CurrentTarget();
    if (!target || playlist.own || !HasAppleMusicDisplayScope(AppleMusicDisplayScope::Playlists)) return;
    PlaylistDisplayNames names;
    {
        std::lock_guard lock(m_mutex);
        const auto found = m_playlists.find(Key(*target, L"playlist", playlist.id));
        if (found == m_playlists.end()) return;
        found->second.used = ++m_clock;
        names = found->second.names;
    }
    if (!names.name.empty()) playlist.name = names.name;
    if (!names.description.empty()) playlist.description = names.description;
    if (!names.coverUrl.empty()) playlist.coverUrl = names.coverUrl;
}

} // namespace glasswave::applemusic
