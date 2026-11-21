#include "pch.h"
#include "media/list_sources.h"
#include "core/text.h"
#include "download/downloads.h"
#include "media/play_stats.h"
#include "media/special_playlists.h"
#include "media/recent_lists.h"
#include "media/favorites.h"
#include "ui/win7_menu.h"
#include "media/source_identity.h"
#include "stream/provider.h"
#include "ui/text.h"
#include "ui/i18n.h"
#include <cwctype>

namespace glasswave::media {

namespace {

titleformat_object::ptr Compile(const char* pattern) {
    titleformat_object::ptr s;
    titleformat_compiler::get()->compile_safe(s, pattern);
    return s;
}

// Linguistic, case-insensitive, numbers by value ("2" < "10"): -1, 0, 1.
int CompareText(const std::wstring& a, const std::wstring& b) {
    return CompareStringEx(LOCALE_NAME_USER_DEFAULT, LINGUISTIC_IGNORECASE | SORT_DIGITSASNUMBERS, a.c_str(),
                           static_cast<int>(a.size()), b.c_str(), static_cast<int>(b.size()), nullptr, nullptr, 0) -
           2;
}

// Columns WMP 12 offers under "Choose columns" beyond the default ones (hidden by default).
std::vector<Column> ExtraColumns() {
    return {
        {L"Album artist", 150, false, false, true, "[%album artist%]"},
        {L"Composer", 150, false, false, true, "[%composer%]"},
        {L"Rating", 80, false, false, false, kRatingFormat},
        {L"Favorite", 58, false, false, false, kFavoriteFormat},
        {L"Count", 70, false, true, true, kPlayCountFormat},
        {L"Bit rate", 70, false, true, true, "[%bitrate% kbps]"},
        {L"Type", 60, false, false, true, "[%codec%]"},
        {L"Size", 70, false, true, true, "[%filesize_natural%]"},
        {L"Date added", 120, false, false, true, "[%added%]"},
        {L"File name", 200, false, false, true, "%filename_ext%"},
        {L"Folder", 260, false, false, true, "$directory_path(%path%)"},
    };
}

bool HasFormat(const Column& column, const char* format) {
    return column.format && std::strcmp(column.format, format) == 0;
}

bool IsPlayCountColumn(const Column& column) { return HasFormat(column, kPlayCountFormat); }

// Ratings go through foobar2000's own Playback Statistics context command
// ("Playback Statistics" / "Rating" / "1".."5"), so they land where %rating% reads them.
bool RateTrack(const metadb_handle_ptr& track, int stars) {
    metadb_handle_list items;
    items.add_item(track);
    auto cm = contextmenu_manager::get();
    cm->init_context(items, contextmenu_manager::flag_view_full);
    contextmenu_node* node = cm->get_root();
    const std::string value = stars > 0 ? std::to_string(stars) : "<not set>";
    for (const char* want : {"Playback Statistics", "Rating", value.c_str()}) {
        contextmenu_node* next = nullptr;
        for (t_size i = 0, n = node ? node->get_num_children() : 0; i < n && !next; ++i) {
            contextmenu_node* c = node->get_child(i);
            if (!c || !c->get_name()) continue;
            std::string name = c->get_name();
            name.erase(std::remove(name.begin(), name.end(), '&'), name.end());
            if (pfc::stricmp_ascii(name.c_str(), want) == 0) next = c;
        }
        if (!next) return false;
        node = next;
    }
    node->execute();
    return true;
}

template <class IsSel, class SetSel>
void RangeSelect(size_t count, size_t a, size_t b, bool keepOthers, IsSel, SetSel set) {
    if (a > b) std::swap(a, b);
    for (size_t i = 0; i < count; ++i) {
        const bool in = i >= a && i <= b;
        if (in || !keepOthers) set(i, in);
    }
}

}  // namespace

void PlayItems(const metadb_handle_list& items, size_t start) { PlayQueue(items, start); }

Column ArtColumn(bool hidden) { return {L"Album art", 42, false, false, hidden, kArtFormat}; }

core::Signal<>::Connection WatchControlCells(std::function<void()> changed) {
    return download::Subscribe(std::move(changed));
}

bool ListSource::IsFavorite(size_t row) const { return media::IsFavorite(Item(row)); }

DownloadCell ListSource::Download(size_t row) const {
    const metadb_handle_ptr item = Item(row);
    if (!item.is_valid()) return {};
    const auto state = download::StateOf(item);
    if (!state) return {download::Downloadable(item) ? DownloadCell::Look::Offer : DownloadCell::Look::None};
    using Look = DownloadCell::Look;
    switch (state->state) {
    case download::State::Waiting: return {Look::Waiting, state->progress};
    case download::State::Running: return {Look::Running, state->progress};
    case download::State::Paused: return {Look::Paused, state->progress};
    case download::State::Failed: return {Look::Failed, state->progress};
    case download::State::Done: break;
    }
    return {Look::Done, 1.0f};
}

void ListSource::Rate(size_t row, int stars) {
    const metadb_handle_ptr item = Item(row);
    if (item.is_valid() && !RateTrack(item, stars))
        popup_message::g_show(
            core::Utf8(ui::Tr(L"Ratings need foobar2000's Playback Statistics (Library > Playback Statistics)."))
                .c_str(),
            core::Utf8(ui::Tr(L"Rating")).c_str());
}

void ListSource::ToggleFavorite(size_t row) { media::ToggleFavorite(Item(row)); }

void ListSource::ToggleDownload(size_t row) {
    const metadb_handle_ptr item = Item(row);
    if (!item.is_valid()) return;
    if (const auto state = download::StateOf(item)) {
        if (state->state == download::State::Done &&
            MessageBoxW(core_api::get_main_window(),
                        ui::Tr(L"Remove this download? Its file goes to the Recycle Bin.").c_str(),
                        ui::Tr(L"Download").c_str(), MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) == IDYES)
            download::Forget({state->id}, true);
    } else if (download::Downloadable(item)) {
        metadb_handle_list items;
        items.add_item(item);
        download::Add(items);
    }
}

std::wstring ListSource::RowKey(size_t row) const {
    if (const auto item = Item(row); item.is_valid())
        return core::Wide(item->get_path()) + L'\x1f' + std::to_wstring(item->get_subsong_index());
    return ArtKey(row);
}

// ---- PlaylistsSource --------------------------------------------------------------------

PlaylistsSource::PlaylistsSource(bool smart, Open open, Menu menu)
    : m_smart(smart), m_open(std::move(open)), m_menu(std::move(menu)) {
    m_columns = {{L"Title", 0, true}, {L"Count", 70, false, true}};
    SetFilter({});
}

std::wstring PlaylistsSource::Cell(size_t row, size_t column) const {
    if (row >= m_rows.size()) return {};
    auto pm = playlist_manager::get();
    if (column == 1) return std::to_wstring(pm->playlist_get_item_count(m_rows[row]));
    pfc::string8 name;
    pm->playlist_get_name(m_rows[row], name);
    // Built-in names (Favorites, the smart presets) are shown in the UI language.
    return IsSpecial(m_rows[row]) ? ui::Tr(core::Wide(name)) : core::Wide(name);
}

TileInfo PlaylistsSource::Tile(size_t row) const {
    TileInfo info;
    if (row >= m_rows.size()) return info;
    const t_size p = m_rows[row];
    // WMP's 192 px icons: 29722 playlist, 29726 star (Favorites). wmploc has no large auto
    // playlist icon (29572 stops at 32 px), so smart playlists use the playlist one.
    info.icon = p == FavoritesPlaylist(false) ? 29726 : 29722;
    const t_size n = playlist_manager::get()->playlist_get_item_count(p);
    info.lines = {Cell(row, 0), std::to_wstring(n) + ui::Tr(n == 1 ? L" song" : L" songs"), {}};
    return info;
}

void PlaylistsSource::SelectOnly(size_t row) {
    std::fill(m_selected.begin(), m_selected.end(), uint8_t{0});
    if (row < m_selected.size()) m_selected[row] = 1;
}

void PlaylistsSource::Toggle(size_t row) {
    if (row < m_selected.size()) m_selected[row] ^= 1;
}

void PlaylistsSource::SelectRange(size_t a, size_t b, bool keepOthers) {
    if (!keepOthers) std::fill(m_selected.begin(), m_selected.end(), uint8_t{0});
    if (a > b) std::swap(a, b);
    for (size_t r = a; r <= b && r < m_selected.size(); ++r) m_selected[r] = 1;
}

void PlaylistsSource::SelectAll() { std::fill(m_selected.begin(), m_selected.end(), uint8_t{1}); }

void PlaylistsSource::SetSelection(const std::vector<size_t>& rows) {
    std::fill(m_selected.begin(), m_selected.end(), uint8_t{0});
    for (size_t r : rows)
        if (r < m_selected.size()) m_selected[r] = 1;
}

void PlaylistsSource::Activate(size_t row) {
    if (row < m_rows.size() && m_open) m_open(m_rows[row]);
}

void PlaylistsSource::PlayTile(size_t row) {
    if (row >= m_rows.size()) return;
    auto pm = playlist_manager::get();
    metadb_handle_list items;
    pm->playlist_get_all_items(m_rows[row], items);
    if (!items.get_count()) return;
    PlayItems(items, 0);
    pfc::string8 name;
    pm->playlist_get_name(m_rows[row], name);
    const std::wstring wide = core::Wide(name.get_ptr());
    RecordRecentList({}, wide, wide);
}

void PlaylistsSource::ContextMenu(HWND, POINT screen) {
    if (m_menu) m_menu(m_focus < m_rows.size() ? m_rows[m_focus] : SIZE_MAX, screen);
}

metadb_handle_list PlaylistsSource::SelectedItems() const {
    metadb_handle_list out;
    auto pm = playlist_manager::get();
    for (size_t r = 0; r < m_rows.size(); ++r)
        if (m_selected[r]) {
            metadb_handle_list items;
            pm->playlist_get_all_items(m_rows[r], items);
            out.add_items(items);
        }
    return out;
}

void PlaylistsSource::SetFilter(std::wstring_view filterLower) {
    auto pm = playlist_manager::get();
    m_rows.clear();
    pfc::string8 name;
    const t_size favorites = FavoritesPlaylist(false);
    for (t_size p = 0, n = pm->get_playlist_count(); p < n; ++p) {
        if (IsQueue(p)) continue;  // the queue has its own place
        if ((p == favorites || IsAutoPlaylist(p)) != m_smart) continue;
        pm->playlist_get_name(p, name);
        if (!filterLower.empty() && Lower(core::Wide(name)).find(filterLower) == std::wstring::npos) continue;
        m_rows.push_back(p);
    }
    m_selected.assign(m_rows.size(), 0);
    if (m_focus >= m_rows.size()) m_focus = SIZE_MAX;
}

std::wstring PlaylistsSource::RowKey(size_t row) const {
    return row < m_rows.size() ? L"playlist:" + Cell(row, 0) : std::wstring();
}

// ---- PlaylistSource ---------------------------------------------------------------------

namespace {
enum PlaylistField {
    kPlAlbumColumn,
    kPlNumber,
    kPlArt,
    kPlTitle,
    kPlArtist,
    kPlAlbum,
    kPlLength,
    kPlSource,
    kPlFormat
};

// The library a track comes from, for lists that mix providers (the queue and playlists).
std::wstring SourceLabel(const metadb_handle_ptr& track) {
    const auto* provider = stream::ProviderOf(track);
    return ui::Tr(provider ? provider->Name() : std::wstring(L"Local Music"));
}
}  // namespace

PlaylistSource::PlaylistSource(size_t playlist) : m_playlist(playlist) { SetViewStyle(ViewStyle::Details); }

void PlaylistSource::SetViewStyle(ViewStyle style) {
    m_style = style == ViewStyle::Tiles ? ViewStyle::Tiles : ViewStyle::Details;
    if (m_style == ViewStyle::Tiles) {  // like the library's Expanded tile
        m_columns = {{L"Album", 200},
                     {L"#", 34, false, true},
                     {L"Title", 0, true},
                     {L"Length", 56, false, true},
                     {L"Contributing artist", 190},
                     {L"Source", 140},
                     {L"Genre", 110, false, false, false, "[%genre%]"},
                     {L"Release year", 80, false, false, true, "[$year(%date%)]"}};
        m_fields = {kPlAlbumColumn, kPlNumber, kPlTitle, kPlLength, kPlArtist, kPlSource, kPlFormat, kPlFormat};
    } else {
        m_columns = {{L"#", 34, false, true},
                     ArtColumn(false),
                     {L"Title", 0, true},
                     {L"Contributing artist", 170},
                     {L"Album", 170},
                     {L"Source", 140},
                     {L"Length", 56, false, true},
                     {L"Genre", 110, false, false, true, "[%genre%]"},
                     {L"Release year", 80, false, false, true, "[$year(%date%)]"}};
        m_fields = {kPlNumber, kPlArt, kPlTitle, kPlArtist, kPlAlbum, kPlSource, kPlLength, kPlFormat, kPlFormat};
    }
    for (auto& extra : ExtraColumns()) {
        m_fields.push_back(kPlFormat);
        m_columns.push_back(std::move(extra));
    }
    static const char* const kScripts[] = {nullptr, nullptr, nullptr, "$if2(%title%,%filename%)", "[%artist%]",
                                           "[%album%]", "[%length%]", nullptr, nullptr};
    m_scripts.clear();
    for (size_t c = 0; c < m_columns.size(); ++c) {
        const char* f = m_fields[c] == kPlFormat ? m_columns[c].format : kScripts[m_fields[c]];
        m_scripts.push_back(f && !IsPlayCountColumn(m_columns[c]) ? Compile(f) : titleformat_object::ptr());
    }
    m_sortColumn = SIZE_MAX;
    Rebuild();
}

bool PlaylistSource::CanSort(size_t column) const {
    return column < m_fields.size() && m_fields[column] != kPlNumber && m_fields[column] != kPlArt;
}

void PlaylistSource::Rebuild() {
    auto pm = playlist_manager::get();
    const t_size n = m_playlist < pm->get_playlist_count() ? pm->playlist_get_item_count(m_playlist) : 0;
    m_rows.clear();
    m_artKeys.clear();
    m_artTracks.clear();
    m_artReps.clear();
    if (m_filter.empty()) {
        m_rows.resize(n);
        for (size_t i = 0; i < n; ++i) m_rows[i] = i;
        BuildGroups();
        return;
    }
    auto script = Compile("[%artist% ][%album artist% ][%album% ][%title% ][%genre%]");
    pfc::string8 buf;
    for (size_t i = 0; i < n; ++i) {
        std::wstring text;
        if (const RepairedText* r = CueCatalog::Get().Repair(pm->playlist_get_item_handle(m_playlist, i))) {
            text = r->artist + L' ' + r->albumArtist + L' ' + r->album + L' ' + r->title + L' ' + r->genre;
        } else {
            pm->playlist_item_format_title(m_playlist, i, nullptr, buf, script, nullptr,
                                           playback_control::display_level_none);
            text = core::Wide(buf);
        }
        if (MatchesFilter(Lower(text), m_filter)) m_rows.push_back(i);
    }
    BuildGroups();
}

void PlaylistSource::BuildGroups() {
    m_groups.clear();
    if (m_style != ViewStyle::Tiles) return;
    // Expanded tile collects all songs of an album into one column, ordered by the album's
    // first appearance in the playlist. Only the display order changes; the foobar2000
    // playlist keeps its own order, so leaving the view restores it.
    auto pm = playlist_manager::get();
    static const auto script = Compile(
        "[%album%]\x1f$if2(%album artist%,%artist%)\x1f[%genre%]\x1f[$year(%date%)]\x1f[%album artist%]");
    struct RowInfo {
        size_t row = 0;  // index in the unsorted m_rows
        std::wstring key, album, albumArtist, genre, year, albumArtistLink;
        metadb_handle_ptr item;
    };
    std::vector<RowInfo> infos;
    infos.reserve(m_rows.size());
    pfc::string8 buf;
    for (size_t r = 0; r < m_rows.size(); ++r) {
        pm->playlist_item_format_title(m_playlist, m_rows[r], nullptr, buf, script, nullptr,
                                       playback_control::display_level_none);
        std::wstring fields = core::Wide(buf);
        std::array<std::wstring, 5> f;
        size_t k = 0;
        for (size_t from = 0; k < f.size(); ++k) {
            const size_t to = fields.find(L'\x1f', from);
            f[k] = fields.substr(from, to == std::wstring::npos ? std::wstring::npos : to - from);
            if (to == std::wstring::npos) break;
            from = to + 1;
        }
        RowInfo info;
        info.row = r;
        info.item = pm->playlist_get_item_handle(m_playlist, m_rows[r]);
        if (const RepairedText* fix = CueCatalog::Get().Repair(info.item)) {
            if (!fix->album.empty()) f[0] = fix->album;
            if (!fix->albumArtist.empty()) f[1] = f[4] = fix->albumArtist;
        }
        // A row without an album stays its own group; otherwise the album name is the key
        // (soundtracks often credit a different artist per track).
        info.key = f[0].empty() ? std::wstring(L"\x1f") +
                                      (info.item.is_valid() ? core::Wide(info.item->get_path()) : L"")
                                : f[0];
        info.album = f[0];
        info.albumArtist = f[1];
        info.genre = f[2];
        info.year = f[3];
        info.albumArtistLink = f[4];
        infos.push_back(std::move(info));
    }
    std::map<std::wstring, size_t> rank;
    for (size_t i = 0; i < infos.size(); ++i) rank.emplace(infos[i].key, i);
    std::vector<size_t> order(infos.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(),
                     [&](size_t a, size_t b) { return rank[infos[a].key] < rank[infos[b].key]; });
    std::vector<size_t> rows;
    rows.reserve(m_rows.size());
    for (size_t i : order) rows.push_back(m_rows[infos[i].row]);
    m_rows = std::move(rows);
    for (size_t i = 0; i < order.size(); ++i) {
        const RowInfo& info = infos[order[i]];
        if (!m_groups.empty() && m_groups.back().key == info.key) {
            ++m_groups.back().count;
            continue;
        }
        RowGroup g;
        g.key = info.key;
        g.first = i;
        g.count = 1;
        g.art = info.item;
        const std::wstring album = info.album.empty() ? ui::Tr(L"Unknown Album") : info.album;
        g.header = album;
        g.lines = {album, info.albumArtist, info.genre, info.year};
        // Album, album artist and genre open their library views; a streaming provider's
        // tracks open that provider's album and artist pages instead.
        g.links = {Link{Link::Kind::LocalAlbum, GroupKind::Album, info.album, {}},
                   Link::ToGroup(GroupKind::Artist, info.albumArtistLink), Link::ToGroup(GroupKind::Genre, info.genre),
                   Link{}};
        if (info.album.empty()) g.links[0] = {};
        m_groups.push_back(std::move(g));
    }
    for (auto& g : m_groups) {
        auto* provider = stream::ProviderOf(g.art);
        if (!provider) continue;
        g.links = {};
        // Old cached records may lack the album id; another song of the group may have it.
        std::optional<stream::Track> track;
        for (size_t k = 0; k < g.count && !(track && !track->albumId.empty()); ++k) {
            const auto item = pm->playlist_get_item_handle(m_playlist, m_rows[g.first + k]);
            if (auto found = provider->FindTrack(provider->TrackId(item)); found && (!track || !found->albumId.empty()))
                track = std::move(found);
        }
        if (!track) continue;
        g.links[0] = Link::Remote(Link::Kind::RemoteAlbum, provider->Key(), track->albumId);
        for (const auto& artist : track->artists)
            if (!artist.id.empty()) {
                g.links[1] = Link::Remote(Link::Kind::RemoteArtist, provider->Key(), artist.id);
                break;
            }
    }
}

void PlaylistSource::SetFilter(std::wstring_view filterLower) {
    m_filter = filterLower;
    Rebuild();
}

// A source's song keeps its tags before its artist in a foobar2000 playlist too.
std::vector<std::wstring> PlaylistSource::CellBadges(size_t row, size_t column) const {
    if (row >= m_rows.size() || column >= m_fields.size() || m_fields[column] != kPlArtist) return {};
    return stream::TrackBadges(playlist_manager::get()->playlist_get_item_handle(m_playlist, m_rows[row]));
}

std::wstring PlaylistSource::Cell(size_t row, size_t column) const {
    const size_t item = m_rows[row];
    const int field = column < m_fields.size() ? m_fields[column] : -1;
    if (field == kPlNumber) return std::to_wstring(item + 1);
    if (field == kPlAlbumColumn || field == kPlArt || field < 0) return {};  // drawn by the list
    if (field == kPlTitle || field == kPlArtist || field == kPlAlbum) {  // from a mis-decoded cue sheet
        auto pm = playlist_manager::get();
        if (const RepairedText* r = CueCatalog::Get().Repair(pm->playlist_get_item_handle(m_playlist, item))) {
            const std::wstring& v = field == kPlTitle ? r->title : field == kPlArtist ? r->artist : r->album;
            if (!v.empty()) return v;
        }
    }
    if (field == kPlSource)
        return SourceLabel(playlist_manager::get()->playlist_get_item_handle(m_playlist, item));
    if (field == kPlFormat && IsPlayCountColumn(m_columns[column]))
        return std::to_wstring(
            PlayStats::Get().CountFor(playlist_manager::get()->playlist_get_item_handle(m_playlist, item)));
    pfc::string8 out;
    playlist_manager::get()->playlist_item_format_title(m_playlist, item, nullptr, out, m_scripts[column], nullptr,
                                                        playback_control::display_level_all);
    return core::Wide(out);
}

void PlaylistSource::Sort(size_t column, bool descending) {
    auto pm = playlist_manager::get();
    const t_size n = pm->playlist_get_item_count(m_playlist);
    if (!CanSort(column)) {  // back to the order the playlist had before sorting
        if (m_sortColumn == SIZE_MAX) return;
        m_sortColumn = SIZE_MAX;
        metadb_handle_list current;
        pm->playlist_get_all_items(m_playlist, current);
        std::vector<bool> used(n, false);
        std::vector<t_size> order;
        order.reserve(n);
        for (t_size k = 0; k < m_unsorted.get_count(); ++k)
            for (t_size i = 0; i < n; ++i)
                if (!used[i] && current[i] == m_unsorted[k]) {
                    used[i] = true;
                    order.push_back(i);
                    break;
                }
        for (t_size i = 0; i < n; ++i)  // added since: keep them at the end
            if (!used[i]) order.push_back(i);
        m_unsorted.remove_all();
        pm->playlist_undo_backup(m_playlist);
        pm->playlist_reorder_items(m_playlist, order.data(), n);
        return;
    }
    if (m_sortColumn == SIZE_MAX) pm->playlist_get_all_items(m_playlist, m_unsorted);
    std::vector<std::wstring> keys(n);
    pfc::string8 buf;
    // Sort lengths by duration, play counts by value, the album column by album, everything
    // else by its text.
    const bool byLength = m_fields[column] == kPlLength;
    const bool bySource = m_fields[column] == kPlSource;
    const bool byPlayCount = m_fields[column] == kPlFormat && IsPlayCountColumn(m_columns[column]);
    titleformat_object::ptr script = byLength                             ? Compile("%length_seconds_fp%")
                                     : m_fields[column] == kPlAlbumColumn ? Compile("[%album%]")
                                                                          : m_scripts[column];
    for (t_size i = 0; i < n; ++i) {
        if (bySource) {  // local first, then each provider
            const auto* provider = stream::ProviderOf(pm->playlist_get_item_handle(m_playlist, i));
            keys[i] = provider ? L"1" + std::wstring(provider->Key()) : L"0";
            continue;
        }
        if (byPlayCount) {  // zero-pad so the text compare orders numbers
            wchar_t pad[32];
            swprintf_s(pad, L"%010u", PlayStats::Get().CountFor(pm->playlist_get_item_handle(m_playlist, i)));
            keys[i] = pad;
            continue;
        }
        pm->playlist_item_format_title(m_playlist, i, nullptr, buf, script, nullptr, playback_control::display_level_none);
        keys[i] = core::Wide(buf);
        if (byLength) {  // zero-pad so the text compare orders numbers
            const double v = _wtof(keys[i].c_str());
            wchar_t pad[32];
            swprintf_s(pad, L"%012.3f", v);
            keys[i] = pad;
        }
    }
    std::vector<t_size> order(n);
    for (t_size i = 0; i < n; ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](t_size a, t_size b) {
        const int c = CompareText(keys[a], keys[b]);
        return descending ? c > 0 : c < 0;
    });
    m_sortColumn = column;
    m_sortDescending = descending;
    pm->playlist_undo_backup(m_playlist);
    pm->playlist_reorder_items(m_playlist, order.data(), n);
}

std::wstring PlaylistSource::EmptyText() const {
    if (!m_filter.empty()) return L"No items match the search.";
    return L"This playlist is empty. Drag files here, or use Organize ▸ Add files.";
}

bool PlaylistSource::IsPlaying(size_t row) const {
    // Playing from this playlist: its exact item. Otherwise (songs play from the queue, a
    // copy of the list the user started them in) the same song shows as playing here too.
    auto pm = playlist_manager::get();
    t_size p = 0, i = 0;
    if (pm->get_playing_item_location(&p, &i) && p == m_playlist) return i == m_rows[row];
    metadb_handle_ptr now;
    return playback_control::get()->get_now_playing(now) && now == pm->playlist_get_item_handle(m_playlist, m_rows[row]);
}

bool PlaylistSource::IsSelected(size_t row) const {
    return playlist_manager::get()->playlist_is_item_selected(m_playlist, m_rows[row]);
}

void PlaylistSource::SelectOnly(size_t row) {
    auto pm = playlist_manager::get();
    pm->playlist_set_selection(m_playlist, pfc::bit_array_true(), pfc::bit_array_one(m_rows[row]));
}

void PlaylistSource::Toggle(size_t row) {
    auto pm = playlist_manager::get();
    const size_t i = m_rows[row];
    pm->playlist_set_selection_single(m_playlist, i, !pm->playlist_is_item_selected(m_playlist, i));
}

void PlaylistSource::SelectRange(size_t a, size_t b, bool keepOthers) {
    auto pm = playlist_manager::get();
    const t_size n = pm->playlist_get_item_count(m_playlist);
    pfc::bit_array_bittable affected(n), state(n);
    if (a > b) std::swap(a, b);
    if (!keepOthers)
        for (t_size i = 0; i < n; ++i) affected.set(i, true);
    for (size_t r = a; r <= b && r < m_rows.size(); ++r) {
        affected.set(m_rows[r], true);
        state.set(m_rows[r], true);
    }
    pm->playlist_set_selection(m_playlist, affected, state);
}

void PlaylistSource::SetSelection(const std::vector<size_t>& rows) {
    auto pm = playlist_manager::get();
    const t_size n = pm->playlist_get_item_count(m_playlist);
    pfc::bit_array_bittable state(n);
    for (size_t r : rows)
        if (r < m_rows.size()) state.set(m_rows[r], true);
    pm->playlist_set_selection(m_playlist, pfc::bit_array_true(), state);
}

metadb_handle_ptr PlaylistSource::Item(size_t row) const {
    return row < m_rows.size() ? playlist_manager::get()->playlist_get_item_handle(m_playlist, m_rows[row]) : metadb_handle_ptr{};
}

std::wstring PlaylistSource::ArtKey(size_t row) const {
    if (row >= m_rows.size()) return {};
    if (m_artKeys.size() != m_rows.size()) {
        m_artKeys.assign(m_rows.size(), {});
        m_artTracks.assign(m_rows.size(), {});
        m_artReps.clear();
    }
    if (!m_artKeys[row].empty()) return m_artKeys[row];
    auto pm = playlist_manager::get();
    const metadb_handle_ptr item = pm->playlist_get_item_handle(m_playlist, m_rows[row]);
    std::wstring key;
    if (auto* provider = stream::ProviderOf(item)) {
        // Share the provider's album covers with its album pages and grids.
        const stream::Id id = provider->TrackId(item);
        const auto track = provider->FindTrack(id);
        key = track && !track->albumId.empty() ? provider->ArtKey(stream::ArtKind::Album, track->albumId)
                                               : provider->ArtKey(stream::ArtKind::Track, id);
    } else {
        // Album + folder, not album artist: one album often lists different contributing
        // artists per track, but they all share the same cover.
        static const auto script = Compile("[%album%]\x1f$directory_path(%path%)");
        pfc::string8 buf;
        pm->playlist_item_format_title(m_playlist, m_rows[row], nullptr, buf, script, nullptr,
                                       playback_control::display_level_none);
        std::wstring text = core::Wide(buf);
        if (text.find_first_not_of(L'\x1f') != std::wstring::npos && item.is_valid()) {
            key = L"pl:" + text;
        } else if (item.is_valid()) {  // no album/folder information: keep the track distinct
            key = L"tr:" + core::Wide(item->get_path()) + L'|' + std::to_wstring(item->get_subsong_index());
        } else {
            key = L"row:" + std::to_wstring(row);
        }
    }
    // One representative track per album: the first row that asked for this cover.
    const auto it = m_artReps.try_emplace(key, item).first;
    m_artKeys[row] = key;
    m_artTracks[row] = it->second;
    return key;
}

metadb_handle_ptr PlaylistSource::ArtTrack(size_t row) const {
    ArtKey(row);  // fills the per-row cache
    return row < m_artTracks.size() ? m_artTracks[row] : Item(row);
}

size_t PlaylistSource::FindRow(const metadb_handle_ptr& track) const {
    auto pm = playlist_manager::get();
    for (size_t r = 0; r < m_rows.size(); ++r)
        if (pm->playlist_get_item_handle(m_playlist, m_rows[r]) == track) return r;
    return SIZE_MAX;
}

void PlaylistSource::SelectAll() {
    auto pm = playlist_manager::get();
    const t_size n = pm->playlist_get_item_count(m_playlist);
    pfc::bit_array_bittable affected(n), state(n);
    for (size_t i = 0; i < n; ++i) affected.set(i, true);
    for (size_t r : m_rows) state.set(r, true);
    pm->playlist_set_selection(m_playlist, affected, state);
}

size_t PlaylistSource::Focus() const {
    const t_size f = playlist_manager::get()->playlist_get_focus_item(m_playlist);
    if (f == SIZE_MAX) return SIZE_MAX;
    for (size_t r = 0; r < m_rows.size(); ++r)
        if (m_rows[r] == f) return r;
    return SIZE_MAX;
}

void PlaylistSource::SetFocus(size_t row) {
    if (row < m_rows.size()) playlist_manager::get()->playlist_set_focus_item(m_playlist, m_rows[row]);
}

void PlaylistSource::Activate(size_t row) {
    auto pm = playlist_manager::get();
    if (IsQueue(m_playlist)) {  // in the queue itself: just play that song
        pm->set_playing_playlist(m_playlist);
        pm->playlist_execute_default_action(m_playlist, m_rows[row]);
        return;
    }
    // Any other playlist: the queue becomes this list (as shown) and plays from the song.
    // The search box only filters the view: a filtered list still plays the whole playlist.
    metadb_handle_list items;
    if (m_filter.empty()) {
        for (size_t r : m_rows) items.add_item(pm->playlist_get_item_handle(m_playlist, r));
        PlayQueue(items, row);
        return;
    }
    pm->playlist_get_all_items(m_playlist, items);
    PlayQueue(items, m_rows[row]);
}

void PlaylistSource::ContextMenu(HWND owner, POINT screen) {
    // Playlist context commands act on the active playlist; afterwards the queue (normally
    // active, see MainWindow::KeepQueueActive) is active again.
    auto pm = playlist_manager::get();
    const t_size previous = pm->get_active_playlist();
    pm->set_active_playlist(m_playlist);
    ui::RunPlaylistContextMenu(owner, screen);
    if (previous != m_playlist && previous < pm->get_playlist_count()) pm->set_active_playlist(previous);
}

bool PlaylistSource::DeleteSelected() {
    auto pm = playlist_manager::get();
    pm->playlist_undo_backup(m_playlist);
    pm->playlist_remove_selection(m_playlist);
    return true;
}

metadb_handle_list PlaylistSource::SelectedItems() const {
    metadb_handle_list out;
    playlist_manager::get()->playlist_get_selected_items(m_playlist, out);
    return out;
}

// ---- TrackSource ------------------------------------------------------------------------

TrackSource::TrackSource(const LibraryModel& model, std::vector<size_t> tracks, std::wstring emptyText)
    : m_model(model), m_all(std::move(tracks)), m_emptyText(std::move(emptyText)) {
    SetViewStyle(ViewStyle::Tiles);
    // Album by album in "All music" order; tracks keep their library order inside an album.
    std::stable_sort(m_all.begin(), m_all.end(), [&](size_t a, size_t b) {
        return m_model.AlbumRank(m_model.Tracks()[a].albumIndex) < m_model.AlbumRank(m_model.Tracks()[b].albumIndex);
    });
    m_libraryOrder = m_all;
    m_selected.assign(m_all.size(), 0);
    SetFilter({});
}

namespace {
enum Field { kAlbumColumn, kNumber, kTitle, kLength, kArtist, kAlbum, kGenre, kYear, kFormat, kArt };
}

void TrackSource::SetViewStyle(ViewStyle style) {
    m_style = style == ViewStyle::Icons ? ViewStyle::Tiles : style;
    // WMP 12 music views: "Expanded tile" has the album column (cover and album details),
    // "Details" lists everything in columns under the album headers.
    if (m_style == ViewStyle::Tiles) {
        m_columns = {{L"Album", 200},
                     {L"#", 34, false, true},
                     {L"Title", 0, true},
                     {L"Length", 56, false, true},
                     {L"Contributing artist", 190},
                     {L"Genre", 110},
                     {L"Release year", 80, false, false, true}};
        m_fields = {kAlbumColumn, kNumber, kTitle, kLength, kArtist, kGenre, kYear};
    } else {
        // The album headers already show the album, so the cover column starts hidden.
        m_columns = {{L"#", 34, false, true}, ArtColumn(true), {L"Title", 0, true},  {L"Length", 56, false, true},
                     {L"Contributing artist", 170}, {L"Album", 170}, {L"Genre", 110}, {L"Release year", 80}};
        m_fields = {kNumber, kArt, kTitle, kLength, kArtist, kAlbum, kGenre, kYear};
    }
    m_scripts.assign(m_columns.size(), nullptr);
    for (auto& extra : ExtraColumns()) {
        m_scripts.push_back(IsPlayCountColumn(extra) ? titleformat_object::ptr() : Compile(extra.format));
        m_fields.push_back(kFormat);
        m_columns.push_back(std::move(extra));
    }
    m_sortColumn = SIZE_MAX;  // columns changed: back to the library order
    if (!m_libraryOrder.empty()) ApplySort();
}

bool TrackSource::CanSort(size_t column) const { return column < m_fields.size() && m_fields[column] != kArt; }

void TrackSource::Sort(size_t column, bool descending) {
    m_sortColumn = CanSort(column) ? column : SIZE_MAX;
    m_sortDescending = descending;
    ApplySort();
}

bool TrackSource::GroupBySort() const {
    // Expanded tile always groups by album. Details groups by the sort column while grouping
    // is on; an album sort keeps the album headers, anything else starts a flat list.
    if (m_style != ViewStyle::Details || m_sortColumn == SIZE_MAX || !m_grouping ||
        m_sortColumn >= m_fields.size())
        return false;
    const int field = m_fields[m_sortColumn];
    return field != kAlbum && field != kAlbumColumn;
}

void TrackSource::SetGrouping(bool enabled) {
    if (m_grouping == enabled) return;
    m_grouping = enabled;
    SetFilter(m_filter);
}

std::pair<std::wstring, std::wstring> TrackSource::GroupInfo(const Track& t) const {
    if (m_sortColumn >= m_fields.size()) return {t.album, t.album};
    const int field = m_fields[m_sortColumn];
    const Column& column = m_columns[m_sortColumn];
    switch (field) {
    case kNumber: {
        const std::wstring v = t.number.empty() ? ui::Tr(L"Unknown") : t.number;
        return {v, v};
    }
    case kTitle: {
        wchar_t c = t.title.empty() ? L'#' : t.title[0];
        if (c >= L'a' && c <= L'z') c = static_cast<wchar_t>(c - L'a' + L'A');
        if (std::iswpunct(static_cast<wint_t>(c)) || std::iswspace(static_cast<wint_t>(c))) c = L'#';
        std::wstring label(1, c);
        return {label, label};
    }
    case kLength: {
        const long long minutes = static_cast<long long>(t.seconds) / 60;
        const std::wstring key = std::to_wstring(minutes);
        return {key, key + ui::Tr(minutes == 1 ? L" minute" : L" minutes")};
    }
    case kArtist: {
        const std::wstring v = t.artist.empty() ? ui::Tr(L"Unknown Artist") : t.artist;
        return {v, v};
    }
    case kGenre: {
        const std::wstring v = t.genre.empty() ? ui::Tr(L"Unknown Genre") : t.genre;
        return {v, v};
    }
    case kYear: {
        const std::wstring v = t.year.empty() ? ui::Tr(L"Unknown Year") : t.year;
        return {v, v};
    }
    case kFormat: {
        if (HasFormat(column, kFavoriteFormat)) {
            const bool favorite = IsFavorite(t.handle);
            return {favorite ? L"1" : L"0", ui::Tr(favorite ? L"Favorite" : L"Not in Favorites")};
        }
        if (IsPlayCountColumn(column)) {
            const std::wstring v = std::to_wstring(PlayStats::Get().CountFor(t.handle));
            return {v, v};
        }
        if (HasFormat(column, kRatingFormat)) {
            static const wchar_t* const kStars[] = {L"1 Star", L"2 Stars", L"3 Stars", L"4 Stars", L"5 Stars"};
            const int n = t.rating.size() == 1 ? t.rating[0] - L'0' : 0;
            const std::wstring label = t.rating.empty() ? ui::Tr(L"Unrated")
                                       : n >= 1 && n <= 5 ? ui::Tr(kStars[n - 1])
                                                          : t.rating;
            return {t.rating, label};
        }
        pfc::string8 buf;
        if (m_sortColumn < m_scripts.size() && m_scripts[m_sortColumn].is_valid())
            t.handle->format_title(nullptr, buf, m_scripts[m_sortColumn], nullptr);
        std::wstring v = core::Wide(buf);
        if (v.empty()) v = ui::Tr(L"Unknown");
        return {v, v};
    }
    default:
        return {t.album, t.album};
    }
}

void TrackSource::ApplySort() {
    m_all = m_libraryOrder;
    const auto& tracks = m_model.Tracks();
    const auto& albums = m_model.Groups(GroupKind::Album);
    if (m_sortColumn != SIZE_MAX) {
        const int field = m_fields[m_sortColumn];
        const bool byAlbum = field == kAlbumColumn || field == kAlbum;
        auto albumName = [&](size_t t) {
            const size_t a = tracks[t].albumIndex;
            return a < albums.size() ? albums[a].name : tracks[t].album;
        };
        std::vector<std::wstring> keys(tracks.size());
        if (!byAlbum) {
            pfc::string8 buf;
            const Column& column = m_columns[m_sortColumn];
            const bool favorite = HasFormat(column, kFavoriteFormat);
            const bool rating = HasFormat(column, kRatingFormat);
            const bool playCount = IsPlayCountColumn(column);
            for (size_t t : m_all) {
                const Track& tr = tracks[t];
                switch (field) {
                case kNumber: {
                    wchar_t pad[32];
                    swprintf_s(pad, L"%06d", _wtoi(tr.number.c_str()));
                    keys[t] = pad;
                    break;
                }
                case kTitle: keys[t] = tr.title; break;
                case kLength: {
                    wchar_t pad[32];
                    swprintf_s(pad, L"%012.3f", tr.seconds);
                    keys[t] = pad;
                    break;
                }
                case kArtist: keys[t] = tr.artist.empty() ? ui::Tr(L"Unknown Artist") : tr.artist; break;
                case kGenre: keys[t] = tr.genre.empty() ? ui::Tr(L"Unknown Genre") : tr.genre; break;
                case kYear: keys[t] = tr.year.empty() ? ui::Tr(L"Unknown Year") : tr.year; break;
                default:
                    if (favorite)
                        keys[t] = IsFavorite(tr.handle) ? L"1" : L"0";
                    else if (rating)
                        keys[t] = tr.rating;
                    else if (playCount) {  // zero-pad so the text compare orders numbers
                        wchar_t pad[32];
                        swprintf_s(pad, L"%010u", PlayStats::Get().CountFor(tr.handle));
                        keys[t] = pad;
                    } else {
                        tr.handle->format_title(nullptr, buf, m_scripts[m_sortColumn], nullptr);
                        keys[t] = core::Wide(buf);
                        if (keys[t].empty()) keys[t] = ui::Tr(L"Unknown");
                    }
                    break;
                }
            }
        }
        const bool desc = m_sortDescending;
        std::stable_sort(m_all.begin(), m_all.end(), [&](size_t a, size_t b) {
            if (byAlbum) {
                const int c = CompareText(albumName(a), albumName(b));
                if (c) return desc ? c > 0 : c < 0;
                return tracks[a].albumIndex < tracks[b].albumIndex;  // same title: keep albums apart
            }
            const int c = CompareText(keys[a], keys[b]);
            if (m_style != ViewStyle::Tiles) return desc ? c > 0 : c < 0;  // Details: group by the column
            const size_t ra = m_model.AlbumRank(tracks[a].albumIndex), rb = m_model.AlbumRank(tracks[b].albumIndex);
            if (ra != rb) return ra < rb;
            return desc ? c > 0 : c < 0;
        });
    }
    m_selected.assign(m_all.size(), 0);
    SetFilter(m_filter);
}

std::wstring TrackSource::Cell(size_t row, size_t column) const {
    const Track& t = m_model.Tracks()[m_all[m_rows[row]]];
    switch (column < m_fields.size() ? m_fields[column] : -1) {
    case kNumber: return t.number;
    case kTitle: return t.title;
    case kLength: return t.length;
    case kArtist: return t.artist;
    case kAlbum: return t.album;
    case kGenre: return t.genre;
    case kYear: return t.year;
    case kFormat: {
        if (column < m_columns.size() && IsPlayCountColumn(m_columns[column]))
            return std::to_wstring(PlayStats::Get().CountFor(t.handle));
        pfc::string8 out;
        if (column < m_scripts.size() && m_scripts[column].is_valid())
            t.handle->format_title(nullptr, out, m_scripts[column], nullptr);
        return core::Wide(out);
    }
    default: return {};  // the album column is drawn by the list
    }
}

bool TrackSource::IsPlaying(size_t row) const {
    metadb_handle_ptr now;
    return playback_control::get()->get_now_playing(now) && now == m_model.Tracks()[m_all[m_rows[row]]].handle;
}

void TrackSource::SelectOnly(size_t row) {
    std::fill(m_selected.begin(), m_selected.end(), uint8_t{0});
    m_selected[m_rows[row]] = 1;
}

void TrackSource::Toggle(size_t row) { m_selected[m_rows[row]] ^= 1; }

void TrackSource::SelectRange(size_t a, size_t b, bool keepOthers) {
    if (!keepOthers) std::fill(m_selected.begin(), m_selected.end(), uint8_t{0});
    if (a > b) std::swap(a, b);
    for (size_t r = a; r <= b && r < m_rows.size(); ++r) m_selected[m_rows[r]] = 1;
}

void TrackSource::SelectAll() {
    for (size_t r : m_rows) m_selected[r] = 1;
}

metadb_handle_ptr TrackSource::Item(size_t row) const {
    return row < m_rows.size() ? m_model.Tracks()[m_all[m_rows[row]]].handle : metadb_handle_ptr{};
}

std::wstring TrackSource::ArtKey(size_t row) const {
    if (row >= m_rows.size()) return {};
    const Track& t = m_model.Tracks()[m_all[m_rows[row]]];
    const auto& albums = m_model.Groups(GroupKind::Album);
    return t.albumIndex < albums.size() ? L"lib:" + albums[t.albumIndex].key : std::wstring();
}

size_t TrackSource::FindRow(const metadb_handle_ptr& track) const {
    const auto& tracks = m_model.Tracks();
    for (size_t r = 0; r < m_rows.size(); ++r)
        if (tracks[m_all[m_rows[r]]].handle == track) return r;
    return SIZE_MAX;
}

void TrackSource::SetSelection(const std::vector<size_t>& rows) {
    std::fill(m_selected.begin(), m_selected.end(), uint8_t{0});
    for (size_t r : rows)
        if (r < m_rows.size()) m_selected[m_rows[r]] = 1;
}

void TrackSource::Activate(size_t row) {
    if (row >= m_rows.size()) return;
    // The search box only filters the view: play every song in display order from this one.
    metadb_handle_list items;
    for (size_t r = 0; r < m_all.size(); ++r) items.add_item(m_model.Tracks()[m_all[r]].handle);
    PlayItems(items, m_filter.empty() ? row : m_rows[row]);
}

void TrackSource::ContextMenu(HWND owner, POINT screen) {
    auto items = SelectedItems();
    if (items.get_count()) ui::RunContextMenu(owner, items, screen);
}

metadb_handle_list TrackSource::SelectedItems() const {
    metadb_handle_list out;
    for (size_t r : m_rows)
        if (m_selected[r]) out.add_item(m_model.Tracks()[m_all[r]].handle);
    return out;
}

void TrackSource::SetFilter(std::wstring_view filterLower) {
    m_filter = filterLower;
    m_rows.clear();
    for (size_t i = 0; i < m_all.size(); ++i)
        if (filterLower.empty() || MatchesFilter(m_model.Tracks()[m_all[i]].search, filterLower)) m_rows.push_back(i);
    m_focus = SIZE_MAX;

    m_groups.clear();
    // Details shows one continuous list unless it is sorted and grouping is on.
    if (m_style != ViewStyle::Tiles && (m_sortColumn == SIZE_MAX || !m_grouping)) return;
    const bool bySort = GroupBySort();
    const auto& albums = m_model.Groups(GroupKind::Album);
    for (size_t r = 0; r < m_rows.size(); ++r) {
        const Track& t = m_model.Tracks()[m_all[m_rows[r]]];
        std::wstring key, header;
        if (bySort) {
            auto info = GroupInfo(t);
            key = std::move(info.first);
            header = std::move(info.second);
        } else {
            key = L"a:" + std::to_wstring(t.albumIndex);
            const Group* album = t.albumIndex < albums.size() ? &albums[t.albumIndex] : nullptr;
            header = album ? album->secondary : t.albumArtist;
        }
        if (!m_groups.empty() && m_groups.back().key == key) {
            ++m_groups.back().count;
            continue;
        }
        RowGroup g;
        g.key = std::move(key);
        g.first = r;
        g.count = 1;
        g.art = t.handle;
        g.header = std::move(header);
        if (!bySort) {
            const Group* album = t.albumIndex < albums.size() ? &albums[t.albumIndex] : nullptr;
            const std::wstring artist = album ? album->secondary : t.albumArtist;
            const std::wstring year = album ? album->year : t.year;
            g.lines = {t.album, artist, t.genre, year};
            g.links = {Link::ToGroup(GroupKind::Album, album ? album->key : std::wstring()),
                       Link::ToGroup(GroupKind::Artist,
                                     m_model.FindGroup(GroupKind::Artist, artist) ? artist : std::wstring()),
                       Link::ToGroup(GroupKind::Genre, t.genre), Link::ToGroup(GroupKind::Year, year)};
        }
        m_groups.push_back(std::move(g));
    }
}

// ---- GroupSource ------------------------------------------------------------------------

GroupSource::GroupSource(const LibraryModel& model, GroupKind kind, DrillDown onOpen)
    : m_model(model), m_kind(kind), m_onOpen(std::move(onOpen)) {
    switch (kind) {
    case GroupKind::Album:
        m_columns = {{L"Album", 0, true}, {L"Album artist", 180}, {L"Genre", 110}, {L"Release year", 90},
                     {L"Count", 60, false, true}, {L"Length", 70, false, true}};
        m_style = ViewStyle::Icons;
        break;
    case GroupKind::Artist:
        m_columns = {{L"Album artist", 0, true}, {L"Count", 60, false, true}, {L"Length", 70, false, true}};
        m_style = ViewStyle::Tiles;  // WMP 12 opens artists as expanded tiles
        break;
    default: {
        // Genre, year, rating, contributing artist, composer, folder: name, count, length.
        static const wchar_t* const kTitles[] = {L"Album artist", L"Album",       L"Genre",    L"Year",
                                                 L"Rating",       L"Contributing artist", L"Composer", L"Folder"};
        m_columns = {{kTitles[std::min(static_cast<size_t>(kind), std::size(kTitles) - 1)], 0, true},
                     {L"Count", 60, false, true},
                     {L"Length", 70, false, true}};
        m_style = ViewStyle::Tiles;
        break;
    }
    }
    m_selected.assign(m_model.Groups(kind).size(), 0);
    SetFilter({});
}

// Group names the model makes up (Unknown Year, Unrated, 3 Stars, ...) are translated; tag
// values are shown as they are.
std::wstring GroupSource::Name(const Group& g) const {
    return g.key.empty() || m_kind == GroupKind::Rating ? ui::Tr(g.name) : g.name;
}

std::wstring GroupSource::Cell(size_t row, size_t column) const {
    const Group& g = At(row);
    if (column == 0) return Name(g);
    const size_t tail = m_columns.size() - column;  // Count and Length are the last two columns
    if (tail == 2) return std::to_wstring(g.tracks.size());
    if (tail == 1) return ui::FormatTime(g.seconds);
    // Album: artist, genre, year
    if (column == 1) return g.secondary;
    if (column == 2) return g.tracks.empty() ? std::wstring() : m_model.Tracks()[g.tracks.front()].genre;
    return g.year;
}

void GroupSource::Sort(size_t column, bool descending) {
    if (!CanSort(column)) {  // default order
        m_sortColumn = SIZE_MAX;
        SetFilter(m_filter);
        return;
    }
    m_sortColumn = column;
    m_sortDescending = descending;
    const auto& groups = m_model.Groups(m_kind);
    const size_t tail = m_columns.size() - column;  // Count and Length are the last two columns
    std::vector<std::wstring> keys(groups.size());
    for (size_t r = 0; r < m_rows.size(); ++r) {
        const Group& g = groups[m_rows[r]];
        wchar_t pad[32];
        if (tail <= 2) {
            swprintf_s(pad, L"%012.3f", tail == 2 ? static_cast<double>(g.tracks.size()) : g.seconds);
            keys[m_rows[r]] = pad;
        } else {
            keys[m_rows[r]] = Cell(r, column);
        }
    }
    std::stable_sort(m_rows.begin(), m_rows.end(), [&](size_t a, size_t b) {
        const int c = CompareText(keys[a], keys[b]);
        return descending ? c > 0 : c < 0;
    });
}

TileInfo GroupSource::Tile(size_t row) const {
    const Group& g = At(row);
    TileInfo info;
    if (!g.tracks.empty()) info.art = m_model.Tracks()[g.tracks.front()].handle;
    const auto songs = g.tracks.size();
    const std::wstring count = std::to_wstring(songs) + ui::Tr(songs == 1 ? L" song" : L" songs");
    const long minutes = std::lround(g.seconds / 60.0);
    const std::wstring length = std::to_wstring(minutes) + ui::Tr(minutes == 1 ? L" minute" : L" minutes");
    if (m_style == ViewStyle::Icons) {  // cover above: two lines
        info.lines = {Name(g), m_kind == GroupKind::Album ? g.secondary : count, {}};
    } else if (m_kind == GroupKind::Album) {  // cover beside: three lines
        info.lines = {Name(g), g.secondary, g.year};
    } else {
        info.lines = {Name(g), count, length};
    }
    return info;
}

std::wstring GroupSource::EmptyText() const {
    if (!m_model.Enabled() || m_model.Tracks().empty())
        return L"Your library is empty. Use Organize ▸ Manage music library to add your music folders.";
    return L"No items match the search.";
}

std::wstring GroupSource::ArtKey(size_t row) const {
    const Group& g = At(row);
    if (g.tracks.empty()) return {};
    const auto& tracks = m_model.Tracks();
    const size_t t = g.tracks.front();
    if (t >= tracks.size()) return {};
    const auto& albums = m_model.Groups(GroupKind::Album);
    const size_t album = tracks[t].albumIndex;
    return album < albums.size() ? L"lib:" + albums[album].key : std::wstring{};
}

void GroupSource::SelectOnly(size_t row) {
    std::fill(m_selected.begin(), m_selected.end(), uint8_t{0});
    m_selected[m_rows[row]] = 1;
}

void GroupSource::Toggle(size_t row) { m_selected[m_rows[row]] ^= 1; }

void GroupSource::SelectRange(size_t a, size_t b, bool keepOthers) {
    if (!keepOthers) std::fill(m_selected.begin(), m_selected.end(), uint8_t{0});
    if (a > b) std::swap(a, b);
    for (size_t r = a; r <= b && r < m_rows.size(); ++r) m_selected[m_rows[r]] = 1;
}

void GroupSource::SelectAll() {
    for (size_t r : m_rows) m_selected[r] = 1;
}

void GroupSource::SetSelection(const std::vector<size_t>& rows) {
    std::fill(m_selected.begin(), m_selected.end(), uint8_t{0});
    for (size_t r : rows)
        if (r < m_rows.size()) m_selected[m_rows[r]] = 1;
}

void GroupSource::Activate(size_t row) {
    if (m_onOpen) m_onOpen(At(row));
}

void GroupSource::PlayTile(size_t row) {
    if (row >= m_rows.size()) return;
    metadb_handle_list items;
    for (size_t t : At(row).tracks) items.add_item(m_model.Tracks()[t].handle);
    if (items.get_count()) PlayItems(items, 0);
}

void GroupSource::ContextMenu(HWND owner, POINT screen) {
    auto items = SelectedItems();
    if (items.get_count()) ui::RunContextMenu(owner, items, screen);
}

metadb_handle_list GroupSource::SelectedItems() const {
    metadb_handle_list out;
    const auto& groups = m_model.Groups(m_kind);
    for (size_t r : m_rows)
        if (m_selected[r])
            for (size_t t : groups[r].tracks) out.add_item(m_model.Tracks()[t].handle);
    return out;
}

void GroupSource::SetFilter(std::wstring_view filterLower) {
    m_filter = filterLower;
    m_rows.clear();
    const auto& groups = m_model.Groups(m_kind);
    for (size_t i = 0; i < groups.size(); ++i)
        if (filterLower.empty() || MatchesFilter(groups[i].search, filterLower)) m_rows.push_back(i);
    m_focus = SIZE_MAX;
    if (m_sortColumn != SIZE_MAX) Sort(m_sortColumn, m_sortDescending);
}

std::wstring GroupSource::RowKey(size_t row) const {
    if (row >= m_rows.size()) return {};
    const Group& group = m_model.Groups(m_kind)[m_rows[row]];
    return L"group:" + std::to_wstring(static_cast<int>(m_kind)) + L'\x1f' +
           (group.key.empty() ? Name(group) : group.key);
}

}  // namespace glasswave::media
