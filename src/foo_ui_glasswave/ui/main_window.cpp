#include "pch.h"
#include "ui/main_window.h"
#include "aero/aero_atlas.h"
#include "aero/aero_metrics.h"
#include "aero/aero_painter.h"
#include "gfx/composition_host.h"
#include "media/source_identity.h"
#include "ui/main_menu.h"
#include "ui/shell_glue.h"
#include "ui/text.h"
#include "wmp/wmp_painter.h"
#include "wmp/wmp_transport.h"
#include "win7/win7_resources.h"

namespace glasswave::ui {

namespace m = aero::metrics;
namespace paint = aero::paint;
namespace wp = wmp::paint;
namespace wt = wmp::transport;
namespace wuc = winrt::Windows::UI::Composition;
using paint::Layer;
using paint::TransportGlyph;
using winrt::com_ptr;
using winrt::Windows::Foundation::Numerics::float2;
using winrt::Windows::Foundation::Numerics::float3;

namespace {

constexpr wchar_t kClassName[] = L"Glasswave.MainWindow";
constexpr wchar_t kAppName[] = L"Glasswave";
constexpr UINT_PTR kLibraryTimer = 1;
constexpr float kNavPaneWidth = 190.0f;

enum WidgetId {
    kCapMin = 1, kCapMax, kCapClose,
    kBack, kForward, kOrganize, kAddFiles, kAddFolder, kNewPlaylist,
    kSeek, kShuffle, kRepeat, kStop, kPrev, kNext, kPlay, kMute, kVolume, kViewOptions,
    kViewOptionsArrow, kNewPlaylistArrow,
};
bool IsCaptionWidget(int id) { return id >= kCapMin && id <= kCapClose; }


// {0C9E7B42-1F3A-4D5E-8B6C-7A2F9E1D4C83}
cfg_var_modern::cfg_struct_t<WINDOWPLACEMENT> cfg_placement(
    {0x0c9e7b42, 0x1f3a, 0x4d5e, {0x8b, 0x6c, 0x7a, 0x2f, 0x9e, 0x1d, 0x4c, 0x83}});

// WMP view option chosen per library view (media::ViewStyle values).
// {6A1F3C58-2B7E-4D90-8C15-3E9A7B2D4F61} .. {6A1F3C5B-...}
cfg_var_modern::cfg_int cfg_viewSongs({0x6a1f3c58, 0x2b7e, 0x4d90, {0x8c, 0x15, 0x3e, 0x9a, 0x7b, 0x2d, 0x4f, 0x61}},
                                      static_cast<int>(media::ViewStyle::Tiles));
cfg_var_modern::cfg_int cfg_viewAlbums({0x6a1f3c59, 0x2b7e, 0x4d90, {0x8c, 0x15, 0x3e, 0x9a, 0x7b, 0x2d, 0x4f, 0x61}},
                                       static_cast<int>(media::ViewStyle::Icons));
cfg_var_modern::cfg_int cfg_viewArtists({0x6a1f3c5a, 0x2b7e, 0x4d90, {0x8c, 0x15, 0x3e, 0x9a, 0x7b, 0x2d, 0x4f, 0x61}},
                                        static_cast<int>(media::ViewStyle::Tiles));
cfg_var_modern::cfg_int cfg_viewGenres({0x6a1f3c5b, 0x2b7e, 0x4d90, {0x8c, 0x15, 0x3e, 0x9a, 0x7b, 0x2d, 0x4f, 0x61}},
                                       static_cast<int>(media::ViewStyle::Tiles));

D2D1_COLOR_F Hex(uint32_t rgb, float a = 1.0f) {
    return D2D1::ColorF(((rgb >> 16) & 0xFF) / 255.0f, ((rgb >> 8) & 0xFF) / 255.0f, (rgb & 0xFF) / 255.0f, a);
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

// A horizontally stretchable strip: painted once a few pixels wide, stretched to any width.
wuc::CompositionSurfaceBrush Strip(int hPx, float scale, const std::function<void(ID2D1DeviceContext*, float)>& painter) {
    auto& host = gfx::CompositionHost::Get();
    auto surface = host.CreateSurface(4, hPx);
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

// Windows Media Player's own icon from the user's wmploc.dll (icon group 102), else
// foobar2000's.
constexpr int kWmpIcon = 102;
HICON CreateAppIcon(int sizePx) {
    if (HICON icon = win7::Resources::Get().Icon(win7::Module::Wmploc, kWmpIcon, sizePx)) return icon;
    return ui_control::get()->load_main_icon(sizePx, sizePx);
}

com_ptr<IWICBitmapSource> LoadAppIcon(int sizePx) {
    HICON icon = CreateAppIcon(sizePx);
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

// Volume slider position <-> foobar2000 volume in dB (-100..0); 40*log10 feels even.
float SliderFromDb(float db) { return db <= -99.0f ? 0.0f : std::clamp(std::pow(10.0f, db / 40.0f), 0.0f, 1.0f); }
float DbFromSlider(float v) { return v <= 0.01f ? -100.0f : std::max(-100.0f, 40.0f * std::log10(v)); }

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

bool KeyDown(int vk) { return (GetKeyState(vk) & 0x8000) != 0; }

// Shuffle and repeat are independent like in current players; foobar2000 has one playback
// order, so the pair is kept here and mapped onto it (see ApplyPlaybackOrder).
// {8D2F6A13-4B7C-4E19-9F3A-2C6E8B1D5A70} / {8D2F6A14-...}
cfg_var_modern::cfg_int cfg_shuffle({0x8d2f6a13, 0x4b7c, 0x4e19, {0x9f, 0x3a, 0x2c, 0x6e, 0x8b, 0x1d, 0x5a, 0x70}}, 0);
cfg_var_modern::cfg_int cfg_repeat({0x8d2f6a14, 0x4b7c, 0x4e19, {0x9f, 0x3a, 0x2c, 0x6e, 0x8b, 0x1d, 0x5a, 0x70}}, 0);

}  // namespace

// ---- Core callbacks ----------------------------------------------------------------------

struct MainWindow::PlayCallback : play_callback_impl_base {
    explicit PlayCallback(MainWindow& w)
        : play_callback_impl_base(flag_on_playback_starting | flag_on_playback_new_track | flag_on_playback_stop |
                                  flag_on_playback_pause | flag_on_playback_edited | flag_on_playback_seek |
                                  flag_on_playback_time | flag_on_playback_dynamic_info_track | flag_on_volume_change),
          window(w) {}

    void on_playback_starting(play_control::t_track_command cmd, bool) override {
        // A track the user picked starts a new shuffled pass; next / previous continue it.
        if (cmd != play_control::track_command_next && cmd != play_control::track_command_prev)
            window.m_shufflePass = 0;
        window.OnPlaybackStateChanged();
    }
    void on_playback_new_track(metadb_handle_ptr) override {
        window.OnShuffleTrack();
        window.OnTrackChanged();
        window.OnPlaybackStateChanged();
    }
    void on_playback_stop(play_control::t_stop_reason reason) override {
        if (reason == play_control::stop_reason_starting_another) return;
        window.OnPlaybackStateChanged();
        window.OnTrackChanged();
    }
    void on_playback_pause(bool) override { window.OnPlaybackStateChanged(); }
    void on_playback_seek(double) override { window.OnPlaybackTime(); }
    void on_playback_time(double) override { window.OnPlaybackTime(); }
    void on_playback_edited(metadb_handle_ptr) override { window.OnTrackChanged(); }
    void on_playback_dynamic_info_track(const file_info&) override { window.OnTrackChanged(); }
    void on_volume_change(float) override { window.OnVolumeChanged(); }

    MainWindow& window;
};

struct MainWindow::PlaylistCallback : playlist_callback_impl_base {
    explicit PlaylistCallback(MainWindow& w) : playlist_callback_impl_base(static_cast<t_uint32>(flag_all)), window(w) {}

    void on_items_added(t_size p, t_size, metadb_handle_list_cref, const pfc::bit_array&) override {
        window.OnPlaylistContentChanged(p);
    }
    void on_items_reordered(t_size p, const t_size*, t_size) override { window.OnPlaylistContentChanged(p); }
    void on_items_removed(t_size p, const pfc::bit_array&, t_size, t_size) override { window.OnPlaylistContentChanged(p); }
    void on_items_modified(t_size p, const pfc::bit_array&) override { window.OnPlaylistContentChanged(p); }
    void on_items_replaced(t_size p, const pfc::bit_array&, const pfc::list_base_const_t<t_on_items_replaced_entry>&) override {
        window.OnPlaylistContentChanged(p);
    }
    void on_items_selection_change(t_size p, const pfc::bit_array&, const pfc::bit_array&) override {
        window.OnPlaylistSelectionChanged(p);
    }
    void on_item_focus_change(t_size p, t_size, t_size) override { window.OnPlaylistSelectionChanged(p); }
    void on_playlist_created(t_size, const char*, t_size) override { window.OnPlaylistsChanged(); }
    void on_playlists_reorder(const t_size*, t_size) override { window.OnPlaylistsChanged(); }
    void on_playlists_removed(const pfc::bit_array&, t_size, t_size) override { window.OnPlaylistsChanged(); }
    void on_playlist_renamed(t_size, const char*, t_size) override { window.OnPlaylistsChanged(); }
    void on_playback_order_changed(t_size) override { window.OnPlaybackOrderChanged(); }

    MainWindow& window;
};

struct MainWindow::LibraryCallback : library_callback_v2_dynamic_impl_base {
    explicit LibraryCallback(MainWindow& w) : window(w) {}
    void on_items_added(metadb_handle_list_cref) override { window.ScheduleLibraryReload(); }
    void on_items_removed(metadb_handle_list_cref) override { window.ScheduleLibraryReload(); }
    void on_items_modified_v2(metadb_handle_list_cref, metadb_io_callback_v2_data&) override {
        window.ScheduleLibraryReload();
    }
    void on_library_initialized() override { window.ScheduleLibraryReload(); }
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

    const float sys = GetDpiForSystem() / 96.0f;
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    const int w = static_cast<int>(1000 * sys), h = static_cast<int>(680 * sys);
    const int x = work.left + (work.right - work.left - w) / 2, y = work.top + (work.bottom - work.top - h) / 2;

    CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP | WS_EX_APPWINDOW, kClassName, kAppName,
                    WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, x, y, w, h, nullptr, nullptr, inst, this);
    if (!m_hwnd) throw exception_win32(GetLastError());

    int show = SW_SHOWNORMAL;
    WINDOWPLACEMENT wpl = cfg_placement.get();
    if (wpl.length == sizeof(WINDOWPLACEMENT)) {
        show = (wpl.showCmd == SW_SHOWMAXIMIZED) ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL;
        wpl.showCmd = SW_HIDE;
        wpl.flags = 0;
        SetWindowPlacement(m_hwnd, &wpl);
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

    // The frame outline sits above the rounded clip, not inside it: clipping an anti-aliased
    // one-pixel line along the same curve multiplies the two coverages and the dark line
    // all but vanishes at the corners while the light line inside it stays.
    m_windowRoot = comp.CreateContainerVisual();
    m_windowRoot.RelativeSizeAdjustment(float2{1, 1});
    m_target.Root(m_windowRoot);
    m_root = comp.CreateContainerVisual();
    m_root.RelativeSizeAdjustment(float2{1, 1});
    m_windowRoot.Children().InsertAtTop(m_root);

    // Win7 windows have all four corners rounded (radius 6 at 96 DPI).
    m_clipGeometry = comp.CreateRoundedRectangleGeometry();
    m_root.Clip(comp.CreateGeometricClip(m_clipGeometry));

    m_glass = std::make_unique<aero::GlassMaterial>(aero::kWin7Sky, aero::atlas::Available());
    auto children = m_root.Children();
    children.InsertAtTop(m_glass->Root());

    m_frameOutline = comp.CreateSpriteVisual();
    m_frameOutline.RelativeSizeAdjustment(float2{1, 1});
    for (auto* v : {&m_clientEdge, &m_navBar, &m_cmdBar, &m_contentTop, &m_transportBand, &m_capsule, &m_cluster,
                    &m_divider, &m_clientGlow, &m_title, &m_nowPlaying, &m_breadcrumb, &m_navGroove})
        *v = comp.CreateSpriteVisual();
    for (auto& v : m_sideHighlight) {
        v = comp.CreateSpriteVisual();
        children.InsertAtTop(v);
    }
    m_windowRoot.Children().InsertAtTop(m_frameOutline);
    for (const auto& v : {m_clientEdge, m_navBar, m_navGroove, m_cmdBar, m_contentTop, m_transportBand})
        children.InsertAtTop(v);
    // Navigation pane, list and search box live in their own layer above the bars.
    m_contentLayer = comp.CreateContainerVisual();
    children.InsertAtTop(m_contentLayer);
    // WMP 12's library background (light blue with soft diagonal streaks) behind the list,
    // scaled to cover it; painted once, the compositor scales it on resize.
    m_libraryBg = comp.CreateSpriteVisual();
    if (win7::Resources::Get().Has(win7::Module::Wmploc, L"LIBRARY_BACKGROUND7.PNG")) {
        const D2D1_SIZE_U bs = win7::Resources::Get().Size(win7::Module::Wmploc, L"LIBRARY_BACKGROUND7.PNG");
        auto surface = host.CreateSurface(static_cast<int>(bs.width), static_cast<int>(bs.height));
        host.Draw(surface, 1.0f, [&](ID2D1DeviceContext* dc) {
            win7::DrawStretched(dc, win7::Module::Wmploc, L"LIBRARY_BACKGROUND7.PNG",
                                D2D1::RectF(0, 0, static_cast<float>(bs.width), static_cast<float>(bs.height)));
        });
        auto brush = comp.CreateSurfaceBrush(surface);
        brush.Stretch(wuc::CompositionStretch::UniformToFill);
        brush.HorizontalAlignmentRatio(0.0f);
        brush.VerticalAlignmentRatio(0.0f);
        m_libraryBg.Brush(brush);
    }
    m_contentLayer.Children().InsertAtBottom(m_libraryBg);
    for (const auto& v : {m_capsule, m_cluster, m_divider, m_clientGlow, m_title, m_nowPlaying, m_breadcrumb})
        children.InsertAtTop(v);
    m_win7Art = wt::Available();
    m_aeroArt = aero::atlas::Available();
    m_divider.IsVisible(!m_win7Art);  // part of the original capsule bitmap
    m_cluster.IsVisible(m_win7Art);

    m_nav = std::make_unique<NavPane>(m_contentLayer);
    m_list = std::make_unique<ListView>(m_contentLayer);
    m_list->onLink = [this](size_t group, size_t line) { OnAlbumLink(group, line); };
    m_search = std::make_unique<SearchBox>(m_contentLayer);
    m_search->onChange = [this](const std::wstring& text) {
        m_filter = media::Lower(text);
        if (m_source) {
            m_source->SetFilter(m_filter);
            m_list->SetSource(m_source.get(), true);
        }
    };
}

void MainWindow::CreateWidgets() {
    auto add = [&](int id, Widget::Painter painter, Widget::Options opts) -> Widget* {
        auto w = std::make_unique<Widget>(id, std::move(painter), std::move(opts));
        m_root.Children().InsertAtTop(w->Visual());
        m_widgets.push_back(std::move(w));
        return m_widgets.back().get();
    };

    // Caption button cluster
    // Caption button cluster: Win7's own DWM images when aero.msstyles is present.
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
    add(kCapMin, [dwmArt](auto* dc, D2D1_SIZE_F s, Layer l) {
        if (dwmArt) aero::atlas::CaptionButton(dc, s, aero::atlas::Button::Minimize, paint::CaptionGlyph::Minimize, l);
        else paint::CaptionButton(dc, s, paint::CaptionGlyph::Minimize, l, true, false);
    }, captionOptions(false))->onClick = [this] { ShowWindow(m_hwnd, SW_MINIMIZE); };
    add(kCapMax, [this, dwmArt](auto* dc, D2D1_SIZE_F s, Layer l) {
        const auto glyph = IsWindowMaximized() ? paint::CaptionGlyph::Restore : paint::CaptionGlyph::Maximize;
        if (dwmArt) aero::atlas::CaptionButton(dc, s, aero::atlas::Button::Maximize, glyph, l);
        else paint::CaptionButton(dc, s, glyph, l, false, false);
    }, captionOptions(false))->onClick = [this] { ShowWindow(m_hwnd, IsWindowMaximized() ? SW_RESTORE : SW_MAXIMIZE); };
    add(kCapClose, [dwmArt](auto* dc, D2D1_SIZE_F s, Layer l) {
        if (dwmArt) aero::atlas::CaptionButton(dc, s, aero::atlas::Button::Close, paint::CaptionGlyph::Close, l);
        else paint::CaptionButton(dc, s, paint::CaptionGlyph::Close, l, false, true);
    }, captionOptions(true))->onClick = [this] { PostMessageW(m_hwnd, WM_SYSCOMMAND, SC_CLOSE, 0); };

    // Navigation bar
    Widget::Options round;
    round.circular = true;
    auto* back = add(kBack, [this](auto* dc, D2D1_SIZE_F s, Layer l) {
        if (m_win7Art) wt::NavOrb(dc, s, l, true, m_historyPos > 0);
        else wp::NavArrow(dc, s, l, true, m_historyPos > 0);
    }, round);
    back->onClick = [this] { GoBack(); };
    back->enabled = [this] { return m_historyPos > 0; };
    auto* forward = add(kForward, [this](auto* dc, D2D1_SIZE_F s, Layer l) {
        const bool enabled = m_historyPos + 1 < m_history.size();
        if (m_win7Art) wt::NavOrb(dc, s, l, false, enabled);
        else wp::NavArrow(dc, s, l, false, enabled);
    }, round);
    forward->onClick = [this] { GoForward(); };
    forward->enabled = [this] { return m_historyPos + 1 < m_history.size(); };

    // Command bar
    add(kOrganize, [](auto* dc, D2D1_SIZE_F s, Layer l) { wp::CommandButton(dc, s, l, L"Organize", true); }, {})
        ->onClick = [this] { ShowOrganizeMenu(); };
    add(kAddFiles, [](auto* dc, D2D1_SIZE_F s, Layer l) { wp::CommandButton(dc, s, l, L"Add files", false); }, {})
        ->onClick = [] { standard_commands::main_add_files(); };
    add(kAddFolder, [](auto* dc, D2D1_SIZE_F s, Layer l) { wp::CommandButton(dc, s, l, L"Add folder", false); }, {})
        ->onClick = [] { standard_commands::main_add_directory(); };
    // View options split button: the icon steps to the next view, the arrow lists them all.
    add(kViewOptions, [this](auto* dc, D2D1_SIZE_F s, Layer l) {
        wp::CommandButton(dc, s, l, L"", false);
        wp::ViewOptionsGlyph(dc, s, m_source ? m_source->GetViewStyle() : media::ViewStyle::Details);
    }, {})->onClick = [this] { CycleViewStyle(); };
    add(kViewOptionsArrow, [](auto* dc, D2D1_SIZE_F s, Layer l) { wp::CommandButton(dc, s, l, L"", true); }, {})
        ->onClick = [this] { ShowViewMenu(); };
    add(kNewPlaylist, [](auto* dc, D2D1_SIZE_F s, Layer l) { wp::CommandButton(dc, s, l, L"Create playlist", false); }, {})
        ->onClick = [this] { CreatePlaylist(false); };
    add(kNewPlaylistArrow, [](auto* dc, D2D1_SIZE_F s, Layer l) { wp::CommandButton(dc, s, l, L"", true); }, {})
        ->onClick = [this] {
              POINT pt{m_layout.newPlaylist.left, m_layout.newPlaylist.bottom};
              ClientToScreen(m_hwnd, &pt);
              ShowActionMenu(m_hwnd, pt, {{L"Create playlist", [this] { CreatePlaylist(false); }},
                                          {L"Create auto playlist", [this] { CreatePlaylist(true); }}});
          };

    // Seek bar (the band's top rule, as in WMP 12)
    add(kSeek, [this](auto* dc, D2D1_SIZE_F s, Layer l) {
        const float progress = m_seeking ? m_seekPreview
                                         : (m_length > 0 ? static_cast<float>(m_position / m_length) : 0.0f);
        wp::SeekBar(dc, s, progress, l == Layer::Hover || l == Layer::Pressed, m_length > 0,
                    (m_layout.transport.top - m_layout.seek.top) / m_scale);
    }, {});

    // Transport controls
    add(kShuffle, [this](auto* dc, D2D1_SIZE_F s, Layer l) {
        if (m_win7Art) wt::Toggle(dc, l, true, m_shuffle);
        else paint::IconButton(dc, s, l, TransportGlyph::Shuffle, true, m_shuffle);
    }, {})->onClick = [this] { SetShuffleRepeat(!m_shuffle, m_repeat); };
    // Repeat cycles off -> all -> one; "one" is the original icon with a small "1".
    add(kRepeat, [this](auto* dc, D2D1_SIZE_F s, Layer l) {
        const bool on = m_repeat != Repeat::Off;
        if (m_win7Art) wt::Toggle(dc, l, false, on);
        else paint::IconButton(dc, s, l, TransportGlyph::Repeat, true, on);
        if (m_repeat == Repeat::One) {
            auto t = MakeText(L"1", 8.0f, DWRITE_FONT_WEIGHT_BOLD, 8, 10);
            com_ptr<ID2D1SolidColorBrush> halo, ink;
            dc->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 0.9f), halo.put());
            dc->CreateSolidColorBrush(Hex(0x1C5DB2), ink.put());
            const D2D1_POINT_2F at{s.width - 8.5f, s.height - 12.0f};
            for (float dx = -0.75f; dx <= 0.75f; dx += 0.75f)
                for (float dy = -0.75f; dy <= 0.75f; dy += 0.75f) dc->DrawTextLayout({at.x + dx, at.y + dy}, t.get(), halo.get());
            dc->DrawTextLayout(at, t.get(), ink.get());
        }
    }, {})->onClick = [this] {
        SetShuffleRepeat(m_shuffle, m_repeat == Repeat::Off ? Repeat::All : m_repeat == Repeat::All ? Repeat::One : Repeat::Off);
    };
    add(kStop, [this](auto* dc, D2D1_SIZE_F s, Layer l) {
        if (m_win7Art) wt::ClusterButton(dc, ClusterRegion(m_layout.stop), wt::Button::Stop, l, m_playing);
        else paint::IconButton(dc, s, l, TransportGlyph::Stop, m_playing, false);
    }, {})->onClick = [] { playback_control::get()->stop(); };
    // Like WMP 12, stop / previous / next are disabled while nothing plays: no hover, no click.
    add(kPrev, [this](auto* dc, D2D1_SIZE_F s, Layer l) {
        if (m_win7Art) wt::ClusterButton(dc, ClusterRegion(m_layout.prev), wt::Button::Prev, l, m_playing);
        else paint::Segment(dc, s, true, l, TransportGlyph::Previous, true);
    }, {})->onClick = [] { playback_control::get()->previous(); };
    add(kNext, [this](auto* dc, D2D1_SIZE_F s, Layer l) {
        if (m_win7Art) wt::ClusterButton(dc, ClusterRegion(m_layout.next), wt::Button::Next, l, m_playing);
        else paint::Segment(dc, s, false, l, TransportGlyph::Next, true);
    }, {})->onClick = [] { playback_control::get()->next(); };
    for (int id : {kStop, kPrev, kNext})
        WidgetById(id)->enabled = [this] { return m_playing; };

    Widget::Options orb;
    orb.circular = true;
    if (m_win7Art) {
        // WMP 12: the orb brightens and a glow ring fades in around it (no pulsing).
        orb.haloFactor = wt::kGlowFactor;
        orb.haloPulse = false;
        orb.halo = [](ID2D1DeviceContext* dc, float size) { wt::GlowRing(dc, size); };
    } else {
        orb.haloFactor = 1.6f;
        orb.halo = [](ID2D1DeviceContext* dc, float size) { paint::OrbHalo(dc, size); };
    }
    add(kPlay, [this](auto* dc, D2D1_SIZE_F s, Layer l) {
        const bool showPause = m_playing && !m_paused;
        if (m_win7Art) wt::Orb(dc, l, showPause);
        else paint::PlayOrb(dc, s.width, l, showPause ? TransportGlyph::Pause : TransportGlyph::Play);
    }, orb)->onClick = [this] {
        auto pc = playback_control::get();
        if (pc->is_playing()) {
            pc->play_or_pause();
        } else if (m_list && m_source && m_source->Focus() != SIZE_MAX && m_source->Focus() < m_source->Count()) {
            m_source->Activate(m_source->Focus());  // like WMP: play what is selected
        } else {
            pc->start();
        }
    };

    add(kMute, [this](auto* dc, D2D1_SIZE_F s, Layer l) {
        if (m_win7Art) wt::Mute(dc, l, m_muted, m_volume);
        else paint::IconButton(dc, s, l, m_muted ? TransportGlyph::Mute : TransportGlyph::Volume, true, false);
    }, {})->onClick = [] { playback_control::get()->volume_mute_toggle(); };
    add(kVolume, [this](auto* dc, D2D1_SIZE_F s, Layer l) {
        if (m_win7Art) wt::Volume(dc, l, m_volume);
        else paint::VolumeSlider(dc, s, m_volume, l);
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
    // Light + dark client edge lines, one device pixel each (DWM frame bitmap, 1:1).
    L.clientEdge = {L.client.left - 2, L.client.top - 2, L.client.right + 2, L.client.bottom + 2};
    L.navBar = {L.client.left, L.client.top, L.client.right, L.client.top + PxI(NavBarHeight())};
    L.cmdBar = {L.client.left, L.navBar.bottom, L.client.right, L.navBar.bottom + PxI(m::kCommandBarHeight)};
    L.transport = {L.client.left, L.client.bottom - PxI(TransportHeight()), L.client.right, L.client.bottom};
    L.content = {L.client.left, L.cmdBar.bottom, L.client.right, L.transport.top};

    // Caption buttons hang from the top edge (flush when maximized).
    // With the DWM artwork: Win7 sizes measured at 150 % (43 / 41 / 73 x 29 px), hanging one
    // device pixel below the frame's outer line.
    const bool dwm = m_aeroArt;
    const int capRight = W - (max ? PxI(2) : PxI(dwm ? 20.0f / 3 : m::kCapBtnRightInset));
    const int capTop = max ? 0 : (dwm ? line : PxI(m::kCapBtnTopInset));
    const int bh = PxI(dwm ? 29.0f / 1.5f : m::kCapBtnH);
    L.capClose = {capRight - PxI(dwm ? 73.0f / 1.5f : m::kCapBtnCloseW), capTop, capRight, capTop + bh};
    L.capMax = {L.capClose.left - PxI(dwm ? 41.0f / 1.5f : m::kCapBtnMaxW), capTop, L.capClose.left, capTop + bh};
    L.capMin = {L.capMax.left - PxI(dwm ? 43.0f / 1.5f : m::kCapBtnMinW), capTop, L.capMax.left, capTop + bh};

    // Icon and caption text (the title visual carries a glow margin on every side)
    const int cy = max ? capH / 2 : (capH + PxI(2)) / 2;
    const int iconSz = PxI(m::kIconSize);
    const int iconLeft = L.border + PxI(max ? 4.0f : 1.0f);
    const int pad = PxI(paint::kGlowPadding);
    L.icon = {iconLeft, cy - iconSz / 2, iconLeft + iconSz, cy - iconSz / 2 + iconSz};
    L.title = {iconLeft - pad, cy - PxI(12) - pad, std::max<int>(iconLeft, L.capMin.left - PxI(4)),
               cy + PxI(12) + pad};

    // Navigation bar: back / forward orbs and the breadcrumb
    const int nbcy = (L.navBar.top + L.navBar.bottom) / 2;
    if (m_win7Art) {
        // WMP 12 orbs in their recess, 8 DIP from the client edge, scaled to the command bar's
        // button height; the orbs sit one bitmap pixel inside the recess.
        const float k = wt::kNavOrbShown / wt::kNavOrb;
        const int gw = PxI(wt::kNavGrooveSize.width * k), gh = PxI(wt::kNavGrooveSize.height * k);
        const int gx = L.client.left + PxI(8), gy = nbcy - gh / 2;
        L.navGroove = {gx, gy, gx + gw, gy + gh};
        const int orb = PxI(wt::kNavOrbShown), inset = PxI(k);
        L.back = {gx + inset, gy + inset, gx + inset + orb, gy + inset + orb};
        L.forward = {L.back.right, L.back.top, L.back.right + orb, L.back.bottom};
        L.breadcrumb = {L.navGroove.right + PxI(8), L.navBar.top + PxI(3), L.client.right - PxI(8), L.navBar.bottom - line};
    } else {
        const int orb = PxI(24);
        L.back = CenteredRect(L.client.left + PxI(18), nbcy, orb, orb);
        L.forward = CenteredRect(L.client.left + PxI(44), nbcy, orb, orb);
        L.breadcrumb = {L.client.left + PxI(64), L.navBar.top, L.client.right - PxI(8), L.navBar.bottom - line};
    }

    // Command bar
    const int cbcy = (L.cmdBar.top + L.cmdBar.bottom) / 2 - line;
    const int bh2 = PxI(23);
    int x = L.client.left + PxI(6);
    auto button = [&](float wDip) {
        RECT r{x, cbcy - bh2 / 2, x + PxI(wDip), cbcy - bh2 / 2 + bh2};
        x = r.right + PxI(2);
        return r;
    };
    L.organize = button(82);
    L.addFiles = button(70);
    L.addFolder = button(76);
    // Split buttons (WMP 12): the main part acts at once, the arrow part opens a menu.
    L.newPlaylist = button(86);
    x -= PxI(2);
    L.newPlaylistArrow = button(16);
    L.search = {L.client.right - PxI(8) - PxI(230), cbcy - PxI(11), L.client.right - PxI(8), cbcy + PxI(11)};
    // View options (WMP: icon / expanded tile / details) left of the search box, library only.
    if (m_source && ViewStyles() > 1) {
        const int right = L.search.left - PxI(8);
        L.viewOptionsArrow = {right - PxI(14), cbcy - PxI(11), right, cbcy + PxI(11)};
        L.viewOptions = {L.viewOptionsArrow.left - PxI(26), cbcy - PxI(11), L.viewOptionsArrow.left, cbcy + PxI(11)};
    } else {
        L.viewOptions = L.viewOptionsArrow = {};
    }

    // Content: navigation pane + list
    L.navPane = {L.content.left, L.content.top, L.content.left + PxI(kNavPaneWidth), L.content.bottom};
    L.list = {L.navPane.right, L.content.top, L.content.right, L.content.bottom};

    // Transport band: seek rule on top, capsule centered on the orb below it.
    // WMP 12: the seek bar runs the full client width right under the band's top rule;
    // the widget is taller than the bar for the hover thumb and an easier grab.
    L.seek = {L.client.left, L.transport.top - PxI(6), L.client.right, L.transport.top + PxI(9)};
    const int tcx = (L.transport.left + L.transport.right) / 2;
    if (m_win7Art) {
        // Original bitmaps, placed relative to the cluster as in WMP 12 (orb center = band center).
        L.cluster = {tcx - PxI(108), L.transport.top + PxI(wt::kClusterTop)};
        L.capsule = ClusterRect(wt::kCapsuleRect);
        L.play = ClusterRect(wt::kOrbRect);
        L.prev = ClusterRect(wt::kPrevRect);
        L.next = ClusterRect(wt::kNextRect);
        L.stop = ClusterRect(wt::kStopRect);
        L.shuffle = ClusterRect(wt::kShuffleRect);
        L.repeat = ClusterRect(wt::kRepeatRect);
        L.mute = ClusterRect(wt::kMuteRect);
        L.volume = ClusterRect(wt::kVolumeRect);
        L.divider = {};
    } else {
        const int tcy = (L.transport.top + PxI(6) + L.transport.bottom) / 2;
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
    }
    const int capsuleLeft = m_win7Art ? ClusterRect(wt::kCapsuleVisible).left : L.capsule.left;
    L.nowPlaying = {L.client.left + PxI(10), L.transport.top + PxI(7),
                    std::max<int>(L.client.left + PxI(10), capsuleLeft - PxI(12)), L.transport.bottom - PxI(3)};

    // Apply to the visual tree.
    const float radius = max ? 0.0f : Px(m::kCornerRadius);
    m_clipGeometry.Size(float2{static_cast<float>(W), static_cast<float>(H)});
    m_clipGeometry.CornerRadius(float2{radius, radius});
    m_frameOutline.IsVisible(!max);

    // DWM's side highlights run down the left / right frame from the top and end sharply at
    // H / 4 + 20 DIP (fitted to Win7 captures: 110 px on a 360 px window at 96 DPI, 290 px on
    // a 1050 px window at 144 DPI).
    {
        const bool show = m_aeroArt && !max && L.clientEdge.left > 0;
        const SIZE size{L.clientEdge.left, H / 4 + PxI(20)};
        for (auto& v : m_sideHighlight) v.IsVisible(show);
        if (show) {
            Place(m_sideHighlight[0], {0, 0, size.cx, size.cy});
            Place(m_sideHighlight[1], {W - size.cx, 0, W, size.cy});
            if (size.cx != m_sideHighlightSize.cx || size.cy != m_sideHighlightSize.cy) {
                m_sideHighlightSize = size;
                PaintSideHighlights();
            }
        }
    }

    Place(m_navGroove, L.navGroove);
    if (const SIZE gs{L.navGroove.right - L.navGroove.left, L.navGroove.bottom - L.navGroove.top};
        m_win7Art && gs.cx > 0 && (gs.cx != m_navGrooveSize.cx || gs.cy != m_navGrooveSize.cy)) {
        m_navGrooveSize = gs;
        gfx::CompositionHost::Get().Repaint(m_navGroove, gs.cx, gs.cy, m_scale, [&](ID2D1DeviceContext* dc) {
            wt::NavGroove(dc, D2D1::SizeF(gs.cx / m_scale, gs.cy / m_scale));
        });
    }
    Place(m_clientEdge, L.clientEdge);
    Place(m_clientGlow, L.client);
    Place(m_navBar, L.navBar);
    Place(m_cmdBar, L.cmdBar);
    Place(m_contentTop, {L.content.left, L.content.top, L.content.right, L.content.top + PxI(14)});
    Place(m_libraryBg, L.list);
    Place(m_transportBand, L.transport);
    Place(m_capsule, L.capsule);
    Place(m_cluster, ClusterRect(wt::kClusterRect));
    Place(m_divider, L.divider);
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
    {
        RECT t = L.breadcrumb;
        t.right = std::min<LONG>(t.right, t.left + PxI(900));
        Place(m_breadcrumb, t);
    }
    m_nav->SetBounds(L.navPane, m_scale);
    m_list->SetBounds(L.list, m_scale);
    m_search->SetBounds(L.search, m_scale);

    for (auto& w : m_widgets) {
        const RECT* r = nullptr;
        switch (w->Id()) {
        case kCapMin: r = &L.capMin; break;
        case kCapMax: r = &L.capMax; break;
        case kCapClose: r = &L.capClose; break;
        case kBack: r = &L.back; break;
        case kForward: r = &L.forward; break;
        case kOrganize: r = &L.organize; break;
        case kAddFiles: r = &L.addFiles; break;
        case kAddFolder: r = &L.addFolder; break;
        case kNewPlaylist: r = &L.newPlaylist; break;
        case kViewOptions: r = &L.viewOptions; break;
        case kViewOptionsArrow: r = &L.viewOptionsArrow; break;
        case kNewPlaylistArrow: r = &L.newPlaylistArrow; break;
        case kSeek: r = &L.seek; break;
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
    m_glass->SetScale(s);
    PaintSideHighlights();
    m_navGrooveSize = {};  // repainted at the new scale by the next layout
    const int line = LinePx();

    // Frame lines are one device pixel at any DPI (as DWM draws them); the corner scales with DPI.
    const int frameSrc = PxI(32);
    m_frameOutline.Brush(NineGrid(frameSrc, frameSrc, s, static_cast<float>(PxI(m::kCornerRadius + 2)), true, [&](ID2D1DeviceContext* dc) {
        paint::FrameOutline(dc, D2D1::SizeF(frameSrc / s, frameSrc / s), m::kCornerRadius);
    }));
    const int edgeSrc = PxI(16);
    m_clientEdge.Brush(NineGrid(edgeSrc, edgeSrc, s, 3.0f, false, [&](ID2D1DeviceContext* dc) {
        paint::ClientEdge(dc, D2D1::SizeF(edgeSrc / s, edgeSrc / s));
    }));
    m_clientGlow.Brush(NineGrid(edgeSrc, edgeSrc, s, 4.0f, true, [&](ID2D1DeviceContext* dc) {
        paint::ClientGlow(dc, D2D1::SizeF(edgeSrc / s, edgeSrc / s));
    }));

    // WMP 12 client chrome (measured; docs/design/visual-spec.md)
    const float lineDip = line / s;
    m_navBar.Brush(Strip(PxI(NavBarHeight()), s, [&](ID2D1DeviceContext* dc, float h) {
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
    m_transportBand.Brush(Strip(PxI(TransportHeight()), s, [&](ID2D1DeviceContext* dc, float h) {
        if (m_win7Art) {
            // Native WMP 12 capture, 96 DPI: dark top rule, then E9F1F9 fading to DFEAF7.
            dc->FillRectangle({-1, 0, 100, h}, VGrad(dc, 0, h, {{0.0f, 0xEBF2FA}, {0.1f, 0xE9F1F9},
                                                                 {0.4f, 0xE3EDF8}, {1.0f, 0xDFEAF7}})
                                                    .get());
            FillLine(dc, 0, lineDip, 0x9DA1A6);
            return;
        }
        dc->FillRectangle({-1, 0, 100, h}, VGrad(dc, 0, h, {{0.0f, 0xEBF2FB}, {1.0f, 0xE3ECF7}}).get());
        FillLine(dc, 0, lineDip, 0x99A1AA);
        FillLine(dc, lineDip, lineDip, 0xF6FAFE);
    }));
    if (m_win7Art) {
        auto& host = gfx::CompositionHost::Get();
        const RECT cap = ClusterRect(wt::kCapsuleRect), cl = ClusterRect(wt::kClusterRect);
        host.Repaint(m_capsule, cap.right - cap.left, cap.bottom - cap.top, s, [](ID2D1DeviceContext* dc) { wt::Capsule(dc); });
        host.Repaint(m_cluster, cl.right - cl.left, cl.bottom - cl.top, s,
                     [](ID2D1DeviceContext* dc) { wt::ClusterBackground(dc); });
    } else {
        const int capH = PxI(m::kCapsuleHeight);
        m_capsule.Brush(NineGrid(2 * capH, capH, s, static_cast<float>(capH / 2), false, [&](ID2D1DeviceContext* dc) {
            paint::TransportCapsule(dc, D2D1::SizeF(2 * capH / s, capH / s));
        }));
    }
    {
        auto& host = gfx::CompositionHost::Get();
        const int w = PxI(4), h = PxI(16);
        host.Repaint(m_divider, w, h, s,
                     [&](ID2D1DeviceContext* dc) { paint::CapsuleDivider(dc, D2D1::SizeF(w / s, h / s)); });
    }
    m_icon = LoadAppIcon(PxI(m::kIconSize));
    // Window icons for the taskbar, Alt+Tab and the system menu, at this monitor's sizes.
    const UINT dpi = static_cast<UINT>(std::lround(m_scale * 96));
    for (auto [which, metric] : {std::pair{ICON_SMALL, SM_CXSMICON}, std::pair{ICON_BIG, SM_CXICON}}) {
        HICON icon = CreateAppIcon(GetSystemMetricsForDpi(metric, dpi));
        if (HICON old = reinterpret_cast<HICON>(SendMessageW(m_hwnd, WM_SETICON, which, reinterpret_cast<LPARAM>(icon))))
            if (old != icon) DestroyIcon(old);
    }
    RepaintTitle();
    RepaintNowPlaying();
    RepaintBreadcrumb();
}

void MainWindow::RepaintTitle() {
    auto& host = gfx::CompositionHost::Get();
    const float pad = paint::kGlowPadding;
    const int w = PxI(600 + 2 * pad), h = PxI(24 + 2 * pad);
    host.Repaint(m_title, w, h, m_scale, [&](ID2D1DeviceContext* dc) {
        const float iconSz = m::kIconSize;
        paint::Icon(dc, m_icon.get(), D2D1::RectF(pad, pad + 12 - iconSz / 2, pad + iconSz, pad + 12 + iconSz / 2));
        auto layout = MakeText(kAppName, m::kTitleFontSize, DWRITE_FONT_WEIGHT_NORMAL, 560, 24);
        paint::GlowText(dc, layout.get(), D2D1::Point2F(pad + iconSz + 6, pad), D2D1::ColorF(0, 0, 0, 1));
    });
    UpdateLayout();  // Repaint resets the visual to the surface size; re-apply the layout clip.
}

void MainWindow::RepaintNowPlaying() {
    auto& host = gfx::CompositionHost::Get();
    const int w = PxI(360), h = PxI(TransportHeight() - 10);
    host.Repaint(m_nowPlaying, w, h, m_scale, [&](ID2D1DeviceContext* dc) {
        if (m_trackTitle.empty()) return;
        const float H = h / m_scale;
        float x = 0;
        if (m_art) {
            com_ptr<ID2D1Bitmap> bmp;
            if (SUCCEEDED(dc->CreateBitmapFromWicBitmap(m_art.get(), nullptr, bmp.put()))) {
                const D2D1_SIZE_F sz = bmp->GetSize();
                const float side = H - 2;
                const float k = side / std::max(sz.width, sz.height);
                const D2D1_RECT_F dst{2, 1 + (side - sz.height * k) / 2, 2 + sz.width * k, 1 + (side + sz.height * k) / 2};
                // Same edge as the covers in the library (soft shadow, one-pixel 18 % black line).
                const float m = (dst.right - dst.left) * 8.0f / 184.0f;
                win7::DrawStretched(dc, win7::Module::Wmploc, L"ALBUMDROPSHADOW_200X200.PNG",
                                    {dst.left - m + 1, dst.top - m + 2, dst.right + m + 1, dst.bottom + m + 2}, 0.45f);
                dc->DrawBitmap(bmp.get(), &dst, 1.0f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, nullptr, nullptr);
                com_ptr<ID2D1SolidColorBrush> frame;
                dc->CreateSolidColorBrush(D2D1::ColorF(0, 0.18f), frame.put());
                const float px = 1.0f / m_scale;
                dc->DrawRectangle({dst.left + px / 2, dst.top + px / 2, dst.right - px / 2, dst.bottom - px / 2}, frame.get(), px);
                x = dst.right + 8;
            }
        }
        com_ptr<ID2D1SolidColorBrush> dark, grey;
        dc->CreateSolidColorBrush(Hex(0x1E1E1E), dark.put());
        dc->CreateSolidColorBrush(Hex(0x5B5B5B), grey.put());
        auto t = MakeText(m_trackTitle, 12.0f, DWRITE_FONT_WEIGHT_NORMAL, 350 - x, 17);
        dc->DrawTextLayout({x, 0}, t.get(), dark.get());
        std::wstring second = FormatTime(m_position);
        if (m_length > 0) second += L" / " + FormatTime(m_length);
        if (!m_trackArtist.empty()) second = m_trackArtist + L"   " + second;
        auto a = MakeText(second, 12.0f, DWRITE_FONT_WEIGHT_NORMAL, 350 - x, 17);
        dc->DrawTextLayout({x, 16}, a.get(), grey.get());
    });
    UpdateLayout();
}

void MainWindow::RepaintBreadcrumb() {
    BuildCrumbs();
    PaintBreadcrumb();
    UpdateLayout();
}

void MainWindow::BuildCrumbs() {
    auto go = [this](View v) { return [this, v] { Navigate(v, true); }; };
    View all;
    all.kind = View::Kind::AllMusic;
    auto kindView = [](View::Kind k) {
        View v;
        v.kind = k;
        return v;
    };
    auto playlists = [this, go] {
        std::vector<MenuAction> out;
        auto pm = playlist_manager::get();
        for (t_size i = 0, n = pm->get_playlist_count(); i < n; ++i) {
            pfc::string8 name;
            pm->playlist_get_name(i, name);
            View v;
            v.kind = View::Kind::Playlist;
            v.playlist = i;
            out.push_back({Wide(name), go(v)});
        }
        return out;
    };
    auto groupsOf = [this, go](media::GroupKind kind) {
        return [this, go, kind] {
            std::vector<MenuAction> out;
            for (const auto& g : m_library.Groups(kind)) {
                if (out.size() >= 300) break;  // a menu, not a list view
                View v;
                v.kind = View::Kind::Group;
                v.group = kind;
                v.key = g.key;
                v.name = g.name;
                out.push_back({g.name.empty() ? L"Unknown" : g.name, go(v)});
            }
            return out;
        };
    };
    auto music = [go, kindView, all] {
        return std::vector<MenuAction>{{L"Artist", go(kindView(View::Kind::Artists))},
                                       {L"Album", go(kindView(View::Kind::Albums))},
                                       {L"All music", go(all)},
                                       {L"Genre", go(kindView(View::Kind::Genres))}};
    };
    View nowPlaying;
    nowPlaying.kind = View::Kind::Playlist;
    nowPlaying.playlist = playlist_manager::get()->get_active_playlist();

    m_crumbs.clear();
    m_crumbs.push_back({L"", {}, [go, all] { return std::vector<MenuAction>{{L"Library", go(all)}}; }});
    m_crumbs.push_back({L"Library", go(all), [go, all, nowPlaying] {
                            return std::vector<MenuAction>{{L"Playlists", go(nowPlaying)}, {L"Music", go(all)}};
                        }});
    if (m_view.kind == View::Kind::Playlist) {
        m_crumbs.push_back({L"Playlists", go(nowPlaying), playlists});
        m_crumbs.push_back({m_view.name, go(m_view), {}});
    } else {
        m_crumbs.push_back({L"Music", go(all), music});
        const media::GroupKind kind = m_view.kind == View::Kind::Artists  ? media::GroupKind::Artist
                                      : m_view.kind == View::Kind::Albums ? media::GroupKind::Album
                                      : m_view.kind == View::Kind::Genres ? media::GroupKind::Genre
                                                                          : m_view.group;
        if (m_view.kind == View::Kind::AllMusic) {
            m_crumbs.push_back({L"All music", go(all), {}});
        } else {
            const View::Kind kv = kind == media::GroupKind::Artist  ? View::Kind::Artists
                                  : kind == media::GroupKind::Album ? View::Kind::Albums
                                                                    : View::Kind::Genres;
            const wchar_t* label = kind == media::GroupKind::Artist  ? L"Artist"
                                   : kind == media::GroupKind::Album ? L"Album"
                                                                     : L"Genre";
            m_crumbs.push_back({label, go(kindView(kv)), groupsOf(kind)});
            if (m_view.kind == View::Kind::Group) m_crumbs.push_back({m_view.name, go(m_view), {}});
        }
    }

    // Lay out: name with 6 DIP padding, arrow 15 DIP; the root crumb is an arrow only.
    float x = 0;
    for (auto& c : m_crumbs) {
        c.x0 = x;
        if (!c.text.empty()) {
            auto t = MakeText(c.text, 12.0f, DWRITE_FONT_WEIGHT_NORMAL, 400, 20);
            DWRITE_TEXT_METRICS tm{};
            t->GetMetrics(&tm);
            x += std::ceil(tm.widthIncludingTrailingWhitespace) + 12;
        }
        c.split = x;
        if (c.children) x += 15;
        c.x1 = x;
    }
    if (m_crumbHot >= static_cast<int>(m_crumbs.size())) m_crumbHot = -1;
}

void MainWindow::PaintBreadcrumb() {
    auto& host = gfx::CompositionHost::Get();
    const int w = PxI(900), h = PxI(NavBarHeight() - 1);
    host.Repaint(m_breadcrumb, w, h, m_scale, [&](ID2D1DeviceContext* dc) {
        const float H = h / m_scale, top = 1, bottom = std::min(H - 2, 23.0f);
        com_ptr<ID2D1SolidColorBrush> ink, divider;
        dc->CreateSolidColorBrush(Hex(0x1E395B), ink.put());
        dc->CreateSolidColorBrush(Hex(0x8DB2E3, 0.8f), divider.put());
        for (int i = 0; i < static_cast<int>(m_crumbs.size()); ++i) {
            const Crumb& c = m_crumbs[i];
            const bool hot = i == m_crumbHot, open = i == m_crumbOpen;
            // Hover lights the whole crumb. WMP 12 art (WMP_BREADCRUMB_MAINBUTTON / _SPLITBUTTON,
            // states empty / hot / pressed / grey): the part under the mouse is hot, the
            // other half grey, the arrow pressed while its menu is open.
            const bool crumbArt = win7::Resources::Get().Has(win7::Module::Wmploc, L"WMP_BREADCRUMB_MAINBUTTON.PNG");
            if ((hot || open) && crumbArt) {
                const int nameState = open || m_crumbHotPart == 1 ? 3 : 1;
                const int arrowState = open ? 2 : m_crumbHotPart == 1 ? 1 : 3;
                if (!c.text.empty())
                    win7::DrawNineGrid(dc, win7::Module::Wmploc, L"WMP_BREADCRUMB_MAINBUTTON.PNG",
                                       {16.0f * nameState, 0, 16.0f * nameState + 16, 22}, {3, 3, 3, 3},
                                       {c.x0, top, c.split, bottom});
                if (c.children)
                    win7::DrawNineGrid(dc, win7::Module::Wmploc, L"WMP_BREADCRUMB_SPLITBUTTON.PNG",
                                       {10.0f * arrowState, 0, 10.0f * arrowState + 10, 22}, {3, 3, 3, 3},
                                       {c.split, top, c.x1, bottom});
            } else if (hot || open) {
                wp::RowPlate(dc, {c.x0, top, c.x1, bottom}, wp::RowState::Hot, true);
                if (open || m_crumbHotPart == 1) {
                    if (c.children && (open || hot))
                        wp::RowPlate(dc, {c.split, top, c.x1, bottom}, wp::RowState::Selected, true);
                } else if (!c.text.empty()) {
                    wp::RowPlate(dc, {c.x0, top, c.split, bottom}, wp::RowState::SelectedHot, true);
                }
                if (!c.text.empty() && c.children) {
                    const float px = 1.0f / m_scale;
                    dc->FillRectangle({c.split, top + 1, c.split + px, bottom - 1}, divider.get());
                }
            }
            if (!c.text.empty()) {
                auto t = MakeText(c.text, 12.0f, DWRITE_FONT_WEIGHT_NORMAL, c.split - c.x0, H);
                dc->DrawTextLayout({c.x0 + 6, 0}, t.get(), ink.get());
            }
            if (c.children) {  // small black triangle: right, or down while the menu is open
                const float cx = std::round(((c.split + c.x1) / 2) * m_scale) / m_scale;
                const float cy = std::round((H / 2) * m_scale) / m_scale;
                // WMP 12's 9x9 arrows: 5235 right, 5237 down.
                if (win7::Draw(dc, win7::Module::Wmploc, open ? 5237 : 5235,
                               {std::round(cx - 4.5f), std::round(cy - 4.5f)}))
                    continue;
                com_ptr<ID2D1Factory> f;
                dc->GetFactory(f.put());
                com_ptr<ID2D1PathGeometry> tri;
                com_ptr<ID2D1GeometrySink> sink;
                f->CreatePathGeometry(tri.put());
                tri->Open(sink.put());
                if (open) {
                    sink->BeginFigure({cx - 3.5f, cy - 1.5f}, D2D1_FIGURE_BEGIN_FILLED);
                    sink->AddLine({cx + 3.5f, cy - 1.5f});
                    sink->AddLine({cx, cy + 2.5f});
                } else {
                    sink->BeginFigure({cx - 1.5f, cy - 3.5f}, D2D1_FIGURE_BEGIN_FILLED);
                    sink->AddLine({cx + 2.5f, cy});
                    sink->AddLine({cx - 1.5f, cy + 3.5f});
                }
                sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                sink->Close();
                com_ptr<ID2D1SolidColorBrush> arrow;
                dc->CreateSolidColorBrush(Hex(0x1A1A1A), arrow.put());
                dc->FillGeometry(tri.get(), arrow.get());
            }
        }
    });
}

bool MainWindow::CrumbAt(POINT pt, int* index, int* part) const {
    if (!PtInRect(&m_layout.breadcrumb, pt)) return false;
    const float x = (pt.x - m_layout.breadcrumb.left) / m_scale;
    for (int i = 0; i < static_cast<int>(m_crumbs.size()); ++i) {
        const Crumb& c = m_crumbs[i];
        if (x >= c.x0 && x < c.x1) {
            *index = i;
            *part = x >= c.split ? 1 : 0;
            return true;
        }
    }
    return false;
}

void MainWindow::UpdateCrumbHover(POINT pt) {
    int index = -1, part = 0;
    if (!CrumbAt(pt, &index, &part)) index = -1;
    if (index == m_crumbHot && (index < 0 || part == m_crumbHotPart)) return;
    m_crumbHot = index;
    m_crumbHotPart = part;
    PaintBreadcrumb();
}

void MainWindow::OnCrumbClick(POINT pt) {
    int index = -1, part = 0;
    if (!CrumbAt(pt, &index, &part)) return;
    const Crumb c = m_crumbs[index];  // navigation rebuilds m_crumbs
    if (part == 0 && c.open) {
        c.open();
        return;
    }
    if (!c.children) return;
    m_crumbOpen = index;
    PaintBreadcrumb();
    POINT screen{m_layout.breadcrumb.left + static_cast<LONG>(std::lround(c.split * m_scale)),
                 m_layout.breadcrumb.top + PxI(23)};
    ClientToScreen(m_hwnd, &screen);
    const auto items = c.children();
    ShowActionMenu(m_hwnd, screen, items);  // runs the chosen action
    m_crumbOpen = -1;
    m_crumbHot = -1;
    PaintBreadcrumb();
}

// ---- Navigation --------------------------------------------------------------------------

void MainWindow::Navigate(const View& requested, bool push) {
    View view = requested;
    auto pm = playlist_manager::get();
    std::unique_ptr<media::ListSource> source;

    if (view.kind == View::Kind::Playlist && view.playlist >= pm->get_playlist_count()) view.kind = View::Kind::AllMusic;
    switch (view.kind) {
    case View::Kind::Playlist: {
        pfc::string8 name;
        pm->playlist_get_name(view.playlist, name);
        view.name = Wide(name);
        if (pm->get_active_playlist() != view.playlist) pm->set_active_playlist(view.playlist);
        source = std::make_unique<media::PlaylistSource>(view.playlist);
        m_nav->Select(NavPane::Target::Playlist, view.playlist);
        break;
    }
    case View::Kind::AllMusic: {
        std::vector<size_t> all(m_library.Tracks().size());
        for (size_t i = 0; i < all.size(); ++i) all[i] = i;
        source = std::make_unique<media::TrackSource>(
            m_library, std::move(all),
            L"Your library is empty. Use Organize ▸ Manage music library to add your music folders.");
        m_nav->Select(NavPane::Target::Music, 0);
        break;
    }
    case View::Kind::Artists:
    case View::Kind::Albums:
    case View::Kind::Genres: {
        const auto kind = view.kind == View::Kind::Artists  ? media::GroupKind::Artist
                          : view.kind == View::Kind::Albums ? media::GroupKind::Album
                                                            : media::GroupKind::Genre;
        source = std::make_unique<media::GroupSource>(m_library, kind, [this, kind](const media::Group& g) {
            View v;
            v.kind = View::Kind::Group;
            v.group = kind;
            v.key = g.key;
            v.name = g.secondary.empty() ? g.name : g.name + L" (" + g.secondary + L")";
            Navigate(v, true);
        });
        m_nav->Select(view.kind == View::Kind::Artists  ? NavPane::Target::Artists
                      : view.kind == View::Kind::Albums ? NavPane::Target::Albums
                                                        : NavPane::Target::Genres,
                      0);
        break;
    }
    case View::Kind::Group: {
        const media::Group* g = m_library.FindGroup(view.group, view.key);
        source = std::make_unique<media::TrackSource>(m_library, g ? g->tracks : std::vector<size_t>{},
                                                      L"No songs.");
        m_nav->Select(view.group == media::GroupKind::Artist  ? NavPane::Target::Artists
                      : view.group == media::GroupKind::Album ? NavPane::Target::Albums
                                                              : NavPane::Target::Genres,
                      0);
        break;
    }
    }

    const bool sameView = view.kind == m_view.kind && view.playlist == m_view.playlist && view.key == m_view.key &&
                          view.group == m_view.group;
    if (push && !sameView) {
        // A new view clears the search, like WMP.
        m_filter.clear();
        if (m_search) {
            auto keep = std::move(m_search->onChange);
            m_search->Clear();
            m_search->onChange = std::move(keep);
        }
    }
    if (auto* style = ViewStyleSetting(static_cast<int>(view.kind)); style && source->SupportsViewStyle(static_cast<media::ViewStyle>(style->get())))
        source->SetViewStyle(static_cast<media::ViewStyle>(style->get()));
    if (!m_filter.empty()) source->SetFilter(m_filter);
    m_source = std::move(source);
    m_list->SetSource(m_source.get(), !sameView);
    m_view = view;

    if (push && !sameView) {
        if (!m_history.empty()) m_history.resize(m_historyPos + 1);
        m_history.push_back(view);
        m_historyPos = m_history.size() - 1;
    } else if (!m_history.empty()) {
        m_history[m_historyPos] = view;
    } else {
        m_history.push_back(view);
        m_historyPos = 0;
    }
    for (int id : {kBack, kForward, kViewOptions})
        if (auto* w = WidgetById(id)) w->Invalidate();
    RepaintBreadcrumb();  // also re-applies the layout (view options button)
}

cfg_var_modern::cfg_int* MainWindow::ViewStyleSetting(int kind) const {
    switch (static_cast<View::Kind>(kind)) {
    case View::Kind::AllMusic:
    case View::Kind::Group: return &cfg_viewSongs;
    case View::Kind::Albums: return &cfg_viewAlbums;
    case View::Kind::Artists: return &cfg_viewArtists;
    case View::Kind::Genres: return &cfg_viewGenres;
    default: return nullptr;
    }
}

int MainWindow::ViewStyles() const {
    int n = 0;
    for (auto s : {media::ViewStyle::Icons, media::ViewStyle::Tiles, media::ViewStyle::Details})
        if (m_source && m_source->SupportsViewStyle(s)) ++n;
    return n;
}

void MainWindow::ShowViewMenu() {
    if (!m_source) return;
    HMENU menu = CreatePopupMenu();
    const std::pair<media::ViewStyle, const wchar_t*> items[] = {
        {media::ViewStyle::Icons, L"&Icon"}, {media::ViewStyle::Tiles, L"&Expanded tile"},
        {media::ViewStyle::Details, L"&Details"}};
    for (const auto& [style, label] : items) {
        UINT flags = MF_STRING;
        if (!m_source->SupportsViewStyle(style)) flags |= MF_GRAYED;
        AppendMenuW(menu, flags, static_cast<UINT_PTR>(style) + 1, label);
    }
    const UINT current = static_cast<UINT>(m_source->GetViewStyle()) + 1;
    CheckMenuRadioItem(menu, 1, 3, current, MF_BYCOMMAND);
    POINT pt{m_layout.viewOptions.left, m_layout.viewOptions.bottom};
    ClientToScreen(m_hwnd, &pt);
    const UINT cmd = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_LEFTALIGN | TPM_TOPALIGN, pt.x, pt.y,
                                      m_hwnd, nullptr);
    DestroyMenu(menu);
    if (cmd == 0 || cmd == current) return;
    ApplyViewStyle(static_cast<media::ViewStyle>(cmd - 1));
}

void MainWindow::ApplyViewStyle(media::ViewStyle style) {
    m_source->SetViewStyle(style);
    if (auto* setting = ViewStyleSetting(static_cast<int>(m_view.kind))) setting->set(static_cast<int>(style));
    m_list->SetSource(m_source.get(), true);
    if (auto* w = WidgetById(kViewOptions)) w->Invalidate();
}

void MainWindow::CycleViewStyle() {
    // WMP 12: clicking the view icon steps Icon -> Expanded tile -> Details -> Icon,
    // skipping styles the current view does not offer.
    if (!m_source) return;
    int s = static_cast<int>(m_source->GetViewStyle());
    for (int i = 0; i < 3; ++i) {
        s = (s + 1) % 3;
        if (m_source->SupportsViewStyle(static_cast<media::ViewStyle>(s))) {
            ApplyViewStyle(static_cast<media::ViewStyle>(s));
            return;
        }
    }
}

void MainWindow::CreatePlaylist(bool autoPlaylist) {
    auto pm = playlist_manager::get();
    const t_size p = pm->create_playlist(autoPlaylist ? "New Auto Playlist" : "New Playlist", SIZE_MAX, SIZE_MAX);
    if (p == SIZE_MAX) return;
    pm->set_active_playlist(p);
    View v;
    v.kind = View::Kind::Playlist;
    v.playlist = p;
    Navigate(v, true);
    if (!autoPlaylist) return;
    // foobar2000 auto playlist: starts as the whole library, its own dialog edits the query.
    try {
        auto apm = autoplaylist_manager::get();
        apm->add_client_simple("ALL", "", p, 0);
        if (auto client = apm->query_client(p); client.is_valid()) client->show_ui(p);
    } catch (const std::exception& e) {
        popup_message::g_show(e.what(), "Create auto playlist");
    }
}

void MainWindow::OnAlbumLink(size_t group, size_t line) {
    const auto* groups = m_source ? m_source->Groups() : nullptr;
    if (!groups || group >= groups->size() || line > 2) return;
    const auto& g = (*groups)[group];
    View v;
    v.kind = View::Kind::Group;
    v.group = line == 0 ? media::GroupKind::Album : line == 1 ? media::GroupKind::Artist : media::GroupKind::Genre;
    v.key = g.linkKeys[line];
    v.name = g.lines[line];
    if (!v.key.empty()) Navigate(v, true);
}

void MainWindow::GoBack() {
    if (m_historyPos == 0) return;
    --m_historyPos;
    Navigate(m_history[m_historyPos], false);
}

void MainWindow::GoForward() {
    if (m_historyPos + 1 >= m_history.size()) return;
    ++m_historyPos;
    Navigate(m_history[m_historyPos], false);
}

void MainWindow::OnNavItem(const NavPane::Item& item) {
    View v;
    switch (item.target) {
    case NavPane::Target::Playlist: v.kind = View::Kind::Playlist; v.playlist = item.playlist; break;
    case NavPane::Target::Music: v.kind = View::Kind::AllMusic; break;
    case NavPane::Target::Artists: v.kind = View::Kind::Artists; break;
    case NavPane::Target::Albums: v.kind = View::Kind::Albums; break;
    case NavPane::Target::Genres: v.kind = View::Kind::Genres; break;
    case NavPane::Target::PlaylistsHeader: {
        auto pm = playlist_manager::get();
        const t_size active = pm->get_active_playlist();
        if (active == SIZE_MAX) return;
        v.kind = View::Kind::Playlist;
        v.playlist = active;
        break;
    }
    default: return;
    }
    Navigate(v, true);
}

void MainWindow::ShowOrganizeMenu() {
    POINT pt{m_layout.organize.left, m_layout.organize.bottom};
    ClientToScreen(m_hwnd, &pt);
    ShowMainMenu(m_hwnd, pt,
                 {{L"Manage music library...", [] { library_manager::get()->show_preferences(); }},
                  {L"Rescan library", [] { library_manager::get()->rescan(); }},
                  {L"", nullptr},
                  {L"Add files...", [] { standard_commands::main_add_files(); }},
                  {L"Add folder...", [] { standard_commands::main_add_directory(); }},
                  {L"Add location...", [] { standard_commands::run_main(standard_commands::guid_main_add_location); }},
                  {L"", nullptr},
                  {L"Preferences...", [] { standard_commands::run_main(standard_commands::guid_main_preferences); }}});
}

void MainWindow::ShowPlaylistMenu(size_t playlist, POINT screen) {
    auto pm = playlist_manager::get();
    std::vector<MenuAction> actions;
    if (playlist != SIZE_MAX && playlist < pm->get_playlist_count()) {
        actions.push_back({L"Play", [playlist] {
                               auto p = playlist_manager::get();
                               p->set_active_playlist(playlist);
                               const t_size focus = p->playlist_get_focus_item(playlist);
                               if (p->playlist_get_item_count(playlist))
                                   p->playlist_execute_default_action(playlist, focus == SIZE_MAX ? 0 : focus);
                           }});
        actions.push_back({L"Rename...", [playlist] {
                               playlist_manager::get()->set_active_playlist(playlist);
                               standard_commands::run_main(standard_commands::guid_main_rename_playlist);
                           }});
        actions.push_back({L"Remove", [playlist] { playlist_manager::get()->remove_playlist(playlist); }});
        actions.push_back({L"", nullptr});
    }
    actions.push_back({L"New playlist", [this] {
                           auto p = playlist_manager::get();
                           const t_size i = p->create_playlist("New Playlist", SIZE_MAX, SIZE_MAX);
                           if (i == SIZE_MAX) return;
                           View v;
                           v.kind = View::Kind::Playlist;
                           v.playlist = i;
                           Navigate(v, true);
                       }});
    ShowMainMenu(m_hwnd, screen, actions);
}

void MainWindow::AddDroppedFiles(IDataObject* data) {
    // Drop target: the playlist on screen, else the active playlist (created if needed).
    auto pm = playlist_manager::get();
    t_size target = m_view.kind == View::Kind::Playlist ? m_view.playlist : pm->get_active_playlist();
    if (target == SIZE_MAX || target >= pm->get_playlist_count()) target = pm->find_or_create_playlist("Default");
    auto notify = process_locations_notify::create([target](metadb_handle_list_cref items) {
        auto p = playlist_manager::get();
        if (target < p->get_playlist_count()) {
            p->playlist_undo_backup(target);
            p->playlist_add_items(target, items, pfc::bit_array_false());
        }
    });
    playlist_incoming_item_filter_v2::get()->process_dropped_files_async(
        data, playlist_incoming_item_filter_v2::op_flag_delay_ui, m_hwnd, notify);
}

void MainWindow::SetSearchFocus(bool focused) {
    m_search->SetFocused(focused);
    m_list->SetFocused(!focused);
}

// ---- Core state ------------------------------------------------------------------------

void MainWindow::OnPlaybackStateChanged() {
    auto pc = playback_control::get();
    const bool playing = pc->is_playing(), paused = pc->is_paused();
    if (playing == m_playing && paused == m_paused) return;
    m_playing = playing;
    m_paused = paused;
    for (int id : {kPlay, kStop, kPrev, kNext})
        if (auto* w = WidgetById(id)) w->Invalidate();
    if (m_list) m_list->Refresh();  // playing marker
}

void MainWindow::OnTrackChanged() {
    static titleformat_object::ptr s_title, s_artist;
    if (!s_title) {
        titleformat_compiler::get()->compile_safe(s_title, "$if2(%title%,%filename%)");
        titleformat_compiler::get()->compile_safe(s_artist, "[%artist%]");
    }
    auto pc = playback_control::get();
    pfc::string8 title, artist;
    if (pc->is_playing()) {
        pc->playback_format_title(nullptr, title, s_title, nullptr, playback_control::display_level_all);
        pc->playback_format_title(nullptr, artist, s_artist, nullptr, playback_control::display_level_all);
    } else {
        m_art = nullptr;
    }
    m_trackTitle = Wide(title);
    m_trackArtist = Wide(artist);
    if (metadb_handle_ptr now; pc->get_now_playing(now)) {
        if (const media::RepairedText* r = media::CueCatalog::Get().Repair(now)) {  // mis-decoded cue sheet
            if (!r->title.empty()) m_trackTitle = r->title;
            if (!r->artist.empty()) m_trackArtist = r->artist;
        }
    }
    m_length = pc->is_playing() ? pc->playback_get_length() : 0;
    m_position = pc->is_playing() ? pc->playback_get_position() : 0;
    RepaintNowPlaying();
    if (auto* w = WidgetById(kSeek)) w->Invalidate();
    if (m_list) m_list->Refresh();

    std::wstring caption = m_trackTitle.empty() ? std::wstring(kAppName)
                                                : m_trackTitle + (m_trackArtist.empty() ? L"" : L" - " + m_trackArtist) +
                                                      L" - " + kAppName;
    SetWindowTextW(m_hwnd, caption.c_str());
}

void MainWindow::OnPlaybackTime() {
    auto pc = playback_control::get();
    m_position = pc->playback_get_position();
    m_length = pc->playback_get_length();
    if (!m_seeking)
        if (auto* w = WidgetById(kSeek)) w->Invalidate();
    RepaintNowPlaying();
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
        for (int id : {kVolume, kMute})  // the speaker icon shows the level
            if (auto* w = WidgetById(id)) w->Invalidate();
    }
}

void MainWindow::OnPlaybackOrderChanged() {
    if (m_applyingOrder) return;
    // Changed elsewhere (foobar2000's menu): read the pair back from the order, keeping what
    // the order cannot express from the saved state.
    const std::string name = ActiveOrderName();
    bool shuffle = m_shuffle;
    Repeat repeat = m_repeat;
    if (pfc::stricmp_ascii(name.c_str(), "Default") == 0) {
        shuffle = false;
        repeat = Repeat::Off;
    } else if (pfc::stricmp_ascii(name.c_str(), "Repeat (playlist)") == 0) {
        shuffle = false;
        repeat = Repeat::All;
    } else if (pfc::stricmp_ascii(name.c_str(), "Repeat (track)") == 0) {
        repeat = Repeat::One;
    } else if (name.find("Shuffle") != std::string::npos || name.find("Random") != std::string::npos) {
        shuffle = true;
        if (repeat == Repeat::One) repeat = Repeat::All;
    }
    UpdateShuffleRepeat(shuffle, repeat);
}

void MainWindow::UpdateShuffleRepeat(bool shuffle, Repeat repeat) {
    if (shuffle != m_shuffle) {
        m_shuffle = shuffle;
        if (auto* w = WidgetById(kShuffle)) w->Invalidate();
    }
    if (repeat != m_repeat) {
        m_repeat = repeat;
        if (auto* w = WidgetById(kRepeat)) w->Invalidate();
    }
    cfg_shuffle = shuffle ? 1 : 0;
    cfg_repeat = static_cast<int>(repeat);
}

void MainWindow::SetShuffleRepeat(bool shuffle, Repeat repeat) {
    UpdateShuffleRepeat(shuffle, repeat);
    m_shufflePass = 0;
    ApplyPlaybackOrder();
}

void MainWindow::ApplyPlaybackOrder() {
    // Repeat one wins; shuffle without repeat is shuffle that stops after one pass
    // (OnShuffleTrack), since foobar2000's shuffle orders loop.
    const char* name = m_repeat == Repeat::One ? "Repeat (track)"
                       : m_shuffle             ? "Shuffle (tracks)"
                       : m_repeat == Repeat::All ? "Repeat (playlist)"
                                                 : "Default";
    m_applyingOrder = true;
    if (pfc::stricmp_ascii(ActiveOrderName().c_str(), name) != 0) SetOrderByName(name);
    m_applyingOrder = false;
    if (m_stopAfterPass) {  // undo our own "stop after this one" when the mode changes
        m_stopAfterPass = false;
        playback_control::get()->set_stop_after_current(false);
    }
}

void MainWindow::OnShuffleTrack() {
    if (!m_shuffle || m_repeat != Repeat::Off) return;
    auto pm = playlist_manager::get();
    const t_size playing = pm->get_playing_playlist();
    const t_size count = playing != SIZE_MAX ? pm->playlist_get_item_count(playing) : 0;
    if (++m_shufflePass >= count && count > 0) {
        m_stopAfterPass = true;
        playback_control::get()->set_stop_after_current(true);
        m_shufflePass = 0;
    }
}

void MainWindow::OnPlaylistContentChanged(size_t playlist) {
    if (m_view.kind == View::Kind::Playlist && m_view.playlist == playlist && m_source) {
        static_cast<media::PlaylistSource*>(m_source.get())->Rebuild();
        m_list->Refresh();
    } else if (m_list) {
        m_list->Refresh();  // playing marker may have moved
    }
}

void MainWindow::OnPlaylistSelectionChanged(size_t playlist) {
    if (m_view.kind == View::Kind::Playlist && m_view.playlist == playlist && m_list) m_list->Refresh();
}

void MainWindow::OnPlaylistsChanged() {
    if (!m_nav) return;
    m_nav->Rebuild();
    if (m_view.kind == View::Kind::Playlist) Navigate(m_view, false);  // index or name may have changed
}

void MainWindow::ScheduleLibraryReload() {
    if (m_hwnd) SetTimer(m_hwnd, kLibraryTimer, 400, nullptr);  // debounce bursts of library events
}

RECT MainWindow::ClusterRect(const D2D1_RECT_F& r) const {
    const POINT o = m_layout.cluster;
    return {o.x + PxI(r.left), o.y + PxI(r.top), o.x + PxI(r.right), o.y + PxI(r.bottom)};
}

D2D1_RECT_F MainWindow::ClusterRegion(const RECT& r) const {
    const POINT o = m_layout.cluster;
    return {(r.left - o.x) / m_scale, (r.top - o.y) / m_scale, (r.right - o.x) / m_scale, (r.bottom - o.y) / m_scale};
}

// With the original artwork the bars follow WMP 12's bitmaps (27-DIP orbs, 51-DIP cluster).
float MainWindow::NavBarHeight() const { return m_win7Art ? 33.0f : m::kNavBarHeight; }
float MainWindow::TransportHeight() const { return m_win7Art ? wt::kBandHeight : m::kTransportHeight; }

void MainWindow::SetVolumeFromX(int x) {
    const RECT& r = m_layout.volume;
    float x0, x1;
    if (m_win7Art) {
        x0 = m_layout.cluster.x + Px(wt::kThumbMin);
        x1 = m_layout.cluster.x + Px(wt::kThumbMax);
    } else {
        const float thumb = Px(11.0f);
        x0 = r.left + thumb / 2;
        x1 = r.right - thumb / 2;
    }
    const float v = std::clamp((x - x0) / std::max(1.0f, x1 - x0), 0.0f, 1.0f);
    m_volume = v;
    for (int id : {kVolume, kMute})
        if (auto* w = WidgetById(id)) w->Invalidate();
    playback_control::get()->set_volume(DbFromSlider(v));
    ShowVolumeTip(x0 + v * (x1 - x0));
}

void MainWindow::ShowVolumeTip(float thumbX) {
    // Win7 tooltip: white to #E4E5F0, #767676 border, small radius, soft shadow, grey text.
    auto& host = gfx::CompositionHost::Get();
    if (!m_volumeTip) {
        m_volumeTip = host.Compositor().CreateSpriteVisual();
        m_volumeTip.Opacity(0.0f);
    }
    m_root.Children().Remove(m_volumeTip);
    m_root.Children().InsertAtTop(m_volumeTip);  // above everything added since
    const std::wstring text = std::to_wstring(static_cast<int>(std::lround(m_volume * 100))) + L"%";
    auto layout = MakeText(text, 12.0f, DWRITE_FONT_WEIGHT_NORMAL, 200, 20);
    DWRITE_TEXT_METRICS tm{};
    layout->GetMetrics(&tm);
    const float shadow = 3, w = std::ceil(tm.widthIncludingTrailingWhitespace) + 14, h = 21;
    const int pw = PxI(w + shadow), ph = PxI(h + shadow);
    host.Repaint(m_volumeTip, pw, ph, m_scale, [&](ID2D1DeviceContext* dc) {
        const float px = 1.0f / m_scale;
        for (int i = 3; i >= 1; --i) {
            com_ptr<ID2D1SolidColorBrush> s;
            dc->CreateSolidColorBrush(D2D1::ColorF(0, 0.06f), s.put());
            dc->FillRoundedRectangle({{static_cast<float>(i), static_cast<float>(i), w - 1 + i, h - 1 + i}, 3, 3}, s.get());
        }
        com_ptr<ID2D1SolidColorBrush> border, ink;
        dc->CreateSolidColorBrush(Hex(0x767676), border.put());
        dc->CreateSolidColorBrush(Hex(0x575757), ink.put());
        // Win7's own tooltip (aero.msstyles Tooltip part 1, image 881, sizing margins 4).
        if (!win7::DrawNineGrid(dc, win7::Module::Aero, 881, {0, 0, 9, 22}, {4, 4, 4, 4}, {0, 0, w, h})) {
            com_ptr<ID2D1GradientStopCollection> stops;
            const D2D1_GRADIENT_STOP gs[] = {{0.0f, Hex(0xFFFFFF)}, {1.0f, Hex(0xE4E5F0)}};
            dc->CreateGradientStopCollection(gs, 2, stops.put());
            com_ptr<ID2D1LinearGradientBrush> fill;
            dc->CreateLinearGradientBrush({{0, 0}, {0, h}}, stops.get(), fill.put());
            const D2D1_ROUNDED_RECT box{{px / 2, px / 2, w - px / 2, h - px / 2}, 2.5f, 2.5f};
            dc->FillRoundedRectangle(box, fill.get());
            dc->DrawRoundedRectangle(box, border.get(), px);
        }
        dc->DrawTextLayout({7, (h - tm.height) / 2}, layout.get(), ink.get());
    });
    RECT client{};
    GetClientRect(m_hwnd, &client);
    const float left = std::clamp(thumbX - Px(w) / 2, 0.0f, static_cast<float>(client.right - pw));
    const float top = static_cast<float>(m_layout.volume.top - ph - PxI(2));
    m_volumeTip.Offset({std::round(left), std::round(top), 0});
    m_volumeTip.Size({static_cast<float>(pw), static_cast<float>(ph)});
    if (!m_volumeTipShown) {
        m_volumeTipShown = true;
        host.AnimateOpacity(m_volumeTip, 1.0f, 100);
    }
}

void MainWindow::HideVolumeTip() {
    if (!m_volumeTipShown) return;
    m_volumeTipShown = false;
    gfx::CompositionHost::Get().AnimateOpacity(m_volumeTip, 0.0f, 300);
}

float MainWindow::SeekFractionAt(int x) const {
    const RECT& r = m_layout.seek;
    return std::clamp(static_cast<float>(x - r.left) / std::max<LONG>(r.right - r.left, 1), 0.0f, 1.0f);
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
    PaintSideHighlights();
}

void MainWindow::PaintSideHighlights() {
    const SIZE size = m_sideHighlightSize;
    if (!m_aeroArt || size.cx <= 0 || size.cy <= 0 || !m_sideHighlight[0]) return;
    auto& host = gfx::CompositionHost::Get();
    const D2D1_SIZE_F dip{size.cx / m_scale, size.cy / m_scale};
    for (auto& v : m_sideHighlight)
        host.Repaint(v, size.cx, size.cy, m_scale,
                     [&](ID2D1DeviceContext* dc) { aero::atlas::SideHighlight(dc, m_active, dip); });
}

// ---- Window procedure ----------------------------------------------------------------

LRESULT MainWindow::WndProc(UINT msg, WPARAM wp, LPARAM lp) {
    const HWND hwnd = m_hwnd;
    switch (msg) {
    case WM_CREATE: {
        m_scale = GetDpiForWindow(hwnd) / 96.0f;
        InitComposition();
        CreateWidgets();
        RepaintScaleDependent();
        m_shadow.Create(hwnd);
        m_library.Reload();
        m_playCallback = std::make_unique<PlayCallback>(*this);
        m_playlistCallback = std::make_unique<PlaylistCallback>(*this);
        m_libraryCallback = std::make_unique<LibraryCallback>(*this);
        m_artNotify = now_playing_album_art_notify_manager::get()->add([this](album_art_data::ptr data) {
            m_art = DecodeArt(data, PxI(TransportHeight()));
            RepaintNowPlaying();
        });
        m_dropTarget = MakeFileDropTarget([this](IDataObject* data) { AddDroppedFiles(data); });
        RegisterDragDrop(hwnd, m_dropTarget.get());

        // Start where foobar2000 is: the active playlist, else the music library.
        View start;
        const t_size active = playlist_manager::get()->get_active_playlist();
        if (active != SIZE_MAX) {
            start.kind = View::Kind::Playlist;
            start.playlist = active;
        }
        Navigate(start, true);

        OnPlaybackStateChanged();
        OnTrackChanged();
        OnVolumeChanged();
        m_shuffle = cfg_shuffle.get() != 0;
        m_repeat = static_cast<Repeat>(std::clamp<int>(static_cast<int>(cfg_repeat.get()), 0, 2));
        OnPlaybackOrderChanged();
        SetWindowPos(hwnd, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER);
        return 0;
    }

    case WM_TIMER:
        if (wp == kLibraryTimer) {
            KillTimer(hwnd, kLibraryTimer);
            m_library.Reload();
            if (m_view.kind != View::Kind::Playlist) Navigate(m_view, false);
            return 0;
        }
        break;

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

    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            POINT pt{};
            GetCursorPos(&pt);
            ScreenToClient(hwnd, &pt);
            if (m_search && m_search->HitTest(pt)) {
                SetCursor(LoadCursorW(nullptr, IDC_IBEAM));
                return TRUE;
            }
            if (m_list && m_list->HitTest(pt))
                if (LPCWSTR cursor = m_list->CursorAt(pt)) {  // column edges, album links
                    SetCursor(LoadCursorW(nullptr, cursor));
                    return TRUE;
                }
        }
        break;

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

    case WM_WINDOWPOSCHANGING: {
        const auto* pos = reinterpret_cast<const WINDOWPOS*>(lp);
        if (m_glass && !(pos->flags & SWP_NOMOVE) && !IsIconic(hwnd)) {
            RECT wr{};
            GetWindowRect(hwnd, &wr);
            const LONG w = (pos->flags & SWP_NOSIZE) ? wr.right - wr.left : pos->cx;
            const LONG h = (pos->flags & SWP_NOSIZE) ? wr.bottom - wr.top : pos->cy;
            m_glass->UpdateReflectionAnchor(hwnd, RECT{pos->x, pos->y, pos->x + w, pos->y + h});
        }
        break;
    }

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
        mmi->ptMinTrackSize = {PxI(640), PxI(420)};
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
            m_capture = Capture::Widget;
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
                m_capture = Capture::None;
                if (GetCapture() == hwnd) ReleaseCapture();
            }
            if (Widget* w = WidgetById(id); same && w && w->onClick) w->onClick();
            return 0;
        }
        break;

    // -- Client-area mouse --
    case WM_MOUSEMOVE: {
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        if (m_capture == Capture::List) {
            m_list->OnMouseMove(pt);
        } else if (m_pressed) {
            if (m_pressed->Id() == kVolume) {
                SetVolumeFromX(pt.x);
            } else if (m_pressed->Id() == kSeek) {
                m_seekPreview = SeekFractionAt(pt.x);
                m_pressed->Invalidate();
            } else {
                m_pressed->SetPressed(m_pressed->HitTest(pt));
            }
        } else {
            SetHoverWidget(WidgetAt(pt));
            m_list->OnMouseMove(pt);
            m_nav->OnMouseMove(pt);
            UpdateCrumbHover(pt);
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
        if (m_capture == Capture::None) {
            m_list->OnMouseLeave();
            m_nav->OnMouseLeave();
            UpdateCrumbHover({-1, -1});
        }
        return 0;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK: {
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        const bool dbl = msg == WM_LBUTTONDBLCLK;
        if (PtInRect(&m_layout.breadcrumb, pt)) {
            OnCrumbClick(pt);
            return 0;
        }
        if (Widget* w = WidgetAt(pt)) {
            m_pressed = w;
            w->SetPressed(true);
            m_capture = Capture::Widget;
            SetCapture(hwnd);
            if (w->Id() == kVolume) SetVolumeFromX(pt.x);
            if (w->Id() == kSeek && m_length > 0) {
                m_seeking = true;
                m_seekPreview = SeekFractionAt(pt.x);
                w->Invalidate();
            }
            return 0;
        }
        if (m_list->HitTest(pt)) {
            SetSearchFocus(false);
            m_list->OnLButtonDown(pt, KeyDown(VK_CONTROL), KeyDown(VK_SHIFT));
            if (dbl) m_list->OnDoubleClick(pt);
            if (m_list->Capturing()) {
                m_capture = Capture::List;
                SetCapture(hwnd);
            }
            return 0;
        }
        if (m_nav->HitTest(pt)) {
            if (const NavPane::Item* item = m_nav->OnLButtonDown(pt)) {
                const NavPane::Item copy = *item;
                OnNavItem(copy);
                if (dbl && copy.target == NavPane::Target::Playlist && m_source && m_source->Count())
                    m_source->Activate(0);
            }
            return 0;
        }
        if (m_search->HitTest(pt)) {
            SetSearchFocus(true);
            return 0;
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        m_ncDownId = 0;
        if (m_capture == Capture::List) {
            m_capture = Capture::None;
            ReleaseCapture();
            m_list->OnLButtonUp(pt);
            return 0;
        }
        if (Widget* w = m_pressed) {
            // ReleaseCapture() synchronously sends WM_CAPTURECHANGED, which resets the drag
            // state; take what we need first.
            const bool seek = w->Id() == kSeek && m_seeking;
            const float preview = m_seekPreview;
            m_pressed = nullptr;
            m_capture = Capture::None;
            ReleaseCapture();
            w->SetPressed(false);
            if (w->Id() == kVolume) HideVolumeTip();
            if (w->Id() == kSeek) {
                if (seek && m_length > 0) playback_control::get()->playback_seek(preview * m_length);
                m_seeking = false;
                w->Invalidate();
                return 0;
            }
            const bool inside = w->HitTest(pt);
            if (!inside || IsCaptionWidget(w->Id())) SetHoverWidget(nullptr);
            if (inside && w->onClick && w->IsEnabled()) w->onClick();
        }
        return 0;
    }
    case WM_CAPTURECHANGED:
        if (reinterpret_cast<HWND>(lp) != hwnd) {
            HideVolumeTip();
            if (m_pressed) {
                m_pressed->SetPressed(false);
                m_pressed = nullptr;
            }
            if (m_capture == Capture::List) m_list->CancelDrag();
            m_capture = Capture::None;
            m_seeking = false;
        }
        return 0;
    case WM_MOUSEWHEEL: {
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ScreenToClient(hwnd, &pt);
        const int delta = GET_WHEEL_DELTA_WPARAM(wp);
        if (m_list->HitTest(pt)) {
            m_list->OnWheel(delta, (GET_KEYSTATE_WPARAM(wp) & MK_SHIFT) != 0);
            m_list->OnMouseMove(pt);
            return 0;
        }
        if (m_nav->HitTest(pt)) {
            m_nav->OnWheel(delta);
            return 0;
        }
        if (PtInRect(&m_layout.transport, pt)) {  // wheel over the transport band: volume, like WMP
            auto pc = playback_control::get();
            if (delta > 0) pc->volume_up(); else pc->volume_down();
            return 0;
        }
        break;
    }
    case WM_MOUSEHWHEEL: {  // tilt wheel / touchpad sideways: the list scrolls horizontally
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ScreenToClient(hwnd, &pt);
        if (m_list->HitTest(pt)) {
            m_list->OnWheel(-GET_WHEEL_DELTA_WPARAM(wp), true);
            return TRUE;
        }
        break;
    }
    case WM_RBUTTONUP: {
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        if (m_list->HitTest(pt)) {
            m_list->OnRButtonUp(hwnd, pt);
            return 0;
        }
        POINT screen = pt;
        ClientToScreen(hwnd, &screen);
        if (m_nav->HitTest(pt)) {
            const NavPane::Item* item = m_nav->ItemAt(pt);
            ShowPlaylistMenu(item && item->target == NavPane::Target::Playlist ? item->playlist : SIZE_MAX, screen);
            return 0;
        }
        if (pt.y >= m_layout.client.top) {
            ShowMainMenu(hwnd, screen);
            return 0;
        }
        break;
    }

    // -- Keyboard --
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: {
        const bool ctrl = KeyDown(VK_CONTROL), shift = KeyDown(VK_SHIFT);
        if (msg == WM_KEYDOWN) {
            if ((ctrl && wp == 'E') || wp == VK_F3) {
                SetSearchFocus(true);
                return 0;
            }
            if (m_search->Focused()) {
                if (m_search->OnKeyDown(static_cast<UINT>(wp), ctrl, hwnd)) return 0;
                if (wp == VK_DOWN || wp == VK_TAB) {
                    SetSearchFocus(false);
                    m_list->OnKeyDown(VK_HOME, false, false);
                    return 0;
                }
            }
            if (wp == VK_BACK && !m_search->Focused()) {
                GoBack();
                return 0;
            }
            if (m_list->OnKeyDown(static_cast<UINT>(wp), ctrl, shift)) return 0;
        }
        if (msg == WM_SYSKEYDOWN && wp == VK_LEFT) {
            GoBack();
            return 0;
        }
        if (msg == WM_SYSKEYDOWN && wp == VK_RIGHT) {
            GoForward();
            return 0;
        }
        if (keyboard_shortcut_manager::get()->on_keydown_auto(wp)) return 0;
        break;
    }
    case WM_CHAR:
        if (m_search->Focused() && m_search->OnChar(static_cast<wchar_t>(wp), hwnd)) return 0;
        break;
    case WM_IME_STARTCOMPOSITION:
        if (m_search->Focused()) {
            if (HIMC imc = ImmGetContext(hwnd)) {
                COMPOSITIONFORM cf{CFS_POINT, m_search->CaretClientPoint(), {}};
                ImmSetCompositionWindow(imc, &cf);
                ImmReleaseContext(hwnd, imc);
            }
        }
        break;
    case WM_XBUTTONUP:
        if (GET_XBUTTON_WPARAM(wp) == XBUTTON1) GoBack();
        else if (GET_XBUTTON_WPARAM(wp) == XBUTTON2) GoForward();
        return TRUE;
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
        KillTimer(hwnd, kLibraryTimer);
        RevokeDragDrop(hwnd);
        m_dropTarget = nullptr;
        if (m_artNotify) {
            now_playing_album_art_notify_manager::get()->remove(m_artNotify);
            m_artNotify = nullptr;
        }
        m_shadow.Destroy();
        m_libraryCallback.reset();
        m_playlistCallback.reset();
        m_playCallback.reset();
        m_hover = m_pressed = nullptr;
        m_list.reset();
        m_nav.reset();
        m_search.reset();
        m_source.reset();
        m_widgets.clear();
        m_glass.reset();
        m_root = nullptr;
        m_windowRoot = nullptr;
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
