#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <span>

// Direct2D helpers every painter shares: colours, brushes, the device-pixel grid and small
// geometries. Header-only; all of it works in DIP on a device context whose DPI is the scale.

namespace glasswave::gfx {

// ---- Colours ---------------------------------------------------------------------------------

inline D2D1_COLOR_F Rgba(int r, int g, int b, float a = 1.0f) {
    return D2D1::ColorF(r / 255.0f, g / 255.0f, b / 255.0f, a);
}
inline D2D1_COLOR_F Hex(uint32_t rgb, float a = 1.0f) {
    return Rgba((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF, a);
}
inline D2D1_COLOR_F WithAlpha(D2D1_COLOR_F c, float a) { return D2D1::ColorF(c.r, c.g, c.b, a); }

// ---- Brushes ---------------------------------------------------------------------------------

struct Stop {
    float pos;
    D2D1_COLOR_F color;
};

inline winrt::com_ptr<ID2D1SolidColorBrush> Solid(ID2D1DeviceContext* dc, D2D1_COLOR_F c) {
    winrt::com_ptr<ID2D1SolidColorBrush> b;
    winrt::check_hresult(dc->CreateSolidColorBrush(c, b.put()));
    return b;
}

inline winrt::com_ptr<ID2D1GradientStopCollection> Stops(ID2D1DeviceContext* dc,
                                                         std::span<const D2D1_GRADIENT_STOP> stops) {
    winrt::com_ptr<ID2D1GradientStopCollection> c;
    winrt::check_hresult(dc->CreateGradientStopCollection(stops.data(), static_cast<UINT32>(stops.size()), c.put()));
    return c;
}
inline winrt::com_ptr<ID2D1GradientStopCollection> Stops(ID2D1DeviceContext* dc, std::initializer_list<Stop> stops) {
    std::vector<D2D1_GRADIENT_STOP> v;
    v.reserve(stops.size());
    for (const auto& s : stops) v.push_back({s.pos, s.color});
    return Stops(dc, std::span<const D2D1_GRADIENT_STOP>(v));
}

inline winrt::com_ptr<ID2D1LinearGradientBrush> Linear(ID2D1DeviceContext* dc, D2D1_POINT_2F a, D2D1_POINT_2F b,
                                                       ID2D1GradientStopCollection* stops) {
    winrt::com_ptr<ID2D1LinearGradientBrush> br;
    winrt::check_hresult(dc->CreateLinearGradientBrush({a, b}, stops, br.put()));
    return br;
}
inline winrt::com_ptr<ID2D1LinearGradientBrush> Linear(ID2D1DeviceContext* dc, D2D1_POINT_2F a, D2D1_POINT_2F b,
                                                       std::initializer_list<Stop> stops) {
    return Linear(dc, a, b, Stops(dc, stops).get());
}
inline winrt::com_ptr<ID2D1LinearGradientBrush> Vertical(ID2D1DeviceContext* dc, float y0, float y1,
                                                         std::initializer_list<Stop> stops) {
    return Linear(dc, {0, y0}, {0, y1}, stops);
}
inline winrt::com_ptr<ID2D1LinearGradientBrush> Horizontal(ID2D1DeviceContext* dc, float x0, float x1,
                                                           std::initializer_list<Stop> stops) {
    return Linear(dc, {x0, 0}, {x1, 0}, stops);
}
inline winrt::com_ptr<ID2D1RadialGradientBrush> Radial(ID2D1DeviceContext* dc, D2D1_POINT_2F center, float rx,
                                                       float ry, std::initializer_list<Stop> stops) {
    winrt::com_ptr<ID2D1RadialGradientBrush> br;
    winrt::check_hresult(dc->CreateRadialGradientBrush({center, {0, 0}, rx, ry}, Stops(dc, stops).get(), br.put()));
    return br;
}

// ---- Device-pixel grid -----------------------------------------------------------------------
// Win7 kept 1-pixel hairlines at 125-175% DPI. A 1-DIP line at 175% would be 1.75 px and smear
// into two half-transparent rows, so hairlines are floor(scale) device pixels wide and edges
// are snapped to the pixel grid.

inline float ScaleOf(ID2D1DeviceContext* dc) {
    float dx = 96, dy = 96;
    dc->GetDpi(&dx, &dy);
    return dx / 96.0f;
}
// A hairline's width in DIP: floor(scale) device pixels, at least one.
inline float Hairline(float scale) { return std::max(1.0f, std::floor(scale)) / scale; }
inline float Hairline(ID2D1DeviceContext* dc) { return Hairline(ScaleOf(dc)); }
// `v` (DIP) on the nearest device pixel.
inline float Snap(float v, float scale) { return std::round(v * scale) / scale; }
inline float Snap(ID2D1DeviceContext* dc, float v) { return Snap(v, ScaleOf(dc)); }

inline D2D1_RECT_F Inset(D2D1_RECT_F r, float d) { return {r.left + d, r.top + d, r.right - d, r.bottom - d}; }

// ---- Geometry --------------------------------------------------------------------------------

inline winrt::com_ptr<ID2D1Factory> FactoryOf(ID2D1DeviceContext* dc) {
    winrt::com_ptr<ID2D1Factory> f;
    dc->GetFactory(f.put());
    return f;
}

// A closed, filled polygon through `pts`.
inline winrt::com_ptr<ID2D1PathGeometry> Polygon(ID2D1Factory* f, std::initializer_list<D2D1_POINT_2F> pts) {
    winrt::com_ptr<ID2D1PathGeometry> g;
    winrt::check_hresult(f->CreatePathGeometry(g.put()));
    winrt::com_ptr<ID2D1GeometrySink> s;
    winrt::check_hresult(g->Open(s.put()));
    auto it = pts.begin();
    s->BeginFigure(*it, D2D1_FIGURE_BEGIN_FILLED);
    for (++it; it != pts.end(); ++it) s->AddLine(*it);
    s->EndFigure(D2D1_FIGURE_END_CLOSED);
    winrt::check_hresult(s->Close());
    return g;
}
inline winrt::com_ptr<ID2D1PathGeometry> Polygon(ID2D1DeviceContext* dc, std::initializer_list<D2D1_POINT_2F> pts) {
    return Polygon(FactoryOf(dc).get(), pts);
}

// Strokes with round joins (and `cap` ends): glyphs drawn as lines.
inline winrt::com_ptr<ID2D1StrokeStyle> RoundJoin(ID2D1DeviceContext* dc, D2D1_CAP_STYLE cap = D2D1_CAP_STYLE_FLAT) {
    const D2D1_STROKE_STYLE_PROPERTIES p = D2D1::StrokeStyleProperties(cap, cap, cap, D2D1_LINE_JOIN_ROUND);
    winrt::com_ptr<ID2D1StrokeStyle> s;
    FactoryOf(dc)->CreateStrokeStyle(p, nullptr, 0, s.put());
    return s;
}

} // namespace glasswave::gfx
