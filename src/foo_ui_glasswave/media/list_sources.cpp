#include "pch.h"
#include "media/list_sources.h"
#include "ui/text.h"

namespace glasswave::media {

namespace {

titleformat_object::ptr Compile(const char* pattern) {
    titleformat_object::ptr s;
    titleformat_compiler::get()->compile_safe(s, pattern);
    return s;
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

void PlayItems(const metadb_handle_list& items, size_t start) {
    if (items.get_count() == 0) return;
    auto pm = playlist_manager::get();
    const t_size p = pm->find_or_create_playlist("Now Playing");
    pm->playlist_undo_backup(p);
    pm->playlist_clear(p);
    pm->playlist_add_items(p, items, pfc::bit_array_false());
    pm->set_active_playlist(p);
    pm->set_playing_playlist(p);
    pm->playlist_set_focus_item(p, std::min<t_size>(start, items.get_count() - 1));
    pm->playlist_execute_default_action(p, std::min<t_size>(start, items.get_count() - 1));
}

// ---- PlaylistSource ---------------------------------------------------------------------

PlaylistSource::PlaylistSource(size_t playlist) : m_playlist(playlist) {
    m_columns = {{L"#", 34, false, true}, {L"Title", 0, true}, {L"Contributing artist", 170},
                 {L"Album", 170},         {L"Length", 56, false, true}};
    m_scripts = {nullptr, Compile("$if2(%title%,%filename%)"), Compile("[%artist%]"), Compile("[%album%]"),
                 Compile("[%length%]")};
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
        pm->playlist_item_format_title(m_playlist, i, nullptr, buf, script, nullptr, playback_control::display_level_none);
        if (MatchesFilter(Lower(ui::Wide(buf)), m_filter)) m_rows.push_back(i);
    }
}

void PlaylistSource::SetFilter(std::wstring_view filterLower) {
    m_filter = filterLower;
    Rebuild();
}

std::wstring PlaylistSource::Cell(size_t row, size_t column) const {
    const size_t item = m_rows[row];
    if (column == 0) return std::to_wstring(item + 1);
    pfc::string8 out;
    playlist_manager::get()->playlist_item_format_title(m_playlist, item, nullptr, out, m_scripts[column], nullptr,
                                                        playback_control::display_level_all);
    return ui::Wide(out);
}

std::wstring PlaylistSource::EmptyText() const {
    if (!m_filter.empty()) return L"No items match the search.";
    return L"This playlist is empty. Drag files here, or use Organize ▸ Add files.";
}

bool PlaylistSource::IsPlaying(size_t row) const {
    t_size p = 0, i = 0;
    return playlist_manager::get()->get_playing_item_location(&p, &i) && p == m_playlist && i == m_rows[row];
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
    pm->set_active_playlist(m_playlist);
    pm->playlist_execute_default_action(m_playlist, m_rows[row]);
}

void PlaylistSource::ContextMenu(HWND owner, POINT screen) {
    playlist_manager::get()->set_active_playlist(m_playlist);
    contextmenu_manager::win32_run_menu_context_playlist(owner, &screen);
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
    m_columns = {{L"Title", 0, true},  {L"Contributing artist", 170}, {L"Album", 170},
                 {L"#", 34, false, true}, {L"Length", 56, false, true}, {L"Year", 50, false, true}};
    m_selected.assign(m_all.size(), 0);
    SetFilter({});
}

std::wstring TrackSource::Cell(size_t row, size_t column) const {
    const Track& t = m_model.Tracks()[m_all[m_rows[row]]];
    switch (column) {
    case 0: return t.title;
    case 1: return t.artist;
    case 2: return t.album;
    case 3: return t.number;
    case 4: return t.length;
    default: return t.year;
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

void TrackSource::Activate(size_t row) {
    metadb_handle_list items;
    for (size_t r : m_rows) items.add_item(m_model.Tracks()[m_all[r]].handle);
    PlayItems(items, row);
}

void TrackSource::ContextMenu(HWND owner, POINT screen) {
    auto items = SelectedItems();
    if (items.get_count()) contextmenu_manager::win32_run_menu_context(owner, items, &screen);
}

metadb_handle_list TrackSource::SelectedItems() const {
    metadb_handle_list out;
    for (size_t r : m_rows)
        if (m_selected[r]) out.add_item(m_model.Tracks()[m_all[r]].handle);
    return out;
}

void TrackSource::SetFilter(std::wstring_view filterLower) {
    m_rows.clear();
    for (size_t i = 0; i < m_all.size(); ++i)
        if (filterLower.empty() || MatchesFilter(m_model.Tracks()[m_all[i]].search, filterLower)) m_rows.push_back(i);
    m_focus = SIZE_MAX;
}

// ---- GroupSource ------------------------------------------------------------------------

GroupSource::GroupSource(const LibraryModel& model, GroupKind kind, DrillDown onOpen)
    : m_model(model), m_kind(kind), m_onOpen(std::move(onOpen)) {
    switch (kind) {
    case GroupKind::Album:
        m_columns = {{L"Album", 0, true}, {L"Album artist", 200}, {L"Year", 50, false, true},
                     {L"Songs", 56, false, true}};
        break;
    case GroupKind::Artist:
        m_columns = {{L"Album artist", 0, true}, {L"Songs", 56, false, true}};
        break;
    default:
        m_columns = {{L"Genre", 0, true}, {L"Songs", 56, false, true}};
        break;
    }
    m_selected.assign(m_model.Groups(kind).size(), 0);
    SetFilter({});
}

std::wstring GroupSource::Cell(size_t row, size_t column) const {
    const Group& g = At(row);
    if (column == 0) return g.name;
    if (m_kind == GroupKind::Album) {
        if (column == 1) return g.secondary;
        if (column == 2) return g.year;
    }
    return std::to_wstring(g.tracks.size());
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

void GroupSource::Activate(size_t row) {
    if (m_onOpen) m_onOpen(At(row));
}

void GroupSource::ContextMenu(HWND owner, POINT screen) {
    auto items = SelectedItems();
    if (items.get_count()) contextmenu_manager::win32_run_menu_context(owner, items, &screen);
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
    m_rows.clear();
    const auto& groups = m_model.Groups(m_kind);
    for (size_t i = 0; i < groups.size(); ++i)
        if (filterLower.empty() || MatchesFilter(groups[i].search, filterLower)) m_rows.push_back(i);
    m_focus = SIZE_MAX;
}

}  // namespace glasswave::media
