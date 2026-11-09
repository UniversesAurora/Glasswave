#include "pch.h"
#include "netease/stream_provider.h"
#include "stream/playback_source.h"
#include "netease/catalog.h"
#include "netease/config.h"
#include "netease/cover_cache.h"
#include "netease/handle.h"
#include "netease/session.h"
#include "ui/i18n.h"
#include "ui/netease_login.h"

#include <ctime>

namespace glasswave::netease {

namespace {

using stream::Id;

Session& Ncm() { return Session::Get(); }

int64_t Num(const Id& id) { return id.empty() ? 0 : static_cast<int64_t>(_wcstoi64(id.c_str(), nullptr, 10)); }

Id Text(int64_t id) { return id ? std::to_wstring(id) : Id(); }

std::vector<int64_t> Nums(const std::vector<Id>& ids) {
    std::vector<int64_t> out;
    out.reserve(ids.size());
    for (const auto& id : ids)
        if (const int64_t n = Num(id)) out.push_back(n);
    return out;
}

int Year(int64_t publishTimeMs) {
    if (publishTimeMs <= 0) return 0;
    const time_t seconds = static_cast<time_t>(publishTimeMs / 1000);
    std::tm local{};
    return localtime_s(&local, &seconds) == 0 ? local.tm_year + 1900 : 0;
}

std::vector<stream::ArtistRef> Refs(const std::vector<ArtistRef>& refs) {
    std::vector<stream::ArtistRef> out;
    out.reserve(refs.size());
    for (const auto& ref : refs) out.push_back({Text(ref.id), ref.name});
    return out;
}

stream::Track ToTrack(const Song& song) {
    stream::Track track;
    track.id = Text(song.id);
    track.title = song.title;
    track.album = song.album;
    track.albumId = Text(song.albumId);
    track.artists = Refs(song.artistRefs);
    if (track.artists.empty())  // songs cached before ids were stored
        for (const auto& name : song.artists) track.artists.push_back({{}, name});
    track.durationMs = song.durationMs;
    track.popularity = song.popularity;
    return track;
}

std::vector<stream::Track> ToTracks(const std::vector<Song>& songs) {
    std::vector<stream::Track> out;
    out.reserve(songs.size());
    for (const auto& song : songs) out.push_back(ToTrack(song));
    return out;
}

std::optional<std::vector<stream::Track>> ToTracks(const std::optional<std::vector<Song>>& songs) {
    if (!songs) return std::nullopt;
    return ToTracks(*songs);
}

bool IsOwn(const Playlist& playlist) {
    const int64_t user = Ncm().CurrentAccount().userId;
    return playlist.creatorId ? playlist.creatorId == user : !playlist.subscribed;
}

stream::Playlist ToPlaylist(const Playlist& playlist) {
    stream::Playlist out;
    out.id = Text(playlist.id);
    out.name = playlist.name;
    out.coverUrl = playlist.coverUrl;
    out.creator = playlist.creatorName;
    out.description = playlist.description;
    out.trackCount = playlist.trackCount;
    out.own = Ncm().FindPlaylistScope(playlist.id) == Session::PlaylistScope::Created || IsOwn(playlist);
    out.saved = !out.own && (playlist.subscribed ||
                             Ncm().FindPlaylistScope(playlist.id) == Session::PlaylistScope::Collected);
    return out;
}

std::vector<stream::Playlist> ToPlaylists(const std::vector<Playlist>& playlists) {
    std::vector<stream::Playlist> out;
    out.reserve(playlists.size());
    for (const auto& playlist : playlists) out.push_back(ToPlaylist(playlist));
    return out;
}

stream::Album ToAlbum(const Album& album) {
    stream::Album out;
    out.id = Text(album.id);
    out.name = album.name;
    out.coverUrl = album.coverUrl;
    out.artists = Refs(album.artistRefs);
    out.description = album.description;
    out.trackCount = album.trackCount;
    out.year = Year(album.publishTime);
    return out;
}

std::vector<stream::Album> ToAlbums(const std::vector<Album>& albums) {
    std::vector<stream::Album> out;
    out.reserve(albums.size());
    for (const auto& album : albums) out.push_back(ToAlbum(album));
    return out;
}

stream::Artist ToArtist(const Artist& artist) {
    stream::Artist out;
    out.id = Text(artist.id);
    out.name = artist.name;
    out.avatarUrl = artist.avatarUrl;
    out.bio = artist.brief;
    out.albumCount = artist.albumCount;
    out.trackCount = artist.songCount;
    out.followKnown = artist.followKnown;
    out.followed = artist.followed;
    // The followed-artists list is authoritative even when a profile lags behind a follow.
    for (const auto& saved : Ncm().FollowedArtists())
        if (saved.id == artist.id) {
            out.followKnown = out.followed = true;
            break;
        }
    return out;
}

std::vector<stream::Artist> ToArtists(const std::vector<Artist>& artists) {
    std::vector<stream::Artist> out;
    out.reserve(artists.size());
    for (const auto& artist : artists) out.push_back(ToArtist(artist));
    return out;
}

// netease::Session reports (ok, error); stream reports an error text only.
Session::DoneCallback Adapt(stream::Provider::Done done) {
    if (!done) return {};
    return [done = std::move(done)](bool ok, std::wstring error) {
        done(ok ? std::wstring() : (error.empty() ? std::wstring(L"The request failed.") : std::move(error)));
    };
}

// Registered while the component loads, so the UI finds it however early it is created.
// Started by the UI or at foobar2000 start-up on the main thread, whichever comes first, so
// ncm:// items also play with another UI; stopped before the component is unloaded (no
// joins in static destructors).
const bool g_registered = [] {
    stream::Register(StreamProvider::Get());
    return true;
}();

class StreamProviderInit : public initquit {
public:
    void on_init() override { StreamProvider::Get().Start(); }
    void on_quit() override { StreamProvider::Get().Shutdown(); }
};
FB2K_SERVICE_FACTORY(StreamProviderInit);

}  // namespace

StreamProvider& StreamProvider::Get() {
    static StreamProvider provider;
    return provider;
}

void StreamProvider::Start() {
    if (m_started) return;
    m_started = true;
    Ncm().Initialize();
    m_signedIn = Ncm().LoggedIn();
    // The session reports sign-in changes and account refreshes the same way; only a
    // changed sign-in state is an account change, the rest refreshes library lists.
    Ncm().SetStateCallback([this] { OnSessionChanged(); });
    Ncm().SetLibraryCallback([this] { NotifyLibraryChanged(); });
    Ncm().SetLikesCallback([this] { NotifyFavoritesChanged(); });
}

void StreamProvider::Shutdown() {
    Ncm().SetStateCallback({});
    Ncm().SetLibraryCallback({});
    Ncm().SetLikesCallback({});
    Ncm().Shutdown();  // stops the helper and its workers
}

stream::Provider::Features StreamProvider::Offers() const {
    Features f;
    f.favorites = f.followArtists = f.saveAlbums = f.savePlaylists = f.editPlaylists = true;
    f.reorderPlaylists = f.recommended = f.artistTopTracks = true;
    return f;
}

std::vector<settings::Field> StreamProvider::SettingFields() {
    using Field = settings::Field;
    std::vector<Field> fields;
    Field account;
    account.kind = Field::Kind::Note;
    account.label = L"Account";
    account.note = [this] { return SignedIn() ? AccountName() : std::wstring(L"Not signed in"); };
    fields.push_back(std::move(account));

    Field signIn;
    signIn.kind = Field::Kind::Button;
    signIn.label = L"Sign in...";
    signIn.press = [this](HWND owner) { SignIn(owner); };
    fields.push_back(std::move(signIn));

    Field signOut;
    signOut.kind = Field::Kind::Button;
    signOut.label = L"Sign out";
    signOut.press = [this](HWND) {
        if (SignedIn()) SignOut();
    };
    fields.push_back(std::move(signOut));

    // The order of Quality's values.
    Field quality;
    quality.kind = Field::Kind::Choice;
    quality.label = L"Audio quality";
    quality.choices = {L"Best available", L"Lossless", L"Hi-Res", L"Very high (320 kbps)", L"Standard"};
    quality.defaultValue = static_cast<int>(Quality::Auto);
    quality.get = [] { return static_cast<int>(GetQuality()); };
    quality.set = [](int value) { SetQuality(static_cast<Quality>(value)); };
    fields.push_back(std::move(quality));

    Field cacheSize;
    cacheSize.label = L"Cover disk cache";
    cacheSize.unit = L"MB";
    cacheSize.min = kCoverCacheMinMb;
    cacheSize.max = kCoverCacheMaxMb;
    cacheSize.defaultValue = kCoverCacheDefaultMb;
    cacheSize.get = [] { return CoverCacheLimitMb(); };
    cacheSize.set = [](int mb) {
        SetCoverCacheLimitMb(mb);
        CoverCache::Get().Trim();
    };
    fields.push_back(std::move(cacheSize));

    Field usage;
    usage.kind = Field::Kind::Note;
    usage.label = L"Cover disk cache in use";
    usage.note = [] {
        const auto use = CoverCache::Get().CurrentUsage();
        wchar_t text[64];
        swprintf_s(text, L"%.1f MB, %zu ", static_cast<double>(use.bytes) / (1024.0 * 1024.0), use.files);
        return text + ui::Tr(L"covers");
    };
    fields.push_back(std::move(usage));

    Field clear;
    clear.kind = Field::Kind::Button;
    clear.label = L"Clear cover disk cache";
    clear.press = [](HWND) { CoverCache::Get().Clear(); };
    fields.push_back(std::move(clear));

    Field service;
    service.kind = Field::Kind::Text;
    service.label = L"Service address (empty: built-in)";
    service.restart = true;
    service.getText = [] { return GetServiceUrlOverride(); };
    service.setText = [](const std::wstring& url) { SetServiceUrlOverride(url); };
    fields.push_back(std::move(service));
    return fields;
}

bool StreamProvider::SignedIn() const { return Ncm().LoggedIn(); }

std::wstring StreamProvider::AccountName() const { return Ncm().CurrentAccount().nickname; }

void StreamProvider::SignIn(HWND owner) {
    ui::ShowNeteaseLogin(owner, [this] { OnSessionChanged(); });
}

void StreamProvider::OnSessionChanged() {
    stream::ResetSourceAvailability();
    const bool signedIn = Ncm().LoggedIn();
    if (signedIn == m_signedIn) return NotifyLibraryChanged();
    m_signedIn = signedIn;
    NotifyAccountChanged();
}

void StreamProvider::SignOut() { Ncm().Logout(); }

void StreamProvider::RefreshLibrary(Done done) { Ncm().RefreshAccount(Adapt(std::move(done))); }

stream::Id StreamProvider::TrackId(const metadb_handle_ptr& item) const {
    int64_t id = 0;
    return item.is_valid() && ParseSongPath(item->get_path(), id) ? Text(id) : Id();
}

std::wstring StreamProvider::Lyrics(const Id& track, abort_callback& abort) {
    std::wstring lrc, error;
    if (const int64_t id = Num(track)) Session::Get().Api().GetLyrics(id, abort, lrc, error);
    return lrc;
}

metadb_handle_ptr StreamProvider::TrackItem(const Id& track) const {
    const int64_t id = Num(track);
    return id ? Catalog::Get().Handle(id) : metadb_handle_ptr();
}

std::optional<stream::Track> StreamProvider::FindTrack(const Id& track) const {
    if (auto song = Catalog::Get().FindSong(Num(track))) return ToTrack(*song);
    return std::nullopt;
}

metadb_handle_ptr StreamProvider::ArtItem(stream::ArtKind kind, const Id& id) const {
    const int64_t n = Num(id);
    if (!n) return {};
    switch (kind) {
    case stream::ArtKind::Track: return Catalog::Get().Handle(n);
    case stream::ArtKind::Album: return Catalog::Get().AlbumHandle(n);
    case stream::ArtKind::Artist: return Catalog::Get().ArtistHandle(n);
    case stream::ArtKind::Playlist: return Catalog::Get().PlaylistHandle(n);
    }
    return {};
}

bool StreamProvider::SameCover(std::wstring_view a, std::wstring_view b) const {
    // NetEase serves one image under many "?param=WxH" size suffixes.
    const auto base = [](std::wstring_view url) { return url.substr(0, url.find(L'?')); };
    return base(a) == base(b);
}

bool StreamProvider::IsFavorite(const Id& track) const { return Ncm().IsLiked(Num(track)); }

std::vector<stream::Playlist> StreamProvider::Playlists(stream::Shelf shelf) const {
    switch (shelf) {
    case stream::Shelf::Created: return ToPlaylists(Ncm().CreatedPlaylists());
    case stream::Shelf::Saved: return ToPlaylists(Ncm().CollectedPlaylists());
    case stream::Shelf::Recommended: return ToPlaylists(Ncm().RecommendedPlaylists());
    }
    return {};
}

std::optional<stream::Shelf> StreamProvider::ShelfOf(const Id& playlist) const {
    switch (Ncm().FindPlaylistScope(Num(playlist))) {
    case Session::PlaylistScope::Created: return stream::Shelf::Created;
    case Session::PlaylistScope::Collected: return stream::Shelf::Saved;
    case Session::PlaylistScope::Recommended: return stream::Shelf::Recommended;
    case Session::PlaylistScope::Unknown: break;
    }
    return std::nullopt;
}

std::vector<stream::Artist> StreamProvider::FollowedArtists() const { return ToArtists(Ncm().FollowedArtists()); }

std::vector<stream::Album> StreamProvider::SavedAlbums() const { return ToAlbums(Ncm().CollectedAlbums()); }

std::optional<stream::Playlist> StreamProvider::FindPlaylist(const Id& playlist) const {
    if (auto found = Catalog::Get().FindPlaylist(Num(playlist))) return ToPlaylist(*found);
    return std::nullopt;
}

std::optional<stream::Album> StreamProvider::FindAlbum(const Id& album) const {
    if (auto found = Catalog::Get().FindAlbum(Num(album))) return ToAlbum(*found);
    return std::nullopt;
}

std::optional<std::vector<stream::Track>> StreamProvider::CachedFavorites() const {
    return ToTracks(Ncm().CachedLikedSongs());
}

std::optional<std::vector<stream::Track>> StreamProvider::CachedPlaylist(const Id& playlist) const {
    return ToTracks(Ncm().CachedPlaylistSongs(Num(playlist)));
}

std::optional<std::vector<stream::Track>> StreamProvider::CachedAlbum(const Id& album) const {
    return ToTracks(Ncm().CachedAlbumSongs(Num(album)));
}

void StreamProvider::FetchFavorites(TracksDone done) {
    Ncm().FetchLiked([done = std::move(done)](std::vector<Song> songs, std::wstring error) {
        done(ToTracks(songs), std::move(error));
    });
}

void StreamProvider::FetchPlaylists(stream::Shelf shelf, PlaylistsDone done) {
    if (shelf == stream::Shelf::Recommended) {
        Ncm().FetchRecommended([done = std::move(done)](std::vector<Playlist> playlists, std::wstring error) {
            done(ToPlaylists(playlists), std::move(error));
        });
        return;
    }
    // One request returns the created and the collected playlists together.
    Ncm().FetchMyPlaylists([shelf, done = std::move(done)](std::vector<Playlist> playlists, std::wstring error) {
        std::vector<stream::Playlist> out;
        for (const auto& playlist : playlists)
            if (IsOwn(playlist) == (shelf == stream::Shelf::Created)) out.push_back(ToPlaylist(playlist));
        done(std::move(out), std::move(error));
    });
}

void StreamProvider::FetchPlaylist(const Id& playlist, PlaylistDone done) {
    Ncm().FetchPlaylist(Num(playlist), [done = std::move(done)](PlaylistDetail detail, std::wstring error) {
        done(detail.info.id ? ToPlaylist(detail.info) : stream::Playlist{}, ToTracks(detail.songs), std::move(error));
    });
}

void StreamProvider::FetchAlbum(const Id& album, AlbumDone done) {
    Ncm().FetchAlbumDetail(Num(album),
                               [done = std::move(done)](Album info, std::vector<Song> songs, std::wstring error) {
                                   done(info.id ? ToAlbum(info) : stream::Album{}, ToTracks(songs), std::move(error));
                               });
}

void StreamProvider::FetchAlbumsTracks(std::vector<Id> albums, TracksDone done) {
    std::vector<Album> wanted;
    for (const int64_t id : Nums(albums)) {
        Album album;
        album.id = id;
        wanted.push_back(album);
    }
    Ncm().FetchAlbumTracks(std::move(wanted), [done = std::move(done)](std::vector<Song> songs, std::wstring error) {
        done(ToTracks(songs), std::move(error));
    });
}

void StreamProvider::FetchArtist(const Id& artist, ArtistDone done) {
    Ncm().FetchArtistDetail(Num(artist), [done = std::move(done)](Artist info, std::wstring error) {
        done(info.id ? ToArtist(info) : stream::Artist{}, std::move(error));
    });
}

void StreamProvider::FetchArtistBio(const Id& artist, TextDone done) {
    // The profile's brief is a summary; /artist/desc has the full article when it exists.
    Ncm().FetchArtistDescription(
        Num(artist), [done = std::move(done)](std::wstring brief, std::wstring full, std::wstring error) {
            done(full.empty() ? std::move(brief) : std::move(full), std::move(error));
        });
}

void StreamProvider::FetchArtistAlbums(const Id& artist, size_t offset, size_t limit, AlbumPageDone done) {
    Ncm().FetchArtistAlbumsPage(
        Num(artist), static_cast<int>(offset), static_cast<int>(limit),
        [artist = Num(artist), done = std::move(done)](std::vector<Album> albums, bool more, bool followedKnown,
                                                       bool followed, std::wstring error) {
            std::optional<bool> follows;
            if (followedKnown) {
                follows = followed;
                for (const auto& saved : Ncm().FollowedArtists())
                    if (saved.id == artist) follows = true;
            }
            done(ToAlbums(albums), more, follows, std::move(error));
        });
}

void StreamProvider::FetchArtistTopTracks(const Id& artist, size_t offset, size_t limit, TrackPageDone done) {
    Ncm().FetchArtistSongsPage(Num(artist), static_cast<int>(offset), static_cast<int>(limit),
                                   [done = std::move(done)](std::vector<Song> songs, bool more, std::wstring error) {
                                       done(ToTracks(songs), more, std::move(error));
                                   });
}

void StreamProvider::FetchFollowedArtists(ArtistsDone done) {
    Ncm().FetchFollowedArtists([done = std::move(done)](std::vector<Artist> artists, std::wstring error) {
        done(ToArtists(artists), std::move(error));
    });
}

void StreamProvider::FetchSavedAlbums(size_t offset, size_t limit, AlbumPageDone done) {
    Ncm().FetchCollectedAlbumsPage(
        static_cast<int>(offset), static_cast<int>(limit),
        [done = std::move(done)](std::vector<Album> albums, bool more, std::wstring error) {
            done(ToAlbums(albums), more, std::nullopt, std::move(error));
        });
}

void StreamProvider::SetFavorite(const Id& track, bool favorite, Done done) {
    Ncm().ToggleLike(Num(track), favorite, Adapt(std::move(done)));
}

void StreamProvider::SetFollowed(const Id& artist, bool follow, Done done) {
    Ncm().SetArtistFollow(Num(artist), follow, Adapt(std::move(done)));
}

void StreamProvider::SetAlbumSaved(const Id& album, bool saved, Done done) {
    Ncm().SetAlbumCollected(Num(album), saved, Adapt(std::move(done)));
}

void StreamProvider::SetPlaylistSaved(const Id& playlist, bool saved, Done done) {
    Ncm().SetPlaylistSubscribed(Num(playlist), saved, Adapt(std::move(done)));
}

void StreamProvider::AddToPlaylist(const Id& playlist, std::vector<Id> tracks, Done done) {
    Ncm().AddToPlaylist(Num(playlist), Nums(tracks), Adapt(std::move(done)));
}

void StreamProvider::RemoveFromPlaylist(const Id& playlist, std::vector<Id> tracks, Done done) {
    Ncm().RemoveFromPlaylist(Num(playlist), Nums(tracks), Adapt(std::move(done)));
}

void StreamProvider::CreatePlaylist(std::wstring name, CreatedDone done) {
    Ncm().CreatePlaylist(std::move(name), [done = std::move(done)](bool ok, int64_t id, std::wstring error) {
        if (!done) return;
        if (ok && id) done(Text(id), {});
        else done({}, error.empty() ? std::wstring(L"The request failed.") : std::move(error));
    });
}

void StreamProvider::RenamePlaylist(const Id& playlist, std::wstring name, Done done) {
    Ncm().RenamePlaylist(Num(playlist), std::move(name), Adapt(std::move(done)));
}

void StreamProvider::DeletePlaylist(const Id& playlist, Done done) {
    Ncm().DeletePlaylist(Num(playlist), Adapt(std::move(done)));
}

void StreamProvider::ReorderPlaylists(std::vector<Id> playlists, Done done) {
    Ncm().ReorderPlaylists(Nums(playlists), Adapt(std::move(done)));
}

}  // namespace glasswave::netease
