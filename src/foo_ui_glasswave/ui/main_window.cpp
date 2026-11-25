#include "pch.h"
#include "ui/main_window_parts.h"

namespace glasswave::ui {

using namespace mainwin;

namespace mainwin {

cfg_var_modern::cfg_string cfg_navCategories(
    {0x6a1f3c5d, 0x2b7e, 0x4d90, {0x8c, 0x15, 0x3e, 0x9a, 0x7b, 0x2d, 0x4f, 0x61}}, "012");

cfg_var_modern::cfg_int cfg_navSmartAt(
    {0x6a1f3c64, 0x2b7e, 0x4d90, {0x8c, 0x15, 0x3e, 0x9a, 0x7b, 0x2d, 0x4f, 0x61}}, 0);

cfg_var_modern::cfg_int cfg_lyricsOpen(
    {0x6a1f3c68, 0x2b7e, 0x4d90, {0x8c, 0x15, 0x3e, 0x9a, 0x7b, 0x2d, 0x4f, 0x61}}, 0);

} // namespace mainwin

namespace {

constexpr wchar_t kClassName[] = L"Glasswave.MainWindow";
constexpr UINT_PTR kLibraryTimer = 1;
constexpr UINT_PTR kVolumeTipTimer = 2;
constexpr UINT_PTR kScrollAnimTimer = 3;
constexpr UINT_PTR kScrollRepeatTimer = 4;
constexpr UINT_PTR kSuggestTimer = 7; // main_window_search.cpp: typing paused, ask for suggestions
constexpr UINT_PTR kWindowOpenTimer = 9;  // the opening animation ended: the shadow joins
constexpr UINT_PTR kWindowCloseTimer = 10; // the closing animation ended: foobar2000 exits
// Windows 7's window opening and closing (DWM): the window grows to its size from a little smaller
// while it fades in, and shrinks a little as it fades out. Estimated from the owner's memory of
// it, not measured yet (docs/design/visual-spec.md "Window open and close").
constexpr float kOpenFrom = 0.92f, kCloseTo = 0.94f;
constexpr int kOpenMs = 250, kCloseMs = 180;
constexpr float kTabBand = 44.0f;     // DIP: the page's tabs over the list (main_window_search.cpp)
constexpr float kNavPaneWidth = 190.0f;

bool IsCaptionWidget(int id) {
    return id >= kCapMin && id <= kCapClose;
}

// Split buttons: arrow half width, and the view options button's icon half (DIP).
constexpr float kSplitArrow = 16, kViewOptionsSplit = 26;

// {0C9E7B42-1F3A-4D5E-8B6C-7A2F9E1D4C83}
cfg_var_modern::cfg_struct_t<WINDOWPLACEMENT> cfg_placement(
    {0x0c9e7b42, 0x1f3a, 0x4d5e, {0x8b, 0x6c, 0x7a, 0x2f, 0x9e, 0x1d, 0x4c, 0x83}});

// Static navigation sections, in order: root and the library child groups.
// {6A1F3C67-2B7E-4D90-8C15-3E9A7B2D4F61}
cfg_var_modern::cfg_string cfg_navOrder(
    {0x6a1f3c67, 0x2b7e, 0x4d90, {0x8c, 0x15, 0x3e, 0x9a, 0x7b, 0x2d, 0x4f, 0x61}}, "");

// The lyrics button's glyph: a speech bubble with a quotation mark (as in Apple Music).
void DrawLyricsGlyph(ID2D1DeviceContext* dc, D2D1_SIZE_F s, ID2D1Brush* brush) {
    com_ptr<ID2D1Factory> f;
    dc->GetFactory(f.put());
    const float cx = std::round(s.width / 2), cy = std::round(s.height / 2);
    const float l = cx - 7.5f, t = cy - 6.5f, r = cx + 7.5f, b = cy + 4.0f, rad = 3;
    com_ptr<ID2D1PathGeometry> path;
    if (FAILED(f->CreatePathGeometry(path.put()))) return;
    com_ptr<ID2D1GeometrySink> sink;
    if (FAILED(path->Open(sink.put()))) return;
    const auto arc = [&](float x, float y) {
        sink->AddArc({{x, y}, {rad, rad}, 0, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL});
    };
    sink->BeginFigure({l + rad, t}, D2D1_FIGURE_BEGIN_HOLLOW);
    sink->AddLine({r - rad, t});
    arc(r, t + rad);
    sink->AddLine({r, b - rad});
    arc(r - rad, b);
    sink->AddLine({l + 6.5f, b});
    sink->AddLine({l + 2.5f, b + 3.5f}); // tail
    sink->AddLine({l + rad, b});
    arc(l, b - rad);
    sink->AddLine({l, t + rad});
    arc(l + rad, t);
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    sink->Close();
    dc->DrawGeometry(path.get(), brush, 1.3f);
    for (const float x : {cx - 2.2f, cx + 2.4f}) {
        dc->FillEllipse(D2D1::Ellipse({x, cy - 1.9f}, 1.35f, 1.35f), brush);
        dc->DrawLine({x + 1.1f, cy - 1.6f}, {x - 0.6f, cy + 1.2f}, brush, 1.1f);
    }
}

std::vector<media::GroupKind> LoadNavCategories() {
    std::vector<media::GroupKind> out;
    for (char c : std::string(cfg_navCategories.get().c_str()))
        if (c >= '0' && c < '0' + static_cast<int>(media::kGroupKinds)) {
            const auto kind = static_cast<media::GroupKind>(c - '0');
            if (std::find(out.begin(), out.end(), kind) == out.end()) out.push_back(kind);
        }
    return out;
}

std::wstring LyricsSettingsSignature() {
    std::wstring signature;
    auto add = [&signature](std::wstring_view value) {
        signature += value;
        signature.push_back(L'\x1f');
    };
    for (const auto& field : lyrics::scripts::SettingFields()) {
        if (field.kind != settings::Field::Kind::Choice && field.kind != settings::Field::Kind::Text)
            continue;
        add(field.label);
        add(field.get ? std::to_wstring(field.get()) : field.getText());
    }
    // Reloading a script replaces its registered object and clears cached automatic results,
    // even when its visible name and settings stay the same.
    for (const auto& source : lyrics::Searchers()) {
        add(source->Name());
        add(std::to_wstring(reinterpret_cast<std::uintptr_t>(source.get())));
    }
    for (const auto* provider : stream::Providers()) {
        const std::wstring locale = provider->LyricsLocale();
        if (!locale.empty()) {
            add(provider->Key());
            add(locale);
        }
    }
    return signature;
}

stream::NavigationSection CreatedPlaylistSection(const stream::Provider& provider) {
    if (const auto section =
            FindProviderSection(provider, stream::SectionKind::PlaylistShelf, stream::Shelf::Created))
        return *section;
    return {L"created", L"Created playlists", stream::SectionKind::PlaylistShelf, stream::Shelf::Created,
            false};
}

// A horizontally stretchable strip: painted once a few pixels wide, stretched to any width.
wuc::CompositionSurfaceBrush Strip(int hPx, float scale,
                                   const std::function<void(ID2D1DeviceContext*, float)>& painter) {
    auto& host = gfx::CompositionHost::Get();
    auto surface = host.CreateSurface(4, hPx);
    host.Draw(surface, scale, [&](ID2D1DeviceContext* dc) { painter(dc, hPx / scale); });
    auto brush = host.Compositor().CreateSurfaceBrush(surface);
    brush.Stretch(wuc::CompositionStretch::Fill);
    return brush;
}

void FillLine(ID2D1DeviceContext* dc, float y, float h, uint32_t color) {
    com_ptr<ID2D1SolidColorBrush> b;
    dc->CreateSolidColorBrush(Hex(color), b.put());
    dc->FillRectangle({-1, y, 100, y + h}, b.get());
}

RECT CenteredRect(int cx, int cy, int w, int h) {
    return {cx - w / 2, cy - h / 2, cx - w / 2 + w, cy - h / 2 + h};
}

com_ptr<IWICBitmapSource> LoadAppIcon(int sizePx) {
    HICON icon = CreateAppIcon(sizePx);
    if (!icon) return nullptr;
    auto* wic = gfx::CompositionHost::Get().Wic();
    com_ptr<IWICBitmap> bmp;
    com_ptr<IWICFormatConverter> conv;
    com_ptr<IWICBitmapSource> result;
    if (SUCCEEDED(wic->CreateBitmapFromHICON(icon, bmp.put())) &&
        SUCCEEDED(wic->CreateFormatConverter(conv.put())) &&
        SUCCEEDED(conv->Initialize(bmp.get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr,
                                   0, WICBitmapPaletteTypeMedianCut)))
        result = conv;
    DestroyIcon(icon);
    return result;
}

// Volume slider position <-> foobar2000 volume in dB (-100..0); 40*log10 feels even.
float SliderFromDb(float db) {
    return db <= -99.0f ? 0.0f : std::clamp(std::pow(10.0f, db / 40.0f), 0.0f, 1.0f);
}

bool KeyDown(int vk) {
    return (GetKeyState(vk) & 0x8000) != 0;
}

// Glasswave owns predictable shuffle/repeat scheduling; the core owns decoding and output.
// {8D2F6A13-4B7C-4E19-9F3A-2C6E8B1D5A70} / {8D2F6A14-...}
cfg_var_modern::cfg_int cfg_shuffle(
    {0x8d2f6a13, 0x4b7c, 0x4e19, {0x9f, 0x3a, 0x2c, 0x6e, 0x8b, 0x1d, 0x5a, 0x70}}, 0);
cfg_var_modern::cfg_int cfg_repeat(
    {0x8d2f6a14, 0x4b7c, 0x4e19, {0x9f, 0x3a, 0x2c, 0x6e, 0x8b, 0x1d, 0x5a, 0x70}}, 0);

} // namespace

// ---- Core callbacks ----------------------------------------------------------------------

struct MainWindow::PlayCallback : play_callback_impl_base {
    explicit PlayCallback(MainWindow& w)
        : play_callback_impl_base(flag_on_playback_starting | flag_on_playback_new_track |
                                  flag_on_playback_stop | flag_on_playback_pause | flag_on_playback_edited |
                                  flag_on_playback_seek | flag_on_playback_time |
                                  flag_on_playback_dynamic_info | flag_on_playback_dynamic_info_track |
                                  flag_on_volume_change),
          window(w) {}

    void on_playback_starting(play_control::t_track_command command, bool) override {
        window.OnSwitching(true);
        window.PreviewStartingTrack(command);
        window.OnPlaybackStateChanged();
    }
    void on_playback_new_track(metadb_handle_ptr track) override {
        if (track.is_valid() && !stream::CanPlaySource(track->get_path())) return;
        window.OnSwitching(false, track);
        media::PlayStats::Get().Record(track);
        window.RefreshPlayCounts();
        window.OnTrackChanged(true);
        window.OnPlaybackStateChanged();
    }
    void on_playback_stop(play_control::t_stop_reason reason) override {
        if (reason == play_control::stop_reason_starting_another) {
            window.OnSwitching(true);
            return;
        }
        window.OnSwitching(false);
        window.OnPlaybackStateChanged();
        window.ClearShownTrack(); // stopped: nothing plays, nothing is shown as playing
        window.OnTrackChanged();
    }
    void on_playback_pause(bool) override { window.OnPlaybackStateChanged(); }
    void on_playback_seek(double time) override { window.OnPlaybackSeek(time); }
    void on_playback_time(double) override { window.OnPlaybackTime(); }
    void on_playback_edited(metadb_handle_ptr track) override {
        window.OnTrackChanged();
        window.RequestLyrics(track, false);
    }
    void on_playback_dynamic_info(const file_info&) override { window.OnTrackChanged(); }
    void on_playback_dynamic_info_track(const file_info&) override { window.OnTrackChanged(); }
    void on_volume_change(float) override { window.OnVolumeChanged(); }

    MainWindow& window;
};

struct MainWindow::PlaylistCallback : playlist_callback_impl_base {
    explicit PlaylistCallback(MainWindow& w)
        : playlist_callback_impl_base(static_cast<t_uint32>(flag_all)), window(w) {}

    void on_items_added(t_size p, t_size, metadb_handle_list_cref, const pfc::bit_array&) override {
        window.OnPlaylistContentChanged(p);
    }
    void on_items_reordered(t_size p, const t_size*, t_size) override { window.OnPlaylistContentChanged(p); }
    void on_items_removed(t_size p, const pfc::bit_array&, t_size, t_size) override {
        window.OnPlaylistContentChanged(p);
    }
    void on_items_modified(t_size p, const pfc::bit_array&) override { window.OnPlaylistContentChanged(p); }
    void on_items_replaced(t_size p, const pfc::bit_array&,
                           const pfc::list_base_const_t<t_on_items_replaced_entry>&) override {
        window.OnPlaylistContentChanged(p);
    }
    void on_items_selection_change(t_size p, const pfc::bit_array&, const pfc::bit_array&) override {
        window.OnPlaylistSelectionChanged(p);
    }
    void on_item_focus_change(t_size p, t_size, t_size) override { window.OnPlaylistSelectionChanged(p); }
    // Views hold playlist indices; every change of the set moves them along.
    void on_playlist_created(t_size p, const char*, t_size) override {
        window.OnPlaylistsMoved([p](size_t i) { return i >= p ? i + 1 : i; });
    }
    void on_playlist_locked(t_size, bool) override { window.OnPlaylistLockChanged(); }
    void on_playlists_reorder(const t_size* order, t_size count) override {
        std::vector<size_t> moved(count);
        for (t_size to = 0; to < count; ++to)
            moved[order[to]] = to; // order[new] = old
        window.OnPlaylistsMoved([&moved](size_t i) { return i < moved.size() ? moved[i] : i; });
    }
    void on_playlists_removed(const pfc::bit_array& mask, t_size oldCount, t_size) override {
        std::vector<size_t> moved(oldCount, SIZE_MAX);
        for (t_size i = 0, next = 0; i < oldCount; ++i)
            if (!mask[i]) moved[i] = next++;
        window.OnPlaylistsMoved([&moved](size_t i) { return i < moved.size() ? moved[i] : SIZE_MAX; });
    }
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

// Provider notifications, delivered on the main thread while the window exists.
struct MainWindow::ProviderListener : stream::Provider::Listener {
    explicit ProviderListener(MainWindow& owner) : window(owner) {
        for (auto* provider : stream::Providers())
            provider->AddListener(this);
    }
    ~ProviderListener() override {
        for (auto* provider : stream::Providers())
            provider->RemoveListener(this);
    }
    void OnAccountObserved(stream::Provider& provider) override {
        window.OnProviderAccountObserved(provider);
    }
    void OnAccountChanged(stream::Provider& provider) override { window.OnProviderAccount(provider); }
    void OnLibraryChanged(stream::Provider& provider) override { window.OnProviderLibrary(provider); }
    void OnFavoritesChanged(stream::Provider& provider) override { window.OnProviderFavorites(provider); }
    void OnLanguageChanged(stream::Provider& provider) override { window.OnProviderLanguage(provider); }
    void OnDisplayChanged(stream::Provider& provider) override { window.OnProviderDisplay(provider); }
    MainWindow& window;
};

namespace {
MainWindow* g_instance = nullptr;

class CoreQueueViewEvents : public playback_queue_callback {
  public:
    void on_changed(t_change_origin) override {
        if (auto* window = MainWindow::Instance()) window->OnCoreQueueChanged();
    }
};
service_factory_single_t<CoreQueueViewEvents> g_coreQueueViewEvents;

// View > Lyrics: shows or hides the lyrics pane (bindable to a keyboard shortcut).
class LyricsCommand : public mainmenu_commands {
  public:
    // {6A1F3C69-2B7E-4D90-8C15-3E9A7B2D4F61}
    // {6A1F3C69-2B7E-4D90-8C15-3E9A7B2D4F61}, {6A1F3C6A-2B7E-4D90-8C15-3E9A7B2D4F61}
    static constexpr GUID kLyrics{
        0x6a1f3c69, 0x2b7e, 0x4d90, {0x8c, 0x15, 0x3e, 0x9a, 0x7b, 0x2d, 0x4f, 0x61}};
    static constexpr GUID kUpNext{
        0x6a1f3c6a, 0x2b7e, 0x4d90, {0x8c, 0x15, 0x3e, 0x9a, 0x7b, 0x2d, 0x4f, 0x61}};
    static constexpr GUID kMini{
        0x6a1f3c6b, 0x2b7e, 0x4d90, {0x8c, 0x15, 0x3e, 0x9a, 0x7b, 0x2d, 0x4f, 0x61}};
    static constexpr GUID kFullScreen{
        0x6a1f3c6c, 0x2b7e, 0x4d90, {0x8c, 0x15, 0x3e, 0x9a, 0x7b, 0x2d, 0x4f, 0x61}};
    t_uint32 get_command_count() override { return 4; }
    GUID get_command(t_uint32 index) override {
        return index == 0 ? kLyrics : index == 1 ? kUpNext : index == 2 ? kMini : kFullScreen;
    }
    void get_name(t_uint32 index, pfc::string_base& out) override {
        static const char* const names[] = {"Lyrics", "Up Next", "Mini player", "Full screen"};
        out = names[index < 4 ? index : 0];
    }
    bool get_description(t_uint32 index, pfc::string_base& out) override {
        static const char* const texts[] = {"Shows or hides the Glasswave lyrics pane.",
                                            "Shows the queue and history in the Glasswave pane.",
                                            "Switches Glasswave to its mini player.",
                                            "Shows what is playing on the whole screen."};
        out = texts[index < 4 ? index : 0];
        return true;
    }
    GUID get_parent() override { return mainmenu_groups::view; }
    bool get_display(t_uint32 index, pfc::string_base& text, t_uint32& flags) override {
        const bool on = g_instance && (index == 0   ? g_instance->LyricsPaneOpen()
                                       : index == 1 ? g_instance->QueuePaneShown()
                                                    : g_instance->PlayerShown(index == 3));
        flags = on ? flag_checked : 0;
        get_name(index, text);
        return g_instance != nullptr;
    }
    void execute(t_uint32 index, service_ptr_t<service_base>) override {
        if (!g_instance) return;
        if (index == 0)
            g_instance->ToggleLyricsPane();
        else if (index == 1)
            g_instance->ToggleQueuePane();
        else
            g_instance->ShowPlayer(index == 3);
    }
};
mainmenu_commands_factory_t<LyricsCommand> g_lyricsCommand;
} // namespace

MainWindow* MainWindow::Instance() {
    return g_instance;
}

MainWindow::MainWindow() {
    g_instance = this;
    stream::SetPlaybackOpeningListener([](const std::string& path) {
        if (g_instance) g_instance->OnTrackOpening(path);
    });
}

MainWindow::~MainWindow() {
    if (g_instance == this) {
        g_instance = nullptr;
        stream::SetPlaybackOpeningListener({});
    }
    if (m_hwnd) DestroyWindow(m_hwnd);
}

HWND MainWindow::Create(user_interface::HookProc_t hook) {
    stream::StartProviders(); // the UI can come up before foobar2000's initquit services
    m_hook = hook;
    const HINSTANCE inst = core_api::get_my_instance();

    WNDCLASSEXW wc{sizeof(wc)};
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = &MainWindow::WndProcThunk;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = ui_control::get()->get_main_icon();
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc); // failing because it already exists is fine

    const float sys = GetDpiForSystem() / 96.0f;
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    const int w = static_cast<int>(1000 * sys), h = static_cast<int>(680 * sys);
    const int x = work.left + (work.right - work.left - w) / 2,
              y = work.top + (work.bottom - work.top - h) / 2;

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
    const bool animate = WindowAnimations();
    if (animate) {
        m_windowRoot.Opacity(0.0f); // the first frame is already the animation's
        m_shadow.SetSuppressed(true);
    }
    ShowWindow(m_hwnd, show);
    if (animate) AnimateWindow(true);
    m_shadow.Sync();
    return m_hwnd;
}

bool MainWindow::WindowAnimations() {
    BOOL on = TRUE;
    SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &on, 0);
    return on != FALSE;
}

// The window's content grows (or shrinks) about its middle while it fades; the shadow window
// is out of the way meanwhile and comes back once the window stands at its size.
void MainWindow::AnimateWindow(bool open) {
    if (!m_windowRoot) return;
    RECT client{};
    GetClientRect(m_hwnd, &client);
    const auto& comp = gfx::CompositionHost::Get().Compositor();
    m_windowRoot.CenterPoint(float3{client.right / 2.0f, client.bottom / 2.0f, 0});
    const auto ease = open ? gfx::Easing(gfx::curve::kFastOut)
                           : gfx::Easing({0.4f, 0.0f, 1.0f, 1.0f});
    const auto duration = std::chrono::milliseconds(open ? kOpenMs : kCloseMs);
    auto scale = comp.CreateVector3KeyFrameAnimation();
    const float from = open ? kOpenFrom : 1.0f, to = open ? 1.0f : kCloseTo;
    scale.InsertKeyFrame(0.0f, float3{from, from, 1});
    scale.InsertKeyFrame(1.0f, float3{to, to, 1}, ease);
    scale.Duration(duration);
    m_windowRoot.StartAnimation(L"Scale", scale);
    auto fade = comp.CreateScalarKeyFrameAnimation();
    fade.InsertKeyFrame(0.0f, open ? 0.0f : 1.0f);
    fade.InsertKeyFrame(1.0f, open ? 1.0f : 0.0f, ease);
    fade.Duration(duration);
    m_windowRoot.StartAnimation(L"Opacity", fade);
    if (!open) {
        m_shadow.SetSuppressed(true);
        m_shadow.Sync();
    }
    SetTimer(m_hwnd, open ? kWindowOpenTimer : kWindowCloseTimer, open ? kOpenMs : kCloseMs, nullptr);
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
    return GuardedWindowProc(hwnd, msg, wp, lp, [&] { return self->WndProc(msg, wp, lp); });
}

// ---- Composition visual tree ---------------------------------------------------------

void MainWindow::InitComposition() {
    m_frame = std::make_unique<aero::GlassWindow>(m_hwnd, WindowColorization(), aero::atlas::Available());
    m_windowRoot = m_frame->Root();
    m_root = m_frame->Clipped();

    auto& host = gfx::CompositionHost::Get();
    const auto& comp = host.Compositor();
    auto children = m_root.Children();
    // The content base stops at the library boundary. The shared edge also surrounds
    // the now-playing pane, whose glass must remain visible through its dark tint.
    m_clientBackground = comp.CreateSpriteVisual();
    m_clientBackground.Brush(
        comp.CreateColorBrush(winrt::Windows::UI::ColorHelper::FromArgb(255, 0xF1, 0xF7, 0xFD)));
    children.InsertAtTop(m_clientBackground);

    for (auto* v : {&m_clientEdge, &m_navBar, &m_cmdBar, &m_contentTop, &m_transportBand, &m_capsule,
                    &m_cluster, &m_divider, &m_clientGlow, &m_title, &m_nowPlaying, &m_breadcrumb,
                    &m_navGroove, &m_time, &m_streamVisual})
        *v = comp.CreateSpriteVisual();
    m_crumbBar = std::make_unique<Breadcrumb>([this] { StartScrollAnimation(); });
    m_crumbBar->SetVisual(m_breadcrumb);
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
            win7::DrawStretched(
                dc, win7::Module::Wmploc, L"LIBRARY_BACKGROUND7.PNG",
                D2D1::RectF(0, 0, static_cast<float>(bs.width), static_cast<float>(bs.height)));
        });
        auto brush = comp.CreateSurfaceBrush(surface);
        brush.Stretch(wuc::CompositionStretch::UniformToFill);
        brush.HorizontalAlignmentRatio(0.0f);
        brush.VerticalAlignmentRatio(0.0f);
        m_libraryBg.Brush(brush);
    }
    m_contentLayer.Children().InsertAtBottom(m_libraryBg);
    for (const auto& v : {m_capsule, m_cluster, m_divider, m_clientGlow, m_title, m_nowPlaying, m_breadcrumb,
                          m_time, m_streamVisual})
        children.InsertAtTop(v);
    m_npLyric = comp.CreateSpriteVisual();
    children.InsertAtTop(m_npLyric);
    m_win7Art = wt::Available();
    m_aeroArt = aero::atlas::Available();
    m_divider.IsVisible(!m_win7Art); // part of the original capsule bitmap
    m_cluster.IsVisible(m_win7Art);

    m_detailHeader = comp.CreateSpriteVisual();
    m_detailHeader.IsVisible(false);
    m_contentLayer.Children().InsertAtTop(m_detailHeader);
    m_detailArtToken = ArtCache::Get().Subscribe([this] {
        if (m_detail.visible) PaintDetailHeader();
        RefreshNowPlayingArt();
    });
    m_radioToken = media::radio::Subscribe([this] { OnRadioChanged(); });
    m_nav = std::make_unique<NavPane>(m_contentLayer);
    m_nav->SetCategories(LoadNavCategories());
    m_nav->SetSmartPosition(static_cast<int>(cfg_navSmartAt.get()));
    m_nav->SetMoveCallback([this](const NavPane::Item& dragged, const NavPane::Item& target, bool after) {
        OnNavMove(dragged, target, after);
    });
    m_nav->SetStaticOrder(pfc::stringcvt::string_wide_from_utf8(cfg_navOrder.get()).get_ptr());
    m_nav->SetOrderCallback([](std::wstring text) {
        cfg_navOrder.set(pfc::stringcvt::string_utf8_from_wide(text.c_str()).get_ptr());
    });
    m_providerListener = std::make_unique<ProviderListener>(*this);
    RefreshProviderSections();
    m_discsToken = cd::AddDiscsListener([this] { OnDiscsChanged(); });
    OnDiscsChanged();
    SetContextActionProvider([this](metadb_handle_list_cref items) { return BuildContextActions(items); });
    m_list = std::make_unique<ListView>(m_contentLayer);
    m_list->onLink = [this](size_t group, size_t line) { OnAlbumLink(group, line); };
    m_list->onNearEnd = [this] {
        if (m_page) m_page->NearEnd();
    };
    m_list->onEmptyContext = [this](POINT screen) { ShowPageContextMenu(screen); };
    m_list->onSourceChanged = [this] { ApplyPendingScroll(); };
    m_list->onPresentationChanged = [this] {
        if (m_page) m_page->OnPresentationChanged();
    };
    m_list->onScrollAnim = [this] { StartScrollAnimation(); };
    m_list->onScrollHold = [this] { StartScrollRepeat(); };
    m_search = std::make_unique<SearchBox>(m_contentLayer);
    m_search->onChange = [this](const std::wstring& text) { OnSearchTextChanged(text); };
    m_search->onTokenRemoved = [this] { SetSearchScope({}); };
    // A playlist's songs played from its page: the playlist goes to Recently Played's Playlists.
    media::SetPlayQueueListener([this] {
        if (m_view.kind == View::Kind::RemotePlaylist)
            media::RecordRecentList(m_view.provider, m_view.remoteId, m_view.name);
        else if (m_view.kind == View::Kind::Playlist && !media::IsQueue(m_view.playlist))
            media::RecordRecentList({}, m_view.name, m_view.name);
    });
    m_search->onCompose = [this] { OnSearchTextChanged(m_search->Text()); };
    m_paneSlot = gfx::CompositionHost::Get().Compositor().CreateContainerVisual();
    m_paneSlot.RelativeSizeAdjustment(float2{1, 1});
    m_paneSlot.Comment(L"pane slot");
    m_root.Children().InsertAtTop(m_paneSlot);
    m_pane = std::make_unique<NowPlayingPane>(m_paneSlot);
    m_pane->onFrames = [this] { StartLyricsFrames(); };
    m_pane->onSeek = [](double seconds) {
        // Like Apple Music: playback continues from the clicked line.
        auto pc = playback_control::get();
        if (!pc->is_playing()) return;
        pc->playback_seek(seconds);
        if (pc->is_paused()) pc->pause(false);
    };
    m_pane->onLyricsMenu = [this](POINT screen) { ShowLyricsMenu(screen); };
    m_pane->onQueueShown = [this] { RefreshPaneQueue(); };
    ConnectPaneQueue();
    m_lyricsOpen = cfg_lyricsOpen.get() != 0;
    m_lyricsSettingsSignature = LyricsSettingsSignature();
}

void MainWindow::CreateWidgets() {
    auto add = [&](int id, Widget::Painter painter, Widget::Options opts) -> Widget* {
        auto w = std::make_unique<Widget>(id, std::move(painter), std::move(opts));
        m_root.Children().InsertAtTop(w->Visual());
        m_widgets.push_back(std::move(w));
        return m_widgets.back().get();
    };

    // Caption button cluster: Win7's own DWM images when aero.msstyles is present.
    add(kCapMin, CaptionPainter(CaptionButton::Minimize, m_aeroArt), CaptionOptions(CaptionButton::Minimize, m_aeroArt))
        ->onClick = [this] { ShowWindow(m_hwnd, SW_MINIMIZE); };
    add(kCapMax, CaptionPainter(CaptionButton::Maximize, m_aeroArt, [this] { return IsWindowMaximized(); }),
        CaptionOptions(CaptionButton::Maximize, m_aeroArt))
        ->onClick = [this] { ShowWindow(m_hwnd, IsWindowMaximized() ? SW_RESTORE : SW_MAXIMIZE); };
    add(kCapClose, CaptionPainter(CaptionButton::Close, m_aeroArt), CaptionOptions(CaptionButton::Close, m_aeroArt))
        ->onClick = [this] { PostMessageW(m_hwnd, WM_SYSCOMMAND, SC_CLOSE, 0); };

    // Navigation bar
    Widget::Options round;
    round.circular = true;
    auto* back = add(
        kBack,
        [this](auto* dc, D2D1_SIZE_F s, Layer l) {
            if (m_win7Art)
                wt::NavOrb(dc, s, l, true, m_historyPos > 0);
            else
                wp::NavArrow(dc, s, l, true, m_historyPos > 0);
        },
        round);
    back->onClick = [this] { GoBack(); };
    back->enabled = [this] { return m_historyPos > 0; };
    auto* forward = add(
        kForward,
        [this](auto* dc, D2D1_SIZE_F s, Layer l) {
            const bool enabled = m_historyPos + 1 < m_history.size();
            if (m_win7Art)
                wt::NavOrb(dc, s, l, false, enabled);
            else
                wp::NavArrow(dc, s, l, false, enabled);
        },
        round);
    forward->onClick = [this] { GoForward(); };
    forward->enabled = [this] { return m_historyPos + 1 < m_history.size(); };

    // Command bar
    add(kOrganize, [](auto* dc, D2D1_SIZE_F s, Layer l) { wp::CommandButton(dc, s, l, L"Organize", true); },
        {})
        ->onClick = [this] {
        // Pressed while its menu is up, like WMP's drop-down buttons.
        Widget* w = WidgetById(kOrganize);
        if (w) w->SetPressed(true);
        ShowOrganizeMenu();
        if (w) w->SetPressed(false);
    };
    add(kAddFiles, [](auto* dc, D2D1_SIZE_F s, Layer l) { wp::CommandButton(dc, s, l, L"Add files", false); },
        {})
        ->onClick = [this] {
        ActivateAddTarget();
        standard_commands::main_add_files();
    };
    add(kAddFolder,
        [](auto* dc, D2D1_SIZE_F s, Layer l) { wp::CommandButton(dc, s, l, L"Add folder", false); }, {})
        ->onClick = [this] {
        ActivateAddTarget();
        standard_commands::main_add_directory();
    };
    // The page's command at the command bar's right (an album's or playlist's Collect); Refresh
    // left the bar (F5 and the page's menu still refresh).
    add(kRefresh,
        [this](auto* dc, D2D1_SIZE_F s, Layer l) {
            wp::CommandButton(dc, s, l, L"", false); // plate only
            const std::wstring label = m_detail.saveLabel;
            auto layout = MakeText(label, 12.0f, DWRITE_FONT_WEIGHT_NORMAL, s.width, s.height,
                                   DWRITE_TEXT_ALIGNMENT_CENTER);
            com_ptr<ID2D1SolidColorBrush> ink;
            dc->CreateSolidColorBrush(Hex(0x1E395B), ink.put());
            dc->DrawTextLayout({0, 0}, layout.get(), ink.get());
        },
        {})
        ->onClick = [this] {
        if (m_page) m_page->SaveAction();
    };
    if (Widget* save = WidgetById(kRefresh)) save->enabled = [this] { return !m_detail.saveBusy; };
    // Play on a local album's or playlist's command bar (they have no detail band): the green
    // triangle and the label, as the band's Play draws them.
    add(kLocalPlay,
        [](auto* dc, D2D1_SIZE_F s, Layer l) {
            wp::CommandButton(dc, s, l, L"", false); // plate only
            auto layout = MakeText(Tr(L"Play"), 12.0f, DWRITE_FONT_WEIGHT_NORMAL, s.width, s.height);
            DWRITE_TEXT_METRICS tm{};
            layout->GetMetrics(&tm);
            const float icon = 14.0f, gap = 5.0f;
            const float x = std::round((s.width - tm.width - icon - gap) / 2);
            const float top = std::round((s.height - icon) / 2);
            wp::GreenPlay(dc, {x, top, x + icon, top + icon});
            com_ptr<ID2D1SolidColorBrush> ink;
            dc->CreateSolidColorBrush(Hex(0x1E395B), ink.put());
            dc->DrawTextLayout({x + icon + gap, 0}, layout.get(), ink.get());
        },
        {})
        ->onClick = [this] {
        if (!m_source) return;
        metadb_handle_list items; // the list as shown, filtered and sorted
        for (size_t row = 0; row < m_source->Count(); ++row)
            if (const auto track = m_source->Item(row); track.is_valid()) items.add_item(track);
        if (items.get_count()) media::PlayQueue(items, 0); // a playlist is recorded as played
    };

    // Download all, right of Collect: the page's songs go to Downloads.
    add(kDownloadAll,
        [this](auto* dc, D2D1_SIZE_F s, Layer l) {
            wp::CommandButton(dc, s, l, L"", false); // plate only
            auto layout = MakeText(m_detail.downloadLabel, 12.0f, DWRITE_FONT_WEIGHT_NORMAL, s.width, s.height,
                                   DWRITE_TEXT_ALIGNMENT_CENTER);
            com_ptr<ID2D1SolidColorBrush> ink;
            dc->CreateSolidColorBrush(Hex(0x1E395B), ink.put());
            dc->DrawTextLayout({0, 0}, layout.get(), ink.get());
        },
        {})
        ->onClick = [this] {
        if (m_page) m_page->DownloadAction();
    };
    // Split buttons (WMP 12): one control; the main part acts at once, the arrow part opens a
    // menu. m_splitPart says which half the mouse is over.
    add(kViewOptions,
        [this](auto* dc, D2D1_SIZE_F s, Layer l) {
            const float split = kViewOptionsSplit;
            if (!wp::SplitButtonPlate(dc, s, l, split, m_splitPart, m_splitMenu == kViewOptions))
                wp::CommandButton(dc, s, l, L"", true);
            wp::ViewOptionsGlyph(dc, {split, s.height},
                                 m_source ? m_source->GetViewStyle() : media::ViewStyle::Details);
        },
        {})
        ->onClick = [this] {
        if (m_splitPart == 1)
            RunSplitMenu(kViewOptions, [this] { ShowViewMenu(); });
        else
            CycleViewStyle();
    };
    add(kNewPlaylist,
        [this](auto* dc, D2D1_SIZE_F s, Layer l) {
            if (!wp::SplitButtonPlate(dc, s, l, m_newPlaylistSplit, m_splitPart, m_splitMenu == kNewPlaylist))
                wp::CommandButton(dc, s, l, L"", true);
            wp::CommandButton(dc, {m_newPlaylistSplit, s.height}, Layer::Normal, L"Create playlist",
                              false); // text only
        },
        {})
        ->onClick = [this] {
        if (m_splitPart == 1)
            RunSplitMenu(kNewPlaylist, [this] {
                POINT pt{m_layout.newPlaylist.left, m_layout.newPlaylist.bottom};
                ClientToScreen(m_hwnd, &pt);
                auto add = AddPlaylistActions();
                ShowActionMenu(m_hwnd, pt, add.front().children); // new playlist / smart / presets
            });
        else
            CreatePlaylist(false);
    };

    // Detail band: Follow / Following, or Play with a green triangle, centred in the button.
    add(kDetailAction,
        [this](auto* dc, D2D1_SIZE_F s, Layer l) {
            const std::wstring label = m_detail.actionLabel.empty() ? Tr(L"Collect") : m_detail.actionLabel;
            wp::CommandButton(dc, s, l, L"", false); // plate only
            auto layout = MakeText(label, 12.0f, DWRITE_FONT_WEIGHT_NORMAL, s.width, s.height);
            DWRITE_TEXT_METRICS tm{};
            layout->GetMetrics(&tm);
            const bool play = m_detail.action == DetailInfo::Action::Play;
            const float icon = play ? 14.0f : 0.0f, gap = play ? 5.0f : 0.0f;
            const float x = std::round((s.width - tm.width - icon - gap) / 2);
            const float iconTop = std::round((s.height - icon) / 2);
            if (play) wp::GreenPlay(dc, {x, iconTop, x + icon, iconTop + icon});
            com_ptr<ID2D1SolidColorBrush> ink;
            dc->CreateSolidColorBrush(Hex(0x1E395B), ink.put());
            dc->DrawTextLayout({x + icon + gap, 0}, layout.get(), ink.get()); // the layout centres it vertically
        },
        {})
        ->onClick = [this] { OnDetailAction(); };
    if (Widget* action = WidgetById(kDetailAction))
        action->enabled = [this] {
            return m_detail.action != DetailInfo::Action::None && !m_detail.actionBusy;
        };

    // The description's Show more / Show less link.
    add(kDetailBriefToggle,
        [this](auto* dc, D2D1_SIZE_F s, Layer l) {
            const std::wstring label = Tr(m_detailBriefExpanded ? L"Show less" : L"Show more");
            auto layout = MakeText(label, 12.0f, DWRITE_FONT_WEIGHT_NORMAL, s.width, s.height);
            layout->SetUnderline(TRUE, DWRITE_TEXT_RANGE{0, static_cast<UINT32>(label.size())});
            com_ptr<ID2D1SolidColorBrush> ink;
            dc->CreateSolidColorBrush(Hex(l == Layer::Normal ? 0x0066CC : 0x1E5BA8), ink.put());
            dc->DrawTextLayout({0, 0}, layout.get(), ink.get());
        },
        {})
        ->onClick = [this] {
        m_detailBriefExpanded = !m_detailBriefExpanded;
        m_detailBriefScroll = 0;
        RepaintBreadcrumb();
        RepaintDetailHeader();
    };

    // Seek bar (the band's top rule, as in WMP 12)
    add(kSeek,
        [this](auto* dc, D2D1_SIZE_F s, Layer l) {
            const float progress =
                m_seeking ? m_seekPreview : (m_length > 0 ? static_cast<float>(m_position / m_length) : 0.0f);
            wp::SeekBar(dc, s, progress, l == Layer::Hover || l == Layer::Pressed, m_length > 0,
                        (m_layout.transport.top - m_layout.seek.top) / m_scale);
        },
        {});

    // Transport controls
    // A station playing (radio mode) has no shuffle: its "recommend less" takes the place where
    // it has one (NetEase's 不感兴趣), else the button is greyed.
    auto* shuffle = add(
        kShuffle,
        [this](auto* dc, D2D1_SIZE_F s, Layer l) {
            if (media::radio::Active()) {
                if (media::radio::CanDislike()) {
                    wt::Dislike(dc, l);
                    return;
                }
                dc->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                                     D2D1::IdentityMatrix(), 0.35f),
                              nullptr);
                if (m_win7Art)
                    wt::Toggle(dc, Layer::Normal, true, false);
                else
                    paint::IconButton(dc, s, Layer::Normal, TransportGlyph::Shuffle, false, false);
                dc->PopLayer();
                return;
            }
            if (m_win7Art)
                wt::Toggle(dc, l, true, m_shuffle);
            else
                paint::IconButton(dc, s, l, TransportGlyph::Shuffle, true, m_shuffle);
        },
        {});
    shuffle->onClick = [this] {
        const auto modes = media::transport::ShufflePressed({m_shuffle, static_cast<int>(m_repeat)});
        if (modes.shuffle != m_shuffle) SetShuffleRepeat(modes.shuffle, static_cast<Repeat>(modes.repeat));
    };
    shuffle->enabled = [] { return !media::radio::Active() || media::radio::CanDislike(); };
    // Repeat cycles off -> all -> one; "one" is the original icon with a small "1".
    add(kRepeat,
        [this](auto* dc, D2D1_SIZE_F s, Layer l) {
            const bool on = m_repeat != Repeat::Off;
            if (m_win7Art)
                wt::Toggle(dc, l, false, on);
            else
                paint::IconButton(dc, s, l, TransportGlyph::Repeat, true, on);
            // A station repeats one song or none: its button is repeat-one's, lit or not.
            if (m_repeat == Repeat::One || media::radio::Active()) {
                auto t = MakeText(L"1", 8.0f, DWRITE_FONT_WEIGHT_BOLD, 8, 10);
                com_ptr<ID2D1SolidColorBrush> halo, ink;
                dc->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 0.9f), halo.put());
                dc->CreateSolidColorBrush(Hex(0x1C5DB2), ink.put());
                const D2D1_POINT_2F at{s.width - 8.5f, s.height - 12.0f};
                for (float dx = -0.75f; dx <= 0.75f; dx += 0.75f)
                    for (float dy = -0.75f; dy <= 0.75f; dy += 0.75f)
                        dc->DrawTextLayout({at.x + dx, at.y + dy}, t.get(), halo.get());
                dc->DrawTextLayout(at, t.get(), ink.get());
            }
        },
        {})
        ->onClick = [this] { CycleRepeat(); };
    add(kStop,
        [this](auto* dc, D2D1_SIZE_F s, Layer l) {
            if (m_win7Art)
                wt::ClusterButton(dc, ClusterRegion(m_layout.stop), wt::Button::Stop, l, m_playing);
            else
                paint::IconButton(dc, s, l, TransportGlyph::Stop, m_playing, false);
        },
        {})
        ->onClick = [] { media::transport::Stop(); };
    // Like WMP 12, stop / previous / next are disabled while nothing plays: no hover, no click.
    add(kPrev,
        [this](auto* dc, D2D1_SIZE_F s, Layer l) {
            const bool enabled = m_playing && media::radio::CanGoBack();
            if (m_win7Art)
                wt::ClusterButton(dc, ClusterRegion(m_layout.prev), wt::Button::Prev, l, enabled);
            else
                paint::Segment(dc, s, true, l, TransportGlyph::Previous, enabled);
        },
        {})
        ->onClick = [] { media::transport::Previous(); };
    add(kNext,
        [this](auto* dc, D2D1_SIZE_F s, Layer l) {
            const bool enabled = m_playing && media::radio::CanGoNext();
            if (m_win7Art)
                wt::ClusterButton(dc, ClusterRegion(m_layout.next), wt::Button::Next, l, enabled);
            else
                paint::Segment(dc, s, false, l, TransportGlyph::Next, enabled);
        },
        {})
        ->onClick = [] { media::transport::Next(); };
    WidgetById(kStop)->enabled = [this] { return m_playing; };
    WidgetById(kPrev)->enabled = [this] { return m_playing && media::radio::CanGoBack(); };
    WidgetById(kNext)->enabled = [this] { return m_playing && media::radio::CanGoNext(); };

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
    add(
        kPlay,
        [this](auto* dc, D2D1_SIZE_F s, Layer l) {
            const bool showPause = m_playing && !m_paused;
            if (m_win7Art)
                wt::Orb(dc, l, showPause);
            else
                paint::PlayOrb(dc, s.width, l, showPause ? TransportGlyph::Pause : TransportGlyph::Play);
        },
        orb)
        ->onClick = [this] {
        media::transport::PlayPause([this] {
            if (!m_list || !m_source || m_source->Focus() == SIZE_MAX || m_source->Focus() >= m_source->Count())
                return false;
            m_source->Activate(m_source->Focus()); // like WMP: play what is selected
            return true;
        });
    };

    add(kMute,
        [this](auto* dc, D2D1_SIZE_F s, Layer l) {
            if (m_win7Art && m_layout.transportLevel >= 2) {
                // Folded: the speaker's half of WMP 12's split button, as in the mini player.
                // Lit (4628) or pressed (4627) by itself over the plain plate both halves share
                // (4630, m_splitPlate), which shows while either half is under the pointer.
                if (l != Layer::Normal)
                    win7::DrawPartTo(dc, win7::Module::Wmploc, l == Layer::Pressed ? L"#4627" : L"#4628",
                                     {0, 0, 25, 25}, {0, 0, s.width, s.height});
                const int speaker = m_muted            ? wt::kMuted
                                    : m_volume <= 0.0f ? wt::kVolume0
                                    : m_volume < 0.67f ? wt::kVolume1
                                                       : wt::kVolume2;
                win7::DrawStretched(dc, win7::Module::Wmploc, L"#" + std::to_wstring(speaker),
                                    {0, 0, s.width, s.height});
            } else if (m_win7Art)
                wt::Mute(dc, l, m_muted, m_volume);
            else
                paint::IconButton(dc, s, l, m_muted ? TransportGlyph::Mute : TransportGlyph::Volume, true,
                                  false);
        },
        {})
        ->onClick = [] { playback_control::get()->volume_mute_toggle(); };
    add(kVolume,
        [this](auto* dc, D2D1_SIZE_F s, Layer l) {
            if (m_win7Art)
                wt::Volume(dc, l, m_volume);
            else
                paint::VolumeSlider(dc, s, m_volume, l);
        },
        {});

    // Lyrics pane toggle, bottom-right of the transport band.
    add(kLyricsToggle,
        [this](auto* dc, D2D1_SIZE_F s, Layer l) {
            paint::ButtonPlate(dc, s, l, m_lyricsOpen);
            DrawLyricsGlyph(
                dc, s, paint::TransportGlyphBrush(dc, s.height / 2 - 7, s.height / 2 + 7, true, l).get());
        },
        {})
        ->onClick = [this] { ToggleLyrics(); };

    // A narrow window folds the volume slider behind an arrow beside the speaker, as WMP 12
    // does: the arrow (its half of WMP's split button when lit) opens the flyout.
    add(kVolArrow,
        [this](auto* dc, D2D1_SIZE_F s, Layer l) {
            const float k = s.height / 25.0f;
            if (m_win7Art) { // the arrow's half of the split button, held while the flyout is open
                if (l != Layer::Normal || m_volFlyoutOpen)
                    wt::SplitPlate(dc, l == Layer::Hover && !m_volFlyoutOpen ? 4628 : 4627, true, 0, k);
                D2D1_MATRIX_3X2_F base;
                dc->GetTransform(&base);
                dc->SetTransform(D2D1::Matrix3x2F::Translation(-25 * k, 0) * base);
                wt::SplitArrow(dc, k);
                dc->SetTransform(base);
                return;
            }
            paint::ButtonPlate(dc, s, l, m_volFlyoutOpen);
            com_ptr<ID2D1Factory> factory;
            dc->GetFactory(factory.put());
            com_ptr<ID2D1PathGeometry> path;
            com_ptr<ID2D1GeometrySink> sink;
            if (FAILED(factory->CreatePathGeometry(path.put())) || FAILED(path->Open(sink.put()))) return;
            const float cx = s.width / 2, cy = s.height / 2 + 0.5f * k;
            sink->BeginFigure({cx - 3.5f * k, cy - 2 * k}, D2D1_FIGURE_BEGIN_FILLED);
            sink->AddLine({cx + 3.5f * k, cy - 2 * k});
            sink->AddLine({cx, cy + 2 * k});
            sink->EndFigure(D2D1_FIGURE_END_CLOSED);
            sink->Close();
            const bool lit = l != Layer::Normal || m_volFlyoutOpen;
            dc->FillGeometry(path.get(), wt::RowBands(dc, cy - 2 * k, cy + 2 * k,
                                                      {Hex(lit ? 0xFFFFFF : 0x7FA9E6), Hex(lit ? 0xE4EEFA : 0x2D62B8)},
                                                      true)
                                             .get());
        },
        {})
        ->onClick = [this] { SetVolumeFlyout(!m_volFlyoutOpen); };

    // The command bar's buttons that no longer fit go behind a » at their end (WMP 12).
    add(kCmdMore, [](auto* dc, D2D1_SIZE_F s, Layer l) { wp::CommandButton(dc, s, l, L"\x00BB", false); }, {})
        ->onClick = [this] {
        std::vector<MenuAction> actions;
        for (const int id : m_cmdHidden) {
            const wchar_t* label = id == kAddFiles ? L"Add files" : id == kAddFolder ? L"Add folder" : L"Create playlist";
            actions.push_back({Tr(label), [this, id] {
                                   m_splitPart = 0; // the button's main action, not its menu
                                   if (Widget* w = WidgetById(id); w && w->onClick) w->onClick();
                               }});
        }
        POINT pt{m_layout.cmdMore.left, m_layout.cmdMore.bottom};
        ClientToScreen(m_hwnd, &pt);
        ShowActionMenu(m_hwnd, pt, actions);
    };

    // The flyout lies over everything in the band; the slider widget moves into it on top.
    {
        auto& comp = gfx::CompositionHost::Get().Compositor();
        m_volFlyoutGlass = comp.CreateSpriteVisual();
        m_volFlyout = comp.CreateSpriteVisual();
        // Last in the list too, so that it is hit first where the flyout covers the speaker.
        const auto at = std::find_if(m_widgets.begin(), m_widgets.end(), [](const auto& w) { return w->Id() == kVolume; });
        std::rotate(at, at + 1, m_widgets.end());
        Widget* volume = m_widgets.back().get();
        m_root.Children().Remove(volume->Visual());
        m_root.Children().InsertAtTop(volume->Visual());
        m_root.Children().InsertBelow(m_volFlyoutGlass, volume->Visual());
        m_root.Children().InsertBelow(m_volFlyout, volume->Visual());
        m_volFlyoutGlass.IsVisible(false);
        m_volFlyout.IsVisible(false);
        // The split button's shared plate, under both halves.
        m_splitPlate = comp.CreateSpriteVisual();
        m_splitPlate.Opacity(0.0f);
        m_root.Children().InsertBelow(m_splitPlate, WidgetById(kMute)->Visual());
    }
}

void MainWindow::SetVolumeFlyout(bool open) {
    open = open && m_layout.transportLevel >= 2;
    if (open == m_volFlyoutOpen) return;
    m_volFlyoutOpen = open;
    for (int id : {kVolArrow, kMute})
        if (auto* w = WidgetById(id)) w->Invalidate();
    UpdateLayout(); // the slider moves into the flyout, or leaves with it
    UpdateSplitPlate();
}

// The split button's plain plate (4630) shows while either half is under the pointer or
// held, or the flyout is open, and fades as in the mini player.
void MainWindow::UpdateSplitPlate() {
    if (!m_splitPlate) return;
    const auto isSplit = [](const Widget* w) { return w && (w->Id() == kMute || w->Id() == kVolArrow); };
    const bool on = m_win7Art && m_layout.transportLevel >= 2 &&
                    (m_volFlyoutOpen || isSplit(m_hover) || isSplit(m_pressed));
    if (on == m_splitPlateShown) return;
    m_splitPlateShown = on;
    gfx::CompositionHost::Get().AnimateOpacity(m_splitPlate, on ? 1.0f : 0.0f, on ? 160 : 260);
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
    // The lyrics pane takes the right of the window: glass joined to the client area.
    L.lyricsPane = {};
    if (m_lyricsOpen) {
        int paneW = std::min(PxI(kLyricsPaneWidth), std::max(0, (W - 2 * L.border) / 2));
        if (m_paneReveal >= 0) paneW = std::min(paneW, m_paneReveal);
        L.lyricsPane = {W - L.border - paneW, capH, W - L.border, H - L.border};
        L.client.right = L.lyricsPane.left;
    }
    // Light + dark client edge lines, one device pixel each (DWM frame bitmap, 1:1).
    L.clientEdge = {L.client.left - 2, L.client.top - 2, L.client.right + 2, L.client.bottom + 2};
    if (m_lyricsOpen) L.clientEdge.right = L.lyricsPane.right + 2; // one edge around client and pane
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
    L.capMax = {L.capClose.left - PxI(dwm ? 41.0f / 1.5f : m::kCapBtnMaxW), capTop, L.capClose.left,
                capTop + bh};
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
        L.breadcrumb = {L.navGroove.right + PxI(6), L.navBar.top, L.client.right - PxI(8),
                        L.navBar.bottom - line};
    } else {
        const int orb = PxI(24);
        L.back = CenteredRect(L.client.left + PxI(18), nbcy, orb, orb);
        L.forward = CenteredRect(L.client.left + PxI(44), nbcy, orb, orb);
        L.breadcrumb = {L.client.left + PxI(64), L.navBar.top, L.client.right - PxI(8),
                        L.navBar.bottom - line};
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
    // The search box gives way first in a narrow window: 230 DIP down to 140.
    const float clientDip = (L.client.right - L.client.left) / m_scale;
    const int searchW = PxI(std::clamp(clientDip * 0.32f, 140.0f, 230.0f));
    L.search = {L.client.right - PxI(8) - searchW, cbcy - PxI(11), L.client.right - PxI(8), cbcy + PxI(11)};
    // View options (WMP: icon / expanded tile / details) left of the search box, library only.
    if (m_source && ViewStyles() > 1) {
        const int right = L.search.left - PxI(8);
        L.viewOptions = {right - PxI(kViewOptionsSplit + kSplitArrow), cbcy - PxI(11), right, cbcy + PxI(11)};
    } else {
        L.viewOptions = {};
    }
    {
        // The page's command (Collect) sits left of the view options (or the search box), only
        // on pages that have one. Its width follows the centred label.
        const DetailInfo detail = m_page ? m_page->Detail() : DetailInfo{};
        int right = (L.viewOptions.right > L.viewOptions.left ? L.viewOptions.left : L.search.left) - PxI(8);
        L.download = {};
        if (!detail.downloadLabel.empty()) {
            const float w = fit(detail.downloadLabel.c_str(), false);
            L.download = {right - PxI(w), cbcy - PxI(11), right, cbcy + PxI(11)};
            right = L.download.left - PxI(2);
        }
        if (!detail.saveLabel.empty()) {
            const float w = fit(detail.saveLabel.c_str(), false);
            L.refresh = {right - PxI(w), cbcy - PxI(11), right, cbcy + PxI(11)};
            right = L.refresh.left - PxI(2);
        } else {
            L.refresh = {};
        }
        L.localPlay = {};
        if (LocalPlayShown()) {
            const float w = fit(L"Play", false) + 14 + 5; // the triangle and its gap
            L.localPlay = {right - PxI(w), cbcy - PxI(11), right, cbcy + PxI(11)};
        }
    }
    // Then the buttons on the left that would run into the right group go, last first, behind
    // a » after the ones that stay (WMP 12); Organize always stays.
    {
        LONG rightGroup = L.search.left;
        for (const RECT* r : {&L.viewOptions, &L.refresh, &L.download, &L.localPlay})
            if (r->right > r->left) rightGroup = std::min(rightGroup, r->left);
        const int moreW = PxI(22);
        m_cmdHidden.clear();
        RECT* order[] = {&L.addFiles, &L.addFolder, &L.newPlaylist};
        const int ids[] = {kAddFiles, kAddFolder, kNewPlaylist};
        int kept = 3; // how many of the three stay: all, or as many as leave room for the »
        if (order[2]->right + PxI(8) > rightGroup) {
            kept = 2;
            while (kept > 0 && order[kept - 1]->right + PxI(2) + moreW + PxI(8) > rightGroup) --kept;
        }
        for (int i = kept; i < 3; ++i) {
            m_cmdHidden.push_back(ids[i]);
            *order[i] = {};
        }
        if (m_cmdHidden.empty()) {
            L.cmdMore = {};
        } else {
            LONG lastRight = L.organize.right;
            for (const RECT* r : order)
                if (r->right > r->left) lastRight = r->right;
            L.cmdMore = {lastRight + PxI(2), cbcy - bh2 / 2, lastRight + PxI(2) + moreW, cbcy - bh2 / 2 + bh2};
        }
    }

    // Content: navigation pane + list (a detail band sits above artist, album and playlist pages).
    // The navigation pane narrows with the window, from 190 DIP to 150.
    const int navW = PxI(std::clamp(clientDip * 0.3f, 150.0f, kNavPaneWidth));
    L.navPane = {L.content.left, L.content.top, L.content.left + navW, L.content.bottom - PxI(8)};
    L.list = {L.navPane.right, L.content.top, L.content.right, L.content.bottom};
    L.detailHeader = {};
    L.detailAction = {};
    L.detailBriefToggle = {};
    L.detailBriefArea = {};
    L.detailBriefBar = {};
    m_detailBriefOverflow = false;
    m_detailBriefScrollable = false;
    m_detailBriefViewH = 0;
    m_detailBriefContentH = 0;
    m_detailBriefTextW = 0;
    m_detail = m_page ? m_page->Detail() : DetailInfo{};
    if (m_detail.visible) {
        L.detailHeader = {L.navPane.right, L.content.top, L.content.right,
                          std::min<LONG>(L.content.top + PxI(kDetailHeaderHeight), L.content.bottom)};
        if (m_detail.action != DetailInfo::Action::None) {
            const int right = L.detailHeader.right - PxI(18);
            L.detailAction = {right - PxI(kDetailActionWidth), L.detailHeader.top + PxI(22), right,
                              L.detailHeader.top + PxI(22 + kDetailActionHeight)};
        }
        // The facts and the tagline come under the stats line, the description under them.
        {
            const float textW = DetailHeaderTextWidth();
            m_detailBriefTop = DetailTextTop() + DetailFactsHeight(textW) + DetailTaglineHeight(textW);
            L.detailHeader.bottom =
                std::min<LONG>(L.content.top + std::max<int>(PxI(kDetailHeaderHeight), PxI(m_detailBriefTop + 12)),
                               L.content.bottom);
        }
        const float briefTop = m_detailBriefTop;
        // The description is limited to three lines until expanded. Expanded text has its own
        // maximum height and scrollbar, so the song list below always stays reachable.
        if (!m_detail.brief.empty()) {
            const float baseW = DetailHeaderTextWidth();
            const float textX = 18 + kDetailArt + 18;
            const float bandSpace = (L.content.bottom - L.content.top) / m_scale;
            const float maxView =
                std::min(kDetailBriefMaxHeight, std::max(96.0f, bandSpace - briefTop - 12 - 18 - 8));
            auto measure = [&](float width) {
                auto layout =
                    MakeWrappedText(m_detail.brief, 12.0f, DWRITE_FONT_WEIGHT_NORMAL, width, 4000.0f);
                DWRITE_TEXT_METRICS metrics{};
                layout->GetMetrics(&metrics);
                return metrics.height;
            };
            const float fullBase = measure(baseW);
            const float collapsed = std::min(fullBase, 48.0f);
            m_detailBriefOverflow = fullBase > collapsed + 1.0f;
            float textW = baseW;
            float contentH = fullBase;
            float used = collapsed;
            bool scrollable = false;
            if (m_detailBriefExpanded) {
                if (fullBase > maxView + 1.0f) {
                    scrollable = true;
                    textW = std::max(80.0f, baseW - kDetailScrollWidth - 6);
                    contentH = measure(textW);
                    used = std::min(contentH, maxView);
                } else {
                    used = fullBase;
                }
            }
            // Collapsed, Show more follows the last line's text instead of standing on a line of
            // its own under a blank one (owner, 2026-11-14): blank lines close up and the last
            // line is cut with an ellipsis where the link needs the room.
            m_detailBriefCollapsed.clear();
            D2D1_POINT_2F inlineLink{-1, -1};
            if (m_detailBriefOverflow && !m_detailBriefExpanded) {
                auto link = MakeText(Tr(L"Show more"), 12.0f, DWRITE_FONT_WEIGHT_NORMAL, 200.0f, 18.0f);
                DWRITE_TEXT_METRICS lm{};
                link->GetMetrics(&lm);
                const float linkW = std::ceil(lm.widthIncludingTrailingWhitespace) + 4.0f;
                std::wstring text;
                for (size_t i = 0; i < m_detail.brief.size(); ++i) { // blank lines closed up
                    const wchar_t c = m_detail.brief[i];
                    if (c == L'\r') continue;
                    if (c == L'\n' && (text.empty() || text.back() == L'\n')) continue;
                    text += c;
                }
                while (!text.empty() && iswspace(text.back())) text.pop_back();
                const auto fits = [&](const std::wstring& t, D2D1_POINT_2F* end) {
                    auto layout = MakeWrappedText(t, 12.0f, DWRITE_FONT_WEIGHT_NORMAL, textW, 4000.0f);
                    DWRITE_TEXT_METRICS tm{};
                    layout->GetMetrics(&tm);
                    if (tm.lineCount > 3) return false;
                    DWRITE_HIT_TEST_METRICS hm{};
                    float x = 0, y = 0;
                    layout->HitTestTextPosition(static_cast<UINT32>(t.size() - 1), TRUE, &x, &y, &hm);
                    // The last line's top from the line metrics: the caret's y put the link a
                    // line too low (owner, 2026-11-14).
                    UINT32 count = 0;
                    layout->GetLineMetrics(nullptr, 0, &count);
                    std::vector<DWRITE_LINE_METRICS> lines(count);
                    layout->GetLineMetrics(lines.data(), count, &count);
                    float top = 0;
                    for (UINT32 i = 0; i + 1 < count; ++i) top += lines[i].height;
                    *end = {x, top};
                    return x + 6 + linkW <= textW;
                };
                D2D1_POINT_2F end{};
                if (!text.empty() && fits(text, &end)) {
                    m_detailBriefCollapsed = text;
                } else {
                    // The first three lines' text, then shortened until "…" and the link fit.
                    auto layout = MakeWrappedText(text, 12.0f, DWRITE_FONT_WEIGHT_NORMAL, textW, 4000.0f);
                    UINT32 count = 0;
                    layout->GetLineMetrics(nullptr, 0, &count);
                    std::vector<DWRITE_LINE_METRICS> lines(count);
                    layout->GetLineMetrics(lines.data(), count, &count);
                    size_t cut = 0;
                    for (UINT32 i = 0; i < std::min<UINT32>(count, 3); ++i) cut += lines[i].length;
                    cut = std::min(cut, text.size());
                    for (int tries = 0; cut > 0 && tries < 400; ++tries, --cut) {
                        std::wstring t = text.substr(0, cut);
                        while (!t.empty() && iswspace(t.back())) t.pop_back();
                        t += L'\x2026';
                        if (fits(t, &end)) {
                            m_detailBriefCollapsed = std::move(t);
                            break;
                        }
                    }
                }
                if (!m_detailBriefCollapsed.empty()) {
                    inlineLink = {end.x + 6, end.y};
                    used = std::min(used, end.y + 16.0f);
                }
            }
            float needed = briefTop + used + 12;
            if (m_detailBriefOverflow && inlineLink.x < 0) needed += 18;
            if (m_detailBriefExpanded) needed += 6; // pixel rounding must not clip the last line
            L.detailHeader.bottom = std::min<LONG>(
                L.content.top + std::max<int>(PxI(kDetailHeaderHeight), PxI(needed)), L.content.bottom);
            m_detailBriefScrollable = scrollable;
            m_detailBriefViewH = used;
            m_detailBriefContentH = contentH;
            m_detailBriefTextW = textW;
            const float maxScroll = std::max(0.0f, contentH - used);
            m_detailBriefScroll = scrollable ? std::clamp(m_detailBriefScroll, 0.0f, maxScroll) : 0.0f;
            const int briefX = L.detailHeader.left + PxI(textX);
            const int briefY = L.detailHeader.top + PxI(briefTop);
            L.detailBriefArea = {briefX, briefY, briefX + PxI(textW), briefY + PxI(used)};
            if (scrollable) {
                const int barLeft = L.detailHeader.left + PxI(textX + baseW - kDetailScrollWidth);
                L.detailBriefBar = {barLeft, briefY, barLeft + PxI(kDetailScrollWidth), briefY + PxI(used)};
                L.detailBriefArea.right = barLeft - PxI(4);
            }
            if (m_detailBriefOverflow) {
                const std::wstring label = Tr(m_detailBriefExpanded ? L"Show less" : L"Show more");
                auto link = MakeText(label, 12.0f, DWRITE_FONT_WEIGHT_NORMAL, 200.0f, 18.0f);
                DWRITE_TEXT_METRICS linkMetrics{};
                link->GetMetrics(&linkMetrics);
                const bool inLine = inlineLink.x >= 0;
                const int linkX = L.detailHeader.left + PxI(textX + (inLine ? inlineLink.x : 0));
                const int y = L.detailHeader.top + PxI(briefTop + (inLine ? inlineLink.y : used + 2));
                const int w = PxI(std::ceil(linkMetrics.widthIncludingTrailingWhitespace) + 4.0f);
                L.detailBriefToggle = {linkX, y, linkX + w, y + PxI(18)};
            }
        }
        L.list.top = L.detailHeader.bottom;
    }
    L.tabs = {};
    if (m_page && !m_page->Tabs().empty()) {
        L.tabs = {L.list.left, L.list.top, L.list.right, std::min<LONG>(L.list.top + PxI(kTabBand), L.list.bottom)};
        L.list.top = L.tabs.bottom;
    }

    // Transport band: seek rule on top, capsule centered on the orb below it.
    // WMP 12: the seek bar runs the full client width right under the band's top rule;
    // the widget is taller than the bar for the hover thumb and an easier grab.
    L.seek = {L.client.left, L.transport.top - PxI(6), L.client.right, L.transport.top + PxI(9)};
    const int tcx = (L.transport.left + L.transport.right) / 2;
    // A narrow window gives way in WMP 12's order: shuffle and repeat leave the capsule, then
    // the volume slider folds behind an arrow beside the speaker (a flyout, as in the mini
    // player), then stop leaves. The capsule shortens to what it holds and stays centred on
    // the orb; the time moves to the left of the capsule when it no longer fits on the right.
    // Ends of the capsule per level, DIP from the orb's centre.
    const auto span = [&](int level) -> std::pair<float, float> {
        if (m_win7Art) {
            const float o = wt::kOrbRect.left + (wt::kOrbRect.right - wt::kOrbRect.left) / 2; // orb centre, cluster x
            const float left = level == 0 ? wt::kCapsuleVisible.left : level < 3 ? -8.0f : 28.0f;
            const float right = level < 2 ? wt::kCapsuleVisible.right : wt::kMuteRect.right + 14 + 9;
            return {left - o, right - o};
        }
        const float ib = m::kIconButtonW / 2;
        const float left = level == 0 ? m::kShuffleX - ib - 10 : level < 3 ? m::kStopX - ib - 10 : -m::kSegmentReach - 8;
        const float right = level < 2 ? m::kSliderRight + 10 : m::kVolumeX + ib + 16 + 10;
        return {left, right};
    };
    // Nothing may overlap (owner, 2026-11-13: WMP's own narrow layout lets the time run into
    // the capsule): left to right the song's text (at least 110 DIP), the time when it stands
    // left, the capsule, the time when it stands right, the lyrics toggle, 12 DIP apart. The
    // capsule stays centred on the band while some step allows it; else it moves off centre
    // just enough, and only then do further steps give way.
    const int toggleLeft = L.client.right - PxI(10 + 28);
    // The time box is as wide as its text can get for this song ("4:56 / 4:56"; the digits
    // are tabular), not a fixed 110 DIP: the stream block starts right after it.
    const bool timeRemaining = m_length > 0 && settings::PlayerTimeRemaining();
    if (m_timeMeasuredFor != m_length || m_timeMeasuredRemaining != timeRemaining) {
        m_timeMeasuredFor = m_length;
        m_timeMeasuredRemaining = timeRemaining;
        const std::wstring longest =
            m_length > 0 ? (timeRemaining ? L"-" : L"") + FormatTime(m_length) + L" / " + FormatTime(m_length)
                         : L"0:00";
        DWRITE_TEXT_METRICS tm{};
        MakeText(longest, 12.0f, DWRITE_FONT_WEIGHT_NORMAL, 400, 20)->GetMetrics(&tm);
        m_timeTextW = std::ceil(tm.widthIncludingTrailingWhitespace) + 4;
    }
    const int timeW = PxI(std::max(40.0f, m_timeTextW)), gap = PxI(12), textLeft = L.client.left + PxI(10);
    const int nowMin = PxI(110);
    struct Fit {
        int level;
        bool timeLeft;
        int lo, hi; // range of the orb centre that fits
    };
    const auto fitTransport = [&](int lv, bool left) {
        const auto [sl, sr] = span(lv);
        const int minLeft = textLeft + nowMin + gap + (left ? timeW + gap : 0);
        const int maxRight = toggleLeft - gap - (left ? 0 : timeW + gap);
        return Fit{lv, left, minLeft - PxI(sl), maxRight - PxI(sr)};
    };
    std::optional<Fit> chosen;
    for (const bool centred : {true, false}) {
        for (int lv = 0; lv <= 3 && !chosen; ++lv)
            for (const bool left : {false, true}) {
                const Fit f = fitTransport(lv, left);
                if (f.lo <= f.hi && (!centred || (tcx >= f.lo && tcx <= f.hi))) {
                    chosen = f;
                    break;
                }
            }
        if (chosen) break;
    }
    // Narrower than the minimum allows (it should not be): the capsule keeps clear of the
    // time and the toggle, and the song's text gets what is left.
    const Fit placed = chosen ? *chosen : fitTransport(3, false);
    const int orb = chosen ? std::clamp(tcx, placed.lo, placed.hi) : placed.hi;
    const int level = placed.level;
    const bool timeLeft = placed.timeLeft;
    L.transportLevel = level;
    const bool timeMoved = L.timeLeft != timeLeft;
    L.timeLeft = timeLeft;
    {
        const auto [sl, sr] = span(level);
        const float o = m_win7Art ? wt::kOrbRect.left + (wt::kOrbRect.right - wt::kOrbRect.left) / 2 : 0.0f;
        L.capsuleLeft = sl + o;
        L.capsuleRight = sr + o;
    }
    if (m_win7Art) {
        // Original bitmaps, placed relative to the cluster as in WMP 12 (orb center = band center,
        // unless a narrow window moved it).
        L.cluster = {orb - PxI(108), L.transport.top + PxI(wt::kClusterTop)};
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
        auto at = [&](float dx) { return orb + PxI(dx); };
        L.capsule = CenteredRect(orb, tcy, PxI(m::kCapsuleWidth), PxI(m::kCapsuleHeight));
        const int orbPx = PxI(m::kOrbDiameter + 2.0f);
        L.play = CenteredRect(orb, tcy, orbPx, orbPx);
        const int segH = PxI(m::kSegmentHeight);
        L.prev = {at(-m::kSegmentReach), tcy - segH / 2, orb, tcy - segH / 2 + segH};
        L.next = {orb, tcy - segH / 2, at(m::kSegmentReach), tcy - segH / 2 + segH};
        const int ibw = PxI(m::kIconButtonW), ibh = PxI(m::kIconButtonH);
        L.shuffle = CenteredRect(at(m::kShuffleX), tcy, ibw, ibh);
        L.repeat = CenteredRect(at(m::kRepeatX), tcy, ibw, ibh);
        L.stop = CenteredRect(at(m::kStopX), tcy, ibw, ibh);
        L.mute = CenteredRect(at(m::kVolumeX), tcy, ibw, ibh);
        L.volume = {at(m::kSliderLeft), tcy - ibh / 2, at(m::kSliderRight), tcy - ibh / 2 + ibh};
        L.divider = CenteredRect(at(m::kDividerX), tcy, PxI(4), PxI(16));
        L.capsule.left = at(L.capsuleLeft);
        L.capsule.right = at(L.capsuleRight);
    }
    const int capL = m_win7Art ? L.cluster.x + PxI(L.capsuleLeft) : L.capsule.left;
    const int capR = m_win7Art ? L.cluster.x + PxI(L.capsuleRight) : L.capsule.right;
    if (level >= 1) L.shuffle = L.repeat = {};
    if (level >= 3) L.stop = {};
    L.volArrow = L.volFlyout = {};
    if (level >= 2) {
        // The speaker keeps its place; the arrow follows it, 14 DIP wide (WMP's split button).
        L.volArrow = {L.mute.right, L.mute.top, L.mute.right + PxI(14), L.mute.top + PxI(25)};
        if (!m_win7Art) L.volArrow.bottom = L.mute.bottom;
        // The flyout over the button, its right edge on the arrow's (WMP 12): a 5 DIP frame
        // round the slider.
        const int sw = PxI(80), sh = PxI(23), fw = sw + PxI(16), fh = sh + PxI(12);
        const int mid = (L.mute.top + L.mute.bottom) / 2;
        const int right = std::min<LONG>(L.volArrow.right, L.client.right - PxI(2));
        L.volFlyout = {right - fw, mid - fh / 2, right, mid - fh / 2 + fh};
        L.volume = m_volFlyoutOpen ? RECT{L.volFlyout.left + PxI(8), L.volFlyout.top + PxI(6),
                                          L.volFlyout.left + PxI(8) + sw, L.volFlyout.top + PxI(6) + sh}
                                   : RECT{};
    }
    if (level < 2) m_volFlyoutOpen = false;
    // Time right of the controls (after the volume) when it fits there, else left of them;
    // stream format and output device right-aligned at the band's end, where there is room.
    {
        const int tcyMid = (L.transport.top + PxI(4) + L.transport.bottom) / 2;
        L.lyricsToggle = {toggleLeft, tcyMid - PxI(12), L.client.right - PxI(10), tcyMid + PxI(12)};
        const int timeX = timeLeft ? capL - PxI(12) - timeW : capR + PxI(12);
        L.time = {timeX, tcyMid - PxI(10), timeX + timeW, tcyMid + PxI(10)};
        const LONG streamLeft = std::max<LONG>(timeLeft ? capR + gap : L.time.right + gap,
                                               L.lyricsToggle.left - PxI(8) - PxI(320));
        L.stream = {streamLeft, L.transport.top + PxI(8), L.lyricsToggle.left - PxI(8), L.transport.bottom - PxI(4)};
        if (L.stream.right - L.stream.left < PxI(60)) L.stream = {};
    }
    L.nowPlaying = {L.client.left + PxI(10), L.transport.top + PxI(7),
                    std::max<int>(L.client.left + PxI(10), (timeLeft ? L.time.left : capL) - PxI(12)),
                    L.transport.bottom - PxI(3)};

    // Apply to the visual tree.
    m_frame->Layout({W, H}, L.clientEdge.left, !max);

    Place(m_navGroove, L.navGroove);
    if (const SIZE gs{L.navGroove.right - L.navGroove.left, L.navGroove.bottom - L.navGroove.top};
        m_win7Art && gs.cx > 0 && (gs.cx != m_navGrooveSize.cx || gs.cy != m_navGrooveSize.cy)) {
        m_navGrooveSize = gs;
        gfx::CompositionHost::Get().Repaint(m_navGroove, gs.cx, gs.cy, m_scale, [&](ID2D1DeviceContext* dc) {
            wt::NavGroove(dc, D2D1::SizeF(gs.cx / m_scale, gs.cy / m_scale));
        });
    }
    Place(m_clientEdge, L.clientEdge);
    Place(m_clientBackground, L.client);
    Place(m_clientGlow, L.client);
    Place(m_navBar, L.navBar);
    Place(m_cmdBar, L.cmdBar);
    Place(m_contentTop, {L.content.left, L.content.top, L.content.right, L.content.top + PxI(14)});
    Place(m_libraryBg, {L.list.left, L.tabs.bottom > L.tabs.top ? L.tabs.top : L.list.top, L.list.right,
                        L.list.bottom});
    LayoutTabs();
    if (m_detailHeader) {
        const bool show = m_detail.visible && L.detailHeader.right > L.detailHeader.left;
        m_detailHeader.IsVisible(show);
        if (show) {
            const SIZE size{L.detailHeader.right - L.detailHeader.left,
                            L.detailHeader.bottom - L.detailHeader.top};
            if (m_detailHeaderSize.cx != size.cx || m_detailHeaderSize.cy != size.cy)
                PaintDetailHeader(); // a new surface size: repaint and re-place
            else
                Place(m_detailHeader, L.detailHeader);
        }
    }
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
        Place(m_npLyric, t);
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
        m_crumbBar->SetBounds(t, m_scale);
    }
    m_nav->SetBounds(L.navPane, m_scale);
    m_list->SetBounds(L.list, m_scale);
    m_search->SetBounds(L.search, m_scale);
    if (m_pane && !m_paneLent) {
        m_pane->SetBounds(L.lyricsPane, m_scale);
        m_pane->SetVisible(m_lyricsOpen && L.lyricsPane.right > L.lyricsPane.left);
    }

    for (auto& w : m_widgets) {
        const RECT* r = nullptr;
        switch (w->Id()) {
        case kCapMin:
            r = &L.capMin;
            break;
        case kCapMax:
            r = &L.capMax;
            break;
        case kCapClose:
            r = &L.capClose;
            break;
        case kBack:
            r = &L.back;
            break;
        case kForward:
            r = &L.forward;
            break;
        case kOrganize:
            r = &L.organize;
            break;
        case kAddFiles:
            r = &L.addFiles;
            break;
        case kAddFolder:
            r = &L.addFolder;
            break;
        case kNewPlaylist:
            r = &L.newPlaylist;
            break;
        case kViewOptions:
            r = &L.viewOptions;
            break;
        case kRefresh:
            r = &L.refresh;
            break;
        case kDownloadAll:
            r = &L.download;
            break;
        case kLocalPlay:
            r = &L.localPlay;
            break;
        case kDetailAction:
            r = &L.detailAction;
            break;
        case kDetailBriefToggle:
            r = &L.detailBriefToggle;
            break;
        case kSeek:
            r = &L.seek;
            break;
        case kShuffle:
            r = &L.shuffle;
            break;
        case kRepeat:
            r = &L.repeat;
            break;
        case kStop:
            r = &L.stop;
            break;
        case kPrev:
            r = &L.prev;
            break;
        case kNext:
            r = &L.next;
            break;
        case kPlay:
            r = &L.play;
            break;
        case kMute:
            r = &L.mute;
            break;
        case kVolume:
            r = &L.volume;
            break;
        case kLyricsToggle:
            r = &L.lyricsToggle;
            break;
        case kVolArrow:
            r = &L.volArrow;
            break;
        case kCmdMore:
            r = &L.cmdMore;
            break;
        }
        if (r) w->SetBounds(*r, m_scale);
    }
    PaintCapsule();
    // The folded speaker's plate (WMP's split button 4630) behind both halves; the speaker
    // itself draws differently folded, at the same size.
    if (const bool folded = m_win7Art && L.transportLevel >= 2; folded != m_speakerFolded) {
        m_speakerFolded = folded;
        if (auto* w = WidgetById(kMute)) w->Invalidate();
        UpdateSplitPlate();
    }
    m_splitPlate.IsVisible(m_speakerFolded);
    if (m_speakerFolded) {
        const RECT pr{L.mute.left, L.mute.top, L.volArrow.right, L.mute.top + PxI(25)};
        Place(m_splitPlate, pr);
        const int pw = pr.right - pr.left, ph = pr.bottom - pr.top;
        gfx::CompositionHost::Get().Repaint(m_splitPlate, pw, ph, m_scale, [&](ID2D1DeviceContext* dc) {
            const float k = ph / m_scale / 25.0f;
            wt::SplitPlate(dc, 4630, false, 0, k);
            wt::SplitPlate(dc, 4630, true, 25 * k, k);
        });
    }
    // The folded volume's flyout: WMP's frame over a frosted copy of the band.
    {
        const bool open = m_volFlyoutOpen && L.volFlyout.right > L.volFlyout.left;
        m_volFlyout.IsVisible(open);
        m_volFlyoutGlass.IsVisible(open);
        if (open) {
            Place(m_volFlyout, L.volFlyout);
            Place(m_volFlyoutGlass, L.volFlyout);
            if (m_volFlyoutScale != m_scale) {
                m_volFlyoutScale = m_scale;
                m_volFlyoutGlass.Brush(gfx::MakeFrostBrush(m_volFlyoutGlass.Compositor(), Px(4.0f)));
            }
            const int w = L.volFlyout.right - L.volFlyout.left, h = L.volFlyout.bottom - L.volFlyout.top;
            gfx::CompositionHost::Get().Repaint(m_volFlyout, w, h, m_scale, [&](ID2D1DeviceContext* dc) {
                wt::FlyoutFrame(dc, D2D1::SizeF(w / m_scale, h / m_scale));
            });
        }
    }
    m_frame->Glass().UpdateReflectionAnchor(m_hwnd);
    // The time's alignment follows its side, and the song's text is laid out within the width
    // the block has (cut short with an ellipsis, its star never under the capsule). Painting
    // lays out again, which then finds both unchanged.
    if (timeMoved || NowPlayingWidth() != m_npPaintedWidth || L.time.right - L.time.left != m_timePaintedW)
        RepaintNowPlaying();
}

// The capsule holds what the transport shows: WMP's bitmap with its ends moved in for a
// narrow window (drawn again only when they move).
void MainWindow::PaintCapsule() {
    if (!m_win7Art) return;
    const Layout& L = m_layout;
    if (L.capsuleRight <= L.capsuleLeft) return; // not laid out yet
    if (L.capsuleLeft == m_capsulePaintedLeft && L.capsuleRight == m_capsulePaintedRight &&
        m_capsulePaintedScale == m_scale)
        return;
    m_capsulePaintedLeft = L.capsuleLeft;
    m_capsulePaintedRight = L.capsuleRight;
    m_capsulePaintedScale = m_scale;
    const RECT cap = ClusterRect(wt::kCapsuleRect);
    const float left = L.capsuleLeft, right = L.capsuleRight;
    gfx::CompositionHost::Get().Repaint(m_capsule, cap.right - cap.left, cap.bottom - cap.top, m_scale,
                                        [left, right](ID2D1DeviceContext* dc) { wt::CapsuleSpan(dc, left, right); });
}

// Every surface came back empty: what is painted once and kept (the frame, the bars, the
// capsule, the now-playing block, the widgets) is painted again, the scale-keyed marks first
// forgotten. The panes and views repaint themselves on the same notification.
void MainWindow::OnDeviceReplaced() {
    m_capsulePaintedScale = 0;
    m_npPaintedWidth = -1;
    m_timePaintedW = 0;
    RepaintScaleDependent();
    UpdateLayout();
    for (auto& w : m_widgets)
        w->Invalidate();
    m_seekPaint.Request();
    FlushPlaybackPaints();
    RepaintNowPlaying();
}

void MainWindow::RepaintScaleDependent() {
    const float s = m_scale;
    m_frame->SetScale(s);
    m_navGrooveSize = {}; // repainted at the new scale by the next layout
    const int line = LinePx();

    const int edgeSrc = PxI(16);
    m_clientEdge.Brush(gfx::CompositionHost::Get().CreateNineGrid(edgeSrc, edgeSrc, s, 3.0f, true, [&](ID2D1DeviceContext* dc) {
        paint::ClientEdge(dc, D2D1::SizeF(edgeSrc / s, edgeSrc / s));
    }));
    m_clientGlow.Brush(gfx::CompositionHost::Get().CreateNineGrid(edgeSrc, edgeSrc, s, 4.0f, true, [&](ID2D1DeviceContext* dc) {
        paint::ClientGlow(dc, D2D1::SizeF(edgeSrc / s, edgeSrc / s));
    }));

    // WMP 12 client chrome (measured; docs/design/visual-spec.md)
    const float lineDip = line / s;
    m_navBar.Brush(Strip(PxI(NavBarHeight()), s, [&](ID2D1DeviceContext* dc, float h) {
        dc->FillRectangle({-1, 0, 100, h},
                          VGrad(dc, 0, h, {{0.0f, 0xDAEFFF}, {0.3f, 0xE2EBF8}, {1.0f, 0xE7EEFE}}).get());
        FillLine(dc, h - lineDip, lineDip, 0xC3CCCF);
    }));
    m_cmdBar.Brush(Strip(PxI(m::kCommandBarHeight), s, [&](ID2D1DeviceContext* dc, float h) {
        dc->FillRectangle(
            {-1, 0, 100, h},
            VGrad(
                dc, 0, h,
                {{0.0f, 0xF5FEFF}, {0.3f, 0xEDF7FD}, {0.48f, 0xE9F3FD}, {0.52f, 0xDCE6F2}, {1.0f, 0xE1EAF9}})
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
            dc->FillRectangle(
                {-1, 0, 100, h},
                VGrad(dc, 0, h, {{0.0f, 0xEBF2FA}, {0.1f, 0xE9F1F9}, {0.4f, 0xE3EDF8}, {1.0f, 0xDFEAF7}})
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
        const RECT cl = ClusterRect(wt::kClusterRect);
        m_capsulePaintedScale = 0; // drawn by the layout at this scale, as long as it is
        PaintCapsule();
        host.Repaint(m_cluster, cl.right - cl.left, cl.bottom - cl.top, s,
                     [](ID2D1DeviceContext* dc) { wt::ClusterBackground(dc); });
    } else {
        const int capH = PxI(m::kCapsuleHeight);
        m_capsule.Brush(
            gfx::CompositionHost::Get().CreateNineGrid(2 * capH, capH, s, static_cast<float>(capH / 2), false, [&](ID2D1DeviceContext* dc) {
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
        if (HICON old = reinterpret_cast<HICON>(
                SendMessageW(m_hwnd, WM_SETICON, which, reinterpret_cast<LPARAM>(icon))))
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
        paint::Icon(dc, m_icon.get(),
                    D2D1::RectF(pad, pad + 12 - iconSz / 2, pad + iconSz, pad + 12 + iconSz / 2));
        auto layout = MakeText(kAppName, m::kTitleFontSize, DWRITE_FONT_WEIGHT_NORMAL, 560, 24);
        paint::GlowText(dc, layout.get(), D2D1::Point2F(pad + iconSz + 6, pad), D2D1::ColorF(0, 0, 0, 1));
    });
    UpdateLayout(); // Repaint resets the visual to the surface size; re-apply the layout clip.
}

// ---- Animation timers and the input method ------------------------------------------------

void MainWindow::StartScrollAnimation() {
    if (m_scrollAnimActive) return;
    m_scrollAnimActive = true;
    SetTimer(m_hwnd, kScrollAnimTimer, 16, nullptr);
}

void MainWindow::StartScrollRepeat() {
    if (m_scrollRepeatActive) return;
    m_scrollRepeatActive = true;
    m_scrollRepeatSince = GetTickCount64();
    SetTimer(m_hwnd, kScrollRepeatTimer, 50, nullptr);
}

void MainWindow::PlaceImeWindows() {
    HIMC imc = ImmGetContext(m_hwnd);
    if (!imc) return;
    const RECT box = m_search->Bounds();
    const POINT caret = m_search->CaretClientPoint();
    COMPOSITIONFORM composition{CFS_POINT, caret, {}};
    ImmSetCompositionWindow(imc, &composition);
    // CFS_EXCLUDE keeps the list off the box: below it, or above near the screen bottom.
    CANDIDATEFORM candidate{0, CFS_EXCLUDE, {caret.x, box.bottom}, box};
    ImmSetCandidateWindow(imc, &candidate);
    ImmReleaseContext(m_hwnd, imc);
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
    if (m_playerTaskbar) m_playerTaskbar->SetPlayState(playing, paused);
    if (m_list) m_list->Refresh(); // playing marker
    if (m_pane) m_pane->SetPlaying(playing, paused);
    if (playing && !paused && m_npLyrics && m_npLyrics->synced) StartLyricsFrames();
    StartSeekFrames();
    OnTrackChanged(); // paused resume still has codec/bitrate information
}

double MainWindow::SafeCorePosition(const metadb_handle_ptr& playing) {
    if (!playing.is_valid()) return m_position;
    const double pos = playback_control::get()->playback_get_position();
    const std::wstring id = lyrics::Service::TrackIdentity(playing);
    // Natural end-of-track transitions in foobar2000 can briefly report position 0.0 while
    // get_now_playing() still points to the outgoing song. Genuine backward jumps reset
    // m_lastCorePos through OnPlaybackSeek() or OnSwitching(false, opened) first.
    if (id == m_lastCoreIdentity && pos + 0.5 < m_lastCorePos) return m_lastCorePos;
    m_lastCoreIdentity = id;
    m_lastCorePos = pos;
    return pos;
}

void MainWindow::OnPlaybackSeek(double seconds) {
    const double clamped = std::max(0.0, seconds);
    metadb_handle_ptr track;
    if (playback_control::get()->get_now_playing(track) && track.is_valid()) {
        m_lastCoreIdentity = lyrics::Service::TrackIdentity(track);
        m_lastCorePos = clamped;
    }
    m_position = clamped;
    if (m_pane) m_pane->OnSeek(clamped);
    if (m_player) m_player->OnSeek(clamped);
    OnPlaybackTime();
}

void MainWindow::OnPlaybackTime() {
    auto pc = playback_control::get();
    metadb_handle_ptr track;
    if (pc->get_now_playing(track)) {
        m_position = SafeCorePosition(track);
        m_length = pc->playback_get_length();
    } else if (!m_lyricsTrack.is_valid()) {
        m_position = 0;
        m_length = 0;
    }
    if (!m_seeking) {
        m_seekPaint.Request();
        FlushPlaybackPaints();
    }
    RepaintNowPlaying();
    if (m_pane) m_pane->OnPlaybackTime();
    StartSeekFrames(); // after a seek, a restore or a song change
    if (m_player) m_player->OnTime();
}

// The seek bar's head follows the music on the compositor's frames, a device pixel at a time,
// instead of jumping with the playback time callback, which comes once a second.
void MainWindow::StartSeekFrames() {
    if (!m_playing || m_paused || !PlaybackPaintAllowed()) return;
    if (!m_frameClock) m_frameClock = std::make_unique<gfx::FrameClock>(m_hwnd, kFrameMessage);
    m_frameClock->Start();
}

bool MainWindow::TickSeek() {
    if (!m_playing || m_paused || m_seeking || m_length <= 0) return false;
    auto* seek = WidgetById(kSeek);
    if (!seek) return false;
    const RECT& r = seek->Bounds();
    if (r.right <= r.left) return false;
    metadb_handle_ptr playing;
    if (!playback_control::get()->get_now_playing(playing)) return true;
    const double position = SafeCorePosition(playing);
    const long head = std::lround((r.right - r.left) * std::clamp(position / m_length, 0.0, 1.0));
    if (head != m_seekHead) {
        m_seekHead = head;
        m_position = position;
        seek->Invalidate();
    }
    return true;
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
        for (int id : {kVolume, kMute}) // the speaker icon shows the level
            if (auto* w = WidgetById(id)) w->Invalidate();
        if (m_volumeTipShown) ShowVolumeTip(VolumeThumbX()); // keep a visible bubble current
    }
    if (m_player) m_player->OnVolume();
}

void MainWindow::OnPlaybackOrderChanged() {
    auto& scheduler = media::PlaybackScheduler::Get();
    scheduler.CorePlaybackOrderChanged();
    if (m_applyingOrder) return;
    fb2k::inMainThread([this, alive = std::weak_ptr<int>(m_alive)] {
        if (alive.expired()) return;
        if (auto* host = media::PlaybackScheduler::Existing())
            UpdateShuffleRepeat(host->QueueShuffleEnabled(), static_cast<Repeat>(host->QueueRepeatMode()));
    });
}

void MainWindow::UpdateShuffleRepeat(bool shuffle, Repeat repeat) {
    struct NotifyPlayer {
        MainWindow& window;
        ~NotifyPlayer() {
            if (window.m_player) window.m_player->OnPlayback();
        }
    } notifyPlayer{*this};
    m_shuffle = shuffle;
    m_repeat = repeat;
    // Repaint even when the values did not change: at startup they come from cfg after the
    // buttons were first painted, and the toggles must show the restored state.
    for (int id : {kShuffle, kRepeat})
        if (auto* w = WidgetById(id)) w->Invalidate();
    cfg_shuffle = shuffle ? 1 : 0;
    cfg_repeat = static_cast<int>(repeat);
    SchedulePaneQueueRefresh();
}

void MainWindow::SetShuffleRepeat(bool shuffle, Repeat repeat) {
    if (media::radio::Active()) { // no shuffle and no list repeat on a station
        shuffle = false;
        if (repeat == Repeat::All) repeat = Repeat::Off;
    }
    UpdateShuffleRepeat(shuffle, repeat);
    ApplyPlaybackOrder();
}

// A station started: shuffle and list repeat go (kept for after it); it ended: they come back.
// Its songs moving on can change whether Previous does anything, so the buttons follow each call.
void MainWindow::OnRadioChanged() {
    const bool active = media::radio::Active();
    if (active != m_radioActive) {
        m_radioActive = active;
        if (active) {
            if (!m_beforeRadio) m_beforeRadio = std::make_pair(m_shuffle, static_cast<int>(m_repeat));
            SetShuffleRepeat(false, m_repeat == Repeat::One ? Repeat::One : Repeat::Off);
        } else if (m_beforeRadio) {
            const auto [shuffle, repeat] = *std::exchange(m_beforeRadio, std::nullopt);
            SetShuffleRepeat(shuffle, static_cast<Repeat>(repeat));
        }
        SchedulePaneQueueRefresh();
    }
    for (int id : {kShuffle, kRepeat, kPrev, kNext})
        if (auto* w = WidgetById(id)) w->Invalidate();
    if (m_taskbar) m_taskbar->RefreshButtons();
    if (m_player) m_player->RadioChanged();
}

void MainWindow::CycleRepeat() {
    const auto modes = media::transport::RepeatPressed({m_shuffle, static_cast<int>(m_repeat)});
    SetShuffleRepeat(modes.shuffle, static_cast<Repeat>(modes.repeat));
}

void MainWindow::SelectPlaybackMode(bool shuffle, int repeat) {
    SetShuffleRepeat(shuffle, static_cast<Repeat>(std::clamp(repeat, 0, 2)));
}

void MainWindow::ApplyPlaybackOrder() {
    m_applyingOrder = true;
    media::PlaybackScheduler::Get().SetPlaybackMode(m_shuffle, static_cast<int>(m_repeat));
    m_applyingOrder = false;
}

void MainWindow::ApplyLanguage(Language language) {
    SetLanguageSetting(language);
    for (auto& w : m_widgets)
        w->Invalidate();
    if (m_nav) m_nav->Rebuild();
    RepaintBreadcrumb();
    RepaintStreamInfo();
    if (m_list) m_list->Refresh();
    UpdateLayout(); // button widths follow the text
}

void MainWindow::UpdateFavoriteState() {
    metadb_handle_ptr now;
    const bool fav = playback_control::get()->get_now_playing(now) && media::IsFavorite(now);
    if (m_taskbar) m_taskbar->SetFavorite(fav);
    if (m_playerTaskbar) m_playerTaskbar->SetFavorite(fav);
    if (fav != m_npFavorite) {
        m_npFavorite = fav;
        RepaintNowPlaying();
        if (m_player) m_player->OnPlayback();
    }
}

void MainWindow::OnPlaylistContentChanged(size_t playlist) {
    if (playlist == media::FavoritesPlaylist(false)) UpdateFavoriteState();
    if (m_player && playlist == playlist_manager::get()->get_playing_playlist()) m_player->WallAlbumsChanged();
    if (media::IsQueue(playlist)) SchedulePaneQueueRefresh();
    if (m_view.kind == View::Kind::Playlist && m_view.playlist == playlist && m_source) {
        static_cast<media::PlaylistSource*>(m_source.get())->Rebuild();
        m_list->Refresh();
    } else if (m_list) {
        m_list->Refresh(); // playing marker may have moved
    }
}

void MainWindow::OnPlaylistSelectionChanged(size_t playlist) {
    if (m_view.kind == View::Kind::Playlist && m_view.playlist == playlist && m_list) m_list->Refresh();
}

void MainWindow::OnPlaylistsMoved(const std::function<size_t(size_t)>& remap) {
    // A page whose playlist was deleted becomes that section's Playlists page.
    auto fix = [&remap](View& view) {
        if (view.kind != View::Kind::Playlist) return;
        if (const size_t moved = remap(view.playlist); moved != SIZE_MAX) {
            view.playlist = moved;
            return;
        }
        const bool smart = view.library == View::Library::Local;
        view = {};
        view.library = smart ? View::Library::Local : View::Library::All;
        view.kind = smart ? View::Kind::SmartPlaylists : View::Kind::Playlists;
    };
    fix(m_view);
    for (auto& view : m_history)
        fix(view);
    OnPlaylistsChanged();
}

void MainWindow::OnPlaylistsChanged() {
    SchedulePaneQueueRefresh();
    if (!m_nav) return;
    m_nav->Rebuild();
    if (m_view.kind == View::Kind::Playlist || m_view.kind == View::Kind::Playlists ||
        m_view.kind == View::Kind::SmartPlaylists)
        Navigate(m_view, false); // index, name or the set of playlists may have changed
}

void MainWindow::OnPlaylistLockChanged() {
    // foobar2000 restores auto playlists after the UI is up; each one shows up as a lock.
    // The lock's filter mask tells the nav pane it is an auto playlist before the client is
    // registered, so this one rebuild is enough.
    if (!m_nav) return;
    m_nav->Rebuild();
    if (m_view.kind == View::Kind::Playlists || m_view.kind == View::Kind::SmartPlaylists)
        Navigate(m_view, false);
    else
        RepaintBreadcrumb();
}

void MainWindow::ScheduleLibraryReload() {
    if (m_hwnd) SetTimer(m_hwnd, kLibraryTimer, 400, nullptr); // debounce bursts of library events
}

RECT MainWindow::ClusterRect(const D2D1_RECT_F& r) const {
    const POINT o = m_layout.cluster;
    return {o.x + PxI(r.left), o.y + PxI(r.top), o.x + PxI(r.right), o.y + PxI(r.bottom)};
}

D2D1_RECT_F MainWindow::ClusterRegion(const RECT& r) const {
    const POINT o = m_layout.cluster;
    return {(r.left - o.x) / m_scale, (r.top - o.y) / m_scale, (r.right - o.x) / m_scale,
            (r.bottom - o.y) / m_scale};
}

// With the original artwork the bars follow WMP 12's bitmaps (27-DIP orbs, 51-DIP cluster).
// WMP 12 at 150 %: the nav bar is 35 px with the 26 px orbs nearly filling it; ours keeps the larger
// orbs (22 DIP) with 3-4 DIP above and below.
float MainWindow::NavBarHeight() const {
    return m_win7Art ? 29.0f : m::kNavBarHeight;
}
float MainWindow::TransportHeight() const {
    return m_win7Art ? wt::kBandHeight : m::kTransportHeight;
}

void MainWindow::SetVolumeFromX(int x) {
    const RECT& r = m_layout.volume;
    float x0, x1;
    if (m_win7Art) { // from the slider's own place: it may stand in the flyout
        x0 = r.left + Px(wt::kThumbMin - wt::kVolumeRect.left);
        x1 = r.left + Px(wt::kThumbMax - wt::kVolumeRect.left);
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
    KillTimer(m_hwnd, kVolumeTipTimer); // dragging shows at once, no hover delay
    m_volumeHover = true;
    ShowVolumeTip(VolumeThumbX());
}

float MainWindow::VolumeThumbX() const {
    float x0, x1;
    if (m_win7Art) {
        x0 = m_layout.volume.left + Px(wt::kThumbMin - wt::kVolumeRect.left);
        x1 = m_layout.volume.left + Px(wt::kThumbMax - wt::kVolumeRect.left);
    } else {
        const float thumb = Px(11.0f);
        x0 = m_layout.volume.left + thumb / 2;
        x1 = m_layout.volume.right - thumb / 2;
    }
    return x0 + m_volume * std::max(1.0f, x1 - x0);
}

void MainWindow::ShowVolumeTip(float thumbX) {
    const std::wstring text = std::to_wstring(static_cast<int>(std::lround(m_volume * 100))) + L"%";
    ShowTip(text, thumbX, static_cast<float>(m_layout.volume.top - PxI(2)), true);
}

// Windows 7's tooltip: the volume while it is dragged or hovered, and text cut short (a detail
// value) after the pointer rests on it. One bubble for the window; long text wraps.
// `above`: centred over (anchorX, anchorY), else its left edge at anchorX below anchorY.
void MainWindow::ShowTip(const std::wstring& text, float anchorX, float anchorY, bool above) {
    // Win7 tooltip: white to #E4E5F0, #767676 border, small radius, soft shadow, grey text.
    auto& host = gfx::CompositionHost::Get();
    if (!m_volumeTip) {
        m_volumeTip = host.Compositor().CreateSpriteVisual();
        m_volumeTip.Opacity(0.0f);
        m_windowRoot.Children().InsertAtTop(m_volumeTip); // above the clipped content and frame
    }
    constexpr float kMaxTextW = 420;
    auto measure = MakeWrappedText(text, 12.0f, DWRITE_FONT_WEIGHT_NORMAL, kMaxTextW, 2000);
    DWRITE_TEXT_METRICS tm{};
    measure->GetMetrics(&tm);
    const float textW = std::ceil(tm.widthIncludingTrailingWhitespace), textH = std::ceil(tm.height);
    const float shadow = 3, w = textW + 14, h = std::max(21.0f, textH + 5);
    auto layout = MakeWrappedText(text, 12.0f, DWRITE_FONT_WEIGHT_NORMAL, textW + 1, textH + 1);
    const int pw = PxI(w + shadow), ph = PxI(h + shadow);
    host.Repaint(m_volumeTip, pw, ph, m_scale, [&](ID2D1DeviceContext* dc) {
        const float px = 1.0f / m_scale;
        for (int i = 3; i >= 1; --i) {
            com_ptr<ID2D1SolidColorBrush> s;
            dc->CreateSolidColorBrush(D2D1::ColorF(0, 0.06f), s.put());
            dc->FillRoundedRectangle(
                {{static_cast<float>(i), static_cast<float>(i), w - 1 + i, h - 1 + i}, 3, 3}, s.get());
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
        dc->DrawTextLayout({7, std::round((h - textH) / 2)}, layout.get(), ink.get());
    });
    RECT client{};
    GetClientRect(m_hwnd, &client);
    const float left = std::clamp(above ? anchorX - Px(w) / 2 : anchorX, 0.0f,
                                  static_cast<float>(std::max<LONG>(0, client.right - pw)));
    const float top = std::clamp(above ? anchorY - ph : anchorY, 0.0f,
                                 static_cast<float>(std::max<LONG>(0, client.bottom - ph)));
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
    HideTip();
}

void MainWindow::HideTip() {
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
    for (auto it = m_widgets.rbegin(); it != m_widgets.rend(); ++it) // later widgets are on top
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
        const bool nearL = pt.x < corner, nearR = pt.x >= W - corner, nearT = pt.y < corner,
                   nearB = pt.y >= H - corner;
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
    UpdateSplitPlate();
}

bool MainWindow::IsSplit(const Widget* w) const {
    return w && (w->Id() == kNewPlaylist || w->Id() == kViewOptions);
}

void MainWindow::UpdateSplitPart(POINT pt) {
    // Which half of the hovered split button the mouse is over; the plates repaint on change.
    Widget* w = m_pressed ? m_pressed : m_hover;
    if (!IsSplit(w)) return;
    const float split = w->Id() == kNewPlaylist ? m_newPlaylistSplit : kViewOptionsSplit;
    const int part = pt.x - w->Bounds().left >= PxI(split) ? 1 : 0;
    if (part == m_splitPart || m_pressed) return; // keep the pressed half while held
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
    SetHoverWidget(nullptr); // the menu loop swallowed the mouse leave
}
void MainWindow::OnActivate(bool active) {
    if (m_active == active) return;
    m_active = active;
    if (!active) SetVolumeFlyout(false);
    if (!active) HideSearchPopup();
    if (m_frame) m_frame->SetActive(active);
    if (m_pane) m_pane->SetActive(active);
    for (auto& w : m_widgets)
        if (IsCaptionWidget(w->Id())) w->SetInactive(!active);
    if (m_title) gfx::CompositionHost::Get().AnimateOpacity(m_title, active ? 1.0f : 0.85f, 200);
    m_shadow.SetActive(active);
}

// ---- Window procedure ----------------------------------------------------------------

LRESULT MainWindow::WndProc(UINT msg, WPARAM wp, LPARAM lp) {
    const HWND hwnd = m_hwnd;
    if (m_taskbar && m_taskbar->OnMessage(msg, wp, lp)) return 0;
    switch (msg) {
    case WM_CREATE: {
        // user_interface::init precedes initquit::on_init. Initialize this UI's queue
        // service before any widget/startup callback queries or changes its mode.
        media::PlaybackScheduler::Start();
        m_scheduleListener =
            media::PlaybackScheduler::Get().AddListener([this, alive = std::weak_ptr<int>(m_alive)] {
                if (alive.expired()) return;
                if (auto* scheduler = media::PlaybackScheduler::Existing())
                    UpdateShuffleRepeat(scheduler->QueueShuffleEnabled(),
                                        static_cast<Repeat>(scheduler->QueueRepeatMode()));
                SchedulePaneQueueRefresh();
            });
        m_scale = GetDpiForWindow(hwnd) / 96.0f;
        InitComposition();
        CreateWidgets();
        RepaintScaleDependent();
        m_deviceReplaced = gfx::CompositionHost::Get().OnDeviceReplaced([this] { OnDeviceReplaced(); });
        m_shadow.Create(hwnd);
        media::EnsureSpecialPlaylists(false); // queue, Favorites (protected), smart playlists
        if (m_nav) m_nav->Rebuild();
        fb2k::inMainThread([this] { // once start-up is done
            // Keep lang/strings-en.txt current for scripts/translate-deepseek.ps1: our strings
            // and foobar2000's main menu plus a song's context menu.
            metadb_handle_list sample;
            if (!m_library.Tracks().empty()) sample.add_item(m_library.Tracks().front().handle);
            ExportStrings(sample);
        });
        m_taskbar = std::make_unique<Taskbar>(hwnd);
        m_taskbar->onFavorite = [this] {
            metadb_handle_ptr now;
            if (playback_control::get()->get_now_playing(now)) {
                auto* provider = stream::ProviderOf(now);
                const bool next = !media::IsFavorite(now);
                if (!provider || provider->CanSetFavorite(next)) media::ToggleFavorite(now);
            }
            UpdateFavoriteState();
        };
        Taskbar::SetupJumpList();
        m_library.Reload();
        m_playCallback = std::make_unique<PlayCallback>(*this);
        m_playlistCallback = std::make_unique<PlaylistCallback>(*this);
        m_libraryCallback = std::make_unique<LibraryCallback>(*this);
        m_artNotify = now_playing_album_art_notify_manager::get()->add(
            [this, alive = std::weak_ptr<int>(m_alive)](album_art_data::ptr) {
                fb2k::inMainThread([this, alive] {
                    if (!alive.expired()) RefreshNowPlayingArt();
                });
            });
        m_dropTarget = MakeFileDropTarget([this](IDataObject* data) { AddDroppedFiles(data); });
        RegisterDragDrop(hwnd, m_dropTarget.get());

        // Start on the page that was open when the window closed; otherwise where
        // foobar2000 is (the active playlist, else the music library).
        View start;
        size_t startTop = SIZE_MAX;
        if (!LoadLastView(start, startTop)) {
            const t_size active = playlist_manager::get()->get_active_playlist();
            if (active != SIZE_MAX) {
                start.library = View::Library::All;
                start.kind = View::Kind::Playlist;
                start.playlist = active;
            }
        } else if (startTop != SIZE_MAX) {
            m_scrollPositions[ViewKey(start)] = {start.IsRemote() ? start.provider : stream::Id{}, startTop};
        }
        Navigate(start, true);

        OnPlaybackStateChanged();
        OnTrackChanged();
        OnVolumeChanged();
        m_shuffle = cfg_shuffle.get() != 0;
        m_repeat = static_cast<Repeat>(std::clamp<int>(static_cast<int>(cfg_repeat.get()), 0, 2));
        SetShuffleRepeat(m_shuffle, m_repeat);
        for (auto* provider : stream::Providers()) // the navigation lists and saved items
            if (provider->AccountVerified()) provider->RefreshLibrary();
        try { // output device changed elsewhere (preferences)
            m_outputCallback = output_manager_v2::get()->addCallback([this] {
                RefreshDeviceName();
                RepaintStreamInfo();
            });
        } catch (...) {
        }
        SetWindowPos(hwnd, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER);
        return 0;
    }

    case settings::kChangedMessage: // Preferences > Display > Glasswave applied something
        ApplyLanguage(CurrentLanguageSetting());
        ApplyTint(true);
        if (m_lyricsResult.status == lyrics::Status::Found) ReapplyLyrics();
        if (m_pane) m_pane->RestyleLyrics(); // sizes, fonts, highlight modes
        if (std::wstring signature = LyricsSettingsSignature(); signature != m_lyricsSettingsSignature) {
            m_lyricsSettingsSignature = std::move(signature);
            if (m_lyricsTrack.is_valid()) RequestLyrics(m_lyricsTrack, false);
        }
        if (m_view.kind == View::Kind::AllRecentPlayed || m_view.kind == View::Kind::AllMostPlayed)
            Navigate(m_view, false);
        else if (m_list)
            m_list->Refresh(); // rows drawn with the settings (song tags before the artist)
        RefreshPaneQueue();
        return 0;

    case kFrameMessage:
        if (m_frameClock) {
            m_frameClock->Ack();
            if (!PlaybackPaintAllowed()) {
                m_playbackPresentationSuspended = true;
                m_frameClock->Stop();
                return 0;
            }
            // A pane lent to the player runs on the player's frames.
            bool more = !m_paneLent && m_pane && m_pane->Tick();
            more |= TickNowPlayingLyric();
            more |= TickSeek();
            if (!more) m_frameClock->Stop();
        }
        return 0;

    case WM_TIMER:
        if (wp == kWindowOpenTimer) {
            KillTimer(hwnd, kWindowOpenTimer);
            m_shadow.SetSuppressed(false);
            m_shadow.Sync();
            return 0;
        }
        if (wp == kWindowCloseTimer) {
            KillTimer(hwnd, kWindowCloseTimer);
            standard_commands::main_exit();
            return 0;
        }
        if (wp == kArtHoldTimer) { // the waiting song shows now, its cover still to come
            ReleaseHeldTrack(true);
            return 0;
        }
        if (wp == kPaneRevealTimer) {
            StepPaneReveal();
            return 0;
        }
        if (wp == kLibraryTimer) {
            KillTimer(hwnd, kLibraryTimer);
            m_library.Reload();
            // Local pages show the new snapshot; provider pages do not depend on it.
            if (m_view.kind != View::Kind::Playlist && !m_view.IsRemote()) Navigate(m_view, false);
            return 0;
        }
        if (wp == kVolumeTipTimer) {
            KillTimer(hwnd, kVolumeTipTimer);
            if (m_volumeHover) ShowVolumeTip(VolumeThumbX());
            return 0;
        }
        if (wp == kDetailTipTimer) {
            KillTimer(hwnd, kDetailTipTimer);
            // Below the pointer, clear of the cursor, as Windows places its tooltips.
            // In the control bar, above the text instead (the window's bottom edge is near).
            if (!m_tipText.empty())
                ShowTip(m_tipText, static_cast<float>(m_detailTipAt.x),
                        static_cast<float>(m_tipAbove ? m_tipRect.top - PxI(2) : m_detailTipAt.y + PxI(20)),
                        m_tipAbove);
            return 0;
        }
        if (wp == kSuggestTimer) {
            KillTimer(hwnd, kSuggestTimer);
            FetchSuggestions();
            return 0;
        }
        if (wp == kScrollAnimTimer) {
            bool active = m_list && m_list->AnimateScrollBar();
            active |= AnimateDetailScrollBar();
            active |= m_crumbBar->Animate();
            active |= AnimateTabs();
            if (m_search) active |= m_search->Animate();
            if (!active) {
                KillTimer(hwnd, kScrollAnimTimer);
                m_scrollAnimActive = false;
            }
            return 0;
        }
        if (wp == kScrollRepeatTimer) {
            if (GetTickCount64() - m_scrollRepeatSince < 350) return 0;
            bool active = m_list && m_list->OnScrollRepeat();
            active |= OnDetailBriefRepeat();
            if (!active) {
                KillTimer(hwnd, kScrollRepeatTimer);
                m_scrollRepeatActive = false;
            }
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
                const int fx =
                    GetSystemMetricsForDpi(SM_CXFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
                const int fy =
                    GetSystemMetricsForDpi(SM_CYFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
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
                if (LPCWSTR cursor = m_list->CursorAt(pt)) { // column edges, album links
                    SetCursor(LoadCursorW(nullptr, cursor));
                    return TRUE;
                }
            if (NowPlayingPartAt(pt)) { // now-playing links, output device
                SetCursor(LoadCursorW(nullptr, IDC_HAND));
                return TRUE;
            }
        }
        break;

    case WM_NCACTIVATE:
        OnActivate(wp != FALSE);
        return DefWindowProcW(hwnd, msg, wp, -1); // -1: do not repaint the (non-existent) frame

    case WM_SHOWWINDOW: {
        const LRESULT result = DefWindowProcW(hwnd, msg, wp, lp);
        // The library shown from elsewhere (tray, another instance): the player gives way.
        if (wp && m_player && m_player->Shown()) m_player->Close();
        UpdatePlaybackPresentation();
        return result;
    }

    case WM_SIZE: {
        HideSearchPopup();
        if (wp == SIZE_MINIMIZED) {
            m_playbackPresentationSuspended = true;
            if (m_frameClock) m_frameClock->Stop();
        }
        if (m_list) m_list->SetMotion(wp != SIZE_MINIMIZED); // motion covers rest while minimized
        const bool wasMax = m_layout.border == 0;
        UpdateLayout();
        if (wp != SIZE_MINIMIZED) UpdatePlaybackPresentation();
        if (wasMax != (m_layout.border == 0))
            if (auto* w = WidgetById(kCapMax)) w->Invalidate();
        return 0;
    }

    case WM_MOVE:
        HideSearchPopup();
        if (m_frame) m_frame->Glass().UpdateReflectionAnchor(hwnd);
        return 0;

    case WM_WINDOWPOSCHANGING: {
        const auto* pos = reinterpret_cast<const WINDOWPOS*>(lp);
        if (m_frame && !(pos->flags & SWP_NOMOVE) && !IsIconic(hwnd)) {
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
        UpdatePlaybackPresentation();
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
        // Narrow, but with nothing overlapping: the transport's narrowest arrangement (song text
        // 110, time 110, the capsule without shuffle, repeat, stop and slider 212, the lyrics
        // toggle, 12 DIP gaps) needs a 516 DIP client; the command bar and the navigation pane
        // give way before (UpdateLayout).
        mmi->ptMinTrackSize = {PxI(540 + (m_paneReveal >= 0 ? m_paneReveal / m_scale
                                                          : m_lyricsOpen ? kLyricsPaneWidth : 0.0f)),
                               PxI(420)};
        return 0;
    }

    case WM_SETTINGCHANGE:
        if (m_frame) m_frame->Glass().RefreshTransparencyPolicy();
        break;

    case WM_DEVICECHANGE:
        // A disc came or went: the navigation pane shows the drives' CDs (cd/discs.h), and a
        // disc taken out leaves the queue (cd/playback.h).
        if ((wp == DBT_DEVICEARRIVAL || wp == DBT_DEVICEREMOVECOMPLETE) && lp &&
            reinterpret_cast<const DEV_BROADCAST_HDR*>(lp)->dbch_devicetype == DBT_DEVTYP_VOLUME &&
            (reinterpret_cast<const DEV_BROADCAST_VOLUME*>(lp)->dbcv_flags & DBTF_MEDIA)) {
            cd::OnMediaChanged(reinterpret_cast<const DEV_BROADCAST_VOLUME*>(lp)->dbcv_unitmask,
                               wp == DBT_DEVICEARRIVAL);
            if (wp == DBT_DEVICEREMOVECOMPLETE)
                cd::DropEjectedDiscs([] {
                    auto* window = Instance();
                    if (!window) return;
                    window->OnCoreQueueChanged(); // history rows
                    // The disc's songs leave Recently Played and Most Played (media::PlayStats).
                    if (window->m_view.kind == View::Kind::AllRecentPlayed ||
                        window->m_view.kind == View::Kind::AllMostPlayed)
                        window->Navigate(window->m_view, false);
                });
        }
        break;

    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        ValidateRect(hwnd, nullptr);
        return 0;

    // -- Caption buttons (non-client messages) --
    case WM_NCMOUSEMOVE: {
        Widget* w = nullptr;
        if (wp == HTMINBUTTON)
            w = WidgetById(kCapMin);
        else if (wp == HTMAXBUTTON)
            w = WidgetById(kCapMax);
        else if (wp == HTCLOSE)
            w = WidgetById(kCapClose);
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
            return 0; // keep DefWindowProc from drawing classic buttons
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
        if (m_capture == Capture::Lyrics) {
            m_pane->OnMouseMove(pt);
            return 0;
        }
        if (m_pane) {
            const bool over = m_capture == Capture::None && m_pane->HitTest(pt);
            if (over != m_lyricsHover) {
                m_lyricsHover = over;
                if (!over) m_pane->OnMouseLeave();
            }
            if (over) m_pane->OnMouseMove(pt);
        }
        if (m_capture == Capture::DetailScroll) {
            OnDetailBriefBarMove(pt);
            return 0;
        }
        if (m_capture == Capture::Search) {
            m_search->OnMouseDrag(pt);
            return 0;
        }
        if (m_capture == Capture::Nav) {
            m_nav->OnMouseMove(pt);
            return 0;
        }
        if (m_capture == Capture::List) {
            m_list->OnMouseMove(pt);
        } else if (m_pressed) {
            if (m_pressed->Id() == kVolume) {
                SetVolumeFromX(pt.x);
            } else if (m_pressed->Id() == kSeek) {
                const int prevSec = static_cast<int>(m_seekPreview * m_length);
                m_seekPreview = SeekFractionAt(pt.x);
                m_pressed->Invalidate();
                if (static_cast<int>(m_seekPreview * m_length) != prevSec) RepaintNowPlaying();
            } else {
                m_pressed->SetPressed(m_pressed->HitTest(pt));
            }
        } else {
            SetHoverWidget(WidgetAt(pt));
            UpdateSplitPart(pt);
            m_list->OnMouseMove(pt);
            m_nav->OnMouseMove(pt);
            m_crumbBar->OnMouseMove(pt);
            UpdateTabHover(pt);
            m_search->SetClearHot(m_search->HitClear(pt));
            if (m_search->Animating()) StartScrollAnimation();
            UpdateNowPlayingHover(pt);
            UpdateDetailBriefHover(pt);
            UpdateDetailTip(pt);
            const bool overVolume = PtInRect(&m_layout.volume, pt) != FALSE;
            if (overVolume) {
                if (!m_volumeHover) {
                    m_volumeHover = true;
                    SetTimer(hwnd, kVolumeTipTimer, static_cast<UINT>(settings::VolumeTipDelayMs()), nullptr);
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
        if (m_lyricsHover && m_capture != Capture::Lyrics) {
            m_lyricsHover = false;
            m_pane->OnMouseLeave();
        }
        if (m_briefScroll.SetHot(ScrollBarModel::kNone)) StartScrollAnimation();
        if (m_hover && !IsCaptionWidget(m_hover->Id()) && !m_pressed) SetHoverWidget(nullptr);
        if (m_capture == Capture::None) {
            m_list->OnMouseLeave();
            m_nav->OnMouseLeave();
            m_crumbBar->OnMouseLeave();
            UpdateTabHover({-1, -1});
            m_search->SetClearHot(false);
            if (m_search->Animating()) StartScrollAnimation();
            UpdateNowPlayingHover({-1, -1});
            m_volumeHover = false;
            HideVolumeTip();
            UpdateDetailTip({-1, -1});
        }
        return 0;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK: {
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        const bool dbl = msg == WM_LBUTTONDBLCLK;
        // A click anywhere but on the volume flyout or its arrow closes the flyout.
        if (m_volFlyoutOpen && !PtInRect(&m_layout.volFlyout, pt) && !PtInRect(&m_layout.volArrow, pt))
            SetVolumeFlyout(false);
        if (m_pane && m_pane->HitTest(pt)) {
            m_pane->OnLButtonDown(pt);
            m_capture = Capture::Lyrics;
            SetCapture(hwnd);
            return 0;
        }
        if (m_detailBriefScrollable && PtInRect(&m_layout.detailBriefBar, pt)) {
            OnDetailBriefBarDown(pt);
            m_capture = Capture::DetailScroll;
            SetCapture(hwnd);
            return 0;
        }
        if (PtInRect(&m_layout.breadcrumb, pt)) {
            m_crumbBar->OnClick(m_hwnd, pt);
            return 0;
        }
        if (PtInRect(&m_layout.tabs, pt)) {
            OnTabClick(pt);
            return 0;
        }
        if (const int part = NowPlayingPartAt(pt)) {
            OnNowPlayingClick(part);
            return 0;
        }
        if (Widget* w = WidgetAt(pt)) {
            m_pressed = w;
            w->SetPressed(true);
            UpdateSplitPlate();
            m_capture = Capture::Widget;
            SetCapture(hwnd);
            if (w->Id() == kVolume) SetVolumeFromX(pt.x);
            if (w->Id() == kSeek && m_length > 0) {
                m_seeking = true;
                m_seekPreview = SeekFractionAt(pt.x);
                w->Invalidate();
                RepaintNowPlaying();
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
            if (m_nav->OnLButtonDown(pt)) {
                m_capture = Capture::Nav;
                SetCapture(hwnd);
            }
            return 0;
        }
        if (m_search->HitClear(pt)) { // held: the pressed plate; it clears when released on it
            m_search->SetClearPressed(true);
            StartScrollAnimation();
            m_capture = Capture::SearchClear;
            SetCapture(hwnd);
            return 0;
        }
        if (m_search->HitTest(pt)) {
            if (!m_search->Focused()) SetSearchFocus(true);
            if (dbl) {
                m_search->OnDoubleClick(pt);
            } else {
                m_search->OnMouseDown(pt, KeyDown(VK_SHIFT));
                m_capture = Capture::Search;
                SetCapture(hwnd);
            }
            return 0;
        }
        SetSearchFocus(false);
        return 0;
    }
    case WM_LBUTTONUP: {
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        m_ncDownId = 0;
        if (m_capture == Capture::Lyrics) {
            m_capture = Capture::None;
            ReleaseCapture();
            m_pane->OnLButtonUp(pt);
            return 0;
        }
        if (m_capture == Capture::DetailScroll) {
            m_capture = Capture::None;
            ReleaseCapture();
            OnDetailBriefBarUp();
            return 0;
        }
        if (m_capture == Capture::Search) {
            m_capture = Capture::None;
            ReleaseCapture();
            return 0;
        }
        if (m_capture == Capture::SearchClear) {
            m_capture = Capture::None;
            ReleaseCapture();
            m_search->SetClearPressed(false);
            StartScrollAnimation();
            if (m_search->HitClear(pt)) OnSearchClear();
            return 0;
        }
        if (m_capture == Capture::Nav) {
            m_capture = Capture::None;
            ReleaseCapture();
            if (const NavPane::Item* item = m_nav->OnLButtonUp(pt)) {
                const NavPane::Item copy = *item;
                OnNavItem(copy);
            }
            return 0;
        }
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
            UpdateSplitPlate();
            if (w->Id() == kVolume) {
                m_volumeHover = PtInRect(&m_layout.volume, pt) != FALSE;
                if (m_volumeHover)
                    ShowVolumeTip(VolumeThumbX());
                else
                    HideVolumeTip();
            }
            if (w->Id() == kSeek) {
                if (seek && m_length > 0) playback_control::get()->playback_seek(preview * m_length);
                m_seeking = false;
                w->Invalidate();
                RepaintNowPlaying();
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
                UpdateSplitPlate();
            }
            if (m_capture == Capture::List) m_list->CancelDrag();
            if (m_capture == Capture::Nav) m_nav->CancelDrag();
            if (m_capture == Capture::Lyrics && m_pane) m_pane->OnCaptureLost();
            if (m_capture == Capture::DetailScroll) {
                m_briefScroll.Release();
                StartScrollAnimation();
            }
            m_capture = Capture::None;
            m_seeking = false;
        }
        return 0;
    case WM_MOUSEWHEEL: {
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ScreenToClient(hwnd, &pt);
        const int delta = GET_WHEEL_DELTA_WPARAM(wp);
        if (m_pane && m_pane->HitTest(pt)) {
            m_pane->OnWheel(delta);
            return 0;
        }
        if (m_detailBriefScrollable &&
            (PtInRect(&m_layout.detailBriefArea, pt) || PtInRect(&m_layout.detailBriefBar, pt))) {
            ScrollDetailBrief(-delta / static_cast<float>(WHEEL_DELTA) * 54.0f);
            return 0;
        }
        if (m_list->HitTest(pt)) {
            m_list->OnWheel(delta, (GET_KEYSTATE_WPARAM(wp) & MK_SHIFT) != 0);
            m_list->OnMouseMove(pt);
            return 0;
        }
        if (m_nav->HitTest(pt)) {
            m_nav->OnWheel(delta);
            return 0;
        }
        if (PtInRect(&m_layout.volume, pt)) { // the volume slider itself, not the whole band
            auto pc = playback_control::get();
            if (delta > 0)
                pc->volume_up();
            else
                pc->volume_down();
            return 0;
        }
        break;
    }
    case WM_MOUSEHWHEEL: { // tilt wheel / touchpad sideways: the list scrolls horizontally
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
        // The playing song's cover and title: the song's own menu, as on its row in a list.
        if (PtInRect(&m_npArtRect, pt) || PtInRect(&m_npTitleRect, pt)) {
            POINT screen = pt;
            ClientToScreen(hwnd, &screen);
            ShowNowPlayingSongMenu(screen);
            return 0;
        }
        // The song's lines and the lyric line under them (the menu turns a hidden line back on).
        const RECT lyricRect{m_layout.nowPlaying.left + PxI(m_npTextX), m_layout.nowPlaying.top,
                             m_layout.nowPlaying.right, m_layout.nowPlaying.bottom};
        if (PtInRect(&lyricRect, pt)) {
            POINT screen = pt;
            ClientToScreen(hwnd, &screen);
            ShowLyricsMenu(screen);
            return 0;
        }
        if (m_pane && m_pane->HitTest(pt)) {
            POINT screen = pt;
            ClientToScreen(hwnd, &screen);
            m_pane->OnRButtonUp(pt, screen);
            return 0;
        }
        if (m_list->HitTest(pt)) {
            m_list->OnRButtonUp(hwnd, pt);
            return 0;
        }
        POINT screen = pt;
        ClientToScreen(hwnd, &screen);
        if (m_nav->HitTest(pt)) {
            const NavPane::Item* item = m_nav->ItemAt(pt);
            if (item && (item->target == NavPane::Target::LibraryLocal ||
                         item->target == NavPane::Target::Category)) {
                ShowNavCustomizeMenu(screen);
                return 0;
            }
            if (item) {
                switch (item->target) {
                case NavPane::Target::LibraryRemote:
                case NavPane::Target::RemoteSection:
                case NavPane::Target::RemotePlaylist:
                    ShowRemoteNavMenu(*item, screen);
                    return 0;
                case NavPane::Target::LibraryAll:
                case NavPane::Target::AllFavorites:
                case NavPane::Target::AllRecentPlayed:
                case NavPane::Target::AllMostPlayed:
                case NavPane::Target::AllDownloads:
                case NavPane::Target::Home:
                    // Page actions live on the list's own empty area; nothing applies to the row.
                    return 0;
                case NavPane::Target::AudioCd:
                    ShowDiscMenu(static_cast<wchar_t>(item->playlist), screen);
                    return 0;
                case NavPane::Target::DiscsHeader:
                    return 0;
                default:
                    break;
                }
            }
            const bool playlistMenu =
                item && (item->target == NavPane::Target::Playlist || item->target == NavPane::Target::Queue);
            ShowPlaylistMenu(playlistMenu ? item->playlist : SIZE_MAX, screen);
            return 0;
        }
        if (PtInRect(&m_layout.detailHeader, pt) && m_detail.visible) {
            ShowPageContextMenu(screen);
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
                if (OnSearchKey(wp)) return 0;
                if (m_search->OnKeyDown(static_cast<UINT>(wp), ctrl, hwnd)) return 0;
                if (wp == VK_DOWN || wp == VK_TAB) {
                    SetSearchFocus(false);
                    m_list->OnKeyDown(VK_HOME, false, false);
                    return 0;
                }
            }
            if (wp == VK_F11) {
                ShowPlayer(true);
                return 0;
            }
            if (ctrl && shift && wp == 'M') { // WMP 12: switch to the mini player
                ShowPlayer(false);
                return 0;
            }
            if (wp == VK_SPACE && !m_search->Focused()) { // WMP's play / pause
                media::transport::PlayPause();
                return 0;
            }
            if (wp == VK_BACK && !m_search->Focused()) {
                GoBack();
                return 0;
            }
            if (wp == VK_F5 && !m_search->Focused()) {
                RefreshCurrentPage();
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
    // The search field draws the IME composition itself (the IME's own composition window
    // floated over the box) and keeps the candidate list just under the box.
    case WM_IME_SETCONTEXT:
        if (wp && m_search->Focused()) lp &= ~static_cast<LPARAM>(ISC_SHOWUICOMPOSITIONWINDOW);
        break;
    case WM_IME_STARTCOMPOSITION:
        if (m_search->Focused()) {
            PlaceImeWindows();
            return 0;
        }
        break;
    case WM_IME_COMPOSITION:
        if (m_search->Focused()) {
            if (HIMC imc = ImmGetContext(hwnd)) {
                auto read = [imc](DWORD kind) {
                    const LONG bytes = ImmGetCompositionStringW(imc, kind, nullptr, 0);
                    std::wstring text(bytes > 0 ? static_cast<size_t>(bytes) / sizeof(wchar_t) : 0, L'\0');
                    if (!text.empty())
                        ImmGetCompositionStringW(imc, kind, text.data(), static_cast<DWORD>(bytes));
                    return text;
                };
                if (lp & GCS_RESULTSTR) m_search->InsertText(read(GCS_RESULTSTR));
                if (lp & GCS_COMPSTR) {
                    const LONG cursor =
                        (lp & GCS_CURSORPOS) ? ImmGetCompositionStringW(imc, GCS_CURSORPOS, nullptr, 0) : -1;
                    std::wstring text = read(GCS_COMPSTR);
                    const size_t at = cursor >= 0 ? static_cast<size_t>(cursor) : text.size();
                    m_search->SetComposition(std::move(text), at);
                } else if (lp & GCS_RESULTSTR) {
                    m_search->SetComposition({}, 0);
                }
                ImmReleaseContext(hwnd, imc);
            }
            PlaceImeWindows();
            return 0;
        }
        break;
    case WM_IME_ENDCOMPOSITION:
        if (m_search->Focused()) {
            m_search->SetComposition({}, 0);
            return 0;
        }
        break;
    case WM_XBUTTONUP:
        if (GET_XBUTTON_WPARAM(wp) == XBUTTON1)
            GoBack();
        else if (GET_XBUTTON_WPARAM(wp) == XBUTTON2)
            GoForward();
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
        // Visible, it first closes the way Windows 7 closed windows (kWindowCloseTimer).
        if (!m_closing && IsWindowVisible(hwnd) && !IsIconic(hwnd) && WindowAnimations()) {
            m_closing = true;
            AnimateWindow(false);
            return 0;
        }
        standard_commands::main_exit();
        return 0;

    case WM_DESTROY: {
        if (auto* scheduler = media::PlaybackScheduler::Existing())
            scheduler->RemoveListener(std::exchange(m_scheduleListener, 0));
        CloseLyricsSearch();
        WINDOWPLACEMENT wpl{sizeof(wpl)};
        if (GetWindowPlacement(hwnd, &wpl)) cfg_placement.set(wpl);
        SaveLastView();
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
        if (auto* cache = ArtCache::Existing()) cache->Unsubscribe(m_detailArtToken);
        m_detailArtToken = 0;
        media::radio::Unsubscribe(m_radioToken);
        m_radioToken = 0;
        SetContextActionProvider({});
        m_providerListener.reset();
        cd::RemoveDiscsListener(m_discsToken);
        m_discsToken = 0;
        m_alive.reset(); // provider callbacks that outlive the window do nothing
        m_page.reset();
        m_hover = m_pressed = nullptr;
        m_list.reset();
        m_nav.reset();
        media::SetPlayQueueListener({});
        m_searchPopup.reset();
        m_search.reset();
        m_frameClock.reset();
        m_paneLent = false;
        m_playerTaskbar.reset();
        m_player.reset();
        m_pane.reset();
        m_nowPlayingPaint.Clear();
        m_seekPaint.Clear();
        m_nowPlayingBacking.Clear();
        m_timeBacking.Clear();
        m_npLyric = nullptr;
        m_npLyricSurface = nullptr;
        m_nowPlaying = nullptr;
        m_time = nullptr;
        ++m_lyricsRequestGeneration;
        lyrics::Service::Get().Cancel();
        m_source.reset();
        m_widgets.clear();
        m_outputCallback.release();
        m_root = nullptr;
        m_windowRoot = nullptr;
        m_frame.reset();
        return 0;
    }
    case WM_NCDESTROY:
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        m_hwnd = nullptr;
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---- PageHost ------------------------------------------------------------------------------

void MainWindow::SetPageName(const std::wstring& name) {
    if (name.empty() || name == m_view.name) return;
    if (m_historyPos < m_history.size() && SameView(m_history[m_historyPos], m_view))
        m_history[m_historyPos].name = name;
    m_view.name = name;
    RepaintBreadcrumb();
}

void MainWindow::ShowSource(std::unique_ptr<media::ListSource> source) {
    if (!m_list || !source) return;
    // One linear pass per snapshot preserves exact duplicate occurrences. Repeated
    // FindRow calls both collapse equal tracks and become quadratic for Select All.
    const bool hadRows = m_source && m_source->Count();
    const size_t topRow = hadRows ? m_list->TopRow() : SIZE_MAX;
    const float topOffset = topRow != SIZE_MAX ? m_list->RowOffset(topRow) : 0.0f;
    // At the top, the new layout starts at its top too: its headers (an artist's band, a
    // group's caption) differ in height, so the first row's offset would leave a sliver.
    const bool atTop = hadRows && m_list->AtTop();
    const size_t oldFocus = hadRows ? m_source->Focus() : SIZE_MAX;
    const size_t oldSort = m_source ? m_source->SortColumn() : SIZE_MAX;
    const bool descending = m_source && m_source->SortDescending();
    std::unordered_set<std::wstring> selected;
    std::wstring anchor, focused;
    const auto identity = [](media::ListSource& list, size_t row,
                             std::unordered_map<std::wstring, size_t>& occurrences) {
        std::wstring base = list.RowKey(row);
        if (base.empty()) base = L"row:" + std::to_wstring(row);
        const size_t ordinal = occurrences[base]++;
        return std::to_wstring(base.size()) + L":" + base + L":" + std::to_wstring(ordinal);
    };
    if (hadRows) {
        std::unordered_map<std::wstring, size_t> occurrences;
        for (size_t row = 0; row < m_source->Count(); ++row) {
            const std::wstring key = identity(*m_source, row, occurrences);
            if (m_source->IsSelected(row)) selected.insert(key);
            if (row == topRow) anchor = key;
            if (row == oldFocus) focused = key;
        }
    }
    m_source = std::move(source);
    if (auto* style = ViewStyleSetting(m_view);
        style && m_source->SupportsViewStyle(static_cast<media::ViewStyle>(style->get())))
        m_source->SetViewStyle(static_cast<media::ViewStyle>(style->get()));
    if (oldSort != SIZE_MAX && m_source->CanSort(oldSort)) m_source->Sort(oldSort, descending);
    m_source->SetFilter(m_filter);
    m_list->SetSource(m_source.get(), false);
    std::vector<size_t> selection;
    size_t anchorRow = SIZE_MAX, focusRow = SIZE_MAX;
    std::unordered_map<std::wstring, size_t> occurrences;
    for (size_t row = 0; row < m_source->Count(); ++row) {
        const std::wstring key = identity(*m_source, row, occurrences);
        if (selected.contains(key)) selection.push_back(row);
        if (key == anchor) anchorRow = row;
        if (key == focused) focusRow = row;
    }
    m_source->SetSelection(selection);
    if (focusRow != SIZE_MAX) m_source->SetFocus(focusRow);
    if (anchorRow == SIZE_MAX && topRow != SIZE_MAX && m_source->Count())
        anchorRow = std::min(topRow, m_source->Count() - 1);
    if (atTop)
        m_list->ScrollToTop();
    else if (anchorRow != SIZE_MAX)
        m_list->ScrollToRow(anchorRow, topOffset);
    m_list->Refresh();
    if (m_page) m_page->OnPresentationChanged();
    if (auto* w = WidgetById(kViewOptions)) w->Invalidate();
    SelectPending();
}
void MainWindow::BeforeListChange() {
    m_pageAnchor.clear();
    m_pageAnchorOrdinal = 0;
    m_pageAnchorRow = m_list ? m_list->TopRow() : SIZE_MAX;
    if (!m_source || m_pageAnchorRow >= m_source->Count()) return;
    m_pageAnchorOffset = m_list->RowOffset(m_pageAnchorRow);
    m_pageAnchor = m_source->RowKey(m_pageAnchorRow);
    if (m_pageAnchor.empty()) return;
    for (size_t row = 0; row < m_pageAnchorRow; ++row)
        if (m_source->RowKey(row) == m_pageAnchor) ++m_pageAnchorOrdinal;
}

void MainWindow::ListChanged() {
    if (m_list) m_list->Refresh();
    if (m_list && m_source && m_pageAnchorRow != SIZE_MAX) {
        size_t ordinal = 0, target = SIZE_MAX;
        for (size_t row = 0; row < m_source->Count(); ++row)
            if (!m_pageAnchor.empty() && m_source->RowKey(row) == m_pageAnchor &&
                ordinal++ == m_pageAnchorOrdinal) {
                target = row;
                break;
            }
        if (target == SIZE_MAX && m_source->Count())
            target = std::min(m_pageAnchorRow, m_source->Count() - 1);
        // Back at the same distance from the top, not at the top: an album's header (or the
        // band) above the row stays in view (owner, 2026-11-14: every load hid the first header).
        if (target != SIZE_MAX) m_list->ScrollToRow(target, m_pageAnchorOffset);
    }
    m_pageAnchor.clear();
    m_pageAnchorRow = SIZE_MAX;
    SelectPending();
}

void MainWindow::ChromeChanged() {
    RefreshProviderSections();
    RepaintBreadcrumb(); // re-applies the layout: the band's height follows its text
    RepaintDetailHeader();
}

media::ViewStyle MainWindow::SavedStyle(media::ViewStyle fallback) const {
    auto* style = ViewStyleSetting(m_view);
    return style ? static_cast<media::ViewStyle>(style->get()) : fallback;
}

} // namespace glasswave::ui
