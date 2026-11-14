#include "pch.h"
#include "wmp/wmp_transport.h"
#include "gfx/composition_host.h"
#include "win7/win7_resources.h"

namespace glasswave::wmp::transport {

namespace {

using win7::Module;

constexpr D2D1_POINT_2F kOrbCenter{108, 26};
constexpr float kOrbRadius = 22.5f;

void Bitmap(ID2D1DeviceContext* dc, int id, float x, float y, float opacity = 1.0f) {
    win7::Draw(dc, Module::Wmploc, id, D2D1::Point2F(x, y), opacity);
}

// Draw a cluster state bitmap so that it lines up with `region`, clipped to the region
// and, for the segments, excluding the orb (the orb widget sits on top of them).
void ClusterCrop(ID2D1DeviceContext* dc, int id, D2D1_RECT_F region, bool excludeOrb) {
    const float w = region.right - region.left, h = region.bottom - region.top;
    winrt::com_ptr<ID2D1Factory> factory;
    dc->GetFactory(factory.put());
    winrt::com_ptr<ID2D1RectangleGeometry> rect;
    factory->CreateRectangleGeometry(D2D1::RectF(0, 0, w, h), rect.put());
    winrt::com_ptr<ID2D1Geometry> mask = rect;
    if (excludeOrb) {
        winrt::com_ptr<ID2D1EllipseGeometry> orb;
        factory->CreateEllipseGeometry(
            D2D1::Ellipse(D2D1::Point2F(kOrbCenter.x - region.left, kOrbCenter.y - region.top), kOrbRadius, kOrbRadius),
            orb.put());
        winrt::com_ptr<ID2D1PathGeometry> path;
        winrt::com_ptr<ID2D1GeometrySink> sink;
        factory->CreatePathGeometry(path.put());
        path->Open(sink.put());
        rect->CombineWithGeometry(orb.get(), D2D1_COMBINE_MODE_EXCLUDE, nullptr, sink.get());
        sink->Close();
        mask = path;
    }
    dc->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), mask.get()), nullptr);
    Bitmap(dc, id, -region.left, -region.top);
    dc->PopLayer();
}

}  // namespace

bool Available() { return win7::Resources::Get().Has(Module::Wmploc, kCluster); }

void ClusterBackground(ID2D1DeviceContext* dc) { Bitmap(dc, kCluster, 0, 0); }

// The capsule bitmap (558x68, cluster x = column - 171) holds, besides its ends, two divider
// lines: columns 162 - 164 (between repeat and stop) and 398 - 400 (before the slider). Its
// columns are otherwise the same between them: 93 - 161, 165 - 258, 299 - 397 and 401 - 462.
// Its ends are columns 77 - 92 and 463 - 480. A shortened half is its end, moved in, joined
// to the orb by one plain column stretched (no divider of a control that has gone); a half
// that keeps its end is the bitmap's own. The halves meet under the orb.
void CapsuleSpan(ID2D1DeviceContext* dc, float left, float right) {
    const std::wstring name = L"#" + std::to_wstring(kCapsule);
    constexpr float kColumn = -kCapsuleRect.left; // cluster x to bitmap column
    const float seam = kOrbCenter.x + kColumn;
    const auto half = [&](D2D1_RECT_F clip, const auto& draw) {
        dc->PushAxisAlignedClip(clip, D2D1_ANTIALIAS_MODE_ALIASED);
        draw();
        dc->PopAxisAlignedClip();
    };
    constexpr UINT32 rows = 68; // the bitmap's height
    const float h = static_cast<float>(rows);
    half(D2D1::RectF(-1000, -1000, seam, 1000), [&] {
        if (left <= kCapsuleVisible.left) {
            Bitmap(dc, kCapsule, 0, 0);
            return;
        }
        const float x = left + kColumn; // where column 78 (the end's first opaque one) goes
        win7::DrawPartTo(dc, Module::Wmploc, name, {77, 0, 93, rows}, {x - 1, 0, x + 15, h});
        win7::DrawPartTo(dc, Module::Wmploc, name, {120, 0, 121, rows}, {x + 15, 0, seam, h});
    });
    half(D2D1::RectF(seam, -1000, 2000, 1000), [&] {
        if (right >= kCapsuleVisible.right) {
            Bitmap(dc, kCapsule, 0, 0);
            return;
        }
        const float x = right + kColumn; // where column 480 ends
        win7::DrawPartTo(dc, Module::Wmploc, name, {340, 0, 341, rows}, {seam, 0, x - 17, h});
        win7::DrawPartTo(dc, Module::Wmploc, name, {463, 0, 481, rows}, {x - 17, 0, x + 1, h});
    });
}

winrt::com_ptr<ID2D1LinearGradientBrush> RowBands(ID2D1DeviceContext* dc, float y0, float y1,
                                                  std::initializer_list<D2D1_COLOR_F> rows, bool smooth) {
    std::vector<D2D1_GRADIENT_STOP> stops;
    const float n = static_cast<float>(rows.size());
    int i = 0;
    for (const auto& c : rows) {
        if (smooth) {
            stops.push_back({(i + 0.5f) / n, c});
        } else {
            stops.push_back({i / n, c});
            stops.push_back({(i + 1) / n - 0.0001f, c});
        }
        ++i;
    }
    winrt::com_ptr<ID2D1GradientStopCollection> collection;
    dc->CreateGradientStopCollection(stops.data(), static_cast<UINT32>(stops.size()), collection.put());
    winrt::com_ptr<ID2D1LinearGradientBrush> brush;
    dc->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({0, y0}, {0, y1}), collection.get(), brush.put());
    return brush;
}

// The arrow as wmploc 4626 draws it (39x25 units, `k` DIP each): white rows 11 - 14 of 7, 5, 3
// and 1 pixels (#FFFFFF, #FEFEFE, #E9E9E9, #DFDFDF) from x 28, in a one-pixel navy (#00001D)
// shadow, heaviest below the point.
void SplitArrow(ID2D1DeviceContext* dc, float k) {
    winrt::com_ptr<ID2D1Factory> factory;
    dc->GetFactory(factory.put());
    const auto triangle = [&](float left, float right, float top, float tipY) {
        winrt::com_ptr<ID2D1PathGeometry> path;
        winrt::com_ptr<ID2D1GeometrySink> sink;
        factory->CreatePathGeometry(path.put());
        path->Open(sink.put());
        sink->BeginFigure({left * k, top * k}, D2D1_FIGURE_BEGIN_FILLED);
        sink->AddLine({right * k, top * k});
        sink->AddLine({(left + right) / 2 * k, tipY * k});
        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
        sink->Close();
        return path;
    };
    winrt::com_ptr<ID2D1SolidColorBrush> haze, shadow;
    dc->CreateSolidColorBrush(D2D1::ColorF(0x00001D, 0.12f), haze.put());
    dc->CreateSolidColorBrush(D2D1::ColorF(0x00001D, 0.20f), shadow.put());
    dc->FillGeometry(triangle(26.6f, 36.4f, 10.0f, 16.8f).get(), haze.get());
    dc->FillGeometry(triangle(27.4f, 35.6f, 10.5f, 15.9f).get(), shadow.get());
    dc->FillGeometry(triangle(28, 35, 11, 15).get(),
                     RowBands(dc, 11 * k, 15 * k,
                              {D2D1::ColorF(0xFFFFFF), D2D1::ColorF(0xFEFEFE), D2D1::ColorF(0xE9E9E9),
                               D2D1::ColorF(0xDFDFDF)},
                              true)
                         .get());
}

void SplitPlate(ID2D1DeviceContext* dc, int id, bool arrowHalf, float x, float k) {
    const std::wstring name = L"#" + std::to_wstring(id);
    if (!arrowHalf) {
        win7::DrawPartTo(dc, Module::Wmploc, name, {0, 0, 25, 25}, {x, 0, x + 25 * k, 25 * k});
        return;
    }
    winrt::com_ptr<ID2D1Factory> factory;
    dc->GetFactory(factory.put());
    winrt::com_ptr<ID2D1RectangleGeometry> part, patch;
    factory->CreateRectangleGeometry({x, 0, x + 14 * k, 25 * k}, part.put());
    factory->CreateRectangleGeometry({x + 2 * k, 10 * k, x + 11 * k, 17 * k}, patch.put());
    winrt::com_ptr<ID2D1PathGeometry> around;
    winrt::com_ptr<ID2D1GeometrySink> sink;
    factory->CreatePathGeometry(around.put());
    around->Open(sink.put());
    part->CombineWithGeometry(patch.get(), D2D1_COMBINE_MODE_EXCLUDE, nullptr, sink.get());
    sink->Close();
    dc->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), around.get(), D2D1_ANTIALIAS_MODE_ALIASED), nullptr);
    win7::DrawPartTo(dc, Module::Wmploc, name, {25, 0, 39, 25}, {x, 0, x + 14 * k, 25 * k});
    dc->PopLayer();
    win7::DrawPartTo(dc, Module::Wmploc, name, {26, 10, 27, 17}, {x + 2 * k, 10 * k, x + 11 * k, 17 * k});
}

// Measured on the owner's WMP at 200 % (docs/design/player-window.md, "Volume"): 5 DIP of
// half-DIP rings lit white at the top and left, cyan along the foot and right, over a frosted
// inside.
D2D1_RECT_F FlyoutFrame(ID2D1DeviceContext* dc, D2D1_SIZE_F size) {
    struct Tone {
        uint32_t rgb;
        float alpha;
    };
    static constexpr Tone kTopLeft[10] = {{0x767678, 0.55f}, {0xE9E9EA, 0.82f}, {0xF0F0F1, 0.82f}, {0x8C8C8D, 0.60f},
                                          {0x525253, 0.60f}, {0xE4E4E6, 0.45f}, {0xC2C2C4, 0.45f}, {0x555557, 0.60f},
                                          {0x808080, 0.60f}, {0xDADADA, 0.85f}};
    static constexpr Tone kFootRight[10] = {{0x00728E, 0.45f}, {0x00B4E1, 0.85f}, {0x08BCE8, 0.85f},
                                            {0x248197, 0.55f}, {0x4E4E4E, 0.60f}, {0x9E9E9E, 0.45f},
                                            {0x9B9B9B, 0.45f}, {0x434343, 0.60f}, {0x7A7A7A, 0.60f},
                                            {0xDADADA, 0.85f}};
    dc->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED); // rings meet without seams
    D2D1_RECT_F ring{0, 0, size.width, size.height};
    for (int i = 0; i < 10; ++i) {
        constexpr float d = 0.5f;
        winrt::com_ptr<ID2D1SolidColorBrush> lit, foot;
        dc->CreateSolidColorBrush(D2D1::ColorF(kTopLeft[i].rgb, kTopLeft[i].alpha), lit.put());
        dc->CreateSolidColorBrush(D2D1::ColorF(kFootRight[i].rgb, kFootRight[i].alpha), foot.put());
        dc->FillRectangle({ring.left, ring.top, ring.right - d, ring.top + d}, lit.get());           // top
        dc->FillRectangle({ring.left, ring.top + d, ring.left + d, ring.bottom}, lit.get());         // left
        dc->FillRectangle({ring.right - d, ring.top, ring.right, ring.bottom}, foot.get());          // right
        dc->FillRectangle({ring.left + d, ring.bottom - d, ring.right - d, ring.bottom}, foot.get()); // foot
        ring = {ring.left + d, ring.top + d, ring.right - d, ring.bottom - d};
    }
    // Inside, WMP's flat #F2F2F2 becomes frosted glass: the blurred transport under a milky
    // white, lighter at the top, with a soft sheen over the upper half.
    const D2D1_RECT_F inside = ring;
    dc->FillRectangle(inside, RowBands(dc, inside.top, inside.bottom,
                                       {D2D1::ColorF(0xFFFFFF, 0.74f), D2D1::ColorF(0xF4F6F8, 0.62f),
                                        D2D1::ColorF(0xEDF1F5, 0.56f)},
                                       true)
                                  .get());
    dc->FillRectangle({inside.left, inside.top, inside.right, (inside.top + inside.bottom) / 2},
                      RowBands(dc, inside.top, (inside.top + inside.bottom) / 2,
                               {D2D1::ColorF(1, 1, 1, 0.30f), D2D1::ColorF(1, 1, 1, 0.08f)}, true)
                          .get());
    dc->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    return inside;
}

void Capsule(ID2D1DeviceContext* dc) { Bitmap(dc, kCapsule, 0, 0); }

void ClusterButton(ID2D1DeviceContext* dc, D2D1_RECT_F region, Button button, Layer layer, bool enabled) {
    // The normal look is the cluster background below the widgets; hover and pressed are
    // the matching parts of the hover / pressed cluster bitmaps.
    if (enabled && (layer == Layer::Hover || layer == Layer::Pressed))
        ClusterCrop(dc, layer == Layer::Hover ? kClusterHover : kClusterPressed, region, button != Button::Stop);
    int glyph = 0;
    D2D1_POINT_2F at{};
    switch (button) {
    case Button::Stop: glyph = enabled ? kStopGlyph : kStopGlyphDisabled; at = {2, 13}; break;
    case Button::Prev: glyph = enabled ? kPrevGlyph : kPrevGlyphDisabled; at = {38, 13}; break;
    case Button::Next: glyph = enabled ? kNextGlyph : kNextGlyphDisabled; at = {126, 13}; break;
    }
    Bitmap(dc, glyph, at.x - region.left, at.y - region.top);
}

void Orb(ID2D1DeviceContext* dc, Layer layer, bool pause) {
    if (layer == Layer::Hover || layer == Layer::Pressed)
        ClusterCrop(dc, layer == Layer::Hover ? kClusterHover : kClusterPressed, kOrbRect, false);
    Bitmap(dc, pause ? kPauseGlyph : kPlayGlyph, 87 - kOrbRect.left, 4 - kOrbRect.top);
}

void GlowRing(ID2D1DeviceContext* dc, float size) {
    const float k = size / 52.0f;
    D2D1_MATRIX_3X2_F current;
    dc->GetTransform(&current);
    dc->SetTransform(D2D1::Matrix3x2F::Scale(k, k) * current);
    Bitmap(dc, kGlowRing, 0, 0);
    dc->SetTransform(current);
}

void Toggle(ID2D1DeviceContext* dc, Layer layer, bool shuffle, bool on) {
    const bool lit = on || layer == Layer::Hover || layer == Layer::Pressed;
    const int id = shuffle ? (lit ? kShuffleOn : kShuffleOff) : (lit ? kRepeatOn : kRepeatOff);
    Bitmap(dc, id, 0, 0, layer == Layer::Pressed ? 0.8f : 1.0f);
}

void Mute(ID2D1DeviceContext* dc, Layer layer, bool muted, float volume) {
    // WMP 12 square button plate under the speaker: 4725 hot, 4724 pressed (25x25).
    if (layer == Layer::Hover || layer == Layer::Pressed) Bitmap(dc, layer == Layer::Hover ? 4725 : 4724, 0, 0);
    const int id = muted ? kMuted : volume <= 0.0f ? kVolume0 : volume < 0.67f ? kVolume1 : kVolume2;
    Bitmap(dc, id, 0, 0);
}

void Volume(ID2D1DeviceContext* dc, Layer layer, float value) {
    const float cx = kThumbMin + std::clamp(value, 0.0f, 1.0f) * (kThumbMax - kThumbMin) - kVolumeRect.left;
    // Blue fill from the track start to the thumb center
    win7::DrawPart(dc, Module::Wmploc, kVolumeFill, D2D1::RectF(0, 0, cx, 23), D2D1::Point2F(0, 0));
    const int thumb = layer == Layer::Pressed ? kVolumeThumbPressed
                      : layer == Layer::Hover ? kVolumeThumbHover
                                              : kVolumeThumb;
    Bitmap(dc, thumb, std::round(cx - 7), 0);
}

void NavOrb(ID2D1DeviceContext* dc, D2D1_SIZE_F size, Layer layer, bool back, bool enabled) {
    const int id = !enabled                 ? kNavArrowsDisabled
                   : layer == Layer::Hover   ? kNavArrowsHover
                   : layer == Layer::Pressed ? kNavArrowsPressed
                                             : kNavArrows;
    const UINT32 n = static_cast<UINT32>(kNavOrb);
    win7::DrawPartTo(dc, Module::Wmploc, id, D2D1::RectU(back ? 0 : n, 0, back ? n : 2 * n, n),
                     D2D1::RectF(0, 0, size.width, size.height));
}

void NavGroove(ID2D1DeviceContext* dc, D2D1_SIZE_F size) {
    win7::DrawStretched(dc, Module::Wmploc, L"#" + std::to_wstring(kNavGroove), D2D1::RectF(0, 0, size.width, size.height));
}

}  // namespace glasswave::wmp::transport
