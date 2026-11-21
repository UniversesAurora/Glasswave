#include "pch.h"
#include "ui/search_page.h"
#include "core/text.h"

#include "media/library_model.h"
#include "stream/list_session.h"
#include "ui/i18n.h"
#include "ui/remote_grids.h"
#include "ui/item_menu.h"
#include "ui/song_actions.h"
#include "ui/remote_pages.h"
#include "ui/song_table.h"
#include "ui/text.h"
#include "ui/win7_menu.h"

#include <variant>

namespace glasswave::ui {

using core::Wide;

namespace {

using Value = std::variant<stream::Track, stream::Artist, stream::Album, stream::Playlist>;

// One result: a provider's record, or a local one (a file, an artist or album of the library, a
// foobar2000 playlist) with the file that stands for it.
struct Result {
    Value value;
    stream::Provider* provider = nullptr; // nullptr: local
    metadb_handle_ptr local;              // local: the file (a song) or one of its files (cover)
};

// The tabs, in order, and the search each one shows.
struct TabDef {
    const wchar_t* key;
    const wchar_t* label;
    stream::ListKind kind;
};
constexpr TabDef kTabs[] = {
    {L"all", L"All results", stream::ListKind::CatalogSearchTopResults},
    {L"songs", L"Songs", stream::ListKind::CatalogSearch},
    {L"artists", L"Artists", stream::ListKind::CatalogSearchArtists},
    {L"albums", L"Albums", stream::ListKind::CatalogSearchAlbums},
    {L"playlists", L"Playlists", stream::ListKind::CatalogSearchPlaylists},
    {L"lyrics", L"Lyrics", stream::ListKind::CatalogSearchLyrics},
};

// The overview's groups: what each shows, its heading and the tab its link opens.
// Each is a shelf that pages sideways (Apple Music's search page): the top results and the songs
// as small cards three rows high, the others a row of covers.
struct SectionDef {
    stream::ListKind kind;
    const wchar_t* label;
    const wchar_t* tab; // empty: no link
    size_t lines;       // rows of tiles
    media::TileStyle tiles;
};
constexpr SectionDef kOverview[] = {
    {stream::ListKind::CatalogSearchTopResults, L"Top results", L"", 3, media::TileStyle::Song},
    {stream::ListKind::CatalogSearchArtists, L"Artists", L"artists", 1, media::TileStyle::Album},
    {stream::ListKind::CatalogSearchAlbums, L"Albums", L"albums", 1, media::TileStyle::Album},
    {stream::ListKind::CatalogSearch, L"Songs", L"songs", 3, media::TileStyle::Song},
    {stream::ListKind::CatalogSearchPlaylists, L"Playlists", L"playlists", 1, media::TileStyle::Album},
};

const TabDef* FindTab(std::wstring_view key) {
    for (const auto& tab : kTabs)
        if (key == tab.key) return &tab;
    return nullptr;
}

// The sources a scope searches: the local library (nullptr) first, then the providers.
std::vector<stream::Provider*> ScopeSources(const SearchScope& scope) {
    std::vector<stream::Provider*> sources;
    if (scope.kind == SearchScope::Kind::All || scope.kind == SearchScope::Kind::Local) sources.push_back(nullptr);
    if (scope.kind == SearchScope::Kind::All)
        for (auto* provider : stream::Providers())
            if (provider->Can(stream::Capability::CatalogSearch)) sources.push_back(provider);
    if (auto* provider = ScopeProvider(scope)) sources.push_back(provider);
    return sources;
}

// What the local library can be searched for: songs, artists, albums and playlists.
bool LocalSearches(stream::ListKind kind) {
    return kind == stream::ListKind::CatalogSearch || kind == stream::ListKind::CatalogSearchArtists ||
           kind == stream::ListKind::CatalogSearchAlbums || kind == stream::ListKind::CatalogSearchPlaylists;
}

bool Searches(const stream::Provider* source, stream::ListKind kind) {
    if (!source) return LocalSearches(kind);
    const auto kinds = source->CatalogSearchKinds();
    return std::find(kinds.begin(), kinds.end(), kind) != kinds.end();
}

constexpr size_t kLocalLimit = 500; // a local category lists this many at most

// The local library's results of one kind, in the library's order.
std::vector<Result> LocalResults(const media::LibraryModel& library, stream::ListKind kind, const std::wstring& term) {
    std::vector<Result> items;
    const std::wstring needle = media::Lower(term);
    const auto groupItems = [&](media::GroupKind groupKind) {
        for (const auto& group : library.Groups(groupKind)) {
            if (items.size() >= kLocalLimit) break;
            if (!media::MatchesFilter(media::Lower(group.name + L" " + group.secondary), needle)) continue;
            Result item;
            item.local = group.tracks.empty() ? metadb_handle_ptr() : library.Tracks()[group.tracks.front()].handle;
            if (groupKind == media::GroupKind::Artist) {
                stream::Artist artist;
                artist.id = group.key;
                artist.name = group.name;
                artist.trackCount = static_cast<int>(group.tracks.size());
                item.value = std::move(artist);
            } else {
                stream::Album album;
                album.id = group.key;
                album.name = group.name;
                if (!group.secondary.empty()) album.artists.push_back({{}, group.secondary});
                album.year = _wtoi(group.year.c_str());
                album.trackCount = static_cast<int>(group.tracks.size());
                item.value = std::move(album);
            }
            items.push_back(std::move(item));
        }
    };
    switch (kind) {
    case stream::ListKind::CatalogSearch:
        for (const auto& track : library.Tracks()) {
            if (items.size() >= kLocalLimit) break;
            if (!media::MatchesFilter(track.search, needle)) continue;
            Result item;
            stream::Track value;
            value.title = track.title;
            value.album = track.album;
            if (!track.artist.empty()) value.artists.push_back({{}, track.artist});
            value.durationMs = static_cast<int>(track.seconds * 1000);
            item.value = std::move(value);
            item.local = track.handle;
            items.push_back(std::move(item));
        }
        break;
    case stream::ListKind::CatalogSearchArtists: groupItems(media::GroupKind::Artist); break;
    case stream::ListKind::CatalogSearchAlbums: groupItems(media::GroupKind::Album); break;
    case stream::ListKind::CatalogSearchPlaylists: {
        auto pm = playlist_manager::get();
        for (t_size p = 0, n = pm->get_playlist_count(); p < n; ++p) {
            pfc::string8 name;
            pm->playlist_get_name(p, name);
            const std::wstring wide = Wide(name.get_ptr());
            if (!media::MatchesFilter(media::Lower(wide), needle)) continue;
            stream::Playlist playlist;
            playlist.id = std::to_wstring(p);
            playlist.name = wide;
            playlist.trackCount = static_cast<int>(pm->playlist_get_item_count(p));
            Result item;
            item.value = std::move(playlist);
            if (pm->playlist_get_item_count(p)) item.local = pm->playlist_get_item_handle(p, 0);
            items.push_back(std::move(item));
        }
        break;
    }
    default: break;
    }
    return items;
}

// A provider's rows as results.
std::vector<Result> ProviderResults(stream::Provider& provider, const stream::ListRows& rows) {
    std::vector<Result> items;
    std::visit(
        [&](const auto& list) {
            using Row = typename std::decay_t<decltype(list)>::value_type;
            for (const auto& row : list) {
                if constexpr (std::is_same_v<Row, stream::CatalogSearchItem>) {
                    std::visit([&](const auto& v) { items.push_back({Value(v), &provider, {}}); }, row);
                } else if constexpr (std::is_same_v<Row, stream::Track> || std::is_same_v<Row, stream::Artist> ||
                                     std::is_same_v<Row, stream::Album> || std::is_same_v<Row, stream::Playlist>) {
                    items.push_back({Value(row), &provider, {}});
                }
            }
        },
        rows);
    return items;
}

// One source's results of one kind: a provider's paged search, or the local library's list.
struct Lane {
    stream::Provider* provider = nullptr;
    stream::ListKind kind = stream::ListKind::CatalogSearch;
    std::shared_ptr<stream::ListSession> session;
    std::vector<Result> items;
    std::optional<size_t> total;
    bool more = false, loading = false;
    std::wstring error;
};

// Several sources' results by rank: each one's first, then each one's second... A source with
// more pages to come holds the others back at its length, so rows already shown never move.
std::vector<const Result*> Mixed(const std::vector<const Lane*>& lanes) {
    size_t limit = 0;
    bool held = false;
    for (const Lane* lane : lanes) {
        if (lane->more || lane->loading) {
            limit = held ? std::min(limit, lane->items.size()) : lane->items.size();
            held = true;
        }
    }
    if (!held)
        for (const Lane* lane : lanes) limit = std::max(limit, lane->items.size());
    std::vector<const Result*> out;
    for (size_t rank = 0; rank < limit; ++rank)
        for (const Lane* lane : lanes)
            if (rank < lane->items.size()) out.push_back(&lane->items[rank]);
    return out;
}

std::wstring Artists(const std::vector<stream::ArtistRef>& artists) {
    return stream::JoinArtists(artists);
}

// ---- the tiles ---------------------------------------------------------------------------------

// The overview's groups or one category of artists, albums or playlists as tiles.
class ResultTiles final : public GridSource {
  public:
    struct Group {
        std::wstring key, label, tab; // tab: the link's tab; empty: no link
        size_t lines = 0;             // rows of tiles; 0: all
        media::TileStyle tiles = media::TileStyle::None;
        std::vector<Result> items;
        std::optional<size_t> total;
        bool more = false;
    };
    struct Actions {
        std::function<void(const Result&, const std::vector<Result>& group)> activate;
        std::function<void(POINT screen, const Result&)> menu;
        std::function<void(const std::wstring& tab)> openTab;
        std::function<void(const Result&, const std::vector<Result>& group)> play;
    };

    ResultTiles(bool mixed, bool grouped, std::wstring emptyText, Actions actions)
        : GridSource({{L"Name", 0, true}}, std::move(emptyText)), m_mixed(mixed), m_grouped(grouped),
          m_actions(std::move(actions)) {}

    void SetGroups(std::vector<Group> groups) {
        m_data = std::move(groups);
        m_entries.clear();
        for (size_t g = 0; g < m_data.size(); ++g)
            for (size_t i = 0; i < m_data[g].items.size(); ++i) m_entries.push_back({g, i});
        Refilter();
        BuildGroups();
    }
    void SetEmpty(std::wstring text) { SetEmptyText(std::move(text)); }

    std::wstring ColumnSetKey() const override { return L"search-tiles"; }
    std::wstring Cell(size_t row, size_t column) const override {
        return row < m_rows.size() ? ItemCell(m_rows[row], column) : std::wstring();
    }
    // Covers, or (Expanded tile, a category's own tab) a cover beside three lines.
    media::TileStyle Tiles() const override {
        return m_style == media::ViewStyle::Tiles ? media::TileStyle::Artist : media::TileStyle::Album;
    }
    const std::vector<media::RowGroup>* Groups() const override { return m_grouped ? &m_groups : nullptr; }
    bool GroupArt() const override { return false; }
    void ShowGroupMore(size_t group) override {
        if (group >= m_groups.size()) return;
        for (const auto& data : m_data)
            if (data.key == m_groups[group].key && !data.tab.empty() && m_actions.openTab) m_actions.openTab(data.tab);
    }
    bool CanSort(size_t) const override { return false; }
    bool SupportsViewStyle(media::ViewStyle style) const override {
        return style == media::ViewStyle::Icons || (!m_grouped && style == media::ViewStyle::Tiles);
    }
    media::ViewStyle GetViewStyle() const override { return m_style; }
    void SetViewStyle(media::ViewStyle style) override {
        if (SupportsViewStyle(style)) m_style = style;
    }
    bool CanPlayTile(size_t row) const override {
        const auto* item = ItemAt(row);
        return item && !std::holds_alternative<stream::Artist>(item->value);
    }
    void PlayTile(size_t row) override {
        const auto* item = ItemAt(row);
        if (item && m_actions.play) m_actions.play(*item, m_data[m_entries[m_rows[row]].group].items);
    }

    media::TileInfo Tile(size_t row) const override {
        media::TileInfo tile;
        const Result* item = ItemAt(row);
        if (!item) return tile;
        const bool top = m_grouped && m_data[m_entries[m_rows[row]].group].key == L"top";
        stream::Provider* p = item->provider;
        std::visit(
            [&](const auto& v) {
                using T = std::decay_t<decltype(v)>;
                const std::wstring count =
                    [&] {
                        if constexpr (std::is_same_v<T, stream::Track>)
                            return std::wstring();
                        else
                            return v.trackCount > 0 ? std::to_wstring(v.trackCount) + Tr(L" songs") : std::wstring();
                    }();
                if constexpr (std::is_same_v<T, stream::Track>) {
                    tile.art = p ? (v.albumId.empty() ? p->TrackItem(v.id) : p->ArtItem(stream::ArtKind::Album, v.albumId))
                                 : item->local;
                    tile.lines = {v.title, Kind(top, L"Song", Artists(v.artists)), v.album};
                    tile.badges = v.badges;
                } else if constexpr (std::is_same_v<T, stream::Album>) {
                    tile.art = p ? p->ArtItem(stream::ArtKind::Album, v.id) : item->local;
                    std::wstring year = v.year ? std::to_wstring(v.year) : std::wstring();
                    if (!count.empty()) year += (year.empty() ? L"" : L" \u00B7 ") + count;
                    tile.lines = {v.name, Kind(top, L"Album", Artists(v.artists)), year};
                } else if constexpr (std::is_same_v<T, stream::Playlist>) {
                    if (p && !v.coverUrl.empty())
                        tile.art = p->ArtItem(stream::ArtKind::Playlist, v.id);
                    else
                        tile.icon = 29722; // WMP's large playlist artwork
                    tile.lines = {v.name, Kind(top, L"Playlist", v.creator), count};
                } else {
                    tile.art = p ? p->ArtItem(stream::ArtKind::Artist, v.id) : item->local;
                    tile.lines = {v.name, top ? Tr(L"Artist") : std::wstring(), count};
                }
            },
            item->value);
        if (m_mixed) tile.source = p ? std::wstring(p->Key()) : L"local";
        return tile;
    }

    std::wstring ArtKey(size_t row) const override {
        const Result* item = ItemAt(row);
        if (!item || !item->provider) return {};
        stream::Provider& p = *item->provider;
        return std::visit(
            [&](const auto& v) -> std::wstring {
                using T = std::decay_t<decltype(v)>;
                if constexpr (std::is_same_v<T, stream::Track>)
                    return v.albumId.empty() ? p.ArtKey(stream::ArtKind::Track, v.id)
                                             : p.ArtKey(stream::ArtKind::Album, v.albumId);
                else if constexpr (std::is_same_v<T, stream::Album>)
                    return p.ArtKey(stream::ArtKind::Album, v.id);
                else if constexpr (std::is_same_v<T, stream::Playlist>)
                    return p.ArtKey(stream::ArtKind::Playlist, v.id);
                else
                    return p.ArtKey(stream::ArtKind::Artist, v.id);
            },
            item->value);
    }

    metadb_handle_ptr Item(size_t row) const override {
        const auto* item = ItemAt(row);
        return item ? SongOf(*item) : metadb_handle_ptr();
    }
    metadb_handle_list SelectedItems() const override {
        metadb_handle_list items;
        for (size_t row = 0; row < m_rows.size(); ++row)
            if (IsSelected(row))
                if (auto song = Item(row); song.is_valid()) items.add_item(song);
        return items;
    }
    void Activate(size_t row) override {
        const auto* item = ItemAt(row);
        if (item && m_actions.activate) m_actions.activate(*item, m_data[m_entries[m_rows[row]].group].items);
    }
    void ContextMenu(HWND owner, POINT screen) override {
        const auto* item = ItemAt(Focus());
        if (!item) return;
        if (std::holds_alternative<stream::Track>(item->value)) {
            metadb_handle_list items = SelectedItems();
            if (!items.get_count())
                if (auto song = Item(Focus()); song.is_valid()) items.add_item(song);
            if (items.get_count()) RunContextMenu(owner, items, screen);
            return;
        }
        if (m_actions.menu) m_actions.menu(screen, *item);
    }
    void SetFilter(std::wstring_view filterLower) override {
        GridSource::SetFilter(filterLower);
        BuildGroups();
    }

    static metadb_handle_ptr SongOf(const Result& item) {
        const auto* track = std::get_if<stream::Track>(&item.value);
        if (!track) return {};
        return item.provider ? item.provider->TrackItem(track->id) : item.local;
    }

  private:
    struct Entry {
        size_t group = 0, index = 0;
    };

    static std::wstring Kind(bool top, const wchar_t* kind, const std::wstring& detail) {
        if (!top) return detail;
        return detail.empty() ? Tr(kind) : Tr(kind) + L" · " + detail;
    }

    const Result* ItemAt(size_t row) const {
        if (row >= m_rows.size()) return nullptr;
        const Entry& e = m_entries[m_rows[row]];
        return &m_data[e.group].items[e.index];
    }

    size_t ItemCount() const override { return m_entries.size(); }
    std::wstring ItemCell(size_t index, size_t) const override {
        const Entry& e = m_entries[index];
        return std::visit(
            [](const auto& v) -> std::wstring {
                if constexpr (std::is_same_v<std::decay_t<decltype(v)>, stream::Track>)
                    return v.title;
                else
                    return v.name;
            },
            m_data[e.group].items[e.index].value);
    }
    std::wstring FilterText(size_t index) const override { return ItemCell(index, 0); }
    std::wstring Identity(size_t index) const override {
        const Entry& e = m_entries[index];
        const auto& item = m_data[e.group].items[e.index];
        const std::wstring source = item.provider ? std::wstring(item.provider->Key()) : L"local";
        const std::wstring id = std::visit([](const auto& v) { return v.id; }, item.value);
        return m_data[e.group].key + L"|" + source + L"|" + std::to_wstring(item.value.index()) + L"|" +
               (id.empty() && item.local.is_valid() ? Wide(item.local->get_path()) : id);
    }

    void BuildGroups() {
        m_groups.clear();
        if (!m_grouped) return;
        for (size_t row = 0; row < m_rows.size(); ++row) {
            const size_t g = m_entries[m_rows[row]].group;
            const Group& data = m_data[g];
            if (m_groups.empty() || m_groups.back().key != data.key) {
                media::RowGroup group;
                group.key = data.key;
                group.first = row;
                group.header = Tr(data.label);
                group.tileLines = data.lines;
                group.tiles = data.tiles;
                group.carousel = true;
                if (!data.tab.empty()) {
                    size_t n = data.items.size();
                    if (data.total) n = std::max(n, *data.total);
                    group.more = Tr(L"Show all") + L" (" + std::to_wstring(n) +
                                 (data.more && !data.total ? L"+" : L"") + L")";
                }
                m_groups.push_back(std::move(group));
            }
            ++m_groups.back().count;
        }
    }

    bool m_mixed = false, m_grouped = false;
    media::ViewStyle m_style = media::ViewStyle::Icons;
    Actions m_actions;
    std::vector<Group> m_data;
    std::vector<Entry> m_entries;
    std::vector<media::RowGroup> m_groups;
};

// ---- the page ----------------------------------------------------------------------------------

class SearchPage final : public Page {
  public:
    SearchPage(PageHost& host, View view) : Page(host, std::move(view)) {
        m_scope = SearchViewScope(m_view);
        m_term = m_view.remoteId;
        m_sources = ScopeSources(m_scope);
        for (const auto& tab : kTabs)
            if (std::any_of(m_sources.begin(), m_sources.end(),
                            [&](stream::Provider* s) { return tab.kind == stream::ListKind::CatalogSearchTopResults ||
                                                              Searches(s, tab.kind); }))
                m_tabs.push_back(&tab);
        m_tab = FindTab(m_view.sectionId);
        if (!m_tab || std::find(m_tabs.begin(), m_tabs.end(), m_tab) == m_tabs.end()) m_tab = &kTabs[0];
    }

    std::unique_ptr<media::ListSource> Start(bool) override {
        m_lanes.clear();
        m_tiles = nullptr;
        m_songs = nullptr;
        const bool mixed = m_sources.size() > 1;
        const auto openLane = [&](stream::Provider* source, stream::ListKind kind) {
            auto lane = std::make_unique<Lane>();
            lane->provider = source;
            lane->kind = kind;
            if (!source) {
                lane->items = LocalResults(m_host.Library(), kind, m_term);
                lane->total = lane->items.size();
            } else {
                stream::ListQuery query;
                query.kind = kind;
                query.search = m_term;
                query.cache = stream::ListCachePolicy::None;
                Lane* raw = lane.get();
                lane->session = stream::ListSession::Open(*source, std::move(query),
                                                          Guard([this, raw](const stream::ListState& state) {
                                                              Take(*raw, state);
                                                              Publish();
                                                          }));
                Take(*lane, lane->session->View());
            }
            m_lanes.push_back(std::move(lane));
        };
        if (m_tab->kind == stream::ListKind::CatalogSearchTopResults) {
            for (const auto& section : kOverview)
                for (auto* source : m_sources)
                    if (Searches(source, section.kind)) openLane(source, section.kind);
        } else {
            for (auto* source : m_sources)
                if (Searches(source, m_tab->kind)) openLane(source, m_tab->kind);
        }

        std::unique_ptr<media::ListSource> source;
        const bool songs = m_tab->kind == stream::ListKind::CatalogSearch ||
                           m_tab->kind == stream::ListKind::CatalogSearchLyrics;
        if (songs) {
            auto table = std::make_unique<SearchSongSource>(std::vector<SearchSongSource::Hit>{}, mixed,
                                                            m_tab->kind == stream::ListKind::CatalogSearchLyrics,
                                                            Tr(L"Searching..."));
            m_songs = table.get();
            source = std::move(table);
        } else {
            ResultTiles::Actions actions;
            actions.activate = Guard([this](const Result& item, const std::vector<Result>& group) { Open(item, group); });
            actions.menu = Guard([this](POINT screen, const Result& item) { Menu(screen, item); });
            actions.play = Guard([this](const Result& item, const std::vector<Result>& group) { Play(item, group); });
            // Called from the list's click on the link: the navigation destroys this source, so it
            // runs after the click has been handled.
            actions.openTab = [alive = LifetimeToken(), this](const std::wstring& tab) {
                fb2k::inMainThread([alive, this, tab] {
                    if (!alive.expired()) SelectTab(tab);
                });
            };
            auto tiles = std::make_unique<ResultTiles>(
                mixed, m_tab->kind == stream::ListKind::CatalogSearchTopResults, Tr(L"Searching..."),
                std::move(actions));
            m_tiles = tiles.get();
            source = std::move(tiles);
        }
        m_shown = source.get();
        Fill();
        return source;
    }

    void NearEnd() override {
        if (m_tab->kind == stream::ListKind::CatalogSearchTopResults) return;
        for (auto& lane : m_lanes)
            if (lane->session && lane->more && !lane->loading) lane->session->RequestNext();
    }

    std::vector<PageTab> Tabs() const override {
        std::vector<PageTab> tabs;
        for (const TabDef* tab : m_tabs) tabs.push_back({tab->key, Tr(tab->label)});
        return tabs;
    }
    std::wstring Tab() const override { return m_tab->key; }
    void SelectTab(const std::wstring& key) override {
        if (key == m_tab->key || !FindTab(key)) return;
        m_host.Open(SearchView(m_scope, m_term, key));
    }

    bool CanRefresh() const override { return true; }

    void AddSongActions(std::vector<MenuAction>& actions, const metadb_handle_list& items) override {
        if (!m_songs || items.get_count() != 1) return;
        std::wstring lyrics = m_songs->LyricsOf(items[0]);
        if (lyrics.empty()) return;
        actions.push_back({Tr(L"Copy lyrics"), [this, lyrics] { CopyText(lyrics); }});
    }

  private:
    void Take(Lane& lane, const stream::ListState& state) {
        lane.items = ProviderResults(*lane.provider, state.page.rows);
        lane.total = state.page.total;
        lane.loading = state.loading || state.refreshing;
        lane.more = !state.complete && state.error.empty();
        lane.error = state.error;
    }

    std::vector<const Lane*> LanesOf(stream::ListKind kind) const {
        std::vector<const Lane*> lanes;
        for (const auto& lane : m_lanes)
            if (lane->kind == kind) lanes.push_back(lane.get());
        return lanes;
    }

    // Whether any lane is still reading its first page, and the first error when none has rows.
    std::wstring Status(const std::vector<const Lane*>& lanes) const {
        std::wstring error;
        for (const Lane* lane : lanes) {
            if (lane->loading && lane->items.empty()) return Tr(L"Searching...");
            if (error.empty() && !lane->error.empty()) error = lane->error;
        }
        return error.empty() ? Tr(L"No results.") : Tr(error);
    }

    void Publish() {
        if (m_host.Source() != m_shown) return;
        m_host.BeforeListChange();
        Fill();
        m_host.ListChanged();
    }

    void Fill() {
        std::vector<const Lane*> all;
        for (const auto& lane : m_lanes) all.push_back(lane.get());
        if (m_songs) {
            std::vector<SearchSongSource::Hit> hits;
            for (const Result* item : Mixed(all)) {
                SearchSongSource::Hit hit;
                hit.provider = item->provider;
                if (const auto* track = std::get_if<stream::Track>(&item->value)) hit.track = *track;
                hit.local = item->local;
                hits.push_back(std::move(hit));
            }
            m_songs->SetEmptyText(Status(all));
            m_songs->ReplaceHits(hits);
            return;
        }
        if (!m_tiles) return;
        std::vector<ResultTiles::Group> groups;
        const auto group = [&](stream::ListKind kind, std::wstring key, std::wstring label, std::wstring tab,
                               size_t lines, media::TileStyle tiles) {
            const auto lanes = LanesOf(kind);
            ResultTiles::Group g;
            g.key = std::move(key);
            g.label = std::move(label);
            g.tab = std::move(tab);
            g.lines = lines;
            g.tiles = tiles;
            size_t total = 0;
            bool known = true;
            for (const Lane* lane : lanes) {
                g.more |= lane->more;
                if (lane->total)
                    total += *lane->total;
                else
                    known = false;
            }
            if (known && !lanes.empty()) g.total = total;
            for (const Result* item : Mixed(lanes)) g.items.push_back(*item);
            if (!g.items.empty()) groups.push_back(std::move(g));
        };
        if (m_tab->kind == stream::ListKind::CatalogSearchTopResults) {
            for (const auto& section : kOverview)
                group(section.kind, section.kind == stream::ListKind::CatalogSearchTopResults ? L"top" : section.tab,
                      section.label, section.tab, section.lines, section.tiles);
        } else {
            group(m_tab->kind, m_tab->key, L"", L"", 0, media::TileStyle::None);
        }
        m_tiles->SetEmpty(Status(all));
        m_tiles->SetGroups(std::move(groups));
    }

    // A song plays its group from it; anything else opens its page.
    void Open(const Result& item, const std::vector<Result>& group) {
        if (std::holds_alternative<stream::Track>(item.value)) {
            metadb_handle_list songs;
            size_t start = 0;
            for (const auto& other : group) {
                auto song = ResultTiles::SongOf(other);
                if (!song.is_valid()) continue;
                if (&other == &item) start = songs.get_count();
                songs.add_item(song);
            }
            if (songs.get_count()) media::PlayItems(songs, start);
            return;
        }
        if (auto view = ViewOf(item)) m_host.Open(*view);
    }

    // A tile's play button: a song plays its group from it, an album or a playlist all its songs.
    void Play(const Result& item, const std::vector<Result>& group) {
        const auto play = [](metadb_handle_list items) {
            if (items.get_count()) media::PlayItems(items, 0);
        };
        std::visit(
            [&](const auto& v) {
                using T = std::decay_t<decltype(v)>;
                if constexpr (std::is_same_v<T, stream::Track>) {
                    Open(item, group);
                } else if constexpr (std::is_same_v<T, stream::Album>) {
                    if (stream::Provider* p = item.provider) {
                        PlayRemoteAlbum(*p, v.id);
                    } else if (const auto* g = m_host.Library().FindGroup(media::GroupKind::Album, v.id)) {
                        metadb_handle_list items;
                        for (size_t t : g->tracks) items.add_item(m_host.Library().Tracks()[t].handle);
                        play(items);
                    }
                } else if constexpr (std::is_same_v<T, stream::Playlist>) {
                    if (stream::Provider* p = item.provider) {
                        PlayRemotePlaylist(*p, v.id);
                    } else {
                        auto pm = playlist_manager::get();
                        const auto index = static_cast<t_size>(_wtoi64(v.id.c_str()));
                        metadb_handle_list items;
                        if (index < pm->get_playlist_count()) pm->playlist_get_all_items(index, items);
                        play(items);
                    }
                }
            },
            item.value);
    }

    // Opened through the host, the page's breadcrumb leads back to these results (View::from).
    std::optional<View> ViewOf(const Result& item) const { return ViewOfItem(item); }

    std::optional<View> ViewOfItem(const Result& item) const {
        return std::visit(
            [&](const auto& v) -> std::optional<View> {
                using T = std::decay_t<decltype(v)>;
                if constexpr (std::is_same_v<T, stream::Track>) {
                    if (item.provider && item.provider->Can(stream::Capability::SongDetails))
                        return SongView(*item.provider, v.id, v.title);
                    return std::nullopt;
                } else if constexpr (std::is_same_v<T, stream::Playlist>) {
                    if (item.provider) return PlaylistView(*item.provider, v.id, v.name, std::nullopt);
                    View view;
                    view.library = View::Library::All;
                    view.kind = View::Kind::Playlist;
                    view.playlist = static_cast<size_t>(_wtoi64(v.id.c_str()));
                    return view;
                } else {
                    constexpr bool artist = std::is_same_v<T, stream::Artist>;
                    if (item.provider)
                        return artist ? ArtistView(*item.provider, v.id, v.name)
                                      : AlbumView(*item.provider, v.id, v.name);
                    View view;
                    view.kind = View::Kind::Group;
                    view.group = artist ? media::GroupKind::Artist : media::GroupKind::Album;
                    view.key = v.id;
                    view.name = v.name;
                    return view;
                }
            },
            item.value);
    }

    // A source's album, artist or playlist has its menu everywhere (ui/item_menu.h); the local
    // library's offer the same commands on their songs.
    void Menu(POINT screen, const Result& item) {
        RecordMenuContext context;
        context.page.push_back({L"Refresh", [this] { m_host.Refresh(); }});
        if (item.provider) {
            std::visit(
                [&](const auto& v) {
                    using T = std::decay_t<decltype(v)>;
                    if constexpr (!std::is_same_v<T, stream::Track>)
                        ShowRecordMenu(m_host, *item.provider, MenuRecord(v), screen, context);
                },
                item.value);
            return;
        }
        std::vector<MenuAction> actions;
        if (auto view = ViewOf(item)) actions.push_back({L"View", [this, v = *view] { m_host.Open(v); }});
        const metadb_handle_list songs = LocalSongs(item);
        if (songs.get_count()) {
            actions.push_back({});
            AddBulkActions(actions, Ready(songs));
        }
        std::wstring name;
        const wchar_t* what = L"Search for album";
        std::visit(
            [&](const auto& v) {
                using T = std::decay_t<decltype(v)>;
                if constexpr (std::is_same_v<T, stream::Artist>) what = L"Search for artist";
                if constexpr (std::is_same_v<T, stream::Playlist>) what = L"Search for playlist";
                if constexpr (!std::is_same_v<T, stream::Track>) name = v.name;
            },
            item.value);
        if (!name.empty()) {
            actions.push_back({});
            actions.push_back({Tr(what) + L" \u201C" + name + L"\u201D", [this, name] { m_host.Search(name); }});
        }
        actions.push_back({});
        actions.push_back({L"Refresh", [this] { m_host.Refresh(); }});
        ShowActionMenu(m_host.Owner(), screen, actions);
    }

    // A local result's songs (an album's, an artist's, a playlist's).
    metadb_handle_list LocalSongs(const Result& item) const {
        metadb_handle_list items;
        std::visit(
            [&](const auto& v) {
                using T = std::decay_t<decltype(v)>;
                if constexpr (std::is_same_v<T, stream::Playlist>) {
                    auto pm = playlist_manager::get();
                    const auto index = static_cast<t_size>(_wtoi64(v.id.c_str()));
                    if (index < pm->get_playlist_count()) pm->playlist_get_all_items(index, items);
                } else if constexpr (std::is_same_v<T, stream::Album> || std::is_same_v<T, stream::Artist>) {
                    const auto kind =
                        std::is_same_v<T, stream::Album> ? media::GroupKind::Album : media::GroupKind::Artist;
                    if (const auto* g = m_host.Library().FindGroup(kind, v.id))
                        for (size_t t : g->tracks) items.add_item(m_host.Library().Tracks()[t].handle);
                }
            },
            item.value);
        return items;
    }

    void CopyText(const std::wstring& text) {
        if (!OpenClipboard(m_host.Owner())) return;
        EmptyClipboard();
        const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
        if (HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
            if (void* p = GlobalLock(memory)) {
                memcpy(p, text.c_str(), bytes);
                GlobalUnlock(memory);
                if (!SetClipboardData(CF_UNICODETEXT, memory)) GlobalFree(memory);
            } else {
                GlobalFree(memory);
            }
        }
        CloseClipboard();
    }

    SearchScope m_scope;
    std::wstring m_term;
    std::vector<stream::Provider*> m_sources;
    std::vector<const TabDef*> m_tabs;
    const TabDef* m_tab = nullptr;
    std::vector<std::unique_ptr<Lane>> m_lanes;
    media::ListSource* m_shown = nullptr; // the source this page gave the window
    ResultTiles* m_tiles = nullptr;
    SearchSongSource* m_songs = nullptr;
};

} // namespace

View SearchView(const SearchScope& scope, std::wstring term, std::wstring tab) {
    View view;
    view.kind = View::Kind::Search;
    switch (scope.kind) {
    case SearchScope::Kind::Local: view.library = View::Library::Local; break;
    case SearchScope::Kind::Provider:
        view.library = View::Library::Remote;
        view.provider = scope.provider;
        break;
    default: view.library = View::Library::All; break;
    }
    view.remoteId = term;
    view.name = std::move(term);
    view.sectionId = tab == kTabs[0].key ? std::wstring() : std::move(tab);
    return view;
}

SearchScope SearchViewScope(const View& view) {
    switch (view.library) {
    case View::Library::Local: return {SearchScope::Kind::Local, {}};
    case View::Library::Remote: return {SearchScope::Kind::Provider, view.provider};
    default: return {SearchScope::Kind::All, {}};
    }
}

std::unique_ptr<Page> MakeSearchPage(PageHost& host, const View& view) {
    return std::make_unique<SearchPage>(host, view);
}

} // namespace glasswave::ui
