#include "pch.h"
#include "ui/song_table.h"
#include "core/text.h"

#include "cd/disc.h"
#include "media/favorites.h"
#include "media/special_playlists.h"
#include "ui/i18n.h"
#include "ui/song_grouping.h"
#include "ui/text.h"
#include "ui/win7_menu.h"

#include <cwctype>
#include <numeric>
#include <unordered_set>

namespace glasswave::ui {

using core::Wide;

namespace {

enum SongField {
    kAlbumColumn,
    kNumber,
    kArt,
    kTitle,
    kLength,
    kArtist,
    kAlbum,
    kPopularity,
    kPlayCount,
    kSource,
    kFavorite,
    kLyric,
    kDownload
};

int CompareText(const std::wstring& a, const std::wstring& b) {
    return CompareStringEx(LOCALE_NAME_USER_DEFAULT, LINGUISTIC_IGNORECASE | SORT_DIGITSASNUMBERS, a.c_str(),
                           static_cast<int>(a.size()), b.c_str(), static_cast<int>(b.size()), nullptr, nullptr, 0) -
           2;
}

std::wstring LengthText(int milliseconds) {
    const int total = milliseconds > 0 ? milliseconds / 1000 : 0;
    wchar_t buffer[16];
    swprintf_s(buffer, L"%d:%02d", total / 60, total % 60);
    return buffer;
}

std::wstring Padded(long long value) {
    wchar_t pad[32];
    swprintf_s(pad, L"%012lld", value);
    return pad;
}

// A titleformat field of a local track, with the scripts compiled once (main thread).
std::wstring LocalField(const metadb_handle_ptr& track, const char* script) {
    static std::map<std::string, titleformat_object::ptr> scripts;
    auto it = scripts.find(script);
    if (it == scripts.end()) {
        titleformat_object::ptr compiled;
        titleformat_compiler::get()->compile_safe(compiled, script);
        it = scripts.emplace(script, compiled).first;
    }
    pfc::string8 out;
    if (it->second.is_valid()) track->format_title(nullptr, out, it->second, nullptr);
    return Wide(out.get_ptr());
}

std::wstring AlbumLabel(const SongTableRow& row) { return row.album.empty() ? Tr(L"Unknown Album") : row.album; }

std::wstring ArtistLabel(const SongTableRow& row) {
    return row.artist.empty() ? Tr(L"Unknown Artist") : row.artist;
}

void FinishRow(SongTableRow& row) {
    row.length = LengthText(row.lengthMs);
    row.search = media::Lower(row.title + L'\n' + row.note + L'\n' + row.artist + L'\n' + row.album);
    row.playCount = media::PlayStats::Get().CountFor(row.handle);
}

SongTableRow TrackRow(stream::Provider& provider, const stream::Track& track) {
    SongTableRow row;
    row.provider = &provider;
    row.handle = provider.TrackItem(track.id);
    row.trackId = track.id;
    row.albumId = track.albumId;
    for (const auto& artist : track.artists)
        if (!artist.id.empty()) {
            row.artistId = artist.id;
            break;
        }
    row.title = track.title;
    row.note = track.note;
    row.badges = track.badges;
    row.artist = stream::JoinArtists(track.artists);
    row.album = track.album;
    row.lengthMs = track.durationMs;
    row.popularity = track.popularity;
    row.albumKey =
        RemoteAlbumGroupKey(provider.Key(), track.albumId, track.album, row.artist, track.id);
    row.artKey = track.albumId.empty() ? provider.ArtKey(stream::ArtKind::Track, track.id)
                                       : provider.ArtKey(stream::ArtKind::Album, track.albumId);
    row.favorite = provider.IsFavorite(track.id);
    row.source = Tr(provider.Name());
    FinishRow(row);
    return row;
}

std::vector<SongTableRow> TrackRows(stream::Provider& provider, const std::vector<stream::Track>& tracks,
                                    size_t first = 0, size_t count = SIZE_MAX) {
    first = std::min(first, tracks.size());
    count = std::min(count, tracks.size() - first);
    std::vector<SongTableRow> rows;
    rows.reserve(count);
    for (size_t i = first; i < first + count; ++i) rows.push_back(TrackRow(provider, tracks[i]));
    return rows;
}

// Local album rows share one cover key per album, like the library's album groups.
std::wstring LocalAlbumKey(const std::wstring& prefix, const SongTableRow& row, const std::wstring& albumArtist,
                           const metadb_handle_ptr& track) {
    if (row.album.empty())
        return prefix + Wide(track->get_path()) + L'|' + std::to_wstring(track->get_subsong_index());
    return prefix + media::Lower(row.album) + L'\x1f' + media::Lower(albumArtist);
}

// A local file's row; `artPrefix` keeps the page's covers apart from other pages' keys.
SongTableRow LocalRow(const metadb_handle_ptr& track, const wchar_t* artPrefix) {
    SongTableRow row;
    row.handle = track;
    row.title = LocalField(track, "$if2(%title%,%filename%)");
    row.artist = LocalField(track, "[%artist%]");
    row.album = LocalField(track, "[%album%]");
    row.lengthMs = _wtoi(LocalField(track, "%length_seconds%").c_str()) * 1000;
    row.source = Tr(L"Local Music");
    const std::wstring albumArtist = LocalField(track, "[%album artist%]");
    row.albumKey = LocalAlbumKey(L"l:", row, albumArtist, track);
    row.artKey = LocalAlbumKey(artPrefix, row, albumArtist, track);
    row.favorite = media::IsFavorite(track);
    FinishRow(row);
    return row;
}

std::vector<SongTableRow> FavoriteRows(const std::vector<metadb_handle_ptr>& local,
                                       const AllFavoritesSource::RemoteFavorites& remote) {
    std::vector<SongTableRow> rows;
    for (const auto& track : local) {
        if (!track.is_valid()) continue;
        SongTableRow row = LocalRow(track, L"fav:");
        row.favorite = true;  // every local row comes from the Favorites playlist
        rows.push_back(std::move(row));
    }
    for (const auto& [provider, tracks] : remote)
        for (auto& row : TrackRows(*provider, tracks)) rows.push_back(std::move(row));
    return rows;
}

std::vector<SongTableRow> PlayStatRows(const std::vector<media::PlayRecord>& records) {
    std::vector<SongTableRow> rows;
    rows.reserve(records.size());
    for (const auto& record : records) {
        SongTableRow row;
        row.title = Wide(record.title.c_str());
        row.artist = Wide(record.artist.c_str());
        row.album = Wide(record.album.c_str());
        row.lengthMs = record.durationMs;
        if (auto* provider = record.provider.empty() ? nullptr : stream::FindProvider(record.provider)) {
            row.provider = provider;
            row.handle = provider->TrackItem(record.trackId);
            row.trackId = record.trackId;
            row.albumId = record.albumId;
            row.artistId = record.artistId;
            row.albumKey =
                RemoteAlbumGroupKey(provider->Key(), record.albumId, row.album, row.artist, record.trackId);
            row.artKey = record.albumId.empty() ? provider->ArtKey(stream::ArtKind::Track, record.trackId)
                                                : provider->ArtKey(stream::ArtKind::Album, record.albumId);
            row.favorite = provider->IsFavorite(record.trackId);
            row.source = Tr(provider->Name());
        } else if (record.provider.empty()) {
            row.handle = metadb::get()->handle_create(record.path.c_str(), record.subsong);
            row.albumKey = L"l:" + media::Lower(row.album) + L'\x1f' + media::Lower(row.artist);
            row.artKey = record.album.empty() ? L"stats:" + Wide(record.key.c_str())
                                              : L"stats:" + media::Lower(row.album) + L'\x1f' + media::Lower(row.artist);
            row.favorite = media::IsFavorite(row.handle);
            row.source = Tr(L"Local Music");
        } else {
            continue;  // a provider that is no longer compiled in
        }
        FinishRow(row);
        row.playCount = record.count;
        rows.push_back(std::move(row));
    }
    return rows;
}

std::wstring StableRowBase(const SongTableRow& row) {
    if (row.provider) {
        return L"p:" + std::wstring(row.provider->Key()) + L'\x1f' + row.trackId;
    }
    if (row.handle.is_valid()) {
        return L"l:" + Wide(row.handle->get_path()) + L'\x1f' +
               std::to_wstring(row.handle->get_subsong_index());
    }
    return L"x:" + row.source + L'\x1f' + row.title + L'\x1f' + row.artist + L'\x1f' + row.album + L'\x1f' +
           std::to_wstring(row.lengthMs);
}

std::vector<std::wstring> StableRowIdentities(const std::vector<SongTableRow>& rows) {
    std::map<std::wstring, size_t> occurrences;
    std::vector<std::wstring> identities;
    identities.reserve(rows.size());
    for (const auto& row : rows) {
        const std::wstring base = StableRowBase(row);
        const size_t occurrence = occurrences[base]++;
        identities.push_back(base + L'\x1e' + std::to_wstring(occurrence));
    }
    return identities;
}

void RefreshRemoteAlbumKeys(std::vector<SongTableRow>& rows) {
    std::vector<RemoteAlbumGroupRow> identities;
    std::vector<size_t> indices;
    for (size_t i = 0; i < rows.size(); ++i) {
        const SongTableRow& row = rows[i];
        if (!row.provider) continue;
        identities.push_back({row.provider->Key(), row.albumId, row.album, row.artist, row.trackId});
        indices.push_back(i);
    }
    const auto keys = RemoteAlbumGroupKeys(identities);
    for (size_t i = 0; i < indices.size(); ++i) rows[indices[i]].albumKey = keys[i];
}

std::wstring RemoteAlbumNameKey(const SongTableRow& row) {
    return row.provider ? detail::AlbumNameGroupKey(row.provider->Key(), row.album, row.artist) : std::wstring();
}

using RemoteAlbumIdIndex = std::unordered_map<std::wstring, std::unordered_set<std::wstring>>;

void AddKnownRemoteAlbums(const std::vector<SongTableRow>& rows, RemoteAlbumIdIndex& byName) {
    for (const SongTableRow& row : rows) {
        if (!row.provider || row.albumId.empty() || row.album.empty() || row.artist.empty()) continue;
        byName[RemoteAlbumNameKey(row)].insert(row.albumId);
    }
}

void RebuildKnownRemoteAlbums(const std::vector<SongTableRow>& rows, RemoteAlbumIdIndex& byName) {
    byName.clear();
    AddKnownRemoteAlbums(rows, byName);
}

std::unordered_set<std::wstring> RefreshAppendedRemoteAlbumKeys(std::vector<SongTableRow>& rows,
                                                               const RemoteAlbumIdIndex& previous) {
    RefreshRemoteAlbumKeys(rows);
    RemoteAlbumIdIndex appended;
    AddKnownRemoteAlbums(rows, appended);
    std::unordered_set<std::wstring> resolved;
    for (SongTableRow& row : rows) {
        if (!row.provider || !row.albumId.empty() || row.album.empty() || row.artist.empty()) continue;
        const std::wstring name = RemoteAlbumNameKey(row);
        const auto old = previous.find(name);
        const auto added = appended.find(name);
        std::unordered_set<std::wstring> ids;
        if (old != previous.end()) ids.insert(old->second.begin(), old->second.end());
        if (added != appended.end()) ids.insert(added->second.begin(), added->second.end());
        if (ids.size() == 1) {
            row.albumKey = RemoteAlbumGroupKey(row.provider->Key(), *ids.begin(), row.album, row.artist,
                                               row.trackId);
            resolved.insert(name);
        }
    }
    return resolved;
}

void RebuildResolvedMissingAlbums(const std::vector<SongTableRow>& rows, const RemoteAlbumIdIndex& byName,
                                  std::unordered_set<std::wstring>& resolved) {
    resolved.clear();
    for (const SongTableRow& row : rows) {
        if (!row.provider || !row.albumId.empty() || row.album.empty() || row.artist.empty()) continue;
        const std::wstring name = RemoteAlbumNameKey(row);
        const auto known = byName.find(name);
        if (known != byName.end() && known->second.size() == 1) resolved.insert(name);
    }
}

bool CreatesAmbiguousRemoteAlbum(const std::vector<SongTableRow>& rows,
                                 const RemoteAlbumIdIndex& previous,
                                 const std::unordered_set<std::wstring>& resolvedMissing) {
    for (const SongTableRow& row : rows) {
        if (!row.provider || row.albumId.empty() || row.album.empty() || row.artist.empty()) continue;
        const std::wstring name = RemoteAlbumNameKey(row);
        if (!resolvedMissing.contains(name)) continue;
        const auto known = previous.find(name);
        if (known != previous.end() &&
            (known->second.size() > 1 || *known->second.begin() != row.albumId))
            return true;
    }
    return false;
}

void AddUnresolvedRemoteAlbums(const std::vector<SongTableRow>& rows,
                               std::unordered_set<std::wstring>& unresolved) {
    for (const SongTableRow& row : rows) {
        if (!row.provider || !row.albumId.empty() || row.album.empty() || row.artist.empty()) continue;
        const std::wstring fallback =
            RemoteAlbumGroupKey(row.provider->Key(), {}, row.album, row.artist, row.trackId);
        if (row.albumKey == fallback) unresolved.insert(RemoteAlbumNameKey(row));
    }
}

void RebuildUnresolvedRemoteAlbums(const std::vector<SongTableRow>& rows,
                                   std::unordered_set<std::wstring>& unresolved) {
    unresolved.clear();
    AddUnresolvedRemoteAlbums(rows, unresolved);
}

bool ResolvesUnresolvedRemoteAlbum(const std::vector<SongTableRow>& rows,
                                   const std::unordered_set<std::wstring>& unresolved) {
    return std::any_of(rows.begin(), rows.end(), [&](const SongTableRow& row) {
        return row.provider && !row.albumId.empty() && !row.album.empty() && !row.artist.empty() &&
               unresolved.contains(RemoteAlbumNameKey(row));
    });
}

}  // namespace

// ---- SongTableSource ------------------------------------------------------------------------

SongTableSource::SongTableSource(std::vector<SongTableRow> rows, bool showSource, std::wstring emptyText,
                                 std::wstring columnKey)
    : m_all(std::move(rows)),
      m_emptyText(std::move(emptyText)),
      m_columnKey(std::move(columnKey)),
      m_showSource(showSource) {
    RefreshRemoteAlbumKeys(m_all);
    RebuildUnresolvedRemoteAlbums(m_all, m_unresolvedAlbumNames);
    RebuildKnownRemoteAlbums(m_all, m_remoteAlbumIdsByName);
    RebuildResolvedMissingAlbums(m_all, m_remoteAlbumIdsByName, m_resolvedMissingAlbumNames);
    SetViewStyle(media::ViewStyle::Details);
}

std::wstring SongTableSource::ColumnSetKey() const {
    return m_columnKey + (m_style == media::ViewStyle::Tiles ? L"-tiles" : L"-details");
}

void SongTableSource::SetViewStyle(media::ViewStyle style) {
    m_style = style == media::ViewStyle::Icons ? media::ViewStyle::Tiles : style;
    if (m_style == media::ViewStyle::Tiles) {
        m_columns = {{L"Album", 200},
                     {L"#", 34, false, true},
                     {L"Title", 0, true},
                     {L"Length", 56, false, true},
                     {L"Artist", 190}};
        m_fields = {kAlbumColumn, kNumber, kTitle, kLength, kArtist};
    } else {
        m_columns = {{L"#", 34, false, true}, media::ArtColumn(false), {L"Title", 0, true},
                     {L"Length", 56, false, true}, {L"Artist", 170}, {L"Album", 170}};
        m_fields = {kNumber, kArt, kTitle, kLength, kArtist, kAlbum};
    }
    if (m_showLyric) {  // a lyrics search: the line that matched, right after the title
        const size_t at = static_cast<size_t>(std::find(m_fields.begin(), m_fields.end(), kTitle) - m_fields.begin()) + 1;
        m_columns.insert(m_columns.begin() + static_cast<std::ptrdiff_t>(at), {L"Lyrics", 260});
        m_fields.insert(m_fields.begin() + static_cast<std::ptrdiff_t>(at), kLyric);
    }
    if (m_offerDownloads) { // Apple Music's place for it: before the length
        const size_t at = static_cast<size_t>(std::find(m_fields.begin(), m_fields.end(), kLength) - m_fields.begin());
        m_columns.insert(m_columns.begin() + static_cast<std::ptrdiff_t>(at),
                         {L"Download", 32, false, false, false, media::kDownloadFormat});
        m_fields.insert(m_fields.begin() + static_cast<std::ptrdiff_t>(at), kDownload);
    }
    if (m_offerPopularity) {
        m_columns.push_back({L"Popularity", 90, false, false, !m_popularityShown, media::kPopularityFormat});
        m_fields.push_back(kPopularity);
    }
    if (m_showSource) {
        m_columns.push_back({L"Source", 140});
        m_fields.push_back(kSource);
    }
    m_columns.push_back({L"Count", 70, false, true, !m_showPlayCount});
    m_fields.push_back(kPlayCount);
    m_columns.push_back({L"Favorite", 58, false, false, false, media::kFavoriteFormat});
    m_fields.push_back(kFavorite);
    m_sortColumn = SIZE_MAX;  // columns changed: back to the view's own order
    m_sortDescending = false;
    ApplySort();
}

bool SongTableSource::CanSort(size_t column) const {
    return column < m_fields.size() && m_fields[column] != kArt && m_fields[column] != kAlbumColumn;
}

void SongTableSource::Sort(size_t column, bool descending) {
    const bool sortable = CanSort(column);
    m_sortColumn = sortable ? column : SIZE_MAX;
    m_sortDescending = sortable && descending;
    ApplySort();
}

void SongTableSource::SetGrouping(bool enabled) {
    if (m_grouping == enabled) return;
    m_grouping = enabled;
    BuildGroups();
}

bool SongTableSource::NeedsAlbumIdentity() const {
    if (m_style == media::ViewStyle::Tiles) return true;
    if (m_style != media::ViewStyle::Details || !m_grouping || m_sortColumn >= m_fields.size())
        return false;
    return m_fields[m_sortColumn] == kAlbum;
}

void SongTableSource::ShowPlayCount() {
    m_showPlayCount = true;
    SetViewStyle(m_style);
}

void SongTableSource::ShowLyric() {
    m_showLyric = true;
    SetViewStyle(m_style);
}

void SongTableSource::OfferDownloads() {
    m_offerDownloads = true;
    SetViewStyle(m_style);
}

void SongTableSource::OfferPopularity(bool shown) {
    m_offerPopularity = true;
    m_popularityShown = shown;
    SetViewStyle(m_style);
}

void SongTableSource::RefreshPlayCounts() {
    for (auto& row : m_all) row.playCount = media::PlayStats::Get().CountFor(row.handle);
}

void SongTableSource::RefreshFavorites() {
    for (auto& row : m_all)
        if (row.provider) row.favorite = row.provider->IsFavorite(row.trackId);
}

size_t SongTableSource::RemoveUnfavorited() {
    return RemoveRows([](const SongTableRow& row) { return row.provider && !row.provider->IsFavorite(row.trackId); });
}

// Section key and label of a row under the current sort column.
std::pair<std::wstring, std::wstring> SongTableSource::GroupInfo(size_t index) const {
    const SongTableRow& item = m_all[index];
    switch (m_fields[m_sortColumn]) {
    case kTitle: {
        wchar_t c = item.title.empty() ? L'#' : static_cast<wchar_t>(std::towupper(item.title[0]));
        if (std::iswpunct(static_cast<wint_t>(c)) || std::iswspace(static_cast<wint_t>(c))) c = L'#';
        const std::wstring label(1, c);
        return {label, label};
    }
    case kLength: {
        const long long minutes = item.lengthMs / 60000;
        return {std::to_wstring(minutes), std::to_wstring(minutes) + Tr(minutes == 1 ? L" minute" : L" minutes")};
    }
    case kArtist: return {media::Lower(item.artist), ArtistLabel(item)};
    case kAlbum: return {item.albumKey, AlbumLabel(item)};
    case kSource: return {item.source, item.source};
    case kLyric: return {media::Lower(item.lyric), item.lyric};
    case kPopularity: {
        const int tens = std::clamp(item.popularity, 0, 100) / 10 * 10;
        return {std::to_wstring(tens), std::to_wstring(tens) + L"-" + std::to_wstring(std::min(tens + 9, 100))};
    }
    case kPlayCount: return {std::to_wstring(item.playCount), std::to_wstring(item.playCount)};
    case kFavorite: return {item.favorite ? L"1" : L"0", Tr(item.favorite ? L"Favorite" : L"Not in Favorites")};
    default: return {};  // # is the row number itself: nothing to group by
    }
}

std::wstring SongTableSource::SortKey(size_t index) const {
    if (index >= m_all.size() || m_sortColumn >= m_fields.size()) return {};
    const SongTableRow& item = m_all[index];
    switch (m_fields[m_sortColumn]) {
    case kNumber: return Padded(static_cast<long long>(index));
    case kTitle: return item.title;
    case kLength: return Padded(item.lengthMs);
    case kArtist: return ArtistLabel(item);
    case kAlbum: return AlbumLabel(item);
    case kSource: return item.source;
    case kLyric: return item.lyric;
    case kPopularity: return Padded(item.popularity);
    case kPlayCount: return Padded(item.playCount);
    case kFavorite: return std::wstring(item.favorite ? L"1" : L"0");
    default: return {};
    }
}

bool SongTableSource::SortLess(size_t a, size_t b) const {
    const bool tiles = m_style == media::ViewStyle::Tiles;
    if (tiles && m_all[a].albumKey != m_all[b].albumKey) {
        const auto left = m_albumRank.find(m_all[a].albumKey);
        const auto right = m_albumRank.find(m_all[b].albumKey);
        if (left != m_albumRank.end() && right != m_albumRank.end() && left->second != right->second)
            return left->second < right->second;
    }
    if (m_sortColumn < m_fields.size() && m_sortKeys.size() == m_all.size()) {
        if (const int c = CompareText(m_sortKeys[a], m_sortKeys[b]))
            return m_sortDescending ? c > 0 : c < 0;
    }
    return a < b;
}

void SongTableSource::UpdateAlbumRanks(size_t first) {
    if (m_style != media::ViewStyle::Tiles) return;
    first = std::min(first, m_all.size());
    for (size_t i = first; i < m_all.size(); ++i) m_albumRank.try_emplace(m_all[i].albumKey, i);
}

void SongTableSource::SortOrder() {
    const bool tiles = m_style == media::ViewStyle::Tiles;
    const bool fieldSort = m_sortColumn < m_fields.size();
    m_sortKeys.clear();
    if (fieldSort) {
        m_sortKeys.resize(m_all.size());
        for (size_t i = 0; i < m_all.size(); ++i) m_sortKeys[i] = SortKey(i);
    }
    m_albumRank.clear();
    if (tiles) UpdateAlbumRanks(0);

    m_order.resize(m_all.size());
    for (size_t i = 0; i < m_order.size(); ++i) m_order[i] = i;
    // Unsorted Details keeps the order the view was built with (a playlist's order, a
    // hot-song chart). Expanded tile always collects an album's songs into one column,
    // ordered by where the album first appears, and a column sort applies inside albums.
    if (!tiles && !fieldSort) return;
    std::stable_sort(m_order.begin(), m_order.end(),
                     [this](size_t a, size_t b) { return SortLess(a, b); });
}

std::vector<std::wstring> SongTableSource::RowIdentities() const { return StableRowIdentities(m_all); }

std::wstring SongTableSource::FocusIdentity() const {
    if (m_focus >= m_visible.size()) return {};
    const auto identities = RowIdentities();
    return identities[m_visible[m_focus]];
}

SongTableSource::PresentationState SongTableSource::CapturePresentation() const {
    PresentationState state;
    state.focus = FocusIdentity();
    const auto identities = RowIdentities();
    for (size_t index = 0; index < m_selected.size() && index < identities.size(); ++index)
        if (m_selected[index]) state.selected.push_back(identities[index]);
    return state;
}

size_t SongTableSource::FindVisibleByIdentity(const std::wstring& identity) const {
    if (identity.empty()) return SIZE_MAX;
    const auto identities = RowIdentities();
    for (size_t row = 0; row < m_visible.size(); ++row)
        if (identities[m_visible[row]] == identity) return row;
    return SIZE_MAX;
}

void SongTableSource::RestorePresentation(const PresentationState& state) {
    const std::unordered_set<std::wstring> selected(state.selected.begin(), state.selected.end());
    const auto identities = RowIdentities();
    m_selected.assign(m_all.size(), 0);
    for (size_t index = 0; index < identities.size(); ++index)
        if (selected.count(identities[index])) m_selected[index] = 1;
    m_focus = FindVisibleByIdentity(state.focus);
}

void SongTableSource::ApplySort() {
    const PresentationState presentation = CapturePresentation();
    SortOrder();
    RebuildVisible();
    RestorePresentation(presentation);
}

void SongTableSource::Append(std::vector<SongTableRow> rows) {
    if (rows.empty()) return;
    const size_t previousSize = m_all.size();
    const size_t focusIndex = m_focus < m_visible.size() ? m_visible[m_focus] : SIZE_MAX;
    const bool resolvesPreviousMissing = ResolvesUnresolvedRemoteAlbum(rows, m_unresolvedAlbumNames);
    const auto newlyResolvedMissing = RefreshAppendedRemoteAlbumKeys(rows, m_remoteAlbumIdsByName);
    const bool needsFullAlbumRefresh =
        resolvesPreviousMissing ||
        CreatesAmbiguousRemoteAlbum(rows, m_remoteAlbumIdsByName, m_resolvedMissingAlbumNames);
    if (!needsFullAlbumRefresh) {
        m_resolvedMissingAlbumNames.insert(newlyResolvedMissing.begin(), newlyResolvedMissing.end());
        AddKnownRemoteAlbums(rows, m_remoteAlbumIdsByName);
        AddUnresolvedRemoteAlbums(rows, m_unresolvedAlbumNames);
    }
    m_all.insert(m_all.end(), std::make_move_iterator(rows.begin()), std::make_move_iterator(rows.end()));
    m_selected.resize(m_all.size(), 0);

    bool reordered = false;
    if (needsFullAlbumRefresh) {
        // A missing album id can be resolved by an id that appears on a later page. This is
        // rare, so pay for a full normalization rather than split a logical album.
        RefreshRemoteAlbumKeys(m_all);
        RebuildUnresolvedRemoteAlbums(m_all, m_unresolvedAlbumNames);
        RebuildKnownRemoteAlbums(m_all, m_remoteAlbumIdsByName);
        RebuildResolvedMissingAlbums(m_all, m_remoteAlbumIdsByName, m_resolvedMissingAlbumNames);
        SortOrder();
        RebuildVisible();
        reordered = m_style == media::ViewStyle::Tiles || m_sortColumn < m_fields.size();
    } else {
        const size_t added = m_all.size() - previousSize;
        const bool sorted = m_style == media::ViewStyle::Tiles || m_sortColumn < m_fields.size();
        reordered = sorted;
        std::vector<size_t> appended(added);
        std::iota(appended.begin(), appended.end(), previousSize);
        if (!sorted) {
            m_order.insert(m_order.end(), appended.begin(), appended.end());
        } else {
            if (m_style == media::ViewStyle::Tiles) UpdateAlbumRanks(previousSize);
            if (m_sortColumn < m_fields.size()) {
                m_sortKeys.resize(m_all.size());
                for (size_t i = previousSize; i < m_all.size(); ++i) m_sortKeys[i] = SortKey(i);
            }
            std::stable_sort(appended.begin(), appended.end(),
                             [this](size_t a, size_t b) { return SortLess(a, b); });
            std::vector<size_t> merged;
            merged.reserve(m_order.size() + appended.size());
            std::merge(m_order.begin(), m_order.end(), appended.begin(), appended.end(),
                       std::back_inserter(merged),
                       [this](size_t a, size_t b) { return SortLess(a, b); });
            m_order = std::move(merged);
        }

        std::vector<size_t> newVisible;
        newVisible.reserve(appended.size());
        for (size_t index : appended)
            if (media::MatchesFilter(m_all[index].search, m_filter)) newVisible.push_back(index);
        if (!sorted) {
            m_visible.insert(m_visible.end(), newVisible.begin(), newVisible.end());
        } else {
            std::vector<size_t> merged;
            merged.reserve(m_visible.size() + newVisible.size());
            std::merge(m_visible.begin(), m_visible.end(), newVisible.begin(), newVisible.end(),
                       std::back_inserter(merged),
                       [this](size_t a, size_t b) { return SortLess(a, b); });
            m_visible = std::move(merged);
        }
        BuildGroups();
    }

    if (focusIndex != SIZE_MAX && reordered) {
        const auto found = std::find(m_visible.begin(), m_visible.end(), focusIndex);
        m_focus = found == m_visible.end() ? SIZE_MAX : static_cast<size_t>(found - m_visible.begin());
    }
}

bool SongTableSource::UpdateRows(size_t offset, std::vector<SongTableRow> rows) {
    if (rows.empty()) return true;
    if (offset > m_all.size() || rows.size() > m_all.size() - offset) return false;
    for (size_t i = 0; i < rows.size(); ++i) {
        const SongTableRow& current = m_all[offset + i];
        if (current.provider != rows[i].provider || current.trackId != rows[i].trackId) return false;
    }

    const bool fieldSort = m_sortColumn < m_fields.size();
    if (fieldSort && m_sortKeys.size() != m_all.size()) return false;
    if (m_order.size() != m_all.size()) return false;

    const PresentationState presentation = CapturePresentation();
    std::vector<std::wstring> previousSortKeys;
    if (fieldSort) {
        previousSortKeys.reserve(rows.size());
        for (size_t i = 0; i < rows.size(); ++i)
            previousSortKeys.push_back(m_sortKeys[offset + i]);
    }

    bool albumInputsChanged = false;
    bool searchChanged = false;
    bool displayChanged = false;
    for (size_t i = 0; i < rows.size(); ++i) {
        const SongTableRow& previous = m_all[offset + i];
        const SongTableRow& next = rows[i];
        albumInputsChanged = albumInputsChanged || previous.albumId != next.albumId ||
                             previous.album != next.album || previous.artist != next.artist;
        searchChanged = searchChanged || previous.search != next.search;
        displayChanged = displayChanged || previous.title != next.title || previous.artist != next.artist ||
                         previous.album != next.album || previous.albumId != next.albumId ||
                         previous.artistId != next.artistId || previous.length != next.length ||
                         previous.lengthMs != next.lengthMs || previous.source != next.source ||
                         previous.albumKey != next.albumKey || previous.artKey != next.artKey ||
                         previous.favorite != next.favorite || previous.playCount != next.playCount;
    }
    std::move(rows.begin(), rows.end(), m_all.begin() + offset);

    bool sortKeysChanged = false;
    if (fieldSort) {
        for (size_t i = 0; i < rows.size(); ++i) {
            std::wstring key = SortKey(offset + i);
            if (key != previousSortKeys[i]) {
                m_sortKeys[offset + i] = std::move(key);
                sortKeysChanged = true;
            }
        }
    }

    if (albumInputsChanged) {
        // This is the incremental equivalent of ReplaceRows' normalization. Only rows whose
        // relationship inputs changed trigger it; ordinary title/length patches do not.
        RefreshRemoteAlbumKeys(m_all);
        RebuildUnresolvedRemoteAlbums(m_all, m_unresolvedAlbumNames);
        RebuildKnownRemoteAlbums(m_all, m_remoteAlbumIdsByName);
        RebuildResolvedMissingAlbums(m_all, m_remoteAlbumIdsByName, m_resolvedMissingAlbumNames);
    }

    const bool orderChanged = sortKeysChanged || (m_style == media::ViewStyle::Tiles && albumInputsChanged);
    if (orderChanged) {
        if (m_style == media::ViewStyle::Tiles) {
            m_albumRank.clear();
            UpdateAlbumRanks(0);
        }
        std::stable_sort(m_order.begin(), m_order.end(),
                         [this](size_t a, size_t b) { return SortLess(a, b); });
    }

    if (orderChanged || (searchChanged && !m_filter.empty()))
        RebuildVisible();
    else if (displayChanged &&
             (m_style == media::ViewStyle::Tiles || (m_grouping && m_sortColumn < m_fields.size())))
        BuildGroups();
    RestorePresentation(presentation);
    return true;
}

void SongTableSource::ReplaceRows(std::vector<SongTableRow> rows) {
    const PresentationState presentation = CapturePresentation();
    m_all = std::move(rows);
    RefreshRemoteAlbumKeys(m_all);
    RebuildUnresolvedRemoteAlbums(m_all, m_unresolvedAlbumNames);
    RebuildKnownRemoteAlbums(m_all, m_remoteAlbumIdsByName);
    RebuildResolvedMissingAlbums(m_all, m_remoteAlbumIdsByName, m_resolvedMissingAlbumNames);
    if (m_style == media::ViewStyle::Tiles || m_sortColumn != SIZE_MAX)
        SortOrder();
    else {
        m_order.resize(m_all.size());
        for (size_t i = 0; i < m_order.size(); ++i) m_order[i] = i;
    }
    RebuildVisible();
    RestorePresentation(presentation);
}

size_t SongTableSource::RemoveRows(const std::function<bool(const SongTableRow&)>& remove) {
    const PresentationState presentation = CapturePresentation();
    const size_t before = m_all.size();
    m_all.erase(std::remove_if(m_all.begin(), m_all.end(), remove), m_all.end());
    if (m_all.size() == before) return 0;
    RefreshRemoteAlbumKeys(m_all);
    RebuildUnresolvedRemoteAlbums(m_all, m_unresolvedAlbumNames);
    RebuildKnownRemoteAlbums(m_all, m_remoteAlbumIdsByName);
    RebuildResolvedMissingAlbums(m_all, m_remoteAlbumIdsByName, m_resolvedMissingAlbumNames);
    SortOrder();
    RebuildVisible();
    RestorePresentation(presentation);
    return before - m_all.size();
}

void SongTableSource::RebuildVisible() {
    m_visible.clear();
    for (size_t index : m_order)
        if (media::MatchesFilter(m_all[index].search, m_filter)) m_visible.push_back(index);
    BuildGroups();
}

void SongTableSource::BuildGroups() {
    m_groups.clear();
    if (m_visible.empty()) return;
    const bool tiles = m_style == media::ViewStyle::Tiles;
    if (!tiles && (m_sortColumn >= m_fields.size() || !m_grouping)) return;

    for (size_t r = 0; r < m_visible.size(); ++r) {
        const SongTableRow& item = m_all[m_visible[r]];
        std::wstring key, header;
        if (tiles) {
            key = L"a:" + item.albumKey;
            header = AlbumLabel(item);
        } else {
            auto info = GroupInfo(m_visible[r]);
            if (info.second.empty()) {  // sorted by #: the row numbers are not a grouping
                m_groups.clear();
                return;
            }
            key = std::move(info.first);
            header = std::move(info.second);
        }
        if (!m_groups.empty() && m_groups.back().key == key) {
            ++m_groups.back().count;
            continue;
        }
        media::RowGroup group;
        group.key = std::move(key);
        group.first = r;
        group.count = 1;
        group.art = item.handle;
        group.header = std::move(header);
        group.lines[0] = group.header;
        if (tiles) {
            using media::Link;
            group.lines[1] = ArtistLabel(item);
            if (item.provider) {
                group.links[0] = Link::Remote(Link::Kind::RemoteAlbum, item.provider->Key(), item.albumId);
                group.links[1] = Link::Remote(Link::Kind::RemoteArtist, item.provider->Key(), item.artistId);
            } else {
                if (!item.album.empty()) group.links[0] = {Link::Kind::LocalAlbum, media::GroupKind::Album, item.album};
                // Local favorites: the first artist of the credit opens that artist.
                std::wstring first = item.artist.substr(0, item.artist.find(L"; "));
                if (!first.empty()) group.links[1] = {Link::Kind::LocalArtist, media::GroupKind::Artist, first};
            }
        }
        m_groups.push_back(std::move(group));
    }
}

std::wstring SongTableSource::Cell(size_t row, size_t column) const {
    if (row >= m_visible.size() || column >= m_fields.size()) return {};
    const SongTableRow& item = m_all[m_visible[row]];
    switch (m_fields[column]) {
    case kNumber: return std::to_wstring(row + 1);
    case kTitle: return item.title;
    case kLength: return item.length;
    case kArtist: return item.artist;
    case kAlbum: return item.album;
    case kPopularity: return std::to_wstring(std::clamp(item.popularity, 0, 100));  // drawn as a bar
    case kPlayCount: return std::to_wstring(item.playCount);
    case kSource: return item.source;
    case kLyric: return item.lyric;
    default: return {};  // the album, art and favorite columns are drawn by the list
    }
}

std::wstring SongTableSource::CellNote(size_t row, size_t column) const {
    if (row >= m_visible.size() || column >= m_fields.size() || m_fields[column] != kTitle) return {};
    return m_all[m_visible[row]].note;
}

std::vector<std::wstring> SongTableSource::CellBadges(size_t row, size_t column) const {
    if (row >= m_visible.size() || column >= m_fields.size() || m_fields[column] != kArtist) return {};
    return m_all[m_visible[row]].badges;
}

bool SongTableSource::IsPlaying(size_t row) const {
    metadb_handle_ptr now;
    return row < m_visible.size() && playback_control::get()->get_now_playing(now) && now == Item(row);
}

void SongTableSource::SelectOnly(size_t row) {
    std::fill(m_selected.begin(), m_selected.end(), uint8_t{0});
    if (row < m_visible.size()) m_selected[m_visible[row]] = 1;
    m_focus = row;
}

void SongTableSource::Toggle(size_t row) {
    if (row < m_visible.size()) m_selected[m_visible[row]] ^= 1;
    m_focus = row;
}

void SongTableSource::SelectRange(size_t a, size_t b, bool keepOthers) {
    if (!keepOthers) std::fill(m_selected.begin(), m_selected.end(), uint8_t{0});
    if (a > b) std::swap(a, b);
    for (size_t r = a; r <= b && r < m_visible.size(); ++r) m_selected[m_visible[r]] = 1;
}

void SongTableSource::SelectAll() {
    for (size_t index : m_visible) m_selected[index] = 1;
}

void SongTableSource::SetSelection(const std::vector<size_t>& rows) {
    std::fill(m_selected.begin(), m_selected.end(), uint8_t{0});
    for (size_t row : rows)
        if (row < m_visible.size()) m_selected[m_visible[row]] = 1;
}

metadb_handle_ptr SongTableSource::Item(size_t row) const {
    return row < m_visible.size() ? m_all[m_visible[row]].handle : metadb_handle_ptr{};
}

std::wstring SongTableSource::ArtKey(size_t row) const {
    return row < m_visible.size() ? m_all[m_visible[row]].artKey : std::wstring();
}

size_t SongTableSource::FindRow(const metadb_handle_ptr& track) const {
    if (!track.is_valid()) return SIZE_MAX;
    for (size_t r = 0; r < m_visible.size(); ++r)
        if (Item(r) == track) return r;
    return SIZE_MAX;
}

std::wstring SongTableSource::RowKey(size_t row) const {
    return row < m_visible.size() ? StableRowBase(m_all[m_visible[row]]) : std::wstring();
}

void SongTableSource::Activate(size_t row) {
    if (row >= m_visible.size()) return;
    if (m_activate) {
        m_activate(row, RowIdentity(row));
        return;
    }
    PlayFrom(row);
}

void SongTableSource::PlayFrom(size_t row) const {
    if (row >= m_visible.size()) return;
    // The search box only filters the view: every song plays, in display order, from this one.
    metadb_handle_list items;
    size_t start = 0;
    for (size_t index : m_order) {
        if (index == m_visible[row]) start = items.get_count();
        items.add_item(m_all[index].handle);
    }
    media::PlayItems(items, start);
}

void SongTableSource::PlayGroupFrom(size_t row) const {
    if (row >= m_visible.size()) return;
    for (const auto& group : m_groups) {
        if (row < group.first || row >= group.first + group.count) continue;
        metadb_handle_list items;
        for (size_t r = group.first; r < group.first + group.count; ++r) items.add_item(m_all[m_visible[r]].handle);
        media::PlayItems(items, row - group.first);
        return;
    }
    PlayFrom(row);
}

void SongTableSource::ContextMenu(HWND owner, POINT screen) {
    auto items = SelectedItems();
    if (items.get_count() == 0 && m_focus < m_visible.size()) items.add_item(Item(m_focus));
    if (items.get_count()) RunContextMenu(owner, items, screen);
}

metadb_handle_list SongTableSource::SelectedItems() const {
    metadb_handle_list items;
    for (size_t index : m_visible)
        if (m_selected[index]) items.add_item(m_all[index].handle);
    return items;
}

metadb_handle_list SongTableSource::VisibleItems() const {
    metadb_handle_list items;
    for (size_t index : m_visible) items.add_item(m_all[index].handle);
    return items;
}

void SongTableSource::SetFilter(std::wstring_view filterLower) {
    const PresentationState presentation = CapturePresentation();
    m_filter.assign(filterLower);
    RebuildVisible();
    RestorePresentation(presentation);
}

std::wstring SongTableSource::RowIdentity(size_t row) const {
    if (row >= m_visible.size()) return {};
    const auto identities = RowIdentities();
    return identities[m_visible[row]];
}

size_t SongTableSource::FindRowByIdentity(const std::wstring& identity) const {
    return FindVisibleByIdentity(identity);
}

// ---- concrete tables ----------------------------------------------------------------------

namespace {

// The part of a remote list's column key that names its kind of page.
const wchar_t* ListKindKey(stream::ListKind kind) {
    switch (kind) {
    case stream::ListKind::PlaylistTracks: return L"playlist";
    case stream::ListKind::Favorites: return L"favorites";
    case stream::ListKind::AlbumTracks: return L"album";
    case stream::ListKind::SectionTracks: return L"section";
    case stream::ListKind::ArtistTopTracks: return L"artist-top";
    case stream::ListKind::ArtistAlbums: return L"artist-albums";
    case stream::ListKind::SavedAlbums: return L"saved-albums";
    case stream::ListKind::Groups: return L"group";
    case stream::ListKind::CatalogSearch:
    case stream::ListKind::CatalogSearchTopResults: return L"search";
    default: return L"songs";
    }
}

}  // namespace

RemoteSongSource::RemoteSongSource(stream::Provider& provider, stream::ListKind kind,
                                   const std::vector<stream::Track>& tracks, std::wstring emptyText, size_t count)
    : SongTableSource(TrackRows(provider, tracks, 0, count), false, std::move(emptyText),
                      L"stream-" + std::wstring(provider.Key()) + L"-" + ListKindKey(kind)),
      m_provider(provider) {
    // NetEase shows its 热度 bars on album pages only; elsewhere they are a column to choose.
    if (provider.Can(stream::Capability::TrackPopularity))
        OfferPopularity(kind == stream::ListKind::AlbumTracks);
    if (provider.Can(stream::Capability::Download)) OfferDownloads();
}

void RemoteSongSource::AppendTracks(const std::vector<stream::Track>& tracks, size_t begin, size_t end) {
    begin = std::min(begin, tracks.size());
    end = std::min(end, tracks.size());
    if (end <= begin) return;
    Append(TrackRows(m_provider, tracks, begin, end - begin));
}

void RemoteSongSource::ReplaceTracks(const std::vector<stream::Track>& tracks, size_t count) {
    ReplaceRows(TrackRows(m_provider, tracks, 0, count));
}

bool RemoteSongSource::UpdateTrackRange(const std::vector<stream::Track>& tracks, size_t offset,
                                        size_t count) {
    if (offset > tracks.size() || count > tracks.size() - offset) return false;
    return UpdateRows(offset, TrackRows(m_provider, tracks, offset, count));
}

size_t RemoteSongSource::RemoveTracks(const std::vector<stream::Id>& ids) {
    if (ids.empty()) return 0;
    const std::unordered_set<stream::Id> removed(ids.begin(), ids.end());
    return RemoveRows([&removed](const SongTableRow& row) { return removed.count(row.trackId) != 0; });
}

AllFavoritesSource::AllFavoritesSource(const std::vector<metadb_handle_ptr>& local, const RemoteFavorites& remote)
    : SongTableSource(FavoriteRows(local, remote), true, L"No favorites.", L"all-favorites") {}

void AllFavoritesSource::ReplaceFavorites(const std::vector<metadb_handle_ptr>& local,
                                          const RemoteFavorites& remote) {
    ReplaceRows(FavoriteRows(local, remote));
}


namespace {
std::vector<SongTableRow> DiscRows(const metadb_handle_list& tracks) {
    std::vector<SongTableRow> rows;
    for (const auto& track : tracks) {
        if (!track.is_valid()) continue;
        SongTableRow row;
        row.handle = track;
        row.title = LocalField(track, "[%title%]");
        if (row.title.empty()) // not tagged (yet): its number, as on the disc
            row.title = Tr(L"Track") + L" " +
                        std::to_wstring(cd::TrackNumber(track->get_path(), track->get_subsong_index()));
        row.artist = LocalField(track, "[%artist%]");
        row.album = LocalField(track, "[%album%]");
        row.lengthMs = static_cast<int>(track->get_length() * 1000);
        // One album: the disc, whatever its tags say.
        row.albumKey = L"cd:" + Wide(cd::DiscKey(track->get_path()).c_str());
        row.artKey = row.albumKey;
        FinishRow(row);
        rows.push_back(std::move(row));
    }
    return rows;
}
} // namespace

DiscSource::DiscSource(const metadb_handle_list& tracks, std::wstring emptyText)
    : SongTableSource(DiscRows(tracks), false, std::move(emptyText), L"audio-cd") {}

PlayStatsSource::PlayStatsSource(const std::vector<media::PlayRecord>& records, std::wstring emptyText)
    : SongTableSource(PlayStatRows(records), true, std::move(emptyText), L"all-play-stats") {
    ShowPlayCount();
}

namespace {

std::vector<SongTableRow> SearchRows(const std::vector<SearchSongSource::Hit>& hits) {
    std::vector<SongTableRow> rows;
    rows.reserve(hits.size());
    for (const auto& hit : hits) {
        if (hit.provider) {
            SongTableRow row = TrackRow(*hit.provider, hit.track);
            row.lyric = hit.track.lyricMatch;
            row.lyricText = hit.track.lyricText;
            rows.push_back(std::move(row));
        } else if (hit.local.is_valid()) {
            rows.push_back(LocalRow(hit.local, L"search:"));
        }
    }
    return rows;
}

}  // namespace

SearchSongSource::SearchSongSource(const std::vector<Hit>& hits, bool showSource, bool lyrics, std::wstring emptyText)
    : SongTableSource(SearchRows(hits), showSource, std::move(emptyText),
                      std::wstring(L"search-songs") + (showSource ? L"-all" : L"") + (lyrics ? L"-lyrics" : L"")) {
    if (lyrics) ShowLyric();
    for (auto* provider : stream::Providers())
        if (provider->Can(stream::Capability::Download)) {
            OfferDownloads();
            break;
        }
}

void SearchSongSource::ReplaceHits(const std::vector<Hit>& hits) {
    ReplaceRows(SearchRows(hits));
}

std::wstring SearchSongSource::LyricsOf(const metadb_handle_ptr& item) const {
    for (const auto& row : AllRows())
        if (row.handle == item && !row.lyricText.empty()) return row.lyricText;
    return {};
}

}  // namespace glasswave::ui
