#include "pch.h"
#include "ui/artist_overview.h"
#include "ui/i18n.h"
#include "ui/win7_menu.h"

namespace glasswave::ui {

namespace {

// The parts of a mixed album list (a provider with one list of every release: NetEase). EPs and
// singles go apart where the service tells them apart; a release it types as either ("EP/Single")
// stays in the combined group, as Apple Music shows both.
enum class AlbumPart { Albums, Eps, Singles, SinglesAndEps, Live, Compilations };
constexpr AlbumPart kAlbumParts[] = {AlbumPart::Albums,        AlbumPart::Eps,  AlbumPart::Singles,
                                     AlbumPart::SinglesAndEps, AlbumPart::Live, AlbumPart::Compilations};

AlbumPart PartOf(const stream::Album& album) {
    const std::wstring& t = album.type;
    if (t == L"Live album") return AlbumPart::Live;
    if (t == L"Compilation") return AlbumPart::Compilations;
    if (t == L"EP") return AlbumPart::Eps;
    if (t == L"Single") return AlbumPart::Singles;
    if (t == L"EP or single") return AlbumPart::SinglesAndEps;
    return AlbumPart::Albums;
}

const wchar_t* PartLabel(AlbumPart part) {
    switch (part) {
    case AlbumPart::Eps: return L"EPs";
    case AlbumPart::Singles: return L"Singles";
    case AlbumPart::SinglesAndEps: return L"Singles and EPs";
    case AlbumPart::Live: return L"Live albums";
    case AlbumPart::Compilations: return L"Compilation albums";
    default: return L"Full albums";
    }
}

// "2019-05-01" or the year, for picking the newest release of a mixed list.
std::wstring ReleaseKey(const stream::Album& album) {
    if (!album.released.empty()) return album.released;
    return album.year ? std::to_wstring(album.year) : std::wstring();
}

std::wstring Subtitle(const stream::Album& album) {
    std::wstring year = album.year ? std::to_wstring(album.year) : std::wstring();
    if (year.empty() && album.released.size() >= 4) year = album.released.substr(0, 4);
    return year;
}

const stream::Id& IdOf(const ArtistOverviewSource::Value& value) {
    return std::visit([](const auto& v) -> const stream::Id& { return v.id; }, value);
}

}  // namespace

ArtistOverviewSource::ArtistOverviewSource(stream::Provider& provider, std::vector<stream::ArtistSection> sections,
                                           Actions actions)
    : GridSource({{L"Name", 0, true}}, L""),
      m_provider(provider),
      m_sections(std::move(sections)),
      m_data(m_sections.size()),
      m_actions(std::move(actions)) {
    Rebuild();
}

void ArtistOverviewSource::SetSection(size_t section, const stream::ListState& state) {
    if (section >= m_data.size()) return;
    Data& data = m_data[section];
    data.rows.clear();
    std::visit(
        [&](const auto& rows) {
            using Row = typename std::decay_t<decltype(rows)>::value_type;
            if constexpr (std::is_same_v<Row, stream::Track> || std::is_same_v<Row, stream::Album> ||
                          std::is_same_v<Row, stream::Playlist> || std::is_same_v<Row, stream::Artist>)
                for (const auto& row : rows) data.rows.emplace_back(row);
        },
        state.page.rows);
    data.total = state.page.total;
    data.more = !state.complete && state.error.empty();
    Rebuild();
}

// The groups and their tiles from the sections' rows: a mixed album list splits into its
// parts, its newest release first of all as the latest release (unless the source has one).
void ArtistOverviewSource::Rebuild() {
    m_groupInfo.clear();
    m_entries.clear();
    const bool ownLatest = std::any_of(m_sections.begin(), m_sections.end(),
                                       [](const stream::ArtistSection& s) { return s.id == L"latest-release"; });
    const auto addGroup = [&](std::wstring key, std::wstring label, size_t section) {
        m_groupInfo.push_back({std::move(key), std::move(label), section, 0});
        return m_groupInfo.size() - 1;
    };
    const auto add = [&](size_t group, const Value& value) {
        m_entries.push_back({group, value});
        ++m_groupInfo[group].count;
    };
    // The newest release of a mixed album list leads, as Apple Music's latest release does.
    if (!ownLatest)
        for (size_t s = 0; s < m_sections.size(); ++s) {
            if (!m_sections[s].mixedAlbums) continue;
            const stream::Album* newest = nullptr;
            for (const auto& value : m_data[s].rows)
                if (const auto* album = std::get_if<stream::Album>(&value))
                    if (!ReleaseKey(*album).empty() && (!newest || ReleaseKey(*album) > ReleaseKey(*newest)))
                        newest = album;
            if (newest) add(addGroup(L"latest", L"Latest release", s), *newest);
            break;
        }
    for (size_t s = 0; s < m_sections.size(); ++s) {
        const auto& section = m_sections[s];
        const auto& rows = m_data[s].rows;
        if (rows.empty()) continue;
        const std::wstring key = L"s" + std::to_wstring(s);
        if (!section.mixedAlbums) {
            const size_t group = addGroup(key, section.label, s);
            for (const auto& value : rows) add(group, value);
            continue;
        }
        for (const AlbumPart part : kAlbumParts) {
            size_t group = SIZE_MAX;
            for (const auto& value : rows) {
                const auto* album = std::get_if<stream::Album>(&value);
                if (!album || PartOf(*album) != part) continue;
                if (group == SIZE_MAX)
                    group = addGroup(key + L"." + std::to_wstring(static_cast<int>(part)), PartLabel(part), s);
                add(group, value);
            }
        }
    }
    // Entries go in group order (the latest release was added first, its group is first).
    std::stable_sort(m_entries.begin(), m_entries.end(),
                     [](const Entry& a, const Entry& b) { return a.group < b.group; });
    Refilter();
    BuildGroups();
}

// One header per group over the rows the filter left, one row of tiles unless opened.
void ArtistOverviewSource::BuildGroups() {
    m_groups.clear();
    for (size_t row = 0; row < m_rows.size(); ++row) {
        const size_t gi = m_entries[m_rows[row]].group;
        if (m_groups.empty() || m_groups.back().key != m_groupInfo[gi].key) {
            const Group& info = m_groupInfo[gi];
            const Data& data = m_data[info.section];
            media::RowGroup group;
            group.key = info.key;
            group.first = row;
            group.header = Tr(info.label);
            const bool open = m_open.count(info.key) != 0;
            const bool whole = !m_sections[info.section].mixedAlbums && info.key != L"latest";
            group.tileLines = open ? 0 : 1;
            group.moreBeyond = data.more && info.key != L"latest";
            if (open) {
                group.more = Tr(L"Show less");
            } else {
                // The section's own total where the service gives one, else what has been read.
                size_t n = info.count;
                if (whole && data.total) n = std::max(n, *data.total);
                group.more = Tr(L"Show all") + L" (" + std::to_wstring(n) + (data.more && !(whole && data.total) ? L"+" : L"") +
                             L")";
            }
            m_groups.push_back(std::move(group));
        }
        ++m_groups.back().count;
    }
}

void ArtistOverviewSource::SetFilter(std::wstring_view filterLower) {
    GridSource::SetFilter(filterLower);
    BuildGroups();
}

void ArtistOverviewSource::ShowGroupMore(size_t group) {
    if (group >= m_groups.size()) return;
    const std::wstring key = m_groups[group].key;
    const auto info = std::find_if(m_groupInfo.begin(), m_groupInfo.end(), [&](const Group& g) { return g.key == key; });
    if (info == m_groupInfo.end()) return;
    const bool open = !m_open.count(key);
    if (open)
        m_open.insert(key);
    else
        m_open.erase(key);
    BuildGroups();
    if (m_actions.showAll) m_actions.showAll(info->section, open);
}

std::wstring ArtistOverviewSource::ItemCell(size_t item, size_t) const {
    if (item >= m_entries.size()) return {};
    return std::visit(
        [](const auto& v) -> std::wstring {
            if constexpr (std::is_same_v<std::decay_t<decltype(v)>, stream::Track>)
                return v.title;
            else
                return v.name;
        },
        m_entries[item].value);
}

std::wstring ArtistOverviewSource::Cell(size_t row, size_t column) const {
    return row < m_rows.size() ? ItemCell(m_rows[row], column) : std::wstring();
}

std::wstring ArtistOverviewSource::FilterText(size_t item) const {
    std::wstring text = ItemCell(item, 0);
    if (const auto* track = std::get_if<stream::Track>(&m_entries[item].value)) text += L" " + track->album;
    return text;
}

std::wstring ArtistOverviewSource::Identity(size_t item) const {
    const Entry& e = m_entries[item];
    return m_groupInfo[e.group].key + L"|" + IdOf(e.value);
}

media::TileInfo ArtistOverviewSource::Tile(size_t row) const {
    media::TileInfo tile;
    const Entry* entry = EntryAt(row);
    if (!entry) return tile;
    std::visit(
        [&](const auto& v) {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, stream::Track>) {
                tile.art = v.albumId.empty() ? m_provider.TrackItem(v.id)
                                             : m_provider.ArtItem(stream::ArtKind::Album, v.albumId);
                tile.lines = {v.title, v.album.empty() ? stream::JoinArtists(v.artists) : v.album};
                tile.badges = v.badges;
            } else if constexpr (std::is_same_v<T, stream::Album>) {
                tile.art = m_provider.ArtItem(stream::ArtKind::Album, v.id);
                tile.lines = {v.name, Subtitle(v)};
            } else if constexpr (std::is_same_v<T, stream::Playlist>) {
                if (v.coverUrl.empty())
                    tile.icon = 29722; // WMP's large playlist artwork
                else
                    tile.art = m_provider.ArtItem(stream::ArtKind::Playlist, v.id);
                tile.lines = {v.name, v.creator};
            } else {
                tile.art = m_provider.ArtItem(stream::ArtKind::Artist, v.id);
                tile.lines = {v.name, {}};
            }
        },
        entry->value);
    return tile;
}

std::wstring ArtistOverviewSource::ArtKey(size_t row) const {
    const Entry* entry = EntryAt(row);
    if (!entry) return {};
    return std::visit(
        [&](const auto& v) -> std::wstring {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, stream::Track>)
                return v.albumId.empty() ? m_provider.ArtKey(stream::ArtKind::Track, v.id)
                                         : m_provider.ArtKey(stream::ArtKind::Album, v.albumId);
            else if constexpr (std::is_same_v<T, stream::Album>)
                return m_provider.ArtKey(stream::ArtKind::Album, v.id);
            else if constexpr (std::is_same_v<T, stream::Playlist>)
                return m_provider.ArtKey(stream::ArtKind::Playlist, v.id);
            else
                return m_provider.ArtKey(stream::ArtKind::Artist, v.id);
        },
        entry->value);
}

metadb_handle_ptr ArtistOverviewSource::Item(size_t row) const {
    const Entry* entry = EntryAt(row);
    if (!entry) return {};
    const auto* track = std::get_if<stream::Track>(&entry->value);
    return track ? m_provider.TrackItem(track->id) : metadb_handle_ptr();
}

metadb_handle_list ArtistOverviewSource::SelectedItems() const {
    metadb_handle_list items;
    for (size_t row = 0; row < m_rows.size(); ++row)
        if (IsSelected(row))
            if (auto item = Item(row); item.is_valid()) items.add_item(item);
    return items;
}

// A song plays its group's songs from it; anything else opens.
void ArtistOverviewSource::Activate(size_t row) {
    const Entry* entry = EntryAt(row);
    if (!entry) return;
    std::visit(
        [&](const auto& v) {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, stream::Track>) {
                metadb_handle_list items;
                size_t start = 0;
                for (size_t r = 0; r < m_rows.size(); ++r) {
                    if (m_entries[m_rows[r]].group != entry->group) continue;
                    if (auto item = Item(r); item.is_valid()) {
                        if (r == row) start = items.get_count();
                        items.add_item(item);
                    }
                }
                if (items.get_count()) media::PlayItems(items, start);
            } else if constexpr (std::is_same_v<T, stream::Album>) {
                if (m_actions.openAlbum) m_actions.openAlbum(v);
            } else if constexpr (std::is_same_v<T, stream::Playlist>) {
                if (m_actions.openPlaylist) m_actions.openPlaylist(v);
            } else {
                if (m_actions.openArtist) m_actions.openArtist(v);
            }
        },
        entry->value);
}

void ArtistOverviewSource::PlayTile(size_t row) {
    const Entry* entry = EntryAt(row);
    if (!entry) return;
    if (const auto* album = std::get_if<stream::Album>(&entry->value))
        PlayRemoteAlbum(m_provider, album->id);
    else if (const auto* playlist = std::get_if<stream::Playlist>(&entry->value))
        PlayRemotePlaylist(m_provider, playlist->id);
    else if (std::holds_alternative<stream::Track>(entry->value))
        Activate(row); // a song plays its group from it
}

void ArtistOverviewSource::ContextMenu(HWND owner, POINT screen) {
    const Entry* entry = EntryAt(Focus());
    if (!entry) return;
    if (std::holds_alternative<stream::Track>(entry->value)) {
        metadb_handle_list items = SelectedItems();
        if (!items.get_count())
            if (auto item = Item(Focus()); item.is_valid()) items.add_item(item);
        if (items.get_count()) RunContextMenu(owner, items, screen);
        return;
    }
    if (m_actions.menu) m_actions.menu(screen, entry->value);
}

}  // namespace glasswave::ui
