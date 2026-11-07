#pragma once
#include "aero/glass_material.h"
#include "aero/shadow_frame.h"
#include "ui/widget.h"

namespace glasswave::ui {

// The Glasswave main window, laid out like WMP 12 library mode: Aero glass caption
// and frame, a light client area (navigation bar, command bar, content, transport band),
// all presented through a Windows.UI.Composition visual tree (WS_EX_NOREDIRECTIONBITMAP).
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
    void OnVolumeChanged();
    void OnPlaybackOrderChanged();

private:
    struct Layout {
        int border = 0;
        RECT caption{}, client{}, clientEdge{}, navBar{}, cmdBar{}, content{}, transport{};
        RECT capMin{}, capMax{}, capClose{}, icon{}, title{};
        RECT capsule{}, divider{}, shuffle{}, repeat{}, stop{}, prev{}, next{}, play{}, mute{}, volume{};
        RECT nowPlaying{};
    };

    static LRESULT CALLBACK WndProcThunk(HWND, UINT, WPARAM, LPARAM);
    LRESULT WndProc(UINT msg, WPARAM wp, LPARAM lp);

    void InitComposition();
    void CreateWidgets();
    void UpdateLayout();
    void RepaintScaleDependent();
    void RepaintTitle();
    void RepaintNowPlaying();
    void RepaintContentText();

    LRESULT HitTest(POINT clientPt) const;
    Widget* WidgetAt(POINT clientPt) const;
    Widget* WidgetById(int id) const;
    void SetHoverWidget(Widget* w);
    void OnActivate(bool active);
    void SetVolumeFromX(int x);

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
    winrt::Windows::UI::Composition::SpriteVisual m_contentText{nullptr};
    winrt::Windows::UI::Composition::SpriteVisual m_title{nullptr};
    winrt::Windows::UI::Composition::SpriteVisual m_nowPlaying{nullptr};
    winrt::com_ptr<IWICBitmapSource> m_icon;

    std::vector<std::unique_ptr<Widget>> m_widgets;
    Widget* m_hover = nullptr;
    Widget* m_pressed = nullptr;
    int m_ncDownId = 0;  // caption button that received the non-client button-down

    std::wstring m_trackTitle, m_trackArtist;
    bool m_playing = false, m_paused = false;
    float m_volume = 1.0f;  // slider position, 0..1
    bool m_muted = false, m_shuffle = false, m_repeat = false;

    struct PlayCallback;
    struct PlaylistCallback;
    std::unique_ptr<PlayCallback> m_playCallback;
    std::unique_ptr<PlaylistCallback> m_playlistCallback;
};

}  // namespace glasswave::ui
