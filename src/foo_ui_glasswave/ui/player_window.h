#pragma once

#include "aero/glass_material.h"
#include "aero/shadow_frame.h"
#include "gfx/frame_clock.h"
#include "lyrics/lyrics.h"
#include "ui/aero_light.h"
#include "ui/album_wall.h"
#include "ui/now_playing_pane.h"
#include "ui/widget.h"
#include "ui/wmc_volume.h"

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
    // The song to show: the playing one; while playback switches songs, the one being opened
    // as soon as it is known, the previous one until then; none once playback stopped.
    virtual metadb_handle_ptr PlayerTrack() const = 0;
    // A song's text as the library window shows it (provider and cue-sheet names included).
    virtual void PlayerTrackText(const metadb_handle_ptr& track, std::wstring& title, std::wstring& artist,
                                 std::wstring& album) const = 0;
    // Shuffle and repeat (0 off, 1 all, 2 one), as the library window keeps them.
    virtual void PlayerMode(bool& shuffle, int& repeat) const = 0;
    virtual void SetPlayerMode(bool shuffle, int repeat) = 0;
    virtual std::shared_ptr<const lyrics::Lyrics> PlayerLyrics() const = 0;
    virtual bool PlayerFavorite() const = 0;
    virtual void TogglePlayerFavorite() = 0;
    // The lyrics menu of the library window's lyric line (search, sources, file, hide / show).
    virtual void ShowPlayerLyricsMenu(POINT screen) = 0;
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
    // A station started or ended, or its songs moved on (media/radio.h): shuffle, repeat and
    // Previous follow.
    void RadioChanged();
    // The playing list changed (a station's songs arriving): the album wall takes the new albums.
    void WallAlbumsChanged();
    void OnTime();
    void OnVolume();
    void OnLyricLineSetting(); // the lyric line was hidden or shown
    void StartFrames(); // the lent pane or the sung line wants frames
    void SetColorization(const aero::Colorization& c, int fadeMs) {
        if (m_glass) m_glass->SetColorization(c, fadeMs);
    }

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
        RECT library{}, minimize{}, restore{}, exit{}, popup{}, favorite{}, lyric{}, menuShade{};
        RECT slider{}; // mini, wide enough: the volume slider beside the speaker (else its arrow)
        RECT shuffle{}, repeat{}; // mini: WMP 12's pair before stop, where there is room
        std::array<RECT, 4> menu{}; // full screen: lyrics, up next, shuffle, repeat
        RECT capMin{}, capMax{}, capClose{};
        std::array<RECT, 2> divider{};
        POINT cluster{};
        int infoStyle = 0; // 0 centred under the cover, 1 beside the cover (WMC), 2 strip
        float textScale = 1; // style 0: the lines' size, with the window's width and height
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
    std::wstring TimeText() const; // "1:23 / 4:56", or counting down "-3:33 / 4:56"
    void PaintCover();
    void PaintSeek();
    void UpdateSeekThumb();
    void PaintTimes();
    void PaintPopup();
    void PaintSlider();
    void UpdateSliderThumb();
    void PaintFlyout();
    void UpdateFlyoutBall();
    void UpdateSplitPlate();
    void ShowBubble(bool show);
    void PaintLyric();
    bool LyricLineLive() const; // the sung line follows the words (playing, shown, synced)
    bool TickLyric();           // a frame: repaints the sung line when it moved; false when idle
    bool SeekLive() const;      // the seek bar's head follows the music (playing, shown)
    bool TickSeek();            // a frame: moves the head a device pixel, the times a second
    float SeekHead(float progress) const; // the played part's front, DIP from the bar's left
    void PaintMenuShade();
    void PaintSideHighlights();
    void PlaceWall();
    void PaintPaneCard();
    void MenuPressed(int index);
    std::wstring MenuText(int index) const;
    void AnimateWindowTo(const RECT& target);
    int CurrentLyricLine(int* ms) const;
    void ReadTrack();
    void RequestArt();
    void SetPaneShown(bool shown, NowPlayingPane::Mode mode);
    void SlideBack(bool animate); // Media Center's back orb in or out with the pane
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
    bool m_backIn = true, m_backWmc = false; // SlideBack: the orb in place; under WMC's controls
    RECT m_miniRect{}; // screen px, remembered between visits
    bool m_haveMiniRect = false;
    SIZE m_client{};
    Layout m_layout;
    bool m_wmp = false;   // wmploc.dll artwork (WMP 12 transport)
    bool m_ehres = false; // ehres.dll artwork (Media Center controls, bars, shades)
    bool m_aeroArt = false;

    winrt::Windows::UI::Composition::Desktop::DesktopWindowTarget m_target{nullptr};
    winrt::Windows::UI::Composition::ContainerVisual m_windowRoot{nullptr}, m_root{nullptr}, m_content{nullptr},
        m_stage{nullptr}, m_paneHost{nullptr}, m_barLayer{nullptr}, m_capsLayer{nullptr}, m_frameLayer{nullptr},
        m_menuLayer{nullptr};
    winrt::Windows::UI::Composition::CompositionRoundedRectangleGeometry m_clipGeometry{nullptr};
    winrt::Windows::UI::Composition::InsetClip m_contentClip{nullptr};
    winrt::Windows::UI::Composition::SpriteVisual m_frameOutline{nullptr}, m_clientEdge{nullptr}, m_shade{nullptr},
        m_cover{nullptr}, m_coverOld{nullptr}, m_info{nullptr}, m_barBack{nullptr}, m_topBack{nullptr},
        m_times{nullptr}, m_cluster{nullptr}, m_shadeOld{nullptr}, m_lyric{nullptr}, m_menuShade{nullptr},
        m_menuSweep{nullptr}, m_menuAfter{nullptr}, m_menuInk{nullptr}, m_seek{nullptr}, m_slider{nullptr},
        m_paneCard{nullptr}, m_menuInkWhite{nullptr}, m_volumeFlyout{nullptr};
    gfx::RetainedSurface m_flyoutBacking;
    winrt::Windows::UI::Composition::SpriteVisual m_paneGlass{nullptr}; // full screen: the card's glass
    winrt::Windows::UI::Composition::CompositionRoundedRectangleGeometry m_paneGlassShape{nullptr};
    float m_paneGlassScale = 0;
    winrt::Windows::UI::Composition::SpriteVisual m_flyoutGlass{nullptr}; // frosted, under the flyout
    float m_frostScale = 0;
    bool m_flyoutHot = false;
    bool m_splitVolume = false; // mini, narrow: WMP's speaker + arrow split button
    // The split button's plain plate (wmploc 4630) under both halves while either is lit.
    winrt::Windows::UI::Composition::SpriteVisual m_splitPlate{nullptr};
    gfx::RetainedSurface m_splitPlateBacking;
    bool m_splitPlateShown = false;
    bool m_bubbleHover = false; // the pointer is on a volume slider (the bubble follows after a pause)
    // A slider's thumb in its own visuals: its three looks (at rest, lit, held) painted once and
    // crossfaded, and the whole faded in and out where the thumb comes and goes.
    struct FadingThumb {
        winrt::Windows::UI::Composition::ContainerVisual root{nullptr};
        std::array<winrt::Windows::UI::Composition::SpriteVisual, 3> looks{nullptr, nullptr, nullptr};
        std::array<gfx::RetainedSurface, 3> backings;
        float paintedScale = 0;
        bool shown = false, hot = false, held = false;
        void Create(const winrt::Windows::UI::Composition::ContainerVisual& parent, bool visible);
        // Paints the looks (0 rest, 1 lit, 2 held) when the scale changed or `force`.
        void Paint(int w, int h, float scale, bool force, const std::function<void(ID2D1DeviceContext*, int)>& look);
        void Place(int x, int y, int w, int h);
        void Show(bool show, int inMs, int outMs);
        void SetState(bool hot, bool held, int inMs, int outMs);
    };
    FadingThumb m_seekThumb, m_flyoutBall, m_sliderThumb;
    LONG m_seekPointerX = -1;   // px, the pointer over the seek bar
    RECT m_timeHit{};           // px, the time line among the song's lines (a click counts down / up)
    int m_menuInking = -1; // the menu item whose press draws its type
    uint64_t m_menuInkGeneration = 0;
    std::shared_ptr<int> m_alive = std::make_shared<int>(0); // for compositor callbacks
    std::array<winrt::Windows::UI::Composition::SpriteVisual, 2> m_sideHighlight{nullptr, nullptr};
    SIZE m_sideHighlightSize{};
    winrt::Windows::UI::Composition::CompositionLinearGradientBrush m_menuInkMask{nullptr},
        m_menuInkMaskWhite{nullptr};
    std::array<winrt::Windows::UI::Composition::SpriteVisual, 4> m_menuGlow{nullptr, nullptr, nullptr, nullptr};
    winrt::Windows::UI::Composition::ContainerVisual m_menuFx{nullptr};
    winrt::Windows::UI::Composition::ContainerVisual m_menuSweepHost{nullptr}; // clipped to the plate
    winrt::Windows::UI::Composition::CompositionRoundedRectangleGeometry m_menuSweepShape{nullptr};
    winrt::Windows::UI::Composition::InsetClip m_menuFxClip{nullptr};
    std::unique_ptr<aero::GlassMaterial> m_glass;
    aero::ShadowFrame m_shadow;
    std::unique_ptr<AlbumWall> m_wall;
    std::unique_ptr<WmcVolume> m_volumePanel, m_volumeBubble;
    std::unique_ptr<AeroLight> m_light, m_veil; // behind the wall, and its light over the covers
    gfx::RetainedSurface m_infoBacking, m_timesBacking, m_barBacking, m_topBacking, m_lyricBacking,
        m_menuShadeBacking, m_seekBacking, m_sliderBacking, m_paneCardBacking;
    SIZE m_coverPainted{};
    float m_framePainted = 0;

    std::vector<std::unique_ptr<Widget>> m_widgets;
    Widget* m_hover = nullptr;
    Widget* m_pressed = nullptr;
    Capture m_capture = Capture::None;
    NowPlayingPane* m_pane = nullptr; // lent by the host while shown
    bool m_paneHover = false, m_seekHover = false, m_sliderHover = false;
    POINT m_clickPoint{}; // where the button that is being clicked was released
    bool m_chromeShown = true, m_tracking = false, m_popupOpen = false, m_cursorHidden = false;
    POINT m_lastMouse{-1, -1};
    std::unique_ptr<gfx::FrameClock> m_frameClock;

    // Playback
    metadb_handle_ptr m_track;
    std::wstring m_title, m_artist, m_album;
    bool m_playing = false, m_paused = false, m_muted = false, m_shuffle = false;
    int m_repeat = 0;
    double m_position = 0, m_length = 0;
    long m_seekHead = -1; // PaintSeek: the head's device pixel
    bool m_seeking = false;
    float m_seekPreview = 0, m_volume = 1;
    bool m_favorite = false;
    std::shared_ptr<const lyrics::Lyrics> m_lyrics;
    int m_lyricLine = -2, m_lyricFill = -1;
    std::array<winrt::Windows::UI::Composition::SpriteVisual, 2> m_lyricLines{nullptr, nullptr};
    std::array<gfx::RetainedSurface, 2> m_lyricBackings;
    int m_lyricCurrent = 0;
    int m_miniCoverHeight = 0; // px: the mini window's height before the pane opened
    RECT m_sizeFrom{}, m_sizeTo{};
    ULONGLONG m_sizeStart = 0;
    winrt::com_ptr<IWICBitmapSource> m_art;
    bool m_artPending = false;
    int m_artToken = 0;
};

} // namespace glasswave::ui
