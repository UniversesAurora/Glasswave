#include "pch.h"

#include "ui/aero_light.h"

#include "win7/win7_resources.h"

#include <winrt/Windows.Foundation.Collections.h>

namespace glasswave::ui {

namespace wuc = winrt::Windows::UI::Composition;
using winrt::com_ptr;
using winrt::Windows::Foundation::Numerics::float2;
using winrt::Windows::Foundation::Numerics::float3;

namespace {

constexpr auto kEhres = win7::Module::Ehres;

std::chrono::duration<int64_t, std::ratio<1, 10000000>> Seconds(float s) {
    return std::chrono::duration<int64_t, std::ratio<1, 10000000>>(static_cast<int64_t>(s * 1e7f));
}

// A Media Center bitmap as a brush, one bitmap pixel per surface pixel.
wuc::CompositionSurfaceBrush ArtBrush(const wchar_t* resource) {
    auto& host = gfx::CompositionHost::Get();
    const D2D1_SIZE_U size = win7::Resources::Get().Size(kEhres, resource);
    if (size.width == 0 || size.height == 0) return nullptr;
    const auto surface = host.CreateSurface(static_cast<int>(size.width), static_cast<int>(size.height));
    host.Draw(surface, 1.0f, [&](ID2D1DeviceContext* dc) {
        win7::DrawStretched(dc, kEhres, resource,
                            {0, 0, static_cast<float>(size.width), static_cast<float>(size.height)});
    });
    auto brush = host.Compositor().CreateSurfaceBrush(surface);
    brush.Stretch(wuc::CompositionStretch::Fill);
    return brush;
}

} // namespace

AeroLight::AeroLight(const wuc::ContainerVisual& parent, Kind kind, bool atBottom) : m_kind(kind) {
    auto& host = gfx::CompositionHost::Get();
    const auto& comp = host.Compositor();
    m_root = comp.CreateContainerVisual();
    m_root.Clip(comp.CreateInsetClip());
    if (atBottom)
        parent.Children().InsertAtBottom(m_root);
    else
        parent.Children().InsertAtTop(m_root);

    if (kind == Kind::Ground) {
        m_ground = comp.CreateSpriteVisual();
        m_ground.RelativeSizeAdjustment(float2{1, 1});
        if (auto brush = ArtBrush(L"COMMON.BACKGROUND.PNG")) {
            m_ground.Brush(brush);
        } else {
            auto gradient = comp.CreateLinearGradientBrush();
            gradient.StartPoint(float2{0, 0});
            gradient.EndPoint(float2{1, 1});
            gradient.ColorStops().Append(
                comp.CreateColorGradientStop(0.0f, winrt::Windows::UI::ColorHelper::FromArgb(255, 5, 24, 70)));
            gradient.ColorStops().Append(
                comp.CreateColorGradientStop(0.7f, winrt::Windows::UI::ColorHelper::FromArgb(255, 14, 70, 150)));
            gradient.ColorStops().Append(
                comp.CreateColorGradientStop(1.0f, winrt::Windows::UI::ColorHelper::FromArgb(255, 8, 38, 104)));
            m_ground.Brush(gradient);
        }
        m_root.Children().InsertAtTop(m_ground);
    }

    // Soft sheets of light: wide, slow, each on its own period so the pattern never repeats
    // within minutes. Placement {x, y, w, h}, sway and its period, opacity low / high and its
    // period, start delay.
    const float veil = kind == Kind::Veil ? 0.22f : 1.0f;
    if (kind == Kind::Ground) {
        Add(L"COMMON.BACKGROUND.GRAD1.PNG", {nullptr, 0.45f, 0.0f, 0.55f, 0.75f, 0.05f, 41, 0.45f, 0.95f, 23, 0});
        Add(L"COMMON.BACKGROUND.GRAD2.PNG", {nullptr, 0.0f, 0.0f, 0.6f, 0.6f, -0.05f, 37, 0.30f, 0.85f, 29, 4});
    }
    Add(L"COMMON.BACKGROUND.LAYER4.PNG", {nullptr, -0.1f, 0.05f, 1.2f, 0.95f, 0.10f, 47, 0.25f * veil, 0.85f * veil, 17, 0});
    Add(L"COMMON.BACKGROUND.LAYER5.PNG", {nullptr, -0.05f, 0.0f, 1.2f, 1.0f, -0.12f, 59, 0.20f * veil, 0.90f * veil, 21, 6});
    Add(L"COMMON.BACKGROUND.LAYER6.PNG", {nullptr, -0.15f, 0.1f, 1.25f, 0.9f, 0.14f, 53, 0.15f * veil, 0.80f * veil, 26, 11});
    Add(L"COMMON.BACKGROUND.LAYER3.PNG", {nullptr, 0.1f, 0.55f, 0.9f, 0.36f, -0.08f, 43, 0.20f * veil, 0.85f * veil, 19, 3});
    if (kind == Kind::Ground) {
        Add(L"COMMON.BACKGROUND.LAYER1.PNG", {nullptr, 0.62f, 0.38f, 0.26f, 0.6f, 0.06f, 31, 0.25f, 0.9f, 13, 2});
        Add(L"COMMON.BACKGROUND.LAYER2.PNG", {nullptr, 0.18f, 0.42f, 0.34f, 0.56f, -0.07f, 35, 0.15f, 0.8f, 16, 8});
    }
    // Shards: thin beams, leaning, crossing slowly and fading in and out.
    const float beam = kind == Kind::Veil ? 0.62f : 0.85f;
    Add(L"COMMON.BACKGROUND.SHARDA1.PNG", {nullptr, 0.12f, -0.05f, 0.045f, 1.1f, 0.10f, 33, 0.0f, beam, 11, 0, 14});
    Add(L"COMMON.BACKGROUND.SHARDA2.PNG", {nullptr, 0.31f, -0.05f, 0.055f, 1.1f, -0.08f, 39, 0.0f, beam, 14, 5, 20});
    Add(L"COMMON.BACKGROUND.SHARDA3.PNG", {nullptr, 0.52f, -0.05f, 0.085f, 1.1f, 0.12f, 45, 0.0f, beam * 0.9f, 17, 9, 17});
    Add(L"COMMON.BACKGROUND.SHARDB1.PNG", {nullptr, 0.68f, -0.05f, 0.06f, 1.1f, -0.11f, 37, 0.0f, beam, 13, 3, 23});
    Add(L"COMMON.BACKGROUND.SHARDB2.PNG", {nullptr, 0.80f, -0.05f, 0.10f, 1.1f, 0.07f, 51, 0.0f, beam * 0.9f, 19, 12, 16});
    Add(L"COMMON.BACKGROUND.SHARDB3.PNG", {nullptr, 0.40f, -0.05f, 0.075f, 1.1f, -0.13f, 43, 0.0f, beam, 15, 7, 26});
    Add(L"COMMON.BACKGROUND.GOODSHARD.PNG", {nullptr, 0.22f, 0.05f, 0.16f, 0.95f, 0.09f, 49, 0.0f, beam * 0.8f, 23, 14, 0});
    Add(L"COMMON.BACKGROUND.LITESHARD.PNG", {nullptr, 0.58f, 0.0f, 0.26f, 1.0f, -0.10f, 57, 0.0f, beam * 0.8f, 27, 18, 0});
}

AeroLight::~AeroLight() {
    if (m_root) {
        if (auto parent = m_root.Parent()) parent.Children().Remove(m_root);
    }
}

void AeroLight::Add(const wchar_t* resource, Piece piece) {
    auto brush = ArtBrush(resource);
    if (!brush) return;
    piece.visual = gfx::CompositionHost::Get().Compositor().CreateSpriteVisual();
    piece.visual.Brush(brush);
    piece.visual.Opacity(piece.low);
    m_root.Children().InsertAtTop(piece.visual);
    m_pieces.push_back(std::move(piece));
}

void AeroLight::SetSize(SIZE px) {
    if (px.cx == m_size.cx && px.cy == m_size.cy) return;
    m_size = px;
    m_root.Size(float2{static_cast<float>(px.cx), static_cast<float>(px.cy)});
    Layout();
    if (m_running) {
        // The sways are in pixels: start them again for the new size.
        m_started = false;
        m_running = false;
        SetRunning(true);
    }
}

void AeroLight::Layout() {
    const float W = static_cast<float>(m_size.cx), H = static_cast<float>(m_size.cy);
    for (auto& p : m_pieces) {
        p.visual.Size(float2{p.w * W, p.h * H});
        p.visual.CenterPoint(float3{p.w * W / 2, p.h * H / 2, 0});
        p.visual.RotationAngleInDegrees(p.angle);
        p.visual.StopAnimation(L"Offset");
        p.visual.Offset(float3{p.x * W, p.y * H, 0});
    }
}

void AeroLight::Animate(Piece& p) {
    const auto& comp = gfx::CompositionHost::Get().Compositor();
    const float W = static_cast<float>(m_size.cx), H = static_cast<float>(m_size.cy);
    // Slow in, slow out: the light eases through its turning points like a swell.
    const auto ease = comp.CreateCubicBezierEasingFunction({0.37f, 0.0f}, {0.63f, 1.0f});
    if (p.breathSeconds > 0) {
        auto breath = comp.CreateScalarKeyFrameAnimation();
        breath.InsertKeyFrame(0.0f, p.low);
        breath.InsertKeyFrame(0.5f, p.high, ease);
        breath.InsertKeyFrame(1.0f, p.low, ease);
        breath.Duration(Seconds(p.breathSeconds));
        breath.DelayTime(Seconds(p.delay));
        breath.DelayBehavior(wuc::AnimationDelayBehavior::SetInitialValueBeforeDelay);
        breath.IterationBehavior(wuc::AnimationIterationBehavior::Forever);
        p.visual.StartAnimation(L"Opacity", breath);
    }
    if (p.swaySeconds > 0) {
        auto sway = comp.CreateVector3KeyFrameAnimation();
        const float3 from{p.x * W, p.y * H, 0}, to{(p.x + p.sway) * W, p.y * H, 0};
        sway.InsertKeyFrame(0.0f, from);
        sway.InsertKeyFrame(0.5f, to, ease);
        sway.InsertKeyFrame(1.0f, from, ease);
        sway.Duration(Seconds(p.swaySeconds));
        sway.IterationBehavior(wuc::AnimationIterationBehavior::Forever);
        p.visual.StartAnimation(L"Offset", sway);
    }
}

void AeroLight::SetRunning(bool running) {
    if (running == m_running) return;
    m_running = running;
    if (m_size.cx <= 0 || m_size.cy <= 0) {
        m_running = false;
        return;
    }
    if (running && !m_started) {
        m_started = true;
        for (auto& p : m_pieces) Animate(p);
        return;
    }
    for (auto& p : m_pieces) {
        for (const wchar_t* property : {L"Opacity", L"Offset"}) {
            try {
                if (auto controller = p.visual.TryGetAnimationController(property)) {
                    if (running)
                        controller.Resume();
                    else
                        controller.Pause();
                }
            } catch (...) {
            }
        }
    }
}

} // namespace glasswave::ui
