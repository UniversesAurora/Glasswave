#include "pch.h"
#include "ui/search_popup.h"

#include "gfx/composition_host.h"
#include "win7/win7_resources.h"

#include <gdiplus.h>
#include <windowsx.h>

namespace glasswave::ui {

namespace {

constexpr wchar_t kClass[] = L"GlasswaveSearchPopup";

// Measured on the owner's Windows 7 Explorer captures (docs/design/visual-spec.md).
constexpr COLORREF kBorder = RGB(0x64, 0x64, 0x64);
constexpr COLORREF kBack = RGB(0xFF, 0xFF, 0xFF);
constexpr COLORREF kFooterBack = RGB(0xFC, 0xFC, 0xFC);
constexpr COLORREF kFooterRule = RGB(0xDF, 0xDF, 0xDF);
constexpr COLORREF kFooterShade = RGB(0xED, 0xED, 0xED); // the rule's fade into the footer
constexpr COLORREF kText = RGB(0x00, 0x00, 0x00);
constexpr COLORREF kGrey = RGB(0x6D, 0x6D, 0x6D);
constexpr COLORREF kLink = RGB(0x00, 0x70, 0xC0);
// Layout, DIP.
constexpr float kRowHeight = 21, kTextLeft = 8, kTextRight = 8, kPadY = 2;
constexpr float kFooterShadeHeight = 5, kFooterPadX = 8, kFooterPadY = 4, kFooterLabel = 18, kFooterLine = 20;
constexpr float kLinkGap = 4, kLinkPadX = 3; // scope buttons: Explorer's filter buttons, a little roomier
constexpr UINT_PTR kFadeTimer = 1;

ULONG_PTR Gdiplus() {
    static const ULONG_PTR token = [] {
        Gdiplus::GdiplusStartupInput input;
        ULONG_PTR out = 0;
        Gdiplus::GdiplusStartup(&out, &input, nullptr);
        return out;
    }();
    return token;
}

void RoundedPath(Gdiplus::GraphicsPath& path, const Gdiplus::RectF& box, Gdiplus::REAL radius) {
    const Gdiplus::REAL d = radius * 2;
    path.AddArc(box.X, box.Y, d, d, 180, 90);
    path.AddArc(box.GetRight() - d, box.Y, d, d, 270, 90);
    path.AddArc(box.GetRight() - d, box.GetBottom() - d, d, d, 0, 90);
    path.AddArc(box.X, box.GetBottom() - d, d, d, 90, 90);
    path.CloseFigure();
}

// Explorer's search filter button under the mouse (owner's capture at 150 %, 28 px high): a
// #B0B0B0 frame with 2 px corners, a white line inside it, and a glossy grey: #FBFBFB down to
// #F2F2F2 at the middle, a step to #E4E4E4, then up to #FBFBFB at the foot. `alpha` fades it.
void FilterPlate(HDC dc, const RECT& r, float scale, float alpha) {
    if (!Gdiplus() || alpha <= 0) return;
    const BYTE a = static_cast<BYTE>(std::lround(255 * std::clamp(alpha, 0.0f, 1.0f)));
    Gdiplus::Graphics g(dc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    const Gdiplus::RectF box(r.left + 0.5f, r.top + 0.5f, static_cast<Gdiplus::REAL>(r.right - r.left - 1),
                             static_cast<Gdiplus::REAL>(r.bottom - r.top - 1));
    Gdiplus::GraphicsPath path;
    RoundedPath(path, box, 1.6f * scale);
    Gdiplus::LinearGradientBrush fill(Gdiplus::PointF(0, box.Y - 0.5f), Gdiplus::PointF(0, box.GetBottom() + 0.5f),
                                      Gdiplus::Color(a, 0xFB, 0xFB, 0xFB), Gdiplus::Color(a, 0xFB, 0xFB, 0xFB));
    const Gdiplus::Color colors[] = {Gdiplus::Color(a, 0xFB, 0xFB, 0xFB), Gdiplus::Color(a, 0xF2, 0xF2, 0xF2),
                                     Gdiplus::Color(a, 0xE4, 0xE4, 0xE4), Gdiplus::Color(a, 0xEC, 0xEC, 0xEC),
                                     Gdiplus::Color(a, 0xFB, 0xFB, 0xFB)};
    const Gdiplus::REAL positions[] = {0.0f, 0.43f, 0.44f, 0.65f, 1.0f};
    fill.SetInterpolationColors(colors, positions, 5);
    g.FillPath(&fill, &path);
    // The white line inside the frame (brightest over the upper half).
    const Gdiplus::RectF inner(box.X + 1, box.Y + 1, box.Width - 2, box.Height - 2);
    Gdiplus::GraphicsPath innerPath;
    RoundedPath(innerPath, inner, 1.0f * scale);
    Gdiplus::LinearGradientBrush glint(Gdiplus::PointF(0, inner.Y), Gdiplus::PointF(0, inner.GetBottom()),
                                       Gdiplus::Color(static_cast<BYTE>(a * 0.95f), 0xFF, 0xFF, 0xFF),
                                       Gdiplus::Color(static_cast<BYTE>(a * 0.35f), 0xFF, 0xFF, 0xFF));
    Gdiplus::Pen highlight(&glint, 1.0f);
    g.DrawPath(&highlight, &innerPath);
    Gdiplus::Pen frame(Gdiplus::Color(a, 0xB0, 0xB0, 0xB0), 1.0f);
    g.DrawPath(&frame, &path);
}

// aero.msstyles' tooltip close button (IMAGE 878 / 879 / 880 for 96 / 120 / 144 DPI, three states
// stacked: a grey cross, a red cross on a plate, pressed), as Windows 7 deletes a list entry.
void DeleteCross(HDC dc, const RECT& r, int state, UINT dpi) {
    const int id = dpi >= 144 ? 880 : dpi >= 120 ? 879 : 878;
    struct Dib {
        HBITMAP bmp = nullptr;
        int w = 0, h = 0;
    };
    static std::map<int, Dib> cache;
    auto it = cache.find(id);
    if (it == cache.end()) {
        Dib d;
        if (IWICBitmapSource* src = win7::Resources::Get().Image(win7::Module::Aero, id)) {
            winrt::com_ptr<IWICFormatConverter> conv;
            auto* wic = gfx::CompositionHost::Get().Wic();
            UINT w = 0, h = 0;
            if (SUCCEEDED(wic->CreateFormatConverter(conv.put())) &&
                SUCCEEDED(conv->Initialize(src, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                           WICBitmapPaletteTypeCustom)) &&
                SUCCEEDED(conv->GetSize(&w, &h))) {
                BITMAPINFO bi{};
                bi.bmiHeader = {sizeof(BITMAPINFOHEADER), static_cast<LONG>(w), -static_cast<LONG>(h), 1, 32, BI_RGB};
                void* bits = nullptr;
                if (HBITMAP bmp = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0)) {
                    if (SUCCEEDED(conv->CopyPixels(nullptr, w * 4, w * h * 4, static_cast<BYTE*>(bits))))
                        d = {bmp, static_cast<int>(w), static_cast<int>(h)};
                    else
                        DeleteObject(bmp);
                }
            }
        }
        it = cache.emplace(id, d).first;
    }
    const Dib& d = it->second;
    const int cell = d.h / 3;
    if (!d.bmp || cell <= 0) {  // without the theme: a grey or red cross
        HPEN pen = CreatePen(PS_SOLID, std::max(1, static_cast<int>(dpi / 72)), state ? RGB(0xC4, 0x2B, 0x1C) : RGB(0x8A, 0x8A, 0x8A));
        HGDIOBJ old = SelectObject(dc, pen);
        const int a = (r.bottom - r.top) / 4, cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
        MoveToEx(dc, cx - a, cy - a, nullptr);
        LineTo(dc, cx + a + 1, cy + a + 1);
        MoveToEx(dc, cx - a, cy + a, nullptr);
        LineTo(dc, cx + a + 1, cy - a - 1);
        SelectObject(dc, old);
        DeleteObject(pen);
        return;
    }
    HDC mem = CreateCompatibleDC(dc);
    HGDIOBJ old = SelectObject(mem, d.bmp);
    const BLENDFUNCTION bf{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    const int x = (r.left + r.right - d.w) / 2, y = (r.top + r.bottom - cell) / 2;
    AlphaBlend(dc, x, y, d.w, cell, mem, 0, cell * state, d.w, cell, bf);
    SelectObject(mem, old);
    DeleteDC(mem);
}

void Fill(HDC dc, const RECT& r, COLORREF color) {
    SetDCBrushColor(dc, color);
    FillRect(dc, &r, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
}

// Explorer's hot item: a 1 px #83ABDC frame with rounded corners over a pale blue gradient.
void HotPlate(HDC dc, const RECT& r, float scale) {
    if (!Gdiplus()) return;
    Gdiplus::Graphics g(dc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    const Gdiplus::REAL radius = 2.0f * scale;
    const Gdiplus::RectF box(r.left + 0.5f, r.top + 0.5f, static_cast<Gdiplus::REAL>(r.right - r.left - 1),
                             static_cast<Gdiplus::REAL>(r.bottom - r.top - 1));
    Gdiplus::GraphicsPath path;
    const Gdiplus::REAL d = radius * 2;
    path.AddArc(box.X, box.Y, d, d, 180, 90);
    path.AddArc(box.GetRight() - d, box.Y, d, d, 270, 90);
    path.AddArc(box.GetRight() - d, box.GetBottom() - d, d, d, 0, 90);
    path.AddArc(box.X, box.GetBottom() - d, d, d, 90, 90);
    path.CloseFigure();
    Gdiplus::LinearGradientBrush fill(Gdiplus::PointF(0, box.Y), Gdiplus::PointF(0, box.GetBottom()),
                                      Gdiplus::Color(255, 0xEF, 0xF5, 0xFC), Gdiplus::Color(255, 0xCC, 0xE1, 0xFB));
    g.FillPath(&fill, &path);
    Gdiplus::Pen frame(Gdiplus::Color(255, 0x83, 0xAB, 0xDC), 1.0f);
    g.DrawPath(&frame, &path);
}

} // namespace

SearchPopup::SearchPopup(HWND owner) : m_owner(owner) {
    WNDCLASSEXW wc{sizeof(wc)};
    if (!GetClassInfoExW(GetModuleHandleW(nullptr), kClass, &wc)) {
        wc.style = CS_DROPSHADOW;
        wc.lpfnWndProc = Proc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kClass;
        RegisterClassExW(&wc);
    }
    m_hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kClass, L"", WS_POPUP, 0, 0, 0, 0, owner, nullptr,
                             GetModuleHandleW(nullptr), this);
    if (m_hwnd) {
        DWM_WINDOW_CORNER_PREFERENCE corner = DWMWCP_DONOTROUND; // Win7's square drop-down
        DwmSetWindowAttribute(m_hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
        COLORREF none = DWMWA_COLOR_NONE;
        DwmSetWindowAttribute(m_hwnd, DWMWA_BORDER_COLOR, &none, sizeof(none));
    }
}

SearchPopup::~SearchPopup() {
    if (m_hwnd) DestroyWindow(m_hwnd);
    if (m_font) DeleteObject(m_font);
    if (m_underline) DeleteObject(m_underline);
}

void SearchPopup::Show(const RECT& anchor, Model model) {
    if (!m_hwnd) return;
    const UINT dpi = GetDpiForWindow(m_owner);
    if (dpi != m_dpi || !m_font) {
        m_dpi = dpi;
        if (m_font) DeleteObject(m_font);
        if (m_underline) DeleteObject(m_underline);
        NONCLIENTMETRICSW ncm{sizeof(ncm)};
        SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, dpi);
        m_font = CreateFontIndirectW(&ncm.lfMessageFont);
        LOGFONTW underline = ncm.lfMessageFont;
        underline.lfUnderline = TRUE;
        m_underline = CreateFontIndirectW(&underline);
    }
    const bool rowsChanged = model.rows.size() != m_model.rows.size();
    m_model = std::move(model);
    if (rowsChanged || m_hot >= static_cast<int>(m_model.rows.size())) m_hot = -1;
    const int width = std::max<int>(anchor.right - anchor.left, Px(200));
    Layout(width);
    SetWindowPos(m_hwnd, nullptr, anchor.left, anchor.bottom, width, m_height,
                 SWP_NOACTIVATE | SWP_NOZORDER | SWP_SHOWWINDOW);
    SetWindowPos(m_hwnd, HWND_TOP, 0, 0, 0, 0, SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE);
    InvalidateRect(m_hwnd, nullptr, FALSE);
}

void SearchPopup::Hide() {
    if (m_hwnd && IsWindowVisible(m_hwnd)) ShowWindow(m_hwnd, SW_HIDE);
    if (m_hwnd) KillTimer(m_hwnd, kFadeTimer);
    m_hot = -1;
    m_hotLink = -1;
    std::fill(m_linkBlend.begin(), m_linkBlend.end(), 0.0f);
}

void SearchPopup::MoveHot(int delta) {
    const int n = static_cast<int>(m_model.rows.size());
    if (!n) return;
    if (m_hot < 0)
        m_hot = delta > 0 ? 0 : n - 1;
    else
        m_hot = m_hot + delta < 0 || m_hot + delta >= n ? -1 : m_hot + delta;
    InvalidateRect(m_hwnd, nullptr, FALSE);
}

void SearchPopup::Layout(int width) {
    m_parts.clear();
    HDC dc = GetDC(m_hwnd);
    HGDIOBJ old = SelectObject(dc, m_font);
    // Nothing to list (no history yet): only the footer.
    const bool list = !m_model.rows.empty() || !m_model.note.empty();
    int y = 1 + (list ? Px(kPadY) : 0);
    for (size_t i = 0; i < m_model.rows.size(); ++i) {
        m_parts.push_back({{1, y, width - 1, y + Px(kRowHeight)}, 1, i});
        if (m_model.rows[i].history) {
            const int side = Px(kRowHeight) - Px(2);
            m_parts.push_back({{width - 1 - Px(4) - side, y + Px(1), width - 1 - Px(4), y + Px(1) + side}, 4, i});
        }
        y += Px(kRowHeight);
    }
    if (m_model.rows.empty() && !m_model.note.empty()) y += Px(kRowHeight);
    if (list) y += Px(kPadY);
    m_footerTop = list ? y : 1;
    // The footer: its label, then the links, wrapped to the width.
    const int left = Px(kFooterPadX), right = width - Px(kFooterPadX), line = Px(kFooterLine);
    int fy = y + Px(kFooterPadY);
    const auto measure = [&](const std::wstring& text) {
        SIZE size{};
        GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()), &size);
        return static_cast<int>(size.cx);
    };
    if (!m_model.clearLink.empty()) {
        const int w = measure(m_model.clearLink);
        m_parts.push_back({{right - w, fy, right, fy + Px(kFooterLabel)}, 3, 0});
    }
    fy += Px(kFooterLabel);
    int x = left;
    for (size_t i = 0; i < m_model.links.size(); ++i) {
        const int w = measure(m_model.links[i].text) + 2 * Px(kLinkPadX);
        if (x > left && x + w > right) {
            x = left;
            fy += line;
        }
        m_parts.push_back({{x, fy + Px(1), x + w, fy + line - Px(1)}, 2, i}); // 18 DIP plates
        x += w + Px(kLinkGap);
    }
    fy += line + Px(kFooterPadY);
    m_height = fy + 1;
    m_linkBlend.resize(m_model.links.size(), 0.0f);
    SelectObject(dc, old);
    ReleaseDC(m_hwnd, dc);
}

const SearchPopup::Part* SearchPopup::PartAt(POINT pt) const {
    const Part* found = nullptr;
    for (const auto& part : m_parts)  // a row's cross lies over the row
        if (PtInRect(&part.rect, pt) && (!found || part.kind == 4)) found = &part;
    return found;
}

void SearchPopup::Paint(HDC dc) {
    RECT client{};
    GetClientRect(m_hwnd, &client);
    Fill(dc, client, kBack);
    RECT footer{client.left, m_footerTop, client.right, client.bottom};
    Fill(dc, footer, kFooterBack);
    // The footer's top rule and its fade (Explorer: #DFDFDF, then #EDEDED down to the footer).
    Fill(dc, {client.left, m_footerTop, client.right, m_footerTop + 1}, kFooterRule);
    const int shade = Px(kFooterShadeHeight);
    for (int i = 1; i < shade; ++i) {
        const float t = static_cast<float>(i - 1) / std::max(1, shade - 1);
        const auto mix = [t](int shift) {
            const int a = static_cast<int>((kFooterShade >> shift) & 0xFF), b = static_cast<int>((kFooterBack >> shift) & 0xFF);
            return static_cast<COLORREF>(std::lround(a + (b - a) * t)) << shift;
        };
        Fill(dc, {client.left, m_footerTop + i, client.right, m_footerTop + i + 1}, mix(0) | mix(8) | mix(16));
    }
    SetBkMode(dc, TRANSPARENT);
    HGDIOBJ old = SelectObject(dc, m_font);
    const float scale = m_dpi / 96.0f;
    for (size_t i = 0; i < m_parts.size(); ++i) {
        const Part& part = m_parts[i];
        if (part.kind == 1) {
            const auto& row = m_model.rows[part.index];
            RECT plate{part.rect.left + Px(2), part.rect.top, part.rect.right - Px(2), part.rect.bottom};
            if (static_cast<int>(part.index) == m_hot) HotPlate(dc, plate, scale);
            RECT text{part.rect.left + Px(kTextLeft), part.rect.top, part.rect.right - Px(kTextRight),
                      part.rect.bottom};
            if (row.history) text.right -= Px(kRowHeight); // room for its cross
            if (!row.detail.empty()) {
                RECT measure{};
                DrawTextW(dc, row.detail.c_str(), -1, &measure, DT_SINGLELINE | DT_CALCRECT | DT_NOPREFIX);
                const int w = std::min<int>(measure.right, (text.right - text.left) / 2);
                RECT detail{text.right - w, text.top, text.right, text.bottom};
                SetTextColor(dc, kGrey);
                DrawTextW(dc, row.detail.c_str(), -1, &detail,
                          DT_SINGLELINE | DT_VCENTER | DT_RIGHT | DT_NOPREFIX | DT_END_ELLIPSIS);
                text.right = detail.left - Px(10);
            }
            SetTextColor(dc, kText);
            DrawTextW(dc, row.text.c_str(), -1, &text, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
        } else if (part.kind == 4) {
            // An earlier search's cross: shown on the row under the mouse, red on its plate when
            // the mouse is on it.
            if (static_cast<int>(part.index) != m_hot) continue;
            const bool on = static_cast<int>(i) == m_hotLink;
            DeleteCross(dc, part.rect, on ? (static_cast<int>(i) == m_pressed ? 2 : 1) : 0, m_dpi);
        } else if (part.kind == 2) {
            // A scope: a glassy button under the mouse; the chosen one keeps its button.
            const bool current = m_model.links[part.index].current;
            const float blend = part.index < m_linkBlend.size() ? m_linkBlend[part.index] : 0.0f;
            FilterPlate(dc, part.rect, scale, current ? 1.0f : blend);
            SetTextColor(dc, current ? kText : kLink);
            RECT r = part.rect;
            DrawTextW(dc, m_model.links[part.index].text.c_str(), -1, &r,
                      DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_NOPREFIX);
        } else {
            SelectObject(dc, static_cast<int>(i) == m_hotLink ? m_underline : m_font);
            SetTextColor(dc, kLink);
            RECT r = part.rect;
            DrawTextW(dc, m_model.clearLink.c_str(), -1, &r, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
            SelectObject(dc, m_font);
        }
    }
    if (m_model.rows.empty() && !m_model.note.empty()) {
        RECT note{Px(kTextLeft), 1 + Px(kPadY), client.right - Px(kTextRight), 1 + Px(kPadY + kRowHeight)};
        SetTextColor(dc, kGrey);
        DrawTextW(dc, m_model.note.c_str(), -1, &note, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
    }
    // The footer label sits on the first footer line, left of the clear link.
    {
        RECT label{Px(kFooterPadX), m_footerTop + Px(kFooterPadY), client.right - Px(kFooterPadX),
                   m_footerTop + Px(kFooterPadY + kFooterLabel)};
        SetTextColor(dc, kGrey);
        DrawTextW(dc, m_model.footer.c_str(), -1, &label, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
    }
    SelectObject(dc, old);
    SetDCBrushColor(dc, kBorder);
    FrameRect(dc, &client, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
}

LRESULT CALLBACK SearchPopup::Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto* self = static_cast<SearchPopup*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->m_hwnd = hwnd;
    }
    auto* self = reinterpret_cast<SearchPopup*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    return self ? self->Handle(msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT SearchPopup::Handle(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE; // the search box keeps the keyboard
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(m_hwnd, &ps);
        RECT client{};
        GetClientRect(m_hwnd, &client);
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, client.right, client.bottom);
        HGDIOBJ oldBmp = SelectObject(mem, bmp);
        Paint(mem);
        BitBlt(dc, 0, 0, client.right, client.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, oldBmp);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(m_hwnd, &ps);
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (!m_tracking) {
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, m_hwnd, 0};
            m_tracking = TrackMouseEvent(&tme) != FALSE;
        }
        const POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        const Part* part = PartAt(pt);
        int hot = -1, link = -1; // the highlight follows the mouse, off the rows too
        if (part && (part->kind == 1 || part->kind == 4)) hot = static_cast<int>(part->index);
        if (part && part->kind != 1) link = static_cast<int>(part - m_parts.data());
        SetCursor(LoadCursorW(nullptr, link >= 0 ? IDC_HAND : IDC_ARROW));
        if (hot != m_hot || link != m_hotLink) {
            m_hot = hot;
            m_hotLink = link;
            InvalidateRect(m_hwnd, nullptr, FALSE);
            SetTimer(m_hwnd, kFadeTimer, 16, nullptr); // the scope buttons fade in and out
        }
        return 0;
    }
    case WM_TIMER: {
        if (wp != kFadeTimer) break;
        bool active = false;
        for (size_t i = 0; i < m_linkBlend.size(); ++i) {
            bool hot = false;
            for (size_t k = 0; k < m_parts.size(); ++k)
                if (m_parts[k].kind == 2 && m_parts[k].index == i) hot = static_cast<int>(k) == m_hotLink;
            float& blend = m_linkBlend[i];
            const float target = hot ? 1.0f : 0.0f;
            blend = blend < target ? std::min(target, blend + 0.25f) : std::max(target, blend - 0.1f);
            active |= blend != target;
        }
        InvalidateRect(m_hwnd, nullptr, FALSE);
        if (!active) KillTimer(m_hwnd, kFadeTimer);
        return 0;
    }
    case WM_MOUSELEAVE:
        // The mouse's highlight goes with it, so Enter submits the typed text again (only Up and
        // Down choose a row from the keyboard).
        m_tracking = false;
        if (m_hotLink >= 0 || m_hot >= 0) {
            m_hotLink = -1;
            m_hot = -1;
            InvalidateRect(m_hwnd, nullptr, FALSE);
            SetTimer(m_hwnd, kFadeTimer, 16, nullptr);
        }
        return 0;
    case WM_LBUTTONDOWN: {
        const Part* part = PartAt({GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
        if (part && part->kind == 4) {
            m_pressed = static_cast<int>(part - m_parts.data());
            InvalidateRect(m_hwnd, nullptr, FALSE);
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        const POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        const Part* part = PartAt(pt);
        m_pressed = -1;
        if (!part) return 0;
        const Part chosen = *part; // a callback may rebuild the parts
        if (chosen.kind == 4 && onDelete) onDelete(chosen.index);
        if (chosen.kind == 1 && onRow) onRow(chosen.index);
        if (chosen.kind == 2 && onLink && !m_model.links[chosen.index].current) onLink(chosen.index);
        if (chosen.kind == 3 && onClear) onClear();
        return 0;
    }
    default: break;
    }
    return DefWindowProcW(m_hwnd, msg, wp, lp);
}

} // namespace glasswave::ui
