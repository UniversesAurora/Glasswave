#include "pch.h"
#include "ui/player_window_parts.h"

// The player window's controls: the transport, menu and caption widgets with their
// artwork, the hover, the menu's press effect.
namespace glasswave::ui {

using namespace playerwin;

namespace {

constexpr int kSwitchToLibrary = 4603; // wmploc: the "switch to library" corner glyph, 25x25

// `area` is the plate; its height is 25 units.
void WmpPlate(ID2D1DeviceContext* dc, D2D1_RECT_F area, bool pressed) {
    const auto& cols = pressed ? kPlatePressed : kPlateHot;
    const float k = (area.bottom - area.top) / 25.0f;
    // Rows that start a band of their own: the rim, the frame, the horizon, the bright foot.
    static constexpr int kBreaks[] = {1, 2, 12, 23, 24};
    const auto brush = [&](const PlateColumn (&col)[25]) {
        std::vector<D2D1_GRADIENT_STOP> stops;
        const auto at = [&](int row, float y) {
            stops.push_back({y / 25.0f, Hex(col[row].rgb, col[row].alpha / 255.0f)});
        };
        for (int row = 0; row < 25; ++row) {
            const bool opens = std::find(std::begin(kBreaks), std::end(kBreaks), row) != std::end(kBreaks);
            const bool closes = std::find(std::begin(kBreaks), std::end(kBreaks), row + 1) != std::end(kBreaks);
            if (opens) at(row, static_cast<float>(row) + 0.001f);
            at(row, row + 0.5f);
            if (closes) at(row, row + 0.999f);
        }
        com_ptr<ID2D1LinearGradientBrush> b;
        dc->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({0, area.top}, {0, area.bottom}),
                                      Stops(dc, stops).get(), b.put());
        return b;
    };
    com_ptr<ID2D1Factory> factory;
    dc->GetFactory(factory.put());
    com_ptr<ID2D1RoundedRectangleGeometry> shape;
    factory->CreateRoundedRectangleGeometry({area, 2.2f * k, 2.2f * k}, shape.put());
    dc->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), shape.get()), nullptr);
    const float L = area.left, R = area.right;
    for (int i = 0; i < 3; ++i) { // rim, frame and the column inside it, on both sides
        const auto b = brush(cols[i]);
        dc->PushAxisAlignedClip({L + i * k, area.top, L + (i + 1) * k, area.bottom}, D2D1_ANTIALIAS_MODE_ALIASED);
        dc->FillRectangle(area, b.get());
        dc->PopAxisAlignedClip();
        dc->PushAxisAlignedClip({R - (i + 1) * k, area.top, R - i * k, area.bottom}, D2D1_ANTIALIAS_MODE_ALIASED);
        dc->FillRectangle(area, b.get());
        dc->PopAxisAlignedClip();
    }
    dc->PushAxisAlignedClip({L + 3 * k, area.top, R - 3 * k, area.bottom}, D2D1_ANTIALIAS_MODE_ALIASED);
    dc->FillRectangle(area, brush(cols[3]).get());
    dc->PopAxisAlignedClip();
    dc->PopLayer();
}

// A Media Center windowed-mode control (ehres TWOFOOT.CONTROL.<name>.<state>) filling `s`.
bool DrawControlAt(ID2D1DeviceContext* dc, D2D1_RECT_F dst, const wchar_t* control, Layer layer,
                   bool enabled = true) {
    const wchar_t* state = !enabled                  ? L"DISABLED"
                           : layer == Layer::Hover   ? L"FOCUS"
                           : layer == Layer::Pressed ? L"PRESSED"
                                                     : L"NOFOCUS";
    std::wstring name = std::wstring(L"TWOFOOT.CONTROL.") + control + L"." + state + L".PNG";
    if (!win7::Resources::Get().Has(kEhres, name)) // not every control has every state
        name = std::wstring(L"TWOFOOT.CONTROL.") + control + L".NOFOCUS.PNG";
    return win7::DrawStretched(dc, kEhres, name, dst);
}

bool DrawControl(ID2D1DeviceContext* dc, D2D1_SIZE_F s, const wchar_t* control, Layer layer, bool enabled = true) {
    return DrawControlAt(dc, {0, 0, s.width, s.height}, control, layer, enabled);
}

// The plate of a Media Center windowed control without its glyph, for buttons the artwork has
// no image of: TWOFOOT.CONTROL.PLUS's focus frame or pressed glow, its glyph-free columns at
// either side kept and column 10 (frame and glow only) stretched between them.
bool ControlPlate(ID2D1DeviceContext* dc, D2D1_RECT_F dst, Layer layer) {
    if (layer != Layer::Hover && layer != Layer::Pressed) return true;
    const wchar_t* name = layer == Layer::Pressed ? L"TWOFOOT.CONTROL.PLUS.PRESSED.PNG" : L"TWOFOOT.CONTROL.PLUS.FOCUS.PNG";
    // A plate narrower than its two sides squeezes them (the arrow's plate beside the speaker).
    const float side = std::min(10 * (dst.bottom - dst.top) / 31, (dst.right - dst.left) / 2);
    if (!win7::DrawPartTo(dc, kEhres, name, {0, 0, 10, 31}, {dst.left, dst.top, dst.left + side, dst.bottom}) ||
        !win7::DrawPartTo(dc, kEhres, name, {28, 0, 38, 31}, {dst.right - side, dst.top, dst.right, dst.bottom}))
        return false;
    if (dst.right - dst.left > 2 * side)
        win7::DrawPartTo(dc, kEhres, name, {10, 0, 11, 31}, {dst.left + side, dst.top, dst.right - side, dst.bottom});
    return true;
}

// A heart in the manner of Media Center's control glyphs (TWOFOOT.CONTROL.*: a navy rim, a
// face lit from above, a soft shadow below), centred in a 38 x 31 control scaled to `s`.
// Filled for a favourite, an outline otherwise.
void HeartGlyph(ID2D1DeviceContext* dc, D2D1_SIZE_F s, bool filled) {
    const float k = s.height / 31.0f;
    const float x0 = s.width / 2 - 8 * k, y0 = s.height / 2 - 7.4f * k;
    com_ptr<ID2D1Factory> factory;
    dc->GetFactory(factory.put());
    const auto heart = [&](float dx, float dy) {
        com_ptr<ID2D1PathGeometry> path;
        com_ptr<ID2D1GeometrySink> sink;
        if (FAILED(factory->CreatePathGeometry(path.put())) || FAILED(path->Open(sink.put()))) return path;
        const auto p = [&](float x, float y) { return D2D1::Point2F(x0 + dx + x * k, y0 + dy + y * k); };
        sink->BeginFigure(p(8, 14.6f), D2D1_FIGURE_BEGIN_FILLED);
        sink->AddBezier({p(3.2f, 11.0f), p(0, 8.1f), p(0, 4.5f)});
        sink->AddBezier({p(0, 1.9f), p(1.9f, 0), p(4.3f, 0)});
        sink->AddBezier({p(5.9f, 0), p(7.3f, 0.9f), p(8, 2.3f)});
        sink->AddBezier({p(8.7f, 0.9f), p(10.1f, 0), p(11.7f, 0)});
        sink->AddBezier({p(14.1f, 0), p(16, 1.9f), p(16, 4.5f)});
        sink->AddBezier({p(16, 8.1f), p(12.8f, 11.0f), p(8, 14.6f)});
        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
        sink->Close();
        return path;
    };
    // Measured on TWOFOOT.CONTROL.PLUS: rim #063E67, face #E3EDF1 at the top to #C5D9E2.
    com_ptr<ID2D1SolidColorBrush> rim, shadow;
    dc->CreateSolidColorBrush(D2D1::ColorF(0x063E67), rim.put());
    dc->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 0.30f), shadow.put());
    const D2D1_GRADIENT_STOP stops[] = {{0, D2D1::ColorF(0xE6EFF3)}, {0.55f, D2D1::ColorF(0xD3E2E9)},
                                        {1, D2D1::ColorF(0xAFC8D7)}};
    com_ptr<ID2D1GradientStopCollection> collection;
    dc->CreateGradientStopCollection(stops, 3, collection.put());
    com_ptr<ID2D1LinearGradientBrush> face;
    dc->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({0, y0}, {0, y0 + 14.6f * k}), collection.get(),
                                  face.put());
    const auto shape = heart(0, 0), drop = heart(0.4f * k, 1.4f * k);
    if (!shape || !drop) return;
    if (filled) {
        dc->FillGeometry(drop.get(), shadow.get());
        dc->DrawGeometry(shape.get(), rim.get(), 2.0f * k);
        dc->FillGeometry(shape.get(), face.get());
    } else {
        dc->DrawGeometry(drop.get(), shadow.get(), 4.2f * k);
        dc->DrawGeometry(shape.get(), rim.get(), 4.4f * k);
        dc->DrawGeometry(shape.get(), face.get(), 2.2f * k);
    }
}

// Runs `paint` with the origin moved so that a w x h box (a Media Center control's bitmap, in
// DIP) sits centred in `s`.
void InBox(ID2D1DeviceContext* dc, D2D1_SIZE_F s, float w, float h, const std::function<void()>& paint) {
    D2D1_MATRIX_3X2_F base;
    dc->GetTransform(&base);
    dc->SetTransform(D2D1::Matrix3x2F::Translation((s.width - w) / 2, (s.height - h) / 2) * base);
    paint();
    dc->SetTransform(base);
}

com_ptr<ID2D1Geometry> UnionOf(ID2D1Factory* factory, ID2D1Geometry* a, ID2D1Geometry* b) {
    com_ptr<ID2D1PathGeometry> path;
    com_ptr<ID2D1GeometrySink> sink;
    if (FAILED(factory->CreatePathGeometry(path.put())) || FAILED(path->Open(sink.put()))) return nullptr;
    a->CombineWithGeometry(b, D2D1_COMBINE_MODE_UNION, nullptr, sink.get());
    sink->Close();
    return path;
}

// Shuffle and repeat in Media Center's glyph style, as the heart (navy rim, a face lit from
// above, a soft shadow), in WMP 12's shapes (wmploc 4175 two arrows crossing upwards, 4177 a
// circle turning back on itself; repeating one song: a 1 inside it). A 16x14 glyph centred in
// a 38x31 box at (0, 0), one unit a DIP. dislike: a station's "recommend less" in shuffle's
// place (a heart's outline struck through); grey: the button does nothing (a station's shuffle):
// a grey rim and face, a fainter shadow.
void PlayModeGlyph(ID2D1DeviceContext* dc, bool shuffle, int repeat, bool dislike = false, bool grey = false) {
    com_ptr<ID2D1Factory> factory;
    dc->GetFactory(factory.put());
    const float x0 = 19 - 8, y0 = 15.5f - 7;
    const auto P = [&](float x, float y) { return D2D1::Point2F(x0 + x, y0 + y); };
    const auto stroke = [&](const std::function<void(ID2D1GeometrySink*)>& figure, float width) {
        com_ptr<ID2D1PathGeometry> path, wide;
        com_ptr<ID2D1GeometrySink> sink, wideSink;
        factory->CreatePathGeometry(path.put());
        path->Open(sink.put());
        figure(sink.get());
        sink->Close();
        factory->CreatePathGeometry(wide.put());
        wide->Open(wideSink.put());
        path->Widen(width, nullptr, nullptr, D2D1_DEFAULT_FLATTENING_TOLERANCE, wideSink.get());
        wideSink->Close();
        return com_ptr<ID2D1Geometry>(wide);
    };
    const auto triangle = [&](D2D1_POINT_2F a, D2D1_POINT_2F b, D2D1_POINT_2F c) { // a solid head
        com_ptr<ID2D1PathGeometry> path;
        com_ptr<ID2D1GeometrySink> sink;
        factory->CreatePathGeometry(path.put());
        path->Open(sink.put());
        sink->BeginFigure(a, D2D1_FIGURE_BEGIN_FILLED);
        sink->AddLine(b);
        sink->AddLine(c);
        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
        sink->Close();
        return com_ptr<ID2D1Geometry>(path);
    };
    std::vector<com_ptr<ID2D1Geometry>> parts;
    if (dislike) {
        parts.push_back(stroke([&](ID2D1GeometrySink* s) {
            s->BeginFigure(P(8.0f, 13.2f), D2D1_FIGURE_BEGIN_HOLLOW);
            s->AddBezier({P(5.6f, 11.4f), P(2.0f, 8.9f), P(2.0f, 5.6f)});
            s->AddBezier({P(2.0f, 3.4f), P(3.7f, 1.8f), P(5.6f, 1.8f)});
            s->AddBezier({P(6.8f, 1.8f), P(7.6f, 2.5f), P(8.0f, 3.4f)});
            s->AddBezier({P(8.4f, 2.5f), P(9.2f, 1.8f), P(10.4f, 1.8f)});
            s->AddBezier({P(12.3f, 1.8f), P(14.0f, 3.4f), P(14.0f, 5.6f)});
            s->AddBezier({P(14.0f, 8.9f), P(10.4f, 11.4f), P(8.0f, 13.2f)});
            s->EndFigure(D2D1_FIGURE_END_CLOSED);
        }, 2.0f));
        parts.push_back(stroke([&](ID2D1GeometrySink* s) {
            s->BeginFigure(P(14.6f, 0.2f), D2D1_FIGURE_BEGIN_HOLLOW);
            s->AddLine(P(1.4f, 13.8f));
            s->EndFigure(D2D1_FIGURE_END_OPEN);
        }, 2.0f));
    } else if (shuffle) {
        for (const float m : {1.0f, -1.0f}) { // the arrow to the top left, then its mirror
            const auto X = [&](float x) { return m > 0 ? x : 16 - x; };
            parts.push_back(stroke([&](ID2D1GeometrySink* s) {
                s->BeginFigure(P(X(14.2f), 13.4f), D2D1_FIGURE_BEGIN_HOLLOW);
                s->AddBezier({P(X(8.6f), 12.6f), P(X(3.4f), 9.6f), P(X(3.2f), 4.2f)});
                s->EndFigure(D2D1_FIGURE_END_OPEN);
            }, 2.3f));
            parts.push_back(triangle(P(X(0.3f), 4.6f), P(X(6.1f), 4.6f), P(X(3.2f), 0.4f)));
        }
    } else {
        const D2D1_POINT_2F c = P(8, 7.6f);
        constexpr float r = 5.4f, from = -1.15f, to = 3.75f; // radians, clockwise on the screen
        const auto on = [&](float a, float rr) { return D2D1::Point2F(c.x + rr * std::cos(a), c.y + rr * std::sin(a)); };
        parts.push_back(stroke([&](ID2D1GeometrySink* s) {
            s->BeginFigure(on(from, r), D2D1_FIGURE_BEGIN_HOLLOW);
            s->AddArc({on(to, r), {r, r}, 0, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_LARGE});
            s->EndFigure(D2D1_FIGURE_END_OPEN);
        }, 2.3f));
        // The head at the arc's end, pointing on round the circle.
        const D2D1_POINT_2F e = on(to, r);
        const D2D1_POINT_2F t{-std::sin(to), std::cos(to)}, n{std::cos(to), std::sin(to)};
        parts.push_back(triangle({e.x - t.x * 0.4f + n.x * 2.9f, e.y - t.y * 0.4f + n.y * 2.9f},
                                 {e.x - t.x * 0.4f - n.x * 2.9f, e.y - t.y * 0.4f - n.y * 2.9f},
                                 {e.x + t.x * 3.4f, e.y + t.y * 3.4f}));
        if (repeat == 2)
            parts.push_back(stroke([&](ID2D1GeometrySink* s) {
                s->BeginFigure(P(7.0f, 6.4f), D2D1_FIGURE_BEGIN_HOLLOW);
                s->AddLine(P(8.4f, 5.4f));
                s->AddLine(P(8.4f, 10.2f));
                s->EndFigure(D2D1_FIGURE_END_OPEN);
            }, 1.5f));
    }
    com_ptr<ID2D1Geometry> glyph = parts.front();
    for (size_t i = 1; i < parts.size() && glyph; ++i) glyph = UnionOf(factory.get(), glyph.get(), parts[i].get());
    if (!glyph) return;
    // As the heart: rim #063E67, face #E6EFF3 to #AFC8D7, a shadow below.
    com_ptr<ID2D1SolidColorBrush> rim, shadow;
    dc->CreateSolidColorBrush(D2D1::ColorF(grey ? 0x5E666C : 0x063E67), rim.put());
    dc->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, grey ? 0.15f : 0.30f), shadow.put());
    const D2D1_GRADIENT_STOP stops[] = {{0, D2D1::ColorF(grey ? 0xB9BEC2 : 0xE6EFF3)},
                                        {0.55f, D2D1::ColorF(grey ? 0xA9AFB4 : 0xD3E2E9)},
                                        {1, D2D1::ColorF(grey ? 0x92999F : 0xAFC8D7)}};
    com_ptr<ID2D1GradientStopCollection> collection;
    dc->CreateGradientStopCollection(stops, 3, collection.put());
    com_ptr<ID2D1LinearGradientBrush> face;
    dc->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(P(0, 0), P(0, 14)), collection.get(), face.put());
    D2D1_MATRIX_3X2_F base;
    dc->GetTransform(&base);
    dc->SetTransform(D2D1::Matrix3x2F::Translation(0.4f, 1.4f) * base);
    dc->DrawGeometry(glyph.get(), shadow.get(), 2.0f);
    dc->FillGeometry(glyph.get(), shadow.get());
    dc->SetTransform(base);
    dc->DrawGeometry(glyph.get(), rim.get(), 2.0f);
    dc->FillGeometry(glyph.get(), face.get());
}

// The mini window's small buttons (speaker excepted): WMP 12's square plate, lit under the
// pointer (4725) and pressed (4724), nothing at rest; the glyph over it. A button that is on
// (shuffle, repeat, the pane's side that shows) glows as WMP's toggles do (4174 / 4176 against
// 4175 / 4177): a cyan #63CFFC disc behind the glyph, solid to about 5 DIP from its centre and
// fading out by 12 (alpha 0.69 at 6.5, 0.42 at 8, 0.20 at 9.5, 0.07 at 11).
void PlateButton(ID2D1DeviceContext* dc, D2D1_SIZE_F s, Layer layer, bool on, const std::function<void()>& glyph) {
    if (layer == Layer::Hover)
        WmpPlate(dc, {0, 0, s.width, s.height}, false);
    else if (layer == Layer::Pressed)
        WmpPlate(dc, {0, 0, s.width, s.height}, true);
    if (on) {
        const D2D1_GRADIENT_STOP stops[] = {{0.00f, D2D1::ColorF(0x63CFFC, 1.00f)}, {0.42f, D2D1::ColorF(0x63CFFC, 0.95f)},
                                            {0.54f, D2D1::ColorF(0x63CFFC, 0.69f)}, {0.67f, D2D1::ColorF(0x63CFFC, 0.42f)},
                                            {0.79f, D2D1::ColorF(0x63CFFC, 0.20f)}, {0.92f, D2D1::ColorF(0x63CFFC, 0.07f)},
                                            {1.00f, D2D1::ColorF(0x63CFFC, 0.00f)}};
        com_ptr<ID2D1GradientStopCollection> collection;
        dc->CreateGradientStopCollection(stops, 7, collection.put());
        com_ptr<ID2D1RadialGradientBrush> glow;
        const D2D1_POINT_2F c{s.width / 2, s.height / 2};
        dc->CreateRadialGradientBrush(D2D1::RadialGradientBrushProperties(c, {0, 0}, 12, 12), collection.get(),
                                      glow.put());
        dc->FillEllipse(D2D1::Ellipse(c, 12, 12), glow.get());
    }
    glyph();
}

// Media Center's menu type, laid out the same for the item and for its ink on a press. In
// units of the 51-unit button (misc.mp4): 28 units, from 16 units in, centred.
constexpr float kMenuTextX = 16, kMenuTextSize = 28;

void MenuLabel(ID2D1DeviceContext* dc, D2D1_SIZE_F s, const std::wstring& text, ID2D1Brush* ink,
               ID2D1Brush* shadow) {
    const float k = s.height / 51.0f, size = kMenuTextSize * k;
    const D2D1_POINT_2F at{kMenuTextX * k, (s.height - size * 1.33f) / 2};
    const float width = s.width - (kMenuTextX + 42) * k; // up to the check mark
    if (shadow) pane::DrawTextLine(dc, text, size, DWRITE_FONT_WEIGHT_NORMAL, {at.x, at.y + 1.2f * k}, width, shadow);
    pane::DrawTextLine(dc, text, size, DWRITE_FONT_WEIGHT_NORMAL, at, width, ink);
}

// Fallback plate for the buttons on the dark bar when the original artwork is missing.
void DarkPlate(ID2D1DeviceContext* dc, D2D1_SIZE_F s, Layer layer, bool checked) {
    const float alpha = layer == Layer::Pressed ? 0.10f : layer == Layer::Hover ? 0.20f : checked ? 0.16f : 0.0f;
    if (alpha <= 0) return;
    const D2D1_ROUNDED_RECT plate{{0.5f, 0.5f, s.width - 0.5f, s.height - 0.5f}, 3, 3};
    dc->FillRoundedRectangle(plate, pane::White(dc, alpha).get());
    dc->DrawRoundedRectangle(plate, pane::White(dc, alpha + 0.12f).get(), 1.0f);
}

// A Media Center bitmap as a stretching brush.
wuc::CompositionSurfaceBrush ArtBrush(const wchar_t* resource) {
    auto& host = gfx::CompositionHost::Get();
    const D2D1_SIZE_U size = win7::Resources::Get().Size(kEhres, resource);
    if (size.width == 0 || size.height == 0) return nullptr;
    const auto surface = host.CreateSurface(static_cast<int>(size.width) * 2, static_cast<int>(size.height) * 2);
    host.Draw(surface, 2.0f, [&](ID2D1DeviceContext* dc) {
        win7::DrawStretched(dc, kEhres, resource,
                            {0, 0, static_cast<float>(size.width), static_cast<float>(size.height)});
    });
    auto brush = host.Compositor().CreateSurfaceBrush(surface);
    brush.Stretch(wuc::CompositionStretch::Fill);
    return brush;
}

} // namespace

void PlayerWindow::CreateWidgets() {
    auto add = [&](int id, const wuc::ContainerVisual& layer, Widget::Painter painter,
                   Widget::Options opts) -> Widget* {
        auto w = std::make_unique<Widget>(id, std::move(painter), std::move(opts));
        layer.Children().InsertAtTop(w->Visual());
        m_widgets.push_back(std::move(w));
        return m_widgets.back().get();
    };

    // Transport: Media Center's windowed controls when full screen, WMP 12's cluster in the
    // mini window, procedural painting where the user's files have neither.
    add(kStop, m_barLayer,
        [this](auto* dc, D2D1_SIZE_F s, Layer l) {
            if (Wmc())
                DrawControl(dc, s, L"STOP", l, m_playing);
            else if (m_ehres) // the mini window's plate, Media Center's glyph
                PlateButton(dc, s, l, false, [&] {
                    InBox(dc, s, 38, 31, [&] { DrawControlAt(dc, {0, 0, 38, 31}, L"STOP", Layer::Normal, m_playing); });
                });
            else if (m_wmp)
                wt::ClusterButton(dc, ClusterRegion(m_layout.stop), wt::Button::Stop, l, m_playing);
            else
                paint::IconButton(dc, s, l, TransportGlyph::Stop, m_playing, false);
        },
        {})
        ->onClick = [] { media::transport::Stop(); };
    add(kPrev, m_barLayer,
        [this](auto* dc, D2D1_SIZE_F s, Layer l) {
            const bool enabled = m_playing && media::radio::CanGoBack();
            if (Wmc())
                DrawControl(dc, s, L"PREVIOUS", l, enabled);
            else if (m_wmp)
                wt::ClusterButton(dc, ClusterRegion(m_layout.prev), wt::Button::Prev, l, enabled);
            else
                paint::Segment(dc, s, true, l, TransportGlyph::Previous, enabled);
        },
        {})
        ->onClick = [] { media::transport::Previous(); };
    add(kNext, m_barLayer,
        [this](auto* dc, D2D1_SIZE_F s, Layer l) {
            if (Wmc())
                DrawControl(dc, s, L"NEXT", l, m_playing);
            else if (m_wmp)
                wt::ClusterButton(dc, ClusterRegion(m_layout.next), wt::Button::Next, l, m_playing);
            else
                paint::Segment(dc, s, false, l, TransportGlyph::Next, true);
        },
        {})
        ->onClick = [] { media::transport::Next(); };
    for (int id : {kStop, kNext}) WidgetById(id)->enabled = [this] { return m_playing; };
    WidgetById(kPrev)->enabled = [this] { return m_playing && media::radio::CanGoBack(); };

    Widget::Options orb;
    orb.circular = true;
    orb.haloPulse = !m_wmp;
    orb.haloFactor = m_wmp ? wt::kGlowFactor : 1.6f;
    orb.halo = [this](ID2D1DeviceContext* dc, float size) {
        if (Wmc()) return; // the FOCUS image carries its own glow
        if (m_wmp)
            wt::GlowRing(dc, size);
        else
            paint::OrbHalo(dc, size);
    };
    add(kPlay, m_barLayer,
        [this](auto* dc, D2D1_SIZE_F s, Layer l) {
            const bool showPause = m_playing && !m_paused;
            if (Wmc())
                DrawControl(dc, s, showPause ? L"PAUSE" : L"PLAY", l);
            else if (m_wmp)
                wt::Orb(dc, l, showPause);
            else
                paint::PlayOrb(dc, s.width, l, showPause ? TransportGlyph::Pause : TransportGlyph::Play);
        },
        orb)
        ->onClick = [] { media::transport::PlayPause(); };

    // The speaker. In a narrow mini window it carries the arrow that opens the volume flyout.
    add(kMute, m_barLayer,
        [this](auto* dc, D2D1_SIZE_F s, Layer l) {
            if (m_splitVolume) {
                // WMP 12's narrow transport: its split button (wmploc, 39x25), the speaker's
                // half (25) here, the arrow's (14) its own widget beside it, so that each half
                // fades in and out by itself. Under the pointer the half lights (4628) over the
                // plain plate both share (4630, m_splitPlate); pressed it glows (4627).
                if (l != Layer::Normal)
                    win7::DrawPartTo(dc, kWmploc, l == Layer::Pressed ? L"#4627" : L"#4628", {0, 0, 25, 25},
                                     {0, 0, s.width, s.height});
                const int speaker = m_muted ? wt::kMuted : m_volume <= 0.0f ? wt::kVolume0 : m_volume < 0.67f ? wt::kVolume1
                                                                                                                : wt::kVolume2;
                win7::DrawStretched(dc, kWmploc, L"#" + std::to_wstring(speaker), {0, 0, s.width, s.height});
                return;
            }
            if (m_ehres && (Wmc() || !m_wmp)) {
                const float k = s.height / 31.0f, speaker = 38 * k;
                if (s.width <= speaker + 1) {
                    DrawControl(dc, s, m_muted ? L"MUTEON" : L"MUTE", l);
                    return;
                }
                // WMP 12's split button: the speaker's plate and the arrow's narrow one side by
                // side, a hair apart, lit together.
                const Layer lit = m_popupOpen && l == Layer::Normal ? Layer::Hover : l;
                const float split = s.width - 15 * k;
                ControlPlate(dc, {0, 0, split - 1 * k, s.height}, lit);
                ControlPlate(dc, {split, 0, s.width, s.height}, lit);
                win7::DrawStretched(dc, kEhres, m_muted ? L"TWOFOOT.CONTROL.MUTEON.NOFOCUS.PNG"
                                                        : L"TWOFOOT.CONTROL.MUTE.NOFOCUS.PNG",
                                    {(split - 1 * k - speaker) / 2, 0, (split - 1 * k + speaker) / 2, s.height});
                const float ax = (split + s.width) / 2 - 8 * k;
                win7::DrawStretched(dc, kEhres, L"TWOFOOT.CONTROL.DOWN.NOFOCUS.PNG",
                                    {ax, (s.height - 16 * k) / 2, ax + 16 * k, (s.height + 16 * k) / 2});
            } else if (m_wmp) {
                wt::Mute(dc, l, m_muted, m_volume);
            } else {
                DarkPlate(dc, s, l, false);
                const wchar_t glyph = m_muted ? L'\xE74F' : m_volume < 0.34f ? L'\xE993'
                                                        : m_volume < 0.67f ? L'\xE994'
                                                                           : L'\xE995';
                pane::DrawIcon(dc, glyph, 15, {s.width / 2, s.height / 2}, pane::White(dc, 0.92f).get());
            }
        },
        {})
        ->onClick = [this] {
        // The arrow's part of the button opens the panel; the speaker's mutes.
        const RECT& r = m_layout.mute;
        if (!m_splitVolume && m_ehres && r.right - r.left > PxI(39) && m_clickPoint.x >= r.right - PxI(16))
            SetPopup(!m_popupOpen);
        else
            playback_control::get()->volume_mute_toggle();
    };
    // Without Media Center's art: WMP 12's narrow transport, the arrow beside the speaker.
    add(kVolArrow, m_barLayer,
        [this](auto* dc, D2D1_SIZE_F s, Layer l) {
            if (m_splitVolume) { // the split button's arrow half: lit, pressed, held while the flyout is open
                const float k = s.height / 25.0f;
                if (l != Layer::Normal || m_popupOpen)
                    SplitPlate(dc, l == Layer::Hover && !m_popupOpen ? 4628 : 4627, true, 0, k);
                D2D1_MATRIX_3X2_F base;
                dc->GetTransform(&base);
                dc->SetTransform(D2D1::Matrix3x2F::Translation(-25 * k, 0) * base);
                SplitArrow(dc, k);
                dc->SetTransform(base);
                return;
            }
            DarkPlate(dc, s, l, m_popupOpen);
            const float cx = s.width / 2, cy = s.height / 2;
            com_ptr<ID2D1Factory> f;
            dc->GetFactory(f.put());
            com_ptr<ID2D1PathGeometry> path;
            com_ptr<ID2D1GeometrySink> sink;
            if (FAILED(f->CreatePathGeometry(path.put())) || FAILED(path->Open(sink.put()))) return;
            sink->BeginFigure({cx - 3.5f, cy - 1.5f}, D2D1_FIGURE_BEGIN_FILLED);
            sink->AddLine({cx + 3.5f, cy - 1.5f});
            sink->AddLine({cx, cy + 2.5f});
            sink->EndFigure(D2D1_FIGURE_END_CLOSED);
            sink->Close();
            dc->FillGeometry(path.get(), pane::White(dc, 0.9f).get());
        },
        {})
        ->onClick = [this] { SetPopup(!m_popupOpen); };
    // Media Center: volume down / up beside the speaker.
    add(kVolDown, m_barLayer, [](auto* dc, D2D1_SIZE_F s, Layer l) { DrawControl(dc, s, L"MINUS", l); }, {})
        ->onClick = [] {}; // steps on the press, and again while held (WM_LBUTTONDOWN)
    add(kVolUp, m_barLayer, [](auto* dc, D2D1_SIZE_F s, Layer l) { DrawControl(dc, s, L"PLUS", l); }, {})
        ->onClick = [] {};

    // Lyrics and queue. Windows 7 has no lyrics glyph: Media Center's teletext page (a page
    // of text) stands for lyrics, its guide list for the queue. The side that shows keeps
    // its focus plate.
    add(kLyrics, m_barLayer,
        [this](auto* dc, D2D1_SIZE_F s, Layer l) {
            const bool on = m_paneShown && m_pane && m_pane->CurrentMode() == NowPlayingPane::Mode::Lyrics;
            const Layer lit = on && l == Layer::Normal ? Layer::Hover : l;
            if (m_ehres && !Wmc()) { // WMP's plate, Media Center's teletext glyph (29x24)
                PlateButton(dc, s, l, on, [&] {
                    InBox(dc, s, 29, 24, [&] { DrawControlAt(dc, {0, 0, 29, 24}, L"TELETEXT", Layer::Normal); });
                });
                return;
            }
            if (m_ehres && DrawControl(dc, s, L"TELETEXT", lit)) return;
            DarkPlate(dc, s, l, on);
            pane::DrawIcon(dc, L'\xE8BD', 16, {s.width / 2, s.height / 2 + 0.5f}, pane::White(dc, 0.92f).get());
        },
        {})
        ->onClick = [this] { TogglePane(NowPlayingPane::Mode::Lyrics); };
    add(kList, m_barLayer,
        [this](auto* dc, D2D1_SIZE_F s, Layer l) {
            const bool on = m_paneShown && m_pane && m_pane->CurrentMode() == NowPlayingPane::Mode::Queue;
            const Layer lit = on && l == Layer::Normal ? Layer::Hover : l;
            if (m_ehres && !Wmc()) {
                PlateButton(dc, s, l, on, [&] {
                    InBox(dc, s, 38, 31, [&] { DrawControlAt(dc, {0, 0, 38, 31}, L"GUIDE", Layer::Normal); });
                });
                return;
            }
            if (m_ehres && DrawControl(dc, s, L"GUIDE", lit)) return;
            DarkPlate(dc, s, l, on);
            pane::DrawIcon(dc, pane::icon::kList, 16, {s.width / 2, s.height / 2}, pane::White(dc, 0.92f).get());
        },
        {})
        ->onClick = [this] { TogglePane(NowPlayingPane::Mode::Queue); };

    // Back to the library: Media Center's back orb when full screen, WMP 12's "switch to
    // library" corner button in the mini window.
    add(kLibrary, m_capsLayer,
        [this](auto* dc, D2D1_SIZE_F s, Layer l) {
            if (m_ehres) {
                DrawControl(dc, s, L"GOBACK", l);
            } else if (m_wmp && win7::Resources::Get().Has(kWmploc, kSwitchToLibrary)) {
                if (l == Layer::Hover || l == Layer::Pressed)
                    win7::Draw(dc, kWmploc, l == Layer::Hover ? 4725 : 4724, {0, 0});
                win7::Draw(dc, kWmploc, kSwitchToLibrary, {0, 0});
            } else {
                com_ptr<ID2D1SolidColorBrush> dark;
                dc->CreateSolidColorBrush(D2D1::ColorF(0x061633, l == Layer::Normal ? 0.45f : 0.65f), dark.put());
                dc->FillRoundedRectangle({{0, 0, s.width, s.height}, 3, 3}, dark.get());
                pane::DrawIcon(dc, L'\xE8F1', 13, {s.width / 2, s.height / 2}, pane::White(dc, 0.95f).get());
            }
        },
        {})
        ->onClick = [this] {
        // Back is a step inside the full screen: from the lyrics or the queue to the song.
        // Media Center's orb is there only for that step (SlideBack).
        if (m_mode == Mode::Full && m_paneShown && m_pane)
            SetPaneShown(false, m_pane->CurrentMode());
        else
            Close();
    };
    WidgetById(kLibrary)->enabled = [this] { return !Wmc() || m_paneShown; };
    // Media Center's own caption buttons, top right of the full screen.
    add(kMinimize, m_capsLayer, [](auto* dc, D2D1_SIZE_F s, Layer l) { DrawControl(dc, s, L"MINIMIZE", l); }, {})
        ->onClick = [this] { ShowWindow(m_hwnd, SW_MINIMIZE); };
    add(kRestore, m_capsLayer, [](auto* dc, D2D1_SIZE_F s, Layer l) { DrawControl(dc, s, L"RESTORE", l); }, {})
        ->onClick = [this] { Show(Mode::Mini); };
    add(kExit, m_capsLayer, [](auto* dc, D2D1_SIZE_F s, Layer l) { DrawControl(dc, s, L"EXIT", l); }, {})
        ->onClick = [this] { Close(); };

    // Full screen: Media Center's menu beside the cover. Text items; the one under the pointer
    // lights its glass button (COMMON.BUTTON.LEFT.FOCUS, faded in and out by the widget), what
    // is on carries Media Center's check mark.
    const auto menuItem = [this](int index) {
        return [this, index](ID2D1DeviceContext* dc, D2D1_SIZE_F s, Layer l) {
            const float k = s.height / 51.0f;
            const bool lit = l == Layer::Hover || l == Layer::Pressed;
            if (lit && !(m_ehres && win7::DrawStretched(dc, kEhres, L"COMMON.BUTTON.LEFT.FOCUS.PNG",
                                                         {0, 0, s.width, s.height}, l == Layer::Pressed ? 0.75f : 1.0f)))
                DarkPlate(dc, s, l, false);
            const bool lyrics = m_paneShown && m_pane && m_pane->CurrentMode() == NowPlayingPane::Mode::Lyrics;
            const bool queue = m_paneShown && m_pane && m_pane->CurrentMode() == NowPlayingPane::Mode::Queue;
            const bool radio = index == 2 && media::radio::Active();
            const bool on = index == 0 ? lyrics : index == 1 ? queue : index == 2 ? m_shuffle && !radio : m_repeat != 0;
            // Media Center's menu type (misc.mp4): #9CDDFC at rest, #F2F2F2 under the pointer, no
            // shadow. While a press runs, its layers draw the type instead (MenuPressed).
            if (index != m_menuInking) {
                com_ptr<ID2D1SolidColorBrush> ink;
                // A station's shuffle does nothing: grey type, and no light (the widget is off).
                const bool off = radio && !media::radio::CanDislike();
                dc->CreateSolidColorBrush(D2D1::ColorF(off ? 0x6C7C88 : lit ? 0xF2F2F2 : 0x9CDDFC), ink.put());
                MenuLabel(dc, s, MenuText(index), ink.get(), nullptr);
            }
            if (!on) return;
            // The check mark centred 229 units from the button's left (misc.mp4), 27x26.
            const D2D1_RECT_F mark{229 * k - 13.5f * k, (s.height - 26 * k) / 2, 229 * k + 13.5f * k,
                                   (s.height + 26 * k) / 2};
            if (!(m_ehres && win7::DrawStretched(dc, kEhres, L"NOWPLAYING.CHECK.ENABLED.PNG", mark)))
                pane::DrawIcon(dc, L'\xE73E', 15 * k, {(mark.left + mark.right) / 2, s.height / 2},
                               pane::White(dc, 0.95f).get());
        };
    };
    add(kMenuLyrics, m_menuLayer, menuItem(0), {})->onClick = [this] {
        MenuPressed(0);
        TogglePane(NowPlayingPane::Mode::Lyrics);
    };
    add(kMenuList, m_menuLayer, menuItem(1), {})->onClick = [this] {
        MenuPressed(1);
        TogglePane(NowPlayingPane::Mode::Queue);
    };
    // Shuffle and repeat take their new state at once, so the item, its label (repeat has
    // three: off, all, one) and the press's ink all show the state the press set.
    add(kMenuShuffle, m_menuLayer, menuItem(2), {})->onClick = [this] {
        MenuPressed(2);
        ApplyModes(media::transport::ShufflePressed({m_shuffle, m_repeat}));
    };
    WidgetById(kMenuShuffle)->enabled = [] { return !media::radio::Active() || media::radio::CanDislike(); };
    add(kMenuRepeat, m_menuLayer, menuItem(3), {})->onClick = [this] {
        ApplyModes(media::transport::RepeatPressed({m_shuffle, m_repeat}));
        MenuPressed(3);
    };
    // Media Center's focus light: a glow that breathes over the focused button, and on a press
    // a highlight that sweeps across it with an afterglow.
    {
        const auto& comp = gfx::CompositionHost::Get().Compositor();
        m_menuShade = comp.CreateSpriteVisual();
        m_menuLayer.Children().InsertAtBottom(m_menuShade);
        for (auto& glow : m_menuGlow) {
            glow = comp.CreateSpriteVisual();
            glow.Opacity(0.0f);
            if (m_ehres) glow.Brush(ArtBrush(L"COMMON.BUTTON.LEFT.FOCUSOVERLAY.PNG"));
            m_menuLayer.Children().InsertAtTop(glow);
        }
        m_menuFx = comp.CreateContainerVisual();
        // Not clipped: the afterglow (327x63) is larger than the 251x51 button and lies centred
        // on it. The highlight (443x56) runs inside the button's plate (button.mp4): its own
        // host is clipped to the plate.
        m_menuLayer.Children().InsertAtTop(m_menuFx);
        // The pressed item's type (misc.mp4): black where the highlight has passed, white again
        // behind a second, slower front. Its letters in black and in white under complementary
        // masks with four stops: white | ramp | black | ramp | white.
        for (auto* v : {&m_menuInk, &m_menuInkWhite}) {
            *v = comp.CreateSpriteVisual();
            v->Opacity(0.0f);
            m_menuFx.Children().InsertAtTop(*v);
        }
        for (auto* mask : {&m_menuInkMask, &m_menuInkMaskWhite}) {
            const float black = mask == &m_menuInkMask ? 1.0f : 0.0f;
            *mask = comp.CreateLinearGradientBrush();
            mask->MappingMode(wuc::CompositionMappingMode::Absolute);
            for (const float a : {1 - black, black, black, 1 - black})
                mask->ColorStops().Append(comp.CreateColorGradientStop(0.0f, Rgba(0xFFFFFF, a)));
        }
        m_menuAfter = comp.CreateSpriteVisual();
        m_menuAfter.Opacity(0.0f);
        m_menuFx.Children().InsertAtTop(m_menuAfter);
        m_menuSweepHost = comp.CreateContainerVisual();
        m_menuSweepShape = comp.CreateRoundedRectangleGeometry();
        m_menuSweepHost.Clip(comp.CreateGeometricClip(m_menuSweepShape));
        m_menuFx.Children().InsertAtTop(m_menuSweepHost);
        m_menuSweep = comp.CreateSpriteVisual();
        m_menuSweep.Opacity(0.0f);
        m_menuSweepHost.Children().InsertAtTop(m_menuSweep);
        if (m_ehres) {
            m_menuAfter.Brush(ArtBrush(L"COMMON.BUTTON.PRESSEDAFTERGLOW.PNG"));
            m_menuSweep.Brush(ArtBrush(L"COMMON.BUTTON.PRESSEDHIGHLIGHT.PNG"));
        }
    }

    // The playing song among the favourites: a heart drawn as one more of Media Center's
    // controls, the same size, plate and glyph style as the speaker and the queue beside it.
    add(kFavorite, m_barLayer,
        [this](auto* dc, D2D1_SIZE_F s, Layer l) {
            if (!Wmc() && (m_ehres || m_wmp)) {
                PlateButton(dc, s, l, false, [&] { InBox(dc, s, 38, 31, [&] { HeartGlyph(dc, {38, 31}, m_favorite); }); });
                return;
            }
            if (m_ehres) {
                ControlPlate(dc, {0, 0, s.width, s.height}, l);
                HeartGlyph(dc, s, m_favorite);
                return;
            }
            DarkPlate(dc, s, l, false);
            pane::DrawIcon(dc, m_favorite ? L'\xEB52' : L'\xEB51', 16, {s.width / 2, s.height / 2},
                           pane::White(dc, 0.92f).get());
        },
        {})
        ->onClick = [this] { m_host.TogglePlayerFavorite(); };

    // Shuffle and repeat before stop, as in WMP 12's transport: on, their plate stays pressed.
    // A station has no shuffle: its "recommend less" takes the place where it has one, else the
    // glyph is grey and the button does nothing (owner, 2026-11-20).
    add(kShuffle, m_barLayer,
        [this](auto* dc, D2D1_SIZE_F s, Layer l) {
            if (media::radio::Active()) {
                const bool dislike = media::radio::CanDislike();
                PlateButton(dc, s, dislike ? l : Layer::Normal, false,
                            [&] { InBox(dc, s, 38, 31, [&] { PlayModeGlyph(dc, true, 0, dislike, !dislike); }); });
                return;
            }
            PlateButton(dc, s, l, m_shuffle, [&] { InBox(dc, s, 38, 31, [&] { PlayModeGlyph(dc, true, 0); }); });
        },
        {})
        ->onClick = [this] { ApplyModes(media::transport::ShufflePressed({m_shuffle, m_repeat})); };
    WidgetById(kShuffle)->enabled = [] { return !media::radio::Active() || media::radio::CanDislike(); };
    add(kRepeat, m_barLayer,
        [this](auto* dc, D2D1_SIZE_F s, Layer l) {
            PlateButton(dc, s, l, m_repeat != 0, [&] {
                // A station repeats one song or none: repeat-one's glyph either way.
                InBox(dc, s, 38, 31, [&] { PlayModeGlyph(dc, false, media::radio::Active() ? 2 : m_repeat); });
            });
        },
        {})
        ->onClick = [this] { ApplyModes(media::transport::RepeatPressed({m_shuffle, m_repeat})); };

    // The mini window's Win7 caption buttons (the library window's, same artwork).
    add(kCapMin, m_frameLayer, CaptionPainter(CaptionButton::Minimize, m_aeroArt),
        CaptionOptions(CaptionButton::Minimize, m_aeroArt))
        ->onClick = [this] { ShowWindow(m_hwnd, SW_MINIMIZE); };
    add(kCapMax, m_frameLayer, CaptionPainter(CaptionButton::Maximize, m_aeroArt),
        CaptionOptions(CaptionButton::Maximize, m_aeroArt))
        ->onClick = [this] { Show(Mode::Full); }; // maximizing the player is the full screen
    add(kCapClose, m_frameLayer, CaptionPainter(CaptionButton::Close, m_aeroArt),
        CaptionOptions(CaptionButton::Close, m_aeroArt))
        ->onClick = [this] { Close(); };
}

Widget* PlayerWindow::WidgetById(int id) const {
    for (const auto& w : m_widgets)
        if (w->Id() == id) return w.get();
    return nullptr;
}

Widget* PlayerWindow::WidgetAt(POINT pt) const {
    if (m_popupOpen && m_mode == Mode::Mini && PtInRect(&m_layout.popup, pt)) return nullptr; // the flyout
    for (auto it = m_widgets.rbegin(); it != m_widgets.rend(); ++it)
        if ((*it)->HitTest(pt)) return it->get();
    return nullptr;
}

void PlayerWindow::SetHover(Widget* w) {
    if (w == m_hover) return;
    auto& host = gfx::CompositionHost::Get();
    const auto glow = [&](Widget* widget, bool on) {
        if (!widget) return;
        const int index = widget->Id() - kMenuLyrics;
        if (index < 0 || index > 3 || !m_menuGlow[static_cast<size_t>(index)]) return;
        const auto& visual = m_menuGlow[static_cast<size_t>(index)];
        // An item that is off (a station's shuffle) lights nothing: its plate stays dark, and so
        // does the breathing glow over it (owner, 2026-11-21: the glow was left on it).
        if (on && widget->IsEnabled())
            host.StartPulse(visual, 0.25f, 1.0f, 2200);
        else
            host.StopPulse(visual, 0.0f, 320);
    };
    glow(m_hover, false);
    if (m_hover) m_hover->SetHover(false);
    m_hover = w;
    if (m_hover) m_hover->SetHover(true);
    glow(m_hover, true);
    UpdateSplitPlate();
}

// The split button's plain plate shows under both halves while either is lit or the flyout is
// open, fading as the halves do.
void PlayerWindow::UpdateSplitPlate() {
    if (!m_splitPlate) return;
    const bool on = m_splitVolume && m_mode == Mode::Mini &&
                    (m_popupOpen || (m_hover && (m_hover->Id() == kMute || m_hover->Id() == kVolArrow)) ||
                     (m_pressed && (m_pressed->Id() == kMute || m_pressed->Id() == kVolArrow)));
    if (on == m_splitPlateShown) return;
    m_splitPlateShown = on;
    FadeTo(m_splitPlate, on ? 1.0f : 0.0f, on ? 160 : 260);
}

// The press: Media Center's highlight runs across the button and an afterglow fades behind it.
void PlayerWindow::MenuPressed(int index) {
    const RECT& r = m_layout.menu[static_cast<size_t>(index)];
    if (Empty(r) || !m_menuFx) return;
    const auto& comp = gfx::CompositionHost::Get().Compositor();
    const float w = static_cast<float>(r.right - r.left), h = static_cast<float>(r.bottom - r.top);
    m_menuFx.Offset(float3{static_cast<float>(r.left), static_cast<float>(r.top), 0});
    m_menuFx.Size(float2{w, h});
    const auto out = gfx::Easing({0.2f, 0.7f, 0.2f, 1.0f});
    const auto linear = comp.CreateLinearEasingFunction();
    // Measured frame by frame on the owner's recording (button.mp4, 30 fps, five presses), time
    // from the first changed frame, fronts as the 50 % point of a ramp 44 units wide (about a
    // character and a half) in fractions of the type's width, the highlight's head in fractions
    // of the button's:
    //   black front   -0.15 at 0 ms, 0.47 at 33, 1.0 at 67, past the end by 85: even and fast
    //   white front   from the left at 55 ms: 0.11 at 67, 0.38 at 100, 0.555 at 167, 0.645 at
    //                 200, 0.725 at 233, 0.80 at 267, 0.91 at 333, 1.0 at 367, gone by 400:
    //                 a leap and then slowing down
    //   highlight     head at 0.03, 0.61, 0.96 (0, 33, 67 ms), on to 1.08 at 100 while it fades
    //                 (0.3, 1, 1, 0.35, 0 at 167); always inside the button, ahead of the black
    const float u = h / 51.0f;              // px per unit of the 51-unit button
    const float k = h / Px(51.0f);          // DIP per unit
    const float ramp = 44 * u;
    auto& host = gfx::CompositionHost::Get();
    float textEnd = w * 0.7f;
    {
        com_ptr<IDWriteTextLayout> measure;
        auto format = pane::Format(kMenuTextSize * k, DWRITE_FONT_WEIGHT_NORMAL);
        const std::wstring text = MenuText(index);
        host.DWrite()->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), format.get(), 10000, 100,
                                        measure.put());
        DWRITE_TEXT_METRICS metrics{};
        if (measure && SUCCEEDED(measure->GetMetrics(&metrics))) textEnd = Px(kMenuTextX * k + metrics.width);
    }
    const float textStart = kMenuTextX * u, tw = std::max(textEnd - textStart, 8 * u);
    constexpr int kTotalMs = 400;
    // The fronts' 50 % points (px of the button) over time; the masks' stops sit half a ramp
    // either side: white | a ramp b | black | c ramp d | white.
    const float black0 = textStart - 0.15f * tw;
    const std::vector<std::pair<int, float>> blackPath{{0, black0},
                                                       {33, textStart + 0.47f * tw},
                                                       {67, textStart + tw},
                                                       {85, textEnd + ramp / 2 + 2 * u},
                                                       // out of the white front's way (stops stay in order)
                                                       {kTotalMs, textEnd + 2 * ramp + 4 * u}};
    std::vector<std::pair<int, float>> whitePath{{0, black0 - ramp - 2 * u}, {55, textStart - ramp / 2}};
    for (const auto& [ms, p] : std::initializer_list<std::pair<int, float>>{
             {67, 0.11f}, {100, 0.38f}, {167, 0.555f}, {200, 0.645f}, {233, 0.725f}, {267, 0.80f}, {333, 0.91f}, {367, 1.0f}})
        whitePath.push_back({ms, textStart + p * tw});
    whitePath.push_back({kTotalMs, textEnd + ramp / 2 + 2 * u});

    // The highlight (443x56 units, its bright head at 330), inside the button, then fading.
    const float hw = 443 * u, hh = 56 * u, head = 330 * u;
    m_menuSweepHost.Size(float2{w, h});
    m_menuSweepShape.Offset(float2{2 * u, 2 * u});
    m_menuSweepShape.Size(float2{w - 4 * u, h - 4 * u});
    m_menuSweepShape.CornerRadius(float2{5 * u, 5 * u});
    m_menuSweep.Size(float2{hw, hh});
    auto sweep = comp.CreateVector3KeyFrameAnimation();
    const float sy = (h - hh) / 2;
    sweep.InsertKeyFrame(0.0f, float3{0.03f * w - head, sy, 0});
    sweep.InsertKeyFrame(33.0f / 167, float3{0.61f * w - head, sy, 0}, linear);
    sweep.InsertKeyFrame(67.0f / 167, float3{0.96f * w - head, sy, 0}, linear);
    sweep.InsertKeyFrame(100.0f / 167, float3{1.08f * w - head, sy, 0}, linear);
    sweep.InsertKeyFrame(1.0f, float3{1.12f * w - head, sy, 0}, linear);
    sweep.Duration(Ms(167));
    m_menuSweep.StartAnimation(L"Offset", sweep);
    auto flash = comp.CreateScalarKeyFrameAnimation();
    flash.InsertKeyFrame(0.0f, 0.3f);
    flash.InsertKeyFrame(33.0f / 167, 1.0f, linear);
    flash.InsertKeyFrame(67.0f / 167, 1.0f, linear);
    flash.InsertKeyFrame(100.0f / 167, 0.35f, linear);
    flash.InsertKeyFrame(1.0f, 0.0f, out);
    flash.Duration(Ms(167));
    m_menuSweep.StartAnimation(L"Opacity", flash);

    // The type itself: while the press runs the item does not draw it (no white edge of it can
    // show around the black); the two layers do.
    const auto layer = [&](uint32_t rgb) {
        const auto surface = host.CreateSurface(static_cast<int>(w), static_cast<int>(h));
        host.Draw(surface, m_scale, [&](ID2D1DeviceContext* dc) {
            com_ptr<ID2D1SolidColorBrush> ink;
            dc->CreateSolidColorBrush(D2D1::ColorF(rgb), ink.put());
            MenuLabel(dc, D2D1::SizeF(w / m_scale, h / m_scale), MenuText(index), ink.get(), nullptr);
        });
        return comp.CreateSurfaceBrush(surface);
    };
    // The masks span three button widths from one to the left, so every front stays on them.
    const auto at = [w](float x) { return std::clamp((x + w) / (3 * w), 0.0f, 1.0f); };
    for (auto [visual, mask, rgb] : {std::tuple{m_menuInk, m_menuInkMask, 0x000000u},
                                     std::tuple{m_menuInkWhite, m_menuInkMaskWhite, 0xF2F2F2u}}) {
        auto masked = comp.CreateMaskBrush();
        masked.Source(layer(rgb));
        masked.Mask(mask);
        visual.Brush(masked);
        visual.Size(float2{w, h});
        visual.StopAnimation(L"Opacity");
        visual.Opacity(1.0f);
        mask.StartPoint(float2{-w, 0});
        mask.EndPoint(float2{2 * w, 0});
    }
    // The item pressed before, if its press still runs, draws its own type again: the ink moves
    // here, and its own end (a later generation) will not come.
    const int before = std::exchange(m_menuInking, index);
    if (before >= 0 && before != index)
        if (auto* item = WidgetById(kMenuLyrics + before)) item->Invalidate();
    if (auto* item = WidgetById(kMenuLyrics + index)) item->Invalidate();
    const uint64_t generation = ++m_menuInkGeneration;
    auto batch = comp.CreateScopedBatch(wuc::CompositionBatchTypes::Animation);
    // A stop follows its front's path, `offset` px from the 50 % point, over the whole press.
    const auto run = [&](uint32_t stop, const std::vector<std::pair<int, float>>& path, float offset) {
        auto move = comp.CreateScalarKeyFrameAnimation();
        for (const auto& [ms, x] : path) {
            const float f = static_cast<float>(ms) / kTotalMs;
            if (ms == 0)
                move.InsertKeyFrame(0.0f, at(x + offset));
            else
                move.InsertKeyFrame(f, at(x + offset), linear);
        }
        if (path.back().first < kTotalMs) move.InsertKeyFrame(1.0f, at(path.back().second + offset), linear);
        move.Duration(Ms(kTotalMs));
        for (const auto& mask : {m_menuInkMask, m_menuInkMaskWhite}) mask.ColorStops().GetAt(stop).StartAnimation(L"Offset", move);
    };
    run(2, blackPath, -ramp / 2); // black spreads behind the highlight
    run(3, blackPath, ramp / 2);
    run(0, whitePath, -ramp / 2); // white comes back behind it
    run(1, whitePath, ramp / 2);
    batch.End();
    // Then the item draws its type again, in whatever state the pointer has left it.
    batch.Completed([this, generation, index, alive = std::weak_ptr<int>(m_alive)](const auto&, const auto&) {
        if (alive.expired() || generation != m_menuInkGeneration) return;
        m_menuInking = -1;
        if (auto* item = WidgetById(kMenuLyrics + index)) item->Invalidate();
        m_menuInk.Opacity(0.0f);
        m_menuInkWhite.Opacity(0.0f);
    });
    // The afterglow at its own size (327x63 units), centred on the button.
    m_menuAfter.Size(float2{327 * u, 63 * u});
    m_menuAfter.Offset(float3{(w - 327 * u) / 2, (h - 63 * u) / 2, 0});
    // On the recording it is faint: the plate is a little lighter for half a second.
    auto after = comp.CreateScalarKeyFrameAnimation();
    after.InsertKeyFrame(0.0f, 0.0f);
    after.InsertKeyFrame(0.2f, 0.55f);
    after.InsertKeyFrame(1.0f, 0.0f, out);
    after.Duration(Ms(750));
    m_menuAfter.StartAnimation(L"Opacity", after);
}

// The modes a shuffle or repeat press set (media::transport): shown at once by both forms'
// buttons, and handed to the library window, which owns them.
void PlayerWindow::ApplyModes(media::transport::Modes modes) {
    if (modes.shuffle == m_shuffle && modes.repeat == m_repeat) return; // a station's dislike
    m_shuffle = modes.shuffle;
    m_repeat = modes.repeat;
    m_host.SetPlayerMode(m_shuffle, m_repeat);
    for (int id : {kShuffle, kMenuShuffle, kRepeat, kMenuRepeat})
        if (auto* w = WidgetById(id)) w->Invalidate();
}

std::wstring PlayerWindow::MenuText(int index) const {
    if (index == 2 && media::radio::CanDislike()) return Tr(L"Not interested");
    return index == 0   ? Tr(L"Lyrics")
           : index == 1 ? Tr(L"Up Next")
           : index == 2 ? Tr(L"Shuffle")
                        : Tr(m_repeat == 2 || media::radio::Active() ? L"Repeat one" : L"Repeat");
}

} // namespace glasswave::ui
