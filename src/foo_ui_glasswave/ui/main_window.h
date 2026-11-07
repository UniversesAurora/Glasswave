#pragma once
#include "aero/glass_material.h"
#include "aero/shadow_frame.h"
#include "media/library_model.h"
#include "media/list_sources.h"
#include "ui/list_view.h"
#include "ui/nav_pane.h"
#include "ui/search_box.h"
#include "ui/widget.h"

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
    void ScheduleLibraryReload();

private:
    struct Layout {
        int border = 0;
        RECT caption{}, client{}, clientEdge{}, navBar{}, cmdBar{}, content{}, transport{};
        RECT capMin{}, capMax{}, capClose{}, icon{}, title{};
        RECT back{}, forward{}, breadcrumb{};
        RECT organize{}, addFiles{}, addFolder{}, newPlaylist{}, search{};
        RECT navPane{}, list{};
        RECT seek{}, capsule{}, divider{}, shuffle{}, repeat{}, stop{}, prev{}, next{}, play{}, mute{}, volume{};
        RECT nowPlaying{};
    };

    struct View {
        enum class Kind { Playlist, AllMusic, Artists, Albums, Genres, Group } kind = Kind::AllMusic;
        size_t playlist = 0;
        media::GroupKind group = media::GroupKind::Artist;
        std::wstring key, name;
    };

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
    void OnNavItem(const NavPane::Item& item);
    void AddDroppedFiles(IDataObject* data);
    void SetSearchFocus(bool focused);

    LRESULT HitTest(POINT clientPt) const;
    Widget* WidgetAt(POINT clientPt) const;
    Widget* WidgetById(int id) const;
    void SetHoverWidget(Widget* w);
    void OnActivate(bool active);
    void SetVolumeFromX(int x);
    float SeekFractionAt(int x) const;

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

    // Composition visual tree
    winrt::Windows::UI::Composition::Desktop::DesktopWindowTarget m_target{nullptr};
    winrt::Windows::UI::Composition::ContainerVisual m_root{nullptr};
    winrt::Windows::UI::Composition::ContainerVisual m_contentLayer{nullptr};
    winrt::Windows::UI::Composition::CompositionRoundedRectangleGeometry m_clipGeometry{nullptr};
    std::unique_ptr<aero::GlassMaterial> m_glass;
    winrt::Windows::UI::Composition::SpriteVisual m_frameOutline{nullptr};
    winrt::Windows::UI::Composition::SpriteVisual m_clientEdge{nullptr};
    winrt::Windows::UI::Composition::SpriteVisual m_navBar{nullptr};
    winrt::Windows::UI::Composition::SpriteVisual m_cmdBar{nullptr};
    winrt::Windows::UI::Composition::SpriteVisual m_contentTop{nullptr};
    winrt::Windows::UI::Composition::SpriteVisual m_transportBand{nullptr};
    winrt::Windows::UI::Composition::SpriteVisual m_capsule{nullptr};
    winrt::Windows::UI::Composition::SpriteVisual m_divider{nullptr};
    winrt::Windows::UI::Composition::SpriteVisual m_clientGlow{nullptr};
    winrt::Windows::UI::Composition::SpriteVisual m_title{nullptr};
    winrt::Windows::UI::Composition::SpriteVisual m_nowPlaying{nullptr};
    winrt::Windows::UI::Composition::SpriteVisual m_breadcrumb{nullptr};
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
    bool m_muted = false, m_shuffle = false, m_repeat = false;

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
