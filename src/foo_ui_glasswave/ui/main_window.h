#pragma once
#include "aero/glass_material.h"
#include "aero/shadow_frame.h"
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
#include "ui/page.h"
#include "ui/search_box.h"
#include "ui/widget.h"
#include "ui/win7_menu.h"

#include <cstdint>
#include <optional>
#include <unordered_map>

namespace glasswave::ui {

// The Glasswave main window, laid out like WMP 12 library mode: Aero glass caption and
// frame, a light client area (navigation bar, command bar, navigation pane + list,
// transport band), all presented through a Windows.UI.Composition visual tree.
class MainWindow : public PageHost {
  public:
    MainWindow();
    ~MainWindow();
    MainWindow(const MainWindow&) = delete;
    MainWindow& operator=(const MainWindow&) = delete;

    HWND Create(user_interface::HookProc_t hook);
    void Destroy();
    HWND Hwnd() const { return m_hwnd; }
    static MainWindow* Instance(); // the window while it exists (main menu commands)
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

    // Forwarded from the core callbacks.
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
    const View& CurrentView() const override { return m_view; }
    void Open(const View& view) override { Navigate(view, true); }
    void Replace(const View& view) override { Navigate(view, false); }
    void Reload() override { Navigate(m_view, false); }
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
    struct Layout {
        int border = 0;
        RECT caption{}, client{}, clientEdge{}, navBar{}, cmdBar{}, content{}, transport{};
        RECT capMin{}, capMax{}, capClose{}, icon{}, title{};
        RECT back{}, forward{}, breadcrumb{}, navGroove{};
        RECT organize{}, addFiles{}, addFolder{}, newPlaylist{}, search{}, viewOptions{}, refresh{};
        RECT navPane{}, list{};
        RECT detailHeader{}, detailAction{}; // NetEase detail pages: artist, album, playlist
        RECT detailBriefToggle{};
        RECT detailBriefArea{}; // text area of the description (wheel), client px
        RECT detailBriefBar{};  // its Win7 scrollbar, client px; empty when not scrollable
        RECT seek{}, capsule{}, divider{}, shuffle{}, repeat{}, stop{}, prev{}, next{}, play{}, mute{},
            volume{};
        RECT nowPlaying{}, time{}, stream{};
        RECT lyricsPane{}, lyricsToggle{}; // lyrics pane on the glass right of the client area
        POINT cluster{};                   // origin of the WMP 12 transport cluster bitmap
    };

    // Breadcrumb (WMP 12 address bar): each level is a name that navigates there and, if
    // the level has children, an arrow after it that lists them.
    struct Crumb {
        std::wstring text;                                 // empty: the leading root arrow
        std::function<void()> open;                        // click on the name
        std::function<std::vector<MenuAction>()> children; // arrow menu; empty = no arrow
        bool literal = false;                              // a user's name: shown untranslated
        float x0 = 0, split = 0, x1 = 0;                   // DIP: name [x0, split), arrow [split, x1)
    };
    std::vector<Crumb> m_crumbs;
    int m_crumbHot = -1, m_crumbHotPart = 0; // part 0 = name, 1 = arrow
    int m_crumbOpen = -1;                    // crumb whose arrow menu is open
    // One navigation model for every library: a path of levels, each with its own children
    // menu. Structural levels always get a menu (the arrow also separates the crumbs).
    std::vector<Crumb> BuildCrumbPath();
    void BuildCrumbs();
    void LayoutCrumbs();
    std::vector<MenuAction> LibraryMenu();
    std::vector<MenuAction> AllSectionsMenu();
    std::vector<MenuAction> LocalSectionsMenu();
    std::vector<MenuAction> ProviderSectionsMenu(stream::Provider& provider);
    std::vector<MenuAction> PlaylistListMenu(bool smart);
    std::vector<MenuAction> GroupListMenu(media::GroupKind kind);
    std::vector<MenuAction> ShelfMenu(stream::Provider& provider, const stream::NavigationSection& section);
    std::vector<MenuAction> FollowedArtistsMenu(stream::Provider& provider, const stream::Id& sectionId);
    std::vector<MenuAction> SavedAlbumsMenu(stream::Provider& provider, const stream::Id& sectionId);
    void AddRemoteCrumbs(std::vector<Crumb>& path, stream::Provider& provider);
    void PaintBreadcrumb();
    bool CrumbAt(POINT pt, int* index, int* part) const;
    void UpdateCrumbHover(POINT pt);
    void OnCrumbClick(POINT pt);

    enum class Capture {
        None,
        Widget,
        List,
        DetailScroll,
        Nav,
        Lyrics
    };

    static LRESULT CALLBACK WndProcThunk(HWND, UINT, WPARAM, LPARAM);
    LRESULT WndProc(UINT msg, WPARAM wp, LPARAM lp);

    void InitComposition();
    void CreateWidgets();
    void UpdateLayout();
    void RepaintScaleDependent();
    void RepaintTitle();
    void RefreshNowPlayingArt(bool refreshText = false);
    void RepaintNowPlaying();
    bool PlaybackPaintAllowed() const;
    void FlushPlaybackPaints();
    void UpdatePlaybackPresentation();
    void RepaintBreadcrumb();

    // Navigation
    void Navigate(const View& view, bool push);
    void ApplyPendingScroll();
    void SaveLastView();
    bool LoadLastView(View& view, size_t& top);
    void GoBack();
    void GoForward();
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
    void PaintDetailHeader();
    void RepaintDetailHeader();
    float DetailHeaderTextWidth() const; // DIP available for the name, stats and description
    void OnDetailAction();
    // The description's own scroll area (long artist/album/playlist introductions).
    bool DetailBriefThumb(float* top, float* height) const; // DIP within the bar
    int DetailBriefPartAt(POINT pt) const;                  // 0 none, 1 up, 2 down, 3 track, 4 thumb
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
    std::vector<ContextAction> BuildContextActions(metadb_handle_list_cref items);
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
    void PaintSideHighlights();
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
    std::unique_ptr<gfx::FrameClock> m_frameClock;
    bool m_lyricsOpen = false;
    bool m_lyricsHover = false;         // the mouse is over the pane
    metadb_handle_ptr m_lyricsTrack;    // track the lyrics were requested for
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
    void ShowLyricsMenu(POINT screen);
    void RefreshPaneQueue(bool playbackChanged = false);
    void SchedulePaneQueueRefresh(bool playbackChanged = false);
    void ConnectPaneQueue();
    size_t m_panePlayingIndex = SIZE_MAX; // retain the queue cursor when playback stops
    metadb_handle_ptr m_panePlayingTrack;
    uint64_t m_paneScheduleRevision = 0;
    uint64_t m_scheduleListener = 0;
    std::unordered_map<size_t, media::ScheduledUpcomingEntry> m_paneUpcoming;
    bool m_queueRefreshPending = false, m_queueFollowPending = false;
    RECT m_npTitleRect{}, m_npArtistRect{}, m_npStarRect{}; // client px, for clicks
    bool m_npFavorite = false;                              // the playing song is in Favorites
    D2D1_RECT_F m_deviceText{};                             // DIP inside m_layout.stream
    std::wstring m_deviceName;                              // cached: listing devices is slow
    LONG m_streamWidth = 0;                                 // width the stream block was painted at
    void RefreshDeviceName();
    RECT DeviceRect() const;
    int m_npHot = 0; // 1 title, 2 artist, 3 device, 4 Favorite star
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
    winrt::Windows::UI::Composition::Desktop::DesktopWindowTarget m_target{nullptr};
    winrt::Windows::UI::Composition::ContainerVisual m_windowRoot{nullptr}; // unclipped: root + frame outline
    winrt::Windows::UI::Composition::ContainerVisual m_root{nullptr};       // clipped to the rounded window
    winrt::Windows::UI::Composition::ContainerVisual m_contentLayer{nullptr};
    winrt::Windows::UI::Composition::CompositionRoundedRectangleGeometry m_clipGeometry{nullptr};
    std::unique_ptr<aero::GlassMaterial> m_glass;
    winrt::Windows::UI::Composition::SpriteVisual m_frameOutline{nullptr};
    std::array<winrt::Windows::UI::Composition::SpriteVisual, 2> m_sideHighlight{nullptr,
                                                                                 nullptr}; // left, right
    SIZE m_sideHighlightSize{};
    winrt::Windows::UI::Composition::SpriteVisual m_clientEdge{nullptr}, m_clientBackground{nullptr};
    winrt::Windows::UI::Composition::SpriteVisual m_navBar{nullptr};
    winrt::Windows::UI::Composition::SpriteVisual m_navGroove{nullptr}; // recess behind back / forward
    winrt::Windows::UI::Composition::SpriteVisual m_libraryBg{nullptr}; // WMP library background image
    SIZE m_navGrooveSize{};
    winrt::Windows::UI::Composition::SpriteVisual m_cmdBar{nullptr};
    winrt::Windows::UI::Composition::SpriteVisual m_contentTop{nullptr};
    winrt::Windows::UI::Composition::SpriteVisual m_transportBand{nullptr};
    winrt::Windows::UI::Composition::SpriteVisual m_capsule{nullptr};
    winrt::Windows::UI::Composition::SpriteVisual m_cluster{nullptr}; // WMP 12 transport cluster bitmap
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
    bool m_detailBriefExpanded = false;
    bool m_detailBriefOverflow = false;
    float m_detailBriefScroll = 0; // DIP into the description
    float m_detailBriefViewH = 0;  // DIP visible description height
    float m_detailBriefContentH = 0;
    float m_detailBriefTextW = 0; // DIP text width (without the bar)
    bool m_detailBriefScrollable = false;
    int m_detailBriefHot = 0; // 1 up, 2 down, 3 track, 4 thumb
    int m_detailBriefPressed = 0;
    int m_detailBriefTrackDir = 0;   // track click above (1) or below (2) the thumb
    int m_detailBriefDragOffset = 0; // px
    POINT m_detailBriefPressPt{};
    std::array<float, 5> m_detailBriefBlend{}; // 0 normal .. 1 hot .. 2 pressed
    bool m_scrollAnimActive = false;
    bool m_scrollRepeatActive = false;
    ULONGLONG m_scrollRepeatSince = 0;
    bool m_forcePageRefresh = false; // the next Navigate re-reads the page from its service
    int m_detailArtToken = 0;
    winrt::com_ptr<IWICBitmapSource> m_icon, m_art;
    std::wstring m_artTrackIdentity;
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
    };
    std::map<std::wstring, ScrollPosition> m_scrollPositions; // last top row per visited view
    std::wstring m_pendingScrollKey;                          // view whose top row is being restored
    std::wstring m_filter;                                    // lower-cased search text

    // Playback state
    std::wstring m_trackTitle, m_trackArtist;
    bool m_playing = false, m_paused = false;
    double m_position = 0, m_length = 0;
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
    std::shared_ptr<int> m_alive = std::make_shared<int>(0);
    now_playing_album_art_notify* m_artNotify = nullptr;
    winrt::com_ptr<IDropTarget> m_dropTarget;
};

} // namespace glasswave::ui
