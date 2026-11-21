#pragma once
#include "core/signal.h"
#include "media/library_model.h"

#include <functional>
#include <span>
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
// The cell text is a 0-100 score; the list draws it as a bar (NetEase's 热度 column).
inline constexpr const char* kPopularityFormat = "<popularity>";
// The cell text is "<permille> <state>" (state: n normal, r running: with the moving glow, p
// paused, e error); the list draws
// Windows 7's progress bar (aero.msstyles PROGRESS: trough 648, fills 656 / 658 / 657).
inline constexpr const char* kProgressFormat = "<progress>";
// A song's download (ListSource::Download): the list draws a round download mark, a progress
// ring while it downloads, and acts on a click (ListSource::ToggleDownload).
inline constexpr const char* kDownloadFormat = "<download>";
// The song's rating, drawn as WMP's stars; a click on a star rates the song (ListSource::Rate).
inline constexpr const char* kRatingFormat = "[%rating%]";
// Whether the song is a favorite (ListSource::IsFavorite), drawn as WMP's star; a click toggles it.
inline constexpr const char* kFavoriteFormat = "<favorite>";
// How often the song was played: our cross-source play history, not a titleformat field.
inline constexpr const char* kPlayCountFormat = "<playcount>";

// What a song's download cell shows.
struct DownloadCell {
    enum class Look {
        None,    // not downloadable
        Offer,   // downloadable: the mark, on the hovered row
        Waiting, // a ring filling with `progress`
        Running,
        Paused,
        Failed,
        Done // the downloaded mark
    };
    Look look = Look::None;
    float progress = 0; // 0-1 while unfinished
};

// Control cells change outside any list (a download's progress): the callback runs on the main
// thread until the connection goes.
[[nodiscard]] core::Signal<>::Connection WatchControlCells(std::function<void()> changed);

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

// WMP 12 icon views: albums (cover above two lines) or artists (cover beside three lines); Song:
// a small card, a small cover beside two lines (Apple Music's song shelves, Explorer's tiles).
// Card: a featured card (the home page's top rows), drawn by the list's card layer
// (ui/card_layer.h) from ListSource::Card instead of as a tile.
enum class TileStyle { None, Album, Artist, Song, Card };

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
    // Icon views in groups (an artist's overview): the group's tiles show this many rows at
    // most (0: all), and `more` ("Show all (12)") stands at the header's right end as a link
    // to ListSource::ShowGroupMore.
    size_t tileLines = 0;
    std::wstring more;
    bool moreBeyond = false; // the source has more than these rows (another page to load)
    // Icon views in groups: the group's own tiles (None: the source's), and a shelf that pages
    // sideways (Apple Music's): tileLines rows, filled column by column, arrows at its ends.
    TileStyle tiles = TileStyle::None;
    bool carousel = false;
};

// WMP 12 view options: Icon, Expanded tile, Details.
enum class ViewStyle { Icons, Tiles, Details };

struct TileInfo {
    metadb_handle_ptr art;
    std::array<std::wstring, 3> lines;
    int icon = 0;  // wmploc PNG drawn instead of a cover (playlists page), 0 = none
    // Results mixed from several sources: whose this tile is, marked on the cover's corner
    // (a provider's key, or "local"); empty: no mark.
    std::wstring source;
    std::vector<std::wstring> badges; // drawn before lines[1] (a song's artist): stream::Track::badges
};

// A featured card (TileStyle::Card): words, picture, motion cover and colours as its source
// describes them (stream::HomeCard).
struct CardInfo {
    std::wstring key;      // identity of the card's visuals, stable while the list lives
    std::wstring provider; // whose pictures (the image cache's identity)
    std::wstring label, title, subtitle, badge;
    std::wstring imageUrl;
    bool imageTall = false; // the picture fills the card; else a square cover above the words
    std::wstring videoUrl;  // an HLS master playlist; empty: none
    bool colors = false;    // background / inks known; else taken from the picture
    uint32_t background = 0, titleInk = 0, textInk = 0;
    bool playable = false;  // its play orb shows (ListSource::CanPlayTile)
};

// The selection of a source whose rows show some of its items (filtered, sorted): one flag per
// item, `rows` mapping each row to its item, so a selected item stays selected while a filter
// hides it. Rows past the end are ignored.
class RowSelection {
  public:
    void Reset(size_t items) { m_flags.assign(items, 0); } // nothing selected
    void Resize(size_t items) { m_flags.resize(items, 0); } // new items unselected
    size_t Size() const { return m_flags.size(); }
    bool Item(size_t item) const { return item < m_flags.size() && m_flags[item]; }
    void SetItem(size_t item, bool selected) {
        if (item < m_flags.size()) m_flags[item] = selected ? 1 : 0;
    }

    bool IsSelected(std::span<const size_t> rows, size_t row) const { return row < rows.size() && Item(rows[row]); }
    void SelectOnly(std::span<const size_t> rows, size_t row) {
        Clear();
        if (row < rows.size()) SetItem(rows[row], true);
    }
    void Toggle(std::span<const size_t> rows, size_t row) {
        if (row < rows.size()) SetItem(rows[row], !Item(rows[row]));
    }
    void SelectRange(std::span<const size_t> rows, size_t a, size_t b, bool keepOthers) {
        if (!keepOthers) Clear();
        if (a > b) std::swap(a, b);
        for (size_t r = a; r <= b && r < rows.size(); ++r) SetItem(rows[r], true);
    }
    void SelectAll(std::span<const size_t> rows) {
        for (size_t item : rows) SetItem(item, true);
    }
    void SetSelection(std::span<const size_t> rows, const std::vector<size_t>& selected) {
        Clear();
        for (size_t r : selected)
            if (r < rows.size()) SetItem(rows[r], true);
    }

  private:
    void Clear() { std::fill(m_flags.begin(), m_flags.end(), uint8_t{0}); }
    std::vector<uint8_t> m_flags;
};

class ListSource {
public:
    virtual ~ListSource() = default;
    // Never reused, unlike the object's address: what outlives a source and later asks
    // whether the list on screen is still the one it began with compares this.
    uint64_t Id() const { return m_id; }

    virtual const std::vector<Column>& Columns() const = 0;
    // Extra discriminator for the saved column layout (provider-specific column sets).
    virtual std::wstring ColumnSetKey() const { return {}; }
    virtual size_t Count() const = 0;
    virtual std::wstring Cell(size_t row, size_t column) const = 0;
    // Grey text the list draws after the cell's own (a song's note after its title,
    // stream::Track::note); not part of the cell's value.
    virtual std::wstring CellNote(size_t /*row*/, size_t /*column*/) const { return {}; }
    // Small tags the list draws before the cell's text (a song's stream::Track::badges before its
    // artist, as NetEase puts 原唱), as untranslated UI keys. Not a column of their own.
    virtual std::vector<std::wstring> CellBadges(size_t /*row*/, size_t /*column*/) const { return {}; }
    virtual std::wstring EmptyText() const = 0;
    virtual bool IsPlaying(size_t /*row*/) const { return false; }

    // Optional layouts. Groups (if any) cover all rows in order; column 0 is the album column.
    virtual const std::vector<RowGroup>* Groups() const { return nullptr; }
    virtual void ShowGroupMore(size_t /*group*/) {} // the header's `more` link was clicked
    // A double click on a group's album column (Expanded tile): plays only that group.
    virtual void ActivateGroup(size_t group) {
        if (const auto* groups = Groups(); groups && group < groups->size() && (*groups)[group].count)
            Activate((*groups)[group].first);
    }
    virtual TileStyle Tiles() const { return TileStyle::None; }
    virtual TileInfo Tile(size_t /*row*/) const { return {}; }
    // A card's description, for rows of a group with TileStyle::Card; null: none.
    virtual const CardInfo* Card(size_t /*row*/) const { return nullptr; }
    // A tile's play button (shown over its cover under the mouse): plays what the tile is.
    virtual bool CanPlayTile(size_t /*row*/) const { return false; }
    virtual void PlayTile(size_t /*row*/) {}
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
    // Control cells (kRatingFormat, kFavoriteFormat, kDownloadFormat): what they show and what a
    // click on them does. The defaults serve any source whose rows are songs, through Item.
    virtual bool IsFavorite(size_t row) const;
    virtual DownloadCell Download(size_t row) const;
    virtual void Rate(size_t row, int stars); // 1-5
    virtual void ToggleFavorite(size_t row);
    // Not downloaded: download it; downloaded: remove the download (asked first).
    virtual void ToggleDownload(size_t row);
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
  static uint64_t NextId() {
      static uint64_t last = 0;
      return ++last;
  }
  const uint64_t m_id = NextId();
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
    std::vector<std::wstring> CellBadges(size_t row, size_t column) const override;
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
    bool IsSelected(size_t row) const override { return m_selection.IsSelected(m_rows, row); }
    void SelectOnly(size_t row) override { m_selection.SelectOnly(m_rows, row); }
    void Toggle(size_t row) override { m_selection.Toggle(m_rows, row); }
    void SelectRange(size_t a, size_t b, bool keepOthers) override { m_selection.SelectRange(m_rows, a, b, keepOthers); }
    void SelectAll() override { m_selection.SelectAll(m_rows); }
    void SetSelection(const std::vector<size_t>& rows) override { m_selection.SetSelection(m_rows, rows); }
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
    RowSelection m_selection;  // per position in m_all
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
    bool IsSelected(size_t row) const override { return m_selection.IsSelected(m_rows, row); }
    void SelectOnly(size_t row) override { m_selection.SelectOnly(m_rows, row); }
    void Toggle(size_t row) override { m_selection.Toggle(m_rows, row); }
    void SelectRange(size_t a, size_t b, bool keepOthers) override { m_selection.SelectRange(m_rows, a, b, keepOthers); }
    void SelectAll() override { m_selection.SelectAll(m_rows); }
    void SetSelection(const std::vector<size_t>& rows) override { m_selection.SetSelection(m_rows, rows); }
    size_t Focus() const override { return m_focus; }
    void SetFocus(size_t row) override { m_focus = row; }
    std::wstring RowKey(size_t row) const override;
    void Activate(size_t row) override;
    void ContextMenu(HWND owner, POINT screen) override;
    metadb_handle_list SelectedItems() const override;
    void SetFilter(std::wstring_view filterLower) override;
    // An album's cover plays the album.
    bool CanPlayTile(size_t row) const override {
        return m_kind == GroupKind::Album && row < m_rows.size() && Tiles() != TileStyle::None;
    }
    void PlayTile(size_t row) override;

private:
    const Group& At(size_t row) const { return m_model.Groups(m_kind)[m_rows[row]]; }

    const LibraryModel& m_model;
    std::wstring Name(const Group& g) const;
    GroupKind m_kind;
    DrillDown m_onOpen;
    std::vector<size_t> m_rows;
    RowSelection m_selection;
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
    bool IsSelected(size_t row) const override { return m_selection.IsSelected(m_rows, row); }
    void SelectOnly(size_t row) override { m_selection.SelectOnly(m_rows, row); }
    void Toggle(size_t row) override { m_selection.Toggle(m_rows, row); }
    void SelectRange(size_t a, size_t b, bool keepOthers) override { m_selection.SelectRange(m_rows, a, b, keepOthers); }
    void SelectAll() override { m_selection.SelectAll(m_rows); }
    void SetSelection(const std::vector<size_t>& rows) override { m_selection.SetSelection(m_rows, rows); }
    size_t Focus() const override { return m_focus; }
    void SetFocus(size_t row) override { m_focus = row; }
    std::wstring RowKey(size_t row) const override;
    void Activate(size_t row) override;
    void ContextMenu(HWND owner, POINT screen) override;
    metadb_handle_list SelectedItems() const override;
    void SetFilter(std::wstring_view filterLower) override;
    bool CanPlayTile(size_t row) const override { return m_style == ViewStyle::Icons && row < m_rows.size(); }
    void PlayTile(size_t row) override;

private:
    bool m_smart;
    Open m_open;
    Menu m_menu;
    std::vector<Column> m_columns;
    std::vector<size_t> m_rows;  // playlist indices
    RowSelection m_selection;
    size_t m_focus = SIZE_MAX;
    ViewStyle m_style = ViewStyle::Icons;
};

// Replace the "Now Playing" playlist with `items` and start playback at `start`
// (WMP semantics: playing from a library view queues the whole view).
void PlayItems(const metadb_handle_list& items, size_t start);
// Plays a list of `count` entries from the chosen one: `track(i)` is entry i's song, and
// entries without one are left out (the chosen one too: the list then plays from its start).
void PlayListFrom(size_t count, size_t chosen, const std::function<metadb_handle_ptr(size_t)>& track);

}  // namespace glasswave::media
