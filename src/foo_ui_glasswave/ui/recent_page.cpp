#include "pch.h"
#include "ui/song_actions.h"
#include "core/text.h"
#include "ui/item_menu.h"
#include "ui/recent_page.h"

#include "app/settings.h"
#include "media/library_model.h"
#include "media/play_stats.h"
#include "media/recent_lists.h"
#include "stream/provider.h"
#include "ui/i18n.h"
#include "ui/main_menu.h"
#include "ui/remote_grids.h"
#include "ui/remote_pages.h"
#include "ui/song_table.h"
#include "ui/text.h"

namespace glasswave::ui {

using core::Utf8;
using core::Wide;

namespace {

// One album or playlist of the Albums and Playlists tabs.
struct Entry {
    stream::Provider* provider = nullptr; // nullptr: the local library or a foobar2000 playlist
    bool playlist = false;
    stream::Id id;              // the provider's id; a foobar2000 playlist's name
    std::wstring name, detail;  // the tile's two lines
    std::wstring localArtist;   // a local album: its artist, to find it in the library
    metadb_handle_ptr art;      // a local entry's cover: one of its files
};

constexpr const wchar_t* kTabs[][2] = {{L"", L"Songs"}, {L"albums", L"Albums"}, {L"playlists", L"Playlists"}};

class RecentTiles final : public GridSource {
  public:
    using Act = std::function<void(const Entry&)>;
    RecentTiles(std::vector<Entry> entries, std::wstring empty, Act open, Act play, Act menu)
        : GridSource({{L"Name", 0, true}}, std::move(empty)), m_entries(std::move(entries)), m_open(std::move(open)),
          m_play(std::move(play)), m_menu(std::move(menu)) {
        Refilter();
    }
    std::wstring ColumnSetKey() const override { return L"recent-tiles"; }
    std::wstring Cell(size_t row, size_t) const override { return row < m_rows.size() ? m_entries[m_rows[row]].name : L""; }
    media::TileStyle Tiles() const override {
        return m_style == media::ViewStyle::Tiles ? media::TileStyle::Artist : media::TileStyle::Album;
    }
    bool SupportsViewStyle(media::ViewStyle style) const override { return style != media::ViewStyle::Details; }
    media::ViewStyle GetViewStyle() const override { return m_style; }
    void SetViewStyle(media::ViewStyle style) override {
        if (SupportsViewStyle(style)) m_style = style;
    }
    bool CanSort(size_t) const override { return false; }
    media::TileInfo Tile(size_t row) const override {
        media::TileInfo tile;
        if (row >= m_rows.size()) return tile;
        const Entry& e = m_entries[m_rows[row]];
        if (e.provider)
            tile.art = e.provider->ArtItem(e.playlist ? stream::ArtKind::Playlist : stream::ArtKind::Album, e.id);
        else
            tile.art = e.art;
        if (!tile.art.is_valid() && e.playlist) tile.icon = 29722; // WMP's large playlist artwork
        tile.lines = {e.name, e.detail, {}};
        // Recently Played mixes the sources: each cover carries its own.
        tile.source = e.provider ? std::wstring(e.provider->Key()) : L"local";
        return tile;
    }
    std::wstring ArtKey(size_t row) const override {
        if (row >= m_rows.size()) return {};
        const Entry& e = m_entries[m_rows[row]];
        return e.provider ? e.provider->ArtKey(e.playlist ? stream::ArtKind::Playlist : stream::ArtKind::Album, e.id)
                          : std::wstring();
    }
    void Activate(size_t row) override {
        if (row < m_rows.size() && m_open) m_open(m_entries[m_rows[row]]);
    }
    bool CanPlayTile(size_t row) const override { return row < m_rows.size(); }
    void PlayTile(size_t row) override {
        if (row < m_rows.size() && m_play) m_play(m_entries[m_rows[row]]);
    }
    void ContextMenu(HWND, POINT screen) override {
        if (Focus() < m_rows.size() && m_menu) {
            m_menuAt = screen;
            m_menu(m_entries[m_rows[Focus()]]);
        }
    }
    POINT MenuAt() const { return m_menuAt; }

  private:
    size_t ItemCount() const override { return m_entries.size(); }
    std::wstring ItemCell(size_t item, size_t) const override { return m_entries[item].name; }
    std::wstring FilterText(size_t item) const override { return m_entries[item].name + L" " + m_entries[item].detail; }
    std::wstring Identity(size_t item) const override {
        const Entry& e = m_entries[item];
        return (e.provider ? std::wstring(e.provider->Key()) : L"local") + L"|" + e.id + L"|" + e.localArtist;
    }

    std::vector<Entry> m_entries;
    Act m_open, m_play, m_menu;
    media::ViewStyle m_style = media::ViewStyle::Icons;
    POINT m_menuAt{};
};

class RecentPage final : public Page {
  public:
    RecentPage(PageHost& host, View view) : Page(host, std::move(view)) {}

    std::unique_ptr<media::ListSource> Start(bool) override {
        const size_t rows = static_cast<size_t>(settings::PlayHistoryRows());
        const auto records = media::PlayStats::Get().Recent(rows);
        if (m_view.sectionId == L"albums") return Tiles(Albums(records), L"No recently played albums.");
        if (m_view.sectionId == L"playlists") return Tiles(Playlists(), L"No recently played playlists.");
        return std::make_unique<PlayStatsSource>(records, L"No recently played songs.");
    }

    std::vector<PageTab> Tabs() const override {
        std::vector<PageTab> tabs;
        for (const auto& tab : kTabs) tabs.push_back({tab[0], Tr(tab[1])});
        return tabs;
    }
    std::wstring Tab() const override { return m_view.sectionId; }
    void SelectTab(const std::wstring& key) override {
        if (key == m_view.sectionId) return;
        View v = m_view;
        v.sectionId = key;
        m_host.Open(v);
    }

  private:
    // The albums of the recent songs, each once, the most recently played first.
    std::vector<Entry> Albums(const std::vector<media::PlayRecord>& records) const {
        std::vector<Entry> out;
        std::set<std::wstring> seen;
        for (const auto& r : records) {
            const std::wstring album = Wide(r.album.c_str()), artist = Wide(r.artist.c_str());
            if (album.empty()) continue;
            Entry e;
            e.name = album;
            e.detail = artist;
            if (!r.provider.empty()) {
                e.provider = stream::FindProvider(r.provider);
                if (!e.provider || r.albumId.empty()) continue;
                e.id = r.albumId;
            } else {
                e.id = media::Lower(album);
                e.localArtist = artist;
                e.art = metadb::get()->handle_create(r.path.c_str(), r.subsong);
            }
            const std::wstring key = (e.provider ? std::wstring(e.provider->Key()) : L"local") + L"|" + e.id + L"|" +
                                     (e.provider ? L"" : media::Lower(artist));
            if (seen.insert(key).second) out.push_back(std::move(e));
        }
        return out;
    }

    std::vector<Entry> Playlists() const {
        std::vector<Entry> out;
        auto pm = playlist_manager::get();
        for (const auto& list : media::RecentLists()) {
            Entry e;
            e.playlist = true;
            e.id = list.id;
            e.name = list.name;
            if (!list.provider.empty()) {
                e.provider = stream::FindProvider(list.provider);
                if (!e.provider) continue;
                if (const auto found = e.provider->FindPlaylist(list.id)) e.detail = found->creator;
            } else {
                const t_size p = pm->find_playlist(Utf8(list.id).c_str());
                if (p == SIZE_MAX) continue; // deleted since
                if (pm->playlist_get_item_count(p)) e.art = pm->playlist_get_item_handle(p, 0);
                e.detail = std::to_wstring(pm->playlist_get_item_count(p)) + Tr(L" songs");
            }
            out.push_back(std::move(e));
        }
        return out;
    }

    std::unique_ptr<media::ListSource> Tiles(std::vector<Entry> entries, const wchar_t* empty) {
        auto source = std::make_unique<RecentTiles>(
            std::move(entries), Tr(empty), Guard([this](const Entry& e) { Open(e); }),
            Guard([this](const Entry& e) { Play(e); }), Guard([this](const Entry& e) { Menu(e); }));
        m_tiles = source.get();
        return source;
    }

    void Open(const Entry& e) {
        if (e.provider) {
            m_host.Open(e.playlist ? PlaylistView(*e.provider, e.id, e.name, std::nullopt)
                                   : AlbumView(*e.provider, e.id, e.name));
            return;
        }
        if (e.playlist) {
            const t_size p = playlist_manager::get()->find_playlist(Utf8(e.id).c_str());
            if (p == SIZE_MAX) return;
            View v;
            v.library = View::Library::All;
            v.kind = View::Kind::Playlist;
            v.playlist = p;
            m_host.Open(v);
            return;
        }
        if (const media::Group* g = LocalAlbum(e)) {
            View v;
            v.kind = View::Kind::Group;
            v.group = media::GroupKind::Album;
            v.key = g->key;
            v.name = g->name;
            m_host.Open(v);
        }
    }

    void Play(const Entry& e) {
        if (e.provider) {
            if (e.playlist)
                PlayRemotePlaylist(*e.provider, e.id);
            else
                PlayRemoteAlbum(*e.provider, e.id);
            return;
        }
        metadb_handle_list items;
        if (e.playlist) {
            auto pm = playlist_manager::get();
            const t_size p = pm->find_playlist(Utf8(e.id).c_str());
            if (p != SIZE_MAX) pm->playlist_get_all_items(p, items);
            if (items.get_count()) media::RecordRecentList({}, e.id, e.name);
        } else if (const media::Group* g = LocalAlbum(e)) {
            for (size_t t : g->tracks) items.add_item(m_host.Library().Tracks()[t].handle);
        }
        if (items.get_count()) media::PlayItems(items, 0);
    }

    // A source's album or playlist has its menu everywhere (ui/item_menu.h); a local one the same
    // commands on its songs. This page adds forgetting the list.
    void Menu(const Entry& e) {
        const POINT at = m_tiles ? m_tiles->MenuAt() : POINT{};
        std::vector<MenuAction> page;
        if (e.playlist) {
            page.push_back({L"Remove from Recently Played", [this, e] {
                                media::ForgetRecentList(e.provider ? std::wstring(e.provider->Key()) : L"", e.id);
                                m_host.Reload();
                            }});
        } else {
            page.push_back({L"Remove from Recently Played", [this, e] {
                                media::PlayStats::Get().ForgetRecentAlbum(
                                    e.provider ? std::wstring(e.provider->Key()) : L"", e.id, media::Lower(e.name),
                                    media::Lower(e.localArtist));
                                m_host.Reload();
                            }});
        }
        if (e.provider) {
            RecordMenuContext context;
            context.page = std::move(page);
            if (e.playlist) {
                stream::Playlist playlist;
                playlist.id = e.id;
                playlist.name = e.name;
                ShowRecordMenu(m_host, *e.provider, playlist, at, context);
            } else {
                stream::Album album;
                album.id = e.id;
                album.name = e.name;
                ShowRecordMenu(m_host, *e.provider, album, at, context);
            }
            return;
        }
        std::vector<MenuAction> actions{{L"View", [this, e] { Open(e); }}, {}};
        metadb_handle_list songs;
        if (e.playlist) {
            auto pm = playlist_manager::get();
            if (const t_size p = pm->find_playlist(Utf8(e.id).c_str()); p != SIZE_MAX) pm->playlist_get_all_items(p, songs);
        } else if (const media::Group* g = LocalAlbum(e)) {
            for (size_t t : g->tracks) songs.add_item(m_host.Library().Tracks()[t].handle);
        }
        AddBulkActions(actions, Ready(songs), nullptr, {}, SIZE_MAX, [this, e] { Play(e); });
        actions.push_back({});
        actions.push_back({Tr(e.playlist ? L"Search for playlist" : L"Search for album") + L" \u201C" + e.name +
                               L"\u201D",
                           [this, name = e.name] { m_host.Search(name); }});
        if (!page.empty()) {
            actions.push_back({});
            for (auto& action : page) actions.push_back(std::move(action));
        }
        ShowActionMenu(m_host.Owner(), at, actions);
    }

    // A local album by its name, the one by the song's artist where names repeat.
    const media::Group* LocalAlbum(const Entry& e) const {
        const media::Group* found = nullptr;
        for (const auto& g : m_host.Library().Groups(media::GroupKind::Album)) {
            if (media::Lower(g.name) != e.id) continue;
            if (!found) found = &g;
            if (g.secondary == e.localArtist) return &g;
        }
        return found;
    }

    RecentTiles* m_tiles = nullptr;
};

} // namespace

std::unique_ptr<Page> MakeRecentPage(PageHost& host, const View& view) {
    return std::make_unique<RecentPage>(host, view);
}

} // namespace glasswave::ui
