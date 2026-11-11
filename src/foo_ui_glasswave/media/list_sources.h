#pragma once
#include "media/library_model.h"

#include <functional>
#include <string_view>

// Data sources behind the content list. The ListView only knows this interface.

namespace glasswave::media {

struct Column {
    std::wstring title;
    float width = 100;  // DIP; ignored for the flexible column
    bool flex = false;
    bool rightAlign = false;
    bool hidden = false;  // offered under "Choose columns" but off by default
    const char* format = nullptr;  // titleformat for columns read straight from the track
};

// Cover thumbnail column (Apple Music style); the list draws the row's front cover in it.
inline constexpr const char* kArtFormat = "<art>";
Column ArtColumn(bool hidden);

// Where a line of the album column leads, like WMP's hyperlinks.
struct Link {
    enum class Kind {
        None,
        Group,        // a LibraryModel group: `group` + its `key`
        LocalAlbum,   // a local album found by name (`key`), preferring the group's artist line
        LocalArtist,  // a local artist found by name (`key`)
        RemoteAlbum,  // a streaming provider's album: `provider` + id in `key`
        RemoteArtist  // a streaming provider's artist page
    };
    Kind kind = Kind::None;
    GroupKind group = GroupKind::Artist;
    std::wstring key;
    std::wstring provider;

    static Link ToGroup(GroupKind kind, std::wstring key) {
        return key.empty() ? Link{} : Link{Kind::Group, kind, std::move(key), {}};
    }
    static Link Remote(Kind kind, std::wstring_view provider, std::wstring id) {
        return id.empty() ? Link{} : Link{kind, GroupKind::Artist, std::move(id), std::wstring(provider)};
    }
    explicit operator bool() const { return kind != Kind::None; }
};

// WMP 12 details view grouped by album: a header line, then the tracks next to an
// album column showing the cover and up to four lines of album information.
struct RowGroup {
    std::wstring key;             // album identity, or the sort column's group value
    size_t first = 0, count = 0;  // rows
    metadb_handle_ptr art;        // track whose front cover represents the group
    std::wstring header;
    std::array<std::wstring, 4> lines;
    // What each album column line opens (album, artist, genre, year); none = plain text.
    std::array<Link, 4> links;
};

// WMP 12 view options: Icon, Expanded tile, Details.
enum class ViewStyle { Icons, Tiles, Details };

// WMP 12 icon views: albums (cover above two lines) or artists (cover beside three lines).
enum class TileStyle { None, Album, Artist };
struct TileInfo {
    metadb_handle_ptr art;
    std::array<std::wstring, 3> lines;
    int icon = 0;  // wmploc PNG drawn instead of a cover (playlists page), 0 = none
};

class ListSource {
public:
    virtual ~ListSource() = default;

    virtual const std::vector<Column>& Columns() const = 0;
    // Extra discriminator for the saved column layout (provider-specific column sets).
    virtual std::wstring ColumnSetKey() const { return {}; }
    virtual size_t Count() const = 0;
    virtual std::wstring Cell(size_t row, size_t column) const = 0;
    virtual std::wstring EmptyText() const = 0;
    virtual bool IsPlaying(size_t /*row*/) const { return false; }

    // Optional layouts. Groups (if any) cover all rows in order; column 0 is the album column.
    virtual const std::vector<RowGroup>* Groups() const { return nullptr; }
    virtual TileStyle Tiles() const { return TileStyle::None; }
    virtual TileInfo Tile(size_t /*row*/) const { return {}; }
    // Draw the album column (cover + lines) beside grouped rows, or only header lines.
    virtual bool GroupArt() const { return Groups() != nullptr; }

    // Sorting by a column (header click / "Sort by"). SortColumn() is SIZE_MAX when unsorted.
    virtual bool CanSort(size_t /*column*/) const { return false; }
    virtual void Sort(size_t /*column*/, bool /*descending*/) {}
    virtual size_t SortColumn() const { return SIZE_MAX; }
    virtual bool SortDescending() const { return false; }
    // View options offered by the view button.
    virtual bool SupportsViewStyle(ViewStyle) const { return false; }
    virtual ViewStyle GetViewStyle() const { return ViewStyle::Details; }
    virtual void SetViewStyle(ViewStyle) {}
    // A non-empty local filter matches items from every page. A sorted or expanded layout
    // also needs the whole logical list before it can present a truthful final order.
    virtual bool HasFilter() const { return false; }
    virtual bool NeedsCompleteList() const {
        return HasFilter() || SortColumn() != SIZE_MAX || GetViewStyle() == ViewStyle::Tiles;
    }

    // Details sections: rows are split by the sort column while grouping is on. Expanded
    // tile always groups by album and does not offer the option.
    virtual bool CanGroup() const { return false; }
    virtual bool Grouping() const { return false; }
    virtual void SetGrouping(bool /*enabled*/) {}

    // Selection / focus
    virtual bool IsSelected(size_t row) const = 0;
    virtual void SelectOnly(size_t row) = 0;
    virtual void Toggle(size_t row) = 0;
    virtual void SelectRange(size_t a, size_t b, bool keepOthers) = 0;
    virtual void SelectAll() = 0;
    // The track a row shows (rating / favourite cells); empty for group rows.
    virtual metadb_handle_ptr Item(size_t /*row*/) const { return {}; }
    // Album art column: rows of the same album share one cache key, so the cover is fetched
    // and decoded once. ArtTrack is the track whose front cover represents the album.
    virtual std::wstring ArtKey(size_t /*row*/) const { return {}; }
    virtual metadb_handle_ptr ArtTrack(size_t row) const { return Item(row); }
    // Stable identity before duplicate occurrences are counted by the host. A local item
    // uses its core path and subsong; an object without an item falls back to its art key.
    virtual std::wstring RowKey(size_t row) const;
    // Row showing this track, or SIZE_MAX ("jump to the playing song").
    virtual size_t FindRow(const metadb_handle_ptr& /*track*/) const { return SIZE_MAX; }
    // Exactly these rows selected, all others not (rubber-band selection).
    virtual void SetSelection(const std::vector<size_t>& rows) = 0;
    virtual size_t Focus() const = 0;
    virtual void SetFocus(size_t row) = 0;

    // Actions
    virtual void Activate(size_t row) = 0;  // double-click / Enter
    virtual void ContextMenu(HWND owner, POINT screen) = 0;
    virtual bool DeleteSelected() { return false; }
    virtual metadb_handle_list SelectedItems() const = 0;

    virtual void SetFilter(std::wstring_view filterLower) = 0;

    // A transient band above the list (initial load, background revalidation or a partial
    // page error). Empty when the source is settled.
    virtual std::wstring LoadingText() const { return m_loadingText; }
    void SetLoadingText(std::wstring text) { m_loadingText = std::move(text); }

private:
    std::wstring m_loadingText;
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
    // Expanded tile: runs of consecutive songs from one album share an album column.
    const std::vector<RowGroup>* Groups() const override {
        return m_style == ViewStyle::Tiles ? &m_groups : nullptr;
    }
    bool SupportsViewStyle(ViewStyle s) const override { return s != ViewStyle::Icons; }
    ViewStyle GetViewStyle() const override { return m_style; }
    void SetViewStyle(ViewStyle style) override;
    bool IsSelected(size_t row) const override;
    void SelectOnly(size_t row) override;
    void Toggle(size_t row) override;
    void SelectRange(size_t a, size_t b, bool keepOthers) override;
    void SelectAll() override;
    void SetSelection(const std::vector<size_t>& rows) override;
    size_t FindRow(const metadb_handle_ptr& track) const override;
    metadb_handle_ptr Item(size_t row) const override;
    std::wstring ArtKey(size_t row) const override;
    metadb_handle_ptr ArtTrack(size_t row) const override;
    size_t Focus() const override;
    void SetFocus(size_t row) override;
    void Activate(size_t row) override;
    void ContextMenu(HWND owner, POINT screen) override;
    bool DeleteSelected() override;
    metadb_handle_list SelectedItems() const override;
    void SetFilter(std::wstring_view filterLower) override;
    // Sorting reorders the foobar2000 playlist itself (undoable), as WMP 12 does.
    bool CanSort(size_t column) const override;
    void Sort(size_t column, bool descending) override;
    size_t SortColumn() const override { return m_sortColumn; }
    bool SortDescending() const override { return m_sortDescending; }

private:
    void BuildGroups();

    size_t m_playlist;
    ViewStyle m_style = ViewStyle::Details;
    std::vector<int> m_fields;  // what each column shows
    std::vector<RowGroup> m_groups;
    size_t m_sortColumn = SIZE_MAX;
    bool m_sortDescending = false;
    metadb_handle_list m_unsorted;  // playlist order before the first sort
    std::wstring m_filter;
    std::vector<size_t> m_rows;  // playlist item index per visible row
    std::vector<Column> m_columns;
    std::vector<titleformat_object::ptr> m_scripts;
    mutable std::vector<std::wstring> m_artKeys;
    mutable std::vector<metadb_handle_ptr> m_artTracks;
    mutable std::map<std::wstring, metadb_handle_ptr> m_artReps;
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
    bool GroupArt() const override { return m_style == ViewStyle::Tiles; }
    bool SupportsViewStyle(ViewStyle s) const override { return s != ViewStyle::Icons; }
    ViewStyle GetViewStyle() const override { return m_style; }
    void SetViewStyle(ViewStyle style) override;
    bool CanGroup() const override { return m_style == ViewStyle::Details; }
    bool Grouping() const override { return m_grouping; }
    void SetGrouping(bool enabled) override;
    bool CanSort(size_t column) const override;
    void Sort(size_t column, bool descending) override;
    size_t SortColumn() const override { return m_sortColumn; }
    bool SortDescending() const override { return m_sortDescending; }
    bool IsSelected(size_t row) const override { return m_selected[m_rows[row]] != 0; }
    void SelectOnly(size_t row) override;
    void Toggle(size_t row) override;
    void SelectRange(size_t a, size_t b, bool keepOthers) override;
    void SelectAll() override;
    void SetSelection(const std::vector<size_t>& rows) override;
    size_t FindRow(const metadb_handle_ptr& track) const override;
    metadb_handle_ptr Item(size_t row) const override;
    std::wstring ArtKey(size_t row) const override;
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
    std::vector<int> m_fields;  // what each column shows
    std::vector<titleformat_object::ptr> m_scripts;  // per column with a format
    std::vector<RowGroup> m_groups;
    ViewStyle m_style = ViewStyle::Tiles;
    size_t m_sortColumn = SIZE_MAX;
    bool m_sortDescending = false;
    bool m_grouping = true;
    std::vector<size_t> m_libraryOrder;  // m_all as built, for sorting back
    std::wstring m_filter;

    bool GroupBySort() const;  // Details view with a non-album sort column
    std::pair<std::wstring, std::wstring> GroupInfo(const Track& track) const;  // key, label
    void ApplySort();
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
    std::wstring ArtKey(size_t row) const override;
    TileStyle Tiles() const override {
        return m_style == ViewStyle::Icons ? TileStyle::Album
               : m_style == ViewStyle::Tiles ? TileStyle::Artist
                                             : TileStyle::None;
    }
    bool SupportsViewStyle(ViewStyle) const override { return true; }
    ViewStyle GetViewStyle() const override { return m_style; }
    void SetViewStyle(ViewStyle style) override { m_style = style; }
    bool CanSort(size_t column) const override { return column < m_columns.size(); }
    void Sort(size_t column, bool descending) override;
    size_t SortColumn() const override { return m_sortColumn; }
    bool SortDescending() const override { return m_sortDescending; }
    TileInfo Tile(size_t row) const override;
    bool IsSelected(size_t row) const override { return m_selected[m_rows[row]] != 0; }
    void SelectOnly(size_t row) override;
    void Toggle(size_t row) override;
    void SelectRange(size_t a, size_t b, bool keepOthers) override;
    void SelectAll() override;
    void SetSelection(const std::vector<size_t>& rows) override;
    size_t Focus() const override { return m_focus; }
    void SetFocus(size_t row) override { m_focus = row; }
    std::wstring RowKey(size_t row) const override;
    void Activate(size_t row) override;
    void ContextMenu(HWND owner, POINT screen) override;
    metadb_handle_list SelectedItems() const override;
    void SetFilter(std::wstring_view filterLower) override;

private:
    const Group& At(size_t row) const { return m_model.Groups(m_kind)[m_rows[row]]; }

    const LibraryModel& m_model;
    std::wstring Name(const Group& g) const;
    GroupKind m_kind;
    DrillDown m_onOpen;
    std::vector<size_t> m_rows;
    std::vector<uint8_t> m_selected;
    size_t m_focus = SIZE_MAX;
    std::vector<Column> m_columns;
    std::wstring m_filter;
    ViewStyle m_style = ViewStyle::Icons;
    size_t m_sortColumn = SIZE_MAX;
    bool m_sortDescending = false;
};

// WMP 12's Playlists page: every playlist but the queue, as icons or details (title, count).
// Opening and the context menu are the main window's.
class PlaylistsSource : public ListSource {
public:
    using Open = std::function<void(size_t playlist)>;
    using Menu = std::function<void(size_t playlist, POINT screen)>;
    // smart = false: the user's playlists; true: Favorites and the auto playlists.
    PlaylistsSource(bool smart, Open open, Menu menu);

    const std::vector<Column>& Columns() const override { return m_columns; }
    size_t Count() const override { return m_rows.size(); }
    std::wstring Cell(size_t row, size_t column) const override;
    std::wstring EmptyText() const override { return m_smart ? L"No smart playlists." : L"No playlists."; }
    TileStyle Tiles() const override { return m_style == ViewStyle::Icons ? TileStyle::Album : TileStyle::None; }
    TileInfo Tile(size_t row) const override;
    bool SupportsViewStyle(ViewStyle s) const override { return s != ViewStyle::Tiles; }
    ViewStyle GetViewStyle() const override { return m_style; }
    void SetViewStyle(ViewStyle style) override { m_style = style; }
    bool IsSelected(size_t row) const override { return row < m_selected.size() && m_selected[row]; }
    void SelectOnly(size_t row) override;
    void Toggle(size_t row) override;
    void SelectRange(size_t a, size_t b, bool keepOthers) override;
    void SelectAll() override;
    void SetSelection(const std::vector<size_t>& rows) override;
    size_t Focus() const override { return m_focus; }
    void SetFocus(size_t row) override { m_focus = row; }
    std::wstring RowKey(size_t row) const override;
    void Activate(size_t row) override;
    void ContextMenu(HWND owner, POINT screen) override;
    metadb_handle_list SelectedItems() const override;
    void SetFilter(std::wstring_view filterLower) override;

private:
    bool m_smart;
    Open m_open;
    Menu m_menu;
    std::vector<Column> m_columns;
    std::vector<size_t> m_rows;  // playlist indices
    std::vector<uint8_t> m_selected;
    size_t m_focus = SIZE_MAX;
    ViewStyle m_style = ViewStyle::Icons;
};

// Replace the "Now Playing" playlist with `items` and start playback at `start`
// (WMP semantics: playing from a library view queues the whole view).
void PlayItems(const metadb_handle_list& items, size_t start);

}  // namespace glasswave::media
