#include "pch.h"
#include "gfx/composition_host.h"

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
    m_easeOut = m_compositor.CreateCubicBezierEasingFunction(float2{0.1f, 0.9f}, float2{0.2f, 1.0f});
    m_easeInOut = m_compositor.CreateCubicBezierEasingFunction(float2{0.45f, 0.0f}, float2{0.55f, 1.0f});

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

    auto interop = m_compositor.as<abi_comp::ICompositorInterop>();
    winrt::check_hresult(interop->CreateGraphicsDevice(
        m_d2dDevice.get(), reinterpret_cast<abi_comp::ICompositionGraphicsDevice**>(winrt::put_abi(m_graphics))));
}

wuc::CompositionDrawingSurface CompositionHost::CreateSurface(int widthPx, int heightPx) {
    using namespace winrt::Windows::Graphics::DirectX;
    return m_graphics.CreateDrawingSurface(
        winrt::Windows::Foundation::Size{static_cast<float>(std::max(widthPx, 1)),
                                         static_cast<float>(std::max(heightPx, 1))},
        DirectXPixelFormat::B8G8R8A8UIntNormalized, DirectXAlphaMode::Premultiplied);
}

void CompositionHost::Draw(const wuc::CompositionDrawingSurface& surface, float scale,
                           const std::function<void(ID2D1DeviceContext*)>& paint) {
    auto interop = surface.as<abi_comp::ICompositionDrawingSurfaceInterop>();
    const SurfacePixels pixels = SurfacePixelsFor(surface);
    const RECT updateRect = pixels.UpdateRect();
    winrt::com_ptr<ID2D1DeviceContext> dc;
    POINT offset{};
    HRESULT hr = interop->BeginDraw(&updateRect, __uuidof(ID2D1DeviceContext), dc.put_void(), &offset);
    if (FAILED(hr)) {
        // Skip this paint on device loss and similar failures; the next state change repaints.
        // TODO(M1): handle RenderingDeviceReplaced and rebuild every surface.
        return;
    }
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
    interop->EndDraw();
    endSurface.Dismiss();
}

winrt::com_ptr<ID2D1DeviceContext> CompositionHost::CreateDeviceContext() {
    winrt::com_ptr<ID2D1DeviceContext> dc;
    winrt::check_hresult(m_d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, dc.put()));
    return dc;
}

void CompositionHost::Draw(const wuc::CompositionDrawingSurface& surface, float scale, ID2D1DeviceContext* dc,
                           const std::function<void(ID2D1DeviceContext*)>& paint) {
    auto interop = surface.as<abi_comp::ICompositionDrawingSurfaceInterop>();
    const SurfacePixels pixels = SurfacePixelsFor(surface);
    const RECT updateRect = pixels.UpdateRect();
    winrt::com_ptr<IDXGISurface> dxgi;
    POINT offset{};
    if (FAILED(interop->BeginDraw(&updateRect, __uuidof(IDXGISurface), dxgi.put_void(), &offset))) return;
    auto endSurface = detail::ScopeExit([&] { interop->EndDraw(); });
    const float dpi = 96.0f * scale;
    const D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), dpi, dpi);
    winrt::com_ptr<ID2D1Bitmap1> target;
    if (FAILED(dc->CreateBitmapFromDxgiSurface(dxgi.get(), &props, target.put()))) return;
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
    dc->EndDraw();
    endDraw.Dismiss();
    dc->SetTarget(nullptr);
    unbindTarget.Dismiss();
    interop->EndDraw();
    endSurface.Dismiss();
}

wuc::SpriteVisual CompositionHost::CreatePaintedVisual(int widthPx, int heightPx, float scale,
                                                       const std::function<void(ID2D1DeviceContext*)>& paint) {
    auto visual = m_compositor.CreateSpriteVisual();
    Repaint(visual, widthPx, heightPx, scale, paint);
    return visual;
}

void CompositionHost::Repaint(const wuc::SpriteVisual& visual, int widthPx, int heightPx, float scale,
                              const std::function<void(ID2D1DeviceContext*)>& paint) {
    auto surface = CreateSurface(widthPx, heightPx);
    Draw(surface, scale, paint);
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
        switch (retained.Prepare(widthPx, heightPx)) {
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
        Draw(retained.m_surface, scale, paint);
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

void CompositionHost::AnimateOpacity(const wuc::Visual& v, float to, int durationMs) {
    auto anim = m_compositor.CreateScalarKeyFrameAnimation();
    anim.InsertKeyFrame(1.0f, to, m_easeOut);
    anim.Duration(Ms(std::max(durationMs, 1)));
    v.StartAnimation(L"Opacity", anim);
}

void CompositionHost::AnimateScale(const wuc::Visual& v, float to, int durationMs) {
    auto anim = m_compositor.CreateVector3KeyFrameAnimation();
    anim.InsertKeyFrame(1.0f, float3{to, to, 1.0f}, m_easeOut);
    anim.Duration(Ms(std::max(durationMs, 1)));
    v.StartAnimation(L"Scale", anim);
}

void CompositionHost::StartPulse(const wuc::Visual& v, float low, float high, int periodMs) {
    auto anim = m_compositor.CreateScalarKeyFrameAnimation();
    anim.InsertKeyFrame(0.0f, low);
    anim.InsertKeyFrame(0.5f, high, m_easeInOut);
    anim.InsertKeyFrame(1.0f, low, m_easeInOut);
    anim.Duration(Ms(periodMs));
    anim.IterationBehavior(wuc::AnimationIterationBehavior::Forever);
    v.StartAnimation(L"Opacity", anim);
}

void CompositionHost::StopPulse(const wuc::Visual& v, float settleTo, int durationMs) {
    v.StopAnimation(L"Opacity");
    AnimateOpacity(v, settleTo, durationMs);
}

}  // namespace glasswave::gfx
