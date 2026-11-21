#include "pch.h"

#include "ui/player_window.h"
#include "media/radio.h"
#include "media/special_playlists.h"

#include "aero/aero_atlas.h"
#include "aero/aero_metrics.h"
#include "aero/aero_painter.h"
#include "app/settings.h"
#include "gfx/composition_host.h"
#include "gfx/effects.h"
#include "media/favorites.h"
#include "ui/art_cache.h"
#include "ui/i18n.h"
#include "ui/pane_common.h"
#include "ui/text.h"
#include "win7/win7_resources.h"
#include "wmp/wmp_transport.h"

#include <winrt/Windows.Foundation.Collections.h>

namespace glasswave::ui {

namespace m = aero::metrics;
namespace paint = aero::paint;
namespace wt = wmp::transport;
namespace wuc = winrt::Windows::UI::Composition;
using paint::Layer;
using paint::TransportGlyph;
using winrt::com_ptr;
using winrt::Windows::Foundation::Numerics::float2;
using winrt::Windows::Foundation::Numerics::float3;

namespace {

constexpr wchar_t kClassName[] = L"Glasswave.PlayerWindow";
constexpr UINT kFrameMessage = WM_APP + 0x41;
constexpr UINT_PTR kHideTimer = 1, kOverlayTimer = 2, kRepeatTimer = 3, kSizeTimer = 5,
                   kBubbleTimer = 7;
constexpr int kSizeMs = 340;          // the mini window growing or shrinking for the pane
constexpr float kVolumeStep = 0.01f;  // one of the hundred steps every other volume control shows
constexpr UINT kHideAfterMs = 3000; // full screen: controls and pointer leave after this
constexpr UINT kOverlayMs = 1600;   // full screen: the volume overlay stays this long

// DIP. See docs/design/player-window.md.
constexpr float kMiniBar = 80;              // mini: seek row + transport row
constexpr float kWmcBar = 53, kWmcTop = 64; // TWOFOOT.FULLSCREEN.BOTTOMBAR / TOPBAR heights
// The smallest mini window: the transport's row with the volume's split button, the cluster and
// lyrics / queue (the heart, shuffle and repeat leave first), and a client a little higher
// than the control bar, which lies over the picture.
constexpr float kMiniMinW = 344, kMiniMinClientH = 96;
// Narrower than that the mini window is compact (owner, 2026-11-14, after WMP 12's own small
// Now Playing window): a square client filled by the cover, the title over its top and only the
// transport and the volume over its foot, both coming and going with the pointer. Growing from
// the smallest square the wall's margin round the cover opens first, the cover's glass edge and
// shadow fading in with it, then the cover grows.
constexpr float kCompactSide = 250, kCompactMargin = 16, kCompactTitle = 14;
constexpr float kCoverShadow = 0.6f; // the cover's drop shadow on the wall
constexpr float kMiniW = 396, kMiniH = 540, kMiniPaneH = 720;
// The strip form (cover at the left, the lines beside it) for a stage lower than this, or one
// this much wider than high.
constexpr float kStripBelow = 200, kStripAspect = 1.5f;
// The strip's lines, in DIP at text scale 1: title 17, artist / album 13, the time 13 (blue),
// the sung line 14; the block is centred on the cover.
constexpr float kStripTitle = 17, kStripSecond = 13, kStripLyric = 14;
// The mini window's lines under the cover at text scale 1: title 19, the others 14.
constexpr float kStackTitle = 19, kStackLine = 14;
constexpr float kLineHeight = 1.33f; // a line's height for its type size
// Mini, lyrics or queue open: the pane keeps at least this height (more if a lyric line needs
// it); dragging the window this much further closes it.
constexpr float kPaneMinH = 150, kPaneCloseDrag = 56;
constexpr float kFullBarMaxW = 760, kFullBarMargin = 24; // full screen without ehres.dll
constexpr uint32_t kNavy = 0x0B2C6B; // WMC's blue over the dimmed tiles
constexpr uint32_t kCardRim = 0x2F64B4; // the full-screen card's clear rim: a lighter blue than kNavy
constexpr uint32_t kTimeBlue = 0x8CCBFF;
constexpr float kSeekInset = 6; // DIP at either end of the seek rectangle: room for the thumb
constexpr float kCardPad = 10, kCardShadow = 32; // DIP: full screen pane card around the pane
constexpr float kFlyoutW = 92, kFlyoutH = 32; // DIP: WMP's volume flyout (its slider art fills the inside)
constexpr float kFlyoutTrackStart = 8, kFlyoutTrackEnd = 75; // its slider's track in the 84 DIP art
// Its glass frame is 5 DIP all round; the slider art's inside (82 DIP) fills the inside.
constexpr float kFlyoutFrame = 5;
constexpr float kFlyoutSliderX = kFlyoutFrame + (kFlyoutW - 2 * kFlyoutFrame - 84) / 2;
constexpr float kFlyoutSliderY = kFlyoutFrame + (kFlyoutH - 2 * kFlyoutFrame - 17) / 2;
// The mini window's inline slider, WMP 12's (wmploc 5678, 80 x 23): the thumb's centre runs
// from 11 to 66 DIP in it.
constexpr float kSliderW = 80, kSliderH = 23, kSliderThumbMin = 11, kSliderThumbMax = 66;
constexpr float kSeekThumbW = 16, kSeekThumbH = 9; // WMP's seek thumb (4058 / 3884 / 3883)
constexpr int kSeekThumbInMs = 160, kSeekThumbOutMs = 280;
constexpr int kMoveMs = 480, kFadeMs = 260;
// Media Center's back orb coming down with the lyrics or the queue, and going back up.
constexpr int kBackInMs = 520, kBackOutMs = 300;
constexpr auto kEhres = win7::Module::Ehres;
constexpr auto kWmploc = win7::Module::Wmploc;
constexpr int kSwitchToLibrary = 4603; // wmploc: the "switch to library" corner glyph, 25x25

enum Id : int {
    kStop = 1,
    kPrev,
    kPlay,
    kNext,
    kMute,
    kVolArrow,
    kVolDown,
    kVolUp,
    kLyrics,
    kList,
    kLibrary,
    kMinimize,
    kRestore,
    kExit,
    kMenuLyrics, // full screen: Media Center's menu, in this order
    kMenuList,
    kMenuShuffle,
    kMenuRepeat,
    kFavorite,
    kCapMin,
    kCapMax,
    kCapClose,
    kShuffle, // mini: WMP 12's pair before stop
    kRepeat
};

winrt::Windows::UI::Color Rgba(uint32_t rgb, float alpha) {
    return winrt::Windows::UI::ColorHelper::FromArgb(static_cast<uint8_t>(std::lround(alpha * 255)),
                                                     static_cast<uint8_t>(rgb >> 16),
                                                     static_cast<uint8_t>(rgb >> 8), static_cast<uint8_t>(rgb));
}

// The song's lines one under another (DIP): each line's top, and the block's height. At text
// scale `t` the type grows with t but the gaps between the lines grow faster (x (1 + 0.9 (t - 1))
// above scale 1), so that large lines in a roomy window do not crowd while small ones stay
// close: 4.4 DIP after the title and 3.2 after the others at scale 1.
struct LineStack {
    std::array<float, 6> top{};
    float block = 0;
};
LineStack StackLines(std::initializer_list<float> sizes, float t) {
    const float open = 1 + 0.9f * std::max(0.0f, t - 1);
    LineStack out;
    float y = 0;
    size_t i = 0;
    for (const float size : sizes) {
        if (i > 0) y += (i == 1 ? 4.4f : 3.2f) * t * open;
        out.top[i++] = y;
        y += size * kLineHeight;
    }
    out.block = y;
    return out;
}
// The strip's: title, artist / album, time, the sung line.
LineStack StripStack(float t, bool lyric) {
    return lyric ? StackLines({kStripTitle * t, kStripSecond * t, kStripSecond * t, kStripLyric * t}, t)
                 : StackLines({kStripTitle * t, kStripSecond * t, kStripSecond * t}, t);
}
// Under the cover: title, artist, album, time, the sung line.
LineStack UnderStack(float t, bool lyric) {
    const float a = kStackTitle * t, b = kStackLine * t;
    return lyric ? StackLines({a, b, b, b, b}, t) : StackLines({a, b, b, b}, t);
}

RECT CenteredRect(int cx, int cy, int w, int h) {
    return {cx - w / 2, cy - h / 2, cx - w / 2 + w, cy - h / 2 + h};
}

bool Empty(const RECT& r) {
    return r.right <= r.left || r.bottom <= r.top;
}

// Eases a visual's opacity from where it is to `to`.
void FadeTo(const wuc::Visual& v, float to, int ms) {
    const auto comp = v.Compositor();
    auto fade = comp.CreateScalarKeyFrameAnimation();
    fade.InsertKeyFrame(1.0f, to, comp.CreateCubicBezierEasingFunction({0.25f, 0.1f}, {0.25f, 1.0f}));
    fade.Duration(std::chrono::milliseconds(ms));
    v.StartAnimation(L"Opacity", fade);
}

// The split volume button's plate and arrow and the row gradients are WMP's transport art,
// shared with the library window (wmp/wmp_transport.h). The arrow baked into the plate
// bitmaps came out jagged when enlarged: SplitPlate leaves it out for SplitArrow to draw.
using wt::RowBands;
using wt::SplitArrow;
using wt::SplitPlate;

// WMP 12's glass thumbs in vectors: the volume flyout's ball (wmploc 2102, 2103 lit, 2104
// held; a circle of radius 6 about (6, 8.5) in its 12x17 bitmap) and the seek thumb (4058,
// 3884 lit, 3883 held; a 16x9 pill). Each is glass cut by a sharp horizon (y 8 and y 4): the
// upper half a pale dome whose light gathers in a crescent under the rim, the lower half deep
// blue lit from below by a cyan glow. Each half is a vertical gradient with two radial layers
// over it (a crescent rising towards the rim, a glow fading from its centre), radial about the
// shape's centre line so that the pill's ends curve like the ball. The layers' geometry and
// colours were fitted to the bitmaps' pixels (least squares over 4x4 subsamples, the
// outermost ring left to the rim); the rim is a one-unit stroke in the bitmaps' outermost
// colours. In bitmap units: the caller scales.
struct GlassHalf {
    uint32_t base[3];        // top, middle and foot of the half
    float ay, ax, ayr, sA;   // crescent: centre below the shape's (units), radii (x R), starts at sA
    uint32_t ca;
    float aA;
    float by, bx, byr;       // glow: centre below the shape's, radii (x R)
    uint32_t cb;
    float aB, mB;            // alpha at the centre, and halfway (x aB)
};
struct GlassRim {
    float y;
    uint32_t rgb;
};
struct GlassLook {
    GlassHalf upper, lower;
    GlassRim rim[13];
    int rims;
};
struct GlassShape {
    float x0, x1, cy, r, horizon; // the centre line from x0 to x1 at cy, radius, horizon
};
constexpr GlassShape kBallShape{6, 6, 8.5f, 6, 8};
constexpr GlassShape kPillShape{4.5f, 11.5f, 4.5f, 4.5f, 4};

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
constexpr GlassLook kPillLooks[3] = {
    {{{0xEFFDFF, 0xB8C4E1, 0xD2C2CC}, 0.897f, 0.600f, 1.600f, 0.658f, 0xEDFAFF, 1.000f, -0.522f, 1.093f, 0.510f, 0x0060BA, 0.737f, 0.726f},
     {{0x0453AF, 0x009FFF, 0x00FFFF}, 0.882f, 1.484f, 0.600f, 0.511f, 0xC1FFFF, 1.000f, 0.815f, 1.600f, 0.300f, 0x000043, 0.400f, 1.000f},
     {{0.5f, 0x4262AE}, {1.5f, 0x496CB2}, {2.5f, 0x4064AE}, {3.5f, 0x4E78BA}, {4.5f, 0x2C50A9}, {5.5f, 0x0E298E}, {6.5f, 0x314487}, {7.5f, 0x20417F}, {8.5f, 0x334472}},
     9},
    {{{0xFCC9F9, 0xAFC9E9, 0x62ABEB}, -0.482f, 0.600f, 0.600f, 0.773f, 0xE6FFFF, 0.674f, -1.401f, 1.056f, 0.320f, 0x0075F0, 0.493f, 0.069f},
     {{0x0049CD, 0x67FFFF, 0xFFFEFF}, 2.109f, 0.600f, 1.600f, 0.859f, 0x5886D4, 1.000f, 0.888f, 1.600f, 0.336f, 0x0074E7, 0.924f, 1.000f},
     {{0.5f, 0x1B64C9}, {1.5f, 0x2E78D5}, {2.5f, 0x1B66CC}, {3.5f, 0x4B8CD9}, {4.5f, 0x2764D0}, {5.5f, 0x0130A9}, {6.5f, 0x0B37B2}, {7.5f, 0x0947C2}, {8.5f, 0x0837BA}},
     9},
    {{{0x6D97D3, 0xA2A5AC, 0x2456A3}, -3.000f, 0.639f, 0.735f, 0.491f, 0xC5E1EB, 0.806f, -0.825f, 0.942f, 0.357f, 0x2D4A77, 0.658f, 0.946f},
     {{0x002667, 0x000022, 0xFFFFFF}, 1.766f, 1.242f, 0.603f, 0.768f, 0x6872AA, 0.832f, 2.726f, 0.994f, 0.457f, 0x0AB5FF, 0.943f, 0.662f},
     {{0.5f, 0x3E424C}, {1.5f, 0x3A424C}, {2.5f, 0x414A63}, {3.5f, 0x4A5F83}, {4.5f, 0x293B6A}, {5.5f, 0x0E1552}, {6.5f, 0x384174}, {7.5f, 0x193C73}, {8.5f, 0x345283}},
     9}};

D2D1_COLOR_F Argb(uint32_t rgb, float alpha) {
    return D2D1::ColorF(rgb, alpha);
}

com_ptr<ID2D1GradientStopCollection> Stops(ID2D1DeviceContext* dc, const std::vector<D2D1_GRADIENT_STOP>& stops) {
    com_ptr<ID2D1GradientStopCollection> collection;
    dc->CreateGradientStopCollection(stops.data(), static_cast<UINT32>(stops.size()), collection.put());
    return collection;
}

// Fills `area` with a gradient that is radial about the segment (x0..x1, cy): radial brushes
// on its ends, a vertical gradient (the same stops mirrored) between them.
void FillAboutSegment(ID2D1DeviceContext* dc, D2D1_RECT_F area, float x0, float x1, float cy, float rx, float ry,
                      const std::vector<D2D1_GRADIENT_STOP>& stops) {
    const auto radial = [&](float cx, D2D1_RECT_F part) {
        if (part.right <= part.left) return;
        com_ptr<ID2D1RadialGradientBrush> brush;
        dc->CreateRadialGradientBrush(D2D1::RadialGradientBrushProperties({cx, cy}, {0, 0}, rx, ry),
                                      Stops(dc, stops).get(), brush.put());
        dc->PushAxisAlignedClip(part, D2D1_ANTIALIAS_MODE_ALIASED);
        dc->FillRectangle(area, brush.get());
        dc->PopAxisAlignedClip();
    };
    if (x1 - x0 < 0.01f) {
        radial(x0, area);
        return;
    }
    radial(x0, {area.left, area.top, x0, area.bottom});
    radial(x1, {x1, area.top, area.right, area.bottom});
    std::vector<D2D1_GRADIENT_STOP> mirrored;
    for (auto it = stops.rbegin(); it != stops.rend(); ++it) mirrored.push_back({0.5f - it->position / 2, it->color});
    for (const auto& s : stops) mirrored.push_back({0.5f + s.position / 2, s.color});
    com_ptr<ID2D1LinearGradientBrush> brush;
    dc->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({0, cy - ry}, {0, cy + ry}),
                                  Stops(dc, mirrored).get(), brush.put());
    dc->PushAxisAlignedClip({x0, area.top, x1, area.bottom}, D2D1_ANTIALIAS_MODE_ALIASED);
    dc->FillRectangle(area, brush.get());
    dc->PopAxisAlignedClip();
}

void PaintGlass(ID2D1DeviceContext* dc, const GlassShape& s, const GlassLook& look) {
    com_ptr<ID2D1Factory> factory;
    dc->GetFactory(factory.put());
    const auto outline = [&](float inset) {
        com_ptr<ID2D1Geometry> g;
        const float r = s.r - inset;
        if (s.x1 - s.x0 < 0.01f) {
            com_ptr<ID2D1EllipseGeometry> e;
            factory->CreateEllipseGeometry(D2D1::Ellipse({s.x0, s.cy}, r, r), e.put());
            g = e;
        } else {
            com_ptr<ID2D1RoundedRectangleGeometry> p;
            factory->CreateRoundedRectangleGeometry({{s.x0 - r, s.cy - r, s.x1 + r, s.cy + r}, r, r}, p.put());
            g = p;
        }
        return g;
    };
    const auto shape = outline(0);
    const float top = s.cy - s.r, foot = s.cy + s.r, left = s.x0 - s.r, right = s.x1 + s.r;
    dc->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), shape.get()), nullptr);
    for (int half = 0; half < 2; ++half) {
        const GlassHalf& g = half == 0 ? look.upper : look.lower;
        const float y0 = half == 0 ? top : s.horizon, y1 = half == 0 ? s.horizon : foot;
        const D2D1_RECT_F area{left, y0, right, y1};
        dc->PushAxisAlignedClip(area, D2D1_ANTIALIAS_MODE_ALIASED); // the horizon stays sharp
        com_ptr<ID2D1LinearGradientBrush> base;
        dc->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({0, y0}, {0, y1}),
                                      Stops(dc, {{0, Argb(g.base[0], 1)}, {0.5f, Argb(g.base[1], 1)},
                                                 {1, Argb(g.base[2], 1)}})
                                          .get(),
                                      base.put());
        dc->FillRectangle(area, base.get());
        FillAboutSegment(dc, area, s.x0, s.x1, s.cy + g.ay, s.r * g.ax, s.r * g.ayr,
                         {{0, Argb(g.ca, 0)}, {g.sA, Argb(g.ca, 0)}, {1, Argb(g.ca, g.aA)}});
        FillAboutSegment(dc, area, s.x0, s.x1, s.cy + g.by, s.r * g.bx, s.r * g.byr,
                         {{0, Argb(g.cb, g.aB)}, {0.5f, Argb(g.cb, g.aB * g.mB)}, {1, Argb(g.cb, 0)}});
        dc->PopAxisAlignedClip();
    }
    // The rim: one unit inside the outline, in the bitmap's outermost colour row by row.
    std::vector<D2D1_GRADIENT_STOP> rim;
    for (int i = 0; i < look.rims; ++i) {
        const float y = (look.rim[i].y - top) / (foot - top);
        rim.push_back({std::clamp(y, 0.0f, 1.0f), Argb(look.rim[i].rgb, 0.85f)});
    }
    com_ptr<ID2D1LinearGradientBrush> rimBrush;
    dc->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({0, top}, {0, foot}), Stops(dc, rim).get(),
                                  rimBrush.put());
    dc->DrawGeometry(outline(0.5f).get(), rimBrush.get(), 1.0f);
    dc->PopLayer();
}

// The seek thumb at (0, 0), 16x9 DIP; `state` 0 at rest, 1 lit, 2 held.
void PaintSeekThumb(ID2D1DeviceContext* dc, int state) {
    PaintGlass(dc, kPillShape, kPillLooks[std::clamp(state, 0, 2)]);
}

// The flyout's ball with its bitmap's top left at (0, 0), 12x17 DIP.
void PaintFlyoutBall(ID2D1DeviceContext* dc, int state) {
    PaintGlass(dc, kBallShape, kBallLooks[std::clamp(state, 0, 2)]);
}

// WMP 12's square button plates, 25x25 (wmploc 4725 under the pointer, 4724 pressed), the
// look of its speaker button: a navy rim, a light inner frame, a pale glass top over a sharp
// horizon at row 12 and a cyan glow rising to a bright foot. In vectors: four columns of the
// bitmap (rim, frame, the column inside it, the middle), each a vertical gradient through its
// 25 rows (smooth within the bands, sharp between them), the middle stretched across `w`.
struct PlateColumn {
    uint32_t rgb;
    uint8_t alpha;
};
constexpr PlateColumn kPlateHot[4][25] = {
    {{0x3399FF, 5}, {0x1457B1, 42}, {0x0B4199, 65}, {0x073B92, 72}, {0x073990, 79}, {0x06368D, 85}, {0x05348C, 91},
     {0x04338B, 96}, {0x04328A, 102}, {0x033089, 107}, {0x032F87, 112}, {0x022E87, 117}, {0x032B84, 124},
     {0x022981, 132}, {0x022981, 131}, {0x022880, 130}, {0x022880, 129}, {0x022780, 127}, {0x02277F, 126},
     {0x02267D, 123}, {0x02257C, 120}, {0x02247B, 118}, {0x042880, 112}, {0x1144A0, 85}, {0x3399FF, 43}},
    {{0x1B6DC8, 27}, {0x97B1D6, 99}, {0xF2F8FB, 195}, {0xF4FAFC, 206}, {0xF0F8FA, 199}, {0xEAF6F9, 192},
     {0xE3F1F7, 184}, {0xDDEFF5, 176}, {0xD7ECF4, 169}, {0xD2E9F1, 162}, {0xCDE5F0, 155}, {0xC2DEED, 154},
     {0x7D8EB8, 140}, {0x7A8AB5, 131}, {0x7889B4, 126}, {0x738BB5, 123}, {0x708DB7, 122}, {0x7DA0C4, 130},
     {0x8FB3D2, 141}, {0xA0C4DC, 152}, {0xB0D4E5, 166}, {0xBFE0EC, 180}, {0xC2E2EC, 187}, {0x7A9CC5, 155},
     {0x1248A3, 90}},
    {{0x115DB5, 41}, {0xEEF4F9, 177}, {0xF3FAFC, 191}, {0xEAF5F9, 174}, {0xE3F2F7, 167}, {0xDAEEF5, 160},
     {0xCFE8F0, 153}, {0xC6E3EE, 145}, {0xBDDFEC, 139}, {0xB5DAE9, 133}, {0xB0D6E7, 126}, {0xA1CBE2, 128},
     {0x3E5C9A, 99}, {0x3C5998, 90}, {0x3D5F9B, 89}, {0x3B6AA2, 88}, {0x4079AB, 90}, {0x4085B5, 91},
     {0x4695C2, 95}, {0x4CA8CE, 101}, {0x5EBBDA, 112}, {0x72CDE2, 127}, {0xA6E4EF, 163}, {0xD8F1F7, 213},
     {0x052980, 110}},
    {{0x0C55AC, 46}, {0xFCFDFE, 212}, {0xF1F8FB, 180}, {0xE9F4F9, 171}, {0xE1F1F6, 163}, {0xD7EDF4, 156},
     {0xCDE7F0, 151}, {0xC3E2ED, 145}, {0xBADEEB, 138}, {0xB2D9E8, 131}, {0xABD5E6, 124}, {0x98CAE2, 130},
     {0x3F74AC, 85}, {0x3E7BB0, 78}, {0x4084B5, 75}, {0x4094BF, 75}, {0x42A4CA, 79}, {0x42B6D8, 84},
     {0x44CAE7, 96}, {0x47DAF1, 111}, {0x58E8F8, 134}, {0x74F3FC, 166}, {0x98F9FE, 198}, {0xF1FFFF, 248},
     {0x022278, 113}}};
constexpr PlateColumn kPlatePressed[4][25] = {
    {{0x0051C7, 22}, {0x233A63, 130}, {0x2B3C5A, 188}, {0x2B3B59, 201}, {0x293957, 204}, {0x273755, 207},
     {0x243453, 211}, {0x213250, 215}, {0x1E2E4E, 219}, {0x1A2B4C, 223}, {0x17284A, 227}, {0x16284A, 241},
     {0x132849, 253}, {0x112749, 255}, {0x11274A, 254}, {0x11284C, 251}, {0x10294D, 248}, {0x102A50, 243},
     {0x102C53, 238}, {0x0F2F55, 232}, {0x0F3057, 227}, {0x0E3159, 222}, {0x0D315A, 204}, {0x0A3062, 140},
     {0x0051C7, 23}},
    {{0x1F3B6A, 110}, {0x424F69, 178}, {0xBBC3CE, 175}, {0xC7CDD8, 178}, {0xBFC7D4, 173}, {0xB6C0D0, 166},
     {0xACB9CB, 161}, {0xA2B2C5, 155}, {0x99AABF, 149}, {0x8FA3B9, 144}, {0x869AB3, 138}, {0x7E93AD, 134},
     {0x293B5F, 140}, {0x203356, 133}, {0x1D2D52, 127}, {0x142A52, 126}, {0x102851, 125}, {0x0C2B56, 125},
     {0x0B2F5C, 125}, {0x083462, 126}, {0x083E70, 129}, {0x0C4C81, 135}, {0x0E5389, 147}, {0x0D406C, 193},
     {0x0A3163, 139}},
    {{0x2A3C5D, 171}, {0xAAB1BD, 170}, {0xBFC7D2, 170}, {0xB2BBCA, 160}, {0xAAB6C6, 156}, {0xA2AFC3, 152},
     {0x99A9BE, 148}, {0x90A2B9, 143}, {0x879BB4, 139}, {0x7F95AF, 135}, {0x778EAA, 131}, {0x7088A5, 128},
     {0x1A3056, 125}, {0x152A50, 118}, {0x142A51, 115}, {0x122D56, 115}, {0x102F5A, 116}, {0x0D3460, 115},
     {0x0B3867, 116}, {0x0C4577, 120}, {0x0E5388, 128}, {0x126097, 137}, {0x136EA8, 145}, {0x1473AE, 156},
     {0x0C325C, 198}},
    {{0x2F3E5B, 194}, {0xCFD3DC, 183}, {0xB8C1CD, 164}, {0xB2BBCA, 160}, {0xAAB6C6, 156}, {0xA2AFC3, 152},
     {0x99A9BE, 148}, {0x90A2B9, 143}, {0x879BB4, 139}, {0x7F95AF, 135}, {0x7590AD, 135}, {0x6E8DAD, 135},
     {0x20436D, 119}, {0x1B4470, 114}, {0x1B4573, 113}, {0x1A4C7B, 116}, {0x1C598A, 122}, {0x236EA2, 137},
     {0x2D86BB, 160}, {0x369BD1, 186}, {0x3DA9E1, 209}, {0x41B1E9, 223}, {0x41B4EE, 227}, {0x3DBBF7, 227},
     {0x0D335C, 208}}};

// `area` is the plate; its height is 25 units.
void WmpPlate(ID2D1DeviceContext* dc, D2D1_RECT_F area, bool pressed) {
    const auto& cols = pressed ? kPlatePressed : kPlateHot;
    const float k = (area.bottom - area.top) / 25.0f;
    // Rows that start a band of their own: the rim, the frame, the horizon, the bright foot.
    static constexpr int kBreaks[] = {1, 2, 12, 23, 24};
    const auto brush = [&](const PlateColumn (&col)[25]) {
        std::vector<D2D1_GRADIENT_STOP> stops;
        const auto at = [&](int row, float y) {
            stops.push_back({y / 25.0f, Argb(col[row].rgb, col[row].alpha / 255.0f)});
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

float SliderFromDb(float db) {
    return db <= -99.0f ? 0.0f : std::clamp(std::pow(10.0f, db / 40.0f), 0.0f, 1.0f);
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

std::chrono::milliseconds Ms(int ms) {
    return std::chrono::milliseconds(ms);
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

// A pane of Aero glass: blue body, a bright reflection over its upper half, a dark hairline
// outside and a light one inside.
void GlassFace(ID2D1DeviceContext* dc, float W, float H, float radius, float scale) {
    const auto vertical = [&](float top, float bottom, std::initializer_list<D2D1_GRADIENT_STOP> stops) {
        com_ptr<ID2D1GradientStopCollection> collection;
        dc->CreateGradientStopCollection(stops.begin(), static_cast<UINT32>(stops.size()), collection.put());
        com_ptr<ID2D1LinearGradientBrush> brush;
        dc->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({0, top}, {0, bottom}), collection.get(),
                                      brush.put());
        return brush;
    };
    const float line = std::max(1.0f, std::floor(scale)) / scale;
    const D2D1_ROUNDED_RECT body{{line / 2, line / 2, W - line / 2, H - line / 2}, radius, radius};
    dc->FillRoundedRectangle(body, vertical(0, H, {{0, D2D1::ColorF(0x3F82D6, 0.78f)},
                                                   {0.5f, D2D1::ColorF(0x17489A, 0.82f)},
                                                   {1, D2D1::ColorF(0x0A2C6C, 0.88f)}})
                                       .get());
    const float gloss = H * 0.46f;
    dc->FillRoundedRectangle({{line * 2, line * 2, W - line * 2, gloss}, radius * 0.8f, radius * 0.8f},
                             vertical(0, gloss, {{0, D2D1::ColorF(1, 1, 1, 0.46f)}, {1, D2D1::ColorF(1, 1, 1, 0.10f)}})
                                 .get());
    dc->FillRoundedRectangle({{line * 2, H - H * 0.30f, W - line * 2, H - line * 2}, radius * 0.8f, radius * 0.8f},
                             vertical(H * 0.7f, H, {{0, D2D1::ColorF(0x6FC4FF, 0.0f)}, {1, D2D1::ColorF(0x6FC4FF, 0.26f)}})
                                 .get());
    com_ptr<ID2D1SolidColorBrush> dark;
    dc->CreateSolidColorBrush(D2D1::ColorF(0x020A1E, 0.75f), dark.put());
    dc->DrawRoundedRectangle(body, dark.get(), line);
    dc->DrawRoundedRectangle({{line * 1.5f, line * 1.5f, W - line * 1.5f, H - line * 1.5f}, radius - line, radius - line},
                             pane::White(dc, 0.52f).get(), line);
}

void Place(const wuc::Visual& v, const RECT& r, bool animate = false) {
    const float3 offset{static_cast<float>(r.left), static_cast<float>(r.top), 0};
    const float2 size{static_cast<float>(std::max<LONG>(0, r.right - r.left)),
                      static_cast<float>(std::max<LONG>(0, r.bottom - r.top))};
    if (animate) {
        const auto comp = v.Compositor();
        const auto ease = comp.CreateCubicBezierEasingFunction({0.16f, 0.9f}, {0.2f, 1.0f});
        auto move = comp.CreateVector3KeyFrameAnimation();
        move.InsertKeyFrame(1.0f, offset, ease);
        move.Duration(std::chrono::milliseconds(kMoveMs));
        v.StartAnimation(L"Offset", move);
        auto grow = comp.CreateVector2KeyFrameAnimation();
        grow.InsertKeyFrame(1.0f, size, ease);
        grow.Duration(std::chrono::milliseconds(kMoveMs));
        v.StartAnimation(L"Size", grow);
        return;
    }
    v.StopAnimation(L"Offset");
    v.StopAnimation(L"Size");
    v.Offset(offset);
    v.Size(size);
}

// A visual already placed (and painted) at its new rect starts out drawn at `from`, scaled by
// `scale` from its top left, and grows or shrinks into place on Place's curve, so it keeps
// step with a neighbour that Place animates.
void Morph(const wuc::Visual& v, const RECT& from, float scale) {
    const auto comp = v.Compositor();
    const auto ease = comp.CreateCubicBezierEasingFunction({0.16f, 0.9f}, {0.2f, 1.0f});
    const float3 to = v.Offset();
    v.StopAnimation(L"Opacity");
    v.Opacity(1.0f);
    v.CenterPoint(float3{0, 0, 0});
    auto move = comp.CreateVector3KeyFrameAnimation();
    move.InsertKeyFrame(0.0f, float3{static_cast<float>(from.left), static_cast<float>(from.top), 0});
    move.InsertKeyFrame(1.0f, to, ease);
    move.Duration(std::chrono::milliseconds(kMoveMs));
    v.StartAnimation(L"Offset", move);
    auto grow = comp.CreateVector3KeyFrameAnimation();
    grow.InsertKeyFrame(0.0f, float3{scale, scale, 1});
    grow.InsertKeyFrame(1.0f, float3{1, 1, 1}, ease);
    grow.Duration(std::chrono::milliseconds(kMoveMs));
    v.StartAnimation(L"Scale", grow);
}

wuc::CompositionNineGridBrush NineGrid(int wPx, int hPx, float scale, float insetPx,
                                       const std::function<void(ID2D1DeviceContext*)>& painter) {
    auto& host = gfx::CompositionHost::Get();
    auto surface = host.CreateSurface(wPx, hPx);
    host.Draw(surface, scale, painter);
    auto source = host.Compositor().CreateSurfaceBrush(surface);
    source.Stretch(wuc::CompositionStretch::Fill);
    auto brush = host.Compositor().CreateNineGridBrush();
    brush.Source(source);
    brush.SetInsets(insetPx);
    brush.IsCenterHollow(true);
    return brush;
}

} // namespace

PlayerWindow::PlayerWindow(PlayerHost& host) : m_host(host) {}

PlayerWindow::~PlayerWindow() {
    if (auto* cache = ArtCache::Existing()) cache->Unsubscribe(m_artToken);
    m_frameClock.reset();
    m_shadow.Destroy();
    m_widgets.clear();
    m_volumePanel.reset();
    m_volumeBubble.reset();
    m_wall.reset();
    m_light.reset();
    m_veil.reset();
    m_glass.reset();
    if (m_hwnd) {
        SetWindowLongPtrW(m_hwnd, GWLP_USERDATA, 0);
        DestroyWindow(m_hwnd);
    }
}

// ---- Window --------------------------------------------------------------------------------

LRESULT CALLBACK PlayerWindow::WndProcThunk(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    PlayerWindow* self = nullptr;
    if (msg == WM_NCCREATE) {
        self = static_cast<PlayerWindow*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        self->m_hwnd = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<PlayerWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
    try {
        return self->WndProc(msg, wp, lp);
    } catch (const std::exception& e) {
        console::complain("Glasswave", e.what());
    } catch (const winrt::hresult_error& e) {
        console::complain("Glasswave", pfc::stringcvt::string_utf8_from_wide(e.message().c_str()));
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void PlayerWindow::Create() {
    const HINSTANCE inst = core_api::get_my_instance();
    WNDCLASSEXW wc{sizeof(wc)};
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = &PlayerWindow::WndProcThunk;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = ui_control::get()->get_main_icon();
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc); // failing because it already exists is fine
    CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP | WS_EX_APPWINDOW, kClassName, L"Glasswave",
                    WS_POPUP | WS_CLIPCHILDREN, 0, 0, 100, 100, nullptr, nullptr, inst, this);
    if (!m_hwnd) throw exception_win32(GetLastError());
    m_scale = GetDpiForWindow(m_hwnd) / 96.0f;
    // The same window attributes as the library window: our own Win7 frame and shadow.
    BOOL enable = TRUE;
    DwmSetWindowAttribute(m_hwnd, DWMWA_USE_HOSTBACKDROPBRUSH, &enable, sizeof(enable));
    DWM_WINDOW_CORNER_PREFERENCE corner = DWMWCP_DONOTROUND;
    DwmSetWindowAttribute(m_hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
    COLORREF border = DWMWA_COLOR_NONE;
    DwmSetWindowAttribute(m_hwnd, DWMWA_BORDER_COLOR, &border, sizeof(border));
    DWM_SYSTEMBACKDROP_TYPE backdrop = DWMSBT_NONE;
    DwmSetWindowAttribute(m_hwnd, DWMWA_SYSTEMBACKDROP_TYPE, &backdrop, sizeof(backdrop));
    DWMNCRENDERINGPOLICY ncrp = DWMNCRP_DISABLED;
    DwmSetWindowAttribute(m_hwnd, DWMWA_NCRENDERING_POLICY, &ncrp, sizeof(ncrp));

    m_wmp = m_host.OriginalTransportArt();
    m_ehres = win7::Resources::Get().Has(kEhres, L"TWOFOOT.CONTROL.PLAY.NOFOCUS.PNG");
    m_aeroArt = aero::atlas::Available();
    InitVisuals();
    CreateWidgets();
    m_shadow.Create(m_hwnd);
    m_host.AdoptPlayerWindow(m_hwnd); // the library window's icon and taskbar buttons
    m_artToken = ArtCache::Get().Subscribe([this] {
        if (m_shown && m_artPending) RequestArt();
    });
}

void PlayerWindow::InitVisuals() {
    auto& host = gfx::CompositionHost::Get();
    const auto& comp = host.Compositor();
    auto interop = comp.as<ABI::Windows::UI::Composition::Desktop::ICompositorDesktopInterop>();
    winrt::check_hresult(interop->CreateDesktopWindowTarget(
        m_hwnd, FALSE,
        reinterpret_cast<ABI::Windows::UI::Composition::Desktop::IDesktopWindowTarget**>(
            winrt::put_abi(m_target))));
    // As in the library window: the frame outline lies above the rounded clip, not inside it.
    m_windowRoot = comp.CreateContainerVisual();
    m_windowRoot.RelativeSizeAdjustment(float2{1, 1});
    m_target.Root(m_windowRoot);
    m_root = comp.CreateContainerVisual();
    m_root.RelativeSizeAdjustment(float2{1, 1});
    m_windowRoot.Children().InsertAtTop(m_root);
    m_clipGeometry = comp.CreateRoundedRectangleGeometry();
    m_root.Clip(comp.CreateGeometricClip(m_clipGeometry));
    m_frameOutline = comp.CreateSpriteVisual();
    m_frameOutline.RelativeSizeAdjustment(float2{1, 1});
    m_windowRoot.Children().InsertAtTop(m_frameOutline);

    m_glass = std::make_unique<aero::GlassMaterial>(m_host.PlayerColorization(), m_aeroArt);
    m_root.Children().InsertAtTop(m_glass->Root());
    // DWM's highlights down the upper left and right frame, as on the library window.
    for (auto& v : m_sideHighlight) {
        v = comp.CreateSpriteVisual();
        m_root.Children().InsertAtTop(v);
    }

    // Everything inside the frame, clipped to the client area.
    m_content = comp.CreateContainerVisual();
    m_content.RelativeSizeAdjustment(float2{1, 1});
    m_contentClip = comp.CreateInsetClip();
    m_content.Clip(m_contentClip);
    m_root.Children().InsertAtTop(m_content);
    // Media Center's living light behind everything: it shows before the tiles arrive and
    // through the seams of the wall as its swell passes.
    m_light = std::make_unique<AeroLight>(m_content, AeroLight::Kind::Ground, true);
    m_wall = std::make_unique<AlbumWall>(m_content);

    m_shade = comp.CreateSpriteVisual();
    m_shade.RelativeSizeAdjustment(float2{1, 1});
    m_content.Children().InsertAtTop(m_shade);
    m_shadeOld = comp.CreateSpriteVisual(); // the shade before a change, fading out
    m_shadeOld.RelativeSizeAdjustment(float2{1, 1});
    m_shadeOld.Opacity(0.0f);
    m_content.Children().InsertAtTop(m_shadeOld);
    // The same light once more over the covers and the blue, faint: beams wandering across.
    m_veil = std::make_unique<AeroLight>(m_content, AeroLight::Kind::Veil, false);
    m_veil->Root().Opacity(0.9f);

    m_stage = comp.CreateContainerVisual();
    m_stage.RelativeSizeAdjustment(float2{1, 1});
    m_content.Children().InsertAtTop(m_stage);
    for (auto* v : {&m_coverOld, &m_cover, &m_info, &m_lyric}) {
        *v = comp.CreateSpriteVisual();
        m_stage.Children().InsertAtTop(*v);
    }
    // The sung line: two lines taking turns inside a clip, so one can roll out as the next
    // rolls in.
    m_lyric.Clip(comp.CreateInsetClip());
    for (auto& v : m_lyricLines) {
        v = comp.CreateSpriteVisual();
        v.Opacity(0.0f);
        m_lyric.Children().InsertAtTop(v);
    }
    m_coverOld.Opacity(0.0f);
    for (const auto& v : {m_cover, m_coverOld}) {
        auto shadow = comp.CreateDropShadow();
        shadow.Color(Rgba(0x000000, 1.0f));
        shadow.Opacity(kCoverShadow);
        v.Shadow(shadow);
    }
    // The cover's glass edge over each picture, apart from it so the compact window can fade it.
    for (size_t i = 0; i < m_coverFrame.size(); ++i) {
        m_coverFrame[i] = comp.CreateSpriteVisual();
        m_coverFrame[i].RelativeSizeAdjustment(float2{1, 1});
        (i == 0 ? m_cover : m_coverOld).Children().InsertAtTop(m_coverFrame[i]);
    }

    m_paneHost = comp.CreateContainerVisual();
    m_paneHost.RelativeSizeAdjustment(float2{1, 1});
    m_content.Children().InsertAtTop(m_paneHost);
    m_paneCard = comp.CreateSpriteVisual();
    m_paneCard.IsVisible(false);
    m_paneHost.Children().InsertAtBottom(m_paneCard);
    m_paneGlass = comp.CreateSpriteVisual(); // the card's glass, over its shadow and outline
    m_paneGlass.IsVisible(false);
    m_paneGlassShape = comp.CreateRoundedRectangleGeometry();
    m_paneGlass.Clip(comp.CreateGeometricClip(m_paneGlassShape));
    m_paneHost.Children().InsertAbove(m_paneGlass, m_paneCard);

    m_barLayer = comp.CreateContainerVisual();
    m_barLayer.RelativeSizeAdjustment(float2{1, 1});
    m_content.Children().InsertAtTop(m_barLayer);
    for (auto* v : {&m_barBack, &m_times, &m_seek, &m_slider}) {
        *v = comp.CreateSpriteVisual();
        m_barLayer.Children().InsertAtTop(*v);
    }
    m_seekThumb.Create(m_barLayer, false);
    m_sliderThumb.Create(m_barLayer, true);
    m_splitPlate = comp.CreateSpriteVisual(); // under the split button's two widgets
    m_splitPlate.Opacity(0.0f);
    m_barLayer.Children().InsertAtTop(m_splitPlate);
    m_cluster = comp.CreateSpriteVisual();
    m_barLayer.Children().InsertAtTop(m_cluster);

    m_capsLayer = comp.CreateContainerVisual();
    m_capsLayer.RelativeSizeAdjustment(float2{1, 1});
    m_content.Children().InsertAtTop(m_capsLayer);
    m_topBack = comp.CreateSpriteVisual();
    m_capsLayer.Children().InsertAtTop(m_topBack);

    m_menuLayer = comp.CreateContainerVisual();
    m_menuLayer.RelativeSizeAdjustment(float2{1, 1});
    m_content.Children().InsertAtTop(m_menuLayer);

    m_volumePanel = std::make_unique<WmcVolume>(m_content, WmcVolume::Kind::Panel);
    m_flyoutGlass = comp.CreateSpriteVisual(); // the transport behind the flyout's frame, frosted
    m_flyoutGlass.IsVisible(false);
    m_content.Children().InsertAtTop(m_flyoutGlass);
    m_volumeFlyout = comp.CreateSpriteVisual();
    m_volumeFlyout.IsVisible(false);
    m_content.Children().InsertAtTop(m_volumeFlyout);
    m_flyoutBall.Create(m_content, true); // its thumb, over it
    m_flyoutBall.root.IsVisible(false);
    m_volumeBubble = std::make_unique<WmcVolume>(m_content, WmcVolume::Kind::Bubble); // over the flyout

    // The frame: client edge lines and the caption buttons, above the content.
    m_clientEdge = comp.CreateSpriteVisual();
    m_root.Children().InsertAtTop(m_clientEdge);
    m_frameLayer = comp.CreateContainerVisual();
    m_frameLayer.RelativeSizeAdjustment(float2{1, 1});
    m_root.Children().InsertAtTop(m_frameLayer);
}

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
        ->onClick = [] {
        media::CancelPendingPlay(); // Stop also gives up a list still being read to play
        playback_control::get()->stop();
    };
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
        ->onClick = [] { playback_control::get()->previous(); };
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
        ->onClick = [] { playback_control::get()->next(); };
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
        ->onClick = [] {
        if (media::PendingPlay()) return; // a list asked to play is still read: not the old queue
        auto pc = playback_control::get();
        if (pc->is_playing())
            pc->play_or_pause();
        else
            pc->start();
    };

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
        if (media::radio::Active()) {
            media::radio::DislikeCurrent();
            return;
        }
        m_shuffle = !m_shuffle;
        m_host.SetPlayerMode(m_shuffle, m_repeat);
        if (auto* w = WidgetById(kMenuShuffle)) w->Invalidate();
    };
    WidgetById(kMenuShuffle)->enabled = [] { return !media::radio::Active() || media::radio::CanDislike(); };
    add(kMenuRepeat, m_menuLayer, menuItem(3), {})->onClick = [this] {
        m_repeat = NextRepeat(m_repeat);
        m_host.SetPlayerMode(m_shuffle, m_repeat);
        if (auto* w = WidgetById(kMenuRepeat)) w->Invalidate();
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
        ->onClick = [this] {
        if (media::radio::Active()) {
            media::radio::DislikeCurrent();
            return;
        }
        m_shuffle = !m_shuffle;
        m_host.SetPlayerMode(m_shuffle, m_repeat);
        for (int id : {kShuffle, kMenuShuffle})
            if (auto* w = WidgetById(id)) w->Invalidate();
    };
    WidgetById(kShuffle)->enabled = [] { return !media::radio::Active() || media::radio::CanDislike(); };
    add(kRepeat, m_barLayer,
        [this](auto* dc, D2D1_SIZE_F s, Layer l) {
            PlateButton(dc, s, l, m_repeat != 0, [&] {
                // A station repeats one song or none: repeat-one's glyph either way.
                InBox(dc, s, 38, 31, [&] { PlayModeGlyph(dc, false, media::radio::Active() ? 2 : m_repeat); });
            });
        },
        {})
        ->onClick = [this] {
        m_repeat = NextRepeat(m_repeat);
        m_host.SetPlayerMode(m_shuffle, m_repeat);
        for (int id : {kRepeat, kMenuRepeat})
            if (auto* w = WidgetById(id)) w->Invalidate();
    };

    // The mini window's Win7 caption buttons (the library window's, same artwork).
    const bool dwmArt = m_aeroArt;
    auto captionOptions = [&](bool close) {
        Widget::Options o;
        o.inactiveLayer = true;
        if (dwmArt) {
            o.glow = [close](ID2D1DeviceContext* dc, D2D1_SIZE_F s) { aero::atlas::CaptionGlow(dc, s, close); };
            o.glowOutset = aero::atlas::GlowOutset(close);
            o.glowOutsetInPixels = true;
        }
        return o;
    };
    add(kCapMin, m_frameLayer,
        [dwmArt](auto* dc, D2D1_SIZE_F s, Layer l) {
            if (dwmArt)
                aero::atlas::CaptionButton(dc, s, aero::atlas::Button::Minimize, paint::CaptionGlyph::Minimize, l);
            else
                paint::CaptionButton(dc, s, paint::CaptionGlyph::Minimize, l, true, false);
        },
        captionOptions(false))
        ->onClick = [this] { ShowWindow(m_hwnd, SW_MINIMIZE); };
    add(kCapMax, m_frameLayer,
        [dwmArt](auto* dc, D2D1_SIZE_F s, Layer l) {
            if (dwmArt)
                aero::atlas::CaptionButton(dc, s, aero::atlas::Button::Maximize, paint::CaptionGlyph::Maximize, l);
            else
                paint::CaptionButton(dc, s, paint::CaptionGlyph::Maximize, l, false, false);
        },
        captionOptions(false))
        ->onClick = [this] { Show(Mode::Full); }; // maximizing the player is the full screen
    add(kCapClose, m_frameLayer,
        [dwmArt](auto* dc, D2D1_SIZE_F s, Layer l) {
            if (dwmArt)
                aero::atlas::CaptionButton(dc, s, aero::atlas::Button::Close, paint::CaptionGlyph::Close, l);
            else
                paint::CaptionButton(dc, s, paint::CaptionGlyph::Close, l, false, true);
        },
        captionOptions(true))
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

void PlayerWindow::FadingThumb::Create(const wuc::ContainerVisual& parent, bool visible) {
    const auto& comp = gfx::CompositionHost::Get().Compositor();
    root = comp.CreateContainerVisual();
    root.Opacity(visible ? 1.0f : 0.0f);
    shown = visible;
    parent.Children().InsertAtTop(root);
    for (size_t i = 0; i < looks.size(); ++i) {
        looks[i] = comp.CreateSpriteVisual();
        looks[i].Opacity(i == 0 ? 1.0f : 0.0f); // at rest, lit, held
        root.Children().InsertAtTop(looks[i]);
    }
}

void PlayerWindow::FadingThumb::Paint(int w, int h, float scale, bool force,
                                      const std::function<void(ID2D1DeviceContext*, int)>& look) {
    if (!root || (paintedScale == scale && !force)) return;
    paintedScale = scale;
    for (int i = 0; i < 3; ++i) {
        auto& sprite = looks[static_cast<size_t>(i)];
        sprite.Size(float2{static_cast<float>(w), static_cast<float>(h)});
        gfx::CompositionHost::Get().Repaint(sprite, backings[static_cast<size_t>(i)], w, h, scale,
                                            [&](ID2D1DeviceContext* dc) { look(dc, i); });
    }
}

void PlayerWindow::FadingThumb::Place(int x, int y, int w, int h) {
    if (!root) return;
    root.Offset(float3{static_cast<float>(x), static_cast<float>(y), 0});
    root.Size(float2{static_cast<float>(w), static_cast<float>(h)});
}

void PlayerWindow::FadingThumb::Show(bool show, int inMs, int outMs) {
    if (!root || show == shown) return;
    shown = show;
    if (inMs <= 0 && outMs <= 0) {
        root.StopAnimation(L"Opacity");
        root.Opacity(show ? 1.0f : 0.0f);
        return;
    }
    FadeTo(root, show ? 1.0f : 0.0f, show ? inMs : outMs);
}

void PlayerWindow::FadingThumb::SetState(bool lit, bool hold, int inMs, int outMs) {
    if (!root) return;
    if (lit != hot) {
        hot = lit;
        FadeTo(looks[1], lit ? 1.0f : 0.0f, lit ? inMs : outMs);
    }
    if (hold != held) {
        held = hold;
        FadeTo(looks[2], hold ? 1.0f : 0.0f, hold ? 60 : outMs);
    }
}

// The press: Media Center's highlight runs across the button and an afterglow fades behind it.
void PlayerWindow::MenuPressed(int index) {
    const RECT& r = m_layout.menu[static_cast<size_t>(index)];
    if (Empty(r) || !m_menuFx) return;
    const auto& comp = gfx::CompositionHost::Get().Compositor();
    const float w = static_cast<float>(r.right - r.left), h = static_cast<float>(r.bottom - r.top);
    m_menuFx.Offset(float3{static_cast<float>(r.left), static_cast<float>(r.top), 0});
    m_menuFx.Size(float2{w, h});
    const auto out = comp.CreateCubicBezierEasingFunction({0.2f, 0.7f}, {0.2f, 1.0f});
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

// Off -> all -> one; a station repeats one song or none.
int PlayerWindow::NextRepeat(int repeat) {
    if (media::radio::Active()) return repeat == 0 ? 2 : 0;
    return (repeat + 1) % 3;
}

void PlayerWindow::WallAlbumsChanged() {
    if (m_shown && m_wall) m_wall->UpdateAlbums(m_host.WallAlbums());
}

void PlayerWindow::RadioChanged() {
    bool shuffle = false;
    int repeat = 0;
    m_host.PlayerMode(shuffle, repeat);
    m_shuffle = shuffle;
    m_repeat = repeat;
    for (int id : {kPrev, kShuffle, kRepeat, kMenuShuffle, kMenuRepeat})
        if (auto* w = WidgetById(id)) w->Invalidate();
}

std::wstring PlayerWindow::MenuText(int index) const {
    if (index == 2 && media::radio::CanDislike()) return Tr(L"Not interested");
    return index == 0   ? Tr(L"Lyrics")
           : index == 1 ? Tr(L"Up Next")
           : index == 2 ? Tr(L"Shuffle")
                        : Tr(m_repeat == 2 || media::radio::Active() ? L"Repeat one" : L"Repeat");
}

RECT PlayerWindow::ClusterRect(const D2D1_RECT_F& r) const {
    const POINT o = m_layout.cluster;
    return {o.x + PxI(r.left), o.y + PxI(r.top), o.x + PxI(r.right), o.y + PxI(r.bottom)};
}

D2D1_RECT_F PlayerWindow::ClusterRegion(const RECT& r) const {
    const POINT o = m_layout.cluster;
    return {(r.left - o.x) / m_scale, (r.top - o.y) / m_scale, (r.right - o.x) / m_scale,
            (r.bottom - o.y) / m_scale};
}

// ---- Showing -------------------------------------------------------------------------------

void PlayerWindow::Show(Mode mode) {
    const bool first = !m_shown;
    if (!m_hwnd) Create();
    if (!first && mode == m_mode) {
        if (IsIconic(m_hwnd)) ShowWindow(m_hwnd, SW_RESTORE);
        SetForegroundWindow(m_hwnd);
        return;
    }
    if (first) {
        m_pane = m_host.LendPane(m_paneHost);
        m_wall->SetAlbums(m_host.WallAlbums());
        m_volume = m_host.PlayerVolume();
        m_muted = playback_control::get()->is_muted();
        m_track.release();
        m_art = nullptr;
    }
    m_mode = mode;
    m_shown = true;
    // The pane changes size, scale of its content and form between the two: hide it across
    // the change so its rows are laid out and painted afresh instead of showing old surfaces.
    const bool repane = !first && m_pane && m_paneShown;
    if (repane) m_pane->SetVisible(false);
    ApplyMode(first);
    if (repane && m_paneShown) { // ApplyMode may have closed it (a mini window too small)
        m_pane->SetVisible(true);
        StartFrames();
    }
    if (first) {
        ReadTrack();
        m_wall->Reveal();
        // The stage and the controls follow the first tiles.
        for (const wuc::Visual& v : {wuc::Visual(m_stage), wuc::Visual(m_barLayer)}) {
            v.Opacity(0.0f);
            auto fade = v.Compositor().CreateScalarKeyFrameAnimation();
            fade.InsertKeyFrame(0.0f, 0.0f);
            fade.InsertKeyFrame(1.0f, 1.0f);
            fade.Duration(std::chrono::milliseconds(420));
            fade.DelayTime(std::chrono::milliseconds(260));
            fade.DelayBehavior(wuc::AnimationDelayBehavior::SetInitialValueBeforeDelay);
            v.StartAnimation(L"Opacity", fade);
        }
        if (m_pane) m_pane->SetVisible(m_paneShown);
        if (m_paneShown && m_mode == Mode::Mini) {
            m_stage.StopAnimation(L"Opacity");
            m_stage.Opacity(0.0f);
        }
    } else {
        gfx::CompositionHost::Get().AnimateOpacity(m_stage, m_paneShown && m_mode == Mode::Mini ? 0.0f : 1.0f,
                                                   kFadeMs);
    }
    UpdateMotion();
    SetForegroundWindow(m_hwnd);
}

void PlayerWindow::ApplyMode(bool first) {
    SetPopup(false);
    HMONITOR monitor = MonitorFromWindow(first ? core_api::get_main_window() : m_hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(monitor, &mi);
    if (!first && m_mode == Mode::Full && !IsIconic(m_hwnd)) {
        RememberMiniRect(); // leaving the mini form: remember where it was
    }
    const bool mini = m_mode == Mode::Mini;
    m_shadow.SetSuppressed(!mini); // its corners showed as rounded lines round the screen
    m_glass->Root().IsVisible(mini);
    m_frameOutline.IsVisible(mini);
    m_clientEdge.IsVisible(mini);
    m_frameLayer.IsVisible(mini);
    if (!mini) {
        SetWindowLongPtrW(m_hwnd, GWL_STYLE, WS_POPUP | WS_CLIPCHILDREN | WS_MINIMIZEBOX);
        const RECT& r = mi.rcMonitor;
        SetWindowPos(m_hwnd, HWND_NOTOPMOST, r.left, r.top, r.right - r.left, r.bottom - r.top,
                     SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    } else {
        SetWindowLongPtrW(m_hwnd, GWL_STYLE,
                          WS_POPUP | WS_THICKFRAME | WS_MINIMIZEBOX | WS_SYSMENU | WS_CLIPCHILDREN);
        if (!m_haveMiniRect) {
            // Where it was left last time, if that is still on a screen.
            if (const auto saved = settings::MiniPlayerRect(); saved && MonitorFromRect(&*saved, MONITOR_DEFAULTTONULL)) {
                m_miniRect = *saved;
                m_haveMiniRect = true;
            }
        }
        if (!m_haveMiniRect) {
            const float scale = GetDpiForWindow(m_hwnd) / 96.0f;
            const int w = static_cast<int>(kMiniW * scale);
            const int h = static_cast<int>((m_paneShown ? kMiniPaneH : kMiniH) * scale);
            const int margin = static_cast<int>(32 * scale);
            m_miniRect = {mi.rcWork.right - margin - w, mi.rcWork.bottom - margin - h, mi.rcWork.right - margin,
                          mi.rcWork.bottom - margin};
            m_haveMiniRect = true;
        }
        SetWindowPos(m_hwnd, HWND_NOTOPMOST, m_miniRect.left, m_miniRect.top, m_miniRect.right - m_miniRect.left,
                     m_miniRect.bottom - m_miniRect.top, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        // The lyrics or the queue opened full screen do not fit every mini window: one lower
        // than the mini window grows to for them (or a compact one) shows the song instead, at
        // the size it was left (owner, 2026-11-14: the pane stood under the control bar).
        if (m_paneShown && m_pane) {
            const float s = GetDpiForWindow(m_hwnd) / 96.0f;
            const int w = m_miniRect.right - m_miniRect.left, h = m_miniRect.bottom - m_miniRect.top;
            if (w < std::lround(kMiniMinW * s) || h < std::lround(kMiniPaneH * s)) {
                m_paneShown = false;
                m_miniCoverHeight = 0;
                m_pane->SetVisible(false);
                m_stage.StopAnimation(L"Opacity");
                m_stage.Opacity(1.0f);
                for (int id : {kLyrics, kList, kMenuLyrics, kMenuList, kLibrary})
                    if (auto* widget = WidgetById(id)) widget->Invalidate();
            }
        }
    }
    m_scale = GetDpiForWindow(m_hwnd) / 96.0f;
    m_chromeShown = false;
    ShowChrome(true);
    UpdateLayout(false);
    for (auto& w : m_widgets) w->Invalidate(); // the two forms draw different artwork
    m_glass->UpdateReflectionAnchor(m_hwnd);
    m_shadow.Sync();
    ArmHideTimer();
}

// The mini window's place, for its next showing and the next start.
void PlayerWindow::RememberMiniRect() {
    if (!GetWindowRect(m_hwnd, &m_miniRect)) return;
    m_haveMiniRect = true;
    settings::SetMiniPlayerRect(m_miniRect);
}

void PlayerWindow::Close() {
    if (!m_shown) return;
    m_shown = false;
    SetPopup(false);
    for (UINT_PTR timer : {kHideTimer, kOverlayTimer, kRepeatTimer, kSizeTimer})
        KillTimer(m_hwnd, timer);
    if (m_cursorHidden) {
        m_cursorHidden = false;
        SetCursor(LoadCursorW(nullptr, IDC_ARROW));
    }
    if (m_mode == Mode::Mini && !IsIconic(m_hwnd)) RememberMiniRect();
    if (m_frameClock) m_frameClock->Stop();
    if (m_pane) m_pane->SetVisible(false);
    m_pane = nullptr;
    m_capture = Capture::None;
    SetHover(nullptr);
    m_pressed = nullptr;
    m_host.ReturnPane();
    ShowWindow(m_hwnd, SW_HIDE);
    UpdateMotion();
    m_shadow.Sync();
    m_host.PlayerClosed();
}

void PlayerWindow::UpdateMotion() {
    const bool visible = m_shown && m_hwnd && !IsIconic(m_hwnd);
    // The sung line follows the words while music plays, on the compositor's frames: a
    // WM_TIMER is only synthesised when no posted message waits, and the frame clocks of this
    // thread's windows keep posting at the display's rate, so a timer could starve to the
    // once-a-second playback time callback.
    if (LyricLineLive() || SeekLive()) StartFrames();
    if (m_wall) { // paused, the wall slows down rather than stopping (WMC)
        m_wall->SetPaused(m_paused);
        m_wall->SetRunning(visible && m_playing);
    }
    if (m_light) m_light->SetRunning(visible);
    if (m_veil) m_veil->SetRunning(visible);
}

void PlayerWindow::StartFrames() {
    if (!m_shown || !m_hwnd) return;
    if (!m_frameClock) m_frameClock = std::make_unique<gfx::FrameClock>(m_hwnd, kFrameMessage);
    m_frameClock->Start();
}

bool PlayerWindow::LyricLineLive() const {
    return m_shown && m_hwnd && !IsIconic(m_hwnd) && m_playing && !m_paused && m_lyrics && m_lyrics->synced &&
           !m_paneShown && settings::LyricLineShown();
}

// The seek bar's head and the times follow the music on the same frames, a device pixel and a
// second at a time, instead of jumping with the playback time callback once a second.
bool PlayerWindow::SeekLive() const {
    return m_shown && m_hwnd && !IsIconic(m_hwnd) && m_playing && !m_paused && m_length > 0 &&
           !Empty(m_layout.seek);
}

bool PlayerWindow::TickSeek() {
    if (!SeekLive()) return false;
    if (m_seeking) return true;
    double position = m_position;
    try {
        position = playback_control::get()->playback_get_position();
    } catch (...) {
        return true;
    }
    const float progress = static_cast<float>(std::clamp(position / m_length, 0.0, 1.0));
    const bool moved = std::lround(SeekHead(progress) * m_scale) != m_seekHead;
    const bool newSecond = static_cast<long>(position) != static_cast<long>(m_position);
    if (!moved && !newSecond) return true;
    m_position = position;
    if (moved) PaintSeek();
    if (newSecond) {
        PaintTimes();
        PaintInfo(); // its time line
    }
    return true;
}

bool PlayerWindow::TickLyric() {
    if (!LyricLineLive() || Empty(m_layout.lyric)) return false;
    int ms = 0;
    const int index = CurrentLyricLine(&ms);
    // Every frame while a word-timed line fills (as in the library window's transport band);
    // otherwise only when the next line comes.
    const bool filling = index >= 0 && !m_lyrics->lines[static_cast<size_t>(index)].words.empty() &&
                         ms <= m_lyrics->lines[static_cast<size_t>(index)].end + 50;
    if (filling || index != m_lyricLine) PaintLyric();
    return true;
}

void PlayerWindow::SetPaneShown(bool shown, NowPlayingPane::Mode mode) {
    if (!m_pane) return;
    const bool changed = shown != m_paneShown;
    const bool mini = m_mode == Mode::Mini;
    m_paneShown = shown;
    if (shown) m_pane->SetMode(mode);
    auto& host = gfx::CompositionHost::Get();
    const auto& comp = host.Compositor();
    if (changed) {
        m_pane->SetVisible(shown);
        host.AnimateOpacity(m_stage, shown && mini ? 0.0f : 1.0f, kFadeMs);
        if (shown) {
            // The pane arrives: it rises out of the blue (from the right when full screen)
            // and settles, instead of standing there at once.
            const float3 from = mini ? float3{0, Px(28), 0} : float3{Px(56), 0, 0};
            auto move = comp.CreateVector3KeyFrameAnimation();
            move.InsertKeyFrame(0.0f, from);
            move.InsertKeyFrame(1.0f, float3{0, 0, 0}, comp.CreateCubicBezierEasingFunction({0.16f, 1.0f}, {0.3f, 1.0f}));
            move.Duration(Ms(560));
            m_paneHost.StartAnimation(L"Offset", move);
            auto fade = comp.CreateScalarKeyFrameAnimation();
            fade.InsertKeyFrame(0.0f, 0.0f);
            fade.InsertKeyFrame(1.0f, 1.0f, comp.CreateCubicBezierEasingFunction({0.3f, 0.0f}, {0.3f, 1.0f}));
            fade.Duration(Ms(460));
            fade.DelayTime(Ms(mini ? 60 : 120));
            fade.DelayBehavior(wuc::AnimationDelayBehavior::SetInitialValueBeforeDelay);
            m_paneHost.StartAnimation(L"Opacity", fade);
        }
    }
    if (changed && mini && !IsIconic(m_hwnd)) {
        // The mini window grows downwards for the pane and returns to the very height it had.
        RECT wr{};
        GetWindowRect(m_hwnd, &wr);
        MONITORINFO mi{sizeof(mi)};
        GetMonitorInfoW(MonitorFromWindow(m_hwnd, MONITOR_DEFAULTTONEAREST), &mi);
        int height = wr.bottom - wr.top;
        if (shown) {
            m_miniCoverHeight = height;
            height = std::max(height, PxI(kMiniPaneH));
        } else if (m_miniCoverHeight > 0) {
            height = m_miniCoverHeight;
        }
        height = std::min<int>(height, mi.rcWork.bottom - mi.rcWork.top);
        int top = wr.top;
        if (top + height > mi.rcWork.bottom) top = mi.rcWork.bottom - height;
        AnimateWindowTo({wr.left, top, wr.right, top + height});
    }
    UpdateLayout(changed);
    for (int id : {kLyrics, kList, kMenuLyrics, kMenuList, kLibrary})
        if (auto* w = WidgetById(id)) w->Invalidate();
}

// Media Center's back orb belongs to the lyrics and the queue: when one of them opens it
// slides down from above the screen, quickly out of the top and softly into place, and when
// the song returns it goes back up the way it came. Out of sight it is transparent and takes
// no clicks; Widget::SetBounds keeps placing it, which the running slide overrides.
void PlayerWindow::SlideBack(bool animate) {
    auto* w = WidgetById(kLibrary);
    if (!w) return;
    const bool in = !Wmc() || m_paneShown;
    const RECT& b = w->Bounds();
    const wuc::Visual v = w->Visual();
    const float3 at{static_cast<float>(b.left), static_cast<float>(b.top), 0};
    const float3 above{at.x, -static_cast<float>(b.bottom - b.top) - Px(8), 0};
    // Only the pane's coming and going slides it; entering or leaving the full screen sets it.
    const bool slide = animate && m_shown && !Empty(b) && Wmc() == m_backWmc;
    m_backWmc = Wmc();
    if (in == m_backIn) return;
    m_backIn = in;
    v.StopAnimation(L"Offset");
    v.StopAnimation(L"Opacity");
    v.Offset(at);
    if (!slide) {
        v.Opacity(in ? 1.0f : 0.0f);
        return;
    }
    const auto& comp = gfx::CompositionHost::Get().Compositor();
    const auto ease = in ? comp.CreateCubicBezierEasingFunction({0.16f, 1.0f}, {0.3f, 1.0f})
                         : comp.CreateCubicBezierEasingFunction({0.5f, 0.0f}, {0.85f, 0.35f});
    auto move = comp.CreateVector3KeyFrameAnimation();
    move.InsertKeyFrame(0.0f, in ? above : at);
    move.InsertKeyFrame(1.0f, in ? at : above, ease);
    move.Duration(Ms(in ? kBackInMs : kBackOutMs));
    v.StartAnimation(L"Offset", move);
    auto fade = comp.CreateScalarKeyFrameAnimation();
    fade.InsertKeyFrame(0.0f, in ? 0.0f : 1.0f);
    fade.InsertKeyFrame(1.0f, in ? 1.0f : 0.0f, ease);
    fade.Duration(Ms(in ? kBackInMs / 2 : kBackOutMs));
    v.StartAnimation(L"Opacity", fade);
}

// Sizes the mini window over a few frames, quick at first and soft at the end.
void PlayerWindow::AnimateWindowTo(const RECT& target) {
    GetWindowRect(m_hwnd, &m_sizeFrom);
    m_sizeTo = target;
    if (EqualRect(&m_sizeFrom, &m_sizeTo)) return;
    m_sizeStart = GetTickCount64();
    SetTimer(m_hwnd, kSizeTimer, 8, nullptr);
}

void PlayerWindow::TogglePane(NowPlayingPane::Mode mode) {
    if (!m_pane) return;
    if (m_paneShown && m_pane->CurrentMode() == mode)
        SetPaneShown(false, mode);
    else
        SetPaneShown(true, mode);
}

// Full screen, Media Center's chrome does not just fade: the top bar leaves upwards, the
// bottom bar downwards, the menu to the left, and they come back the same way, quickly out of
// rest and softly into place. The stage then regroups around what is left.
void PlayerWindow::ShowChrome(bool show) {
    if (show == m_chromeShown) return;
    m_chromeShown = show;
    // Only the full screen hides the pointer with its chrome. Leaving it with the chrome away
    // (Escape, a double click) used to keep the pointer hidden over the mini window.
    const bool hideCursor = !show && m_mode == Mode::Full;
    if (hideCursor != m_cursorHidden) {
        m_cursorHidden = hideCursor;
        SetCursor(hideCursor ? nullptr : LoadCursorW(nullptr, IDC_ARROW));
    }
    auto& host = gfx::CompositionHost::Get();
    const auto& comp = host.Compositor();
    if (m_mode != Mode::Full) {
        // The mini window: the control bar and the caption buttons fade where they stand -
        // quickly back on a move, slowly away when the pointer has rested or left.
        for (const wuc::Visual& v : {wuc::Visual(m_barLayer), wuc::Visual(m_capsLayer), wuc::Visual(m_menuLayer)}) {
            v.StopAnimation(L"Offset");
            v.Offset(float3{0, 0, 0});
        }
        host.AnimateOpacity(m_barLayer, show ? 1.0f : 0.0f, show ? 180 : 600);
        host.AnimateOpacity(m_capsLayer, show ? 1.0f : 0.0f, show ? 160 : 420);
        if (!show) {
            SetPopup(false);
            ShowBubble(false);
        }
        return;
    }
    // Each layer - its controls and the shade or bar behind them together - drifts toward its
    // edge of the screen while it fades, and comes back from there fading in: the fade runs
    // ahead of the move, so a layer is mostly gone before it reaches the edge (owner,
    // 2026-11-14: the shades slid away whole instead of fading on the way).
    const auto slide = [&](const wuc::Visual& v, float dx, float dy, int delayMs) {
        const auto moveEase = show ? comp.CreateCubicBezierEasingFunction({0.16f, 1.0f}, {0.3f, 1.0f})
                                    : comp.CreateCubicBezierEasingFunction({0.4f, 0.0f}, {0.6f, 1.0f});
        const auto fadeEase = comp.CreateCubicBezierEasingFunction({0.25f, 0.1f}, {0.25f, 1.0f});
        auto move = comp.CreateVector3KeyFrameAnimation();
        move.InsertKeyFrame(1.0f, show ? float3{0, 0, 0} : float3{dx * 0.6f, dy * 0.6f, 0}, moveEase);
        move.Duration(Ms(show ? 480 : 600));
        move.DelayTime(Ms(delayMs));
        v.StartAnimation(L"Offset", move);
        auto fade = comp.CreateScalarKeyFrameAnimation();
        fade.InsertKeyFrame(1.0f, show ? 1.0f : 0.0f, fadeEase);
        fade.Duration(Ms(show ? 360 : 420));
        fade.DelayTime(Ms(delayMs));
        v.StartAnimation(L"Opacity", fade);
    };
    const float barH = static_cast<float>(m_client.cy - m_layout.bar.top) + Px(12);
    const float topH = static_cast<float>(std::max<LONG>(m_layout.top.bottom, PxI(70)));
    const float menuW = static_cast<float>(m_layout.menuShade.right) + Px(20);
    slide(m_barLayer, 0, barH, 0);
    slide(m_capsLayer, 0, -topH, 0);
    slide(m_menuLayer, -menuW, 0, show ? 60 : 0);
    if (!m_paneShown) UpdateLayout(true); // the cover and its lines take the room the menu left
}

void PlayerWindow::ArmHideTimer() {
    KillTimer(m_hwnd, kHideTimer);
    if (m_shown) SetTimer(m_hwnd, kHideTimer, kHideAfterMs, nullptr);
}

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

// ---- Layout --------------------------------------------------------------------------------

void PlayerWindow::UpdateLayout(bool animate) {
    RECT rc{};
    GetClientRect(m_hwnd, &rc);
    const int W = rc.right, H = rc.bottom;
    if (W <= 0 || H <= 0) return;
    m_client = {W, H};
    Layout L;
    const bool full = m_mode == Mode::Full;
    m_splitVolume = false; // set again below for a narrow mini window
    const bool wmc = Wmc();
    const int line = std::max(1, static_cast<int>(std::floor(m_scale)));

    // Frame.
    if (full) {
        L.client = {0, 0, W, H};
    } else {
        const int border = PxI(m::kResizeBorder), capH = PxI(m::kCaptionHeight);
        L.client = {border, capH, W - border, H - border};
        L.clientEdge = {L.client.left - 2, L.client.top - 2, L.client.right + 2, L.client.bottom + 2};
        const bool dwm = m_aeroArt;
        const int capRight = W - PxI(dwm ? 20.0f / 3 : m::kCapBtnRightInset);
        const int capTop = dwm ? line : PxI(m::kCapBtnTopInset);
        const int bh = PxI(dwm ? 29.0f / 1.5f : m::kCapBtnH);
        L.capClose = {capRight - PxI(dwm ? 73.0f / 1.5f : m::kCapBtnCloseW), capTop, capRight, capTop + bh};
        L.capMax = {L.capClose.left - PxI(dwm ? 41.0f / 1.5f : m::kCapBtnMaxW), capTop, L.capClose.left, capTop + bh};
        L.capMin = {L.capMax.left - PxI(dwm ? 43.0f / 1.5f : m::kCapBtnMinW), capTop, L.capMax.left, capTop + bh};
    }
    const RECT C = L.client;
    const int CW = C.right - C.left;
    const bool compact = !full && !m_paneShown && W < PxI(kMiniMinW);
    L.compact = compact;

    if (wmc) {
        // Media Center: the bottom bar across the screen, its controls at the right in WMC's
        // order, the seek bar in the space left of them; back orb and caption buttons on top.
        L.bar = {0, H - PxI(kWmcBar), W, H};
        L.top = {0, 0, W, PxI(kWmcTop)};
        const int cy = L.bar.top + PxI(kWmcBar / 2 + 1);
        int x = W - PxI(14);
        const auto take = [&](float w, float h, int gap = 0) {
            const RECT r{x - PxI(w), cy - PxI(h) / 2, x, cy - PxI(h) / 2 + PxI(h)};
            x = r.left - gap;
            return r;
        };
        L.volUp = take(38, 31);
        L.volDown = take(38, 31);
        L.mute = take(38, 31, PxI(5));
        L.divider[0] = take(2, 20, PxI(5));
        L.next = take(38, 31);
        L.play = take(42, 43);
        L.prev = take(38, 31);
        L.stop = take(38, 31, PxI(20)); // lyrics and queue are in the menu beside the cover
        L.favorite = take(38, 31, PxI(20));
        L.timeLeft = {PxI(20), cy - PxI(9), PxI(20 + 52), cy + PxI(9)};
        L.timeRight = {x - PxI(52), cy - PxI(9), x, cy + PxI(9)};
        L.seek = {L.timeLeft.right + PxI(2), cy - PxI(12), L.timeRight.left - PxI(2), cy + PxI(12)};
        // Back to the song: placed always, shown only with the lyrics or the queue (SlideBack).
        L.library = {PxI(14), PxI(8), PxI(14 + 42), PxI(8 + 42)};
        L.exit = {W - PxI(12 + 29), PxI(10), W - PxI(12), PxI(10 + 24)};
        L.restore = {L.exit.left - PxI(29), L.exit.top, L.exit.left, L.exit.bottom};
        L.minimize = {L.restore.left - PxI(29), L.exit.top, L.restore.left, L.exit.bottom};
        // Media Center's volume overlay: 57 DIP in from the right, 58 down (volume.mp4).
        L.popup = {W - PxI(57 + WmcVolume::kPanelW), PxI(58), W - PxI(57), PxI(58 + WmcVolume::kPanelH)};
    } else {
        // WMP 12's narrow transport: seek row, then stop / previous / play / next centred,
        // the speaker with its arrow at the left, lyrics and queue at the right.
        const int barH = PxI(kMiniBar);
        if (full) {
            const int bw = std::min(W - 2 * PxI(kFullBarMargin), PxI(kFullBarMaxW));
            L.bar = {(W - bw) / 2, H - PxI(kFullBarMargin) - barH, (W - bw) / 2 + bw, H - PxI(kFullBarMargin)};
            L.library = {W - PxI(16 + 30), PxI(16), W - PxI(16), PxI(16 + 26)};
        } else {
            L.bar = {C.left, C.bottom - barH, C.right, C.bottom};
            // No back button here: the frame's close button returns to the library.
        }
        const int rowY = L.bar.top + PxI(4);
        L.timeLeft = {L.bar.left + PxI(8), rowY + PxI(3), L.bar.left + PxI(8 + 46), rowY + PxI(21)};
        L.timeRight = {L.bar.right - PxI(8 + 46), rowY + PxI(3), L.bar.right - PxI(8), rowY + PxI(21)};
        L.seek = {L.timeLeft.right, rowY, L.timeRight.left, rowY + PxI(24)};
        const int cx = (L.bar.left + L.bar.right) / 2, cy = L.bar.top + PxI(52);
        if (m_wmp) {
            m_layout.cluster = L.cluster = {cx - PxI(108), cy - PxI(26)};
            L.play = ClusterRect(wt::kOrbRect);
            L.prev = ClusterRect(wt::kPrevRect);
            L.next = ClusterRect(wt::kNextRect);
            L.stop = ClusterRect(wt::kStopRect); // 25x25, the plate of the buttons beside it
        } else {
            const int orbPx = PxI(m::kOrbDiameter + 2.0f), segH = PxI(m::kSegmentHeight);
            L.play = CenteredRect(cx, cy, orbPx, orbPx);
            L.prev = {cx - PxI(m::kSegmentReach), cy - segH / 2, cx, cy - segH / 2 + segH};
            L.next = {cx, cy - segH / 2, cx + PxI(m::kSegmentReach), cy - segH / 2 + segH};
            L.stop = CenteredRect(cx + PxI(m::kStopX), cy, PxI(m::kIconButtonW), PxI(m::kIconButtonH));
        }
        if (compact) { // WMP's small window: the transport at the left, the volume at the right
            const int dx = L.bar.left + PxI(8) - std::min({L.stop.left, L.prev.left, L.play.left});
            for (RECT* r : {&L.stop, &L.prev, &L.play, &L.next}) OffsetRect(r, dx, 0);
            L.cluster.x += dx;
            m_layout.cluster = L.cluster;
        }
        // The speaker; beside it the volume slider where there is room before the transport,
        // else an arrow on the speaker's button that opens the volume flyout. With WMP's art
        // both are WMP 12's (its speaker 25x25, its slider 80x23 five DIP on); else Media
        // Center's speaker and bar.
        const bool wmcSpeaker = m_ehres && !m_wmp;
        const int muteW = PxI(wmcSpeaker ? 38.0f : m_wmp ? 25.0f : 28.0f),
                  muteH = PxI(wmcSpeaker ? 31.0f : m_wmp ? 25.0f : 26.0f);
        const int muteL = L.bar.left + PxI(wmcSpeaker ? 4.0f : 10.0f);
        const int sliderL = muteL + muteW + PxI(m_wmp ? 5.0f : 4.0f), sliderR = sliderL + PxI(m_wmp ? kSliderW : 90.0f);
        const bool slider = sliderR + PxI(14) <= std::min({L.stop.left, L.prev.left, L.play.left});
        m_splitVolume = !slider && m_wmp; // WMP 12's split button, 39x25: speaker 25, arrow 14
        if (m_splitVolume) {
            const int top = cy - PxI(12.5f);
            L.mute = {muteL, top, muteL + PxI(25), top + PxI(25)};
            L.volArrow = {L.mute.right, top, L.mute.right + PxI(14), top + PxI(25)};
        } else
            L.mute = {muteL, cy - muteH / 2, muteL + muteW + (wmcSpeaker && !slider ? PxI(16) : 0), cy - muteH / 2 + muteH};
        if (slider && m_wmp)
            L.slider = {sliderL, L.mute.top, sliderR, L.mute.top + PxI(kSliderH)};
        else if (slider)
            L.slider = {sliderL, cy - PxI(9), sliderR, cy + PxI(9)};
        else if (!m_ehres && !m_splitVolume)
            L.volArrow = {L.mute.right + PxI(1), cy - PxI(8), L.mute.right + PxI(17), cy + PxI(8)};
        if (compact) {
            const int dv = L.bar.right - PxI(8) - std::max(L.mute.right, L.volArrow.right);
            OffsetRect(&L.mute, dv, 0);
            if (!Empty(L.volArrow)) OffsetRect(&L.volArrow, dv, 0);
        }
        // Shuffle and repeat before stop (WMP 12's 25x25 buttons, 6 apart, 8 before stop),
        // where they leave room after the volume.
        {
            const int bs = PxI(25), top = cy - PxI(12.5f);
            L.repeat = {L.stop.left - PxI(8) - bs, top, L.stop.left - PxI(8), top + bs};
            L.shuffle = {L.repeat.left - PxI(6) - bs, top, L.repeat.left - PxI(6), top + bs};
            const LONG volumeRight = std::max({L.mute.right, L.volArrow.right, L.slider.right});
            if (L.shuffle.left < volumeRight + PxI(10)) L.shuffle = L.repeat = {};
        }
        if (m_ehres) { // three buttons on WMP's plate, 29x25, 6 apart
            const int bw = PxI(29), bh = PxI(25), gap = PxI(6);
            L.list = {L.bar.right - PxI(10) - bw, cy - bh / 2, L.bar.right - PxI(10), cy - bh / 2 + bh};
            L.lyrics = {L.list.left - gap - bw, L.list.top, L.list.left - gap, L.list.bottom};
            L.favorite = {L.lyrics.left - gap - bw, L.list.top, L.lyrics.left - gap, L.list.bottom};
        } else {
            L.list = {L.bar.right - PxI(10 + 30), cy - PxI(13), L.bar.right - PxI(10), cy + PxI(13)};
            L.lyrics = {L.list.left - PxI(34), L.list.top, L.list.left - PxI(4), L.list.bottom};
            L.favorite = {L.lyrics.left - PxI(4 + 30), L.list.top, L.lyrics.left - PxI(4), L.list.bottom};
        }
        // A narrow window has no room for the heart beside the transport: it leaves first.
        if (L.favorite.left < std::max({L.next.right, L.play.right, L.stop.right}) + PxI(6)) L.favorite = {};
        if (compact) L.favorite = L.lyrics = L.list = L.shuffle = L.repeat = {};
        if (full)
            L.popup = {W - PxI(57 + WmcVolume::kPanelW), PxI(58), W - PxI(57), PxI(58 + WmcVolume::kPanelH)};
        else { // WMP's flyout over the button, its right edge on the arrow's
            const int fw = PxI(kFlyoutW), fh = PxI(kFlyoutH), mid = (L.mute.top + L.mute.bottom) / 2;
            const LONG right = m_splitVolume ? L.volArrow.right : L.mute.right;
            const int left = std::max<int>(right - fw, L.client.left + PxI(2));
            L.popup = {left, mid - fh / 2, left + fw, mid - fh / 2 + fh};
        }
    }

    // The compact window's cover: the square client (WM_SIZING keeps it square), less a margin
    // that opens first as the window grows from its smallest.
    if (compact) {
        const int S = std::min<int>(CW, C.bottom - C.top);
        const int fullMargin = PxI(kCompactMargin);
        const int margin = std::clamp((S - PxI(kCompactSide)) / 2, 0, fullMargin);
        const int side = S - 2 * margin;
        const int left = C.left + (CW - side) / 2, top = C.top + (C.bottom - C.top - side) / 2;
        L.cover = {left, top, left + side, top + side};
        L.coverEdge = static_cast<float>(margin) / std::max(1, fullMargin);
        L.top = {C.left, C.top, C.right, C.top + PxI(kCompactTitle * 2.6f)};
    }

    // The stage (cover and text) and the lyrics / queue pane.
    // In the mini window the control bar comes and goes over the picture (it hides when the
    // pointer rests): the stage, the lines and the pane reach the window's foot under it.
    const int stageBottom = full ? L.bar.top : C.bottom;
    const int stageTop = C.top, stageH = stageBottom - C.top;
    int menuRight = 0;
    const bool idle = full && !m_chromeShown && !m_paneShown; // the chrome has left: regroup
    const bool lyricLine =
        m_lyrics && m_lyrics->synced && !m_lyrics->lines.empty() && !m_paneShown && settings::LyricLineShown();
    if (full) {
        // Media Center's menu at the left, centred on the cover's height. misc.mp4 (2.37 px per
        // unit of its 768-unit high screen): buttons the 251x51 bitmap, 55 apart, 41 in from
        // the left; its shade 410 x (menu + 130), from the screen's edge.
        const float f = std::clamp(H / m_scale / 768.0f, 1.0f, 1.6f);
        const int iw = PxI(251 * f), ih = PxI(51 * f), pitch = PxI(55 * f), x = PxI(41 * f);
        const int block = pitch * 3 + ih, top = (H - block) / 2 - static_cast<int>(H * 0.03f);
        for (int i = 0; i < 4; ++i) L.menu[i] = {x, top + pitch * i, x + iw, top + pitch * i + ih};
        menuRight = idle ? 0 : x + iw + PxI(20);
        // Taller than misc.mp4's 65 units beyond the menu: the shade's falloff needs the room.
        L.menuShade = {0, top - PxI(190 * f), PxI(470 * f), top + block + PxI(190 * f)};
        if (L.lyrics.right > L.lyrics.left) L.lyrics = L.list = {}; // the bar's own pair gives way
    }
    if (compact) {
        // Placed above; the title is over the cover (PaintCompactTitle).
    } else if (full) {
        if (m_paneShown) {
            const int colL = menuRight, colR = static_cast<int>(W * 0.50f), colW = colR - colL;
            const int side = static_cast<int>(std::min(H * 0.44f, colW * 0.74f));
            L.textScale = std::clamp((colR - colL - PxI(16)) / m_scale / 340.0f, 1.0f, 1.7f);
            const int textH = PxI(UnderStack(L.textScale, false).block);
            const int top = std::max(PxI(48), (H - side - textH) / 2 - PxI(24));
            L.cover = {colL + (colW - side) / 2, top, colL + (colW - side) / 2 + side, top + side};
            L.info = {colL + PxI(8), L.cover.bottom + PxI(22), colR - PxI(8), L.cover.bottom + PxI(22) + textH};
            L.infoStyle = 0;
            L.panel = {static_cast<int>(W * 0.52f), PxI(wmc ? kWmcTop + 4 : 64.0f), W - static_cast<int>(W * 0.04f),
                       L.bar.top - PxI(8)};
            L.pane = L.panel;
        } else {
            // WMC: the cover with the song's lines beside it, centred as a group.
            const int side = static_cast<int>(H * (idle ? 0.47f : 0.40f)); // larger once it stands alone
            const int gap = static_cast<int>(side * 0.085f);
            const int textW = static_cast<int>(std::min(W * 0.34f, side * 1.7f));
            // With the menu the cover stands 0.10 of the height right of its buttons (misc.mp4);
            // without it the group is centred.
            const int left = idle ? std::max(0, (W - side - gap - textW) / 2)
                                  : static_cast<int>(L.menu[0].right) + static_cast<int>(H * 0.10f);
            const int top = (H - side) / 2 - static_cast<int>(H * 0.03f);
            L.cover = {left, top, left + side, top + side};
            L.info = {L.cover.right + gap, top, L.cover.right + gap + textW, top + side};
            L.infoStyle = 1;
            if (lyricLine) {
                // The sung line is the last of the song's lines beside the cover (PaintInfo
                // centres them all as one block); it may run on as far as the blue behind
                // them reaches (PaintShade), no further onto the bright wall.
                const float pitch = side * 0.172f;
                const int lines = 1 + (m_artist.empty() ? 0 : 1) + (m_album.empty() ? 0 : 1) + (m_track.is_valid() ? 1 : 0);
                const float y = (side - pitch * (lines + 1)) / 2 + pitch * lines;
                L.lyric = {L.info.left, top + static_cast<int>(y),
                           std::min<LONG>(L.info.right + static_cast<LONG>(side * 0.3f), W - PxI(48)),
                           top + static_cast<int>(y + pitch)};
            }
        }
    } else if (m_paneShown) {
        L.panel = {C.left, C.top, C.right, stageBottom};
        L.pane = L.panel;
    } else if (stageH < PxI(kStripBelow) || CW >= stageH * kStripAspect) {
        // The strip: the cover as high as the stage allows (and at most 45 % of the width), the
        // lines beside it scaled with the cover (a title about a seventh of its side), within
        // the width they have, and the block never higher than the stage.
        // The margins and the gap between cover and lines grow with the cover (owner,
        // 2026-11-13: fixed, they looked cramped in a large window): about an eighth of its
        // side, as at the smallest strip, never less than there.
        const int side = std::max(PxI(40), std::min(stageH - PxI(24), static_cast<int>(CW * 0.45f)));
        const int top = stageTop + (stageH - side) / 2;
        const int margin = std::max(PxI(12), static_cast<int>(side * 0.07f));
        const int gap = std::max(PxI(14), static_cast<int>(side * 0.13f));
        L.cover = {C.left + margin, top, C.left + margin + side, top + side};
        L.info = {L.cover.right + gap, L.cover.top, C.right - std::max(PxI(16), margin), L.cover.bottom};
        L.infoStyle = 2;
        float t = std::clamp(std::min(side / m_scale / 115.0f, (L.info.right - L.info.left) / m_scale / 170.0f), 0.8f,
                             2.0f);
        const float room = stageH / m_scale - 12;
        for (int i = 0; i < 3; ++i) { // the gaps grow faster than t: settle in a few steps
            const float block = StripStack(t, lyricLine).block;
            if (block <= room) break;
            t *= room / block;
        }
        L.textScale = t;
        if (lyricLine) { // the sung line under the others, its rect 2 DIP round the type
            const LineStack s = StripStack(t, true);
            const int blockTop = (L.cover.top + L.cover.bottom) / 2 - PxI(s.block / 2);
            L.lyric = {L.info.left, blockTop + PxI(s.top[3] - 2 * t), L.info.right,
                       blockTop + PxI(s.top[3] + kStripLyric * t * kLineHeight + 2 * t)};
        }
    } else {
        // Three lines under the cover, and the sung line as a fourth in their style (no room is
        // kept for it without synced lyrics). They grow with a wider window and shrink with a
        // lower one, so that cover and lines keep about the proportion they have at the
        // window's first size (a stage 500 DIP high) instead of the lines squeezing the cover.
        const float tkWide = std::clamp((CW / m_scale - 28) / 340.0f, 1.0f, 1.7f);
        const float tk = std::clamp(std::min(tkWide, stageH / m_scale / 500.0f), 0.62f, 1.7f);
        L.textScale = tk;
        const LineStack s = UnderStack(tk, lyricLine);
        const int textH = PxI(s.block);
        // The cover leaves a good margin of wall on every side.
        const int side = std::max(PxI(60), std::min(static_cast<int>(CW * 0.64f), stageH - textH - PxI(52)));
        const int top = stageTop + std::max(PxI(12), (stageH - textH - side) / 2);
        L.cover = {C.left + (CW - side) / 2, top, C.left + (CW - side) / 2 + side, top + side};
        L.info = {C.left + PxI(14), L.cover.bottom + PxI(12), C.right - PxI(14), L.cover.bottom + PxI(12) + textH};
        L.infoStyle = 0;
        if (lyricLine)
            L.lyric = {L.info.left, L.info.top + PxI(s.top[4] - 2 * tk), L.info.right,
                       L.info.top + PxI(s.top[4] + kStackLine * tk * kLineHeight + 2 * tk)};
    }
    const bool infoChanged = !EqualRect(&L.info, &m_layout.info) || L.infoStyle != m_layout.infoStyle;
    const bool lyricMoved = Empty(m_layout.lyric) != Empty(L.lyric) || L.infoStyle != m_layout.infoStyle;
    // Full screen, the menu coming or going only resizes the cover and moves the group: the
    // song's lines and the sung line then grow or shrink with the cover on its curve instead
    // of fading out and in (owner, 2026-11-13).
    const RECT oldCover = m_layout.cover, oldInfo = m_layout.info, oldLyric = m_layout.lyric;
    const bool morph = animate && full && L.infoStyle == 1 && m_layout.infoStyle == 1 && !Empty(oldCover) &&
                       !Empty(L.cover) && !Empty(oldInfo);
    m_layout = L;

    // Apply.
    const float radius = full ? 0.0f : Px(m::kCornerRadius);
    m_clipGeometry.Size(float2{static_cast<float>(W), static_cast<float>(H)});
    m_clipGeometry.CornerRadius(float2{radius, radius});
    m_contentClip.LeftInset(static_cast<float>(C.left));
    m_contentClip.TopInset(static_cast<float>(C.top));
    m_contentClip.RightInset(static_cast<float>(W - C.right));
    m_contentClip.BottomInset(static_cast<float>(H - C.bottom));
    if (!full) {
        Place(m_clientEdge, L.clientEdge);
        if (m_framePainted != m_scale) PaintFrame();
    }
    // DWM's side highlights, as on the library window: down the left and right frame from the
    // top, ending sharply at H / 4 + 20 DIP.
    {
        const bool show = m_aeroArt && !full && L.clientEdge.left > 0;
        const SIZE size{L.clientEdge.left, H / 4 + PxI(20)};
        for (auto& v : m_sideHighlight) v.IsVisible(show);
        if (show) {
            Place(m_sideHighlight[0], {0, 0, size.cx, size.cy});
            Place(m_sideHighlight[1], {W - size.cx, 0, W, size.cy});
            if (size.cx != m_sideHighlightSize.cx || size.cy != m_sideHighlightSize.cy) {
                m_sideHighlightSize = size;
                PaintSideHighlights();
            }
        }
    }
    if (m_pane) m_pane->SetEmbedded(true, full); // full screen shows cover, title and modes itself
    PlaceWall();
    m_light->SetSize({W, H});
    m_veil->SetSize({W, H});
    if (m_pane && m_paneShown) m_pane->SetBounds(L.pane, m_scale);
    PaintPaneCard();

    for (auto& w : m_widgets) {
        RECT r{};
        switch (w->Id()) {
        case kStop: r = L.stop; break;
        case kPrev: r = L.prev; break;
        case kPlay: r = L.play; break;
        case kNext: r = L.next; break;
        case kMute: r = L.mute; break;
        case kVolArrow: r = L.volArrow; break;
        case kVolDown: r = L.volDown; break;
        case kVolUp: r = L.volUp; break;
        case kLyrics: r = L.lyrics; break;
        case kList: r = L.list; break;
        case kLibrary: r = L.library; break;
        case kMinimize: r = L.minimize; break;
        case kRestore: r = L.restore; break;
        case kExit: r = L.exit; break;
        case kMenuLyrics: r = L.menu[0]; break;
        case kMenuList: r = L.menu[1]; break;
        case kMenuShuffle: r = L.menu[2]; break;
        case kMenuRepeat: r = L.menu[3]; break;
        case kFavorite: r = L.favorite; break;
        case kShuffle: r = L.shuffle; break;
        case kRepeat: r = L.repeat; break;
        case kCapMin: r = L.capMin; break;
        case kCapMax: r = L.capMax; break;
        case kCapClose: r = L.capClose; break;
        }
        w->SetBounds(r, m_scale);
    }
    SlideBack(animate);
    if (m_splitVolume) {
        const RECT pr{L.mute.left, L.mute.top, L.volArrow.right, L.mute.bottom};
        Place(m_splitPlate, pr);
        gfx::CompositionHost::Get().Repaint(m_splitPlate, m_splitPlateBacking, pr.right - pr.left, pr.bottom - pr.top,
                                            m_scale, [&](ID2D1DeviceContext* dc) {
                                                const float k = (pr.bottom - pr.top) / m_scale / 25.0f;
                                                SplitPlate(dc, 4630, false, 0, k);
                                                SplitPlate(dc, 4630, true, 25 * k, k);
                                            });
    } else if (m_splitPlateShown) {
        m_splitPlateShown = false;
        m_splitPlate.StopAnimation(L"Opacity");
        m_splitPlate.Opacity(0.0f);
    }
    UpdateSplitPlate();
    m_cluster.IsVisible(m_wmp && !wmc);
    if (m_wmp && !wmc) {
        const RECT cl = ClusterRect(wt::kClusterRect);
        Place(m_cluster, cl);
        gfx::CompositionHost::Get().Repaint(m_cluster, cl.right - cl.left, cl.bottom - cl.top, m_scale,
                                            [](ID2D1DeviceContext* dc) { wt::ClusterBackground(dc); });
    }

    if (!Empty(L.cover)) {
        const SIZE want{L.cover.right - L.cover.left, L.cover.bottom - L.cover.top};
        // Paint for the larger of the two sizes before an animated move, so nothing is upscaled.
        if (want.cx > m_coverPainted.cx || !animate) {
            m_coverPainted = want;
            PaintCover();
            RequestArt();
        }
        Place(m_cover, L.cover, animate);
        Place(m_coverOld, L.cover, animate);
        for (const auto& v : {m_cover, m_coverOld}) {
            if (auto shadow = v.Shadow().try_as<wuc::DropShadow>()) {
                shadow.BlurRadius(want.cx * 0.07f + Px(8));
                shadow.Offset(float3{0, want.cx * 0.02f + Px(2), 0});
                shadow.Opacity(kCoverShadow * L.coverEdge);
            }
        }
        for (const auto& f : m_coverFrame) f.Opacity(L.coverEdge);
    }
    const float morphFrom =
        morph ? static_cast<float>(oldCover.right - oldCover.left) / std::max<LONG>(1, L.cover.right - L.cover.left) : 1.0f;
    m_info.IsVisible(!Empty(L.info)); // the compact window has none: its title is over the cover
    if (L.compact) PaintInfo();
    if (!Empty(L.info)) {
        Place(m_info, L.info);
        if (morph) {
            Morph(m_info, oldInfo, morphFrom);
        } else if (infoChanged && animate) {
            m_info.Opacity(0.0f);
            auto fade = m_info.Compositor().CreateScalarKeyFrameAnimation();
            fade.InsertKeyFrame(0.0f, 0.0f);
            fade.InsertKeyFrame(1.0f, 1.0f);
            fade.Duration(std::chrono::milliseconds(320));
            fade.DelayTime(std::chrono::milliseconds(180));
            fade.DelayBehavior(wuc::AnimationDelayBehavior::SetInitialValueBeforeDelay);
            m_info.StartAnimation(L"Opacity", fade);
        }
        PaintInfo();
    }
    for (size_t i = 0; i < 4; ++i) Place(m_menuGlow[i], L.menu[i]);
    Place(m_menuShade, L.menuShade);
    PaintMenuShade();
    // The sung line does not travel with the cover when the pane opens or closes: it goes at
    // once, and comes back in its new place fading in with the song's other lines.
    Place(m_lyric, L.lyric);
    m_lyric.IsVisible(!Empty(L.lyric));
    if (morph && !lyricMoved && !Empty(L.lyric) && !Empty(oldLyric)) {
        Morph(m_lyric, oldLyric, morphFrom);
    } else if (lyricMoved && animate && !Empty(L.lyric)) {
        auto fade = m_lyric.Compositor().CreateScalarKeyFrameAnimation();
        fade.InsertKeyFrame(0.0f, 0.0f);
        fade.InsertKeyFrame(1.0f, 1.0f);
        fade.Duration(std::chrono::milliseconds(320));
        fade.DelayTime(std::chrono::milliseconds(180));
        fade.DelayBehavior(wuc::AnimationDelayBehavior::SetInitialValueBeforeDelay);
        m_lyric.StartAnimation(L"Opacity", fade);
    }
    m_lyricLine = -2;
    PaintLyric();
    // The sung line's or the seek bar's room may only have come now.
    if ((LyricLineLive() && !Empty(L.lyric)) || SeekLive()) StartFrames();
    Place(m_barBack, L.bar);
    Place(m_topBack, L.top);
    Place(m_times, RECT{L.bar.left, L.timeLeft.top, L.bar.right, L.timeLeft.bottom});
    m_volumePanel->SetPosition({L.popup.left, L.popup.top}, m_scale);
    if (Empty(L.slider)) {
        ShowBubble(false);
    } else if (m_popupOpen && !full) {
        SetPopup(false); // room for the slider: the panel's arrow is gone
    }
    Place(m_seek, L.seek);
    Place(m_slider, L.slider);
    m_slider.IsVisible(!Empty(L.slider));
    PaintSlider();
    PaintBars();
    PaintSeek();
    PaintTimes();
    if (animate && m_shade.Brush()) {
        // The blue changes shape with the layout: the old one fades out as the new one comes.
        auto& host = gfx::CompositionHost::Get();
        m_shadeOld.Brush(m_shade.Brush());
        m_shadeOld.Opacity(1.0f);
        host.AnimateOpacity(m_shadeOld, 0.0f, 620);
        m_shade.Opacity(0.0f);
        host.AnimateOpacity(m_shade, 1.0f, 480);
    }
    PaintShade();
    if (m_popupOpen) PaintPopup();
}

// The mini window's Win7 frame lines; one device pixel at any DPI, as DWM draws them.
void PlayerWindow::PaintFrame() {
    m_framePainted = m_scale;
    const float s = m_scale;
    m_glass->SetScale(s);
    const int frameSrc = PxI(32);
    m_frameOutline.Brush(NineGrid(frameSrc, frameSrc, s, static_cast<float>(PxI(m::kCornerRadius + 2)),
                                  [&](ID2D1DeviceContext* dc) {
                                      paint::FrameOutline(dc, D2D1::SizeF(frameSrc / s, frameSrc / s),
                                                          m::kCornerRadius);
                                  }));
    const int edgeSrc = PxI(16);
    m_clientEdge.Brush(NineGrid(edgeSrc, edgeSrc, s, 3.0f, [&](ID2D1DeviceContext* dc) {
        paint::ClientEdge(dc, D2D1::SizeF(edgeSrc / s, edgeSrc / s));
    }));
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

void PlayerWindow::PaintSideHighlights() {
    const SIZE size = m_sideHighlightSize;
    if (!m_aeroArt || size.cx <= 0 || size.cy <= 0 || !m_sideHighlight[0]) return;
    auto& host = gfx::CompositionHost::Get();
    const D2D1_SIZE_F dip{size.cx / m_scale, size.cy / m_scale};
    for (auto& v : m_sideHighlight)
        host.Repaint(v, size.cx, size.cy, m_scale,
                     [&](ID2D1DeviceContext* dc) { aero::atlas::SideHighlight(dc, m_active, dip); });
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
            const float half = std::min((info.right - info.left) / 2 + 30, 290.0f * L.textScale);
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
        const float line = std::max(1.0f, std::floor(m_scale)) / m_scale;
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

// The seek bar is WMP 12's, drawn in vectors to its bitmaps' colours.
void PlayerWindow::PaintSeek() {
    const RECT& r = m_layout.seek;
    if (Empty(r)) return;
    const float progress =
        m_seeking ? m_seekPreview : (m_length > 0 ? static_cast<float>(std::clamp(m_position / m_length, 0.0, 1.0)) : 0);
    const int w = r.right - r.left, h = r.bottom - r.top;
    m_seekHead = std::lround(SeekHead(progress) * m_scale);
    gfx::CompositionHost::Get().Repaint(m_seek, m_seekBacking, w, h, m_scale, [&](ID2D1DeviceContext* dc) {
        const float W = w / m_scale, H = h / m_scale;
        // WMP 12's seek bar drawn afresh at any resolution, its colours taken pixel for pixel from
        // its bitmaps (wmploc; one bitmap pixel is one DIP, the bar 9 high):
        //   4159 the track: a translucent groove 6 high, rounded, its outer ring light (#DCD8E5
        //        at 29 % on top, #ECEAF0 at 32 % below), its inner ring dark (#373639 at 44 % on
        //        top, #3F3C41 at 25 % below), clear inside;
        //   3881 the played part, three rows #7FC9FA / #014CB6 / #619FF2 inside the groove;
        //   3882 its head, the last 16 columns, each row brightening unevenly to the front;
        //   4058 / 3884 / 3883 the thumb (on the bar / on the thumb / dragged), a 16 x 9 pill
        //        lit in bands from above, its ends shaded.
        const float x0 = kSeekInset, x1 = W - kSeekInset;
        const float top = std::round((H - 9) / 2 * m_scale) / m_scale;
        const auto bands = [&](float y0, float y1, std::initializer_list<D2D1_COLOR_F> rows, bool smooth) {
            return RowBands(dc, y0, y1, rows, smooth);
        };
        const auto rgba = [](uint32_t rgb, float a = 1.0f) { return D2D1::ColorF(rgb, a); };

        // The track's two rings.
        const D2D1_RECT_F groove{x0, top + 1, x1, top + 7};
        dc->DrawRoundedRectangle({{groove.left + 0.5f, groove.top + 0.5f, groove.right - 0.5f, groove.bottom - 0.5f}, 2.5f, 2.5f},
                                 bands(groove.top, groove.bottom,
                                       {rgba(0xDCD8E5, 0.29f), rgba(0xE4E0EC, 0.10f), rgba(0xD8D2DE, 0.17f),
                                        rgba(0xD8D2DE, 0.16f), rgba(0xE8E4EE, 0.10f), rgba(0xECEAF0, 0.32f)},
                                       true)
                                     .get(),
                                 1.0f);
        dc->DrawRoundedRectangle({{groove.left + 1.5f, groove.top + 1.5f, groove.right - 1.5f, groove.bottom - 1.5f}, 1.5f, 1.5f},
                                 bands(groove.top + 1, groove.bottom - 1,
                                       {rgba(0x373639, 0.44f), rgba(0x2D2C2E, 0.38f), rgba(0x3A3339, 0.22f),
                                        rgba(0x3F3C41, 0.25f)},
                                       true)
                                     .get(),
                                 1.0f);
        if (m_length <= 0) return;
        const float px = std::round(SeekHead(progress) * m_scale) / m_scale; // the head's front
        if (px > x0 + 2) {
            // The played part: the three rows (crisp, as in the bitmap), the first column a
            // little clear where it meets the groove's end.
            const float fy = top + 3;
            dc->FillRectangle({x0 + 2, fy, px, fy + 3},
                              bands(fy, fy + 3, {rgba(0x7FC9FA), rgba(0x014CB6), rgba(0x619FF2)}, false).get());
            // The head: per row a smooth gradient through 3882's sixteen columns.
            static constexpr uint32_t kHead[3][16] = {
                {0x82CFFB, 0x82CFFB, 0x82CFFB, 0x82CFFB, 0x82CFFB, 0x82CFFB, 0x82CFFB, 0x82CFFB, 0x82CFFB, 0x90DBFE,
                 0x9FE2FF, 0xAFEDFF, 0xBDF5FF, 0xC5FCFF, 0xC5F9FF, 0xB5EAFF},
                {0x0146A8, 0x0146A8, 0x0146A8, 0x0146A8, 0x0156C8, 0x0162DC, 0x066DE7, 0x1179EE, 0x1A86F1, 0x208FF2,
                 0x2A9CF5, 0x38A9F6, 0x47B3F9, 0x50B4F7, 0x5EBFFA, 0x59B7F4},
                {0x61A3F3, 0x61A3F3, 0x61A3F3, 0x70B0F5, 0x7BB8F6, 0x7DB9F6, 0x83BEF7, 0x87C2F8, 0x8DC7F9, 0x90C9F9,
                 0x94CBF9, 0x9BD2FA, 0xA0D5FB, 0xA1D6FB, 0xA2D7FC, 0x92C8F9}};
            static constexpr float kHeadEdge[3] = {0.69f, 0.98f, 0.69f}; // the last column's alpha
            const float h0 = std::max(x0 + 2, px - 16);
            dc->PushAxisAlignedClip({h0, fy, px, fy + 3}, D2D1_ANTIALIAS_MODE_ALIASED);
            for (int row = 0; row < 3; ++row) {
                D2D1_GRADIENT_STOP stops[16];
                for (int c = 0; c < 16; ++c)
                    stops[c] = {(c + 0.5f) / 16, rgba(kHead[row][c], c == 15 ? kHeadEdge[row] : 1.0f)};
                com_ptr<ID2D1GradientStopCollection> collection;
                dc->CreateGradientStopCollection(stops, 16, collection.put());
                com_ptr<ID2D1LinearGradientBrush> brush;
                dc->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({px - 16, 0}, {px, 0}), collection.get(),
                                              brush.put());
                dc->FillRectangle({px - 16, fy + row, px, fy + row + 1}, brush.get());
            }
            dc->PopAxisAlignedClip();
        }
    });
    UpdateSeekThumb();
}

// The front of the played part, in DIP from the seek bar's left (not yet on a device pixel).
float PlayerWindow::SeekHead(float progress) const {
    const float W = (m_layout.seek.right - m_layout.seek.left) / m_scale, x0 = kSeekInset, x1 = W - kSeekInset;
    return x0 + 2 + (x1 - x0 - 4) * progress;
}

// The seek thumb rides the head in its own visuals, so it can fade: in while the pointer is
// on the bar and out when it leaves, its lit state in and out under the pointer, the dragged
// state at once.
void PlayerWindow::UpdateSeekThumb() {
    const RECT& r = m_layout.seek;
    if (!m_seekThumb.root) return;
    if (Empty(r) || m_length <= 0) {
        m_seekThumb.Show(false, 0, 0);
        return;
    }
    const int w = PxI(kSeekThumbW), h = PxI(kSeekThumbH);
    m_seekThumb.Paint(w, h, m_scale, false, PaintSeekThumb);
    // Where PaintSeek puts the head (whole device pixels), the thumb centred on it.
    const float W = (r.right - r.left) / m_scale, x0 = kSeekInset, x1 = W - kSeekInset;
    const float progress =
        m_seeking ? m_seekPreview : static_cast<float>(std::clamp(m_position / m_length, 0.0, 1.0));
    const float px = std::round(SeekHead(progress) * m_scale) / m_scale;
    const float cx = std::clamp(px, x0 + kSeekThumbW / 2, x1 - kSeekThumbW / 2);
    const float top = std::round(((r.bottom - r.top) / m_scale - kSeekThumbH) / 2 * m_scale) / m_scale;
    m_seekThumb.Place(r.left + PxI(cx - kSeekThumbW / 2), r.top + PxI(top), w, h);
    const bool hot = !m_seeking && m_seekHover && std::abs(m_seekPointerX - (r.left + Px(cx))) <= Px(kSeekThumbW / 2);
    m_seekThumb.Show(m_seeking || m_seekHover, kSeekThumbInMs, kSeekThumbOutMs);
    m_seekThumb.SetState(hot, m_seeking, kSeekThumbInMs, kSeekThumbOutMs);
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

void PlayerWindow::PaintTimes() {
    const RECT r{m_layout.bar.left, m_layout.timeLeft.top, m_layout.bar.right, m_layout.timeLeft.bottom};
    if (Empty(r)) return;
    const int w = r.right - r.left, h = r.bottom - r.top;
    const double position = m_seeking ? m_seekPreview * m_length : m_position;
    const float s = m_scale;
    const float leftX = (m_layout.timeLeft.left - r.left) / s;
    const float leftW = (m_layout.timeLeft.right - m_layout.timeLeft.left) / s;
    const float rightX = (m_layout.timeRight.left - r.left) / s;
    gfx::CompositionHost::Get().Repaint(m_times, m_timesBacking, w, h, m_scale, [&](ID2D1DeviceContext* dc) {
        if (!m_track.is_valid()) return;
        const auto ink = pane::White(dc, 0.88f);
        pane::DrawTextLine(dc, FormatTime(position), 12, DWRITE_FONT_WEIGHT_NORMAL, {leftX, 0}, leftW, ink.get(),
                           DWRITE_TEXT_ALIGNMENT_CENTER);
        if (m_length > 0)
            pane::DrawTextLine(dc, L"-" + FormatTime(std::max(0.0, m_length - position)), 12, DWRITE_FONT_WEIGHT_NORMAL,
                               {rightX, 0}, leftW, ink.get(), DWRITE_TEXT_ALIGNMENT_CENTER);
    });
}

// Media Center's volume overlay: full screen it shows for a moment when the level changes; in
// a narrow mini window the speaker's arrow opens it and its bar can be dragged.
void PlayerWindow::PaintPopup() {
    if (m_volumePanel) m_volumePanel->SetLevel(std::clamp(m_volume, 0.0f, 1.0f), m_muted);
    if (m_popupOpen && m_mode == Mode::Mini) PaintFlyout();
    if (m_volumeBubble && m_volumeBubble->Shown()) ShowBubble(true); // follows the level
}

// ---- The sung line ---------------------------------------------------------------------------

// Index of the lyric line being sung (-1: none), and the position in milliseconds.
int PlayerWindow::CurrentLyricLine(int* ms) const {
    double now = m_position;
    try {
        if (auto pc = playback_control::get(); pc->is_playing()) now = pc->playback_get_position();
    } catch (...) {
    }
    *ms = static_cast<int>(now * 1000);
    int index = -1;
    if (m_lyrics && m_lyrics->synced && m_track.is_valid())
        for (size_t i = 0; i < m_lyrics->lines.size() && m_lyrics->lines[i].start <= *ms; ++i)
            index = static_cast<int>(i);
    return index;
}

// One line of lyrics where the lyrics pane is not showing, like the line in the library
// window's transport band: dim, the sung part bright, word by word where the lyrics have it.
void PlayerWindow::PaintLyric() {
    const RECT& r = m_layout.lyric;
    if (Empty(r)) return;
    int ms = 0;
    const int index = CurrentLyricLine(&ms);
    const int w = r.right - r.left, h = r.bottom - r.top;
    const float W = w / m_scale, H = h / m_scale;
    // The line is the last of the song's lines and looks like them: beside the cover (full
    // screen, the mini strip) flush left, under it (mini) centred at the album line's size.
    const bool beside = m_layout.infoStyle != 0;
    float size = m_layout.infoStyle == 2 ? kStripLyric * m_layout.textScale
                 : beside                ? std::clamp(H * 0.44f, 12.0f, 30.0f)
                                         : 14.0f * m_layout.textScale;
    float fill = -1;
    com_ptr<IDWriteTextLayout> layout;
    if (index >= 0) {
        const lyrics::Line& line = m_lyrics->lines[static_cast<size_t>(index)];
        const auto make = [&](float points) {
            layout = nullptr;
            auto format = pane::Format(points, DWRITE_FONT_WEIGHT_NORMAL);
            format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            format->SetTextAlignment(beside ? DWRITE_TEXT_ALIGNMENT_LEADING : DWRITE_TEXT_ALIGNMENT_CENTER);
            gfx::CompositionHost::Get().DWrite()->CreateTextLayout(
                line.text.c_str(), static_cast<UINT32>(line.text.size()), format.get(), W, H, layout.put());
        };
        make(size);
        // A long line gets smaller (to 70 %) rather than cut off at the edge.
        DWRITE_TEXT_METRICS metrics{};
        if (layout && SUCCEEDED(layout->GetMetrics(&metrics)) && metrics.widthIncludingTrailingWhitespace > W) {
            size = std::max(size * 0.7f, size * W / metrics.widthIncludingTrailingWhitespace);
            make(size);
            if (layout) {
                com_ptr<IDWriteInlineObject> ellipsis;
                gfx::CompositionHost::Get().DWrite()->CreateEllipsisTrimmingSign(layout.get(), ellipsis.put());
                const DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
                layout->SetTrimming(&trimming, ellipsis.get());
            }
        }
        if (layout && !line.words.empty()) {
            fill = 0;
            UINT32 pos = 0;
            for (const auto& word : line.words) {
                const UINT32 len = static_cast<UINT32>(word.text.size());
                if (word.start > ms || pos >= line.text.size()) break;
                DWRITE_HIT_TEST_METRICS hit{};
                UINT32 count = 0;
                layout->HitTestTextRange(pos, std::min<UINT32>(len, static_cast<UINT32>(line.text.size()) - pos), 0, 0,
                                         &hit, 1, &count);
                const float p = word.end > word.start
                                    ? std::clamp(static_cast<float>(ms - word.start) / (word.end - word.start), 0.0f, 1.0f)
                                    : 1.0f;
                if (count > 0) fill = hit.left + hit.width * p;
                pos += len;
            }
        }
    }
    const int fillKey = static_cast<int>(std::lround(fill * m_scale * 2));
    if (index == m_lyricLine && fillKey == m_lyricFill) return;
    const bool newLine = index != m_lyricLine;
    // A new line rolls in from below while the old one rolls up and away (not after a
    // relayout, where the line is only being drawn afresh).
    const bool roll = newLine && m_lyricLine != -2;
    m_lyricLine = index;
    m_lyricFill = fillKey;
    if (roll) m_lyricCurrent ^= 1;
    auto& line = m_lyricLines[static_cast<size_t>(m_lyricCurrent)];
    gfx::CompositionHost::Get().Repaint(line, m_lyricBackings[static_cast<size_t>(m_lyricCurrent)], w, h, m_scale,
                                        [&](ID2D1DeviceContext* dc) {
        if (!layout) return;
        // No shadow: the song's other lines have none.
        const D2D1_POINT_2F at{0, (H - size * 1.33f) / 2};
        if (fill < 0) { // timed per line: the whole line is the sung one
            dc->DrawTextLayout(at, layout.get(), pane::White(dc, 0.92f).get());
            return;
        }
        dc->DrawTextLayout(at, layout.get(), pane::White(dc, 0.45f).get());
        if (fill > 0) {
            // The sung part bright, its front softened over a few pixels.
            const float soft = 3.0f;
            const D2D1_GRADIENT_STOP stops[] = {{0, D2D1::ColorF(1, 1, 1, 0.97f)}, {1, D2D1::ColorF(1, 1, 1, 0.0f)}};
            com_ptr<ID2D1GradientStopCollection> collection;
            dc->CreateGradientStopCollection(stops, 2, collection.put());
            com_ptr<ID2D1LinearGradientBrush> sung;
            dc->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({at.x + fill - soft, 0}, {at.x + fill + soft, 0}),
                                          collection.get(), sung.put());
            dc->DrawTextLayout(at, layout.get(), sung.get());
        }
    });
    auto& old = m_lyricLines[static_cast<size_t>(m_lyricCurrent ^ 1)];
    if (!roll) {
        if (newLine) {
            for (auto* v : {&line, &old}) v->StopAnimation(L"Offset"), v->StopAnimation(L"Opacity");
            line.Offset(float3{0, 0, 0});
            line.Opacity(1.0f);
            old.Opacity(0.0f);
        }
        return;
    }
    // Both move by a line's height on one curve: off quickly, then settling softly; the old line
    // fades as it goes, the new one comes up clear.
    const auto& comp = gfx::CompositionHost::Get().Compositor();
    const auto curve = comp.CreateCubicBezierEasingFunction({0.3f, 0.0f}, {0.1f, 1.0f});
    const float rise = static_cast<float>(h);
    const auto slide = [&](const wuc::Visual& v, float fromY, float toY, float fromA, float toA) {
        auto move = comp.CreateVector3KeyFrameAnimation();
        move.InsertKeyFrame(0.0f, float3{0, fromY, 0});
        move.InsertKeyFrame(1.0f, float3{0, toY, 0}, curve);
        move.Duration(Ms(520));
        v.StartAnimation(L"Offset", move);
        auto fade = comp.CreateScalarKeyFrameAnimation();
        fade.InsertKeyFrame(0.0f, fromA);
        fade.InsertKeyFrame(1.0f, toA, curve);
        fade.Duration(Ms(fromA > toA ? 360 : 520));
        v.StartAnimation(L"Opacity", fade);
    };
    slide(line, rise, 0, 0, 1);
    slide(old, 0, -rise, 1, 0);
}

// ---- Playback ------------------------------------------------------------------------------

void PlayerWindow::ReadTrack() {
    auto pc = playback_control::get();
    m_playing = pc->is_playing();
    m_paused = pc->is_paused();
    // While playback switches songs the host keeps the previous song, or names the next one as
    // soon as it is being opened: there is no empty moment between two songs.
    const metadb_handle_ptr now = m_host.PlayerTrack();
    const bool has = now.is_valid();
    metadb_handle_ptr decoding;
    const bool loaded = pc->get_now_playing(decoding) && decoding == now;
    const bool changed = has ? (!m_track.is_valid() || now != m_track) : m_track.is_valid();
    std::wstring title, artist, album;
    if (has) {
        m_host.PlayerTrackText(now, title, artist, album);
        if (loaded) {
            m_position = pc->playback_get_position();
            m_length = pc->playback_get_length();
        } else if (changed) {
            m_position = 0;
            m_length = 0;
        }
        // A paused track that has not decoded yet reports no length; the library knows it.
        if (m_length <= 0) m_length = now->get_length();
    } else {
        m_position = m_length = 0;
    }
    bool shuffle = false;
    int repeat = 0;
    m_host.PlayerMode(shuffle, repeat);
    if (shuffle != m_shuffle || repeat != m_repeat) {
        m_shuffle = shuffle;
        m_repeat = repeat;
        for (int id : {kMenuShuffle, kMenuRepeat, kShuffle, kRepeat})
            if (auto* w = WidgetById(id)) w->Invalidate();
    }
    const bool favorite = has && (loaded ? m_host.PlayerFavorite() : media::IsFavorite(now));
    if (favorite != m_favorite) {
        m_favorite = favorite;
        if (auto* w = WidgetById(kFavorite)) w->Invalidate();
    }
    // The library's lyrics belong to the song that played; a song still opening has none yet.
    auto lyrics = !has ? nullptr : loaded ? m_host.PlayerLyrics() : changed ? nullptr : m_lyrics;
    const auto synced = [](const auto& l) { return l && l->synced && !l->lines.empty(); };
    const bool hadLine = synced(m_lyrics), hasLine = synced(lyrics);
    const bool lyricsChanged = lyrics != m_lyrics;
    if (lyricsChanged) {
        m_lyrics = std::move(lyrics);
        m_lyricLine = -2;
    }
    const bool textChanged = title != m_title || artist != m_artist || album != m_album;
    m_title = std::move(title);
    m_artist = std::move(artist);
    m_album = std::move(album);
    if (changed) {
        m_track = has ? now : metadb_handle_ptr();
        // The old cover fades out under the new one, which grows in a little (WMC's change).
        auto& host = gfx::CompositionHost::Get();
        m_coverOld.Brush(m_cover.Brush());
        m_coverOld.Opacity(1.0f);
        host.AnimateOpacity(m_coverOld, 0.0f, 380);
        m_art = nullptr;
        RequestArt();
        if (!m_art) PaintCover();
        m_cover.Opacity(0.0f);
        host.AnimateOpacity(m_cover, 1.0f, 380);
        const float side = static_cast<float>(m_layout.cover.right - m_layout.cover.left);
        m_cover.CenterPoint(float3{side / 2, side / 2, 0});
        m_cover.Scale(float3{0.955f, 0.955f, 1});
        host.AnimateScale(m_cover, 1.0f, 460);
    }
    // Laid out only now that the song's lines are the new song's: room for the sung line comes
    // and goes with synced lyrics, and beside the cover its place follows the number of lines.
    if ((hadLine != hasLine || (hasLine && (changed || textChanged))) && m_shown && m_client.cx > 0)
        UpdateLayout(false);
    else if (changed || textChanged)
        PaintInfo();
    if (lyricsChanged) PaintLyric();
    for (int id : {kPlay, kStop, kPrev, kNext})
        if (auto* w = WidgetById(id)) w->Invalidate();
    PaintSeek();
    PaintTimes();
    UpdateMotion();
}

void PlayerWindow::RequestArt() {
    m_artPending = false;
    if (!m_track.is_valid()) {
        if (m_art) {
            m_art = nullptr;
            PaintCover();
        }
        return;
    }
    const int side = std::max<int>(m_coverPainted.cx, PxI(160));
    const int sizePx = side <= 640 ? 640 : 1280;
    com_ptr<IWICBitmapSource> art;
    const auto state = ArtCache::Get().Find(m_track, sizePx, &art);
    m_artPending = state == ArtCache::State::Pending;
    if (art.get() != m_art.get() && (art || state != ArtCache::State::Pending)) {
        m_art = std::move(art);
        PaintCover();
    }
}

void PlayerWindow::OnPlayback() {
    if (!m_shown) return;
    ReadTrack();
}

void PlayerWindow::OnTime() {
    if (!m_shown) return;
    auto pc = playback_control::get();
    m_position = pc->playback_get_position();
    m_length = pc->playback_get_length();
    if (m_length <= 0 && m_track.is_valid()) m_length = m_track->get_length();
    if (m_seeking || IsIconic(m_hwnd)) return;
    PaintSeek();
    PaintTimes();
    PaintInfo(); // its time line
    PaintLyric();
    if (SeekLive()) StartFrames(); // after a seek or a song change
}

void PlayerWindow::OnVolume() {
    if (!m_shown) return;
    auto pc = playback_control::get();
    m_muted = pc->is_muted();
    if (!m_muted) m_volume = SliderFromDb(pc->get_volume());
    if (auto* w = WidgetById(kMute)) w->Invalidate();
    if (m_popupOpen) PaintPopup();
}

void PlayerWindow::OnLyricLineSetting() {
    if (!m_shown || m_client.cx <= 0) return;
    m_lyricLine = -2;
    UpdateLayout(false); // the line's room comes or goes with it
    UpdateMotion();
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

float PlayerWindow::SeekFractionAt(int x) const {
    const RECT& r = m_layout.seek;
    const float x0 = r.left + Px(kSeekInset + 2), x1 = r.right - Px(kSeekInset + 2); // where the head runs
    return std::clamp((x - x0) / std::max(1.0f, x1 - x0), 0.0f, 1.0f);
}

// ---- Messages ------------------------------------------------------------------------------

LRESULT PlayerWindow::WndProc(UINT msg, WPARAM wp, LPARAM lp) {
    const HWND hwnd = m_hwnd;
    if (m_host.PlayerWindowMessage(msg, wp, lp)) return 0; // taskbar thumbnail buttons
    switch (msg) {
    case WM_NCCALCSIZE:
        if (wp) return 0; // the whole window is ours; WS_THICKFRAME only gives sizing
        break;

    case WM_NCACTIVATE: {
        const bool active = wp != FALSE;
        if (active != m_active) {
            m_active = active;
            if (m_glass) m_glass->SetActive(active);
            for (auto& w : m_widgets)
                if (w->Id() >= kCapMin && w->Id() <= kCapClose) w->SetInactive(!active); // caption only
            m_shadow.SetActive(active);
            PaintSideHighlights();
        }
        return DefWindowProcW(hwnd, msg, wp, -1); // -1: do not repaint the (non-existent) frame
    }

    case WM_NCHITTEST: {
        if (m_mode == Mode::Full) return HTCLIENT;
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ScreenToClient(hwnd, &pt);
        const int edge = PxI(m::kResizeBorder);
        const bool left = pt.x < edge, right = pt.x >= m_client.cx - edge;
        const bool top = pt.y < edge / 2 + 1, bottom = pt.y >= m_client.cy - edge;
        if (top && left) return HTTOPLEFT;
        if (top && right) return HTTOPRIGHT;
        if (bottom && left) return HTBOTTOMLEFT;
        if (bottom && right) return HTBOTTOMRIGHT;
        if (left) return HTLEFT;
        if (right) return HTRIGHT;
        if (top) return HTTOP;
        if (bottom) return HTBOTTOM;
        return HTCLIENT;
    }

    case WM_SIZING: {
        // With the lyrics or the queue open the mini window keeps room for them (the lyrics:
        // their tallest line whole); dragged on past that, the pane closes and the window
        // follows the pointer again.
        if (m_mode != Mode::Mini) break;
        auto* r = reinterpret_cast<RECT*>(lp);
        const bool topEdge = wp == WMSZ_TOP || wp == WMSZ_TOPLEFT || wp == WMSZ_TOPRIGHT;
        if (m_paneShown && m_pane) {
            const int panelH = m_layout.panel.bottom - m_layout.panel.top;
            const int minH = (m_client.cy - panelH) + PxI(std::max(kPaneMinH, m_pane->MinHeight()));
            const int h = r->bottom - r->top;
            if (h >= minH) break;
            if (h < minH - PxI(kPaneCloseDrag)) {
                m_miniCoverHeight = 0; // stay where the pointer took it
                SetPaneShown(false, m_pane->CurrentMode());
                break;
            }
            if (topEdge)
                r->top = r->bottom - minH;
            else
                r->bottom = r->top + minH;
            return TRUE;
        }
        // Narrower than the full transport needs the window is compact and square, the cover
        // filling it: the edge being dragged sets the side, the opposite edges stay. Leaving
        // the compact size takes a wider window, so a narrow one never grows tall.
        const int border = PxI(m::kResizeBorder), capH = PxI(m::kCaptionHeight);
        const int w = r->right - r->left, h = r->bottom - r->top;
        if (w >= PxI(kMiniMinW)) break;
        const bool byHeight = wp == WMSZ_TOP || wp == WMSZ_BOTTOM;
        const bool corner = wp == WMSZ_TOPLEFT || wp == WMSZ_TOPRIGHT || wp == WMSZ_BOTTOMLEFT ||
                            wp == WMSZ_BOTTOMRIGHT;
        int side = byHeight ? h - capH - border : w - 2 * border;
        if (corner) side = std::max(side, h - capH - border);
        side = std::clamp(side, PxI(kCompactSide), PxI(kMiniMinW) - 2 * border - 1);
        const int nw = side + 2 * border, nh = side + capH + border;
        const bool leftEdge = wp == WMSZ_LEFT || wp == WMSZ_TOPLEFT || wp == WMSZ_BOTTOMLEFT;
        if (leftEdge)
            r->left = r->right - nw;
        else
            r->right = r->left + nw;
        if (topEdge)
            r->top = r->bottom - nh;
        else
            r->bottom = r->top + nh;
        return TRUE;
    }

    case WM_GETMINMAXINFO: {
        // The compact square at its smallest; a wider window may be as low as the strip allows.
        auto* info = reinterpret_cast<MINMAXINFO*>(lp);
        info->ptMinTrackSize = {PxI(kCompactSide + 2 * m::kResizeBorder),
                                PxI(kMiniMinClientH + m::kCaptionHeight + m::kResizeBorder)};
        return 0;
    }

    case WM_SIZE:
        if (wp != SIZE_MINIMIZED && m_shown) UpdateLayout(false);
        return 0;

    case WM_MOVE:
        if (m_glass && m_mode == Mode::Mini) m_glass->UpdateReflectionAnchor(hwnd);
        return 0;

    case WM_WINDOWPOSCHANGING: {
        const auto* pos = reinterpret_cast<const WINDOWPOS*>(lp);
        if (m_glass && m_mode == Mode::Mini && !(pos->flags & SWP_NOMOVE) && !IsIconic(hwnd)) {
            RECT wr{};
            GetWindowRect(hwnd, &wr);
            const LONG w = (pos->flags & SWP_NOSIZE) ? wr.right - wr.left : pos->cx;
            const LONG h = (pos->flags & SWP_NOSIZE) ? wr.bottom - wr.top : pos->cy;
            m_glass->UpdateReflectionAnchor(hwnd, RECT{pos->x, pos->y, pos->x + w, pos->y + h});
        }
        break;
    }

    case WM_WINDOWPOSCHANGED: {
        const LRESULT r = DefWindowProcW(hwnd, msg, wp, lp); // generates WM_SIZE / WM_MOVE
        m_shadow.Sync();
        UpdateMotion();
        return r;
    }

    case WM_DPICHANGED: {
        m_scale = HIWORD(wp) / 96.0f;
        const RECT* r = reinterpret_cast<const RECT*>(lp);
        SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        m_coverPainted = {};
        if (m_shown) UpdateLayout(false);
        return 0;
    }

    case WM_EXITSIZEMOVE:
        if (m_mode == Mode::Mini && !IsIconic(hwnd)) RememberMiniRect();
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT:
        ValidateRect(hwnd, nullptr);
        return 0;

    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT && m_cursorHidden && m_mode == Mode::Full) {
            SetCursor(nullptr);
            return TRUE;
        }
        break;

    case kFrameMessage:
        if (m_frameClock) {
            m_frameClock->Ack();
            bool more = m_shown && !IsIconic(hwnd) && m_pane && m_pane->Tick();
            more |= TickLyric();
            more |= TickSeek();
            if (!more) m_frameClock->Stop();
        }
        return 0;

    case WM_TIMER:
        if (wp == kHideTimer) {
            KillTimer(hwnd, kHideTimer);
            POINT pt{};
            GetCursorPos(&pt);
            ScreenToClient(hwnd, &pt);
            // The controls stay while the pointer is on them, something is held, or the volume
            // flyout is open; full screen also over the pane (it scrolls under the pointer).
            const bool onControls =
                PtInRect(&m_layout.bar, pt) || PtInRect(&m_layout.top, pt) || WidgetAt(pt) != nullptr;
            // Nor while a menu is open: hidden then, the pointer stayed hidden after the menu,
            // whatever the mouse did (owner, 2026-11-14).
            GUITHREADINFO gui{sizeof(gui)};
            const bool menu = GetGUIThreadInfo(GetCurrentThreadId(), &gui) &&
                              (gui.flags & (GUI_INMENUMODE | GUI_POPUPMENUMODE)) != 0;
            const bool busy = m_capture != Capture::None || m_popupOpen || onControls || menu ||
                              (m_mode == Mode::Full && m_pane && m_paneShown && m_pane->HitTest(pt));
            if (!busy)
                ShowChrome(false);
            else
                ArmHideTimer();
        } else if (wp == kOverlayTimer) {
            KillTimer(hwnd, kOverlayTimer);
            if (m_mode == Mode::Full)
                SetPopup(false);
            else if (!m_bubbleHover && m_capture != Capture::Volume)
                ShowBubble(false);
        } else if (wp == kBubbleTimer) {
            KillTimer(hwnd, kBubbleTimer); // the pointer has rested on a volume slider
            if (m_bubbleHover) ShowBubble(true);
        } else if (wp == kRepeatTimer) {
            // A volume key held down keeps stepping, faster after the first pause.
            const int id = m_capture == Capture::Widget && m_pressed ? m_pressed->Id() : 0;
            if (id == kVolDown || id == kVolUp) {
                StepVolume(id == kVolUp ? kVolumeStep : -kVolumeStep);
                SetTimer(hwnd, kRepeatTimer, 45, nullptr);
            } else {
                KillTimer(hwnd, kRepeatTimer);
            }
        } else if (wp == kSizeTimer) {
            const float t = std::clamp(static_cast<float>(GetTickCount64() - m_sizeStart) / kSizeMs, 0.0f, 1.0f);
            const float e = 1.0f - std::pow(1.0f - t, 4.0f);
            const auto mix = [e](LONG a, LONG b) { return static_cast<int>(std::lround(a + (b - a) * e)); };
            if (t >= 1.0f) KillTimer(hwnd, kSizeTimer);
            const int left = mix(m_sizeFrom.left, m_sizeTo.left), top = mix(m_sizeFrom.top, m_sizeTo.top);
            SetWindowPos(hwnd, nullptr, left, top, mix(m_sizeFrom.right, m_sizeTo.right) - left,
                         mix(m_sizeFrom.bottom, m_sizeTo.bottom) - top, SWP_NOZORDER | SWP_NOACTIVATE);
            if (t >= 1.0f && m_mode == Mode::Mini) RememberMiniRect();
        }
        return 0;

    case WM_MOUSEMOVE: {
        const POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        if (!m_tracking) {
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
            TrackMouseEvent(&tme);
            m_tracking = true;
        }
        if (pt.x != m_lastMouse.x || pt.y != m_lastMouse.y) {
            m_lastMouse = pt;
            ShowChrome(true);
            ArmHideTimer();
        }
        switch (m_capture) {
        case Capture::Seek:
            m_seekPreview = SeekFractionAt(pt.x);
            PaintSeek();
            PaintTimes();
            PaintInfo(); // its time line follows the drag
            return 0;
        case Capture::Volume:
            SetVolumeFromX(pt.x);
            return 0;
        case Capture::Pane:
            if (m_pane) m_pane->OnMouseMove(pt);
            return 0;
        case Capture::Widget:
            if (m_pressed) m_pressed->SetPressed(m_pressed->HitTest(pt));
            return 0;
        case Capture::None:
            break;
        }
        Widget* w = WidgetAt(pt);
        SetHover(w);
        const bool seekHover = !w && PtInRect(&m_layout.seek, pt) != FALSE;
        if (seekHover != m_seekHover || seekHover) {
            m_seekHover = seekHover;
            m_seekPointerX = pt.x; // WMP's thumb lights while the pointer is on it
            UpdateSeekThumb();
        }
        const bool flyoutHot = m_popupOpen && m_mode == Mode::Mini && PtInRect(&m_layout.popup, pt) != FALSE;
        if (flyoutHot != m_flyoutHot) {
            m_flyoutHot = flyoutHot; // WMP's thumb lights under the pointer
            UpdateFlyoutBall();
        }
        const bool sliderHover = !w && PtInRect(&m_layout.slider, pt) != FALSE;
        if (sliderHover != m_sliderHover) {
            m_sliderHover = sliderHover;
            UpdateSliderThumb();
        }
        // The level shows over a slider once the pointer has rested on it a moment (a
        // tooltip's pause), and goes when it leaves.
        const bool bubbleHover = m_sliderHover || m_flyoutHot;
        if (bubbleHover != m_bubbleHover) {
            m_bubbleHover = bubbleHover;
            KillTimer(hwnd, kOverlayTimer);
            KillTimer(hwnd, kBubbleTimer);
            if (bubbleHover)
                SetTimer(hwnd, kBubbleTimer, static_cast<UINT>(settings::VolumeTipDelayMs()), nullptr);
            else
                ShowBubble(false);
        }
        if (m_pane) {
            const bool over = !w && m_paneShown && m_pane->HitTest(pt) &&
                              !(m_popupOpen && PtInRect(&m_layout.popup, pt));
            if (over != m_paneHover) {
                m_paneHover = over;
                if (!over) m_pane->OnMouseLeave();
            }
            if (over) m_pane->OnMouseMove(pt);
        }
        return 0;
    }

    case WM_MOUSELEAVE:
        m_tracking = false;
        SetHover(nullptr);
        if (m_paneHover && m_pane) m_pane->OnMouseLeave();
        m_paneHover = false;
        if (m_seekHover) {
            m_seekHover = false;
            UpdateSeekThumb();
        }
        if (m_sliderHover) {
            m_sliderHover = false;
            UpdateSliderThumb();
        }
        if (m_flyoutHot) {
            m_flyoutHot = false;
            UpdateFlyoutBall();
        }
        if (m_bubbleHover) {
            m_bubbleHover = false;
            KillTimer(hwnd, kBubbleTimer);
            if (m_capture != Capture::Volume) ShowBubble(false);
        }
        m_lastMouse = {-1, -1};
        if (m_mode == Mode::Mini) ShowChrome(false);
        return 0;

    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK: {
        const POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        SetFocus(hwnd);
        if (m_popupOpen && m_mode == Mode::Mini) {
            if (PtInRect(&m_layout.popup, pt)) { // the whole flyout takes the press
                m_capture = Capture::Volume;
                SetCapture(hwnd);
                SetVolumeFromX(pt.x);
                return 0;
            }
            const Widget* under = WidgetAt(pt);
            if (!under || (under->Id() != kVolArrow && under->Id() != kMute)) SetPopup(false);
        }
        if (PtInRect(&m_layout.slider, pt) && !WidgetAt(pt)) {
            m_capture = Capture::Volume;
            SetCapture(hwnd);
            KillTimer(hwnd, kOverlayTimer);
            SetVolumeFromX(pt.x);
            return 0;
        }
        if (Widget* w = WidgetAt(pt)) {
            if (!w->IsEnabled()) return 0;
            m_pressed = w;
            w->SetPressed(true);
            m_capture = Capture::Widget;
            SetCapture(hwnd);
            if (w->Id() == kVolDown || w->Id() == kVolUp) {
                StepVolume(w->Id() == kVolUp ? kVolumeStep : -kVolumeStep);
                SetTimer(hwnd, kRepeatTimer, 380, nullptr);
            }
            return 0;
        }
        if (PtInRect(&m_layout.seek, pt) && m_length > 0) {
            m_capture = Capture::Seek;
            m_seeking = true;
            m_seekPreview = SeekFractionAt(pt.x);
            SetCapture(hwnd);
            PaintSeek();
            PaintTimes();
            return 0;
        }
        if (m_pane && m_paneShown && m_pane->HitTest(pt)) {
            m_capture = Capture::Pane;
            SetCapture(hwnd);
            m_pane->OnLButtonDown(pt);
            return 0;
        }
        if (PtInRect(&m_layout.bar, pt)) return 0;
        if (PtInRect(&m_timeHit, pt)) { // the time counts down instead, or up again
            settings::SetPlayerTimeRemaining(!settings::PlayerTimeRemaining());
            PaintInfo();
            return 0;
        }
        if (msg == WM_LBUTTONDBLCLK) {
            Show(m_mode == Mode::Full ? Mode::Mini : Mode::Full);
            return 0;
        }
        if (m_mode == Mode::Mini) {
            // Anywhere else moves the window, like a caption.
            ReleaseCapture();
            SendMessageW(hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
        }
        return 0;
    }

    case WM_LBUTTONUP: {
        const POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        const Capture capture = std::exchange(m_capture, Capture::None);
        // Taken before the capture goes: ReleaseCapture sends WM_CAPTURECHANGED at once, and
        // that handler forgets the pressed widget.
        Widget* const pressed = std::exchange(m_pressed, nullptr);
        KillTimer(hwnd, kRepeatTimer);
        if (capture != Capture::None) ReleaseCapture();
        switch (capture) {
        case Capture::Widget: {
            Widget* w = pressed;
            if (w) {
                w->SetPressed(false);
                m_clickPoint = pt;
                if (w->HitTest(pt) && w->IsEnabled() && w->onClick) w->onClick();
            }
            break;
        }
        case Capture::Seek:
            m_seeking = false;
            if (m_length > 0) {
                m_position = SeekFractionAt(pt.x) * m_length;
                playback_control::get()->playback_seek(m_position);
            }
            PaintSeek();
            PaintTimes();
            break;
        case Capture::Pane:
            if (m_pane) {
                m_pane->OnLButtonUp(pt);
                for (int id : {kLyrics, kList, kMenuLyrics, kMenuList}) // the pane may have changed sides
                    if (auto* w = WidgetById(id)) w->Invalidate();
            }
            break;
        case Capture::Volume:
            // Let go off the slider, the bubble goes; on it, it stays while the pointer does.
            m_sliderHover = PtInRect(&m_layout.slider, pt) != FALSE;
            m_flyoutHot = m_popupOpen && m_mode == Mode::Mini && PtInRect(&m_layout.popup, pt) != FALSE;
            m_bubbleHover = m_sliderHover || m_flyoutHot;
            if (!m_bubbleHover) ShowBubble(false);
            PaintSlider();
            PaintPopup(); // the thumb lets go
            break;
        case Capture::None:
            break;
        }
        UpdateSplitPlate();
        return 0;
    }

    case WM_CAPTURECHANGED:
        if (m_capture == Capture::Pane && m_pane) m_pane->OnCaptureLost();
        if (m_capture == Capture::Widget && m_pressed) m_pressed->SetPressed(false);
        if (m_capture == Capture::Seek) {
            m_seeking = false;
            PaintSeek();
            PaintTimes();
        }
        m_pressed = nullptr;
        if (std::exchange(m_capture, Capture::None) == Capture::Volume) {
            UpdateSliderThumb(); // the thumb lets go
            UpdateFlyoutBall();
        }
        UpdateSplitPlate();
        return 0;

    case WM_MOUSEWHEEL: {
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ScreenToClient(hwnd, &pt);
        const int delta = GET_WHEEL_DELTA_WPARAM(wp);
        if (m_pane && m_paneShown && m_pane->HitTest(pt) && !PtInRect(&m_layout.bar, pt)) {
            m_pane->OnWheel(delta);
            return 0;
        }
        StepVolume(delta > 0 ? 0.02f : -0.02f); // elsewhere the wheel is the volume
        return 0;
    }

    case WM_RBUTTONUP: {
        const POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        POINT screen = pt;
        ClientToScreen(hwnd, &screen);
        if (m_pane && m_paneShown && m_pane->HitTest(pt)) {
            m_pane->OnRButtonUp(pt, screen);
        } else if (!PtInRect(&m_timeHit, pt) && (PtInRect(&m_layout.cover, pt) || PtInRect(&m_songHit, pt))) {
            m_host.ShowPlayerSongMenu(screen); // cover, title, artist, album
        } else if (PtInRect(&m_layout.lyric, pt) || PtInRect(&m_layout.info, pt) || PtInRect(&m_timeHit, pt)) {
            // The library window's lyrics menu, on the sung line, the time and the rest of the
            // song's lines (so a hidden line can be shown again).
            m_host.ShowPlayerLyricsMenu(screen);
        }
        return 0;
    }

    case WM_KEYDOWN:
        switch (wp) {
        case VK_ESCAPE:
            if (m_popupOpen && m_mode == Mode::Mini)
                SetPopup(false);
            else if (m_mode == Mode::Full)
                Show(Mode::Mini);
            else
                Close();
            return 0;
        case VK_F11:
            Show(m_mode == Mode::Full ? Mode::Mini : Mode::Full);
            return 0;
        case VK_SPACE:
            if (!media::PendingPlay()) playback_control::get()->play_or_pause();
            return 0;
        case VK_LEFT:
        case VK_RIGHT:
            if (m_length > 0)
                playback_control::get()->playback_seek(
                    std::clamp(m_position + (wp == VK_RIGHT ? 5.0 : -5.0), 0.0, m_length));
            return 0;
        case VK_UP:
        case VK_DOWN:
            StepVolume(wp == VK_UP ? 0.02f : -0.02f);
            return 0;
        case 'L':
            TogglePane(NowPlayingPane::Mode::Lyrics);
            return 0;
        case 'Q':
            TogglePane(NowPlayingPane::Mode::Queue);
            return 0;
        }
        return 0;

    case WM_CLOSE:
        Close();
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace glasswave::ui
