#include "pch.h"
#include "ui/main_window_parts.h"

// Navigation: the breadcrumb and its menus, the pages' history and last view, the
// navigation pane's items, organizing and playlist menus.
namespace glasswave::ui {

using namespace mainwin;

namespace {

constexpr size_t kMenuItemLimit = 300; // breadcrumb menus list at most this many items

// The page and top row shown when the window closed.
// {6A1F3C65-2B7E-4D90-8C15-3E9A7B2D4F61}
cfg_var_modern::cfg_string cfg_lastView(
    {0x6a1f3c65, 0x2b7e, 0x4d90, {0x8c, 0x15, 0x3e, 0x9a, 0x7b, 0x2d, 0x4f, 0x61}}, "");

std::wstring CleanConfigField(std::wstring text) {
    for (auto& c : text)
        if (c == L'\t' || c == L'\r' || c == L'\n') c = L' ';
    return text;
}

std::vector<std::wstring> SplitConfigFields(const std::wstring& text) {
    std::vector<std::wstring> fields;
    size_t start = 0;
    while (true) {
        const size_t tab = text.find(L'\t', start);
        fields.push_back(text.substr(start, tab == std::wstring::npos ? std::wstring::npos : tab - start));
        if (tab == std::wstring::npos) break;
        start = tab + 1;
    }
    return fields;
}

// A section of the navigation pane (or a page it lists directly): it always stands in its own
// place, whatever page links to it.
bool SectionPage(const View& view) {
    switch (view.kind) {
    case View::Kind::Home:
    case View::Kind::AllMusic:
    case View::Kind::Playlists:
    case View::Kind::SmartPlaylists:
    case View::Kind::AllFavorites:
    case View::Kind::AllRecentPlayed:
    case View::Kind::AllMostPlayed:
    case View::Kind::Downloads:
    case View::Kind::Category:
    case View::Kind::AudioCd:
    case View::Kind::RemoteFavorites:
    case View::Kind::RemoteShelf:
    case View::Kind::RemoteArtists:
    case View::Kind::RemoteAlbums:
    case View::Kind::RemoteSectionTracks:
    case View::Kind::RemoteGroups:
        return true;
    case View::Kind::Playlist:
        return media::IsQueue(view.playlist);
    default:
        return false;
    }
}

} // namespace

void MainWindow::RepaintBreadcrumb() {
    auto path = BuildCrumbPath(m_view);
    // The path in the log as it changes, so every page's place can be read back.
    std::wstring text = L"path:";
    for (const auto& level : path)
        if (!level.text.empty()) text += L" > " + (level.literal ? level.text : Tr(level.text));
    if (text != m_loggedPath) {
        m_loggedPath = text;
        core::Log(text);
    }
    m_crumbBar->SetPath(std::move(path));
    UpdateLayout();
    // The view options glyph follows the current source, which asynchronous loads replace.
    if (auto* w = WidgetById(kViewOptions)) w->Invalidate();
}

std::vector<MenuAction> MainWindow::LibraryMenu() {
    auto go = [this](View v) { return [this, v] { Navigate(v, true); }; };
    View all;
    all.library = View::Library::All;
    all.kind = View::Kind::AllMusic;
    View local;
    local.library = View::Library::Local;
    local.kind = View::Kind::AllMusic;
    // The navigation pane's top level, in its order (NavPane::Rebuild): the root of every path.
    std::vector<MenuAction> out;
    if (std::any_of(stream::Providers().begin(), stream::Providers().end(),
                    [](const stream::Provider* p) { return p->Can(stream::Capability::Home); })) {
        View home;
        home.library = View::Library::All;
        home.kind = View::Kind::Home;
        out.push_back({L"Home", go(home)});
    }
    if (const t_size queue = media::QueuePlaylist(false); queue != SIZE_MAX) {
        View now;
        now.library = View::Library::All;
        now.kind = View::Kind::Playlist;
        now.playlist = queue;
        out.push_back({L"Now Playing", go(now)});
    }
    out.push_back({L"All Music", go(all)});
    out.push_back({L"Local Music", go(local)});
    for (auto* provider : stream::Providers())
        out.push_back({provider->Name(), go(ProviderLandingView(*provider))});
    return out;
}

std::vector<MenuAction> MainWindow::AllSectionsMenu() {
    auto go = [this](View v) { return [this, v] { Navigate(v, true); }; };
    View favorites;
    favorites.library = View::Library::All;
    favorites.kind = View::Kind::AllFavorites;
    View recent;
    recent.library = View::Library::All;
    recent.kind = View::Kind::AllRecentPlayed;
    View most;
    most.library = View::Library::All;
    most.kind = View::Kind::AllMostPlayed;
    View downloads;
    downloads.library = View::Library::All;
    downloads.kind = View::Kind::Downloads;
    View playlists;
    playlists.library = View::Library::All;
    playlists.kind = View::Kind::Playlists;
    return {{L"Favorites", go(favorites)},
            {L"Recently Played", go(recent)},
            {L"Most Played", go(most)},
            {L"Downloads", go(downloads)},
            {L"Playlists", go(playlists)}};
}

// A local album's or (not the queue's) playlist's page: Play on the command bar.
bool MainWindow::LocalPlayShown() const {
    if (m_page || !m_source) return false;
    return (m_view.kind == View::Kind::Group && m_view.group == media::GroupKind::Album) ||
           (m_view.kind == View::Kind::Playlist && !media::IsQueue(m_view.playlist));
}

std::vector<MenuAction> MainWindow::LocalSectionsMenu() {
    auto go = [this](View v) { return [this, v] { Navigate(v, true); }; };
    const auto kinds = m_nav->Categories();
    const int count = static_cast<int>(kinds.size());
    const int at = std::clamp(m_nav->SmartPosition(), 0, count);
    std::vector<MenuAction> out;
    for (int i = 0; i <= count; ++i) {
        if (i == at) { // the same interleaving the navigation pane shows
            View smart;
            smart.library = View::Library::Local;
            smart.kind = View::Kind::SmartPlaylists;
            out.push_back({L"Smart playlists", go(smart)});
        }
        if (i < count) {
            View v;
            v.library = View::Library::Local;
            v.kind = View::Kind::Category;
            v.group = kinds[static_cast<size_t>(i)];
            out.push_back({CategoryLabel(v.group), go(v)});
        }
    }
    return out;
}

MenuAction MainWindow::SearchProviderAction(stream::Provider& provider) {
    return {provider.NamedText(L"Search {0}"),
            [this, key = std::wstring(provider.Key())] { BeginSearch({SearchScope::Kind::Provider, key}); }};
}

std::vector<MenuAction> MainWindow::ProviderSectionsMenu(stream::Provider& provider) {
    auto go = [this](View v) { return [this, v] { Navigate(v, true); }; };
    std::vector<MenuAction> out;
    for (const auto& section : provider.Sections())
        if (!section.id.empty()) out.push_back({Tr(section.label), go(SectionView(provider, section))});
    if (provider.Can(stream::Capability::CatalogSearch)) out.push_back(SearchProviderAction(provider));
    return out;
}

std::vector<MenuAction> MainWindow::ShelfMenu(stream::Provider& provider,
                                              const stream::NavigationSection& section) {
    auto go = [this](View v) { return [this, v] { Navigate(v, true); }; };
    std::vector<MenuAction> out;
    for (const auto& playlist : provider.SectionPlaylists(section.id)) {
        if (out.size() >= kMenuItemLimit) break;
        out.push_back({playlist.name.empty() ? provider.Text(L"Unnamed playlist") : playlist.name,
                       go(PlaylistView(provider, playlist.id, playlist.name, section.shelf, section.id))});
    }
    if (out.empty()) out.push_back({Tr(section.label), go(SectionView(provider, section))});
    return out;
}

std::vector<MenuAction> MainWindow::FollowedArtistsMenu(stream::Provider& provider,
                                                        const stream::Id& sectionId) {
    auto go = [this](View v) { return [this, v] { Navigate(v, true); }; };
    std::vector<MenuAction> out;
    for (const auto& artist : provider.FollowedArtists()) {
        if (out.size() >= kMenuItemLimit) break;
        out.push_back({artist.name, go(ArtistView(provider, artist.id, artist.name, sectionId))});
    }
    if (out.empty()) {
        const auto section = FindProviderSection(provider, sectionId);
        if (section) out.push_back({Tr(section->label), go(SectionView(provider, *section))});
    }
    return out;
}

std::vector<MenuAction> MainWindow::SavedAlbumsMenu(stream::Provider& provider, const stream::Id& sectionId) {
    auto go = [this](View v) { return [this, v] { Navigate(v, true); }; };
    std::vector<MenuAction> out;
    for (const auto& album : provider.SavedAlbums()) {
        if (out.size() >= kMenuItemLimit) break;
        out.push_back({album.name, go(AlbumView(provider, album.id, album.name, sectionId))});
    }
    if (out.empty()) {
        const auto section = FindProviderSection(provider, sectionId);
        if (section) out.push_back({Tr(section->label), go(SectionView(provider, *section))});
    }
    return out;
}

// A provider's levels: the library, then its section and the page's subject.
void MainWindow::AddRemoteCrumbs(std::vector<Crumb>& path, stream::Provider& provider, const View& view) {
    auto go = [this](View v) { return [this, v] { Navigate(v, true); }; };
    stream::Provider* p = &provider;
    // A user's name is shown as is; an unnamed page gets a translated fallback.
    auto named = [&path](const std::wstring& name, const wchar_t* fallback, std::function<void()> open) {
        path.push_back(name.empty() ? Crumb{fallback, std::move(open), {}}
                                    : Crumb{name, std::move(open), {}, true});
    };
    auto addSection = [&](const stream::NavigationSection& section) {
        std::function<std::vector<MenuAction>()> children;
        if (section.kind == stream::SectionKind::PlaylistShelf)
            children = [this, p, section] { return ShelfMenu(*p, section); };
        else if (section.kind == stream::SectionKind::Artists)
            children = [this, p, id = section.id] { return FollowedArtistsMenu(*p, id); };
        else if (section.kind == stream::SectionKind::Albums)
            children = [this, p, id = section.id] { return SavedAlbumsMenu(*p, id); };
        path.push_back({section.label, go(SectionView(provider, section)), std::move(children)});
    };
    const auto section = SectionForView(provider, view);
    path.push_back(
        {provider.Name(), go(ProviderLandingView(provider)), [this, p] { return ProviderSectionsMenu(*p); }});
    switch (view.kind) {
    case View::Kind::RemoteFavorites:
    case View::Kind::RemoteSectionTracks:
    case View::Kind::RemoteGroups:
        if (section) addSection(*section);
        break;
    case View::Kind::RemoteGroupTracks:
        if (section) addSection(*section);
        named(view.name, L"Group", go(view));
        break;
    case View::Kind::RemoteShelf:
        if (section) addSection(*section);
        break;
    case View::Kind::RemotePlaylist:
        if (section) addSection(*section);
        named(view.name, L"Playlist", go(view));
        break;
    case View::Kind::RemoteArtists:
        if (section) addSection(*section);
        break;
    case View::Kind::RemoteAlbums:
        if (section) addSection(*section);
        break;
    case View::Kind::RemoteArtist:
        if (section) addSection(*section);
        named(view.name, L"Artist", go(view));
        break;
    case View::Kind::RemoteAlbum:
        if (section) addSection(*section);
        named(view.name, L"Album", go(view));
        break;
    case View::Kind::RemoteTopTracks:
        if (section) addSection(*section);
        named(view.name, L"Artist",
              go(ArtistView(provider, view.remoteId, view.name, view.sectionId)));
        path.push_back({provider.ArtistTracksLabel(view.remoteId), go(view), {}});
        break;
    case View::Kind::Search:
        // As the pages opened from it name it: "Search results (term)".
        path.push_back({Tr(L"Search results") + L" (" + view.remoteId + L")", go(view), {}, true});
        break;
    case View::Kind::RemoteSong:
        named(view.name, L"Song", go(view));
        break;
    default:
        break;
    }
}

std::vector<MenuAction> MainWindow::PlaylistListMenu(bool smart) {
    auto go = [this](View v) { return [this, v] { Navigate(v, true); }; };
    std::vector<MenuAction> out;
    auto pm = playlist_manager::get();
    auto isSmart = [](t_size p) { return p == media::FavoritesPlaylist(false) || media::IsAutoPlaylist(p); };
    for (t_size i = 0, n = pm->get_playlist_count(); i < n; ++i) {
        if (media::IsQueue(i) || isSmart(i) != smart) continue;
        pfc::string8 name;
        pm->playlist_get_name(i, name);
        View v;
        v.library = smart ? View::Library::Local : View::Library::All;
        v.kind = View::Kind::Playlist;
        v.playlist = i;
        out.push_back({Wide(name), go(v)});
    }
    if (out.empty()) {
        View page;
        page.library = smart ? View::Library::Local : View::Library::All;
        page.kind = smart ? View::Kind::SmartPlaylists : View::Kind::Playlists;
        out.push_back({smart ? L"Smart playlists" : L"Playlists", go(page)});
    }
    return out;
}

std::vector<MenuAction> MainWindow::GroupListMenu(media::GroupKind kind) {
    auto go = [this](View v) { return [this, v] { Navigate(v, true); }; };
    std::vector<MenuAction> out;
    for (const auto& g : m_library.Groups(kind)) {
        if (out.size() >= kMenuItemLimit) break; // a menu, not a list view
        View v;
        v.kind = View::Kind::Group;
        v.group = kind;
        v.key = g.key;
        v.name = g.name;
        out.push_back({g.name.empty() ? L"Unknown" : g.name, go(v)});
    }
    if (out.empty()) {
        View page;
        page.kind = View::Kind::Category;
        page.group = kind;
        out.push_back({CategoryLabel(kind), go(page)});
    }
    return out;
}

// A page's own place: its library or section from the root, as the navigation pane has it.
std::vector<MainWindow::Crumb> MainWindow::PlacePath(const View& view) {
    auto go = [this](View v) { return [this, v] { Navigate(v, true); }; };
    std::vector<Crumb> path;
    path.push_back({L"", {}, [this] { return LibraryMenu(); }});
    // Playlists: the queue is a root page, smart playlists sit flat under Local Music,
    // and the regular ones keep their section under All Music.
    if (view.kind == View::Kind::AudioCd) { // Local Music > Discs > the disc
        View home;
        home.library = View::Library::Local;
        home.kind = View::Kind::AllMusic;
        path.push_back({L"Local Music", go(home), [this] { return LocalSectionsMenu(); }});
        path.push_back({L"Discs", {}, [this] {
                            std::vector<MenuAction> drives;
                            for (const auto& drive : cd::Drives()) {
                                View v;
                                v.library = View::Library::Local;
                                v.kind = View::Kind::AudioCd;
                                v.playlist = drive.drive;
                                drives.push_back({cd::DriveTitle(drive), [this, v] { Navigate(v, true); }});
                            }
                            return drives;
                        }});
        path.push_back({view.name, go(view), {}, true});
        return path;
    }
    if (view.kind == View::Kind::Home || view.kind == View::Kind::RemoteHomeList) {
        View home;
        home.library = View::Library::All;
        home.kind = View::Kind::Home;
        path.push_back({L"Home", go(home), {}});
        if (view.kind == View::Kind::RemoteHomeList) path.push_back({view.name, go(view), {}, true});
        return path;
    }
    if (view.kind == View::Kind::Playlist) {
        if (media::IsQueue(view.playlist)) {
            path.push_back({L"Now Playing", go(view), {}});
            return path;
        }
        const bool smart =
            view.playlist == media::FavoritesPlaylist(false) || media::IsAutoPlaylist(view.playlist);
        if (smart) {
            View home;
            home.library = View::Library::Local;
            home.kind = View::Kind::AllMusic;
            path.push_back({L"Local Music", go(home), [this] { return LocalSectionsMenu(); }});
            View section;
            section.library = View::Library::Local;
            section.kind = View::Kind::SmartPlaylists;
            path.push_back({L"Smart playlists", go(section), [this] { return PlaylistListMenu(true); }});
        } else {
            View home;
            home.library = View::Library::All;
            home.kind = View::Kind::AllMusic;
            path.push_back({L"All Music", go(home), [this] { return AllSectionsMenu(); }});
            View section;
            section.library = View::Library::All;
            section.kind = View::Kind::Playlists;
            path.push_back({L"Playlists", go(section), [this] { return PlaylistListMenu(false); }});
        }
        path.push_back({view.name, go(view), {}, true});
        return path;
    }
    switch (view.library) {
    case View::Library::All: {
        View home;
        home.library = View::Library::All;
        home.kind = View::Kind::AllMusic;
        path.push_back({L"All Music", go(home), [this] { return AllSectionsMenu(); }});
        if (view.kind == View::Kind::AllFavorites) {
            path.push_back({L"Favorites", go(view), {}});
        } else if (view.kind == View::Kind::AllRecentPlayed) {
            path.push_back({L"Recently Played", go(view), {}});
        } else if (view.kind == View::Kind::AllMostPlayed) {
            path.push_back({L"Most Played", go(view), {}});
        } else if (view.kind == View::Kind::Downloads) {
            path.push_back({L"Downloads", go(view), {}});
        } else if (view.kind == View::Kind::Search) {
            path.push_back({Tr(L"Search results") + L" (" + view.remoteId + L")", go(view), {}, true});
        } else if (view.kind == View::Kind::Playlists) {
            path.push_back({L"Playlists", go(view), [this] { return PlaylistListMenu(false); }});
        }
        break;
    }
    case View::Library::Local: {
        View home;
        home.library = View::Library::Local;
        home.kind = View::Kind::AllMusic;
        path.push_back({L"Local Music", go(home), [this] { return LocalSectionsMenu(); }});
        if (view.kind == View::Kind::SmartPlaylists) {
            path.push_back({L"Smart playlists", go(view), [this] { return PlaylistListMenu(true); }});
        } else if (view.kind == View::Kind::Category) {
            path.push_back({CategoryLabel(view.group), go(view),
                            [this, kind = view.group] { return GroupListMenu(kind); }});
        } else if (view.kind == View::Kind::Group) {
            View category;
            category.library = View::Library::Local;
            category.kind = View::Kind::Category;
            category.group = view.group;
            path.push_back({CategoryLabel(view.group), go(category),
                            [this, kind = view.group] { return GroupListMenu(kind); }});
            path.push_back({view.name, go(view), {}, true});
        } else if (view.kind == View::Kind::Search) {
            path.push_back({Tr(L"Search results") + L" (" + view.remoteId + L")", go(view), {}, true});
        }
        break;
    }
    case View::Library::Remote:
        if (auto* provider = stream::FindProvider(view.provider)) AddRemoteCrumbs(path, *provider, view);
        break;
    }
    return path;
}

// The breadcrumb: the place of the page it was opened from (and so on up), then this page; or,
// opened from no page, its own place. A page adds only its own level: everything above it is
// its opener's path, unchanged (owner, 2026-11-20: a playlist opened from Home showed "my
// playlists" above it).
std::vector<MainWindow::Crumb> MainWindow::BuildCrumbPath(const View& view) {
    if (!view.from) return PlacePath(view);
    std::vector<Crumb> path = BuildCrumbPath(*view.from);
    View own = view;
    own.from = nullptr;
    auto place = PlacePath(own);
    if (place.size() < 2) return path;
    Crumb level = std::move(place.back());
    level.open = [this, view] { Navigate(view, true); }; // back here, with its own path
    path.push_back(std::move(level));
    return path;
}

void MainWindow::Open(const View& view) {
    View next = view;
    if (!next.from && !SectionPage(next)) {
        // Opened from the page on screen. A page already on its path (a link back up) is
        // reached the way it was: the path is cut there, never looped.
        std::shared_ptr<const View> from = std::make_shared<View>(m_view);
        for (auto at = from; at; at = at->from)
            if (SameView(*at, next)) {
                from = at->from;
                break;
            }
        next.from = std::move(from);
    }
    Navigate(next, true);
}

void MainWindow::Replace(const View& view) {
    View next = view;
    if (!next.from) next.from = m_view.from;
    Navigate(next, false);
}

// The page that was open when the window closed, and its top row:
// version, library, kind, playlist, name, group, key, provider, shelf, shelfKnown,
// remoteId, sectionId, groupId, top row; tab-separated. (Where it was opened from is not kept:
// restored, a page shows its own place.)
void MainWindow::SaveLastView() {
    if (!m_list) return;
    const size_t top = m_list->TopRow();
    std::wstring text = L"4";
    auto add = [&text](const std::wstring& value) {
        text += L'\t';
        text += CleanConfigField(value);
    };
    add(std::to_wstring(static_cast<int>(m_view.library)));
    add(std::to_wstring(static_cast<int>(m_view.kind)));
    add(std::to_wstring(m_view.playlist));
    add(m_view.name);
    add(std::to_wstring(static_cast<int>(m_view.group)));
    add(m_view.key);
    add(m_view.provider);
    add(std::to_wstring(static_cast<int>(m_view.shelf)));
    add(m_view.shelfKnown ? L"1" : L"0");
    add(m_view.remoteId);
    add(m_view.sectionId);
    add(m_view.groupId);
    add(top == SIZE_MAX ? L"-1" : std::to_wstring(top));
    cfg_lastView.set(pfc::stringcvt::string_utf8_from_wide(text.c_str()).get_ptr());
}

bool MainWindow::LoadLastView(View& view, size_t& top) {
    const pfc::string8 stored = cfg_lastView.get();
    if (stored.is_empty()) return false;
    const auto f = SplitConfigFields(pfc::stringcvt::string_wide_from_utf8(stored.get_ptr()).get_ptr());
    if (f.empty() || f[0] != L"4" || f.size() < 14u) return false;
    auto inRange = [](const std::wstring& text, int last) {
        const int value = _wtoi(text.c_str());
        return value >= 0 && value <= last ? value : -1;
    };
    const int library = inRange(f[1], static_cast<int>(View::Library::Remote));
    int kind = inRange(f[2], static_cast<int>(View::Kind::RemoteGroupTracks));
    if (_wtoi(f[2].c_str()) == static_cast<int>(View::Kind::Home)) kind = static_cast<int>(View::Kind::Home);
    const int group = inRange(f[5], static_cast<int>(media::kGroupKinds) - 1);
    const int shelf = inRange(f[8], static_cast<int>(stream::Shelf::Recommended));
    if (library < 0 || kind < 0 || group < 0 || shelf < 0) return false;
    View v;
    v.library = static_cast<View::Library>(library);
    v.kind = static_cast<View::Kind>(kind);
    v.name = f[4];
    v.group = static_cast<media::GroupKind>(group);
    v.key = f[6];
    v.provider = f[7];
    v.shelf = static_cast<stream::Shelf>(shelf);
    v.shelfKnown = f[9] == L"1";
    v.remoteId = f[10];
    v.sectionId = f[11];
    v.groupId = f[12];
    const std::wstring& scrollText = f[13];
    const long long scroll = _wcstoi64(scrollText.c_str(), nullptr, 10);
    top = scroll < 0 ? SIZE_MAX : static_cast<size_t>(scroll);
    if (v.kind == View::Kind::Group && v.key.empty()) return false;
    if (v.kind != View::Kind::Home && v.IsRemote() != (v.kind >= View::Kind::RemoteFavorites)) return false;
    if (v.IsRemote()) {
        auto* provider = stream::FindProvider(v.provider);
        if (!provider) return false;
        if (v.kind == View::Kind::Search && !provider->Can(stream::Capability::CatalogSearch)) return false;
        v = NormalizeRemoteView(*provider, std::move(v), false);
    }
    switch (v.kind) {
    case View::Kind::RemotePlaylist:
    case View::Kind::RemoteAlbum:
    case View::Kind::RemoteArtist:
    case View::Kind::RemoteTopTracks:
    case View::Kind::Search:
    case View::Kind::RemoteSong:
        if (v.remoteId.empty()) return false;
        break;
    case View::Kind::RemoteSectionTracks:
    case View::Kind::RemoteGroups:
        if (v.sectionId.empty()) return false;
        break;
    case View::Kind::RemoteGroupTracks:
        if (v.sectionId.empty() || v.groupId.empty()) return false;
        break;
    default:
        break;
    }
    if (v.kind == View::Kind::Playlist) {
        // By name first: reordering playlists must not open a different one.
        auto pm = playlist_manager::get();
        t_size p = v.name.empty()
                       ? SIZE_MAX
                       : pm->find_playlist(pfc::stringcvt::string_utf8_from_wide(v.name.c_str()).get_ptr());
        const auto index = static_cast<t_size>(_wcstoui64(f[3].c_str(), nullptr, 10));
        if (p == SIZE_MAX && index < pm->get_playlist_count()) p = index;
        if (p == SIZE_MAX) return false;
        v.playlist = p;
        pfc::string8 name;
        pm->playlist_get_name(p, name);
        v.name = Wide(name);
    }
    view = std::move(v);
    return true;
}

void MainWindow::ApplyPendingScroll() {
    if (m_pendingScrollKey.empty() || !m_list || !m_source || m_source->Count() == 0) return;
    // Back at the same distance from the top: restored by its row alone, the first row of a group
    // came back at the very top and the group's header (or the album line above it) was hidden.
    if (auto it = m_scrollPositions.find(m_pendingScrollKey); it != m_scrollPositions.end()) {
        m_list->SetShelfPositions(it->second.shelves);
        if (it->second.atTop)
            m_list->ScrollToTop();
        else
            m_list->ScrollToRow(std::min(it->second.top, m_source->Count() - 1), it->second.offset);
    }
    m_pendingScrollKey.clear();
}

void MainWindow::Navigate(const View& requested, bool push) {
    View view = requested;
    auto pm = playlist_manager::get();
    stream::Provider* provider = view.IsRemote() ? stream::FindProvider(view.provider) : nullptr;
    if ((view.kind == View::Kind::Playlist && view.playlist >= pm->get_playlist_count()) ||
        (view.IsRemote() && !provider)) {
        view = {}; // the page is gone: Local Music
    }
    // Resolve older or now-missing section ids before identity and scroll bookkeeping.
    if (provider) view = NormalizeRemoteView(*provider, std::move(view), false);
    // A link to the page on screen (the playing song's artist while on that artist) leaves it
    // as it is; Refresh asks for a reload with m_forcePageRefresh.
    if (push && !m_forcePageRefresh && SameView(view, m_view) && (m_page || m_source)) return;
    // Remember where the page we are leaving was scrolled; ApplyPendingScroll puts the new
    // source back there (also after an asynchronous page replaces its loading placeholder).
    if (m_list && m_source && m_source->Count()) {
        const size_t top = m_list->TopRow();
        if (top != SIZE_MAX)
            m_scrollPositions[ViewKey(m_view)] = {m_view.IsRemote() ? m_view.provider : stream::Id{}, top,
                                                  m_list->RowOffset(top), m_list->AtTop(),
                                                  m_list->ShelfPositions()};
    }
    m_pendingScrollKey = ViewKey(view);
    m_pendingSelect = view.select; // "View album" from a song: that song, once its row shows
    view.select = nullptr;
    const bool sameView = SameView(view, m_view);
    m_detailHeaderSize = {}; // repaint the band for this page, not the previous one
    if (!sameView) {
        m_detailBriefExpanded = false;
        m_detailBriefScroll = 0;
        m_briefScroll.Reset();
    }
    if (push && !sameView) {
        // A new view clears the search, like WMP.
        m_filter.clear();
        if (m_search) {
            auto keep = std::move(m_search->onChange);
            m_search->Clear();
            m_search->onChange = std::move(keep);
        }
    }
    // A search page shows its scope and term in the box; another page the list scope.
    if (m_search && (!sameView || view.kind == View::Kind::Search)) SyncSearchBox(view);
    // The page on screen goes first; results still in flight for it are dropped with it.
    m_page.reset();
    const bool reuse = !push && !m_forcePageRefresh;
    m_forcePageRefresh = false;
    m_view = view; // pages read the current view (its saved style) while they start

    std::unique_ptr<media::ListSource> source;
    NavPane::Target target = NavPane::Target::None;
    size_t navIndex = 0;
    stream::Id navId;
    if (provider) {
        m_page = MakeRemotePage(*this, *provider, view);
        switch (view.kind) {
        case View::Kind::RemoteFavorites:
        case View::Kind::RemoteArtists:
        case View::Kind::RemoteAlbums:
        case View::Kind::RemoteShelf:
        case View::Kind::RemoteArtist:
        case View::Kind::RemoteAlbum:
        case View::Kind::RemoteTopTracks:
        case View::Kind::RemoteSectionTracks:
        case View::Kind::RemoteGroups:
        case View::Kind::RemoteGroupTracks:
            target = NavPane::Target::RemoteSection;
            navId = view.sectionId;
            break;
        case View::Kind::Search:
            target = NavPane::Target::LibraryRemote;
            break;
        case View::Kind::RemoteHomeList: // opened from Home
            target = NavPane::Target::Home;
            break;
        case View::Kind::RemotePlaylist:
            target = NavPane::Target::RemotePlaylist;
            navId = view.remoteId;
            break;
        default:
            break;
        }
    }
    switch (view.kind) {
    case View::Kind::Search:
        m_page = MakeSearchPage(*this, view);
        if (!provider)
            target = view.library == View::Library::All ? NavPane::Target::LibraryAll : NavPane::Target::LibraryLocal;
        break;
    case View::Kind::AllFavorites:
        m_page = MakeAllFavoritesPage(*this, view);
        target = NavPane::Target::AllFavorites;
        break;
    case View::Kind::AudioCd: {
        const wchar_t letter = static_cast<wchar_t>(view.playlist);
        // A drive not read yet (auto-loading off) is read when shown; one found empty only when
        // its row is clicked again (OnNavItem), not on every refresh this page gets.
        if (const auto* unread = cd::FindDrive(letter); unread && unread->state == cd::CdDrive::State::Unread)
            cd::ReadDrive(letter);
        const auto* drive = cd::FindDrive(letter);
        const bool reading = drive && drive->state == cd::CdDrive::State::Reading;
        view.name = m_view.name = drive ? cd::DriveTitle(*drive) : Tr(L"Audio CD");
        source = std::make_unique<DiscSource>(
            drive && drive->state == cd::CdDrive::State::Loaded ? drive->tracks : metadb_handle_list(),
            reading ? L"Reading the disc..." : L"No disc in the drive.");
        target = NavPane::Target::AudioCd;
        navIndex = view.playlist;
        break;
    }
    case View::Kind::AllRecentPlayed: // songs, albums and playlists tabs (ui/recent_page.h)
        m_page = MakeRecentPage(*this, view);
        target = NavPane::Target::AllRecentPlayed;
        break;
    case View::Kind::Home: // the sources' home pages (ui/home_page.h)
        m_page = MakeHomePage(*this, view);
        target = NavPane::Target::Home;
        break;
    case View::Kind::Downloads: // downloading and downloaded tabs (ui/downloads_page.h)
        m_page = MakeDownloadsPage(*this, view);
        target = NavPane::Target::AllDownloads;
        break;
    case View::Kind::AllMostPlayed: {
        const bool recent = view.kind == View::Kind::AllRecentPlayed;
        const size_t rows = static_cast<size_t>(settings::PlayHistoryRows());
        source = std::make_unique<PlayStatsSource>(
            recent ? media::PlayStats::Get().Recent(rows) : media::PlayStats::Get().MostPlayed(rows),
            recent ? L"No recently played songs." : L"No most played songs.");
        target = recent ? NavPane::Target::AllRecentPlayed : NavPane::Target::AllMostPlayed;
        break;
    }
    case View::Kind::Playlist: {
        pfc::string8 name;
        pm->playlist_get_name(view.playlist, name);
        view.name = m_view.name = Wide(name);
        KeepQueueActive();
        source = std::make_unique<media::PlaylistSource>(view.playlist);
        target = media::IsQueue(view.playlist) ? NavPane::Target::Queue : NavPane::Target::Playlist;
        navIndex = view.playlist;
        break;
    }
    case View::Kind::AllMusic: {
        std::vector<size_t> all(m_library.Tracks().size());
        for (size_t i = 0; i < all.size(); ++i)
            all[i] = i;
        source = std::make_unique<media::TrackSource>(
            m_library, std::move(all),
            L"Your library is empty. Use Organize ▸ Manage music library to add your music folders.");
        target =
            view.library == View::Library::All ? NavPane::Target::LibraryAll : NavPane::Target::LibraryLocal;
        break;
    }
    case View::Kind::Playlists:        // WMP's Playlists page: every playlist as an icon
    case View::Kind::SmartPlaylists: { // the same for Favorites and the auto playlists
        const bool smart = view.kind == View::Kind::SmartPlaylists;
        source = std::make_unique<media::PlaylistsSource>(
            smart,
            [this, smart](size_t playlist) {
                View v;
                v.library = smart ? View::Library::Local : View::Library::All;
                v.kind = View::Kind::Playlist;
                v.playlist = playlist;
                Navigate(v, true);
            },
            [this](size_t playlist, POINT screen) { ShowPlaylistMenu(playlist, screen); });
        target = smart ? NavPane::Target::SmartHeader : NavPane::Target::PlaylistsHeader;
        break;
    }
    case View::Kind::Category: {
        const auto kind = view.group;
        source = std::make_unique<media::GroupSource>(m_library, kind, [this, kind](const media::Group& g) {
            View v;
            v.kind = View::Kind::Group;
            v.group = kind;
            v.key = g.key;
            v.name = g.secondary.empty() ? g.name : g.name + L" (" + g.secondary + L")";
            Navigate(v, true);
        });
        target = NavPane::Target::Category;
        navIndex = static_cast<size_t>(kind);
        break;
    }
    case View::Kind::Group: {
        const media::Group* g = m_library.FindGroup(view.group, view.key);
        source = std::make_unique<media::TrackSource>(m_library, g ? g->tracks : std::vector<size_t>{},
                                                      L"No songs.");
        target = NavPane::Target::Category;
        navIndex = static_cast<size_t>(view.group);
        break;
    }
    default:
        break; // a provider's page
    }
    m_nav->Select(target, navIndex, provider, navId);
    if (m_page) source = m_page->Start(reuse);

    if (auto* style = ViewStyleSetting(view);
        style && source->SupportsViewStyle(static_cast<media::ViewStyle>(style->get())))
        source->SetViewStyle(static_cast<media::ViewStyle>(style->get()));
    if (!m_filter.empty()) source->SetFilter(m_filter);
    m_source = std::move(source);
    m_list->SetSource(m_source.get(), !sameView);
    if (m_page) m_page->OnPresentationChanged();

    if (push && !sameView) {
        if (!m_history.empty()) m_history.resize(m_historyPos + 1);
        m_history.push_back(view);
        m_historyPos = m_history.size() - 1;
    } else if (!m_history.empty()) {
        m_history[m_historyPos] = view;
    } else {
        m_history.push_back(view);
        m_historyPos = 0;
    }
    for (int id : {kBack, kForward, kViewOptions})
        if (auto* w = WidgetById(id)) w->Invalidate();
    RepaintBreadcrumb(); // also re-applies the layout (view options button, detail band)
    SelectPending();
}

// A page opened for one of its songs selects it and scrolls to it as soon as its row is there
// (an album page reads its songs after it opens).
void MainWindow::SelectPending() {
    if (!m_pendingSelect.is_valid() || !m_source || !m_list) return;
    const size_t row = m_source->FindRow(m_pendingSelect);
    if (row == SIZE_MAX) return;
    m_pendingSelect = nullptr;
    m_pendingScrollKey.clear();
    m_source->SelectOnly(row);
    m_source->SetFocus(row);
    m_list->EnsureVisible(row);
    m_list->Refresh();
}

void MainWindow::OnAlbumLink(size_t group, size_t line) {
    const auto* groups = m_source ? m_source->Groups() : nullptr;
    if (!groups || group >= groups->size() || line > 3) return;
    const auto& g = (*groups)[group];
    const media::Link& link = g.links[line];
    using Kind = media::Link::Kind;
    View v;
    switch (link.kind) {
    case Kind::None:
        return;
    case Kind::RemoteAlbum:
    case Kind::RemoteArtist: {
        auto* provider = stream::FindProvider(link.provider);
        if (!provider) return;
        const stream::Id sectionId = m_view.provider == provider->Key() ? m_view.sectionId : stream::Id{};
        v = link.kind == Kind::RemoteAlbum ? AlbumView(*provider, link.key, g.lines[0], sectionId)
                                           : ArtistView(*provider, link.key, g.lines[line], sectionId);
        break;
    }
    case Kind::LocalAlbum: { // by name; the group's artist line picks between namesakes
        const media::Group* found = nullptr;
        for (const auto& album : m_library.Groups(media::GroupKind::Album)) {
            if (album.name != link.key) continue;
            if (!found) found = &album;
            if (!g.lines[1].empty() && album.secondary == g.lines[1]) {
                found = &album;
                break;
            }
        }
        if (!found) return;
        v.kind = View::Kind::Group;
        v.group = media::GroupKind::Album;
        v.key = found->key;
        v.name = found->name;
        break;
    }
    case Kind::LocalArtist: {
        media::GroupKind kind = media::GroupKind::ContributingArtist;
        if (!m_library.FindGroup(kind, link.key)) {
            kind = media::GroupKind::Artist;
            if (!m_library.FindGroup(kind, link.key)) return;
        }
        v.kind = View::Kind::Group;
        v.group = kind;
        v.key = v.name = link.key;
        break;
    }
    case Kind::Group:
        v.kind = View::Kind::Group;
        v.group = link.group;
        v.key = link.key;
        v.name = g.lines[line];
        break;
    }
    Navigate(v, true);
}

void MainWindow::GoBack() {
    if (m_historyPos == 0) return;
    --m_historyPos;
    Navigate(m_history[m_historyPos], false);
}

void MainWindow::GoForward() {
    if (m_historyPos + 1 >= m_history.size()) return;
    ++m_historyPos;
    Navigate(m_history[m_historyPos], false);
}

void MainWindow::OnNavItem(const NavPane::Item& item) {
    View v;
    switch (item.target) {
    case NavPane::Target::Playlist:
        v.library = item.playlist == media::FavoritesPlaylist(false) || media::IsAutoPlaylist(item.playlist)
                        ? View::Library::Local
                        : View::Library::All;
        v.kind = View::Kind::Playlist;
        v.playlist = item.playlist;
        break;
    case NavPane::Target::Queue:
        v.library = View::Library::All;
        v.kind = View::Kind::Playlist;
        v.playlist = media::QueuePlaylist(true);
        break;
    case NavPane::Target::AudioCd:
    case NavPane::Target::DiscsHeader: // the first drive
        if (cd::Drives().empty()) return;
        v.library = View::Library::Local;
        v.kind = View::Kind::AudioCd;
        v.playlist = item.target == NavPane::Target::AudioCd ? item.playlist : cd::Drives().front().drive;
        cd::ReadDrive(static_cast<wchar_t>(v.playlist)); // looks again in a drive found empty
        break;
    case NavPane::Target::SmartHeader:
        v.library = View::Library::Local;
        v.kind = View::Kind::SmartPlaylists;
        break;
    case NavPane::Target::Category:
        v.kind = View::Kind::Category;
        v.group = static_cast<media::GroupKind>(item.playlist);
        break;
    case NavPane::Target::PlaylistsHeader:
        v.library = View::Library::All;
        v.kind = View::Kind::Playlists; // WMP's Playlists page
        break;
    case NavPane::Target::LibraryAll:
        v.library = View::Library::All;
        v.kind = View::Kind::AllMusic;
        break;
    case NavPane::Target::AllFavorites:
        v.library = View::Library::All;
        v.kind = View::Kind::AllFavorites;
        break;
    case NavPane::Target::AllRecentPlayed:
        v.library = View::Library::All;
        v.kind = View::Kind::AllRecentPlayed;
        break;
    case NavPane::Target::AllMostPlayed:
        v.library = View::Library::All;
        v.kind = View::Kind::AllMostPlayed;
        break;
    case NavPane::Target::AllDownloads:
        v.library = View::Library::All;
        v.kind = View::Kind::Downloads;
        break;
    case NavPane::Target::Home:
        v.library = View::Library::All;
        v.kind = View::Kind::Home;
        break;
    case NavPane::Target::LibraryLocal:
        v.library = View::Library::Local;
        v.kind = View::Kind::AllMusic;
        break;
    case NavPane::Target::LibraryRemote:
        v = ProviderLandingView(*item.provider);
        break;
    case NavPane::Target::RemoteSection:
        if (const auto section = FindProviderSection(*item.provider, item.sectionId))
            v = SectionView(*item.provider, *section);
        else
            return;
        break;
    case NavPane::Target::RemotePlaylist:
        v = PlaylistView(*item.provider, item.remoteId, item.text, item.sectionShelf, item.sectionId);
        break;
    default:
        return;
    }
    Navigate(v, true);
}

void MainWindow::OnNavMove(const NavPane::Item& dragged, const NavPane::Item& target, bool after) {
    if (dragged.target == NavPane::Target::SmartHeader && target.target == NavPane::Target::Category) {
        const auto kinds = m_nav->Categories();
        const auto it = std::find(kinds.begin(), kinds.end(), static_cast<media::GroupKind>(target.playlist));
        if (it == kinds.end()) return;
        SetNavSmartPosition(static_cast<int>(it - kinds.begin()) + (after ? 1 : 0));
        return;
    }
    if (dragged.target == NavPane::Target::Category && target.target == NavPane::Target::SmartHeader) {
        auto kinds = m_nav->Categories();
        const auto it =
            std::find(kinds.begin(), kinds.end(), static_cast<media::GroupKind>(dragged.playlist));
        if (it == kinds.end()) return;
        const auto kind = *it;
        int at = m_nav->SmartPosition();
        if (static_cast<int>(it - kinds.begin()) < at) --at; // removing it shifts the boundary
        kinds.erase(it);
        at = std::clamp(at, 0, static_cast<int>(kinds.size()));
        kinds.insert(kinds.begin() + at, kind);
        SetNavCategories(std::move(kinds));
        // Above the header row keeps the category before the section, below puts it after.
        SetNavSmartPosition(at + (after ? 0 : 1));
        return;
    }
    if (dragged.target == NavPane::Target::Category && target.target == NavPane::Target::Category) {
        auto kinds = m_nav->Categories();
        const auto kind = static_cast<media::GroupKind>(dragged.playlist);
        const auto destination = static_cast<media::GroupKind>(target.playlist);
        auto from = std::find(kinds.begin(), kinds.end(), kind);
        if (from == kinds.end()) return;
        kinds.erase(from);
        auto to = std::find(kinds.begin(), kinds.end(), destination);
        if (to == kinds.end()) return;
        const size_t index = static_cast<size_t>(to - kinds.begin()) + (after ? 1 : 0);
        kinds.insert(kinds.begin() + static_cast<std::ptrdiff_t>(index), kind);
        SetNavCategories(std::move(kinds));
        return;
    }
    if (dragged.target == NavPane::Target::Playlist && target.target == NavPane::Target::Playlist) {
        auto pm = playlist_manager::get();
        const t_size count = pm->get_playlist_count();
        if (!count) return;
        auto smart = [](t_size p) {
            return p == media::FavoritesPlaylist(false) || media::IsAutoPlaylist(p);
        };
        const bool groupSmart = smart(dragged.playlist);
        if (smart(target.playlist) != groupSmart) return;
        auto inGroup = [&](t_size p) {
            return groupSmart ? smart(p) : !media::IsSpecial(p) && !media::IsAutoPlaylist(p);
        };
        std::vector<t_size> group;
        for (t_size p = 0; p < count; ++p)
            if (inGroup(p)) group.push_back(p);
        auto from = std::find(group.begin(), group.end(), dragged.playlist);
        if (from == group.end()) return;
        const t_size moving = *from;
        group.erase(from);
        auto to = std::find(group.begin(), group.end(), target.playlist);
        if (to == group.end()) return;
        const size_t index = static_cast<size_t>(to - group.begin()) + (after ? 1 : 0);
        group.insert(group.begin() + static_cast<std::ptrdiff_t>(index), moving);
        std::vector<t_size> full(count);
        for (t_size i = 0; i < count; ++i)
            full[i] = i;
        size_t next = 0;
        for (auto& value : full)
            if (inGroup(value)) {
                if (next >= group.size()) return;
                value = group[next++];
            }
        if (next != group.size()) return;
        pm->reorder(full.data(), count); // on_playlists_reorder moves the views along
        return;
    }
    // A provider's playlists: within the own or the saved ones, synced to the service.
    if (dragged.target != NavPane::Target::RemotePlaylist ||
        target.target != NavPane::Target::RemotePlaylist || dragged.provider != target.provider ||
        dragged.sectionId != target.sectionId || dragged.sectionKind != stream::SectionKind::PlaylistShelf ||
        !dragged.reorderChildren || !dragged.provider->Can(stream::Capability::ReorderPlaylists))
        return;
    stream::Provider& provider = *dragged.provider;
    std::vector<stream::Id> ids;
    std::set<stream::Id> seenSections;
    bool moved = false;
    for (const auto& section : provider.Sections()) {
        if (section.kind != stream::SectionKind::PlaylistShelf || !section.reorderChildren ||
            !seenSections.emplace(section.id).second)
            continue;
        auto playlists = provider.SectionPlaylists(section.id);
        if (section.id == dragged.sectionId) {
            auto byId = [](const stream::Id& wanted) {
                return [&wanted](const stream::Playlist& playlist) { return playlist.id == wanted; };
            };
            auto from = std::find_if(playlists.begin(), playlists.end(), byId(dragged.remoteId));
            if (from == playlists.end()) return;
            stream::Playlist value = std::move(*from);
            playlists.erase(from);
            auto to = std::find_if(playlists.begin(), playlists.end(), byId(target.remoteId));
            if (to == playlists.end()) return;
            playlists.insert(to + (after ? 1 : 0), std::move(value));
            moved = true;
        }
        for (const auto& playlist : playlists)
            ids.push_back(playlist.id);
    }
    if (!moved) return;
    provider.ReorderPlaylists(std::move(ids)); // the provider shows the new order at once
    RefreshProviderSections();
}

void MainWindow::ShowOrganizeMenu() {
    POINT pt{m_layout.organize.left, m_layout.organize.bottom};
    ClientToScreen(m_hwnd, &pt);
    ShowMainMenu(
        m_hwnd, pt,
        {{L"Manage music library...", [] { library_manager::get()->show_preferences(); }},
         {L"Rescan library", [] { library_manager::get()->rescan(); }},
         {L"", nullptr},
         {L"Add files...",
          [this] {
              ActivateAddTarget();
              standard_commands::main_add_files();
          }},
         {L"Add folder...",
          [this] {
              ActivateAddTarget();
              standard_commands::main_add_directory();
          }},
         {L"Add location...",
          [this] {
              ActivateAddTarget();
              standard_commands::run_main(standard_commands::guid_main_add_location);
          }},
         {L"", nullptr},
         {L"Language",
          nullptr,
          {{L"Follow Windows",
            [this] { ApplyLanguage(Language::Auto); },
            {},
            CurrentLanguageSetting() == Language::Auto},
           {L"English",
            [this] { ApplyLanguage(Language::English); },
            {},
            CurrentLanguageSetting() == Language::English},
           {L"中文 (Chinese)",
            [this] { ApplyLanguage(Language::Chinese); },
            {},
            CurrentLanguageSetting() == Language::Chinese}}},
         {L"", nullptr},
         {L"Preferences...", [] { standard_commands::run_main(standard_commands::guid_main_preferences); }}});
    SetHoverWidget(nullptr); // the menu loop swallowed the mouse leave
}

// WMP: right-click Local Music > Customize navigation pane picks the categories listed under it.
void MainWindow::ShowNavCustomizeMenu(POINT screen) {
    std::vector<MenuAction> categories;
    const auto shown = m_nav->Categories();
    for (size_t i = 0; i < media::kGroupKinds; ++i) {
        const auto kind = static_cast<media::GroupKind>(i);
        const bool on = std::find(shown.begin(), shown.end(), kind) != shown.end();
        MenuAction a{CategoryLabel(kind), [this, kind, on, shown] {
                         auto next = shown;
                         if (on)
                             next.erase(std::find(next.begin(), next.end(), kind));
                         else
                             next.push_back(kind);
                         std::sort(next.begin(), next.end()); // always listed in WMP order
                         SetNavCategories(std::move(next));
                     }};
        a.checked = on;
        a.checkbox = true;
        categories.push_back(std::move(a));
    }
    categories.push_back({});
    categories.push_back(
        {L"Restore defaults", [this] {
             SetNavCategories({media::GroupKind::Artist, media::GroupKind::Album, media::GroupKind::Genre});
         }});
    View all;
    all.library = View::Library::Local;
    all.kind = View::Kind::AllMusic;
    std::vector<MenuAction> actions{{L"Open", [this, all] { Navigate(all, true); }},
                                    {L"Customize navigation pane", {}, std::move(categories)}};
    ShowActionMenu(m_hwnd, screen, actions);
}

void MainWindow::SetNavCategories(std::vector<media::GroupKind> kinds) {
    std::string digits;
    for (auto k : kinds)
        digits += static_cast<char>('0' + static_cast<int>(k));
    cfg_navCategories.set(digits.c_str());
    m_nav->SetCategories(std::move(kinds));
    cfg_navSmartAt.set(m_nav->SmartPosition()); // clamped when a category disappeared
    RepaintBreadcrumb();
}

void MainWindow::SetNavSmartPosition(int position) {
    position = std::clamp(position, 0, static_cast<int>(m_nav->Categories().size()));
    cfg_navSmartAt.set(position);
    m_nav->SetSmartPosition(position);
    RepaintBreadcrumb();
}

void MainWindow::ShowPlaylistMenu(size_t playlist, POINT screen) {
    auto pm = playlist_manager::get();
    std::vector<MenuAction> actions;
    if (playlist != SIZE_MAX && playlist < pm->get_playlist_count()) {
        actions.push_back({L"Play", [this, playlist] {
                               auto p = playlist_manager::get();
                               p->set_active_playlist(playlist);
                               const t_size focus = p->playlist_get_focus_item(playlist);
                               if (p->playlist_get_item_count(playlist))
                                   p->playlist_execute_default_action(playlist,
                                                                      focus == SIZE_MAX ? 0 : focus);
                               KeepQueueActive();
                           }});
        if (media::IsQueue(playlist))
            actions.push_back({TrMenu(L"Clear Now Playing"), [this] { ClearNowPlaying(); }});
        else {
            auto contents = [playlist] {
                metadb_handle_list items;
                playlist_manager::get()->playlist_get_all_items(playlist, items);
                return items;
            };
            actions.push_back({L"Play next", [contents] { media::PlayNext(contents()); }});
            actions.push_back({L"Add to queue", [contents] { media::AddToQueue(contents()); }});
            std::vector<MenuAction> targets;
            for (const auto& [id, name] : LocalPlaylistTargets(playlist)) {
                targets.push_back({name.empty() ? L"Unnamed playlist" : name, [contents, id] {
                                       auto items = contents();
                                       if (items.get_count())
                                           playlist_manager::get()->playlist_add_items(
                                               static_cast<t_size>(id), items, pfc::bit_array_false());
                                   }});
            }
            if (targets.empty()) targets.push_back({L"No playlists", nullptr});
            actions.push_back({L"Add to playlist", nullptr, std::move(targets)});
        }
        if (media::IsSmart(playlist))
            actions.push_back({L"Edit smart playlist...", [playlist] {
                                   if (auto c = autoplaylist_manager::get()->query_client(playlist);
                                       c.is_valid())
                                       c->show_ui(playlist);
                               }});
        // The queue and Favorites are built in: no renaming or removing.
        if (!media::IsQueue(playlist) && playlist != media::FavoritesPlaylist(false)) {
            actions.push_back({L"Rename...", [this, playlist] {
                                   playlist_manager::get()->set_active_playlist(playlist);
                                   standard_commands::run_main(standard_commands::guid_main_rename_playlist);
                                   KeepQueueActive();
                               }});
            actions.push_back(
                {L"Remove", [playlist] { playlist_manager::get()->remove_playlist(playlist); }});
        }
        actions.push_back({L"", nullptr});
    }
    for (auto& a : AddPlaylistActions())
        actions.push_back(std::move(a));
    actions.push_back({L"", nullptr});
    actions.push_back({L"Load playlist...", [] { standard_commands::main_load_playlist(); }});
    actions.push_back({L"Save all playlists...", [] { standard_commands::main_save_all_playlists(); }});
    actions.push_back({L"Restore smart playlists", [this] {
                           media::EnsureSpecialPlaylists(true);
                           m_nav->Rebuild();
                       }});
    ShowActionMenu(m_hwnd, screen, actions);
}

std::vector<MenuAction> MainWindow::AddPlaylistActions() {
    // As in the owner's localized foobar2000: Add > New playlist / New smart playlist /
    // Preset smart playlists > (library, never played, history, most played, recently
    // added | Favorites | rated 5..1, unrated).
    auto open = [this](t_size p) {
        if (p == SIZE_MAX) return;
        m_nav->Rebuild();
        View v;
        v.library = View::Library::All;
        v.kind = View::Kind::Playlist;
        v.playlist = p;
        Navigate(v, true);
    };
    std::vector<MenuAction> presets;
    int group = 0;
    for (const auto& preset : media::SmartPresets()) {
        if (preset.group != group && !presets.empty()) presets.push_back({L"", nullptr});
        group = preset.group;
        presets.push_back({Wide(preset.name), [open, &preset] { open(media::AddSmartPreset(preset)); }});
    }
    return {{L"Add",
             nullptr,
             {{L"New playlist", [this] { CreatePlaylist(false); }},
              {L"New smart playlist", [open] { open(media::NewSmartPlaylist()); }},
              {L"Preset smart playlists", nullptr, std::move(presets)}}}};
}

void MainWindow::AddDroppedFiles(IDataObject* data) {
    // Drop target: the playlist on screen, else the active playlist (created if needed).
    auto pm = playlist_manager::get();
    t_size target = m_view.kind == View::Kind::Playlist ? m_view.playlist : pm->get_active_playlist();
    if (target == SIZE_MAX || target >= pm->get_playlist_count())
        target = pm->find_or_create_playlist("Default");
    auto notify = process_locations_notify::create([target](metadb_handle_list_cref items) {
        auto p = playlist_manager::get();
        if (target < p->get_playlist_count()) {
            p->playlist_undo_backup(target);
            p->playlist_add_items(target, items, pfc::bit_array_false());
        }
    });
    playlist_incoming_item_filter_v2::get()->process_dropped_files_async(
        data, playlist_incoming_item_filter_v2::op_flag_delay_ui, m_hwnd, notify);
}

void MainWindow::SetSearchFocus(bool focused) {
    m_search->SetFocused(focused);
    m_list->SetFocused(!focused);
    m_nav->SetFocused(false);
    if (focused)
        ShowSearchPopup();
    else
        HideSearchPopup();
}

void MainWindow::FocusNav() {
    HideSearchPopup();
    m_search->SetFocused(false);
    m_list->SetFocused(false);
    m_nav->SetFocused(true);
}

} // namespace glasswave::ui
