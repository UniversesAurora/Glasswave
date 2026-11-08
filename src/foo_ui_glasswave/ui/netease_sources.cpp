#include "pch.h"
#include "ui/netease_sources.h"
#include "ui/win7_menu.h"
#include "netease/catalog.h"
#include "netease/handle.h"
#include "netease/provider.h"

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

std::wstring LengthText(int milliseconds) {
    const int total = milliseconds > 0 ? milliseconds / 1000 : 0;
    wchar_t buffer[16];
    swprintf_s(buffer, L"%d:%02d", total / 60, total % 60);
    return buffer;
}

std::wstring ArtistText(const netease::Song& song) { return netease::JoinArtists(song.artists); }

bool SameHandle(const metadb_handle_ptr& a, const metadb_handle_ptr& b) {
    return a.is_valid() && b.is_valid() && a == b;
}

}  // namespace

NeteaseSongSource::NeteaseSongSource(std::vector<netease::Song> songs, std::wstring emptyText)
    : m_songs(std::move(songs)), m_emptyText(std::move(emptyText)) {
    m_columns = {
        {L"#", 34, false, true},
        media::ArtColumn(true),
        {L"Title", 0, true},
        {L"Length", 56, false, true},
        {L"Artist", 170},
        {L"Album", 170},
        {L"Favorite", 58, false, false, false, "<favorite>"},
    };
    m_selected.assign(m_songs.size(), 0);
    RebuildRows();
}

void NeteaseSongSource::RebuildRows() {
    m_rows.clear();
    for (size_t i = 0; i < m_songs.size(); ++i) {
        const auto& song = m_songs[i];
        if (!m_filter.empty()) {
            const std::wstring haystack =
                Lower(song.title) + L"\n" + Lower(ArtistText(song)) + L"\n" + Lower(song.album);
            if (!Contains(haystack, m_filter)) continue;
        }
        m_rows.push_back(i);
    }
    if (m_sortColumn != SIZE_MAX) {
        const bool desc = m_sortDescending;
        std::stable_sort(m_rows.begin(), m_rows.end(), [&](size_t a, size_t b) {
            const auto& x = m_songs[a];
            const auto& y = m_songs[b];
            std::wstring xv, yv;
            switch (m_sortColumn) {
            case 3: xv = std::to_wstring(x.durationMs); yv = std::to_wstring(y.durationMs); break;
            case 4: xv = ArtistText(x); yv = ArtistText(y); break;
            case 5: xv = x.album; yv = y.album; break;
            case 2:
            default: xv = x.title; yv = y.title; break;
            }
            return desc ? xv > yv : xv < yv;
        });
    }
}

std::wstring NeteaseSongSource::Cell(size_t row, size_t column) const {
    if (row >= m_rows.size()) return {};
    const auto& song = m_songs[m_rows[row]];
    switch (column) {
    case 0: return std::to_wstring(row + 1);
    case 2: return song.title;
    case 3: return LengthText(song.durationMs);
    case 4: return ArtistText(song);
    case 5: return song.album;
    default: return {};
    }
}

bool NeteaseSongSource::IsPlaying(size_t row) const {
    metadb_handle_ptr now;
    return row < m_rows.size() && playback_control::get()->get_now_playing(now) && SameHandle(now, Item(row));
}

bool NeteaseSongSource::CanSort(size_t column) const {
    return column != 1 && column != 6 && column < m_columns.size();
}

void NeteaseSongSource::Sort(size_t column, bool descending) {
    if (!CanSort(column)) {
        m_sortColumn = SIZE_MAX;
        m_sortDescending = false;
    } else {
        m_sortColumn = column;
        m_sortDescending = descending;
    }
    RebuildRows();
}

void NeteaseSongSource::SelectOnly(size_t row) {
    std::fill(m_selected.begin(), m_selected.end(), uint8_t{0});
    if (row < m_rows.size()) m_selected[m_rows[row]] = 1;
    m_focus = row;
}

void NeteaseSongSource::Toggle(size_t row) {
    if (row < m_rows.size()) m_selected[m_rows[row]] ^= 1;
    m_focus = row;
}

void NeteaseSongSource::SelectRange(size_t a, size_t b, bool keepOthers) {
    if (!keepOthers) std::fill(m_selected.begin(), m_selected.end(), uint8_t{0});
    if (a > b) std::swap(a, b);
    for (size_t r = a; r <= b && r < m_rows.size(); ++r) m_selected[m_rows[r]] = 1;
}

void NeteaseSongSource::SelectAll() {
    for (size_t r : m_rows) m_selected[r] = 1;
}

void NeteaseSongSource::SetSelection(const std::vector<size_t>& rows) {
    std::fill(m_selected.begin(), m_selected.end(), uint8_t{0});
    for (size_t row : rows)
        if (row < m_rows.size()) m_selected[m_rows[row]] = 1;
}

metadb_handle_ptr NeteaseSongSource::Item(size_t row) const {
    if (row >= m_rows.size()) return {};
    return netease::Catalog::Get().Handle(m_songs[m_rows[row]].id);
}

std::wstring NeteaseSongSource::ArtKey(size_t row) const {
    if (row >= m_rows.size()) return {};
    const auto& song = m_songs[m_rows[row]];
    return L"ncm:" + std::to_wstring(song.albumId ? song.albumId : song.id);
}

size_t NeteaseSongSource::FindRow(const metadb_handle_ptr& track) const {
    for (size_t r = 0; r < m_rows.size(); ++r)
        if (SameHandle(Item(r), track)) return r;
    return SIZE_MAX;
}

void NeteaseSongSource::Activate(size_t row) {
    if (row >= m_rows.size()) return;
    metadb_handle_list items;
    for (size_t r = 0; r < m_rows.size(); ++r) items.add_item(Item(r));
    media::PlayItems(items, row);
}

void NeteaseSongSource::ContextMenu(HWND owner, POINT screen) {
    auto items = SelectedItems();
    if (items.get_count() == 0 && m_focus < m_rows.size()) items.add_item(Item(m_focus));
    if (items.get_count()) RunContextMenu(owner, items, screen);
}

metadb_handle_list NeteaseSongSource::SelectedItems() const {
    metadb_handle_list items;
    for (size_t r = 0; r < m_rows.size(); ++r)
        if (m_selected[m_rows[r]]) items.add_item(Item(r));
    return items;
}

void NeteaseSongSource::SetFilter(std::wstring_view filterLower) {
    m_filter.assign(filterLower);
    m_selected.assign(m_songs.size(), 0);
    m_focus = SIZE_MAX;
    RebuildRows();
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
