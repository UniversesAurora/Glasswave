#include "pch.h"

#include "ui/player_window_parts.h"

namespace glasswave::ui {

using namespace playerwin;

namespace {

constexpr wchar_t kClassName[] = L"Glasswave.PlayerWindow";
constexpr UINT kFrameMessage = WM_APP + 0x41;
constexpr int kSizeMs = 340;          // the mini window growing or shrinking for the pane
constexpr float kVolumeStep = 0.01f;  // one of the hundred steps every other volume control shows
constexpr UINT kHideAfterMs = 3000; // full screen: controls and pointer leave after this

// DIP. See docs/design/player-window.md.
constexpr float kMiniBar = 80;              // mini: seek row + transport row
constexpr float kWmcBar = 53, kWmcTop = 64; // TWOFOOT.FULLSCREEN.BOTTOMBAR / TOPBAR heights
// The smallest mini window: the transport's row with the volume's split button, the cluster and
// lyrics / queue (the heart, shuffle and repeat leave first), and a client a little higher
// than the control bar, which lies over the picture.
constexpr float kMiniMinW = 344, kMiniMinClientH = 96;
constexpr float kCoverShadow = 0.6f; // the cover's drop shadow on the wall
constexpr float kMiniW = 396, kMiniH = 540, kMiniPaneH = 720;
// The strip form (cover at the left, the lines beside it) for a stage lower than this, or one
// this much wider than high.
constexpr float kStripBelow = 200, kStripAspect = 1.5f;
// Mini, lyrics or queue open: the pane keeps at least this height (more if a lyric line needs
// it); dragging the window this much further closes it.
constexpr float kPaneMinH = 150, kPaneCloseDrag = 56;
constexpr float kFullBarMaxW = 760, kFullBarMargin = 24; // full screen without ehres.dll
// Media Center's back orb coming down with the lyrics or the queue, and going back up.
constexpr int kBackInMs = 520, kBackOutMs = 300;

RECT CenteredRect(int cx, int cy, int w, int h) {
    return {cx - w / 2, cy - h / 2, cx - w / 2 + w, cy - h / 2 + h};
}

// A pane of Aero glass: blue body, a bright reflection over its upper half, a dark hairline
// outside and a light one inside.
void GlassFace(ID2D1DeviceContext* dc, float W, float H, float radius, float scale) {
    const auto vertical = [&](float top, float bottom, std::initializer_list<D2D1_GRADIENT_STOP> stops) {
        com_ptr<ID2D1GradientStopCollection> collection;
        dc->CreateGradientStopCollection(stops.begin(), static_cast<UINT32>(stops.size()), collection.put());
        com_ptr<ID2D1LinearGradientBrush> brush;
        dc->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({0, top}, {0, bottom}), collection.get(),
                                      brush.put());
        return brush;
    };
    const float line = gfx::Hairline(scale);
    const D2D1_ROUNDED_RECT body{{line / 2, line / 2, W - line / 2, H - line / 2}, radius, radius};
    dc->FillRoundedRectangle(body, vertical(0, H, {{0, D2D1::ColorF(0x3F82D6, 0.78f)},
                                                   {0.5f, D2D1::ColorF(0x17489A, 0.82f)},
                                                   {1, D2D1::ColorF(0x0A2C6C, 0.88f)}})
                                       .get());
    const float gloss = H * 0.46f;
    dc->FillRoundedRectangle({{line * 2, line * 2, W - line * 2, gloss}, radius * 0.8f, radius * 0.8f},
                             vertical(0, gloss, {{0, D2D1::ColorF(1, 1, 1, 0.46f)}, {1, D2D1::ColorF(1, 1, 1, 0.10f)}})
                                 .get());
    dc->FillRoundedRectangle({{line * 2, H - H * 0.30f, W - line * 2, H - line * 2}, radius * 0.8f, radius * 0.8f},
                             vertical(H * 0.7f, H, {{0, D2D1::ColorF(0x6FC4FF, 0.0f)}, {1, D2D1::ColorF(0x6FC4FF, 0.26f)}})
                                 .get());
    com_ptr<ID2D1SolidColorBrush> dark;
    dc->CreateSolidColorBrush(D2D1::ColorF(0x020A1E, 0.75f), dark.put());
    dc->DrawRoundedRectangle(body, dark.get(), line);
    dc->DrawRoundedRectangle({{line * 1.5f, line * 1.5f, W - line * 1.5f, H - line * 1.5f}, radius - line, radius - line},
                             pane::White(dc, 0.52f).get(), line);
}

// A visual already placed (and painted) at its new rect starts out drawn at `from`, scaled by
// `scale` from its top left, and grows or shrinks into place on Place's curve, so it keeps
// step with a neighbour that Place animates.
void Morph(const wuc::Visual& v, const RECT& from, float scale) {
    const auto comp = v.Compositor();
    const auto ease = gfx::Easing(gfx::curve::kPlace);
    const float3 to = v.Offset();
    v.StopAnimation(L"Opacity");
    v.Opacity(1.0f);
    v.CenterPoint(float3{0, 0, 0});
    auto move = comp.CreateVector3KeyFrameAnimation();
    move.InsertKeyFrame(0.0f, float3{static_cast<float>(from.left), static_cast<float>(from.top), 0});
    move.InsertKeyFrame(1.0f, to, ease);
    move.Duration(std::chrono::milliseconds(kMoveMs));
    v.StartAnimation(L"Offset", move);
    auto grow = comp.CreateVector3KeyFrameAnimation();
    grow.InsertKeyFrame(0.0f, float3{scale, scale, 1});
    grow.InsertKeyFrame(1.0f, float3{1, 1, 1}, ease);
    grow.Duration(std::chrono::milliseconds(kMoveMs));
    v.StartAnimation(L"Scale", grow);
}

} // namespace

PlayerWindow::PlayerWindow(PlayerHost& host) : m_host(host) {
    m_deviceReplaced = gfx::CompositionHost::Get().OnDeviceReplaced([this] { OnDeviceReplaced(); });
}

// Every surface came back empty. The scale-keyed "already painted" marks are forgotten, and
// what UpdateLayout and the widgets paint is painted again (retained surfaces made under the
// old device are recreated by their Repaint). A hidden window does this when it is shown.
void PlayerWindow::OnDeviceReplaced() {
    m_coverPainted = {};
    m_framePainted = 0;
    m_paneGlassScale = 0;
    m_frostScale = 0;
    if (!m_shown || !m_hwnd) return;
    UpdateLayout(false);
    for (auto& w : m_widgets)
        w->Invalidate();
}

PlayerWindow::~PlayerWindow() {
    if (auto* cache = ArtCache::Existing()) cache->Unsubscribe(m_artToken);
    m_frameClock.reset();
    m_shadow.Destroy();
    m_widgets.clear();
    m_volumePanel.reset();
    m_volumeBubble.reset();
    m_wall.reset();
    m_light.reset();
    m_veil.reset();
    m_frame.reset();
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
    return GuardedWindowProc(hwnd, msg, wp, lp, [&] { return self->WndProc(msg, wp, lp); });
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
    // The library window's Win7 frame: the same window attributes, glass and outline.
    m_frame = std::make_unique<aero::GlassWindow>(m_hwnd, m_host.PlayerColorization(), m_aeroArt);
    m_root = m_frame->Clipped();

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
    m_shadeOld = comp.CreateSpriteVisual(); // the shade before a change, fading out
    m_shadeOld.RelativeSizeAdjustment(float2{1, 1});
    m_shadeOld.Opacity(0.0f);
    m_content.Children().InsertAtTop(m_shadeOld);
    // The same light once more over the covers and the blue, faint: beams wandering across.
    m_veil = std::make_unique<AeroLight>(m_content, AeroLight::Kind::Veil, false);
    m_veil->Root().Opacity(0.9f);

    m_stage = comp.CreateContainerVisual();
    m_stage.RelativeSizeAdjustment(float2{1, 1});
    m_content.Children().InsertAtTop(m_stage);
    for (auto* v : {&m_coverOld, &m_cover, &m_info, &m_lyric}) {
        *v = comp.CreateSpriteVisual();
        m_stage.Children().InsertAtTop(*v);
    }
    // The sung line: two lines taking turns inside a clip, so one can roll out as the next
    // rolls in.
    m_lyric.Clip(comp.CreateInsetClip());
    for (auto& v : m_lyricLines) {
        v = comp.CreateSpriteVisual();
        v.Opacity(0.0f);
        m_lyric.Children().InsertAtTop(v);
    }
    m_coverOld.Opacity(0.0f);
    for (const auto& v : {m_cover, m_coverOld}) {
        auto shadow = comp.CreateDropShadow();
        shadow.Color(Rgba(0x000000, 1.0f));
        shadow.Opacity(kCoverShadow);
        v.Shadow(shadow);
    }
    // The cover's glass edge over each picture, apart from it so the compact window can fade it.
    for (size_t i = 0; i < m_coverFrame.size(); ++i) {
        m_coverFrame[i] = comp.CreateSpriteVisual();
        m_coverFrame[i].RelativeSizeAdjustment(float2{1, 1});
        (i == 0 ? m_cover : m_coverOld).Children().InsertAtTop(m_coverFrame[i]);
    }

    m_paneHost = comp.CreateContainerVisual();
    m_paneHost.RelativeSizeAdjustment(float2{1, 1});
    m_content.Children().InsertAtTop(m_paneHost);
    m_paneCard = comp.CreateSpriteVisual();
    m_paneCard.IsVisible(false);
    m_paneHost.Children().InsertAtBottom(m_paneCard);
    m_paneGlass = comp.CreateSpriteVisual(); // the card's glass, over its shadow and outline
    m_paneGlass.IsVisible(false);
    m_paneGlassShape = comp.CreateRoundedRectangleGeometry();
    m_paneGlass.Clip(comp.CreateGeometricClip(m_paneGlassShape));
    m_paneHost.Children().InsertAbove(m_paneGlass, m_paneCard);

    m_barLayer = comp.CreateContainerVisual();
    m_barLayer.RelativeSizeAdjustment(float2{1, 1});
    m_content.Children().InsertAtTop(m_barLayer);
    for (auto* v : {&m_barBack, &m_times, &m_seek, &m_slider}) {
        *v = comp.CreateSpriteVisual();
        m_barLayer.Children().InsertAtTop(*v);
    }
    m_seekThumb.Create(m_barLayer, false);
    m_sliderThumb.Create(m_barLayer, true);
    m_splitPlate = comp.CreateSpriteVisual(); // under the split button's two widgets
    m_splitPlate.Opacity(0.0f);
    m_barLayer.Children().InsertAtTop(m_splitPlate);
    m_cluster = comp.CreateSpriteVisual();
    m_barLayer.Children().InsertAtTop(m_cluster);

    m_capsLayer = comp.CreateContainerVisual();
    m_capsLayer.RelativeSizeAdjustment(float2{1, 1});
    m_content.Children().InsertAtTop(m_capsLayer);
    m_topBack = comp.CreateSpriteVisual();
    m_capsLayer.Children().InsertAtTop(m_topBack);

    m_menuLayer = comp.CreateContainerVisual();
    m_menuLayer.RelativeSizeAdjustment(float2{1, 1});
    m_content.Children().InsertAtTop(m_menuLayer);

    m_volumePanel = std::make_unique<WmcVolume>(m_content, WmcVolume::Kind::Panel);
    m_flyoutGlass = comp.CreateSpriteVisual(); // the transport behind the flyout's frame, frosted
    m_flyoutGlass.IsVisible(false);
    m_content.Children().InsertAtTop(m_flyoutGlass);
    m_volumeFlyout = comp.CreateSpriteVisual();
    m_volumeFlyout.IsVisible(false);
    m_content.Children().InsertAtTop(m_volumeFlyout);
    m_flyoutBall.Create(m_content, true); // its thumb, over it
    m_flyoutBall.root.IsVisible(false);
    m_volumeBubble = std::make_unique<WmcVolume>(m_content, WmcVolume::Kind::Bubble); // over the flyout

    // The frame: client edge lines and the caption buttons, above the content.
    m_clientEdge = comp.CreateSpriteVisual();
    m_root.Children().InsertAtTop(m_clientEdge);
    m_frameLayer = comp.CreateContainerVisual();
    m_frameLayer.RelativeSizeAdjustment(float2{1, 1});
    m_root.Children().InsertAtTop(m_frameLayer);
}

void PlayerWindow::FadingThumb::Create(const wuc::ContainerVisual& parent, bool visible) {
    const auto& comp = gfx::CompositionHost::Get().Compositor();
    root = comp.CreateContainerVisual();
    root.Opacity(visible ? 1.0f : 0.0f);
    shown = visible;
    parent.Children().InsertAtTop(root);
    for (size_t i = 0; i < looks.size(); ++i) {
        looks[i] = comp.CreateSpriteVisual();
        looks[i].Opacity(i == 0 ? 1.0f : 0.0f); // at rest, lit, held
        root.Children().InsertAtTop(looks[i]);
    }
}

void PlayerWindow::FadingThumb::Paint(int w, int h, float scale, bool force,
                                      const std::function<void(ID2D1DeviceContext*, int)>& look) {
    if (!root || (paintedScale == scale && !force)) return;
    paintedScale = scale;
    for (int i = 0; i < 3; ++i) {
        auto& sprite = looks[static_cast<size_t>(i)];
        sprite.Size(float2{static_cast<float>(w), static_cast<float>(h)});
        gfx::CompositionHost::Get().Repaint(sprite, backings[static_cast<size_t>(i)], w, h, scale,
                                            [&](ID2D1DeviceContext* dc) { look(dc, i); });
    }
}

void PlayerWindow::FadingThumb::Place(int x, int y, int w, int h) {
    if (!root) return;
    root.Offset(float3{static_cast<float>(x), static_cast<float>(y), 0});
    root.Size(float2{static_cast<float>(w), static_cast<float>(h)});
}

void PlayerWindow::FadingThumb::Show(bool show, int inMs, int outMs) {
    if (!root || show == shown) return;
    shown = show;
    if (inMs <= 0 && outMs <= 0) {
        root.StopAnimation(L"Opacity");
        root.Opacity(show ? 1.0f : 0.0f);
        return;
    }
    FadeTo(root, show ? 1.0f : 0.0f, show ? inMs : outMs);
}

void PlayerWindow::FadingThumb::SetState(bool lit, bool hold, int inMs, int outMs) {
    if (!root) return;
    if (lit != hot) {
        hot = lit;
        FadeTo(looks[1], lit ? 1.0f : 0.0f, lit ? inMs : outMs);
    }
    if (hold != held) {
        held = hold;
        FadeTo(looks[2], hold ? 1.0f : 0.0f, hold ? 60 : outMs);
    }
}

void PlayerWindow::WallAlbumsChanged() {
    if (m_shown && m_wall) m_wall->UpdateAlbums(m_host.WallAlbums());
}

void PlayerWindow::RadioChanged() {
    bool shuffle = false;
    int repeat = 0;
    m_host.PlayerMode(shuffle, repeat);
    m_shuffle = shuffle;
    m_repeat = repeat;
    for (int id : {kPrev, kShuffle, kRepeat, kMenuShuffle, kMenuRepeat})
        if (auto* w = WidgetById(id)) w->Invalidate();
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
    // The pane changes size, scale of its content and form between the two: hide it across
    // the change so its rows are laid out and painted afresh instead of showing old surfaces.
    const bool repane = !first && m_pane && m_paneShown;
    if (repane) m_pane->SetVisible(false);
    ApplyMode(first);
    if (repane && m_paneShown) { // ApplyMode may have closed it (a mini window too small)
        m_pane->SetVisible(true);
        StartFrames();
    }
    if (first) {
        ReadTrack();
        m_wall->Reveal();
        // The stage and the controls follow the first tiles.
        for (const wuc::Visual& v : {wuc::Visual(m_stage), wuc::Visual(m_barLayer)}) {
            v.Opacity(0.0f);
            gfx::FadeIn(v, 420, 260);
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
        RememberMiniRect(); // leaving the mini form: remember where it was
    }
    const bool mini = m_mode == Mode::Mini;
    m_shadow.SetSuppressed(!mini); // its corners showed as rounded lines round the screen
    m_frame->Glass().Root().IsVisible(mini); // the outline follows in UpdateLayout
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
            // Where it was left last time, if that is still on a screen.
            if (const auto saved = settings::MiniPlayerRect(); saved && MonitorFromRect(&*saved, MONITOR_DEFAULTTONULL)) {
                m_miniRect = *saved;
                m_haveMiniRect = true;
            }
        }
        if (!m_haveMiniRect) {
            // The window itself still sits on the primary monitor: size it for the one it goes to.
            UINT dpiX = 96, dpiY = 96;
            if (FAILED(GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY)) || !dpiX) dpiX = 96;
            const float scale = dpiX / 96.0f;
            const int w = static_cast<int>(std::lround(kMiniW * scale));
            const int h = static_cast<int>(std::lround((m_paneShown ? kMiniPaneH : kMiniH) * scale));
            const int margin = static_cast<int>(std::lround(32 * scale));
            m_miniRect = {mi.rcWork.right - margin - w, mi.rcWork.bottom - margin - h, mi.rcWork.right - margin,
                          mi.rcWork.bottom - margin};
            m_haveMiniRect = true;
        }
        SetWindowPos(m_hwnd, HWND_NOTOPMOST, m_miniRect.left, m_miniRect.top, m_miniRect.right - m_miniRect.left,
                     m_miniRect.bottom - m_miniRect.top, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        // The lyrics or the queue opened full screen do not fit every mini window: one lower
        // than the mini window grows to for them (or a compact one) shows the song instead, at
        // the size it was left (owner, 2026-11-14: the pane stood under the control bar).
        if (m_paneShown && m_pane) {
            const float s = GetDpiForWindow(m_hwnd) / 96.0f;
            const int w = m_miniRect.right - m_miniRect.left, h = m_miniRect.bottom - m_miniRect.top;
            if (w < std::lround(kMiniMinW * s) || h < std::lround(kMiniPaneH * s)) {
                m_paneShown = false;
                m_miniCoverHeight = 0;
                m_pane->SetVisible(false);
                m_stage.StopAnimation(L"Opacity");
                m_stage.Opacity(1.0f);
                for (int id : {kLyrics, kList, kMenuLyrics, kMenuList, kLibrary})
                    if (auto* widget = WidgetById(id)) widget->Invalidate();
            }
        }
    }
    m_scale = GetDpiForWindow(m_hwnd) / 96.0f;
    m_chromeShown = false;
    ShowChrome(true);
    UpdateLayout(false);
    for (auto& w : m_widgets) w->Invalidate(); // the two forms draw different artwork
    m_frame->Glass().UpdateReflectionAnchor(m_hwnd);
    m_shadow.Sync();
    ArmHideTimer();
}

// The mini window's place, for its next showing and the next start.
void PlayerWindow::RememberMiniRect() {
    if (!GetWindowRect(m_hwnd, &m_miniRect)) return;
    m_haveMiniRect = true;
    settings::SetMiniPlayerRect(m_miniRect);
}

void PlayerWindow::Close() {
    if (!m_shown) return;
    m_shown = false;
    SetPopup(false);
    for (UINT_PTR timer : {kHideTimer, kOverlayTimer, kRepeatTimer, kSizeTimer, kBubbleTimer})
        KillTimer(m_hwnd, timer);
    if (m_cursorHidden) {
        m_cursorHidden = false;
        SetCursor(LoadCursorW(nullptr, IDC_ARROW));
    }
    if (m_mode == Mode::Mini && !IsIconic(m_hwnd)) RememberMiniRect();
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
    // The sung line follows the words while music plays, on the compositor's frames: a
    // WM_TIMER is only synthesised when no posted message waits, and the frame clocks of this
    // thread's windows keep posting at the display's rate, so a timer could starve to the
    // once-a-second playback time callback.
    if (LyricLineLive() || SeekLive()) StartFrames();
    if (m_wall) { // paused, the wall slows down rather than stopping (WMC)
        m_wall->SetPaused(m_paused);
        m_wall->SetRunning(visible && m_playing);
    }
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
    const bool mini = m_mode == Mode::Mini;
    m_paneShown = shown;
    if (shown) m_pane->SetMode(mode);
    auto& host = gfx::CompositionHost::Get();
    const auto& comp = host.Compositor();
    if (changed) {
        m_pane->SetVisible(shown);
        host.AnimateOpacity(m_stage, shown && mini ? 0.0f : 1.0f, kFadeMs);
        if (shown) {
            // The pane arrives: it rises out of the blue (from the right when full screen)
            // and settles, instead of standing there at once.
            const float3 from = mini ? float3{0, Px(28), 0} : float3{Px(56), 0, 0};
            auto move = comp.CreateVector3KeyFrameAnimation();
            move.InsertKeyFrame(0.0f, from);
            move.InsertKeyFrame(1.0f, float3{0, 0, 0}, gfx::Easing(gfx::curve::kSlideIn));
            move.Duration(Ms(560));
            m_paneHost.StartAnimation(L"Offset", move);
            constexpr gfx::Curve kPaneFade{0.3f, 0.0f, 0.3f, 1.0f};
            gfx::FadeIn(m_paneHost, 460, mini ? 60 : 120, &kPaneFade);
        }
    }
    if (changed && mini && !IsIconic(m_hwnd)) {
        // The mini window grows downwards for the pane and returns to the very height it had.
        RECT wr{};
        GetWindowRect(m_hwnd, &wr);
        MONITORINFO mi{sizeof(mi)};
        GetMonitorInfoW(MonitorFromWindow(m_hwnd, MONITOR_DEFAULTTONEAREST), &mi);
        int height = wr.bottom - wr.top;
        if (shown) {
            m_miniCoverHeight = height;
            height = std::max(height, PxI(kMiniPaneH));
        } else if (m_miniCoverHeight > 0) {
            height = m_miniCoverHeight;
        }
        height = std::min<int>(height, mi.rcWork.bottom - mi.rcWork.top);
        int top = wr.top;
        if (top + height > mi.rcWork.bottom) top = mi.rcWork.bottom - height;
        AnimateWindowTo({wr.left, top, wr.right, top + height});
    }
    UpdateLayout(changed);
    for (int id : {kLyrics, kList, kMenuLyrics, kMenuList, kLibrary})
        if (auto* w = WidgetById(id)) w->Invalidate();
}

// Media Center's back orb belongs to the lyrics and the queue: when one of them opens it
// slides down from above the screen, quickly out of the top and softly into place, and when
// the song returns it goes back up the way it came. Out of sight it is transparent and takes
// no clicks; Widget::SetBounds keeps placing it, which the running slide overrides.
void PlayerWindow::SlideBack(bool animate) {
    auto* w = WidgetById(kLibrary);
    if (!w) return;
    const bool in = !Wmc() || m_paneShown;
    const RECT& b = w->Bounds();
    const wuc::Visual v = w->Visual();
    const float3 at{static_cast<float>(b.left), static_cast<float>(b.top), 0};
    const float3 above{at.x, -static_cast<float>(b.bottom - b.top) - Px(8), 0};
    // Only the pane's coming and going slides it; entering or leaving the full screen sets it.
    const bool slide = animate && m_shown && !Empty(b) && Wmc() == m_backWmc;
    m_backWmc = Wmc();
    if (in == m_backIn) return;
    m_backIn = in;
    v.StopAnimation(L"Offset");
    v.StopAnimation(L"Opacity");
    v.Offset(at);
    if (!slide) {
        v.Opacity(in ? 1.0f : 0.0f);
        return;
    }
    const auto& comp = gfx::CompositionHost::Get().Compositor();
    const auto ease = in ? gfx::Easing(gfx::curve::kSlideIn)
                         : gfx::Easing({0.5f, 0.0f, 0.85f, 0.35f});
    auto move = comp.CreateVector3KeyFrameAnimation();
    move.InsertKeyFrame(0.0f, in ? above : at);
    move.InsertKeyFrame(1.0f, in ? at : above, ease);
    move.Duration(Ms(in ? kBackInMs : kBackOutMs));
    v.StartAnimation(L"Offset", move);
    auto fade = comp.CreateScalarKeyFrameAnimation();
    fade.InsertKeyFrame(0.0f, in ? 0.0f : 1.0f);
    fade.InsertKeyFrame(1.0f, in ? 1.0f : 0.0f, ease);
    fade.Duration(Ms(in ? kBackInMs / 2 : kBackOutMs));
    v.StartAnimation(L"Opacity", fade);
}

// Sizes the mini window over a few frames, quick at first and soft at the end.
void PlayerWindow::AnimateWindowTo(const RECT& target) {
    GetWindowRect(m_hwnd, &m_sizeFrom);
    m_sizeTo = target;
    if (EqualRect(&m_sizeFrom, &m_sizeTo)) return;
    m_sizeStart = GetTickCount64();
    SetTimer(m_hwnd, kSizeTimer, 8, nullptr);
}

void PlayerWindow::TogglePane(NowPlayingPane::Mode mode) {
    if (!m_pane) return;
    if (m_paneShown && m_pane->CurrentMode() == mode)
        SetPaneShown(false, mode);
    else
        SetPaneShown(true, mode);
}

// Full screen, Media Center's chrome does not just fade: the top bar leaves upwards, the
// bottom bar downwards, the menu to the left, and they come back the same way, quickly out of
// rest and softly into place. The stage then regroups around what is left.
void PlayerWindow::ShowChrome(bool show) {
    if (show == m_chromeShown) return;
    m_chromeShown = show;
    if (show && SeekLive()) StartFrames(); // the head catches up on the next frame
    // Only the full screen hides the pointer with its chrome. Leaving it with the chrome away
    // (Escape, a double click) used to keep the pointer hidden over the mini window.
    const bool hideCursor = !show && m_mode == Mode::Full;
    if (hideCursor != m_cursorHidden) {
        m_cursorHidden = hideCursor;
        SetCursor(hideCursor ? nullptr : LoadCursorW(nullptr, IDC_ARROW));
    }
    auto& host = gfx::CompositionHost::Get();
    const auto& comp = host.Compositor();
    if (m_mode != Mode::Full) {
        // The mini window: the control bar and the caption buttons fade where they stand -
        // quickly back on a move, slowly away when the pointer has rested or left.
        for (const wuc::Visual& v : {wuc::Visual(m_barLayer), wuc::Visual(m_capsLayer), wuc::Visual(m_menuLayer)}) {
            v.StopAnimation(L"Offset");
            v.Offset(float3{0, 0, 0});
        }
        host.AnimateOpacity(m_barLayer, show ? 1.0f : 0.0f, show ? 180 : 600);
        host.AnimateOpacity(m_capsLayer, show ? 1.0f : 0.0f, show ? 160 : 420);
        if (!show) {
            SetPopup(false);
            ShowBubble(false);
        }
        return;
    }
    // Each layer - its controls and the shade or bar behind them together - drifts toward its
    // edge of the screen while it fades, and comes back from there fading in: the fade runs
    // ahead of the move, so a layer is mostly gone before it reaches the edge (owner,
    // 2026-11-14: the shades slid away whole instead of fading on the way).
    const auto slide = [&](const wuc::Visual& v, float dx, float dy, int delayMs) {
        const auto moveEase = show ? gfx::Easing(gfx::curve::kSlideIn)
                                    : gfx::Easing(gfx::curve::kStandard);
        const auto fadeEase = gfx::Easing(gfx::curve::kEase);
        auto move = comp.CreateVector3KeyFrameAnimation();
        move.InsertKeyFrame(1.0f, show ? float3{0, 0, 0} : float3{dx * 0.6f, dy * 0.6f, 0}, moveEase);
        move.Duration(Ms(show ? 480 : 600));
        move.DelayTime(Ms(delayMs));
        v.StartAnimation(L"Offset", move);
        auto fade = comp.CreateScalarKeyFrameAnimation();
        fade.InsertKeyFrame(1.0f, show ? 1.0f : 0.0f, fadeEase);
        fade.Duration(Ms(show ? 360 : 420));
        fade.DelayTime(Ms(delayMs));
        v.StartAnimation(L"Opacity", fade);
    };
    const float barH = static_cast<float>(m_client.cy - m_layout.bar.top) + Px(12);
    const float topH = static_cast<float>(std::max<LONG>(m_layout.top.bottom, PxI(70)));
    const float menuW = static_cast<float>(m_layout.menuShade.right) + Px(20);
    slide(m_barLayer, 0, barH, 0);
    slide(m_capsLayer, 0, -topH, 0);
    slide(m_menuLayer, -menuW, 0, show ? 60 : 0);
    if (!m_paneShown) UpdateLayout(true); // the cover and its lines take the room the menu left
}

void PlayerWindow::ArmHideTimer() {
    KillTimer(m_hwnd, kHideTimer);
    if (m_shown) SetTimer(m_hwnd, kHideTimer, kHideAfterMs, nullptr);
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
    m_splitVolume = false; // set again below for a narrow mini window
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
    const bool compact = !full && !m_paneShown && W < PxI(kMiniMinW);
    L.compact = compact;

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
        L.stop = take(38, 31, PxI(20)); // lyrics and queue are in the menu beside the cover
        L.favorite = take(38, 31, PxI(20));
        L.timeLeft = {PxI(20), cy - PxI(9), PxI(20 + 52), cy + PxI(9)};
        L.timeRight = {x - PxI(52), cy - PxI(9), x, cy + PxI(9)};
        L.seek = {L.timeLeft.right + PxI(2), cy - PxI(12), L.timeRight.left - PxI(2), cy + PxI(12)};
        // Back to the song: placed always, shown only with the lyrics or the queue (SlideBack).
        L.library = {PxI(14), PxI(8), PxI(14 + 42), PxI(8 + 42)};
        L.exit = {W - PxI(12 + 29), PxI(10), W - PxI(12), PxI(10 + 24)};
        L.restore = {L.exit.left - PxI(29), L.exit.top, L.exit.left, L.exit.bottom};
        L.minimize = {L.restore.left - PxI(29), L.exit.top, L.restore.left, L.exit.bottom};
        // Media Center's volume overlay: 57 DIP in from the right, 58 down (volume.mp4).
        L.popup = {W - PxI(57 + WmcVolume::kPanelW), PxI(58), W - PxI(57), PxI(58 + WmcVolume::kPanelH)};
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
            // No back button here: the frame's close button returns to the library.
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
            L.stop = ClusterRect(wt::kStopRect); // 25x25, the plate of the buttons beside it
        } else {
            const int orbPx = PxI(m::kOrbDiameter + 2.0f), segH = PxI(m::kSegmentHeight);
            L.play = CenteredRect(cx, cy, orbPx, orbPx);
            L.prev = {cx - PxI(m::kSegmentReach), cy - segH / 2, cx, cy - segH / 2 + segH};
            L.next = {cx, cy - segH / 2, cx + PxI(m::kSegmentReach), cy - segH / 2 + segH};
            L.stop = CenteredRect(cx + PxI(m::kStopX), cy, PxI(m::kIconButtonW), PxI(m::kIconButtonH));
        }
        if (compact) { // WMP's small window: the transport at the left, the volume at the right
            const int dx = L.bar.left + PxI(8) - std::min({L.stop.left, L.prev.left, L.play.left});
            for (RECT* r : {&L.stop, &L.prev, &L.play, &L.next}) OffsetRect(r, dx, 0);
            L.cluster.x += dx;
            m_layout.cluster = L.cluster;
        }
        // The speaker; beside it the volume slider where there is room before the transport,
        // else an arrow on the speaker's button that opens the volume flyout. With WMP's art
        // both are WMP 12's (its speaker 25x25, its slider 80x23 five DIP on); else Media
        // Center's speaker and bar.
        const bool wmcSpeaker = m_ehres && !m_wmp;
        const int muteW = PxI(wmcSpeaker ? 38.0f : m_wmp ? 25.0f : 28.0f),
                  muteH = PxI(wmcSpeaker ? 31.0f : m_wmp ? 25.0f : 26.0f);
        const int muteL = L.bar.left + PxI(wmcSpeaker ? 4.0f : 10.0f);
        const int sliderL = muteL + muteW + PxI(m_wmp ? 5.0f : 4.0f), sliderR = sliderL + PxI(m_wmp ? kSliderW : 90.0f);
        const bool slider = sliderR + PxI(14) <= std::min({L.stop.left, L.prev.left, L.play.left});
        m_splitVolume = !slider && m_wmp; // WMP 12's split button, 39x25: speaker 25, arrow 14
        if (m_splitVolume) {
            const int top = cy - PxI(12.5f);
            L.mute = {muteL, top, muteL + PxI(25), top + PxI(25)};
            L.volArrow = {L.mute.right, top, L.mute.right + PxI(14), top + PxI(25)};
        } else
            L.mute = {muteL, cy - muteH / 2, muteL + muteW + (wmcSpeaker && !slider ? PxI(16) : 0), cy - muteH / 2 + muteH};
        if (slider && m_wmp)
            L.slider = {sliderL, L.mute.top, sliderR, L.mute.top + PxI(kSliderH)};
        else if (slider)
            L.slider = {sliderL, cy - PxI(9), sliderR, cy + PxI(9)};
        else if (!m_ehres && !m_splitVolume)
            L.volArrow = {L.mute.right + PxI(1), cy - PxI(8), L.mute.right + PxI(17), cy + PxI(8)};
        if (compact) {
            const int dv = L.bar.right - PxI(8) - std::max(L.mute.right, L.volArrow.right);
            OffsetRect(&L.mute, dv, 0);
            if (!Empty(L.volArrow)) OffsetRect(&L.volArrow, dv, 0);
        }
        // Shuffle and repeat before stop (WMP 12's 25x25 buttons, 6 apart, 8 before stop),
        // where they leave room after the volume.
        {
            const int bs = PxI(25), top = cy - PxI(12.5f);
            L.repeat = {L.stop.left - PxI(8) - bs, top, L.stop.left - PxI(8), top + bs};
            L.shuffle = {L.repeat.left - PxI(6) - bs, top, L.repeat.left - PxI(6), top + bs};
            const LONG volumeRight = std::max({L.mute.right, L.volArrow.right, L.slider.right});
            if (L.shuffle.left < volumeRight + PxI(10)) L.shuffle = L.repeat = {};
        }
        if (m_ehres) { // three buttons on WMP's plate, 29x25, 6 apart
            const int bw = PxI(29), bh = PxI(25), gap = PxI(6);
            L.list = {L.bar.right - PxI(10) - bw, cy - bh / 2, L.bar.right - PxI(10), cy - bh / 2 + bh};
            L.lyrics = {L.list.left - gap - bw, L.list.top, L.list.left - gap, L.list.bottom};
            L.favorite = {L.lyrics.left - gap - bw, L.list.top, L.lyrics.left - gap, L.list.bottom};
        } else {
            L.list = {L.bar.right - PxI(10 + 30), cy - PxI(13), L.bar.right - PxI(10), cy + PxI(13)};
            L.lyrics = {L.list.left - PxI(34), L.list.top, L.list.left - PxI(4), L.list.bottom};
            L.favorite = {L.lyrics.left - PxI(4 + 30), L.list.top, L.lyrics.left - PxI(4), L.list.bottom};
        }
        // A narrow window has no room for the heart beside the transport: it leaves first.
        if (L.favorite.left < std::max({L.next.right, L.play.right, L.stop.right}) + PxI(6)) L.favorite = {};
        if (compact) L.favorite = L.lyrics = L.list = L.shuffle = L.repeat = {};
        if (full)
            L.popup = {W - PxI(57 + WmcVolume::kPanelW), PxI(58), W - PxI(57), PxI(58 + WmcVolume::kPanelH)};
        else { // WMP's flyout over the button, its right edge on the arrow's
            const int fw = PxI(kFlyoutW), fh = PxI(kFlyoutH), mid = (L.mute.top + L.mute.bottom) / 2;
            const LONG right = m_splitVolume ? L.volArrow.right : L.mute.right;
            const int left = std::max<int>(right - fw, L.client.left + PxI(2));
            L.popup = {left, mid - fh / 2, left + fw, mid - fh / 2 + fh};
        }
    }

    // The compact window's cover: the square client (WM_SIZING keeps it square), less a margin
    // that opens first as the window grows from its smallest.
    if (compact) {
        const int S = std::min<int>(CW, C.bottom - C.top);
        const int fullMargin = PxI(kCompactMargin);
        const int margin = std::clamp((S - PxI(kCompactSide)) / 2, 0, fullMargin);
        const int side = S - 2 * margin;
        const int left = C.left + (CW - side) / 2, top = C.top + (C.bottom - C.top - side) / 2;
        L.cover = {left, top, left + side, top + side};
        L.coverEdge = static_cast<float>(margin) / std::max(1, fullMargin);
        L.top = {C.left, C.top, C.right, C.top + PxI(kCompactTitle * 2.6f)};
    }

    // The stage (cover and text) and the lyrics / queue pane.
    // In the mini window the control bar comes and goes over the picture (it hides when the
    // pointer rests): the stage, the lines and the pane reach the window's foot under it.
    const int stageBottom = full ? L.bar.top : C.bottom;
    const int stageTop = C.top, stageH = stageBottom - C.top;
    int menuRight = 0;
    const bool idle = full && !m_chromeShown && !m_paneShown; // the chrome has left: regroup
    const bool lyricLine =
        m_lyrics && m_lyrics->synced && !m_lyrics->lines.empty() && !m_paneShown && settings::LyricLineShown();
    if (full) {
        // Media Center's menu at the left, centred on the cover's height. misc.mp4 (2.37 px per
        // unit of its 768-unit high screen): buttons the 251x51 bitmap, 55 apart, 41 in from
        // the left; its shade 410 x (menu + 130), from the screen's edge.
        const float f = std::clamp(H / m_scale / 768.0f, 1.0f, 1.6f);
        const int iw = PxI(251 * f), ih = PxI(51 * f), pitch = PxI(55 * f), x = PxI(41 * f);
        const int block = pitch * 3 + ih, top = (H - block) / 2 - static_cast<int>(H * 0.03f);
        for (int i = 0; i < 4; ++i) L.menu[i] = {x, top + pitch * i, x + iw, top + pitch * i + ih};
        menuRight = idle ? 0 : x + iw + PxI(20);
        // Taller than misc.mp4's 65 units beyond the menu: the shade's falloff needs the room.
        L.menuShade = {0, top - PxI(190 * f), PxI(470 * f), top + block + PxI(190 * f)};
        if (L.lyrics.right > L.lyrics.left) L.lyrics = L.list = {}; // the bar's own pair gives way
    }
    if (compact) {
        // Placed above; the title is over the cover (PaintCompactTitle).
    } else if (full) {
        if (m_paneShown) {
            const int colL = menuRight, colR = static_cast<int>(W * 0.50f), colW = colR - colL;
            const int side = static_cast<int>(std::min(H * 0.44f, colW * 0.74f));
            L.textScale = std::clamp((colR - colL - PxI(16)) / m_scale / 340.0f, 1.0f, 1.7f);
            const int textH = PxI(UnderStack(L.textScale, false).block);
            const int top = std::max(PxI(48), (H - side - textH) / 2 - PxI(24));
            L.cover = {colL + (colW - side) / 2, top, colL + (colW - side) / 2 + side, top + side};
            L.info = {colL + PxI(8), L.cover.bottom + PxI(22), colR - PxI(8), L.cover.bottom + PxI(22) + textH};
            L.infoStyle = 0;
            L.panel = {static_cast<int>(W * 0.52f), PxI(wmc ? kWmcTop + 4 : 64.0f), W - static_cast<int>(W * 0.04f),
                       L.bar.top - PxI(8)};
            L.pane = L.panel;
        } else {
            // WMC: the cover with the song's lines beside it, centred as a group.
            const int side = static_cast<int>(H * (idle ? 0.47f : 0.40f)); // larger once it stands alone
            const int gap = static_cast<int>(side * 0.085f);
            const int textW = static_cast<int>(std::min(W * 0.34f, side * 1.7f));
            // With the menu the cover stands 0.10 of the height right of its buttons (misc.mp4);
            // without it the group is centred.
            const int left = idle ? std::max(0, (W - side - gap - textW) / 2)
                                  : static_cast<int>(L.menu[0].right) + static_cast<int>(H * 0.10f);
            const int top = (H - side) / 2 - static_cast<int>(H * 0.03f);
            L.cover = {left, top, left + side, top + side};
            L.info = {L.cover.right + gap, top, L.cover.right + gap + textW, top + side};
            L.infoStyle = 1;
            if (lyricLine) {
                // The sung line is the last of the song's lines beside the cover (PaintInfo
                // centres them all as one block); it may run on as far as the blue behind
                // them reaches (PaintShade), no further onto the bright wall.
                const float pitch = side * 0.172f;
                const int lines = 1 + (m_artist.empty() ? 0 : 1) + (m_album.empty() ? 0 : 1) + (m_track.is_valid() ? 1 : 0);
                const float y = (side - pitch * (lines + 1)) / 2 + pitch * lines;
                L.lyric = {L.info.left, top + static_cast<int>(y),
                           std::min<LONG>(L.info.right + static_cast<LONG>(side * 0.3f), W - PxI(48)),
                           top + static_cast<int>(y + pitch)};
            }
        }
    } else if (m_paneShown) {
        L.panel = {C.left, C.top, C.right, stageBottom};
        L.pane = L.panel;
    } else if (stageH < PxI(kStripBelow) || CW >= stageH * kStripAspect) {
        // The strip: the cover as high as the stage allows (and at most 45 % of the width), the
        // lines beside it scaled with the cover (a title about a seventh of its side), within
        // the width they have, and the block never higher than the stage.
        // The margins and the gap between cover and lines grow with the cover (owner,
        // 2026-11-13: fixed, they looked cramped in a large window): about an eighth of its
        // side, as at the smallest strip, never less than there.
        const int side = std::max(PxI(40), std::min(stageH - PxI(24), static_cast<int>(CW * 0.45f)));
        const int top = stageTop + (stageH - side) / 2;
        const int margin = std::max(PxI(12), static_cast<int>(side * 0.07f));
        const int gap = std::max(PxI(14), static_cast<int>(side * 0.13f));
        L.cover = {C.left + margin, top, C.left + margin + side, top + side};
        L.info = {L.cover.right + gap, L.cover.top, C.right - std::max(PxI(16), margin), L.cover.bottom};
        L.infoStyle = 2;
        float t = std::clamp(std::min(side / m_scale / 115.0f, (L.info.right - L.info.left) / m_scale / 170.0f), 0.8f,
                             2.0f);
        const float room = stageH / m_scale - 12;
        for (int i = 0; i < 3; ++i) { // the gaps grow faster than t: settle in a few steps
            const float block = StripStack(t, lyricLine).block;
            if (block <= room) break;
            t *= room / block;
        }
        L.textScale = t;
        if (lyricLine) { // the sung line under the others, its rect 2 DIP round the type
            const LineStack s = StripStack(t, true);
            const int blockTop = (L.cover.top + L.cover.bottom) / 2 - PxI(s.block / 2);
            L.lyric = {L.info.left, blockTop + PxI(s.top[3] - 2 * t), L.info.right,
                       blockTop + PxI(s.top[3] + kStripLyric * t * kLineHeight + 2 * t)};
        }
    } else {
        // Three lines under the cover, and the sung line as a fourth in their style (no room is
        // kept for it without synced lyrics). They grow with a wider window and shrink with a
        // lower one, so that cover and lines keep about the proportion they have at the
        // window's first size (a stage 500 DIP high) instead of the lines squeezing the cover.
        const float tkWide = std::clamp((CW / m_scale - 28) / 340.0f, 1.0f, 1.7f);
        const float tk = std::clamp(std::min(tkWide, stageH / m_scale / 500.0f), 0.62f, 1.7f);
        L.textScale = tk;
        const LineStack s = UnderStack(tk, lyricLine);
        const int textH = PxI(s.block);
        // The cover leaves a good margin of wall on every side.
        const int side = std::max(PxI(60), std::min(static_cast<int>(CW * 0.64f), stageH - textH - PxI(52)));
        const int top = stageTop + std::max(PxI(12), (stageH - textH - side) / 2);
        L.cover = {C.left + (CW - side) / 2, top, C.left + (CW - side) / 2 + side, top + side};
        L.info = {C.left + PxI(14), L.cover.bottom + PxI(12), C.right - PxI(14), L.cover.bottom + PxI(12) + textH};
        L.infoStyle = 0;
        if (lyricLine)
            L.lyric = {L.info.left, L.info.top + PxI(s.top[4] - 2 * tk), L.info.right,
                       L.info.top + PxI(s.top[4] + kStackLine * tk * kLineHeight + 2 * tk)};
    }
    const bool infoChanged = !EqualRect(&L.info, &m_layout.info) || L.infoStyle != m_layout.infoStyle;
    const bool lyricMoved = Empty(m_layout.lyric) != Empty(L.lyric) || L.infoStyle != m_layout.infoStyle;
    // Full screen, the menu coming or going only resizes the cover and moves the group: the
    // song's lines and the sung line then grow or shrink with the cover on its curve instead
    // of fading out and in (owner, 2026-11-13).
    const RECT oldCover = m_layout.cover, oldInfo = m_layout.info, oldLyric = m_layout.lyric;
    const bool morph = animate && full && L.infoStyle == 1 && m_layout.infoStyle == 1 && !Empty(oldCover) &&
                       !Empty(L.cover) && !Empty(oldInfo);
    m_layout = L;

    // Apply.
    if (!full && m_framePainted != m_scale) PaintFrame(); // before the frame's layout, which uses its scale
    m_frame->Layout({W, H}, L.clientEdge.left, !full);
    m_contentClip.LeftInset(static_cast<float>(C.left));
    m_contentClip.TopInset(static_cast<float>(C.top));
    m_contentClip.RightInset(static_cast<float>(W - C.right));
    m_contentClip.BottomInset(static_cast<float>(H - C.bottom));
    if (!full) Place(m_clientEdge, L.clientEdge);
    if (m_pane) m_pane->SetEmbedded(true, full); // full screen shows cover, title and modes itself
    PlaceWall();
    m_light->SetSize({W, H});
    m_veil->SetSize({W, H});
    if (m_pane && m_paneShown) m_pane->SetBounds(L.pane, m_scale);
    PaintPaneCard();

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
        case kMenuLyrics: r = L.menu[0]; break;
        case kMenuList: r = L.menu[1]; break;
        case kMenuShuffle: r = L.menu[2]; break;
        case kMenuRepeat: r = L.menu[3]; break;
        case kFavorite: r = L.favorite; break;
        case kShuffle: r = L.shuffle; break;
        case kRepeat: r = L.repeat; break;
        case kCapMin: r = L.capMin; break;
        case kCapMax: r = L.capMax; break;
        case kCapClose: r = L.capClose; break;
        }
        w->SetBounds(r, m_scale);
    }
    SlideBack(animate);
    if (m_splitVolume) {
        const RECT pr{L.mute.left, L.mute.top, L.volArrow.right, L.mute.bottom};
        Place(m_splitPlate, pr);
        gfx::CompositionHost::Get().Repaint(m_splitPlate, m_splitPlateBacking, pr.right - pr.left, pr.bottom - pr.top,
                                            m_scale, [&](ID2D1DeviceContext* dc) {
                                                const float k = (pr.bottom - pr.top) / m_scale / 25.0f;
                                                SplitPlate(dc, 4630, false, 0, k);
                                                SplitPlate(dc, 4630, true, 25 * k, k);
                                            });
    } else if (m_splitPlateShown) {
        m_splitPlateShown = false;
        m_splitPlate.StopAnimation(L"Opacity");
        m_splitPlate.Opacity(0.0f);
    }
    UpdateSplitPlate();
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
                shadow.Opacity(kCoverShadow * L.coverEdge);
            }
        }
        for (const auto& f : m_coverFrame) f.Opacity(L.coverEdge);
    }
    const float morphFrom =
        morph ? static_cast<float>(oldCover.right - oldCover.left) / std::max<LONG>(1, L.cover.right - L.cover.left) : 1.0f;
    m_info.IsVisible(!Empty(L.info)); // the compact window has none: its title is over the cover
    if (L.compact) PaintInfo();
    if (!Empty(L.info)) {
        Place(m_info, L.info);
        if (morph) {
            Morph(m_info, oldInfo, morphFrom);
        } else if (infoChanged && animate) {
            m_info.Opacity(0.0f);
            gfx::FadeIn(m_info, 320, 180); // with the song's other lines
        }
        PaintInfo();
    }
    for (size_t i = 0; i < 4; ++i) Place(m_menuGlow[i], L.menu[i]);
    Place(m_menuShade, L.menuShade);
    PaintMenuShade();
    // The sung line does not travel with the cover when the pane opens or closes: it goes at
    // once, and comes back in its new place fading in with the song's other lines.
    Place(m_lyric, L.lyric);
    m_lyric.IsVisible(!Empty(L.lyric));
    if (morph && !lyricMoved && !Empty(L.lyric) && !Empty(oldLyric)) {
        Morph(m_lyric, oldLyric, morphFrom);
    } else if (lyricMoved && animate && !Empty(L.lyric)) {
        gfx::FadeIn(m_lyric, 320, 180);
    }
    m_lyricLine = -2;
    PaintLyric();
    // The sung line's or the seek bar's room may only have come now.
    if ((LyricLineLive() && !Empty(L.lyric)) || SeekLive()) StartFrames();
    Place(m_barBack, L.bar);
    Place(m_topBack, L.top);
    Place(m_times, RECT{L.bar.left, L.timeLeft.top, L.bar.right, L.timeLeft.bottom});
    m_volumePanel->SetPosition({L.popup.left, L.popup.top}, m_scale);
    if (Empty(L.slider)) {
        ShowBubble(false);
    } else if (m_popupOpen && !full) {
        SetPopup(false); // room for the slider: the panel's arrow is gone
    }
    Place(m_seek, L.seek);
    Place(m_slider, L.slider);
    m_slider.IsVisible(!Empty(L.slider));
    PaintSlider();
    PaintBars();
    PaintSeek();
    PaintTimes();
    if (animate && m_shade.Brush()) {
        // The blue changes shape with the layout: the old one fades out as the new one comes.
        auto& host = gfx::CompositionHost::Get();
        m_shadeOld.Brush(m_shade.Brush());
        m_shadeOld.Opacity(1.0f);
        host.AnimateOpacity(m_shadeOld, 0.0f, 620);
        m_shade.Opacity(0.0f);
        host.AnimateOpacity(m_shade, 1.0f, 480);
    }
    PaintShade();
    if (m_popupOpen) PaintPopup();
}

// ---- Playback ------------------------------------------------------------------------------

void PlayerWindow::ReadTrack() {
    auto pc = playback_control::get();
    m_playing = pc->is_playing();
    m_paused = pc->is_paused();
    // While playback switches songs the host keeps the previous song, or names the next one as
    // soon as it is being opened: there is no empty moment between two songs.
    const metadb_handle_ptr now = m_host.PlayerTrack();
    const bool has = now.is_valid();
    metadb_handle_ptr decoding;
    const bool loaded = pc->get_now_playing(decoding) && decoding == now;
    const bool changed = has ? (!m_track.is_valid() || now != m_track) : m_track.is_valid();
    std::wstring title, artist, album;
    if (has) {
        m_host.PlayerTrackText(now, title, artist, album);
        if (loaded) {
            m_position = pc->playback_get_position();
            m_length = pc->playback_get_length();
        } else if (changed) {
            m_position = 0;
            m_length = 0;
        }
        // A paused track that has not decoded yet reports no length; the library knows it.
        if (m_length <= 0) m_length = now->get_length();
    } else {
        m_position = m_length = 0;
    }
    bool shuffle = false;
    int repeat = 0;
    m_host.PlayerMode(shuffle, repeat);
    if (shuffle != m_shuffle || repeat != m_repeat) {
        m_shuffle = shuffle;
        m_repeat = repeat;
        for (int id : {kMenuShuffle, kMenuRepeat, kShuffle, kRepeat})
            if (auto* w = WidgetById(id)) w->Invalidate();
    }
    const bool favorite = has && (loaded ? m_host.PlayerFavorite() : media::IsFavorite(now));
    if (favorite != m_favorite) {
        m_favorite = favorite;
        if (auto* w = WidgetById(kFavorite)) w->Invalidate();
    }
    // The library's lyrics belong to the song that played; a song still opening has none yet.
    auto lyrics = !has ? nullptr : loaded ? m_host.PlayerLyrics() : changed ? nullptr : m_lyrics;
    const auto synced = [](const auto& l) { return l && l->synced && !l->lines.empty(); };
    const bool hadLine = synced(m_lyrics), hasLine = synced(lyrics);
    const bool lyricsChanged = lyrics != m_lyrics;
    if (lyricsChanged) {
        m_lyrics = std::move(lyrics);
        m_lyricLine = -2;
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
    // Laid out only now that the song's lines are the new song's: room for the sung line comes
    // and goes with synced lyrics, and beside the cover its place follows the number of lines.
    if ((hadLine != hasLine || (hasLine && (changed || textChanged))) && m_shown && m_client.cx > 0)
        UpdateLayout(false);
    else if (changed || textChanged)
        PaintInfo();
    if (lyricsChanged) PaintLyric();
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
    if (m_length <= 0 && m_track.is_valid()) m_length = m_track->get_length();
    if (m_seeking || IsIconic(m_hwnd)) return;
    PaintSeek();
    PaintTimes();
    PaintInfo(); // its time line
    PaintLyric();
    if (SeekLive()) StartFrames(); // after a seek or a song change
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
            if (m_frame) m_frame->SetActive(active);
            for (auto& w : m_widgets)
                if (w->Id() >= kCapMin && w->Id() <= kCapClose) w->SetInactive(!active); // caption only
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

    case WM_SIZING: {
        // With the lyrics or the queue open the mini window keeps room for them (the lyrics:
        // their tallest line whole); dragged on past that, the pane closes and the window
        // follows the pointer again.
        if (m_mode != Mode::Mini) break;
        auto* r = reinterpret_cast<RECT*>(lp);
        const bool topEdge = wp == WMSZ_TOP || wp == WMSZ_TOPLEFT || wp == WMSZ_TOPRIGHT;
        if (m_paneShown && m_pane) {
            const int panelH = m_layout.panel.bottom - m_layout.panel.top;
            const int minH = (m_client.cy - panelH) + PxI(std::max(kPaneMinH, m_pane->MinHeight()));
            const int h = r->bottom - r->top;
            if (h >= minH) break;
            if (h < minH - PxI(kPaneCloseDrag)) {
                m_miniCoverHeight = 0; // stay where the pointer took it
                SetPaneShown(false, m_pane->CurrentMode());
                break;
            }
            if (topEdge)
                r->top = r->bottom - minH;
            else
                r->bottom = r->top + minH;
            return TRUE;
        }
        // Narrower than the full transport needs the window is compact and square, the cover
        // filling it: the edge being dragged sets the side, the opposite edges stay. Leaving
        // the compact size takes a wider window, so a narrow one never grows tall.
        const int border = PxI(m::kResizeBorder), capH = PxI(m::kCaptionHeight);
        const int w = r->right - r->left, h = r->bottom - r->top;
        if (w >= PxI(kMiniMinW)) break;
        const bool byHeight = wp == WMSZ_TOP || wp == WMSZ_BOTTOM;
        const bool corner = wp == WMSZ_TOPLEFT || wp == WMSZ_TOPRIGHT || wp == WMSZ_BOTTOMLEFT ||
                            wp == WMSZ_BOTTOMRIGHT;
        int side = byHeight ? h - capH - border : w - 2 * border;
        if (corner) side = std::max(side, h - capH - border);
        side = std::clamp(side, PxI(kCompactSide), PxI(kMiniMinW) - 2 * border - 1);
        const int nw = side + 2 * border, nh = side + capH + border;
        const bool leftEdge = wp == WMSZ_LEFT || wp == WMSZ_TOPLEFT || wp == WMSZ_BOTTOMLEFT;
        if (leftEdge)
            r->left = r->right - nw;
        else
            r->right = r->left + nw;
        if (topEdge)
            r->top = r->bottom - nh;
        else
            r->bottom = r->top + nh;
        return TRUE;
    }

    case WM_GETMINMAXINFO: {
        // The compact square at its smallest; a wider window may be as low as the strip allows.
        auto* info = reinterpret_cast<MINMAXINFO*>(lp);
        info->ptMinTrackSize = {PxI(kCompactSide + 2 * m::kResizeBorder),
                                PxI(kMiniMinClientH + m::kCaptionHeight + m::kResizeBorder)};
        return 0;
    }

    case WM_SIZE:
        if (wp != SIZE_MINIMIZED && m_shown) UpdateLayout(false);
        return 0;

    case WM_MOVE:
        if (m_frame && m_mode == Mode::Mini) m_frame->Glass().UpdateReflectionAnchor(hwnd);
        return 0;

    case WM_WINDOWPOSCHANGING: {
        const auto* pos = reinterpret_cast<const WINDOWPOS*>(lp);
        if (m_frame && m_mode == Mode::Mini && !(pos->flags & SWP_NOMOVE) && !IsIconic(hwnd)) {
            RECT wr{};
            GetWindowRect(hwnd, &wr);
            const LONG w = (pos->flags & SWP_NOSIZE) ? wr.right - wr.left : pos->cx;
            const LONG h = (pos->flags & SWP_NOSIZE) ? wr.bottom - wr.top : pos->cy;
            m_frame->Glass().UpdateReflectionAnchor(hwnd, RECT{pos->x, pos->y, pos->x + w, pos->y + h});
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
        if (m_mode == Mode::Mini && !IsIconic(hwnd)) RememberMiniRect();
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT:
        ValidateRect(hwnd, nullptr);
        return 0;

    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT && m_cursorHidden && m_mode == Mode::Full) {
            SetCursor(nullptr);
            return TRUE;
        }
        break;

    case kFrameMessage:
        if (m_frameClock) {
            m_frameClock->Ack();
            bool more = m_shown && !IsIconic(hwnd) && m_pane && m_pane->Tick();
            more |= TickLyric();
            more |= TickSeek();
            if (!more) m_frameClock->Stop();
        }
        return 0;

    case WM_TIMER:
        if (wp == kHideTimer) {
            KillTimer(hwnd, kHideTimer);
            POINT pt{};
            GetCursorPos(&pt);
            ScreenToClient(hwnd, &pt);
            // The controls stay while the pointer is on them, something is held, or the volume
            // flyout is open; full screen also over the pane (it scrolls under the pointer).
            const bool onControls =
                PtInRect(&m_layout.bar, pt) || PtInRect(&m_layout.top, pt) || WidgetAt(pt) != nullptr;
            // Nor while a menu is open: hidden then, the pointer stayed hidden after the menu,
            // whatever the mouse did (owner, 2026-11-14).
            GUITHREADINFO gui{sizeof(gui)};
            const bool menu = GetGUIThreadInfo(GetCurrentThreadId(), &gui) &&
                              (gui.flags & (GUI_INMENUMODE | GUI_POPUPMENUMODE)) != 0;
            const bool busy = m_capture != Capture::None || m_popupOpen || onControls || menu ||
                              (m_mode == Mode::Full && m_pane && m_paneShown && m_pane->HitTest(pt));
            if (!busy)
                ShowChrome(false);
            else
                ArmHideTimer();
        } else if (wp == kOverlayTimer) {
            KillTimer(hwnd, kOverlayTimer);
            if (m_mode == Mode::Full)
                SetPopup(false);
            else if (!m_bubbleHover && m_capture != Capture::Volume)
                ShowBubble(false);
        } else if (wp == kBubbleTimer) {
            KillTimer(hwnd, kBubbleTimer); // the pointer has rested on a volume slider
            if (m_shown && m_bubbleHover) ShowBubble(true);
        } else if (wp == kRepeatTimer) {
            // A volume key held down keeps stepping, faster after the first pause.
            const int id = m_capture == Capture::Widget && m_pressed ? m_pressed->Id() : 0;
            if (id == kVolDown || id == kVolUp) {
                StepVolume(id == kVolUp ? kVolumeStep : -kVolumeStep);
                SetTimer(hwnd, kRepeatTimer, 45, nullptr);
            } else {
                KillTimer(hwnd, kRepeatTimer);
            }
        } else if (wp == kSizeTimer) {
            const float t = std::clamp(static_cast<float>(GetTickCount64() - m_sizeStart) / kSizeMs, 0.0f, 1.0f);
            const float e = 1.0f - std::pow(1.0f - t, 4.0f);
            const auto mix = [e](LONG a, LONG b) { return static_cast<int>(std::lround(a + (b - a) * e)); };
            if (t >= 1.0f) KillTimer(hwnd, kSizeTimer);
            const int left = mix(m_sizeFrom.left, m_sizeTo.left), top = mix(m_sizeFrom.top, m_sizeTo.top);
            SetWindowPos(hwnd, nullptr, left, top, mix(m_sizeFrom.right, m_sizeTo.right) - left,
                         mix(m_sizeFrom.bottom, m_sizeTo.bottom) - top, SWP_NOZORDER | SWP_NOACTIVATE);
            if (t >= 1.0f && m_mode == Mode::Mini) RememberMiniRect();
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
            PaintInfo(); // its time line follows the drag
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
        if (seekHover != m_seekHover || seekHover) {
            m_seekHover = seekHover;
            m_seekPointerX = pt.x; // WMP's thumb lights while the pointer is on it
            UpdateSeekThumb();
        }
        const bool flyoutHot = m_popupOpen && m_mode == Mode::Mini && PtInRect(&m_layout.popup, pt) != FALSE;
        if (flyoutHot != m_flyoutHot) {
            m_flyoutHot = flyoutHot; // WMP's thumb lights under the pointer
            UpdateFlyoutBall();
        }
        const bool sliderHover = !w && PtInRect(&m_layout.slider, pt) != FALSE;
        if (sliderHover != m_sliderHover) {
            m_sliderHover = sliderHover;
            UpdateSliderThumb();
        }
        // The level shows over a slider once the pointer has rested on it a moment (a
        // tooltip's pause), and goes when it leaves.
        const bool bubbleHover = m_sliderHover || m_flyoutHot;
        if (bubbleHover != m_bubbleHover) {
            m_bubbleHover = bubbleHover;
            KillTimer(hwnd, kOverlayTimer);
            KillTimer(hwnd, kBubbleTimer);
            if (bubbleHover)
                SetTimer(hwnd, kBubbleTimer, static_cast<UINT>(settings::VolumeTipDelayMs()), nullptr);
            else
                ShowBubble(false);
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
            UpdateSeekThumb();
        }
        if (m_sliderHover) {
            m_sliderHover = false;
            UpdateSliderThumb();
        }
        if (m_flyoutHot) {
            m_flyoutHot = false;
            UpdateFlyoutBall();
        }
        if (m_bubbleHover) {
            m_bubbleHover = false;
            KillTimer(hwnd, kBubbleTimer);
            if (m_capture != Capture::Volume) ShowBubble(false);
        }
        m_lastMouse = {-1, -1};
        if (m_mode == Mode::Mini) ShowChrome(false);
        return 0;

    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK: {
        const POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        SetFocus(hwnd);
        if (m_popupOpen && m_mode == Mode::Mini) {
            if (PtInRect(&m_layout.popup, pt)) { // the whole flyout takes the press
                m_capture = Capture::Volume;
                SetCapture(hwnd);
                SetVolumeFromX(pt.x);
                return 0;
            }
            const Widget* under = WidgetAt(pt);
            if (!under || (under->Id() != kVolArrow && under->Id() != kMute)) SetPopup(false);
        }
        if (PtInRect(&m_layout.slider, pt) && !WidgetAt(pt)) {
            m_capture = Capture::Volume;
            SetCapture(hwnd);
            KillTimer(hwnd, kOverlayTimer);
            SetVolumeFromX(pt.x);
            return 0;
        }
        if (Widget* w = WidgetAt(pt)) {
            if (!w->IsEnabled()) return 0;
            m_pressed = w;
            w->SetPressed(true);
            m_capture = Capture::Widget;
            SetCapture(hwnd);
            if (w->Id() == kVolDown || w->Id() == kVolUp) {
                StepVolume(w->Id() == kVolUp ? kVolumeStep : -kVolumeStep);
                SetTimer(hwnd, kRepeatTimer, 380, nullptr);
            }
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
        if (PtInRect(&m_timeHit, pt)) { // the time counts down instead, or up again
            settings::SetPlayerTimeRemaining(!settings::PlayerTimeRemaining());
            PaintInfo();
            return 0;
        }
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
        // Taken before the capture goes: ReleaseCapture sends WM_CAPTURECHANGED at once, and
        // that handler forgets the pressed widget.
        Widget* const pressed = std::exchange(m_pressed, nullptr);
        KillTimer(hwnd, kRepeatTimer);
        if (capture != Capture::None) ReleaseCapture();
        switch (capture) {
        case Capture::Widget: {
            Widget* w = pressed;
            if (w) {
                w->SetPressed(false);
                m_clickPoint = pt;
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
                for (int id : {kLyrics, kList, kMenuLyrics, kMenuList}) // the pane may have changed sides
                    if (auto* w = WidgetById(id)) w->Invalidate();
            }
            break;
        case Capture::Volume:
            // Let go off the slider, the bubble goes; on it, it stays while the pointer does.
            m_sliderHover = PtInRect(&m_layout.slider, pt) != FALSE;
            m_flyoutHot = m_popupOpen && m_mode == Mode::Mini && PtInRect(&m_layout.popup, pt) != FALSE;
            m_bubbleHover = m_sliderHover || m_flyoutHot;
            if (!m_bubbleHover) ShowBubble(false);
            PaintSlider();
            PaintPopup(); // the thumb lets go
            break;
        case Capture::None:
            break;
        }
        UpdateSplitPlate();
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
        if (std::exchange(m_capture, Capture::None) == Capture::Volume) {
            UpdateSliderThumb(); // the thumb lets go
            UpdateFlyoutBall();
        }
        UpdateSplitPlate();
        return 0;

    case WM_MOUSEWHEEL: {
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ScreenToClient(hwnd, &pt);
        const int delta = GET_WHEEL_DELTA_WPARAM(wp);
        if (m_pane && m_paneShown && m_pane->HitTest(pt) && !PtInRect(&m_layout.bar, pt)) {
            m_pane->OnWheel(delta);
            return 0;
        }
        StepVolume(delta > 0 ? 0.02f : -0.02f); // elsewhere the wheel is the volume
        return 0;
    }

    case WM_RBUTTONUP: {
        const POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        POINT screen = pt;
        ClientToScreen(hwnd, &screen);
        if (m_pane && m_paneShown && m_pane->HitTest(pt)) {
            m_pane->OnRButtonUp(pt, screen);
        } else if (!PtInRect(&m_timeHit, pt) && (PtInRect(&m_layout.cover, pt) || PtInRect(&m_songHit, pt))) {
            m_host.ShowPlayerSongMenu(screen); // cover, title, artist, album
        } else if (PtInRect(&m_layout.lyric, pt) || PtInRect(&m_layout.info, pt) || PtInRect(&m_timeHit, pt)) {
            // The library window's lyrics menu, on the sung line, the time and the rest of the
            // song's lines (so a hidden line can be shown again).
            m_host.ShowPlayerLyricsMenu(screen);
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
            media::transport::PlayPause();
            return 0;
        case VK_LEFT:
        case VK_RIGHT:
            if (m_length > 0)
                playback_control::get()->playback_seek(
                    std::clamp(m_position + (wp == VK_RIGHT ? 5.0 : -5.0), 0.0, m_length));
            return 0;
        case VK_UP:
        case VK_DOWN:
            StepVolume(wp == VK_UP ? 0.02f : -0.02f);
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
