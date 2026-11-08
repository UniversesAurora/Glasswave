#include "pch.h"
#include <filesystem>
#include <fstream>
#include <cwchar>
#include <cwctype>
#include "ui/main_window.h"
#include "aero/aero_atlas.h"
#include "aero/aero_metrics.h"
#include "aero/aero_painter.h"
#include "gfx/composition_host.h"
#include "media/source_identity.h"
#include "ui/main_menu.h"
#include "ui/shell_glue.h"
#include "ui/taskbar.h"
#include "ui/win7_menu.h"
#include "ui/i18n.h"
#include "media/favorites.h"
#include "media/special_playlists.h"
#include "netease/provider.h"
#include "ui/netease_login.h"
#include "ui/netease_sources.h"
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
constexpr UINT_PTR kVolumeTipTimer = 2;
constexpr UINT kVolumeTipDelayMs = 800;  // hover before the bubble appears; dragging is immediate
constexpr float kNavPaneWidth = 190.0f;

enum WidgetId {
    kCapMin = 1, kCapMax, kCapClose,
    kBack, kForward, kOrganize, kAddFiles, kAddFolder, kNewPlaylist,
    kSeek, kShuffle, kRepeat, kStop, kPrev, kNext, kPlay, kMute, kVolume, kViewOptions,
};
bool IsCaptionWidget(int id) { return id >= kCapMin && id <= kCapClose; }

// Split buttons: arrow half width, and the view options button's icon half (DIP).
constexpr float kSplitArrow = 16, kViewOptionsSplit = 26;


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
// Playlists: Details (WMP's default for a playlist) or Expanded tile.
cfg_var_modern::cfg_int cfg_viewPlaylist({0x6a1f3c5e, 0x2b7e, 0x4d90, {0x8c, 0x15, 0x3e, 0x9a, 0x7b, 0x2d, 0x4f, 0x61}},
                                         static_cast<int>(media::ViewStyle::Details));
// The other Music categories (Year, Rating, Composer, ...) share one view option.
cfg_var_modern::cfg_int cfg_viewCategories({0x6a1f3c5c, 0x2b7e, 0x4d90, {0x8c, 0x15, 0x3e, 0x9a, 0x7b, 0x2d, 0x4f, 0x61}},
                                           static_cast<int>(media::ViewStyle::Tiles));
// All Music > Favorites; NetEase playlists; NetEase songs.
// {6A1F3C5F-2B7E-4D90-8C15-3E9A7B2D4F61} .. {6A1F3C61-...}
cfg_var_modern::cfg_int cfg_viewAllFavorites({0x6a1f3c5f, 0x2b7e, 0x4d90, {0x8c, 0x15, 0x3e, 0x9a, 0x7b, 0x2d, 0x4f, 0x61}},
                                             static_cast<int>(media::ViewStyle::Details));
cfg_var_modern::cfg_int cfg_viewNcmPlaylists({0x6a1f3c60, 0x2b7e, 0x4d90, {0x8c, 0x15, 0x3e, 0x9a, 0x7b, 0x2d, 0x4f, 0x61}},
                                             static_cast<int>(media::ViewStyle::Icons));
cfg_var_modern::cfg_int cfg_viewNcmSongs({0x6a1f3c61, 0x2b7e, 0x4d90, {0x8c, 0x15, 0x3e, 0x9a, 0x7b, 0x2d, 0x4f, 0x61}},
                                         static_cast<int>(media::ViewStyle::Details));
// Music categories shown in the navigation pane, in order: media::GroupKind values as digits.
// {6A1F3C5D-2B7E-4D90-8C15-3E9A7B2D4F61}
cfg_var_modern::cfg_string cfg_navCategories({0x6a1f3c5d, 0x2b7e, 0x4d90, {0x8c, 0x15, 0x3e, 0x9a, 0x7b, 0x2d, 0x4f, 0x61}},
                                             "012");

std::vector<media::GroupKind> LoadNavCategories() {
    std::vector<media::GroupKind> out;
    for (char c : std::string(cfg_navCategories.get().c_str()))
        if (c >= '0' && c < '0' + static_cast<int>(media::kGroupKinds)) {
            const auto kind = static_cast<media::GroupKind>(c - '0');
            if (std::find(out.begin(), out.end(), kind) == out.end()) out.push_back(kind);
        }
    return out;
}

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

// WMP's large icon frames carry uneven transparent margins (the 64 px one shows 50x46 px,
// low and left), so the taskbar drew it small and off-centre. This builds a big icon
// from the 256 px frame cropped to its visible part, centred and scaled to `sizePx`.
HICON CreateCenteredAppIcon(int sizePx) {
    HICON big = win7::Resources::Get().Icon(win7::Module::Wmploc, kWmpIcon, 256);
    if (!big) return CreateAppIcon(sizePx);
    auto* wic = gfx::CompositionHost::Get().Wic();
    com_ptr<IWICBitmap> src;
    HICON result = nullptr;
    if (SUCCEEDED(wic->CreateBitmapFromHICON(big, src.put()))) {
        UINT w = 0, h = 0;
        src->GetSize(&w, &h);
        std::vector<BYTE> px(static_cast<size_t>(w) * h * 4);
        src->CopyPixels(nullptr, w * 4, static_cast<UINT>(px.size()), px.data());
        UINT l = w, t = h, r = 0, b = 0;
        for (UINT y = 0; y < h; ++y)
            for (UINT x = 0; x < w; ++x)
                if (px[(static_cast<size_t>(y) * w + x) * 4 + 3] > 8) {
                    l = std::min(l, x), t = std::min(t, y), r = std::max(r, x), b = std::max(b, y);
                }
        if (r > l && b > t) {
            com_ptr<IWICBitmapClipper> clip;
            com_ptr<IWICBitmapScaler> scaler;
            const UINT cw = r - l + 1, ch = b - t + 1, side = std::max(cw, ch);
            const UINT dw = std::max<UINT>(1, sizePx * cw / side), dh = std::max<UINT>(1, sizePx * ch / side);
            const WICRect rc{static_cast<INT>(l), static_cast<INT>(t), static_cast<INT>(cw), static_cast<INT>(ch)};
            if (SUCCEEDED(wic->CreateBitmapClipper(clip.put())) && SUCCEEDED(clip->Initialize(src.get(), &rc)) &&
                SUCCEEDED(wic->CreateBitmapScaler(scaler.put())) &&
                SUCCEEDED(scaler->Initialize(clip.get(), dw, dh, WICBitmapInterpolationModeHighQualityCubic))) {
                std::vector<BYTE> scaled(static_cast<size_t>(dw) * dh * 4);
                scaler->CopyPixels(nullptr, dw * 4, static_cast<UINT>(scaled.size()), scaled.data());
                // Into a centred square 32-bpp DIB (BGRA, straight alpha as icons expect).
                BITMAPV5HEADER bh{};
                bh.bV5Size = sizeof(bh);
                bh.bV5Width = sizePx;
                bh.bV5Height = -sizePx;
                bh.bV5Planes = 1;
                bh.bV5BitCount = 32;
                bh.bV5Compression = BI_BITFIELDS;
                bh.bV5RedMask = 0x00FF0000, bh.bV5GreenMask = 0x0000FF00, bh.bV5BlueMask = 0x000000FF,
                bh.bV5AlphaMask = 0xFF000000;
                void* bits = nullptr;
                HDC screen = GetDC(nullptr);
                HBITMAP color = CreateDIBSection(screen, reinterpret_cast<BITMAPINFO*>(&bh), DIB_RGB_COLORS, &bits, nullptr, 0);
                ReleaseDC(nullptr, screen);
                if (color) {
                    auto* out = static_cast<BYTE*>(bits);
                    std::memset(out, 0, static_cast<size_t>(sizePx) * sizePx * 4);
                    const UINT ox = (sizePx - dw) / 2, oy = (sizePx - dh) / 2;
                    for (UINT y = 0; y < dh; ++y)
                        std::memcpy(out + ((static_cast<size_t>(y) + oy) * sizePx + ox) * 4, scaled.data() + static_cast<size_t>(y) * dw * 4,
                                    static_cast<size_t>(dw) * 4);
                    HBITMAP mask = CreateBitmap(sizePx, sizePx, 1, 1, nullptr);
                    ICONINFO ii{TRUE, 0, 0, mask, color};
                    result = CreateIconIndirect(&ii);
                    DeleteObject(mask);
                    DeleteObject(color);
                }
            }
        }
    }
    DestroyIcon(big);
    return result ? result : CreateAppIcon(sizePx);
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
                                  flag_on_playback_time | flag_on_playback_dynamic_info |
                                  flag_on_playback_dynamic_info_track | flag_on_volume_change),
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
    void on_playback_dynamic_info(const file_info&) override { window.OnTrackChanged(); }
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
    void on_playlist_locked(t_size, bool) override { window.OnPlaylistLockChanged(); }
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
    netease::Provider::Get().Initialize();
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
                    &m_divider, &m_clientGlow, &m_title, &m_nowPlaying, &m_breadcrumb, &m_navGroove, &m_time,
                    &m_streamVisual})
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
    for (const auto& v : {m_capsule, m_cluster, m_divider, m_clientGlow, m_title, m_nowPlaying, m_breadcrumb, m_time,
                          m_streamVisual})
        children.InsertAtTop(v);
    m_win7Art = wt::Available();
    m_aeroArt = aero::atlas::Available();
    m_divider.IsVisible(!m_win7Art);  // part of the original capsule bitmap
    m_cluster.IsVisible(m_win7Art);

    m_nav = std::make_unique<NavPane>(m_contentLayer);
    m_nav->SetCategories(LoadNavCategories());
    m_nav->SetNeteaseState(netease::Provider::Get().LoggedIn(), netease::Provider::Get().CurrentAccount().nickname);
    netease::Provider::Get().SetStateCallback([this] { OnNeteaseLoginChanged(); });
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
        ->onClick = [this] {
              // Pressed while its menu is up, like WMP's drop-down buttons.
              Widget* w = WidgetById(kOrganize);
              if (w) w->SetPressed(true);
              ShowOrganizeMenu();
              if (w) w->SetPressed(false);
          };
    add(kAddFiles, [](auto* dc, D2D1_SIZE_F s, Layer l) { wp::CommandButton(dc, s, l, L"Add files", false); }, {})
        ->onClick = [] { standard_commands::main_add_files(); };
    add(kAddFolder, [](auto* dc, D2D1_SIZE_F s, Layer l) { wp::CommandButton(dc, s, l, L"Add folder", false); }, {})
        ->onClick = [] { standard_commands::main_add_directory(); };
    // Split buttons (WMP 12): one control; the main part acts at once, the arrow part opens a
    // menu. m_splitPart says which half the mouse is over.
    add(kViewOptions, [this](auto* dc, D2D1_SIZE_F s, Layer l) {
        const float split = kViewOptionsSplit;
        if (!wp::SplitButtonPlate(dc, s, l, split, m_splitPart, m_splitMenu == kViewOptions))
            wp::CommandButton(dc, s, l, L"", true);
        wp::ViewOptionsGlyph(dc, {split, s.height}, m_source ? m_source->GetViewStyle() : media::ViewStyle::Details);
    }, {})->onClick = [this] {
        if (m_splitPart == 1) RunSplitMenu(kViewOptions, [this] { ShowViewMenu(); });
        else CycleViewStyle();
    };
    add(kNewPlaylist, [this](auto* dc, D2D1_SIZE_F s, Layer l) {
        if (!wp::SplitButtonPlate(dc, s, l, m_newPlaylistSplit, m_splitPart, m_splitMenu == kNewPlaylist))
            wp::CommandButton(dc, s, l, L"", true);
        wp::CommandButton(dc, {m_newPlaylistSplit, s.height}, Layer::Normal, L"Create playlist", false);  // text only
    }, {})->onClick = [this] {
        if (m_splitPart == 1)
            RunSplitMenu(kNewPlaylist, [this] {
                POINT pt{m_layout.newPlaylist.left, m_layout.newPlaylist.bottom};
                ClientToScreen(m_hwnd, &pt);
                auto add = AddPlaylistActions();
                ShowActionMenu(m_hwnd, pt, add.front().children);  // new playlist / smart / presets
            });
        else
            CreatePlaylist(false);
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
        L.breadcrumb = {L.navGroove.right + PxI(6), L.navBar.top, L.client.right - PxI(8), L.navBar.bottom - line};
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
    // Widths follow the text (6 DIP padding each side, 12 more for a drop-down arrow).
    auto fit = [](const wchar_t* text, bool drop) {
        auto layout = MakeText(Tr(text), 12.0f, DWRITE_FONT_WEIGHT_NORMAL, 400, 20);
        DWRITE_TEXT_METRICS tm{};
        layout->GetMetrics(&tm);
        return std::ceil(tm.widthIncludingTrailingWhitespace) + 12 + (drop ? 12.0f : 0.0f);
    };
    L.organize = button(fit(L"Organize", true));
    L.addFiles = button(fit(L"Add files", false));
    L.addFolder = button(fit(L"Add folder", false));
    // Split buttons (WMP 12): the main part acts at once, the arrow part opens a menu.
    m_newPlaylistSplit = fit(L"Create playlist", false);
    L.newPlaylist = button(m_newPlaylistSplit + kSplitArrow);
    L.search = {L.client.right - PxI(8) - PxI(230), cbcy - PxI(11), L.client.right - PxI(8), cbcy + PxI(11)};
    // View options (WMP: icon / expanded tile / details) left of the search box, library only.
    if (m_source && ViewStyles() > 1) {
        const int right = L.search.left - PxI(8);
        L.viewOptions = {right - PxI(kViewOptionsSplit + kSplitArrow), cbcy - PxI(11), right, cbcy + PxI(11)};
    } else {
        L.viewOptions = {};
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
    // Time right of the controls (after the volume slider); stream format and the output
    // device right-aligned at the band's end.
    {
        const int tcyMid = (L.transport.top + PxI(4) + L.transport.bottom) / 2;
        const int after = (m_win7Art ? ClusterRect(wt::kCapsuleVisible).right : L.volume.right) + PxI(12);
        L.time = {after, tcyMid - PxI(10), after + PxI(110), tcyMid + PxI(10)};
        L.stream = {std::max<LONG>(L.time.right, L.client.right - PxI(10) - PxI(320)), L.transport.top + PxI(8),
                    L.client.right - PxI(10), L.transport.bottom - PxI(4)};
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
    Place(m_time, L.time);
    Place(m_streamVisual, L.stream);
    if (const LONG sw = L.stream.right - L.stream.left; sw != m_streamWidth) {
        m_streamWidth = sw;
        RepaintStreamInfo();
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
        const int size = GetSystemMetricsForDpi(metric, dpi);
        HICON icon = which == ICON_BIG ? CreateCenteredAppIcon(size) : CreateAppIcon(size);
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
        com_ptr<ID2D1SolidColorBrush> dark, grey, link, lyricInk;
        dc->CreateSolidColorBrush(Hex(0x1E1E1E), dark.put());
        dc->CreateSolidColorBrush(Hex(0x5B5B5B), grey.put());
        dc->CreateSolidColorBrush(Hex(0x0066CC), link.put());
        dc->CreateSolidColorBrush(Hex(0x2E64B4), lyricInk.put());
        // Title and artist are links (to the song in the list, to the artist); the synced
        // lyric line, if any, goes under the artist.
        std::wstring lyric;
        for (const auto& [at, text] : m_lyrics) {
            if (at > m_position + 0.05) break;
            lyric = text;
        }
        const float lh = 15, top = std::round((H - (m_lyrics.empty() ? 2 : 3) * lh) / 2);
        auto line = [&](const std::wstring& text, float y, ID2D1SolidColorBrush* ink, bool hot) {
            auto layout = MakeText(text, 12.0f, DWRITE_FONT_WEIGHT_NORMAL, 350 - x, lh + 2);
            if (hot) layout->SetUnderline(TRUE, {0, static_cast<UINT32>(text.size())});
            dc->DrawTextLayout({x, y - 1}, layout.get(), hot ? link.get() : ink);
            DWRITE_TEXT_METRICS tm{};
            layout->GetMetrics(&tm);
            return D2D1_RECT_F{x, y, x + std::min(tm.widthIncludingTrailingWhitespace, 350 - x), y + lh};
        };
        const auto toClient = [&](D2D1_RECT_F r) {
            return RECT{m_layout.nowPlaying.left + PxI(r.left), m_layout.nowPlaying.top + PxI(r.top),
                        m_layout.nowPlaying.left + PxI(r.right), m_layout.nowPlaying.top + PxI(r.bottom)};
        };
        // The title leaves room for the Favorite star right after it (as in current players).
        const D2D1_RECT_F title = [&] {
            auto layout = MakeText(m_trackTitle, 12.0f, DWRITE_FONT_WEIGHT_NORMAL, 350 - x - 22, lh + 2);
            if (m_npHot == 1) layout->SetUnderline(TRUE, {0, static_cast<UINT32>(m_trackTitle.size())});
            dc->DrawTextLayout({x, top - 1}, layout.get(), m_npHot == 1 ? link.get() : dark.get());
            DWRITE_TEXT_METRICS tm{};
            layout->GetMetrics(&tm);
            return D2D1_RECT_F{x, top, x + std::min(tm.widthIncludingTrailingWhitespace, 350 - x - 22), top + lh};
        }();
        m_npTitleRect = toClient(title);
        // WMP's stars: gold (5530) in Favorites, gold outline (5531) on hover, empty (5533).
        const float sx = std::round((title.right + 6) * m_scale) / m_scale, sy = std::round((top + (lh - 14) / 2) * m_scale) / m_scale;
        win7::Draw(dc, win7::Module::Wmploc, m_npFavorite ? 5530 : m_npHot == 4 ? 5531 : 5533, {sx, sy});
        m_npStarRect = toClient({sx - 3, sy - 3, sx + 17, sy + 17});
        m_npArtistRect = m_trackArtist.empty() ? RECT{}
                                               : toClient(line(m_trackArtist, top + lh, grey.get(), m_npHot == 2));
        if (!m_lyrics.empty()) line(lyric, top + 2 * lh, lyricInk.get(), false);
    });
    if (m_trackTitle.empty()) m_npTitleRect = m_npArtistRect = m_npStarRect = {};
    // Time right of the controls.
    host.Repaint(m_time, PxI(110), PxI(20), m_scale, [&](ID2D1DeviceContext* dc) {
        if (m_length <= 0 && m_trackTitle.empty()) return;
        std::wstring text = FormatTime(m_position);
        if (m_length > 0) text += L" / " + FormatTime(m_length);
        com_ptr<ID2D1SolidColorBrush> grey;
        dc->CreateSolidColorBrush(Hex(0x4A4A4A), grey.put());
        auto layout = MakeText(text, 12.0f, DWRITE_FONT_WEIGHT_NORMAL, 110, 20);
        dc->DrawTextLayout({0, 0}, layout.get(), grey.get());
    });
    UpdateLayout();
}

void MainWindow::RepaintStreamInfo() {
    // Right-aligned: stream format on top, the output device (a menu) below.
    auto& host = gfx::CompositionHost::Get();
    // Painted at the block's real width (narrow windows shrink it); long names get an ellipsis.
    const int w = std::max<int>(1, m_layout.stream.right - m_layout.stream.left), h = PxI(TransportHeight() - 12);
    if (m_deviceName.empty()) RefreshDeviceName();
    const std::wstring device = m_deviceName.empty() ? Tr(L"Output device") : m_deviceName;
    float deviceLeft = 0, deviceTop = 0, deviceRight = 0;
    host.Repaint(m_streamVisual, w, h, m_scale, [&](ID2D1DeviceContext* dc) {
        const float W = w / m_scale, H = h / m_scale;
        com_ptr<ID2D1SolidColorBrush> grey, link;
        dc->CreateSolidColorBrush(Hex(0x5B5B5B), grey.put());
        dc->CreateSolidColorBrush(Hex(m_npHot == 3 ? 0x0066CC : 0x2E4E7A), link.put());
        const float lh = 15, top = std::round((H - 2 * lh) / 2);
        if (!m_streamInfo.empty()) {
            auto info = MakeText(m_streamInfo, 11.5f, DWRITE_FONT_WEIGHT_NORMAL, W, lh + 2);
            info->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
            dc->DrawTextLayout({0, top - 1}, info.get(), grey.get());
        }
        const std::wstring label = device + L"  ▾";
        auto dev = MakeText(label, 11.5f, DWRITE_FONT_WEIGHT_NORMAL, W, lh + 2);
        dev->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
        dev->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        const DWRITE_TRIMMING ellipsis{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        com_ptr<IDWriteInlineObject> sign;
        if (com_ptr<IDWriteFactory> f; SUCCEEDED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(f.put()))))
            f->CreateEllipsisTrimmingSign(dev.get(), sign.put());
        dev->SetTrimming(&ellipsis, sign.get());
        if (m_npHot == 3) dev->SetUnderline(TRUE, {0, static_cast<UINT32>(device.size())});
        dc->DrawTextLayout({0, top + lh - 1}, dev.get(), link.get());
        DWRITE_TEXT_METRICS tm{};
        dev->GetMetrics(&tm);
        deviceLeft = tm.left;
        deviceRight = tm.left + tm.width;
        deviceTop = top + lh;
    });
    m_deviceText = {deviceLeft, deviceTop, deviceRight, deviceTop + 15};
}

void MainWindow::RefreshDeviceName() {
    m_deviceName.clear();
    try {
        const auto cfg = output_manager::get()->getCoreConfig();
        output_manager_v2::get()->listDevices([&](const char* name, const GUID& output, const GUID& dev) {
            if (output == cfg.m_output && dev == cfg.m_device) m_deviceName = Wide(name);
        });
    } catch (...) {
    }
}

RECT MainWindow::DeviceRect() const {
    const RECT& s = m_layout.stream;
    if (m_deviceText.right <= m_deviceText.left) return {};
    return {s.left + PxI(m_deviceText.left), s.top + PxI(m_deviceText.top), s.left + PxI(m_deviceText.right),
            s.top + PxI(m_deviceText.bottom)};
}

void MainWindow::LoadLyrics(const metadb_handle_ptr& track) {
    // Synced lyrics only ([mm:ss.xx] lines): a SYNCEDLYRICS / LYRICS tag, else a .lrc file
    // next to the audio file.
    m_lyrics.clear();
    if (!track.is_valid()) return;
    std::wstring text;
    if (auto info = track->get_info_ref(); info.is_valid()) {
        for (const char* field : {"SYNCEDLYRICS", "LYRICS", "UNSYNCEDLYRICS"}) {
            if (const char* v = info->info().meta_get(field, 0); v && std::strchr(v, '[')) {
                text = Wide(v);
                break;
            }
        }
    }
    if (text.empty()) {
        pfc::string8 native;
        if (filesystem::g_get_native_path(track->get_path(), native)) {
            std::wstring path = Wide(native);
            if (const size_t dot = path.find_last_of(L'.'); dot != std::wstring::npos && dot > path.find_last_of(L"\\/"))
                path = path.substr(0, dot);
            path += L".lrc";
            if (std::ifstream in{std::filesystem::path(path), std::ios::binary}) {
                const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                if (bytes.size() >= 2 && static_cast<unsigned char>(bytes[0]) == 0xFF &&
                    static_cast<unsigned char>(bytes[1]) == 0xFE) {
                    text.assign(reinterpret_cast<const wchar_t*>(bytes.data() + 2), (bytes.size() - 2) / 2);
                } else {
                    const size_t skip = bytes.rfind("\xEF\xBB\xBF", 0) == 0 ? 3 : 0;
                    const char* p = bytes.data() + skip;
                    const int n = static_cast<int>(bytes.size() - skip);
                    UINT cp = CP_UTF8;
                    int len = MultiByteToWideChar(cp, MB_ERR_INVALID_CHARS, p, n, nullptr, 0);
                    if (len <= 0) {
                        cp = CP_ACP;
                        len = MultiByteToWideChar(cp, 0, p, n, nullptr, 0);
                    }
                    text.resize(static_cast<size_t>(std::max(len, 0)));
                    if (len > 0) MultiByteToWideChar(cp, 0, p, n, text.data(), len);
                }
            }
        }
    }
    std::wstringstream in(text);
    for (std::wstring l; std::getline(in, l);) {
        if (!l.empty() && l.back() == L'\r') l.pop_back();
        std::vector<double> times;
        size_t pos = 0;
        while (pos < l.size() && l[pos] == L'[') {
            const size_t end = l.find(L']', pos);
            if (end == std::wstring::npos) break;
            int mm = 0;
            double ss = 0;
            if (swscanf_s(l.c_str() + pos + 1, L"%d:%lf", &mm, &ss) == 2) times.push_back(mm * 60 + ss);
            pos = end + 1;
        }
        const std::wstring words = l.substr(pos);
        for (double t : times) m_lyrics.emplace_back(t, words);
    }
    std::stable_sort(m_lyrics.begin(), m_lyrics.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
}

int MainWindow::NowPlayingPartAt(POINT pt) const {
    if (PtInRect(&m_npTitleRect, pt)) return 1;
    if (PtInRect(&m_npArtistRect, pt)) return 2;
    if (PtInRect(&m_npStarRect, pt)) return 4;
    if (const RECT d = DeviceRect(); PtInRect(&d, pt)) return 3;
    return 0;
}

void MainWindow::UpdateNowPlayingHover(POINT pt) {
    const int part = NowPlayingPartAt(pt);
    if (part == m_npHot) return;
    const int old = m_npHot;
    m_npHot = part;
    if (old == 3 || part == 3) RepaintStreamInfo();
    if (old == 1 || old == 2 || old == 4 || part == 1 || part == 2 || part == 4) RepaintNowPlaying();
}

void MainWindow::OnNowPlayingClick(int part) {
    switch (part) {
    case 1: JumpToPlaying(); break;
    case 2: {  // the artist's page (artists are grouped by album artist)
        metadb_handle_ptr now;
        if (!playback_control::get()->get_now_playing(now)) break;
        const media::Track* t = m_library.FindTrack(now);
        const media::Group* g = t ? m_library.FindGroup(media::GroupKind::Artist, t->albumArtist) : nullptr;
        if (!g) g = m_library.FindGroup(media::GroupKind::Artist, m_trackArtist);
        if (!g) break;
        View v;
        v.kind = View::Kind::Group;
        v.group = media::GroupKind::Artist;
        v.key = g->key;
        v.name = g->name;
        Navigate(v, true);
        break;
    }
    case 3: ShowDeviceMenu(); break;
    case 4: {  // Favorite star next to the title
        metadb_handle_ptr now;
        if (playback_control::get()->get_now_playing(now)) media::ToggleFavorite(now);
        UpdateFavoriteState();
        break;
    }
    default: break;
    }
}

void MainWindow::JumpToPlaying() {
    metadb_handle_ptr now;
    if (!playback_control::get()->get_now_playing(now)) return;
    size_t row = m_source ? m_source->FindRow(now) : SIZE_MAX;
    if (row == SIZE_MAX) {  // not on this page: open the playlist it plays from
        t_size playlist = SIZE_MAX, index = SIZE_MAX;
        if (!playlist_manager::get()->get_playing_item_location(&playlist, &index)) return;
        View v;
        v.kind = View::Kind::Playlist;
        v.playlist = playlist;
        Navigate(v, true);
        row = m_source ? m_source->FindRow(now) : SIZE_MAX;
    }
    if (row == SIZE_MAX) return;
    m_source->SelectOnly(row);
    m_source->SetFocus(row);
    m_list->EnsureVisible(row);
    m_list->Refresh();
}

void MainWindow::ShowDeviceMenu() {
    struct Device {
        GUID output, device;
    };
    std::vector<Device> devices;
    std::vector<MenuAction> actions;
    size_t current = SIZE_MAX;
    try {
        const auto cfg = output_manager::get()->getCoreConfig();
        output_manager_v2::get()->listDevices([&](const char* name, const GUID& output, const GUID& device) {
            if (output == cfg.m_output && device == cfg.m_device) current = devices.size();
            devices.push_back({output, device});
            actions.push_back({Wide(name), [this, output, device] {
                                   output_manager_v2::get()->setCoreConfigDevice(output, device);
                                   RefreshDeviceName();
                                   RepaintStreamInfo();
                               }});
        });
    } catch (const std::exception& e) {
        popup_message::g_show(e.what(), "Output device");
        return;
    }
    const RECT d = DeviceRect();
    POINT pt{d.left, d.bottom};
    ClientToScreen(m_hwnd, &pt);
    ShowActionMenu(m_hwnd, pt, actions, current);
}

void MainWindow::RepaintBreadcrumb() {
    BuildCrumbs();
    PaintBreadcrumb();
    UpdateLayout();
}

void MainWindow::BuildCrumbs() {
    auto go = [this](View v) { return [this, v] { Navigate(v, true); }; };
    // Names are laid out after the crumb list is built; early provider branches must call
    // this too, or every crumb keeps x0 = split = x1 = 0 and they draw on top of each other.
    auto layoutCrumbs = [this] {
        float x = 0;
        for (auto& c : m_crumbs) {
            c.x0 = x;
            if (!c.text.empty()) {
                auto t = MakeText(Tr(c.text), 12.0f, DWRITE_FONT_WEIGHT_NORMAL, 400, 20);
                DWRITE_TEXT_METRICS tm{};
                t->GetMetrics(&tm);
                x += std::ceil(tm.widthIncludingTrailingWhitespace) + 8;
            }
            c.split = x;
            if (c.children) x += 12;
            c.x1 = x;
        }
        if (m_crumbHot >= static_cast<int>(m_crumbs.size())) m_crumbHot = -1;
    };
    View all;
    all.kind = View::Kind::AllMusic;
    auto categoryView = [](media::GroupKind k) {
        View v;
        v.kind = View::Kind::Category;
        v.group = k;
        return v;
    };
    auto isSmartList = [](t_size p) { return p == media::FavoritesPlaylist(false) || media::IsAutoPlaylist(p); };
    auto playlistsOf = [this, go, isSmartList](bool smart) {
        std::vector<MenuAction> out;
        auto pm = playlist_manager::get();
        for (t_size i = 0, n = pm->get_playlist_count(); i < n; ++i) {
            if (media::IsQueue(i) || isSmartList(i) != smart) continue;
            pfc::string8 name;
            pm->playlist_get_name(i, name);
            View v;
            v.kind = View::Kind::Playlist;
            v.playlist = i;
            out.push_back({Wide(name), go(v)});
        }
        return out;
    };
    auto playlists = [playlistsOf] { return playlistsOf(false); };
    auto smartLists = [playlistsOf] { return playlistsOf(true); };
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
    auto music = [this, go, categoryView, all] {
        std::vector<MenuAction> out{{L"All music", go(all)}};
        for (auto kind : m_nav->Categories()) out.push_back({CategoryLabel(kind), go(categoryView(kind))});
        return out;
    };
    View playlistsPage;
    playlistsPage.kind = View::Kind::Playlists;
    View smartPage;
    smartPage.kind = View::Kind::SmartPlaylists;
    View nowPlaying;
    nowPlaying.kind = View::Kind::Playlist;
    nowPlaying.playlist = playlist_manager::get()->get_active_playlist();

    View allLibrary;
    allLibrary.library = View::Library::All;
    allLibrary.kind = View::Kind::AllMusic;
    View localLibrary;
    localLibrary.library = View::Library::Local;
    localLibrary.kind = View::Kind::AllMusic;
    View neteaseHome;
    neteaseHome.library = View::Library::Netease;
    neteaseHome.kind = View::Kind::NeteasePlaylists;
    auto libraries = [go, allLibrary, localLibrary, neteaseHome] {
        return std::vector<MenuAction>{{L"All Music", go(allLibrary)},
                                       {L"Local Music", go(localLibrary)},
                                       {L"NetEase Cloud Music", go(neteaseHome)}};
    };
    auto neteaseMenu = [go, neteaseHome] {
        View mine = neteaseHome;
        View recommended = neteaseHome;
        recommended.recommended = true;
        View liked;
        liked.library = View::Library::Netease;
        liked.kind = View::Kind::NeteaseLiked;
        return std::vector<MenuAction>{{L"My playlists", go(mine)},
                                       {L"Recommended", go(recommended)},
                                       {L"Favorites", go(liked)}};
    };

    if (m_view.library == View::Library::All) {
        m_crumbs.clear();
        m_crumbs.push_back({L"", {}, libraries});
        m_crumbs.push_back({L"All Music", go(allLibrary), {}});
        if (m_view.kind == View::Kind::AllFavorites) m_crumbs.push_back({L"Favorites", go(m_view), {}});
        if (m_view.kind == View::Kind::Search) m_crumbs.push_back({L"Search results", go(m_view), {}});
        layoutCrumbs();
        return;
    }
    if (m_view.library == View::Library::Netease) {
        m_crumbs.clear();
        m_crumbs.push_back({L"", {}, libraries});
        m_crumbs.push_back({L"NetEase Cloud Music", go(neteaseHome), neteaseMenu});
        if (m_view.kind == View::Kind::NeteasePlaylists) {
            m_crumbs.push_back({m_view.recommended ? L"Recommended" : L"My playlists", go(m_view), neteaseMenu});
        } else if (m_view.kind == View::Kind::NeteasePlaylist) {
            m_crumbs.push_back({L"My playlists", go(neteaseHome), neteaseMenu});
            m_crumbs.push_back({m_view.name.empty() ? L"Playlist" : m_view.name, go(m_view), {}});
        } else if (m_view.kind == View::Kind::NeteaseLiked) {
            m_crumbs.push_back({L"Favorites", go(m_view), neteaseMenu});
        } else if (m_view.kind == View::Kind::Search) {
            m_crumbs.push_back({L"Search results", go(m_view), neteaseMenu});
        }
        layoutCrumbs();
        return;
    }

    m_crumbs.clear();
    m_crumbs.push_back({L"", {}, [go, all] { return std::vector<MenuAction>{{L"Library", go(all)}}; }});
    m_crumbs.push_back({L"Library", go(all), [go, all, playlistsPage, smartPage] {
                            return std::vector<MenuAction>{{L"Playlists", go(playlistsPage)},
                                                           {L"Smart playlists", go(smartPage)},
                                                           {L"Music", go(all)}};
                        }});
    if (m_view.kind == View::Kind::Playlist && media::IsQueue(m_view.playlist)) {
        m_crumbs.push_back({L"Now Playing", go(m_view), {}});  // the queue sits beside Playlists
    } else if (m_view.kind == View::Kind::Playlists) {
        m_crumbs.push_back({L"Playlists", go(m_view), playlists});
    } else if (m_view.kind == View::Kind::SmartPlaylists) {
        m_crumbs.push_back({L"Smart playlists", go(m_view), smartLists});
    } else if (m_view.kind == View::Kind::Playlist) {
        if (isSmartList(m_view.playlist))
            m_crumbs.push_back({L"Smart playlists", go(smartPage), smartLists});
        else
            m_crumbs.push_back({L"Playlists", go(playlistsPage), playlists});
        m_crumbs.push_back({m_view.name, go(m_view), {}});  // Tr() at layout translates built-in names
    } else {
        m_crumbs.push_back({L"Music", go(all), music});
        const media::GroupKind kind = m_view.group;
        if (m_view.kind == View::Kind::AllMusic) {
            m_crumbs.push_back({L"All music", go(all), {}});
        } else {
            m_crumbs.push_back({CategoryLabel(kind), go(categoryView(kind)), groupsOf(kind)});
            if (m_view.kind == View::Kind::Group) m_crumbs.push_back({m_view.name, go(m_view), {}});
        }
    }

    // Lay out: name with 4 DIP padding, arrow 12 DIP; the root crumb is an arrow only.
    layoutCrumbs();
}

void MainWindow::PaintBreadcrumb() {
    auto& host = gfx::CompositionHost::Get();
    const int w = PxI(900), h = PxI(NavBarHeight() - 1);
    host.Repaint(m_breadcrumb, w, h, m_scale, [&](ID2D1DeviceContext* dc) {
        // Plates 21 DIP high (owner: 17 was too low), centred with the text in the bar.
        const float H = h / m_scale, top = std::round((H - 21) / 2 * m_scale) / m_scale, bottom = top + 21;
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
                if (!c.text.empty()) {
                    // MAINBUTTON is the left half of a split button and has no right edge; a
                    // level without an arrow is closed with the split part's last 3 px.
                    const float px = 1.0f / m_scale, edge = c.children ? 0 : 3 * px;
                    win7::DrawNineGrid(dc, win7::Module::Wmploc, L"WMP_BREADCRUMB_MAINBUTTON.PNG",
                                       {16.0f * nameState, 0, 16.0f * nameState + 16, 22}, {3, 3, 3, 3},
                                       {c.x0, top, c.split - edge, bottom});
                    if (!c.children)
                        win7::DrawNineGrid(dc, win7::Module::Wmploc, L"WMP_BREADCRUMB_SPLITBUTTON.PNG",
                                           {10.0f * nameState + 7, 0, 10.0f * nameState + 10, 22}, {0, 3, 0, 3},
                                           {c.split - edge, top, c.split, bottom});
                }
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
                auto t = MakeText(Tr(c.text), 12.0f, DWRITE_FONT_WEIGHT_NORMAL, c.split - c.x0, H);
                t->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);  // centered in the bar, like WMP
                dc->DrawTextLayout({c.x0 + 4, 0}, t.get(), ink.get());
            }
            if (c.children) {  // small black triangle: right, or down while the menu is open
                const float cx = std::round(((c.split + c.x1) / 2) * m_scale) / m_scale;
                const float cy = std::round((H / 2) * m_scale) / m_scale;
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
                dc->CreateSolidColorBrush(Hex(0x1E3250), arrow.put());  // WMP's dark navy, as a vector
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
    case View::Kind::NeteaseLiked: {
        source = std::make_unique<ui::NeteaseSongSource>(std::vector<netease::Song>{},
                                                         L"Loading your NetEase favorites...");
        m_nav->Select(NavPane::Target::NeteaseLiked, 0);
        const View requestedView = view;
        netease::Provider::Get().FetchLiked(
            [this, requestedView](std::vector<netease::Song> songs, std::wstring error) {
                ShowNeteaseSongs(requestedView, std::move(songs), std::move(error));
            });
        break;
    }
    case View::Kind::AllFavorites: {
        source = std::make_unique<ui::NeteaseSongSource>(std::vector<netease::Song>{},
                                                         L"Loading your NetEase favorites...");
        m_nav->Select(NavPane::Target::AllFavorites, 0);
        const View requestedView = view;
        netease::Provider::Get().FetchLiked(
            [this, requestedView](std::vector<netease::Song> songs, std::wstring error) {
                ShowNeteaseSongs(requestedView, std::move(songs), std::move(error));
            });
        break;
    }
    case View::Kind::NeteasePlaylists: {
        source = std::make_unique<ui::NeteasePlaylistSource>(
            std::vector<netease::Playlist>{}, L"Loading NetEase playlists...",
            [this](int64_t id, std::wstring name) { OpenNeteasePlaylist(id, std::move(name)); });
        m_nav->Select(view.recommended ? NavPane::Target::NeteaseRecommended : NavPane::Target::NeteaseMyPlaylists, 0);
        const View requestedView = view;
        auto done = [this, requestedView](std::vector<netease::Playlist> playlists, std::wstring error) {
            ShowNeteasePlaylists(requestedView, std::move(playlists), std::move(error));
        };
        if (view.recommended)
            netease::Provider::Get().FetchRecommended(done);
        else
            netease::Provider::Get().FetchMyPlaylists(done);
        break;
    }
    case View::Kind::NeteasePlaylist: {
        source = std::make_unique<ui::NeteaseSongSource>(std::vector<netease::Song>{}, L"Loading playlist...");
        m_nav->Select(NavPane::Target::NeteaseMyPlaylists, 0);
        const View requestedView = view;
        const int64_t id = static_cast<int64_t>(_wcstoi64(view.remoteId.c_str(), nullptr, 10));
        netease::Provider::Get().FetchPlaylist(
            id, [this, requestedView](netease::PlaylistDetail detail, std::wstring error) {
                ShowNeteaseSongs(requestedView, std::move(detail.songs), std::move(error));
            });
        break;
    }
    case View::Kind::Playlist: {
        pfc::string8 name;
        pm->playlist_get_name(view.playlist, name);
        view.name = Wide(name);
        if (pm->get_active_playlist() != view.playlist) pm->set_active_playlist(view.playlist);
        source = std::make_unique<media::PlaylistSource>(view.playlist);
        m_nav->Select(media::IsQueue(view.playlist) ? NavPane::Target::Queue : NavPane::Target::Playlist, view.playlist);
        break;
    }
    case View::Kind::AllMusic: {
        std::vector<size_t> all(m_library.Tracks().size());
        for (size_t i = 0; i < all.size(); ++i) all[i] = i;
        source = std::make_unique<media::TrackSource>(
            m_library, std::move(all),
            L"Your library is empty. Use Organize ▸ Manage music library to add your music folders.");
        m_nav->Select(view.library == View::Library::All ? NavPane::Target::LibraryAll : NavPane::Target::Music, 0);
        break;
    }
    case View::Kind::Playlists:         // WMP's Playlists page: every playlist as an icon
    case View::Kind::SmartPlaylists: {  // the same for Favorites and the auto playlists
        const bool smart = view.kind == View::Kind::SmartPlaylists;
        source = std::make_unique<media::PlaylistsSource>(
            smart,
            [this](size_t playlist) {
                View v;
                v.kind = View::Kind::Playlist;
                v.playlist = playlist;
                Navigate(v, true);
            },
            [this](size_t playlist, POINT screen) { ShowPlaylistMenu(playlist, screen); });
        m_nav->Select(smart ? NavPane::Target::SmartHeader : NavPane::Target::PlaylistsHeader, 0);
        break;
    }
    case View::Kind::Category: {
        const auto kind = view.group;
        source = std::make_unique<media::GroupSource>(m_library, kind, [this, kind](const media::Group& g) {
            View v;
            v.kind = View::Kind::Group;
            v.group = kind;
            v.key = g.key;
            v.name = g.secondary.empty() ? g.name : g.name + L" (" + g.secondary + L")";
            Navigate(v, true);
        });
        m_nav->Select(NavPane::Target::Category, static_cast<size_t>(kind));
        break;
    }
    case View::Kind::Group: {
        const media::Group* g = m_library.FindGroup(view.group, view.key);
        source = std::make_unique<media::TrackSource>(m_library, g ? g->tracks : std::vector<size_t>{},
                                                      L"No songs.");
        m_nav->Select(NavPane::Target::Category, static_cast<size_t>(view.group));
        break;
    }
    }

    const bool sameView = view.kind == m_view.kind && view.library == m_view.library && view.playlist == m_view.playlist &&
                          view.key == m_view.key && view.group == m_view.group &&
                          view.recommended == m_view.recommended && view.remoteId == m_view.remoteId &&
                          view.query == m_view.query;
    if (push && !sameView) {
        // A new view clears the search, like WMP.
        m_filter.clear();
        if (m_search) {
            auto keep = std::move(m_search->onChange);
            m_search->Clear();
            m_search->onChange = std::move(keep);
        }
    }
    if (auto* style = ViewStyleSetting(view); style && source->SupportsViewStyle(static_cast<media::ViewStyle>(style->get())))
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

void MainWindow::ShowNeteaseSongs(const View& view, std::vector<netease::Song> songs, std::wstring error) {
    if (m_view.kind != view.kind || m_view.library != view.library || m_view.remoteId != view.remoteId ||
        m_view.recommended != view.recommended)
        return;
    auto source = std::make_unique<ui::NeteaseSongSource>(
        std::move(songs), error.empty() ? L"No songs." : std::move(error));
    m_source = std::move(source);
    if (auto* style = ViewStyleSetting(view);
        style && m_source->SupportsViewStyle(static_cast<media::ViewStyle>(style->get())))
        m_source->SetViewStyle(static_cast<media::ViewStyle>(style->get()));
    m_list->SetSource(m_source.get(), false);
}

void MainWindow::ShowNeteasePlaylists(const View& view, std::vector<netease::Playlist> playlists,
                                      std::wstring error) {
    if (m_view.kind != view.kind || m_view.library != view.library || m_view.recommended != view.recommended) return;
    auto source = std::make_unique<ui::NeteasePlaylistSource>(
        std::move(playlists), error.empty() ? L"No playlists." : std::move(error),
        [this](int64_t id, std::wstring name) { OpenNeteasePlaylist(id, std::move(name)); });
    m_source = std::move(source);
    if (auto* style = ViewStyleSetting(view);
        style && m_source->SupportsViewStyle(static_cast<media::ViewStyle>(style->get())))
        m_source->SetViewStyle(static_cast<media::ViewStyle>(style->get()));
    m_list->SetSource(m_source.get(), false);
}

void MainWindow::OpenNeteasePlaylist(int64_t id, std::wstring name) {
    View view;
    view.library = View::Library::Netease;
    view.kind = View::Kind::NeteasePlaylist;
    view.remoteId = std::to_wstring(id);
    view.name = std::move(name);
    Navigate(view, true);
}

void MainWindow::ShowNeteaseLogin() {
    ui::ShowNeteaseLogin(m_hwnd, [this] { OnNeteaseLoginChanged(); });
}

void MainWindow::OnNeteaseLoginChanged() {
    m_nav->SetNeteaseState(netease::Provider::Get().LoggedIn(), netease::Provider::Get().CurrentAccount().nickname);
    UpdateFavoriteState();
    if (m_view.library == View::Library::Netease || m_view.kind == View::Kind::AllFavorites)
        Navigate(m_view, false);
    if (m_list) m_list->Refresh();
    RepaintBreadcrumb();
}

cfg_var_modern::cfg_int* MainWindow::ViewStyleSetting(const View& view) {
    switch (view.kind) {
    case View::Kind::AllMusic:
    case View::Kind::Group: return &cfg_viewSongs;
    case View::Kind::Playlist: return &cfg_viewPlaylist;
    case View::Kind::AllFavorites: return &cfg_viewAllFavorites;
    case View::Kind::NeteasePlaylists: return &cfg_viewNcmPlaylists;
    case View::Kind::NeteasePlaylist:
    case View::Kind::NeteaseLiked:
    case View::Kind::Search: return &cfg_viewNcmSongs;
    case View::Kind::Category:
        switch (view.group) {
        case media::GroupKind::Album: return &cfg_viewAlbums;
        case media::GroupKind::Artist: return &cfg_viewArtists;
        case media::GroupKind::Genre: return &cfg_viewGenres;
        default: return &cfg_viewCategories;
        }
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
    // WMP 12: each view with its icon (strip 331 slots 57 icon, 56 expanded tile, 55 details),
    // the current one in bold.
    const UINT current = static_cast<UINT>(m_source->GetViewStyle()) + 1;
    SetMenuDefaultItem(menu, current, FALSE);
    const std::map<UINT, int> icons{{static_cast<UINT>(media::ViewStyle::Icons) + 1, 57},
                                    {static_cast<UINT>(media::ViewStyle::Tiles) + 1, 56},
                                    {static_cast<UINT>(media::ViewStyle::Details) + 1, 55}};
    POINT pt{m_layout.viewOptions.left, m_layout.viewOptions.bottom};
    ClientToScreen(m_hwnd, &pt);
    const UINT cmd =
        TrackWin7Menu(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_LEFTALIGN | TPM_TOPALIGN, pt.x, pt.y, m_hwnd, &icons);
    DestroyMenu(menu);
    if (cmd == 0 || cmd == current) return;
    ApplyViewStyle(static_cast<media::ViewStyle>(cmd - 1));
}

void MainWindow::ApplyViewStyle(media::ViewStyle style) {
    m_source->SetViewStyle(style);
    if (auto* setting = ViewStyleSetting(m_view)) setting->set(static_cast<int>(style));
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
    case NavPane::Target::Queue:
        v.kind = View::Kind::Playlist;
        v.playlist = media::QueuePlaylist(true);
        break;
    case NavPane::Target::SmartHeader: v.kind = View::Kind::SmartPlaylists; break;
    case NavPane::Target::Music: v.kind = View::Kind::AllMusic; break;
    case NavPane::Target::Category:
        v.kind = View::Kind::Category;
        v.group = static_cast<media::GroupKind>(item.playlist);
        break;
    case NavPane::Target::PlaylistsHeader: v.kind = View::Kind::Playlists; break;  // WMP's Playlists page
    case NavPane::Target::LibraryAll:
        v.library = View::Library::All;
        v.kind = View::Kind::AllMusic;
        break;
    case NavPane::Target::AllFavorites:
        v.library = View::Library::All;
        v.kind = View::Kind::AllFavorites;
        break;
    case NavPane::Target::LibraryLocal: v.kind = View::Kind::AllMusic; break;
    case NavPane::Target::LibraryNetease:
        v.library = View::Library::Netease;
        v.kind = View::Kind::NeteasePlaylists;
        break;
    case NavPane::Target::NeteaseMyPlaylists:
        v.library = View::Library::Netease;
        v.kind = View::Kind::NeteasePlaylists;
        break;
    case NavPane::Target::NeteaseRecommended:
        v.library = View::Library::Netease;
        v.kind = View::Kind::NeteasePlaylists;
        v.recommended = true;
        break;
    case NavPane::Target::NeteaseLiked:
        v.library = View::Library::Netease;
        v.kind = View::Kind::NeteaseLiked;
        break;
    case NavPane::Target::NeteaseLogin:
        ShowNeteaseLogin();
        return;
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
                  {L"Language", nullptr,
                   {{L"Follow Windows", [this] { ApplyLanguage(Language::Auto); }, {},
                     CurrentLanguageSetting() == Language::Auto},
                    {L"English", [this] { ApplyLanguage(Language::English); }, {},
                     CurrentLanguageSetting() == Language::English},
                    {L"中文 (Chinese)", [this] { ApplyLanguage(Language::Chinese); }, {},
                     CurrentLanguageSetting() == Language::Chinese}}},
                  {L"", nullptr},
                  {L"Preferences...", [] { standard_commands::run_main(standard_commands::guid_main_preferences); }}});
    SetHoverWidget(nullptr);  // the menu loop swallowed the mouse leave
}

// WMP: right-click Music > Customize navigation pane picks the categories listed under it.
void MainWindow::ShowNavCustomizeMenu(POINT screen) {
    std::vector<MenuAction> categories;
    const auto shown = m_nav->Categories();
    for (size_t i = 0; i < media::kGroupKinds; ++i) {
        const auto kind = static_cast<media::GroupKind>(i);
        const bool on = std::find(shown.begin(), shown.end(), kind) != shown.end();
        MenuAction a{CategoryLabel(kind), [this, kind, on, shown] {
                         auto next = shown;
                         if (on)
                             next.erase(std::find(next.begin(), next.end(), kind));
                         else
                             next.push_back(kind);
                         std::sort(next.begin(), next.end());  // always listed in WMP order
                         SetNavCategories(std::move(next));
                     }};
        a.checked = on;
        a.checkbox = true;
        categories.push_back(std::move(a));
    }
    categories.push_back({});
    categories.push_back({L"Restore defaults", [this] {
                              SetNavCategories({media::GroupKind::Artist, media::GroupKind::Album,
                                                media::GroupKind::Genre});
                          }});
    View all;
    all.kind = View::Kind::AllMusic;
    std::vector<MenuAction> actions{{L"Open", [this, all] { Navigate(all, true); }},
                                    {L"Customize navigation pane", {}, std::move(categories)}};
    ShowActionMenu(m_hwnd, screen, actions);
}

void MainWindow::SetNavCategories(std::vector<media::GroupKind> kinds) {
    std::string digits;
    for (auto k : kinds) digits += static_cast<char>('0' + static_cast<int>(k));
    cfg_navCategories.set(digits.c_str());
    m_nav->SetCategories(std::move(kinds));
    RepaintBreadcrumb();
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
        if (media::IsSmart(playlist))
            actions.push_back({L"Edit smart playlist...", [playlist] {
                                   if (auto c = autoplaylist_manager::get()->query_client(playlist); c.is_valid())
                                       c->show_ui(playlist);
                               }});
        // The queue and Favorites are built in: no renaming or removing.
        if (!media::IsQueue(playlist) && playlist != media::FavoritesPlaylist(false)) {
            actions.push_back({L"Rename...", [playlist] {
                                   playlist_manager::get()->set_active_playlist(playlist);
                                   standard_commands::run_main(standard_commands::guid_main_rename_playlist);
                               }});
            actions.push_back({L"Remove", [playlist] { playlist_manager::get()->remove_playlist(playlist); }});
        }
        actions.push_back({L"", nullptr});
    }
    for (auto& a : AddPlaylistActions()) actions.push_back(std::move(a));
    actions.push_back({L"", nullptr});
    actions.push_back({L"Load playlist...", [] { standard_commands::main_load_playlist(); }});
    actions.push_back({L"Save all playlists...", [] { standard_commands::main_save_all_playlists(); }});
    actions.push_back({L"Restore smart playlists", [this] {
                           media::EnsureSpecialPlaylists(true);
                           m_nav->Rebuild();
                       }});
    ShowActionMenu(m_hwnd, screen, actions);
}

std::vector<MenuAction> MainWindow::AddPlaylistActions() {
    // As in the owner's localized foobar2000: Add > New playlist / New smart playlist /
    // Preset smart playlists > (library, never played, history, most played, recently
    // added | Favorites | rated 5..1, unrated).
    auto open = [this](t_size p) {
        if (p == SIZE_MAX) return;
        m_nav->Rebuild();
        View v;
        v.kind = View::Kind::Playlist;
        v.playlist = p;
        Navigate(v, true);
    };
    std::vector<MenuAction> presets;
    int group = 0;
    for (const auto& preset : media::SmartPresets()) {
        if (preset.group != group && !presets.empty()) presets.push_back({L"", nullptr});
        group = preset.group;
        presets.push_back({Wide(preset.name), [open, &preset] { open(media::AddSmartPreset(preset)); }});
    }
    return {{L"Add", nullptr,
             {{L"New playlist", [this] { CreatePlaylist(false); }},
              {L"New smart playlist", [open] { open(media::NewSmartPlaylist()); }},
              {L"Preset smart playlists", nullptr, std::move(presets)}}}};
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
    m_nav->SetFocused(false);
}

void MainWindow::FocusNav() {
    m_search->SetFocused(false);
    m_list->SetFocused(false);
    m_nav->SetFocused(true);
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
    if (m_taskbar) m_taskbar->SetPlayState(playing, paused);
    if (m_list) m_list->Refresh();  // playing marker
    OnTrackChanged();  // paused resume still has codec/bitrate information
}

void MainWindow::OnTrackChanged() {
    static titleformat_object::ptr s_title, s_artist;
    if (!s_title) {
        titleformat_compiler::get()->compile_safe(s_title, "$if2(%title%,%filename%)");
        titleformat_compiler::get()->compile_safe(s_artist, "[%artist%]");
    }
    auto pc = playback_control::get();
    metadb_handle_ptr now;
    const bool loaded = pc->get_now_playing(now);
    pfc::string8 title, artist;
    if (loaded) {
        pc->playback_format_title(nullptr, title, s_title, nullptr, playback_control::display_level_all);
        pc->playback_format_title(nullptr, artist, s_artist, nullptr, playback_control::display_level_all);
    } else {
        m_art = nullptr;
    }
    m_trackTitle = Wide(title);
    m_trackArtist = Wide(artist);
    if (loaded) {
        if (const media::RepairedText* r = media::CueCatalog::Get().Repair(now)) {  // mis-decoded cue sheet
            if (!r->title.empty()) m_trackTitle = r->title;
            if (!r->artist.empty()) m_trackArtist = r->artist;
        }
    }
    m_length = loaded ? pc->playback_get_length() : 0;
    m_position = loaded ? pc->playback_get_position() : 0;

    // Stream format: codec, bit rate, sample rate, bit depth, channels. A paused resumed
    // track still has all of this; gate on a loaded track, not on is_playing().
    static titleformat_object::ptr s_stream;
    if (!s_stream)
        titleformat_compiler::get()->compile_safe(
            s_stream, "[%codec%]|[%bitrate%]|[%samplerate%]|[$info(bitspersample)]|[%channels%]");
    m_streamInfo.clear();
    if (loaded) {
        pfc::string8 raw;
        pc->playback_format_title(nullptr, raw, s_stream, nullptr, playback_control::display_level_all);
        std::vector<std::wstring> f;
        std::wstringstream in(Wide(raw));
        for (std::wstring part; std::getline(in, part, L'|');) f.push_back(part);
        f.resize(5);
        auto add = [&](const std::wstring& s) {
            if (s.empty()) return;
            if (!m_streamInfo.empty()) m_streamInfo += L"   ";
            m_streamInfo += s;
        };
        add(f[0]);
        if (!f[1].empty()) add(f[1] + L" kbps");
        if (!f[2].empty()) {
            wchar_t khz[32];
            const double rate = _wtof(f[2].c_str()) / 1000.0;
            swprintf_s(khz, std::fmod(rate, 1.0) == 0 ? L"%.0f kHz" : L"%.1f kHz", rate);
            add(khz);
        }
        if (!f[3].empty()) add(f[3] + L" bit");
        if (!f[4].empty()) {
            std::wstring ch = f[4];
            ch[0] = static_cast<wchar_t>(::towupper(ch[0]));
            add(ch);
        }
    }
    if (loaded) LoadLyrics(now);
    else m_lyrics.clear();
    RepaintStreamInfo();
    UpdateFavoriteState();
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
        if (m_volumeTipShown) ShowVolumeTip(VolumeThumbX());  // keep a visible bubble current
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

void MainWindow::ApplyLanguage(Language language) {
    SetLanguageSetting(language);
    for (auto& w : m_widgets) w->Invalidate();
    if (m_nav) m_nav->Rebuild();
    RepaintBreadcrumb();
    RepaintStreamInfo();
    if (m_list) m_list->Refresh();
    UpdateLayout();  // button widths follow the text
}

void MainWindow::UpdateFavoriteState() {
    metadb_handle_ptr now;
    const bool fav = playback_control::get()->get_now_playing(now) && media::IsFavorite(now);
    if (m_taskbar) m_taskbar->SetFavorite(fav);
    if (fav != m_npFavorite) {
        m_npFavorite = fav;
        RepaintNowPlaying();
    }
}

void MainWindow::OnPlaylistContentChanged(size_t playlist) {
    if (playlist == media::FavoritesPlaylist(false)) UpdateFavoriteState();
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
    if (m_view.kind == View::Kind::Playlist || m_view.kind == View::Kind::Playlists ||
        m_view.kind == View::Kind::SmartPlaylists)
        Navigate(m_view, false);  // index, name or the set of playlists may have changed
}

void MainWindow::OnPlaylistLockChanged() {
    // foobar2000 restores auto playlists after the UI is up; each one shows up as a lock.
    // The lock's filter mask tells the nav pane it is an auto playlist before the client is
    // registered, so this one rebuild is enough.
    if (!m_nav) return;
    m_nav->Rebuild();
    if (m_view.kind == View::Kind::Playlists || m_view.kind == View::Kind::SmartPlaylists) Navigate(m_view, false);
    else RepaintBreadcrumb();
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
// WMP 12 at 150 %: the nav bar is 35 px with the 26 px orbs nearly filling it; ours keeps the larger
// orbs (22 DIP) with 3-4 DIP above and below.
float MainWindow::NavBarHeight() const { return m_win7Art ? 29.0f : m::kNavBarHeight; }
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
    KillTimer(m_hwnd, kVolumeTipTimer);  // dragging shows at once, no hover delay
    m_volumeHover = true;
    ShowVolumeTip(VolumeThumbX());
}

float MainWindow::VolumeThumbX() const {
    float x0, x1;
    if (m_win7Art) {
        x0 = m_layout.cluster.x + Px(wt::kThumbMin);
        x1 = m_layout.cluster.x + Px(wt::kThumbMax);
    } else {
        const float thumb = Px(11.0f);
        x0 = m_layout.volume.left + thumb / 2;
        x1 = m_layout.volume.right - thumb / 2;
    }
    return x0 + m_volume * std::max(1.0f, x1 - x0);
}

void MainWindow::ShowVolumeTip(float thumbX) {
    // Win7 tooltip: white to #E4E5F0, #767676 border, small radius, soft shadow, grey text.
    auto& host = gfx::CompositionHost::Get();
    if (!m_volumeTip) {
        m_volumeTip = host.Compositor().CreateSpriteVisual();
        m_volumeTip.Opacity(0.0f);
        m_windowRoot.Children().InsertAtTop(m_volumeTip);  // above the clipped content and frame
    }
    const std::wstring text = std::to_wstring(static_cast<int>(std::lround(m_volume * 100))) + L"%";
    auto measure = MakeText(text, 12.0f, DWRITE_FONT_WEIGHT_NORMAL, 200, 20);
    DWRITE_TEXT_METRICS tm{};
    measure->GetMetrics(&tm);
    const float shadow = 3, w = std::ceil(tm.widthIncludingTrailingWhitespace) + 14, h = 21;
    auto layout = MakeText(text, 12.0f, DWRITE_FONT_WEIGHT_NORMAL, w, h, DWRITE_TEXT_ALIGNMENT_CENTER);
    layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
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
        dc->DrawTextLayout({0, 0}, layout.get(), ink.get());
    });
    RECT client{};
    GetClientRect(m_hwnd, &client);
    const float left = std::clamp(thumbX - Px(w) / 2, 0.0f, static_cast<float>(client.right - pw));
    const float top = static_cast<float>(m_layout.volume.top - ph - PxI(2));
    m_volumeTip.Offset({std::round(left), std::round(top), 0});
    m_volumeTip.Size({static_cast<float>(pw), static_cast<float>(ph)});
    if (!m_volumeTipShown) {
        m_volumeTipShown = true;
        m_volumeTip.Opacity(0.0f);
        // Explicit 0 -> 1: AnimateOpacity starts from the current value, which can already
        // be 1 when the bubble is shown again before the fade-out finished.
        auto fade = host.Compositor().CreateScalarKeyFrameAnimation();
        fade.InsertKeyFrame(0.0f, 0.0f);
        fade.InsertKeyFrame(1.0f, 1.0f);
        fade.Duration(std::chrono::milliseconds(180));
        m_volumeTip.StartAnimation(L"Opacity", fade);
    }
}

void MainWindow::HideVolumeTip() {
    KillTimer(m_hwnd, kVolumeTipTimer);
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

bool MainWindow::IsSplit(const Widget* w) const { return w && (w->Id() == kNewPlaylist || w->Id() == kViewOptions); }

void MainWindow::UpdateSplitPart(POINT pt) {
    // Which half of the hovered split button the mouse is over; the plates repaint on change.
    Widget* w = m_pressed ? m_pressed : m_hover;
    if (!IsSplit(w)) return;
    const float split = w->Id() == kNewPlaylist ? m_newPlaylistSplit : kViewOptionsSplit;
    const int part = pt.x - w->Bounds().left >= PxI(split) ? 1 : 0;
    if (part == m_splitPart || m_pressed) return;  // keep the pressed half while held
    m_splitPart = part;
    w->Invalidate();
}

void MainWindow::RunSplitMenu(int id, const std::function<void()>& menu) {
    // While the menu is up the arrow half looks pressed and the main half grey (WMP 12).
    Widget* w = WidgetById(id);
    m_splitMenu = id;
    if (w) {
        w->Invalidate();
        w->SetPressed(true);
    }
    menu();
    m_splitMenu = 0;
    if (w) {
        w->SetPressed(false);
        w->Invalidate();
    }
    SetHoverWidget(nullptr);  // the menu loop swallowed the mouse leave
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
    if (m_taskbar && m_taskbar->OnMessage(msg, wp, lp)) return 0;
    switch (msg) {
    case WM_CREATE: {
        m_scale = GetDpiForWindow(hwnd) / 96.0f;
        InitComposition();
        CreateWidgets();
        RepaintScaleDependent();
        m_shadow.Create(hwnd);
        media::EnsureSpecialPlaylists(false);  // queue, Favorites (protected), smart playlists
        if (m_nav) m_nav->Rebuild();
        fb2k::inMainThread([this] {  // once start-up is done
            // Keep lang/strings-en.txt current for scripts/translate-deepseek.ps1: our strings
            // and foobar2000's main menu plus a song's context menu.
            metadb_handle_list sample;
            if (!m_library.Tracks().empty()) sample.add_item(m_library.Tracks().front().handle);
            ExportStrings(sample);
        });
        m_taskbar = std::make_unique<Taskbar>(hwnd);
        m_taskbar->onFavorite = [this] {
            metadb_handle_ptr now;
            if (playback_control::get()->get_now_playing(now)) media::ToggleFavorite(now);
            UpdateFavoriteState();
        };
        Taskbar::SetupJumpList();
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
        try {  // output device changed elsewhere (preferences)
            m_outputCallback = output_manager_v2::get()->addCallback([this] {
                RefreshDeviceName();
                RepaintStreamInfo();
            });
        } catch (...) {
        }
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
        if (wp == kVolumeTipTimer) {
            KillTimer(hwnd, kVolumeTipTimer);
            if (m_volumeHover) ShowVolumeTip(VolumeThumbX());
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
            if (NowPlayingPartAt(pt)) {  // now-playing links, output device
                SetCursor(LoadCursorW(nullptr, IDC_HAND));
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
            UpdateSplitPart(pt);
            m_list->OnMouseMove(pt);
            m_nav->OnMouseMove(pt);
            UpdateCrumbHover(pt);
            UpdateNowPlayingHover(pt);
            const bool overVolume = PtInRect(&m_layout.volume, pt) != FALSE;
            if (overVolume) {
                if (!m_volumeHover) {
                    m_volumeHover = true;
                    SetTimer(hwnd, kVolumeTipTimer, kVolumeTipDelayMs, nullptr);
                }
            } else if (m_volumeHover) {
                m_volumeHover = false;
                HideVolumeTip();
            }
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
            UpdateNowPlayingHover({-1, -1});
            m_volumeHover = false;
            HideVolumeTip();
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
        if (const int part = NowPlayingPartAt(pt)) {
            OnNowPlayingClick(part);
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
            FocusNav();
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
            if (w->Id() == kVolume) {
                m_volumeHover = PtInRect(&m_layout.volume, pt) != FALSE;
                if (m_volumeHover) ShowVolumeTip(VolumeThumbX());
                else HideVolumeTip();
            }
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
            m_volumeHover = false;
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
        if (PtInRect(&m_layout.volume, pt)) {  // the volume slider itself, not the whole band
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
            if (item && (item->target == NavPane::Target::Music || item->target == NavPane::Target::Category)) {
                ShowNavCustomizeMenu(screen);
                return 0;
            }
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
        m_outputCallback.release();
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
