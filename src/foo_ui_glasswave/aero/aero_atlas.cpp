#include "pch.h"
#include "aero/aero_atlas.h"
#include "win7/win7_resources.h"

namespace glasswave::aero::atlas {

namespace {

using win7::Module;
constexpr int kAtlas = 971;

struct Part {
    D2D1_RECT_F rect;     // whole part in the atlas (all states stacked vertically)
    int states;           // images stacked in `rect`
    D2D1_RECT_F sizing;   // nine-grid borders: left, top, right, bottom
    D2D1_RECT_F content;  // content margins: left, top, right, bottom
};

// DWMWINDOW parts of the Windows 7 SP1 aero.msstyles (TMT_ATLASRECT, TMT_IMAGECOUNT,
// TMT_SIZINGMARGINS, TMT_CONTENTMARGINS). States: normal, hot, pressed, disabled.
constexpr Part kMinActive{{83, 88, 114, 176}, 4, {12, 6, 12, 8}, {3, 2, 1, 4}};      // part 5
constexpr Part kMinInactive{{83, 264, 114, 352}, 4, {12, 6, 12, 8}, {3, 2, 1, 4}};   // part 6
constexpr Part kMaxActive{{83, 352, 114, 440}, 4, {12, 6, 12, 8}, {2, 2, 3, 4}};     // part 3
constexpr Part kMaxInactive{{83, 176, 114, 264}, 4, {12, 6, 12, 8}, {2, 2, 3, 4}};   // part 4
constexpr Part kCloseActive{{83, 568, 134, 656}, 4, {20, 6, 20, 8}, {1, 2, 2, 4}};   // part 7
constexpr Part kCloseInactive{{134, 0, 185, 88}, 4, {20, 6, 20, 8}, {1, 2, 2, 4}};   // part 8
constexpr Part kGlow{{83, 440, 132, 480}, 1, {9, 8, 9, 7}, {}};                      // part 16
constexpr Part kCloseGlow{{261, 0, 327, 40}, 1, {9, 8, 8, 7}, {}};                   // part 11
constexpr D2D1_RECT_F kGlare{327, 0, 555, 350};                                     // part 34, 2 states
constexpr D2D1_RECT_F kStreaks{555, 0, 1357, 604};                                  // part 40
constexpr Part kSide{{0, 0, 7, 236}, 2, {0, 0, 0, 2}, {}};  // part 43, opacity 100 / 50

// Glyph sets for 96, 120, 144 and 192 DPI, four states each.
constexpr D2D1_RECT_F kCloseGlyph[4] = {{0, 484, 13, 540}, {15, 378, 32, 446}, {36, 270, 57, 350}, {58, 423, 83, 511}};
constexpr D2D1_RECT_F kMaxGlyph[4] = {{15, 60, 31, 116}, {15, 514, 32, 582}, {36, 350, 57, 430}, {58, 687, 83, 775}};
constexpr D2D1_RECT_F kMinGlyph[4] = {{0, 596, 14, 652}, {15, 310, 32, 378}, {36, 430, 57, 510}, {58, 599, 83, 687}};
constexpr D2D1_RECT_F kRestoreGlyph[4] = {{0, 540, 14, 596}, {15, 446, 32, 514}, {36, 510, 57, 590}, {58, 511, 83, 599}};
constexpr float kGlyphScale[4] = {1.0f, 1.25f, 1.5f, 2.0f};

// The atlas packer surrounds every image with a one-pixel gutter (its edge pixels
// repeated) against filtering bleed; the image proper is one pixel inside.
D2D1_RECT_F Inner(const D2D1_RECT_F& r) { return {r.left + 1, r.top + 1, r.right - 1, r.bottom - 1}; }

D2D1_RECT_F State(const D2D1_RECT_F& r, int states, int state) {
    const float h = (r.bottom - r.top) / states;
    return Inner({r.left, r.top + h * state, r.right, r.top + h * (state + 1)});
}

float Scale(ID2D1DeviceContext* dc) {
    float x = 96, y = 96;
    dc->GetDpi(&x, &y);
    return x / 96.0f;
}

float Snap(float v, float s) { return std::round(v * s) / s; }

}  // namespace

bool Available() {
    const D2D1_SIZE_U s = win7::Resources::Get().Size(Module::AeroAtlas, kAtlas);
    return s.width == 1357 && s.height == 780;  // the Windows 7 atlas these rectangles describe
}

D2D1_RECT_F GlowOutset(bool close) { return close ? kCloseGlow.sizing : kGlow.sizing; }

void CaptionButton(ID2D1DeviceContext* dc, D2D1_SIZE_F size, Button button, paint::CaptionGlyph glyph,
                   paint::Layer layer) {
    const bool inactive = layer == paint::Layer::Inactive;
    const Part& part = button == Button::Close  ? (inactive ? kCloseInactive : kCloseActive)
                       : button == Button::Maximize ? (inactive ? kMaxInactive : kMaxActive)
                                                    : (inactive ? kMinInactive : kMinActive);
    const int state = layer == paint::Layer::Hover ? 1 : layer == paint::Layer::Pressed ? 2 : 0;
    win7::DrawNineGrid(dc, Module::AeroAtlas, kAtlas, State(part.rect, part.states, state), part.sizing,
                       D2D1::RectF(0, 0, size.width, size.height));

    // Glyph. Up to 150 % the set made for the DPI is drawn 1:1. Win7's 192-DPI set is
    // drawn relatively smaller than the others, so above 150 % it is scaled to the size the
    // 96 / 120 / 144 sets have (the proportions of the owner's Win7 at 150 %).
    const float s = Scale(dc);
    const int set = s > 1.5f ? 3 : s >= 1.5f ? 2 : s >= 1.25f ? 1 : 0;
    const D2D1_RECT_F* glyphs = glyph == paint::CaptionGlyph::Close      ? kCloseGlyph
                                : glyph == paint::CaptionGlyph::Maximize ? kMaxGlyph
                                : glyph == paint::CaptionGlyph::Restore  ? kRestoreGlyph
                                                                         : kMinGlyph;
    // Inactive windows show the dimmed glyph, as Win7 does.
    const int glyphState = inactive ? 3 : state;
    const D2D1_RECT_F src = State(glyphs[set], 4, glyphState);
    float k = 1.0f / kGlyphScale[set];  // DIP per glyph pixel
    if (set == 3) {
        const D2D1_RECT_F ref = State(glyphs[2], 4, glyphState);
        k = (ref.bottom - ref.top) / 1.5f / (src.bottom - src.top);
    }
    const float w = (src.right - src.left) * k, h = (src.bottom - src.top) * k;
    // Content margins are bitmap pixels, like the borders: one device pixel each.
    const D2D1_RECT_F& cm = part.content;
    const float px = 1.0f / s;
    const float cx = (cm.left * px + size.width - cm.right * px) / 2, cy = (cm.top * px + size.height - cm.bottom * px) / 2;
    const float x = Snap(cx - w / 2, s), y = Snap(cy - h / 2, s);
    win7::DrawNineGrid(dc, Module::AeroAtlas, kAtlas, src, {}, D2D1::RectF(x, y, x + w, y + h));
}

void CaptionGlow(ID2D1DeviceContext* dc, D2D1_SIZE_F size, bool close) {
    const Part& part = close ? kCloseGlow : kGlow;
    win7::DrawNineGrid(dc, Module::AeroAtlas, kAtlas, Inner(part.rect), part.sizing,
                       D2D1::RectF(0, 0, size.width, size.height));
}

void Glare(ID2D1DeviceContext* dc, bool active, D2D1_SIZE_F size) {
    win7::DrawNineGrid(dc, Module::AeroAtlas, kAtlas, State(kGlare, 2, active ? 0 : 1), {},
                       D2D1::RectF(0, 0, size.width, size.height));
}

void SideHighlight(ID2D1DeviceContext* dc, bool active, D2D1_SIZE_F size) {
    win7::DrawNineGrid(dc, Module::AeroAtlas, kAtlas, State(kSide.rect, kSide.states, active ? 0 : 1), kSide.sizing,
                       D2D1::RectF(0, 0, size.width, size.height), active ? 1.0f : 0.5f);
}

void Streaks(ID2D1DeviceContext* dc, D2D1_SIZE_F size) {
    win7::DrawNineGrid(dc, Module::AeroAtlas, kAtlas, Inner(kStreaks), {}, D2D1::RectF(0, 0, size.width, size.height));
}

}  // namespace glasswave::aero::atlas
