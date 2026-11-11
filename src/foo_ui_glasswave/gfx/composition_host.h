#pragma once
// Process-wide composition / drawing infrastructure:
//  - DispatcherQueue and Windows.UI.Composition Compositor on the current (foobar2000 main) thread;
//  - D3D11 + Direct2D devices and the CompositionGraphicsDevice bound to the compositor;
//  - DirectWrite / WIC factories;
//  - helpers for drawing surfaces, painting and animations.
// Main thread only.

#include <algorithm>
#include <functional>
#include <utility>

namespace glasswave::gfx {

namespace wuc = winrt::Windows::UI::Composition;

namespace detail {

// Runs a cleanup action exactly once. Destructors unwind in reverse declaration order.
template <typename F>
class ScopeExit {
public:
    explicit ScopeExit(F callback) : m_callback(std::move(callback)) {}

    ScopeExit(const ScopeExit&) = delete;
    ScopeExit& operator=(const ScopeExit&) = delete;

    ~ScopeExit() noexcept {
        if (m_active) m_callback();
    }

    void Dismiss() noexcept { m_active = false; }

private:
    F m_callback;
    bool m_active = true;
};

template <typename F>
ScopeExit(F) -> ScopeExit<F>;

}  // namespace detail

// BeginDraw can return a sub-rectangle of a larger, pooled drawing allocation. This is the
// clip in DIPs, with an identity transform, that covers updateOffset + the logical surface.
inline D2D1_RECT_F SurfaceDrawClip(POINT offset, int widthPx, int heightPx, float scale) noexcept {
    const float safeScale = scale > 0.0f ? scale : 1.0f;
    const float left = static_cast<float>(offset.x) / safeScale;
    const float top = static_cast<float>(offset.y) / safeScale;
    return D2D1::RectF(left, top, left + std::max(widthPx, 1) / safeScale,
                       top + std::max(heightPx, 1) / safeScale);
}

// Allocation decision shared by retained surfaces and their standalone regression test.
class SurfaceReusePolicy {
public:
    enum class Update {
        Create,
        Resize,
        Reuse,
    };

    Update Plan(bool hasSurface, int widthPx, int heightPx) noexcept {
        widthPx = std::max(widthPx, 1);
        heightPx = std::max(heightPx, 1);
        if (!hasSurface) {
            m_widthPx = widthPx;
            m_heightPx = heightPx;
            return Update::Create;
        }
        if (m_widthPx != widthPx || m_heightPx != heightPx) {
            m_widthPx = widthPx;
            m_heightPx = heightPx;
            return Update::Resize;
        }
        return Update::Reuse;
    }

    void Clear() noexcept {
        m_widthPx = 0;
        m_heightPx = 0;
    }

    int WidthPx() const noexcept { return m_widthPx; }
    int HeightPx() const noexcept { return m_heightPx; }

private:
    int m_widthPx = 0;
    int m_heightPx = 0;
};

// Caller-owned backing for a visual whose content changes repeatedly. The cache keeps its
// drawing surface and brush until the caller clears it or the visual is removed.
class RetainedSurface {
public:
    SurfaceReusePolicy::Update Prepare(int widthPx, int heightPx) noexcept;
    void Clear() noexcept;
    bool Empty() const noexcept { return !m_surface; }
    int WidthPx() const noexcept { return m_policy.WidthPx(); }
    int HeightPx() const noexcept { return m_policy.HeightPx(); }

private:
    friend class CompositionHost;

    wuc::CompositionDrawingSurface m_surface{nullptr};
    wuc::CompositionSurfaceBrush m_brush{nullptr};
    SurfaceReusePolicy m_policy;
};

// Coalesces repaint requests while presentation is unavailable.
class PaintCoalescer {
public:
    void Request() noexcept { m_pending = true; }
    bool Pending() const noexcept { return m_pending; }
    void Clear() noexcept { m_pending = false; }
    bool Take(bool canPaint) noexcept {
        if (!canPaint || !m_pending) return false;
        m_pending = false;
        return true;
    }

private:
    bool m_pending = false;
};

class CompositionHost {
public:
    static CompositionHost& Get();
    static void Shutdown();

    const wuc::Compositor& Compositor() const { return m_compositor; }
    ID2D1Factory1* D2DFactory() const { return m_d2dFactory.get(); }
    IDWriteFactory* DWrite() const { return m_dwrite.get(); }
    IWICImagingFactory* Wic() const { return m_wic.get(); }

    // Create a premultiplied BGRA drawing surface, size in device pixels.
    wuc::CompositionDrawingSurface CreateSurface(int widthPx, int heightPx);

    // Paint a surface with D2D. The callback works in DIP (DPI is set from `scale`);
    // the surface is cleared to transparent first.
    void Draw(const wuc::CompositionDrawingSurface& surface, float scale,
              const std::function<void(ID2D1DeviceContext*)>& paint);

    // A device context of our own on the composition D2D device. Effects and command lists
    // created with it stay usable across frames, which per-frame animated views need.
    winrt::com_ptr<ID2D1DeviceContext> CreateDeviceContext();
    // Like Draw, painting through `dc` (from CreateDeviceContext).
    void Draw(const wuc::CompositionDrawingSurface& surface, float scale, ID2D1DeviceContext* dc,
              const std::function<void(ID2D1DeviceContext*)>& paint);

    // Convenience: a SpriteVisual whose brush is a freshly painted surface of the same size.
    wuc::SpriteVisual CreatePaintedVisual(int widthPx, int heightPx, float scale,
                                          const std::function<void(ID2D1DeviceContext*)>& paint);
    void Repaint(const wuc::SpriteVisual& visual, int widthPx, int heightPx, float scale,
                 const std::function<void(ID2D1DeviceContext*)>& paint);
    // Repaint through caller-owned backing. Content-only updates redraw the existing surface;
    // a size change resizes or recreates it without changing the visual's identity.
    void Repaint(const wuc::SpriteVisual& visual, RetainedSurface& retained, int widthPx, int heightPx,
                 float scale, const std::function<void(ID2D1DeviceContext*)>& paint);

    // Animations
    void AnimateOpacity(const wuc::Visual& v, float to, int durationMs);
    void AnimateScale(const wuc::Visual& v, float to, int durationMs);
    void StartPulse(const wuc::Visual& v, float low, float high, int periodMs);
    void StopPulse(const wuc::Visual& v, float settleTo, int durationMs);

private:
    CompositionHost();
    void CreateDevices();

    winrt::Windows::System::DispatcherQueueController m_queueController{nullptr};
    wuc::Compositor m_compositor{nullptr};
    wuc::CompositionGraphicsDevice m_graphics{nullptr};
    wuc::CompositionEasingFunction m_easeOut{nullptr};
    wuc::CompositionEasingFunction m_easeInOut{nullptr};

    winrt::com_ptr<ID3D11Device> m_d3d;
    winrt::com_ptr<ID2D1Factory1> m_d2dFactory;
    winrt::com_ptr<ID2D1Device> m_d2dDevice;
    winrt::com_ptr<IDWriteFactory> m_dwrite;
    winrt::com_ptr<IWICImagingFactory> m_wic;
};

}  // namespace glasswave::gfx
