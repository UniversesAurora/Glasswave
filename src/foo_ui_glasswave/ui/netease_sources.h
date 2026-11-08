#pragma once

#include "media/list_sources.h"
#include "netease/types.h"

#include <functional>
#include <vector>

namespace glasswave::ui {

struct SongTableRow {
    metadb_handle_ptr handle;
    std::wstring title;
    std::wstring artist;
    std::wstring album;
    std::wstring length;
    int lengthMs = 0;
    std::wstring source;    // Local Music / NetEase Cloud Music, when shown
    std::wstring albumKey;  // album grouping key
    std::wstring artKey;
    std::wstring search;    // lower-cased title/artist/album for filtering
    bool favorite = false;
};

// Shared table for song rows from one or more providers: Details / Expanded tile,
// optional Source column, sort-driven regrouping, selection, playback and menus.
class SongTableSource : public media::ListSource {
public:
    const std::vector<media::Column>& Columns() const override { return m_columns; }
    std::wstring ColumnSetKey() const override;
    size_t Count() const override { return m_visible.size(); }
    std::wstring Cell(size_t row, size_t column) const override;
    std::wstring EmptyText() const override { return m_emptyText; }
    bool IsPlaying(size_t row) const override;
    const std::vector<media::RowGroup>* Groups() const override { return &m_groups; }
    bool GroupArt() const override { return m_style == media::ViewStyle::Tiles; }
    bool SupportsViewStyle(media::ViewStyle style) const override { return style != media::ViewStyle::Icons; }
    media::ViewStyle GetViewStyle() const override { return m_style; }
    void SetViewStyle(media::ViewStyle style) override;
    bool CanGroup() const override { return m_style == media::ViewStyle::Details; }
    bool Grouping() const override { return m_grouping; }
    void SetGrouping(bool enabled) override;
    bool CanSort(size_t column) const override;
    void Sort(size_t column, bool descending) override;
    size_t SortColumn() const override { return m_sortColumn; }
    bool SortDescending() const override { return m_sortDescending; }
    bool IsSelected(size_t row) const override {
        return row < m_visible.size() && m_selected[m_visible[row]] != 0;
    }
    void SelectOnly(size_t row) override;
    void Toggle(size_t row) override;
    void SelectRange(size_t a, size_t b, bool keepOthers) override;
    void SelectAll() override;
    void SetSelection(const std::vector<size_t>& rows) override;
    size_t Focus() const override { return m_focus; }
    void SetFocus(size_t row) override { m_focus = row; }
    metadb_handle_ptr Item(size_t row) const override;
    std::wstring ArtKey(size_t row) const override;
    metadb_handle_ptr ArtTrack(size_t row) const override { return Item(row); }
    size_t FindRow(const metadb_handle_ptr& track) const override;
    void Activate(size_t row) override;
    void ContextMenu(HWND owner, POINT screen) override;
    metadb_handle_list SelectedItems() const override;
    void SetFilter(std::wstring_view filterLower) override;

protected:
    SongTableSource(std::vector<SongTableRow> rows, bool showSource, std::wstring emptyText, std::wstring columnKey);

private:
    void RebuildVisible();
    void BuildGroups();
    void ApplySort();
    std::pair<std::wstring, std::wstring> GroupInfo(size_t row) const;

    std::vector<SongTableRow> m_all;
    std::vector<size_t> m_visible;
    std::vector<size_t> m_order;
    std::vector<media::RowGroup> m_groups;
    std::vector<uint8_t> m_selected;
    size_t m_focus = SIZE_MAX;
    std::wstring m_emptyText;
    std::wstring m_filter;
    std::wstring m_columnKey;
    std::vector<media::Column> m_columns;
    std::vector<int> m_fields;
    media::ViewStyle m_style = media::ViewStyle::Details;
    size_t m_sortColumn = SIZE_MAX;
    bool m_sortDescending = false;
    bool m_showSource = false;
    bool m_grouping = true;
};

// NetEase songs in a table (liked list, playlist, search results).
class NeteaseSongSource : public SongTableSource {
public:
    NeteaseSongSource(std::vector<netease::Song> songs, std::wstring emptyText);
};

// Favorites from every provider: local Favorites and the NetEase liked list, with a
// sortable Source column. The default view is flat; sorting by a column groups the rows.
class AllFavoritesSource : public SongTableSource {
public:
    AllFavoritesSource(std::vector<metadb_handle_ptr> local, std::vector<netease::Song> remote);
};

// NetEase playlists as WMP-style tiles or a Details list of name / song count.
class NeteasePlaylistSource : public media::ListSource {public:
    using Open = std::function<void(int64_t playlistId, std::wstring name)>;
    NeteasePlaylistSource(std::vector<netease::Playlist> playlists, std::wstring emptyText, Open open);

    const std::vector<media::Column>& Columns() const override { return m_columns; }
    std::wstring ColumnSetKey() const override { return L"netease-playlists"; }
    size_t Count() const override { return m_rows.size(); }
    std::wstring Cell(size_t row, size_t column) const override;
    std::wstring EmptyText() const override { return m_emptyText; }
    media::TileStyle Tiles() const override {
        return m_style == media::ViewStyle::Icons ? media::TileStyle::Album : media::TileStyle::None;
    }
    media::TileInfo Tile(size_t row) const override;
    bool SupportsViewStyle(media::ViewStyle style) const override { return style != media::ViewStyle::Tiles; }
    media::ViewStyle GetViewStyle() const override { return m_style; }
    void SetViewStyle(media::ViewStyle style) override { m_style = style; }
    bool IsSelected(size_t row) const override { return row < m_selected.size() && m_selected[row]; }
    void SelectOnly(size_t row) override;
    void Toggle(size_t row) override;
    void SelectRange(size_t a, size_t b, bool keepOthers) override;
    void SelectAll() override;
    void SetSelection(const std::vector<size_t>& rows) override;
    size_t Focus() const override { return m_focus; }
    void SetFocus(size_t row) override { m_focus = row; }
    void Activate(size_t row) override;
    void ContextMenu(HWND owner, POINT screen) override;
    metadb_handle_list SelectedItems() const override { return {}; }
    void SetFilter(std::wstring_view filterLower) override;

private:
    std::vector<netease::Playlist> m_playlists;
    std::vector<size_t> m_rows;
    std::vector<uint8_t> m_selected;
    size_t m_focus = SIZE_MAX;
    std::wstring m_emptyText;
    std::wstring m_filter;
    std::vector<media::Column> m_columns;
    Open m_open;
    media::ViewStyle m_style = media::ViewStyle::Icons;
};

}  // namespace glasswave::ui
