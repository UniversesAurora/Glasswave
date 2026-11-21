#include "pch.h"

#include "ui/wmc_volume.h"

#include "aero/aero_atlas.h"
#include "gfx/effects.h"
#include "ui/pane_common.h"
#include "win7/win7_resources.h"

namespace glasswave::ui {

namespace wuc = winrt::Windows::UI::Composition;
using winrt::com_ptr;
using winrt::Windows::Foundation::Numerics::float2;
using winrt::Windows::Foundation::Numerics::float3;

namespace {

constexpr auto kEhres = win7::Module::Ehres;
// Media Center's overlay, measured on the owner's recording (volume.mp4, 2.23 px per DIP):
// the outline 160 x 86, the plate inside it 150 x 74 from (5, 6.5), the bar its last 10.
constexpr D2D1_RECT_F kPlate{5, 6.5f, 155, 80.5f};
constexpr float kBarH = 10;
constexpr float kFigure = 35;   // DIP, Segoe UI: the figures stand 25 DIP high
constexpr int kInMs = 250, kOutMs = 300;

D2D1_SIZE_F SizeOf(WmcVolume::Kind kind) {
    return kind == WmcVolume::Kind::Panel ? D2D1::SizeF(WmcVolume::kPanelW, WmcVolume::kPanelH)
                                          : D2D1::SizeF(WmcVolume::kBubbleW, WmcVolume::kBubbleH);
}

// The plate when ehres.dll is missing: OVERLAYS.BACKGROUND.DARK's gradient.
void PlateFallback(ID2D1DeviceContext* dc, D2D1_RECT_F r) {
    const D2D1_GRADIENT_STOP stops[] = {{0, D2D1::ColorF(0x17406F, 0.90f)}, {0.85f, D2D1::ColorF(0x4C8AC1, 0.96f)},
                                        {1, D2D1::ColorF(0x4C8AC1, 0.96f)}};
    com_ptr<ID2D1GradientStopCollection> collection;
    dc->CreateGradientStopCollection(stops, 3, collection.put());
    com_ptr<ID2D1LinearGradientBrush> brush;
    dc->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({0, r.top}, {0, r.bottom}), collection.get(),
                                  brush.put());
    dc->FillRectangle(r, brush.get());
}

// OVERLAYS.OUTLINE as a nine-grid whose borders keep their size in DIP (one bitmap pixel per
// DIP, as Media Center scales it with the screen): drawn one bitmap pixel per device pixel, at
// 200 % its rim came out half as wide as on the recording.
bool OutlineNineGrid(ID2D1DeviceContext* dc, D2D1_RECT_F dst) {
    constexpr float m = 8;
    constexpr UINT32 w = 287, h = 86, mi = 8;
    const float xs[4] = {dst.left, dst.left + m, dst.right - m, dst.right};
    const float ys[4] = {dst.top, dst.top + m, dst.bottom - m, dst.bottom};
    const UINT32 sx[4] = {0, mi, w - mi, w}, sy[4] = {0, mi, h - mi, h};
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            if (!win7::DrawPartTo(dc, kEhres, L"OVERLAYS.OUTLINE.PNG", {sx[c], sy[r], sx[c + 1], sy[r + 1]},
                                  {xs[c], ys[r], xs[c + 1], ys[r + 1]}))
                return false;
    return true;
}

} // namespace

WmcVolume::WmcVolume(const wuc::ContainerVisual& parent, Kind kind) : m_kind(kind) {
    const auto comp = gfx::CompositionHost::Get().Compositor();
    m_deviceReplaced = gfx::CompositionHost::Get().OnDeviceReplaced([this] { Paint(); });
    m_root = comp.CreateContainerVisual();
    m_root.Opacity(0.0f);
    m_root.IsVisible(false);
    parent.Children().InsertAtTop(m_root);
    // The window's content behind it, blurred, in its shape: the bubble's frost, and under the
    // panel's translucent outline clear glass that deepens and mixes the colours behind (the
    // owner: iOS's materials rather than a bare see-through rim).
    m_glass = comp.CreateSpriteVisual();
    m_glassShape = comp.CreateRoundedRectangleGeometry();
    m_glass.Clip(comp.CreateGeometricClip(m_glassShape));
    m_root.Children().InsertAtTop(m_glass);
    m_visual = comp.CreateSpriteVisual();
    m_root.Children().InsertAtTop(m_visual);
    if (kind == Kind::Panel) {
        m_barOn = comp.CreateSpriteVisual();
        m_barClip = comp.CreateInsetClip();
        m_barOn.Clip(m_barClip);
        m_root.Children().InsertAtTop(m_barOn);
    }
}

WmcVolume::~WmcVolume() {
    if (m_root)
        if (auto parent = m_root.Parent()) parent.Children().Remove(m_root);
}

void WmcVolume::SetPosition(POINT topLeft, float scale) {
    const bool rescale = scale != m_scale;
    m_at = topLeft;
    m_scale = scale;
    const D2D1_SIZE_F s = SizeOf(m_kind);
    const float2 size{std::round(s.width * scale), std::round(s.height * scale)};
    m_root.Offset(float3{static_cast<float>(topLeft.x), static_cast<float>(topLeft.y), 0});
    m_visual.Size(size);
    m_glass.Size(size);
    m_glassShape.Offset(float2{0.5f * scale, 0.5f * scale});
    m_glassShape.Size(float2{size.x - scale, size.y - scale});
    const float radius = (m_kind == Kind::Panel ? kPanelRadius : kBubbleRadius) * scale;
    m_glassShape.CornerRadius(float2{radius, radius});
    if (rescale)
        m_glass.Brush(m_kind == Kind::Panel
                          ? gfx::MakeGlassBrush(m_glass.Compositor(), 10.0f * scale, 1.7f, D2D1::ColorF(0x9CC8F0, 0.16f))
                          : gfx::MakeFrostBrush(m_glass.Compositor(), 2.5f * scale));
    if (m_barOn) {
        const auto px = [&](float dip) { return std::round(dip * scale); };
        m_barOn.Offset(float3{px(kPlate.left), px(kPlate.bottom - kBarH), 0});
        m_barOn.Size(float2{px(kPlate.right) - px(kPlate.left), px(kPlate.bottom) - px(kPlate.bottom - kBarH)});
    }
    if (rescale && m_level >= 0) Paint();
}

void WmcVolume::SetLevel(float level, bool muted) {
    level = std::clamp(level, 0.0f, 1.0f);
    const bool same = std::lround(level * 1000) == std::lround(m_level * 1000) && muted == m_muted;
    const bool first = m_level < 0;
    m_level = level;
    m_muted = muted;
    if (!same && m_scale > 0) Paint();
    PlaceLevel(!first && m_shown);
}

// The lit bar ends at the level. A change does not jump there: the end springs to it (a quick
// spring that just overshoots and settles), so a run of steps reads as one fluid motion.
void WmcVolume::PlaceLevel(bool animate) {
    if (!m_barOn || m_scale <= 0) return;
    const float width = m_barOn.Size().x;
    const float inset = std::round(width * (1 - std::max(0.0f, m_level)));
    if (animate) {
        try {
            auto spring = m_barOn.Compositor().CreateSpringScalarAnimation();
            spring.FinalValue(inset);
            spring.DampingRatio(0.72f);
            spring.Period(std::chrono::milliseconds(48));
            m_barClip.StartAnimation(L"RightInset", spring);
            return;
        } catch (...) {
        }
    }
    m_barClip.StopAnimation(L"RightInset");
    m_barClip.RightInset(inset);
}

// Media Center's overlay neither moves nor grows: it fades in quickly where it stands and
// fades out a little slower. The bubble, a tooltip, is quicker still.
void WmcVolume::Show(bool show) {
    if (show == m_shown) return;
    m_shown = show;
    *m_showing = show;
    const auto& comp = m_root.Compositor();
    if (show) {
        if (m_scale > 0) Paint();
        PlaceLevel(false); // it appears showing the level
        m_root.IsVisible(true);
    }
    const bool bubble = m_kind == Kind::Bubble;
    auto batch = comp.CreateScopedBatch(wuc::CompositionBatchTypes::Animation);
    auto fade = comp.CreateScalarKeyFrameAnimation();
    fade.InsertKeyFrame(1.0f, show ? 1.0f : 0.0f,
                        show ? comp.CreateCubicBezierEasingFunction({0.25f, 0.1f}, {0.25f, 1.0f})
                             : comp.CreateCubicBezierEasingFunction({0.4f, 0.0f}, {0.6f, 1.0f}));
    fade.Duration(std::chrono::milliseconds(show ? (bubble ? 140 : kInMs) : (bubble ? 220 : kOutMs)));
    m_root.StartAnimation(L"Opacity", fade);
    batch.End();
    if (!show) {
        batch.Completed([visual = m_root, showing = std::weak_ptr<bool>(m_showing)](const auto&, const auto&) {
            if (auto still = showing.lock(); still && !*still) visual.IsVisible(false);
        });
    }
}

RECT WmcVolume::Bounds() const {
    const D2D1_SIZE_F s = SizeOf(m_kind);
    return {m_at.x, m_at.y, m_at.x + static_cast<LONG>(std::lround(s.width * m_scale)),
            m_at.y + static_cast<LONG>(std::lround(s.height * m_scale))};
}

RECT WmcVolume::Bar() const {
    if (m_kind != Kind::Panel) return {};
    const auto px = [&](float dip) { return static_cast<LONG>(std::lround(dip * m_scale)); };
    return {m_at.x + px(kPlate.left), m_at.y + px(kPlate.bottom - kBarH), m_at.x + px(kPlate.right),
            m_at.y + px(kPlate.bottom)};
}

void WmcVolume::Paint() {
    const D2D1_SIZE_F s = SizeOf(m_kind);
    const int w = static_cast<int>(std::round(s.width * m_scale)), h = static_cast<int>(std::round(s.height * m_scale));
    const bool panel = m_kind == Kind::Panel;
    gfx::CompositionHost::Get().Repaint(m_visual, m_backing, w, h, m_scale, [&](ID2D1DeviceContext* dc) {
        if (!panel) {
            PaintBubble(dc);
            return;
        }
        const D2D1_RECT_F box{0, 0, s.width, s.height};
        // The bubble keeps the panel's proportions of rim to plate.
        const D2D1_RECT_F plate = panel ? kPlate : D2D1_RECT_F{3, 3.5f, s.width - 3, s.height - 3};
        if (!OutlineNineGrid(dc, box)) {
            com_ptr<ID2D1SolidColorBrush> glass;
            dc->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 0.16f), glass.put());
            dc->FillRoundedRectangle({box, 4, 4}, glass.get());
        }
        if (!win7::DrawStretched(dc, kEhres, L"OVERLAYS.BACKGROUND.DARK.PNG", plate)) PlateFallback(dc, plate);
        D2D1_RECT_F face = plate;
        if (panel) {
            // The bar along the plate's foot: the level lit, cut square where it ends.
            const D2D1_RECT_F bar{plate.left, plate.bottom - kBarH, plate.right, plate.bottom};
            face.bottom = bar.top;
            if (!win7::DrawStretched(dc, kEhres, L"OVERLAYS.VOLUME.BAR.OFF.PNG", bar)) {
                com_ptr<ID2D1SolidColorBrush> off;
                dc->CreateSolidColorBrush(D2D1::ColorF(0x1C2A48), off.put());
                dc->FillRectangle(bar, off.get());
            }
            // The lit part is m_barOn, over this.
        }
        const D2D1_POINT_2F mid{(face.left + face.right) / 2, (face.top + face.bottom) / 2};
        if (m_muted) {
            constexpr float k = 1.0f;
            const D2D1_RECT_F mark{mid.x - 20.5f * k, mid.y - 18 * k, mid.x + 20.5f * k, mid.y + 18 * k};
            if (win7::DrawStretched(dc, kEhres, L"OVERLAYS.VOLUME.MUTE.PNG", mark)) return;
            pane::DrawIcon(dc, L'\xE74F', 26 * k, mid, pane::White(dc, 1.0f).get());
            return;
        }
        // Media Center's white (#F2F2F2) in a light cut: the recording's strokes are about 2 DIP.
        com_ptr<ID2D1SolidColorBrush> ink;
        dc->CreateSolidColorBrush(D2D1::ColorF(0xF2F2F2), ink.put());
        pane::DrawTextLine(dc, std::to_wstring(std::lround(m_level * 100)), kFigure, DWRITE_FONT_WEIGHT_SEMI_LIGHT,
                           {face.left, mid.y - kFigure * 0.69f}, face.right - face.left, ink.get(),
                           DWRITE_TEXT_ALIGNMENT_CENTER);
    });
    if (m_barOn) {
        const float2 size = m_barOn.Size();
        gfx::CompositionHost::Get().Repaint(m_barOn, m_barBacking, static_cast<int>(size.x), static_cast<int>(size.y),
                                            m_scale, [&](ID2D1DeviceContext* dc) {
                                                const D2D1_RECT_F bar{0, 0, size.x / m_scale, size.y / m_scale};
                                                if (!win7::DrawStretched(dc, kEhres, L"OVERLAYS.VOLUME.BAR.ON.PNG", bar)) {
                                                    com_ptr<ID2D1SolidColorBrush> on;
                                                    dc->CreateSolidColorBrush(D2D1::ColorF(0xBFD9EA), on.put());
                                                    dc->FillRectangle(bar, on.get());
                                                }
                                            });
    }
}

// The bubble is a small pane of the window frame's glass over the frosted window behind it
// (m_glass): Aero's tint, brighter at the top; the frame's own reflection streaks and top-left
// glare from DWM's atlas; a gloss over the upper half and a cyan glow rising from the foot; a
// dark outer edge and a light inner one, as Aero draws a window's border. The figure is white
// on a soft dark glow.
void WmcVolume::PaintBubble(ID2D1DeviceContext* dc) {
    const float W = kBubbleW, H = kBubbleH, B = H;
    com_ptr<ID2D1Factory> factory;
    dc->GetFactory(factory.put());
    com_ptr<ID2D1RoundedRectangleGeometry> shape;
    factory->CreateRoundedRectangleGeometry({{0.5f, 0.5f, W - 0.5f, H - 0.5f}, kBubbleRadius, kBubbleRadius},
                                            shape.put());

    const auto gradient = [&](std::initializer_list<D2D1_GRADIENT_STOP> list, float y0, float y1) {
        std::vector<D2D1_GRADIENT_STOP> stops(list);
        com_ptr<ID2D1GradientStopCollection> collection;
        dc->CreateGradientStopCollection(stops.data(), static_cast<UINT32>(stops.size()), collection.put());
        com_ptr<ID2D1LinearGradientBrush> brush;
        dc->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({0, y0}, {0, y1}), collection.get(),
                                      brush.put());
        return brush;
    };
    // The tint.
    dc->FillGeometry(shape.get(), gradient({{0.00f, D2D1::ColorF(0xC4D8EC, 0.24f)},
                                            {0.48f, D2D1::ColorF(0x8AA6C6, 0.14f)},
                                            {0.52f, D2D1::ColorF(0x5A7AA4, 0.18f)},
                                            {1.00f, D2D1::ColorF(0x3A5E90, 0.26f)}},
                                           0, H)
                                      .get());
    dc->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), shape.get()), nullptr);
    D2D1_MATRIX_3X2_F base;
    dc->GetTransform(&base);
    if (aero::atlas::Available()) {
        // The frame's reflection streaks, a slice of the texture DWM stretches over a screen,
        // and its glare in the top-left corner.
        dc->SetTransform(D2D1::Matrix3x2F::Translation(-W * 2.2f, -H * 3.0f) * base);
        aero::atlas::Streaks(dc, {W * 6.0f, H * 8.0f});
        dc->SetTransform(base);
        aero::atlas::Glare(dc, true, {W * 0.9f, H * 1.3f});
    }
    // The gloss over the upper half, and the cyan glow rising from the foot.
    dc->FillRectangle({0, 0, W, B * 0.5f},
                      gradient({{0, D2D1::ColorF(1, 1, 1, 0.34f)}, {1, D2D1::ColorF(1, 1, 1, 0.06f)}}, 0, B * 0.5f).get());
    {
        const D2D1_GRADIENT_STOP stops[] = {{0.0f, D2D1::ColorF(0xA8F0FF, 0.50f)},
                                            {0.55f, D2D1::ColorF(0x5CC8F4, 0.14f)},
                                            {1.0f, D2D1::ColorF(0x3AA8E8, 0.0f)}};
        com_ptr<ID2D1GradientStopCollection> collection;
        dc->CreateGradientStopCollection(stops, 3, collection.put());
        com_ptr<ID2D1RadialGradientBrush> glow;
        dc->CreateRadialGradientBrush(D2D1::RadialGradientBrushProperties({W / 2, H + 1}, {0, 0}, W * 0.55f, H * 0.62f),
                                      collection.get(), glow.put());
        dc->FillRectangle({0, 0, W, H}, glow.get());
    }
    dc->PopLayer();
    // The light inner edge, then the dark outer one.
    dc->DrawRoundedRectangle({{1.5f, 1.5f, W - 1.5f, B - 1.5f}, kBubbleRadius - 1, kBubbleRadius - 1},
                             gradient({{0, D2D1::ColorF(1, 1, 1, 0.78f)}, {0.5f, D2D1::ColorF(1, 1, 1, 0.34f)},
                                       {1, D2D1::ColorF(0xC8F4FF, 0.55f)}},
                                      1, B)
                                 .get(),
                             1.0f);
    com_ptr<ID2D1SolidColorBrush> rim;
    dc->CreateSolidColorBrush(D2D1::ColorF(0x0A1420, 0.70f), rim.put());
    dc->DrawGeometry(shape.get(), rim.get(), 1.0f);

    // The figure (the speaker with its cross while muted), white on a soft dark glow.
    constexpr float kSize = 13;
    const D2D1_POINT_2F mid{W / 2, B / 2 + 0.5f};
    com_ptr<ID2D1SolidColorBrush> glow, ink;
    dc->CreateSolidColorBrush(D2D1::ColorF(0x000000, 0.30f), glow.put());
    dc->CreateSolidColorBrush(D2D1::ColorF(0xFFFFFF), ink.put());
    if (m_muted) {
        for (const D2D1_POINT_2F d : {D2D1_POINT_2F{0, 1}, D2D1_POINT_2F{1, 0}, D2D1_POINT_2F{-1, 0}})
            pane::DrawIcon(dc, L'\xE74F', kSize, {mid.x + d.x, mid.y + d.y}, glow.get());
        pane::DrawIcon(dc, L'\xE74F', kSize, mid, ink.get());
        return;
    }
    const std::wstring text = std::to_wstring(std::lround(m_level * 100));
    const D2D1_POINT_2F at{0, mid.y - kSize * 0.69f};
    for (const D2D1_POINT_2F d : {D2D1_POINT_2F{0, 1}, D2D1_POINT_2F{1, 0}, D2D1_POINT_2F{-1, 0}, D2D1_POINT_2F{0, -1}})
        pane::DrawTextLine(dc, text, kSize, DWRITE_FONT_WEIGHT_SEMI_BOLD, {at.x + d.x, at.y + d.y}, W, glow.get(),
                           DWRITE_TEXT_ALIGNMENT_CENTER);
    pane::DrawTextLine(dc, text, kSize, DWRITE_FONT_WEIGHT_SEMI_BOLD, at, W, ink.get(), DWRITE_TEXT_ALIGNMENT_CENTER);
}

} // namespace glasswave::ui
