#pragma once
#include "aero/glass_material.h"
#include "aero/shadow_frame.h"
#include "media/library_model.h"
#include "media/list_sources.h"
#include "netease/types.h"
#include "ui/list_view.h"
#include "ui/main_menu.h"
#include "ui/i18n.h"
#include "ui/nav_pane.h"
#include "ui/search_box.h"
#include "ui/widget.h"

#include <optional>

namespace glasswave::ui {

// The Glasswave main window, laid out like WMP 12 library mode: Aero glass caption and
// frame, a light client area (navigation bar, command bar, navigation pane + list,
// transport band), all presented through a Windows.UI.Composition visual tree.
class MainWindow {
public:
    MainWindow();
    ~MainWindow();
    MainWindow(const MainWindow&) = delete;
    MainWindow& operator=(const MainWindow&) = delete;

    HWND Create(user_interface::HookProc_t hook);
    void Destroy();
    HWND Hwnd() const { return m_hwnd; }

    // Forwarded from the core callbacks.
    void OnPlaybackStateChanged();
    void OnTrackChanged();
    void OnPlaybackTime();
    void OnVolumeChanged();
    void OnPlaybackOrderChanged();
    void OnPlaylistContentChanged(size_t playlist);
    void OnPlaylistSelectionChanged(size_t playlist);
    void OnPlaylistsChanged();
    void OnPlaylistLockChanged();  // auto playlists attach as locks, also after start-up
    void ScheduleLibraryReload();
    void OnNeteaseLoginChanged();

private:
    struct Layout {
        int border = 0;
        RECT caption{}, client{}, clientEdge{}, navBar{}, cmdBar{}, content{}, transport{};
        RECT capMin{}, capMax{}, capClose{}, icon{}, title{};
        RECT back{}, forward{}, breadcrumb{}, navGroove{};
        RECT organize{}, addFiles{}, addFolder{}, newPlaylist{}, search{}, viewOptions{};
        RECT navPane{}, list{};
        RECT artistHeader{}, artistFollow{};  // NetEase artist pages
        RECT seek{}, capsule{}, divider{}, shuffle{}, repeat{}, stop{}, prev{}, next{}, play{}, mute{}, volume{};
        RECT nowPlaying{}, time{}, stream{};
        POINT cluster{};  // origin of the WMP 12 transport cluster bitmap
    };

    struct View {
        // Category: every group of `group` (Artist, Album, ...); Group: the songs of one of them.
        enum class Library { All, Local, Netease };
        enum class NeteaseScope { Created, Collected, Recommended };
        enum class Kind {
            Playlist, AllMusic, Category, Group, Playlists, SmartPlaylists,
            AllFavorites, NeteasePlaylists, NeteasePlaylist, NeteaseLiked,
            NeteaseArtist, NeteaseAlbum, NeteaseArtistSongs, Search
        };
        Library library = Library::Local;
        Kind kind = Kind::AllMusic;
        size_t playlist = 0;
        media::GroupKind group = media::GroupKind::Artist;
        std::wstring key, name;
        NeteaseScope neteaseScope = NeteaseScope::Created;
        std::wstring remoteId;     // NetEase playlist id
        std::wstring query;        // Search
    };

    // Breadcrumb (WMP 12 address bar): each level is a name that navigates there and, if
    // the level has children, an arrow after it that lists them.
    struct Crumb {
        std::wstring text;                                 // empty: the leading root arrow
        std::function<void()> open;                        // click on the name
        std::function<std::vector<MenuAction>()> children; // arrow menu; empty = no arrow
        float x0 = 0, split = 0, x1 = 0;                   // DIP: name [x0, split), arrow [split, x1)
    };
    std::vector<Crumb> m_crumbs;
    int m_crumbHot = -1, m_crumbHotPart = 0;  // part 0 = name, 1 = arrow
    int m_crumbOpen = -1;                      // crumb whose arrow menu is open
    void BuildCrumbs();
    void PaintBreadcrumb();
    bool CrumbAt(POINT pt, int* index, int* part) const;
    void UpdateCrumbHover(POINT pt);
    void OnCrumbClick(POINT pt);

    enum class Capture { None, Widget, List };

    static LRESULT CALLBACK WndProcThunk(HWND, UINT, WPARAM, LPARAM);
    LRESULT WndProc(UINT msg, WPARAM wp, LPARAM lp);

    void InitComposition();
    void CreateWidgets();
    void UpdateLayout();
    void RepaintScaleDependent();
    void RepaintTitle();
    void RepaintNowPlaying();
    void RepaintBreadcrumb();

    // Navigation
    void Navigate(const View& view, bool push);
    void GoBack();
    void GoForward();
    void ShowOrganizeMenu();
    void ShowPlaylistMenu(size_t playlist, POINT screen);
    void ShowViewMenu();
    void ApplyViewStyle(media::ViewStyle style);
    void CycleViewStyle();
    void CreatePlaylist(bool autoPlaylist);
    std::vector<MenuAction> AddPlaylistActions();  // Add > new / smart / preset playlists
    void OnAlbumLink(size_t group, size_t line);
    static cfg_var_modern::cfg_int* ViewStyleSetting(const View& view);
    void ShowNavCustomizeMenu(POINT screen);
    void SetNavCategories(std::vector<media::GroupKind> kinds);
    void ShowNeteaseLogin();
    std::vector<metadb_handle_ptr> LocalFavoriteHandles() const;
    void ShowNeteaseSongs(const View& view, std::vector<netease::Song> songs, std::wstring error);
    void ShowNeteasePlaylists(const View& view, std::vector<netease::Playlist> playlists, std::wstring error);
    void ShowNeteaseArtist(const View& view, netease::Artist artist, std::vector<netease::Album> albums,
                           std::wstring error);
    void OpenNeteasePlaylist(int64_t id, std::wstring name);
    void OpenNeteaseArtist(int64_t id, std::wstring name);
    void OpenNeteaseAlbum(int64_t id, std::wstring name);
    void OpenNeteaseArtistSongs(int64_t artistId, std::wstring name);
    void PaintArtistHeader();
    void RepaintArtistHeader();
    void ToggleArtistFollow();
    int ViewStyles() const;  // view options the current source offers
    void OnNavItem(const NavPane::Item& item);
    void AddDroppedFiles(IDataObject* data);
    void SetSearchFocus(bool focused);
    void FocusNav();  // the navigation pane has the focus (its selection turns blue)

    LRESULT HitTest(POINT clientPt) const;
    Widget* WidgetAt(POINT clientPt) const;
    Widget* WidgetById(int id) const;
    void SetHoverWidget(Widget* w);
    bool IsSplit(const Widget* w) const;
    void UpdateSplitPart(POINT pt);
    void RunSplitMenu(int id, const std::function<void()>& menu);
    int m_splitPart = 0;            // half of the hovered split button: 0 main, 1 arrow
    int m_splitMenu = 0;            // split button whose menu is open
    float m_newPlaylistSplit = 90;  // DIP, width of Create playlist's main half
    void OnActivate(bool active);
    void PaintSideHighlights();
    // Now playing details in the transport band: title and artist are links, a synced
    // lyric line under them; the time right of the controls; stream format and the
    // output device (a menu) right-aligned.
    std::wstring m_streamInfo;
    std::vector<std::pair<double, std::wstring>> m_lyrics;  // synced lines (seconds, text)
    RECT m_npTitleRect{}, m_npArtistRect{}, m_npStarRect{};  // client px, for clicks
    bool m_npFavorite = false;                                // the playing song is in Favorites
    D2D1_RECT_F m_deviceText{};             // DIP inside m_layout.stream
    std::wstring m_deviceName;              // cached: listing devices is slow
    LONG m_streamWidth = 0;                 // width the stream block was painted at
    void RefreshDeviceName();
    RECT DeviceRect() const;
    int m_npHot = 0;  // 1 title, 2 artist, 3 device, 4 Favorite star
    winrt::Windows::UI::Composition::SpriteVisual m_time{nullptr}, m_streamVisual{nullptr};
    service_ptr m_outputCallback;
    void RepaintStreamInfo();
    void LoadLyrics(const metadb_handle_ptr& track);
    int NowPlayingPartAt(POINT pt) const;
    void UpdateNowPlayingHover(POINT pt);
    void OnNowPlayingClick(int part);
    void JumpToPlaying();
    void ShowDeviceMenu();
    void UpdateFavoriteState();  // taskbar Favorite button for the playing song
    void ApplyLanguage(Language language);
    void SetVolumeFromX(int x);
    float VolumeThumbX() const;  // centre of the volume thumb in client pixels
    void ShowVolumeTip(float thumbX);  // Win7 tooltip bubble with the volume while dragging
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
    winrt::Windows::UI::Composition::ContainerVisual m_windowRoot{nullptr};  // unclipped: root + frame outline
    winrt::Windows::UI::Composition::ContainerVisual m_root{nullptr};        // clipped to the rounded window
    winrt::Windows::UI::Composition::ContainerVisual m_contentLayer{nullptr};
    winrt::Windows::UI::Composition::CompositionRoundedRectangleGeometry m_clipGeometry{nullptr};
    std::unique_ptr<aero::GlassMaterial> m_glass;
    winrt::Windows::UI::Composition::SpriteVisual m_frameOutline{nullptr};
    std::array<winrt::Windows::UI::Composition::SpriteVisual, 2> m_sideHighlight{nullptr, nullptr};  // left, right
    SIZE m_sideHighlightSize{};
    winrt::Windows::UI::Composition::SpriteVisual m_clientEdge{nullptr};
    winrt::Windows::UI::Composition::SpriteVisual m_navBar{nullptr};
    winrt::Windows::UI::Composition::SpriteVisual m_navGroove{nullptr};  // recess behind back / forward
    winrt::Windows::UI::Composition::SpriteVisual m_libraryBg{nullptr};  // WMP library background image
    SIZE m_navGrooveSize{};
    winrt::Windows::UI::Composition::SpriteVisual m_cmdBar{nullptr};
    winrt::Windows::UI::Composition::SpriteVisual m_contentTop{nullptr};
    winrt::Windows::UI::Composition::SpriteVisual m_transportBand{nullptr};
    winrt::Windows::UI::Composition::SpriteVisual m_capsule{nullptr};
    winrt::Windows::UI::Composition::SpriteVisual m_cluster{nullptr};  // WMP 12 transport cluster bitmap
    bool m_win7Art = false;  // original WMP 12 artwork available (ADR 0007)
    bool m_aeroArt = false;  // Win7 DWM atlas from aero.msstyles available
    winrt::Windows::UI::Composition::SpriteVisual m_divider{nullptr};
    winrt::Windows::UI::Composition::SpriteVisual m_clientGlow{nullptr};
    winrt::Windows::UI::Composition::SpriteVisual m_title{nullptr};
    winrt::Windows::UI::Composition::SpriteVisual m_nowPlaying{nullptr};
    winrt::Windows::UI::Composition::SpriteVisual m_breadcrumb{nullptr};
    winrt::Windows::UI::Composition::SpriteVisual m_artistHeader{nullptr};
    SIZE m_artistHeaderSize{};
    std::optional<netease::Artist> m_artist;  // the NetEase artist page on screen
    bool m_followPending = false;
    int m_artistArtToken = 0;
    winrt::com_ptr<IWICBitmapSource> m_icon, m_art;

    std::vector<std::unique_ptr<Widget>> m_widgets;
    Widget* m_hover = nullptr;
    Widget* m_pressed = nullptr;
    Capture m_capture = Capture::None;
    int m_ncDownId = 0;  // caption button that received the non-client button-down

    // Library and content
    media::LibraryModel m_library;
    std::unique_ptr<media::ListSource> m_source;
    std::unique_ptr<ListView> m_list;
    std::unique_ptr<NavPane> m_nav;
    std::unique_ptr<SearchBox> m_search;
    std::vector<View> m_history;
    size_t m_historyPos = 0;
    View m_view;
    std::wstring m_filter;  // lower-cased search text

    // Playback state
    std::wstring m_trackTitle, m_trackArtist;
    bool m_playing = false, m_paused = false;
    double m_position = 0, m_length = 0;
    bool m_seeking = false;
    float m_seekPreview = 0;
    float m_volume = 1.0f;  // slider position, 0..1
    bool m_muted = false, m_shuffle = false;
    enum class Repeat { Off, All, One } m_repeat = Repeat::Off;
    bool m_applyingOrder = false;  // our own playback order change in flight
    bool m_stopAfterPass = false;  // we set "stop after current" to end a shuffled pass
    size_t m_shufflePass = 0;      // tracks played in the current shuffled pass
    void UpdateShuffleRepeat(bool shuffle, Repeat repeat);
    void SetShuffleRepeat(bool shuffle, Repeat repeat);
    void ApplyPlaybackOrder();
    void OnShuffleTrack();

    struct PlayCallback;
    struct PlaylistCallback;
    struct LibraryCallback;
    std::unique_ptr<PlayCallback> m_playCallback;
    std::unique_ptr<PlaylistCallback> m_playlistCallback;
    std::unique_ptr<LibraryCallback> m_libraryCallback;
    now_playing_album_art_notify* m_artNotify = nullptr;
    winrt::com_ptr<IDropTarget> m_dropTarget;
};

}  // namespace glasswave::ui
