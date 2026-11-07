#pragma once
#include "media/library_model.h"

// Data sources behind the content list. The ListView only knows this interface.

namespace glasswave::media {

struct Column {
    std::wstring title;
    float width = 100;  // DIP; ignored for the flexible column
    bool flex = false;
    bool rightAlign = false;
};

// WMP 12 details view grouped by album: a header line, then the tracks next to an
// album column showing the cover and up to four lines of album information.
struct RowGroup {
    size_t first = 0, count = 0;  // rows
    metadb_handle_ptr art;        // track whose front cover represents the group
    std::wstring header;
    std::array<std::wstring, 4> lines;
};

// WMP 12 icon views: albums (cover above two lines) or artists (cover beside three lines).
enum class TileStyle { None, Album, Artist };
struct TileInfo {
    metadb_handle_ptr art;
    std::array<std::wstring, 3> lines;
};

class ListSource {
public:
    virtual ~ListSource() = default;

    virtual const std::vector<Column>& Columns() const = 0;
    virtual size_t Count() const = 0;
    virtual std::wstring Cell(size_t row, size_t column) const = 0;
    virtual std::wstring EmptyText() const = 0;
    virtual bool IsPlaying(size_t /*row*/) const { return false; }

    // Optional layouts. Groups (if any) cover all rows in order; column 0 is the album column.
    virtual const std::vector<RowGroup>* Groups() const { return nullptr; }
    virtual TileStyle Tiles() const { return TileStyle::None; }
    virtual TileInfo Tile(size_t /*row*/) const { return {}; }

    // Selection / focus
    virtual bool IsSelected(size_t row) const = 0;
    virtual void SelectOnly(size_t row) = 0;
    virtual void Toggle(size_t row) = 0;
    virtual void SelectRange(size_t a, size_t b, bool keepOthers) = 0;
    virtual void SelectAll() = 0;
    virtual size_t Focus() const = 0;
    virtual void SetFocus(size_t row) = 0;

    // Actions
    virtual void Activate(size_t row) = 0;  // double-click / Enter
    virtual void ContextMenu(HWND owner, POINT screen) = 0;
    virtual bool DeleteSelected() { return false; }
    virtual metadb_handle_list SelectedItems() const = 0;

    virtual void SetFilter(std::wstring_view filterLower) = 0;
};

// A foobar2000 playlist. Selection and focus live in the playlist itself so foobar2000's
// context menus and shortcuts act on the same items.
class PlaylistSource : public ListSource {
public:
    explicit PlaylistSource(size_t playlist);
    size_t Playlist() const { return m_playlist; }
    void Rebuild();  // after playlist content changes

    const std::vector<Column>& Columns() const override { return m_columns; }
    size_t Count() const override { return m_rows.size(); }
    std::wstring Cell(size_t row, size_t column) const override;
    std::wstring EmptyText() const override;
    bool IsPlaying(size_t row) const override;
    bool IsSelected(size_t row) const override;
    void SelectOnly(size_t row) override;
    void Toggle(size_t row) override;
    void SelectRange(size_t a, size_t b, bool keepOthers) override;
    void SelectAll() override;
    size_t Focus() const override;
    void SetFocus(size_t row) override;
    void Activate(size_t row) override;
    void ContextMenu(HWND owner, POINT screen) override;
    bool DeleteSelected() override;
    metadb_handle_list SelectedItems() const override;
    void SetFilter(std::wstring_view filterLower) override;

private:
    size_t m_playlist;
    std::wstring m_filter;
    std::vector<size_t> m_rows;  // playlist item index per visible row
    std::vector<Column> m_columns;
    std::vector<titleformat_object::ptr> m_scripts;
};

// Tracks from the library snapshot (all music, or the contents of a group).
class TrackSource : public ListSource {
public:
    TrackSource(const LibraryModel& model, std::vector<size_t> tracks, std::wstring emptyText);

    const std::vector<Column>& Columns() const override { return m_columns; }
    size_t Count() const override { return m_rows.size(); }
    std::wstring Cell(size_t row, size_t column) const override;
    std::wstring EmptyText() const override { return m_emptyText; }
    bool IsPlaying(size_t row) const override;
    const std::vector<RowGroup>* Groups() const override { return &m_groups; }
    bool IsSelected(size_t row) const override { return m_selected[m_rows[row]] != 0; }
    void SelectOnly(size_t row) override;
    void Toggle(size_t row) override;
    void SelectRange(size_t a, size_t b, bool keepOthers) override;
    void SelectAll() override;
    size_t Focus() const override { return m_focus; }
    void SetFocus(size_t row) override { m_focus = row; }
    void Activate(size_t row) override;
    void ContextMenu(HWND owner, POINT screen) override;
    metadb_handle_list SelectedItems() const override;
    void SetFilter(std::wstring_view filterLower) override;

private:
    const LibraryModel& m_model;
    std::vector<size_t> m_all;   // track indices in this view
    std::vector<size_t> m_rows;  // positions in m_all that pass the filter
    std::vector<uint8_t> m_selected;  // per position in m_all
    size_t m_focus = SIZE_MAX;
    std::wstring m_emptyText;
    std::vector<Column> m_columns;
    std::vector<RowGroup> m_groups;
};

// Artists, albums or genres. Activating a row drills into its tracks.
class GroupSource : public ListSource {
public:
    using DrillDown = std::function<void(const Group&)>;
    GroupSource(const LibraryModel& model, GroupKind kind, DrillDown onOpen);

    const std::vector<Column>& Columns() const override { return m_columns; }
    size_t Count() const override { return m_rows.size(); }
    std::wstring Cell(size_t row, size_t column) const override;
    std::wstring EmptyText() const override;
    TileStyle Tiles() const override { return m_kind == GroupKind::Artist ? TileStyle::Artist : TileStyle::Album; }
    TileInfo Tile(size_t row) const override;
    bool IsSelected(size_t row) const override { return m_selected[m_rows[row]] != 0; }
    void SelectOnly(size_t row) override;
    void Toggle(size_t row) override;
    void SelectRange(size_t a, size_t b, bool keepOthers) override;
    void SelectAll() override;
    size_t Focus() const override { return m_focus; }
    void SetFocus(size_t row) override { m_focus = row; }
    void Activate(size_t row) override;
    void ContextMenu(HWND owner, POINT screen) override;
    metadb_handle_list SelectedItems() const override;
    void SetFilter(std::wstring_view filterLower) override;

private:
    const Group& At(size_t row) const { return m_model.Groups(m_kind)[m_rows[row]]; }

    const LibraryModel& m_model;
    GroupKind m_kind;
    DrillDown m_onOpen;
    std::vector<size_t> m_rows;
    std::vector<uint8_t> m_selected;
    size_t m_focus = SIZE_MAX;
    std::vector<Column> m_columns;
};

// Replace the "Now Playing" playlist with `items` and start playback at `start`
// (WMP semantics: playing from a library view queues the whole view).
void PlayItems(const metadb_handle_list& items, size_t start);

}  // namespace glasswave::media
