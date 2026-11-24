#include "pch.h"
#include "ui/main_window_parts.h"

// What the library window does when a streaming provider's account, library, favorites,
// language or display changes, and when discs come and go.
namespace glasswave::ui {

using namespace mainwin;

void MainWindow::OnProviderAccount(stream::Provider& provider) {
    const stream::Id providerKey{provider.Key()};
    InvalidateRemotePageCache(providerKey);
    for (stream::ArtKind kind :
         {stream::ArtKind::Track, stream::ArtKind::Album, stream::ArtKind::Artist, stream::ArtKind::Playlist,
          stream::ArtKind::Station})
        ArtCache::Get().Forget(provider.ArtKeyPrefix(kind));
    RefreshProviderSections();
    UpdateFavoriteState();
    const bool currentProvider = m_view.IsRemote() && m_view.provider == providerKey;
    const bool currentAllFavorites = m_view.kind == View::Kind::AllFavorites;
    View replacement = m_view;
    if (currentProvider) {
        if (provider.SignedIn()) {
            replacement = ProviderLandingView(provider);
        } else {
            replacement = {};
            replacement.library = View::Library::All;
            replacement.kind = View::Kind::AllMusic;
        }
    }
    // Drop another account's private pages and names from Back/Forward, replacing the
    // current entry when it belongs to the provider that changed.
    std::vector<View> history;
    size_t historyPos = SIZE_MAX;
    auto sameProvider = [&providerKey](const View& view) {
        return view.IsRemote() && view.provider == providerKey;
    };
    for (size_t i = 0; i < m_history.size(); ++i) {
        if (i == m_historyPos) {
            historyPos = history.size();
            history.push_back(sameProvider(m_history[i]) ? replacement : m_history[i]);
        } else if (!sameProvider(m_history[i])) {
            history.push_back(m_history[i]);
        }
    }
    if (history.empty()) {
        history.push_back(replacement);
        historyPos = 0;
    } else if (historyPos == SIZE_MAX) {
        historyPos = std::min(m_historyPos, history.size() - 1);
    }
    m_history = std::move(history);
    m_historyPos = historyPos;
    m_pendingScrollKey.clear();
    DiscardProviderScrollState(providerKey);

    // Signing in or out changes what the provider's pages and the combined favorites show.
    if (currentProvider || currentAllFavorites) {
        m_forcePageRefresh = true;
        Navigate(currentProvider ? replacement : m_view, false);
        DiscardProviderScrollState(providerKey);
        m_pendingScrollKey.clear();
    }
    if (m_list) m_list->Refresh();
    RepaintBreadcrumb();
}

void MainWindow::OnProviderAccountObserved(stream::Provider& provider) {
    RefreshProviderSections();
    UpdateFavoriteState();
    const bool currentProvider = m_view.IsRemote() && m_view.provider == provider.Key();
    // The combined favorites list the providers that are signed in now.
    if ((currentProvider || m_view.kind == View::Kind::AllFavorites) && m_page) m_page->OnLibraryChanged();
    if ((currentProvider || m_view.kind == View::Kind::AllFavorites) && m_list) m_list->Refresh();
    RepaintBreadcrumb();
    RepaintDetailHeader();
}

void MainWindow::OnProviderLibrary(stream::Provider& provider) {
    // Background count updates repeat this event; retry only art that never loaded.
    for (stream::ArtKind kind :
         {stream::ArtKind::Track, stream::ArtKind::Album, stream::ArtKind::Artist, stream::ArtKind::Playlist,
          stream::ArtKind::Station})
        ArtCache::Get().InvalidateMissing(provider.ArtKeyPrefix(kind));
    RefreshProviderSections();
    metadb_handle_ptr now;
    if (playback_control::get()->get_now_playing(now)) {
        if (const auto id = provider.TrackId(now); !id.empty()) {
            if (const auto track = provider.FindTrack(id);
                track &&
                (track->title != m_trackTitle || stream::JoinArtists(track->artists) != m_trackArtist))
                OnTrackChanged();
        }
    }
    if (m_page && m_view.provider == provider.Key()) m_page->OnLibraryChanged();
    if (m_list) m_list->Refresh();
    RepaintBreadcrumb();
    RepaintDetailHeader();
}

void MainWindow::OnProviderFavorites(stream::Provider&) {
    UpdateFavoriteState(); // the taskbar and now-playing star follow the playing song
    if (!m_source || !m_list) return;
    if (m_page) {
        m_page->OnFavoritesChanged();
    } else if (auto* source = dynamic_cast<SongTableSource*>(m_source.get())) {
        source->RefreshFavorites();
        m_list->Refresh();
    } else {
        m_list->Refresh();
    }
}

void MainWindow::OnProviderLanguage(stream::Provider& provider) {
    const stream::Id providerKey{provider.Key()};
    // Names change with the language; pictures do not, so the art stays as it is.
    InvalidateRemotePageCache(providerKey);
    RefreshProviderSections();
    auto refresh = [&provider, &providerKey](View& view) {
        if (view.IsRemote() && view.provider == providerKey)
            view = NormalizeRemoteView(provider, std::move(view), true);
    };
    refresh(m_view);
    for (auto& view : m_history)
        refresh(view);
    DiscardProviderScrollState(providerKey);
    m_pendingScrollKey.clear();
    if ((m_view.IsRemote() && m_view.provider == providerKey) || m_view.kind == View::Kind::AllFavorites) {
        m_forcePageRefresh = true;
        Navigate(m_view, false);
        DiscardProviderScrollState(providerKey);
        m_pendingScrollKey.clear();
    } else if (m_list) {
        m_list->Refresh();
    }
    RepaintBreadcrumb();
}

void MainWindow::OnProviderDisplay(stream::Provider& provider) {
    const stream::Id providerKey{provider.Key()};
    // Drop cached page objects from the old projection, but keep account, library and artwork
    // caches. The current page reopens from canonical IDs and publishes its cached prefix first.
    InvalidateRemotePageCache(providerKey);
    RefreshProviderSections();
    auto refresh = [&provider, &providerKey](View& view) {
        if (view.IsRemote() && view.provider == providerKey)
            view = NormalizeRemoteView(provider, std::move(view), true);
    };
    refresh(m_view);
    for (auto& view : m_history)
        refresh(view);
    if ((m_view.IsRemote() && m_view.provider == providerKey) || m_view.kind == View::Kind::AllFavorites ||
        m_view.kind == View::Kind::Home) {
        m_forcePageRefresh = true;
        Navigate(m_view, false);
    } else if (m_list) {
        m_list->Refresh();
    }
    OnTrackChanged();
    RepaintBreadcrumb();
}

void MainWindow::DiscardProviderScrollState(const stream::Id& provider) {
    if (const auto pending = m_scrollPositions.find(m_pendingScrollKey);
        pending != m_scrollPositions.end() && pending->second.provider == provider)
        m_pendingScrollKey.clear();
    std::erase_if(m_scrollPositions,
                  [&provider](const auto& entry) { return entry.second.provider == provider; });
}

stream::Provider* MainWindow::ViewProvider() const {
    return m_view.IsRemote() ? stream::FindProvider(m_view.provider) : nullptr;
}

void MainWindow::RefreshProviderSections() {
    if (!m_nav) return;
    std::vector<NavPane::ProviderSection> sections;
    for (auto* provider : stream::Providers()) {
        NavPane::ProviderSection section;
        section.provider = provider;
        section.signedIn = provider->SignedIn() || provider->SessionRestoring();
        section.sections = provider->Sections();
        if (section.signedIn) {
            for (const auto& navigation : section.sections)
                if (navigation.kind == stream::SectionKind::PlaylistShelf &&
                    section.playlists.find(navigation.id) == section.playlists.end())
                    section.playlists[navigation.id] = provider->SectionPlaylists(navigation.id);
        }
        sections.push_back(std::move(section));
    }
    m_nav->SetProviders(std::move(sections));
}

void MainWindow::OnDiscsChanged() {
    if (!m_nav) return;
    std::vector<NavPane::DiscRow> rows;
    for (const auto& drive : cd::Drives())
        rows.push_back({drive.drive, cd::DriveTitle(drive)});
    m_nav->SetDiscs(std::move(rows));
    if (m_view.kind != View::Kind::AudioCd) return;
    if (cd::FindDrive(static_cast<wchar_t>(m_view.playlist))) {
        Navigate(m_view, false); // read, tagged or taken out: the disc's name and rows
        return;
    }
    View queue; // the drive itself went (an external drive unplugged)
    queue.library = View::Library::All;
    queue.kind = View::Kind::Playlist;
    queue.playlist = media::QueuePlaylist(true);
    Navigate(queue, true);
}

} // namespace glasswave::ui
