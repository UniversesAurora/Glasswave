#pragma once

#include "media/list_sources.h"
#include "media/play_stats.h"
#include "stream/list.h"
#include "stream/provider.h"

#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace glasswave::ui {

// One song in a SongTableSource: a local file or a streaming provider's track.
struct SongTableRow {
    metadb_handle_ptr handle;
    stream::Provider* provider = nullptr;  // nullptr: a local file
    stream::Id trackId, albumId, artistId;  // provider ids (album column links)
    std::wstring title;
    std::wstring note; // grey after the title (stream::Track::note)
    std::vector<std::wstring> badges; // tags after the title (stream::Track::badges)
    std::wstring artist;
    std::wstring album;
    std::wstring length;
    std::wstring source;    // provider or "Local Music", when the Source column is shown
    std::wstring lyric;     // a lyrics search: the line that matched and the two after it
    std::wstring lyricText; // ... and the lyrics whole
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
    using ActivateHandler = std::function<void(size_t row, const std::wstring& identity)>;

    const std::vector<media::Column>& Columns() const override { return m_columns; }
    std::wstring ColumnSetKey() const override;
    size_t Count() const override { return m_visible.size(); }
    std::wstring Cell(size_t row, size_t column) const override;
    std::wstring CellNote(size_t row, size_t column) const override;
    std::vector<std::wstring> CellBadges(size_t row, size_t column) const override;
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
    // Tiles need exact album identity. Details only needs it when grouping by album; the
    // ordinary unsorted Details layout can show and play every available row immediately.
    bool NeedsAlbumIdentity() const;
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
    std::wstring RowKey(size_t row) const override;
    void Activate(size_t row) override;
    void ContextMenu(HWND owner, POINT screen) override;
    metadb_handle_list SelectedItems() const override;
    void SetFilter(std::wstring_view filterLower) override;
    bool HasFilter() const override { return !m_filter.empty(); }

    // Every visible row, in display order (the page's bulk actions).
    metadb_handle_list VisibleItems() const;
    // Plays the whole list in display order, ignoring the search filter, from visible `row`.
    void PlayFrom(size_t row) const;
    // Only the songs of the row's group (an album in the expanded tile view), from this one.
    void PlayGroupFrom(size_t row) const;
    void ActivateGroup(size_t group) override {
        if (group < m_groups.size() && m_groups[group].count) PlayGroupFrom(m_groups[group].first);
    }
    // Re-reads the play counts (a track started while this list is on screen).
    void RefreshPlayCounts();
    // Re-reads the favorite flag of every provider row.
    void RefreshFavorites();
    // Drops provider rows that are no longer favorites; returns how many went.
    size_t RemoveUnfavorited();

    // Replace the complete currently published prefix. An explicit sort is applied to each
    // newly published prefix; an unsorted list keeps the provider's server order.
    void ReplaceRows(std::vector<SongTableRow> rows);
    // Stable per-duplicate-occurrence identity used to resolve an activated row after a
    // generic session has drained the remaining pages.
    std::wstring RowIdentity(size_t row) const;
    size_t FindRowByIdentity(const std::wstring& identity) const;
    void SetActivateHandler(ActivateHandler handler) { m_activate = std::move(handler); }
    void SetEmptyText(std::wstring text) { m_emptyText = std::move(text); }

protected:
    const std::vector<SongTableRow>& AllRows() const { return m_all; }
    // emptyText is shown while there are no rows; columnKey names the saved column layout.
    SongTableSource(std::vector<SongTableRow> rows, bool showSource, std::wstring emptyText, std::wstring columnKey);
    // Appends rows (the next page) without moving existing rows or clearing the selection.
    void Append(std::vector<SongTableRow> rows);
    // Replaces one stable, contiguous occurrence range without rebuilding the formatted
    // prefix. Returns false when the range no longer matches the source identities.
    bool UpdateRows(size_t offset, std::vector<SongTableRow> rows);
    // Removes rows and returns how many disappeared.
    size_t RemoveRows(const std::function<bool(const SongTableRow&)>& remove);
    void ShowPlayCount();  // the Count column is on by default (Most Played)
    void ShowLyric();      // the Lyrics column (a lyrics search)
    void OfferDownloads(); // the Download column (a source that can download its songs)
    // The provider scores its tracks 0-100: the column is offered, on by default when `shown`.
    void OfferPopularity(bool shown);

private:
    struct PresentationState {
        std::vector<std::wstring> selected;
        std::wstring focus;
    };

    void RebuildVisible();
    void BuildGroups();
    void ApplySort();
    void SortOrder();
    void UpdateAlbumRanks(size_t first);
    std::wstring SortKey(size_t index) const;
    bool SortLess(size_t a, size_t b) const;
    std::pair<std::wstring, std::wstring> GroupInfo(size_t index) const;
    std::vector<std::wstring> RowIdentities() const;
    PresentationState CapturePresentation() const;
    void RestorePresentation(const PresentationState& state);
    std::wstring FocusIdentity() const;
    size_t FindVisibleByIdentity(const std::wstring& identity) const;

    std::vector<SongTableRow> m_all;
    std::vector<size_t> m_visible;  // indices into m_all, display order, filtered
    std::vector<size_t> m_order;    // indices into m_all, display order
    std::vector<std::wstring> m_sortKeys;  // per m_all while an explicit field sort is active
    std::unordered_map<std::wstring, size_t> m_albumRank;
    std::unordered_set<std::wstring> m_unresolvedAlbumNames;
    std::unordered_map<std::wstring, std::unordered_set<std::wstring>> m_remoteAlbumIdsByName;
    std::unordered_set<std::wstring> m_resolvedMissingAlbumNames;
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
    bool m_showLyric = false;
    bool m_offerDownloads = false;
    bool m_offerPopularity = false, m_popularityShown = false;
    bool m_grouping = true;
    ActivateHandler m_activate;
};

// Tracks of one streaming provider (favorites, a playlist, an album, top tracks).
class RemoteSongSource : public SongTableSource {
public:
    // The columns' layout is kept per provider and kind of list (an album, a playlist, an
    // artist's albums...): the same kind of page looks the same whichever list it shows.
    RemoteSongSource(stream::Provider& provider, stream::ListKind kind, const std::vector<stream::Track>& tracks,
                     std::wstring emptyText, size_t count = SIZE_MAX);
    // Converts and merges only [begin, end). The ids and order must match the published prefix.
    void AppendTracks(const std::vector<stream::Track>& tracks, size_t begin = 0, size_t end = SIZE_MAX);
    // Replaces the first `count` tracks, or all tracks when `count` is SIZE_MAX.
    void ReplaceTracks(const std::vector<stream::Track>& tracks, size_t count = SIZE_MAX);
    // Patches only one metadata range whose occurrence ids and order are unchanged.
    bool UpdateTrackRange(const std::vector<stream::Track>& tracks, size_t offset, size_t count);
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
    void ReplaceFavorites(const std::vector<metadb_handle_ptr>& local, const RemoteFavorites& remote);
};

// Songs a search found, from any mix of sources in the order given (a Source column when more
// than one source answered), with the lyric line that matched for a lyrics search.
class SearchSongSource : public SongTableSource {
public:
    struct Hit {
        stream::Provider* provider = nullptr;  // nullptr: a local file
        stream::Track track;                   // provider hits
        metadb_handle_ptr local;               // local hits
    };
    SearchSongSource(const std::vector<Hit>& hits, bool showSource, bool lyrics, std::wstring emptyText);
    // The hits found so far, a longer list with the same beginning.
    void ReplaceHits(const std::vector<Hit>& hits);
    // The whole lyrics of a lyrics search hit (empty for anything else).
    std::wstring LyricsOf(const metadb_handle_ptr& item) const;
};

// An audio CD's tracks, in disc order. Playing one makes the queue the disc. `emptyText`
// says why there are none (no disc, still reading).
class DiscSource : public SongTableSource {
public:
    DiscSource(const metadb_handle_list& tracks, std::wstring emptyText);
};

// Recently Played / Most Played, rows in the order given, with the Count column shown.
class PlayStatsSource : public SongTableSource {
public:
    PlayStatsSource(const std::vector<media::PlayRecord>& records, std::wstring emptyText);
};

}  // namespace glasswave::ui
