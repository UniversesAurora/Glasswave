#include "pch.h"
#include "ui/remote_grids.h"
#include "ui/i18n.h"

namespace glasswave::ui {

namespace {

std::wstring CountText(int n, const wchar_t* unit) { return std::to_wstring(n) + L" " + Tr(unit); }

std::wstring AlbumSubtitle(const stream::Album& album) {
    const std::wstring count = CountText(album.trackCount, L"songs");
    return album.year ? std::to_wstring(album.year) + L" · " + count : count;
}

}  // namespace

// ---- GridSource ---------------------------------------------------------------------------

GridSource::GridSource(std::vector<media::Column> columns, std::wstring emptyText)
    : m_columns(std::move(columns)), m_emptyText(std::move(emptyText)) {}

void GridSource::SelectOnly(size_t row) {
    std::fill(m_selected.begin(), m_selected.end(), uint8_t{0});
    if (row < m_selected.size()) m_selected[row] = 1;
    m_focus = row;
}

void GridSource::Toggle(size_t row) {
    if (row < m_selected.size()) m_selected[row] ^= 1;
    m_focus = row;
}

void GridSource::SelectRange(size_t a, size_t b, bool keepOthers) {
    if (!keepOthers) std::fill(m_selected.begin(), m_selected.end(), uint8_t{0});
    if (a > b) std::swap(a, b);
    for (size_t r = a; r <= b && r < m_selected.size(); ++r) m_selected[r] = 1;
}

void GridSource::SelectAll() { std::fill(m_selected.begin(), m_selected.end(), uint8_t{1}); }

void GridSource::SetSelection(const std::vector<size_t>& rows) {
    std::fill(m_selected.begin(), m_selected.end(), uint8_t{0});
    for (size_t row : rows)
        if (row < m_selected.size()) m_selected[row] = 1;
}

void GridSource::SetFilter(std::wstring_view filterLower) {
    m_filter.assign(filterLower);
    m_rows.clear();
    for (size_t i = 0; i < Leading(); ++i) m_rows.push_back(SIZE_MAX - i);  // always shown
    for (size_t i = 0, n = ItemCount(); i < n; ++i)
        if (media::MatchesFilter(media::Lower(FilterText(i)), m_filter)) m_rows.push_back(i);
    m_selected.assign(m_rows.size(), 0);
    m_focus = SIZE_MAX;
}

// ---- albums -------------------------------------------------------------------------------

AlbumGridSource::AlbumGridSource(stream::Provider& provider, std::vector<stream::Album> albums,
                                 std::wstring hotSongsArtist, std::wstring emptyText, Open open, Menu menu)
    : GridSource({{L"Album", 0, true}, {L"Release year", 80, false, true}, {L"Songs", 60, false, true}},
                 std::move(emptyText)),
      m_provider(provider),
      m_albums(std::move(albums)),
      m_hotSongsArtist(std::move(hotSongsArtist)),
      m_open(std::move(open)),
      m_menu(std::move(menu)) {
    Refilter();
}

const stream::Album* AlbumGridSource::AlbumAt(size_t row) const {
    return row < m_rows.size() && m_rows[row] < m_albums.size() ? &m_albums[m_rows[row]] : nullptr;
}

std::wstring AlbumGridSource::Cell(size_t row, size_t column) const {
    const stream::Album* album = AlbumAt(row);
    if (!album) return row < m_rows.size() && column == 0 ? Tr(L"Hot songs") : std::wstring();
    switch (column) {
    case 0: return album->name;
    case 1: return album->year ? std::to_wstring(album->year) : std::wstring();
    case 2: return std::to_wstring(album->trackCount);
    default: return {};
    }
}

media::TileInfo AlbumGridSource::Tile(size_t row) const {
    media::TileInfo tile;
    if (row >= m_rows.size()) return tile;
    if (const stream::Album* album = AlbumAt(row)) {
        tile.art = m_provider.ArtItem(stream::ArtKind::Album, album->id);
        tile.lines = {album->name, AlbumSubtitle(*album)};
    } else {
        tile.icon = 29711;  // WMP's large music note
        tile.lines = {Tr(L"Hot songs"), m_hotSongsArtist};
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

void AlbumGridSource::AppendAlbums(const std::vector<stream::Album>& albums) {
    if (albums.empty()) return;
    m_albums.insert(m_albums.end(), albums.begin(), albums.end());
    // Appending keeps the rows already shown and their selection.
    const size_t before = m_rows.size();
    std::vector<size_t> selected;
    for (size_t r = 0; r < before; ++r)
        if (IsSelected(r)) selected.push_back(r);
    const size_t focus = Focus();
    Refilter();
    SetSelection(selected);
    SetFocus(focus);
}

void AlbumGridSource::RemoveAlbum(const stream::Id& album) {
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
    if (row >= m_rows.size()) return {};
    const auto& artist = m_artists[m_rows[row]];
    switch (column) {
    case 0: return artist.name;
    case 1: return std::to_wstring(artist.albumCount);
    case 2: return std::to_wstring(artist.trackCount);
    default: return {};
    }
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
    if (row >= m_rows.size()) return {};
    const auto& playlist = m_playlists[m_rows[row]];
    return column == 1 ? std::to_wstring(playlist.trackCount) : playlist.name;
}

media::TileInfo PlaylistGridSource::Tile(size_t row) const {
    media::TileInfo tile;
    if (row >= m_rows.size()) return tile;
    const auto& playlist = m_playlists[m_rows[row]];
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

void PlaylistGridSource::RemovePlaylist(const stream::Id& playlist) {
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

}  // namespace glasswave::ui
