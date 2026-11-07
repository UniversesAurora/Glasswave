#include "pch.h"
#include "aero/aero_painter.h"

// Every color and size below is traceable to docs/design/visual-spec.md.

namespace glasswave::aero::paint {

namespace {

using winrt::com_ptr;

D2D1_COLOR_F Rgba(int r, int g, int b, float a = 1.0f) { return D2D1::ColorF(r / 255.0f, g / 255.0f, b / 255.0f, a); }
D2D1_COLOR_F Hex(uint32_t rgb, float a = 1.0f) { return Rgba((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF, a); }
D2D1_COLOR_F WithAlpha(D2D1_COLOR_F c, float a) { return D2D1::ColorF(c.r, c.g, c.b, a); }

// ---- Device-pixel grid -----------------------------------------------------------------
// Win7 kept 1-pixel hairlines at 125-175% DPI. A 1-DIP line at 175% would be 1.75 px and
// smear into two half-transparent rows, so hairlines are floor(scale) device pixels wide
// and rectangle edges are snapped to the pixel grid.
struct Grid {
    float s = 1.0f;  // device pixels per DIP
    float L = 1.0f;  // hairline width in DIP (a whole number of device pixels)
    float Snap(float v) const { return std::round(v * s) / s; }
    D2D1_RECT_F Snap(D2D1_RECT_F r) const { return {Snap(r.left), Snap(r.top), Snap(r.right), Snap(r.bottom)}; }
};

Grid GridOf(ID2D1DeviceContext* dc) {
    float dx = 96, dy = 96;
    dc->GetDpi(&dx, &dy);
    Grid g;
    g.s = dx / 96.0f;
    g.L = std::max(1.0f, std::floor(g.s)) / g.s;
    return g;
}

// DWM draws the frame from bitmaps 1:1 in device pixels at any DPI: its lines stay one
// device pixel wide (measured on Win7 at 150 %).
Grid DeviceGridOf(ID2D1DeviceContext* dc) {
    Grid g = GridOf(dc);
    g.L = 1.0f / g.s;
    return g;
}

com_ptr<ID2D1Factory> FactoryOf(ID2D1DeviceContext* dc) {
    com_ptr<ID2D1Factory> f;
    dc->GetFactory(f.put());
    return f;
}

com_ptr<ID2D1SolidColorBrush> Solid(ID2D1DeviceContext* dc, D2D1_COLOR_F c) {
    com_ptr<ID2D1SolidColorBrush> b;
    winrt::check_hresult(dc->CreateSolidColorBrush(c, b.put()));
    return b;
}

struct Stop {
    float pos;
    D2D1_COLOR_F color;
};

com_ptr<ID2D1GradientStopCollection> Stops(ID2D1DeviceContext* dc, std::initializer_list<Stop> stops) {
    std::vector<D2D1_GRADIENT_STOP> v;
    for (auto& s : stops) v.push_back({s.pos, s.color});
    com_ptr<ID2D1GradientStopCollection> c;
    winrt::check_hresult(dc->CreateGradientStopCollection(v.data(), static_cast<UINT32>(v.size()), c.put()));
    return c;
}

com_ptr<ID2D1LinearGradientBrush> Vertical(ID2D1DeviceContext* dc, float y0, float y1,
                                           std::initializer_list<Stop> stops) {
    com_ptr<ID2D1LinearGradientBrush> br;
    winrt::check_hresult(dc->CreateLinearGradientBrush({{0, y0}, {0, y1}}, Stops(dc, stops).get(), br.put()));
    return br;
}

com_ptr<ID2D1LinearGradientBrush> Linear(ID2D1DeviceContext* dc, D2D1_POINT_2F a, D2D1_POINT_2F b,
                                         std::initializer_list<Stop> stops) {
    com_ptr<ID2D1LinearGradientBrush> br;
    winrt::check_hresult(dc->CreateLinearGradientBrush({a, b}, Stops(dc, stops).get(), br.put()));
    return br;
}

com_ptr<ID2D1RadialGradientBrush> Radial(ID2D1DeviceContext* dc, D2D1_POINT_2F center, float rx, float ry,
                                         std::initializer_list<Stop> stops) {
    com_ptr<ID2D1RadialGradientBrush> br;
    winrt::check_hresult(
        dc->CreateRadialGradientBrush({center, {0, 0}, rx, ry}, Stops(dc, stops).get(), br.put()));
    return br;
}

struct Radii {
    float tl = 0, tr = 0, br = 0, bl = 0;
    Radii Shrink(float d) const {
        auto f = [d](float r) { return r > 0 ? std::max(0.0f, r - d) : 0.0f; };
        return {f(tl), f(tr), f(br), f(bl)};
    }
};

com_ptr<ID2D1PathGeometry> RoundRect(ID2D1Factory* f, D2D1_RECT_F r, Radii rad) {
    com_ptr<ID2D1PathGeometry> g;
    winrt::check_hresult(f->CreatePathGeometry(g.put()));
    com_ptr<ID2D1GeometrySink> s;
    winrt::check_hresult(g->Open(s.put()));
    auto arc = [&](float x, float y, float radius) {
        s->AddArc({{x, y}, {radius, radius}, 0, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL});
    };
    s->BeginFigure({r.left, r.top + rad.tl}, D2D1_FIGURE_BEGIN_FILLED);
    if (rad.tl > 0) arc(r.left + rad.tl, r.top, rad.tl);
    s->AddLine({r.right - rad.tr, r.top});
    if (rad.tr > 0) arc(r.right, r.top + rad.tr, rad.tr);
    s->AddLine({r.right, r.bottom - rad.br});
    if (rad.br > 0) arc(r.right - rad.br, r.bottom, rad.br);
    s->AddLine({r.left + rad.bl, r.bottom});
    if (rad.bl > 0) arc(r.left, r.bottom - rad.bl, rad.bl);
    s->EndFigure(D2D1_FIGURE_END_CLOSED);
    winrt::check_hresult(s->Close());
    return g;
}

D2D1_RECT_F Inset(D2D1_RECT_F r, float d) { return {r.left + d, r.top + d, r.right - d, r.bottom - d}; }

// Hairline ring k (0 = outermost) of a rounded rect: covers exactly [k*L, (k+1)*L).
com_ptr<ID2D1PathGeometry> Ring(ID2D1Factory* f, const Grid& g, D2D1_RECT_F outer, Radii rad, int k) {
    const float inset = k * g.L + g.L / 2;
    return RoundRect(f, Inset(outer, inset), rad.Shrink(inset));
}

// Area inside ring k.
com_ptr<ID2D1PathGeometry> Inner(ID2D1Factory* f, const Grid& g, D2D1_RECT_F outer, Radii rad, int k) {
    const float inset = k * g.L;
    return RoundRect(f, Inset(outer, inset), rad.Shrink(inset));
}

template <class F>
void Clipped(ID2D1DeviceContext* dc, ID2D1Geometry* clip, F&& body) {
    dc->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), clip, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE), nullptr);
    body();
    dc->PopLayer();
}

com_ptr<ID2D1PathGeometry> Polygon(ID2D1Factory* f, std::initializer_list<D2D1_POINT_2F> pts) {
    com_ptr<ID2D1PathGeometry> g;
    winrt::check_hresult(f->CreatePathGeometry(g.put()));
    com_ptr<ID2D1GeometrySink> s;
    winrt::check_hresult(g->Open(s.put()));
    auto it = pts.begin();
    s->BeginFigure(*it, D2D1_FIGURE_BEGIN_FILLED);
    for (++it; it != pts.end(); ++it) s->AddLine(*it);
    s->EndFigure(D2D1_FIGURE_END_CLOSED);
    winrt::check_hresult(s->Close());
    return g;
}

com_ptr<ID2D1Geometry> Group(ID2D1Factory* f, std::vector<com_ptr<ID2D1Geometry>> parts) {
    std::vector<ID2D1Geometry*> raw;
    for (auto& p : parts) raw.push_back(p.get());
    com_ptr<ID2D1GeometryGroup> g;
    winrt::check_hresult(
        f->CreateGeometryGroup(D2D1_FILL_MODE_WINDING, raw.data(), static_cast<UINT32>(raw.size()), g.put()));
    return g;
}

com_ptr<ID2D1Geometry> RectGeo(ID2D1Factory* f, D2D1_RECT_F r) {
    com_ptr<ID2D1RectangleGeometry> g;
    winrt::check_hresult(f->CreateRectangleGeometry(r, g.put()));
    return g;
}

com_ptr<ID2D1Geometry> Combine(ID2D1Factory* f, ID2D1Geometry* a, ID2D1Geometry* b, D2D1_COMBINE_MODE mode) {
    com_ptr<ID2D1PathGeometry> out;
    winrt::check_hresult(f->CreatePathGeometry(out.put()));
    com_ptr<ID2D1GeometrySink> s;
    winrt::check_hresult(out->Open(s.put()));
    winrt::check_hresult(a->CombineWithGeometry(b, mode, nullptr, s.get()));
    winrt::check_hresult(s->Close());
    return out;
}

com_ptr<ID2D1StrokeStyle> RoundJoin(ID2D1DeviceContext* dc, D2D1_CAP_STYLE cap = D2D1_CAP_STYLE_FLAT) {
    D2D1_STROKE_STYLE_PROPERTIES p = D2D1::StrokeStyleProperties(cap, cap, cap, D2D1_LINE_JOIN_ROUND);
    com_ptr<ID2D1StrokeStyle> s;
    FactoryOf(dc)->CreateStrokeStyle(p, nullptr, 0, s.put());
    return s;
}

// White glyph with a one-hairline outline (stroke 2L centered; the fill covers the inner half).
void OutlinedGlyph(ID2D1DeviceContext* dc, const Grid& g, ID2D1Geometry* shape, D2D1_COLOR_F outline,
                   ID2D1Brush* fill) {
    auto o = Solid(dc, outline);
    dc->DrawGeometry(shape, o.get(), 2 * g.L, RoundJoin(dc).get());
    dc->FillGeometry(shape, fill);
}

// ---- Caption buttons ----------------------------------------------------------------------
// Solved from the same button over light glass (#b8dbfd) and dark glass (#3d70a1): the upper
// half is a nearly opaque pale blue (a ~0.72 -> 0.40), the lower half a faint navy veil
// that lets the glass through, brightening toward the bottom.

struct ButtonPalette {
    D2D1_COLOR_F upperTop, upperBottom, lowerTop, lowerBottom;
    D2D1_COLOR_F glow;  // bottom glow (alpha 0 = none)
    D2D1_COLOR_F ringTop, ringSide, ringBottom;
    D2D1_COLOR_F outline, outerLight;
    D2D1_COLOR_F glyphOutline;
    float glyphAlpha;
};

ButtonPalette CaptionPalette(bool close, Layer layer) {
    if (layer == Layer::Inactive) {
        return {Rgba(225, 236, 247, 0.48f), Rgba(215, 230, 245, 0.22f), Rgba(0, 20, 50, 0.05f),
                Rgba(0, 20, 50, 0.03f),     Rgba(0, 0, 0, 0.0f),        Rgba(255, 255, 255, 0.70f),
                Rgba(255, 255, 255, 0.28f), Rgba(230, 240, 250, 0.35f), Rgba(0, 18, 40, 0.48f),
                Rgba(255, 255, 255, 0.25f), Rgba(0, 20, 40, 0.38f),     0.9f};
    }
    if (close) {
        switch (layer) {
        case Layer::Hover:
            return {Hex(0xF3B2A6, 0.95f), Hex(0xEA7C68), Hex(0xDE3A1C), Hex(0xF06A34), Rgba(255, 205, 110, 0.85f),
                    Rgba(255, 245, 240, 0.95f), Rgba(250, 205, 195, 0.70f), Hex(0xFFC79C), Rgba(70, 20, 16, 0.90f),
                    Rgba(255, 255, 255, 0.35f), Rgba(80, 16, 10, 0.55f), 1.0f};
        case Layer::Pressed:
            return {Hex(0xC98A80, 0.95f), Hex(0xB2584A), Hex(0x8E1A0E), Hex(0xA8361C), Rgba(255, 140, 70, 0.40f),
                    Rgba(230, 170, 160, 0.90f), Rgba(200, 140, 130, 0.60f), Hex(0xC97E66), Rgba(60, 14, 10, 0.92f),
                    Rgba(255, 255, 255, 0.30f), Rgba(70, 12, 8, 0.60f), 1.0f};
        default:  // measured: upper #dc9694 -> #c9746f, lower #b13a2e -> #c15044, rim #f0cac7 / #daa8a7
            return {Hex(0xE2968F, 0.85f), Hex(0xCC6F66, 0.95f), Hex(0xB13A2E), Hex(0xC2584A), Rgba(255, 150, 120, 0.30f),
                    Rgba(255, 238, 238, 0.90f), Rgba(240, 190, 186, 0.60f), Hex(0xDAA8A7), Rgba(60, 25, 28, 0.85f),
                    Rgba(255, 255, 255, 0.35f), Rgba(70, 20, 16, 0.50f), 1.0f};
        }
    }
    switch (layer) {
    case Layer::Hover:
        return {Rgba(238, 248, 255, 0.88f), Rgba(205, 236, 255, 0.62f), Rgba(40, 140, 220, 0.45f),
                Rgba(80, 200, 255, 0.75f),  Rgba(170, 240, 255, 0.90f), Rgba(255, 255, 255, 0.95f),
                Rgba(255, 255, 255, 0.55f), Rgba(220, 250, 255, 0.70f), Rgba(0, 18, 40, 0.75f),
                Rgba(255, 255, 255, 0.40f), Rgba(0, 30, 60, 0.50f),     1.0f};
    case Layer::Pressed:
        return {Rgba(200, 222, 242, 0.80f), Rgba(150, 190, 225, 0.60f), Rgba(10, 60, 132, 0.65f),
                Rgba(40, 130, 205, 0.80f),  Rgba(110, 210, 255, 0.50f), Rgba(255, 255, 255, 0.55f),
                Rgba(255, 255, 255, 0.30f), Rgba(200, 235, 255, 0.45f), Rgba(0, 18, 40, 0.80f),
                Rgba(255, 255, 255, 0.30f), Rgba(0, 30, 60, 0.55f),     1.0f};
    default:
        return {Rgba(210, 232, 253, 0.72f), Rgba(200, 226, 252, 0.40f), Rgba(0, 20, 50, 0.16f),
                Rgba(0, 20, 50, 0.08f),     Rgba(150, 200, 240, 0.25f), Rgba(255, 255, 255, 0.88f),
                Rgba(255, 255, 255, 0.35f), Rgba(200, 230, 255, 0.50f), Rgba(0, 18, 40, 0.72f),
                Rgba(255, 255, 255, 0.35f), Rgba(0, 20, 40, 0.50f),     1.0f};
    }
}

// The two-tone Aero glass button: outline, inner rim, upper/lower gradients, bottom glow.
void GlassButtonBody(ID2D1DeviceContext* dc, const Grid& g, D2D1_RECT_F rc, Radii rad, const ButtonPalette& p,
                     float split) {
    auto f = FactoryOf(dc);
    auto body = Inner(f.get(), g, rc, rad, 1);
    const float mid = g.Snap(rc.top + (rc.bottom - rc.top) * split);
    Clipped(dc, body.get(), [&] {
        auto upper = Vertical(dc, rc.top, mid, {{0.0f, p.upperTop}, {1.0f, p.upperBottom}});
        dc->FillRectangle({rc.left, rc.top, rc.right, mid}, upper.get());
        auto lower = Vertical(dc, mid, rc.bottom, {{0.0f, p.lowerTop}, {1.0f, p.lowerBottom}});
        dc->FillRectangle({rc.left, mid, rc.right, rc.bottom}, lower.get());
        if (p.glow.a > 0) {
            const float w = rc.right - rc.left, h = rc.bottom - rc.top;
            auto glow = Radial(dc, {(rc.left + rc.right) / 2, rc.bottom}, w * 0.55f, h * 0.55f,
                               {{0.0f, p.glow}, {1.0f, WithAlpha(p.glow, 0.0f)}});
            dc->FillRectangle({rc.left, mid, rc.right, rc.bottom}, glow.get());
        }
    });
    auto rim = Vertical(dc, rc.top, rc.bottom,
                        {{0.0f, p.ringTop}, {0.15f, p.ringSide}, {0.85f, p.ringSide}, {1.0f, p.ringBottom}});
    dc->DrawGeometry(Ring(f.get(), g, rc, rad, 1).get(), rim.get(), g.L);
    auto outline = Solid(dc, p.outline);
    dc->DrawGeometry(Ring(f.get(), g, rc, rad, 0).get(), outline.get(), g.L);
}

com_ptr<ID2D1Geometry> CaptionGlyphShape(ID2D1Factory* f, const Grid& g, CaptionGlyph glyph, D2D1_POINT_2F c) {
    auto rect = [&](float l, float t, float r, float b) { return RectGeo(f, g.Snap(D2D1_RECT_F{l, t, r, b})); };
    auto frame = [&](float l, float t, float w, float h, float th) {
        auto outer = rect(l, t, l + w, t + h);
        auto hole = rect(l + th, t + th, l + w - th, t + h - th);
        return Combine(f, outer.get(), hole.get(), D2D1_COMBINE_MODE_EXCLUDE);
    };
    switch (glyph) {
    case CaptionGlyph::Minimize:  // 12.5 x 2.5 bar, 4 DIP below center (measured at 120%)
        return rect(c.x - 6.0f, c.y + 2.75f, c.x + 6.5f, c.y + 5.25f);
    case CaptionGlyph::Maximize:  // 13 x 10 frame, 2.5 thick
        return frame(c.x - 6.5f, c.y - 5.0f, 13.0f, 10.0f, 2.5f);
    case CaptionGlyph::Restore: {
        auto back = frame(c.x - 3.5f, c.y - 6.5f, 10.0f, 8.0f, 2.0f);
        auto frontArea = rect(c.x - 7.5f, c.y - 2.5f, c.x + 3.5f, c.y + 6.5f);
        auto backVisible = Combine(f, back.get(), frontArea.get(), D2D1_COMBINE_MODE_EXCLUDE);
        return Group(f, {backVisible, frame(c.x - 6.5f, c.y - 1.5f, 9.0f, 7.0f, 2.0f)});
    }
    case CaptionGlyph::Close: {  // flat-ended diagonal bars, 14 x 10.5
        const float hw = 7.0f, hh = 5.25f, bar = 3.6f;
        const float t = g.Snap(c.y - hh), b = g.Snap(c.y + hh);
        const float l = c.x - hw, r = c.x + hw;
        auto a = Polygon(f, {{l, t}, {l + bar, t}, {r, b}, {r - bar, b}});
        auto d = Polygon(f, {{r - bar, t}, {r, t}, {l + bar, b}, {l, b}});
        return Group(f, {a, d});
    }
    }
    return nullptr;
}

// ---- Transport glyphs ----------------------------------------------------------------------

com_ptr<ID2D1Geometry> FilledGlyph(ID2D1Factory* f, const Grid& g, TransportGlyph glyph, D2D1_POINT_2F c, float u) {
    // u: glyph unit (DIP); shapes are designed on a 1-unit grid.
    switch (glyph) {
    case TransportGlyph::Play:
        return Polygon(f, {{c.x - 4.6f * u, c.y - 7.5f * u}, {c.x + 7.2f * u, c.y}, {c.x - 4.6f * u, c.y + 7.5f * u}});
    case TransportGlyph::Pause:
        return Group(f, {RectGeo(f, g.Snap(D2D1_RECT_F{c.x - 5.5f * u, c.y - 7 * u, c.x - 1.5f * u, c.y + 7 * u})),
                         RectGeo(f, g.Snap(D2D1_RECT_F{c.x + 1.5f * u, c.y - 7 * u, c.x + 5.5f * u, c.y + 7 * u}))});
    case TransportGlyph::Stop:
        return RectGeo(f, g.Snap(D2D1_RECT_F{c.x - 3.75f * u, c.y - 3.75f * u, c.x + 3.75f * u, c.y + 3.75f * u}));
    case TransportGlyph::Previous: {
        const float x0 = c.x - 7.0f * u;
        return Group(f, {RectGeo(f, g.Snap(D2D1_RECT_F{x0, c.y - 4.5f * u, x0 + 2.0f * u, c.y + 4.5f * u})),
                         Polygon(f, {{x0 + 2.0f * u, c.y}, {x0 + 8.0f * u, c.y - 4.5f * u}, {x0 + 8.0f * u, c.y + 4.5f * u}}),
                         Polygon(f, {{x0 + 8.0f * u, c.y}, {x0 + 14.0f * u, c.y - 4.5f * u}, {x0 + 14.0f * u, c.y + 4.5f * u}})});
    }
    case TransportGlyph::Next: {
        const float x1 = c.x + 7.0f * u;
        return Group(f, {RectGeo(f, g.Snap(D2D1_RECT_F{x1 - 2.0f * u, c.y - 4.5f * u, x1, c.y + 4.5f * u})),
                         Polygon(f, {{x1 - 2.0f * u, c.y}, {x1 - 8.0f * u, c.y - 4.5f * u}, {x1 - 8.0f * u, c.y + 4.5f * u}}),
                         Polygon(f, {{x1 - 8.0f * u, c.y}, {x1 - 14.0f * u, c.y - 4.5f * u}, {x1 - 14.0f * u, c.y + 4.5f * u}})});
    }
    default:
        return nullptr;
    }
}

// Stroked line-art glyphs (shuffle, repeat, volume) drawn with `brush`.
void LineGlyph(ID2D1DeviceContext* dc, TransportGlyph glyph, D2D1_POINT_2F c, ID2D1Brush* brush) {
    auto f = FactoryOf(dc);
    const float w = 1.9f;
    auto stroke = RoundJoin(dc, D2D1_CAP_STYLE_ROUND);
    auto arrowHead = [&](D2D1_POINT_2F tip, float dx, float dy) {
        // dx/dy: unit direction of travel
        const float s = 3.2f;
        const D2D1_POINT_2F a{tip.x - dx * s - dy * s, tip.y - dy * s + dx * s};
        const D2D1_POINT_2F b{tip.x - dx * s + dy * s, tip.y - dy * s - dx * s};
        dc->FillGeometry(Polygon(f.get(), {tip, a, b}).get(), brush);
    };
    switch (glyph) {
    case TransportGlyph::Repeat: {
        com_ptr<ID2D1PathGeometry> g;
        f->CreatePathGeometry(g.put());
        com_ptr<ID2D1GeometrySink> s;
        g->Open(s.put());
        const float r = 5.0f;
        s->BeginFigure({c.x + r * 0.70f, c.y - r * 0.71f}, D2D1_FIGURE_BEGIN_HOLLOW);
        s->AddArc({{c.x + r * 0.95f, c.y - r * 0.30f}, {r, r}, 0, D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE,
                   D2D1_ARC_SIZE_LARGE});
        s->EndFigure(D2D1_FIGURE_END_OPEN);
        s->Close();
        dc->DrawGeometry(g.get(), brush, w, stroke.get());
        arrowHead({c.x + r * 1.05f, c.y - r * 1.05f}, 0.7f, -0.7f);
        break;
    }
    case TransportGlyph::Shuffle: {
        com_ptr<ID2D1PathGeometry> g;
        f->CreatePathGeometry(g.put());
        com_ptr<ID2D1GeometrySink> s;
        g->Open(s.put());
        s->BeginFigure({c.x - 5.5f, c.y + 5.0f}, D2D1_FIGURE_BEGIN_HOLLOW);
        s->AddBezier({{c.x - 1.0f, c.y + 5.0f}, {c.x + 1.0f, c.y - 4.0f}, {c.x + 4.0f, c.y - 5.5f}});
        s->EndFigure(D2D1_FIGURE_END_OPEN);
        s->BeginFigure({c.x - 5.5f, c.y - 5.0f}, D2D1_FIGURE_BEGIN_HOLLOW);
        s->AddBezier({{c.x - 1.5f, c.y - 5.0f}, {c.x - 0.5f, c.y + 2.0f}, {c.x + 2.5f, c.y + 1.0f}});
        s->EndFigure(D2D1_FIGURE_END_OPEN);
        s->Close();
        dc->DrawGeometry(g.get(), brush, w, stroke.get());
        arrowHead({c.x + 6.0f, c.y - 6.5f}, 0.8f, -0.6f);
        arrowHead({c.x + 4.5f, c.y + 0.2f}, 0.95f, -0.3f);
        break;
    }
    case TransportGlyph::Volume:
    case TransportGlyph::Mute: {
        auto body = Polygon(f.get(), {{c.x - 6.5f, c.y - 2.2f}, {c.x - 4.0f, c.y - 2.2f}, {c.x - 0.5f, c.y - 5.5f},
                                       {c.x - 0.5f, c.y + 5.5f}, {c.x - 4.0f, c.y + 2.2f}, {c.x - 6.5f, c.y + 2.2f}});
        dc->FillGeometry(body.get(), brush);
        if (glyph == TransportGlyph::Volume) {
            for (float r : {3.0f, 6.0f}) {
                com_ptr<ID2D1PathGeometry> g;
                f->CreatePathGeometry(g.put());
                com_ptr<ID2D1GeometrySink> s;
                g->Open(s.put());
                s->BeginFigure({c.x + 0.5f + r * 0.55f, c.y - r * 0.85f}, D2D1_FIGURE_BEGIN_HOLLOW);
                s->AddArc({{c.x + 0.5f + r * 0.55f, c.y + r * 0.85f}, {r, r}, 0, D2D1_SWEEP_DIRECTION_CLOCKWISE,
                           D2D1_ARC_SIZE_SMALL});
                s->EndFigure(D2D1_FIGURE_END_OPEN);
                s->Close();
                dc->DrawGeometry(g.get(), brush, 1.6f, stroke.get());
            }
        } else {
            dc->DrawLine({c.x + 2.0f, c.y - 3.0f}, {c.x + 7.0f, c.y + 3.0f}, brush, 1.6f, stroke.get());
            dc->DrawLine({c.x + 2.0f, c.y + 3.0f}, {c.x + 7.0f, c.y - 3.0f}, brush, 1.6f, stroke.get());
        }
        break;
    }
    default:
        break;
    }
}

// WMP 12 icon blue (enabled) / grey (disabled), with a 1-hairline white emboss below.
com_ptr<ID2D1LinearGradientBrush> GlyphBrush(ID2D1DeviceContext* dc, float top, float bottom, bool enabled,
                                             Layer layer) {
    if (!enabled) return Vertical(dc, top, bottom, {{0.0f, Hex(0xAAB4BF)}, {1.0f, Hex(0x8C98A5)}});
    if (layer == Layer::Hover)
        return Vertical(dc, top, bottom, {{0.0f, Hex(0x5AA6F0)}, {1.0f, Hex(0x1C62C0)}});
    return Vertical(dc, top, bottom, {{0.0f, Hex(0x3C88DA)}, {1.0f, Hex(0x154EA2)}});
}

}  // namespace

// ---- Caption buttons ----------------------------------------------------------------------

void CaptionButton(ID2D1DeviceContext* dc, D2D1_SIZE_F size, CaptionGlyph glyph, Layer layer, bool first,
                   bool last) {
    const Grid g = GridOf(dc);
    auto f = FactoryOf(dc);
    const bool close = glyph == CaptionGlyph::Close;
    const ButtonPalette p = CaptionPalette(close, layer);
    const float r = 3.0f;

    // The cluster carries a faint light line outside its outline (left, bottom, right).
    const D2D1_RECT_F outer = g.Snap(D2D1_RECT_F{0, 0, size.width, size.height});
    const D2D1_RECT_F rc{outer.left + (first ? g.L : 0), outer.top, outer.right - (last ? g.L : 0),
                         outer.bottom - g.L};
    const Radii rad{0, 0, last ? r : 0, first ? r : 0};
    {
        auto light = Solid(dc, p.outerLight);
        const Radii outerRad{0, 0, last ? r + g.L : 0, first ? r + g.L : 0};
        auto ring = Ring(f.get(), g, outer, outerRad, 0);
        Clipped(dc, RectGeo(f.get(), {outer.left, outer.top + 4, outer.right, outer.bottom}).get(),
                [&] { dc->DrawGeometry(ring.get(), light.get(), g.L); });
    }
    GlassButtonBody(dc, g, rc, rad, p, 0.47f);

    const D2D1_POINT_2F c{g.Snap((rc.left + rc.right) / 2), g.Snap((rc.top + rc.bottom) / 2)};
    auto shape = CaptionGlyphShape(f.get(), g, glyph, c);
    auto white = Solid(dc, Rgba(255, 255, 255, p.glyphAlpha));
    OutlinedGlyph(dc, g, shape.get(), p.glyphOutline, white.get());
}

// ---- Frame and client edge -----------------------------------------------------------------

void FrameOutline(ID2D1DeviceContext* dc, D2D1_SIZE_F size, float cornerRadius) {
    const Grid g = DeviceGridOf(dc);
    auto f = FactoryOf(dc);
    const D2D1_RECT_F rc = g.Snap(D2D1_RECT_F{0, 0, size.width, size.height});
    const Radii rad{cornerRadius, cornerRadius, cornerRadius, cornerRadius};
    // measured: outer #020303 a0.84; inner highlight #f5f5f6 a0.84 (top #f9fafa a0.90)
    auto outer = Solid(dc, Rgba(2, 3, 3, 0.84f));
    dc->DrawGeometry(Ring(f.get(), g, rc, rad, 0).get(), outer.get(), g.L);
    auto inner = Vertical(dc, rc.top, rc.bottom,
                          {{0.0f, Rgba(249, 250, 250, 0.90f)}, {0.3f, Rgba(245, 245, 246, 0.84f)},
                           {1.0f, Rgba(245, 245, 246, 0.84f)}});
    dc->DrawGeometry(Ring(f.get(), g, rc, rad, 1).get(), inner.get(), g.L);
}

void ClientEdge(ID2D1DeviceContext* dc, D2D1_SIZE_F size) {
    const Grid g = DeviceGridOf(dc);
    const D2D1_RECT_F rc = g.Snap(D2D1_RECT_F{0, 0, size.width, size.height});
    // measured: light #e0e5e9 a0.65 outside, dark #1b1e21 a0.66 against the client
    auto light = Solid(dc, Rgba(224, 229, 233, 0.65f));
    dc->DrawRectangle(Inset(rc, g.L / 2), light.get(), g.L);
    auto dark = Solid(dc, Rgba(27, 30, 33, 0.66f));
    dc->DrawRectangle(Inset(rc, g.L * 1.5f), dark.get(), g.L);
    auto base = Solid(dc, Hex(0xF1F7FD));  // WMP 12 content background (#e9f2f8 .. #f2f8ff)
    dc->FillRectangle(Inset(rc, g.L * 2), base.get());
}

void ClientGlow(ID2D1DeviceContext* dc, D2D1_SIZE_F size) {
    // Faint cyan glow just inside the client edge (#d7f7ff -> #def6fe -> #e5f7fc), drawn
    // as translucent rings over whatever the client paints there.
    const Grid g = DeviceGridOf(dc);
    const D2D1_RECT_F rc = g.Snap(D2D1_RECT_F{0, 0, size.width, size.height});
    const float alpha[] = {0.55f, 0.35f, 0.18f};
    for (int i = 0; i < 3; ++i) {
        auto b = Solid(dc, Rgba(205, 245, 255, alpha[i]));
        dc->DrawRectangle(Inset(rc, g.L * (i + 0.5f)), b.get(), g.L);
    }
}

// ---- Reflection -----------------------------------------------------------------------------

void Reflection(ID2D1DeviceContext* dc, D2D1_SIZE_F size) {
    struct Streak {
        float pos, width, alpha, hardness;
    };
    // Win7 glass reflection: soft white bands leaning like "/", denser on the left.
    static constexpr Streak kStreaks[] = {
        {0.05f, 0.09f, 0.16f, 0.0f}, {0.115f, 0.020f, 0.22f, 0.5f}, {0.16f, 0.010f, 0.16f, 0.6f},
        {0.30f, 0.16f, 0.08f, 0.0f}, {0.47f, 0.035f, 0.18f, 0.3f},  {0.52f, 0.012f, 0.16f, 0.6f},
        {0.66f, 0.12f, 0.10f, 0.0f}, {0.80f, 0.028f, 0.18f, 0.4f},  {0.86f, 0.010f, 0.14f, 0.6f},
        {0.95f, 0.07f, 0.10f, 0.0f},
    };
    D2D1_MATRIX_3X2_F saved;
    dc->GetTransform(&saved);
    const float diag = std::hypot(size.width, size.height);
    dc->SetTransform(D2D1::Matrix3x2F::Rotation(22.0f, {size.width / 2, size.height / 2}) * saved);
    for (const auto& s : kStreaks) {
        const float cx = -diag * 0.25f + s.pos * diag * 1.5f;
        const float half = s.width * diag * 0.5f;
        const float edge = 0.5f * (1.0f - s.hardness);
        auto br = Linear(dc, {cx - half, 0}, {cx + half, 0},
                         {{0.0f, Rgba(255, 255, 255, 0.0f)},
                          {0.5f - edge * 0.98f, Rgba(255, 255, 255, s.alpha * 0.35f)},
                          {0.5f, Rgba(255, 255, 255, s.alpha)},
                          {0.5f + edge * 0.98f, Rgba(255, 255, 255, s.alpha * 0.35f)},
                          {1.0f, Rgba(255, 255, 255, 0.0f)}});
        dc->FillRectangle({cx - half, -diag, cx + half, diag * 2}, br.get());
    }
    dc->SetTransform(saved);
}

// ---- Glow text ------------------------------------------------------------------------------

void GlowText(ID2D1DeviceContext* dc, IDWriteTextLayout* layout, D2D1_POINT_2F origin, D2D1_COLOR_F textColor) {
    // Win7's caption glow is a faint, soft, roughly rectangular halo (DTT_GLOWSIZE), not
    // an opaque plate: blur a rounded block covering the ink and draw it once at low alpha.
    DWRITE_TEXT_METRICS tm{};
    layout->GetMetrics(&tm);
    DWRITE_OVERHANG_METRICS om{};
    layout->GetOverhangMetrics(&om);
    com_ptr<ID2D1CommandList> list;
    winrt::check_hresult(dc->CreateCommandList(list.put()));
    com_ptr<ID2D1Image> oldTarget;
    dc->GetTarget(oldTarget.put());
    D2D1_MATRIX_3X2_F saved;
    dc->GetTransform(&saved);
    dc->SetTarget(list.get());
    dc->SetTransform(D2D1::Matrix3x2F::Identity());
    auto white = Solid(dc, Rgba(255, 255, 255));
    const D2D1_RECT_F block{origin.x + tm.left - 1.0f, origin.y + tm.top + 2.0f,
                            origin.x + tm.left + tm.widthIncludingTrailingWhitespace + 1.0f,
                            origin.y + tm.top + tm.height - 1.0f};
    dc->FillRoundedRectangle({block, 3, 3}, white.get());
    dc->SetTarget(oldTarget.get());
    dc->SetTransform(saved);
    winrt::check_hresult(list->Close());

    com_ptr<ID2D1Effect> blur;
    winrt::check_hresult(dc->CreateEffect(CLSID_D2D1Shadow, blur.put()));
    blur->SetInput(0, list.get());
    blur->SetValue(D2D1_SHADOW_PROP_BLUR_STANDARD_DEVIATION, 3.5f);
    blur->SetValue(D2D1_SHADOW_PROP_COLOR, D2D1::Vector4F(1, 1, 1, 0.62f));
    dc->DrawImage(blur.get());

    auto text = Solid(dc, textColor);
    dc->DrawTextLayout(origin, layout, text.get(), D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
}

// ---- WMP 12 transport ---------------------------------------------------------------------
// Measured from WMP 12 library mode (Win7, 120% DPI; values converted to DIP).

void TransportCapsule(ID2D1DeviceContext* dc, D2D1_SIZE_F size) {
    const Grid g = GridOf(dc);
    auto f = FactoryOf(dc);
    const D2D1_RECT_F rc = g.Snap(D2D1_RECT_F{0, 0, size.width, size.height});
    const float r = (rc.bottom - rc.top) / 2;
    const Radii rad{r, r, r, r};
    auto fill = Vertical(dc, rc.top, rc.bottom, {{0.0f, Hex(0xE6EDF5)}, {1.0f, Hex(0xE1EAF7)}});
    dc->FillGeometry(Inner(f.get(), g, rc, rad, 0).get(), fill.get());
    auto rim = Vertical(dc, rc.top, rc.bottom, {{0.0f, Hex(0xDEE6EE)}, {0.5f, Rgba(255, 255, 255, 0.0f)},
                                                {1.0f, Rgba(255, 255, 255, 0.0f)}});
    dc->DrawGeometry(Ring(f.get(), g, rc, rad, 1).get(), rim.get(), g.L);
    auto outline = Vertical(dc, rc.top, rc.bottom, {{0.0f, Hex(0x9BA3AC)}, {1.0f, Hex(0xB3BDCA)}});
    dc->DrawGeometry(Ring(f.get(), g, rc, rad, 0).get(), outline.get(), g.L);
}

void PlayOrb(ID2D1DeviceContext* dc, float size, Layer layer, TransportGlyph glyph) {
    const Grid g = GridOf(dc);
    auto f = FactoryOf(dc);
    const float R = size / 2;
    const D2D1_POINT_2F c{R, R};
    const float L = g.L;
    const float r = R - L;  // orb radius inside the white halo ring

    // White halo ring that separates the orb from the segments (#e0f2ff / #cff2ff).
    auto halo = Vertical(dc, 0, size, {{0.0f, Rgba(240, 248, 255, 0.95f)}, {1.0f, Hex(0xCFF2FF)}});
    dc->FillEllipse({c, R, R}, halo.get());
    // Outline: grey-blue at the top (#7d8396), teal at the bottom (#437b81).
    auto outline = Vertical(dc, c.y - r, c.y + r, {{0.0f, Hex(0x6F7F9A)}, {1.0f, Hex(0x3F7486)}});
    dc->FillEllipse({c, r, r}, outline.get());

    struct OrbPalette {
        D2D1_COLOR_F glossTop, glossBottom, horizon, lowMid, low, bottom, rim;
    } p;
    switch (layer) {
    case Layer::Hover:
        p = {Hex(0xDCE5F4), Hex(0x9CBEE3), Hex(0x2E59A8), Hex(0x2F86DA), Hex(0x3CC2F2), Hex(0x8FECF8), Hex(0xC2FBFF)};
        break;
    case Layer::Pressed:
        p = {Hex(0xA7B2C8), Hex(0x6E8EB4), Hex(0x13306E), Hex(0x1C5DA6), Hex(0x2490CC), Hex(0x4DB4CF), Hex(0x7FD8E8)};
        break;
    default:  // gloss #c0cae2 -> #85a8cc; body #1c357a -> #2685d0 -> #3fb9e8 -> #5fcde5; rim #99f4ff
        p = {Hex(0xC3CDE4), Hex(0x86A6CB), Hex(0x1C3A80), Hex(0x2878C8), Hex(0x34AEE9), Hex(0x6AD3E8), Hex(0x99F4FF)};
        break;
    }
    const float ri = r - L;
    const float top = c.y - ri, bot = c.y + ri;
    auto body = Vertical(dc, top, bot,
                         {{0.00f, p.glossTop},
                          {0.44f, p.glossBottom},
                          {0.47f, p.horizon},
                          {0.70f, p.lowMid},
                          {0.88f, p.low},
                          {1.00f, p.bottom}});
    dc->FillEllipse({c, ri, ri}, body.get());
    // Edge darkening: the sphere is navy (#0d3277) at its left/right edges.
    auto edge = Radial(dc, {c.x, c.y + ri * 0.15f}, ri * 1.05f, ri * 1.15f,
                       {{0.0f, Rgba(8, 30, 90, 0.0f)}, {0.72f, Rgba(8, 30, 90, 0.0f)}, {1.0f, Rgba(8, 30, 90, 0.55f)}});
    dc->FillEllipse({c, ri, ri}, edge.get());
    // Bright cyan rim along the bottom.
    auto rim = Vertical(dc, top, bot, {{0.0f, WithAlpha(p.rim, 0.0f)}, {0.6f, WithAlpha(p.rim, 0.0f)},
                                       {1.0f, WithAlpha(p.rim, 0.95f)}});
    dc->DrawEllipse({c, ri - L / 2, ri - L / 2}, rim.get(), L);

    // Glyph: white with a faint shadow; scaled with the orb (36 DIP reference).
    const float u = (2 * r) / 36.0f;
    auto shape = FilledGlyph(f.get(), g, glyph, {c.x, c.y}, u);
    auto shade = Solid(dc, Rgba(10, 30, 70, 0.30f));
    dc->DrawGeometry(shape.get(), shade.get(), L);
    auto white = Solid(dc, Rgba(255, 255, 255));
    dc->FillGeometry(shape.get(), white.get());
}

void OrbHalo(ID2D1DeviceContext* dc, float size) {
    const float r = size / 2;
    auto halo = Radial(dc, {r, r}, r, r,
                       {{0.0f, Rgba(120, 214, 255, 0.0f)},
                        {0.52f, Rgba(120, 214, 255, 0.0f)},
                        {0.60f, Rgba(110, 205, 255, 0.50f)},
                        {0.78f, Rgba(80, 180, 255, 0.15f)},
                        {1.0f, Rgba(40, 140, 255, 0.0f)}});
    dc->FillEllipse({{r, r}, r, r}, halo.get());
}

void Segment(ID2D1DeviceContext* dc, D2D1_SIZE_F size, bool left, Layer layer, TransportGlyph glyph, bool enabled) {
    const Grid g = GridOf(dc);
    auto f = FactoryOf(dc);
    const D2D1_RECT_F rc = g.Snap(D2D1_RECT_F{0, 0, size.width, size.height});
    const float r = 4.0f;
    const Radii rad = left ? Radii{r, 0, 0, r} : Radii{0, r, r, 0};

    D2D1_COLOR_F uTop, uBot, lTop, lBot, glow{};
    switch (layer) {
    case Layer::Hover:
        uTop = Hex(0xF6FCFF), uBot = Hex(0xDDEEFC), lTop = Hex(0x9CCBF1), lBot = Hex(0xC4E9FF);
        glow = Rgba(170, 240, 255, 0.85f);
        break;
    case Layer::Pressed:
        uTop = Hex(0xD3E2EF), uBot = Hex(0xC0D3E7), lTop = Hex(0x8BA9C8), lBot = Hex(0xA9C5E1);
        glow = Rgba(110, 200, 255, 0.35f);
        break;
    default:  // measured: upper #e5f2f9/#ebf7ff -> #d4e3f5, lower #b1c2d3 -> #d2e2ef
        uTop = Hex(0xE9F6FD), uBot = Hex(0xD4E3F5), lTop = Hex(0xB1C2D3), lBot = Hex(0xD2E2EF);
        break;
    }
    auto body = Inner(f.get(), g, rc, rad, 1);
    const float mid = g.Snap(rc.top + (rc.bottom - rc.top) * 0.5f);
    Clipped(dc, body.get(), [&] {
        dc->FillRectangle({rc.left, rc.top, rc.right, mid},
                          Vertical(dc, rc.top, mid, {{0.0f, uTop}, {1.0f, uBot}}).get());
        dc->FillRectangle({rc.left, mid, rc.right, rc.bottom},
                          Vertical(dc, mid, rc.bottom, {{0.0f, lTop}, {1.0f, lBot}}).get());
        if (glow.a > 0) {
            auto gl = Radial(dc, {left ? rc.right * 0.4f : rc.right * 0.6f, rc.bottom}, size.width * 0.5f,
                             size.height * 0.6f, {{0.0f, glow}, {1.0f, WithAlpha(glow, 0.0f)}});
            dc->FillRectangle({rc.left, mid, rc.right, rc.bottom}, gl.get());
        }
    });
    auto rim = Vertical(dc, rc.top, rc.bottom, {{0.0f, Hex(0xF4FAFE)}, {0.5f, Rgba(255, 255, 255, 0.35f)},
                                                {1.0f, Rgba(255, 255, 255, 0.45f)}});
    dc->DrawGeometry(Ring(f.get(), g, rc, rad, 1).get(), rim.get(), g.L);
    auto outline = Vertical(dc, rc.top, rc.bottom, {{0.0f, Hex(0xA3AFBB)}, {1.0f, Hex(0x6F7D8B)}});
    dc->DrawGeometry(Ring(f.get(), g, rc, rad, 0).get(), outline.get(), g.L);

    // Glyph centered on the visible part (the inner 19 DIP sit under the orb).
    const float visible = size.width - 19.0f;
    const D2D1_POINT_2F c{g.Snap(left ? visible / 2 : size.width - visible / 2), g.Snap(size.height / 2)};
    auto shape = FilledGlyph(f.get(), g, glyph, c, 1.0f);
    auto emboss = Solid(dc, Rgba(255, 255, 255, 0.55f));
    D2D1_MATRIX_3X2_F saved;
    dc->GetTransform(&saved);
    dc->SetTransform(D2D1::Matrix3x2F::Translation(0, g.L) * saved);
    dc->FillGeometry(shape.get(), emboss.get());
    dc->SetTransform(saved);
    dc->FillGeometry(shape.get(), GlyphBrush(dc, c.y - 5, c.y + 5, enabled, layer).get());
}

void IconButton(ID2D1DeviceContext* dc, D2D1_SIZE_F size, Layer layer, TransportGlyph glyph, bool enabled,
                bool checked) {
    const Grid g = GridOf(dc);
    auto f = FactoryOf(dc);
    const D2D1_RECT_F rc = g.Snap(D2D1_RECT_F{0, 0, size.width, size.height});
    const Radii rad{3, 3, 3, 3};
    if (layer == Layer::Hover || layer == Layer::Pressed || checked) {
        const bool pressed = layer == Layer::Pressed;
        auto fill = Vertical(dc, rc.top, rc.bottom,
                             pressed ? std::initializer_list<Stop>{{0.0f, Hex(0xC9DBEC)}, {1.0f, Hex(0xB2CCE4)}}
                                     : std::initializer_list<Stop>{{0.0f, Hex(0xFBFDFF)}, {0.5f, Hex(0xEAF3FC)},
                                                                   {0.5f, Hex(0xDCEBF9)}, {1.0f, Hex(0xE9F4FE)}});
        dc->FillGeometry(Inner(f.get(), g, rc, rad, 0).get(), fill.get());
        auto o = Solid(dc, checked && layer == Layer::Normal ? Hex(0xA9C1D9) : Hex(0x8FA9C3));
        dc->DrawGeometry(Ring(f.get(), g, rc, rad, 0).get(), o.get(), g.L);
    }
    const D2D1_POINT_2F c{g.Snap(size.width / 2), g.Snap(size.height / 2)};
    auto brush = GlyphBrush(dc, c.y - 6, c.y + 6, enabled, layer);
    if (glyph == TransportGlyph::Stop) {
        auto shape = FilledGlyph(f.get(), g, glyph, c, 1.0f);
        dc->FillGeometry(shape.get(), brush.get());
    } else {
        LineGlyph(dc, glyph, c, brush.get());
    }
}

void CapsuleDivider(ID2D1DeviceContext* dc, D2D1_SIZE_F size) {
    const Grid g = GridOf(dc);
    const float x = g.Snap(size.width / 2);
    dc->FillRectangle({x - g.L, 0, x, size.height}, Solid(dc, Hex(0xB2BCC7)).get());
    dc->FillRectangle({x, 0, x + g.L, size.height}, Solid(dc, Rgba(255, 255, 255, 0.85f)).get());
}

void VolumeSlider(ID2D1DeviceContext* dc, D2D1_SIZE_F size, float value, Layer layer) {
    const Grid g = GridOf(dc);
    const float thumb = 11.0f;
    const float x0 = thumb / 2, x1 = size.width - thumb / 2;
    const float y = g.Snap(size.height / 2);
    const float tx = x0 + (x1 - x0) * std::clamp(value, 0.0f, 1.0f);
    // Track: blue left of the thumb, grey right of it, with a light line underneath.
    dc->FillRectangle({x0, y - g.L, x1, y + g.L}, Solid(dc, Hex(0xB9C4D0)).get());
    dc->FillRectangle({x0, y + g.L, x1, y + 2 * g.L}, Solid(dc, Rgba(255, 255, 255, 0.9f)).get());
    dc->FillRectangle({x0, y - g.L, tx, y + g.L},
                      Vertical(dc, y - g.L, y + g.L, {{0.0f, Hex(0x4C93DF)}, {1.0f, Hex(0x2163B8)}}).get());
    // Thumb: a miniature of the play orb.
    const D2D1_POINT_2F c{tx, y};
    const float r = thumb / 2;
    dc->FillEllipse({c, r, r}, Vertical(dc, y - r, y + r, {{0.0f, Hex(0x6F7F9A)}, {1.0f, Hex(0x3F7486)}}).get());
    const bool hot = layer == Layer::Hover || layer == Layer::Pressed;
    dc->FillEllipse({c, r - g.L, r - g.L},
                    Vertical(dc, y - r, y + r,
                             {{0.0f, hot ? Hex(0xE2EAF6) : Hex(0xC3CDE4)},
                              {0.45f, hot ? Hex(0xA6C4E6) : Hex(0x86A6CB)},
                              {0.5f, Hex(0x1C3A80)},
                              {1.0f, hot ? Hex(0x5ED8F5) : Hex(0x34AEE9)}})
                        .get());
}

void Icon(ID2D1DeviceContext* dc, IWICBitmapSource* icon, D2D1_RECT_F dest) {
    if (!icon) return;
    com_ptr<ID2D1Bitmap> bmp;
    if (SUCCEEDED(dc->CreateBitmapFromWicBitmap(icon, nullptr, bmp.put())))
        dc->DrawBitmap(bmp.get(), dest, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
}

}  // namespace glasswave::aero::paint
