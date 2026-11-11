#include "pch.h"
#include "ui/remote_grids.h"
#include "ui/i18n.h"

namespace glasswave::ui {

namespace {

int CompareText(const std::wstring& a, const std::wstring& b) {
    return CompareStringEx(LOCALE_NAME_USER_DEFAULT, LINGUISTIC_IGNORECASE | SORT_DIGITSASNUMBERS, a.c_str(),
                           static_cast<int>(a.size()), b.c_str(), static_cast<int>(b.size()), nullptr, nullptr, 0) -
           2;
}

std::wstring CountText(int n, const wchar_t* unit) {
    return n < 0 ? std::wstring() : std::to_wstring(n) + L" " + Tr(unit);
}

std::wstring CountCell(int n) { return n < 0 ? std::wstring() : std::to_wstring(n); }

std::wstring AlbumSubtitle(const stream::Album& album) {
    const std::wstring count = CountText(album.trackCount, L"songs");
    if (!album.year) return count;
    const std::wstring year = std::to_wstring(album.year);
    return count.empty() ? year : year + L" · " + count;
}

}  // namespace

// ---- GridSource ---------------------------------------------------------------------------

GridSource::GridSource(std::vector<media::Column> columns, std::wstring emptyText)
    : m_columns(std::move(columns)), m_emptyText(std::move(emptyText)) {}

std::wstring GridSource::SelectionKey(size_t item) const {
    if (item >= SIZE_MAX / 2) {
        return L"\x1f" + std::to_wstring(SIZE_MAX - item);
    }
    return item < m_itemKeys.size() ? m_itemKeys[item] : std::wstring();
}

void GridSource::RebuildItemKeys() {
    m_itemKeys.resize(ItemCount());
    std::map<std::wstring, size_t> occurrences;
    for (size_t item = 0; item < m_itemKeys.size(); ++item) {
        const std::wstring identity = Identity(item);
        m_itemKeys[item] = identity + L'\x1e' + std::to_wstring(occurrences[identity]++);
    }
}

std::wstring GridSource::FocusKey() const {
    return m_focus < m_rows.size() ? SelectionKey(m_rows[m_focus]) : std::wstring();
}

std::wstring GridSource::RowKey(size_t row) const {
    if (row >= m_rows.size()) return {};
    const size_t item = m_rows[row];
    if (item >= SIZE_MAX / 2) return L"\x1fleading:" + std::to_wstring(SIZE_MAX - item);
    return Identity(item);
}

void GridSource::SelectOnly(size_t row) {
    m_selected.clear();
    if (row < m_rows.size()) m_selected.insert(SelectionKey(m_rows[row]));
    m_focus = row;
}

void GridSource::Toggle(size_t row) {
    if (row >= m_rows.size()) return;
    const auto key = SelectionKey(m_rows[row]);
    if (!m_selected.erase(key)) m_selected.insert(key);
    m_focus = row;
}

void GridSource::SelectRange(size_t a, size_t b, bool keepOthers) {
    if (!keepOthers) m_selected.clear();
    if (a > b) std::swap(a, b);
    for (size_t r = a; r <= b && r < m_rows.size(); ++r) m_selected.insert(SelectionKey(m_rows[r]));
}

void GridSource::SelectAll() {
    for (size_t item : m_rows) m_selected.insert(SelectionKey(item));
}

void GridSource::SetSelection(const std::vector<size_t>& rows) {
    m_selected.clear();
    for (size_t row : rows)
        if (row < m_rows.size()) m_selected.insert(SelectionKey(m_rows[row]));
}

void GridSource::SetFilter(std::wstring_view filterLower) {
    m_filter.assign(filterLower);
    Refilter();
}

bool GridSource::CanSort(size_t column) const { return column < m_columns.size(); }

void GridSource::Sort(size_t column, bool descending) {
    m_sortColumn = CanSort(column) ? column : SIZE_MAX;
    m_sortDescending = m_sortColumn != SIZE_MAX && descending;
    Refilter();
}

void GridSource::Refilter() {
    const std::wstring oldFocus = FocusKey();
    m_rows.clear();
    for (size_t i = 0; i < Leading(); ++i) m_rows.push_back(SIZE_MAX - i);  // always shown
    const size_t firstItemRow = m_rows.size();
    for (size_t i = 0, n = ItemCount(); i < n; ++i)
        if (media::MatchesFilter(media::Lower(FilterText(i)), m_filter)) m_rows.push_back(i);

    if (m_sortColumn < m_columns.size()) {
        const bool descending = m_sortDescending;
        std::stable_sort(m_rows.begin() + static_cast<std::ptrdiff_t>(firstItemRow), m_rows.end(),
                         [this, descending](size_t a, size_t b) {
                             const int c = CompareText(ItemCell(a, m_sortColumn), ItemCell(b, m_sortColumn));
                             return descending ? c > 0 : c < 0;
                         });
    }

    RebuildItemKeys();
    std::unordered_set<std::wstring> valid;
    for (size_t item : m_rows) valid.insert(SelectionKey(item));
    std::erase_if(m_selected, [&](const std::wstring& key) { return !valid.count(key); });

    m_focus = SIZE_MAX;
    if (!oldFocus.empty()) {
        for (size_t row = 0; row < m_rows.size(); ++row)
            if (SelectionKey(m_rows[row]) == oldFocus) {
                m_focus = row;
                break;
            }
    }
}

void GridSource::ForgetIdentity(const std::wstring& identity) {
    const std::wstring prefix = identity + L'\x1e';
    std::erase_if(m_selected, [&](const std::wstring& key) { return key.starts_with(prefix); });
}

// ---- albums -------------------------------------------------------------------------------

AlbumGridSource::AlbumGridSource(stream::Provider& provider, std::vector<stream::Album> albums,
                                 std::wstring tracksArtist, std::wstring tracksLabel, std::wstring emptyText,
                                 Open open, Menu menu)
    : GridSource({{L"Album", 0, true}, {L"Release year", 80, false, true}, {L"Songs", 60, false, true}},
                 std::move(emptyText)),
      m_provider(provider),
      m_albums(std::move(albums)),
      m_tracksArtist(std::move(tracksArtist)),
      m_tracksLabel(std::move(tracksLabel)),
      m_open(std::move(open)),
      m_menu(std::move(menu)) {
    Refilter();
}

const stream::Album* AlbumGridSource::AlbumAt(size_t row) const {
    return row < m_rows.size() && m_rows[row] < m_albums.size() ? &m_albums[m_rows[row]] : nullptr;
}

std::wstring AlbumGridSource::Cell(size_t row, size_t column) const {
    return row < m_rows.size() ? ItemCell(m_rows[row], column) : std::wstring();
}

std::wstring AlbumGridSource::ItemCell(size_t item, size_t column) const {
    if (item >= SIZE_MAX / 2)
        return column == 0 ? Tr(m_tracksLabel) : std::wstring();
    if (item >= m_albums.size()) return {};
    const stream::Album& album = m_albums[item];
    switch (column) {
    case 0: return album.name;
    case 1: return album.year ? std::to_wstring(album.year) : std::wstring();
    case 2: return CountCell(album.trackCount);
    default: return {};
    }
}

std::wstring AlbumGridSource::Identity(size_t item) const {
    if (item >= m_albums.size()) return {};
    const auto& album = m_albums[item];
    if (!album.id.empty()) return L"id:" + album.id;
    return L"name:" + media::Lower(album.name) + L'\x1f' + media::Lower(stream::JoinArtists(album.artists)) +
           L'\x1f' + std::to_wstring(album.year);
}

media::TileInfo AlbumGridSource::Tile(size_t row) const {
    media::TileInfo tile;
    if (row >= m_rows.size()) return tile;
    if (const stream::Album* album = AlbumAt(row)) {
        tile.art = m_provider.ArtItem(stream::ArtKind::Album, album->id);
        tile.lines = {album->name, AlbumSubtitle(*album)};
    } else {
        tile.icon = 29711;  // WMP's large music note
        tile.lines = {Tr(m_tracksLabel), m_tracksArtist};
    }
    return tile;
}

std::wstring AlbumGridSource::ArtKey(size_t row) const {
    const stream::Album* album = AlbumAt(row);
    return album ? m_provider.ArtKey(stream::ArtKind::Album, album->id) : std::wstring();
}

void AlbumGridSource::Activate(size_t row) {
    if (row < m_rows.size() && m_open) m_open(AlbumAt(row));
}

void AlbumGridSource::ContextMenu(HWND, POINT screen) {
    if (m_menu && Focus() < m_rows.size()) m_menu(screen, AlbumAt(Focus()));
}

void AlbumGridSource::ReplaceAlbums(const std::vector<stream::Album>& albums) {
    m_albums.clear();
    m_albums.reserve(albums.size());
    std::unordered_set<std::wstring> ids;
    for (const auto& album : albums) {
        if (!album.id.empty() && !ids.insert(album.id).second) continue;
        m_albums.push_back(album);
    }
    Refilter();
}

void AlbumGridSource::AppendAlbums(const std::vector<stream::Album>& albums) {
    std::unordered_set<std::wstring> ids;
    for (const auto& album : m_albums)
        if (!album.id.empty()) ids.insert(album.id);
    for (const auto& album : albums) {
        if (!album.id.empty() && !ids.insert(album.id).second) continue;
        m_albums.push_back(album);
    }
    Refilter();
}

void AlbumGridSource::RemoveAlbum(const stream::Id& album) {
    ForgetIdentity(L"id:" + album);
    const auto end = std::remove_if(m_albums.begin(), m_albums.end(),
                                    [&album](const stream::Album& a) { return a.id == album; });
    if (end == m_albums.end()) return;
    m_albums.erase(end, m_albums.end());
    Refilter();
}

// ---- artists ------------------------------------------------------------------------------

ArtistGridSource::ArtistGridSource(stream::Provider& provider, std::vector<stream::Artist> artists,
                                   std::wstring emptyText, Open open, Menu menu)
    : GridSource({{L"Artist", 0, true}, {L"Albums", 70, false, true}, {L"Songs", 70, false, true}},
                 std::move(emptyText)),
      m_provider(provider),
      m_artists(std::move(artists)),
      m_open(std::move(open)),
      m_menu(std::move(menu)) {
    Refilter();
}

std::wstring ArtistGridSource::Cell(size_t row, size_t column) const {
    return row < m_rows.size() ? ItemCell(m_rows[row], column) : std::wstring();
}

std::wstring ArtistGridSource::ItemCell(size_t item, size_t column) const {
    if (item >= m_artists.size()) return {};
    const auto& artist = m_artists[item];
    switch (column) {
    case 0: return artist.name;
    case 1: return CountCell(artist.albumCount);
    case 2: return CountCell(artist.trackCount);
    default: return {};
    }
}

std::wstring ArtistGridSource::Identity(size_t item) const {
    return item < m_artists.size() ? L"id:" + m_artists[item].id : std::wstring();
}

media::TileInfo ArtistGridSource::Tile(size_t row) const {
    media::TileInfo tile;
    if (row >= m_rows.size()) return tile;
    const auto& artist = m_artists[m_rows[row]];
    tile.art = m_provider.ArtItem(stream::ArtKind::Artist, artist.id);
    tile.lines = {artist.name, CountText(artist.albumCount, L"albums")};
    return tile;
}

std::wstring ArtistGridSource::ArtKey(size_t row) const {
    return row < m_rows.size() ? m_provider.ArtKey(stream::ArtKind::Artist, m_artists[m_rows[row]].id)
                               : std::wstring();
}

void ArtistGridSource::Activate(size_t row) {
    if (row < m_rows.size() && m_open) m_open(m_artists[m_rows[row]]);
}

void ArtistGridSource::ContextMenu(HWND, POINT screen) {
    if (m_menu && Focus() < m_rows.size()) m_menu(screen, m_artists[m_rows[Focus()]]);
}

void ArtistGridSource::ReplaceArtists(const std::vector<stream::Artist>& artists) {
    m_artists = artists;
    Refilter();
}

void ArtistGridSource::AppendArtists(const std::vector<stream::Artist>& artists) {
    m_artists.insert(m_artists.end(), artists.begin(), artists.end());
    Refilter();
}

void ArtistGridSource::RemoveArtist(const stream::Id& artist) {
    ForgetIdentity(L"id:" + artist);
    const auto end = std::remove_if(m_artists.begin(), m_artists.end(),
                                    [&artist](const stream::Artist& a) { return a.id == artist; });
    if (end == m_artists.end()) return;
    m_artists.erase(end, m_artists.end());
    Refilter();
}

// ---- playlists ----------------------------------------------------------------------------

PlaylistGridSource::PlaylistGridSource(stream::Provider& provider, std::vector<stream::Playlist> playlists,
                                       std::wstring emptyText, Open open, Menu menu)
    : GridSource({{L"Name", 0, true}, {L"Songs", 80, false, true}}, std::move(emptyText)),
      m_provider(provider),
      m_playlists(std::move(playlists)),
      m_open(std::move(open)),
      m_menu(std::move(menu)) {
    Refilter();
}

std::wstring PlaylistGridSource::Cell(size_t row, size_t column) const {
    return row < m_rows.size() ? ItemCell(m_rows[row], column) : std::wstring();
}

std::wstring PlaylistGridSource::ItemCell(size_t item, size_t column) const {
    if (item >= m_playlists.size()) return {};
    const auto& playlist = m_playlists[item];
    return column == 1 ? CountCell(playlist.trackCount) : playlist.name;
}

std::wstring PlaylistGridSource::Identity(size_t item) const {
    return item < m_playlists.size() ? L"id:" + m_playlists[item].id : std::wstring();
}

media::TileInfo PlaylistGridSource::Tile(size_t row) const {
    media::TileInfo tile;
    if (row >= m_rows.size()) return tile;
    const auto& playlist = m_playlists[m_rows[row]];
    if (playlist.coverUrl.empty())
        tile.icon = 29722;  // WMP's existing large playlist artwork
    else
        tile.art = m_provider.ArtItem(stream::ArtKind::Playlist, playlist.id);
    tile.lines = {playlist.name, CountText(playlist.trackCount, L"songs"), Tr(m_provider.Name())};
    return tile;
}

std::wstring PlaylistGridSource::ArtKey(size_t row) const {
    return row < m_rows.size() ? m_provider.ArtKey(stream::ArtKind::Playlist, m_playlists[m_rows[row]].id)
                               : std::wstring();
}

void PlaylistGridSource::Activate(size_t row) {
    if (row < m_rows.size() && m_open) m_open(m_playlists[m_rows[row]]);
}

void PlaylistGridSource::ContextMenu(HWND, POINT screen) {
    if (m_menu && Focus() < m_rows.size()) m_menu(screen, m_playlists[m_rows[Focus()]]);
}

void PlaylistGridSource::ReplacePlaylists(const std::vector<stream::Playlist>& playlists) {
    m_playlists = playlists;
    Refilter();
}

void PlaylistGridSource::AppendPlaylists(const std::vector<stream::Playlist>& playlists) {
    m_playlists.insert(m_playlists.end(), playlists.begin(), playlists.end());
    Refilter();
}

void PlaylistGridSource::RemovePlaylist(const stream::Id& playlist) {
    ForgetIdentity(L"id:" + playlist);
    const auto end = std::remove_if(m_playlists.begin(), m_playlists.end(),
                                    [&playlist](const stream::Playlist& p) { return p.id == playlist; });
    if (end == m_playlists.end()) return;
    m_playlists.erase(end, m_playlists.end());
    Refilter();
}

void PlaylistGridSource::RenamePlaylist(const stream::Id& playlist, const std::wstring& name) {
    for (auto& p : m_playlists)
        if (p.id == playlist) p.name = name;
    Refilter();
}

void PlaylistGridSource::SetViewStyle(media::ViewStyle style) {
    m_style = style;
    Refilter();
}

}  // namespace glasswave::ui
