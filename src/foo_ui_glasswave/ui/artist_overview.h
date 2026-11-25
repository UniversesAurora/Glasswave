#pragma once

#include "stream/list_session.h"
#include "ui/remote_grids.h"

#include <set>
#include <variant>

// An artist page's overview, the same for every source (owner, 2026-11-14, after Apple Music's
// artist page): one WMP 12 group of tiles per section the source declares
// (stream::Provider::ArtistSections), in its order. A section holds songs, albums, playlists or
// artists; a mixed album list is sorted into Latest release, Albums, Singles and EPs, Live
// albums and Compilations by Album::type. Each group shows one row of tiles and "Show all (N)";
// the link opens the group in place (loading the section's other pages) and closes it again.
// A section the source has nothing in is left out. The page owns the sessions and feeds them in.
namespace glasswave::ui {

class ArtistOverviewSource : public GridSource {
  public:
    using Value = std::variant<stream::Track, stream::Album, stream::Playlist, stream::Artist>;
    struct Actions {
        std::function<void(const stream::Album&)> openAlbum;
        std::function<void(const stream::Playlist&)> openPlaylist;
        std::function<void(const stream::Artist&)> openArtist;
        // Right click on a tile that is not a song (songs get the song menu).
        std::function<void(POINT screen, const Value&)> menu;
        // A group opened (true) or closed: the page loads the rest of section `section`.
        std::function<void(size_t section, bool open)> showAll;
    };

    ArtistOverviewSource(stream::Provider& provider, std::vector<stream::ArtistSection> sections, Actions actions);

    // A section's list as its session holds it.
    void SetSection(size_t section, const stream::ListState& state);
    const std::vector<stream::ArtistSection>& Sections() const { return m_sections; }
    const std::set<std::wstring>& ExpandedGroups() const { return m_open; }
    void SetExpandedGroups(std::set<std::wstring> groups);
    // Indices into Sections() that currently have at least one expanded group.
    std::set<size_t> OpenSections() const;

    std::wstring ColumnSetKey() const override { return L"stream-artist-overview"; }
    std::wstring Cell(size_t row, size_t column) const override;
    media::TileStyle Tiles() const override { return media::TileStyle::Album; }
    media::TileInfo Tile(size_t row) const override;
    std::wstring ArtKey(size_t row) const override;
    const std::vector<media::RowGroup>* Groups() const override { return &m_groups; }
    void ShowGroupMore(size_t group) override;
    bool CanSort(size_t) const override { return false; }
    bool SupportsViewStyle(media::ViewStyle style) const override { return style != media::ViewStyle::Details; }
    media::ViewStyle GetViewStyle() const override { return media::ViewStyle::Icons; }
    metadb_handle_ptr Item(size_t row) const override;
    metadb_handle_list SelectedItems() const override;
    void Activate(size_t row) override;
    void ContextMenu(HWND owner, POINT screen) override;
    void SetFilter(std::wstring_view filterLower) override;
    bool CanPlayTile(size_t row) const override {
        const Entry* entry = EntryAt(row);
        return entry && !std::holds_alternative<stream::Artist>(entry->value);
    }
    void PlayTile(size_t row) override;

  private:
    struct Data {
        std::vector<Value> rows;
        std::optional<size_t> total;
        bool more = false; // pages the session has not read yet
    };
    struct Entry {
        size_t group = 0;
        Value value;
    };
    struct Group {
        std::wstring key; // stable across rebuilds: which section and which part of it
        std::wstring label;
        size_t section = 0;
        size_t count = 0;
    };

    size_t ItemCount() const override { return m_entries.size(); }
    std::wstring ItemCell(size_t item, size_t column) const override;
    std::wstring FilterText(size_t item) const override;
    std::wstring Identity(size_t item) const override;
    void Rebuild();
    void BuildGroups();
    const Entry* EntryAt(size_t row) const { return row < m_rows.size() ? &m_entries[m_rows[row]] : nullptr; }

    stream::Provider& m_provider;
    std::vector<stream::ArtistSection> m_sections;
    std::vector<Data> m_data;
    Actions m_actions;
    std::vector<Group> m_groupInfo;
    std::vector<Entry> m_entries;
    std::vector<media::RowGroup> m_groups;
    std::set<std::wstring> m_open; // groups shown whole
};

}  // namespace glasswave::ui
