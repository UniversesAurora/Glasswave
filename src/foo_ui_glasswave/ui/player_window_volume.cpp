#include "pch.h"
#include "ui/player_window_parts.h"

// The player window's volume: the flyout, its ball and bubble, the slider and the steps.
namespace glasswave::ui {

using namespace playerwin;

namespace {

constexpr UINT kOverlayMs = 1600;   // full screen: the volume overlay stays this long

constexpr float kFlyoutTrackStart = 8, kFlyoutTrackEnd = 75; // its slider's track in the 84 DIP art

// Its glass frame is 5 DIP all round; the slider art's inside (82 DIP) fills the inside.
constexpr float kFlyoutFrame = 5;

constexpr float kFlyoutSliderX = kFlyoutFrame + (kFlyoutW - 2 * kFlyoutFrame - 84) / 2;

constexpr float kFlyoutSliderY = kFlyoutFrame + (kFlyoutH - 2 * kFlyoutFrame - 17) / 2;

constexpr GlassShape kBallShape{6, 6, 8.5f, 6, 8};

// Fitted to wmploc 2102 / 2103 / 2104 (ball) and 4058 / 3884 / 3883 (pill); rms error over the
// fitted pixels 1.4 - 2.1 levels above the horizon, 4.6 - 7.8 below.
constexpr GlassLook kBallLooks[3] = {
    {{{0xB66CBC, 0x8598D5, 0x9ECAF6}, 1.368f, 1.008f, 1.006f, 0.783f, 0xEDFBFF, 1.000f, -0.561f, 0.881f, 0.351f, 0x0031A4, 0.606f, 0.769f},
     {{0x053BA2, 0x0000C3, 0xB6FFFF}, 1.188f, 1.581f, 0.600f, 0.501f, 0xFFFFFF, 1.000f, 3.509f, 1.216f, 0.498f, 0x00EAFF, 0.656f, 0.693f},
     {{2.5f, 0x4151B4}, {3.5f, 0x4256B6}, {4.5f, 0x4357B6}, {5.5f, 0x3E5AB9}, {6.5f, 0x4D63BD}, {7.5f, 0x566FC6}, {8.5f, 0x303FB1}, {9.5f, 0x151A9B}, {10.5f, 0x2F3698}, {11.5f, 0x414F92}, {12.5f, 0x3A4387}, {13.5f, 0x3E457D}, {14.5f, 0x3F4675}},
     13},
    {{{0xB683D2, 0x94B3EF, 0xAEE0F6}, 1.312f, 0.942f, 0.991f, 0.795f, 0xEFFFFF, 1.000f, -0.246f, 0.826f, 0.407f, 0x004EF5, 0.586f, 0.755f},
     {{0x0846E8, 0x006EFC, 0x09FFFF}, 1.202f, 1.600f, 0.600f, 0.483f, 0xFFFFF9, 1.000f, 3.849f, 1.600f, 1.096f, 0x75FFFF, 0.792f, 0.000f},
     {{2.5f, 0x416BDD}, {3.5f, 0x426EE0}, {4.5f, 0x446FE1}, {5.5f, 0x3E73E9}, {6.5f, 0x517DE2}, {7.5f, 0x6590E7}, {8.5f, 0x375BE0}, {9.5f, 0x142CD5}, {10.5f, 0x2F40CF}, {11.5f, 0x4159C6}, {12.5f, 0x384BCF}, {13.5f, 0x3C4DCD}, {14.5f, 0x3E50CD}},
     13},
    {{{0x7E6084, 0x717698, 0x96BBD1}, 2.567f, 1.067f, 1.210f, 0.802f, 0xE2FBFF, 0.639f, -0.454f, 0.916f, 0.370f, 0x000867, 0.675f, 0.731f},
     {{0x020E61, 0x000056, 0x2BFFFF}, 1.525f, 1.600f, 0.600f, 0.572f, 0xFFFFFF, 1.000f, 3.688f, 1.194f, 0.539f, 0x09B7FF, 0.888f, 0.455f},
     {{2.5f, 0x414450}, {3.5f, 0x3F4252}, {4.5f, 0x3C3F53}, {5.5f, 0x3E446B}, {6.5f, 0x474D79}, {7.5f, 0x515E8D}, {8.5f, 0x2A306C}, {9.5f, 0x120F52}, {10.5f, 0x33326F}, {11.5f, 0x444C85}, {12.5f, 0x3F4683}, {13.5f, 0x424C88}, {14.5f, 0x42508B}},
     13}};

// The flyout's ball with its bitmap's top left at (0, 0), 12x17 DIP.
void PaintFlyoutBall(ID2D1DeviceContext* dc, int state) {
    PaintGlass(dc, kBallShape, kBallLooks[std::clamp(state, 0, 2)]);
}

float SliderFromDb(float db) {
    return db <= -99.0f ? 0.0f : std::clamp(std::pow(10.0f, db / 40.0f), 0.0f, 1.0f);
}

} // namespace

void PlayerWindow::SetPopup(bool open) {
    if (!m_volumePanel) return;
    if (open == m_popupOpen) {
        if (open) PaintPopup();
        return;
    }
    m_popupOpen = open;
    if (open) PaintPopup();
    // Full screen Media Center's overlay; in the mini window WMP's flyout, which simply appears
    // over the button and goes again. Closing closes either (the form may just have changed).
    m_flyoutHot = false;
    m_volumePanel->Show(open && m_mode == Mode::Full);
    m_volumeFlyout.IsVisible(open && m_mode == Mode::Mini);
    m_flyoutGlass.IsVisible(open && m_mode == Mode::Mini);
    if (!open) { // its bubble goes with it
        m_bubbleHover = m_sliderHover;
        KillTimer(m_hwnd, kBubbleTimer);
        if (!m_sliderHover) ShowBubble(false);
    }
    for (int id : {kMute, kVolArrow})
        if (auto* w = WidgetById(id)) w->Invalidate();
    UpdateFlyoutBall();
    UpdateSplitPlate();
}

// WMP 12's volume flyout (its narrow transport's arrow beside the speaker), measured on the
// owner's WMP at 200 % (fly_phys): 92 x 32 DIP over the button, right edge on the arrow's. Its
// frame is 5 DIP of translucent half-DIP rings all round, lit from two sides: a white rim at
// the top and left and a cyan one along the whole foot and right. Over black, from the outside:
//   top / left    #414142 #BFBFC0 #C5C5C6 #545455 #313132 #808081 #6D6D6E #333334 #4D4D4D #B9B9B9
//   foot / right  #003340 #0099BF #07A0C5 #144753 #2F2F2F #595959 #575757 #282828 #494949 #B9B9B9
// then the opaque #F2F2F2 inside. Over the light speaker plate the same rings read much lighter
// (#282828 as #626B70, the cyan as #1EBBE2): the frame is glass. It is drawn over the transport
// blurred, each ring a lighter colour at 45 - 85 % (colour x opacity = the value over black).
// In it WMP's own small slider (wmploc 2100 - 2104), whose inside is exactly the frame's.
void PlayerWindow::PaintFlyout() {
    const RECT& r = m_layout.popup;
    if (Empty(r) || m_mode == Mode::Full) return;
    Place(m_volumeFlyout, r);
    // Under the frame the transport, softly blurred: the frame is a pane of glass.
    Place(m_flyoutGlass, r);
    if (m_frostScale != m_scale) {
        m_frostScale = m_scale;
        m_flyoutGlass.Brush(gfx::MakeFrostBrush(m_flyoutGlass.Compositor(), Px(4.0f)));
    }
    const int w = r.right - r.left, h = r.bottom - r.top;
    gfx::CompositionHost::Get().Repaint(m_volumeFlyout, m_flyoutBacking, w, h, m_scale, [&](ID2D1DeviceContext* dc) {
        wt::FlyoutFrame(dc, D2D1::SizeF(w / m_scale, h / m_scale));
        // WMP's own small slider (wmploc 2100 empty, 2101 filled, 84x17), drawn afresh at any
        // resolution: its groove is a hollow tube three rows high from x 8 to 76 (top #ADADAE,
        // ends #C0BFBF, foot #CCCBCD, inside the background), the fill those three rows solid,
        // each a gradient along the whole groove that the level uncovers.
        const float level = std::clamp(m_volume, 0.0f, 1.0f); // muted, WMP's slider keeps its place
        const D2D1_POINT_2F o{kFlyoutSliderX, kFlyoutSliderY};
        const float cx = kFlyoutTrackStart + level * (kFlyoutTrackEnd - kFlyoutTrackStart);
        com_ptr<ID2D1Factory> factory;
        dc->GetFactory(factory.put());
        dc->DrawRoundedRectangle({{o.x + 8.5f, o.y + 7.5f, o.x + 76.5f, o.y + 9.5f}, 1, 1},
                                 RowBands(dc, o.y + 7, o.y + 10,
                                          {D2D1::ColorF(0xADADAE), D2D1::ColorF(0xC0BFBF), D2D1::ColorF(0xCCCBCD)}, true)
                                     .get(),
                                 1.0f);
        if (cx > kFlyoutTrackStart) {
            static constexpr float kCols[] = {10, 16, 24, 32, 40, 48, 56, 64, 72, 74};
            static constexpr uint32_t kFill[3][10] = {
                {0x95B6D4, 0x96BADA, 0x93BCE1, 0x8FBEE9, 0x8CC0EE, 0x8AC2F3, 0x88C3F8, 0x86C4FB, 0x84C5FE, 0x8BC4F7},
                {0x7F98DF, 0x7591DE, 0x6382DB, 0x4D72D7, 0x3D65D5, 0x305BD2, 0x2351D0, 0x1A49CF, 0x1243CD, 0x1041CD},
                {0x9CB0E6, 0x98AEE8, 0x91AAED, 0x88A5F1, 0x81A1F5, 0x7C9EF8, 0x769BFA, 0x7298FD, 0x6F97FE, 0x7C9EFA}};
            com_ptr<ID2D1RoundedRectangleGeometry> tube;
            factory->CreateRoundedRectangleGeometry({{o.x + 8, o.y + 7, o.x + 77, o.y + 10}, 1.5f, 1.5f}, tube.put());
            dc->PushAxisAlignedClip({o.x, o.y, o.x + cx, o.y + 17}, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
            dc->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), tube.get()), nullptr);
            for (int row = 0; row < 3; ++row) {
                D2D1_GRADIENT_STOP stops[10];
                for (int c = 0; c < 10; ++c) stops[c] = {(kCols[c] + 0.5f - 8) / 69, D2D1::ColorF(kFill[row][c])};
                com_ptr<ID2D1GradientStopCollection> collection;
                dc->CreateGradientStopCollection(stops, 10, collection.put());
                com_ptr<ID2D1LinearGradientBrush> brush;
                dc->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({o.x + 8, 0}, {o.x + 77, 0}),
                                              collection.get(), brush.put());
                dc->FillRectangle({o.x + 8, o.y + 7 + row, o.x + 77, o.y + 8 + row}, brush.get());
            }
            dc->PopLayer();
            dc->PopAxisAlignedClip();
        }
    });
    UpdateFlyoutBall();
}

// The flyout's thumb (wmploc 2102, 2103 lit, 2104 held) in its own visuals over the flyout, so
// that its looks fade into each other: a strip the flyout's height, the ball drawn at the
// slider's place in it.
void PlayerWindow::UpdateFlyoutBall() {
    if (!m_flyoutBall.root) return;
    const RECT& r = m_layout.popup;
    const bool open = m_popupOpen && m_mode == Mode::Mini && !Empty(r);
    m_flyoutBall.root.IsVisible(open);
    if (!open) return;
    const int w = PxI(14), h = r.bottom - r.top;
    m_flyoutBall.Paint(w, h, m_scale, false, [](ID2D1DeviceContext* dc, int state) {
        D2D1_MATRIX_3X2_F base;
        dc->GetTransform(&base);
        dc->SetTransform(D2D1::Matrix3x2F::Translation(1, kFlyoutSliderY) * base);
        PaintFlyoutBall(dc, state);
        dc->SetTransform(base);
    });
    const float level = std::clamp(m_volume, 0.0f, 1.0f);
    const float cx = kFlyoutTrackStart + level * (kFlyoutTrackEnd - kFlyoutTrackStart);
    const int left = r.left + static_cast<int>(std::round((kFlyoutSliderX + cx - 6) * m_scale)) - PxI(1);
    m_flyoutBall.Place(left, r.top, w, h);
    m_flyoutBall.SetState(m_flyoutHot, m_capture == Capture::Volume, 140, 260);
}

// The level in a glass bubble centred over the mini window's volume thumb: the inline
// slider's, the open flyout's, or else over the speaker's button.
void PlayerWindow::ShowBubble(bool show) {
    if (!m_volumeBubble) return;
    const float level = std::clamp(m_volume, 0.0f, 1.0f);
    int x = 0, top = 0;
    if (m_mode == Mode::Full) {
        show = false;
    } else if (m_popupOpen && !Empty(m_layout.popup)) {
        x = m_layout.popup.left +
            PxI(kFlyoutSliderX + kFlyoutTrackStart + level * (kFlyoutTrackEnd - kFlyoutTrackStart));
        top = m_layout.popup.top;
    } else if (!Empty(m_layout.slider)) {
        const RECT& r = m_layout.slider;
        x = m_wmp ? r.left + PxI(kSliderThumbMin + level * (kSliderThumbMax - kSliderThumbMin))
                  : r.left + static_cast<int>(std::lround((r.right - r.left) * level));
        top = r.top + (m_wmp ? PxI(3) : 0);
    } else if (!Empty(m_layout.mute)) {
        x = (m_layout.mute.left + m_layout.mute.right) / 2;
        top = m_layout.mute.top;
    } else {
        show = false;
    }
    if (show) {
        const int w = PxI(WmcVolume::kBubbleW), h = PxI(WmcVolume::kBubbleH);
        const int lo = static_cast<int>(m_layout.client.left) + PxI(2);
        const int left = std::clamp(x - w / 2, lo, std::max(lo, static_cast<int>(m_layout.client.right) - PxI(2) - w));
        m_volumeBubble->SetPosition({left, top - h - PxI(3)}, m_scale);
        m_volumeBubble->SetLevel(m_volume, m_muted);
    }
    m_volumeBubble->Show(show);
}

void PlayerWindow::StepVolume(float delta) {
    // Whole hundredths, so the figure moves by exactly one like everywhere else.
    const float stepped = std::round((m_volume + delta) * 100.0f) / 100.0f;
    m_volume = std::clamp(stepped, 0.0f, 1.0f);
    m_host.SetPlayerVolume(m_volume);
    if (auto* w = WidgetById(kMute)) w->Invalidate();
    if (m_mode == Mode::Full) {
        // Media Center shows the level top right for a moment.
        SetPopup(true);
        SetTimer(m_hwnd, kOverlayTimer, kOverlayMs, nullptr);
    } else { // the bubble over the slider, the flyout or the speaker
        KillTimer(m_hwnd, kBubbleTimer);
        ShowBubble(true);
        SetTimer(m_hwnd, kOverlayTimer, kOverlayMs, nullptr);
    }
    PaintPopup();
    PaintSlider();
}

// The mini window's volume slider: WMP 12's, or without its art Media Center's volume bar
// (OVERLAYS.VOLUME.BAR.*), lit to the level, dimmed while muted.
// WMP 12's inline thumb (wmploc 5679, 5681 lit, 5680 held; 14x23) over the slider's fill.
void PlayerWindow::UpdateSliderThumb() {
    if (!m_sliderThumb.root) return;
    const RECT& r = m_layout.slider;
    const bool on = m_wmp && !Empty(r) && !Wmc();
    m_sliderThumb.root.IsVisible(on);
    if (!on) return;
    const int w = PxI(14), h = PxI(kSliderH);
    m_sliderThumb.Paint(w, h, m_scale, false, [](ID2D1DeviceContext* dc, int state) {
        win7::Draw(dc, kWmploc, state == 2 ? wt::kVolumeThumbPressed : state == 1 ? wt::kVolumeThumbHover : wt::kVolumeThumb,
                   {0, 0});
    });
    const float cx = wt::kThumbMin + std::clamp(m_volume, 0.0f, 1.0f) * (wt::kThumbMax - wt::kThumbMin) -
                     wt::kVolumeRect.left;
    m_sliderThumb.Place(r.left + PxI(std::round(cx - 7)), r.top, w, h);
    m_sliderThumb.SetState(m_sliderHover, m_capture == Capture::Volume && !m_popupOpen, 140, 260);
}

void PlayerWindow::PaintSlider() {
    const RECT& r = m_layout.slider;
    UpdateSliderThumb();
    if (Empty(r)) return;
    const int w = r.right - r.left, h = r.bottom - r.top;
    gfx::CompositionHost::Get().Repaint(m_slider, m_sliderBacking, w, h, m_scale, [&](ID2D1DeviceContext* dc) {
        if (m_wmp) {
            // WMP 12's own: its grey groove (2168, 80x23 like the fill), the fill to the thumb
            // and the thumb, lit under the pointer. The groove runs to x 75 but the thumb stops
            // at 66, which left a stretch of empty groove past a full level: its middle loses
            // 4 DIP so that it ends under the thumb's edge, as in WMP's own transport.
            // The thumb is its own visual (UpdateSliderThumb), so that its looks can fade.
            win7::DrawPartTo(dc, kWmploc, L"#2168", {0, 0, 38, 23}, {0, 0, 38, kSliderH});
            win7::DrawPartTo(dc, kWmploc, L"#2168", {42, 0, 80, 23}, {38, 0, 76, kSliderH});
            const float cx = wt::kThumbMin + std::clamp(m_volume, 0.0f, 1.0f) * (wt::kThumbMax - wt::kThumbMin) -
                             wt::kVolumeRect.left;
            win7::DrawPart(dc, kWmploc, wt::kVolumeFill, D2D1::RectF(0, 0, cx, 23), D2D1::Point2F(0, 0));
            return;
        }
        const float W = w / m_scale, H = h / m_scale;
        const D2D1_RECT_F bar{0, H / 2 - 3.5f, W, H / 2 + 3.5f};
        if (!win7::DrawStretched(dc, kEhres, L"OVERLAYS.VOLUME.BAR.OFF.PNG", bar)) {
            com_ptr<ID2D1SolidColorBrush> off;
            dc->CreateSolidColorBrush(D2D1::ColorF(0x1C2A48), off.put());
            dc->FillRoundedRectangle({bar, 3, 3}, off.get());
        }
        const float x = W * std::clamp(m_volume, 0.0f, 1.0f);
        if (x <= 0) return;
        const float lit = m_muted ? 0.4f : (m_sliderHover || m_capture == Capture::Volume ? 1.0f : 0.92f);
        dc->PushAxisAlignedClip({0, 0, x, H}, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        if (!win7::DrawStretched(dc, kEhres, L"OVERLAYS.VOLUME.BAR.ON.PNG", bar, lit)) {
            com_ptr<ID2D1SolidColorBrush> on;
            dc->CreateSolidColorBrush(D2D1::ColorF(0xBFD9EA, lit), on.put());
            dc->FillRoundedRectangle({bar, 3, 3}, on.get());
        }
        dc->PopAxisAlignedClip();
    });
}

// Media Center's volume overlay: full screen it shows for a moment when the level changes; in
// a narrow mini window the speaker's arrow opens it and its bar can be dragged.
void PlayerWindow::PaintPopup() {
    if (m_volumePanel) m_volumePanel->SetLevel(std::clamp(m_volume, 0.0f, 1.0f), m_muted);
    if (m_popupOpen && m_mode == Mode::Mini) PaintFlyout();
    if (m_volumeBubble && m_volumeBubble->Shown()) ShowBubble(true); // follows the level
}

void PlayerWindow::OnVolume() {
    if (!m_shown) return;
    auto pc = playback_control::get();
    m_muted = pc->is_muted();
    if (!m_muted) m_volume = SliderFromDb(pc->get_volume());
    if (auto* w = WidgetById(kMute)) w->Invalidate();
    if (m_popupOpen) PaintPopup();
}

void PlayerWindow::SetVolumeFromX(int x) {
    RECT bar = m_layout.slider;
    if (m_popupOpen && m_mode == Mode::Full) {
        bar = m_volumePanel->Bar();
    } else if (m_popupOpen) { // the flyout's slider track
        const LONG x0 = m_layout.popup.left + PxI(kFlyoutSliderX + kFlyoutTrackStart);
        bar = {x0, m_layout.popup.top, x0 + PxI(kFlyoutTrackEnd - kFlyoutTrackStart), m_layout.popup.bottom};
    } else if (m_wmp) { // where the inline slider's thumb centre runs
        bar.left = m_layout.slider.left + PxI(kSliderThumbMin);
        bar.right = m_layout.slider.left + PxI(kSliderThumbMax);
    }
    m_volume = std::round(std::clamp(static_cast<float>(x - bar.left) / std::max<LONG>(1, bar.right - bar.left), 0.0f,
                                     1.0f) * 100.0f) / 100.0f;
    m_host.SetPlayerVolume(m_volume);
    PaintPopup();
    PaintSlider();
    KillTimer(m_hwnd, kBubbleTimer);
    ShowBubble(true); // dragging shows the level at once
    if (auto* w = WidgetById(kMute)) w->Invalidate();
}

} // namespace glasswave::ui
