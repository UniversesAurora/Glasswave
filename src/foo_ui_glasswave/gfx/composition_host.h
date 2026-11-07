#pragma once
// Process-wide composition / drawing infrastructure:
//  - DispatcherQueue and Windows.UI.Composition Compositor on the current (foobar2000 main) thread;
//  - D3D11 + Direct2D devices and the CompositionGraphicsDevice bound to the compositor;
//  - DirectWrite / WIC factories;
//  - helpers for drawing surfaces, painting and animations.
// Main thread only.

namespace glasswave::gfx {

namespace wuc = winrt::Windows::UI::Composition;

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

    // Convenience: a SpriteVisual whose brush is a freshly painted surface of the same size.
    wuc::SpriteVisual CreatePaintedVisual(int widthPx, int heightPx, float scale,
                                          const std::function<void(ID2D1DeviceContext*)>& paint);
    void Repaint(const wuc::SpriteVisual& visual, int widthPx, int heightPx, float scale,
                 const std::function<void(ID2D1DeviceContext*)>& paint);

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
