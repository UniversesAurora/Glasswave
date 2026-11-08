#include "pch.h"
#include "media/list_sources.h"
#include "media/special_playlists.h"
#include "media/favorites.h"
#include "ui/win7_menu.h"
#include "media/source_identity.h"
#include "ui/text.h"

namespace glasswave::media {

namespace {

titleformat_object::ptr Compile(const char* pattern) {
    titleformat_object::ptr s;
    titleformat_compiler::get()->compile_safe(s, pattern);
    return s;
}

// Linguistic, case-insensitive, numbers by value ("2" < "10"): -1, 0, 1.
int CompareText(const std::wstring& a, const std::wstring& b) {
    return CompareStringEx(LOCALE_NAME_USER_DEFAULT, LINGUISTIC_IGNORECASE | SORT_DIGITSASNUMBERS, a.c_str(),
                           static_cast<int>(a.size()), b.c_str(), static_cast<int>(b.size()), nullptr, nullptr, 0) -
           2;
}

// Columns WMP 12 offers under "Choose columns" beyond the default ones (hidden by default).
std::vector<Column> ExtraColumns() {
    return {
        {L"Album artist", 150, false, false, true, "[%album artist%]"},
        {L"Composer", 150, false, false, true, "[%composer%]"},
        {L"Rating", 80, false, false, false, "[%rating%]"},  // drawn as WMP stars
        {L"Favorite", 58, false, false, false, "<favorite>"},  // toggles the Favorites playlist
        {L"Play count", 70, false, true, true, "[%play_count%]"},
        {L"Bit rate", 70, false, true, true, "[%bitrate% kbps]"},
        {L"Type", 60, false, false, true, "[%codec%]"},
        {L"Size", 70, false, true, true, "[%filesize_natural%]"},
        {L"Date added", 120, false, false, true, "[%added%]"},
        {L"File name", 200, false, false, true, "%filename_ext%"},
        {L"Folder", 260, false, false, true, "$directory_path(%path%)"},
    };
}

template <class IsSel, class SetSel>
void RangeSelect(size_t count, size_t a, size_t b, bool keepOthers, IsSel, SetSel set) {
    if (a > b) std::swap(a, b);
    for (size_t i = 0; i < count; ++i) {
        const bool in = i >= a && i <= b;
        if (in || !keepOthers) set(i, in);
    }
}

}  // namespace

void PlayItems(const metadb_handle_list& items, size_t start) { PlayQueue(items, start); }

// ---- PlaylistsSource --------------------------------------------------------------------

PlaylistsSource::PlaylistsSource(Open open, Menu menu) : m_open(std::move(open)), m_menu(std::move(menu)) {
    m_columns = {{L"Title", 0, true}, {L"Count", 70, false, true}};
    SetFilter({});
}

std::wstring PlaylistsSource::Cell(size_t row, size_t column) const {
    if (row >= m_rows.size()) return {};
    auto pm = playlist_manager::get();
    if (column == 1) return std::to_wstring(pm->playlist_get_item_count(m_rows[row]));
    pfc::string8 name;
    pm->playlist_get_name(m_rows[row], name);
    return ui::Wide(name);
}

TileInfo PlaylistsSource::Tile(size_t row) const {
    TileInfo info;
    if (row >= m_rows.size()) return info;
    const t_size p = m_rows[row];
    // WMP's 192 px icons: 29722 playlist, 29772 auto playlist, 29726 star (Favorites).
    info.icon = p == FavoritesPlaylist(false) ? 29726 : IsSmart(p) ? 29772 : 29722;
    const t_size n = playlist_manager::get()->playlist_get_item_count(p);
    info.lines = {Cell(row, 0), std::to_wstring(n) + (n == 1 ? L" song" : L" songs"), {}};
    return info;
}

void PlaylistsSource::SelectOnly(size_t row) {
    std::fill(m_selected.begin(), m_selected.end(), uint8_t{0});
    if (row < m_selected.size()) m_selected[row] = 1;
}

void PlaylistsSource::Toggle(size_t row) {
    if (row < m_selected.size()) m_selected[row] ^= 1;
}

void PlaylistsSource::SelectRange(size_t a, size_t b, bool keepOthers) {
    if (!keepOthers) std::fill(m_selected.begin(), m_selected.end(), uint8_t{0});
    if (a > b) std::swap(a, b);
    for (size_t r = a; r <= b && r < m_selected.size(); ++r) m_selected[r] = 1;
}

void PlaylistsSource::SelectAll() { std::fill(m_selected.begin(), m_selected.end(), uint8_t{1}); }

void PlaylistsSource::SetSelection(const std::vector<size_t>& rows) {
    std::fill(m_selected.begin(), m_selected.end(), uint8_t{0});
    for (size_t r : rows)
        if (r < m_selected.size()) m_selected[r] = 1;
}

void PlaylistsSource::Activate(size_t row) {
    if (row < m_rows.size() && m_open) m_open(m_rows[row]);
}

void PlaylistsSource::ContextMenu(HWND, POINT screen) {
    if (m_menu) m_menu(m_focus < m_rows.size() ? m_rows[m_focus] : SIZE_MAX, screen);
}

metadb_handle_list PlaylistsSource::SelectedItems() const {
    metadb_handle_list out;
    auto pm = playlist_manager::get();
    for (size_t r = 0; r < m_rows.size(); ++r)
        if (m_selected[r]) {
            metadb_handle_list items;
            pm->playlist_get_all_items(m_rows[r], items);
            out.add_items(items);
        }
    return out;
}

void PlaylistsSource::SetFilter(std::wstring_view filterLower) {
    auto pm = playlist_manager::get();
    m_rows.clear();
    pfc::string8 name;
    for (t_size p = 0, n = pm->get_playlist_count(); p < n; ++p) {
        if (IsQueue(p)) continue;  // the queue has its own place
        pm->playlist_get_name(p, name);
        if (!filterLower.empty() && Lower(ui::Wide(name)).find(filterLower) == std::wstring::npos) continue;
        m_rows.push_back(p);
    }
    m_selected.assign(m_rows.size(), 0);
    if (m_focus >= m_rows.size()) m_focus = SIZE_MAX;
}

// ---- PlaylistSource ---------------------------------------------------------------------

PlaylistSource::PlaylistSource(size_t playlist) : m_playlist(playlist) {
    m_columns = {{L"#", 34, false, true}, {L"Title", 0, true}, {L"Contributing artist", 170},
                 {L"Album", 170},         {L"Length", 56, false, true},
                 {L"Genre", 110, false, false, true, "[%genre%]"},
                 {L"Release year", 80, false, false, true, "[$year(%date%)]"}};
    m_scripts = {nullptr, Compile("$if2(%title%,%filename%)"), Compile("[%artist%]"), Compile("[%album%]"),
                 Compile("[%length%]"), Compile("[%genre%]"), Compile("[$year(%date%)]")};
    for (auto& extra : ExtraColumns()) {
        m_scripts.push_back(Compile(extra.format));
        m_columns.push_back(std::move(extra));
    }
    Rebuild();
}

void PlaylistSource::Rebuild() {
    auto pm = playlist_manager::get();
    const t_size n = m_playlist < pm->get_playlist_count() ? pm->playlist_get_item_count(m_playlist) : 0;
    m_rows.clear();
    if (m_filter.empty()) {
        m_rows.resize(n);
        for (size_t i = 0; i < n; ++i) m_rows[i] = i;
        return;
    }
    auto script = Compile("[%artist% ][%album artist% ][%album% ][%title% ][%genre%]");
    pfc::string8 buf;
    for (size_t i = 0; i < n; ++i) {
        std::wstring text;
        if (const RepairedText* r = CueCatalog::Get().Repair(pm->playlist_get_item_handle(m_playlist, i))) {
            text = r->artist + L' ' + r->albumArtist + L' ' + r->album + L' ' + r->title + L' ' + r->genre;
        } else {
            pm->playlist_item_format_title(m_playlist, i, nullptr, buf, script, nullptr,
                                           playback_control::display_level_none);
            text = ui::Wide(buf);
        }
        if (MatchesFilter(Lower(text), m_filter)) m_rows.push_back(i);
    }
}

void PlaylistSource::SetFilter(std::wstring_view filterLower) {
    m_filter = filterLower;
    Rebuild();
}

std::wstring PlaylistSource::Cell(size_t row, size_t column) const {
    const size_t item = m_rows[row];
    if (column == 0) return std::to_wstring(item + 1);
    if (column >= 1 && column <= 3) {  // title / artist / album from a mis-decoded cue sheet
        auto pm = playlist_manager::get();
        if (const RepairedText* r = CueCatalog::Get().Repair(pm->playlist_get_item_handle(m_playlist, item))) {
            const std::wstring& v = column == 1 ? r->title : column == 2 ? r->artist : r->album;
            if (!v.empty()) return v;
        }
    }
    pfc::string8 out;
    playlist_manager::get()->playlist_item_format_title(m_playlist, item, nullptr, out, m_scripts[column], nullptr,
                                                        playback_control::display_level_all);
    return ui::Wide(out);
}

void PlaylistSource::Sort(size_t column, bool descending) {
    auto pm = playlist_manager::get();
    const t_size n = pm->playlist_get_item_count(m_playlist);
    if (!CanSort(column)) {  // back to the order the playlist had before sorting
        if (m_sortColumn == SIZE_MAX) return;
        m_sortColumn = SIZE_MAX;
        metadb_handle_list current;
        pm->playlist_get_all_items(m_playlist, current);
        std::vector<bool> used(n, false);
        std::vector<t_size> order;
        order.reserve(n);
        for (t_size k = 0; k < m_unsorted.get_count(); ++k)
            for (t_size i = 0; i < n; ++i)
                if (!used[i] && current[i] == m_unsorted[k]) {
                    used[i] = true;
                    order.push_back(i);
                    break;
                }
        for (t_size i = 0; i < n; ++i)  // added since: keep them at the end
            if (!used[i]) order.push_back(i);
        m_unsorted.remove_all();
        pm->playlist_undo_backup(m_playlist);
        pm->playlist_reorder_items(m_playlist, order.data(), n);
        return;
    }
    if (m_sortColumn == SIZE_MAX) pm->playlist_get_all_items(m_playlist, m_unsorted);
    std::vector<std::wstring> keys(n);
    pfc::string8 buf;
    // Sort lengths by duration, everything else by its text.
    titleformat_object::ptr script = column == 4 ? Compile("%length_seconds_fp%") : m_scripts[column];
    for (t_size i = 0; i < n; ++i) {
        pm->playlist_item_format_title(m_playlist, i, nullptr, buf, script, nullptr, playback_control::display_level_none);
        keys[i] = ui::Wide(buf);
        if (column == 4) {  // zero-pad so the text compare orders numbers
            const double v = _wtof(keys[i].c_str());
            wchar_t pad[32];
            swprintf_s(pad, L"%012.3f", v);
            keys[i] = pad;
        }
    }
    std::vector<t_size> order(n);
    for (t_size i = 0; i < n; ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](t_size a, t_size b) {
        const int c = CompareText(keys[a], keys[b]);
        return descending ? c > 0 : c < 0;
    });
    m_sortColumn = column;
    m_sortDescending = descending;
    pm->playlist_undo_backup(m_playlist);
    pm->playlist_reorder_items(m_playlist, order.data(), n);
}

std::wstring PlaylistSource::EmptyText() const {
    if (!m_filter.empty()) return L"No items match the search.";
    return L"This playlist is empty. Drag files here, or use Organize ▸ Add files.";
}

bool PlaylistSource::IsPlaying(size_t row) const {
    // Playing from this playlist: its exact item. Otherwise (songs play from the queue, a
    // copy of the list the user started them in) the same song shows as playing here too.
    auto pm = playlist_manager::get();
    t_size p = 0, i = 0;
    if (pm->get_playing_item_location(&p, &i) && p == m_playlist) return i == m_rows[row];
    metadb_handle_ptr now;
    return playback_control::get()->get_now_playing(now) && now == pm->playlist_get_item_handle(m_playlist, m_rows[row]);
}

bool PlaylistSource::IsSelected(size_t row) const {
    return playlist_manager::get()->playlist_is_item_selected(m_playlist, m_rows[row]);
}

void PlaylistSource::SelectOnly(size_t row) {
    auto pm = playlist_manager::get();
    pm->playlist_set_selection(m_playlist, pfc::bit_array_true(), pfc::bit_array_one(m_rows[row]));
}

void PlaylistSource::Toggle(size_t row) {
    auto pm = playlist_manager::get();
    const size_t i = m_rows[row];
    pm->playlist_set_selection_single(m_playlist, i, !pm->playlist_is_item_selected(m_playlist, i));
}

void PlaylistSource::SelectRange(size_t a, size_t b, bool keepOthers) {
    auto pm = playlist_manager::get();
    const t_size n = pm->playlist_get_item_count(m_playlist);
    pfc::bit_array_bittable affected(n), state(n);
    if (a > b) std::swap(a, b);
    if (!keepOthers)
        for (t_size i = 0; i < n; ++i) affected.set(i, true);
    for (size_t r = a; r <= b && r < m_rows.size(); ++r) {
        affected.set(m_rows[r], true);
        state.set(m_rows[r], true);
    }
    pm->playlist_set_selection(m_playlist, affected, state);
}

void PlaylistSource::SetSelection(const std::vector<size_t>& rows) {
    auto pm = playlist_manager::get();
    const t_size n = pm->playlist_get_item_count(m_playlist);
    pfc::bit_array_bittable state(n);
    for (size_t r : rows)
        if (r < m_rows.size()) state.set(m_rows[r], true);
    pm->playlist_set_selection(m_playlist, pfc::bit_array_true(), state);
}

metadb_handle_ptr PlaylistSource::Item(size_t row) const {
    return row < m_rows.size() ? playlist_manager::get()->playlist_get_item_handle(m_playlist, m_rows[row]) : metadb_handle_ptr{};
}

size_t PlaylistSource::FindRow(const metadb_handle_ptr& track) const {
    auto pm = playlist_manager::get();
    for (size_t r = 0; r < m_rows.size(); ++r)
        if (pm->playlist_get_item_handle(m_playlist, m_rows[r]) == track) return r;
    return SIZE_MAX;
}

void PlaylistSource::SelectAll() {
    auto pm = playlist_manager::get();
    const t_size n = pm->playlist_get_item_count(m_playlist);
    pfc::bit_array_bittable affected(n), state(n);
    for (size_t i = 0; i < n; ++i) affected.set(i, true);
    for (size_t r : m_rows) state.set(r, true);
    pm->playlist_set_selection(m_playlist, affected, state);
}

size_t PlaylistSource::Focus() const {
    const t_size f = playlist_manager::get()->playlist_get_focus_item(m_playlist);
    if (f == SIZE_MAX) return SIZE_MAX;
    auto it = std::lower_bound(m_rows.begin(), m_rows.end(), f);
    return (it != m_rows.end() && *it == f) ? static_cast<size_t>(it - m_rows.begin()) : SIZE_MAX;
}

void PlaylistSource::SetFocus(size_t row) {
    if (row < m_rows.size()) playlist_manager::get()->playlist_set_focus_item(m_playlist, m_rows[row]);
}

void PlaylistSource::Activate(size_t row) {
    auto pm = playlist_manager::get();
    if (IsQueue(m_playlist)) {  // in the queue itself: just play that song
        pm->set_playing_playlist(m_playlist);
        pm->playlist_execute_default_action(m_playlist, m_rows[row]);
        return;
    }
    // Any other playlist: the queue becomes this list (as shown) and plays from the song.
    metadb_handle_list items;
    for (size_t r : m_rows) items.add_item(pm->playlist_get_item_handle(m_playlist, r));
    PlayQueue(items, row);
}

void PlaylistSource::ContextMenu(HWND owner, POINT screen) {
    playlist_manager::get()->set_active_playlist(m_playlist);
    ui::RunPlaylistContextMenu(owner, screen);
}

bool PlaylistSource::DeleteSelected() {
    auto pm = playlist_manager::get();
    pm->playlist_undo_backup(m_playlist);
    pm->playlist_remove_selection(m_playlist);
    return true;
}

metadb_handle_list PlaylistSource::SelectedItems() const {
    metadb_handle_list out;
    playlist_manager::get()->playlist_get_selected_items(m_playlist, out);
    return out;
}

// ---- TrackSource ------------------------------------------------------------------------

TrackSource::TrackSource(const LibraryModel& model, std::vector<size_t> tracks, std::wstring emptyText)
    : m_model(model), m_all(std::move(tracks)), m_emptyText(std::move(emptyText)) {
    SetViewStyle(ViewStyle::Tiles);
    // Album by album in "All music" order; tracks keep their library order inside an album.
    std::stable_sort(m_all.begin(), m_all.end(), [&](size_t a, size_t b) {
        return m_model.AlbumRank(m_model.Tracks()[a].albumIndex) < m_model.AlbumRank(m_model.Tracks()[b].albumIndex);
    });
    m_libraryOrder = m_all;
    m_selected.assign(m_all.size(), 0);
    SetFilter({});
}

namespace {
enum Field { kAlbumColumn, kNumber, kTitle, kLength, kArtist, kAlbum, kGenre, kYear, kFormat };
}

void TrackSource::SetViewStyle(ViewStyle style) {
    m_style = style == ViewStyle::Icons ? ViewStyle::Tiles : style;
    // WMP 12 music views: "Expanded tile" has the album column (cover and album details),
    // "Details" lists everything in columns under the album headers.
    if (m_style == ViewStyle::Tiles) {
        m_columns = {{L"Album", 200},
                     {L"#", 34, false, true},
                     {L"Title", 0, true},
                     {L"Length", 56, false, true},
                     {L"Contributing artist", 190},
                     {L"Genre", 110},
                     {L"Release year", 80, false, false, true}};
        m_fields = {kAlbumColumn, kNumber, kTitle, kLength, kArtist, kGenre, kYear};
    } else {
        m_columns = {{L"#", 34, false, true}, {L"Title", 0, true},  {L"Length", 56, false, true},
                     {L"Contributing artist", 170}, {L"Album", 170}, {L"Genre", 110}, {L"Release year", 80}};
        m_fields = {kNumber, kTitle, kLength, kArtist, kAlbum, kGenre, kYear};
    }
    m_scripts.assign(m_columns.size(), nullptr);
    for (auto& extra : ExtraColumns()) {
        m_scripts.push_back(Compile(extra.format));
        m_fields.push_back(kFormat);
        m_columns.push_back(std::move(extra));
    }
    m_sortColumn = SIZE_MAX;  // columns changed: back to the library order
    if (!m_libraryOrder.empty()) ApplySort();
}

bool TrackSource::CanSort(size_t column) const { return column < m_fields.size(); }

void TrackSource::Sort(size_t column, bool descending) {
    m_sortColumn = CanSort(column) ? column : SIZE_MAX;
    m_sortDescending = descending;
    ApplySort();
}

void TrackSource::ApplySort() {
    m_all = m_libraryOrder;
    const auto& tracks = m_model.Tracks();
    const auto& albums = m_model.Groups(GroupKind::Album);
    if (m_sortColumn != SIZE_MAX) {
        const int field = m_fields[m_sortColumn];
        const bool byAlbum = field == kAlbumColumn || field == kAlbum;
        auto albumName = [&](size_t t) {
            const size_t a = tracks[t].albumIndex;
            return a < albums.size() ? albums[a].name : tracks[t].album;
        };
        // Track columns sort inside each album; the album columns order the albums.
        std::vector<std::wstring> keys(tracks.size());
        if (!byAlbum) {
            pfc::string8 buf;
            for (size_t t : m_all) {
                const Track& tr = tracks[t];
                switch (field) {
                case kNumber: keys[t] = tr.number; break;
                case kTitle: keys[t] = tr.title; break;
                case kLength: {
                    wchar_t pad[32];
                    swprintf_s(pad, L"%012.3f", tr.seconds);
                    keys[t] = pad;
                    break;
                }
                case kArtist: keys[t] = tr.artist; break;
                case kGenre: keys[t] = tr.genre; break;
                case kYear: keys[t] = tr.year; break;
                default:
                    tr.handle->format_title(nullptr, buf, m_scripts[m_sortColumn], nullptr);
                    keys[t] = ui::Wide(buf);
                    break;
                }
            }
        }
        const bool desc = m_sortDescending;
        std::stable_sort(m_all.begin(), m_all.end(), [&](size_t a, size_t b) {
            if (byAlbum) {
                const int c = CompareText(albumName(a), albumName(b));
                if (c) return desc ? c > 0 : c < 0;
                return tracks[a].albumIndex < tracks[b].albumIndex;  // same title: keep albums apart
            }
            const size_t ra = m_model.AlbumRank(tracks[a].albumIndex), rb = m_model.AlbumRank(tracks[b].albumIndex);
            if (ra != rb) return ra < rb;
            const int c = CompareText(keys[a], keys[b]);
            return desc ? c > 0 : c < 0;
        });
    }
    m_selected.assign(m_all.size(), 0);
    SetFilter(m_filter);
}

std::wstring TrackSource::Cell(size_t row, size_t column) const {
    const Track& t = m_model.Tracks()[m_all[m_rows[row]]];
    switch (column < m_fields.size() ? m_fields[column] : -1) {
    case kNumber: return t.number;
    case kTitle: return t.title;
    case kLength: return t.length;
    case kArtist: return t.artist;
    case kAlbum: return t.album;
    case kGenre: return t.genre;
    case kYear: return t.year;
    case kFormat: {
        pfc::string8 out;
        if (column < m_scripts.size() && m_scripts[column].is_valid())
            t.handle->format_title(nullptr, out, m_scripts[column], nullptr);
        return ui::Wide(out);
    }
    default: return {};  // the album column is drawn by the list
    }
}

bool TrackSource::IsPlaying(size_t row) const {
    metadb_handle_ptr now;
    return playback_control::get()->get_now_playing(now) && now == m_model.Tracks()[m_all[m_rows[row]]].handle;
}

void TrackSource::SelectOnly(size_t row) {
    std::fill(m_selected.begin(), m_selected.end(), uint8_t{0});
    m_selected[m_rows[row]] = 1;
}

void TrackSource::Toggle(size_t row) { m_selected[m_rows[row]] ^= 1; }

void TrackSource::SelectRange(size_t a, size_t b, bool keepOthers) {
    if (!keepOthers) std::fill(m_selected.begin(), m_selected.end(), uint8_t{0});
    if (a > b) std::swap(a, b);
    for (size_t r = a; r <= b && r < m_rows.size(); ++r) m_selected[m_rows[r]] = 1;
}

void TrackSource::SelectAll() {
    for (size_t r : m_rows) m_selected[r] = 1;
}

metadb_handle_ptr TrackSource::Item(size_t row) const {
    return row < m_rows.size() ? m_model.Tracks()[m_all[m_rows[row]]].handle : metadb_handle_ptr{};
}

size_t TrackSource::FindRow(const metadb_handle_ptr& track) const {
    const auto& tracks = m_model.Tracks();
    for (size_t r = 0; r < m_rows.size(); ++r)
        if (tracks[m_all[m_rows[r]]].handle == track) return r;
    return SIZE_MAX;
}

void TrackSource::SetSelection(const std::vector<size_t>& rows) {
    std::fill(m_selected.begin(), m_selected.end(), uint8_t{0});
    for (size_t r : rows)
        if (r < m_rows.size()) m_selected[m_rows[r]] = 1;
}

void TrackSource::Activate(size_t row) {
    metadb_handle_list items;
    for (size_t r : m_rows) items.add_item(m_model.Tracks()[m_all[r]].handle);
    PlayItems(items, row);
}

void TrackSource::ContextMenu(HWND owner, POINT screen) {
    auto items = SelectedItems();
    if (items.get_count()) ui::RunContextMenu(owner, items, screen);
}

metadb_handle_list TrackSource::SelectedItems() const {
    metadb_handle_list out;
    for (size_t r : m_rows)
        if (m_selected[r]) out.add_item(m_model.Tracks()[m_all[r]].handle);
    return out;
}

void TrackSource::SetFilter(std::wstring_view filterLower) {
    m_filter = filterLower;
    m_rows.clear();
    for (size_t i = 0; i < m_all.size(); ++i)
        if (filterLower.empty() || MatchesFilter(m_model.Tracks()[m_all[i]].search, filterLower)) m_rows.push_back(i);
    m_focus = SIZE_MAX;

    m_groups.clear();
    const auto& albums = m_model.Groups(GroupKind::Album);
    for (size_t r = 0; r < m_rows.size(); ++r) {
        const Track& t = m_model.Tracks()[m_all[m_rows[r]]];
        if (!m_groups.empty() && m_model.Tracks()[m_all[m_rows[m_groups.back().first]]].albumIndex == t.albumIndex) {
            ++m_groups.back().count;
            continue;
        }
        RowGroup g;
        g.first = r;
        g.count = 1;
        g.art = t.handle;
        const Group* album = t.albumIndex < albums.size() ? &albums[t.albumIndex] : nullptr;
        g.header = album ? album->secondary : t.albumArtist;
        g.lines = {t.album, album ? album->secondary : t.albumArtist, t.genre, album ? album->year : t.year};
        g.linkKeys = {album ? album->key : std::wstring(), t.hasAlbumArtist ? t.albumArtist : std::wstring(),
                      t.genre, {}};
        m_groups.push_back(std::move(g));
    }
}

// ---- GroupSource ------------------------------------------------------------------------

GroupSource::GroupSource(const LibraryModel& model, GroupKind kind, DrillDown onOpen)
    : m_model(model), m_kind(kind), m_onOpen(std::move(onOpen)) {
    switch (kind) {
    case GroupKind::Album:
        m_columns = {{L"Album", 0, true}, {L"Album artist", 180}, {L"Genre", 110}, {L"Release year", 90},
                     {L"Count", 60, false, true}, {L"Length", 70, false, true}};
        m_style = ViewStyle::Icons;
        break;
    case GroupKind::Artist:
        m_columns = {{L"Album artist", 0, true}, {L"Count", 60, false, true}, {L"Length", 70, false, true}};
        m_style = ViewStyle::Tiles;  // WMP 12 opens artists as expanded tiles
        break;
    default:
        m_columns = {{L"Genre", 0, true}, {L"Count", 60, false, true}, {L"Length", 70, false, true}};
        m_style = ViewStyle::Tiles;
        break;
    }
    m_selected.assign(m_model.Groups(kind).size(), 0);
    SetFilter({});
}

std::wstring GroupSource::Cell(size_t row, size_t column) const {
    const Group& g = At(row);
    if (column == 0) return g.name;
    const size_t tail = m_columns.size() - column;  // Count and Length are the last two columns
    if (tail == 2) return std::to_wstring(g.tracks.size());
    if (tail == 1) return ui::FormatTime(g.seconds);
    // Album: artist, genre, year
    if (column == 1) return g.secondary;
    if (column == 2) return g.tracks.empty() ? std::wstring() : m_model.Tracks()[g.tracks.front()].genre;
    return g.year;
}

void GroupSource::Sort(size_t column, bool descending) {
    if (!CanSort(column)) {  // default order
        m_sortColumn = SIZE_MAX;
        SetFilter(m_filter);
        return;
    }
    m_sortColumn = column;
    m_sortDescending = descending;
    const auto& groups = m_model.Groups(m_kind);
    const size_t tail = m_columns.size() - column;  // Count and Length are the last two columns
    std::vector<std::wstring> keys(groups.size());
    for (size_t r = 0; r < m_rows.size(); ++r) {
        const Group& g = groups[m_rows[r]];
        wchar_t pad[32];
        if (tail <= 2) {
            swprintf_s(pad, L"%012.3f", tail == 2 ? static_cast<double>(g.tracks.size()) : g.seconds);
            keys[m_rows[r]] = pad;
        } else {
            keys[m_rows[r]] = Cell(r, column);
        }
    }
    std::stable_sort(m_rows.begin(), m_rows.end(), [&](size_t a, size_t b) {
        const int c = CompareText(keys[a], keys[b]);
        return descending ? c > 0 : c < 0;
    });
}

TileInfo GroupSource::Tile(size_t row) const {
    const Group& g = At(row);
    TileInfo info;
    if (!g.tracks.empty()) info.art = m_model.Tracks()[g.tracks.front()].handle;
    const auto songs = g.tracks.size();
    const std::wstring count = std::to_wstring(songs) + (songs == 1 ? L" song" : L" songs");
    const long minutes = std::lround(g.seconds / 60.0);
    const std::wstring length = std::to_wstring(minutes) + (minutes == 1 ? L" minute" : L" minutes");
    if (m_style == ViewStyle::Icons) {  // cover above: two lines
        info.lines = {g.name, m_kind == GroupKind::Album ? g.secondary : count, {}};
    } else if (m_kind == GroupKind::Album) {  // cover beside: three lines
        info.lines = {g.name, g.secondary, g.year};
    } else {
        info.lines = {g.name, count, length};
    }
    return info;
}

std::wstring GroupSource::EmptyText() const {
    if (!m_model.Enabled() || m_model.Tracks().empty())
        return L"Your library is empty. Use Organize ▸ Manage music library to add your music folders.";
    return L"No items match the search.";
}

void GroupSource::SelectOnly(size_t row) {
    std::fill(m_selected.begin(), m_selected.end(), uint8_t{0});
    m_selected[m_rows[row]] = 1;
}

void GroupSource::Toggle(size_t row) { m_selected[m_rows[row]] ^= 1; }

void GroupSource::SelectRange(size_t a, size_t b, bool keepOthers) {
    if (!keepOthers) std::fill(m_selected.begin(), m_selected.end(), uint8_t{0});
    if (a > b) std::swap(a, b);
    for (size_t r = a; r <= b && r < m_rows.size(); ++r) m_selected[m_rows[r]] = 1;
}

void GroupSource::SelectAll() {
    for (size_t r : m_rows) m_selected[r] = 1;
}

void GroupSource::SetSelection(const std::vector<size_t>& rows) {
    std::fill(m_selected.begin(), m_selected.end(), uint8_t{0});
    for (size_t r : rows)
        if (r < m_rows.size()) m_selected[m_rows[r]] = 1;
}

void GroupSource::Activate(size_t row) {
    if (m_onOpen) m_onOpen(At(row));
}

void GroupSource::ContextMenu(HWND owner, POINT screen) {
    auto items = SelectedItems();
    if (items.get_count()) ui::RunContextMenu(owner, items, screen);
}

metadb_handle_list GroupSource::SelectedItems() const {
    metadb_handle_list out;
    const auto& groups = m_model.Groups(m_kind);
    for (size_t r : m_rows)
        if (m_selected[r])
            for (size_t t : groups[r].tracks) out.add_item(m_model.Tracks()[t].handle);
    return out;
}

void GroupSource::SetFilter(std::wstring_view filterLower) {
    m_filter = filterLower;
    m_rows.clear();
    const auto& groups = m_model.Groups(m_kind);
    for (size_t i = 0; i < groups.size(); ++i)
        if (filterLower.empty() || MatchesFilter(groups[i].search, filterLower)) m_rows.push_back(i);
    m_focus = SIZE_MAX;
    if (m_sortColumn != SIZE_MAX) Sort(m_sortColumn, m_sortDescending);
}

}  // namespace glasswave::media
