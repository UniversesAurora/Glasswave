#pragma once

#include "media/list_sources.h"
#include "stream/provider.h"

#include <functional>
#include <vector>

// Cover-tile pages of a streaming provider: albums (an artist's, or the saved ones),
// followed artists and playlists. Opening an item and its context menu belong to the page.
namespace glasswave::ui {

// Selection, focus and filtering shared by the tile sources. Rows are the indices of the
// items that pass the filter; the selection is per row.
class GridSource : public media::ListSource {
public:
    const std::vector<media::Column>& Columns() const override { return m_columns; }
    size_t Count() const override { return m_rows.size(); }
    std::wstring EmptyText() const override { return m_emptyText; }
    bool IsSelected(size_t row) const override { return row < m_selected.size() && m_selected[row]; }
    void SelectOnly(size_t row) override;
    void Toggle(size_t row) override;
    void SelectRange(size_t a, size_t b, bool keepOthers) override;
    void SelectAll() override;
    void SetSelection(const std::vector<size_t>& rows) override;
    size_t Focus() const override { return m_focus; }
    void SetFocus(size_t row) override { m_focus = row; }
    metadb_handle_list SelectedItems() const override { return {}; }
    void SetFilter(std::wstring_view filterLower) override;

protected:
    GridSource(std::vector<media::Column> columns, std::wstring emptyText);
    // Item count and the text the search box matches for item i.
    virtual size_t ItemCount() const = 0;
    virtual std::wstring FilterText(size_t item) const = 0;
    // Items always listed before the filtered ones (an artist's Hot songs tile).
    virtual size_t Leading() const { return 0; }
    void Refilter() { SetFilter(m_filter); }

    std::vector<size_t> m_rows;  // item index per row; leading tiles use their own indices
    std::vector<media::Column> m_columns;

private:
    std::vector<uint8_t> m_selected;
    size_t m_focus = SIZE_MAX;
    std::wstring m_emptyText;
    std::wstring m_filter;
};

// Albums as tiles: an artist's albums (optionally after a Hot songs tile) or the saved
// albums. The page offers Expanded tile as a separate song list, built by the page.
class AlbumGridSource : public GridSource {
public:
    // album = nullptr: the Hot songs tile.
    using Open = std::function<void(const stream::Album* album)>;
    using Menu = std::function<void(POINT screen, const stream::Album* album)>;
    // hotSongsArtist: non-empty adds the Hot songs tile first.
    AlbumGridSource(stream::Provider& provider, std::vector<stream::Album> albums, std::wstring hotSongsArtist,
                    std::wstring emptyText, Open open, Menu menu);

    std::wstring ColumnSetKey() const override { return L"stream-albums"; }
    std::wstring Cell(size_t row, size_t column) const override;
    media::TileStyle Tiles() const override { return media::TileStyle::Album; }
    media::TileInfo Tile(size_t row) const override;
    std::wstring ArtKey(size_t row) const override;
    bool SupportsViewStyle(media::ViewStyle style) const override { return style != media::ViewStyle::Details; }
    media::ViewStyle GetViewStyle() const override { return media::ViewStyle::Icons; }
    void Activate(size_t row) override;
    void ContextMenu(HWND owner, POINT screen) override;

    void AppendAlbums(const std::vector<stream::Album>& albums);
    void RemoveAlbum(const stream::Id& album);

private:
    size_t ItemCount() const override { return m_albums.size(); }
    std::wstring FilterText(size_t item) const override { return m_albums[item].name; }
    size_t Leading() const override { return m_hotSongsArtist.empty() ? 0 : 1; }
    const stream::Album* AlbumAt(size_t row) const;  // nullptr for the Hot songs tile

    stream::Provider& m_provider;
    std::vector<stream::Album> m_albums;
    std::wstring m_hotSongsArtist;
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

private:
    size_t ItemCount() const override { return m_artists.size(); }
    std::wstring FilterText(size_t item) const override { return m_artists[item].name; }

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
    void SetViewStyle(media::ViewStyle style) override { m_style = style; }
    void Activate(size_t row) override;
    void ContextMenu(HWND owner, POINT screen) override;

    void RemovePlaylist(const stream::Id& playlist);
    void RenamePlaylist(const stream::Id& playlist, const std::wstring& name);

private:
    size_t ItemCount() const override { return m_playlists.size(); }
    std::wstring FilterText(size_t item) const override { return m_playlists[item].name; }

    stream::Provider& m_provider;
    std::vector<stream::Playlist> m_playlists;
    Open m_open;
    Menu m_menu;
    media::ViewStyle m_style = media::ViewStyle::Icons;
};

}  // namespace glasswave::ui
