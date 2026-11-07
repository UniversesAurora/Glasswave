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
    const int id = muted ? kMuted : volume <= 0.0f ? kVolume0 : volume < 0.67f ? kVolume1 : kVolume2;
    Bitmap(dc, id, 0, 0, layer == Layer::Pressed ? 0.8f : 1.0f);
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

void NavOrb(ID2D1DeviceContext* dc, Layer layer, bool back, bool enabled) {
    const int id = !enabled                 ? kNavArrowsDisabled
                   : layer == Layer::Hover   ? kNavArrowsHover
                   : layer == Layer::Pressed ? kNavArrowsPressed
                                             : kNavArrows;
    win7::DrawPart(dc, Module::Wmploc, id, D2D1::RectF(back ? 0.0f : kNavOrb, 0, back ? kNavOrb : 2 * kNavOrb, kNavOrb),
                   D2D1::Point2F(0, 0));
}

}  // namespace glasswave::wmp::transport
