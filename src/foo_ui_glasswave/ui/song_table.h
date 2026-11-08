#pragma once

#include "media/list_sources.h"
#include "media/play_stats.h"
#include "stream/provider.h"

#include <functional>
#include <vector>

namespace glasswave::ui {

// One song in a SongTableSource: a local file or a streaming provider's track.
struct SongTableRow {
    metadb_handle_ptr handle;
    stream::Provider* provider = nullptr;  // nullptr: a local file
    stream::Id trackId, albumId, artistId;  // provider ids (album column links)
    std::wstring title;
    std::wstring artist;
    std::wstring album;
    std::wstring length;
    std::wstring source;    // provider or "Local Music", when the Source column is shown
    std::wstring albumKey;  // album grouping key
    std::wstring artKey;
    std::wstring search;    // lower-cased title/artist/album for filtering
    int lengthMs = 0;
    int popularity = 0;      // provider ranking score
    uint32_t playCount = 0;  // our cross-source play history
    bool favorite = false;
};

// A song table over rows from any mix of sources: Details / Expanded tile, optional Source
// column, sort-driven sections, selection, playback and the context menu.
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
    size_t FindRow(const metadb_handle_ptr& track) const override;
    void Activate(size_t row) override;
    void ContextMenu(HWND owner, POINT screen) override;
    metadb_handle_list SelectedItems() const override;
    void SetFilter(std::wstring_view filterLower) override;

    // Every visible row, in display order (the page's bulk actions).
    metadb_handle_list VisibleItems() const;
    // Re-reads the play counts (a track started while this list is on screen).
    void RefreshPlayCounts();
    // Re-reads the favorite flag of every provider row.
    void RefreshFavorites();
    // Drops provider rows that are no longer favorites; returns how many went.
    size_t RemoveUnfavorited();

protected:
    // emptyText is shown while there are no rows; columnKey names the saved column layout.
    SongTableSource(std::vector<SongTableRow> rows, bool showSource, std::wstring emptyText, std::wstring columnKey);
    // Appends rows (the next page) without moving existing rows or clearing the selection.
    void Append(std::vector<SongTableRow> rows);
    // Removes rows and returns how many disappeared.
    size_t RemoveRows(const std::function<bool(const SongTableRow&)>& remove);
    void ShowPlayCount();  // the Count column is on by default (Most Played)

private:
    void RebuildVisible();
    void BuildGroups();
    void ApplySort();
    void SortOrder();
    std::pair<std::wstring, std::wstring> GroupInfo(size_t index) const;

    std::vector<SongTableRow> m_all;
    std::vector<size_t> m_visible;  // indices into m_all, display order, filtered
    std::vector<size_t> m_order;    // indices into m_all, display order
    std::vector<media::RowGroup> m_groups;
    std::vector<uint8_t> m_selected;  // per m_all
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
    bool m_showPlayCount = false;
    bool m_grouping = true;
};

// Tracks of one streaming provider (favorites, a playlist, an album, top tracks).
class RemoteSongSource : public SongTableSource {
public:
    RemoteSongSource(stream::Provider& provider, const std::vector<stream::Track>& tracks, std::wstring emptyText);
    void AppendTracks(const std::vector<stream::Track>& tracks);
    size_t RemoveTracks(const std::vector<stream::Id>& ids);

private:
    stream::Provider& m_provider;
};

// Favorites of every library: the local Favorites playlist and each provider's favorites,
// with a sortable Source column. Flat until sorted.
class AllFavoritesSource : public SongTableSource {
public:
    using RemoteFavorites = std::vector<std::pair<stream::Provider*, std::vector<stream::Track>>>;
    AllFavoritesSource(const std::vector<metadb_handle_ptr>& local, const RemoteFavorites& remote);
};

// Recently Played / Most Played, rows in the order given, with the Count column shown.
class PlayStatsSource : public SongTableSource {
public:
    PlayStatsSource(const std::vector<media::PlayRecord>& records, std::wstring emptyText);
};

}  // namespace glasswave::ui
