#include "pch.h"
#include "core/text.h"

#include "applemusic/catalog.h"
#include "core/paths.h"
#include "stream/provider.h"
#include "stream/list_session.h"
#include "ui/art_cache.h"
#include "ui/i18n.h"

#include <SDK/initquit.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>

#include <filesystem>
#include <fstream>

namespace glasswave::applemusic {
namespace {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
using stream::Album;
using stream::Artist;
using stream::BrowseGroup;
using stream::Id;
using stream::NavigationSection;
using stream::Playlist;
using stream::SectionKind;
using stream::Shelf;
using stream::Track;
using winrt::Windows::Data::Json::JsonArray;
using winrt::Windows::Data::Json::JsonObject;
using winrt::Windows::Data::Json::JsonValue;

constexpr double kCachedReadBudgetMs = 100.0;
constexpr size_t kArtistResolveAttemptLimit = 5;
constexpr size_t kArtistAvatarSampleLimit = 3;
constexpr size_t kNoCatalogAlbumSampleLimit = 2;
constexpr int kArtDecodeSizePx = 96;
constexpr uintmax_t kMaxRequestBytes = 4096;

struct Paths {
    fs::path request;
    fs::path claimed;
    fs::path result;
};

struct ProbeMarker {
    bool valid = false;
    Id libraryOnlyArtistId;
};

struct PlaylistProbe {
    size_t position = 0;
    int listCount = stream::kUnknownCount;
    size_t detailCount = 0;
    int detailDeclaredCount = stream::kUnknownCount;
    double initialMs = 0;
    double cachedMs = 0;
    bool countMatch = false;
    bool detailCountMatch = false;
    bool cachedSnapshotMatch = false;
    bool detailOk = false;
    std::wstring error;
};

struct ArtProbe {
    std::wstring kind;
    size_t position = 0;
    bool requested = false;
    bool resolved = false;
    bool decoded = false;
    uint32_t width = 0;
    uint32_t height = 0;
};

struct ArtTarget {
    std::wstring kind;
    size_t position = 0;
    std::wstring key;
    metadb_handle_ptr item;
    size_t resultIndex = 0;
};

struct State {
    explicit State(fs::path path) : result_path(std::move(path)) {}

    fs::path result_path;
    JsonObject result;
    std::vector<NavigationSection> sections;
    std::vector<Playlist> created;
    std::vector<Playlist> saved;
    std::vector<Album> savedAlbums;
    std::vector<Artist> followedArtists;
    std::vector<Track> librarySongs;
    std::optional<std::vector<Id>> favoriteIds;
    std::vector<PlaylistProbe> playlistProbes;
    std::vector<ArtProbe> artProbes;
    std::vector<ArtTarget> artTargets;
    std::vector<std::wstring> failures;
    Id songs, genres, composers, genreGroup, composerGroup;
    Id libraryOnlyArtistId;
    Artist selectedArtist;
    size_t playlistIndex = 0;
    size_t artistCandidateIndex = 0;
    int artListener = 0;
    bool artistResolved = false;
    bool artStarted = false;
    bool stop = false;
    bool done = false;
    bool pagedReadDone = false;
    bool pagedFirst = false;
    std::shared_ptr<stream::ListSession> pagedRead;
    size_t noCatalogLibrarySongCount = 0;
    size_t noCatalogAlbumSampleAvailableCount = 0;
};

using core::Utf8;

JsonValue Number(size_t value) {
    return JsonValue::CreateNumberValue(static_cast<double>(value));
}

JsonValue Number(int value) {
    return JsonValue::CreateNumberValue(static_cast<double>(value));
}

JsonValue Number(double value) {
    return JsonValue::CreateNumberValue(value);
}

double ElapsedMs(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

void AddFailure(const std::shared_ptr<State>& state, std::wstring code) {
    if (!state || state->stop || state->done || code.empty()) return;
    if (std::find(state->failures.begin(), state->failures.end(), code) == state->failures.end())
        state->failures.push_back(std::move(code));
}

void StopArt(const std::shared_ptr<State>& state) {
    if (!state || state->artListener == 0) return;
    if (auto* cache = ui::ArtCache::Existing()) cache->Unsubscribe(state->artListener);
    state->artListener = 0;
}

void WriteResult(const State& state) noexcept {
    try {
        const fs::path temporary = state.result_path.wstring() + L".tmp";
        const std::string bytes = Utf8(state.result.Stringify().c_str());
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        output.close();
        if (output) {
            MoveFileExW(temporary.c_str(), state.result_path.c_str(),
                        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
        }
    } catch (...) {
    }
}

void Finish(const std::shared_ptr<State>& state, std::wstring fatal = {}) {
    if (!state || state->stop || state->done) return;
    AddFailure(state, std::move(fatal));
    StopArt(state);

    JsonArray failures;
    for (const auto& code : state->failures)
        failures.Append(JsonValue::CreateStringValue(code));

    JsonArray playlistDetails;
    for (const auto& probe : state->playlistProbes) {
        JsonObject row;
        row.Insert(L"position", Number(probe.position));
        row.Insert(L"listCount", Number(probe.listCount));
        row.Insert(L"detailCount", Number(probe.detailCount));
        row.Insert(L"detailDeclaredCount", Number(probe.detailDeclaredCount));
        row.Insert(L"initialMs", Number(probe.initialMs));
        row.Insert(L"cachedMs", Number(probe.cachedMs));
        row.Insert(L"countMatch", JsonValue::CreateBooleanValue(probe.countMatch));
        row.Insert(L"detailCountMatch", JsonValue::CreateBooleanValue(probe.detailCountMatch));
        row.Insert(L"cachedSnapshotMatch", JsonValue::CreateBooleanValue(probe.cachedSnapshotMatch));
        row.Insert(L"ok", JsonValue::CreateBooleanValue(probe.detailOk));
        row.Insert(L"error", probe.error.empty() ? JsonValue::CreateNullValue()
                                                 : JsonValue::CreateStringValue(probe.error));
        playlistDetails.Append(row);
    }

    JsonArray artDecodes;
    for (const auto& probe : state->artProbes) {
        JsonObject row;
        row.Insert(L"kind", JsonValue::CreateStringValue(probe.kind));
        row.Insert(L"position", Number(probe.position));
        row.Insert(L"requested", JsonValue::CreateBooleanValue(probe.requested));
        row.Insert(L"decoded", JsonValue::CreateBooleanValue(probe.decoded));
        row.Insert(L"width", Number(static_cast<size_t>(probe.width)));
        row.Insert(L"height", Number(static_cast<size_t>(probe.height)));
        artDecodes.Append(row);
    }

    state->result.Insert(L"failureCodes", failures);
    state->result.Insert(L"playlistDetails", playlistDetails);
    state->result.Insert(L"playlistDetailCaseCount", Number(state->playlistProbes.size()));
    state->result.Insert(L"artDecodes", artDecodes);
    state->result.Insert(L"ok", JsonValue::CreateBooleanValue(state->failures.empty()));
    state->result.Insert(L"error", state->failures.empty()
                                       ? JsonValue::CreateStringValue(L"")
                                       : JsonValue::CreateStringValue(state->failures.front()));
    state->done = true;
    WriteResult(*state);
}

std::optional<Paths> ProbePaths() {
    const fs::path dir = core::ProfileDir(L"applemusic");
    if (dir.empty()) return std::nullopt;
    return Paths{dir / L"browse-request.json", dir / L"browse-request.claimed.json",
                 dir / L"browse-result.json"};
}

bool IsValidLibraryArtistId(std::wstring_view id) {
    if (id.size() <= 2 || id.size() > 256 || id[0] != L'r' || id[1] != L'.') return false;
    return std::all_of(id.begin() + 2, id.end(), [](wchar_t c) {
        return (c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z') || (c >= L'0' && c <= L'9') ||
               c == L'.' || c == L'_' || c == L'-';
    });
}

ProbeMarker ReadProbeMarker(const fs::path& path) {
    ProbeMarker marker;
    std::error_code ec;
    const uintmax_t size = fs::file_size(path, ec);
    if (ec || size == 0 || size > kMaxRequestBytes) return marker;

    std::ifstream input(path, std::ios::binary);
    std::string bytes(static_cast<size_t>(size), '\0');
    input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!input || static_cast<size_t>(input.gcount()) != bytes.size()) return marker;

    try {
        const JsonObject request = JsonObject::Parse(winrt::to_hstring(bytes));
        if (!request.HasKey(L"libraryOnlyArtistId")) {
            marker.valid = true;
            return marker;
        }
        const auto value = request.Lookup(L"libraryOnlyArtistId");
        if (!value || value.ValueType() != winrt::Windows::Data::Json::JsonValueType::String) return marker;
        marker.libraryOnlyArtistId = value.GetString().c_str();
        if (!IsValidLibraryArtistId(marker.libraryOnlyArtistId)) return marker;
        marker.valid = true;
        return marker;
    } catch (...) {
        return marker;
    }
}

const wchar_t* KindName(SectionKind kind) {
    switch (kind) {
    case SectionKind::Favorites:
        return L"favorites";
    case SectionKind::Artists:
        return L"artists";
    case SectionKind::Albums:
        return L"albums";
    case SectionKind::PlaylistShelf:
        return L"playlistShelf";
    case SectionKind::Tracks:
        return L"tracks";
    case SectionKind::Groups:
        return L"groups";
    }
    return L"tracks";
}

stream::Provider* Apple(const std::shared_ptr<State>& state) {
    if (!state || state->stop || state->done) return nullptr;
    auto* provider = stream::FindProvider(L"apple");
    if (!provider) Finish(state, L"apple_provider_missing");
    return provider;
}

const NavigationSection* FindSection(const State& state, std::wstring_view id, SectionKind kind) {
    auto found = std::find_if(state.sections.begin(), state.sections.end(),
                              [&](const auto& item) { return item.id == id; });
    if (found != state.sections.end()) return &*found;
    found = std::find_if(state.sections.begin(), state.sections.end(),
                         [&](const auto& item) { return item.kind == kind; });
    return found == state.sections.end() ? nullptr : &*found;
}

bool SamePlaylistSummary(const Playlist& a, const Playlist& b) {
    return a.id == b.id && a.name == b.name && a.coverUrl == b.coverUrl && a.creator == b.creator &&
           a.description == b.description && a.trackCount == b.trackCount && a.own == b.own &&
           a.saved == b.saved;
}

bool SamePlaylistSnapshot(const std::vector<Playlist>& a, const std::vector<Playlist>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (!SamePlaylistSummary(a[i], b[i])) return false;
    return true;
}

std::vector<Id> TrackIds(const std::vector<Track>& tracks) {
    std::vector<Id> ids;
    ids.reserve(tracks.size());
    for (const auto& track : tracks)
        ids.push_back(track.id);
    return ids;
}

template <typename T, typename Equal>
bool SameRows(const std::vector<T>& a, const std::vector<T>& b, Equal equal) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (!equal(a[i], b[i])) return false;
    return true;
}

bool SameSectionSnapshot(const std::vector<NavigationSection>& a, const std::vector<NavigationSection>& b) {
    return SameRows(a, b, [](const auto& x, const auto& y) {
        return x.id == y.id && x.label == y.label && x.kind == y.kind && x.shelf == y.shelf &&
               x.reorderChildren == y.reorderChildren;
    });
}

bool SameAlbumSnapshot(const std::vector<Album>& a, const std::vector<Album>& b) {
    return SameRows(a, b, [](const auto& x, const auto& y) {
        if (x.id != y.id || x.name != y.name || x.coverUrl != y.coverUrl || x.description != y.description ||
            x.trackCount != y.trackCount || x.year != y.year || x.artists.size() != y.artists.size())
            return false;
        for (size_t i = 0; i < x.artists.size(); ++i)
            if (x.artists[i].id != y.artists[i].id || x.artists[i].name != y.artists[i].name) return false;
        return true;
    });
}

bool SameArtistSnapshot(const std::vector<Artist>& a, const std::vector<Artist>& b) {
    return SameRows(a, b, [](const auto& x, const auto& y) {
        return x.id == y.id && x.name == y.name && x.avatarUrl == y.avatarUrl && x.bio == y.bio &&
               x.albumCount == y.albumCount && x.trackCount == y.trackCount &&
               x.followKnown == y.followKnown && x.followed == y.followed;
    });
}

bool SameFavoriteSnapshot(const std::optional<std::vector<Id>>& a, const std::optional<std::vector<Id>>& b) {
    return a.has_value() == b.has_value() && (!a || *a == *b);
}

bool SameAccountSnapshot(const State& state, stream::Provider& provider) {
    const auto favorites = provider.CachedFavorites();
    std::optional<std::vector<Id>> favoriteIds;
    if (favorites) favoriteIds = TrackIds(*favorites);
    return SameSectionSnapshot(state.sections, provider.Sections()) &&
           SamePlaylistSnapshot(state.created, provider.Playlists(Shelf::Created)) &&
           SamePlaylistSnapshot(state.saved, provider.Playlists(Shelf::Saved)) &&
           SameAlbumSnapshot(state.savedAlbums, provider.SavedAlbums()) &&
           SameArtistSnapshot(state.followedArtists, provider.FollowedArtists()) &&
           SameFavoriteSnapshot(state.favoriteIds, favoriteIds);
}

bool MatchesActualCount(int reported, size_t actual) {
    if (actual == 0) return reported == 0;
    return reported > 0 && static_cast<size_t>(reported) == actual;
}

void CaptureSnapshot(const std::shared_ptr<State>& state, stream::Provider& provider) {
    state->sections = provider.Sections();
    state->created = provider.Playlists(Shelf::Created);
    state->saved = provider.Playlists(Shelf::Saved);
    state->savedAlbums = provider.SavedAlbums();
    state->followedArtists = provider.FollowedArtists();
    if (const auto favorites = provider.CachedFavorites()) state->favoriteIds = TrackIds(*favorites);

    JsonArray sections;
    for (const auto& section : state->sections) {
        JsonObject item;
        item.Insert(L"id", JsonValue::CreateStringValue(section.id));
        item.Insert(L"label", JsonValue::CreateStringValue(ui::Tr(section.label)));
        item.Insert(L"kind", JsonValue::CreateStringValue(KindName(section.kind)));
        item.Insert(L"reorderChildren", JsonValue::CreateBooleanValue(section.reorderChildren));
        sections.Append(item);
    }

    JsonArray playlists;
    const auto add = [&](const std::vector<Playlist>& rows, const wchar_t* shelf) {
        for (const auto& playlist : rows) {
            JsonObject item;
            item.Insert(L"id", JsonValue::CreateStringValue(playlist.id));
            item.Insert(L"shelf", JsonValue::CreateStringValue(shelf));
            item.Insert(L"trackCount", Number(playlist.trackCount));
            playlists.Append(item);
        }
    };
    add(state->created, L"created");
    add(state->saved, L"saved");

    state->result.Insert(L"sections", sections);
    state->result.Insert(L"sectionCount", Number(state->sections.size()));
    state->result.Insert(L"createdCount", Number(state->created.size()));
    state->result.Insert(L"savedCount", Number(state->saved.size()));
    state->result.Insert(L"playlists", playlists);

    const auto* songs = FindSection(*state, L"songs", SectionKind::Tracks);
    const auto* genres = FindSection(*state, L"genres", SectionKind::Groups);
    const auto* composers = FindSection(*state, L"composers", SectionKind::Groups);
    if (!songs || !genres || !composers) {
        Finish(state, L"required_section_missing");
        return;
    }
    state->songs = songs->id;
    state->genres = genres->id;
    state->composers = composers->id;
}

void StartArtist(const std::shared_ptr<State>& state);

void BeginArt(const std::shared_ptr<State>& state);

void StartLibrarySectionChecks(const std::shared_ptr<State>& state);

void StartGenreDrilldown(const std::shared_ptr<State>& state);

void StartComposerDrilldown(const std::shared_ptr<State>& state);

void StartLibraryOnlyArtistCheck(const std::shared_ptr<State>& state);

void StartNextPlaylist(const std::shared_ptr<State>& state) {
    auto* provider = Apple(state);
    if (!provider) return;
    if (state->playlistIndex >= state->created.size()) {
        if (state->created.size() != 3) AddFailure(state, L"created_playlist_count_not_3");
        provider->FetchFavorites([state](std::vector<Track> tracks, std::wstring error) {
            if (!state || state->stop || state->done) return;
            state->result.Insert(L"favoriteCount", Number(tracks.size()));
            if (!error.empty())
                AddFailure(state, L"favorites_read_failed");
            else if (tracks.empty())
                AddFailure(state, L"favorites_empty");
            StartLibrarySectionChecks(state);
        });
        return;
    }

    const Playlist playlist = state->created[state->playlistIndex];
    const size_t position = state->playlistIndex + 1;
    if (playlist.id.empty()) {
        PlaylistProbe probe;
        probe.position = position;
        probe.listCount = playlist.trackCount;
        probe.error = L"playlist_id_missing";
        state->playlistProbes.push_back(probe);
        AddFailure(state, L"playlist_id_missing_" + std::to_wstring(position));
        ++state->playlistIndex;
        StartNextPlaylist(state);
        return;
    }

    if (playlist.trackCount > 100 && !state->pagedReadDone) {
        state->pagedReadDone = true;
        stream::ListQuery query;
        query.kind = stream::ListKind::PlaylistTracks;
        query.id = playlist.id;
        query.cache = stream::ListCachePolicy::None;
        const auto began = Clock::now();
        state->pagedRead = stream::ListSession::Open(
            *provider, std::move(query),
            [state, began, expected = playlist.trackCount](const stream::ListState& loaded) {
                if (state->stop || state->done) return;
                const auto* tracks = std::get_if<std::vector<Track>>(&loaded.page.rows);
                if (!tracks || !loaded.error.empty()) {
                    // Provider errors contain request diagnostics, never authentication data.
                    state->result.Insert(L"pagedReadError", JsonValue::CreateStringValue(loaded.error));
                    AddFailure(state, L"paged_playlist_read_failed");
                    state->pagedRead->Cancel();
                    state->pagedRead.reset();
                    StartNextPlaylist(state);
                    return;
                }
                if (loaded.loading || tracks->empty()) return;
                if (!state->pagedFirst) {
                    state->pagedFirst = true;
                    state->result.Insert(L"pagedFirstRows", Number(tracks->size()));
                    state->result.Insert(L"pagedFirstMs", Number(ElapsedMs(began)));
                    state->result.Insert(L"pagedFirstMore", JsonValue::CreateBooleanValue(!loaded.complete));
                    state->result.Insert(L"pagedFirstMetadataReadyRows", Number(loaded.metadataReadyRows));
                    if (tracks->size() > 100 || loaded.complete)
                        AddFailure(state, L"paged_playlist_did_not_publish_first_page");
                    state->pagedRead->SetMode(stream::ListLoadMode::All);
                }
                if (loaded.complete && !state->result.HasKey(L"pagedFinalRows")) {
                    state->result.Insert(L"pagedFinalRows", Number(tracks->size()));
                    state->result.Insert(L"pagedFinalMs", Number(ElapsedMs(began)));
                    state->result.Insert(L"pagedPages", Number(loaded.pages));
                    if (tracks->size() != static_cast<size_t>(expected))
                        AddFailure(state, L"paged_playlist_count_mismatch");
                }
                if (loaded.complete && loaded.metadataReadyRows >= tracks->size()) {
                    state->result.Insert(L"pagedMetadataFinalMs", Number(ElapsedMs(began)));
                    state->pagedRead->Cancel();
                    state->pagedRead.reset();
                    StartNextPlaylist(state);
                }
            });
        return;
    }

    const auto initialStart = Clock::now();
    provider->FetchPlaylist(playlist.id, [state, playlist, position, initialStart](
                                             Playlist info, std::vector<Track> tracks, std::wstring error) {
        if (!state || state->stop || state->done) return;
        PlaylistProbe probe;
        probe.position = position;
        probe.listCount = playlist.trackCount;
        probe.initialMs = ElapsedMs(initialStart);
        probe.detailCount = tracks.size();
        probe.detailDeclaredCount = info.trackCount;
        probe.countMatch = MatchesActualCount(playlist.trackCount, tracks.size());
        probe.detailCountMatch = MatchesActualCount(info.trackCount, tracks.size());
        if (!error.empty() || info.id.empty()) {
            probe.error = L"playlist_detail_read_failed";
            AddFailure(state, L"playlist_detail_read_failed_" + std::to_wstring(position));
            state->playlistProbes.push_back(std::move(probe));
            ++state->playlistIndex;
            StartNextPlaylist(state);
            return;
        }
        if (!probe.countMatch)
            AddFailure(state, L"playlist_list_count_mismatch_" + std::to_wstring(position));
        if (!probe.detailCountMatch)
            AddFailure(state, L"playlist_detail_count_mismatch_" + std::to_wstring(position));

        const auto cachedStart = Clock::now();
        const std::vector<Id> firstIds = TrackIds(tracks);
        auto* apple = Apple(state);
        if (!apple) return;
        apple->FetchPlaylist(playlist.id, [state, info, firstIds, probe = std::move(probe),
                                           cachedStart](Playlist cachedInfo, std::vector<Track> cachedTracks,
                                                        std::wstring cachedError) mutable {
            if (!state || state->stop || state->done) return;
            probe.cachedMs = ElapsedMs(cachedStart);
            probe.cachedSnapshotMatch = cachedError.empty() && SamePlaylistSummary(info, cachedInfo) &&
                                        TrackIds(cachedTracks) == firstIds;
            probe.detailOk = cachedError.empty() && probe.countMatch && probe.detailCountMatch &&
                             probe.cachedSnapshotMatch && probe.cachedMs < kCachedReadBudgetMs;
            if (!cachedError.empty()) {
                probe.error = L"playlist_cached_read_failed";
                AddFailure(state, L"playlist_cached_read_failed_" + std::to_wstring(probe.position));
            } else if (!probe.cachedSnapshotMatch) {
                probe.error = L"playlist_cached_snapshot_mismatch";
                AddFailure(state, L"playlist_cached_snapshot_mismatch_" + std::to_wstring(probe.position));
            } else if (probe.cachedMs >= kCachedReadBudgetMs) {
                probe.error = L"playlist_cached_read_slow";
                AddFailure(state, L"playlist_cached_read_slow_" + std::to_wstring(probe.position));
            }
            state->playlistProbes.push_back(std::move(probe));
            ++state->playlistIndex;
            StartNextPlaylist(state);
        });
    });
}

void StartLibrarySectionChecks(const std::shared_ptr<State>& state) {
    auto* provider = Apple(state);
    if (!provider) return;
    provider->FetchSectionTracks(state->songs, L"", [state](std::vector<Track> songs, std::wstring error) {
        if (!state || state->stop || state->done) return;
        state->result.Insert(L"librarySongCount", Number(songs.size()));
        if (error.empty()) state->librarySongs = songs;
        if (!error.empty())
            AddFailure(state, L"library_songs_read_failed");
        else if (songs.empty())
            AddFailure(state, L"library_songs_empty");

        auto* afterSongs = Apple(state);
        if (!afterSongs) return;
        afterSongs->FetchGroups(
            state->genres, [state](std::vector<BrowseGroup> genres, std::wstring genresError) {
                if (!state || state->stop || state->done) return;
                state->result.Insert(L"genreCount", Number(genres.size()));
                if (!genresError.empty())
                    AddFailure(state, L"genres_read_failed");
                else if (genres.empty())
                    AddFailure(state, L"genres_empty");
                if (!genres.empty()) state->genreGroup = genres.front().id;

                auto* afterGenres = Apple(state);
                if (!afterGenres) return;
                afterGenres->FetchGroups(state->composers, [state](std::vector<BrowseGroup> composers,
                                                                   std::wstring composersError) {
                    if (!state || state->stop || state->done) return;
                    state->result.Insert(L"composerCount", Number(composers.size()));
                    if (!composersError.empty())
                        AddFailure(state, L"composers_read_failed");
                    else if (composers.empty())
                        AddFailure(state, L"composers_empty");
                    if (!composers.empty()) state->composerGroup = composers.front().id;
                    StartGenreDrilldown(state);
                });
            });
    });
}

void StartGenreDrilldown(const std::shared_ptr<State>& state) {
    auto* provider = Apple(state);
    if (!provider) return;
    if (state->genreGroup.empty()) {
        AddFailure(state, L"genre_sample_missing");
        StartComposerDrilldown(state);
        return;
    }
    provider->FetchSectionTracks(state->genres, state->genreGroup,
                                 [state](std::vector<Track> tracks, std::wstring error) {
                                     if (!state || state->stop || state->done) return;
                                     state->result.Insert(L"genreTrackCount", Number(tracks.size()));
                                     if (!error.empty())
                                         AddFailure(state, L"genre_tracks_read_failed");
                                     else if (tracks.empty())
                                         AddFailure(state, L"genre_tracks_empty");
                                     StartComposerDrilldown(state);
                                 });
}

void StartComposerDrilldown(const std::shared_ptr<State>& state) {
    auto* provider = Apple(state);
    if (!provider) return;
    if (state->composerGroup.empty()) {
        AddFailure(state, L"composer_sample_missing");
        StartArtist(state);
        return;
    }
    provider->FetchSectionTracks(state->composers, state->composerGroup,
                                 [state](std::vector<Track> tracks, std::wstring error) {
                                     if (!state || state->stop || state->done) return;
                                     state->result.Insert(L"composerTrackCount", Number(tracks.size()));
                                     if (!error.empty())
                                         AddFailure(state, L"composer_tracks_read_failed");
                                     else if (tracks.empty())
                                         AddFailure(state, L"composer_tracks_empty");
                                     StartArtist(state);
                                 });
}

void StartCachedPlaylistRefresh(const std::shared_ptr<State>& state, stream::Provider& provider) {
    const auto start = Clock::now();
    provider.FetchPlaylists(
        Shelf::Created, [state, start](std::vector<Playlist> playlists, std::wstring error) {
            if (!state || state->stop || state->done) return;
            const double elapsed = ElapsedMs(start);
            bool snapshotMatch = false;
            if (error.empty()) {
                if (auto* apple = Apple(state)) snapshotMatch = SameAccountSnapshot(*state, *apple);
            }
            const bool playlistOrderMatch = error.empty() && SamePlaylistSnapshot(state->created, playlists);
            state->result.Insert(L"cachedFetchPlaylistsMs", Number(elapsed));
            state->result.Insert(L"cachedFetchPlaylistsSnapshotMatch",
                                 JsonValue::CreateBooleanValue(snapshotMatch));
            state->result.Insert(L"cachedFetchPlaylistsOrderMatch",
                                 JsonValue::CreateBooleanValue(playlistOrderMatch));
            state->result.Insert(L"cachedReadBudgetMs", Number(kCachedReadBudgetMs));
            if (!error.empty()) {
                AddFailure(state, L"fetch_playlists_cache_failed");
            } else {
                if (!snapshotMatch) AddFailure(state, L"fetch_playlists_cache_snapshot_mismatch");
                if (elapsed >= kCachedReadBudgetMs) AddFailure(state, L"fetch_playlists_cache_slow");
            }
            StartNextPlaylist(state);
        });
}

void TryArtist(const std::shared_ptr<State>& state);

void StartArtist(const std::shared_ptr<State>& state) {
    auto* provider = Apple(state);
    if (!provider) return;
    state->followedArtists = provider->FollowedArtists();
    state->result.Insert(L"followedArtistCount", Number(state->followedArtists.size()));
    if (state->followedArtists.empty()) {
        AddFailure(state, L"artist_list_empty");
        BeginArt(state);
        return;
    }
    TryArtist(state);
}

void TryArtist(const std::shared_ptr<State>& state) {
    auto* provider = Apple(state);
    if (!provider) return;
    if (state->artistCandidateIndex >= state->followedArtists.size() ||
        state->artistCandidateIndex >= kArtistResolveAttemptLimit) {
        AddFailure(state, L"catalog_resolvable_artist_missing");
        BeginArt(state);
        return;
    }

    const Artist candidate = state->followedArtists[state->artistCandidateIndex];
    state->result.Insert(L"artistResolveAttemptCount", Number(state->artistCandidateIndex + 1));
    provider->FetchArtist(candidate.id, [state](Artist artist, std::wstring error) {
        if (!state || state->stop || state->done) return;
        if (!error.empty() || artist.id.empty()) {
            ++state->artistCandidateIndex;
            TryArtist(state);
            return;
        }

        state->artistResolved = true;
        state->selectedArtist = artist;
        state->result.Insert(L"artistResolved", JsonValue::CreateBooleanValue(true));
        state->result.Insert(L"artistAvatarPresent",
                             JsonValue::CreateBooleanValue(!artist.avatarUrl.empty()));
        state->result.Insert(L"artistAlbumCount", Number(artist.albumCount));
        if (artist.avatarUrl.empty()) AddFailure(state, L"artist_avatar_missing");
        if (artist.albumCount <= 0) AddFailure(state, L"artist_album_count_zero");

        auto* apple = Apple(state);
        if (!apple) return;
        apple->FetchArtistAlbums(
            artist.id, 0, 24,
            [state, albumCount = artist.albumCount](std::vector<Album> albums, bool more, std::optional<bool>,
                                                    std::wstring albumsError) {
                if (!state || state->stop || state->done) return;
                state->result.Insert(L"artistAlbumFirstPageCount", Number(albums.size()));
                state->result.Insert(L"artistAlbumFirstPageMore", JsonValue::CreateBooleanValue(more));
                state->result.Insert(L"artistAlbumReadError",
                                     albumsError.empty()
                                         ? JsonValue::CreateStringValue(L"")
                                         : JsonValue::CreateStringValue(L"artist_albums_read_failed"));
                if (!albumsError.empty())
                    AddFailure(state, L"artist_albums_read_failed");
                else if (albums.empty())
                    AddFailure(state, L"artist_albums_empty");
                if (albumsError.empty() && !more) {
                    const bool countMatch =
                        albumCount > 0 && static_cast<size_t>(albumCount) == albums.size();
                    state->result.Insert(L"artistAlbumCountMatch", JsonValue::CreateBooleanValue(countMatch));
                    if (!countMatch) AddFailure(state, L"artist_album_count_mismatch");
                } else {
                    state->result.Insert(L"artistAlbumCountMatch", JsonValue::CreateNullValue());
                }

                auto* afterAlbums = Apple(state);
                if (!afterAlbums) return;
                afterAlbums->FetchArtistTopTracks(
                    state->selectedArtist.id, 0, 10,
                    [state](std::vector<Track> tracks, bool more, std::wstring tracksError) {
                        if (!state || state->stop || state->done) return;
                        state->result.Insert(L"artistTopTrackCount", Number(tracks.size()));
                        state->result.Insert(L"artistTopTrackMore", JsonValue::CreateBooleanValue(more));
                        state->result.Insert(L"artistTopTrackReadError",
                                             tracksError.empty() ? JsonValue::CreateStringValue(L"")
                                                                 : JsonValue::CreateStringValue(
                                                                       L"artist_top_tracks_read_failed"));
                        if (!tracksError.empty())
                            AddFailure(state, L"artist_top_tracks_read_failed");
                        else if (tracks.empty())
                            AddFailure(state, L"artist_top_tracks_empty");
                        StartLibraryOnlyArtistCheck(state);
                    });
            });
    });
}

void StartLibraryOnlyArtistCheck(const std::shared_ptr<State>& state) {
    auto* provider = Apple(state);
    if (!provider) return;
    if (state->libraryOnlyArtistId.empty()) {
        state->result.Insert(L"libraryOnlyArtistRequested", JsonValue::CreateBooleanValue(false));
        state->result.Insert(L"libraryOnlyArtistSkipped", JsonValue::CreateBooleanValue(true));
        state->result.Insert(L"libraryOnlyArtistAccepted", JsonValue::CreateBooleanValue(true));
        BeginArt(state);
        return;
    }

    state->result.Insert(L"libraryOnlyArtistRequested", JsonValue::CreateBooleanValue(true));
    state->result.Insert(L"libraryOnlyArtistSkipped", JsonValue::CreateBooleanValue(false));
    provider->FetchArtist(state->libraryOnlyArtistId, [state](Artist artist, std::wstring error) {
        if (!state || state->stop || state->done) return;
        const bool resolved = error.empty() && !artist.id.empty();
        state->result.Insert(L"libraryOnlyArtistResolved", JsonValue::CreateBooleanValue(resolved));
        state->result.Insert(L"libraryOnlyArtistAlbumCount", Number(artist.albumCount));
        state->result.Insert(L"libraryOnlyArtistFetchError",
                             resolved ? JsonValue::CreateStringValue(L"")
                                      : JsonValue::CreateStringValue(L"library_only_artist_fetch_failed"));
        if (!resolved) AddFailure(state, L"library_only_artist_fetch_failed");

        auto* apple = Apple(state);
        if (!apple) return;
        apple->FetchArtistAlbums(
            state->libraryOnlyArtistId, 0, 100,
            [state, artist](std::vector<Album> albums, bool more, std::optional<bool>,
                            std::wstring albumsError) {
                if (!state || state->stop || state->done) return;
                const bool pageComplete = albumsError.empty() && !more;
                const bool countKnown = artist.albumCount > 0;
                const bool countMatch =
                    pageComplete && countKnown && static_cast<size_t>(artist.albumCount) == albums.size();
                const bool accepted = albumsError.empty() && !albums.empty() && pageComplete && countKnown &&
                                      countMatch && artist.id == state->libraryOnlyArtistId;
                state->result.Insert(L"libraryOnlyArtistFirstPageCount", Number(albums.size()));
                state->result.Insert(L"libraryOnlyArtistFirstPageMore", JsonValue::CreateBooleanValue(more));
                state->result.Insert(L"libraryOnlyArtistPageComplete",
                                     JsonValue::CreateBooleanValue(pageComplete));
                state->result.Insert(L"libraryOnlyArtistCountKnown",
                                     JsonValue::CreateBooleanValue(countKnown));
                state->result.Insert(L"libraryOnlyArtistCountMatch",
                                     JsonValue::CreateBooleanValue(countMatch));
                state->result.Insert(L"libraryOnlyArtistAccepted", JsonValue::CreateBooleanValue(accepted));
                state->result.Insert(L"libraryOnlyArtistAlbumsReadError",
                                     albumsError.empty() ? JsonValue::CreateStringValue(L"")
                                                         : JsonValue::CreateStringValue(
                                                               L"library_only_artist_albums_read_failed"));

                if (!albumsError.empty())
                    AddFailure(state, L"library_only_artist_albums_read_failed");
                else if (albums.empty())
                    AddFailure(state, L"library_only_artist_albums_empty");
                else if (!pageComplete)
                    AddFailure(state, L"library_only_artist_page_incomplete");
                else if (!countKnown)
                    AddFailure(state, L"library_only_artist_album_count_unknown");
                else if (!countMatch)
                    AddFailure(state, L"library_only_artist_album_count_mismatch");
                else if (artist.id != state->libraryOnlyArtistId)
                    AddFailure(state, L"library_only_artist_id_mismatch");

                auto* tracksProvider = Apple(state);
                if (!tracksProvider) return;
                tracksProvider->FetchArtistTopTracks(
                    state->libraryOnlyArtistId, 0, 100,
                    [state, artist](std::vector<Track> tracks, bool more, std::wstring tracksError) {
                        if (!state || state->stop || state->done) return;
                        const bool albumIdsPresent =
                            std::all_of(tracks.begin(), tracks.end(), [](const Track& track) {
                                return track.albumId.size() > 2 && track.albumId[0] == L'l' &&
                                       track.albumId[1] == L'.';
                            });
                        const bool unranked =
                            std::all_of(tracks.begin(), tracks.end(),
                                        [](const Track& track) { return track.popularity == 0; });
                        const bool creditsPresent =
                            std::all_of(tracks.begin(), tracks.end(), [&artist](const Track& track) {
                                return std::any_of(
                                    track.artists.begin(), track.artists.end(),
                                    [&artist](const stream::ArtistRef& credit) {
                                        return (!credit.id.empty() && credit.id == artist.id) ||
                                               (!artist.name.empty() && credit.name == artist.name);
                                    });
                            });
                        const bool accepted = tracksError.empty() && !tracks.empty() && albumIdsPresent &&
                                              unranked && creditsPresent;
                        state->result.Insert(L"libraryOnlyArtistTrackCount", Number(tracks.size()));
                        state->result.Insert(L"libraryOnlyArtistTrackMore",
                                             JsonValue::CreateBooleanValue(more));
                        state->result.Insert(
                            L"libraryOnlyArtistTrackReadError",
                            tracksError.empty()
                                ? JsonValue::CreateStringValue(L"")
                                : JsonValue::CreateStringValue(L"library_only_artist_tracks_read_failed"));
                        state->result.Insert(L"libraryOnlyArtistAlbumIdsPresent",
                                             JsonValue::CreateBooleanValue(albumIdsPresent));
                        state->result.Insert(L"libraryOnlyArtistTracksUnranked",
                                             JsonValue::CreateBooleanValue(unranked));
                        state->result.Insert(L"libraryOnlyArtistCreditsPresent",
                                             JsonValue::CreateBooleanValue(creditsPresent));
                        state->result.Insert(L"libraryOnlyArtistTracksAccepted",
                                             JsonValue::CreateBooleanValue(accepted));
                        if (!tracksError.empty())
                            AddFailure(state, L"library_only_artist_tracks_read_failed");
                        else if (tracks.empty())
                            AddFailure(state, L"library_only_artist_tracks_empty");
                        else if (!albumIdsPresent)
                            AddFailure(state, L"library_only_artist_album_ids_missing");
                        else if (!unranked)
                            AddFailure(state, L"library_only_artist_tracks_fake_ranked");
                        else if (!creditsPresent)
                            AddFailure(state, L"library_only_artist_unrelated_track");
                        BeginArt(state);
                    });
            });
    });
}

void RecordArtResult(ArtProbe& probe, ui::ArtCache::State state,
                     const winrt::com_ptr<IWICBitmapSource>& bitmap) {
    probe.requested = true;
    probe.resolved = state != ui::ArtCache::State::Pending;
    probe.decoded = state == ui::ArtCache::State::Ready && bitmap;
    probe.width = probe.height = 0;
    if (probe.decoded) {
        UINT width = 0, height = 0;
        if (SUCCEEDED(bitmap->GetSize(&width, &height))) {
            probe.width = width;
            probe.height = height;
        } else {
            probe.decoded = false;
        }
    }
}

void RequestArt(const std::shared_ptr<State>& state, size_t index) {
    if (!state || state->stop || state->done || index >= state->artTargets.size()) return;
    auto& target = state->artTargets[index];
    auto& probe = state->artProbes[target.resultIndex];
    if (!target.item.is_valid()) {
        probe.requested = true;
        probe.resolved = true;
        return;
    }
    // ArtCache extracts through album_art_manager_v2 and decodes with WIC on its workers.
    winrt::com_ptr<IWICBitmapSource> bitmap;
    const auto visual = ui::ArtCache::Get().Find(target.key, target.item, kArtDecodeSizePx, &bitmap);
    RecordArtResult(probe, visual, bitmap);
}

void EvaluateArt(const std::shared_ptr<State>& state) {
    if (!state || state->stop || state->done || !state->artStarted) return;
    bool allResolved = true;
    for (size_t i = 0; i < state->artTargets.size(); ++i) {
        auto& probe = state->artProbes[state->artTargets[i].resultIndex];
        if (!probe.resolved) RequestArt(state, i);
        if (!probe.resolved) allResolved = false;
    }
    if (!allResolved) return;

    size_t playlistDecoded = 0;
    size_t artistRequested = 0;
    size_t artistDecoded = 0;
    size_t noCatalogRequested = 0;
    size_t noCatalogDecoded = 0;
    for (const auto& probe : state->artProbes) {
        if (probe.kind == L"playlist") {
            if (probe.decoded)
                ++playlistDecoded;
            else
                AddFailure(state, L"playlist_cover_decode_failed_" + std::to_wstring(probe.position));
        } else if (probe.kind == L"artist") {
            if (probe.requested) ++artistRequested;
            if (probe.decoded)
                ++artistDecoded;
            else
                AddFailure(state, L"artist_avatar_decode_failed_" + std::to_wstring(probe.position));
        } else if (probe.kind == L"noCatalogAlbum") {
            if (probe.requested) ++noCatalogRequested;
            if (probe.decoded)
                ++noCatalogDecoded;
            else
                AddFailure(state, L"no_catalog_album_cover_decode_failed_" + std::to_wstring(probe.position));
        }
    }
    state->result.Insert(L"playlistCoverDecodedCount", Number(playlistDecoded));
    state->result.Insert(L"artistAvatarSampleRequestedCount", Number(artistRequested));
    state->result.Insert(L"artistAvatarSampleDecodedCount", Number(artistDecoded));
    state->result.Insert(L"noCatalogAlbumRequestedCount", Number(noCatalogRequested));
    state->result.Insert(L"noCatalogAlbumDecodedCount", Number(noCatalogDecoded));
    if (playlistDecoded != state->created.size()) AddFailure(state, L"playlist_cover_decode_incomplete");
    if (artistRequested != artistDecoded) AddFailure(state, L"artist_avatar_decode_incomplete");
    if (state->noCatalogLibrarySongCount > 0 &&
        state->noCatalogAlbumSampleAvailableCount < kNoCatalogAlbumSampleLimit)
        AddFailure(state, L"no_catalog_album_sample_incomplete");
    if (noCatalogRequested != noCatalogDecoded) AddFailure(state, L"no_catalog_album_decode_incomplete");
    Finish(state);
}

void BeginArt(const std::shared_ptr<State>& state) {
    auto* provider = Apple(state);
    if (!provider) return;
    state->artStarted = true;

    for (size_t i = 0; i < state->created.size(); ++i) {
        const auto& playlist = state->created[i];
        ArtProbe probe;
        probe.kind = L"playlist";
        probe.position = i + 1;
        const size_t resultIndex = state->artProbes.size();
        state->artProbes.push_back(std::move(probe));
        state->artTargets.push_back({L"playlist", i + 1,
                                     provider->ArtKey(stream::ArtKind::Playlist, playlist.id),
                                     provider->ArtItem(stream::ArtKind::Playlist, playlist.id), resultIndex});
    }

    std::vector<Artist> avatarArtists;
    std::set<Id> seen;
    const auto addAvatar = [&](const Artist& artist) {
        if (avatarArtists.size() >= kArtistAvatarSampleLimit || artist.id.empty() ||
            artist.avatarUrl.empty() || !seen.emplace(artist.id).second)
            return;
        avatarArtists.push_back(artist);
    };
    if (state->artistResolved) addAvatar(state->selectedArtist);
    for (const auto& artist : state->followedArtists)
        addAvatar(artist);

    state->result.Insert(L"artistAvatarSampleAvailableCount", Number(avatarArtists.size()));
    for (size_t i = 0; i < avatarArtists.size(); ++i) {
        ArtProbe probe;
        probe.kind = L"artist";
        probe.position = i + 1;
        const size_t resultIndex = state->artProbes.size();
        state->artProbes.push_back(std::move(probe));
        state->artTargets.push_back(
            {L"artist", i + 1, provider->ArtKey(stream::ArtKind::Artist, avatarArtists[i].id),
             provider->ArtItem(stream::ArtKind::Artist, avatarArtists[i].id), resultIndex});
    }

    std::vector<Id> noCatalogSongSampleIds;
    std::vector<Id> noCatalogAlbumIds;
    std::set<Id> seenNoCatalogAlbums;
    for (const auto& track : state->librarySongs) {
        if (!IsLibraryId(track.id) || track.id[0] != L'i') continue;
        const auto song = Catalog::Get().FindSong(track.id);
        if (!song || !song->catalogId.empty()) continue;
        ++state->noCatalogLibrarySongCount;
        if (noCatalogSongSampleIds.size() < kNoCatalogAlbumSampleLimit)
            noCatalogSongSampleIds.push_back(track.id);
        if (song->coverUrl.empty() || !IsLibraryId(song->track.albumId) || song->track.albumId[0] != L'l')
            continue;
        if (noCatalogAlbumIds.size() < kNoCatalogAlbumSampleLimit &&
            seenNoCatalogAlbums.insert(song->track.albumId).second)
            noCatalogAlbumIds.push_back(song->track.albumId);
    }
    state->noCatalogAlbumSampleAvailableCount = noCatalogAlbumIds.size();
    JsonArray noCatalogSamples;
    for (const auto& id : noCatalogSongSampleIds)
        noCatalogSamples.Append(JsonValue::CreateStringValue(id));
    state->result.Insert(L"noCatalogLibrarySongCount", Number(state->noCatalogLibrarySongCount));
    state->result.Insert(L"noCatalogSongSampleIds", noCatalogSamples);
    state->result.Insert(L"noCatalogAlbumSampleAvailableCount",
                         Number(state->noCatalogAlbumSampleAvailableCount));
    for (size_t i = 0; i < noCatalogAlbumIds.size(); ++i) {
        ArtProbe probe;
        probe.kind = L"noCatalogAlbum";
        probe.position = i + 1;
        const size_t resultIndex = state->artProbes.size();
        state->artProbes.push_back(std::move(probe));
        state->artTargets.push_back(
            {L"noCatalogAlbum", i + 1, provider->ArtKey(stream::ArtKind::Album, noCatalogAlbumIds[i]),
             provider->ArtItem(stream::ArtKind::Album, noCatalogAlbumIds[i]), resultIndex});
    }

    if (state->artTargets.empty()) {
        AddFailure(state, L"art_decode_targets_missing");
        Finish(state);
        return;
    }

    state->artListener = ui::ArtCache::Get().Subscribe([state] { EvaluateArt(state); });
    EvaluateArt(state);
}

void Continue(const std::shared_ptr<State>& state, unsigned step) {
    auto* provider = Apple(state);
    if (!provider) return;
    if (step == 0) {
        provider->Start();
        provider->RefreshLibrary([state](std::wstring error) {
            if (!state || state->stop || state->done) return;
            if (!error.empty()) return Finish(state, L"provider_refresh_failed");
            auto* apple = Apple(state);
            if (!apple) return;
            CaptureSnapshot(state, *apple);
            if (state->done) return;
            StartCachedPlaylistRefresh(state, *apple);
        });
    } else {
        Finish(state, L"invalid_probe_step");
    }
}

class ProbeLifecycle : public initquit {
  public:
    void on_init() override {
        const auto paths = ProbePaths();
        if (!paths) return;
        std::error_code ec;
        if (!fs::is_regular_file(paths->request, ec) || ec) return;
        if (!MoveFileExW(paths->request.c_str(), paths->claimed.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            return;
        const ProbeMarker marker = ReadProbeMarker(paths->claimed);
        DeleteFileW(paths->claimed.c_str());
        DeleteFileW(paths->result.c_str());

        auto state = std::make_shared<State>(paths->result);
        state->libraryOnlyArtistId = marker.libraryOnlyArtistId;
        state->result.Insert(L"ok", JsonValue::CreateBooleanValue(false));
        state->result.Insert(L"error", JsonValue::CreateStringValue(L""));
        state->result.Insert(L"sections", JsonArray());
        state->result.Insert(L"sectionCount", JsonValue::CreateNumberValue(0));
        state->result.Insert(L"createdCount", JsonValue::CreateNumberValue(0));
        state->result.Insert(L"savedCount", JsonValue::CreateNumberValue(0));
        state->result.Insert(L"favoriteCount", JsonValue::CreateNumberValue(0));
        state->result.Insert(L"librarySongCount", JsonValue::CreateNumberValue(0));
        state->result.Insert(L"genreCount", JsonValue::CreateNumberValue(0));
        state->result.Insert(L"composerCount", JsonValue::CreateNumberValue(0));
        state->result.Insert(L"genreTrackCount", JsonValue::CreateNumberValue(0));
        state->result.Insert(L"composerTrackCount", JsonValue::CreateNumberValue(0));
        state->result.Insert(L"playlists", JsonArray());
        state->result.Insert(L"favoriteLeak", JsonValue::CreateNullValue());
        state->result.Insert(L"cachedFetchPlaylistsMs", JsonValue::CreateNumberValue(0));
        state->result.Insert(L"cachedFetchPlaylistsSnapshotMatch", JsonValue::CreateBooleanValue(false));
        state->result.Insert(L"cachedFetchPlaylistsOrderMatch", JsonValue::CreateBooleanValue(false));
        state->result.Insert(L"cachedReadBudgetMs", JsonValue::CreateNullValue());
        state->result.Insert(L"followedArtistCount", JsonValue::CreateNumberValue(0));
        state->result.Insert(L"artistResolveAttemptCount", JsonValue::CreateNumberValue(0));
        state->result.Insert(L"artistResolved", JsonValue::CreateBooleanValue(false));
        state->result.Insert(L"artistAvatarPresent", JsonValue::CreateBooleanValue(false));
        state->result.Insert(L"artistAlbumCount", JsonValue::CreateNumberValue(stream::kUnknownCount));
        state->result.Insert(L"artistAlbumFirstPageCount", JsonValue::CreateNumberValue(0));
        state->result.Insert(L"artistAlbumFirstPageMore", JsonValue::CreateBooleanValue(false));
        state->result.Insert(L"artistAlbumCountMatch", JsonValue::CreateNullValue());
        state->result.Insert(L"artistAlbumReadError", JsonValue::CreateStringValue(L""));
        state->result.Insert(L"artistTopTrackCount", JsonValue::CreateNumberValue(0));
        state->result.Insert(L"artistTopTrackMore", JsonValue::CreateBooleanValue(false));
        state->result.Insert(L"artistTopTrackReadError", JsonValue::CreateStringValue(L""));
        state->result.Insert(L"artistAvatarSampleAvailableCount", JsonValue::CreateNumberValue(0));
        state->result.Insert(L"artistAvatarSampleRequestedCount", JsonValue::CreateNumberValue(0));
        state->result.Insert(L"artistAvatarSampleDecodedCount", JsonValue::CreateNumberValue(0));
        state->result.Insert(L"noCatalogLibrarySongCount", JsonValue::CreateNumberValue(0));
        state->result.Insert(L"noCatalogSongSampleIds", JsonArray());
        state->result.Insert(L"noCatalogAlbumSampleAvailableCount", JsonValue::CreateNumberValue(0));
        state->result.Insert(L"noCatalogAlbumRequestedCount", JsonValue::CreateNumberValue(0));
        state->result.Insert(L"noCatalogAlbumDecodedCount", JsonValue::CreateNumberValue(0));
        state->result.Insert(L"playlistCoverDecodedCount", JsonValue::CreateNumberValue(0));
        state->result.Insert(L"artDecodePipeline",
                             JsonValue::CreateStringValue(L"album_art_manager_v2+wic-background"));
        state->result.Insert(L"libraryOnlyArtistRequested", JsonValue::CreateBooleanValue(false));
        state->result.Insert(L"libraryOnlyArtistSkipped", JsonValue::CreateBooleanValue(true));
        state->result.Insert(L"libraryOnlyArtistResolved", JsonValue::CreateBooleanValue(false));
        state->result.Insert(L"libraryOnlyArtistAccepted", JsonValue::CreateBooleanValue(false));
        state->result.Insert(L"libraryOnlyArtistAlbumCount",
                             JsonValue::CreateNumberValue(stream::kUnknownCount));
        state->result.Insert(L"libraryOnlyArtistFirstPageCount", JsonValue::CreateNumberValue(0));
        state->result.Insert(L"libraryOnlyArtistFirstPageMore", JsonValue::CreateBooleanValue(false));
        state->result.Insert(L"libraryOnlyArtistPageComplete", JsonValue::CreateBooleanValue(false));
        state->result.Insert(L"libraryOnlyArtistCountKnown", JsonValue::CreateBooleanValue(false));
        state->result.Insert(L"libraryOnlyArtistCountMatch", JsonValue::CreateBooleanValue(false));
        state->result.Insert(L"libraryOnlyArtistFetchError", JsonValue::CreateStringValue(L""));
        state->result.Insert(L"libraryOnlyArtistAlbumsReadError", JsonValue::CreateStringValue(L""));
        state->result.Insert(L"libraryOnlyArtistTrackCount", JsonValue::CreateNumberValue(0));
        state->result.Insert(L"libraryOnlyArtistTrackMore", JsonValue::CreateBooleanValue(false));
        state->result.Insert(L"libraryOnlyArtistTrackReadError", JsonValue::CreateStringValue(L""));
        state->result.Insert(L"libraryOnlyArtistAlbumIdsPresent", JsonValue::CreateBooleanValue(false));
        state->result.Insert(L"libraryOnlyArtistTracksUnranked", JsonValue::CreateBooleanValue(false));
        state->result.Insert(L"libraryOnlyArtistCreditsPresent", JsonValue::CreateBooleanValue(false));
        state->result.Insert(L"libraryOnlyArtistTracksAccepted", JsonValue::CreateBooleanValue(false));
        if (!marker.valid) {
            Finish(state, L"invalid_probe_marker");
            return;
        }
        m_state = state;
        fb2k::inMainThread([state] { Continue(state, 0); });
    }

    void on_quit() override {
        if (m_state) {
            m_state->stop = true;
            if (m_state->pagedRead) m_state->pagedRead->Cancel();
            m_state->pagedRead.reset();
            StopArt(m_state);
        }
        m_state.reset();
    }

  private:
    std::shared_ptr<State> m_state;
};

FB2K_SERVICE_FACTORY(ProbeLifecycle);

} // namespace
} // namespace glasswave::applemusic
