#include "pch.h"
#include "ui/home_page.h"

#include "media/radio.h"
#include "netease/log.h"
#include "stream/list_session.h"
#include "ui/home_store.h"

#include "ui/i18n.h"
#include "ui/main_menu.h"
#include "ui/remote_grids.h"
#include "ui/item_menu.h"
#include "ui/remote_pages.h"
#include "ui/win7_menu.h"

#include <variant>

namespace glasswave::ui {

namespace {

using Value = stream::HomeShelfItem; // Track, Artist, Album, Playlist, Station

// The sources in the owner's order (2026-11-15: Apple Music's cards first, then NetEase's); any
// other source with a home page follows in registration order.
std::vector<stream::Provider*> HomeSources() {
    std::vector<stream::Provider*> sources;
    for (auto* p : stream::Providers())
        if (p->Can(stream::Capability::Home) && (p->SignedIn() || p->SessionRestoring())) sources.push_back(p);
    const auto rank = [](const stream::Provider* p) {
        constexpr std::wstring_view kOrder[] = {L"apple", L"ncm"};
        for (size_t i = 0; i < std::size(kOrder); ++i)
            if (p->Key() == kOrder[i]) return i;
        return std::size(kOrder);
    };
    std::stable_sort(sources.begin(), sources.end(),
                     [&](const stream::Provider* a, const stream::Provider* b) { return rank(a) < rank(b); });
    return sources;
}

// The last home page of each source, from this session's read (and when) or the last run's file.
struct Remembered {
    stream::Home home;
    ULONGLONG at = 0; // 0: from the file, to be read again
};
std::map<std::wstring, Remembered>& Memory() {
    static std::map<std::wstring, Remembered> memory;
    return memory;
}
// A visit reads a source again only after this (owner, 2026-11-20: not on every visit);
// Refresh reads it at once.
constexpr ULONGLONG kFreshMs = 20 * 60 * 1000;

media::CardInfo CardOf(const stream::Provider& provider, const stream::HomeCard& card) {
    media::CardInfo info;
    info.key = std::wstring(provider.Key()) + L"|" + card.key;
    info.provider = provider.Key();
    info.label = card.label;
    info.title = card.title;
    info.subtitle = card.subtitle;
    info.badge = card.badge;
    info.imageUrl = card.imageUrl;
    info.imageTall = card.imageTall;
    info.videoUrl = card.videoUrl;
    info.colors = card.colors;
    info.background = card.background;
    info.titleInk = card.titleInk;
    info.textInk = card.textInk;
    info.playable = !card.list.empty() || !card.station.empty() ||
                    (card.target && !std::holds_alternative<stream::Artist>(*card.target));
    return info;
}

// A card's web page (HomeCard::link) in the default browser; only https is opened.
void OpenLink(const std::wstring& url) {
    if (url.starts_with(L"https://"))
        ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

// A home card's daily list played from its orb, like any service list (PlayRemoteList).
void PlayHomeList(stream::Provider& provider, const stream::Id& list) {
    stream::ListQuery query;
    query.kind = stream::ListKind::HomeList;
    query.id = list;
    PlayRemoteList(provider, std::move(query), false);
}

// ---- the tiles ---------------------------------------------------------------------------------

// One tile: a card, or a row's record.
struct HomeItem {
    stream::Provider* provider = nullptr;
    std::optional<Value> value;           // a row's record, or a card's target
    std::optional<stream::HomeCard> card; // a card
    media::CardInfo cardInfo;
};

// One shelf: a source's cards, or one of its rows.
struct HomeGroup {
    std::wstring key, label;
    media::TileStyle tiles = media::TileStyle::Album;
    size_t lines = 1;
    std::vector<HomeItem> items;
};

class HomeTiles final : public GridSource {
  public:
    using Group = HomeGroup;
    struct Actions {
        std::function<void(const HomeItem&, const HomeGroup&)> activate, play;
        std::function<void(POINT screen, const HomeItem&)> menu;
    };

    HomeTiles(std::wstring emptyText, Actions actions)
        : GridSource({{L"Name", 0, true}}, std::move(emptyText)), m_actions(std::move(actions)) {}

    void SetGroups(std::vector<Group> groups) {
        m_data = std::move(groups);
        m_entries.clear();
        for (size_t g = 0; g < m_data.size(); ++g)
            for (size_t i = 0; i < m_data[g].items.size(); ++i) m_entries.push_back({g, i});
        Refilter();
        BuildGroups();
    }
    void SetEmpty(std::wstring text) { SetEmptyText(std::move(text)); }

    std::wstring ColumnSetKey() const override { return L"home-tiles"; }
    std::wstring Cell(size_t row, size_t column) const override {
        return row < m_rows.size() ? ItemCell(m_rows[row], column) : std::wstring();
    }
    media::TileStyle Tiles() const override { return media::TileStyle::Album; }
    const std::vector<media::RowGroup>* Groups() const override { return &m_groups; }
    bool GroupArt() const override { return false; }
    bool CanSort(size_t) const override { return false; }
    bool SupportsViewStyle(media::ViewStyle style) const override { return style == media::ViewStyle::Icons; }
    media::ViewStyle GetViewStyle() const override { return media::ViewStyle::Icons; }

    const media::CardInfo* Card(size_t row) const override {
        const HomeItem* item = ItemAt(row);
        return item && item->card ? &item->cardInfo : nullptr;
    }

    bool CanPlayTile(size_t row) const override {
        const HomeItem* item = ItemAt(row);
        if (!item) return false;
        if (item->card) return item->cardInfo.playable;
        return item->value && !std::holds_alternative<stream::Artist>(*item->value); // a station plays
    }
    void PlayTile(size_t row) override {
        if (const HomeItem* item = ItemAt(row); item && m_actions.play) m_actions.play(*item, GroupOf(row));
    }
    void Activate(size_t row) override {
        if (const HomeItem* item = ItemAt(row); item && m_actions.activate) m_actions.activate(*item, GroupOf(row));
    }
    void ContextMenu(HWND owner, POINT screen) override {
        const HomeItem* item = ItemAt(Focus());
        if (!item) return;
        if (!item->value) { // a card for no record: a station, a daily list
            if (item->card && m_actions.menu) m_actions.menu(screen, *item);
            return;
        }
        if (std::holds_alternative<stream::Track>(*item->value)) {
            metadb_handle_list items;
            if (auto song = Item(Focus()); song.is_valid()) items.add_item(song);
            if (items.get_count()) RunContextMenu(owner, items, screen);
            return;
        }
        if (m_actions.menu) m_actions.menu(screen, *item);
    }

    media::TileInfo Tile(size_t row) const override {
        media::TileInfo tile;
        const HomeItem* item = ItemAt(row);
        if (!item || !item->value || item->card) return tile;
        stream::Provider* p = item->provider;
        std::visit(
            [&](const auto& v) {
                using T = std::decay_t<decltype(v)>;
                if constexpr (std::is_same_v<T, stream::Track>) {
                    tile.art = v.albumId.empty() ? p->TrackItem(v.id) : p->ArtItem(stream::ArtKind::Album, v.albumId);
                    tile.lines = {v.title, stream::JoinArtists(v.artists), v.album};
                    tile.badges = v.badges;
                } else if constexpr (std::is_same_v<T, stream::Album>) {
                    tile.art = p->ArtItem(stream::ArtKind::Album, v.id);
                    tile.lines = {v.name, stream::JoinArtists(v.artists), {}};
                } else if constexpr (std::is_same_v<T, stream::Playlist>) {
                    if (!v.coverUrl.empty())
                        tile.art = p->ArtItem(stream::ArtKind::Playlist, v.id);
                    else
                        tile.icon = 29722; // WMP's large playlist artwork
                    tile.lines = {v.name, v.creator, {}};
                } else if constexpr (std::is_same_v<T, stream::Station>) {
                    // A tile like the others, as Apple Music shows its stations in a row.
                    tile.art = p->ArtItem(stream::ArtKind::Station, v.id);
                    tile.lines = {v.name, v.subtitle.empty() ? Tr(L"Station") : v.subtitle, {}};
                } else {
                    static_assert(std::is_same_v<T, stream::Artist>);
                    tile.art = p->ArtItem(stream::ArtKind::Artist, v.id);
                    tile.lines = {v.name, {}, {}};
                }
            },
            *item->value);
        tile.source = p->Key(); // rows of several sources: whose cover this is
        return tile;
    }

    std::wstring ArtKey(size_t row) const override {
        const HomeItem* item = ItemAt(row);
        if (!item || !item->value || item->card) return {};
        const stream::Provider& p = *item->provider;
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
                else if constexpr (std::is_same_v<T, stream::Station>)
                    return p.ArtKey(stream::ArtKind::Station, v.id);
                else
                    return p.ArtKey(stream::ArtKind::Artist, v.id);
            },
            *item->value);
    }

    metadb_handle_ptr Item(size_t row) const override {
        const HomeItem* item = ItemAt(row);
        return item ? SongOf(*item) : metadb_handle_ptr();
    }
    metadb_handle_list SelectedItems() const override {
        metadb_handle_list items;
        for (size_t row = 0; row < m_rows.size(); ++row)
            if (IsSelected(row))
                if (auto song = Item(row); song.is_valid()) items.add_item(song);
        return items;
    }
    void SetFilter(std::wstring_view filterLower) override {
        GridSource::SetFilter(filterLower);
        BuildGroups();
    }

    static metadb_handle_ptr SongOf(const HomeItem& item) {
        const auto* track = item.value && !item.card ? std::get_if<stream::Track>(&*item.value) : nullptr;
        return track ? item.provider->TrackItem(track->id) : metadb_handle_ptr();
    }

  private:
    struct Entry {
        size_t group = 0, index = 0;
    };

    const HomeItem* ItemAt(size_t row) const {
        if (row >= m_rows.size()) return nullptr;
        const Entry& e = m_entries[m_rows[row]];
        return &m_data[e.group].items[e.index];
    }
    const Group& GroupOf(size_t row) const { return m_data[m_entries[m_rows[row]].group]; }

    size_t ItemCount() const override { return m_entries.size(); }
    std::wstring ItemCell(size_t index, size_t) const override {
        const Entry& e = m_entries[index];
        const HomeItem& item = m_data[e.group].items[e.index];
        if (item.card) return item.card->title;
        return std::visit(
            [](const auto& v) -> std::wstring {
                if constexpr (std::is_same_v<std::decay_t<decltype(v)>, stream::Track>)
                    return v.title;
                else
                    return v.name;
            },
            *item.value);
    }
    std::wstring FilterText(size_t index) const override { return ItemCell(index, 0); }
    std::wstring Identity(size_t index) const override {
        const Entry& e = m_entries[index];
        const HomeItem& item = m_data[e.group].items[e.index];
        if (item.card) return m_data[e.group].key + L"|" + item.cardInfo.key;
        return m_data[e.group].key + L"|" + std::to_wstring(item.value->index()) + L"|" +
               std::visit([](const auto& v) { return v.id; }, *item.value);
    }

    void BuildGroups() {
        m_groups.clear();
        for (size_t row = 0; row < m_rows.size(); ++row) {
            const Group& data = m_data[m_entries[m_rows[row]].group];
            if (m_groups.empty() || m_groups.back().key != data.key) {
                media::RowGroup group;
                group.key = data.key;
                group.first = row;
                group.header = data.label;
                group.tileLines = data.lines;
                group.tiles = data.tiles;
                group.carousel = true;
                m_groups.push_back(std::move(group));
            }
            ++m_groups.back().count;
        }
    }

    Actions m_actions;
    std::vector<Group> m_data;
    std::vector<Entry> m_entries;
    std::vector<media::RowGroup> m_groups;
};

// ---- the page ----------------------------------------------------------------------------------

class HomePage final : public Page, public stream::Provider::Listener {
  public:
    HomePage(PageHost& host, View view) : Page(host, std::move(view)) {
        for (auto* p : stream::Providers())
            if (p->Can(stream::Capability::Home)) p->AddListener(this);
    }
    ~HomePage() override {
        for (auto* p : stream::Providers())
            if (p->Can(stream::Capability::Home)) p->RemoveListener(this);
    }

    std::unique_ptr<media::ListSource> Start(bool reuse) override {
        m_sources = HomeSources();
        HomeTiles::Actions actions;
        actions.activate = Guard([this](const HomeItem& item, const HomeGroup& group) {
            Open(item, group);
        });
        actions.play = Guard([this](const HomeItem& item, const HomeGroup& group) {
            Play(item, group);
        });
        actions.menu = Guard([this](POINT screen, const HomeItem& item) { Menu(screen, item); });
        auto tiles = std::make_unique<HomeTiles>(Tr(L"Loading..."), std::move(actions));
        m_tiles = tiles.get();
        m_pending = 0;
        m_asked.clear();
        m_error.clear();
        for (auto* source : m_sources) Ask(*source, reuse);
        Fill();
        return tiles;
    }

    // A source that was restoring its session at start-up is read once its account is known;
    // another account's (or none's) home page goes.
    void OnAccountObserved(stream::Provider& provider) override { Revisit(provider, false); }
    void OnAccountChanged(stream::Provider& provider) override { Revisit(provider, true); }

    bool CanRefresh() const override { return true; } // the window's Refresh (menu, F5)
    void RefreshRequested() override {
        for (auto* source : m_sources)
            if (auto it = Memory().find(std::wstring(source->Key())); it != Memory().end()) it->second.at = 0;
    }

  private:
    // Shows the source's last home page (this session's, else the file's) and reads it again
    // unless this session's read is recent and the page is only shown again (Back, a restyle).
    void Ask(stream::Provider& source, bool reuse) {
        const std::wstring key(source.Key());
        auto found = Memory().find(key);
        if (found == Memory().end())
            if (auto saved = home_store::Load(source)) found = Memory().emplace(key, Remembered{std::move(*saved), 0}).first;
        (void)reuse; // a visit, Back or a restyle alike: read again only once it is old
        const bool fresh = found != Memory().end() && found->second.at && GetTickCount64() - found->second.at < kFreshMs;
        if (fresh || m_asked.count(key) || !source.SignedIn()) return;
        m_asked.insert(key);
        ++m_pending;
        source.FetchHome(Guard([this, &source, key](stream::Home home, std::wstring error) {
            --m_pending;
            size_t cards = home.cards.size(), videos = 0, cardRows = 0;
            for (const auto& card : home.cards) videos += card.videoUrl.empty() ? 0 : 1;
            for (const auto& shelf : home.shelves) {
                cardRows += shelf.cards.empty() ? 0 : 1;
                cards += shelf.cards.size();
                for (const auto& card : shelf.cards) videos += card.videoUrl.empty() ? 0 : 1;
            }
            netease::Log(L"home: " + key + L" " + std::to_wstring(cards) + L" cards (" + std::to_wstring(videos) +
                         L" with motion), " + std::to_wstring(home.shelves.size()) + L" rows (" +
                         std::to_wstring(cardRows) + L" of cards)" + (error.empty() ? L"" : L"; " + error));
            if (error.empty() || !home.cards.empty() || !home.shelves.empty()) {
                auto& kept = Memory()[key];
                kept = {std::move(home), GetTickCount64()};
                home_store::Save(source, kept.home);
            } else if (m_error.empty()) {
                m_error = std::move(error);
            }
            Publish();
        }));
    }

    void Revisit(stream::Provider& provider, bool accountChanged) {
        if (!provider.Can(stream::Capability::Home)) return;
        const std::wstring key(provider.Key());
        if (accountChanged) Memory().erase(key);
        m_asked.erase(key);
        m_sources = HomeSources();
        if (std::find(m_sources.begin(), m_sources.end(), &provider) != m_sources.end()) Ask(provider, false);
        Publish();
    }

    void Publish() {
        if (m_host.Source() != m_tiles) return;
        m_host.BeforeListChange();
        Fill();
        m_host.ListChanged();
    }

    void Fill() {
        std::vector<HomeGroup> groups;
        std::vector<std::pair<stream::Provider*, const stream::Home*>> homes;
        for (auto* source : m_sources)
            if (const auto found = Memory().find(std::wstring(source->Key())); found != Memory().end())
                homes.emplace_back(source, &found->second.home);
        // A row of cards: each card a tile of the card style, its motion and colours.
        const auto addCards = [](HomeGroup& group, stream::Provider* source, const std::vector<stream::HomeCard>& cards) {
            group.tiles = media::TileStyle::Card;
            for (const auto& card : cards) {
                HomeItem item;
                item.provider = source;
                if (card.target) item.value = stream::ToHomeShelfItem(*card.target);
                item.card = card;
                item.cardInfo = CardOf(*source, card);
                // One card visual per row and card: a card in two rows was one visual moved from
                // row to row, and the row it left showed a gap (owner, 2026-11-21).
                item.cardInfo.key = group.key + L"|" + card.key;
                group.items.push_back(std::move(item));
            }
        };
        // Each source's top cards in a shelf of their own.
        for (const auto& [source, home] : homes) {
            HomeGroup group;
            group.key = L"cards|" + std::wstring(source->Key());
            const std::wstring heading = home->cardsTitle.empty() ? Tr(L"Made for You") : home->cardsTitle;
            group.label = Tr(source->Name()) + L"  ·  " + heading;
            addCards(group, source, home->cards);
            if (!group.items.empty()) groups.push_back(std::move(group));
        }
        // Then their rows, one of each source in turn.
        for (size_t rank = 0;; ++rank) {
            bool any = false;
            for (const auto& [source, home] : homes) {
                if (rank >= home->shelves.size()) continue;
                any = true;
                const auto& shelf = home->shelves[rank];
                HomeGroup group;
                group.key = L"shelf|" + std::wstring(source->Key()) + L"|" + shelf.id;
                group.label = shelf.title.empty() ? Tr(source->Name()) : shelf.title;
                // Shown as the service shows it: a row of cards, or tiles.
                if (!shelf.cards.empty()) {
                    addCards(group, source, shelf.cards);
                    groups.push_back(std::move(group));
                    continue;
                }
                const bool songs = std::all_of(shelf.items.begin(), shelf.items.end(), [](const Value& v) {
                    return std::holds_alternative<stream::Track>(v);
                });
                group.tiles = songs ? media::TileStyle::Song : media::TileStyle::Album;
                group.lines = songs ? 3 : 1;
                for (const auto& value : shelf.items) {
                    HomeItem item;
                    item.provider = source;
                    item.value = value;
                    group.items.push_back(std::move(item));
                }
                if (!group.items.empty()) groups.push_back(std::move(group));
            }
            if (!any) break;
        }
        if (m_sources.empty())
            m_tiles->SetEmpty(Tr(L"Sign in to Apple Music or NetEase Cloud Music to see their home pages here."));
        else if (m_pending > 0)
            m_tiles->SetEmpty(Tr(L"Loading..."));
        else
            m_tiles->SetEmpty(m_error.empty() ? Tr(L"Nothing to show.") : Tr(m_error));
        m_tiles->SetGroups(std::move(groups));
    }

    // A card opens what it stands for: a record's page, its daily list's page, or its station
    // (radio mode); a song plays its row from it; anything else opens its page.
    void Open(const HomeItem& item, const HomeGroup& group) {
        if (item.card && !item.card->target) {
            const stream::HomeCard& card = *item.card;
            if (!card.list.empty())
                m_host.Open(HomeListView(*item.provider, card.list, card.label.empty() ? card.title : card.label));
            else if (!card.station.empty())
                media::radio::Play(*item.provider, card.station, card.title);
            else if (!card.link.empty())
                OpenLink(card.link);
            return;
        }
        if (!item.value) return;
        if (std::holds_alternative<stream::Track>(*item.value)) {
            PlayRow(item, group);
            return;
        }
        if (const auto* station = std::get_if<stream::Station>(&*item.value)) { // no page: it plays
            media::radio::Play(*item.provider, station->id, station->name);
            return;
        }
        if (auto view = ViewOf(*item.provider, *item.value)) m_host.Open(*view);
    }

    void PlayRow(const HomeItem& item, const HomeGroup& group) {
        metadb_handle_list songs;
        size_t start = 0;
        for (const auto& other : group.items) {
            auto song = HomeTiles::SongOf(other);
            if (!song.is_valid()) continue;
            if (&other == &item) start = songs.get_count();
            songs.add_item(song);
        }
        if (songs.get_count()) media::PlayItems(songs, start);
    }

    // The play orb: a card's songs, an album's or a playlist's songs, or a song's row from it.
    void Play(const HomeItem& item, const HomeGroup& group) {
        stream::Provider* p = item.provider;
        if (item.card && !item.card->target) {
            if (!item.card->list.empty())
                PlayHomeList(*p, item.card->list);
            else if (!item.card->station.empty())
                media::radio::Play(*p, item.card->station, item.card->title);
            return;
        }
        if (!item.value) return;
        std::visit(
            [&](const auto& v) {
                using T = std::decay_t<decltype(v)>;
                if constexpr (std::is_same_v<T, stream::Track>)
                    PlayRow(item, group);
                else if constexpr (std::is_same_v<T, stream::Album>)
                    PlayRemoteAlbum(*p, v.id);
                else if constexpr (std::is_same_v<T, stream::Playlist>)
                    PlayRemotePlaylist(*p, v.id);
                else if constexpr (std::is_same_v<T, stream::Station>)
                    media::radio::Play(*p, v.id, v.name);
            },
            *item.value);
    }

    static std::optional<View> ViewOf(stream::Provider& provider, const Value& value) {
        return std::visit(
            [&](const auto& v) -> std::optional<View> {
                using T = std::decay_t<decltype(v)>;
                if constexpr (std::is_same_v<T, stream::Track>) {
                    if (provider.Can(stream::Capability::SongDetails)) return SongView(provider, v.id, v.title);
                    return std::nullopt;
                } else if constexpr (std::is_same_v<T, stream::Playlist>) {
                    return PlaylistView(provider, v.id, v.name, std::nullopt);
                } else if constexpr (std::is_same_v<T, stream::Album>) {
                    return AlbumView(provider, v.id, v.name);
                } else if constexpr (std::is_same_v<T, stream::Station>) {
                    return std::nullopt; // a station has no page
                } else {
                    return ArtistView(provider, v.id, v.name);
                }
            },
            value);
    }

    // Albums, artists and playlists have their menu everywhere (ui/item_menu.h); Home adds Refresh.
    void Menu(POINT screen, const HomeItem& item) {
        if (!item.provider) return;
        if (!item.value && item.card) { // a station plays as radio; a daily list opens or plays; a link opens its page
            const stream::HomeCard card = *item.card;
            stream::Provider* p = item.provider;
            std::vector<MenuAction> actions;
            if (!card.list.empty()) {
                actions.push_back({L"View", [this, p, card] {
                                       m_host.Open(HomeListView(*p, card.list, card.label.empty() ? card.title : card.label));
                                   }});
                actions.push_back({L"Play", [p, list = card.list] { PlayHomeList(*p, list); }});
            } else if (!card.station.empty()) {
                actions.push_back({L"Play station", [p, card] { media::radio::Play(*p, card.station, card.title); }});
            } else if (!card.link.empty()) {
                actions.push_back({L"Open in browser", [link = card.link] { OpenLink(link); }});
            }
            actions.push_back({});
            actions.push_back({L"Refresh", [this] { m_host.Refresh(); }});
            ShowActionMenu(m_host.Owner(), screen, actions);
            return;
        }
        if (!item.value) return;
        if (const auto* station = std::get_if<stream::Station>(&*item.value)) {
            stream::Provider* p = item.provider;
            ShowActionMenu(m_host.Owner(), screen,
                           {{L"Play station", [p, s = *station] { media::radio::Play(*p, s.id, s.name); }},
                            {},
                            {L"Refresh", [this] { m_host.Refresh(); }}});
            return;
        }
        RecordMenuContext context;
        context.page.push_back({L"Refresh", [this] { m_host.Refresh(); }});
        std::visit(
            [&](const auto& v) {
                using T = std::decay_t<decltype(v)>;
                if constexpr (std::is_same_v<T, stream::Album> || std::is_same_v<T, stream::Artist> ||
                              std::is_same_v<T, stream::Playlist>)
                    ShowRecordMenu(m_host, *item.provider, MenuRecord(v), screen, context);
            },
            *item.value);
    }

    std::vector<stream::Provider*> m_sources;
    std::set<std::wstring> m_asked; // sources read (or being read) for this visit
    HomeTiles* m_tiles = nullptr;
    int m_pending = 0;
    std::wstring m_error;
};

} // namespace

std::unique_ptr<Page> MakeHomePage(PageHost& host, const View& view) {
    return std::make_unique<HomePage>(host, view);
}

} // namespace glasswave::ui
