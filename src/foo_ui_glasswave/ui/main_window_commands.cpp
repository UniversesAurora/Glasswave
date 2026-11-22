#include "pch.h"
#include "ui/main_window_parts.h"

// Commands on songs and pages: the context menu's extra commands, play statistics,
// page menus, view styles and new playlists.
namespace glasswave::ui {

using namespace mainwin;

namespace {

// WMP view option chosen per library view (media::ViewStyle values).
// {6A1F3C58-2B7E-4D90-8C15-3E9A7B2D4F61} .. {6A1F3C5B-...}
cfg_var_modern::cfg_int cfg_viewSongs(
    {0x6a1f3c58, 0x2b7e, 0x4d90, {0x8c, 0x15, 0x3e, 0x9a, 0x7b, 0x2d, 0x4f, 0x61}},
    static_cast<int>(media::ViewStyle::Tiles));

cfg_var_modern::cfg_int cfg_viewAlbums(
    {0x6a1f3c59, 0x2b7e, 0x4d90, {0x8c, 0x15, 0x3e, 0x9a, 0x7b, 0x2d, 0x4f, 0x61}},
    static_cast<int>(media::ViewStyle::Icons));

cfg_var_modern::cfg_int cfg_viewArtists(
    {0x6a1f3c5a, 0x2b7e, 0x4d90, {0x8c, 0x15, 0x3e, 0x9a, 0x7b, 0x2d, 0x4f, 0x61}},
    static_cast<int>(media::ViewStyle::Tiles));

cfg_var_modern::cfg_int cfg_viewGenres(
    {0x6a1f3c5b, 0x2b7e, 0x4d90, {0x8c, 0x15, 0x3e, 0x9a, 0x7b, 0x2d, 0x4f, 0x61}},
    static_cast<int>(media::ViewStyle::Tiles));

// Playlists: Details (WMP's default for a playlist) or Expanded tile.
cfg_var_modern::cfg_int cfg_viewPlaylist(
    {0x6a1f3c5e, 0x2b7e, 0x4d90, {0x8c, 0x15, 0x3e, 0x9a, 0x7b, 0x2d, 0x4f, 0x61}},
    static_cast<int>(media::ViewStyle::Details));

// The other Music categories (Year, Rating, Composer, ...) share one view option.
cfg_var_modern::cfg_int cfg_viewCategories(
    {0x6a1f3c5c, 0x2b7e, 0x4d90, {0x8c, 0x15, 0x3e, 0x9a, 0x7b, 0x2d, 0x4f, 0x61}},
    static_cast<int>(media::ViewStyle::Tiles));

// All Music > Favorites; a provider's playlists, songs and saved albums.
// {6A1F3C5F-2B7E-4D90-8C15-3E9A7B2D4F61} .. {6A1F3C62-...}
cfg_var_modern::cfg_int cfg_viewAllFavorites(
    {0x6a1f3c5f, 0x2b7e, 0x4d90, {0x8c, 0x15, 0x3e, 0x9a, 0x7b, 0x2d, 0x4f, 0x61}},
    static_cast<int>(media::ViewStyle::Details));

// All Music > Recently Played / Most Played: the cross-source play lists.
cfg_var_modern::cfg_int cfg_viewPlayStats(
    {0x6a1f3c63, 0x2b7e, 0x4d90, {0x8c, 0x15, 0x3e, 0x9a, 0x7b, 0x2d, 0x4f, 0x61}},
    static_cast<int>(media::ViewStyle::Details));

cfg_var_modern::cfg_int cfg_viewRemotePlaylists(
    {0x6a1f3c60, 0x2b7e, 0x4d90, {0x8c, 0x15, 0x3e, 0x9a, 0x7b, 0x2d, 0x4f, 0x61}},
    static_cast<int>(media::ViewStyle::Icons));

cfg_var_modern::cfg_int cfg_viewRemoteSongs(
    {0x6a1f3c61, 0x2b7e, 0x4d90, {0x8c, 0x15, 0x3e, 0x9a, 0x7b, 0x2d, 0x4f, 0x61}},
    static_cast<int>(media::ViewStyle::Details));

// The search page's categories: songs (and lyrics), and artists, albums and playlists as tiles.
cfg_var_modern::cfg_int cfg_viewSearchSongs(
    {0x6a1f3c70, 0x2b7e, 0x4d90, {0x8c, 0x15, 0x3e, 0x9a, 0x7b, 0x2d, 0x4f, 0x61}},
    static_cast<int>(media::ViewStyle::Details));

cfg_var_modern::cfg_int cfg_viewSearchTiles(
    {0x6a1f3c71, 0x2b7e, 0x4d90, {0x8c, 0x15, 0x3e, 0x9a, 0x7b, 0x2d, 0x4f, 0x61}},
    static_cast<int>(media::ViewStyle::Icons));

// Saved albums: cover tiles, or the saved albums' songs grouped by album.
cfg_var_modern::cfg_int cfg_viewRemoteAlbums(
    {0x6a1f3c62, 0x2b7e, 0x4d90, {0x8c, 0x15, 0x3e, 0x9a, 0x7b, 0x2d, 0x4f, 0x61}},
    static_cast<int>(media::ViewStyle::Icons));

// Artist page: album tiles, or all album songs grouped by album.
// {6A1F3C66-2B7E-4D90-8C15-3E9A7B2D4F61}
cfg_var_modern::cfg_int cfg_viewRemoteArtist(
    {0x6a1f3c66, 0x2b7e, 0x4d90, {0x8c, 0x15, 0x3e, 0x9a, 0x7b, 0x2d, 0x4f, 0x61}},
    static_cast<int>(media::ViewStyle::Icons));

} // namespace

std::vector<MenuAction> MainWindow::BuildArtistLinks(metadb_handle_list_cref items,
                                                     bool allowSearchFallback) {
    std::vector<MenuAction> links;
    if (items.get_count() == 0 || !items[0].is_valid()) return links;
    const metadb_handle_ptr& first = items[0];
    const auto hasLabel = [&links](const std::wstring& label) {
        return std::any_of(links.begin(), links.end(),
                           [&label](const MenuAction& link) { return link.label == label; });
    };
    const auto addSearch = [this, &links, &hasLabel](const std::wstring& name) {
        if (name.empty() || hasLabel(name)) return;
        links.push_back({name, [this, name] { Search(name); }});
    };
    const auto forEachName = [](const std::wstring& joined, auto&& visit) {
        size_t pos = 0;
        while (pos <= joined.size()) {
            const size_t end = joined.find(L"; ", pos);
            std::wstring name =
                joined.substr(pos, end == std::wstring::npos ? std::wstring::npos : end - pos);
            const size_t begin = name.find_first_not_of(L" \t");
            const size_t last = name.find_last_not_of(L" \t");
            name = begin == std::wstring::npos ? std::wstring() : name.substr(begin, last - begin + 1);
            if (!name.empty()) visit(name);
            if (end == std::wstring::npos) break;
            pos = end + 2;
        }
    };
    const auto addMetadataSearchFallback = [&first, &forEachName, &addSearch]() {
        metadb_info_container::ptr info;
        if (!first->get_info_ref(info) || !info.is_valid()) return;
        const file_info& fi = info->info();
        const t_size count = fi.meta_get_count_by_name("ARTIST");
        for (t_size i = 0; i < count; ++i) {
            const char* raw = fi.meta_get("ARTIST", i);
            if (!raw || !*raw) continue;
            forEachName(core::Wide(raw), addSearch);
        }
    };

    if (auto* provider = stream::ProviderOf(first)) {
        const auto track = provider->FindTrack(provider->TrackId(first));
        if (track) {
            const stream::Id sectionId = m_view.provider == provider->Key() ? m_view.sectionId : stream::Id{};
            std::vector<stream::Id> seen;
            for (const auto& artist : track->artists) {
                if (artist.name.empty()) continue;
                if (!artist.id.empty()) {
                    if (std::find(seen.begin(), seen.end(), artist.id) != seen.end()) continue;
                    seen.push_back(artist.id);
                    links.push_back({artist.name, [this, provider, artist, sectionId] {
                                         Open(ArtistView(*provider, artist.id, artist.name, sectionId));
                                     }});
                } else if (allowSearchFallback) {
                    addSearch(artist.name);
                }
            }
        }
        if (links.empty() && allowSearchFallback) addMetadataSearchFallback();
        return links;
    }

    const media::Track* track = m_library.FindTrack(first);
    if (!track) {
        if (allowSearchFallback) addMetadataSearchFallback();
        return links;
    }
    auto add = [this, &links, &hasLabel](media::GroupKind kind, const std::wstring& key,
                                         const std::wstring& label) {
        if (hasLabel(label)) return;
        links.push_back({label, [this, kind, key, label] {
                             View view;
                             view.library = View::Library::Local;
                             view.kind = View::Kind::Group;
                             view.group = kind;
                             view.key = key;
                             view.name = label;
                             Open(view);
                         }});
    };
    forEachName(track->artist, [&](const std::wstring& name) {
        if (m_library.FindGroup(media::GroupKind::ContributingArtist, name))
            add(media::GroupKind::ContributingArtist, name, name);
        else if (allowSearchFallback)
            addSearch(name);
    });
    if (links.empty() && !track->albumArtist.empty()) {
        if (m_library.FindGroup(media::GroupKind::Artist, track->albumArtist))
            add(media::GroupKind::Artist, track->albumArtist, track->albumArtist);
        else if (allowSearchFallback)
            addSearch(track->albumArtist);
    }
    return links;
}

// Our commands above foobar2000's in a song's context menu: where the song leads (artist,
// album), what it can be added to, then what this page can do with it.
std::vector<MenuAction> MainWindow::BuildContextActions(metadb_handle_list_cref items) {
    std::vector<MenuAction> actions;
    if (items.get_count() == 0) return actions;
    metadb_handle_list selected;
    selected.add_items(items);

    // The same groups as an album's or a playlist's menu (ui/item_menu.h): where it leads (opened
    // from this page: the path goes on from here), what to do with it, searches, the page's own.
    const metadb_handle_ptr& first = items[0];
    stream::Provider* provider = stream::ProviderOf(first);
    std::wstring title, albumName;
    std::vector<std::wstring> artistNames; // each credited artist, for "Search for artist"
    if (provider) {
        if (const auto track = provider->FindTrack(provider->TrackId(first))) {
            title = track->title;
            albumName = track->album;
            for (const auto& artist : track->artists)
                if (!artist.name.empty()) artistNames.push_back(artist.name);
        }
    } else if (const media::Track* track = first.is_valid() ? m_library.FindTrack(first) : nullptr) {
        title = track->title;
        albumName = track->album;
        if (!track->artist.empty()) artistNames.push_back(track->artist);
    }
    if (provider) {
        const auto track = provider->FindTrack(provider->TrackId(first));
        const bool onAlbumPage = track && m_view.kind == View::Kind::RemoteAlbum &&
                                 m_view.provider == provider->Key() && m_view.remoteId == track->albumId;
        const stream::Id sectionId = m_view.provider == provider->Key() ? m_view.sectionId : stream::Id{};
        if (track && !track->albumId.empty() && !onAlbumPage)
            actions.push_back(
                {L"Go to album", [this, provider, id = track->albumId, name = track->album, sectionId, first] {
                     View v = AlbumView(*provider, id, name, sectionId);
                     v.select = first;
                     Open(v);
                 }});
    } else if (const media::Track* track = first.is_valid() ? m_library.FindTrack(first) : nullptr;
               track && track->albumIndex < m_library.Groups(media::GroupKind::Album).size()) {
        const auto& album = m_library.Groups(media::GroupKind::Album)[track->albumIndex];
        const bool onAlbumPage = m_view.kind == View::Kind::Group &&
                                 m_view.group == media::GroupKind::Album && m_view.key == album.key;
        if (!onAlbumPage)
            actions.push_back({L"Go to album", [this, key = album.key, name = album.name, first] {
                                   View v;
                                   v.kind = View::Kind::Group;
                                   v.group = media::GroupKind::Album;
                                   v.key = key;
                                   v.name = name;
                                   v.select = first;
                                   Open(v);
                               }});
    }
    auto artistLinks = BuildArtistLinks(items);
    if (artistLinks.size() == 1)
        actions.push_back({L"Go to artist", artistLinks.front().run});
    else if (!artistLinks.empty())
        actions.push_back({L"Go to artist", nullptr, std::move(artistLinks)});
    // One song of a source that has song details: its page.
    if (provider && selected.get_count() == 1 && provider->Can(stream::Capability::SongDetails) &&
        !(m_view.kind == View::Kind::RemoteSong && m_view.remoteId == provider->TrackId(selected[0]))) {
        const stream::Id id = provider->TrackId(selected[0]);
        if (!id.empty())
            actions.push_back({L"Song info", [this, provider, id, title] { Open(SongView(*provider, id, title)); }});
    }
    if (!actions.empty()) actions.push_back({});

    const bool favorite = media::AllFavorite(selected);
    bool favoriteWritable = true;
    for (size_t i = 0; i < selected.get_count() && favoriteWritable; ++i)
        if (auto* owner = stream::ProviderOf(selected[i]))
            favoriteWritable = owner->CanSetFavorite(!favorite);
    if (favoriteWritable) {
        actions.push_back({favorite ? L"Remove from Favorites" : L"Add to Favorites",
                           [selected, favorite] { media::SetFavorite(selected, !favorite); }});
    }
    // A source whose library takes single songs (Apple Music's Add to Library): the songs
    // themselves, never their albums. Remove only when every one is known to be there.
    if (provider && provider->AccountVerified() && provider->Can(stream::Capability::SaveSongs)) {
        std::vector<stream::Id> ids;
        for (size_t i = 0; i < selected.get_count(); ++i)
            if (stream::ProviderOf(selected[i]) == provider)
                if (stream::Id id = provider->TrackId(selected[i]); !id.empty()) ids.push_back(std::move(id));
        if (!ids.empty()) {
            const bool saved = provider->SongsSavedState(ids).value_or(false);
            actions.push_back({provider->Text(saved ? L"Remove from Library" : L"Add to Library"),
                               [provider, ids, saved] {
                                   provider->SetSongsSaved(ids, !saved, [provider](std::wstring error) {
                                       if (!error.empty()) ShowProviderError(*provider, error);
                                   });
                               }});
        }
    }
    // A foobar2000 playlist takes local and provider tracks alike; a provider's own
    // playlists only its tracks.
    std::vector<MenuAction> local;
    for (auto& target :
         LocalPlaylistTargets(m_view.kind == View::Kind::Playlist ? m_view.playlist : SIZE_MAX)) {
        const std::wstring label = target.name.empty() ? L"Unnamed playlist" : target.name;
        local.push_back(
            {label, [selected, target = std::move(target)] { AddToLocalPlaylist(target, selected); }});
    }
    if (local.empty()) local.push_back({L"No playlists", nullptr});
    actions.push_back({L"Add to playlist", nullptr, std::move(local)});
    if (provider) {
        const bool onPlaylist =
            m_view.kind == View::Kind::RemotePlaylist && m_view.provider == provider->Key();
        AddRemotePlaylistMenu(actions, *provider, Ready(selected),
                              onPlaylist ? m_view.remoteId : stream::Id());
    }
    AddDownloadActions(actions, selected, m_hwnd);

    // Searches name what they look for (owner, 2026-11-21: a song offered only 搜索此唱片集).
    actions.push_back({});
    const auto quoted = [](const std::wstring& name) { return L" \u201C" + name + L"\u201D"; };
    if (!title.empty() && selected.get_count() == 1)
        actions.push_back({Tr(L"Search for song") + quoted(title), [this, title] { Search(title); }});
    for (const auto& artist : artistNames)
        actions.push_back({Tr(L"Search for artist") + quoted(artist), [this, artist] { Search(artist); }});
    if (!albumName.empty())
        actions.push_back({Tr(L"Search for album") + quoted(albumName), [this, albumName] { Search(albumName); }});

    // Removals that belong to the page go last.
    std::vector<MenuAction> page;
    if (m_page) m_page->AddSongActions(page, selected);
    if (m_view.kind == View::Kind::Playlist && media::IsQueue(m_view.playlist))
        page.push_back({L"Remove from Now Playing", [this] { RemoveFromNowPlaying(); }});
    else if (m_view.kind == View::Kind::AllRecentPlayed)
        page.push_back(
            {L"Remove from Recently Played", [this, selected] { ForgetPlayStats(selected, true); }});
    else if (m_view.kind == View::Kind::AllMostPlayed)
        page.push_back({L"Remove from Most Played", [this, selected] { ForgetPlayStats(selected, false); }});
    if (!page.empty() && !page.front().label.empty()) actions.push_back({});
    for (auto& action : page)
        actions.push_back(std::move(action));
    return actions;
}

void MainWindow::RemoveFromNowPlaying() {
    // The selected rows, not every row with the same song: the queue can hold a song twice.
    const t_size queue = media::QueuePlaylist(false);
    if (queue == SIZE_MAX) return;
    auto pm = playlist_manager::get();
    const t_size count = pm->playlist_get_item_count(queue);
    if (!count) return;
    pfc::bit_array_bittable mask(count);
    pm->playlist_get_selection_mask(queue, mask);
    bool any = false;
    for (t_size i = 0; i < count && !any; ++i)
        any = mask[i];
    if (!any) { // a right-click on an unselected row acts on that row
        const t_size focus = pm->playlist_get_focus_item(queue);
        if (focus >= count) return;
        mask.set(focus, true);
    }
    t_size playingPlaylist = SIZE_MAX, playingIndex = SIZE_MAX;
    const bool playing = pm->get_playing_item_location(&playingPlaylist, &playingIndex);
    const bool removesPlaying =
        playing && playingPlaylist == queue && playingIndex < count && mask[playingIndex];
    const bool paused = playback_control::get()->is_paused();
    pm->playlist_undo_backup(queue);
    pm->playlist_remove_items(queue, mask);
    if (!removesPlaying) return;
    // Find the first surviving song after the old playing position and map its index
    // through the removals; if the playing song was last, continue with the new last.
    const t_size remaining = pm->playlist_get_item_count(queue);
    if (!remaining) {
        playback_control::get()->stop();
        return;
    }
    t_size removedBefore = 0;
    for (t_size i = 0; i <= playingIndex && i < count; ++i)
        if (mask[i]) ++removedBefore;
    t_size next = SIZE_MAX;
    for (t_size i = playingIndex + 1; i < count; ++i) {
        if (!mask[i]) {
            next = i - removedBefore;
            break;
        }
        ++removedBefore;
    }
    if (next == SIZE_MAX) next = remaining - 1;
    pm->playlist_set_focus_item(queue, next);
    pm->playlist_execute_default_action(queue, next);
    if (paused) playback_control::get()->pause(true); // the next song waits like the removed one did
}

void MainWindow::ClearNowPlaying() {
    const t_size queue = media::QueuePlaylist(false);
    if (queue == SIZE_MAX) return;
    auto pm = playlist_manager::get();
    t_size playingPlaylist = SIZE_MAX, playingIndex = SIZE_MAX;
    const bool playing =
        pm->get_playing_item_location(&playingPlaylist, &playingIndex) && playingPlaylist == queue;
    pm->playlist_undo_backup(queue);
    pm->playlist_clear(queue);
    if (playing) playback_control::get()->stop();
}

void MainWindow::ForgetPlayStats(metadb_handle_list_cref items, bool recent) {
    if (recent)
        media::PlayStats::Get().ForgetRecent(items);
    else
        media::PlayStats::Get().ClearCounts(items);
    const size_t top = m_list ? m_list->TopRow() : SIZE_MAX;
    Navigate(m_view, false);
    if (m_list && top != SIZE_MAX && m_source && m_source->Count())
        m_list->ScrollToRow(std::min(top, m_source->Count() - 1));
}

void MainWindow::ClearPlayStats(bool recent) {
    if (recent)
        media::PlayStats::Get().ForgetAllRecent();
    else
        media::PlayStats::Get().ClearAllCounts();
    if (m_view.kind == View::Kind::AllRecentPlayed || m_view.kind == View::Kind::AllMostPlayed)
        Navigate(m_view, false);
}

void MainWindow::RefreshPlayCounts() {
    if (!m_list) return;
    if (auto* source = dynamic_cast<ui::SongTableSource*>(m_source.get())) source->RefreshPlayCounts();
    m_list->Refresh(); // the local sources read the count from the store while painting
}

void MainWindow::ShowPageContextMenu(POINT screen) {
    std::vector<MenuAction> actions;
    // Local song pages act on their whole (filtered) contents; provider pages and the
    // combined favorites add the same commands themselves.
    const bool localPlaylist = m_view.kind == View::Kind::Playlist && !media::IsQueue(m_view.playlist);
    const bool songPage = localPlaylist || m_view.kind == View::Kind::AllRecentPlayed ||
                          m_view.kind == View::Kind::AllMostPlayed || m_view.kind == View::Kind::AllMusic ||
                          m_view.kind == View::Kind::Category || m_view.kind == View::Kind::Group;
    if (songPage && m_source && m_source->Count()) {
        metadb_handle_list items;
        for (size_t row = 0; row < m_source->Count(); ++row)
            if (const auto track = m_source->Item(row); track.is_valid()) items.add_item(track);
        if (items.get_count()) {
            AddBulkActions(actions, Ready(items), nullptr, {}, localPlaylist ? m_view.playlist : SIZE_MAX);
            if (download::AnyDownloadable(items)) AddBulkDownloadActions(actions, Ready(items), m_hwnd);
            actions.push_back({});
        }
    }
    if (m_source && m_source->Count()) // every list page can take the whole selection
        actions.push_back({L"Select all", [this] {
                               m_source->SelectAll();
                               m_list->Refresh();
                           }});
    std::vector<MenuAction> page;
    if (m_page) m_page->AddPageActions(page);
    if (m_view.kind == View::Kind::Playlist && media::IsQueue(m_view.playlist))
        page.push_back({L"Clear Now Playing", [this] { ClearNowPlaying(); }});
    else if (m_view.kind == View::Kind::AllRecentPlayed)
        page.push_back({L"Clear Recently Played", [this] { ClearPlayStats(true); }});
    else if (m_view.kind == View::Kind::AllMostPlayed)
        page.push_back({L"Clear Most Played", [this] { ClearPlayStats(false); }});
    actions.push_back({});
    for (auto& action : page)
        actions.push_back(std::move(action));
    if (CanRefreshPage()) actions.insert(actions.end(), {{}, RefreshAction(*this)});

    ShowActionMenu(m_hwnd, screen, actions); // stray separators are left out there
}

void MainWindow::ShowDiscMenu(wchar_t drive, POINT screen) {
    const auto* disc = cd::FindDrive(drive);
    if (!disc) return;
    std::vector<MenuAction> actions;
    if (disc->state == cd::CdDrive::State::Loaded) {
        const metadb_handle_list tracks = disc->tracks;
        actions.push_back({Tr(L"Play"), [tracks] { media::PlayQueue(tracks, 0); }});
        actions.push_back({});
    }
    actions.push_back({Tr(L"Eject"), [drive] { cd::Eject(drive); }});
    ShowActionMenu(m_hwnd, screen, actions);
}

void MainWindow::ShowRemoteNavMenu(const NavPane::Item& item, POINT screen) {
    stream::Provider& provider = *item.provider;
    std::vector<MenuAction> actions;
    switch (item.target) {
    case NavPane::Target::RemotePlaylist:
        ShowRemotePlaylistMenu(*this, provider, item.remoteId, item.text, item.sectionShelf, item.sectionId,
                               screen);
        return;
    default:
        if (item.target == NavPane::Target::LibraryRemote && provider.Can(stream::Capability::CatalogSearch))
            actions.push_back(SearchProviderAction(provider));
        if (provider.Can(stream::Capability::CreatePlaylists) && provider.AccountVerified() &&
            FindProviderSection(provider, stream::SectionKind::PlaylistShelf, stream::Shelf::Created) &&
            (item.target == NavPane::Target::LibraryRemote ||
             (item.target == NavPane::Target::RemoteSection &&
              item.sectionKind == stream::SectionKind::PlaylistShelf &&
              item.sectionShelf == stream::Shelf::Created)))
            actions.push_back(NewPlaylistAction(*this, provider));
        if (item.target == NavPane::Target::LibraryRemote) {
            if (provider.AccountVerified())
                actions.push_back({Tr(L"Sign out"), [&provider] { provider.SignOut(); }});
            else if (!provider.SessionRestoring())
                actions.push_back({Tr(L"Sign in..."), [this, &provider] { provider.SignIn(m_hwnd); }});
        }
        if (provider.AccountVerified()) // re-read the right-clicked section, not just the page on screen
            actions.push_back({L"Refresh", [this, item] {
                                   RefreshProviderLibrary(*item.provider);
                                   m_forcePageRefresh = true;
                                   OnNavItem(item);
                               }});
        break;
    }
    if (!actions.empty()) ShowActionMenu(m_hwnd, screen, actions);
}

void MainWindow::RefreshCurrentPage() {
    if (!CanRefreshPage()) return;
    m_page->RefreshRequested();
    // Only the refreshed source's pictures are revalidated (RefreshProviderLibrary).
    if (auto* provider = ViewProvider())
        RefreshProviderLibrary(*provider);
    else
        ArtCache::Get().Invalidate(L"");
    m_forcePageRefresh = true;
    Navigate(m_view, false);
    if (m_list) m_list->Refresh();
}

cfg_var_modern::cfg_int* MainWindow::ViewStyleSetting(const View& view) {
    switch (view.kind) {
    case View::Kind::AllMusic:
    case View::Kind::Group:
        return &cfg_viewSongs;
    case View::Kind::Playlist:
    case View::Kind::AudioCd:
        return &cfg_viewPlaylist;
    case View::Kind::AllFavorites:
        return &cfg_viewAllFavorites;
    case View::Kind::AllRecentPlayed:
    case View::Kind::AllMostPlayed:
        return &cfg_viewPlayStats;
    case View::Kind::RemoteShelf:
        return &cfg_viewRemotePlaylists;
    case View::Kind::RemotePlaylist:
    case View::Kind::RemoteFavorites:
    case View::Kind::RemoteAlbum:
    case View::Kind::RemoteTopTracks:
    case View::Kind::RemoteSectionTracks:
    case View::Kind::RemoteGroupTracks:
    case View::Kind::RemoteHomeList:
        return &cfg_viewRemoteSongs;
    case View::Kind::Search:
        if (view.sectionId == L"songs" || view.sectionId == L"lyrics") return &cfg_viewSearchSongs;
        return view.sectionId.empty() ? nullptr : &cfg_viewSearchTiles;
    case View::Kind::RemoteArtist:
        return &cfg_viewRemoteArtist;
    case View::Kind::RemoteArtists:
    case View::Kind::RemoteGroups:
    case View::Kind::RemoteSong:
        return nullptr;
    case View::Kind::RemoteAlbums:
        return &cfg_viewRemoteAlbums;
    case View::Kind::Category:
        switch (view.group) {
        case media::GroupKind::Album:
            return &cfg_viewAlbums;
        case media::GroupKind::Artist:
            return &cfg_viewArtists;
        case media::GroupKind::Genre:
            return &cfg_viewGenres;
        default:
            return &cfg_viewCategories;
        }
    default:
        return nullptr;
    }
}

int MainWindow::ViewStyles() const {
    int n = 0;
    for (auto s : {media::ViewStyle::Icons, media::ViewStyle::Tiles, media::ViewStyle::Details})
        if (m_source && m_source->SupportsViewStyle(s)) ++n;
    return n;
}

void MainWindow::ShowViewMenu() {
    if (!m_source) return;
    HMENU menu = CreatePopupMenu();
    const std::pair<media::ViewStyle, const wchar_t*> items[] = {{media::ViewStyle::Icons, L"&Icon"},
                                                                 {media::ViewStyle::Tiles, L"&Expanded tile"},
                                                                 {media::ViewStyle::Details, L"&Details"}};
    const bool browse = m_page && m_page->StyleRebuildsPage();
    for (const auto& [style, label] : items) {
        if (browse && style == media::ViewStyle::Details) continue; // not an album-browsing layout
        UINT flags = MF_STRING;
        if (!browse && !m_source->SupportsViewStyle(style)) flags |= MF_GRAYED;
        AppendMenuW(menu, flags, static_cast<UINT_PTR>(style) + 1, label);
    }
    // WMP 12: each view with its icon (strip 331 slots 57 icon, 56 expanded tile, 55 details),
    // the current one in bold.
    const UINT current = static_cast<UINT>(m_source->GetViewStyle()) + 1;
    SetMenuDefaultItem(menu, current, FALSE);
    const std::map<UINT, int> icons{{static_cast<UINT>(media::ViewStyle::Icons) + 1, 57},
                                    {static_cast<UINT>(media::ViewStyle::Tiles) + 1, 56},
                                    {static_cast<UINT>(media::ViewStyle::Details) + 1, 55}};
    POINT pt{m_layout.viewOptions.left, m_layout.viewOptions.bottom};
    ClientToScreen(m_hwnd, &pt);
    const UINT cmd = TrackWin7Menu(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_LEFTALIGN | TPM_TOPALIGN, pt.x,
                                   pt.y, m_hwnd, &icons);
    DestroyMenu(menu);
    if (cmd == 0 || cmd == current) return;
    ApplyViewStyle(static_cast<media::ViewStyle>(cmd - 1));
}

void MainWindow::ApplyViewStyle(media::ViewStyle style) {
    // The artist and collected-albums pages swap between cover tiles and a song list
    // grouped by album, so they rebuild the view instead of restyling the current source.
    if (m_page && m_page->StyleRebuildsPage()) {
        if (style == media::ViewStyle::Details) return;
        if (auto* setting = ViewStyleSetting(m_view)) setting->set(static_cast<int>(style));
        Navigate(m_view, false);
        if (auto* w = WidgetById(kViewOptions)) w->Invalidate();
        return;
    }
    m_source->SetViewStyle(style);
    if (auto* setting = ViewStyleSetting(m_view)) setting->set(static_cast<int>(style));
    m_list->SetSource(m_source.get(), true);
    if (m_page) m_page->OnPresentationChanged();
    if (auto* w = WidgetById(kViewOptions)) w->Invalidate();
}

void MainWindow::CycleViewStyle() {
    // WMP 12: clicking the view icon steps Icon -> Expanded tile -> Details -> Icon,
    // skipping styles the current view does not offer.
    if (!m_source) return;
    if (m_page && m_page->StyleRebuildsPage()) {
        const bool expanded = m_source->GetViewStyle() == media::ViewStyle::Tiles;
        ApplyViewStyle(expanded ? media::ViewStyle::Icons : media::ViewStyle::Tiles);
        return;
    }
    int s = static_cast<int>(m_source->GetViewStyle());
    for (int i = 0; i < 3; ++i) {
        s = (s + 1) % 3;
        if (m_source->SupportsViewStyle(static_cast<media::ViewStyle>(s))) {
            ApplyViewStyle(static_cast<media::ViewStyle>(s));
            return;
        }
    }
}

// foobar2000 puts files opened from outside (Explorer, the command line) into the active
// playlist, and into a new "New Playlist" when that one is locked (a smart playlist). They
// belong in Now Playing, so the queue stays active; only an explicit Add files / folder /
// location while a regular playlist is shown targets that playlist.
void MainWindow::KeepQueueActive() {
    auto pm = playlist_manager::get();
    const t_size queue = media::QueuePlaylist(true);
    if (queue != SIZE_MAX && pm->get_active_playlist() != queue) pm->set_active_playlist(queue);
}

void MainWindow::ActivateAddTarget() {
    if (m_view.kind == View::Kind::Playlist &&
        m_view.playlist < playlist_manager::get()->get_playlist_count() &&
        !media::IsAutoPlaylist(m_view.playlist))
        playlist_manager::get()->set_active_playlist(m_view.playlist);
    else
        KeepQueueActive();
}

void MainWindow::CreatePlaylist(bool autoPlaylist) {
    auto pm = playlist_manager::get();
    const t_size p =
        pm->create_playlist(autoPlaylist ? "New Auto Playlist" : "New Playlist", SIZE_MAX, SIZE_MAX);
    if (p == SIZE_MAX) return;
    pm->set_active_playlist(p);
    View v;
    v.library = View::Library::All;
    v.kind = View::Kind::Playlist;
    v.playlist = p;
    Navigate(v, true);
    if (!autoPlaylist) return;
    // foobar2000 auto playlist: starts as the whole library, its own dialog edits the query.
    try {
        auto apm = autoplaylist_manager::get();
        apm->add_client_simple("ALL", "", p, 0);
        if (auto client = apm->query_client(p); client.is_valid()) client->show_ui(p);
    } catch (const std::exception& e) {
        popup_message::g_show(e.what(), "Create auto playlist");
    }
}

} // namespace glasswave::ui
