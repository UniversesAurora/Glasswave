#include "pch.h"
#include "ui/main_window_parts.h"

// The library window as the player window's host, and the glass's colour.
namespace glasswave::ui {

using namespace mainwin;

aero::Colorization MainWindow::WindowColorization() const {
    const int tint = settings::Tint();
    uint32_t rgb = aero::kWin7Tints[0].rgb; // following the cover before any: Sky
    if (tint < static_cast<int>(aero::kWin7Tints.size()))
        rgb = aero::kWin7Tints[static_cast<size_t>(tint)].rgb;
    else if (m_windowCoverRgb)
        rgb = *m_windowCoverRgb;
    return aero::Win7Colorization(rgb, settings::TintIntensity());
}

void MainWindow::ApplyTint(bool fade) {
    const aero::Colorization c = WindowColorization();
    const int ms = fade ? 600 : 0;
    if (m_frame) m_frame->Glass().SetColorization(c, ms);
    if (m_player) m_player->SetColorization(c, ms);
}

aero::Colorization MainWindow::PlayerColorization() const { return WindowColorization(); }

void MainWindow::ShowPlayer(bool fullScreen) {
    if (!m_pane) return;
    if (!m_player) m_player = std::make_unique<PlayerWindow>(*this);
    m_player->Show(fullScreen ? PlayerWindow::Mode::Full : PlayerWindow::Mode::Mini);
    if (IsWindowVisible(m_hwnd)) {
        ShowWindow(m_hwnd, SW_HIDE);
        m_shadow.Sync();
    }
}

NowPlayingPane* MainWindow::LendPane(const wuc::ContainerVisual& parent) {
    if (!m_pane) return nullptr;
    m_paneLent = true;
    m_pane->SetVisible(false);
    m_pane->Reparent(parent);
    m_pane->SetEmbedded(true);
    RefreshPaneQueue();
    return m_pane.get();
}

void MainWindow::ReturnPane() {
    if (!m_paneLent) return;
    m_paneLent = false;
    if (!m_pane) return;
    m_pane->SetVisible(false);
    m_pane->SetEmbedded(false);
    m_pane->Reparent(m_paneSlot);
}

void MainWindow::PlayerClosed() {
    if (!m_hwnd) return;
    if (!IsWindowVisible(m_hwnd)) ShowWindow(m_hwnd, SW_SHOW);
    m_timeMeasuredFor = -1;
    UpdateLayout();
    RepaintNowPlaying();
    m_shadow.Sync();
    SetForegroundWindow(m_hwnd);
}

void MainWindow::SetPlayerVolume(float slider) {
    m_volume = std::clamp(slider, 0.0f, 1.0f);
    for (int id : {kVolume, kMute})
        if (auto* w = WidgetById(id)) w->Invalidate();
    playback_control::get()->set_volume(DbFromSlider(m_volume));
}

void MainWindow::OnSwitching(bool switching, const metadb_handle_ptr& opened) {
    // The start command and the stop for it arrive in either order around the input's
    // announcement; a switch already under way keeps the song it is opening.
    if (m_pane) m_pane->SetSwitching(switching);
    if (switching) {
        if (!m_switching) m_openingTrack.release();
        m_switching = true;
        // If the decoder already gaplessly pre-opened the next track before the current song's
        // output buffer drained, promote it now so cover, title and lyrics switch immediately.
        if (!m_openingTrack.is_valid() && m_preopeningTrack.is_valid()) {
            metadb_handle_ptr now;
            const bool sameAsPlaying =
                (playback_control::get()->get_now_playing(now) && now == m_preopeningTrack) ||
                m_preopeningTrack == m_shownTrack;
            if (!sameAsPlaying) {
                m_openingTrack = std::move(m_preopeningTrack);
                PreviewTrack(m_openingTrack, std::nullopt);
                if (m_player) m_player->OnPlayback();
            } else {
                m_preopeningTrack.release();
            }
        }
        return;
    }
    m_switching = false;
    m_startingFrom.clear();
    m_openingTrack.release();
    m_preopeningTrack.release();
    // A real stop forgets the last song: the player shows nothing playing, as before.
    m_shownTrack = opened;
    if (opened.is_valid()) {
        m_lastCoreIdentity = lyrics::Service::TrackIdentity(opened);
        m_lastCorePos = 0;
        if (m_pane) m_pane->OnTrackStarted(m_lastCoreIdentity, 0.0);
        if (m_player) m_player->OnTrackStarted();
    } else {
        m_lastCoreIdentity.clear();
        m_lastCorePos = 0;
    }
}

// Gapless playback opens the next song while this one still plays; remember it for the
// upcoming switch and warm its lyrics, switching the UI only once the switch is under way.
void MainWindow::OnTrackOpening(const std::string& path) {
    metadb_handle_ptr track;
    metadb::get()->handle_create(track, make_playable_location(path.c_str(), 0));
    if (!track.is_valid()) return;
    metadb_handle_ptr now;
    const bool hasNow = playback_control::get()->get_now_playing(now);
    if (hasNow && now == track) return; // already playing it
    if (!m_switching) {
        if (track != m_preopeningTrack) {
            m_preopeningTrack = track;
            lyrics::Service::Get().Prefetch(track);
        }
        return;
    }
    if (track == m_openingTrack) return;
    m_openingTrack = track;
    m_preopeningTrack.release();
    PreviewTrack(track, std::nullopt);
    if (m_player) m_player->OnPlayback();
}

metadb_handle_ptr MainWindow::PlayerTrack() const {
    metadb_handle_ptr now;
    if (playback_control::get()->get_now_playing(now)) return now;
    if (!m_switching) return {};
    return m_openingTrack.is_valid() ? m_openingTrack : m_shownTrack;
}

void MainWindow::PlayerTrackText(const metadb_handle_ptr& track, std::wstring& title, std::wstring& artist,
                                 std::wstring& album) const {
    title.clear();
    artist.clear();
    album.clear();
    if (!track.is_valid()) return;
    auto pc = playback_control::get();
    metadb_handle_ptr now;
    if (pc->get_now_playing(now) && now == track) {
        title = m_trackTitle;
        artist = m_trackArtist;
        static titleformat_object::ptr s_album;
        if (!s_album) titleformat_compiler::get()->compile_safe(s_album, "[%album%]");
        pfc::string8 text;
        pc->playback_format_title(nullptr, text, s_album, nullptr, playback_control::display_level_all);
        album = Wide(text);
    } else {
        // Not decoding (yet, or any more): the library's tags, as the song table shows them.
        static titleformat_object::ptr s_text;
        if (!s_text) titleformat_compiler::get()->compile_safe(s_text, "$if2(%title%,%filename%)|[%artist%]|[%album%]");
        pfc::string8 text;
        track->format_title(nullptr, text, s_text, nullptr);
        std::wstringstream in(Wide(text));
        std::getline(in, title, L'|');
        std::getline(in, artist, L'|');
        std::getline(in, album);
        if (const media::RepairedText* r = media::CueCatalog::Get().Repair(track)) {
            if (!r->title.empty()) title = r->title;
            if (!r->artist.empty()) artist = r->artist;
        }
    }
    if (auto* provider = stream::ProviderOf(track)) {
        if (const auto found = provider->FindTrack(provider->TrackId(track))) {
            if (!found->title.empty()) title = found->title;
            if (!found->artists.empty()) artist = stream::JoinArtists(found->artists);
            if (!found->album.empty()) album = found->album;
        }
    }
}

void MainWindow::TogglePlayerFavorite() {
    if (m_taskbar && m_taskbar->onFavorite) m_taskbar->onFavorite();
}

void MainWindow::AdoptPlayerWindow(HWND window) {
    const UINT dpi = GetDpiForWindow(window);
    for (auto [which, metric] : {std::pair{ICON_SMALL, SM_CXSMICON}, std::pair{ICON_BIG, SM_CXICON}}) {
        const int size = GetSystemMetricsForDpi(metric, dpi);
        HICON icon = which == ICON_BIG ? CreateCenteredAppIcon(size) : CreateAppIcon(size);
        if (HICON old = reinterpret_cast<HICON>(SendMessageW(window, WM_SETICON, which, reinterpret_cast<LPARAM>(icon))))
            if (old != icon) DestroyIcon(old);
    }
    m_playerTaskbar = std::make_unique<Taskbar>(window);
    m_playerTaskbar->onFavorite = [this] {
        if (m_taskbar && m_taskbar->onFavorite) m_taskbar->onFavorite();
    };
    m_playerTaskbar->SetPlayState(m_playing, m_paused);
    m_playerTaskbar->SetFavorite(m_npFavorite);
    metadb_handle_ptr now;
    if (!m_artTrackIdentity.empty() && playback_control::get()->get_now_playing(now))
        m_playerTaskbar->SetCoverTrack(now);
}

bool MainWindow::PlayerWindowMessage(UINT msg, WPARAM wp, LPARAM lp) {
    return m_playerTaskbar && m_playerTaskbar->OnMessage(msg, wp, lp);
}

// One track per album of the playing playlist (else the active one), each album once. A
// streamed track names its album's picture (the art cache's key); a local file belongs to its
// album artist and album, or to its folder when it has no album tag.
std::vector<metadb_handle_ptr> MainWindow::WallAlbums() const {
    std::vector<metadb_handle_ptr> albums;
    auto pm = playlist_manager::get();
    size_t playlist = pm->get_playing_playlist();
    if (playlist == SIZE_MAX) playlist = pm->get_active_playlist();
    if (playlist == SIZE_MAX) return albums;
    metadb_handle_list items;
    pm->playlist_get_all_items(playlist, items);
    static titleformat_object::ptr s_album;
    if (!s_album) titleformat_compiler::get()->compile_safe(s_album, "$if(%album%,%album artist%|%album%)");
    std::unordered_set<std::wstring> seen;
    pfc::string8 text;
    for (size_t i = 0; i < items.get_count(); ++i) {
        const metadb_handle_ptr& track = items[i];
        std::wstring key;
        if (stream::ProviderOf(track)) {
            key = L"art:" + ArtCache::SourceKey(track);
        } else {
            track->format_title(nullptr, text, s_album, nullptr);
            if (!text.is_empty()) {
                key = L"album:" + std::wstring(pfc::stringcvt::string_wide_from_utf8(text.get_ptr()).get_ptr());
            } else {
                std::wstring path = pfc::stringcvt::string_wide_from_utf8(track->get_path()).get_ptr();
                path.resize(path.find_last_of(L"/\\") == std::wstring::npos ? 0 : path.find_last_of(L"/\\"));
                key = L"folder:" + path;
            }
        }
        if (seen.insert(std::move(key)).second) albums.push_back(track);
    }
    return albums;
}

} // namespace glasswave::ui
