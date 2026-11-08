#include "pch.h"
#include "ui/netease_sources.h"
#include "ui/i18n.h"
#include "ui/win7_menu.h"
#include "netease/catalog.h"
#include "netease/handle.h"
#include "netease/provider.h"

#include <ctime>
#include <cwctype>

namespace glasswave::ui {

namespace {

std::wstring Lower(std::wstring_view text) {
    std::wstring out(text);
    for (auto& c : out) c = static_cast<wchar_t>(std::towlower(static_cast<wint_t>(c)));
    return out;
}

bool Contains(const std::wstring& haystack, const std::wstring& needle) {
    return needle.empty() || haystack.find(needle) != std::wstring::npos;
}

std::wstring Wide(const pfc::string8& text) {
    return text.is_empty() ? std::wstring() : pfc::stringcvt::string_wide_from_utf8(text).get_ptr();
}

std::wstring LengthText(int milliseconds) {
    const int total = milliseconds > 0 ? milliseconds / 1000 : 0;
    wchar_t buffer[16];
    swprintf_s(buffer, L"%d:%02d", total / 60, total % 60);
    return buffer;
}

std::wstring AlbumYear(int64_t publishTimeMs) {
    if (publishTimeMs <= 0) return {};
    const time_t seconds = static_cast<time_t>(publishTimeMs / 1000);
    std::tm local{};
    if (localtime_s(&local, &seconds) != 0) return {};
    return std::to_wstring(local.tm_year + 1900);
}

std::wstring AlbumSubtitle(const netease::Album& album) {
    const std::wstring count = std::to_wstring(album.trackCount) + L" " + Tr(L"songs");
    const std::wstring year = AlbumYear(album.publishTime);
    return year.empty() ? count : year + L" · " + count;
}

std::wstring ArtistText(const netease::Song& song) { return netease::JoinArtists(song.artists); }

bool SameHandle(const metadb_handle_ptr& a, const metadb_handle_ptr& b) {
    return a.is_valid() && b.is_valid() && a == b;
}

}  // namespace

namespace {
enum SongField { kAlbumColumn, kNumber, kArt, kTitle, kLength, kArtist, kAlbum, kSource, kFavorite };

int CompareText(const std::wstring& a, const std::wstring& b) {
    return CompareStringEx(LOCALE_NAME_USER_DEFAULT, LINGUISTIC_IGNORECASE | SORT_DIGITSASNUMBERS, a.c_str(),
                           static_cast<int>(a.size()), b.c_str(), static_cast<int>(b.size()), nullptr, nullptr, 0) -
           2;
}

// A titleformat field of a local track, with the scripts compiled once.
std::wstring LocalField(const metadb_handle_ptr& track, const char* script) {
    static std::map<std::string, titleformat_object::ptr> scripts;
    auto it = scripts.find(script);
    if (it == scripts.end()) {
        titleformat_object::ptr compiled;
        titleformat_compiler::get()->compile_safe(compiled, script);
        it = scripts.emplace(script, compiled).first;
    }
    pfc::string8 out;
    if (it->second.is_valid()) track->format_title(nullptr, out, it->second, nullptr);
    return Wide(out);
}

std::wstring AlbumLabel(const SongTableRow& row) {
    return row.album.empty() ? Tr(L"Unknown Album") : row.album;
}

std::wstring ArtistLabel(const SongTableRow& row) {
    return row.artist.empty() ? Tr(L"Unknown Artist") : row.artist;
}

SongTableRow MakeNeteaseRow(const netease::Song& song) {
    SongTableRow row;
    row.handle = netease::Catalog::Get().Handle(song.id);
    row.title = song.title;
    row.artist = ArtistText(song);
    row.album = song.album;
    row.lengthMs = song.durationMs;
    row.length = LengthText(song.durationMs);
    row.albumKey = song.albumId ? std::to_wstring(song.albumId) : L"t:" + std::to_wstring(song.id);
    row.artKey = L"ncm:" + std::to_wstring(song.albumId ? song.albumId : song.id);
    row.search = Lower(row.title) + L"\n" + Lower(row.artist) + L"\n" + Lower(row.album);
    row.albumId = song.albumId;
    row.popularity = song.popularity;
    for (const auto& ref : song.artistRefs)
        if (ref.id) {
            row.artistId = ref.id;
            break;
        }
    row.favorite = netease::Provider::Get().IsLiked(song.id);
    row.remote = true;
    return row;
}

std::vector<SongTableRow> NeteaseRows(const std::vector<netease::Song>& songs) {
    std::vector<SongTableRow> rows;
    rows.reserve(songs.size());
    for (const auto& song : songs) rows.push_back(MakeNeteaseRow(song));
    return rows;
}

std::vector<SongTableRow> FavoriteRows(const std::vector<metadb_handle_ptr>& local,
                                       const std::vector<netease::Song>& remote) {
    std::vector<SongTableRow> rows;
    rows.reserve(local.size() + remote.size());
    for (const auto& track : local) {
        if (!track.is_valid()) continue;
        SongTableRow row;
        row.handle = track;
        row.title = LocalField(track, "%title%");
        row.artist = LocalField(track, "%artist%");
        row.album = LocalField(track, "%album%");
        row.length = LocalField(track, "%length%");
        row.lengthMs = _wtoi(LocalField(track, "%length_seconds%").c_str()) * 1000;
        row.source = Tr(L"Local Music");
        row.albumKey = L"l:" + Lower(row.album) + L"\x1f" + Lower(LocalField(track, "%album artist%"));
        row.artKey = std::wstring(L"fav:") + pfc::stringcvt::string_wide_from_utf8(track->get_path()).get_ptr() +
                     L"|" + std::to_wstring(track->get_subsong_index());
        row.search = Lower(row.title) + L"\n" + Lower(row.artist) + L"\n" + Lower(row.album);
        row.favorite = true;  // every local row comes from the Favorites playlist
        rows.push_back(std::move(row));
    }
    for (auto& row : NeteaseRows(remote)) {
        row.source = Tr(L"NetEase Cloud Music");
        rows.push_back(std::move(row));
    }
    return rows;
}
}  // namespace

SongTableSource::SongTableSource(std::vector<SongTableRow> rows, bool showSource, std::wstring emptyText,
                                 std::wstring columnKey, bool albumHeatOrder)
    : m_all(std::move(rows)),
      m_emptyText(std::move(emptyText)),
      m_columnKey(std::move(columnKey)),
      m_showSource(showSource),
      m_albumHeatOrder(albumHeatOrder) {
    m_order.resize(m_all.size());
    for (size_t i = 0; i < m_order.size(); ++i) m_order[i] = i;
    SetViewStyle(media::ViewStyle::Details);
}

std::wstring SongTableSource::ColumnSetKey() const {
    return m_columnKey + (m_style == media::ViewStyle::Tiles ? L"-tiles" : L"-details");
}

void SongTableSource::SetViewStyle(media::ViewStyle style) {
    m_style = style == media::ViewStyle::Icons ? media::ViewStyle::Tiles : style;
    if (m_style == media::ViewStyle::Tiles) {
        m_columns = {{L"Album", 200},
                     {L"#", 34, false, true},
                     {L"Title", 0, true},
                     {L"Length", 56, false, true},
                     {L"Artist", 190}};
        m_fields = {kAlbumColumn, kNumber, kTitle, kLength, kArtist};
    } else {
        m_columns = {{L"#", 34, false, true},
                     media::ArtColumn(false),
                     {L"Title", 0, true},
                     {L"Length", 56, false, true},
                     {L"Artist", 170},
                     {L"Album", 170}};
        m_fields = {kNumber, kArt, kTitle, kLength, kArtist, kAlbum};
    }
    if (m_showSource) {
        m_columns.push_back({L"Source", 140});
        m_fields.push_back(kSource);
    }
    m_columns.push_back({L"Favorite", 58, false, false, false, "<favorite>"});
    m_fields.push_back(kFavorite);
    m_sortColumn = SIZE_MAX;  // columns changed: back to the view's own order
    m_sortDescending = false;
    ApplySort();
}

bool SongTableSource::CanSort(size_t column) const {
    return column < m_fields.size() && m_fields[column] != kArt && m_fields[column] != kAlbumColumn;
}

void SongTableSource::Sort(size_t column, bool descending) {
    if (!CanSort(column)) {
        m_sortColumn = SIZE_MAX;
        m_sortDescending = false;
    } else {
        m_sortColumn = column;
        m_sortDescending = descending;
    }
    ApplySort();
}

void SongTableSource::SetGrouping(bool enabled) {
    if (m_grouping == enabled) return;
    m_grouping = enabled;
    BuildGroups();
}

std::pair<std::wstring, std::wstring> SongTableSource::GroupInfo(size_t row) const {
    if (m_sortColumn >= m_fields.size()) return {L"", L""};
    const SongTableRow& item = m_all[row];
    switch (m_fields[m_sortColumn]) {
    case kTitle: {
        wchar_t c = item.title.empty() ? L'#' : item.title[0];
        if (c >= L'a' && c <= L'z') c = static_cast<wchar_t>(c - L'a' + L'A');
        if (std::iswpunct(static_cast<wint_t>(c)) || std::iswspace(static_cast<wint_t>(c))) c = L'#';
        const std::wstring label(1, c);
        return {label, label};
    }
    case kLength: {
        const long long minutes = item.lengthMs / 60000;
        return {std::to_wstring(minutes), std::to_wstring(minutes) + Tr(minutes == 1 ? L" minute" : L" minutes")};
    }
    case kArtist: {
        const std::wstring label = ArtistLabel(item);
        return {label, label};
    }
    case kAlbum: {
        const std::wstring label = AlbumLabel(item);
        return {label, label};
    }
    case kSource: return {item.source, item.source};
    case kFavorite:
        return {item.favorite ? L"1" : L"0", Tr(item.favorite ? L"Favorite" : L"Not in Favorites")};
    default: return {L"", L""};  // # is the row number itself: nothing to group by
    }
}

void SongTableSource::SortOrder() {
    m_order.resize(m_all.size());
    for (size_t i = 0; i < m_order.size(); ++i) m_order[i] = i;
    const bool tiles = m_style == media::ViewStyle::Tiles;
    const bool heatOrder = tiles && m_albumHeatOrder;
    const bool columnSort = m_sortColumn != SIZE_MAX && m_sortColumn < m_fields.size();
    if (!heatOrder && !columnSort) return;
    const bool desc = m_sortDescending;
    const int field = columnSort ? m_fields[m_sortColumn] : -1;
    auto key = [&](size_t index) {
        const SongTableRow& item = m_all[index];
        switch (field) {
        case kNumber: {
            wchar_t pad[32];
            swprintf_s(pad, L"%06zu", index + 1);
            return std::wstring(pad);
        }
        case kTitle: return item.title;
        case kLength: {
            wchar_t pad[32];
            swprintf_s(pad, L"%012d", item.lengthMs);
            return std::wstring(pad);
        }
        case kArtist: return ArtistLabel(item);
        case kAlbum: return AlbumLabel(item);
        case kSource: return item.source;
        case kFavorite: return std::wstring(item.favorite ? L"1" : L"0");
        default: return std::wstring();
        }
    };
    std::map<std::wstring, size_t> bestRank;
    if (heatOrder) {
        for (size_t i = 0; i < m_all.size(); ++i) {
            auto it = bestRank.find(m_all[i].albumKey);
            if (it == bestRank.end() || i < it->second) bestRank[m_all[i].albumKey] = i;
        }
    }
    std::stable_sort(m_order.begin(), m_order.end(), [&](size_t a, size_t b) {
        if (heatOrder) {
            const std::wstring& ka = m_all[a].albumKey;
            const std::wstring& kb = m_all[b].albumKey;
            // The album of the hottest song comes first; its remaining songs do not move
            // it. Later pages can only add albums with worse best ranks, so the order is
            // stable while the list is paged in.
            if (ka != kb) return bestRank[ka] < bestRank[kb];
        } else if (tiles && m_all[a].albumKey != m_all[b].albumKey) {
            return a < b;  // albums keep the order they were added in
        }
        if (columnSort) {
            const int c = CompareText(key(a), key(b));
            if (!tiles) return desc ? c > 0 : c < 0;
            if (c) return desc ? c > 0 : c < 0;
        }
        return a < b;
    });
}

void SongTableSource::ApplySort() {
    SortOrder();
    m_selected.assign(m_all.size(), 0);
    m_focus = SIZE_MAX;
    RebuildVisible();
}

void SongTableSource::Append(std::vector<SongTableRow> rows) {
    if (rows.empty()) return;
    m_all.insert(m_all.end(), std::make_move_iterator(rows.begin()), std::make_move_iterator(rows.end()));
    m_selected.resize(m_all.size(), 0);
    SortOrder();
    RebuildVisible();
}

void SongTableSource::RebuildVisible() {
    m_visible.clear();
    for (size_t index : m_order)
        if (m_filter.empty() || Contains(m_all[index].search, m_filter)) m_visible.push_back(index);
    BuildGroups();
}

void SongTableSource::BuildGroups() {
    m_groups.clear();
    if (m_visible.empty()) return;
    const bool tiles = m_style == media::ViewStyle::Tiles;
    if (!tiles && (m_sortColumn == SIZE_MAX || !m_grouping)) return;  // no sort or grouping off: one list

    for (size_t r = 0; r < m_visible.size(); ++r) {
        const SongTableRow& item = m_all[m_visible[r]];
        std::wstring key, header;
        if (tiles) {
            key = L"a:" + item.albumKey;
            header = AlbumLabel(item);
        } else {
            auto info = GroupInfo(m_visible[r]);
            if (info.first.empty()) {  // sorting by #: the row numbers are not a grouping
                m_groups.clear();
                return;
            }
            key = std::move(info.first);
            header = std::move(info.second);
        }
        if (!m_groups.empty() && m_groups.back().key == key) {
            ++m_groups.back().count;
            continue;
        }
        media::RowGroup group;
        group.key = std::move(key);
        group.first = r;
        group.count = 1;
        group.art = item.handle;
        group.header = std::move(header);
        group.lines[0] = group.header;
        if (tiles) {
            if (item.albumId)
                group.linkKeys[0] = L"ncmalbum:" + std::to_wstring(item.albumId);
            else if (!item.remote && !item.album.empty())
                group.linkKeys[0] = L"localalbum:" + item.album;
            group.lines[1] = ArtistLabel(item);
            if (item.artistId) {
                group.linkKeys[1] = L"ncm:" + std::to_wstring(item.artistId);
            } else if (!item.remote && !item.artist.empty()) {  // local favorites: link the first artist
                std::wstring first = item.artist;
                if (const size_t end = first.find(L"; "); end != std::wstring::npos) first.resize(end);
                group.linkKeys[1] = L"local:" + first;
            }
        }
        m_groups.push_back(std::move(group));
    }
}

std::wstring SongTableSource::Cell(size_t row, size_t column) const {
    if (row >= m_visible.size() || column >= m_fields.size()) return {};
    const SongTableRow& item = m_all[m_visible[row]];
    switch (m_fields[column]) {
    case kNumber: return std::to_wstring(row + 1);
    case kTitle: return item.title;
    case kLength: return item.length;
    case kArtist: return item.artist;
    case kAlbum: return item.album;
    case kSource: return item.source;
    default: return {};  // the album, art and favorite columns are drawn by the list
    }
}

bool SongTableSource::IsPlaying(size_t row) const {
    metadb_handle_ptr now;
    return row < m_visible.size() && playback_control::get()->get_now_playing(now) && SameHandle(now, Item(row));
}

void SongTableSource::SelectOnly(size_t row) {
    std::fill(m_selected.begin(), m_selected.end(), uint8_t{0});
    if (row < m_visible.size()) m_selected[m_visible[row]] = 1;
    m_focus = row;
}

void SongTableSource::Toggle(size_t row) {
    if (row < m_visible.size()) m_selected[m_visible[row]] ^= 1;
    m_focus = row;
}

void SongTableSource::SelectRange(size_t a, size_t b, bool keepOthers) {
    if (!keepOthers) std::fill(m_selected.begin(), m_selected.end(), uint8_t{0});
    if (a > b) std::swap(a, b);
    for (size_t r = a; r <= b && r < m_visible.size(); ++r) m_selected[m_visible[r]] = 1;
}

void SongTableSource::SelectAll() {
    for (size_t index : m_visible) m_selected[index] = 1;
}

void SongTableSource::SetSelection(const std::vector<size_t>& rows) {
    std::fill(m_selected.begin(), m_selected.end(), uint8_t{0});
    for (size_t row : rows)
        if (row < m_visible.size()) m_selected[m_visible[row]] = 1;
}

metadb_handle_ptr SongTableSource::Item(size_t row) const {
    return row < m_visible.size() ? m_all[m_visible[row]].handle : metadb_handle_ptr{};
}

std::wstring SongTableSource::ArtKey(size_t row) const {
    return row < m_visible.size() ? m_all[m_visible[row]].artKey : std::wstring();
}

size_t SongTableSource::FindRow(const metadb_handle_ptr& track) const {
    for (size_t r = 0; r < m_visible.size(); ++r)
        if (SameHandle(Item(r), track)) return r;
    return SIZE_MAX;
}

void SongTableSource::Activate(size_t row) {
    if (row >= m_visible.size()) return;
    metadb_handle_list items;
    for (size_t r = 0; r < m_visible.size(); ++r) items.add_item(Item(r));
    media::PlayItems(items, row);
}

void SongTableSource::ContextMenu(HWND owner, POINT screen) {
    auto items = SelectedItems();
    if (items.get_count() == 0 && m_focus < m_visible.size()) items.add_item(Item(m_focus));
    if (items.get_count()) RunContextMenu(owner, items, screen);
}

metadb_handle_list SongTableSource::SelectedItems() const {
    metadb_handle_list items;
    for (size_t r = 0; r < m_visible.size(); ++r)
        if (m_selected[m_visible[r]]) items.add_item(Item(r));
    return items;
}

void SongTableSource::SetFilter(std::wstring_view filterLower) {
    m_filter.assign(filterLower);
    m_focus = SIZE_MAX;
    RebuildVisible();
}

NeteaseSongSource::NeteaseSongSource(std::vector<netease::Song> songs, std::wstring emptyText, bool albumHeatOrder)
    : SongTableSource(NeteaseRows(songs), false, std::move(emptyText), L"netease-songs", albumHeatOrder) {}

void NeteaseSongSource::AppendSongs(const std::vector<netease::Song>& songs) { Append(NeteaseRows(songs)); }

AllFavoritesSource::AllFavoritesSource(std::vector<metadb_handle_ptr> local, std::vector<netease::Song> remote)
    : SongTableSource(FavoriteRows(local, remote), true, L"No favorites.", L"all-favorites") {}

// ---- artist page ------------------------------------------------------------------------

NeteaseAlbumGridSource::NeteaseAlbumGridSource(std::wstring hotArtist, std::vector<netease::Album> albums,
                                               std::wstring emptyText, Open open, bool expandedStyle)
    : m_artistName(std::move(hotArtist)), m_albums(std::move(albums)), m_emptyText(std::move(emptyText)),
      m_open(std::move(open)), m_expandedStyle(expandedStyle) {
    m_columns = {{L"Album", 0, true}, {L"Release year", 80, false, true}, {L"Songs", 60, false, true}};
    SetFilter({});
}

std::wstring NeteaseAlbumGridSource::Cell(size_t row, size_t column) const {
    if (row >= m_rows.size() || column > 2) return {};
    const size_t index = m_rows[row];
    if (!index) return column == 0 ? Tr(L"Hot songs") : std::wstring();
    const auto& album = m_albums[index - 1];
    switch (column) {
    case 0: return album.name;
    case 1: return AlbumYear(album.publishTime);
    case 2: return std::to_wstring(album.trackCount);
    default: return {};
    }
}

media::TileInfo NeteaseAlbumGridSource::Tile(size_t row) const {
    media::TileInfo tile;
    if (row >= m_rows.size()) return tile;
    const size_t index = m_rows[row];
    if (!index) {
        tile.icon = 29711;  // WMP's large music note, for the hot songs tile
        tile.lines = {Tr(L"Hot songs"), m_artistName};
        return tile;
    }
    const auto& album = m_albums[index - 1];
    tile.art = netease::Catalog::Get().AlbumHandle(album.id);
    tile.lines = {album.name, AlbumSubtitle(album)};
    return tile;
}

void NeteaseAlbumGridSource::SelectOnly(size_t row) {
    std::fill(m_selected.begin(), m_selected.end(), uint8_t{0});
    if (row < m_rows.size()) m_selected[row] = 1;
    m_focus = row;
}

void NeteaseAlbumGridSource::Toggle(size_t row) {
    if (row < m_selected.size()) m_selected[row] ^= 1;
    m_focus = row;
}

void NeteaseAlbumGridSource::SelectRange(size_t a, size_t b, bool keepOthers) {
    if (!keepOthers) std::fill(m_selected.begin(), m_selected.end(), uint8_t{0});
    if (a > b) std::swap(a, b);
    for (size_t r = a; r <= b && r < m_selected.size(); ++r) m_selected[r] = 1;
}

void NeteaseAlbumGridSource::SelectAll() { std::fill(m_selected.begin(), m_selected.end(), uint8_t{1}); }

void NeteaseAlbumGridSource::SetSelection(const std::vector<size_t>& rows) {
    std::fill(m_selected.begin(), m_selected.end(), uint8_t{0});
    for (size_t row : rows)
        if (row < m_selected.size()) m_selected[row] = 1;
}

void NeteaseAlbumGridSource::Activate(size_t row) {
    if (row >= m_rows.size() || !m_open) return;
    const size_t index = m_rows[row];
    if (!index) {
        m_open(true, 0, m_artistName);
        return;
    }
    const auto& album = m_albums[index - 1];
    m_open(false, album.id, album.name);
}

void NeteaseAlbumGridSource::ContextMenu(HWND, POINT) {}

void NeteaseAlbumGridSource::SetFilter(std::wstring_view filterLower) {
    m_filter.assign(filterLower);
    m_rows.clear();
    if (!m_artistName.empty()) m_rows.push_back(0);  // the Hot songs tile is always offered
    for (size_t i = 0; i < m_albums.size(); ++i)
        if (m_filter.empty() || Contains(Lower(m_albums[i].name), m_filter)) m_rows.push_back(i + 1);
    m_selected.assign(m_rows.size(), 0);
    m_focus = SIZE_MAX;
}

void NeteaseAlbumGridSource::AppendAlbums(std::vector<netease::Album> albums) {
    if (albums.empty()) return;
    m_albums.insert(m_albums.end(), std::make_move_iterator(albums.begin()), std::make_move_iterator(albums.end()));
    SetFilter(m_filter);
}

// ---- followed artists -------------------------------------------------------------------

NeteaseArtistListSource::NeteaseArtistListSource(std::vector<netease::Artist> artists, std::wstring emptyText,
                                                 Open open)
    : m_artists(std::move(artists)), m_emptyText(std::move(emptyText)), m_open(std::move(open)) {
    m_columns = {{L"Artist", 0, true}, {L"Albums", 70, false, true}, {L"Songs", 70, false, true}};
    SetFilter({});
}

std::wstring NeteaseArtistListSource::Cell(size_t row, size_t column) const {
    if (row >= m_rows.size()) return {};
    const auto& artist = m_artists[m_rows[row]];
    switch (column) {
    case 0: return artist.name;
    case 1: return std::to_wstring(artist.albumCount);
    case 2: return std::to_wstring(artist.songCount);
    default: return {};
    }
}

media::TileInfo NeteaseArtistListSource::Tile(size_t row) const {
    media::TileInfo tile;
    if (row >= m_rows.size()) return tile;
    const auto& artist = m_artists[m_rows[row]];
    tile.art = netease::Catalog::Get().ArtistHandle(artist.id);
    tile.lines = {artist.name, std::to_wstring(artist.albumCount) + L" " + Tr(L"albums")};
    return tile;
}

void NeteaseArtistListSource::SelectOnly(size_t row) {
    std::fill(m_selected.begin(), m_selected.end(), uint8_t{0});
    if (row < m_rows.size()) m_selected[row] = 1;
    m_focus = row;
}

void NeteaseArtistListSource::Toggle(size_t row) {
    if (row < m_selected.size()) m_selected[row] ^= 1;
    m_focus = row;
}

void NeteaseArtistListSource::SelectRange(size_t a, size_t b, bool keepOthers) {
    if (!keepOthers) std::fill(m_selected.begin(), m_selected.end(), uint8_t{0});
    if (a > b) std::swap(a, b);
    for (size_t r = a; r <= b && r < m_selected.size(); ++r) m_selected[r] = 1;
}

void NeteaseArtistListSource::SelectAll() { std::fill(m_selected.begin(), m_selected.end(), uint8_t{1}); }

void NeteaseArtistListSource::SetSelection(const std::vector<size_t>& rows) {
    std::fill(m_selected.begin(), m_selected.end(), uint8_t{0});
    for (size_t row : rows)
        if (row < m_selected.size()) m_selected[row] = 1;
}

void NeteaseArtistListSource::Activate(size_t row) {
    if (row >= m_rows.size() || !m_open) return;
    const auto& artist = m_artists[m_rows[row]];
    m_open(artist.id, artist.name);
}

void NeteaseArtistListSource::ContextMenu(HWND, POINT) {}

void NeteaseArtistListSource::SetFilter(std::wstring_view filterLower) {
    m_filter.assign(filterLower);
    m_rows.clear();
    for (size_t i = 0; i < m_artists.size(); ++i)
        if (m_filter.empty() || Contains(Lower(m_artists[i].name), m_filter)) m_rows.push_back(i);
    m_selected.assign(m_rows.size(), 0);
    m_focus = SIZE_MAX;
}

// ---- playlists --------------------------------------------------------------------------

NeteasePlaylistSource::NeteasePlaylistSource(std::vector<netease::Playlist> playlists, std::wstring emptyText,
                                             Open open)
    : m_playlists(std::move(playlists)), m_emptyText(std::move(emptyText)), m_open(std::move(open)) {
    m_columns = {{L"Name", 0, true}, {L"Songs", 80, false, true}};
    m_selected.assign(m_playlists.size(), 0);
    SetFilter({});
}

std::wstring NeteasePlaylistSource::Cell(size_t row, size_t column) const {
    if (row >= m_rows.size()) return {};
    const auto& playlist = m_playlists[m_rows[row]];
    return column == 1 ? std::to_wstring(playlist.trackCount) : playlist.name;
}

media::TileInfo NeteasePlaylistSource::Tile(size_t row) const {
    media::TileInfo tile;
    if (row >= m_rows.size()) return tile;
    const auto& playlist = m_playlists[m_rows[row]];
    tile.art = netease::Catalog::Get().PlaylistHandle(playlist.id);
    tile.lines = {playlist.name, std::to_wstring(playlist.trackCount) + L" songs", L"NetEase Cloud Music"};
    return tile;
}

void NeteasePlaylistSource::SelectOnly(size_t row) {
    std::fill(m_selected.begin(), m_selected.end(), uint8_t{0});
    if (row < m_rows.size()) m_selected[m_rows[row]] = 1;
    m_focus = row;
}

void NeteasePlaylistSource::Toggle(size_t row) {
    if (row < m_rows.size()) m_selected[m_rows[row]] ^= 1;
    m_focus = row;
}

void NeteasePlaylistSource::SelectRange(size_t a, size_t b, bool keepOthers) {
    if (!keepOthers) std::fill(m_selected.begin(), m_selected.end(), uint8_t{0});
    if (a > b) std::swap(a, b);
    for (size_t r = a; r <= b && r < m_rows.size(); ++r) m_selected[m_rows[r]] = 1;
}

void NeteasePlaylistSource::SelectAll() {
    for (size_t r : m_rows) m_selected[r] = 1;
}

void NeteasePlaylistSource::SetSelection(const std::vector<size_t>& rows) {
    std::fill(m_selected.begin(), m_selected.end(), uint8_t{0});
    for (size_t row : rows)
        if (row < m_rows.size()) m_selected[m_rows[row]] = 1;
}

void NeteasePlaylistSource::Activate(size_t row) {
    if (row < m_rows.size() && m_open) {
        const auto& playlist = m_playlists[m_rows[row]];
        m_open(playlist.id, playlist.name);
    }
}

void NeteasePlaylistSource::ContextMenu(HWND, POINT) {}

void NeteasePlaylistSource::SetFilter(std::wstring_view filterLower) {
    m_filter.assign(filterLower);
    m_rows.clear();
    for (size_t i = 0; i < m_playlists.size(); ++i)
        if (m_filter.empty() || Contains(Lower(m_playlists[i].name), m_filter)) m_rows.push_back(i);
    m_selected.assign(m_playlists.size(), 0);
    m_focus = SIZE_MAX;
}

}  // namespace glasswave::ui
