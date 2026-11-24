#include "applemusic/display_metadata.h"
#include "core/file.h"
#include "core/paths.h"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace fs = std::filesystem;

namespace {

glasswave::applemusic::AppleMusicDisplayRegion g_region =
    glasswave::applemusic::AppleMusicDisplayRegion::Japan;
int g_scope = glasswave::applemusic::kAppleMusicDisplayScopeMetadataMask;
fs::path g_testProfileDir;
std::atomic_size_t g_checks = 0;

void Check(bool condition, std::string_view message) {
    ++g_checks;
    if (!condition) throw std::runtime_error(std::string(message));
}

} // namespace

namespace glasswave::applemusic {

AppleMusicDisplayRegion GetAppleMusicDisplayRegion() {
    return g_region;
}

void SetAppleMusicDisplayRegion(AppleMusicDisplayRegion region) {
    g_region = region;
}

int GetAppleMusicDisplayScope() {
    return g_scope;
}

void SetAppleMusicDisplayScope(int mask) {
    g_scope = mask & kAppleMusicDisplayScopeMask;
}

bool HasAppleMusicDisplayScope(AppleMusicDisplayScope flag) {
    return (g_scope & static_cast<int>(flag)) != 0;
}

void SetAppleMusicDisplayScopeFlag(AppleMusicDisplayScope flag, bool enabled) {
    if (enabled)
        g_scope |= static_cast<int>(flag);
    else
        g_scope &= ~static_cast<int>(flag);
}

} // namespace glasswave::applemusic

namespace glasswave::core {

fs::path ProfileDir(std::wstring_view sub) {
    if (g_testProfileDir.empty()) return {};
    return sub.empty() ? g_testProfileDir : g_testProfileDir / sub;
}

std::optional<std::string> ReadFileLimited(const fs::path& path, size_t maxBytes) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    std::string out((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (out.size() > maxBytes) return std::nullopt;
    return out;
}

void WriteFileAtomic(const fs::path& path, std::string_view bytes) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) throw std::runtime_error("WriteFileAtomic failed to open output");
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!out) throw std::runtime_error("WriteFileAtomic failed to write bytes");
}

} // namespace glasswave::core

int main(int argc, char** argv) {
    using namespace glasswave::applemusic;
    using namespace glasswave::stream;

    try {
        if (argc >= 2) g_testProfileDir = fs::path(argv[1]);
        auto& dm = DisplayMetadata::Get();
        dm.Clear();

        // 1. Source-storefront independence: entries stored while sourceStorefront is empty
        // (early startup before account verification) must be found after storefront is "US".
        SetAppleMusicDisplayRegion(AppleMusicDisplayRegion::Japan);
        dm.SetSourceStorefront(L"");
        SongDisplayNames songNames;
        songNames.title = L"\u591c\u306b\u99c6\u3051\u308b";
        songNames.album = L"THE BOOK";
        songNames.artists = {{L"1490256993", L"YOASOBI"}};
        songNames.artistsAuthoritative = true;
        songNames.composerName = L"Ayase";
        songNames.composerNames = {L"Ayase"};
        songNames.genreNames = {L"J-Pop", L"\u30a2\u30cb\u30e1"};
        songNames.albumId = L"1544228268";
        songNames.coverUrl = L"https://example.test/cover.jpg";

        Check(dm.PutSongFor(AppleMusicDisplayRegion::Japan, L"", L"1544228270", songNames, true),
              "first PutSongFor must report changed");
        Check(!dm.PutSongFor(AppleMusicDisplayRegion::Japan, L"US", L"1544228270", songNames, true),
              "identical PutSongFor under different sourceStorefront must report unchanged");
        Check(dm.HasSong(L"US", L"1544228270"),
              "song stored under empty sourceStorefront must match under US");
        Check(dm.IsSongComplete(AppleMusicDisplayRegion::Japan, L"GB", L"1544228270"),
              "song completeness must be independent of sourceStorefront");

        // Partial (complete=false) update must not clobber a complete entry.
        SongDisplayNames partial;
        partial.title = L"Partial";
        Check(!dm.PutSongFor(AppleMusicDisplayRegion::Japan, L"US", L"1544228270", partial, false),
              "partial PutSongFor must not overwrite a complete entry");

        // 2. Cascading fallback in Apply(Song&) and Apply(Album&).
        ArtistDisplayNames artistNames;
        artistNames.name = L"\u7c73\u6d25\u7384\u5e2b";
        artistNames.bio = L"Line1\tTabbed\nLine2\\Backslash";
        artistNames.avatarUrl = L"https://example.test/artist.jpg";
        Check(dm.PutArtistFor(AppleMusicDisplayRegion::Japan, L"US", L"546572536", artistNames, true),
              "first PutArtistFor must report changed");

        AlbumDisplayNames albumNames;
        albumNames.name = L"STRAY SHEEP (\u65e5\u672c\u76e4)";
        albumNames.description = L"Desc\nSecond line";
        albumNames.artists = {{L"546572536", L"\u7c73\u6d25\u7384\u5e2b"}};
        albumNames.artistsAuthoritative = true;
        Check(dm.PutAlbumFor(AppleMusicDisplayRegion::Japan, L"US", L"1521000000", albumNames, true),
              "first PutAlbumFor must report changed");

        PlaylistDisplayNames playlistNames;
        playlistNames.name = L"\u4eca\u65e5\u306e\u30d2\u30c3\u30c8";
        playlistNames.description = L"Playlist\tdesc\nline2";
        Check(dm.PutPlaylistFor(AppleMusicDisplayRegion::Japan, L"US", L"pl.123", playlistNames, true),
              "first PutPlaylistFor must report changed");

        // Song with NO direct song entry, WITH album entry -> inherits album name & artists.
        Song fromAlbumFallback;
        fromAlbumFallback.track.id = L"999001";
        fromAlbumFallback.track.title = L"Lemon";
        fromAlbumFallback.track.album = L"STRAY SHEEP";
        fromAlbumFallback.track.albumId = L"1521000000";
        fromAlbumFallback.track.artists = {{L"546572536", L"Kenshi Yonezu"}};
        dm.Apply(fromAlbumFallback, L"US");
        Check(fromAlbumFallback.track.album == L"STRAY SHEEP (\u65e5\u672c\u76e4)",
              "Apply(Song) must fall back to AlbumDisplayNames for album title");
        Check(fromAlbumFallback.track.artists.size() == 1 &&
                  fromAlbumFallback.track.artists[0].name == L"\u7c73\u6d25\u7384\u5e2b",
              "Apply(Song) must fall back to AlbumDisplayNames/ArtistDisplayNames for artist");

        // Song with customAlbum=true must keep custom album name even when album entry exists.
        Song customAlbumSong = fromAlbumFallback;
        customAlbumSong.track.album = L"My Custom Album";
        customAlbumSong.customAlbum = true;
        dm.Apply(customAlbumSong, L"US");
        Check(customAlbumSong.track.album == L"My Custom Album",
              "Apply(Song) must respect customAlbum flag");

        // Song with NO song/album entry, WITH artist entry -> inherits artist name.
        Song fromArtistFallback;
        fromArtistFallback.track.id = L"999002";
        fromArtistFallback.track.title = L"Kick Back";
        fromArtistFallback.track.album = L"KICK BACK - Single";
        fromArtistFallback.track.albumId = L"unknown-album";
        fromArtistFallback.track.artists = {{L"546572536", L"Kenshi Yonezu"}};
        dm.Apply(fromArtistFallback, L"US");
        Check(fromArtistFallback.track.artists[0].name == L"\u7c73\u6d25\u7384\u5e2b",
              "Apply(Song) must fall back to ArtistDisplayNames when song and album entries are absent");

        // Album with NO album entry, WITH artist entry -> inherits artist name.
        Album albumArtistFallback;
        albumArtistFallback.id = L"unknown-album-2";
        albumArtistFallback.name = L"BOOTLEG";
        albumArtistFallback.artists = {{L"546572536", L"Kenshi Yonezu"}};
        dm.Apply(albumArtistFallback, L"US");
        Check(albumArtistFallback.artists[0].name == L"\u7c73\u6d25\u7384\u5e2b",
              "Apply(Album) must fall back to ArtistDisplayNames when album entry is absent");

        // 3. Epoch guard rejects stale writes after BeginTransition.
        const uint64_t oldEpoch = dm.Epoch();
        dm.BeginTransition();
        Check(dm.Epoch() > oldEpoch, "BeginTransition must advance epoch");
        SongDisplayNames staleSong;
        staleSong.title = L"Stale";
        Check(!dm.PutSongFor(AppleMusicDisplayRegion::Japan, L"US", L"stale-song", staleSong, true,
                             oldEpoch),
              "PutSongFor with old epoch must be rejected");
        Check(!dm.HasSong(L"US", L"stale-song"), "stale song must not be stored");

        // 4. Persistence round-trip via FlushPendingSave() + Clear() + LoadCache().
        if (!g_testProfileDir.empty()) {
            const std::string serialized = dm.Serialize();
            Check(!serialized.empty(), "Serialize must produce non-empty payload");
            dm.FlushPendingSave();
            const fs::path cachePath = g_testProfileDir / L"applemusic" / L"cache" / L"display.tsv";
            Check(fs::exists(cachePath), "FlushPendingSave must write display.tsv");

            // Keep a copy of serialized bytes, clear in-memory + disk, then rewrite disk & LoadCache.
            dm.Clear();
            Check(!dm.HasSong(L"US", L"1544228270"), "Clear must wipe in-memory entries");
            Check(!fs::exists(cachePath), "Clear must delete display.tsv");

            glasswave::core::WriteFileAtomic(cachePath, serialized);
            dm.LoadCache();

            Check(dm.HasSong(L"US", L"1544228270"), "LoadCache must restore song entry");
            Check(dm.IsSongComplete(AppleMusicDisplayRegion::Japan, L"US", L"1544228270"),
                  "LoadCache must preserve song completeness");

            Song restoredSong;
            restoredSong.track.id = L"1544228270";
            restoredSong.track.title = L"Racing Into the Night";
            dm.Apply(restoredSong, L"US");
            Check(restoredSong.track.title == L"\u591c\u306b\u99c6\u3051\u308b",
                  "restored song title must match");
            Check(restoredSong.composerName == L"Ayase", "restored song composer must match");
            Check(restoredSong.genreNames.size() == 2 &&
                      restoredSong.genreNames[1] == L"\u30a2\u30cb\u30e1",
                  "restored song genres must match");

            Artist restoredArtist;
            restoredArtist.id = L"546572536";
            restoredArtist.name = L"Kenshi Yonezu";
            dm.Apply(restoredArtist, L"US");
            Check(restoredArtist.name == L"\u7c73\u6d25\u7384\u5e2b", "restored artist name must match");
            Check(restoredArtist.bio == L"Line1\tTabbed\nLine2\\Backslash",
                  "restored artist bio must preserve escaped tabs, newlines and backslashes");

            Playlist restoredPlaylist;
            restoredPlaylist.id = L"pl.123";
            restoredPlaylist.name = L"Today's Hits";
            dm.Apply(restoredPlaylist, L"US");
            Check(restoredPlaylist.name == L"\u4eca\u65e5\u306e\u30d2\u30c3\u30c8",
                  "restored playlist name must match");
            Check(restoredPlaylist.description == L"Playlist\tdesc\nline2",
                  "restored playlist description must match");
        }

        // 5. InvalidateFreshness keeps cached first-paint entry while marking it incomplete.
        dm.InvalidateFreshness();
        Check(dm.HasSong(L"US", L"1544228270"),
              "InvalidateFreshness must keep cached song available for first paint");
        Check(!dm.IsSongComplete(AppleMusicDisplayRegion::Japan, L"US", L"1544228270"),
              "InvalidateFreshness must require revalidation");

        // 6. Selective scope filtering: Names-only overlays titles/artists/albums while keeping
        // raw Chinese descriptions, genres, composers and official playlists untouched.
        SetAppleMusicDisplayScope(static_cast<int>(AppleMusicDisplayScope::Names));
        {
            Song s;
            s.track.id = L"1544228270";
            s.track.title = L"Racing Into the Night";
            s.composerName = L"RawComposer";
            s.genreNames = {L"RawGenre"};
            dm.Apply(s, L"US");
            Check(s.track.title == L"\u591c\u306b\u99c6\u3051\u308b",
                  "Names scope must still overlay song title");
            Check(s.composerName == L"RawComposer",
                  "Names-only scope must preserve raw composer");
            Check(s.genreNames.size() == 1 && s.genreNames[0] == L"RawGenre",
                  "Names-only scope must preserve raw genres");

            Artist a;
            a.id = L"546572536";
            a.name = L"Kenshi Yonezu";
            a.bio = L"Chinese Bio";
            dm.Apply(a, L"US");
            Check(a.name == L"\u7c73\u6d25\u7384\u5e2b", "Names scope must overlay artist name");
            Check(a.bio == L"Chinese Bio", "Names-only scope must preserve raw artist bio");

            Album al;
            al.id = L"1521000000";
            al.name = L"STRAY SHEEP";
            al.description = L"Chinese Album Desc";
            dm.Apply(al, L"US");
            Check(al.name == L"STRAY SHEEP (\u65e5\u672c\u76e4)",
                  "Names scope must overlay album name");
            Check(al.description == L"Chinese Album Desc",
                  "Names-only scope must preserve raw album description");

            Playlist p;
            p.id = L"pl.123";
            p.name = L"Chinese Playlist";
            p.description = L"Chinese Playlist Desc";
            dm.Apply(p, L"US");
            Check(p.name == L"Chinese Playlist", "Names-only scope must not overlay editorial playlist");
            Check(p.description == L"Chinese Playlist Desc",
                  "Names-only scope must not overlay playlist description");
        }
        // Enabling Editorial overlays bios/descriptions/genres without touching playlists.
        SetAppleMusicDisplayScope(static_cast<int>(AppleMusicDisplayScope::Editorial));
        {
            Artist a;
            a.id = L"546572536";
            a.name = L"Kenshi Yonezu";
            a.bio = L"Chinese Bio";
            dm.Apply(a, L"US");
            Check(a.name == L"Kenshi Yonezu", "Editorial-only scope must preserve raw artist name");
            Check(a.bio == L"Line1\tTabbed\nLine2\\Backslash", "Editorial scope must overlay bio");
        }
        // Enabling Playlists overlays editorial playlists while Lyrics-only overlays no metadata.
        SetAppleMusicDisplayScope(static_cast<int>(AppleMusicDisplayScope::Lyrics));
        {
            Track t;
            t.id = L"1544228270";
            t.title = L"Raw Title";
            dm.Apply(t, L"US");
            Check(t.title == L"Raw Title", "Lyrics-only scope must not overlay track title");
        }

        std::cout << "apple_display_metadata_tests: " << g_checks.load() << " checks passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << "\n";
        return 1;
    }
}
