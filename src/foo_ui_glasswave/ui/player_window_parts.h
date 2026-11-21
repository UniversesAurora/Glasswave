#pragma once

// What PlayerWindow's source files share (ui/player_window*.cpp only): the includes, the
// namespace aliases, ids, layout constants, the glass painters and helpers.

#include "ui/player_window.h"
#include "gfx/motion.h"
#include "media/radio.h"
#include "media/special_playlists.h"
#include "media/transport.h"
#include "aero/aero_atlas.h"
#include "aero/aero_metrics.h"
#include "aero/aero_painter.h"
#include "app/settings.h"
#include "gfx/composition_host.h"
#include "gfx/d2d_util.h"
#include "gfx/effects.h"
#include "media/favorites.h"
#include "ui/art_cache.h"
#include "ui/caption_buttons.h"
#include "ui/i18n.h"
#include "ui/pane_common.h"
#include "ui/text.h"
#include "win7/win7_resources.h"
#include "wmp/wmp_transport.h"
#include "ui/win32.h"
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

namespace playerwin {

constexpr UINT_PTR kHideTimer = 1, kOverlayTimer = 2, kRepeatTimer = 3, kSizeTimer = 5,
                   kBubbleTimer = 7;

// Narrower than that the mini window is compact (owner, 2026-11-14, after WMP 12's own small
// Now Playing window): a square client filled by the cover, the title over its top and only the
// transport and the volume over its foot, both coming and going with the pointer. Growing from
// the smallest square the wall's margin round the cover opens first, the cover's glass edge and
// shadow fading in with it, then the cover grows.
constexpr float kCompactSide = 250, kCompactMargin = 16, kCompactTitle = 14;

// The strip's lines, in DIP at text scale 1: title 17, artist / album 13, the time 13 (blue),
// the sung line 14; the block is centred on the cover.
constexpr float kStripTitle = 17, kStripSecond = 13, kStripLyric = 14;

// The mini window's lines under the cover at text scale 1: title 19, the others 14.
constexpr float kStackTitle = 19, kStackLine = 14;

constexpr float kLineHeight = 1.33f; // a line's height for its type size

constexpr float kFlyoutW = 92, kFlyoutH = 32; // DIP: WMP's volume flyout (its slider art fills the inside)

// The mini window's inline slider, WMP 12's (wmploc 5678, 80 x 23): the thumb's centre runs
// from 11 to 66 DIP in it.
constexpr float kSliderW = 80, kSliderH = 23, kSliderThumbMin = 11, kSliderThumbMax = 66;

constexpr int kMoveMs = 480, kFadeMs = 260;

constexpr auto kEhres = win7::Module::Ehres;

constexpr auto kWmploc = win7::Module::Wmploc;

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

inline winrt::Windows::UI::Color Rgba(uint32_t rgb, float alpha) {
    return winrt::Windows::UI::ColorHelper::FromHex(static_cast<uint8_t>(std::lround(alpha * 255)),
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

inline LineStack StackLines(std::initializer_list<float> sizes, float t) {
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
inline LineStack StripStack(float t, bool lyric) {
    return lyric ? StackLines({kStripTitle * t, kStripSecond * t, kStripSecond * t, kStripLyric * t}, t)
                 : StackLines({kStripTitle * t, kStripSecond * t, kStripSecond * t}, t);
}

// Under the cover: title, artist, album, time, the sung line.
inline LineStack UnderStack(float t, bool lyric) {
    const float a = kStackTitle * t, b = kStackLine * t;
    return lyric ? StackLines({a, b, b, b, b}, t) : StackLines({a, b, b, b}, t);
}

inline bool Empty(const RECT& r) {
    return r.right <= r.left || r.bottom <= r.top;
}

using gfx::FadeTo;

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

using gfx::Hex;

using gfx::Stops;

// Fills `area` with a gradient that is radial about the segment (x0..x1, cy): radial brushes
// on its ends, a vertical gradient (the same stops mirrored) between them.
inline void FillAboutSegment(ID2D1DeviceContext* dc, D2D1_RECT_F area, float x0, float x1, float cy, float rx, float ry,
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

inline void PaintGlass(ID2D1DeviceContext* dc, const GlassShape& s, const GlassLook& look) {
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
                                      Stops(dc, {{0, Hex(g.base[0], 1)}, {0.5f, Hex(g.base[1], 1)},
                                                 {1, Hex(g.base[2], 1)}})
                                          .get(),
                                      base.put());
        dc->FillRectangle(area, base.get());
        FillAboutSegment(dc, area, s.x0, s.x1, s.cy + g.ay, s.r * g.ax, s.r * g.ayr,
                         {{0, Hex(g.ca, 0)}, {g.sA, Hex(g.ca, 0)}, {1, Hex(g.ca, g.aA)}});
        FillAboutSegment(dc, area, s.x0, s.x1, s.cy + g.by, s.r * g.bx, s.r * g.byr,
                         {{0, Hex(g.cb, g.aB)}, {0.5f, Hex(g.cb, g.aB * g.mB)}, {1, Hex(g.cb, 0)}});
        dc->PopAxisAlignedClip();
    }
    // The rim: one unit inside the outline, in the bitmap's outermost colour row by row.
    std::vector<D2D1_GRADIENT_STOP> rim;
    for (int i = 0; i < look.rims; ++i) {
        const float y = (look.rim[i].y - top) / (foot - top);
        rim.push_back({std::clamp(y, 0.0f, 1.0f), Hex(look.rim[i].rgb, 0.85f)});
    }
    com_ptr<ID2D1LinearGradientBrush> rimBrush;
    dc->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({0, top}, {0, foot}), Stops(dc, rim).get(),
                                  rimBrush.put());
    dc->DrawGeometry(outline(0.5f).get(), rimBrush.get(), 1.0f);
    dc->PopLayer();
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

inline std::chrono::milliseconds Ms(int ms) {
    return std::chrono::milliseconds(ms);
}

inline void Place(const wuc::Visual& v, const RECT& r, bool animate = false) {
    const float3 offset{static_cast<float>(r.left), static_cast<float>(r.top), 0};
    const float2 size{static_cast<float>(std::max<LONG>(0, r.right - r.left)),
                      static_cast<float>(std::max<LONG>(0, r.bottom - r.top))};
    if (animate) {
        const auto comp = v.Compositor();
        const auto ease = gfx::Easing(gfx::curve::kPlace);
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

} // namespace playerwin

} // namespace glasswave::ui
