#pragma once

#include "media/list_sources.h"
#include "stream/provider.h"

#include <functional>
#include <unordered_set>
#include <vector>

// Cover-tile pages of a streaming provider: albums (an artist's, or the saved ones),
// followed artists and playlists. Opening an item and its context menu belong to the page.
namespace glasswave::ui {

// Playing every song of a service's list from the first: a cover's or a card's play button, a
// list page's Play (defined with the pages, remote_pages.cpp). Any provider, any list kind of
// songs: the list is read through the same shared ListSession its page shows (the cache when it
// was confirmed lately, otherwise the service's answer, which the page then shows as well), and
// what plays stops at once, the transport's Play waiting for the list (media::BeginPendingPlay).
void PlayRemoteList(stream::Provider& provider, stream::ListQuery query, bool recordRecent);
void PlayRemoteAlbum(stream::Provider& provider, const stream::Id& album);
void PlayRemotePlaylist(stream::Provider& provider, const stream::Id& playlist); // also a recent list

// Selection, focus and filtering shared by the tile sources. Rows are the indices of the
// items that pass the filter; the selection is per row.
class GridSource : public media::ListSource {
public:
    const std::vector<media::Column>& Columns() const override { return m_columns; }
    size_t Count() const override { return m_rows.size(); }
    std::wstring EmptyText() const override { return m_emptyText; }
    bool IsSelected(size_t row) const override {
        return row < m_rows.size() && m_selected.count(SelectionKey(m_rows[row])) != 0;
    }
    void SelectOnly(size_t row) override;
    void Toggle(size_t row) override;
    void SelectRange(size_t a, size_t b, bool keepOthers) override;
    void SelectAll() override;
    void SetSelection(const std::vector<size_t>& rows) override;
    size_t Focus() const override { return m_focus; }
    void SetFocus(size_t row) override { m_focus = row; }
    std::wstring RowKey(size_t row) const override;
    metadb_handle_list SelectedItems() const override { return {}; }
    void SetFilter(std::wstring_view filterLower) override;
    bool HasFilter() const override { return !m_filter.empty(); }
    bool CanSort(size_t column) const override;
    void Sort(size_t column, bool descending) override;
    size_t SortColumn() const override { return m_sortColumn; }
    bool SortDescending() const override { return m_sortDescending; }

protected:
    GridSource(std::vector<media::Column> columns, std::wstring emptyText);
    // Item count and the text the search box matches for item i.
    virtual size_t ItemCount() const = 0;
    virtual std::wstring ItemCell(size_t item, size_t column) const = 0;
    virtual std::wstring FilterText(size_t item) const = 0;
    // Stable identity of one data item. Duplicate identities remain separate occurrences.
    virtual std::wstring Identity(size_t item) const = 0;
    // Items always listed before the filtered ones (an artist's track-list tile).
    virtual size_t Leading() const { return 0; }
    void Refilter();
    void ForgetIdentity(const std::wstring& identity);
    void SetEmptyText(std::wstring text) { m_emptyText = std::move(text); }

    std::vector<size_t> m_rows;  // item index per row; leading tiles use their own indices
    std::vector<media::Column> m_columns;

private:
    std::wstring SelectionKey(size_t item) const;
    std::wstring FocusKey() const;
    void RebuildItemKeys();
    std::unordered_set<std::wstring> m_selected;
    std::vector<std::wstring> m_itemKeys;
    size_t m_focus = SIZE_MAX;
    std::wstring m_emptyText;
    std::wstring m_filter;
    size_t m_sortColumn = SIZE_MAX;
    bool m_sortDescending = false;
};

// Albums as tiles: an artist's albums (optionally after a track-list tile) or the saved
// albums. The page offers Expanded tile as a separate song list, built by the page.
class AlbumGridSource : public GridSource {
public:
    // album = nullptr: the leading track-list tile.
    using Open = std::function<void(const stream::Album* album)>;
    using Menu = std::function<void(POINT screen, const stream::Album* album)>;
    // tracksLabel: non-empty adds a provider-labelled track-list tile first.
    AlbumGridSource(stream::Provider& provider, std::vector<stream::Album> albums, std::wstring tracksArtist,
                    std::wstring tracksLabel, std::wstring emptyText, Open open, Menu menu);

    std::wstring ColumnSetKey() const override { return L"stream-albums"; }
    std::wstring Cell(size_t row, size_t column) const override;
    media::TileStyle Tiles() const override { return media::TileStyle::Album; }
    media::TileInfo Tile(size_t row) const override;
    std::wstring ArtKey(size_t row) const override;
    bool SupportsViewStyle(media::ViewStyle style) const override { return style != media::ViewStyle::Details; }
    media::ViewStyle GetViewStyle() const override { return media::ViewStyle::Icons; }
    void Activate(size_t row) override;
    void ContextMenu(HWND owner, POINT screen) override;

    void ReplaceAlbums(const std::vector<stream::Album>& albums);
    void AppendAlbums(const std::vector<stream::Album>& albums);
    void RemoveAlbum(const stream::Id& album);

private:
    size_t ItemCount() const override { return m_albums.size(); }
    std::wstring ItemCell(size_t item, size_t column) const override;
    std::wstring FilterText(size_t item) const override { return m_albums[item].name; }
    std::wstring Identity(size_t item) const override;
    size_t Leading() const override { return m_tracksLabel.empty() ? 0 : 1; }
    const stream::Album* AlbumAt(size_t row) const;  // nullptr for the track-list tile

    stream::Provider& m_provider;
    std::vector<stream::Album> m_albums;
    std::wstring m_tracksArtist;
    std::wstring m_tracksLabel;
    Open m_open;
    Menu m_menu;
};

// Followed artists as tiles.
class ArtistGridSource : public GridSource {
public:
    using Open = std::function<void(const stream::Artist& artist)>;
    using Menu = std::function<void(POINT screen, const stream::Artist& artist)>;
    ArtistGridSource(stream::Provider& provider, std::vector<stream::Artist> artists, std::wstring emptyText, Open open,
                     Menu menu);

    std::wstring ColumnSetKey() const override { return L"stream-artists"; }
    std::wstring Cell(size_t row, size_t column) const override;
    media::TileStyle Tiles() const override { return media::TileStyle::Album; }
    media::TileInfo Tile(size_t row) const override;
    std::wstring ArtKey(size_t row) const override;
    bool SupportsViewStyle(media::ViewStyle style) const override { return style == media::ViewStyle::Icons; }
    media::ViewStyle GetViewStyle() const override { return media::ViewStyle::Icons; }
    void Activate(size_t row) override;
    void ContextMenu(HWND owner, POINT screen) override;

    void ReplaceArtists(const std::vector<stream::Artist>& artists);
    void AppendArtists(const std::vector<stream::Artist>& artists);
    void RemoveArtist(const stream::Id& artist);

private:
    size_t ItemCount() const override { return m_artists.size(); }
    std::wstring ItemCell(size_t item, size_t column) const override;
    std::wstring FilterText(size_t item) const override { return m_artists[item].name; }
    std::wstring Identity(size_t item) const override;

    stream::Provider& m_provider;
    std::vector<stream::Artist> m_artists;
    Open m_open;
    Menu m_menu;
};

// A shelf of playlists as WMP-style tiles, or a Details list of name / song count.
class PlaylistGridSource : public GridSource {
public:
    using Open = std::function<void(const stream::Playlist& playlist)>;
    using Menu = std::function<void(POINT screen, const stream::Playlist& playlist)>;
    PlaylistGridSource(stream::Provider& provider, std::vector<stream::Playlist> playlists, std::wstring emptyText,
                       Open open, Menu menu);

    std::wstring ColumnSetKey() const override { return L"stream-playlists"; }
    std::wstring Cell(size_t row, size_t column) const override;
    media::TileStyle Tiles() const override {
        return m_style == media::ViewStyle::Icons ? media::TileStyle::Album : media::TileStyle::None;
    }
    media::TileInfo Tile(size_t row) const override;
    std::wstring ArtKey(size_t row) const override;
    bool SupportsViewStyle(media::ViewStyle style) const override { return style != media::ViewStyle::Tiles; }
    media::ViewStyle GetViewStyle() const override { return m_style; }
    void SetViewStyle(media::ViewStyle style) override;
    void Activate(size_t row) override;
    void ContextMenu(HWND owner, POINT screen) override;

    void ReplacePlaylists(const std::vector<stream::Playlist>& playlists);
    void AppendPlaylists(const std::vector<stream::Playlist>& playlists);
    void RemovePlaylist(const stream::Id& playlist);
    void RenamePlaylist(const stream::Id& playlist, const std::wstring& name);

private:
    size_t ItemCount() const override { return m_playlists.size(); }
    std::wstring ItemCell(size_t item, size_t column) const override;
    std::wstring FilterText(size_t item) const override { return m_playlists[item].name; }
    std::wstring Identity(size_t item) const override;

    stream::Provider& m_provider;
    std::vector<stream::Playlist> m_playlists;
    Open m_open;
    Menu m_menu;
    media::ViewStyle m_style = media::ViewStyle::Icons;
};

// The song page's list: the credits in the service's groups (a person and their roles), then
// the similar songs (title and artist), as WMP 12's details grouped under header lines.
// A credited person with an artist page opens it; a similar song plays from where it is.
class SongInfoSource : public GridSource {
public:
    using OpenArtist = std::function<void(const stream::Id& artist, const std::wstring& name)>;
    SongInfoSource(stream::Provider& provider, const stream::SongDetails& details, std::wstring emptyText,
                   OpenArtist openArtist);

    std::wstring ColumnSetKey() const override { return L"stream-song-info"; }
    std::wstring Cell(size_t row, size_t column) const override;
    const std::vector<media::RowGroup>* Groups() const override;
    bool GroupArt() const override { return false; }
    bool CanSort(size_t) const override { return false; }
    bool SupportsViewStyle(media::ViewStyle style) const override { return style == media::ViewStyle::Details; }
    metadb_handle_ptr Item(size_t row) const override;
    void Activate(size_t row) override;
    void ContextMenu(HWND owner, POINT screen) override;
    metadb_handle_list SelectedItems() const override;

private:
    struct Line {
        size_t group = 0;
        std::wstring name, detail;
        stream::Id artistId; // a credit with an artist page
        stream::Id trackId;  // a similar song
    };
    size_t ItemCount() const override { return m_lines.size(); }
    std::wstring ItemCell(size_t item, size_t column) const override;
    std::wstring FilterText(size_t item) const override { return m_lines[item].name + L" " + m_lines[item].detail; }
    std::wstring Identity(size_t item) const override;

    stream::Provider& m_provider;
    std::vector<std::wstring> m_groupTitles;
    std::vector<Line> m_lines;
    OpenArtist m_openArtist;
    mutable std::vector<media::RowGroup> m_groups;
};

}  // namespace glasswave::ui
