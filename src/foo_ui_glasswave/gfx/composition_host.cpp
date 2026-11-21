#include "pch.h"
#include "gfx/composition_host.h"
#include "core/log.h"

namespace glasswave::gfx {

namespace abi_comp = ABI::Windows::UI::Composition;
using winrt::Windows::Foundation::TimeSpan;
using winrt::Windows::Foundation::Numerics::float2;
using winrt::Windows::Foundation::Numerics::float3;

namespace {
std::unique_ptr<CompositionHost> g_host;

TimeSpan Ms(int ms) { return std::chrono::milliseconds(ms); }

struct SurfacePixels {
    int width = 1;
    int height = 1;

    RECT UpdateRect() const noexcept {
        return {0, 0, width, height};
    }
};

SurfacePixels SurfacePixelsFor(const wuc::CompositionDrawingSurface& surface) {
    const auto size = surface.Size();
    return {std::max(1, static_cast<int>(std::lround(size.Width))),
            std::max(1, static_cast<int>(std::lround(size.Height)))};
}
}  // namespace

SurfaceReusePolicy::Update RetainedSurface::Prepare(int widthPx, int heightPx) noexcept {
    return m_policy.Plan(static_cast<bool>(m_surface), widthPx, heightPx);
}

void RetainedSurface::Clear() noexcept {
    m_brush = nullptr;
    m_surface = nullptr;
    m_policy.Clear();
}

CompositionHost& CompositionHost::Get() {
    if (!g_host) g_host.reset(new CompositionHost());
    return *g_host;
}

void CompositionHost::Shutdown() {
    if (!g_host) return;
    auto controller = g_host->m_queueController;
    g_host.reset();
    // A DQTYPE_THREAD_CURRENT queue must be shut down before its thread exits;
    // shutdown is asynchronous and completes while foobar2000's message loop keeps pumping.
    if (controller) controller.ShutdownQueueAsync();
}

CompositionHost::CompositionHost() {
    // Another component may already have created a DispatcherQueue on this thread; reuse it.
    if (!winrt::Windows::System::DispatcherQueue::GetForCurrentThread()) {
        DispatcherQueueOptions options{sizeof(DispatcherQueueOptions), DQTYPE_THREAD_CURRENT, DQTAT_COM_NONE};
        winrt::check_hresult(CreateDispatcherQueueController(
            options, reinterpret_cast<PDISPATCHERQUEUECONTROLLER*>(winrt::put_abi(m_queueController))));
    }
    m_compositor = wuc::Compositor();

    D2D1_FACTORY_OPTIONS opts{};
#ifdef _DEBUG
    opts.debugLevel = D2D1_DEBUG_LEVEL_WARNING;
#endif
    winrt::check_hresult(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), &opts,
                                           m_d2dFactory.put_void()));
    winrt::check_hresult(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                             reinterpret_cast<IUnknown**>(m_dwrite.put())));
    winrt::check_hresult(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                          IID_PPV_ARGS(m_wic.put())));
    CreateDevices();
}

void CompositionHost::CreateDevices() {
    CreateRenderingDevices();
    auto interop = m_compositor.as<abi_comp::ICompositorInterop>();
    winrt::check_hresult(interop->CreateGraphicsDevice(
        m_d2dDevice.get(),
        reinterpret_cast<abi_comp::ICompositionGraphicsDevice**>(winrt::put_abi(m_graphics))));
    WatchDeviceRemoval();
}

CompositionHost::~CompositionHost() {
    StopWatchingDeviceRemoval();
}

void CompositionHost::WatchDeviceRemoval() {
    StopWatchingDeviceRemoval();
    auto device = m_d3d.try_as<ID3D11Device4>();
    if (!device) return; // a failed draw still reports the loss
    RemovalWatch watch;
    watch.device = std::move(device);
    watch.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!watch.event) return;
    if (FAILED(watch.device->RegisterDeviceRemovedEvent(watch.event, &watch.cookie))) {
        CloseHandle(watch.event);
        return;
    }
    const auto removed = [](void*, BOOLEAN) {
        fb2k::inMainThread([] {
            if (!g_host) return;
            g_host->m_deviceSuspect = true;
            g_host->QueueRecovery();
        });
    };
    if (!RegisterWaitForSingleObject(&watch.wait, watch.event, removed, nullptr, INFINITE, WT_EXECUTEONLYONCE)) {
        watch.device->UnregisterDeviceRemoved(watch.cookie);
        CloseHandle(watch.event);
        return;
    }
    m_removalWatch = std::move(watch);
}

void CompositionHost::StopWatchingDeviceRemoval() noexcept {
    // INVALID_HANDLE_VALUE: returns only after a callback that is running has finished.
    if (m_removalWatch.wait) UnregisterWaitEx(m_removalWatch.wait, INVALID_HANDLE_VALUE);
    if (m_removalWatch.device) m_removalWatch.device->UnregisterDeviceRemoved(m_removalWatch.cookie);
    if (m_removalWatch.event) CloseHandle(m_removalWatch.event);
    m_removalWatch = {};
}

void CompositionHost::CreateRenderingDevices() {
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
                                        D3D_FEATURE_LEVEL_10_0};
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels,
                                   static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION, m_d3d.put(),
                                   nullptr, nullptr);
    if (FAILED(hr)) {
        // No usable GPU (remote desktop, VMs): fall back to the WARP software rasterizer.
        winrt::check_hresult(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, levels,
                                               static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION,
                                               m_d3d.put(), nullptr, nullptr));
    }
    auto dxgi = m_d3d.as<IDXGIDevice>();
    winrt::check_hresult(m_d2dFactory->CreateDevice(dxgi.get(), m_d2dDevice.put()));
}

void CompositionHost::NoteDeviceError(HRESULT hr) {
    if (hr != DXGI_ERROR_DEVICE_REMOVED && hr != DXGI_ERROR_DEVICE_RESET && hr != DXGI_ERROR_DEVICE_HUNG &&
        hr != D2DERR_RECREATE_TARGET)
        return;
    m_deviceSuspect = true;
    QueueRecovery();
}

void CompositionHost::QueueRecovery() {
    if (m_recoveryQueued) return;
    m_recoveryQueued = true;
    fb2k::inMainThread([] {
        if (g_host) g_host->RecoverDevices();
    });
}

// A lost device (its removal event, or a draw that failed the way a lost device does): new D3D
// and D2D devices go to the compositor, which keeps its surfaces (now empty), and everyone who
// drew with the old one is told to paint again.
void CompositionHost::RecoverDevices() {
    m_recoveryQueued = false;
    if (m_drawDepth > 0) { // inside a draw: later
        QueueRecovery();
        return;
    }
    const HRESULT removed = m_d3d ? m_d3d->GetDeviceRemovedReason() : DXGI_ERROR_DEVICE_REMOVED;
    if (SUCCEEDED(removed) && !m_deviceSuspect) return;
    m_deviceSuspect = false;
    wchar_t line[96];
    swprintf_s(line, L"device: lost (hr 0x%08X), creating a new one", static_cast<unsigned>(removed));
    core::Log(line);
    try {
        CreateRenderingDevices();
        auto interop = m_graphics.as<abi_comp::ICompositionGraphicsDeviceInterop>();
        winrt::check_hresult(interop->SetRenderingDevice(m_d2dDevice.get()));
    } catch (const winrt::hresult_error& e) {
        ReportFailure("device recovery", e.code(), 0, 0);
        return;
    }
    WatchDeviceRemoval();
    m_unclean.clear(); // they belonged to the old device
    ++m_deviceGeneration;
    m_deviceReplaced.Emit();
}

wuc::CompositionDrawingSurface CompositionHost::CreateSurface(int widthPx, int heightPx) {
    using namespace winrt::Windows::Graphics::DirectX;
    return m_graphics.CreateDrawingSurface(
        winrt::Windows::Foundation::Size{static_cast<float>(std::max(widthPx, 1)),
                                         static_cast<float>(std::max(heightPx, 1))},
        DirectXPixelFormat::B8G8R8A8UIntNormalized, DirectXAlphaMode::Premultiplied);
}

void CompositionHost::ReportFailure(const char* step, HRESULT hr, int widthPx, int heightPx) {
    NoteDeviceError(hr);
    if (++m_failures > 40) return;
    console::printf("Glasswave: surface %s failed, hr 0x%08X, %dx%d px%s", step, static_cast<unsigned>(hr), widthPx,
                    heightPx, m_failures == 40 ? " (further failures not reported)" : "");
    // Also in the log file: ghosts on screen are rare and the console is gone by the time
    // someone looks into one.
    wchar_t line[160];
    swprintf_s(line, L"surface: %hs failed, hr 0x%08X, %dx%d px", step, static_cast<unsigned>(hr), widthPx, heightPx);
    core::Log(line);
}

// After a failed draw: the surface on its own cleared to transparent, so that it never shows
// what its pooled allocation held before.
bool CompositionHost::ClearAfterFailure(const wuc::CompositionDrawingSurface& surface) {
    auto interop = surface.as<abi_comp::ICompositionDrawingSurfaceInterop>();
    const SurfacePixels pixels = SurfacePixelsFor(surface);
    const RECT updateRect = pixels.UpdateRect();
    winrt::com_ptr<ID2D1DeviceContext> dc;
    POINT offset{};
    if (FAILED(interop->BeginDraw(&updateRect, __uuidof(ID2D1DeviceContext), dc.put_void(), &offset))) return false;
    dc->SetTransform(D2D1::Matrix3x2F::Identity());
    dc->SetDpi(96.0f, 96.0f);
    dc->PushAxisAlignedClip(SurfaceDrawClip(offset, pixels.width, pixels.height, 1.0f), D2D1_ANTIALIAS_MODE_ALIASED);
    dc->Clear(D2D1::ColorF(0, 0, 0, 0));
    dc->PopAxisAlignedClip();
    return SUCCEEDED(interop->EndDraw());
}

// The ghosts in the lyrics (pieces of the library's list header, a playlist card, a cover over
// the pane's header) were other surfaces' old pixels in surfaces that were shown without ever
// being drawn: BeginDraw had failed (one surface at a time per device; a draw begun while
// another is open fails) and most callers attach a fresh surface whatever Draw returns. Such a
// surface is cleared as soon as the UI thread is free.
void CompositionHost::QueueUnclean(const wuc::CompositionDrawingSurface& surface) {
    for (const auto& s : m_unclean)
        if (s == surface) return;
    m_unclean.push_back(surface);
    if (m_drainQueued) return;
    if (auto queue = winrt::Windows::System::DispatcherQueue::GetForCurrentThread()) {
        m_drainQueued = queue.TryEnqueue(winrt::Windows::System::DispatcherQueuePriority::High, [] {
            if (g_host) g_host->DrainUnclean();
        });
    }
}

void CompositionHost::Drew(const wuc::CompositionDrawingSurface& surface) {
    if (m_unclean.empty()) return;
    m_unclean.erase(std::remove(m_unclean.begin(), m_unclean.end(), surface), m_unclean.end());
}

void CompositionHost::DrainUnclean() {
    m_drainQueued = false;
    if (m_drawDepth > 0) { // still inside a draw: later
        if (!m_unclean.empty()) QueueUnclean(m_unclean.back());
        return;
    }
    auto pending = std::exchange(m_unclean, {});
    std::vector<wuc::CompositionDrawingSurface> failed;
    for (const auto& surface : pending)
        if (!ClearAfterFailure(surface)) failed.push_back(surface);
    if (failed.empty()) {
        m_uncleanRetries = 0;
        return;
    }
    // A surface that could not be cleared would keep another surface's pixels on screen for
    // good (a cover in the pane's header, moving with a lyric line): try again, for a while.
    ReportFailure("clear of an undrawn surface", E_FAIL, 0, 0);
    if (++m_uncleanRetries > 120) return;
    for (const auto& surface : failed)
        QueueUnclean(surface);
}

bool CompositionHost::OnUiThread(int widthPx, int heightPx) {
    if (GetCurrentThreadId() == m_uiThread) return true;
    ReportFailure("draw off the UI thread", E_ILLEGAL_METHOD_CALL, widthPx, heightPx);
    return false;
}

bool CompositionHost::Draw(const wuc::CompositionDrawingSurface& surface, float scale,
                           const std::function<void(ID2D1DeviceContext*)>& paint) {
    auto interop = surface.as<abi_comp::ICompositionDrawingSurfaceInterop>();
    const SurfacePixels pixels = SurfacePixelsFor(surface);
    if (!OnUiThread(pixels.width, pixels.height)) return false;
    if (m_drawDepth > 0) ReportFailure("draw nested in another draw", E_ILLEGAL_METHOD_CALL, pixels.width, pixels.height);
    const RECT updateRect = pixels.UpdateRect();
    winrt::com_ptr<ID2D1DeviceContext> dc;
    POINT offset{};
    HRESULT hr = interop->BeginDraw(&updateRect, __uuidof(ID2D1DeviceContext), dc.put_void(), &offset);
    if (FAILED(hr)) {
        // Device loss, a draw still open...: nothing can be drawn now. The surface is cleared
        // as soon as possible; the next state change repaints it.
        ReportFailure("BeginDraw", hr, pixels.width, pixels.height);
        QueueUnclean(surface);
        return false;
    }
    ++m_drawDepth;
    auto depth = detail::ScopeExit([&] { --m_drawDepth; });
    auto endSurface = detail::ScopeExit([&] { interop->EndDraw(); });
    const float dpi = 96.0f * scale;
    dc->SetDpi(dpi, dpi);
    dc->SetTransform(D2D1::Matrix3x2F::Identity());
    dc->PushAxisAlignedClip(SurfaceDrawClip(offset, pixels.width, pixels.height, scale),
                            D2D1_ANTIALIAS_MODE_ALIASED);
    auto popClip = detail::ScopeExit([&] { dc->PopAxisAlignedClip(); });
    dc->SetTransform(D2D1::Matrix3x2F::Translation(offset.x / scale, offset.y / scale));
    dc->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    dc->Clear(D2D1::ColorF(0, 0, 0, 0));
    paint(dc.get());
    dc->PopAxisAlignedClip();
    popClip.Dismiss();
    endSurface.Dismiss();
    // D2D reports here what went wrong in the paint (an unbalanced push / pop, a lost
    // target...), and then none of it - not even the clear - reached the surface.
    hr = interop->EndDraw();
    if (FAILED(hr)) {
        ReportFailure("draw", hr, pixels.width, pixels.height);
        if (!ClearAfterFailure(surface)) QueueUnclean(surface);
        return false;
    }
    Drew(surface);
    return true;
}

winrt::com_ptr<ID2D1DeviceContext> CompositionHost::CreateDeviceContext() {
    winrt::com_ptr<ID2D1DeviceContext> dc;
    winrt::check_hresult(m_d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, dc.put()));
    return dc;
}

bool CompositionHost::Draw(const wuc::CompositionDrawingSurface& surface, float scale, ID2D1DeviceContext* dc,
                           const std::function<void(ID2D1DeviceContext*)>& paint) {
    auto interop = surface.as<abi_comp::ICompositionDrawingSurfaceInterop>();
    const SurfacePixels pixels = SurfacePixelsFor(surface);
    if (!OnUiThread(pixels.width, pixels.height)) return false;
    if (m_drawDepth > 0) ReportFailure("draw nested in another draw", E_ILLEGAL_METHOD_CALL, pixels.width, pixels.height);
    const RECT updateRect = pixels.UpdateRect();
    winrt::com_ptr<IDXGISurface> dxgi;
    POINT offset{};
    HRESULT hr = interop->BeginDraw(&updateRect, __uuidof(IDXGISurface), dxgi.put_void(), &offset);
    if (FAILED(hr)) {
        ReportFailure("BeginDraw (own context)", hr, pixels.width, pixels.height);
        QueueUnclean(surface);
        return false;
    }
    ++m_drawDepth;
    auto depth = detail::ScopeExit([&] { --m_drawDepth; });
    bool cleared = false; // the surface was ended and cleared after a failure
    auto endSurface = detail::ScopeExit([&] {
        if (!cleared) interop->EndDraw();
    });
    const float dpi = 96.0f * scale;
    const D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), dpi, dpi);
    winrt::com_ptr<ID2D1Bitmap1> target;
    hr = dc->CreateBitmapFromDxgiSurface(dxgi.get(), &props, target.put());
    if (FAILED(hr)) {
        ReportFailure("target (own context)", hr, pixels.width, pixels.height);
        interop->EndDraw();
        cleared = true;
        if (!ClearAfterFailure(surface)) QueueUnclean(surface);
        return false;
    }
    dc->SetTarget(target.get());
    auto unbindTarget = detail::ScopeExit([&] { dc->SetTarget(nullptr); });
    dc->SetDpi(dpi, dpi);
    dc->BeginDraw();
    auto endDraw = detail::ScopeExit([&] { dc->EndDraw(); });
    dc->SetTransform(D2D1::Matrix3x2F::Identity());
    dc->PushAxisAlignedClip(SurfaceDrawClip(offset, pixels.width, pixels.height, scale),
                            D2D1_ANTIALIAS_MODE_ALIASED);
    auto popClip = detail::ScopeExit([&] { dc->PopAxisAlignedClip(); });
    dc->SetTransform(D2D1::Matrix3x2F::Translation(offset.x / scale, offset.y / scale));
    dc->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    dc->Clear(D2D1::ColorF(0, 0, 0, 0));
    paint(dc);
    dc->PopAxisAlignedClip();
    popClip.Dismiss();
    hr = dc->EndDraw(); // what went wrong in the paint; then none of it reached the surface
    endDraw.Dismiss();
    dc->SetTarget(nullptr);
    unbindTarget.Dismiss();
    endSurface.Dismiss();
    const HRESULT ended = interop->EndDraw();
    if (FAILED(hr) || FAILED(ended)) {
        ReportFailure("draw (own context)", FAILED(hr) ? hr : ended, pixels.width, pixels.height);
        if (!ClearAfterFailure(surface)) QueueUnclean(surface);
        return false;
    }
    Drew(surface);
    return true;
}

wuc::SpriteVisual CompositionHost::CreatePaintedVisual(int widthPx, int heightPx, float scale,
                                                       const std::function<void(ID2D1DeviceContext*)>& paint) {
    auto visual = m_compositor.CreateSpriteVisual();
    Repaint(visual, widthPx, heightPx, scale, paint);
    return visual;
}

wuc::CompositionNineGridBrush CompositionHost::CreateNineGrid(int widthPx, int heightPx, float scale, float insetPx,
                                                             bool hollow,
                                                             const std::function<void(ID2D1DeviceContext*)>& paint) {
    auto surface = CreateSurface(widthPx, heightPx);
    Draw(surface, scale, paint);
    auto source = m_compositor.CreateSurfaceBrush(surface);
    source.Stretch(wuc::CompositionStretch::Fill);
    auto brush = m_compositor.CreateNineGridBrush();
    brush.Source(source);
    brush.SetInsets(insetPx);
    brush.IsCenterHollow(hollow);
    return brush;
}

void CompositionHost::Repaint(const wuc::SpriteVisual& visual, int widthPx, int heightPx, float scale,
                              const std::function<void(ID2D1DeviceContext*)>& paint) {
    auto surface = CreateSurface(widthPx, heightPx);
    if (!Draw(surface, scale, paint)) {
        visual.Brush(nullptr); // better nothing than a fresh surface's leftovers
        return;
    }
    auto brush = m_compositor.CreateSurfaceBrush(surface);
    brush.Stretch(wuc::CompositionStretch::None);
    brush.HorizontalAlignmentRatio(0.0f);
    brush.VerticalAlignmentRatio(0.0f);
    visual.Brush(brush);
    visual.Size(float2{static_cast<float>(widthPx), static_cast<float>(heightPx)});
}

void CompositionHost::Repaint(const wuc::SpriteVisual& visual, RetainedSurface& retained, int widthPx,
                              int heightPx, float scale,
                              const std::function<void(ID2D1DeviceContext*)>& paint) {
    widthPx = std::max(widthPx, 1);
    heightPx = std::max(heightPx, 1);
    try {
        if (retained.m_generation != m_deviceGeneration) { // made under a device that is gone
            retained.Clear();
            retained.m_generation = m_deviceGeneration;
        }
        const auto update = retained.Prepare(widthPx, heightPx);
        switch (update) {
        case SurfaceReusePolicy::Update::Create:
            retained.m_surface = CreateSurface(widthPx, heightPx);
            break;
        case SurfaceReusePolicy::Update::Resize: {
            const winrt::Windows::Graphics::SizeInt32 size{static_cast<int32_t>(widthPx),
                                                           static_cast<int32_t>(heightPx)};
            retained.m_surface.Resize(size);
            break;
        }
        case SurfaceReusePolicy::Update::Reuse:
            break;
        }
        if (!retained.m_brush) {
            retained.m_brush = m_compositor.CreateSurfaceBrush(retained.m_surface);
            retained.m_brush.Stretch(wuc::CompositionStretch::None);
            retained.m_brush.HorizontalAlignmentRatio(0.0f);
            retained.m_brush.VerticalAlignmentRatio(0.0f);
        }
        if (!Draw(retained.m_surface, scale, paint) && update != SurfaceReusePolicy::Update::Reuse) {
            // A new or resized surface that could not be drawn holds another visual's old
            // pixels: drop it, and start afresh on the next paint.
            visual.Brush(nullptr);
            retained.Clear();
            return;
        }
    } catch (...) {
        // Draw() already balances its begin/end pairs. Drop both owners so a failed retained
        // slot cannot mask a later draw behind stale backing.
        visual.Brush(nullptr);
        retained.Clear();
        throw;
    }
    visual.Brush(retained.m_brush);
    visual.Size(float2{static_cast<float>(widthPx), static_cast<float>(heightPx)});
}

wuc::CubicBezierEasingFunction CompositionHost::Easing(const Curve& c) {
    if (const auto found = m_easings.find(c); found != m_easings.end()) return found->second;
    auto easing = m_compositor.CreateCubicBezierEasingFunction(float2{c.x1, c.y1}, float2{c.x2, c.y2});
    m_easings.emplace(c, easing);
    return easing;
}

void CompositionHost::AnimateOpacity(const wuc::Visual& v, float to, int durationMs) {
    FadeTo(v, to, std::max(durationMs, 1), curve::kFastOut);
}

void CompositionHost::AnimateScale(const wuc::Visual& v, float to, int durationMs) {
    auto anim = m_compositor.CreateVector3KeyFrameAnimation();
    anim.InsertKeyFrame(1.0f, float3{to, to, 1.0f}, Easing(curve::kFastOut));
    anim.Duration(Ms(std::max(durationMs, 1)));
    v.StartAnimation(L"Scale", anim);
}

void CompositionHost::StartPulse(const wuc::Visual& v, float low, float high, int periodMs) {
    auto anim = m_compositor.CreateScalarKeyFrameAnimation();
    anim.InsertKeyFrame(0.0f, low);
    anim.InsertKeyFrame(0.5f, high, Easing(curve::kInOut));
    anim.InsertKeyFrame(1.0f, low, Easing(curve::kInOut));
    anim.Duration(Ms(periodMs));
    anim.IterationBehavior(wuc::AnimationIterationBehavior::Forever);
    v.StartAnimation(L"Opacity", anim);
}

void CompositionHost::StopPulse(const wuc::Visual& v, float settleTo, int durationMs) {
    v.StopAnimation(L"Opacity");
    AnimateOpacity(v, settleTo, durationMs);
}

}  // namespace glasswave::gfx
