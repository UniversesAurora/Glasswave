#include "pch.h"
#include "aero/shadow_frame.h"

namespace glasswave::aero {

namespace {

constexpr wchar_t kShadowClass[] = L"Glasswave.Shadow";

// Alpha (0..255) of the shadow, index 0 = the pixel adjacent to the window
// edge, at 96 DPI. Measured from a native Win7 window over a light background.
constexpr uint8_t kLeft[] = {126, 98, 75, 58, 48, 39, 31, 23, 17, 12, 8, 4, 2};
constexpr uint8_t kTop[] = {115, 86, 63, 47, 37, 29, 23, 16, 12, 7, 4, 2};
constexpr uint8_t kRight[] = {158, 137, 121, 106, 96, 84, 72, 60, 49, 40, 31, 24, 17, 12, 8, 4, 2};
constexpr uint8_t kBottom[] = {162, 144, 129, 116, 106, 95, 84, 72, 60, 49, 39, 31, 24, 17, 12, 7, 4, 2};

constexpr float kCoreRadius = 10.0f;   // DIP; rounding of the shadow's corners
constexpr float kWindowRadius = 6.0f;  // DIP; must match the frame's corner radius

template <size_t N>
float Profile(const uint8_t (&t)[N], float d) {
    if (d <= 0) return t[0];
    const float i = std::floor(d);
    const size_t k = static_cast<size_t>(i);
    if (k + 1 >= N) return k < N ? t[k] * (1.0f - (d - i)) : 0.0f;
    return t[k] + (t[k + 1] - t[k]) * (d - i);
}

void RegisterShadowClass() {
    static bool registered = false;
    if (registered) return;
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = core_api::get_my_instance();
    wc.lpszClassName = kShadowClass;
    RegisterClassExW(&wc);
    registered = true;
}

// Shadow alpha for the pixel at (x, y), relative to the owner's top-left
// corner, in device pixels. W/H: owner size; s: DPI scale.
float ShadowAlpha(int x, int y, int W, int H, float s) {
    const float cx = x + 0.5f, cy = y + 0.5f;
    const float rc = kCoreRadius * s;

    // Corner quadrants: distance to the rounded core, blending the two side profiles.
    const bool left = cx < rc, right = cx > W - rc, top = cy < rc, bottom = cy > H - rc;
    if ((left || right) && (top || bottom)) {
        const float ox = left ? rc : W - rc, oy = top ? rc : H - rc;
        const float vx = cx - ox, vy = cy - oy;
        const float len = std::max(std::hypot(vx, vy), 1e-3f);
        const float d = (len - rc - 0.5f) / s;
        const float wx = vx * vx / (len * len), wy = 1.0f - wx;
        const float a = wx * (left ? Profile(kLeft, d) : Profile(kRight, d)) +
                        wy * (top ? Profile(kTop, d) : Profile(kBottom, d));
        // Inside the owner's rect only the pixels outside its rounded corner show.
        if (cx >= 0 && cx <= W && cy >= 0 && cy <= H) {
            const float rw = kWindowRadius * s;
            const float wox = left ? rw : W - rw, woy = top ? rw : H - rw;
            const bool inCornerBox = (left ? cx < rw : cx > W - rw) && (top ? cy < rw : cy > H - rw);
            if (!inCornerBox) return 0.0f;
            const float outside = std::clamp(std::hypot(cx - wox, cy - woy) - rw + 0.5f, 0.0f, 1.0f);
            return a * outside;
        }
        return a;
    }
    if (cx < 0) return Profile(kLeft, (-cx - 0.5f) / s);
    if (cx > W) return Profile(kRight, (cx - W - 0.5f) / s);
    if (cy < 0) return Profile(kTop, (-cy - 0.5f) / s);
    if (cy > H) return Profile(kBottom, (cy - H - 0.5f) / s);
    return 0.0f;  // under the owner
}

}  // namespace

ShadowFrame::~ShadowFrame() { Destroy(); }

void ShadowFrame::Create(HWND owner) {
    m_owner = owner;
    RegisterShadowClass();
    for (auto& side : m_sides) {
        side.hwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
                                    kShadowClass, L"", WS_POPUP, 0, 0, 1, 1, owner, nullptr,
                                    core_api::get_my_instance(), nullptr);
    }
}

void ShadowFrame::ReleaseSide(Side& s) {
    if (s.dc) {
        SelectObject(s.dc, s.oldBitmap);
        DeleteDC(s.dc);
        s.dc = nullptr;
    }
    if (s.bitmap) {
        DeleteObject(s.bitmap);
        s.bitmap = nullptr;
    }
}

void ShadowFrame::Destroy() {
    for (auto& side : m_sides) {
        ReleaseSide(side);
        if (side.hwnd) {
            DestroyWindow(side.hwnd);
            side.hwnd = nullptr;
        }
    }
    m_owner = nullptr;
}

void ShadowFrame::SetActive(bool active) {
    if (m_active == active) return;
    m_active = active;
    Sync();
}

void ShadowFrame::Sync() {
    if (!m_owner) return;
    const bool show = IsWindowVisible(m_owner) && !IsIconic(m_owner) && !IsZoomed(m_owner);
    if (!show) {
        if (m_visible)
            for (auto& side : m_sides) ShowWindow(side.hwnd, SW_HIDE);
        m_visible = false;
        return;
    }

    RECT rc{};
    GetWindowRect(m_owner, &rc);
    const float scale = GetDpiForWindow(m_owner) / 96.0f;
    const bool sameSize = (rc.right - rc.left) == (m_renderedRect.right - m_renderedRect.left) &&
                          (rc.bottom - rc.top) == (m_renderedRect.bottom - m_renderedRect.top);
    if (!sameSize || scale != m_renderedScale || m_active != m_renderedActive) {
        Render(rc, scale);
    } else if (rc.left != m_renderedRect.left || rc.top != m_renderedRect.top) {
        // Pure move: shift the strips, no re-render.
        const int dx = rc.left - m_renderedRect.left, dy = rc.top - m_renderedRect.top;
        HDWP batch = BeginDeferWindowPos(4);
        for (auto& side : m_sides) {
            RECT w{};
            GetWindowRect(side.hwnd, &w);
            batch = DeferWindowPos(batch, side.hwnd, nullptr, w.left + dx, w.top + dy, 0, 0,
                                   SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
        EndDeferWindowPos(batch);
        m_renderedRect = rc;
    }
    if (!m_visible) {
        for (auto& side : m_sides) ShowWindow(side.hwnd, SW_SHOWNOACTIVATE);
        m_visible = true;
    }
}

void ShadowFrame::Render(const RECT& owner, float s) {
    const int W = owner.right - owner.left, H = owner.bottom - owner.top;
    const int eL = static_cast<int>(std::ceil(std::size(kLeft) * s));
    const int eT = static_cast<int>(std::ceil(std::size(kTop) * s));
    const int eR = static_cast<int>(std::ceil(std::size(kRight) * s));
    const int eB = static_cast<int>(std::ceil(std::size(kBottom) * s));
    const int cz = static_cast<int>(std::ceil(kCoreRadius * s));
    const float strength = m_active ? 1.0f : 0.8f;

    // Strip rectangles relative to the owner's top-left corner.
    const RECT strips[4] = {
        {-eL, -eT, W + eR, cz},          // top (includes the top corners of the owner)
        {-eL, H - cz, W + eR, H + eB},   // bottom
        {-eL, cz, 0, H - cz},            // left
        {W, cz, W + eR, H - cz},         // right
    };

    HDC screen = GetDC(nullptr);
    for (int i = 0; i < 4; ++i) {
        Side& side = m_sides[i];
        const RECT& r = strips[i];
        const int w = std::max<int>(r.right - r.left, 1), h = std::max<int>(r.bottom - r.top, 1);
        ReleaseSide(side);

        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -h;  // top-down
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        side.bitmap = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!side.bitmap || !bits) continue;
        side.dc = CreateCompatibleDC(screen);
        side.oldBitmap = SelectObject(side.dc, side.bitmap);
        side.size = {w, h};

        auto* px = static_cast<uint32_t*>(bits);
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                const float a = ShadowAlpha(r.left + x, r.top + y, W, H, s) * strength;
                const uint32_t A = static_cast<uint32_t>(std::clamp(a, 0.0f, 255.0f) + 0.5f);
                px[y * w + x] = A << 24;  // premultiplied black
            }
        }

        POINT dst{owner.left + r.left, owner.top + r.top};
        POINT src{0, 0};
        BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
        UpdateLayeredWindow(side.hwnd, screen, &dst, &side.size, side.dc, &src, 0, &blend, ULW_ALPHA);
    }
    ReleaseDC(nullptr, screen);

    m_renderedRect = owner;
    m_renderedScale = s;
    m_renderedActive = m_active;
}

}  // namespace glasswave::aero
