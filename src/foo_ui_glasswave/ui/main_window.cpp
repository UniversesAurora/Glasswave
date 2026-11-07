#include "pch.h"
#include "ui/main_window.h"
#include "aero/aero_metrics.h"
#include "aero/aero_painter.h"
#include "gfx/composition_host.h"
#include "ui/main_menu.h"

namespace glasswave::ui {

namespace m = aero::metrics;
namespace paint = aero::paint;
namespace wuc = winrt::Windows::UI::Composition;
using paint::Layer;
using paint::TransportGlyph;
using winrt::com_ptr;
using winrt::Windows::Foundation::Numerics::float2;
using winrt::Windows::Foundation::Numerics::float3;

namespace {

constexpr wchar_t kClassName[] = L"Glasswave.MainWindow";
constexpr wchar_t kAppName[] = L"Glasswave";

enum WidgetId {
    kCapMin = 1, kCapMax, kCapClose,
    kShuffle, kRepeat, kStop, kPrev, kNext, kPlay, kMute, kVolume,
};
bool IsCaptionWidget(int id) { return id >= kCapMin && id <= kCapClose; }

// {0C9E7B42-1F3A-4D5E-8B6C-7A2F9E1D4C83}
cfg_var_modern::cfg_struct_t<WINDOWPLACEMENT> cfg_placement(
    {0x0c9e7b42, 0x1f3a, 0x4d5e, {0x8b, 0x6c, 0x7a, 0x2f, 0x9e, 0x1d, 0x4c, 0x83}});

std::wstring Wide(const char* utf8) { return std::wstring(pfc::stringcvt::string_wide_from_utf8(utf8)); }

D2D1_COLOR_F Hex(uint32_t rgb, float a = 1.0f) {
    return D2D1::ColorF(((rgb >> 16) & 0xFF) / 255.0f, ((rgb >> 8) & 0xFF) / 255.0f, (rgb & 0xFF) / 255.0f, a);
}

com_ptr<IDWriteTextLayout> MakeText(const std::wstring& text, const wchar_t* family, float size,
                                    DWRITE_FONT_WEIGHT weight, float maxW, float maxH) {
    auto* dw = gfx::CompositionHost::Get().DWrite();
    com_ptr<IDWriteTextFormat> fmt;
    winrt::check_hresult(dw->CreateTextFormat(family, nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
                                              DWRITE_FONT_STRETCH_NORMAL, size, L"en-us", fmt.put()));
    fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
    com_ptr<IDWriteInlineObject> ellipsis;
    if (SUCCEEDED(dw->CreateEllipsisTrimmingSign(fmt.get(), ellipsis.put())))
        fmt->SetTrimming(&trimming, ellipsis.get());
    com_ptr<IDWriteTextLayout> layout;
    winrt::check_hresult(
        dw->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), fmt.get(), maxW, maxH, layout.put()));
    return layout;
}

wuc::CompositionNineGridBrush NineGrid(int wPx, int hPx, float scale, float insetPx, bool hollow,
                                       const std::function<void(ID2D1DeviceContext*)>& painter) {
    auto& host = gfx::CompositionHost::Get();
    auto surface = host.CreateSurface(wPx, hPx);
    host.Draw(surface, scale, painter);
    auto source = host.Compositor().CreateSurfaceBrush(surface);
    source.Stretch(wuc::CompositionStretch::Fill);
    auto brush = host.Compositor().CreateNineGridBrush();
    brush.Source(source);
    brush.SetInsets(insetPx);
    brush.IsCenterHollow(hollow);
    return brush;
}

// A horizontally stretchable strip: painted once at a few pixels wide, stretched to any width.
wuc::CompositionSurfaceBrush Strip(int hPx, float scale, const std::function<void(ID2D1DeviceContext*, float)>& painter) {
    auto& host = gfx::CompositionHost::Get();
    const int wPx = 4;
    auto surface = host.CreateSurface(wPx, hPx);
    host.Draw(surface, scale, [&](ID2D1DeviceContext* dc) { painter(dc, hPx / scale); });
    auto brush = host.Compositor().CreateSurfaceBrush(surface);
    brush.Stretch(wuc::CompositionStretch::Fill);
    return brush;
}

com_ptr<ID2D1LinearGradientBrush> VGrad(ID2D1DeviceContext* dc, float y0, float y1,
                                        std::initializer_list<std::pair<float, uint32_t>> stops) {
    std::vector<D2D1_GRADIENT_STOP> v;
    for (auto& [p, c] : stops) v.push_back({p, Hex(c)});
    com_ptr<ID2D1GradientStopCollection> coll;
    winrt::check_hresult(dc->CreateGradientStopCollection(v.data(), static_cast<UINT32>(v.size()), coll.put()));
    com_ptr<ID2D1LinearGradientBrush> b;
    winrt::check_hresult(dc->CreateLinearGradientBrush({{0, y0}, {0, y1}}, coll.get(), b.put()));
    return b;
}

void FillLine(ID2D1DeviceContext* dc, float y, float h, uint32_t color) {
    com_ptr<ID2D1SolidColorBrush> b;
    dc->CreateSolidColorBrush(Hex(color), b.put());
    dc->FillRectangle({-1, y, 100, y + h}, b.get());
}

void Place(const wuc::Visual& v, const RECT& r) {
    v.Offset(float3{static_cast<float>(r.left), static_cast<float>(r.top), 0});
    v.Size(float2{static_cast<float>(std::max<LONG>(r.right - r.left, 0)),
                  static_cast<float>(std::max<LONG>(r.bottom - r.top, 0))});
}

RECT CenteredRect(int cx, int cy, int w, int h) { return {cx - w / 2, cy - h / 2, cx - w / 2 + w, cy - h / 2 + h}; }

com_ptr<IWICBitmapSource> LoadAppIcon(int sizePx) {
    HICON icon = ui_control::get()->load_main_icon(sizePx, sizePx);
    if (!icon) return nullptr;
    auto* wic = gfx::CompositionHost::Get().Wic();
    com_ptr<IWICBitmap> bmp;
    com_ptr<IWICFormatConverter> conv;
    com_ptr<IWICBitmapSource> result;
    if (SUCCEEDED(wic->CreateBitmapFromHICON(icon, bmp.put())) && SUCCEEDED(wic->CreateFormatConverter(conv.put())) &&
        SUCCEEDED(conv->Initialize(bmp.get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                   WICBitmapPaletteTypeMedianCut)))
        result = conv;
    DestroyIcon(icon);
    return result;
}

// Volume slider position <-> foobar2000 volume in dB (-100..0). A 40*log10 curve gives
// a perceptually even slider, like WMP's.
float SliderFromDb(float db) { return db <= -99.0f ? 0.0f : std::clamp(std::pow(10.0f, db / 40.0f), 0.0f, 1.0f); }
float DbFromSlider(float v) { return v <= 0.01f ? -100.0f : std::max(-100.0f, 40.0f * std::log10(v)); }

// Playback order helpers (shuffle / repeat buttons map onto foobar2000's playback orders).
std::string ActiveOrderName() {
    auto pm = playlist_manager::get();
    return pm->playback_order_get_name(pm->playback_order_get_active());
}
void SetOrderByName(const char* name) {
    auto pm = playlist_manager::get();
    for (t_size i = 0, n = pm->playback_order_get_count(); i < n; ++i)
        if (pfc::stricmp_ascii(pm->playback_order_get_name(i), name) == 0) {
            pm->playback_order_set_active(i);
            return;
        }
}

}  // namespace

// ---- Core callbacks ----------------------------------------------------------------------

struct MainWindow::PlayCallback : play_callback_impl_base {
    explicit PlayCallback(MainWindow& w)
        : play_callback_impl_base(flag_on_playback_starting | flag_on_playback_new_track | flag_on_playback_stop |
                                  flag_on_playback_pause | flag_on_playback_edited |
                                  flag_on_playback_dynamic_info_track | flag_on_volume_change),
          window(w) {}

    void on_playback_starting(play_control::t_track_command, bool) override { window.OnPlaybackStateChanged(); }
    void on_playback_new_track(metadb_handle_ptr) override {
        window.OnTrackChanged();
        window.OnPlaybackStateChanged();
    }
    void on_playback_stop(play_control::t_stop_reason reason) override {
        if (reason == play_control::stop_reason_starting_another) return;
        window.OnPlaybackStateChanged();
        window.OnTrackChanged();
    }
    void on_playback_pause(bool) override { window.OnPlaybackStateChanged(); }
    void on_playback_edited(metadb_handle_ptr) override { window.OnTrackChanged(); }
    void on_playback_dynamic_info_track(const file_info&) override { window.OnTrackChanged(); }
    void on_volume_change(float) override { window.OnVolumeChanged(); }

    MainWindow& window;
};

struct MainWindow::PlaylistCallback : playlist_callback_impl_base {
    explicit PlaylistCallback(MainWindow& w) : playlist_callback_impl_base(flag_on_playback_order_changed), window(w) {}
    void on_playback_order_changed(t_size) override { window.OnPlaybackOrderChanged(); }
    MainWindow& window;
};

// ---- Creation / destruction ----------------------------------------------------------

MainWindow::MainWindow() = default;

MainWindow::~MainWindow() {
    if (m_hwnd) DestroyWindow(m_hwnd);
}

HWND MainWindow::Create(user_interface::HookProc_t hook) {
    m_hook = hook;
    const HINSTANCE inst = core_api::get_my_instance();

    WNDCLASSEXW wc{sizeof(wc)};
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = &MainWindow::WndProcThunk;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = ui_control::get()->get_main_icon();
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc);  // failing because it already exists is fine

    // Default: 1000x680 DIP centered on the primary work area.
    const float sys = GetDpiForSystem() / 96.0f;
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    const int w = static_cast<int>(1000 * sys), h = static_cast<int>(680 * sys);
    const int x = work.left + (work.right - work.left - w) / 2, y = work.top + (work.bottom - work.top - h) / 2;

    CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP | WS_EX_APPWINDOW, kClassName, kAppName,
                    WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, x, y, w, h, nullptr, nullptr, inst, this);
    if (!m_hwnd) throw exception_win32(GetLastError());

    int show = SW_SHOWNORMAL;
    WINDOWPLACEMENT wp = cfg_placement.get();
    if (wp.length == sizeof(WINDOWPLACEMENT)) {
        show = (wp.showCmd == SW_SHOWMAXIMIZED) ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL;
        wp.showCmd = SW_HIDE;
        wp.flags = 0;
        SetWindowPlacement(m_hwnd, &wp);
    }
    ShowWindow(m_hwnd, show);
    m_shadow.Sync();
    return m_hwnd;
}

void MainWindow::Destroy() {
    if (m_hwnd) DestroyWindow(m_hwnd);
}

LRESULT CALLBACK MainWindow::WndProcThunk(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    MainWindow* self = nullptr;
    if (msg == WM_NCCREATE) {
        self = static_cast<MainWindow*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        self->m_hwnd = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
    if (self->m_hook) {
        LRESULT ret = 0;
        if (self->m_hook(hwnd, msg, wp, lp, &ret)) return ret;
    }
    try {
        return self->WndProc(msg, wp, lp);
    } catch (const std::exception& e) {
        console::complain("Glasswave", e.what());
        return DefWindowProcW(hwnd, msg, wp, lp);
    } catch (const winrt::hresult_error& e) {
        console::complain("Glasswave", pfc::stringcvt::string_utf8_from_wide(e.message().c_str()));
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

// ---- Composition visual tree ---------------------------------------------------------

void MainWindow::InitComposition() {
    // Allow this non-UWP window to use HostBackdropBrush (Windows 11 22000+, documented).
    BOOL enable = TRUE;
    DwmSetWindowAttribute(m_hwnd, DWMWA_USE_HOSTBACKDROPBRUSH, &enable, sizeof(enable));
    // We draw the Win7 corners and border ourselves: no Win11 rounding, no accent border.
    DWM_WINDOW_CORNER_PREFERENCE corner = DWMWCP_DONOTROUND;
    DwmSetWindowAttribute(m_hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
    COLORREF border = DWMWA_COLOR_NONE;
    DwmSetWindowAttribute(m_hwnd, DWMWA_BORDER_COLOR, &border, sizeof(border));
    DWM_SYSTEMBACKDROP_TYPE backdrop = DWMSBT_NONE;
    DwmSetWindowAttribute(m_hwnd, DWMWA_SYSTEMBACKDROP_TYPE, &backdrop, sizeof(backdrop));
    // The Win11 DWM shadow is replaced by ShadowFrame's Win7-measured shadow.
    DWMNCRENDERINGPOLICY ncrp = DWMNCRP_DISABLED;
    DwmSetWindowAttribute(m_hwnd, DWMWA_NCRENDERING_POLICY, &ncrp, sizeof(ncrp));

    auto& host = gfx::CompositionHost::Get();
    const auto& comp = host.Compositor();
    auto interop = comp.as<ABI::Windows::UI::Composition::Desktop::ICompositorDesktopInterop>();
    winrt::check_hresult(interop->CreateDesktopWindowTarget(
        m_hwnd, FALSE,
        reinterpret_cast<ABI::Windows::UI::Composition::Desktop::IDesktopWindowTarget**>(winrt::put_abi(m_target))));

    m_root = comp.CreateContainerVisual();
    m_root.RelativeSizeAdjustment(float2{1, 1});
    m_target.Root(m_root);

    // Win7 windows have all four corners rounded (radius 6 at 96 DPI).
    m_clipGeometry = comp.CreateRoundedRectangleGeometry();
    m_root.Clip(comp.CreateGeometricClip(m_clipGeometry));

    m_glass = std::make_unique<aero::GlassMaterial>(aero::kWin7Sky);
    auto children = m_root.Children();
    children.InsertAtTop(m_glass->Root());

    m_frameOutline = comp.CreateSpriteVisual();
    m_frameOutline.RelativeSizeAdjustment(float2{1, 1});
    for (auto* v : {&m_clientEdge, &m_navBar, &m_cmdBar, &m_contentTop, &m_transportBand, &m_capsule, &m_divider,
                    &m_clientGlow, &m_contentText, &m_title, &m_nowPlaying})
        *v = comp.CreateSpriteVisual();
    for (const auto& v : {m_frameOutline, m_clientEdge, m_navBar, m_cmdBar, m_contentTop, m_transportBand,
                          m_capsule, m_divider, m_clientGlow, m_contentText, m_title, m_nowPlaying})
        children.InsertAtTop(v);
}

void MainWindow::CreateWidgets() {
    auto add = [&](int id, Widget::Painter painter, Widget::Options opts) -> Widget* {
        auto w = std::make_unique<Widget>(id, std::move(painter), std::move(opts));
        m_root.Children().InsertAtTop(w->Visual());
        m_widgets.push_back(std::move(w));
        return m_widgets.back().get();
    };

    // Caption button cluster
    Widget::Options caption;
    caption.inactiveLayer = true;
    add(kCapMin, [](auto* dc, D2D1_SIZE_F s, Layer l) {
        paint::CaptionButton(dc, s, paint::CaptionGlyph::Minimize, l, true, false);
    }, caption)->onClick = [this] { ShowWindow(m_hwnd, SW_MINIMIZE); };
    add(kCapMax, [this](auto* dc, D2D1_SIZE_F s, Layer l) {
        paint::CaptionButton(dc, s,
                             IsWindowMaximized() ? paint::CaptionGlyph::Restore : paint::CaptionGlyph::Maximize, l,
                             false, false);
    }, caption)->onClick = [this] { ShowWindow(m_hwnd, IsWindowMaximized() ? SW_RESTORE : SW_MAXIMIZE); };
    add(kCapClose, [](auto* dc, D2D1_SIZE_F s, Layer l) {
        paint::CaptionButton(dc, s, paint::CaptionGlyph::Close, l, false, true);
    }, caption)->onClick = [this] { PostMessageW(m_hwnd, WM_SYSCOMMAND, SC_CLOSE, 0); };

    // Transport controls (WMP 12 library mode)
    add(kShuffle, [this](auto* dc, D2D1_SIZE_F s, Layer l) {
        paint::IconButton(dc, s, l, TransportGlyph::Shuffle, true, m_shuffle);
    }, {})->onClick = [this] { SetOrderByName(m_shuffle ? "Default" : "Shuffle (tracks)"); };
    add(kRepeat, [this](auto* dc, D2D1_SIZE_F s, Layer l) {
        paint::IconButton(dc, s, l, TransportGlyph::Repeat, true, m_repeat);
    }, {})->onClick = [this] { SetOrderByName(m_repeat ? "Default" : "Repeat (playlist)"); };
    add(kStop, [this](auto* dc, D2D1_SIZE_F s, Layer l) {
        paint::IconButton(dc, s, l, TransportGlyph::Stop, m_playing, false);
    }, {})->onClick = [] { playback_control::get()->stop(); };
    add(kPrev, [](auto* dc, D2D1_SIZE_F s, Layer l) {
        paint::Segment(dc, s, true, l, TransportGlyph::Previous, true);
    }, {})->onClick = [] { playback_control::get()->previous(); };
    add(kNext, [](auto* dc, D2D1_SIZE_F s, Layer l) {
        paint::Segment(dc, s, false, l, TransportGlyph::Next, true);
    }, {})->onClick = [] { playback_control::get()->next(); };

    Widget::Options orb;
    orb.circular = true;
    orb.haloFactor = 1.6f;
    orb.halo = [](ID2D1DeviceContext* dc, float size) { paint::OrbHalo(dc, size); };
    add(kPlay, [this](auto* dc, D2D1_SIZE_F s, Layer l) {
        const bool showPause = m_playing && !m_paused;
        paint::PlayOrb(dc, s.width, l, showPause ? TransportGlyph::Pause : TransportGlyph::Play);
    }, orb)->onClick = [] { playback_control::get()->play_or_pause(); };

    add(kMute, [this](auto* dc, D2D1_SIZE_F s, Layer l) {
        paint::IconButton(dc, s, l, m_muted ? TransportGlyph::Mute : TransportGlyph::Volume, true, false);
    }, {})->onClick = [] { playback_control::get()->volume_mute_toggle(); };
    add(kVolume, [this](auto* dc, D2D1_SIZE_F s, Layer l) {
        paint::VolumeSlider(dc, s, m_volume, l);
    }, {});
}

// ---- Layout and painting -------------------------------------------------------------

void MainWindow::UpdateLayout() {
    RECT rc{};
    GetClientRect(m_hwnd, &rc);
    const int W = rc.right, H = rc.bottom;
    if (W <= 0 || H <= 0) return;
    const bool max = IsWindowMaximized();
    Layout& L = m_layout;
    const int line = LinePx();

    // Frame and client area
    L.border = max ? 0 : PxI(m::kResizeBorder);
    const int capH = PxI(max ? m::kCaptionHeightMax : m::kCaptionHeight);
    L.caption = {0, 0, W, capH};
    L.client = {L.border, capH, W - L.border, H - L.border};
    L.clientEdge = {L.client.left - 2 * line, L.client.top - 2 * line, L.client.right + 2 * line,
                    L.client.bottom + 2 * line};
    L.navBar = {L.client.left, L.client.top, L.client.right, L.client.top + PxI(m::kNavBarHeight)};
    L.cmdBar = {L.client.left, L.navBar.bottom, L.client.right, L.navBar.bottom + PxI(m::kCommandBarHeight)};
    L.transport = {L.client.left, L.client.bottom - PxI(m::kTransportHeight), L.client.right, L.client.bottom};
    L.content = {L.client.left, L.cmdBar.bottom, L.client.right, L.transport.top};

    // Caption buttons hang from the top edge (flush when maximized).
    const int capRight = W - (max ? PxI(2) : PxI(m::kCapBtnRightInset));
    const int capTop = max ? 0 : PxI(m::kCapBtnTopInset);
    const int bh = PxI(m::kCapBtnH);
    L.capClose = {capRight - PxI(m::kCapBtnCloseW), capTop, capRight, capTop + bh};
    L.capMax = {L.capClose.left - PxI(m::kCapBtnMaxW), capTop, L.capClose.left, capTop + bh};
    L.capMin = {L.capMax.left - PxI(m::kCapBtnMinW), capTop, L.capMax.left, capTop + bh};

    // Icon and caption text (the title visual carries a glow margin on every side)
    const int cy = max ? capH / 2 : (capH + PxI(2)) / 2;
    const int iconSz = PxI(m::kIconSize);
    const int iconLeft = L.border + PxI(max ? 4.0f : 1.0f);
    const int pad = PxI(paint::kGlowPadding);
    L.icon = {iconLeft, cy - iconSz / 2, iconLeft + iconSz, cy - iconSz / 2 + iconSz};
    L.title = {iconLeft - pad, cy - PxI(12) - pad, std::max<int>(iconLeft, L.capMin.left - PxI(4)),
               cy + PxI(12) + pad};

    // Transport band: capsule and controls centered on the orb.
    const int tcx = (L.transport.left + L.transport.right) / 2;
    const int tcy = (L.transport.top + 2 * line + L.transport.bottom) / 2;
    auto at = [&](float dx) { return tcx + PxI(dx); };
    L.capsule = CenteredRect(tcx, tcy, PxI(m::kCapsuleWidth), PxI(m::kCapsuleHeight));
    const int orbPx = PxI(m::kOrbDiameter + 2.0f);
    L.play = CenteredRect(tcx, tcy, orbPx, orbPx);
    const int segH = PxI(m::kSegmentHeight);
    L.prev = {at(-m::kSegmentReach), tcy - segH / 2, tcx, tcy - segH / 2 + segH};
    L.next = {tcx, tcy - segH / 2, at(m::kSegmentReach), tcy - segH / 2 + segH};
    const int ibw = PxI(m::kIconButtonW), ibh = PxI(m::kIconButtonH);
    L.shuffle = CenteredRect(at(m::kShuffleX), tcy, ibw, ibh);
    L.repeat = CenteredRect(at(m::kRepeatX), tcy, ibw, ibh);
    L.stop = CenteredRect(at(m::kStopX), tcy, ibw, ibh);
    L.mute = CenteredRect(at(m::kVolumeX), tcy, ibw, ibh);
    L.volume = {at(m::kSliderLeft), tcy - ibh / 2, at(m::kSliderRight), tcy - ibh / 2 + ibh};
    L.divider = CenteredRect(at(m::kDividerX), tcy, PxI(4), PxI(16));
    L.nowPlaying = {L.client.left + PxI(10), L.transport.top + PxI(6),
                    std::max<int>(L.client.left + PxI(10), L.capsule.left - PxI(12)), L.transport.bottom - PxI(6)};

    // Apply to the visual tree.
    const float radius = max ? 0.0f : Px(m::kCornerRadius);
    m_clipGeometry.Size(float2{static_cast<float>(W), static_cast<float>(H)});
    m_clipGeometry.CornerRadius(float2{radius, radius});
    m_frameOutline.IsVisible(!max);

    Place(m_clientEdge, L.clientEdge);
    Place(m_clientGlow, L.client);
    Place(m_navBar, L.navBar);
    Place(m_cmdBar, L.cmdBar);
    Place(m_contentTop, {L.content.left, L.content.top, L.content.right, L.content.top + PxI(14)});
    Place(m_transportBand, L.transport);
    Place(m_capsule, L.capsule);
    Place(m_divider, L.divider);
    {
        RECT t = L.content;
        t.left += PxI(28);
        t.top += PxI(24);
        t.right = std::max(t.left, std::min<LONG>(t.right - PxI(28), t.left + PxI(900)));
        t.bottom = std::max(t.top, std::min<LONG>(t.bottom, t.top + PxI(220)));
        Place(m_contentText, t);
    }
    {
        RECT t = L.title;
        t.right = std::min<LONG>(t.right, t.left + PxI(600) + 2 * pad);
        Place(m_title, t);
    }
    {
        RECT t = L.nowPlaying;
        t.right = std::min<LONG>(t.right, t.left + PxI(360));
        Place(m_nowPlaying, t);
    }

    for (auto& w : m_widgets) {
        const RECT* r = nullptr;
        switch (w->Id()) {
        case kCapMin: r = &L.capMin; break;
        case kCapMax: r = &L.capMax; break;
        case kCapClose: r = &L.capClose; break;
        case kShuffle: r = &L.shuffle; break;
        case kRepeat: r = &L.repeat; break;
        case kStop: r = &L.stop; break;
        case kPrev: r = &L.prev; break;
        case kNext: r = &L.next; break;
        case kPlay: r = &L.play; break;
        case kMute: r = &L.mute; break;
        case kVolume: r = &L.volume; break;
        }
        if (r) w->SetBounds(*r, m_scale);
    }
    m_glass->UpdateReflectionAnchor(m_hwnd);
}

void MainWindow::RepaintScaleDependent() {
    const float s = m_scale;
    const int line = LinePx();

    const int frameSrc = PxI(32);
    m_frameOutline.Brush(NineGrid(frameSrc, frameSrc, s, static_cast<float>(PxI(m::kCornerRadius + 2)), true,
                                  [&](ID2D1DeviceContext* dc) {
                                      paint::FrameOutline(dc, D2D1::SizeF(frameSrc / s, frameSrc / s),
                                                          m::kCornerRadius);
                                  }));
    const int edgeSrc = PxI(16);
    m_clientEdge.Brush(NineGrid(edgeSrc, edgeSrc, s, static_cast<float>(3 * line), false, [&](ID2D1DeviceContext* dc) {
        paint::ClientEdge(dc, D2D1::SizeF(edgeSrc / s, edgeSrc / s));
    }));
    m_clientGlow.Brush(NineGrid(edgeSrc, edgeSrc, s, static_cast<float>(4 * line), true, [&](ID2D1DeviceContext* dc) {
        paint::ClientGlow(dc, D2D1::SizeF(edgeSrc / s, edgeSrc / s));
    }));

    // WMP 12 client chrome, measured: nav bar #daefff -> #e7eefe over a #c3cccf rule;
    // command bar #f5feff / #e9f3fd | #dce6f2 / #e1eaf9 over #cbd2e2 + #abb1bf rules;
    // transport band #99a1aa + #ecf3fc rules over #e8f0f9.
    const float lineDip = line / s;
    m_navBar.Brush(Strip(PxI(m::kNavBarHeight), s, [&](ID2D1DeviceContext* dc, float h) {
        dc->FillRectangle({-1, 0, 100, h}, VGrad(dc, 0, h, {{0.0f, 0xDAEFFF}, {0.3f, 0xE2EBF8}, {1.0f, 0xE7EEFE}}).get());
        FillLine(dc, h - lineDip, lineDip, 0xC3CCCF);
    }));
    m_cmdBar.Brush(Strip(PxI(m::kCommandBarHeight), s, [&](ID2D1DeviceContext* dc, float h) {
        dc->FillRectangle({-1, 0, 100, h},
                          VGrad(dc, 0, h, {{0.0f, 0xF5FEFF}, {0.3f, 0xEDF7FD}, {0.48f, 0xE9F3FD},
                                           {0.52f, 0xDCE6F2}, {1.0f, 0xE1EAF9}})
                              .get());
        FillLine(dc, h - 2 * lineDip, lineDip, 0xCBD2E2);
        FillLine(dc, h - lineDip, lineDip, 0xABB1BF);
    }));
    m_contentTop.Brush(Strip(PxI(14), s, [&](ID2D1DeviceContext* dc, float h) {
        dc->FillRectangle({-1, 0, 100, h}, VGrad(dc, 0, h, {{0.0f, 0xE0E7F5}, {1.0f, 0xF1F7FD}}).get());
    }));
    m_transportBand.Brush(Strip(PxI(m::kTransportHeight), s, [&](ID2D1DeviceContext* dc, float h) {
        dc->FillRectangle({-1, 0, 100, h}, VGrad(dc, 0, h, {{0.0f, 0xEBF2FB}, {1.0f, 0xE3ECF7}}).get());
        FillLine(dc, 0, lineDip, 0x99A1AA);
        FillLine(dc, lineDip, lineDip, 0xF6FAFE);
    }));
    const int capH = PxI(m::kCapsuleHeight);
    m_capsule.Brush(NineGrid(2 * capH, capH, s, static_cast<float>(capH / 2), false, [&](ID2D1DeviceContext* dc) {
        paint::TransportCapsule(dc, D2D1::SizeF(2 * capH / s, capH / s));
    }));
    {
        auto& host = gfx::CompositionHost::Get();
        const int w = PxI(4), h = PxI(16);
        host.Repaint(m_divider, w, h, s, [&](ID2D1DeviceContext* dc) { paint::CapsuleDivider(dc, D2D1::SizeF(w / s, h / s)); });
    }
    m_icon = LoadAppIcon(PxI(m::kIconSize));
    RepaintTitle();
    RepaintNowPlaying();
    RepaintContentText();
}

void MainWindow::RepaintTitle() {
    auto& host = gfx::CompositionHost::Get();
    const float pad = paint::kGlowPadding;
    const int w = PxI(600 + 2 * pad), h = PxI(24 + 2 * pad);
    host.Repaint(m_title, w, h, m_scale, [&](ID2D1DeviceContext* dc) {
        const float iconSz = m::kIconSize;
        paint::Icon(dc, m_icon.get(), D2D1::RectF(pad, pad + 12 - iconSz / 2, pad + iconSz, pad + 12 + iconSz / 2));
        auto layout = MakeText(kAppName, L"Segoe UI", m::kTitleFontSize, DWRITE_FONT_WEIGHT_NORMAL, 560, 24);
        layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        paint::GlowText(dc, layout.get(), D2D1::Point2F(pad + iconSz + 6, pad), D2D1::ColorF(0, 0, 0, 1));
    });
    UpdateLayout();  // Repaint resets the visual to the surface size; re-apply the layout clip.
}

void MainWindow::RepaintNowPlaying() {
    auto& host = gfx::CompositionHost::Get();
    const int w = PxI(360), h = PxI(33);
    host.Repaint(m_nowPlaying, w, h, m_scale, [&](ID2D1DeviceContext* dc) {
        if (m_trackTitle.empty()) return;
        com_ptr<ID2D1SolidColorBrush> dark, grey;
        dc->CreateSolidColorBrush(Hex(0x1E1E1E), dark.put());
        dc->CreateSolidColorBrush(Hex(0x5B5B5B), grey.put());
        auto t = MakeText(m_trackTitle, L"Segoe UI", 12.0f, DWRITE_FONT_WEIGHT_NORMAL, 350, 17);
        dc->DrawTextLayout({0, 0}, t.get(), dark.get());
        if (!m_trackArtist.empty()) {
            auto a = MakeText(m_trackArtist, L"Segoe UI", 12.0f, DWRITE_FONT_WEIGHT_NORMAL, 350, 17);
            dc->DrawTextLayout({0, 16}, a.get(), grey.get());
        }
    });
    UpdateLayout();
}

void MainWindow::RepaintContentText() {
    auto& host = gfx::CompositionHost::Get();
    const int w = PxI(900), h = PxI(220);
    host.Repaint(m_contentText, w, h, m_scale, [&](ID2D1DeviceContext* dc) {
        auto solid = [&](uint32_t c) {
            com_ptr<ID2D1SolidColorBrush> b;
            dc->CreateSolidColorBrush(Hex(c), b.put());
            return b;
        };
        auto heading = MakeText(L"Now Playing", L"Segoe UI", 12.0f, DWRITE_FONT_WEIGHT_NORMAL, 880, 20);
        dc->DrawTextLayout({0, 0}, heading.get(), solid(0x1E5BA8).get());
        const std::wstring title = m_trackTitle.empty() ? L"Not playing" : m_trackTitle;
        auto t = MakeText(title, L"Segoe UI Light", 26.0f, DWRITE_FONT_WEIGHT_LIGHT, 880, 40);
        dc->DrawTextLayout({0, 24}, t.get(), solid(0x1A1A1A).get());
        if (!m_trackArtist.empty()) {
            auto a = MakeText(m_trackArtist, L"Segoe UI", 14.0f, DWRITE_FONT_WEIGHT_NORMAL, 880, 22);
            dc->DrawTextLayout({2, 68}, a.get(), solid(0x5B5B5B).get());
        }
        auto hint = MakeText(L"Right-click here or press Alt to open the foobar2000 menu (add files, preferences...)",
                             L"Segoe UI", 12.0f, DWRITE_FONT_WEIGHT_NORMAL, 880, 20);
        dc->DrawTextLayout({2, 130}, hint.get(), solid(0x8A8A8A).get());
    });
    UpdateLayout();
}

// ---- Core state ------------------------------------------------------------------------

void MainWindow::OnPlaybackStateChanged() {
    auto pc = playback_control::get();
    const bool playing = pc->is_playing(), paused = pc->is_paused();
    if (playing == m_playing && paused == m_paused) return;
    m_playing = playing;
    m_paused = paused;
    for (int id : {kPlay, kStop})
        if (auto* w = WidgetById(id)) w->Invalidate();
}

void MainWindow::OnTrackChanged() {
    static titleformat_object::ptr s_title, s_artist;
    if (!s_title) {
        titleformat_compiler::get()->compile_safe(s_title, "%title%");
        titleformat_compiler::get()->compile_safe(s_artist, "[%artist%]");
    }
    auto pc = playback_control::get();
    pfc::string8 title, artist;
    if (pc->is_playing()) {
        pc->playback_format_title(nullptr, title, s_title, nullptr, playback_control::display_level_all);
        pc->playback_format_title(nullptr, artist, s_artist, nullptr, playback_control::display_level_all);
    }
    m_trackTitle = Wide(title);
    m_trackArtist = Wide(artist);
    RepaintNowPlaying();
    RepaintContentText();

    std::wstring caption = m_trackTitle.empty() ? std::wstring(kAppName)
                                                : m_trackTitle + (m_trackArtist.empty() ? L"" : L" - " + m_trackArtist) +
                                                      L" - " + kAppName;
    SetWindowTextW(m_hwnd, caption.c_str());
}

void MainWindow::OnVolumeChanged() {
    auto pc = playback_control::get();
    const bool muted = pc->is_muted();
    const float v = muted ? m_volume : SliderFromDb(pc->get_volume());
    if (muted != m_muted) {
        m_muted = muted;
        if (auto* w = WidgetById(kMute)) w->Invalidate();
    }
    if (std::abs(v - m_volume) > 1e-4f) {
        m_volume = v;
        if (auto* w = WidgetById(kVolume)) w->Invalidate();
    }
}

void MainWindow::OnPlaybackOrderChanged() {
    const std::string name = ActiveOrderName();
    const bool shuffle = name.find("Shuffle") != std::string::npos || name.find("Random") != std::string::npos;
    const bool repeat = name.rfind("Repeat", 0) == 0;
    if (shuffle != m_shuffle) {
        m_shuffle = shuffle;
        if (auto* w = WidgetById(kShuffle)) w->Invalidate();
    }
    if (repeat != m_repeat) {
        m_repeat = repeat;
        if (auto* w = WidgetById(kRepeat)) w->Invalidate();
    }
}

void MainWindow::SetVolumeFromX(int x) {
    const RECT& r = m_layout.volume;
    const float thumb = Px(11.0f);
    const float x0 = r.left + thumb / 2, x1 = r.right - thumb / 2;
    const float v = std::clamp((x - x0) / std::max(1.0f, x1 - x0), 0.0f, 1.0f);
    m_volume = v;
    if (auto* w = WidgetById(kVolume)) w->Invalidate();
    playback_control::get()->set_volume(DbFromSlider(v));
}

// ---- Hit testing and interaction -----------------------------------------------------

Widget* MainWindow::WidgetAt(POINT pt) const {
    for (auto it = m_widgets.rbegin(); it != m_widgets.rend(); ++it)  // later widgets are on top
        if ((*it)->HitTest(pt)) return it->get();
    return nullptr;
}

Widget* MainWindow::WidgetById(int id) const {
    for (auto& w : m_widgets)
        if (w->Id() == id) return w.get();
    return nullptr;
}

LRESULT MainWindow::HitTest(POINT pt) const {
    RECT rc{};
    GetClientRect(m_hwnd, &rc);
    const LONG W = rc.right, H = rc.bottom;
    const auto& L = m_layout;

    if (!IsWindowMaximized()) {
        const LONG b = L.border, corner = L.border * 2;
        const bool left = pt.x < b, right = pt.x >= W - b, top = pt.y < b, bottom = pt.y >= H - b;
        const bool nearL = pt.x < corner, nearR = pt.x >= W - corner, nearT = pt.y < corner, nearB = pt.y >= H - corner;
        if ((top && nearL) || (left && nearT)) return HTTOPLEFT;
        if ((top && nearR) || (right && nearT)) return HTTOPRIGHT;
        if ((bottom && nearL) || (left && nearB)) return HTBOTTOMLEFT;
        if ((bottom && nearR) || (right && nearB)) return HTBOTTOMRIGHT;
        if (left) return HTLEFT;
        if (right) return HTRIGHT;
        if (bottom) return HTBOTTOM;
    }
    // Real caption-button hit codes: Windows 11 shows Snap Layouts over HTMAXBUTTON.
    if (PtInRect(&L.capClose, pt)) return HTCLOSE;
    if (PtInRect(&L.capMax, pt)) return HTMAXBUTTON;
    if (PtInRect(&L.capMin, pt)) return HTMINBUTTON;
    if (!IsWindowMaximized() && pt.y < PxI(4)) return HTTOP;
    if (PtInRect(&L.icon, pt)) return HTSYSMENU;
    if (pt.y < L.client.top) return HTCAPTION;
    return HTCLIENT;
}

void MainWindow::SetHoverWidget(Widget* w) {
    if (w == m_hover) return;
    if (m_hover) m_hover->SetHover(false);
    m_hover = w;
    if (m_hover) m_hover->SetHover(true);
}

void MainWindow::OnActivate(bool active) {
    if (m_active == active) return;
    m_active = active;
    if (m_glass) m_glass->SetActive(active);
    for (auto& w : m_widgets)
        if (IsCaptionWidget(w->Id())) w->SetInactive(!active);
    if (m_title) gfx::CompositionHost::Get().AnimateOpacity(m_title, active ? 1.0f : 0.85f, 200);
    m_shadow.SetActive(active);
}

// ---- Window procedure ----------------------------------------------------------------

LRESULT MainWindow::WndProc(UINT msg, WPARAM wp, LPARAM lp) {
    const HWND hwnd = m_hwnd;
    switch (msg) {
    case WM_CREATE:
        m_scale = GetDpiForWindow(hwnd) / 96.0f;
        InitComposition();
        CreateWidgets();
        RepaintScaleDependent();
        m_shadow.Create(hwnd);
        m_playCallback = std::make_unique<PlayCallback>(*this);
        m_playlistCallback = std::make_unique<PlaylistCallback>(*this);
        OnPlaybackStateChanged();
        OnTrackChanged();
        OnVolumeChanged();
        OnPlaybackOrderChanged();
        SetWindowPos(hwnd, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER);
        return 0;

    case WM_NCCALCSIZE:
        if (wp) {
            // The whole window is client area. A maximized window is pushed out by the
            // frame thickness on every side, so pull the client back onto the monitor.
            if (IsWindowMaximized()) {
                auto* p = reinterpret_cast<NCCALCSIZE_PARAMS*>(lp);
                const UINT dpi = GetDpiForWindow(hwnd);
                const int fx = GetSystemMetricsForDpi(SM_CXFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
                const int fy = GetSystemMetricsForDpi(SM_CYFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
                InflateRect(&p->rgrc[0], -fx, -fy);
            }
            return 0;
        }
        break;

    case WM_NCHITTEST: {
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ScreenToClient(hwnd, &pt);
        return HitTest(pt);
    }

    case WM_NCACTIVATE:
        OnActivate(wp != FALSE);
        return DefWindowProcW(hwnd, msg, wp, -1);  // -1: do not repaint the (non-existent) frame

    case WM_SIZE: {
        const bool wasMax = m_layout.border == 0;
        UpdateLayout();
        if (wasMax != (m_layout.border == 0))
            if (auto* w = WidgetById(kCapMax)) w->Invalidate();
        return 0;
    }

    case WM_MOVE:
        if (m_glass) m_glass->UpdateReflectionAnchor(hwnd);
        return 0;

    case WM_WINDOWPOSCHANGED: {
        const LRESULT r = DefWindowProcW(hwnd, msg, wp, lp);  // generates WM_SIZE / WM_MOVE
        m_shadow.Sync();
        return r;
    }

    case WM_DPICHANGED: {
        m_scale = HIWORD(wp) / 96.0f;
        const auto* r = reinterpret_cast<const RECT*>(lp);
        SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        RepaintScaleDependent();
        UpdateLayout();
        return 0;
    }

    case WM_GETMINMAXINFO: {
        auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
        mmi->ptMinTrackSize = {PxI(520), PxI(380)};
        return 0;
    }

    case WM_SETTINGCHANGE:
        if (m_glass) m_glass->RefreshTransparencyPolicy();
        break;

    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        ValidateRect(hwnd, nullptr);
        return 0;

    // -- Caption buttons (non-client messages) --
    case WM_NCMOUSEMOVE: {
        Widget* w = nullptr;
        if (wp == HTMINBUTTON) w = WidgetById(kCapMin);
        else if (wp == HTMAXBUTTON) w = WidgetById(kCapMax);
        else if (wp == HTCLOSE) w = WidgetById(kCapClose);
        if (!m_pressed) SetHoverWidget(w);
        if (!m_trackingNc) {
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE | TME_NONCLIENT, hwnd, 0};
            m_trackingNc = TrackMouseEvent(&tme) != FALSE;
        }
        break;
    }
    case WM_NCMOUSELEAVE:
        m_trackingNc = false;
        if (m_hover && IsCaptionWidget(m_hover->Id()) && !m_pressed) SetHoverWidget(nullptr);
        break;
    case WM_NCLBUTTONDOWN:
    case WM_NCLBUTTONDBLCLK:
        if (wp == HTMINBUTTON || wp == HTMAXBUTTON || wp == HTCLOSE) {
            const int id = wp == HTMINBUTTON ? kCapMin : wp == HTMAXBUTTON ? kCapMax : kCapClose;
            m_ncDownId = id;
            m_pressed = WidgetById(id);
            m_pressed->SetPressed(true);
            SetCapture(hwnd);
            return 0;  // keep DefWindowProc from drawing classic buttons
        }
        break;
    case WM_NCLBUTTONUP:
        // Windows 11 may take the capture away over the maximize button (Snap Layouts);
        // the button-up then arrives here. Down and up on the same button is a click.
        if (wp == HTMINBUTTON || wp == HTMAXBUTTON || wp == HTCLOSE) {
            const int id = wp == HTMINBUTTON ? kCapMin : wp == HTMAXBUTTON ? kCapMax : kCapClose;
            const bool same = (id == m_ncDownId);
            m_ncDownId = 0;
            if (m_pressed) {
                m_pressed->SetPressed(false);
                m_pressed = nullptr;
                if (GetCapture() == hwnd) ReleaseCapture();
            }
            if (Widget* w = WidgetById(id); same && w && w->onClick) w->onClick();
            return 0;
        }
        break;

    // -- Client-area mouse --
    case WM_MOUSEMOVE: {
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        if (m_pressed) {
            if (m_pressed->Id() == kVolume)
                SetVolumeFromX(pt.x);
            else
                m_pressed->SetPressed(m_pressed->HitTest(pt));
        } else {
            SetHoverWidget(WidgetAt(pt));
        }
        if (!m_trackingClient) {
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
            m_trackingClient = TrackMouseEvent(&tme) != FALSE;
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        m_trackingClient = false;
        if (m_hover && !IsCaptionWidget(m_hover->Id()) && !m_pressed) SetHoverWidget(nullptr);
        return 0;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK: {
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        if (Widget* w = WidgetAt(pt)) {
            m_pressed = w;
            w->SetPressed(true);
            SetCapture(hwnd);
            if (w->Id() == kVolume) SetVolumeFromX(pt.x);
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        m_ncDownId = 0;
        if (Widget* w = m_pressed) {
            m_pressed = nullptr;
            ReleaseCapture();
            w->SetPressed(false);
            const bool inside = w->HitTest(pt);
            if (!inside || IsCaptionWidget(w->Id())) SetHoverWidget(nullptr);
            if (inside && w->onClick) w->onClick();
        }
        return 0;
    }
    case WM_CAPTURECHANGED:
        if (m_pressed && reinterpret_cast<HWND>(lp) != hwnd) {
            m_pressed->SetPressed(false);
            m_pressed = nullptr;
        }
        return 0;
    case WM_MOUSEWHEEL: {
        // Wheel over the transport band adjusts the volume, like WMP.
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ScreenToClient(hwnd, &pt);
        if (PtInRect(&m_layout.transport, pt)) {
            const short delta = GET_WHEEL_DELTA_WPARAM(wp);
            auto pc = playback_control::get();
            if (delta > 0) pc->volume_up(); else pc->volume_down();
            return 0;
        }
        break;
    }
    case WM_RBUTTONUP: {
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        if (PtInRect(&m_layout.content, pt)) {
            ClientToScreen(hwnd, &pt);
            ShowMainMenu(hwnd, pt);
            return 0;
        }
        break;
    }

    // -- Keyboard --
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        if (keyboard_shortcut_manager::get()->on_keydown_auto(wp)) return 0;
        break;
    case WM_SYSCOMMAND:
        // Alt or F10 alone opens the foobar2000 main menu (WMP 12 also hides its menu bar).
        if ((wp & 0xFFF0) == SC_KEYMENU && lp == 0) {
            POINT pt{m_layout.client.left, m_layout.client.top};
            ClientToScreen(hwnd, &pt);
            ShowMainMenu(hwnd, pt);
            return 0;
        }
        break;

    case WM_CLOSE:
        // Closing the main window exits foobar2000; the core then calls user_interface::shutdown().
        standard_commands::main_exit();
        return 0;

    case WM_DESTROY: {
        WINDOWPLACEMENT wpl{sizeof(wpl)};
        if (GetWindowPlacement(hwnd, &wpl)) cfg_placement.set(wpl);
        m_shadow.Destroy();
        m_playlistCallback.reset();
        m_playCallback.reset();
        m_hover = m_pressed = nullptr;
        m_widgets.clear();
        m_glass.reset();
        m_root = nullptr;
        m_target = nullptr;
        return 0;
    }
    case WM_NCDESTROY:
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        m_hwnd = nullptr;
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace glasswave::ui
