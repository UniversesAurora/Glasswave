#pragma once

#include "media/list_sources.h"
#include "netease/types.h"

#include <functional>
#include <vector>

namespace glasswave::ui {

// NetEase songs in a Details list (liked list, playlist, search results). Rows carry
// ncm:// handles, so playback, queueing and context menus reuse the local paths.
class NeteaseSongSource : public media::ListSource {
public:
    NeteaseSongSource(std::vector<netease::Song> songs, std::wstring emptyText);

    const std::vector<media::Column>& Columns() const override { return m_columns; }
    std::wstring ColumnSetKey() const override { return L"netease-songs"; }
    size_t Count() const override { return m_rows.size(); }
    std::wstring Cell(size_t row, size_t column) const override;
    std::wstring EmptyText() const override { return m_emptyText; }
    bool IsPlaying(size_t row) const override;
    bool SupportsViewStyle(media::ViewStyle style) const override { return style == media::ViewStyle::Details; }
    media::ViewStyle GetViewStyle() const override { return media::ViewStyle::Details; }
    bool CanSort(size_t column) const override;
    void Sort(size_t column, bool descending) override;
    size_t SortColumn() const override { return m_sortColumn; }
    bool SortDescending() const override { return m_sortDescending; }
    bool IsSelected(size_t row) const override { return row < m_selected.size() && m_selected[row]; }
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

private:
    void RebuildRows();

    std::vector<netease::Song> m_songs;
    std::vector<size_t> m_rows;
    std::vector<uint8_t> m_selected;
    size_t m_focus = SIZE_MAX;
    std::wstring m_emptyText;
    std::wstring m_filter;
    std::vector<media::Column> m_columns;
    size_t m_sortColumn = SIZE_MAX;
    bool m_sortDescending = false;
};

// Favorites from every provider: local Favorites handles and the NetEase liked list,
// split into two section headers. Local tracks keep their metadb metadata.
class AllFavoritesSource : public media::ListSource {
public:
    AllFavoritesSource(std::vector<metadb_handle_ptr> local, std::vector<netease::Song> remote);

    const std::vector<media::Column>& Columns() const override { return m_columns; }
    std::wstring ColumnSetKey() const override { return L"all-favorites"; }
    size_t Count() const override { return m_rows.size(); }
    std::wstring Cell(size_t row, size_t column) const override;
    std::wstring EmptyText() const override { return L"No favorites."; }
    bool IsPlaying(size_t row) const override;
    const std::vector<media::RowGroup>* Groups() const override { return &m_groups; }
    bool GroupArt() const override { return false; }
    bool SupportsViewStyle(media::ViewStyle style) const override { return style == media::ViewStyle::Details; }
    media::ViewStyle GetViewStyle() const override { return media::ViewStyle::Details; }
    bool IsSelected(size_t row) const override { return row < m_selected.size() && m_selected[row]; }
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

private:
    enum class RowKind { Local, Remote };
    struct Row {
        RowKind kind = RowKind::Local;
        size_t index = 0;
    };
    void RebuildRows();
    std::wstring LocalField(const metadb_handle_ptr& track, const char* script) const;

    std::vector<metadb_handle_ptr> m_local;
    std::vector<netease::Song> m_remote;
    std::vector<Row> m_rows;
    std::vector<media::RowGroup> m_groups;
    std::vector<uint8_t> m_selected;
    size_t m_focus = SIZE_MAX;
    std::wstring m_filter;
    std::vector<media::Column> m_columns;
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
