#pragma once

// Windows 7 style drop shadow drawn by four click-through layered windows
// placed around the main window (the same technique Office / Visual Studio
// use for their glow borders). The DWM shadow of Windows 11 is softer, larger
// and symmetric; Win7's is tighter and heavier toward the bottom-right.
// Per-side alpha profiles are measured from a native Win7 screenshot (see
// docs/design/visual-spec.md, "Window shadow").

namespace glasswave::aero {

class ShadowFrame {
public:
    ShadowFrame() = default;
    ~ShadowFrame();
    ShadowFrame(const ShadowFrame&) = delete;
    ShadowFrame& operator=(const ShadowFrame&) = delete;

    void Create(HWND owner);
    void Destroy();

    // Call after the owner moved, resized, changed visibility or activation.
    void Sync();
    void SetActive(bool active);

private:
    struct Side {
        HWND hwnd = nullptr;
        HDC dc = nullptr;
        HBITMAP bitmap = nullptr;
        HGDIOBJ oldBitmap = nullptr;
        SIZE size{};
    };

    void Render(const RECT& owner, float scale);
    void ReleaseSide(Side& s);

    HWND m_owner = nullptr;
    Side m_sides[4];  // top, bottom, left, right
    RECT m_renderedRect{};
    float m_renderedScale = 0;
    bool m_active = true, m_renderedActive = true, m_visible = false;
};

}  // namespace glasswave::aero
