#pragma once
#include "aero/glass_window.h"
#include "aero/shadow_frame.h"
#include "core/signal.h"
#include "gfx/frame_clock.h"
#include "lyrics/lyrics_service.h"
#include "media/library_model.h"
#include "media/list_sources.h"
#include "media/playback_scheduler.h"
#include "stream/provider.h"
#include "ui/list_view.h"
#include "ui/now_playing_pane.h"
#include "ui/main_menu.h"
#include "ui/i18n.h"
#include "ui/nav_pane.h"
#include "ui/breadcrumb.h"
#include "ui/page.h"
#include "ui/scroll_bar.h"
#include "ui/player_window.h"
#include "ui/search_box.h"
#include "ui/search_popup.h"
#include "ui/search_scope.h"
#include "ui/widget.h"
#include "ui/win7_menu.h"

#include <cstdint>
#include <optional>
#include <unordered_map>

namespace glasswave::ui {

// The Glasswave main window, laid out like WMP 12 library mode: Aero glass caption and
// frame, a light client area (navigation bar, command bar, navigation pane + list,
// transport band), all presented through a Windows.UI.Composition visual tree.
class MainWindow : public PageHost, public PlayerHost {
  public:
    MainWindow();
    ~MainWindow();
    MainWindow(const MainWindow&) = delete;
    MainWindow& operator=(const MainWindow&) = delete;

    HWND Create(user_interface::HookProc_t hook);
    void Destroy();
    HWND Hwnd() const { return m_hwnd; }
    // The composition tree's root, for the dev switch that logs what is on screen.
    const winrt::Windows::UI::Composition::ContainerVisual& VisualRoot() const { return m_windowRoot; }
    static MainWindow* Instance(); // the window while it exists (main menu commands)
    void ReapplyLyrics() { OnLyrics(m_lyricsResult); } // after Swap original and translation changed
    void OnCoreQueueChanged() { SchedulePaneQueueRefresh(); }
    void SelectPlaybackMode(bool shuffle, int repeat);

    // The lyrics pane (View > Lyrics, the transport band's lyrics button).
    void ToggleLyricsPane() { ToggleLyrics(); }
    bool LyricsPaneOpen() const { return m_lyricsOpen; }
    // View > Up Next: the pane on its queue side (again: back to the lyrics).
    void ToggleQueuePane();
    bool QueuePaneShown() const {
        return m_lyricsOpen && m_pane && m_pane->CurrentMode() == NowPlayingPane::Mode::Queue;
    }

    // The player window (View > Mini player / Full screen): the library hides while it shows.
    void ShowPlayer(bool fullScreen);
    bool PlayerShown(bool fullScreen) const {
        return m_player && m_player->Shown() &&
               (m_player->CurrentMode() == PlayerWindow::Mode::Full) == fullScreen;
    }
    // PlayerHost
    NowPlayingPane* LendPane(const winrt::Windows::UI::Composition::ContainerVisual& parent) override;
    void ReturnPane() override;
    void PlayerClosed() override;
    float PlayerVolume() const override { return m_volume; }
    void SetPlayerVolume(float slider) override;
    std::vector<metadb_handle_ptr> WallAlbums() const override;
    bool OriginalTransportArt() const override { return m_win7Art; }
    metadb_handle_ptr PlayerTrack() const override;
    void PlayerTrackText(const metadb_handle_ptr& track, std::wstring& title, std::wstring& artist,
                         std::wstring& album) const override;
    void PlayerMode(bool& shuffle, int& repeat) const override {
        shuffle = m_shuffle;
        repeat = static_cast<int>(m_repeat);
    }
    void SetPlayerMode(bool shuffle, int repeat) override { SelectPlaybackMode(shuffle, repeat); }
    std::shared_ptr<const lyrics::Lyrics> PlayerLyrics() const override { return m_npLyrics; }
    bool PlayerFavorite() const override { return m_npFavorite; }
    void TogglePlayerFavorite() override;
    void ShowPlayerLyricsMenu(POINT screen) override { ShowLyricsMenu(screen); }
    void ShowPlayerSongMenu(POINT screen) override { ShowNowPlayingSongMenu(screen); }
    void AdoptPlayerWindow(HWND window) override;
    aero::Colorization PlayerColorization() const override;
    bool PlayerWindowMessage(UINT msg, WPARAM wp, LPARAM lp) override;

    // Forwarded from the core callbacks.
    // Playback switches songs from the stop / start command until the new song is decoding;
    // `opened` is the song playing, if one is.
    void OnSwitching(bool switching, const metadb_handle_ptr& opened = {});
    void OnTrackOpening(const std::string& path); // a provider input began opening a song
    void OnPlaybackStateChanged();
    void OnTrackChanged(bool playbackChanged = false);
    void OnPlaybackTime();
    void OnVolumeChanged();
    void OnPlaybackOrderChanged();
    void OnPlaylistContentChanged(size_t playlist);
    void OnPlaylistSelectionChanged(size_t playlist);
    void OnPlaylistsChanged();
    // The set of playlists changed: remap(old index) is the new index, SIZE_MAX if deleted.
    void OnPlaylistsMoved(const std::function<size_t(size_t)>& remap);
    void OnPlaylistLockChanged(); // auto playlists attach as locks, also after start-up
    void ScheduleLibraryReload();

    // PageHost: what the page on screen may do with the window.
    const media::LibraryModel& Library() const override { return m_library; }
    const View& CurrentView() const override { return m_view; }
    // A page opened from inside the one on screen keeps that page's place in the breadcrumb
    // (View::from); a section of the navigation pane always stands in its own place.
    void Open(const View& view) override;
    void Replace(const View& view) override; // the same page in another state: its place stays
    void Reload() override { Navigate(m_view, false); }
    void Search(const std::wstring& term) override;
    void Refresh() override { RefreshCurrentPage(); }
    void SetPageName(const std::wstring& name) override;
    void ShowSource(std::unique_ptr<media::ListSource> source) override;
    media::ListSource* Source() const override { return m_source.get(); }
    void BeforeListChange() override;
    void ListChanged() override;
    void ChromeChanged() override;
    media::ViewStyle SavedStyle(media::ViewStyle fallback) const override;
    HWND Owner() const override { return m_hwnd; }
    std::weak_ptr<int> Lifetime() const override { return m_alive; }

  private:
    std::wstring m_pageAnchor;
    size_t m_pageAnchorOrdinal = 0;
    size_t m_pageAnchorRow = SIZE_MAX;
    float m_pageAnchorOffset = 0; // DIP from the view's top to the anchor row (ListView::RowOffset)
    struct Layout {
        int border = 0;
        RECT caption{}, client{}, clientEdge{}, navBar{}, cmdBar{}, content{}, transport{};
        RECT capMin{}, capMax{}, capClose{}, icon{}, title{};
        RECT back{}, forward{}, breadcrumb{}, navGroove{};
        RECT organize{}, addFiles{}, addFolder{}, newPlaylist{}, search{}, viewOptions{}, refresh{}, download{}, localPlay{};
        RECT navPane{}, list{};
        RECT tabs{}; // the page's tabs (the search page), above the list
        RECT detailHeader{}, detailAction{}; // NetEase detail pages: artist, album, playlist
        RECT detailBriefToggle{};
        RECT detailBriefArea{}; // text area of the description (wheel), client px
        RECT detailBriefBar{};  // its Win7 scrollbar, client px; empty when not scrollable
        RECT seek{}, capsule{}, divider{}, shuffle{}, repeat{}, stop{}, prev{}, next{}, play{}, mute{},
            volume{};
        RECT volArrow{}, volFlyout{}; // a narrow window's folded volume: the arrow and its flyout
        RECT cmdMore{};               // the command bar's » for buttons that no longer fit
        // How far the transport has given way to a narrow window: 0 everything, 1 without
        // shuffle / repeat, 2 the volume folded behind the speaker's arrow, 3 without stop.
        int transportLevel = 0;
        bool timeLeft = false; // the time stands left of the capsule
        float capsuleLeft = 0, capsuleRight = 0; // its ends, cluster DIP
        RECT nowPlaying{}, time{}, stream{};
        RECT lyricsPane{}, lyricsToggle{}; // lyrics pane on the glass right of the client area
        POINT cluster{};                   // origin of the WMP 12 transport cluster bitmap
    };

    // Breadcrumb (WMP 12 address bar): the window gives it the path; its look and behaviour
    // are the component's (ui/breadcrumb.h).
    using Crumb = Breadcrumb::Level;
    std::unique_ptr<Breadcrumb> m_crumbBar;
    std::wstring m_loggedPath; // the path last written to the log
    // One navigation model for every library: a path of levels, each with its own children
    // menu. Structural levels always get a menu (the arrow also separates the crumbs).
    std::vector<Crumb> BuildCrumbPath(const View& view); // its opener's path, then its own level
    std::vector<Crumb> PlacePath(const View& view);      // a page's own place, from the root
    std::vector<MenuAction> LibraryMenu();
    std::vector<MenuAction> AllSectionsMenu();
    std::vector<MenuAction> LocalSectionsMenu();
    bool LocalPlayShown() const;
    MenuAction SearchProviderAction(stream::Provider& provider); // "Search <provider>"
    std::vector<MenuAction> ProviderSectionsMenu(stream::Provider& provider);
    std::vector<MenuAction> PlaylistListMenu(bool smart);
    std::vector<MenuAction> GroupListMenu(media::GroupKind kind);
    std::vector<MenuAction> ShelfMenu(stream::Provider& provider, const stream::NavigationSection& section);
    std::vector<MenuAction> FollowedArtistsMenu(stream::Provider& provider, const stream::Id& sectionId);
    std::vector<MenuAction> SavedAlbumsMenu(stream::Provider& provider, const stream::Id& sectionId);
    void AddRemoteCrumbs(std::vector<Crumb>& path, stream::Provider& provider, const View& view);

    enum class Capture {
        None,
        Widget,
        SearchClear, // the search box's cross, held down
        List,
        DetailScroll,
        Nav,
        Lyrics,
        Search // selecting text in the search box
    };

    static LRESULT CALLBACK WndProcThunk(HWND, UINT, WPARAM, LPARAM);
    LRESULT WndProc(UINT msg, WPARAM wp, LPARAM lp);

    void InitComposition();
    void CreateWidgets();
    void UpdateLayout();
    void RepaintScaleDependent();
    void OnDeviceReplaced(); // the rendering device was lost: every surface is empty
    void RepaintTitle();
    void RefreshNowPlayingArt(bool refreshText = false);
    // The glass's colour from Preferences (a Win7 colour, or the playing cover's) and intensity.
    aero::Colorization WindowColorization() const;
    void ApplyTint(bool fade);
    void RepaintNowPlaying();
    bool PlaybackPaintAllowed() const;
    void FlushPlaybackPaints();
    void UpdatePlaybackPresentation();
    void RepaintBreadcrumb();

    // Navigation
    void Navigate(const View& view, bool push);
    void ApplyPendingScroll();
    void SelectPending();
    metadb_handle_ptr m_pendingSelect; // the song a page was opened for (View::select)
    void SaveLastView();
    bool LoadLastView(View& view, size_t& top);
    void GoBack();
    void GoForward();
    void PlaceImeWindows();  // IME candidates under the search box, at its caret
    void ShowOrganizeMenu();
    void ShowPlaylistMenu(size_t playlist, POINT screen);
    void ShowViewMenu();
    void ApplyViewStyle(media::ViewStyle style);
    void CycleViewStyle();
    void CreatePlaylist(bool autoPlaylist);
    void KeepQueueActive();   // files opened from outside go to Now Playing
    void ActivateAddTarget(); // Add files / folder: the regular playlist on screen, else the queue
    std::vector<MenuAction> AddPlaylistActions(); // Add > new / smart / preset playlists
    void OnAlbumLink(size_t group, size_t line);
    static cfg_var_modern::cfg_int* ViewStyleSetting(const View& view);
    void ShowNavCustomizeMenu(POINT screen);
    void SetNavCategories(std::vector<media::GroupKind> kinds);
    void SetNavSmartPosition(int position);
    stream::Provider* ViewProvider() const; // the provider of the current Remote view
    void RefreshProviderSections();         // the navigation pane's provider sections
    void ShowRemoteNavMenu(const NavPane::Item& item, POINT screen);
    // The audio CDs in the drives changed (came, went, tagged).
    void OnDiscsChanged();
    void ShowDiscMenu(wchar_t drive, POINT screen);
    void PaintDetailHeader();
    void RepaintDetailHeader();
    float DetailHeaderTextWidth() const; // DIP available for the name, stats and description
    float DetailTextTop() const;          // DIP: the facts' top, under the stats line when there is one
    size_t DetailFactColumns(float textW) const;
    float DetailFactsHeight(float textW) const;   // DIP, 0 without facts
    float DetailTaglineHeight(float textW) const; // DIP, 0 without a tagline
    void DrawDetailFacts(ID2D1DeviceContext* dc, float x, float y, float textW);
    void LogDetail(bool art); // `art`: the band shows the subject's picture
    void OnDetailAction();
    // The description's own scroll area (long artist/album/playlist introductions).
    bool DetailBriefThumb(float* top, float* height) const; // DIP within the bar
    ScrollBarModel::Geometry DetailBriefGeometry() const;
    ScrollBarModel::Part DetailBriefPartAt(POINT pt) const;
    float DetailBriefPage() const;
    void ScrollDetailBrief(float delta);
    void UpdateDetailBriefHover(POINT pt);
    void OnDetailBriefBarDown(POINT pt);
    void OnDetailBriefBarMove(POINT pt);
    void OnDetailBriefBarUp();
    bool AnimateDetailScrollBar(); // one fade tick; true while transitioning
    bool OnDetailBriefRepeat();    // one auto-repeat tick while a bar part is held
    void StartScrollAnimation();
    void StartScrollRepeat();
    void ShowPageContextMenu(POINT screen); // actions for the page itself (no selected item)
    // Context-menu "Go to artist" destinations for the first item (provider or local).
    std::vector<MenuAction> BuildArtistLinks(metadb_handle_list_cref items);
    std::vector<MenuAction> BuildContextActions(metadb_handle_list_cref items);
    void RemoveFromNowPlaying(); // the selected rows of the queue
    void ClearNowPlaying();
    void ForgetPlayStats(metadb_handle_list_cref items, bool recent);
    void ClearPlayStats(bool recent);
    void RefreshPlayCounts(); // after a track starts, updates the visible count column
    int ViewStyles() const;   // view options the current source offers
    void OnNavItem(const NavPane::Item& item);
    void OnNavMove(const NavPane::Item& dragged, const NavPane::Item& target, bool after);
    void AddDroppedFiles(IDataObject* data);
    void SetSearchFocus(bool focused);
    // ---- search (main_window_search.cpp) ----
    // The search box searches the list on screen or a catalog (ui/search_scope.h); a catalog
    // search opens the search page. The drop-down lists earlier searches or suggestions.
    void BeginSearch(const SearchScope& scope); // the box focused, in this scope
    void SetSearchScope(const SearchScope& scope);
    void OnSearchTextChanged(const std::wstring& text);
    bool OnSearchKey(WPARAM key); // Up / Down / Enter / Escape while the box has the focus
    void OnSearchClear();         // the box's cross
    void SubmitSearch(std::wstring term);
    void ShowSearchPopup();
    void HideSearchPopup();
    void FetchSuggestions();
    void ListSuggestions(); // the current list's titles, artists and albums
    void PickSuggestion(size_t row);
    void SyncSearchBox(const View& view); // the box shows a search page's scope and term
    struct Suggestion {
        std::wstring text, detail;
        bool history = false;
        std::function<void()> pick;
    };
    SearchScope m_searchScope;
    std::unique_ptr<SearchPopup> m_searchPopup;
    std::vector<Suggestion> m_suggestions;
    std::shared_ptr<std::atomic_bool> m_suggestCancel;
    // Each source's answer for the text being suggested for (provider key, or "local").
    std::vector<std::pair<std::wstring, stream::Provider::CatalogSearchSuggestions>> m_suggestAnswers;
    size_t m_suggestPending = 0;
    void BuildSuggestions();
    // The page's tabs (WMC's large text tabs): hover fades like the breadcrumb's plates.
    std::vector<PageTab> m_tabs;
    std::wstring m_tabCurrent;
    std::vector<float> m_tabBlend;
    std::vector<float> m_tabX; // DIP: each tab's left edge, then the last one's right edge
    int m_tabHot = -1;
    // The current tab's bar: its own visual, sliding to a newly chosen tab on the compositor.
    winrt::Windows::UI::Composition::SpriteVisual m_tabBar{nullptr};
    float m_tabBarLeft = -1, m_tabBarRight = -1; // DIP, where it stands (or is going)
    bool m_tabSlide = false;                     // the next placement slides
    void PlaceTabBar(float left, float right);
    winrt::Windows::UI::Composition::SpriteVisual m_tabStrip{nullptr};
    SIZE m_tabStripSize{};
    void LayoutTabs();
    void PaintTabs();
    int TabAt(POINT pt) const;
    void UpdateTabHover(POINT pt);
    void OnTabClick(POINT pt);
    bool AnimateTabs();
    void FocusNav(); // the navigation pane has the focus (its selection turns blue)

    LRESULT HitTest(POINT clientPt) const;
    Widget* WidgetAt(POINT clientPt) const;
    Widget* WidgetById(int id) const;
    void SetHoverWidget(Widget* w);
    bool IsSplit(const Widget* w) const;
    void UpdateSplitPart(POINT pt);
    void RunSplitMenu(int id, const std::function<void()>& menu);
    int m_splitPart = 0;           // half of the hovered split button: 0 main, 1 arrow
    int m_splitMenu = 0;           // split button whose menu is open
    float m_newPlaylistSplit = 90; // DIP, width of Create playlist's main half
    void OnActivate(bool active);
    // Now playing details in the transport band: title and artist are links, a synced
    // lyric line under them; the time right of the controls; stream format and the
    // output device (a menu) right-aligned.
    std::wstring m_streamInfo;
    // The current lyric line under the title in the transport band (its own visual: word-timed
    // lines repaint every frame while they fill).
    std::shared_ptr<const lyrics::Lyrics> m_npLyrics;
    winrt::Windows::UI::Composition::SpriteVisual m_npLyric{nullptr};
    winrt::Windows::UI::Composition::CompositionDrawingSurface m_npLyricSurface{nullptr};
    SIZE m_npLyricSize{};
    float m_npTextX = 0, m_npLyricTop = 0; // DIP inside the now-playing block
    int m_npLyricLine = -2;                // line painted last (-1 none yet)
    void PaintNowPlayingLyric();
    bool TickNowPlayingLyric(); // per frame; returns whether frames are still needed
    // Lyrics pane (right of the client area, on the glass).
    std::unique_ptr<NowPlayingPane> m_pane; // lyrics and queue
    // Where the pane lives in this window's tree; the player window borrows the pane.
    winrt::Windows::UI::Composition::ContainerVisual m_paneSlot{nullptr};
    std::unique_ptr<PlayerWindow> m_player;
    std::unique_ptr<class Taskbar> m_playerTaskbar; // the player window's thumbnail buttons
    bool m_paneLent = false;
    // The pane slides open and shut: the window grows by `m_paneReveal` px of the pane's width
    // (-1: not animating, the whole pane shows).
    int m_paneReveal = -1, m_paneRevealFrom = 0, m_paneRevealTo = 0, m_paneRevealBase = 0;
    ULONGLONG m_paneRevealStart = 0;
    void StepPaneReveal();
    std::unique_ptr<gfx::FrameClock> m_frameClock;
    bool m_lyricsOpen = false;
    bool m_lyricsHover = false;         // the mouse is over the pane
    metadb_handle_ptr m_lyricsTrack;    // track the lyrics were requested for
    lyrics::Result m_lyricsResult;      // as found; m_npLyrics may have its lines swapped
    bool m_switching = false;           // between songs: see OnSwitching
    metadb_handle_ptr m_shownTrack;     // the last song that played (kept while switching)
    metadb_handle_ptr m_openingTrack;   // the song being opened while switching
    std::wstring m_lyricsTrackIdentity; // stable track identity, independent of callback timing
    std::wstring m_lyricsSettingsSignature;
    std::uint64_t m_lyricsRequestGeneration = 0;
    bool m_playbackPresentationSuspended = true;
    winrt::com_ptr<IWICBitmapSource> m_lyricsArt; // cover for the pane's header
    void ToggleLyrics();
    void RequestLyrics(const metadb_handle_ptr& track, bool again);
    void ResetLyrics();
    void OnLyrics(const lyrics::Result& result);
    void StartLyricsFrames();
    void StartSeekFrames(); // while playing: the seek bar's head on the compositor's frames
    bool TickSeek();        // per frame; returns whether frames are still needed
    void ShowLyricsMenu(POINT screen);
    void ShowNowPlayingSongMenu(POINT screen);
    void AddLyricsDocumentActions(std::vector<MenuAction>& actions, HWND owner, const metadb_handle_ptr& now);
    void SetLyricLineShown(bool shown);
    void RefreshPaneQueue(bool playbackChanged = false);
    void SchedulePaneQueueRefresh(bool playbackChanged = false);
    void ConnectPaneQueue();
    size_t m_panePlayingIndex = SIZE_MAX; // retain the queue cursor when playback stops
    metadb_handle_ptr m_panePlayingTrack;
    uint64_t m_paneScheduleRevision = 0;
    uint64_t m_scheduleListener = 0;
    core::Signal<>::Connection m_deviceReplaced;
    std::unordered_map<size_t, media::ScheduledUpcomingEntry> m_paneUpcoming;
    bool m_queueRefreshPending = false, m_queueFollowPending = false;
    RECT m_npTitleRect{}, m_npArtistRect{}, m_npStarRect{}; // client px, for clicks
    RECT m_npArtRect{};                                     // the cover: opens the mini player
    bool m_npFavorite = false;                              // the playing song is in Favorites
    D2D1_RECT_F m_deviceText{};                             // DIP inside m_layout.stream
    std::wstring m_deviceName;                              // cached: listing devices is slow
    LONG m_streamWidth = 0;                                 // width the stream block was painted at
    void RefreshDeviceName();
    RECT DeviceRect() const;
    int m_npHot = 0; // 1 title, 2 artist, 3 device, 4 Favorite star, 5 cover
    winrt::Windows::UI::Composition::SpriteVisual m_time{nullptr}, m_streamVisual{nullptr};
    service_ptr m_outputCallback;
    void RepaintStreamInfo();
    int NowPlayingPartAt(POINT pt) const;
    void UpdateNowPlayingHover(POINT pt);
    void OnNowPlayingClick(int part);
    void JumpToPlaying();
    void ShowDeviceMenu();
    void UpdateFavoriteState(); // taskbar Favorite button for the playing song
    void ApplyLanguage(Language language);
    void RefreshCurrentPage(); // art + account lists + the current page re-read from the service
    bool CanRefreshPage() const { return m_page && m_page->CanRefresh(); }
    // A provider reported a change (the page on screen is never re-read for it).
    void OnProviderAccountObserved(stream::Provider& provider);
    void OnProviderAccount(stream::Provider& provider);
    void OnProviderLibrary(stream::Provider& provider);
    void OnProviderFavorites(stream::Provider& provider);
    void OnProviderLanguage(stream::Provider& provider);
    void OnProviderDisplay(stream::Provider& provider);
    void DiscardProviderScrollState(const stream::Id& provider);
    void SetVolumeFromX(int x);
    float VolumeThumbX() const;       // centre of the volume thumb in client pixels
    void ShowVolumeTip(float thumbX); // Win7 tooltip bubble with the volume while dragging
    void HideVolumeTip();
    void ShowTip(const std::wstring& text, float anchorX, float anchorY, bool above); // client px
    void HideTip();
    void UpdateDetailTip(POINT pt); // a cut-short detail value under the pointer
    struct CutText {
        RECT rect{}; // client px
        std::wstring text;
    };
    std::vector<CutText> m_detailCut; // detail values drawn with an ellipsis (DrawDetailFacts)
    // The control bar's texts cut short: title and artist, the lyric line, format and device.
    std::vector<CutText> m_npCut, m_lyricCut, m_streamCut;
    bool m_tipAbove = false; // the tip under the pointer is in the control bar: shown above it
    std::wstring m_tipText; // the cut-short text under the pointer (header details or the list)
    RECT m_tipRect{};
    POINT m_detailTipAt{};
    winrt::Windows::UI::Composition::SpriteVisual m_volumeTip{nullptr};
    bool m_volumeTipShown = false;
    bool m_volumeHover = false;
    float SeekFractionAt(int x) const;

    // Pixel rect of a region given in transport-cluster coordinates, and back.
    RECT ClusterRect(const D2D1_RECT_F& r) const;
    D2D1_RECT_F ClusterRegion(const RECT& r) const;
    float NavBarHeight() const;
    float TransportHeight() const;

    float Px(float dip) const { return dip * m_scale; }
    int PxI(float dip) const { return static_cast<int>(std::lround(dip * m_scale)); }
    int LinePx() const { return std::max(1, static_cast<int>(std::floor(m_scale))); }
    bool IsWindowMaximized() const { return IsZoomed(m_hwnd) != FALSE; }

    HWND m_hwnd = nullptr;
    user_interface::HookProc_t m_hook = nullptr;
    float m_scale = 1.0f;
    bool m_active = true;
    bool m_trackingNc = false, m_trackingClient = false;
    Layout m_layout;
    aero::ShadowFrame m_shadow;
    std::unique_ptr<class Taskbar> m_taskbar;

    // Composition visual tree
    std::unique_ptr<aero::GlassWindow> m_frame;
    winrt::Windows::UI::Composition::ContainerVisual m_windowRoot{nullptr}; // m_frame->Root()
    bool m_closing = false; // the closing animation runs; foobar2000 exits after it
    static bool WindowAnimations(); // Windows' animation effects are on
    void AnimateWindow(bool open);
    winrt::Windows::UI::Composition::ContainerVisual m_root{nullptr};       // m_frame->Clipped()
    winrt::Windows::UI::Composition::ContainerVisual m_contentLayer{nullptr};
    std::optional<uint32_t> m_windowCoverRgb; // the last cover's colour (tint following the cover)
    winrt::Windows::UI::Composition::SpriteVisual m_clientEdge{nullptr}, m_clientBackground{nullptr};
    winrt::Windows::UI::Composition::SpriteVisual m_navBar{nullptr};
    winrt::Windows::UI::Composition::SpriteVisual m_navGroove{nullptr}; // recess behind back / forward
    winrt::Windows::UI::Composition::SpriteVisual m_libraryBg{nullptr}; // WMP library background image
    SIZE m_navGrooveSize{};
    winrt::Windows::UI::Composition::SpriteVisual m_cmdBar{nullptr};
    winrt::Windows::UI::Composition::SpriteVisual m_contentTop{nullptr};
    winrt::Windows::UI::Composition::SpriteVisual m_transportBand{nullptr};
    winrt::Windows::UI::Composition::SpriteVisual m_capsule{nullptr};
    float m_capsulePaintedLeft = 0, m_capsulePaintedRight = 0, m_capsulePaintedScale = 0;
    void PaintCapsule();
    winrt::Windows::UI::Composition::SpriteVisual m_cluster{nullptr}; // WMP 12 transport cluster bitmap
    // The folded volume's flyout (WMP 12's, as in the mini player), over a frosted copy of
    // what lies behind; the volume slider widget moves into it while it is open.
    winrt::Windows::UI::Composition::SpriteVisual m_volFlyout{nullptr}, m_volFlyoutGlass{nullptr};
    bool m_volFlyoutOpen = false;
    float m_volFlyoutScale = 0;
    void SetVolumeFlyout(bool open);
    winrt::Windows::UI::Composition::SpriteVisual m_splitPlate{nullptr}; // folded speaker + arrow plate
    bool m_splitPlateShown = false, m_speakerFolded = false;
    void UpdateSplitPlate();
    std::vector<int> m_cmdHidden; // command bar buttons behind the » (in their order)
    float NowPlayingWidth() const; // DIP the song's text may use in the transport band
    float m_npPaintedWidth = -1;   // ... when it was last painted
    double m_timeMeasuredFor = -1; // the song length the time box was measured for
    float m_timeTextW = 110;       // DIP: the widest the time's text gets for that song
    LONG m_timePaintedW = 0;       // px width the time was painted at
    bool m_win7Art = false; // original WMP 12 artwork available (ADR 0007)
    bool m_aeroArt = false; // Win7 DWM atlas from aero.msstyles available
    winrt::Windows::UI::Composition::SpriteVisual m_divider{nullptr};
    winrt::Windows::UI::Composition::SpriteVisual m_clientGlow{nullptr};
    winrt::Windows::UI::Composition::SpriteVisual m_title{nullptr};
    winrt::Windows::UI::Composition::SpriteVisual m_nowPlaying{nullptr};
    winrt::Windows::UI::Composition::SpriteVisual m_breadcrumb{nullptr};
    winrt::Windows::UI::Composition::SpriteVisual m_detailHeader{nullptr};
    SIZE m_detailHeaderSize{};
    DetailInfo m_detail; // rebuilt by UpdateLayout for the current page
    float m_detailBriefTop = 62; // DIP: the description's top, below the facts and the tagline
    std::wstring m_loggedDetail; // LogDetail: the band's text last written to the log
    bool m_detailBriefExpanded = false;
    bool m_detailBriefOverflow = false;
    std::wstring m_detailBriefCollapsed; // the description shortened to three lines, Show more after it
    float m_detailBriefScroll = 0; // DIP into the description
    float m_detailBriefViewH = 0;  // DIP visible description height
    float m_detailBriefContentH = 0;
    float m_detailBriefTextW = 0; // DIP text width (without the bar)
    bool m_detailBriefScrollable = false;
    ScrollBarModel m_briefScroll; // the description's bar
    int m_detailBriefDragOffset = 0; // px
    bool m_scrollAnimActive = false;
    bool m_scrollRepeatActive = false;
    ULONGLONG m_scrollRepeatSince = 0;
    bool m_forcePageRefresh = false; // the next Navigate re-reads the page from its service
    int m_detailArtToken = 0;
    // Radio mode (media/radio.h): the user's own shuffle and repeat, kept while a station plays.
    int m_radioToken = 0;
    std::optional<std::pair<bool, int>> m_beforeRadio;
    bool m_radioActive = false;
    void OnRadioChanged();
    void CycleRepeat(); // the repeat buttons' step (off, all, one; off and one on a station)
    winrt::com_ptr<IWICBitmapSource> m_icon, m_art;
    std::wstring m_artTrackIdentity;
    // A song's title, artist and cover show together (owner, 2026-11-20): a new song whose cover
    // is still loading waits (kArtHoldMs at most) with the last song shown whole meanwhile; a song
    // playback is only starting towards (Next, a list started) shows as soon as it is known.
    struct HeldTrack {
        metadb_handle_ptr track;
        std::wstring identity, title, artist;
    } m_held;
    // The song being left while playback starts towards the next (shown already): the core may
    // still report it until the next one has opened, which must not bring it back.
    std::wstring m_startingFrom;
    void ShowTrack(const metadb_handle_ptr& track, std::wstring title, std::wstring artist, bool starting);
    void ApplyShownTrack(const HeldTrack& track, winrt::com_ptr<IWICBitmapSource> art);
    void ReleaseHeldTrack(bool timedOut);
    void PreviewStartingTrack(play_control::t_track_command command);
    void TrackText(const metadb_handle_ptr& track, std::wstring& title, std::wstring& artist) const;
    void ClearShownTrack();
    std::uint64_t m_artGeneration = 0;
    gfx::RetainedSurface m_nowPlayingBacking;
    gfx::RetainedSurface m_timeBacking;
    gfx::PaintCoalescer m_nowPlayingPaint;
    gfx::PaintCoalescer m_seekPaint;

    std::vector<std::unique_ptr<Widget>> m_widgets;
    Widget* m_hover = nullptr;
    Widget* m_pressed = nullptr;
    Capture m_capture = Capture::None;
    int m_ncDownId = 0; // caption button that received the non-client button-down

    // Library and content
    media::LibraryModel m_library;
    std::unique_ptr<media::ListSource> m_source;
    std::unique_ptr<Page> m_page; // the asynchronous page on screen, if any; owns its loading state
    std::unique_ptr<ListView> m_list;
    std::unique_ptr<NavPane> m_nav;
    std::unique_ptr<SearchBox> m_search;
    std::vector<View> m_history;
    size_t m_historyPos = 0;
    View m_view;
    struct ScrollPosition {
        stream::Id provider;
        size_t top = 0;
        float offset = 0; // DIP from the view's top to that row: a group's header above it stays seen
        // Scrolled to the very top: it comes back at the top, whatever the new layout's headers
        // (an artist page rebuilt as Icon from Expanded tile has headers of another height).
        bool atTop = false;
        std::map<std::wstring, float> shelves; // where its sideways shelves stood (ListView::ShelfPositions)
    };
    std::map<std::wstring, ScrollPosition> m_scrollPositions; // last top row per visited view
    std::wstring m_pendingScrollKey;                          // view whose top row is being restored
    std::wstring m_filter;                                    // lower-cased search text

    // Playback state
    std::wstring m_trackTitle, m_trackArtist;
    bool m_playing = false, m_paused = false;
    double m_position = 0, m_length = 0;
    long m_seekHead = -1; // TickSeek: the head's device pixel last painted
    bool m_seeking = false;
    float m_seekPreview = 0;
    float m_volume = 1.0f; // slider position, 0..1
    bool m_muted = false, m_shuffle = false;
    enum class Repeat {
        Off,
        All,
        One
    } m_repeat = Repeat::Off;
    bool m_applyingOrder = false; // our own playback order change in flight
    void UpdateShuffleRepeat(bool shuffle, Repeat repeat);
    void SetShuffleRepeat(bool shuffle, Repeat repeat);
    void ApplyPlaybackOrder();

    struct PlayCallback;
    struct PlaylistCallback;
    struct LibraryCallback;
    std::unique_ptr<PlayCallback> m_playCallback;
    std::unique_ptr<PlaylistCallback> m_playlistCallback;
    std::unique_ptr<LibraryCallback> m_libraryCallback;
    struct ProviderListener;
    std::unique_ptr<ProviderListener> m_providerListener;
    int m_discsToken = 0;
    std::shared_ptr<int> m_alive = std::make_shared<int>(0);
    now_playing_album_art_notify* m_artNotify = nullptr;
    winrt::com_ptr<IDropTarget> m_dropTarget;
};

} // namespace glasswave::ui
