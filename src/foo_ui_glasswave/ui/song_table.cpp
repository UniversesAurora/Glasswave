#include "pch.h"
#include "ui/song_table.h"
#include "media/favorites.h"
#include "media/special_playlists.h"
#include "ui/i18n.h"
#include "ui/text.h"
#include "ui/win7_menu.h"

#include <cwctype>
#include <unordered_set>

namespace glasswave::ui {

namespace {

enum SongField { kAlbumColumn, kNumber, kArt, kTitle, kLength, kArtist, kAlbum, kPlayCount, kSource, kFavorite };

int CompareText(const std::wstring& a, const std::wstring& b) {
    return CompareStringEx(LOCALE_NAME_USER_DEFAULT, LINGUISTIC_IGNORECASE | SORT_DIGITSASNUMBERS, a.c_str(),
                           static_cast<int>(a.size()), b.c_str(), static_cast<int>(b.size()), nullptr, nullptr, 0) -
           2;
}

std::wstring LengthText(int milliseconds) {
    const int total = milliseconds > 0 ? milliseconds / 1000 : 0;
    wchar_t buffer[16];
    swprintf_s(buffer, L"%d:%02d", total / 60, total % 60);
    return buffer;
}

std::wstring Padded(long long value) {
    wchar_t pad[32];
    swprintf_s(pad, L"%012lld", value);
    return pad;
}

// A titleformat field of a local track, with the scripts compiled once (main thread).
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
    return Wide(out.get_ptr());
}

std::wstring AlbumLabel(const SongTableRow& row) { return row.album.empty() ? Tr(L"Unknown Album") : row.album; }

std::wstring ArtistLabel(const SongTableRow& row) {
    return row.artist.empty() ? Tr(L"Unknown Artist") : row.artist;
}

void FinishRow(SongTableRow& row) {
    row.length = LengthText(row.lengthMs);
    row.search = media::Lower(row.title + L'\n' + row.artist + L'\n' + row.album);
    row.playCount = media::PlayStats::Get().CountFor(row.handle);
}

SongTableRow TrackRow(stream::Provider& provider, const stream::Track& track) {
    SongTableRow row;
    row.provider = &provider;
    row.handle = provider.TrackItem(track.id);
    row.trackId = track.id;
    row.albumId = track.albumId;
    for (const auto& artist : track.artists)
        if (!artist.id.empty()) {
            row.artistId = artist.id;
            break;
        }
    row.title = track.title;
    row.artist = stream::JoinArtists(track.artists);
    row.album = track.album;
    row.lengthMs = track.durationMs;
    row.popularity = track.popularity;
    row.albumKey = std::wstring(provider.Key()) + L':' + (track.albumId.empty() ? L"t:" + track.id : track.albumId);
    row.artKey = track.albumId.empty() ? provider.ArtKey(stream::ArtKind::Track, track.id)
                                       : provider.ArtKey(stream::ArtKind::Album, track.albumId);
    row.favorite = provider.IsFavorite(track.id);
    row.source = Tr(provider.Name());
    FinishRow(row);
    return row;
}

std::vector<SongTableRow> TrackRows(stream::Provider& provider, const std::vector<stream::Track>& tracks) {
    std::vector<SongTableRow> rows;
    rows.reserve(tracks.size());
    for (const auto& track : tracks) rows.push_back(TrackRow(provider, track));
    return rows;
}

// Local album rows share one cover key per album, like the library's album groups.
std::wstring LocalAlbumKey(const std::wstring& prefix, const SongTableRow& row, const std::wstring& albumArtist,
                           const metadb_handle_ptr& track) {
    if (row.album.empty())
        return prefix + Wide(track->get_path()) + L'|' + std::to_wstring(track->get_subsong_index());
    return prefix + media::Lower(row.album) + L'\x1f' + media::Lower(albumArtist);
}

std::vector<SongTableRow> FavoriteRows(const std::vector<metadb_handle_ptr>& local,
                                       const AllFavoritesSource::RemoteFavorites& remote) {
    std::vector<SongTableRow> rows;
    for (const auto& track : local) {
        if (!track.is_valid()) continue;
        SongTableRow row;
        row.handle = track;
        row.title = LocalField(track, "$if2(%title%,%filename%)");
        row.artist = LocalField(track, "[%artist%]");
        row.album = LocalField(track, "[%album%]");
        row.lengthMs = _wtoi(LocalField(track, "%length_seconds%").c_str()) * 1000;
        row.source = Tr(L"Local Music");
        const std::wstring albumArtist = LocalField(track, "[%album artist%]");
        row.albumKey = LocalAlbumKey(L"l:", row, albumArtist, track);
        row.artKey = LocalAlbumKey(L"fav:", row, albumArtist, track);
        row.favorite = true;  // every local row comes from the Favorites playlist
        FinishRow(row);
        rows.push_back(std::move(row));
    }
    for (const auto& [provider, tracks] : remote)
        for (auto& row : TrackRows(*provider, tracks)) rows.push_back(std::move(row));
    return rows;
}

std::vector<SongTableRow> PlayStatRows(const std::vector<media::PlayRecord>& records) {
    std::vector<SongTableRow> rows;
    rows.reserve(records.size());
    for (const auto& record : records) {
        SongTableRow row;
        row.title = Wide(record.title.c_str());
        row.artist = Wide(record.artist.c_str());
        row.album = Wide(record.album.c_str());
        row.lengthMs = record.durationMs;
        if (auto* provider = record.provider.empty() ? nullptr : stream::FindProvider(record.provider)) {
            row.provider = provider;
            row.handle = provider->TrackItem(record.trackId);
            row.trackId = record.trackId;
            row.albumId = record.albumId;
            row.artistId = record.artistId;
            row.albumKey = std::wstring(provider->Key()) + L':' +
                           (record.albumId.empty() ? L"t:" + record.trackId : record.albumId);
            row.artKey = record.albumId.empty() ? provider->ArtKey(stream::ArtKind::Track, record.trackId)
                                                : provider->ArtKey(stream::ArtKind::Album, record.albumId);
            row.favorite = provider->IsFavorite(record.trackId);
            row.source = Tr(provider->Name());
        } else if (record.provider.empty()) {
            row.handle = metadb::get()->handle_create(record.path.c_str(), record.subsong);
            row.albumKey = L"l:" + media::Lower(row.album) + L'\x1f' + media::Lower(row.artist);
            row.artKey = record.album.empty() ? L"stats:" + Wide(record.key.c_str())
                                              : L"stats:" + media::Lower(row.album) + L'\x1f' + media::Lower(row.artist);
            row.favorite = media::IsFavorite(row.handle);
            row.source = Tr(L"Local Music");
        } else {
            continue;  // a provider that is no longer compiled in
        }
        FinishRow(row);
        row.playCount = record.count;
        rows.push_back(std::move(row));
    }
    return rows;
}

}  // namespace

// ---- SongTableSource ------------------------------------------------------------------------

SongTableSource::SongTableSource(std::vector<SongTableRow> rows, bool showSource, std::wstring emptyText,
                                 std::wstring columnKey)
    : m_all(std::move(rows)),
      m_emptyText(std::move(emptyText)),
      m_columnKey(std::move(columnKey)),
      m_showSource(showSource) {
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
        m_columns = {{L"#", 34, false, true}, media::ArtColumn(false), {L"Title", 0, true},
                     {L"Length", 56, false, true}, {L"Artist", 170}, {L"Album", 170}};
        m_fields = {kNumber, kArt, kTitle, kLength, kArtist, kAlbum};
    }
    if (m_showSource) {
        m_columns.push_back({L"Source", 140});
        m_fields.push_back(kSource);
    }
    m_columns.push_back({L"Count", 70, false, true, !m_showPlayCount});
    m_fields.push_back(kPlayCount);
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
    const bool sortable = CanSort(column);
    m_sortColumn = sortable ? column : SIZE_MAX;
    m_sortDescending = sortable && descending;
    ApplySort();
}

void SongTableSource::SetGrouping(bool enabled) {
    if (m_grouping == enabled) return;
    m_grouping = enabled;
    BuildGroups();
}

void SongTableSource::ShowPlayCount() {
    m_showPlayCount = true;
    SetViewStyle(m_style);
}

void SongTableSource::RefreshPlayCounts() {
    for (auto& row : m_all) row.playCount = media::PlayStats::Get().CountFor(row.handle);
}

void SongTableSource::RefreshFavorites() {
    for (auto& row : m_all)
        if (row.provider) row.favorite = row.provider->IsFavorite(row.trackId);
}

size_t SongTableSource::RemoveUnfavorited() {
    return RemoveRows([](const SongTableRow& row) { return row.provider && !row.provider->IsFavorite(row.trackId); });
}

// Section key and label of a row under the current sort column.
std::pair<std::wstring, std::wstring> SongTableSource::GroupInfo(size_t index) const {
    const SongTableRow& item = m_all[index];
    switch (m_fields[m_sortColumn]) {
    case kTitle: {
        wchar_t c = item.title.empty() ? L'#' : static_cast<wchar_t>(std::towupper(item.title[0]));
        if (std::iswpunct(static_cast<wint_t>(c)) || std::iswspace(static_cast<wint_t>(c))) c = L'#';
        const std::wstring label(1, c);
        return {label, label};
    }
    case kLength: {
        const long long minutes = item.lengthMs / 60000;
        return {std::to_wstring(minutes), std::to_wstring(minutes) + Tr(minutes == 1 ? L" minute" : L" minutes")};
    }
    case kArtist: return {media::Lower(item.artist), ArtistLabel(item)};
    case kAlbum: return {media::Lower(item.album), AlbumLabel(item)};
    case kSource: return {item.source, item.source};
    case kPlayCount: return {std::to_wstring(item.playCount), std::to_wstring(item.playCount)};
    case kFavorite: return {item.favorite ? L"1" : L"0", Tr(item.favorite ? L"Favorite" : L"Not in Favorites")};
    default: return {};  // # is the row number itself: nothing to group by
    }
}

void SongTableSource::SortOrder() {
    m_order.resize(m_all.size());
    for (size_t i = 0; i < m_order.size(); ++i) m_order[i] = i;
    const bool tiles = m_style == media::ViewStyle::Tiles;
    const int field = m_sortColumn < m_fields.size() ? m_fields[m_sortColumn] : -1;
    // Unsorted Details keeps the order the view was built with (a playlist's order, a
    // hot-song chart). Expanded tile always collects an album's songs into one column,
    // ordered by where the album first appears, and a column sort applies inside albums.
    if (!tiles && field < 0) return;
    auto key = [&](size_t index) {
        const SongTableRow& item = m_all[index];
        switch (field) {
        case kNumber: return Padded(static_cast<long long>(index));
        case kTitle: return item.title;
        case kLength: return Padded(item.lengthMs);
        case kArtist: return ArtistLabel(item);
        case kAlbum: return AlbumLabel(item);
        case kSource: return item.source;
        case kPlayCount: return Padded(item.playCount);
        case kFavorite: return std::wstring(item.favorite ? L"1" : L"0");
        default: return std::wstring();
        }
    };
    std::vector<std::wstring> keys(m_all.size());
    if (field >= 0)
        for (size_t i = 0; i < keys.size(); ++i) keys[i] = key(i);
    std::unordered_map<std::wstring, size_t> albumRank;
    if (tiles)
        for (size_t i = 0; i < m_all.size(); ++i) albumRank.emplace(m_all[i].albumKey, i);
    const bool desc = m_sortDescending;
    std::stable_sort(m_order.begin(), m_order.end(), [&](size_t a, size_t b) {
        if (tiles && m_all[a].albumKey != m_all[b].albumKey)
            return albumRank[m_all[a].albumKey] < albumRank[m_all[b].albumKey];
        if (field >= 0)
            if (const int c = CompareText(keys[a], keys[b])) return desc ? c > 0 : c < 0;
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

size_t SongTableSource::RemoveRows(const std::function<bool(const SongTableRow&)>& remove) {
    const size_t before = m_all.size();
    m_all.erase(std::remove_if(m_all.begin(), m_all.end(), remove), m_all.end());
    if (m_all.size() == before) return 0;
    m_selected.assign(m_all.size(), 0);
    m_focus = SIZE_MAX;
    SortOrder();
    RebuildVisible();
    return before - m_all.size();
}

void SongTableSource::RebuildVisible() {
    m_visible.clear();
    for (size_t index : m_order)
        if (media::MatchesFilter(m_all[index].search, m_filter)) m_visible.push_back(index);
    BuildGroups();
}

void SongTableSource::BuildGroups() {
    m_groups.clear();
    if (m_visible.empty()) return;
    const bool tiles = m_style == media::ViewStyle::Tiles;
    if (!tiles && (m_sortColumn >= m_fields.size() || !m_grouping)) return;  // one flat list

    for (size_t r = 0; r < m_visible.size(); ++r) {
        const SongTableRow& item = m_all[m_visible[r]];
        std::wstring key, header;
        if (tiles) {
            key = L"a:" + item.albumKey;
            header = AlbumLabel(item);
        } else {
            auto info = GroupInfo(m_visible[r]);
            if (info.second.empty()) {  // sorted by #: the row numbers are not a grouping
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
            using media::Link;
            group.lines[1] = ArtistLabel(item);
            if (item.provider) {
                group.links[0] = Link::Remote(Link::Kind::RemoteAlbum, item.provider->Key(), item.albumId);
                group.links[1] = Link::Remote(Link::Kind::RemoteArtist, item.provider->Key(), item.artistId);
            } else {
                if (!item.album.empty()) group.links[0] = {Link::Kind::LocalAlbum, media::GroupKind::Album, item.album};
                // Local favorites: the first artist of the credit opens that artist.
                std::wstring first = item.artist.substr(0, item.artist.find(L"; "));
                if (!first.empty()) group.links[1] = {Link::Kind::LocalArtist, media::GroupKind::Artist, first};
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
    case kPlayCount: return std::to_wstring(item.playCount);
    case kSource: return item.source;
    default: return {};  // the album, art and favorite columns are drawn by the list
    }
}

bool SongTableSource::IsPlaying(size_t row) const {
    metadb_handle_ptr now;
    return row < m_visible.size() && playback_control::get()->get_now_playing(now) && now == Item(row);
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
    if (!track.is_valid()) return SIZE_MAX;
    for (size_t r = 0; r < m_visible.size(); ++r)
        if (Item(r) == track) return r;
    return SIZE_MAX;
}

void SongTableSource::Activate(size_t row) {
    if (row < m_visible.size()) media::PlayItems(VisibleItems(), row);
}

void SongTableSource::ContextMenu(HWND owner, POINT screen) {
    auto items = SelectedItems();
    if (items.get_count() == 0 && m_focus < m_visible.size()) items.add_item(Item(m_focus));
    if (items.get_count()) RunContextMenu(owner, items, screen);
}

metadb_handle_list SongTableSource::SelectedItems() const {
    metadb_handle_list items;
    for (size_t index : m_visible)
        if (m_selected[index]) items.add_item(m_all[index].handle);
    return items;
}

metadb_handle_list SongTableSource::VisibleItems() const {
    metadb_handle_list items;
    for (size_t index : m_visible) items.add_item(m_all[index].handle);
    return items;
}

void SongTableSource::SetFilter(std::wstring_view filterLower) {
    m_filter.assign(filterLower);
    m_focus = SIZE_MAX;
    RebuildVisible();
}

// ---- concrete tables ----------------------------------------------------------------------

RemoteSongSource::RemoteSongSource(stream::Provider& provider, const std::vector<stream::Track>& tracks,
                                   std::wstring emptyText)
    : SongTableSource(TrackRows(provider, tracks), false, std::move(emptyText), L"stream-songs"),
      m_provider(provider) {}

void RemoteSongSource::AppendTracks(const std::vector<stream::Track>& tracks) {
    Append(TrackRows(m_provider, tracks));
}

size_t RemoteSongSource::RemoveTracks(const std::vector<stream::Id>& ids) {
    if (ids.empty()) return 0;
    const std::unordered_set<stream::Id> removed(ids.begin(), ids.end());
    return RemoveRows([&removed](const SongTableRow& row) { return removed.count(row.trackId) != 0; });
}

AllFavoritesSource::AllFavoritesSource(const std::vector<metadb_handle_ptr>& local, const RemoteFavorites& remote)
    : SongTableSource(FavoriteRows(local, remote), true, L"No favorites.", L"all-favorites") {}


PlayStatsSource::PlayStatsSource(const std::vector<media::PlayRecord>& records, std::wstring emptyText)
    : SongTableSource(PlayStatRows(records), true, std::move(emptyText), L"all-play-stats") {
    ShowPlayCount();
}

}  // namespace glasswave::ui
