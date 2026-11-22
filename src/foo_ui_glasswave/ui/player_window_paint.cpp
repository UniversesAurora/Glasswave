#include "pch.h"
#include "ui/player_window_parts.h"

// The player window's painted surfaces: frame, pane card, the wall's shade, the bars,
// the song's lines and the cover.
namespace glasswave::ui {

using namespace playerwin;

namespace {

constexpr uint32_t kNavy = 0x0B2C6B; // WMC's blue over the dimmed tiles

constexpr uint32_t kCardRim = 0x2F64B4; // the full-screen card's clear rim: a lighter blue than kNavy

constexpr uint32_t kTimeBlue = 0x8CCBFF;

constexpr float kCardPad = 10, kCardShadow = 32; // DIP: full screen pane card around the pane

// A filled rounded rectangle whose edge fades out over `feather`.
void FeatherRect(ID2D1DeviceContext* dc, D2D1_RECT_F r, float feather, uint32_t rgb, float alpha) {
    constexpr int kSteps = 32;
    const float each = 1.0f - std::pow(1.0f - alpha, 1.0f / kSteps);
    com_ptr<ID2D1SolidColorBrush> brush;
    dc->CreateSolidColorBrush(D2D1::ColorF(rgb, each), brush.put());
    for (int i = 0; i < kSteps; ++i) {
        const float d = feather * (1.0f - static_cast<float>(i) / (kSteps - 1));
        const float rad = d + 4;
        dc->FillRoundedRectangle({{r.left - d, r.top - d, r.right + d, r.bottom + d}, rad, rad}, brush.get());
    }
}

} // namespace

// The mini window's Win7 frame lines; one device pixel at any DPI, as DWM draws them.
void PlayerWindow::PaintFrame() {
    m_framePainted = m_scale;
    const float s = m_scale;
    m_frame->SetScale(s);
    const int edgeSrc = PxI(16);
    m_clientEdge.Brush(gfx::CompositionHost::Get().CreateNineGrid(
        edgeSrc, edgeSrc, s, 3.0f, true,
        [&](ID2D1DeviceContext* dc) { paint::ClientEdge(dc, D2D1::SizeF(edgeSrc / s, edgeSrc / s)); }));
}

// Full screen the lyrics / queue lie on a pane of Media Center's overlay glass, built like its
// volume overlay (OVERLAYS.OUTLINE around OVERLAYS.BACKGROUND.GRADIENT), floating over the wall
// on a soft shadow. It belongs to the pane's layer: it arrives and leaves with it.
// Inside the outline, instead of the overlay's deep opaque blue, clear glass (owner,
// 2026-11-13): the wall behind blurred (14 DIP) and its colours deepened (x 1.5), mixed with
// Media Center's navy at 42 %, about as deep as the blue behind the song's lines and the menu;
// like Win7's window frame but a little heavier, and not frosted.
void PlayerWindow::PaintPaneCard() {
    const bool show = m_mode == Mode::Full && m_paneShown && !Empty(m_layout.panel);
    m_paneCard.IsVisible(show);
    m_paneGlass.IsVisible(show);
    if (!show) return;
    if (EqualRect(&m_layout.panel, &m_paneCardPanel) && m_paneCardScale == m_scale) return;
    m_paneCardPanel = m_layout.panel;
    m_paneCardScale = m_scale;
    const int margin = PxI(kCardShadow);
    const RECT& p = m_layout.panel;
    const RECT r{p.left - PxI(kCardPad) - margin, p.top - PxI(kCardPad) - margin, p.right + PxI(kCardPad) + margin,
                 p.bottom + PxI(kCardPad) + margin};
    Place(m_paneCard, r);
    const int w = r.right - r.left, h = r.bottom - r.top;
    // The plate inside the outline (card inset 5, 6.5, 5, 5.5), in px. The glass reaches a
    // device pixel under the rim: where the card's hole and the glass met edge to edge, both
    // half-covered pixels let the bright wall through, a pale arc in each corner (owner,
    // 2026-11-14).
    const int lap = std::max(1, static_cast<int>(std::floor(m_scale)));
    const RECT plate{r.left + margin + PxI(5) - lap, r.top + margin + PxI(6.5f) - lap,
                     r.right - margin - PxI(5) + lap, r.bottom - margin - PxI(5.5f) + lap};
    Place(m_paneGlass, plate);
    m_paneGlassShape.Size(float2{static_cast<float>(plate.right - plate.left), static_cast<float>(plate.bottom - plate.top)});
    m_paneGlassShape.CornerRadius(float2{Px(3) + lap, Px(3) + lap});
    if (m_paneGlassScale != m_scale) {
        m_paneGlassScale = m_scale;
        m_paneGlass.Brush(gfx::MakeGlassBrush(m_paneGlass.Compositor(), Px(14), 1.5f, D2D1::ColorF(kNavy, 0.42f)));
    }
    gfx::CompositionHost::Get().Repaint(m_paneCard, m_paneCardBacking, w, h, m_scale, [&](ID2D1DeviceContext* dc) {
        const float W = w / m_scale, H = h / m_scale;
        const D2D1_RECT_F card{kCardShadow, kCardShadow, W - kCardShadow, H - kCardShadow};
        FeatherRect(dc, {card.left + 6, card.top + 10, card.right - 6, card.bottom + 2}, kCardShadow - 4, 0x000814, 0.55f);
        com_ptr<ID2D1Factory> factory;
        dc->GetFactory(factory.put());
        // The clear rim round the glass is blue, unblurred and paler than the glass, yet blue
        // enough to read as blue (owner, 2026-11-13 / 14: navy at 20 % was hardly seen). The
        // shadow under it is cleared first so the rim is as even as the glass.
        {
            // The radius of OVERLAYS.OUTLINE's own corner (a white plate with a bright edge,
            // about 3 px round): a rounder rim left a pale crescent of the plate in each corner
            // between the two curves (owner, 2026-11-14).
            com_ptr<ID2D1RoundedRectangleGeometry> outer;
            factory->CreateRoundedRectangleGeometry({card, 3, 3}, outer.put());
            com_ptr<ID2D1SolidColorBrush> rim;
            dc->CreateSolidColorBrush(D2D1::ColorF(kCardRim, 0.36f), rim.put());
            dc->SetPrimitiveBlend(D2D1_PRIMITIVE_BLEND_COPY);
            dc->FillGeometry(outer.get(), rim.get());
            dc->SetPrimitiveBlend(D2D1_PRIMITIVE_BLEND_SOURCE_OVER);
        }
        // Nothing of the card under the glass: it blurs what lies behind the card, not its shadow.
        com_ptr<ID2D1RoundedRectangleGeometry> round;
        factory->CreateRoundedRectangleGeometry(
            {{card.left + 5, card.top + 6.5f, card.right - 5, card.bottom - 5.5f}, 3, 3}, round.put());
        com_ptr<ID2D1SolidColorBrush> clear;
        dc->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 0), clear.put());
        dc->SetPrimitiveBlend(D2D1_PRIMITIVE_BLEND_COPY);
        dc->FillGeometry(round.get(), clear.get());
        dc->SetPrimitiveBlend(D2D1_PRIMITIVE_BLEND_SOURCE_OVER);
        if (!(m_ehres && win7::DrawNineGrid(dc, kEhres, L"OVERLAYS.OUTLINE.PNG", {0, 0, 287, 86}, {8, 8, 8, 8}, card)))
            FeatherRect(dc, card, 0, 0x9CC8F0, 0.10f);
    });
}

// The wall is laid from the window's corner and moves with it. Its tiles have the full
// screen's size in both forms, so the mini window's wall is the full screen's top left: going
// full screen keeps those tiles and the rest of the screen fills by falling tiles.
void PlayerWindow::PlaceWall() {
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromWindow(m_hwnd, MONITOR_DEFAULTTONEAREST), &mi);
    m_wall->SetGeometry(m_client, {mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top},
                        m_scale);
}

// The blue that WMC lays over the tiles behind its text, painted small and stretched. With
// ehres.dll it is WMC's own gradient (NOWPLAYING.MUSIC.META.GRADIENT).
void PlayerWindow::PaintShade() {
    constexpr int kDown = 8;
    const int w = std::max<int>(1, m_client.cx / kDown), h = std::max<int>(1, m_client.cy / kDown);
    auto& host = gfx::CompositionHost::Get();
    const auto surface = host.CreateSurface(w, h);
    const float W = m_client.cx / m_scale, H = m_client.cy / m_scale;
    const Layout& L = m_layout;
    const float s = m_scale;
    const auto dip = [s](const RECT& r) { return D2D1_RECT_F{r.left / s, r.top / s, r.right / s, r.bottom / s}; };
    host.Draw(surface, m_scale / kDown, [&](ID2D1DeviceContext* dc) {
        com_ptr<ID2D1SolidColorBrush> base;
        dc->CreateSolidColorBrush(D2D1::ColorF(kNavy, 0.22f), base.put());
        dc->FillRectangle({0, 0, W, H}, base.get());
        if (!Empty(L.panel)) {
            const D2D1_RECT_F p = dip(L.panel);
            const auto vertical = [&](float top, float bottom, std::initializer_list<D2D1_GRADIENT_STOP> stops) {
                com_ptr<ID2D1GradientStopCollection> collection;
                dc->CreateGradientStopCollection(stops.begin(), static_cast<UINT32>(stops.size()), collection.put());
                com_ptr<ID2D1LinearGradientBrush> brush;
                dc->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({0, top}, {0, bottom}),
                                              collection.get(), brush.put());
                return brush;
            };
            if (m_mode == Mode::Full) {
                // Full screen the lyrics / queue stand on their own pane of glass (PaintPaneCard);
                // the wall stays lit around it.
            } else {
                // The mini window's pane fills it: the wall sinks into deep water from the top
                // down and meets the bar's own dark, with no edge anywhere.
                dc->FillRectangle({0, 0, W, H}, vertical(p.top, p.bottom + 40,
                                                         {{0, D2D1::ColorF(0x061C46, 0.58f)},
                                                          {0.16f, D2D1::ColorF(0x051A42, 0.80f)},
                                                          {0.84f, D2D1::ColorF(0x041638, 0.84f)},
                                                          {1, D2D1::ColorF(0x030F2A, 0.88f)}})
                                                    .get());
                const float r = (p.right - p.left) * 0.75f;
                const D2D1_GRADIENT_STOP glow[] = {{0, D2D1::ColorF(0x3070C8, 0.20f)}, {1, D2D1::ColorF(0x3070C8, 0.0f)}};
                com_ptr<ID2D1GradientStopCollection> collection;
                dc->CreateGradientStopCollection(glow, 2, collection.put());
                com_ptr<ID2D1RadialGradientBrush> light;
                const D2D1_POINT_2F c{(p.left + p.right) / 2, p.top - r * 0.25f};
                dc->CreateRadialGradientBrush(D2D1::RadialGradientBrushProperties(c, {0, 0}, r, r * 0.7f),
                                              collection.get(), light.put());
                dc->FillRectangle({0, 0, W, H}, light.get());
            }
        }
        if (Empty(L.info) || (m_paneShown && m_mode == Mode::Mini)) return;
        const D2D1_RECT_F info = dip(L.info), cover = dip(L.cover);
        D2D1_RECT_F band{};
        if (L.infoStyle == 1) {
            const float pad = (cover.bottom - cover.top) * 0.22f;
            // In Media Center the blue runs on to the screen's right edge (change.mp4).
            band = {cover.left - pad * 2.2f, cover.top - pad, std::max(info.right + pad * 1.5f, W + pad), cover.bottom + pad};
        } else if (L.infoStyle == 0) {
            const float mid = (info.left + info.right) / 2;
            float half = std::min((info.right - info.left) / 2 + 30, 290.0f * L.textScale);
            if (m_mode == Mode::Full && !Empty(L.panel)) {
                const float maxRight = dip(L.panel).left - 12.0f;
                if (mid + half > maxRight) half = std::max(0.0f, maxRight - mid);
            }
            const float bottom = Empty(L.lyric) ? info.bottom : std::max(info.bottom, dip(L.lyric).bottom);
            band = {mid - half, info.top - 30, mid + half, bottom + 6};
        } else {
            band = {-20, cover.top - 30, W + 20, cover.bottom + 30};
        }
        if (!m_ehres || !win7::DrawStretched(dc, kEhres, L"NOWPLAYING.MUSIC.META.GRADIENT.PNG", band)) {
            const float f = (band.bottom - band.top) * 0.2f;
            FeatherRect(dc, {band.left + f, band.top + f, band.right - f, band.bottom - f}, f, kNavy, 0.76f);
        }
    });
    auto brush = host.Compositor().CreateSurfaceBrush(surface);
    brush.Stretch(wuc::CompositionStretch::Fill);
    m_shade.Brush(brush);
}

// Media Center's own shade behind its menu (NOWPLAYING.MUSIC.LEFTMENU.GRADIENT). It belongs to
// the menu's layer, so it leaves and returns with it.
void PlayerWindow::PaintMenuShade() {
    const RECT& r = m_layout.menuShade;
    if (Empty(r)) return;
    // At full size: Repaint sizes the visual to its surface and does not stretch it (painted at
    // a quarter, the shade covered only the quarter of its place at the top left).
    const int w = std::max<int>(1, r.right - r.left), h = std::max<int>(1, r.bottom - r.top);
    const float W = w / m_scale, H = h / m_scale;
    // The bitmap is still 30 - 40 % opaque along its top and bottom rows: stretched into a box it
    // ended in two straight edges. On the recordings the shade is a soft, roundish pool over the
    // left of the screen, so here it lies under an elliptical falloff from the middle of the left
    // edge (whole to half its radius, gone at the ellipse through the box's corners' midpoints).
    gfx::CompositionHost::Get().Repaint(m_menuShade, m_menuShadeBacking, w, h, m_scale, [&](ID2D1DeviceContext* dc) {
        const D2D1_GRADIENT_STOP stops[] = {{0.0f, D2D1::ColorF(1, 1, 1, 1)}, {0.5f, D2D1::ColorF(1, 1, 1, 1)},
                                            {0.75f, D2D1::ColorF(1, 1, 1, 0.55f)}, {1.0f, D2D1::ColorF(1, 1, 1, 0)}};
        com_ptr<ID2D1GradientStopCollection> collection;
        dc->CreateGradientStopCollection(stops, 4, collection.put());
        com_ptr<ID2D1RadialGradientBrush> pool;
        dc->CreateRadialGradientBrush(D2D1::RadialGradientBrushProperties({0, H / 2}, {0, 0}, W, H / 2),
                                      collection.get(), pool.put());
        dc->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                            D2D1::IdentityMatrix(), 1.0f, pool.get()),
                      nullptr);
        if (!(m_ehres && win7::DrawStretched(dc, kEhres, L"NOWPLAYING.MUSIC.LEFTMENU.GRADIENT.PNG", {0, 0, W, H}))) {
            com_ptr<ID2D1SolidColorBrush> navy;
            dc->CreateSolidColorBrush(D2D1::ColorF(kNavy, 0.78f), navy.put());
            dc->FillRectangle({0, 0, W, H}, navy.get());
        }
        dc->PopLayer();
    });
}

// Backgrounds of the control bar and (full screen) the top bar.
void PlayerWindow::PaintBars() {
    auto& host = gfx::CompositionHost::Get();
    const Layout& L = m_layout;
    const float s = m_scale;
    const bool wmc = Wmc();
    {
        const int w = L.bar.right - L.bar.left, h = L.bar.bottom - L.bar.top;
        const float W = w / s, H = h / s;
        host.Repaint(m_barBack, m_barBacking, w, h, s, [&](ID2D1DeviceContext* dc) {
            if (wmc) {
                // Media Center's full-screen bottom bar, its two halves meeting in the middle.
                win7::DrawStretched(dc, kEhres, L"TWOFOOT.FULLSCREEN.BOTTOMBAR.LEFT.PNG", {0, 0, W / 2, H});
                win7::DrawStretched(dc, kEhres, L"TWOFOOT.FULLSCREEN.BOTTOMBAR.RIGHT.PNG", {W / 2, 0, W, H});
                for (const RECT& d : L.divider)
                    win7::DrawStretched(dc, kEhres, L"TWOFOOT.CONTROL.DIVIDER.PNG",
                                        {(d.left - L.bar.left) / s, (d.top - L.bar.top) / s,
                                         (d.right - L.bar.left) / s, (d.bottom - L.bar.top) / s});
                return;
            }
            // WMP 12's now-playing controls stand on black; here the wall fades into it.
            const D2D1_GRADIENT_STOP stops[] = {{0, D2D1::ColorF(0x041230, 0.0f)},
                                                {0.30f, D2D1::ColorF(0x051739, 0.70f)},
                                                {1, D2D1::ColorF(0x030C24, 0.94f)}};
            com_ptr<ID2D1GradientStopCollection> collection;
            dc->CreateGradientStopCollection(stops, 3, collection.put());
            com_ptr<ID2D1LinearGradientBrush> fill;
            dc->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({0, 0}, {0, H}), collection.get(),
                                          fill.put());
            dc->FillRectangle({0, 0, W, H}, fill.get());
        });
    }
    m_topBack.IsVisible(wmc || L.compact); // compact: the title's band (PaintCompactTitle)
    if (wmc) {
        const int w = L.top.right - L.top.left, h = L.top.bottom - L.top.top;
        const float W = w / s;
        host.Repaint(m_topBack, m_topBacking, w, h, s, [&](ID2D1DeviceContext* dc) {
            win7::DrawStretched(dc, kEhres, L"TWOFOOT.FULLSCREEN.TOPBAR.LEFT.PNG", {0, 0, W / 2, 55});
            win7::DrawStretched(dc, kEhres, L"TWOFOOT.FULLSCREEN.TOPBAR.RIGHT.PNG", {W / 2, 0, W, 64});
        });
    }
}

void PlayerWindow::PaintInfo() {
    const RECT& r = m_layout.info;
    m_timeHit = m_songHit = {};
    if (m_layout.compact) {
        PaintCompactTitle();
        return;
    }
    if (Empty(r)) return;
    const int w = r.right - r.left, h = r.bottom - r.top;
    const float W = w / m_scale, H = h / m_scale;
    const std::wstring title = m_title.empty() ? Tr(L"Nothing playing") : m_title;
    const int style = m_layout.infoStyle;
    gfx::CompositionHost::Get().Repaint(m_info, m_infoBacking, w, h, m_scale, [&](ID2D1DeviceContext* dc) {
        const auto white = [&](float a) { return pane::White(dc, a); };
        // The time and the song's length in Media Center's light blue; a click on it counts
        // down instead (or up again), so the line remembers where it lies.
        const auto time = [&](float size, float y, bool centred) {
            const std::wstring text = TimeText();
            com_ptr<ID2D1SolidColorBrush> blue;
            dc->CreateSolidColorBrush(D2D1::ColorF(kTimeBlue), blue.put());
            pane::DrawTextLine(dc, text, size, DWRITE_FONT_WEIGHT_NORMAL, {0, y}, W, blue.get(),
                               centred ? DWRITE_TEXT_ALIGNMENT_CENTER : DWRITE_TEXT_ALIGNMENT_LEADING);
            float tw = W;
            com_ptr<IDWriteTextLayout> measure;
            if (SUCCEEDED(gfx::CompositionHost::Get().DWrite()->CreateTextLayout(
                    text.c_str(), static_cast<UINT32>(text.size()), pane::Format(size, DWRITE_FONT_WEIGHT_NORMAL).get(),
                    W, size * 2, measure.put()))) {
                DWRITE_TEXT_METRICS m{};
                if (SUCCEEDED(measure->GetMetrics(&m))) tw = std::min(W, m.width);
            }
            const float x0 = centred ? (W - tw) / 2 : 0;
            m_timeHit = {r.left + PxI(x0 - 4), r.top + PxI(y - 2), r.left + PxI(x0 + tw + 4),
                         r.top + PxI(y + size * kLineHeight + 2)};
        };
        // The rows of title, artist and album, for their right click (the song's menu).
        const auto songRows = [&](float y0, float y1) {
            m_songHit = {r.left, r.top + PxI(y0), r.right, r.top + PxI(y1)};
        };
        if (style == 1) {
            // WMC: title, artist, album and the time in light blue, one under the other.
            const float side = H, titleSize = side * 0.096f, lineSize = side * 0.072f, pitch = side * 0.172f;
            const int lines = 1 + (m_artist.empty() ? 0 : 1) + (m_album.empty() ? 0 : 1) + (m_track.is_valid() ? 1 : 0) +
                              (Empty(m_layout.lyric) ? 0 : 1); // the sung line closes the block
            float y = (H - pitch * lines) / 2 + (pitch - titleSize * 1.35f) / 2;
            const float firstRow = y;
            pane::DrawTextLine(dc, title, titleSize, DWRITE_FONT_WEIGHT_NORMAL, {0, y}, W, white(0.97f).get());
            y += pitch;
            for (const std::wstring* line : {&m_artist, &m_album}) {
                if (line->empty()) continue;
                pane::DrawTextLine(dc, *line, lineSize, DWRITE_FONT_WEIGHT_NORMAL, {0, y + side * 0.012f}, W,
                                   white(0.90f).get());
                y += pitch;
            }
            songRows(firstRow, y);
            if (m_track.is_valid()) time(titleSize * 0.9f, y, false);
        } else if (style == 0) {
            // The same lines centred under the cover (WMC's type, smaller), the time in WMC's blue.
            const float k = m_layout.textScale;
            const LineStack s = UnderStack(k, false);
            songRows(s.top[0], s.top[3]);
            pane::DrawTextLine(dc, title, kStackTitle * k, DWRITE_FONT_WEIGHT_NORMAL, {0, s.top[0]}, W,
                               white(0.97f).get(), DWRITE_TEXT_ALIGNMENT_CENTER);
            pane::DrawTextLine(dc, m_artist, kStackLine * k, DWRITE_FONT_WEIGHT_NORMAL, {0, s.top[1]}, W,
                               white(0.90f).get(), DWRITE_TEXT_ALIGNMENT_CENTER);
            pane::DrawTextLine(dc, m_album, kStackLine * k, DWRITE_FONT_WEIGHT_NORMAL, {0, s.top[2]}, W,
                               white(0.80f).get(), DWRITE_TEXT_ALIGNMENT_CENTER);
            if (m_track.is_valid()) time(kStackLine * k, s.top[3], true);
        } else {
            // The strip: the block (with the sung line below, UpdateLayout) centred on the cover.
            const float t = m_layout.textScale;
            const LineStack s = StripStack(t, !Empty(m_layout.lyric));
            const float top = H / 2 - s.block / 2;
            std::wstring second = m_artist;
            if (!m_album.empty()) second += (second.empty() ? L"" : L" \x2014 ") + m_album;
            songRows(top + s.top[0], top + s.top[2]);
            pane::DrawTextLine(dc, title, kStripTitle * t, DWRITE_FONT_WEIGHT_NORMAL, {0, top + s.top[0]}, W,
                               white(0.97f).get());
            pane::DrawTextLine(dc, second, kStripSecond * t, DWRITE_FONT_WEIGHT_NORMAL, {0, top + s.top[1]}, W,
                               white(0.84f).get());
            if (m_track.is_valid()) time(kStripSecond * t, top + s.top[2], false);
        }
    });
}

// The compact window's title, as WMP 12's small window shows it: white at the top left of the
// picture, on a shade that darkens its top edge; it comes and goes with the controls.
void PlayerWindow::PaintCompactTitle() {
    const RECT& r = m_layout.top;
    const int w = r.right - r.left, h = r.bottom - r.top;
    if (w <= 0 || h <= 0) return;
    const float W = w / m_scale, H = h / m_scale;
    const std::wstring title = m_title.empty() ? Tr(L"Nothing playing") : m_title;
    constexpr float kInset = 10, kTop = 7;
    gfx::CompositionHost::Get().Repaint(m_topBack, m_topBacking, w, h, m_scale, [&](ID2D1DeviceContext* dc) {
        const D2D1_GRADIENT_STOP stops[] = {{0, D2D1::ColorF(0x000000, 0.62f)},
                                            {0.55f, D2D1::ColorF(0x000000, 0.30f)},
                                            {1, D2D1::ColorF(0x000000, 0.0f)}};
        com_ptr<ID2D1GradientStopCollection> collection;
        dc->CreateGradientStopCollection(stops, 3, collection.put());
        com_ptr<ID2D1LinearGradientBrush> fill;
        dc->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({0, 0}, {0, H}), collection.get(),
                                      fill.put());
        dc->FillRectangle({0, 0, W, H}, fill.get());
        pane::DrawTextLine(dc, title, kCompactTitle, DWRITE_FONT_WEIGHT_NORMAL, {kInset, kTop}, W - 2 * kInset,
                           pane::White(dc, 0.97f).get());
    });
    m_songHit = {r.left, r.top, r.right, r.top + PxI(kTop + kCompactTitle * kLineHeight)};
}

std::wstring PlayerWindow::TimeText() const {
    const double position = m_seeking ? m_seekPreview * m_length : m_position;
    std::wstring text = settings::PlayerTimeRemaining() ? L"-" + FormatTime(std::max(0.0, m_length - position))
                                                        : FormatTime(position);
    if (m_length > 0) text += L" / " + FormatTime(m_length);
    return text;
}

void PlayerWindow::PaintCover() {
    const int px = std::max<int>(1, m_coverPainted.cx);
    auto& host = gfx::CompositionHost::Get();
    const auto surface = host.CreateSurface(px, px);
    const float side = px / m_scale;
    host.Draw(surface, m_scale, [&](ID2D1DeviceContext* dc) {
        const D2D1_RECT_F box{0, 0, side, side};
        com_ptr<ID2D1Bitmap> bitmap;
        if (m_art) dc->CreateBitmapFromWicBitmap(m_art.get(), bitmap.put());
        if (bitmap) {
            const D2D1_SIZE_F s = bitmap->GetSize();
            const float k = std::max(side / std::max(1.0f, s.width), side / std::max(1.0f, s.height));
            const float w = s.width * k, h = s.height * k;
            const D2D1_RECT_F dst{(side - w) / 2, (side - h) / 2, (side + w) / 2, (side + h) / 2};
            dc->DrawBitmap(bitmap.get(), &dst, 1.0f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, nullptr, nullptr);
        } else if (!m_ehres || !win7::DrawStretched(dc, kEhres, L"AUDIO.NOCOVER.PNG", box)) {
            // Media Center's no-cover art is missing too: a blue tile with a note.
            const D2D1_GRADIENT_STOP stops[] = {{0, D2D1::ColorF(0x3F8FD8)}, {1, D2D1::ColorF(0x123E86)}};
            com_ptr<ID2D1GradientStopCollection> collection;
            dc->CreateGradientStopCollection(stops, 2, collection.put());
            com_ptr<ID2D1LinearGradientBrush> fill;
            dc->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({0, 0}, {side, side}), collection.get(),
                                          fill.put());
            dc->FillRectangle(box, fill.get());
            pane::DrawIcon(dc, L'\xEC4F', side * 0.36f, {side / 2, side / 2}, pane::White(dc, 0.85f).get());
        }
    });
    auto brush = host.Compositor().CreateSurfaceBrush(surface);
    brush.Stretch(wuc::CompositionStretch::Fill);
    m_cover.Brush(brush);

    const auto frame = host.CreateSurface(px, px);
    host.Draw(frame, m_scale, [&](ID2D1DeviceContext* dc) {
        dc->Clear(D2D1::ColorF(0, 0, 0, 0));
        // The cover as a pane of glass, like the tiles around it: no white line, but a bevel
        // that takes its colour from the picture (light along the top and left, shade along
        // the bottom and right), a dark hairline to part it from the wall, and a faint
        // reflection across the upper left.
        const auto strip = [&](D2D1_RECT_F r, D2D1_POINT_2F from, D2D1_POINT_2F to, D2D1_COLOR_F a, D2D1_COLOR_F b) {
            const D2D1_GRADIENT_STOP stops[] = {{0, a}, {1, b}};
            com_ptr<ID2D1GradientStopCollection> collection;
            dc->CreateGradientStopCollection(stops, 2, collection.put());
            com_ptr<ID2D1LinearGradientBrush> brush;
            dc->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(from, to), collection.get(), brush.put());
            dc->FillRectangle(r, brush.get());
        };
        const auto white = [](float a) { return D2D1::ColorF(1, 1, 1, a); };
        const auto black = [](float a) { return D2D1::ColorF(0, 0, 0, a); };
        const float bevel = std::max(3.0f, side * 0.016f);
        strip({0, 0, side, bevel}, {0, 0}, {0, bevel}, white(0.42f), white(0.0f));
        strip({0, 0, bevel, side}, {0, 0}, {bevel, 0}, white(0.30f), white(0.0f));
        strip({0, side - bevel, side, side}, {0, side - bevel}, {0, side}, black(0.0f), black(0.38f));
        strip({side - bevel, 0, side, side}, {side - bevel, 0}, {side, 0}, black(0.0f), black(0.30f));
        {
            com_ptr<ID2D1Factory> factory;
            dc->GetFactory(factory.put());
            com_ptr<ID2D1PathGeometry> sheen;
            com_ptr<ID2D1GeometrySink> sink;
            if (SUCCEEDED(factory->CreatePathGeometry(sheen.put())) && SUCCEEDED(sheen->Open(sink.put()))) {
                sink->BeginFigure({0, 0}, D2D1_FIGURE_BEGIN_FILLED);
                sink->AddLine({side * 0.78f, 0});
                sink->AddBezier({{side * 0.52f, side * 0.20f}, {side * 0.22f, side * 0.34f}, {0, side * 0.46f}});
                sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                sink->Close();
                const D2D1_GRADIENT_STOP stops[] = {{0, white(0.13f)}, {1, white(0.02f)}};
                com_ptr<ID2D1GradientStopCollection> collection;
                dc->CreateGradientStopCollection(stops, 2, collection.put());
                com_ptr<ID2D1LinearGradientBrush> gloss;
                dc->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({0, 0}, {side * 0.3f, side * 0.45f}),
                                              collection.get(), gloss.put());
                dc->FillGeometry(sheen.get(), gloss.get());
            }
        }
        const float line = gfx::Hairline(m_scale);
        com_ptr<ID2D1SolidColorBrush> rim, glint;
        dc->CreateSolidColorBrush(black(0.55f), rim.put());
        dc->CreateSolidColorBrush(white(0.45f), glint.put());
        dc->DrawRectangle({line / 2, line / 2, side - line / 2, side - line / 2}, rim.get(), line);
        dc->FillRectangle({line, line, side - line, line * 2}, glint.get());
        dc->FillRectangle({line, line, line * 2, side - line}, glint.get());
    });
    auto frameBrush = host.Compositor().CreateSurfaceBrush(frame);
    frameBrush.Stretch(wuc::CompositionStretch::Fill);
    for (const auto& f : m_coverFrame) f.Brush(frameBrush);
}

} // namespace glasswave::ui
