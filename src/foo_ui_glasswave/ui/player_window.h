#pragma once

#include "aero/glass_material.h"
#include "aero/shadow_frame.h"
#include "gfx/frame_clock.h"
#include "ui/aero_light.h"
#include "ui/album_wall.h"
#include "ui/now_playing_pane.h"
#include "ui/widget.h"

// The player window: Glasswave outside its library. One window in two forms,
//   Mini        WMP 12's now-playing window: an Aero glass frame around the picture, WMP's
//               narrow transport under it (a strip, a cover, or cover + lyrics / queue);
//   Full screen Windows Media Center's now-playing screen with its windowed-mode controls;
// both on WMC's wall of album covers. Controls, bars and shades are the original Windows 7
// artwork (ADR 0007) wherever the user's files have it. The lyrics / queue pane is the library
// window's own, lent while this window shows.
// Design and measurements: docs/design/player-window.md.

namespace glasswave::ui {

// What the player window needs from the library window.
class PlayerHost {
  public:
    virtual NowPlayingPane* LendPane(const winrt::Windows::UI::Composition::ContainerVisual& parent) = 0;
    virtual void ReturnPane() = 0;
    virtual void PlayerClosed() = 0;        // show the library again
    virtual float PlayerVolume() const = 0; // slider position 0..1
    virtual void SetPlayerVolume(float slider) = 0;
    virtual std::vector<metadb_handle_ptr> WallAlbums() const = 0; // one track per album
    virtual bool OriginalTransportArt() const = 0;
    // The playing song as the library window shows it (provider and cue-sheet names included).
    virtual void PlayerTrackText(std::wstring& title, std::wstring& artist, std::wstring& album) const = 0;
    // The player window was created: give it the application's icon and taskbar buttons.
    virtual void AdoptPlayerWindow(HWND window) = 0;
    // Every message of the player window; true when the taskbar integration handled it.
    virtual bool PlayerWindowMessage(UINT msg, WPARAM wp, LPARAM lp) = 0;

  protected:
    ~PlayerHost() = default;
};

class PlayerWindow {
  public:
    enum class Mode {
        Mini,
        Full
    };

    explicit PlayerWindow(PlayerHost& host);
    ~PlayerWindow();
    PlayerWindow(const PlayerWindow&) = delete;
    PlayerWindow& operator=(const PlayerWindow&) = delete;

    void Show(Mode mode);
    void Close(); // back to the library
    bool Shown() const { return m_shown; }
    Mode CurrentMode() const { return m_mode; }
    HWND Hwnd() const { return m_hwnd; }

    // Forwarded by the library window.
    void OnPlayback(); // state or track
    void OnTime();
    void OnVolume();
    void StartFrames(); // the lent pane wants frames

  private:
    void UpdateMotion(); // wall and light run only while there is something to see

  public:

  private:
    enum class Capture {
        None,
        Widget,
        Seek,
        Volume,
        Pane
    };
    struct Layout {
        RECT client{}, clientEdge{}; // inside the glass frame (the whole window when full screen)
        RECT bar{}, top{}, seek{}, timeLeft{}, timeRight{}, cover{}, info{}, pane{}, panel{};
        RECT stop{}, prev{}, play{}, next{}, mute{}, volArrow{}, volDown{}, volUp{}, lyrics{}, list{};
        RECT library{}, minimize{}, restore{}, exit{}, popup{};
        RECT capMin{}, capMax{}, capClose{};
        std::array<RECT, 2> divider{};
        POINT cluster{};
        int infoStyle = 0; // 0 centred under the cover, 1 beside the cover (WMC), 2 strip
    };

    static LRESULT CALLBACK WndProcThunk(HWND, UINT, WPARAM, LPARAM);
    LRESULT WndProc(UINT msg, WPARAM wp, LPARAM lp);
    void Create();
    void InitVisuals();
    void CreateWidgets();
    void ApplyMode(bool first);
    void UpdateLayout(bool animate);
    void PaintFrame();
    void PaintShade();
    void PaintBars();
    void PaintInfo();
    void PaintCover();
    void PaintSeek();
    void PaintTimes();
    void PaintPopup();
    void ReadTrack();
    void RequestArt();
    void SetPaneShown(bool shown, NowPlayingPane::Mode mode);
    void TogglePane(NowPlayingPane::Mode mode);
    void ShowChrome(bool show);
    void ArmHideTimer();
    void SetPopup(bool open);
    void StepVolume(float delta);
    void SetVolumeFromX(int x);
    float SeekFractionAt(int x) const;
    bool Wmc() const { return m_ehres && m_mode == Mode::Full; } // Media Center's control set
    Widget* WidgetAt(POINT pt) const;
    Widget* WidgetById(int id) const;
    void SetHover(Widget* w);
    RECT ClusterRect(const D2D1_RECT_F& r) const;
    D2D1_RECT_F ClusterRegion(const RECT& r) const;
    int PxI(float dip) const { return static_cast<int>(std::lround(dip * m_scale)); }
    float Px(float dip) const { return dip * m_scale; }

    PlayerHost& m_host;
    HWND m_hwnd = nullptr;
    float m_scale = 1.0f;
    bool m_shown = false, m_active = true;
    Mode m_mode = Mode::Mini;
    bool m_paneShown = false;
    RECT m_miniRect{}; // screen px, remembered between visits
    bool m_haveMiniRect = false;
    SIZE m_client{};
    Layout m_layout;
    bool m_wmp = false;   // wmploc.dll artwork (WMP 12 transport)
    bool m_ehres = false; // ehres.dll artwork (Media Center controls, bars, shades)
    bool m_aeroArt = false;

    winrt::Windows::UI::Composition::Desktop::DesktopWindowTarget m_target{nullptr};
    winrt::Windows::UI::Composition::ContainerVisual m_windowRoot{nullptr}, m_root{nullptr}, m_content{nullptr},
        m_stage{nullptr}, m_paneHost{nullptr}, m_barLayer{nullptr}, m_capsLayer{nullptr}, m_frameLayer{nullptr};
    winrt::Windows::UI::Composition::CompositionRoundedRectangleGeometry m_clipGeometry{nullptr};
    winrt::Windows::UI::Composition::InsetClip m_contentClip{nullptr};
    winrt::Windows::UI::Composition::SpriteVisual m_frameOutline{nullptr}, m_clientEdge{nullptr}, m_shade{nullptr},
        m_cover{nullptr}, m_coverOld{nullptr}, m_info{nullptr}, m_barBack{nullptr}, m_topBack{nullptr},
        m_seek{nullptr}, m_times{nullptr}, m_cluster{nullptr}, m_popup{nullptr};
    std::unique_ptr<aero::GlassMaterial> m_glass;
    aero::ShadowFrame m_shadow;
    std::unique_ptr<AlbumWall> m_wall;
    std::unique_ptr<AeroLight> m_light, m_veil; // behind the wall, and its light over the covers
    gfx::RetainedSurface m_infoBacking, m_seekBacking, m_timesBacking, m_popupBacking, m_barBacking, m_topBacking;
    SIZE m_coverPainted{};
    float m_framePainted = 0;

    std::vector<std::unique_ptr<Widget>> m_widgets;
    Widget* m_hover = nullptr;
    Widget* m_pressed = nullptr;
    Capture m_capture = Capture::None;
    NowPlayingPane* m_pane = nullptr; // lent by the host while shown
    bool m_paneHover = false, m_seekHover = false;
    bool m_chromeShown = true, m_tracking = false, m_popupOpen = false, m_cursorHidden = false;
    POINT m_lastMouse{-1, -1};
    std::unique_ptr<gfx::FrameClock> m_frameClock;

    // Playback
    metadb_handle_ptr m_track;
    std::wstring m_title, m_artist, m_album;
    bool m_playing = false, m_paused = false, m_muted = false;
    double m_position = 0, m_length = 0;
    bool m_seeking = false;
    float m_seekPreview = 0, m_volume = 1;
    winrt::com_ptr<IWICBitmapSource> m_art;
    bool m_artPending = false;
    int m_artToken = 0;
};

} // namespace glasswave::ui
