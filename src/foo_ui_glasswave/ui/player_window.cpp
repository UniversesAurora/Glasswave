#include "pch.h"

#include "ui/player_window.h"

#include "aero/aero_atlas.h"
#include "aero/aero_metrics.h"
#include "aero/aero_painter.h"
#include "gfx/composition_host.h"
#include "ui/art_cache.h"
#include "ui/i18n.h"
#include "ui/pane_common.h"
#include "ui/text.h"
#include "win7/win7_resources.h"
#include "wmp/wmp_transport.h"

#include <winrt/Windows.Foundation.Collections.h>

namespace glasswave::ui {

namespace m = aero::metrics;
namespace paint = aero::paint;
namespace wt = wmp::transport;
namespace wuc = winrt::Windows::UI::Composition;
using paint::Layer;
using paint::TransportGlyph;
using winrt::com_ptr;
using winrt::Windows::Foundation::Numerics::float2;
using winrt::Windows::Foundation::Numerics::float3;

namespace {

constexpr wchar_t kClassName[] = L"Glasswave.PlayerWindow";
constexpr UINT kFrameMessage = WM_APP + 0x41;
constexpr UINT_PTR kHideTimer = 1, kOverlayTimer = 2;
constexpr UINT kHideAfterMs = 3000; // full screen: controls and pointer leave after this
constexpr UINT kOverlayMs = 1600;   // full screen: the volume overlay stays this long

// DIP. See docs/design/player-window.md.
constexpr float kMiniBar = 80;              // mini: seek row + transport row
constexpr float kWmcBar = 53, kWmcTop = 64; // TWOFOOT.FULLSCREEN.BOTTOMBAR / TOPBAR heights
constexpr float kMiniMinW = 356, kMiniMinH = 236;
constexpr float kMiniW = 396, kMiniH = 540, kMiniPaneH = 720;
constexpr float kStripBelow = 200; // stage lower than this: the strip form
constexpr float kFullBarMaxW = 760, kFullBarMargin = 24; // full screen without ehres.dll
constexpr uint32_t kNavy = 0x0B2C6B; // WMC's blue over the dimmed tiles
constexpr uint32_t kTimeBlue = 0x8CCBFF;
constexpr int kMoveMs = 480, kFadeMs = 260;
constexpr auto kEhres = win7::Module::Ehres;
constexpr auto kWmploc = win7::Module::Wmploc;
constexpr int kSwitchToLibrary = 4603; // wmploc: the "switch to library" corner glyph, 25x25

enum Id : int {
    kStop = 1,
    kPrev,
    kPlay,
    kNext,
    kMute,
    kVolArrow,
    kVolDown,
    kVolUp,
    kLyrics,
    kList,
    kLibrary,
    kMinimize,
    kRestore,
    kExit,
    kCapMin,
    kCapMax,
    kCapClose
};

winrt::Windows::UI::Color Rgba(uint32_t rgb, float alpha) {
    return winrt::Windows::UI::ColorHelper::FromArgb(static_cast<uint8_t>(std::lround(alpha * 255)),
                                                     static_cast<uint8_t>(rgb >> 16),
                                                     static_cast<uint8_t>(rgb >> 8), static_cast<uint8_t>(rgb));
}

RECT CenteredRect(int cx, int cy, int w, int h) {
    return {cx - w / 2, cy - h / 2, cx - w / 2 + w, cy - h / 2 + h};
}

bool Empty(const RECT& r) {
    return r.right <= r.left || r.bottom <= r.top;
}

float SliderFromDb(float db) {
    return db <= -99.0f ? 0.0f : std::clamp(std::pow(10.0f, db / 40.0f), 0.0f, 1.0f);
}

// A Media Center windowed-mode control (ehres TWOFOOT.CONTROL.<name>.<state>) filling `s`.
bool DrawControl(ID2D1DeviceContext* dc, D2D1_SIZE_F s, const wchar_t* control, Layer layer, bool enabled = true) {
    const wchar_t* state = !enabled                  ? L"DISABLED"
                           : layer == Layer::Hover   ? L"FOCUS"
                           : layer == Layer::Pressed ? L"PRESSED"
                                                     : L"NOFOCUS";
    std::wstring name = std::wstring(L"TWOFOOT.CONTROL.") + control + L"." + state + L".PNG";
    if (!win7::Resources::Get().Has(kEhres, name)) // not every control has every state
        name = std::wstring(L"TWOFOOT.CONTROL.") + control + L".NOFOCUS.PNG";
    return win7::DrawStretched(dc, kEhres, name, {0, 0, s.width, s.height});
}

// Fallback plate for the buttons on the dark bar when the original artwork is missing.
void DarkPlate(ID2D1DeviceContext* dc, D2D1_SIZE_F s, Layer layer, bool checked) {
    const float alpha = layer == Layer::Pressed ? 0.10f : layer == Layer::Hover ? 0.20f : checked ? 0.16f : 0.0f;
    if (alpha <= 0) return;
    const D2D1_ROUNDED_RECT plate{{0.5f, 0.5f, s.width - 0.5f, s.height - 0.5f}, 3, 3};
    dc->FillRoundedRectangle(plate, pane::White(dc, alpha).get());
    dc->DrawRoundedRectangle(plate, pane::White(dc, alpha + 0.12f).get(), 1.0f);
}

// A filled rounded rectangle whose edge fades out over `feather`.
void FeatherRect(ID2D1DeviceContext* dc, D2D1_RECT_F r, float feather, uint32_t rgb, float alpha) {
    constexpr int kSteps = 32;
    const float each = 1.0f - std::pow(1.0f - alpha, 1.0f / kSteps);
    com_ptr<ID2D1SolidColorBrush> brush;
    dc->CreateSolidColorBrush(D2D1::ColorF(rgb, each), brush.put());
    for (int i = 0; i < kSteps; ++i) {
        const float d = feather * (1.0f - static_cast<float>(i) / (kSteps - 1));
        const float rad = d + 4;
        dc->FillRoundedRectangle({{r.left - d, r.top - d, r.right + d, r.bottom + d}, rad, rad}, brush.get());
    }
}

void Place(const wuc::Visual& v, const RECT& r, bool animate = false) {
    const float3 offset{static_cast<float>(r.left), static_cast<float>(r.top), 0};
    const float2 size{static_cast<float>(std::max<LONG>(0, r.right - r.left)),
                      static_cast<float>(std::max<LONG>(0, r.bottom - r.top))};
    if (animate) {
        const auto comp = v.Compositor();
        const auto ease = comp.CreateCubicBezierEasingFunction({0.16f, 0.9f}, {0.2f, 1.0f});
        auto move = comp.CreateVector3KeyFrameAnimation();
        move.InsertKeyFrame(1.0f, offset, ease);
        move.Duration(std::chrono::milliseconds(kMoveMs));
        v.StartAnimation(L"Offset", move);
        auto grow = comp.CreateVector2KeyFrameAnimation();
        grow.InsertKeyFrame(1.0f, size, ease);
        grow.Duration(std::chrono::milliseconds(kMoveMs));
        v.StartAnimation(L"Size", grow);
        return;
    }
    v.StopAnimation(L"Offset");
    v.StopAnimation(L"Size");
    v.Offset(offset);
    v.Size(size);
}

wuc::CompositionNineGridBrush NineGrid(int wPx, int hPx, float scale, float insetPx,
                                       const std::function<void(ID2D1DeviceContext*)>& painter) {
    auto& host = gfx::CompositionHost::Get();
    auto surface = host.CreateSurface(wPx, hPx);
    host.Draw(surface, scale, painter);
    auto source = host.Compositor().CreateSurfaceBrush(surface);
    source.Stretch(wuc::CompositionStretch::Fill);
    auto brush = host.Compositor().CreateNineGridBrush();
    brush.Source(source);
    brush.SetInsets(insetPx);
    brush.IsCenterHollow(true);
    return brush;
}

} // namespace

PlayerWindow::PlayerWindow(PlayerHost& host) : m_host(host) {}

PlayerWindow::~PlayerWindow() {
    if (auto* cache = ArtCache::Existing()) cache->Unsubscribe(m_artToken);
    m_frameClock.reset();
    m_shadow.Destroy();
    m_widgets.clear();
    m_wall.reset();
    m_light.reset();
    m_veil.reset();
    m_glass.reset();
    if (m_hwnd) {
        SetWindowLongPtrW(m_hwnd, GWLP_USERDATA, 0);
        DestroyWindow(m_hwnd);
    }
}

// ---- Window --------------------------------------------------------------------------------

LRESULT CALLBACK PlayerWindow::WndProcThunk(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    PlayerWindow* self = nullptr;
    if (msg == WM_NCCREATE) {
        self = static_cast<PlayerWindow*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        self->m_hwnd = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<PlayerWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
    try {
        return self->WndProc(msg, wp, lp);
    } catch (const std::exception& e) {
        console::complain("Glasswave", e.what());
    } catch (const winrt::hresult_error& e) {
        console::complain("Glasswave", pfc::stringcvt::string_utf8_from_wide(e.message().c_str()));
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void PlayerWindow::Create() {
    const HINSTANCE inst = core_api::get_my_instance();
    WNDCLASSEXW wc{sizeof(wc)};
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = &PlayerWindow::WndProcThunk;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = ui_control::get()->get_main_icon();
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc); // failing because it already exists is fine
    CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP | WS_EX_APPWINDOW, kClassName, L"Glasswave",
                    WS_POPUP | WS_CLIPCHILDREN, 0, 0, 100, 100, nullptr, nullptr, inst, this);
    if (!m_hwnd) throw exception_win32(GetLastError());
    m_scale = GetDpiForWindow(m_hwnd) / 96.0f;
    // The same window attributes as the library window: our own Win7 frame and shadow.
    BOOL enable = TRUE;
    DwmSetWindowAttribute(m_hwnd, DWMWA_USE_HOSTBACKDROPBRUSH, &enable, sizeof(enable));
    DWM_WINDOW_CORNER_PREFERENCE corner = DWMWCP_DONOTROUND;
    DwmSetWindowAttribute(m_hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
    COLORREF border = DWMWA_COLOR_NONE;
    DwmSetWindowAttribute(m_hwnd, DWMWA_BORDER_COLOR, &border, sizeof(border));
    DWM_SYSTEMBACKDROP_TYPE backdrop = DWMSBT_NONE;
    DwmSetWindowAttribute(m_hwnd, DWMWA_SYSTEMBACKDROP_TYPE, &backdrop, sizeof(backdrop));
    DWMNCRENDERINGPOLICY ncrp = DWMNCRP_DISABLED;
    DwmSetWindowAttribute(m_hwnd, DWMWA_NCRENDERING_POLICY, &ncrp, sizeof(ncrp));

    m_wmp = m_host.OriginalTransportArt();
    m_ehres = win7::Resources::Get().Has(kEhres, L"TWOFOOT.CONTROL.PLAY.NOFOCUS.PNG");
    m_aeroArt = aero::atlas::Available();
    InitVisuals();
    CreateWidgets();
    m_shadow.Create(m_hwnd);
    m_host.AdoptPlayerWindow(m_hwnd); // the library window's icon and taskbar buttons
    m_artToken = ArtCache::Get().Subscribe([this] {
        if (m_shown && m_artPending) RequestArt();
    });
}

void PlayerWindow::InitVisuals() {
    auto& host = gfx::CompositionHost::Get();
    const auto& comp = host.Compositor();
    auto interop = comp.as<ABI::Windows::UI::Composition::Desktop::ICompositorDesktopInterop>();
    winrt::check_hresult(interop->CreateDesktopWindowTarget(
        m_hwnd, FALSE,
        reinterpret_cast<ABI::Windows::UI::Composition::Desktop::IDesktopWindowTarget**>(
            winrt::put_abi(m_target))));
    // As in the library window: the frame outline lies above the rounded clip, not inside it.
    m_windowRoot = comp.CreateContainerVisual();
    m_windowRoot.RelativeSizeAdjustment(float2{1, 1});
    m_target.Root(m_windowRoot);
    m_root = comp.CreateContainerVisual();
    m_root.RelativeSizeAdjustment(float2{1, 1});
    m_windowRoot.Children().InsertAtTop(m_root);
    m_clipGeometry = comp.CreateRoundedRectangleGeometry();
    m_root.Clip(comp.CreateGeometricClip(m_clipGeometry));
    m_frameOutline = comp.CreateSpriteVisual();
    m_frameOutline.RelativeSizeAdjustment(float2{1, 1});
    m_windowRoot.Children().InsertAtTop(m_frameOutline);

    m_glass = std::make_unique<aero::GlassMaterial>(aero::kWin7Sky, m_aeroArt);
    m_root.Children().InsertAtTop(m_glass->Root());

    // Everything inside the frame, clipped to the client area.
    m_content = comp.CreateContainerVisual();
    m_content.RelativeSizeAdjustment(float2{1, 1});
    m_contentClip = comp.CreateInsetClip();
    m_content.Clip(m_contentClip);
    m_root.Children().InsertAtTop(m_content);
    // Media Center's living light behind everything: it shows before the tiles arrive and
    // through the seams of the wall as its swell passes.
    m_light = std::make_unique<AeroLight>(m_content, AeroLight::Kind::Ground, true);
    m_wall = std::make_unique<AlbumWall>(m_content);

    m_shade = comp.CreateSpriteVisual();
    m_shade.RelativeSizeAdjustment(float2{1, 1});
    m_content.Children().InsertAtTop(m_shade);
    // The same light once more over the covers and the blue, faint: beams wandering across.
    m_veil = std::make_unique<AeroLight>(m_content, AeroLight::Kind::Veil, false);
    m_veil->Root().Opacity(0.9f);

    m_stage = comp.CreateContainerVisual();
    m_stage.RelativeSizeAdjustment(float2{1, 1});
    m_content.Children().InsertAtTop(m_stage);
    for (auto* v : {&m_coverOld, &m_cover, &m_info}) {
        *v = comp.CreateSpriteVisual();
        m_stage.Children().InsertAtTop(*v);
    }
    m_coverOld.Opacity(0.0f);
    for (const auto& v : {m_cover, m_coverOld}) {
        auto shadow = comp.CreateDropShadow();
        shadow.Color(Rgba(0x000000, 1.0f));
        shadow.Opacity(0.6f);
        v.Shadow(shadow);
    }

    m_paneHost = comp.CreateContainerVisual();
    m_paneHost.RelativeSizeAdjustment(float2{1, 1});
    m_content.Children().InsertAtTop(m_paneHost);

    m_barLayer = comp.CreateContainerVisual();
    m_barLayer.RelativeSizeAdjustment(float2{1, 1});
    m_content.Children().InsertAtTop(m_barLayer);
    for (auto* v : {&m_barBack, &m_times, &m_seek, &m_cluster}) {
        *v = comp.CreateSpriteVisual();
        m_barLayer.Children().InsertAtTop(*v);
    }

    m_capsLayer = comp.CreateContainerVisual();
    m_capsLayer.RelativeSizeAdjustment(float2{1, 1});
    m_content.Children().InsertAtTop(m_capsLayer);
    m_topBack = comp.CreateSpriteVisual();
    m_capsLayer.Children().InsertAtTop(m_topBack);

    m_popup = comp.CreateSpriteVisual();
    m_popup.Opacity(0.0f);
    m_content.Children().InsertAtTop(m_popup);

    // The frame: client edge lines and the caption buttons, above the content.
    m_clientEdge = comp.CreateSpriteVisual();
    m_root.Children().InsertAtTop(m_clientEdge);
    m_frameLayer = comp.CreateContainerVisual();
    m_frameLayer.RelativeSizeAdjustment(float2{1, 1});
    m_root.Children().InsertAtTop(m_frameLayer);
}

void PlayerWindow::CreateWidgets() {
    auto add = [&](int id, const wuc::ContainerVisual& layer, Widget::Painter painter,
                   Widget::Options opts) -> Widget* {
        auto w = std::make_unique<Widget>(id, std::move(painter), std::move(opts));
        layer.Children().InsertAtTop(w->Visual());
        m_widgets.push_back(std::move(w));
        return m_widgets.back().get();
    };

    // Transport: Media Center's windowed controls when full screen, WMP 12's cluster in the
    // mini window, procedural painting where the user's files have neither.
    add(kStop, m_barLayer,
        [this](auto* dc, D2D1_SIZE_F s, Layer l) {
            if (m_ehres)
                DrawControl(dc, s, L"STOP", l, m_playing);
            else if (m_wmp)
                wt::ClusterButton(dc, ClusterRegion(m_layout.stop), wt::Button::Stop, l, m_playing);
            else
                paint::IconButton(dc, s, l, TransportGlyph::Stop, m_playing, false);
        },
        {})
        ->onClick = [] { playback_control::get()->stop(); };
    add(kPrev, m_barLayer,
        [this](auto* dc, D2D1_SIZE_F s, Layer l) {
            if (Wmc())
                DrawControl(dc, s, L"PREVIOUS", l, m_playing);
            else if (m_wmp)
                wt::ClusterButton(dc, ClusterRegion(m_layout.prev), wt::Button::Prev, l, m_playing);
            else
                paint::Segment(dc, s, true, l, TransportGlyph::Previous, true);
        },
        {})
        ->onClick = [] { playback_control::get()->previous(); };
    add(kNext, m_barLayer,
        [this](auto* dc, D2D1_SIZE_F s, Layer l) {
            if (Wmc())
                DrawControl(dc, s, L"NEXT", l, m_playing);
            else if (m_wmp)
                wt::ClusterButton(dc, ClusterRegion(m_layout.next), wt::Button::Next, l, m_playing);
            else
                paint::Segment(dc, s, false, l, TransportGlyph::Next, true);
        },
        {})
        ->onClick = [] { playback_control::get()->next(); };
    for (int id : {kStop, kPrev, kNext}) WidgetById(id)->enabled = [this] { return m_playing; };

    Widget::Options orb;
    orb.circular = true;
    orb.haloPulse = !m_wmp;
    orb.haloFactor = m_wmp ? wt::kGlowFactor : 1.6f;
    orb.halo = [this](ID2D1DeviceContext* dc, float size) {
        if (Wmc()) return; // the FOCUS image carries its own glow
        if (m_wmp)
            wt::GlowRing(dc, size);
        else
            paint::OrbHalo(dc, size);
    };
    add(kPlay, m_barLayer,
        [this](auto* dc, D2D1_SIZE_F s, Layer l) {
            const bool showPause = m_playing && !m_paused;
            if (Wmc())
                DrawControl(dc, s, showPause ? L"PAUSE" : L"PLAY", l);
            else if (m_wmp)
                wt::Orb(dc, l, showPause);
            else
                paint::PlayOrb(dc, s.width, l, showPause ? TransportGlyph::Pause : TransportGlyph::Play);
        },
        orb)
        ->onClick = [] {
        auto pc = playback_control::get();
        if (pc->is_playing())
            pc->play_or_pause();
        else
            pc->start();
    };

    add(kMute, m_barLayer,
        [this](auto* dc, D2D1_SIZE_F s, Layer l) {
            if (m_ehres) {
                DrawControl(dc, s, m_muted ? L"MUTEON" : L"MUTE", l);
            } else if (m_wmp) {
                wt::Mute(dc, l, m_muted, m_volume);
            } else {
                DarkPlate(dc, s, l, false);
                const wchar_t glyph = m_muted ? L'\xE74F' : m_volume < 0.34f ? L'\xE993'
                                                        : m_volume < 0.67f ? L'\xE994'
                                                                           : L'\xE995';
                pane::DrawIcon(dc, glyph, 15, {s.width / 2, s.height / 2}, pane::White(dc, 0.92f).get());
            }
        },
        {})
        ->onClick = [] { playback_control::get()->volume_mute_toggle(); };
    // WMP 12's narrow transport: the arrow beside the speaker opens the volume slider.
    add(kVolArrow, m_barLayer,
        [this](auto* dc, D2D1_SIZE_F s, Layer l) {
            if (m_ehres && DrawControl(dc, s, L"DOWN", m_popupOpen && l == Layer::Normal ? Layer::Hover : l)) return;
            DarkPlate(dc, s, l, m_popupOpen);
            const float cx = s.width / 2, cy = s.height / 2;
            com_ptr<ID2D1Factory> f;
            dc->GetFactory(f.put());
            com_ptr<ID2D1PathGeometry> path;
            com_ptr<ID2D1GeometrySink> sink;
            if (FAILED(f->CreatePathGeometry(path.put())) || FAILED(path->Open(sink.put()))) return;
            sink->BeginFigure({cx - 3.5f, cy - 1.5f}, D2D1_FIGURE_BEGIN_FILLED);
            sink->AddLine({cx + 3.5f, cy - 1.5f});
            sink->AddLine({cx, cy + 2.5f});
            sink->EndFigure(D2D1_FIGURE_END_CLOSED);
            sink->Close();
            dc->FillGeometry(path.get(), pane::White(dc, 0.9f).get());
        },
        {})
        ->onClick = [this] { SetPopup(!m_popupOpen); };
    // Media Center: volume down / up beside the speaker.
    add(kVolDown, m_barLayer, [](auto* dc, D2D1_SIZE_F s, Layer l) { DrawControl(dc, s, L"MINUS", l); }, {})
        ->onClick = [this] { StepVolume(-0.04f); };
    add(kVolUp, m_barLayer, [](auto* dc, D2D1_SIZE_F s, Layer l) { DrawControl(dc, s, L"PLUS", l); }, {})
        ->onClick = [this] { StepVolume(0.04f); };

    // Lyrics and queue. Windows 7 has no lyrics glyph: Media Center's teletext page (a page
    // of text) stands for lyrics, its guide list for the queue. The side that shows keeps
    // its focus plate.
    add(kLyrics, m_barLayer,
        [this](auto* dc, D2D1_SIZE_F s, Layer l) {
            const bool on = m_paneShown && m_pane && m_pane->CurrentMode() == NowPlayingPane::Mode::Lyrics;
            if (m_ehres && DrawControl(dc, s, L"TELETEXT", on && l == Layer::Normal ? Layer::Hover : l)) return;
            DarkPlate(dc, s, l, on);
            pane::DrawIcon(dc, L'\xE8BD', 16, {s.width / 2, s.height / 2 + 0.5f}, pane::White(dc, 0.92f).get());
        },
        {})
        ->onClick = [this] { TogglePane(NowPlayingPane::Mode::Lyrics); };
    add(kList, m_barLayer,
        [this](auto* dc, D2D1_SIZE_F s, Layer l) {
            const bool on = m_paneShown && m_pane && m_pane->CurrentMode() == NowPlayingPane::Mode::Queue;
            if (m_ehres && DrawControl(dc, s, L"GUIDE", on && l == Layer::Normal ? Layer::Hover : l)) return;
            DarkPlate(dc, s, l, on);
            pane::DrawIcon(dc, pane::icon::kList, 16, {s.width / 2, s.height / 2}, pane::White(dc, 0.92f).get());
        },
        {})
        ->onClick = [this] { TogglePane(NowPlayingPane::Mode::Queue); };

    // Back to the library: Media Center's back orb when full screen, WMP 12's "switch to
    // library" corner button in the mini window.
    add(kLibrary, m_capsLayer,
        [this](auto* dc, D2D1_SIZE_F s, Layer l) {
            if (m_ehres) {
                DrawControl(dc, s, L"GOBACK", l);
            } else if (m_wmp && win7::Resources::Get().Has(kWmploc, kSwitchToLibrary)) {
                if (l == Layer::Hover || l == Layer::Pressed)
                    win7::Draw(dc, kWmploc, l == Layer::Hover ? 4725 : 4724, {0, 0});
                win7::Draw(dc, kWmploc, kSwitchToLibrary, {0, 0});
            } else {
                com_ptr<ID2D1SolidColorBrush> dark;
                dc->CreateSolidColorBrush(D2D1::ColorF(0x061633, l == Layer::Normal ? 0.45f : 0.65f), dark.put());
                dc->FillRoundedRectangle({{0, 0, s.width, s.height}, 3, 3}, dark.get());
                pane::DrawIcon(dc, L'\xE8F1', 13, {s.width / 2, s.height / 2}, pane::White(dc, 0.95f).get());
            }
        },
        {})
        ->onClick = [this] { Close(); };
    // Media Center's own caption buttons, top right of the full screen.
    add(kMinimize, m_capsLayer, [](auto* dc, D2D1_SIZE_F s, Layer l) { DrawControl(dc, s, L"MINIMIZE", l); }, {})
        ->onClick = [this] { ShowWindow(m_hwnd, SW_MINIMIZE); };
    add(kRestore, m_capsLayer, [](auto* dc, D2D1_SIZE_F s, Layer l) { DrawControl(dc, s, L"RESTORE", l); }, {})
        ->onClick = [this] { Show(Mode::Mini); };
    add(kExit, m_capsLayer, [](auto* dc, D2D1_SIZE_F s, Layer l) { DrawControl(dc, s, L"EXIT", l); }, {})
        ->onClick = [this] { Close(); };

    // The mini window's Win7 caption buttons (the library window's, same artwork).
    const bool dwmArt = m_aeroArt;
    auto captionOptions = [&](bool close) {
        Widget::Options o;
        o.inactiveLayer = true;
        if (dwmArt) {
            o.glow = [close](ID2D1DeviceContext* dc, D2D1_SIZE_F s) { aero::atlas::CaptionGlow(dc, s, close); };
            o.glowOutset = aero::atlas::GlowOutset(close);
            o.glowOutsetInPixels = true;
        }
        return o;
    };
    add(kCapMin, m_frameLayer,
        [dwmArt](auto* dc, D2D1_SIZE_F s, Layer l) {
            if (dwmArt)
                aero::atlas::CaptionButton(dc, s, aero::atlas::Button::Minimize, paint::CaptionGlyph::Minimize, l);
            else
                paint::CaptionButton(dc, s, paint::CaptionGlyph::Minimize, l, true, false);
        },
        captionOptions(false))
        ->onClick = [this] { ShowWindow(m_hwnd, SW_MINIMIZE); };
    add(kCapMax, m_frameLayer,
        [dwmArt](auto* dc, D2D1_SIZE_F s, Layer l) {
            if (dwmArt)
                aero::atlas::CaptionButton(dc, s, aero::atlas::Button::Maximize, paint::CaptionGlyph::Maximize, l);
            else
                paint::CaptionButton(dc, s, paint::CaptionGlyph::Maximize, l, false, false);
        },
        captionOptions(false))
        ->onClick = [this] { Show(Mode::Full); }; // maximizing the player is the full screen
    add(kCapClose, m_frameLayer,
        [dwmArt](auto* dc, D2D1_SIZE_F s, Layer l) {
            if (dwmArt)
                aero::atlas::CaptionButton(dc, s, aero::atlas::Button::Close, paint::CaptionGlyph::Close, l);
            else
                paint::CaptionButton(dc, s, paint::CaptionGlyph::Close, l, false, true);
        },
        captionOptions(true))
        ->onClick = [this] { Close(); };
}

Widget* PlayerWindow::WidgetById(int id) const {
    for (const auto& w : m_widgets)
        if (w->Id() == id) return w.get();
    return nullptr;
}

Widget* PlayerWindow::WidgetAt(POINT pt) const {
    if (m_popupOpen && m_mode == Mode::Mini && PtInRect(&m_layout.popup, pt)) return nullptr;
    for (auto it = m_widgets.rbegin(); it != m_widgets.rend(); ++it)
        if ((*it)->HitTest(pt)) return it->get();
    return nullptr;
}

void PlayerWindow::SetHover(Widget* w) {
    if (w == m_hover) return;
    if (m_hover) m_hover->SetHover(false);
    m_hover = w;
    if (m_hover) m_hover->SetHover(true);
}

RECT PlayerWindow::ClusterRect(const D2D1_RECT_F& r) const {
    const POINT o = m_layout.cluster;
    return {o.x + PxI(r.left), o.y + PxI(r.top), o.x + PxI(r.right), o.y + PxI(r.bottom)};
}

D2D1_RECT_F PlayerWindow::ClusterRegion(const RECT& r) const {
    const POINT o = m_layout.cluster;
    return {(r.left - o.x) / m_scale, (r.top - o.y) / m_scale, (r.right - o.x) / m_scale,
            (r.bottom - o.y) / m_scale};
}

// ---- Showing -------------------------------------------------------------------------------

void PlayerWindow::Show(Mode mode) {
    const bool first = !m_shown;
    if (!m_hwnd) Create();
    if (!first && mode == m_mode) {
        if (IsIconic(m_hwnd)) ShowWindow(m_hwnd, SW_RESTORE);
        SetForegroundWindow(m_hwnd);
        return;
    }
    if (first) {
        m_pane = m_host.LendPane(m_paneHost);
        m_wall->SetAlbums(m_host.WallAlbums());
        m_volume = m_host.PlayerVolume();
        m_muted = playback_control::get()->is_muted();
        m_track.release();
        m_art = nullptr;
    }
    m_mode = mode;
    m_shown = true;
    ApplyMode(first);
    if (first) {
        ReadTrack();
        m_wall->Reveal();
        // The stage and the controls follow the first tiles.
        for (const wuc::Visual& v : {wuc::Visual(m_stage), wuc::Visual(m_barLayer)}) {
            v.Opacity(0.0f);
            auto fade = v.Compositor().CreateScalarKeyFrameAnimation();
            fade.InsertKeyFrame(0.0f, 0.0f);
            fade.InsertKeyFrame(1.0f, 1.0f);
            fade.Duration(std::chrono::milliseconds(420));
            fade.DelayTime(std::chrono::milliseconds(260));
            fade.DelayBehavior(wuc::AnimationDelayBehavior::SetInitialValueBeforeDelay);
            v.StartAnimation(L"Opacity", fade);
        }
        if (m_pane) m_pane->SetVisible(m_paneShown);
        if (m_paneShown && m_mode == Mode::Mini) {
            m_stage.StopAnimation(L"Opacity");
            m_stage.Opacity(0.0f);
        }
    } else {
        gfx::CompositionHost::Get().AnimateOpacity(m_stage, m_paneShown && m_mode == Mode::Mini ? 0.0f : 1.0f,
                                                   kFadeMs);
    }
    UpdateMotion();
    SetForegroundWindow(m_hwnd);
}

void PlayerWindow::ApplyMode(bool first) {
    SetPopup(false);
    HMONITOR monitor = MonitorFromWindow(first ? core_api::get_main_window() : m_hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(monitor, &mi);
    if (!first && m_mode == Mode::Full && !IsIconic(m_hwnd)) {
        GetWindowRect(m_hwnd, &m_miniRect); // leaving the mini form: remember where it was
        m_haveMiniRect = true;
    }
    const bool mini = m_mode == Mode::Mini;
    m_glass->Root().IsVisible(mini);
    m_frameOutline.IsVisible(mini);
    m_clientEdge.IsVisible(mini);
    m_frameLayer.IsVisible(mini);
    if (!mini) {
        SetWindowLongPtrW(m_hwnd, GWL_STYLE, WS_POPUP | WS_CLIPCHILDREN | WS_MINIMIZEBOX);
        const RECT& r = mi.rcMonitor;
        SetWindowPos(m_hwnd, HWND_NOTOPMOST, r.left, r.top, r.right - r.left, r.bottom - r.top,
                     SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    } else {
        SetWindowLongPtrW(m_hwnd, GWL_STYLE,
                          WS_POPUP | WS_THICKFRAME | WS_MINIMIZEBOX | WS_SYSMENU | WS_CLIPCHILDREN);
        if (!m_haveMiniRect) {
            const float scale = GetDpiForWindow(m_hwnd) / 96.0f;
            const int w = static_cast<int>(kMiniW * scale);
            const int h = static_cast<int>((m_paneShown ? kMiniPaneH : kMiniH) * scale);
            const int margin = static_cast<int>(32 * scale);
            m_miniRect = {mi.rcWork.right - margin - w, mi.rcWork.bottom - margin - h, mi.rcWork.right - margin,
                          mi.rcWork.bottom - margin};
            m_haveMiniRect = true;
        }
        SetWindowPos(m_hwnd, HWND_TOPMOST, m_miniRect.left, m_miniRect.top, m_miniRect.right - m_miniRect.left,
                     m_miniRect.bottom - m_miniRect.top, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    }
    m_scale = GetDpiForWindow(m_hwnd) / 96.0f;
    m_chromeShown = false;
    ShowChrome(true);
    UpdateLayout(false);
    for (auto& w : m_widgets) w->Invalidate(); // the two forms draw different artwork
    m_glass->UpdateReflectionAnchor(m_hwnd);
    m_shadow.Sync();
    ArmHideTimer();
}

void PlayerWindow::Close() {
    if (!m_shown) return;
    m_shown = false;
    SetPopup(false);
    KillTimer(m_hwnd, kHideTimer);
    KillTimer(m_hwnd, kOverlayTimer);
    if (m_cursorHidden) {
        m_cursorHidden = false;
        SetCursor(LoadCursorW(nullptr, IDC_ARROW));
    }
    if (m_mode == Mode::Mini && !IsIconic(m_hwnd)) GetWindowRect(m_hwnd, &m_miniRect);
    if (m_frameClock) m_frameClock->Stop();
    if (m_pane) m_pane->SetVisible(false);
    m_pane = nullptr;
    m_capture = Capture::None;
    SetHover(nullptr);
    m_pressed = nullptr;
    m_host.ReturnPane();
    ShowWindow(m_hwnd, SW_HIDE);
    UpdateMotion();
    m_shadow.Sync();
    m_host.PlayerClosed();
}

void PlayerWindow::UpdateMotion() {
    const bool visible = m_shown && m_hwnd && !IsIconic(m_hwnd);
    if (m_wall) m_wall->SetRunning(visible && m_playing && !m_paused);
    if (m_light) m_light->SetRunning(visible);
    if (m_veil) m_veil->SetRunning(visible);
}

void PlayerWindow::StartFrames() {
    if (!m_shown || !m_hwnd) return;
    if (!m_frameClock) m_frameClock = std::make_unique<gfx::FrameClock>(m_hwnd, kFrameMessage);
    m_frameClock->Start();
}

void PlayerWindow::SetPaneShown(bool shown, NowPlayingPane::Mode mode) {
    if (!m_pane) return;
    const bool changed = shown != m_paneShown;
    m_paneShown = shown;
    if (shown) m_pane->SetMode(mode);
    if (changed && m_mode == Mode::Mini && !IsIconic(m_hwnd)) {
        // The mini window grows downwards for the pane and returns to its cover height.
        RECT wr{};
        GetWindowRect(m_hwnd, &wr);
        MONITORINFO mi{sizeof(mi)};
        GetMonitorInfoW(MonitorFromWindow(m_hwnd, MONITOR_DEFAULTTONEAREST), &mi);
        const int want = PxI(shown ? kMiniPaneH : kMiniH);
        int height = wr.bottom - wr.top;
        if (shown ? height < want : height > want) height = want;
        height = std::min<int>(height, mi.rcWork.bottom - mi.rcWork.top);
        int top = wr.top;
        if (top + height > mi.rcWork.bottom) top = mi.rcWork.bottom - height;
        SetWindowPos(m_hwnd, nullptr, wr.left, top, wr.right - wr.left, height, SWP_NOZORDER | SWP_NOACTIVATE);
    }
    if (changed) {
        m_pane->SetVisible(shown);
        gfx::CompositionHost::Get().AnimateOpacity(m_stage, shown && m_mode == Mode::Mini ? 0.0f : 1.0f, kFadeMs);
    }
    UpdateLayout(changed && m_mode == Mode::Full);
    for (int id : {kLyrics, kList})
        if (auto* w = WidgetById(id)) w->Invalidate();
}

void PlayerWindow::TogglePane(NowPlayingPane::Mode mode) {
    if (!m_pane) return;
    if (m_paneShown && m_pane->CurrentMode() == mode)
        SetPaneShown(false, mode);
    else
        SetPaneShown(true, mode);
}

void PlayerWindow::ShowChrome(bool show) {
    if (show == m_chromeShown) return;
    m_chromeShown = show;
    auto& host = gfx::CompositionHost::Get();
    host.AnimateOpacity(m_capsLayer, show ? 1.0f : 0.0f, show ? 160 : 420);
    if (m_mode == Mode::Full) {
        host.AnimateOpacity(m_barLayer, show ? 1.0f : 0.0f, show ? 160 : 420);
        m_cursorHidden = !show;
        SetCursor(show ? LoadCursorW(nullptr, IDC_ARROW) : nullptr);
    } else {
        m_barLayer.StopAnimation(L"Opacity");
        m_barLayer.Opacity(1.0f);
    }
}

void PlayerWindow::ArmHideTimer() {
    KillTimer(m_hwnd, kHideTimer);
    if (m_shown && m_mode == Mode::Full) SetTimer(m_hwnd, kHideTimer, kHideAfterMs, nullptr);
}

void PlayerWindow::SetPopup(bool open) {
    if (!m_popup) return;
    if (open == m_popupOpen) {
        if (open) PaintPopup();
        return;
    }
    m_popupOpen = open;
    auto& host = gfx::CompositionHost::Get();
    if (open) PaintPopup();
    host.AnimateOpacity(m_popup, open ? 1.0f : 0.0f, open ? 120 : 220);
    if (auto* w = WidgetById(kVolArrow)) w->Invalidate();
}

void PlayerWindow::StepVolume(float delta) {
    m_volume = std::clamp(m_volume + delta, 0.0f, 1.0f);
    m_host.SetPlayerVolume(m_volume);
    if (auto* w = WidgetById(kMute)) w->Invalidate();
    if (m_mode == Mode::Full) {
        // Media Center shows the level top right for a moment.
        SetPopup(true);
        SetTimer(m_hwnd, kOverlayTimer, kOverlayMs, nullptr);
    } else if (m_popupOpen) {
        PaintPopup();
    }
}

// ---- Layout --------------------------------------------------------------------------------

void PlayerWindow::UpdateLayout(bool animate) {
    RECT rc{};
    GetClientRect(m_hwnd, &rc);
    const int W = rc.right, H = rc.bottom;
    if (W <= 0 || H <= 0) return;
    m_client = {W, H};
    Layout L;
    const bool full = m_mode == Mode::Full;
    const bool wmc = Wmc();
    const int line = std::max(1, static_cast<int>(std::floor(m_scale)));

    // Frame.
    if (full) {
        L.client = {0, 0, W, H};
    } else {
        const int border = PxI(m::kResizeBorder), capH = PxI(m::kCaptionHeight);
        L.client = {border, capH, W - border, H - border};
        L.clientEdge = {L.client.left - 2, L.client.top - 2, L.client.right + 2, L.client.bottom + 2};
        const bool dwm = m_aeroArt;
        const int capRight = W - PxI(dwm ? 20.0f / 3 : m::kCapBtnRightInset);
        const int capTop = dwm ? line : PxI(m::kCapBtnTopInset);
        const int bh = PxI(dwm ? 29.0f / 1.5f : m::kCapBtnH);
        L.capClose = {capRight - PxI(dwm ? 73.0f / 1.5f : m::kCapBtnCloseW), capTop, capRight, capTop + bh};
        L.capMax = {L.capClose.left - PxI(dwm ? 41.0f / 1.5f : m::kCapBtnMaxW), capTop, L.capClose.left, capTop + bh};
        L.capMin = {L.capMax.left - PxI(dwm ? 43.0f / 1.5f : m::kCapBtnMinW), capTop, L.capMax.left, capTop + bh};
    }
    const RECT C = L.client;
    const int CW = C.right - C.left;

    if (wmc) {
        // Media Center: the bottom bar across the screen, its controls at the right in WMC's
        // order, the seek bar in the space left of them; back orb and caption buttons on top.
        L.bar = {0, H - PxI(kWmcBar), W, H};
        L.top = {0, 0, W, PxI(kWmcTop)};
        const int cy = L.bar.top + PxI(kWmcBar / 2 + 1);
        int x = W - PxI(14);
        const auto take = [&](float w, float h, int gap = 0) {
            const RECT r{x - PxI(w), cy - PxI(h) / 2, x, cy - PxI(h) / 2 + PxI(h)};
            x = r.left - gap;
            return r;
        };
        L.volUp = take(38, 31);
        L.volDown = take(38, 31);
        L.mute = take(38, 31, PxI(5));
        L.divider[0] = take(2, 20, PxI(5));
        L.next = take(38, 31);
        L.play = take(42, 43);
        L.prev = take(38, 31);
        L.stop = take(38, 31, PxI(5));
        L.divider[1] = take(2, 20, PxI(5));
        L.list = take(38, 31);
        L.lyrics = take(29, 24, PxI(28));
        L.timeLeft = {PxI(20), cy - PxI(9), PxI(20 + 52), cy + PxI(9)};
        L.timeRight = {x - PxI(52), cy - PxI(9), x, cy + PxI(9)};
        L.seek = {L.timeLeft.right + PxI(2), cy - PxI(12), L.timeRight.left - PxI(2), cy + PxI(12)};
        L.library = {PxI(14), PxI(8), PxI(14 + 42), PxI(8 + 42)};
        L.exit = {W - PxI(12 + 29), PxI(10), W - PxI(12), PxI(10 + 24)};
        L.restore = {L.exit.left - PxI(29), L.exit.top, L.exit.left, L.exit.bottom};
        L.minimize = {L.restore.left - PxI(29), L.exit.top, L.restore.left, L.exit.bottom};
        L.popup = {W - PxI(14 + 178), PxI(48), W - PxI(14), PxI(48 + 58)};
    } else {
        // WMP 12's narrow transport: seek row, then stop / previous / play / next centred,
        // the speaker with its arrow at the left, lyrics and queue at the right.
        const int barH = PxI(kMiniBar);
        if (full) {
            const int bw = std::min(W - 2 * PxI(kFullBarMargin), PxI(kFullBarMaxW));
            L.bar = {(W - bw) / 2, H - PxI(kFullBarMargin) - barH, (W - bw) / 2 + bw, H - PxI(kFullBarMargin)};
            L.library = {W - PxI(16 + 30), PxI(16), W - PxI(16), PxI(16 + 26)};
        } else {
            L.bar = {C.left, C.bottom - barH, C.right, C.bottom};
            if (m_ehres)
                L.library = {C.left + PxI(6), C.top + PxI(6), C.left + PxI(6 + 30), C.top + PxI(6 + 30)};
            else
                L.library = {C.right - PxI(8 + 25), C.top + PxI(8), C.right - PxI(8), C.top + PxI(8 + 25)};
        }
        const int rowY = L.bar.top + PxI(4);
        L.timeLeft = {L.bar.left + PxI(8), rowY + PxI(3), L.bar.left + PxI(8 + 46), rowY + PxI(21)};
        L.timeRight = {L.bar.right - PxI(8 + 46), rowY + PxI(3), L.bar.right - PxI(8), rowY + PxI(21)};
        L.seek = {L.timeLeft.right, rowY, L.timeRight.left, rowY + PxI(24)};
        const int cx = (L.bar.left + L.bar.right) / 2, cy = L.bar.top + PxI(52);
        if (m_wmp) {
            m_layout.cluster = L.cluster = {cx - PxI(108), cy - PxI(26)};
            L.play = ClusterRect(wt::kOrbRect);
            L.prev = ClusterRect(wt::kPrevRect);
            L.next = ClusterRect(wt::kNextRect);
            L.stop = ClusterRect(wt::kStopRect);
            if (m_ehres) { // Media Center's stop, on the centre of WMP's
                const int sx = (L.stop.left + L.stop.right) / 2 - PxI(6);
                L.stop = CenteredRect(sx, cy, PxI(38), PxI(31));
            }
        } else {
            const int orbPx = PxI(m::kOrbDiameter + 2.0f), segH = PxI(m::kSegmentHeight);
            L.play = CenteredRect(cx, cy, orbPx, orbPx);
            L.prev = {cx - PxI(m::kSegmentReach), cy - segH / 2, cx, cy - segH / 2 + segH};
            L.next = {cx, cy - segH / 2, cx + PxI(m::kSegmentReach), cy - segH / 2 + segH};
            L.stop = CenteredRect(cx + PxI(m::kStopX), cy, PxI(m::kIconButtonW), PxI(m::kIconButtonH));
        }
        const int muteW = PxI(m_ehres ? 38.0f : m_wmp ? 25.0f : 28.0f), muteH = PxI(m_ehres ? 31.0f : m_wmp ? 25.0f : 26.0f);
        L.mute = {L.bar.left + PxI(m_ehres ? 4.0f : 10.0f), cy - muteH / 2, L.bar.left + PxI(m_ehres ? 4.0f : 10.0f) + muteW,
                  cy - muteH / 2 + muteH};
        L.volArrow = {L.mute.right + PxI(1), cy - PxI(8), L.mute.right + PxI(17), cy + PxI(8)};
        if (m_ehres) {
            L.list = {L.bar.right - PxI(8 + 38), cy - PxI(15), L.bar.right - PxI(8), cy - PxI(15) + PxI(31)};
            L.lyrics = {L.list.left - PxI(29), cy - PxI(12), L.list.left, cy + PxI(12)};
        } else {
            L.list = {L.bar.right - PxI(10 + 30), cy - PxI(13), L.bar.right - PxI(10), cy + PxI(13)};
            L.lyrics = {L.list.left - PxI(34), L.list.top, L.list.left - PxI(4), L.list.bottom};
        }
        if (full)
            L.popup = {W - PxI(16 + 178), PxI(52), W - PxI(16), PxI(52 + 58)};
        else
            L.popup = {L.mute.left, L.mute.top - PxI(34), L.mute.left + PxI(174), L.mute.top - PxI(4)};
    }

    // The stage (cover and text) and the lyrics / queue pane.
    const int stageTop = C.top, stageH = L.bar.top - C.top;
    if (full) {
        if (m_paneShown) {
            const int colW = static_cast<int>(W * 0.46f);
            const int side = static_cast<int>(std::min(H * 0.44f, colW * 0.62f));
            const int textH = PxI(124);
            const int top = std::max(PxI(48), (H - side - textH) / 2 - PxI(24));
            L.cover = {(colW - side) / 2, top, (colW - side) / 2 + side, top + side};
            L.info = {PxI(32), L.cover.bottom + PxI(22), colW - PxI(32), L.cover.bottom + PxI(22) + textH};
            L.infoStyle = 0;
            L.panel = {static_cast<int>(W * 0.52f), PxI(wmc ? kWmcTop + 4 : 64.0f), W - static_cast<int>(W * 0.04f),
                       L.bar.top - PxI(8)};
            L.pane = L.panel;
        } else {
            // WMC: the cover with the song's lines beside it, centred as a group.
            const int side = static_cast<int>(H * 0.40f);
            const int gap = static_cast<int>(side * 0.085f);
            const int textW = static_cast<int>(std::min(W * 0.34f, side * 1.7f));
            const int left = (W - side - gap - textW) / 2, top = (H - side) / 2 - static_cast<int>(H * 0.03f);
            L.cover = {left, top, left + side, top + side};
            L.info = {L.cover.right + gap, top, L.cover.right + gap + textW, top + side};
            L.infoStyle = 1;
        }
    } else if (m_paneShown) {
        L.panel = {C.left, C.top, C.right, L.bar.top};
        L.pane = L.panel;
    } else if (stageH < PxI(kStripBelow)) {
        const int side = std::max(PxI(40), stageH - PxI(24));
        L.cover = {C.left + PxI(12), stageTop + PxI(12), C.left + PxI(12) + side, stageTop + PxI(12) + side};
        L.info = {L.cover.right + PxI(14), L.cover.top, C.right - PxI(12 + 30), L.cover.bottom};
        L.infoStyle = 2;
    } else {
        const int textH = PxI(84);
        // The cover leaves a good margin of wall on every side.
        const int side = std::max(PxI(60), std::min(static_cast<int>(CW * 0.64f), stageH - textH - PxI(52)));
        const int top = stageTop + std::max(PxI(12), (stageH - textH - side) / 2);
        L.cover = {C.left + (CW - side) / 2, top, C.left + (CW - side) / 2 + side, top + side};
        L.info = {C.left + PxI(14), L.cover.bottom + PxI(12), C.right - PxI(14), L.cover.bottom + PxI(12) + textH};
        L.infoStyle = 0;
    }
    const bool infoChanged = !EqualRect(&L.info, &m_layout.info) || L.infoStyle != m_layout.infoStyle;
    m_layout = L;

    // Apply.
    const float radius = full ? 0.0f : Px(m::kCornerRadius);
    m_clipGeometry.Size(float2{static_cast<float>(W), static_cast<float>(H)});
    m_clipGeometry.CornerRadius(float2{radius, radius});
    m_contentClip.LeftInset(static_cast<float>(C.left));
    m_contentClip.TopInset(static_cast<float>(C.top));
    m_contentClip.RightInset(static_cast<float>(W - C.right));
    m_contentClip.BottomInset(static_cast<float>(H - C.bottom));
    if (!full) {
        Place(m_clientEdge, L.clientEdge);
        if (m_framePainted != m_scale) PaintFrame();
    }
    m_wall->SetSize({W, H}, m_scale);
    m_light->SetSize({W, H});
    m_veil->SetSize({W, H});
    if (m_pane && m_paneShown) m_pane->SetBounds(L.pane, m_scale);

    for (auto& w : m_widgets) {
        RECT r{};
        switch (w->Id()) {
        case kStop: r = L.stop; break;
        case kPrev: r = L.prev; break;
        case kPlay: r = L.play; break;
        case kNext: r = L.next; break;
        case kMute: r = L.mute; break;
        case kVolArrow: r = L.volArrow; break;
        case kVolDown: r = L.volDown; break;
        case kVolUp: r = L.volUp; break;
        case kLyrics: r = L.lyrics; break;
        case kList: r = L.list; break;
        case kLibrary: r = L.library; break;
        case kMinimize: r = L.minimize; break;
        case kRestore: r = L.restore; break;
        case kExit: r = L.exit; break;
        case kCapMin: r = L.capMin; break;
        case kCapMax: r = L.capMax; break;
        case kCapClose: r = L.capClose; break;
        }
        w->SetBounds(r, m_scale);
    }
    m_cluster.IsVisible(m_wmp && !wmc);
    if (m_wmp && !wmc) {
        const RECT cl = ClusterRect(wt::kClusterRect);
        Place(m_cluster, cl);
        gfx::CompositionHost::Get().Repaint(m_cluster, cl.right - cl.left, cl.bottom - cl.top, m_scale,
                                            [](ID2D1DeviceContext* dc) { wt::ClusterBackground(dc); });
    }

    if (!Empty(L.cover)) {
        const SIZE want{L.cover.right - L.cover.left, L.cover.bottom - L.cover.top};
        // Paint for the larger of the two sizes before an animated move, so nothing is upscaled.
        if (want.cx > m_coverPainted.cx || !animate) {
            m_coverPainted = want;
            PaintCover();
            RequestArt();
        }
        Place(m_cover, L.cover, animate);
        Place(m_coverOld, L.cover, animate);
        for (const auto& v : {m_cover, m_coverOld}) {
            if (auto shadow = v.Shadow().try_as<wuc::DropShadow>()) {
                shadow.BlurRadius(want.cx * 0.07f + Px(8));
                shadow.Offset(float3{0, want.cx * 0.02f + Px(2), 0});
            }
        }
    }
    if (!Empty(L.info)) {
        Place(m_info, L.info);
        if (infoChanged && animate) {
            m_info.Opacity(0.0f);
            auto fade = m_info.Compositor().CreateScalarKeyFrameAnimation();
            fade.InsertKeyFrame(0.0f, 0.0f);
            fade.InsertKeyFrame(1.0f, 1.0f);
            fade.Duration(std::chrono::milliseconds(320));
            fade.DelayTime(std::chrono::milliseconds(180));
            fade.DelayBehavior(wuc::AnimationDelayBehavior::SetInitialValueBeforeDelay);
            m_info.StartAnimation(L"Opacity", fade);
        }
        PaintInfo();
    }
    Place(m_barBack, L.bar);
    Place(m_topBack, L.top);
    Place(m_seek, L.seek);
    Place(m_times, RECT{L.bar.left, L.timeLeft.top, L.bar.right, L.timeLeft.bottom});
    Place(m_popup, L.popup);
    PaintBars();
    PaintSeek();
    PaintTimes();
    PaintShade();
    if (m_popupOpen) PaintPopup();
}

// The mini window's Win7 frame lines; one device pixel at any DPI, as DWM draws them.
void PlayerWindow::PaintFrame() {
    m_framePainted = m_scale;
    const float s = m_scale;
    m_glass->SetScale(s);
    const int frameSrc = PxI(32);
    m_frameOutline.Brush(NineGrid(frameSrc, frameSrc, s, static_cast<float>(PxI(m::kCornerRadius + 2)),
                                  [&](ID2D1DeviceContext* dc) {
                                      paint::FrameOutline(dc, D2D1::SizeF(frameSrc / s, frameSrc / s),
                                                          m::kCornerRadius);
                                  }));
    const int edgeSrc = PxI(16);
    m_clientEdge.Brush(NineGrid(edgeSrc, edgeSrc, s, 3.0f, [&](ID2D1DeviceContext* dc) {
        paint::ClientEdge(dc, D2D1::SizeF(edgeSrc / s, edgeSrc / s));
    }));
}

// The blue that WMC lays over the tiles behind its text, painted small and stretched. With
// ehres.dll it is WMC's own gradient (NOWPLAYING.MUSIC.META.GRADIENT).
void PlayerWindow::PaintShade() {
    constexpr int kDown = 8;
    const int w = std::max<int>(1, m_client.cx / kDown), h = std::max<int>(1, m_client.cy / kDown);
    auto& host = gfx::CompositionHost::Get();
    const auto surface = host.CreateSurface(w, h);
    const float W = m_client.cx / m_scale, H = m_client.cy / m_scale;
    const Layout& L = m_layout;
    const float s = m_scale;
    const auto dip = [s](const RECT& r) { return D2D1_RECT_F{r.left / s, r.top / s, r.right / s, r.bottom / s}; };
    host.Draw(surface, m_scale / kDown, [&](ID2D1DeviceContext* dc) {
        com_ptr<ID2D1SolidColorBrush> base;
        dc->CreateSolidColorBrush(D2D1::ColorF(kNavy, 0.22f), base.put());
        dc->FillRectangle({0, 0, W, H}, base.get());
        if (!Empty(L.panel)) {
            // Behind the lyrics / queue the wall sinks into deep water: no edge anywhere, the
            // blue only grows from the cover's side towards the text.
            const D2D1_RECT_F p = dip(L.panel);
            if (m_mode == Mode::Full) {
                const float from = p.left - (p.right - p.left) * 0.22f, to = p.left + (p.right - p.left) * 0.10f;
                const D2D1_GRADIENT_STOP stops[] = {{0, D2D1::ColorF(0x06204F, 0.0f)},
                                                    {0.45f, D2D1::ColorF(0x06204F, 0.52f)},
                                                    {1, D2D1::ColorF(0x051A42, 0.86f)}};
                com_ptr<ID2D1GradientStopCollection> collection;
                dc->CreateGradientStopCollection(stops, 3, collection.put());
                com_ptr<ID2D1LinearGradientBrush> deep;
                dc->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({from, 0}, {to, 0}), collection.get(),
                                              deep.put());
                dc->FillRectangle({from, 0, W, H}, deep.get());
            } else {
                com_ptr<ID2D1SolidColorBrush> deep;
                dc->CreateSolidColorBrush(D2D1::ColorF(0x051A42, 0.86f), deep.put());
                dc->FillRectangle(p, deep.get());
            }
        }
        if (Empty(L.info) || (m_paneShown && m_mode == Mode::Mini)) return;
        const D2D1_RECT_F info = dip(L.info), cover = dip(L.cover);
        D2D1_RECT_F band{};
        if (L.infoStyle == 1) {
            const float pad = (cover.bottom - cover.top) * 0.22f;
            band = {cover.left - pad * 2.2f, cover.top - pad, info.right + pad * 1.5f, cover.bottom + pad};
        } else if (L.infoStyle == 0) {
            const float mid = (info.left + info.right) / 2;
            const float half = std::min((info.right - info.left) / 2 + 30, 290.0f * std::clamp((info.right - info.left) / 340.0f, 1.0f, 1.7f));
            band = {mid - half, info.top - 30, mid + half, info.bottom + 6};
        } else {
            band = {-20, cover.top - 30, W + 20, cover.bottom + 30};
        }
        if (!m_ehres || !win7::DrawStretched(dc, kEhres, L"NOWPLAYING.MUSIC.META.GRADIENT.PNG", band)) {
            const float f = (band.bottom - band.top) * 0.2f;
            FeatherRect(dc, {band.left + f, band.top + f, band.right - f, band.bottom - f}, f, kNavy, 0.76f);
        }
    });
    auto brush = host.Compositor().CreateSurfaceBrush(surface);
    brush.Stretch(wuc::CompositionStretch::Fill);
    m_shade.Brush(brush);
}

// Backgrounds of the control bar and (full screen) the top bar.
void PlayerWindow::PaintBars() {
    auto& host = gfx::CompositionHost::Get();
    const Layout& L = m_layout;
    const float s = m_scale;
    const bool wmc = Wmc();
    {
        const int w = L.bar.right - L.bar.left, h = L.bar.bottom - L.bar.top;
        const float W = w / s, H = h / s;
        host.Repaint(m_barBack, m_barBacking, w, h, s, [&](ID2D1DeviceContext* dc) {
            if (wmc) {
                // Media Center's full-screen bottom bar, its two halves meeting in the middle.
                win7::DrawStretched(dc, kEhres, L"TWOFOOT.FULLSCREEN.BOTTOMBAR.LEFT.PNG", {0, 0, W / 2, H});
                win7::DrawStretched(dc, kEhres, L"TWOFOOT.FULLSCREEN.BOTTOMBAR.RIGHT.PNG", {W / 2, 0, W, H});
                for (const RECT& d : L.divider)
                    win7::DrawStretched(dc, kEhres, L"TWOFOOT.CONTROL.DIVIDER.PNG",
                                        {(d.left - L.bar.left) / s, (d.top - L.bar.top) / s,
                                         (d.right - L.bar.left) / s, (d.bottom - L.bar.top) / s});
                return;
            }
            // WMP 12's now-playing controls stand on black; here the wall fades into it.
            const D2D1_GRADIENT_STOP stops[] = {{0, D2D1::ColorF(0x041230, 0.0f)},
                                                {0.30f, D2D1::ColorF(0x051739, 0.70f)},
                                                {1, D2D1::ColorF(0x030C24, 0.94f)}};
            com_ptr<ID2D1GradientStopCollection> collection;
            dc->CreateGradientStopCollection(stops, 3, collection.put());
            com_ptr<ID2D1LinearGradientBrush> fill;
            dc->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({0, 0}, {0, H}), collection.get(),
                                          fill.put());
            dc->FillRectangle({0, 0, W, H}, fill.get());
        });
    }
    m_topBack.IsVisible(wmc);
    if (wmc) {
        const int w = L.top.right - L.top.left, h = L.top.bottom - L.top.top;
        const float W = w / s;
        host.Repaint(m_topBack, m_topBacking, w, h, s, [&](ID2D1DeviceContext* dc) {
            win7::DrawStretched(dc, kEhres, L"TWOFOOT.FULLSCREEN.TOPBAR.LEFT.PNG", {0, 0, W / 2, 55});
            win7::DrawStretched(dc, kEhres, L"TWOFOOT.FULLSCREEN.TOPBAR.RIGHT.PNG", {W / 2, 0, W, 64});
        });
    }
}

void PlayerWindow::PaintInfo() {
    const RECT& r = m_layout.info;
    if (Empty(r)) return;
    const int w = r.right - r.left, h = r.bottom - r.top;
    const float W = w / m_scale, H = h / m_scale;
    const std::wstring title = m_title.empty() ? Tr(L"Nothing playing") : m_title;
    const int style = m_layout.infoStyle;
    gfx::CompositionHost::Get().Repaint(m_info, m_infoBacking, w, h, m_scale, [&](ID2D1DeviceContext* dc) {
        const auto white = [&](float a) { return pane::White(dc, a); };
        if (style == 1) {
            // WMC: title, artist, album and the time in light blue, one under the other.
            const float side = H, titleSize = side * 0.096f, lineSize = side * 0.072f, pitch = side * 0.172f;
            const int lines = 1 + (m_artist.empty() ? 0 : 1) + (m_album.empty() ? 0 : 1) + (m_track.is_valid() ? 1 : 0);
            float y = (H - pitch * lines) / 2 + (pitch - titleSize * 1.35f) / 2;
            pane::DrawTextLine(dc, title, titleSize, DWRITE_FONT_WEIGHT_NORMAL, {0, y}, W, white(0.97f).get());
            y += pitch;
            for (const std::wstring* line : {&m_artist, &m_album}) {
                if (line->empty()) continue;
                pane::DrawTextLine(dc, *line, lineSize, DWRITE_FONT_WEIGHT_NORMAL, {0, y + side * 0.012f}, W,
                                   white(0.90f).get());
                y += pitch;
            }
            if (m_track.is_valid()) {
                com_ptr<ID2D1SolidColorBrush> blue;
                dc->CreateSolidColorBrush(D2D1::ColorF(kTimeBlue), blue.put());
                pane::DrawTextLine(dc, FormatTime(m_seeking ? m_seekPreview * m_length : m_position), titleSize * 0.9f,
                                   DWRITE_FONT_WEIGHT_NORMAL, {0, y}, W, blue.get());
            }
        } else if (style == 0) {
            // The same lines centred under the cover (WMC's type, smaller).
            const float k = std::clamp(W / 340.0f, 1.0f, 1.7f);
            pane::DrawTextLine(dc, title, 19 * k, DWRITE_FONT_WEIGHT_NORMAL, {0, 0}, W, white(0.97f).get(),
                               DWRITE_TEXT_ALIGNMENT_CENTER);
            pane::DrawTextLine(dc, m_artist, 14 * k, DWRITE_FONT_WEIGHT_NORMAL, {0, 29 * k}, W, white(0.90f).get(),
                               DWRITE_TEXT_ALIGNMENT_CENTER);
            pane::DrawTextLine(dc, m_album, 14 * k, DWRITE_FONT_WEIGHT_NORMAL, {0, 51 * k}, W, white(0.80f).get(),
                               DWRITE_TEXT_ALIGNMENT_CENTER);
        } else {
            const float mid = H / 2;
            std::wstring second = m_artist;
            if (!m_album.empty()) second += (second.empty() ? L"" : L" \x2014 ") + m_album;
            pane::DrawTextLine(dc, title, 17, DWRITE_FONT_WEIGHT_NORMAL, {0, mid - 25}, W, white(0.97f).get());
            pane::DrawTextLine(dc, second, 13, DWRITE_FONT_WEIGHT_NORMAL, {0, mid + 2}, W, white(0.84f).get());
        }
    });
}

void PlayerWindow::PaintCover() {
    const int px = std::max<int>(1, m_coverPainted.cx);
    auto& host = gfx::CompositionHost::Get();
    const auto surface = host.CreateSurface(px, px);
    const float side = px / m_scale;
    host.Draw(surface, m_scale, [&](ID2D1DeviceContext* dc) {
        const D2D1_RECT_F box{0, 0, side, side};
        com_ptr<ID2D1Bitmap> bitmap;
        if (m_art) dc->CreateBitmapFromWicBitmap(m_art.get(), bitmap.put());
        if (bitmap) {
            const D2D1_SIZE_F s = bitmap->GetSize();
            const float k = std::max(side / std::max(1.0f, s.width), side / std::max(1.0f, s.height));
            const float w = s.width * k, h = s.height * k;
            const D2D1_RECT_F dst{(side - w) / 2, (side - h) / 2, (side + w) / 2, (side + h) / 2};
            dc->DrawBitmap(bitmap.get(), &dst, 1.0f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, nullptr, nullptr);
        } else if (!m_ehres || !win7::DrawStretched(dc, kEhres, L"AUDIO.NOCOVER.PNG", box)) {
            // Media Center's no-cover art is missing too: a blue tile with a note.
            const D2D1_GRADIENT_STOP stops[] = {{0, D2D1::ColorF(0x3F8FD8)}, {1, D2D1::ColorF(0x123E86)}};
            com_ptr<ID2D1GradientStopCollection> collection;
            dc->CreateGradientStopCollection(stops, 2, collection.put());
            com_ptr<ID2D1LinearGradientBrush> fill;
            dc->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({0, 0}, {side, side}), collection.get(),
                                          fill.put());
            dc->FillRectangle(box, fill.get());
            pane::DrawIcon(dc, L'\xEC4F', side * 0.36f, {side / 2, side / 2}, pane::White(dc, 0.85f).get());
        }
        // WMC frames its cover with a thin light edge.
        const float edge = std::max(1.0f / m_scale, std::round(side * 0.006f * m_scale) / m_scale);
        dc->DrawRectangle({edge / 2, edge / 2, side - edge / 2, side - edge / 2}, pane::White(dc, 0.82f).get(), edge);
    });
    auto brush = host.Compositor().CreateSurfaceBrush(surface);
    brush.Stretch(wuc::CompositionStretch::Fill);
    m_cover.Brush(brush);
}

// Media Center's seek bar (ehres SEEKBAR.*): a dark groove, the past in white, the future
// in blue-grey, a glowing knob. 24 DIP high like its bitmaps.
void PlayerWindow::PaintSeek() {
    const RECT& r = m_layout.seek;
    if (Empty(r)) return;
    const int w = r.right - r.left, h = r.bottom - r.top;
    const float W = w / m_scale, H = h / m_scale;
    const float progress =
        m_seeking ? m_seekPreview : (m_length > 0 ? static_cast<float>(std::clamp(m_position / m_length, 0.0, 1.0)) : 0);
    const bool active = m_seeking || m_seekHover;
    gfx::CompositionHost::Get().Repaint(m_seek, m_seekBacking, w, h, m_scale, [&](ID2D1DeviceContext* dc) {
        const float pad = 12, x0 = pad, x1 = W - pad, x = x0 + (x1 - x0) * progress, top = (H - 24) / 2;
        if (m_ehres) {
            // Measured in the bitmaps: the groove is solid in columns 9 - 17 of 27; the past is
            // a 9-column glow tail and then solid white (columns 9 - 13 of 14), the future the
            // mirror image in a fainter white. Only solid columns may stretch.
            win7::DrawNineGrid(dc, kEhres, L"SEEKBAR.PROGRESS.BACKGROUND.PNG", {0, 0, 27, 24}, {11, 0, 11, 0},
                               {x0 - 11, top, x1 + 11, top + 24});
            if (m_length > 0) {
                if (x - x0 >= 1)
                    win7::DrawNineGrid(dc, kEhres, L"SEEKBAR.PAST.PNG", {0, 0, 14, 24}, {11, 0, 1, 0},
                                       {x0 - 9, top, x, top + 24});
                if (x1 - x >= 1)
                    win7::DrawNineGrid(dc, kEhres, L"SEEKBAR.FUTURE.PNG", {0, 0, 14, 24}, {1, 0, 11, 0},
                                       {x, top, x1 + 9, top + 24});
                const float k = active ? 14.0f : 13.0f;
                win7::DrawStretched(dc, kEhres, active ? L"SEEKBAR.POSITION.ACTIVE.PNG" : L"SEEKBAR.POSITION.DORMANT.PNG",
                                    {x - k, H / 2 - k, x + k, H / 2 + k});
            }
            return;
        }
        const float cy = H / 2, half = 2.0f;
        dc->FillRoundedRectangle({{x0, cy - half, x1, cy + half}, half, half}, pane::White(dc, 0.24f).get());
        if (m_length <= 0) return;
        com_ptr<ID2D1SolidColorBrush> blue;
        dc->CreateSolidColorBrush(D2D1::ColorF(0x4FA8F0), blue.put());
        if (x > x0) dc->FillRoundedRectangle({{x0, cy - half, x, cy + half}, half, half}, blue.get());
        const float rad = active ? 6.0f : 4.5f;
        dc->FillEllipse({{x, cy}, rad, rad}, pane::White(dc, 0.98f).get());
    });
}

void PlayerWindow::PaintTimes() {
    const RECT r{m_layout.bar.left, m_layout.timeLeft.top, m_layout.bar.right, m_layout.timeLeft.bottom};
    if (Empty(r)) return;
    const int w = r.right - r.left, h = r.bottom - r.top;
    const double position = m_seeking ? m_seekPreview * m_length : m_position;
    const float s = m_scale;
    const float leftX = (m_layout.timeLeft.left - r.left) / s;
    const float leftW = (m_layout.timeLeft.right - m_layout.timeLeft.left) / s;
    const float rightX = (m_layout.timeRight.left - r.left) / s;
    gfx::CompositionHost::Get().Repaint(m_times, m_timesBacking, w, h, m_scale, [&](ID2D1DeviceContext* dc) {
        if (!m_track.is_valid()) return;
        const auto ink = pane::White(dc, 0.88f);
        pane::DrawTextLine(dc, FormatTime(position), 12, DWRITE_FONT_WEIGHT_NORMAL, {leftX, 0}, leftW, ink.get(),
                           DWRITE_TEXT_ALIGNMENT_CENTER);
        if (m_length > 0)
            pane::DrawTextLine(dc, L"-" + FormatTime(std::max(0.0, m_length - position)), 12, DWRITE_FONT_WEIGHT_NORMAL,
                               {rightX, 0}, leftW, ink.get(), DWRITE_TEXT_ALIGNMENT_CENTER);
    });
}

// The volume: Media Center's bar (OVERLAYS.VOLUME.BAR) on its blue panel. In the mini window
// it is the slider WMP's arrow opens; full screen it is the overlay with the level in figures.
void PlayerWindow::PaintPopup() {
    const RECT& r = m_layout.popup;
    if (Empty(r)) return;
    const int w = r.right - r.left, h = r.bottom - r.top;
    const float W = w / m_scale, H = h / m_scale;
    const bool overlay = m_mode == Mode::Full;
    const float value = m_muted ? 0.0f : std::clamp(m_volume, 0.0f, 1.0f);
    gfx::CompositionHost::Get().Repaint(m_popup, m_popupBacking, w, h, m_scale, [&](ID2D1DeviceContext* dc) {
        const D2D1_RECT_F box{0, 0, W, H};
        // The panel is translucent; a deeper blue under it keeps the bar readable on bright covers.
        com_ptr<ID2D1SolidColorBrush> under;
        dc->CreateSolidColorBrush(D2D1::ColorF(0x0A2A66, m_ehres ? 0.62f : 0.92f), under.put());
        dc->FillRoundedRectangle({{2, 2, W - 2, H - 2}, 6, 6}, under.get());
        if (!m_ehres || !win7::DrawNineGrid(dc, kEhres, L"CONTEXTMENU.BACKGROUND.PNG", {0, 0, 281, 352},
                                            {12, 12, 12, 12}, box))
            dc->DrawRoundedRectangle({{0.5f, 0.5f, W - 0.5f, H - 0.5f}, 5, 5}, pane::White(dc, 0.35f).get(), 1.0f);
        const float bx = (W - 150) / 2, by = overlay ? H - 20 : (H - 10) / 2;
        if (overlay) {
            wchar_t text[8];
            swprintf_s(text, L"%d", static_cast<int>(std::lround(value * 50)));
            pane::DrawTextLine(dc, text, 22, DWRITE_FONT_WEIGHT_NORMAL, {0, 5}, W, pane::White(dc, 0.97f).get(),
                               DWRITE_TEXT_ALIGNMENT_CENTER);
        }
        const D2D1_RECT_F bar{bx, by, bx + 150, by + 10};
        if (m_ehres && win7::DrawStretched(dc, kEhres, L"OVERLAYS.VOLUME.BAR.OFF.PNG", bar)) {
            dc->PushAxisAlignedClip({bx, by, bx + 150 * value, by + 10}, D2D1_ANTIALIAS_MODE_ALIASED);
            win7::DrawStretched(dc, kEhres, L"OVERLAYS.VOLUME.BAR.ON.PNG", bar);
            dc->PopAxisAlignedClip();
        } else {
            dc->FillRoundedRectangle({bar, 3, 3}, pane::White(dc, 0.25f).get());
            dc->FillRoundedRectangle({{bx, by, bx + 150 * value, by + 10}, 3, 3}, pane::White(dc, 0.9f).get());
        }
    });
}

// ---- Playback ------------------------------------------------------------------------------

void PlayerWindow::ReadTrack() {
    auto pc = playback_control::get();
    m_playing = pc->is_playing();
    m_paused = pc->is_paused();
    metadb_handle_ptr now;
    const bool has = pc->get_now_playing(now);
    const bool changed = has ? (!m_track.is_valid() || now != m_track) : m_track.is_valid();
    std::wstring title, artist, album;
    if (has) {
        m_host.PlayerTrackText(title, artist, album);
        m_position = pc->playback_get_position();
        m_length = pc->playback_get_length();
    } else {
        m_position = m_length = 0;
    }
    const bool textChanged = title != m_title || artist != m_artist || album != m_album;
    m_title = std::move(title);
    m_artist = std::move(artist);
    m_album = std::move(album);
    if (changed) {
        m_track = has ? now : metadb_handle_ptr();
        // The old cover fades out under the new one, which grows in a little (WMC's change).
        auto& host = gfx::CompositionHost::Get();
        m_coverOld.Brush(m_cover.Brush());
        m_coverOld.Opacity(1.0f);
        host.AnimateOpacity(m_coverOld, 0.0f, 380);
        m_art = nullptr;
        RequestArt();
        if (!m_art) PaintCover();
        m_cover.Opacity(0.0f);
        host.AnimateOpacity(m_cover, 1.0f, 380);
        const float side = static_cast<float>(m_layout.cover.right - m_layout.cover.left);
        m_cover.CenterPoint(float3{side / 2, side / 2, 0});
        m_cover.Scale(float3{0.955f, 0.955f, 1});
        host.AnimateScale(m_cover, 1.0f, 460);
    }
    if (changed || textChanged) PaintInfo();
    for (int id : {kPlay, kStop, kPrev, kNext})
        if (auto* w = WidgetById(id)) w->Invalidate();
    PaintSeek();
    PaintTimes();
    UpdateMotion();
}

void PlayerWindow::RequestArt() {
    m_artPending = false;
    if (!m_track.is_valid()) {
        if (m_art) {
            m_art = nullptr;
            PaintCover();
        }
        return;
    }
    const int side = std::max<int>(m_coverPainted.cx, PxI(160));
    const int sizePx = side <= 640 ? 640 : 1280;
    com_ptr<IWICBitmapSource> art;
    const auto state = ArtCache::Get().Find(m_track, sizePx, &art);
    m_artPending = state == ArtCache::State::Pending;
    if (art.get() != m_art.get() && (art || state != ArtCache::State::Pending)) {
        m_art = std::move(art);
        PaintCover();
    }
}

void PlayerWindow::OnPlayback() {
    if (!m_shown) return;
    ReadTrack();
}

void PlayerWindow::OnTime() {
    if (!m_shown) return;
    auto pc = playback_control::get();
    m_position = pc->playback_get_position();
    m_length = pc->playback_get_length();
    if (m_seeking || IsIconic(m_hwnd)) return;
    PaintSeek();
    PaintTimes();
    if (m_layout.infoStyle == 1 && !m_paneShown) PaintInfo();
}

void PlayerWindow::OnVolume() {
    if (!m_shown) return;
    auto pc = playback_control::get();
    m_muted = pc->is_muted();
    if (!m_muted) m_volume = SliderFromDb(pc->get_volume());
    if (auto* w = WidgetById(kMute)) w->Invalidate();
    if (m_popupOpen) PaintPopup();
}

void PlayerWindow::SetVolumeFromX(int x) {
    const RECT& r = m_layout.popup;
    const float mid = (r.left + r.right) / 2.0f, x0 = mid - Px(75), x1 = mid + Px(75);
    m_volume = std::clamp((x - x0) / std::max(1.0f, x1 - x0), 0.0f, 1.0f);
    m_host.SetPlayerVolume(m_volume);
    PaintPopup();
    if (auto* w = WidgetById(kMute)) w->Invalidate();
}

float PlayerWindow::SeekFractionAt(int x) const {
    const RECT& r = m_layout.seek;
    const float x0 = r.left + Px(12), x1 = r.right - Px(12);
    return std::clamp((x - x0) / std::max(1.0f, x1 - x0), 0.0f, 1.0f);
}

// ---- Messages ------------------------------------------------------------------------------

LRESULT PlayerWindow::WndProc(UINT msg, WPARAM wp, LPARAM lp) {
    const HWND hwnd = m_hwnd;
    if (m_host.PlayerWindowMessage(msg, wp, lp)) return 0; // taskbar thumbnail buttons
    switch (msg) {
    case WM_NCCALCSIZE:
        if (wp) return 0; // the whole window is ours; WS_THICKFRAME only gives sizing
        break;

    case WM_NCACTIVATE: {
        const bool active = wp != FALSE;
        if (active != m_active) {
            m_active = active;
            if (m_glass) m_glass->SetActive(active);
            for (auto& w : m_widgets)
                if (w->Id() >= kCapMin) w->SetInactive(!active);
            m_shadow.SetActive(active);
        }
        return DefWindowProcW(hwnd, msg, wp, -1); // -1: do not repaint the (non-existent) frame
    }

    case WM_NCHITTEST: {
        if (m_mode == Mode::Full) return HTCLIENT;
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ScreenToClient(hwnd, &pt);
        const int edge = PxI(m::kResizeBorder);
        const bool left = pt.x < edge, right = pt.x >= m_client.cx - edge;
        const bool top = pt.y < edge / 2 + 1, bottom = pt.y >= m_client.cy - edge;
        if (top && left) return HTTOPLEFT;
        if (top && right) return HTTOPRIGHT;
        if (bottom && left) return HTBOTTOMLEFT;
        if (bottom && right) return HTBOTTOMRIGHT;
        if (left) return HTLEFT;
        if (right) return HTRIGHT;
        if (top) return HTTOP;
        if (bottom) return HTBOTTOM;
        return HTCLIENT;
    }

    case WM_GETMINMAXINFO: {
        auto* info = reinterpret_cast<MINMAXINFO*>(lp);
        info->ptMinTrackSize = {PxI(kMiniMinW), PxI(kMiniMinH)};
        return 0;
    }

    case WM_SIZE:
        if (wp != SIZE_MINIMIZED && m_shown) UpdateLayout(false);
        return 0;

    case WM_MOVE:
        if (m_glass && m_mode == Mode::Mini) m_glass->UpdateReflectionAnchor(hwnd);
        return 0;

    case WM_WINDOWPOSCHANGING: {
        const auto* pos = reinterpret_cast<const WINDOWPOS*>(lp);
        if (m_glass && m_mode == Mode::Mini && !(pos->flags & SWP_NOMOVE) && !IsIconic(hwnd)) {
            RECT wr{};
            GetWindowRect(hwnd, &wr);
            const LONG w = (pos->flags & SWP_NOSIZE) ? wr.right - wr.left : pos->cx;
            const LONG h = (pos->flags & SWP_NOSIZE) ? wr.bottom - wr.top : pos->cy;
            m_glass->UpdateReflectionAnchor(hwnd, RECT{pos->x, pos->y, pos->x + w, pos->y + h});
        }
        break;
    }

    case WM_WINDOWPOSCHANGED: {
        const LRESULT r = DefWindowProcW(hwnd, msg, wp, lp); // generates WM_SIZE / WM_MOVE
        m_shadow.Sync();
        UpdateMotion();
        return r;
    }

    case WM_DPICHANGED: {
        m_scale = HIWORD(wp) / 96.0f;
        const RECT* r = reinterpret_cast<const RECT*>(lp);
        SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        m_coverPainted = {};
        if (m_shown) UpdateLayout(false);
        return 0;
    }

    case WM_EXITSIZEMOVE:
        if (m_mode == Mode::Mini && !IsIconic(hwnd)) GetWindowRect(hwnd, &m_miniRect);
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT:
        ValidateRect(hwnd, nullptr);
        return 0;

    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT && m_cursorHidden) {
            SetCursor(nullptr);
            return TRUE;
        }
        break;

    case kFrameMessage:
        if (m_frameClock) {
            m_frameClock->Ack();
            const bool more = m_shown && !IsIconic(hwnd) && m_pane && m_pane->Tick();
            if (!more) m_frameClock->Stop();
        }
        return 0;

    case WM_TIMER:
        if (wp == kHideTimer) {
            KillTimer(hwnd, kHideTimer);
            POINT pt{};
            GetCursorPos(&pt);
            ScreenToClient(hwnd, &pt);
            const bool busy = m_capture != Capture::None || PtInRect(&m_layout.bar, pt) ||
                              PtInRect(&m_layout.top, pt) || (m_pane && m_paneShown && m_pane->HitTest(pt));
            if (m_mode == Mode::Full && !busy)
                ShowChrome(false);
            else
                ArmHideTimer();
        } else if (wp == kOverlayTimer) {
            KillTimer(hwnd, kOverlayTimer);
            if (m_mode == Mode::Full) SetPopup(false);
        }
        return 0;

    case WM_MOUSEMOVE: {
        const POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        if (!m_tracking) {
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
            TrackMouseEvent(&tme);
            m_tracking = true;
        }
        if (pt.x != m_lastMouse.x || pt.y != m_lastMouse.y) {
            m_lastMouse = pt;
            ShowChrome(true);
            ArmHideTimer();
        }
        switch (m_capture) {
        case Capture::Seek:
            m_seekPreview = SeekFractionAt(pt.x);
            PaintSeek();
            PaintTimes();
            return 0;
        case Capture::Volume:
            SetVolumeFromX(pt.x);
            return 0;
        case Capture::Pane:
            if (m_pane) m_pane->OnMouseMove(pt);
            return 0;
        case Capture::Widget:
            if (m_pressed) m_pressed->SetPressed(m_pressed->HitTest(pt));
            return 0;
        case Capture::None:
            break;
        }
        Widget* w = WidgetAt(pt);
        SetHover(w);
        const bool seekHover = !w && PtInRect(&m_layout.seek, pt) != FALSE;
        if (seekHover != m_seekHover) {
            m_seekHover = seekHover;
            PaintSeek();
        }
        if (m_pane) {
            const bool over = !w && m_paneShown && m_pane->HitTest(pt) &&
                              !(m_popupOpen && PtInRect(&m_layout.popup, pt));
            if (over != m_paneHover) {
                m_paneHover = over;
                if (!over) m_pane->OnMouseLeave();
            }
            if (over) m_pane->OnMouseMove(pt);
        }
        return 0;
    }

    case WM_MOUSELEAVE:
        m_tracking = false;
        SetHover(nullptr);
        if (m_paneHover && m_pane) m_pane->OnMouseLeave();
        m_paneHover = false;
        if (m_seekHover) {
            m_seekHover = false;
            PaintSeek();
        }
        m_lastMouse = {-1, -1};
        if (m_mode == Mode::Mini) ShowChrome(false);
        return 0;

    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK: {
        const POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        SetFocus(hwnd);
        if (m_popupOpen && m_mode == Mode::Mini) {
            if (PtInRect(&m_layout.popup, pt)) {
                m_capture = Capture::Volume;
                SetCapture(hwnd);
                SetVolumeFromX(pt.x);
                return 0;
            }
            const Widget* under = WidgetAt(pt);
            if (!under || under->Id() != kVolArrow) SetPopup(false);
        }
        if (Widget* w = WidgetAt(pt)) {
            if (!w->IsEnabled()) return 0;
            m_pressed = w;
            w->SetPressed(true);
            m_capture = Capture::Widget;
            SetCapture(hwnd);
            return 0;
        }
        if (PtInRect(&m_layout.seek, pt) && m_length > 0) {
            m_capture = Capture::Seek;
            m_seeking = true;
            m_seekPreview = SeekFractionAt(pt.x);
            SetCapture(hwnd);
            PaintSeek();
            PaintTimes();
            return 0;
        }
        if (m_pane && m_paneShown && m_pane->HitTest(pt)) {
            m_capture = Capture::Pane;
            SetCapture(hwnd);
            m_pane->OnLButtonDown(pt);
            return 0;
        }
        if (PtInRect(&m_layout.bar, pt)) return 0;
        if (msg == WM_LBUTTONDBLCLK) {
            Show(m_mode == Mode::Full ? Mode::Mini : Mode::Full);
            return 0;
        }
        if (m_mode == Mode::Mini) {
            // Anywhere else moves the window, like a caption.
            ReleaseCapture();
            SendMessageW(hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
        }
        return 0;
    }

    case WM_LBUTTONUP: {
        const POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        const Capture capture = std::exchange(m_capture, Capture::None);
        if (capture != Capture::None) ReleaseCapture();
        switch (capture) {
        case Capture::Widget: {
            Widget* w = std::exchange(m_pressed, nullptr);
            if (w) {
                w->SetPressed(false);
                if (w->HitTest(pt) && w->IsEnabled() && w->onClick) w->onClick();
            }
            break;
        }
        case Capture::Seek:
            m_seeking = false;
            if (m_length > 0) {
                m_position = SeekFractionAt(pt.x) * m_length;
                playback_control::get()->playback_seek(m_position);
            }
            PaintSeek();
            PaintTimes();
            break;
        case Capture::Pane:
            if (m_pane) {
                m_pane->OnLButtonUp(pt);
                for (int id : {kLyrics, kList}) // its own switch may have changed sides
                    if (auto* w = WidgetById(id)) w->Invalidate();
            }
            break;
        case Capture::Volume:
        case Capture::None:
            break;
        }
        return 0;
    }

    case WM_CAPTURECHANGED:
        if (m_capture == Capture::Pane && m_pane) m_pane->OnCaptureLost();
        if (m_capture == Capture::Widget && m_pressed) m_pressed->SetPressed(false);
        if (m_capture == Capture::Seek) {
            m_seeking = false;
            PaintSeek();
            PaintTimes();
        }
        m_pressed = nullptr;
        m_capture = Capture::None;
        return 0;

    case WM_MOUSEWHEEL: {
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ScreenToClient(hwnd, &pt);
        const int delta = GET_WHEEL_DELTA_WPARAM(wp);
        if (m_pane && m_paneShown && m_pane->HitTest(pt) && !PtInRect(&m_layout.bar, pt)) {
            m_pane->OnWheel(delta);
            return 0;
        }
        StepVolume(delta > 0 ? 0.04f : -0.04f); // elsewhere the wheel is the volume
        return 0;
    }

    case WM_RBUTTONUP: {
        const POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        if (m_pane && m_paneShown && m_pane->HitTest(pt)) {
            POINT screen = pt;
            ClientToScreen(hwnd, &screen);
            m_pane->OnRButtonUp(pt, screen);
        }
        return 0;
    }

    case WM_KEYDOWN:
        switch (wp) {
        case VK_ESCAPE:
            if (m_popupOpen && m_mode == Mode::Mini)
                SetPopup(false);
            else if (m_mode == Mode::Full)
                Show(Mode::Mini);
            else
                Close();
            return 0;
        case VK_F11:
            Show(m_mode == Mode::Full ? Mode::Mini : Mode::Full);
            return 0;
        case VK_SPACE:
            playback_control::get()->play_or_pause();
            return 0;
        case VK_LEFT:
        case VK_RIGHT:
            if (m_length > 0)
                playback_control::get()->playback_seek(
                    std::clamp(m_position + (wp == VK_RIGHT ? 5.0 : -5.0), 0.0, m_length));
            return 0;
        case VK_UP:
        case VK_DOWN:
            StepVolume(wp == VK_UP ? 0.04f : -0.04f);
            return 0;
        case 'L':
            TogglePane(NowPlayingPane::Mode::Lyrics);
            return 0;
        case 'Q':
            TogglePane(NowPlayingPane::Mode::Queue);
            return 0;
        }
        return 0;

    case WM_CLOSE:
        Close();
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace glasswave::ui
