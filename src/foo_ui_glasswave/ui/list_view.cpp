#include "pch.h"
#include "ui/list_view.h"
#include "gfx/d2d_util.h"
#include "gfx/motion.h"
#include "core/text.h"
#include "gfx/composition_host.h"
#include "ui/art_cache.h"
#include "ui/card_layer.h"
#include "ui/text.h"
#include "win7/win7_resources.h"
#include "ui/win7_menu.h"
#include "app/settings.h"
#include "ui/i18n.h"
#include "ui/source_badge.h"
#include "wmp/wmp_painter.h"
#include "wmp/wmp_transport.h"

namespace glasswave::ui {

using core::Utf8;
using core::Wide;

namespace wuc = winrt::Windows::UI::Composition;
namespace wp = wmp::paint;
using winrt::Windows::Foundation::Numerics::float2;
using winrt::Windows::Foundation::Numerics::float3;

namespace {
constexpr float kRowHeight = 22.0f;
constexpr float kArtRowHeight = 40.0f;  // rows with the cover column: room for a real thumbnail
constexpr float kHeaderHeight = 23.0f;  // wmploc LISTVIEWHEADER cells are 23 px
constexpr float kScrollWidth = 17.0f;
constexpr float kScrollArrow = 17.0f;
constexpr float kGutter = 18.0f;  // playing marker column
constexpr float kCellPad = 6.0f;
constexpr float kMinColumn = 24.0f;
constexpr float kFlexWidth = 260.0f;  // default width of a source's "flexible" (title) column
constexpr float kEdgeGrip = 4.0f;     // header edge hit width on each side (DIP)
constexpr float kMoveThreshold = 6.0f;
constexpr size_t kGroupMinLines = 4;  // the album column needs four lines (cover + 4 text lines)
constexpr float kTilePad = 8.0f;
constexpr float kScrollStep = 48.0f; // DIP: a scroll bar arrow's step
constexpr float kTileGroupHeader = 30.0f; // DIP: a group's header line among tiles
constexpr size_t kMoreLink = 100;          // Hit::line of a group header's `more` link
constexpr D2D1_SIZE_F kAlbumTile{132, 160};
constexpr float kAlbumTileArt = 100;
constexpr D2D1_SIZE_F kArtistTile{240, 108};
constexpr float kArtistTileArt = 88;
constexpr float kSourceBadge = 18;  // DIP: a mixed result's source mark on its cover
// Song cards (TileStyle::Song, owner 2026-11-15 after Apple Music's song shelves): new UI values.
constexpr D2D1_SIZE_F kSongTile{280, 60};
constexpr float kSongTileArt = 44;
constexpr float kShowAll = 19;         // DIP: a shelf's "show all" (aero's expando button, 19 px at 96 DPI)
constexpr D2D1_SIZE_F kShelfArrow{26, 56}; // DIP: a shelf's paging arrow, tall as Apple Music's
constexpr float kTilePlay = 30;        // DIP: the play orb on a cover
constexpr float kSongPlay = 22;        // DIP: the play triangle on a song card's cover
// A featured card's tile: the card and room around it for its lifted shadow (ui/card_layer.h).
constexpr D2D1_POINT_2F kCardInset{10, 12}; // 20 DIP between cards, as Apple Music's
constexpr D2D1_SIZE_F kCardTile{CardLayer::kCard.width + 2 * kCardInset.x, CardLayer::kCard.height + 30};
// A shelf's page slides on the compositor with Windows 8.1's fast-out curve.
constexpr int kShelfSlideMs = 500;
constexpr int kShelfWheelMs = 220; // a horizontal wheel's step
constexpr gfx::Curve kShelfCurve = gfx::curve::kFastOut;
float ShelfEase(float t) { return gfx::Ease(kShelfCurve, t); }

constexpr uint32_t kInk = 0x1E1E1E, kSecondaryInk = 0x4A4A4A, kGreyInk = 0x7A7A7A, kPlayingInk = 0x1E5BA8;
constexpr uint32_t kGroupInk = 0x2E64B4, kGroupRule = 0xC9D7E8, kLinkInk = 0x0066CC;
constexpr uint32_t kNoteInk = 0x8C8C8C; // a title's note: lighter than the secondary columns
constexpr uint32_t kBadgeInk = 0xE5484D; // a title's tag: NetEase's 原唱 red (owner capture, 2026-11-21)

// {5E8B2C47-9A1D-4F63-B0E2-6C4A7D9F1B38}: column order and widths per column set
cfg_var_modern::cfg_string cfg_columns({0x5e8b2c47, 0x9a1d, 0x4f63, {0xb0, 0xe2, 0x6c, 0x4a, 0x7d, 0x9f, 0x1b, 0x38}},
                                       "");
// {5E8B2C48-9A1D-4F63-B0E2-6C4A7D9F1B38}: split a sorted Details list into sections
cfg_var_modern::cfg_int cfg_showGroups({0x5e8b2c48, 0x9a1d, 0x4f63, {0xb0, 0xe2, 0x6c, 0x4a, 0x7d, 0x9f, 0x1b, 0x38}},
                                       1);

// Columns drawn as controls instead of text.
enum class CellKind { Text, Rating, Favorite, Art, Popularity, Progress, Download };
CellKind KindOf(const media::Column& c) {
    if (!c.format) return CellKind::Text;
    if (std::strcmp(c.format, media::kRatingFormat) == 0) return CellKind::Rating;
    if (std::strcmp(c.format, media::kFavoriteFormat) == 0) return CellKind::Favorite;
    if (std::strcmp(c.format, media::kArtFormat) == 0) return CellKind::Art;
    if (std::strcmp(c.format, media::kPopularityFormat) == 0) return CellKind::Popularity;
    if (std::strcmp(c.format, media::kProgressFormat) == 0) return CellKind::Progress;
    if (std::strcmp(c.format, media::kDownloadFormat) == 0) return CellKind::Download;
    return CellKind::Text;
}
constexpr float kStar = 13;  // STARS.PNG cell width (13x16)

struct SavedLayout {
    std::vector<size_t> order;
    std::vector<float> widths;
    std::vector<uint8_t> hidden;
};

std::map<std::wstring, SavedLayout>& Store() {
    static std::map<std::wstring, SavedLayout> store;
    static bool loaded = false;
    if (!loaded) {
        loaded = true;
        // One line per column set: key \t order (comma separated) \t widths (comma separated)
        std::wstringstream in(Wide(cfg_columns.get().c_str()));
        std::wstring line;
        while (std::getline(in, line)) {
            const size_t a = line.find(L'\t'), b = line.find(L'\t', a + 1);
            if (a == std::wstring::npos || b == std::wstring::npos) continue;
            const size_t c = line.find(L'\t', b + 1);  // hidden flags (optional)
            SavedLayout s;
            std::wstringstream o(line.substr(a + 1, b - a - 1)), w(line.substr(b + 1, c == std::wstring::npos ? std::wstring::npos : c - b - 1));
            for (std::wstring v; std::getline(o, v, L',');) s.order.push_back(static_cast<size_t>(_wtoi(v.c_str())));
            for (std::wstring v; std::getline(w, v, L',');) s.widths.push_back(static_cast<float>(_wtof(v.c_str())));
            if (c != std::wstring::npos)
                for (wchar_t ch : line.substr(c + 1)) s.hidden.push_back(ch == L'1' ? 1 : 0);
            store[line.substr(0, a)] = std::move(s);
        }
    }
    return store;
}

void Persist() {
    std::wstring out;
    for (const auto& [key, s] : Store()) {
        out += key + L'\t';
        for (size_t i = 0; i < s.order.size(); ++i) out += (i ? L"," : L"") + std::to_wstring(s.order[i]);
        out += L'\t';
        for (size_t i = 0; i < s.widths.size(); ++i)
            out += (i ? L"," : L"") + std::to_wstring(static_cast<int>(std::lround(s.widths[i])));
        out += L'\t';
        for (uint8_t h : s.hidden) out += h ? L'1' : L'0';
        out += L'\n';
    }
    cfg_columns.set(Utf8(out).c_str());
}

D2D1_COLOR_F Hex(uint32_t rgb, float a = 1.0f) {
    return D2D1::ColorF(((rgb >> 16) & 0xFF) / 255.0f, ((rgb >> 8) & 0xFF) / 255.0f, (rgb & 0xFF) / 255.0f, a);
}

winrt::com_ptr<ID2D1SolidColorBrush> Brush(ID2D1DeviceContext* dc, uint32_t rgb, float a = 1.0f) {
    winrt::com_ptr<ID2D1SolidColorBrush> b;
    dc->CreateSolidColorBrush(Hex(rgb, a), b.put());
    return b;
}

void PlaceVisual(const wuc::Visual& v, float x, float y, float w, float h) {
    v.Offset(float3{x, y, 0});
    v.Size(float2{std::max(w, 0.0f), std::max(h, 0.0f)});
}

void Text(ID2D1DeviceContext* dc, const std::wstring& text, D2D1_RECT_F box, uint32_t ink,
          DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING,
          DWRITE_FONT_WEIGHT weight = DWRITE_FONT_WEIGHT_NORMAL, bool underline = false) {
    if (text.empty() || box.right - box.left < 4) return;
    auto layout = MakeText(text, 12.0f, weight, box.right - box.left, box.bottom - box.top, align);
    if (underline) layout->SetUnderline(TRUE, {0, static_cast<UINT32>(text.size())});
    dc->DrawTextLayout({box.left, box.top}, layout.get(), Brush(dc, ink).get());
}

float TextWidth(const std::wstring& text, float maxW) {
    auto layout = MakeText(text, 12.0f, DWRITE_FONT_WEIGHT_NORMAL, maxW, kRowHeight);
    DWRITE_TEXT_METRICS m{};
    layout->GetMetrics(&m);
    return std::min(m.width, maxW);
}
// A shelf's "show all": aero.msstyles' expando button (IMAGE 693 / 694 / 695 at rest, 696 - 698
// lit, 699 - 701 held, for 96 / 120 / 144 DPI) in vectors, measured on the 144-DPI bitmaps (a
// 29 px disc about (14.5, 15.5)): a metallic disc, flat on top and shading down from the middle,
// a light ring inside a one-pixel rim, and a #333333 chevron with a white edge. The chevron
// points right; it keeps the bitmap's offset of one pixel towards its point (the optical centre).
// `hot` and `pressed` (0..1) fade in the lit and held looks.
namespace expando {
struct Stop {
    float y; // bitmap rows (pixel centres)
    uint32_t rgb;
};
struct Look {
    std::initializer_list<Stop> body, ring;
    uint32_t rim;
};
const Look kLooks[3] = {
    {{{11.5f, 0xF4F4F4}, {12.5f, 0xF0F0F0}, {13.5f, 0xEBEBEB}, {14.5f, 0xE5E5E5}, {15.5f, 0xDEDEDE}, {16.5f, 0xD9D8D8},
      {17.5f, 0xD5D4D4}, {18.5f, 0xD1D0D0}, {19.5f, 0xCDCCCC}, {20.5f, 0xCAC9C9}, {21.5f, 0xC7C5C5}, {22.5f, 0xC4C3C3},
      {23.5f, 0xC2C0C0}, {24.5f, 0xC1BFBF}},
     {{2.5f, 0xFBFBFB}, {9.5f, 0xFAFAFA}, {15.5f, 0xF1F1F1}, {20.5f, 0xEAE9E9}, {25.5f, 0xE1E0DF}, {28.5f, 0xE9E9E8}},
     0x8C8C8C},
    {{{4.5f, 0xEEF7FD}, {10.5f, 0xECF7FD}, {11.5f, 0xEAF7FD}, {12.5f, 0xE5F5FD}, {13.5f, 0xDFF2FC}, {14.5f, 0xD8EEFA},
      {15.5f, 0xCFE9F7}, {16.5f, 0xC5E3F4}, {17.5f, 0xBDDFF2}, {18.5f, 0xB7DCF0}, {19.5f, 0xB2D9EF}, {20.5f, 0xACD6ED},
      {21.5f, 0xA8D4EC}, {22.5f, 0xA4D1EA}, {23.5f, 0xA0D0E9}, {24.5f, 0x9FCFE9}},
     {{2.5f, 0xF9FCFE}, {9.5f, 0xF6FBFE}, {15.5f, 0xDFF0FA}, {20.5f, 0xE0F0F8}, {24.5f, 0xD0E8F4}, {28.5f, 0xDDEEF7}},
     0x3C7FB1},
    {{{2.5f, 0xDEF0F9}, {5.5f, 0xDAEEF7}, {8.5f, 0xD7EBF5}, {10.5f, 0xD3E9F4}, {11.5f, 0xD0E7F2}, {12.5f, 0xCAE3F0},
      {13.5f, 0xC3DFED}, {14.5f, 0xBAD9EA}, {15.5f, 0xABD1E4}, {16.5f, 0x97C6DE}, {17.5f, 0x86BCD9}, {18.5f, 0x79B5D4},
      {19.5f, 0x6DAFD1}, {20.5f, 0x64AACE}, {21.5f, 0x61A8CD}},
     {},
     0x2C628B},
};
// The down chevron as the bitmap draws it (pixel edges).
constexpr D2D1_POINT_2F kChevron[] = {{7, 12},    {10, 12}, {14.5f, 16.5f}, {19, 12},
                                      {22, 12},   {22, 13.5f}, {14.5f, 21},  {7, 13.5f}};
constexpr float kSize = 29, kCx = 14.5f, kCy = 15.5f, kTop = 1;
}  // namespace expando

void RoundChevron(ID2D1DeviceContext* dc, D2D1_RECT_F box, float hot, float pressed) {
    winrt::com_ptr<ID2D1Factory> factory;
    dc->GetFactory(factory.put());
    const float k = (box.right - box.left) / expando::kSize; // DIP per bitmap pixel
    const D2D1_POINT_2F c{(box.left + box.right) / 2, (box.top + box.bottom) / 2};
    const auto rowY = [&](float row) { return c.y + (row - expando::kCy) * k; };
    const auto gradient = [&](std::initializer_list<expando::Stop> stops, float opacity) {
        std::vector<D2D1_GRADIENT_STOP> v;
        const float y0 = rowY(expando::kTop), y1 = rowY(expando::kTop + expando::kSize);
        for (const auto& st : stops) v.push_back({(rowY(st.y) - y0) / (y1 - y0), Hex(st.rgb)});
        winrt::com_ptr<ID2D1GradientStopCollection> collection;
        dc->CreateGradientStopCollection(v.data(), static_cast<UINT32>(v.size()), collection.put());
        winrt::com_ptr<ID2D1LinearGradientBrush> brush;
        dc->CreateLinearGradientBrush({{0, y0}, {0, y1}}, collection.get(), brush.put());
        brush->SetOpacity(opacity);
        return brush;
    };
    const float R = expando::kSize / 2 * k;
    const float weights[3] = {1.0f, hot, pressed};
    for (int i = 0; i < 3; ++i) {
        if (weights[i] <= 0) continue;
        const auto& look = expando::kLooks[i];
        dc->FillEllipse(D2D1::Ellipse(c, R, R), gradient(look.body, weights[i]).get());
        if (look.ring.size())
            dc->DrawEllipse(D2D1::Ellipse(c, R - 1.75f * k, R - 1.75f * k), gradient(look.ring, weights[i]).get(),
                            1.8f * k);
        dc->DrawEllipse(D2D1::Ellipse(c, R - 0.5f * k, R - 0.5f * k), Brush(dc, look.rim, weights[i]).get(), 1.0f * k);
    }
    // The chevron, turned to point right about the disc's centre; held, it sinks a little.
    winrt::com_ptr<ID2D1PathGeometry> glyph;
    winrt::com_ptr<ID2D1GeometrySink> sink;
    factory->CreatePathGeometry(glyph.put());
    glyph->Open(sink.put());
    for (size_t i = 0; i < std::size(expando::kChevron); ++i) {
        const float u = expando::kChevron[i].x - expando::kCx, v = expando::kChevron[i].y - expando::kCy;
        const D2D1_POINT_2F q{c.x + v * k, c.y - u * k + 0.5f * pressed};
        if (i == 0)
            sink->BeginFigure(q, D2D1_FIGURE_BEGIN_FILLED);
        else
            sink->AddLine(q);
    }
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    sink->Close();
    dc->DrawGeometry(glyph.get(), Brush(dc, 0xFFFFFF).get(), 2.0f * k); // its white edge, one pixel outside
    dc->FillGeometry(glyph.get(), Brush(dc, 0x333333).get());
}

// A shelf's paging arrow, tall as Apple Music's: a thin chevron, shown while the mouse is over
// the shelf (`shown`), and the command bar's glass button plate (aero Toolbar BUTTON 849) round it
// while the mouse is on it: hot (`plate`), and sunk in while held (`press`, the pressed state, the
// chevron half a DIP lower).
void ShelfArrowButton(ID2D1DeviceContext* dc, D2D1_RECT_F box, bool right, float shown, float plate, float press) {
    if (shown <= 0 && plate <= 0 && press <= 0) return;
    const float hot = plate * (1 - press);
    if (hot > 0 && !win7::DrawNineGrid(dc, win7::Module::Aero, 849, {0, 26, 7, 52}, {3, 3, 3, 3}, box, hot)) {
        dc->FillRoundedRectangle({box, 3, 3}, Brush(dc, 0xEBF3FC, hot).get());
        dc->DrawRoundedRectangle({box, 3, 3}, Brush(dc, 0xA1A8B0, hot).get(), 1.0f);
    }
    if (press > 0 && !win7::DrawNineGrid(dc, win7::Module::Aero, 849, {0, 52, 7, 78}, {3, 3, 3, 3}, box, press)) {
        dc->FillRoundedRectangle({box, 3, 3}, Brush(dc, 0xD6E5F5, press).get());
        dc->DrawRoundedRectangle({box, 3, 3}, Brush(dc, 0x8E9AA6, press).get(), 1.0f);
    }
    if (shown <= 0) return;
    const float w = box.right - box.left, h = box.bottom - box.top;
    const float cx = box.left + w / 2, cy = box.top + h / 2 + press * 0.5f, half = h * 0.22f, depth = half * 0.55f;
    const float tip = right ? cx + depth / 2 : cx - depth / 2, back = right ? cx - depth / 2 : cx + depth / 2;
    winrt::com_ptr<ID2D1Factory> factory;
    dc->GetFactory(factory.put());
    winrt::com_ptr<ID2D1StrokeStyle> round;
    factory->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                                                           D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND),
                               nullptr, 0, round.put());
    winrt::com_ptr<ID2D1PathGeometry> path;
    winrt::com_ptr<ID2D1GeometrySink> sink;
    factory->CreatePathGeometry(path.put());
    path->Open(sink.put());
    sink->BeginFigure({back, cy - half}, D2D1_FIGURE_BEGIN_HOLLOW);
    sink->AddLine({tip, cy});
    sink->AddLine({back, cy + half});
    sink->EndFigure(D2D1_FIGURE_END_OPEN);
    sink->Close();
    dc->DrawGeometry(path.get(), Brush(dc, 0x1E395B, shown).get(), 2.2f, round.get());
}

// A song card's play button: a triangle in Aero's blue (after wmploc's 16 px play arrow, 5346),
// rounded corners, a white rim and a soft shadow; brighter under the mouse.
// A song's download mark after Apple Music's: a 16 DIP disc with a white arrow of round strokes
// (a stem and an open head). `hot` (0..1, fading) is the mouse on it. Downloaded: Apple's grey
// disc, darker under the mouse. The offer to download (a hovered row) is a glass ball in the
// colours of WMP 12's play orb (wmploc 4153 at rest, 4155 lit, sampled down its middle): a deep
// navy body from a sharp horizon (#0C3E90) brightening to a cyan foot (#2BAAEA, #44CAFA), a pale
// gloss over the upper part, a navy rim. Lit it glows: the body turns azure (#0060B1 at the
// horizon), the foot nearly white cyan (#7AF0FC), the rim blue (#0372C0) and a blue halo
// (#23A4EF) shines round it, as the orb's does.
void DownloadMark(ID2D1DeviceContext* dc, D2D1_POINT_2F c, bool offer, float hot, float opacity) {
    if (opacity <= 0) return;
    const float r = 8;
    const auto colour = [&](uint32_t rgb, float a = 1.0f) { return D2D1::ColorF(rgb, a * opacity); };
    const auto gradient = [&](D2D1_POINT_2F from, D2D1_POINT_2F to,
                              std::initializer_list<std::pair<float, D2D1_COLOR_F>> stops) {
        std::vector<D2D1_GRADIENT_STOP> list;
        for (const auto& [at, value] : stops)
            list.push_back({at, value});
        winrt::com_ptr<ID2D1GradientStopCollection> collection;
        dc->CreateGradientStopCollection(list.data(), static_cast<UINT32>(list.size()), collection.put());
        winrt::com_ptr<ID2D1LinearGradientBrush> brush;
        dc->CreateLinearGradientBrush({from, to}, collection.get(), brush.put());
        return brush;
    };
    const auto mix = [](uint32_t a, uint32_t b, float t) {
        const auto ch = [&](int shift) {
            const float x = ((a >> shift) & 0xFF) * (1 - t) + ((b >> shift) & 0xFF) * t;
            return static_cast<uint32_t>(std::lround(x)) << shift;
        };
        return ch(16) | ch(8) | ch(0);
    };
    const D2D1_ELLIPSE disc{c, r, r};
    if (offer) {
        // The halo of the lit orb, round the disc.
        if (hot > 0) {
            D2D1_GRADIENT_STOP haloStops[] = {{0.0f, colour(0x23A4EF, 0.0f)},
                                              {0.72f, colour(0x23A4EF, 0.55f * hot)},
                                              {1.0f, colour(0x23A4EF, 0.0f)}};
            winrt::com_ptr<ID2D1GradientStopCollection> haloList;
            dc->CreateGradientStopCollection(haloStops, 3, haloList.put());
            winrt::com_ptr<ID2D1RadialGradientBrush> halo;
            dc->CreateRadialGradientBrush({c, {0, 0}, r + 3, r + 3}, haloList.get(), halo.put());
            dc->FillEllipse({c, r + 3, r + 3}, halo.get());
        }
        // The body, top to foot.
        dc->FillEllipse(disc, gradient({c.x, c.y - r}, {c.x, c.y + r},
                                       {{0.0f, colour(mix(0x1A4E9E, 0x2A7CC4, hot))},
                                        {0.45f, colour(mix(0x0C3E90, 0x0060B1, hot))},
                                        {0.62f, colour(mix(0x0C55A3, 0x0A7ACA, hot))},
                                        {0.8f, colour(mix(0x0C85D8, 0x38ADE6, hot))},
                                        {0.9f, colour(mix(0x2BAAEA, 0x5FD1F7, hot))},
                                        {1.0f, colour(mix(0x44CAFA, 0x7AF0FC, hot))}})
                                  .get());
        // The foot's glow, near white when lit.
        D2D1_GRADIENT_STOP glowStops[] = {{0.0f, colour(mix(0x5FD6FF, 0xC8FFFF, hot), 0.55f + 0.35f * hot)},
                                          {1.0f, colour(0x5FD6FF, 0)}};
        winrt::com_ptr<ID2D1GradientStopCollection> glowList;
        dc->CreateGradientStopCollection(glowStops, 2, glowList.put());
        winrt::com_ptr<ID2D1RadialGradientBrush> glow;
        dc->CreateRadialGradientBrush({{c.x, c.y + r * 0.85f}, {0, 0}, r * 0.75f, r * 0.45f}, glowList.get(),
                                      glow.put());
        dc->FillEllipse(disc, glow.get());
        // The gloss: pale over the upper part, ending in a sharp horizon.
        dc->FillEllipse({{c.x, c.y - r * 0.5f}, r * 0.8f, r * 0.46f},
                        gradient({c.x, c.y - r * 0.96f}, {c.x, c.y - r * 0.04f},
                                 {{0.0f, colour(0xFFFFFF, 0.78f)}, {1.0f, colour(0xFFFFFF, 0.28f)}})
                            .get());
        winrt::com_ptr<ID2D1SolidColorBrush> rim;
        dc->CreateSolidColorBrush(colour(mix(0x0A3478, 0x0372C0, hot), 0.95f), rim.put());
        dc->DrawEllipse({c, r - 0.4f, r - 0.4f}, rim.get(), 0.8f);
    } else {
        winrt::com_ptr<ID2D1SolidColorBrush> grey;
        dc->CreateSolidColorBrush(colour(mix(0xC4C4C4, 0xA0A0A0, hot)), grey.put());
        dc->FillEllipse(disc, grey.get());
    }
    // The arrow: round strokes, a stem and an open head.
    winrt::com_ptr<ID2D1Factory> factory;
    dc->GetFactory(factory.put());
    winrt::com_ptr<ID2D1StrokeStyle> round;
    factory->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                                                           D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND),
                               nullptr, 0, round.put());
    winrt::com_ptr<ID2D1PathGeometry> arrow;
    winrt::com_ptr<ID2D1GeometrySink> sink;
    factory->CreatePathGeometry(arrow.put());
    arrow->Open(sink.put());
    sink->BeginFigure({c.x, c.y - 4.2f}, D2D1_FIGURE_BEGIN_HOLLOW);
    sink->AddLine({c.x, c.y + 3.6f});
    sink->EndFigure(D2D1_FIGURE_END_OPEN);
    sink->BeginFigure({c.x - 3.3f, c.y + 0.4f}, D2D1_FIGURE_BEGIN_HOLLOW);
    sink->AddLine({c.x, c.y + 3.7f});
    sink->AddLine({c.x + 3.3f, c.y + 0.4f});
    sink->EndFigure(D2D1_FIGURE_END_OPEN);
    sink->Close();
    if (offer) { // a soft shadow lifts it off the glass
        D2D1_MATRIX_3X2_F current;
        dc->GetTransform(&current);
        dc->SetTransform(D2D1::Matrix3x2F::Translation(0, 0.6f) * current);
        winrt::com_ptr<ID2D1SolidColorBrush> shadow;
        dc->CreateSolidColorBrush(colour(0x061E4A, 0.45f), shadow.put());
        dc->DrawGeometry(arrow.get(), shadow.get(), 1.7f, round.get());
        dc->SetTransform(current);
    }
    winrt::com_ptr<ID2D1SolidColorBrush> white;
    dc->CreateSolidColorBrush(colour(0xFFFFFF), white.put());
    dc->DrawGeometry(arrow.get(), white.get(), 1.7f, round.get());
}

// A download in progress: a ring (16 DIP) filling clockwise from the top in Windows 7's progress
// colours (green, yellow paused, red failed) over a grey track; waiting: the track alone.
void ProgressRing(ID2D1DeviceContext* dc, D2D1_POINT_2F c, float share, media::DownloadCell::Look state) {
    const float r = 6.5f, width = 3.0f;
    dc->DrawEllipse({c, r, r}, Brush(dc, 0xDADADA).get(), width);
    share = std::clamp(share, 0.0f, 1.0f);
    if (share <= 0) return;
    using Look = media::DownloadCell::Look;
    const uint32_t light = state == Look::Failed ? 0xF08080 : state == Look::Paused ? 0xFFE27A : 0x7EEB86;
    const uint32_t dark = state == Look::Failed ? 0xC81E1E : state == Look::Paused ? 0xD49B00 : 0x0AB526;
    D2D1_GRADIENT_STOP gradient[2] = {{0.0f, D2D1::ColorF(light)}, {1.0f, D2D1::ColorF(dark)}};
   winrt::com_ptr<ID2D1GradientStopCollection> collection;
    dc->CreateGradientStopCollection(gradient, 2, collection.put());
   winrt::com_ptr<ID2D1LinearGradientBrush> ink;
    dc->CreateLinearGradientBrush({{c.x - r, c.y - r}, {c.x + r, c.y + r}}, collection.get(), ink.put());
    if (share >= 0.999f) {
        dc->DrawEllipse({c, r, r}, ink.get(), width);
        return;
    }
   winrt::com_ptr<ID2D1Factory> factory;
    dc->GetFactory(factory.put());
   winrt::com_ptr<ID2D1PathGeometry> arc;
   winrt::com_ptr<ID2D1GeometrySink> sink;
    factory->CreatePathGeometry(arc.put());
    arc->Open(sink.put());
    const float angle = share * 6.2831853f;
    sink->BeginFigure({c.x, c.y - r}, D2D1_FIGURE_BEGIN_HOLLOW);
    sink->AddArc({{c.x + r * std::sin(angle), c.y - r * std::cos(angle)},
                  {r, r},
                  0,
                  D2D1_SWEEP_DIRECTION_CLOCKWISE,
                  share > 0.5f ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL});
    sink->EndFigure(D2D1_FIGURE_END_OPEN);
    sink->Close();
   winrt::com_ptr<ID2D1StrokeStyle> round;
    factory->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND), nullptr, 0,
                               round.put());
    dc->DrawGeometry(arc.get(), ink.get(), width, round.get());
}

void PlayTriangle(ID2D1DeviceContext* dc, D2D1_RECT_F box, float hot, float opacity) {
    if (opacity <= 0) return;
    winrt::com_ptr<ID2D1Factory> factory;
    dc->GetFactory(factory.put());
    const float w = box.right - box.left, h = box.bottom - box.top;
    const D2D1_POINT_2F p[3] = {{box.left + w * 0.18f, box.top + h * 0.08f},
                                {box.left + w * 0.18f, box.top + h * 0.92f},
                                {box.left + w * 0.92f, box.top + h * 0.5f}};
    const float r = w * 0.12f;
    const auto toward = [](D2D1_POINT_2F a, D2D1_POINT_2F b, float d) {
        const float len = std::hypot(b.x - a.x, b.y - a.y);
        return D2D1_POINT_2F{a.x + (b.x - a.x) * d / len, a.y + (b.y - a.y) * d / len};
    };
    const auto build = [&](D2D1_POINT_2F offset) {
        winrt::com_ptr<ID2D1PathGeometry> path;
        winrt::com_ptr<ID2D1GeometrySink> sink;
        factory->CreatePathGeometry(path.put());
        path->Open(sink.put());
        for (int i = 0; i < 3; ++i) {
            const D2D1_POINT_2F prev = p[(i + 2) % 3], at = p[i], next = p[(i + 1) % 3];
            D2D1_POINT_2F in = toward(at, prev, r), out = toward(at, next, r);
            in = {in.x + offset.x, in.y + offset.y};
            out = {out.x + offset.x, out.y + offset.y};
            if (i == 0)
                sink->BeginFigure(in, D2D1_FIGURE_BEGIN_FILLED);
            else
                sink->AddLine(in);
            sink->AddQuadraticBezier({{at.x + offset.x, at.y + offset.y}, out});
        }
        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
        sink->Close();
        return path;
    };
    dc->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                         D2D1::IdentityMatrix(), opacity),
                  nullptr);
    auto shadow = build({0.6f, 1.0f});
    dc->FillGeometry(shadow.get(), Brush(dc, 0x000000, 0.28f).get());
    auto shape = build({0, 0});
    const auto mix = [hot](uint32_t a, uint32_t b) {
        const auto ch = [&](int shift) {
            const int x = (a >> shift) & 0xFF, y = (b >> shift) & 0xFF;
            return static_cast<uint32_t>(std::lround(x + (y - x) * hot)) << shift;
        };
        return ch(16) | ch(8) | ch(0);
    };
    const D2D1_GRADIENT_STOP stops[] = {{0.0f, Hex(mix(0x5BB6FF, 0x8ED0FF))},
                                        {0.5f, Hex(mix(0x1A84EC, 0x3C9DF5))},
                                        {1.0f, Hex(mix(0x0A4FC0, 0x1C67D6))}};
    winrt::com_ptr<ID2D1GradientStopCollection> collection;
    winrt::com_ptr<ID2D1LinearGradientBrush> fill;
    if (SUCCEEDED(dc->CreateGradientStopCollection(stops, 3, collection.put())) &&
        SUCCEEDED(dc->CreateLinearGradientBrush({{0, box.top}, {0, box.bottom}}, collection.get(), fill.put())))
        dc->FillGeometry(shape.get(), fill.get());
    dc->DrawGeometry(shape.get(), Brush(dc, 0xFFFFFF).get(), std::max(1.2f, w / 16.0f));
    dc->PopLayer();
}
}  // namespace

ListView::ListView(const wuc::ContainerVisual& parent) {
    const auto& comp = gfx::CompositionHost::Get().Compositor();
    m_root = comp.CreateContainerVisual();
    m_clipGeometry = comp.CreateRectangleGeometry();
    m_root.Clip(comp.CreateGeometricClip(m_clipGeometry));
    m_header = comp.CreateSpriteVisual();
    m_status = comp.CreateSpriteVisual();
    m_rows = comp.CreateSpriteVisual();
    m_scroll = comp.CreateSpriteVisual();
    m_viewport = comp.CreateContainerVisual();
    m_viewport.Clip(comp.CreateInsetClip());
    m_content = comp.CreateContainerVisual();
    m_viewport.Children().InsertAtTop(m_content);
    m_root.Children().InsertAtTop(m_viewport);
    m_content.Children().InsertAtTop(m_rows);
    m_root.Children().InsertAtTop(m_status);
    m_root.Children().InsertAtTop(m_header);
    m_root.Children().InsertAtTop(m_scroll);
    m_thumb = comp.CreateSpriteVisual();
    m_root.Children().InsertAtTop(m_thumb);
    // thumb top (px) = clamp(a + b * (top - content offset), lo, hi): content coordinates are
    // absolute, so the scroll position in px is minus m_content's offset, mid-glide included.
    m_thumbPlace = comp.CreatePropertySet();
    for (const wchar_t* term : {L"x", L"a", L"b", L"top", L"lo", L"hi"}) m_thumbPlace.InsertScalar(term, 0);
    auto follow = comp.CreateExpressionAnimation(
        L"Vector3(p.x, Clamp(p.a + p.b * (p.top - c.Offset.Y), p.lo, p.hi), 0)");
    follow.SetReferenceParameter(L"p", m_thumbPlace);
    follow.SetReferenceParameter(L"c", m_content);
    m_thumb.StartAnimation(L"Offset", follow);
    // A shelf sliding to another page: its tiles in their own strip under the rows (whose
    // surface is clear there), clipped to the list, moved by the compositor.
    m_slideClip = comp.CreateContainerVisual();
    m_slideClip.Clip(comp.CreateInsetClip());
    m_slideClip.IsVisible(false);
    m_slide = comp.CreateSpriteVisual();
    m_slideClip.Children().InsertAtTop(m_slide);
    m_content.Children().InsertBelow(m_slideClip, m_rows);
    m_cards = std::make_unique<CardLayer>(m_content, m_rows);
    m_status.IsVisible(false);
    parent.Children().InsertAtTop(m_root);
    // A plain list draws covers only in its Album art column.
    m_artToken = ArtCache::Get().Subscribe([this] {
        if (m_mode != Mode::Plain || ArtColumnVisible()) PaintRows();
    });
    m_iconToken = SubscribeSourceIcons([this] {
        if (m_mode == Mode::Tiles) PaintRows();
    });
    m_deviceReplaced = gfx::CompositionHost::Get().OnDeviceReplaced([this] {
        m_artBitmaps.clear();
        PaintRows();
    });
    m_cellChanges = media::WatchControlCells([this] {
        if (m_source && m_mode != Mode::Tiles)
            for (const auto& column : m_source->Columns())
                if (KindOf(column) == CellKind::Download) {
                    PaintRows();
                    return;
                }
    });
}

ListView::~ListView() {
    if (auto* cache = ArtCache::Existing()) cache->Unsubscribe(m_artToken);
    UnsubscribeSourceIcons(m_iconToken);
}

// ---- Geometry -------------------------------------------------------------------------------

int ListView::RowHeightPx() const {
    return static_cast<int>(std::lround((ArtColumnVisible() ? kArtRowHeight : kRowHeight) * m_scale));
}
int ListView::HeaderHeightPx() const {
    return m_mode == Mode::Tiles ? 0 : static_cast<int>(std::lround(kHeaderHeight * m_scale));
}
int ListView::StatusHeightPx() const {
    return m_statusText.empty() ? 0 : static_cast<int>(std::lround(kRowHeight * m_scale));
}
int ListView::ContentOffsetPx() const {
    return HeaderHeightPx() + StatusHeightPx();
}
int ListView::ScrollWidthPx() const { return static_cast<int>(std::lround(kScrollWidth * m_scale)); }

bool ListView::ArtColumnVisible() const {
    if (!m_source || m_mode == Mode::Tiles) return false;
    // Grouped Details (library songs) has no album column, so growing the rows is safe.
    // Expanded tile has the album column and no Art column; keep its measured height.
    if (m_mode == Mode::Grouped && m_groupArt) return false;
    const auto& columns = m_source->Columns();
    for (size_t i = 0; i < columns.size(); ++i) {
        const bool shown = !(i < m_hidden.size() ? m_hidden[i] != 0 : columns[i].hidden);
        if (shown && KindOf(columns[i]) == CellKind::Art) return true;
    }
    return false;
}

int ListView::LineHeightPx() const {
    if (m_mode == Mode::Tiles) return static_cast<int>(std::lround(TileSize().height * m_scale));
    return RowHeightPx();
}

D2D1_SIZE_F ListView::TileSize() const {
    return TileSizeOf(m_source ? m_source->Tiles() : media::TileStyle::Album);
}

size_t ListView::TileColumns() const {
    return TileColumnsOf(m_source ? m_source->Tiles() : media::TileStyle::Album);
}

float ListView::TilePitch() const {
    return TilePitchOf(m_source ? m_source->Tiles() : media::TileStyle::Album);
}

float ListView::TileLeft(size_t column) const {
    return TileLeftOf(m_source ? m_source->Tiles() : media::TileStyle::Album, static_cast<float>(column));
}

media::TileStyle ListView::GroupTiles(size_t group) const {
    const auto* groups = m_source ? m_source->Groups() : nullptr;
    if (groups && group < groups->size() && (*groups)[group].tiles != media::TileStyle::None)
        return (*groups)[group].tiles;
    return m_source ? m_source->Tiles() : media::TileStyle::Album;
}

D2D1_SIZE_F ListView::TileSizeOf(media::TileStyle style) const {
    switch (style) {
    case media::TileStyle::Artist: return kArtistTile;
    case media::TileStyle::Song: return kSongTile;
    case media::TileStyle::Card: return kCardTile;
    default: return kAlbumTile;
    }
}

size_t ListView::TileColumnsOf(media::TileStyle style) const {
    const float w = RowsWidth() - 2 * kTilePad;
    return static_cast<size_t>(std::max(1.0f, std::floor(w / TileSizeOf(style).width)));
}

// The row's spare width is shared out between the tiles (owner, 2026-11-14: it was all at
// the right), so they spread as the window is resized. A shelf's tiles keep their own width
// instead (owner, 2026-11-20, as the cards): the last one may show in part.
float ListView::TilePitchOf(media::TileStyle style, bool shelf) const {
    if (shelf || style == media::TileStyle::Card) return TileSizeOf(style).width;
    return std::max(TileSizeOf(style).width, (RowsWidth() - 2 * kTilePad) / static_cast<float>(TileColumnsOf(style)));
}

float ListView::TileLeftOf(media::TileStyle style, float column, bool shelf) const {
    const float pitch = TilePitchOf(style, shelf);
    return std::round((kTilePad + column * pitch + (pitch - TileSizeOf(style).width) / 2) * m_scale) / m_scale;
}

size_t ListView::ShelfColumns(size_t group) const {
    const auto& g = (*m_source->Groups())[group];
    const size_t rows = std::max<size_t>(g.tileLines, 1);
    return (g.count + rows - 1) / rows;
}

float ListView::ShelfPos(size_t group) const {
    const auto& g = (*m_source->Groups())[group];
    const auto it = m_shelves.find(g.key);
    // A restored place beyond a shelf that came back shorter: its end.
    return it == m_shelves.end() ? 0.0f : std::min(it->second.pos, ShelfLast(group));
}

std::map<std::wstring, float> ListView::ShelfPositions() const {
    std::map<std::wstring, float> out;
    for (const auto& [key, shelf] : m_shelves)
        if (shelf.target > 0) out[key] = shelf.target;
    return out;
}

void ListView::SetShelfPositions(const std::map<std::wstring, float>& positions) {
    if (positions.empty()) return;
    for (const auto& [key, pos] : positions) {
        Shelf& shelf = m_shelves[key];
        shelf.pos = shelf.target = pos;
    }
    PaintRows();
}

// How many of a shelf's columns the view holds (a part of one counts as that part).
float ListView::ShelfVisible(size_t group) const {
    return (RowsWidth() - 2 * kTilePad) / TilePitchOf(GroupTiles(group), true);
}

float ListView::ShelfLast(size_t group) const {
    return std::max(0.0f, static_cast<float>(ShelfColumns(group)) - ShelfVisible(group));
}

bool ListView::ShelfCanPage(size_t group, bool right) const {
    const auto& g = (*m_source->Groups())[group];
    const auto it = m_shelves.find(g.key);
    const float target = it == m_shelves.end() ? 0.0f : it->second.target;
    return right ? target < ShelfLast(group) - 0.01f : target > 0.01f;
}

void ListView::PageShelf(size_t group, bool right) {
    if (!ShelfCanPage(group, right) || group >= m_groupLine.size()) return;
    const auto& g = (*m_source->Groups())[group];
    const float page = std::max(1.0f, std::floor(ShelfVisible(group)));
    const float target = m_shelves[g.key].target;
    SlideShelf(group, target + (right ? page : -page), kShelfSlideMs);
}

// Where a shelf stands on screen now: mid-slide, between where it slid from and to.
float ListView::ShelfShownPos(size_t group) const {
    if (group != m_shelfSlide.group || m_shelfSlide.ms <= 0) return ShelfPos(group);
    const float t = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - m_shelfSlide.start).count() /
                    static_cast<float>(m_shelfSlide.ms);
    if (t >= 1) return ShelfPos(group);
    return m_shelfSlide.from + (m_shelfSlide.to - m_shelfSlide.from) * ShelfEase(std::max(t, 0.0f));
}

void ListView::SlideShelf(size_t group, float to, int ms) {
    if (group >= m_groupLine.size()) return;
    const auto& g = (*m_source->Groups())[group];
    // From where it shows now, so a slide given while another runs carries on from it.
    const float from = ShelfShownPos(group);
    Shelf& shelf = m_shelves[g.key];
    to = std::clamp(to, 0.0f, ShelfLast(group));
    // Nothing to move (a wheel at the shelf's end): the strip, if one runs, finishes as it is.
    if (std::abs(to - from) < 0.001f && std::abs(to - shelf.target) < 0.001f) return;
    if (!m_slideKey.empty()) {
        m_slideKey.clear();
        m_slide.StopAnimation(L"Offset");
        m_slideClip.IsVisible(false);
    }
    shelf.pos = shelf.target = to;
    m_shelfSlide = {group, from, to, std::chrono::steady_clock::now(), ms};
    const media::TileStyle style = GroupTiles(group);
    if (style == media::TileStyle::Card) { // the cards' strip slides itself (LayoutCards)
        m_cardSlideGroup = group;
        m_cardSlideFrom = from;
        m_cardSlideMs = ms;
        PaintRows();
        return;
    }
    // The strip: every column the slide passes over, the shelf's rows.
    const size_t rows = std::max<size_t>(g.tileLines, 1), shown = std::min(rows, g.count);
    const size_t header = m_groupLine[group];
    if (header + 1 + shown >= m_lineTop.size()) {
        PaintRows();
        return;
    }
    const float tileH = TileSizeOf(style).height, pitch = TilePitchOf(style, true);
    const size_t first = static_cast<size_t>(std::floor(std::min(from, to)));
    const size_t end = std::min(ShelfColumns(group),
                                static_cast<size_t>(std::ceil(std::max(from, to) + ShelfVisible(group))) + 1);
    // In content coordinates (absolute: m_content's offset is minus the scroll position).
    const float top = m_lineTop[header + 1];
    const float stripW = TileLeftOf(style, static_cast<float>(end - first), true) + pitch;
    const float stripH = tileH * static_cast<float>(shown);
    const size_t focus = m_source->Focus();
    auto& host = gfx::CompositionHost::Get();
    const int wpx = std::max(1, static_cast<int>(std::ceil(stripW * m_scale)));
    const int hpx = std::max(1, static_cast<int>(std::ceil(stripH * m_scale)));
    host.Repaint(m_slide, wpx, hpx, m_scale, [&](ID2D1DeviceContext* dc) {
        for (size_t c = first; c < end; ++c)
            for (size_t k = 0; k < shown; ++k) {
                const size_t idx = g.first + c * rows + k;
                if (idx >= g.first + g.count) continue;
                PaintTile(dc, idx, TileLeftOf(style, static_cast<float>(c - first), true), tileH * static_cast<float>(k),
                          style, focus);
            }
    });
    m_slideClip.Offset(float3{0, std::round(top * m_scale), 0});
    m_slideClip.Size(float2{RowsWidth() * m_scale, static_cast<float>(hpx)});
    m_slideClip.IsVisible(true);
    const auto offsetOf = [&](float pos) {
        return float3{std::round(-(pos - static_cast<float>(first)) * pitch * m_scale), 0, 0};
    };
    m_slide.Offset(offsetOf(from));
    const auto& comp = host.Compositor();
    auto slide = comp.CreateVector3KeyFrameAnimation();
    slide.InsertKeyFrame(1.0f, offsetOf(to),
                         gfx::Easing(kShelfCurve));
    slide.Duration(std::chrono::milliseconds(ms));
    m_slide.StartAnimation(L"Offset", slide);
    m_slideKey = g.key;
    m_slideEnd = GetTickCount64() + static_cast<ULONGLONG>(ms);
    m_slideTop = ContentTopDip();
    PaintRows(); // without the shelf's tiles, which the strip shows meanwhile
    if (onScrollAnim) onScrollAnim();
}

void ListView::FinishSlide() {
    if (m_slideKey.empty()) return;
    m_slideKey.clear();
    m_slide.StopAnimation(L"Offset");
    m_slideClip.IsVisible(false);
    PaintRows();
}

D2D1_RECT_F ListView::ShelfArrow(size_t, bool right, float top, float bottom) const {
    const float y = std::round(((top + bottom - kShelfArrow.height) / 2) * m_scale) / m_scale;
    const float x = right ? RowsWidth() - 2 - kShelfArrow.width : 2.0f;
    return {x, y, x + kShelfArrow.width, y + kShelfArrow.height};
}

D2D1_RECT_F ListView::TilePlayRect(media::TileStyle style, float x, float y) const {
    const D2D1_SIZE_F ts = TileSizeOf(style);
    switch (style) {
    case media::TileStyle::Card: {
        const float r = x + kCardInset.x + CardLayer::kCard.width - CardLayer::kPlayInset;
        const float b = y + kCardInset.y + CardLayer::kCard.height - CardLayer::kPlayInset;
        return {r - CardLayer::kPlay, b - CardLayer::kPlay, r, b};
    }
    case media::TileStyle::Song: {
        const float a = kSongTileArt, ay = y + (ts.height - a) / 2, cx = x + 8 + a / 2, cy = ay + a / 2;
        return {cx - kSongPlay / 2, cy - kSongPlay / 2, cx + kSongPlay / 2, cy + kSongPlay / 2};
    }
    case media::TileStyle::Artist: {
        const float a = kArtistTileArt, ax = x + 10, ay = y + (ts.height - a) / 2;
        return {ax + a - kTilePlay - 4, ay + a - kTilePlay - 4, ax + a - 4, ay + a - 4};
    }
    default: {
        const float a = kAlbumTileArt, ax = x + (ts.width - a) / 2, ay = y + 10;
        return {ax + a - kTilePlay - 4, ay + a - kTilePlay - 4, ax + a - 4, ay + a - 4};
    }
    }
}

size_t ListView::TileAtLine(const Line& line, float x, float* left) const {
    const auto& groups = *m_source->Groups();
    const auto& g = groups[line.group];
    const media::TileStyle style = GroupTiles(line.group);
    const float pitch = TilePitchOf(style, g.carousel);
    if (x < kTilePad || x >= RowsWidth()) return SIZE_MAX;
    if (g.carousel) {
        const float pos = ShelfPos(line.group);
        const float column = std::floor((x - kTilePad) / pitch + pos);
        if (column < 0) return SIZE_MAX;
        const size_t rows = std::max<size_t>(g.tileLines, 1);
        const size_t idx = g.first + static_cast<size_t>(column) * rows + line.sub;
        if (idx >= g.first + g.count) return SIZE_MAX;
        if (left) *left = TileLeftOf(style, column - pos, true);
        return idx;
    }
    const size_t col = static_cast<size_t>((x - kTilePad) / pitch);
    const size_t idx = line.row + col;
    if (col >= TileColumnsOf(style) || idx >= TileGroupEnd(line.group)) return SIZE_MAX;
    if (left) *left = TileLeftOf(style, static_cast<float>(col));
    return idx;
}

RECT ListView::RowsRect() const {
    return {m_bounds.left, m_bounds.top + ContentOffsetPx(), m_bounds.right - ScrollWidthPx(),
            m_bounds.bottom};
}

float ListView::RowsWidth() const {
    const RECT r = RowsRect();
    return std::max<LONG>(r.right - r.left, 0) / m_scale;
}

float ListView::ArtSize() const { return kGroupMinLines * (RowHeightPx() / m_scale) - 8; }
float ListView::AlbumTextLeft(float albumLeft) const { return albumLeft + 3 + ArtSize() + 8; }

float ListView::LineHeightDip(const Line& line) const {
    if (m_tileGroups)
        return line.kind == Line::GroupHeader ? kTileGroupHeader : TileSizeOf(GroupTiles(line.group)).height;
    if (m_mode == Mode::Grouped && !m_groupArt && line.kind == Line::GroupHeader)
        return static_cast<float>(std::lround(kRowHeight * m_scale)) / m_scale;
    return RowHeightPx() / m_scale;
}

float ListView::LineTopDip(size_t line) const {
    if (Lined()) return m_lineTop.empty() ? 0 : m_lineTop[std::min(line, m_lineTop.size() - 1)];
    return static_cast<float>(line) * (LineHeightPx() / m_scale);
}

size_t ListView::LineAtDip(float y) const {
    if (Lined()) {
        if (m_lineTop.size() < 2) return 0;
        auto it = std::upper_bound(m_lineTop.begin(), m_lineTop.end(), y);
        const size_t line = it == m_lineTop.begin() ? 0 : static_cast<size_t>(it - m_lineTop.begin() - 1);
        return std::min(line, m_lineTop.size() - 2);
    }
    return y > 0 ? static_cast<size_t>(y / std::max(LineHeightPx() / m_scale, 0.001f)) : 0;
}

float ListView::RowsHeightDip() const {
    if (m_paintViewH >= 0) return m_paintViewH;
    const RECT r = RowsRect();
    return std::max<LONG>(r.bottom - r.top, 0) / m_scale;
}

size_t ListView::LineCount() const {
    const size_t n = m_source ? m_source->Count() : 0;
    switch (m_mode) {
    case Mode::Grouped: return m_lines.size();
    case Mode::Tiles: {
        if (m_tileGroups) return m_lines.size();
        const size_t cols = TileColumns();
        return (n + cols - 1) / cols;
    }
    default: return n;
    }
}

size_t ListView::LineOfRow(size_t row) const {
    switch (m_mode) {
    case Mode::Grouped: return row < m_rowLine.size() ? m_rowLine[row] : 0;
    case Mode::Tiles:
        if (m_tileGroups) return row < m_rowLine.size() ? m_rowLine[row] : 0;
        return row / TileColumns();
    default: return row;
    }
}

size_t ListView::VisibleLines() const {
    const float viewH = RowsHeightDip();
    if (Lined() && !m_lineTop.empty()) {
        size_t n = 0;
        float y = -m_topOffsetDip;
        for (size_t i = m_top; i < m_lines.size() && y < viewH; ++i) {
            y += LineHeightDip(m_lines[i]);
            ++n;
        }
        return n;
    }
    const float lineH = std::max(LineHeightPx() / m_scale, 0.001f);
    return static_cast<size_t>((viewH + std::max(m_topOffsetDip, 0.0f)) / lineH);
}

size_t ListView::MaxTop() const {
    const size_t n = LineCount();
    if (Lined() && !m_lineTop.empty()) {
        const float viewH = RowsHeightDip();
        const float total = m_lineTop.back();
        if (total <= viewH) return 0;
        // Stop where the last line's bottom reaches the viewport bottom; scrolling further
        // would only add empty space below it.
        auto it = std::lower_bound(m_lineTop.begin(), m_lineTop.end(), total - viewH);
        const size_t line = static_cast<size_t>(it - m_lineTop.begin());
        return std::min(line, n ? n - 1 : 0);
    }
    const size_t v = VisibleLines();
    return n > v ? n - v : 0;
}

std::vector<ListView::Col> ListView::Columns(float /*total*/) const {
    std::vector<Col> out;
    if (!m_source) return out;
    const auto& cols = m_source->Columns();
    // Every column keeps its own width, the "flexible" one included (it only gets a wider
    // default), so the last column ends where the user put it, not at the window edge.
    auto widthOf = [&](size_t i) {
        return i < m_widths.size() && m_widths[i] > 0 ? m_widths[i] : (cols[i].flex ? kFlexWidth : cols[i].width);
    };
    auto shown = [&](size_t i) { return !(i < m_hidden.size() ? m_hidden[i] != 0 : cols[i].hidden); };
    // With the album column the playing-marker gutter sits after it, before the tracks.
    const bool album = AlbumColumn();
    float x = album ? 0.0f : kGutter;
    for (size_t d = 0; d < cols.size(); ++d) {
        const size_t src = d < m_order.size() ? m_order[d] : d;
        if (!shown(src)) continue;
        const float w = widthOf(src);
        out.push_back({src, x, w});
        x += w;
        if (album && out.size() == 1) x += kGutter;
    }
    return out;
}

float ListView::ContentWidth() const {
    const auto cols = Columns(0);
    return cols.empty() ? 0.0f : cols.back().x + cols.back().w;
}

void ListView::ScrollXTo(float x) {
    const float max = std::max(0.0f, ContentWidth() + 4 - RowsWidth());
    x = std::clamp(std::round(x * m_scale) / m_scale, 0.0f, max);
    if (x == m_scrollX) return;
    m_scrollX = x;
    PaintHeader();
    PaintRows();
}

int ListView::CellAt(POINT pt, size_t* srcColumn) const {
    if (!m_source || m_mode == Mode::Tiles) return static_cast<int>(CellKind::Text);
    const float x = ContentPoint(pt).x;
    const auto& srcCols = m_source->Columns();
    for (const Col& c : Columns(0))
        if (x >= c.x && x < c.x + c.w) {
            if (srcColumn) *srcColumn = c.src;
            return static_cast<int>(KindOf(srcCols[c.src]));
        }
    return static_cast<int>(CellKind::Text);
}

int ListView::StarAt(POINT pt) const {
    size_t src = SIZE_MAX;
    if (CellAt(pt, &src) != static_cast<int>(CellKind::Rating)) return -1;
    const float x = ContentPoint(pt).x;
    for (const Col& c : Columns(0))
        if (c.src == src) {
            const float rel = x - (c.x + kCellPad);
            return rel >= 0 && rel < 5 * kStar ? static_cast<int>(rel / kStar) : -1;
        }
    return -1;
}

D2D1_POINT_2F ListView::ContentPoint(POINT pt) const {
    const RECT rr = RowsRect();
    return {(pt.x - rr.left) / m_scale + ScrollX(), (pt.y - rr.top) / m_scale + ContentTopDip()};
}

float ListView::ContentTopDip() const {
    return LineTopDip(m_top) + m_topOffsetDip;
}

void ListView::ScrollTopTo(float top) {
    top = std::max(top, 0.0f);
    const size_t n = LineCount();
    if (n == 0) {
        m_top = 0;
        m_topOffsetDip = 0;
        return;
    }
    if (Lined() && !m_lineTop.empty()) {
        const auto it = std::upper_bound(m_lineTop.begin(), m_lineTop.end(), top);
        const size_t line = it == m_lineTop.begin() ? 0 : static_cast<size_t>(it - m_lineTop.begin() - 1);
        m_top = std::min(line, n - 1);
        m_topOffsetDip = std::max(top - LineTopDip(m_top), 0.0f);
        return;
    }
    const float lineH = std::max(LineHeightPx() / m_scale, 0.001f);
    const size_t line = top > 0 ? static_cast<size_t>(top / lineH) : 0;
    m_top = std::min(line, n - 1);
    m_topOffsetDip = std::max(top - LineTopDip(m_top), 0.0f);
}

float ListView::ContentHeightDip() const {
    if (Lined()) return m_lineTop.empty() ? 0.0f : m_lineTop.back();
    return static_cast<float>(LineCount()) * (LineHeightPx() / m_scale);
}

float ListView::MaxScrollDip() const { return std::max(0.0f, ContentHeightDip() - RowsHeightDip()); }

void ListView::SetScrollDip(float top) {
    m_glide.ms = 0;
    top = std::round(std::clamp(top, 0.0f, MaxScrollDip()) * m_scale) / m_scale;
    if (std::abs(top - ContentTopDip()) < 0.01f) return;
    ScrollTopTo(top);
    PaintRows();
    PaintScrollBar();
    CheckNearEnd();
}

namespace {
// The glide's curve: ease-out cubic, cubic-bezier(0.33, 1, 0.68, 1), as browsers scroll.
constexpr gfx::Curve kGlideCurve{0.33f, 1.0f, 0.68f, 1.0f};
constexpr int kGlideMs = 220;
float GlideAt(float t) { return gfx::Ease(kGlideCurve, t); }
} // namespace

bool ListView::Gliding() const {
    return m_glide.ms > 0 &&
           std::chrono::steady_clock::now() - m_glide.start < std::chrono::milliseconds(m_glide.ms);
}

float ListView::VisualTopDip() const {
    if (!Gliding()) return ContentTopDip();
    const float t = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - m_glide.start).count() /
                    static_cast<float>(m_glide.ms);
    return m_glide.from + (m_glide.to - m_glide.from) * GlideAt(std::clamp(t, 0.0f, 1.0f));
}

void ListView::GlideTo(float top) {
    top = std::round(std::clamp(top, 0.0f, MaxScrollDip()) * m_scale) / m_scale;
    if (std::abs(top - ContentTopDip()) < 0.01f) return;
    BOOL animations = TRUE;
    SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animations, 0);
    if (!animations) {
        SetScrollDip(top);
        return;
    }
    const float viewH = RowsHeightDip();
    float from = std::round(VisualTopDip() * m_scale) / m_scale;
    // A long way glides only its last screen and a half (the rows painted for it stay bounded).
    const float run = 1.5f * viewH;
    const bool jump = std::abs(top - from) > run;
    if (jump) from = top - std::copysign(run, top - from);
    ScrollTopTo(top);
    // The rows for the whole way, from where the content stands to where it goes, with a margin
    // either side: `from` is this thread's estimate of where the running glide is, and the
    // compositor (which started it a draw and a commit later) can be a little behind it.
    const float margin = 0.25f * viewH;
    m_glide = {from, top, std::chrono::steady_clock::now(), kGlideMs};
    m_paintTop = std::max(0.0f, std::min(from, top) - margin);
    m_paintHeight = std::max(from, top) + viewH + margin - m_paintTop;
    PaintRows();
    // Content coordinates are absolute (the offset is minus the scroll position, whatever range
    // is painted), so the glide goes on from where the compositor shows the content now, not
    // from an estimate: started from the estimate, the content jumped ahead at every wheel
    // notch of a running glide (owner, 2026-11-21: scrolling up and down, content flashed over
    // the home cards). A long jump starts from its last screen and a half.
    const auto& comp = gfx::CompositionHost::Get().Compositor();
    auto glide = comp.CreateVector3KeyFrameAnimation();
    if (jump)
        glide.InsertKeyFrame(0.0f, float3{0, -std::round(from * m_scale), 0});
    else
        glide.InsertExpressionKeyFrame(0.0f, L"this.StartingValue");
    glide.InsertKeyFrame(1.0f, float3{0, -std::round(top * m_scale), 0},
                         gfx::Easing(kGlideCurve));
    glide.Duration(std::chrono::milliseconds(kGlideMs));
    m_content.StartAnimation(L"Offset", glide);
    PaintScrollBar();
    CheckNearEnd();
}

void ListView::UpdateStatusText() {
    std::wstring next = m_source ? m_source->LoadingText() : std::wstring();
    if (next == m_statusText) return;
    // The band takes its own room above the rows and pushes them down; the scroll position
    // stays, so it never hides the first row (scrolling to make room hid it on every load).
    m_statusText = std::move(next);
}

// ---- Data -----------------------------------------------------------------------------------

void ListView::LoadColumnLayout() {
    const auto& cols = m_source->Columns();
    std::wstring key;
    if (!m_source->ColumnSetKey().empty()) key = m_source->ColumnSetKey() + L":";
    for (const auto& c : cols) key += c.title + L'|';
    if (AlbumColumn()) key += L"#art";
    if (key == m_columnKey && m_order.size() == cols.size()) return;
    m_columnKey = key;
    m_order.resize(cols.size());
    for (size_t i = 0; i < cols.size(); ++i) m_order[i] = i;
    m_widths.assign(cols.size(), -1.0f);
    m_hidden.resize(cols.size());
    for (size_t i = 0; i < cols.size(); ++i) m_hidden[i] = cols[i].hidden ? 1 : 0;
    auto it = Store().find(key);
    if (it == Store().end()) return;
    const SavedLayout& s = it->second;
    std::vector<size_t> sorted = s.order;
    std::sort(sorted.begin(), sorted.end());
    bool valid = sorted.size() == cols.size();
    for (size_t i = 0; valid && i < sorted.size(); ++i) valid = sorted[i] == i;
    if (valid) m_order = s.order;
    if (s.widths.size() == cols.size()) m_widths = s.widths;
    if (s.hidden.size() == cols.size()) m_hidden = s.hidden;
}

void ListView::SaveColumnLayout() const {
    if (m_columnKey.empty()) return;
    Store()[m_columnKey] = {m_order, m_widths, m_hidden};
    Persist();
}

void ListView::BuildLines() {
    m_lines.clear();
    m_lineTop.clear();
    m_rowLine.clear();
    m_groupLine.clear();
    m_mode = Mode::Plain;
    m_groupArt = false;
    m_tileGroups = false;
    if (!m_source) return;
    if (m_source->Tiles() != media::TileStyle::None) {
        m_mode = Mode::Tiles;
        if (const auto* groups = m_source->Groups(); groups && !groups->empty()) {
            m_tileGroups = true;
            m_rowLine.assign(m_source->Count(), 0);
            for (size_t gi = 0; gi < groups->size(); ++gi) {
                const auto& g = (*groups)[gi];
                const size_t cols = TileColumnsOf(GroupTiles(gi));
                m_groupLine.push_back(m_lines.size());
                m_lines.push_back({Line::GroupHeader, SIZE_MAX, gi, 0});
                if (g.carousel) {  // a shelf: its rows, the tiles column by column across them
                    const size_t rows = std::max<size_t>(g.tileLines, 1);
                    const size_t first = m_lines.size();
                    for (size_t k = 0; k < std::min(rows, g.count); ++k) m_lines.push_back({Line::Item, g.first, gi, k});
                    for (size_t t = g.first; t < g.first + g.count && t < m_rowLine.size(); ++t)
                        m_rowLine[t] = first + (t - g.first) % rows;
                    continue;
                }
                const size_t end = TileGroupEnd(gi);
                for (size_t r = g.first, k = 0; r < end; r += cols, ++k) {
                    m_lines.push_back({Line::Item, r, gi, k});
                    for (size_t t = r; t < std::min(end, r + cols); ++t) m_rowLine[t] = m_lines.size() - 1;
                }
                // Rows past the shown ones belong to the group's last line (for scrolling to them).
                for (size_t t = end; t < g.first + g.count && t < m_rowLine.size(); ++t)
                    m_rowLine[t] = m_lines.size() - 1;
            }
        }
    } else if (const auto* groups = m_source->Groups(); groups && !groups->empty()) {
        m_mode = Mode::Grouped;
        m_groupArt = m_source->GroupArt();
        m_rowLine.assign(m_source->Count(), 0);
        for (size_t gi = 0; gi < groups->size(); ++gi) {
            const auto& g = (*groups)[gi];
            m_groupLine.push_back(m_lines.size());
            m_lines.push_back({Line::GroupHeader, SIZE_MAX, gi, 0});
            // With the album column a group is at least as tall as its cover.
            const size_t lines = m_groupArt ? std::max(g.count, kGroupMinLines) : g.count;
            for (size_t k = 0; k < lines; ++k) {
                if (k < g.count) {
                    m_rowLine[g.first + k] = m_lines.size();
                    m_lines.push_back({Line::Item, g.first + k, gi, k});
                } else {
                    m_lines.push_back({Line::Filler, SIZE_MAX, gi, k});
                }
            }
        }
    }
    LoadColumnLayout();
    // The album column stays first and visible.
    if (AlbumColumn() && !m_order.empty() && m_order[0] != 0) {
        m_order.erase(std::find(m_order.begin(), m_order.end(), size_t{0}));
        m_order.insert(m_order.begin(), 0);
    }
    if (AlbumColumn() && !m_hidden.empty()) m_hidden[0] = 0;
    m_lineTop.clear();
    if (Lined()) {
        m_lineTop.reserve(m_lines.size() + 1);
        float y = 0;
        for (const Line& line : m_lines) {
            m_lineTop.push_back(y);
            y += LineHeightDip(line);
        }
        m_lineTop.push_back(y);
    }
}

// The header's `more` link: while the group hides rows or has more to load, and to collapse it.
bool ListView::ShowsMore(size_t group) const {
    const auto* groups = m_source ? m_source->Groups() : nullptr;
    if (!groups || group >= groups->size()) return false;
    const auto& g = (*groups)[group];
    return !g.more.empty() &&
           (g.carousel || g.tileLines == 0 || g.moreBeyond || TileGroupEnd(group) < g.first + g.count);
}

size_t ListView::TileGroupEnd(size_t group) const {
    const auto* groups = m_source ? m_source->Groups() : nullptr;
    if (!groups || group >= groups->size()) return 0;
    const auto& g = (*groups)[group];
    if (g.carousel) return std::min(g.first + g.count, m_source->Count());
    const size_t shown = g.tileLines ? std::min(g.count, g.tileLines * TileColumnsOf(GroupTiles(group))) : g.count;
    return std::min(g.first + shown, m_source->Count());
}

void ListView::SetSource(media::ListSource* source, bool resetScroll) {
    if (source != m_source) m_cards->Clear();
    m_cardPress = SIZE_MAX;
    m_source = source;
    if (m_source && m_source->CanGroup()) m_source->SetGrouping(cfg_showGroups.get() != 0);
    m_hot = SIZE_MAX;
    m_hotGroup = SIZE_MAX;
    m_hotGroupHeader = SIZE_MAX;
    m_hotLinkGroup = SIZE_MAX;
    m_hotShelf = SIZE_MAX;
    m_moreGroup = m_moreOldGroup = m_morePressedGroup = m_shelfPressedGroup = SIZE_MAX;
    m_moreHot = m_moreOldHot = m_morePress = 0;
    m_playRow = m_playOldRow = SIZE_MAX;
    m_playBlend = m_playOldBlend = m_playHotBlend = 0;
    m_playHot = false;
    m_anchor = source ? source->Focus() : SIZE_MAX;
    if (!m_slideKey.empty()) {
        m_slideKey.clear();
        m_slideClip.IsVisible(false);
    }
    if (resetScroll) {
        m_shelves.clear();
        m_top = 0;
        m_topOffsetDip = 0;
        m_statusText = m_source ? m_source->LoadingText() : std::wstring();
        m_scrollX = 0;
    }
    BuildLines();
    PaintHeader();
    Refresh();
    if (onSourceChanged) onSourceChanged();
}

void ListView::SetBounds(const RECT& px, float scale) {
    const bool scaleChanged = scale != m_scale;
    const bool sizeChanged = (px.right - px.left) != (m_bounds.right - m_bounds.left) ||
                             (px.bottom - px.top) != (m_bounds.bottom - m_bounds.top);
    m_bounds = px;
    m_scale = scale;
    const float w = static_cast<float>(px.right - px.left), h = static_cast<float>(px.bottom - px.top);
    m_root.Offset(float3{static_cast<float>(px.left), static_cast<float>(px.top), 0});
    m_root.Size(float2{w, h});
    m_clipGeometry.Size(float2{std::max(w, 0.0f), std::max(h, 0.0f)});
    if (sizeChanged || scaleChanged) {
        PaintHeader();
        Refresh();
    }
}

void ListView::SetFocused(bool focused) {
    if (m_focused == focused) return;
    m_focused = focused;
    PaintRows();
}

void ListView::Refresh() {
    UpdateStatusText();
    BuildLines();  // rows, groups or the view style may have changed
    if (ContentTopDip() > MaxScrollDip()) ScrollTopTo(MaxScrollDip());
    m_scrollX = std::clamp(m_scrollX, 0.0f, std::max(0.0f, ContentWidth() + 4 - RowsWidth()));
    if (m_source && m_hot != SIZE_MAX && m_hot >= m_source->Count()) m_hot = SIZE_MAX;
    m_hotGroup = SIZE_MAX;
    m_hotGroupHeader = SIZE_MAX;
    m_hotLinkGroup = SIZE_MAX;
    PaintStatus();
    PaintHeader();
    PaintRows();
    PaintScrollBar();
    CheckNearEnd();
}

void ListView::ScrollTo(size_t top) {
    top = std::min(top, MaxTop());
    if (top == m_top && m_topOffsetDip == 0) return;
    m_top = top;
    m_topOffsetDip = 0;
    PaintRows();
    PaintScrollBar();
    CheckNearEnd();
}

void ListView::CheckNearEnd() {
    if (!onNearEnd || !m_source || m_source->Count() == 0) return;
    const size_t lines = LineCount();
    if (lines == 0) return;
    if (m_top + VisibleLines() + 3 >= lines) onNearEnd();
}

void ListView::EnsureVisible(size_t row) {
    if (!m_source || row >= m_source->Count()) return;
    const size_t rowLine = LineOfRow(row);
    size_t line = rowLine;
    // Bring a group's header into view together with its first track.
    if (m_mode == Mode::Grouped && rowLine > 0 && rowLine < m_lines.size() &&
        m_lines[rowLine - 1].kind == Line::GroupHeader)
        line = rowLine - 1;
    if (Lined() && !m_lineTop.empty()) {
        const float viewH = RowsHeightDip();
        const float top = ContentTopDip();
        // Visibility is decided by the row itself, not by the group header: a header at the
        // bottom edge does not mean the track below it is visible.
        const float rowTop = LineTopDip(rowLine);
        const float rowBottom = LineTopDip(rowLine + 1);
        if (rowTop >= top && rowBottom <= top + viewH) return;
        if (rowTop < top) {
            ScrollTo(line);
            return;
        }
        // Scroll the smallest amount that puts the row's bottom at the view's bottom.
        auto it = std::lower_bound(m_lineTop.begin(), m_lineTop.end(), rowBottom - viewH);
        const size_t target = it == m_lineTop.end() ? m_lines.size() : static_cast<size_t>(it - m_lineTop.begin());
        ScrollTo(std::min(target, rowLine));
        return;
    }
    const size_t v = std::max<size_t>(VisibleLines(), 1);
    const size_t last = LineOfRow(row);
    if (line < m_top)
        ScrollTo(line);
    else if (last >= m_top + v)
        ScrollTo(last - v + 1);
}

size_t ListView::TopRow() const {
    if (!m_source || m_source->Count() == 0) return SIZE_MAX;
    switch (m_mode) {
    case Mode::Tiles:
        if (!m_tileGroups) return std::min(m_top * TileColumns(), m_source->Count() - 1);
        [[fallthrough]];
    case Mode::Grouped:
        for (size_t line = m_top; line < m_lines.size(); ++line)
            if (m_lines[line].kind == Line::Item && m_lines[line].row < m_source->Count())
                return m_lines[line].row;
        return SIZE_MAX;
    default: return m_top < m_source->Count() ? m_top : SIZE_MAX;
    }
}

void ListView::ScrollToRow(size_t row) {
    if (!m_source || row >= m_source->Count()) return;
    ScrollTo(LineOfRow(row));
}

void ListView::ScrollToTop() {
    ScrollTopTo(0);
    PaintRows();
    PaintScrollBar();
    CheckNearEnd();
}

float ListView::RowOffset(size_t row) const {
    if (!m_source || row >= m_source->Count()) return 0;
    return LineTopDip(LineOfRow(row)) - ContentTopDip();
}

void ListView::ScrollToRow(size_t row, float offsetDip) {
    if (!m_source || row >= m_source->Count()) return;
    const float top = std::min(LineTopDip(LineOfRow(row)) - offsetDip, LineTopDip(MaxTop()));
    ScrollTopTo(top);
    PaintRows();
    PaintScrollBar();
    CheckNearEnd();
}

// ---- Painting -------------------------------------------------------------------------------

void ListView::PaintStatus() {
    const int h = StatusHeightPx();
    if (h <= 0) {
        m_status.IsVisible(false);
        return;
    }
    m_status.IsVisible(true);
    auto& host = gfx::CompositionHost::Get();
    const int w = std::max<int>(m_bounds.right - m_bounds.left, 1);
    const float s = m_scale;
    host.Repaint(m_status, w, h, s, [&](ID2D1DeviceContext* dc) {
        const D2D1_SIZE_F size{w / s, h / s};
        wp::HeaderBackground(dc, size);
        Text(dc, m_statusText, {kCellPad, 0, size.width - kCellPad, size.height}, kSecondaryInk,
             DWRITE_TEXT_ALIGNMENT_CENTER);
    });
    PlaceVisual(m_status, 0, static_cast<float>(HeaderHeightPx()), static_cast<float>(w),
                static_cast<float>(h));
}

void ListView::PaintHeader() {
    if (m_mode == Mode::Tiles) {  // icon views have no column header
        m_header.IsVisible(false);
        return;
    }
    m_header.IsVisible(true);
    auto& host = gfx::CompositionHost::Get();
    const int w = std::max<int>(m_bounds.right - m_bounds.left, 1), h = HeaderHeightPx();
    const float s = m_scale;
    host.Repaint(m_header, w, h, s, [&](ID2D1DeviceContext* dc) {
        const D2D1_SIZE_F size{w / s, h / s};
        // WMP 12 header cells (wmploc LISTVIEWHEADER: normal / hot / pressed, 23 px each);
        // drawn per cell, so the cell edges form the separators.
        static const std::wstring kHeader = L"LISTVIEWHEADER.PNG";
        const bool art = win7::Resources::Get().Has(win7::Module::Wmploc, kHeader);
        auto cell = [&](float x0, float x1, int state) {
            return win7::DrawNineGrid(dc, win7::Module::Wmploc, kHeader, {0, 23.0f * state, 6, 23.0f * state + 23},
                                      {1, 1, 1, 1}, {x0, 0, x1, size.height});
        };
        if (!art || !cell(0, size.width, 0)) wp::HeaderBackground(dc, size);
        if (!m_source) return;
        const auto& srcCols = m_source->Columns();
        const auto cols = Columns(RowsWidth());
        // Cells in content coordinates, shifted by the horizontal scroll.
        D2D1_MATRIX_3X2_F base;
        dc->GetTransform(&base);
        dc->SetTransform(D2D1::Matrix3x2F::Translation(-ScrollX(), 0) * base);
        const float dragX = (m_dragX - m_bounds.left) / s + ScrollX();
        // One header cell (background, title, sort chevron) with its left edge at `x`.
        auto drawCell = [&](const Col& c, float x, int state) {
            if (art) cell(x, x + c.w, state);
            else if (state) wp::RowPlate(dc, {x, 1, x + c.w, size.height - 1}, wp::RowState::Hot, true);
            const auto& col = srcCols[c.src];
            if (KindOf(col) != CellKind::Art)  // the cover column has no title, only covers
                Text(dc, Tr(col.title), {x + kCellPad, 0, x + c.w - kCellPad, size.height}, 0x4C607A,
                     col.rightAlign ? DWRITE_TEXT_ALIGNMENT_TRAILING : DWRITE_TEXT_ALIGNMENT_LEADING);
            if (!art) wp::HeaderSeparator(dc, x + c.w, size.height);
            if (c.src == m_source->SortColumn()) {  // sort chevron at the top center, as in Win7
                winrt::com_ptr<ID2D1Factory> factory;
                dc->GetFactory(factory.put());
                winrt::com_ptr<ID2D1PathGeometry> tri;
                winrt::com_ptr<ID2D1GeometrySink> sink;
                factory->CreatePathGeometry(tri.put());
                tri->Open(sink.put());
                const float cx = std::round((x + c.w / 2) * s) / s, top = 1.5f, bottom = 5.0f;
                const bool down = m_source->SortDescending();
                sink->BeginFigure({cx - 3.5f, down ? top : bottom}, D2D1_FIGURE_BEGIN_FILLED);
                sink->AddLine({cx + 3.5f, down ? top : bottom});
                sink->AddLine({cx, down ? bottom : top});
                sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                sink->Close();
                dc->FillGeometry(tri.get(), Brush(dc, 0x7A96B8).get());
            }
        };
        // Pressed (and dragged) cells use the sunken state, the hovered one the hot state;
        // both fill the whole cell like WMP's.
        for (size_t d = 0; d < cols.size(); ++d) {
            const bool pressed = (m_drag == Drag::PendingMove || m_drag == Drag::Move) && d == m_dragCol;
            drawCell(cols[d], cols[d].x, pressed ? 2 : (d == m_hotHeader && m_drag == Drag::None) ? 1 : 0);
        }
        if (m_drag == Drag::Move && m_dragCol < cols.size()) {
            // Insertion mark where the column would land.
            float mark = cols.empty() ? kGutter : cols.back().x + cols.back().w;
            for (const Col& c : cols)
                if (dragX < c.x + c.w / 2) {
                    mark = c.x;
                    break;
                }
            const float line = gfx::Hairline(s);
            dc->FillRectangle({mark - line, 0, mark + line, size.height}, Brush(dc, 0x3C7FB1).get());
            // A translucent copy of the dragged header follows the cursor, as in WMP.
            const Col& c = cols[m_dragCol];
            const float x = std::round((c.x + (m_dragX - m_dragStartX) / s) * s) / s;
            winrt::com_ptr<ID2D1Layer> layer;
            dc->CreateLayer(nullptr, layer.put());
            dc->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                                D2D1::IdentityMatrix(), 0.6f),
                          layer.get());
            drawCell(c, x, 0);
            dc->PopLayer();
        }
        dc->SetTransform(base);
    });
    PlaceVisual(m_header, 0, 0, static_cast<float>(w), static_cast<float>(h));
}

void ListView::PaintRows() {
    auto& host = gfx::CompositionHost::Get();
    if (!m_slideKey.empty() && ContentTopDip() != m_slideTop) { // scrolled meanwhile: the slide ends
        m_slideKey.clear();
        m_slide.StopAnimation(L"Offset");
        m_slideClip.IsVisible(false);
    }
    const RECT rr = RowsRect();
    const int w = std::max<int>(rr.right - rr.left, 1), viewPx = std::max<int>(rr.bottom - rr.top, 1);
    PlaceVisual(m_viewport, 0, static_cast<float>(ContentOffsetPx()), static_cast<float>(w),
                static_cast<float>(viewPx));
    // A jump elsewhere (a row brought into view, the thumb, new rows) ends a glide.
    if (m_glide.ms && (!Gliding() || std::abs(ContentTopDip() - m_glide.to) > 0.01f)) {
        m_glide.ms = 0;
        m_content.StopAnimation(L"Offset");
    }
    if (!Gliding()) {
        // The glide this thread counts as over may still run its last frame in the compositor
        // (or a jump left it): it stops here, at the place it was going to.
        m_glide.ms = 0;
        m_content.StopAnimation(L"Offset");
        m_paintTop = ContentTopDip();
        m_paintHeight = viewPx / m_scale;
        m_content.Offset(float3{0, -std::round(m_paintTop * m_scale), 0});
    }
    const int h = std::max(1, static_cast<int>(std::ceil(m_paintHeight * m_scale)));
    // Painted as if the view stood at the painted range's top and were as tall.
    const size_t realTop = m_top;
    const float realOffset = m_topOffsetDip;
    ScrollTopTo(m_paintTop);
    m_paintViewH = h / m_scale;
    // Two surfaces drawn in turn: the one on screen is never drawn into. Drawn in place, its new
    // rows (laid out for a new glide's start) could reach the screen a frame before the glide's
    // new offset, the content standing a screen off for that frame (owner, 2026-11-21: what is
    // further down flashed up over the home cards while scrolling up and down). The brush swap,
    // like the offset, is a property: both land in the same frame. A surface only grows (in
    // half views) while the view keeps its size: sized to each glide, every wheel step made one.
    RowsBuffer& back = m_rowsBuffers[m_rowsFront ^ 1];
    if (!back.surface || back.size.cx != w || back.size.cy < h || back.viewPx != viewPx) {
        const int half = std::max(1, viewPx / 2);
        const int surfaceH = (h + half - 1) / half * half;
        back.surface = host.CreateSurface(w, surfaceH);
        back.size = {w, surfaceH};
        back.viewPx = viewPx;
        back.brush = host.Compositor().CreateSurfaceBrush(back.surface);
        back.brush.Stretch(wuc::CompositionStretch::None);
        back.brush.HorizontalAlignmentRatio(0);
        back.brush.VerticalAlignmentRatio(0);
    }
    // The painted range stands at its own place in the (absolute) content.
    const float paintPx = std::round(m_paintTop * m_scale);
    PlaceVisual(m_rows, 0, paintPx, static_cast<float>(w), static_cast<float>(h));
    m_cards->SetBounds({0, static_cast<LONG>(paintPx), w, static_cast<LONG>(paintPx) + h}, m_scale);
    const float s = m_scale;
    m_glowSeen = false;
    const bool drawn = host.Draw(back.surface, s, [&](ID2D1DeviceContext* dc) {
        const D2D1_SIZE_F size{w / s, h / s};
        if (!m_source) return;
        if (m_source->Count() == 0) {
            Text(dc, Tr(m_source->EmptyText()), {20, 20, size.width - 20, 60}, kGreyInk, DWRITE_TEXT_ALIGNMENT_CENTER);
            return;
        }
        D2D1_MATRIX_3X2_F base;
        dc->GetTransform(&base);
        dc->SetTransform(D2D1::Matrix3x2F::Translation(-ScrollX(), 0) * base);
        switch (m_mode) {
        case Mode::Grouped: PaintGrouped(dc, size); break;
        case Mode::Tiles: PaintTiles(dc); break;
        default: PaintPlain(dc, size); break;
        }
        if (m_mode != Mode::Tiles) {
            // WMP 12 column lines, down to the bottom of the view (owner capture at 150 %): on
            // each column's last pixel a translucent blue (#75B5F4 at 15 %), a faint white
            // line just left of it.
            const float line = gfx::Hairline(s);
            auto blue = Brush(dc, 0x75B5F4, 0.15f), white = Brush(dc, 0xFFFFFF, 0.2f);
            for (const Col& c : Columns(size.width)) {
                const float x = std::round((c.x + c.w) * s) / s;
                dc->FillRectangle({x - line, 0, x, size.height}, blue.get());
                dc->FillRectangle({x - 2 * line, 0, x - line, size.height}, white.get());
            }
        }
        dc->SetTransform(base);
        if (m_drag == Drag::Marquee) {
            // Win7 rubber band: #3399FF border over a translucent fill.
            const float top = ContentTopDip();
            const float px = 1.0f / s;
            auto snap = [&](float v) { return std::round(v * s) / s; };
            const D2D1_RECT_F r{snap(std::min(m_marqueeFrom.x, m_marqueeTo.x) - ScrollX()),
                                snap(std::min(m_marqueeFrom.y, m_marqueeTo.y) - top),
                                snap(std::max(m_marqueeFrom.x, m_marqueeTo.x) - ScrollX()),
                                snap(std::max(m_marqueeFrom.y, m_marqueeTo.y) - top)};
            dc->FillRectangle(r, Brush(dc, 0x3399FF, 0.25f).get());
            dc->DrawRectangle({r.left + px / 2, r.top + px / 2, r.right - px / 2, r.bottom - px / 2},
                              Brush(dc, 0x3399FF).get(), px);
        }
    });
    if (drawn) { // shown with the offset set in this same message
        m_rows.Brush(back.brush);
        m_rowsFront ^= 1;
    }
    NoteGlow();
    LayoutCards();
    m_paintViewH = -1;
    m_top = realTop;
    m_topOffsetDip = realOffset;
    PlaceThumb();
}

void ListView::LayoutCards() {
    const size_t slideGroup = std::exchange(m_cardSlideGroup, SIZE_MAX);
    std::vector<CardLayer::Shelf> shelves;
    if (m_source && m_mode == Mode::Tiles && m_tileGroups && m_source->Groups()) {
        const auto& groups = *m_source->Groups();
        const float top0 = ContentTopDip(), viewH = RowsHeightDip();
        constexpr auto style = media::TileStyle::Card;
        const float pitch = TilePitchOf(style, true);
        for (size_t gi = 0; gi < groups.size() && gi < m_groupLine.size(); ++gi) {
            const auto& g = groups[gi];
            if (GroupTiles(gi) != style || !g.carousel || !g.count) continue;
            const size_t header = m_groupLine[gi];
            if (header + 1 >= m_lineTop.size()) continue;
            const float top = m_lineTop[header + 1] - top0;
            if (top + kCardTile.height < 0 || top > viewH) continue;
            // The whole shelf as one strip, scrolled to its page.
            CardLayer::Shelf shelf;
            shelf.key = g.key;
            shelf.top = top + kCardInset.y;
            shelf.scroll = ShelfPos(gi) * pitch;
            if (gi == slideGroup) shelf.slideFrom = m_cardSlideFrom * pitch;
            for (size_t c = 0; c < g.count; ++c)
                shelf.cards.emplace_back(g.first + c, TileLeftOf(style, static_cast<float>(c)) + kCardInset.x);
            shelves.push_back(std::move(shelf));
        }
    }
    if (m_source) m_cards->Layout(*m_source, shelves, slideGroup != SIZE_MAX ? m_cardSlideMs : 0);
}

void ListView::SetMotion(bool on) { m_cards->SetMotion(on); }

// The glow over a running bar starts the animation; it stops once none is painted.
void ListView::NoteGlow() {
    const bool was = m_glowing;
    m_glowing = m_glowSeen;
    if (m_glowing && !was && onScrollAnim) onScrollAnim();
}

void ListView::PaintCells(ID2D1DeviceContext* dc, size_t row, float y, float rowH, const std::vector<Col>& cols) {
    const bool isPlaying = m_source->IsPlaying(row);
    const auto& srcCols = m_source->Columns();
    const bool skipAlbum = AlbumColumn();
    if (isPlaying) {
        // In the gutter: before the first column, or between the album column and the tracks;
        // the same distance from the row highlight's left edge in both layouts.
        const float x = skipAlbum && !cols.empty() ? cols[0].x + cols[0].w + kGutter / 2 - 1 : kGutter / 2 + 1;
        wp::PlayingMarker(dc, {x, y + rowH / 2 + 0.5f}, playback_control::get()->is_paused());
    }
    for (const Col& c : cols) {
        if (skipAlbum && c.src == 0) continue;
        const auto& col = srcCols[c.src];
        const CellKind kind = KindOf(col);
        if (kind == CellKind::Rating) {
            // WMP stars (STARS.PNG 13x16 cells: 0-4 empty, 5-9 gold, 15-19 hover gold,
            // 25-29 white on a selected row); hovering previews the rating under the mouse.
            const int rating = _wtoi(m_source->Cell(row, c.src).c_str());
            const bool hover = row == m_hot && m_hotStar >= 0, sel = m_source->IsSelected(row);
            const float sy = std::round((y + (rowH - 16) / 2) * m_scale) / m_scale;
            bool drawn = true;
            for (int i = 0; i < 5 && drawn; ++i) {
                const UINT32 cell = hover ? (i <= m_hotStar ? 15 + i : i) : (i < rating ? (sel ? 25 : 5) + i : i);
                const float sx = std::round((c.x + kCellPad + i * kStar) * m_scale) / m_scale;
                drawn = win7::DrawPartTo(dc, win7::Module::Wmploc, L"STARS.PNG",
                                         D2D1::RectU(cell * 13, 0, cell * 13 + 13, 16), {sx, sy, sx + kStar, sy + 16});
            }
            if (drawn) continue;
        } else if (kind == CellKind::Art) {
            // The song's front cover, as tall as the row allows, centred in the column.
            const float side = rowH - 4, ax = std::round((c.x + (c.w - side) / 2) * m_scale) / m_scale;
            if (const metadb_handle_ptr item = m_source->Item(row); item.is_valid()) {
                const metadb_handle_ptr art = m_source->ArtTrack(row);
                DrawArt(dc, m_source->ArtKey(row), art.is_valid() ? art : item, {ax, y + 2, ax + side, y + 2 + side});
            }
            continue;
        } else if (kind == CellKind::Favorite) {
            // No heart in WMP's art: a gold star (5530) when the song is in Favorites, its
            // outline (5531) on the hovered row, else the empty one (5533).
            const bool fav = m_source->IsFavorite(row);
            const int id = fav ? 5530 : row == m_hot ? 5531 : 5533;
            const float fx = std::round((c.x + (c.w - 14) / 2) * m_scale) / m_scale;
            const float fy = std::round((y + (rowH - 14) / 2) * m_scale) / m_scale;
            if (win7::Draw(dc, win7::Module::Wmploc, id, {fx, fy})) continue;
            if (fav) Text(dc, L"★", {c.x + kCellPad, y + 0.5f, c.x + c.w - kCellPad, y + 0.5f + rowH}, 0xD4A017,
                          DWRITE_TEXT_ALIGNMENT_CENTER);
            continue;
        } else if (kind == CellKind::Popularity) {
            // NetEase's 热度: a short rounded bar, the score's share of the column, on a
            // track in the list's group-rule tint. No bar for an unscored song.
            const int score = std::clamp(_wtoi(m_source->Cell(row, c.src).c_str()), 0, 100);
            if (score == 0) continue;
            auto snap = [&](float v) { return gfx::Snap(v, m_scale); };
            const float left = snap(c.x + kCellPad), right = snap(c.x + c.w - kCellPad);
            const float h = 4, top = snap(y + (rowH - h) / 2);
            if (right - left < h * 2) continue;
            const float fill = std::max(h, snap((right - left) * score / 100.0f));
            dc->FillRoundedRectangle({{left, top, right, top + h}, h / 2, h / 2}, Brush(dc, kGroupRule).get());
            dc->FillRoundedRectangle({{left, top, left + fill, top + h}, h / 2, h / 2},
                                     Brush(dc, isPlaying ? kPlayingInk : 0x3C7FB1).get());
            continue;
        } else if (kind == CellKind::Download) {
            // Downloaded: the mark always; downloading: its ring; else the offer on the hovered row.
            const media::DownloadCell cell = m_source->Download(row);
            const D2D1_POINT_2F centre{std::round((c.x + c.w / 2) * m_scale) / m_scale,
                                       std::round((y + rowH / 2) * m_scale) / m_scale};
            switch (cell.look) {
            case media::DownloadCell::Look::None: break;
            case media::DownloadCell::Look::Offer:
                if (row == m_hot) DownloadMark(dc, centre, true, m_downloadHot, 1.0f); // at once with the row
                break;
            case media::DownloadCell::Look::Done:
                DownloadMark(dc, centre, false, row == m_hot ? m_downloadHot : 0.0f, 1.0f);
                break;
            default: ProgressRing(dc, centre, cell.progress, cell.look); break;
            }
            continue;
        } else if (kind == CellKind::Progress) {
            // Windows 7's progress bar at its own 14 DIP height: the grey trough, then the
            // fill's share of it, green (yellow paused, red failed), cut off at the share. A
            // running one ('r') has Windows 7's glow (aero 664, the move overlay) sweeping from
            // the bar's start to the fill's end: 1.7 s across, 0.7 s rest.
            const std::wstring cell = m_source->Cell(row, c.src);
            if (cell.empty()) continue;
            const float share = std::clamp(static_cast<float>(_wtoi(cell.c_str())) / 1000.0f, 0.0f, 1.0f);
            const wchar_t state = cell.back();
            auto snap = [&](float v) { return gfx::Snap(v, m_scale); };
            const float left = snap(c.x + kCellPad), right = snap(c.x + c.w - kCellPad);
            const float h = 14, top = snap(y + (rowH - h) / 2);
            if (right - left < 20) continue;
            const D2D1_RECT_F bar{left, top, right, top + h};
            win7::DrawNineGrid(dc, win7::Module::Aero, 648, {0, 0, 19, 14}, {3, 3, 3, 3}, bar);
            const float fill = snap((right - left) * share);
            if (fill > 0) {
                const int id = state == L'e' ? 657 : state == L'p' ? 658 : 656;
                dc->PushAxisAlignedClip({left, top, left + fill, top + h}, D2D1_ANTIALIAS_MODE_ALIASED);
                win7::DrawNineGrid(dc, win7::Module::Aero, id, {0, 0, 51, 14}, {7, 3, 7, 3},
                                   {left, top, left + std::max(fill, 14.0f), top + h});
                if (state == L'r') {
                    m_glowSeen = true;
                    constexpr ULONGLONG kSweep = 1700, kCycle = 2400;
                    const ULONGLONG t = GetTickCount64() % kCycle;
                    if (t < kSweep) {
                        const float glow = 127.0f * h / 18.0f; // the overlay at the bar's height
                        const float x = left - glow + (fill + glow) * static_cast<float>(t) / kSweep;
                        win7::DrawPartTo(dc, win7::Module::Aero, 664, {0, 0, 127, 18}, {x, top, x + glow, top + h});
                    }
                }
                dc->PopAxisAlignedClip();
            }
            continue;
        }
        const bool primary = col.flex;
        D2D1_RECT_F box{c.x + kCellPad, y + 0.5f, c.x + c.w - kCellPad, y + 0.5f + rowH};
        const std::wstring text = m_source->Cell(row, c.src);
        // Tags stand before the text, as NetEase puts 原唱 before the singer (owner, 2026-11-21).
        if (!col.rightAlign) box.left = PaintBadges(dc, m_source->CellBadges(row, c.src), box.left, y + rowH / 2, box.right);
        const DWRITE_FONT_WEIGHT weight =
            isPlaying && primary ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL;
        Text(dc, text, box, isPlaying ? kPlayingInk : (primary ? kInk : kSecondaryInk),
             col.rightAlign ? DWRITE_TEXT_ALIGNMENT_TRAILING : DWRITE_TEXT_ALIGNMENT_LEADING, weight);
        // A note follows the text in grey, "(note)", where the text leaves room for it.
        if (const std::wstring note = m_source->CellNote(row, c.src); !note.empty() && !col.rightAlign) {
            auto layout = MakeText(text, 12.0f, weight, box.right - box.left, rowH);
            DWRITE_TEXT_METRICS m{};
            layout->GetMetrics(&m);
            const float x = box.left + m.widthIncludingTrailingWhitespace + 4;
            if (x + 12 < box.right) Text(dc, L"(" + note + L")", {x, box.top, box.right, box.bottom}, kNoteInk);
        }
    }
}

// A tag, as NetEase draws 原唱: its text in red inside a thin red rounded outline, a little
// smaller than the row's text, centred on `cy`. Returns its width; 0 when it does not fit before
// `right`.
float ListView::PaintBadge(ID2D1DeviceContext* dc, const std::wstring& text, float x, float cy, float right) {
    return DrawTag(dc, text, x, cy, right, Hex(kBadgeInk), 10.0f, m_scale);
}

// Tags (untranslated keys) from `x` on; returns where the text after them starts.
float ListView::PaintBadges(ID2D1DeviceContext* dc, const std::vector<std::wstring>& badges, float x, float cy,
                            float right) {
    if (!settings::SongTagsShown()) return x; // Preferences: tags such as 原唱 off
    for (const auto& badge : badges) {
        const float w = PaintBadge(dc, Tr(badge), x, cy, right);
        if (w <= 0) break;
        x += w + 5;
    }
    return x;
}

void ListView::PaintPlain(ID2D1DeviceContext* dc, D2D1_SIZE_F size) {
    const size_t n = m_source->Count();
    const auto cols = Columns(size.width);
    const size_t focus = m_source->Focus();
    const size_t visible = VisibleLines() + 1;
    const float rowH = RowHeightPx() / m_scale;  // whole device pixels, matches hit testing
    const float right = ContentWidth();  // rows end with the last column
    for (size_t i = 0; i < visible && m_top + i < n; ++i) {
        const size_t row = m_top + i;
        const float y = i * rowH - m_topOffsetDip;
        const bool sel = m_source->IsSelected(row), hot = row == m_hot;
        const wp::RowState st = sel ? (hot ? wp::RowState::SelectedHot : wp::RowState::Selected)
                                    : (hot ? wp::RowState::Hot : wp::RowState::Normal);
        wp::RowPlate(dc, {2, y + 1, right - 2, y + rowH}, st, m_focused);
        if (m_focused && row == focus && !sel)
            dc->DrawRoundedRectangle({{2.5f, y + 1.5f, right - 2.5f, y + rowH - 0.5f}, 2, 2},
                                     Brush(dc, 0x7DA2CE, 0.6f).get(), 1.0f);
        PaintCells(dc, row, y, rowH, cols);
    }
}

void ListView::PaintGrouped(ID2D1DeviceContext* dc, D2D1_SIZE_F size) {
    const auto& groups = *m_source->Groups();
    const auto cols = Columns(size.width);
    const bool art = AlbumColumn() && !cols.empty();
    const float albumL = art ? cols[0].x : kGutter, albumR = art ? cols[0].x + cols[0].w : 2;
    const size_t focus = m_source->Focus();
    const float right = ContentWidth();              // rows end with the last column
    const float viewRight = ScrollX() + size.width;  // group rules run to the view's edge
    // WMP 12: the group title starts a little inside the album column (7 px at 150 %).
    const float titleX = art ? albumL + 8 : kGutter + 2;

    std::vector<size_t> shownGroups;
    float y = -m_topOffsetDip;
    for (size_t i = 0; m_top + i < m_lines.size() && y < size.height; ++i) {
        const Line& line = m_lines[m_top + i];
        const float rowH = LineHeightDip(line);
        if (shownGroups.empty() || shownGroups.back() != line.group) shownGroups.push_back(line.group);
        if (line.kind == Line::GroupHeader) {
            // Album artist in blue, followed by a hairline to the right edge.
            const auto& g = groups[line.group];
            auto layout = MakeText(g.header, 12.0f, DWRITE_FONT_WEIGHT_NORMAL, std::max(40.0f, viewRight - titleX - 20), rowH);
            dc->DrawTextLayout({titleX, y + 1}, layout.get(), Brush(dc, kGroupInk).get());
            DWRITE_TEXT_METRICS tm{};
            layout->GetMetrics(&tm);
            const float lx = titleX + tm.widthIncludingTrailingWhitespace + 6;
            const float line1 = gfx::Hairline(m_scale);
            const float ly = std::round((y + rowH / 2 + 1) * m_scale) / m_scale;
            if (lx < viewRight - 6)
                dc->FillRectangle({lx, ly, viewRight - 6, ly + line1}, Brush(dc, kGroupRule).get());
            // Hovering the bar shows only its outline, as WMP does (no selection fill).
            if (line.group == m_hotGroupHeader) {
                const float px = gfx::Hairline(m_scale);
                dc->DrawRoundedRectangle({{2.5f, y + 1.5f, viewRight - 2.5f, y + rowH - 0.5f}, 2, 2},
                                         Brush(dc, 0x7DA2CE, 0.6f).get(), px);
            }
            y += rowH;
            continue;
        }
        if (line.kind != Line::Item) {
            y += rowH;
            continue;
        }
        const size_t row = line.row;
        const bool sel = m_source->IsSelected(row), hot = row == m_hot;
        const wp::RowState st = sel ? (hot ? wp::RowState::SelectedHot : wp::RowState::Selected)
                                    : (hot ? wp::RowState::Hot : wp::RowState::Normal);
        wp::RowPlate(dc, {albumR, y + 1, right - 2, y + rowH}, st, m_focused);
        if (m_focused && row == focus && !sel)
            dc->DrawRoundedRectangle({{albumR + 0.5f, y + 1.5f, right - 2.5f, y + rowH - 0.5f}, 2, 2},
                                     Brush(dc, 0x7DA2CE, 0.6f).get(), 1.0f);
        PaintCells(dc, row, y, rowH, cols);
        y += rowH;
    }
    if (!art) return;

    // Album column: cover plus album / artist / genre / year beside it, once per group.
    const float artSize = ArtSize();
    const float albumRowH = RowHeightPx() / m_scale;  // album column only exists in uniform tiles
    for (size_t gi : shownGroups) {
        const auto& g = groups[gi];
        const float top = LineTopDip(m_groupLine[gi] + 1) - ContentTopDip();
        // Only the cover and its four lines belong to the album cell; below them (long
        // albums) the column is empty space, e.g. to start a rubber band from.
        const size_t lines = kGroupMinLines;
        bool allSelected = g.count > 0;
        for (size_t k = 0; k < g.count && allSelected; ++k) allSelected = m_source->IsSelected(g.first + k);
        if (allSelected || gi == m_hotGroup) {
            const wp::RowState st = allSelected ? (gi == m_hotGroup ? wp::RowState::SelectedHot : wp::RowState::Selected)
                                                : wp::RowState::Hot;
            wp::RowPlate(dc, {albumL, top + 1, albumR - 2, top + lines * albumRowH}, st, m_focused);
        }
        DrawArt(dc, m_source->ArtKey(g.first), g.art, {albumL + 3, top + 4, albumL + 3 + artSize, top + 4 + artSize});
        const float tx = AlbumTextLeft(albumL);
        for (size_t k = 0; k < g.lines.size(); ++k) {
            const bool hotLink = gi == m_hotLinkGroup && k == m_hotLinkLine;
            Text(dc, g.lines[k], {tx, top + k * albumRowH + 0.5f, albumR - 4, top + (k + 1) * albumRowH + 0.5f},
                 hotLink ? kLinkInk : (k == 0 ? kInk : kSecondaryInk), DWRITE_TEXT_ALIGNMENT_LEADING,
                 DWRITE_FONT_WEIGHT_NORMAL, hotLink);
        }
    }
}

void ListView::PaintTiles(ID2D1DeviceContext* dc) {
    const size_t n = m_source->Count();
    const size_t focus = m_source->Focus();
    if (m_tileGroups) {
        // A header per group as in the grouped details (blue title, a rule to the right edge),
        // its `more` link at the right end; under it the group's rows of tiles.
        const auto& groups = *m_source->Groups();
        const float viewRight = RowsWidth();
        const float viewH = RowsHeightDip();
        float y = LineTopDip(m_top) - ContentTopDip();
        for (size_t i = m_top; i < m_lines.size() && y < viewH; ++i) {
            const Line& line = m_lines[i];
            const float lineH = LineHeightDip(line);
            if (line.kind == Line::GroupHeader) {
                const auto& g = groups[line.group];
                const float titleX = kTilePad + 2, baseY = y + lineH - 22;
                float right = viewRight - 8;
                if (ShowsMore(line.group)) {
                    const bool hotLink = line.group == m_hotLinkGroup && m_hotLinkLine == kMoreLink;
                    (void)hotLink;
                    if (g.carousel) {
                        // A shelf's "show all": the aero expand button turned to the right.
                        const D2D1_RECT_F box{right - kShowAll, baseY + 10 - kShowAll / 2, right,
                                              baseY + 10 + kShowAll / 2};
                        const bool lit = line.group == m_moreGroup;
                        RoundChevron(dc, box, lit ? m_moreHot : line.group == m_moreOldGroup ? m_moreOldHot : 0.0f,
                                     lit ? m_morePress : 0.0f);
                        right -= kShowAll + 6;
                    } else {
                        const float w = TextWidth(g.more, 200);
                        Text(dc, g.more, {right - w, baseY, right, baseY + 20}, hotLink ? kLinkInk : kGroupInk,
                             DWRITE_TEXT_ALIGNMENT_TRAILING, DWRITE_FONT_WEIGHT_NORMAL, hotLink);
                        right -= w + 10;
                    }
                }
                auto layout = MakeText(g.header, 14.0f, DWRITE_FONT_WEIGHT_NORMAL, std::max(40.0f, right - titleX), 20);
                dc->DrawTextLayout({titleX, baseY - 1}, layout.get(), Brush(dc, kGroupInk).get());
                DWRITE_TEXT_METRICS tm{};
                layout->GetMetrics(&tm);
                const float lx = titleX + tm.widthIncludingTrailingWhitespace + 8;
                const float line1 = gfx::Hairline(m_scale);
                const float ly = std::round((baseY + 11) * m_scale) / m_scale;
                if (lx < right) dc->FillRectangle({lx, ly, right, ly + line1}, Brush(dc, kGroupRule).get());
                y += lineH;
                continue;
            }
            const auto& g = groups[line.group];
            const media::TileStyle style = GroupTiles(line.group);
            const size_t cols = TileColumnsOf(style);
            if (g.carousel) {
                if (!m_slideKey.empty() && g.key == m_slideKey) { // the compositor's strip shows it
                    y += lineH;
                    continue;
                }
                const float pos = ShelfPos(line.group);
                const size_t rows = std::max<size_t>(g.tileLines, 1);
                const size_t first = static_cast<size_t>(std::max(0.0f, std::floor(pos)));
                const size_t span = static_cast<size_t>(std::ceil(ShelfVisible(line.group))) + 1;
                dc->PushAxisAlignedClip({0, y, viewRight, y + lineH}, D2D1_ANTIALIAS_MODE_ALIASED);
                for (size_t c = first; c <= first + span; ++c) {
                    const size_t idx = g.first + c * rows + line.sub;
                    if (idx >= g.first + g.count || idx >= n) break;
                    PaintTile(dc, idx, TileLeftOf(style, static_cast<float>(c) - pos, true), y, style, focus);
                }
                dc->PopAxisAlignedClip();
                y += lineH;
                continue;
            }
            const size_t end = TileGroupEnd(line.group);
            for (size_t c = 0; c < cols; ++c) {
                const size_t idx = line.row + c;
                if (idx >= end || idx >= n) break;
                PaintTile(dc, idx, TileLeftOf(style, static_cast<float>(c)), y, style, focus);
            }
            y += lineH;
        }
        PaintShelfArrows(dc);
        return;
    }
    const size_t cols = TileColumns();
    const media::TileStyle style = m_source->Tiles();
    const float lineH = LineHeightPx() / m_scale;
    const size_t visible = VisibleLines() + 1;
    for (size_t i = 0; i < visible; ++i) {
        const float y = i * lineH - m_topOffsetDip;
        for (size_t c = 0; c < cols; ++c) {
            const size_t idx = (m_top + i) * cols + c;
            if (idx >= n) return;
            PaintTile(dc, idx, TileLeft(c), y, style, focus);
        }
    }
}

// The arrows at a shelf's ends, over its tiles, where it can page that way.
void ListView::PaintShelfArrows(ID2D1DeviceContext* dc) {
    const auto& groups = *m_source->Groups();
    const float top0 = ContentTopDip(), viewH = RowsHeightDip();
    for (size_t gi = 0; gi < groups.size() && gi < m_groupLine.size(); ++gi) {
        const auto& g = groups[gi];
        if (!g.carousel || !g.count) continue;
        const size_t header = m_groupLine[gi];
        const size_t rows = std::min(std::max<size_t>(g.tileLines, 1), g.count);
        if (header + 1 + rows >= m_lineTop.size()) continue;
        const float top = m_lineTop[header + 1] - top0, bottom = m_lineTop[header + 1 + rows] - top0;
        if (bottom < 0 || top > viewH) continue;
        const auto shown = m_shelfShown.find(g.key);
        const float show = shown == m_shelfShown.end() ? 0.0f : shown->second;
        for (const bool right : {false, true}) {
            if (!ShelfCanPage(gi, right)) continue;
            const bool hot = m_hotShelf == gi && m_hotShelfSide == (right ? 1u : 0u);
            const bool held = m_shelfPressedGroup == gi && m_shelfPressedSide == (right ? 1u : 0u);
            ShelfArrowButton(dc, ShelfArrow(gi, right, top, bottom), right, show, hot ? m_shelfPlate : 0.0f,
                             held ? m_shelfPress : 0.0f);
        }
    }
}

void ListView::PaintTile(ID2D1DeviceContext* dc, size_t idx, float x, float y, media::TileStyle style, size_t focus) {
    if (style == media::TileStyle::Card) return; // the card layer draws it
    const D2D1_SIZE_F ts = TileSizeOf(style);
    const D2D1_RECT_F cell{x + 2, y + 3, x + ts.width - 2, y + ts.height - 3};
    const bool sel = m_source->IsSelected(idx), hot = idx == m_hot;
    if (sel || hot) {
        const wp::RowState st = sel ? (hot ? wp::RowState::SelectedHot : wp::RowState::Selected)
                                    : wp::RowState::Hot;
        wp::RowPlate(dc, cell, st, m_focused);
    } else if (m_focused && idx == focus) {
        dc->DrawRoundedRectangle({{cell.left + 0.5f, cell.top + 0.5f, cell.right - 0.5f, cell.bottom - 0.5f}, 2, 2},
                                 Brush(dc, 0x7DA2CE, 0.6f).get(), 1.0f);
    }
    const media::TileInfo info = m_source->Tile(idx);
    // The play button fades in on the tile under the mouse and out on the one it left; the
    // source mark gives way to it.
    const float play = idx == m_playRow ? m_playBlend : idx == m_playOldRow ? m_playOldBlend : 0.0f;
    const float playHot = idx == m_playRow ? m_playHotBlend : 0.0f;
    const D2D1_RECT_F playBox = TilePlayRect(style, x, y);
    if (style == media::TileStyle::Song) {
        const float a = kSongTileArt, ay = y + (ts.height - a) / 2;
        const D2D1_RECT_F art{x + 8, ay, x + 8 + a, ay + a};
        if (info.icon)
            win7::DrawStretched(dc, win7::Module::Wmploc, L"#" + std::to_wstring(info.icon), art);
        else
            DrawArt(dc, m_source->ArtKey(idx), info.art, art);
        if (!info.source.empty() && play < 1)  // the source mark gives way to the play button
            DrawSourceBadge(dc, {art.right - 14, art.bottom - 14, art.right + 1, art.bottom + 1}, info.source,
                            1.0f - play);
        if (play > 0) {
            dc->FillRectangle(art, Brush(dc, 0x000000, 0.22f * play).get());
            PlayTriangle(dc, playBox, playHot, play);
        }
        const float tx = art.right + 10, mid = y + ts.height / 2;
        Text(dc, info.lines[0], {tx, mid - 18, x + ts.width - 6, mid}, kInk);
        const float right = x + ts.width - 6;
        Text(dc, info.lines[1], {PaintBadges(dc, info.badges, tx, mid + 9, right), mid, right, mid + 18}, kGreyInk);
        return;
    }
    if (style == media::TileStyle::Artist) {
        const float a = kArtistTileArt, ay = y + (ts.height - a) / 2;
        if (info.icon)
            win7::DrawStretched(dc, win7::Module::Wmploc, L"#" + std::to_wstring(info.icon),
                                {x + 20, ay + 10, x + a, ay + a - 10});
        else
            DrawArt(dc, m_source->ArtKey(idx), info.art, {x + 10, ay, x + 10 + a, ay + a});
        if (!info.source.empty() && play < 1)
            DrawSourceBadge(dc, {x + 10 + a - kSourceBadge + 3, ay + a - kSourceBadge + 3, x + 13 + a, ay + a + 3},
                            info.source, 1.0f - play);
        if (play > 0)
            wmp::transport::PlayOrb(dc, playBox, playHot, play, idx == m_playPress && m_playHot ? 1.0f : 0.0f);
        const float tx = x + 10 + a + 12;
        for (size_t k = 0; k < info.lines.size(); ++k) {
            const float top = ay + 2 + k * 18, right = x + ts.width - 6;
            const float left = k == 1 ? PaintBadges(dc, info.badges, tx, top + 9, right) : tx;
            Text(dc, info.lines[k], {left, top, right, top + 18}, k == 0 ? kInk : kGreyInk);
        }
        return;
    }
    const float a = kAlbumTileArt, ax = x + (ts.width - a) / 2, ay = y + 10;
    if (info.icon)  // playlists page: WMP's large playlist icon instead of a cover
        win7::DrawStretched(dc, win7::Module::Wmploc, L"#" + std::to_wstring(info.icon), {ax + 10, ay + 10, ax + a - 10, ay + a - 10});
    else
        DrawArt(dc, m_source->ArtKey(idx), info.art, {ax, ay, ax + a, ay + a});
    if (!info.source.empty() && play < 1)  // mixed results: whose tile this is, on the cover's corner
        DrawSourceBadge(dc, {ax + a - kSourceBadge - 4, ay + a - kSourceBadge - 4, ax + a - 4, ay + a - 4}, info.source,
                        1.0f - play);
    if (play > 0) wmp::transport::PlayOrb(dc, playBox, playHot, play, idx == m_playPress && m_playHot ? 1.0f : 0.0f);
    for (size_t k = 0; k < 2; ++k)
        Text(dc, info.lines[k], {x + 6, ay + a + 5 + k * 17, x + ts.width - 6, ay + a + 5 + (k + 1) * 17},
             k == 0 ? kInk : kGreyInk, DWRITE_TEXT_ALIGNMENT_CENTER);
}

void ListView::DrawArt(ID2D1DeviceContext* dc, const std::wstring& key, const metadb_handle_ptr& track, D2D1_RECT_F box) {
    const float side = box.right - box.left;
    const int sizePx = static_cast<int>(std::lround(side * m_scale));
    std::wstring cacheKey = key;
    if (cacheKey.empty()) {
        cacheKey = pfc::stringcvt::string_wide_from_utf8(track->get_path()).get_ptr();
        cacheKey += L'|' + std::to_wstring(track->get_subsong_index());
    }
    cacheKey += L'|' + std::to_wstring(sizePx);

    auto& artCache = ArtCache::Get();
    const uint64_t version = artCache.Version(key, track, sizePx);
    winrt::com_ptr<ID2D1Bitmap> bmp;
    winrt::com_ptr<IWICBitmapSource> src;
    UINT sw = 0, sh = 0;
    auto cached = m_artBitmaps.find(cacheKey);
    if (cached != m_artBitmaps.end() && cached->second.version == version) {
        cached->second.used = ++m_artClock;
        bmp = cached->second.bitmap;
    } else {
        const auto state = artCache.Find(key, track, sizePx, &src);
        if (!src && state == ArtCache::State::Missing && cached != m_artBitmaps.end()) {
            m_artBitmaps.erase(cached);
            cached = m_artBitmaps.end();
        }
        if (src) {
            src->GetSize(&sw, &sh);
            if (SUCCEEDED(dc->CreateBitmapFromWicBitmap(src.get(), nullptr, bmp.put()))) {
                m_artBitmaps[cacheKey] = {bmp, ++m_artClock, version};
                if (m_artBitmaps.size() > static_cast<size_t>(settings::ListBitmapCache())) {
                    auto oldest = m_artBitmaps.begin();
                    for (auto it = m_artBitmaps.begin(); it != m_artBitmaps.end(); ++it)
                        if (it->second.used < oldest->second.used) oldest = it;
                    m_artBitmaps.erase(oldest);
                }
            } else {
                bmp = nullptr;
            }
        }
        if (!bmp && cached != m_artBitmaps.end()) {
            // The invalidation keeps the previous cover; draw it until the new one decodes.
            bmp = cached->second.bitmap;
        }
    }
    if (bmp) {
        const D2D1_SIZE_F sz = bmp->GetSize();
        sw = static_cast<UINT>(sz.width);
        sh = static_cast<UINT>(sz.height);
    }

    D2D1_RECT_F dst = box;
    if (sw && sh && sw != sh) {  // keep the aspect ratio, bottom-aligned like WMP covers
        const float k = side / std::max(sw, sh);
        const float w = sw * k, h = sh * k;
        dst = {box.left + (side - w) / 2, box.bottom - h, box.left + (side + w) / 2, box.bottom};
    }
    // WMP's soft drop shadow (wmploc ALBUMDROPSHADOW: a blurred square with an 8/200 margin).
    const float m = (dst.right - dst.left) * 8.0f / 184.0f;
    win7::DrawStretched(dc, win7::Module::Wmploc, L"ALBUMDROPSHADOW_200X200.PNG",
                        {dst.left - m + 1, dst.top - m + 2, dst.right + m + 1, dst.bottom + m + 2}, 0.45f);
    if (bmp) {
        dc->DrawBitmap(bmp.get(), &dst, 1.0f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, nullptr, nullptr);
    } else if (!win7::DrawStretched(dc, win7::Module::Wmploc, L"ALBUM_0.PNG", dst)) {
        dc->FillRectangle(dst, Brush(dc, 0xDCE4EE).get());  // no artwork files: plain placeholder
    }
    const float px = 1.0f / m_scale;
    dc->DrawRectangle({dst.left + px / 2, dst.top + px / 2, dst.right - px / 2, dst.bottom - px / 2},
                      Brush(dc, 0x000000, 0.18f).get(), px);
}

// By pixels: the thumb is the view's share of the content, wherever the tall rows are.
bool ListView::ThumbGeometry(float* top, float* height) const {
    const float arrow = kScrollArrow * m_scale;
    const float trackH = static_cast<float>(std::max<LONG>(RowsRect().bottom - RowsRect().top, 0)) - arrow * 2;
    const float content = ContentHeightDip(), view = RowsHeightDip(), max = MaxScrollDip();
    if (max <= 0.5f || trackH <= 0 || content <= 0) return false;
    const float minH = 24.0f * m_scale;
    *height = std::min(trackH, std::max(minH, trackH * view / content));
    *top = arrow + (trackH - *height) * std::clamp(ContentTopDip() / max, 0.0f, 1.0f);
    return true;
}

bool ListView::AnimateScrollBar() {
    bool active = false, changed = false;
    for (int part = 1; part <= 4; ++part) {
        const float target = m_scrollPressed == part ? 2.0f : m_scrollHot == part ? 1.0f : 0.0f;
        float& blend = m_scrollBlend[part];
        if (blend < target) {
            blend = std::min(target, blend + 0.12f);
            changed = true;
        } else if (blend > target) {
            blend = std::max(target, blend - 0.12f);
            changed = true;
        }
        if (blend != target) active = true;
    }
    if (changed) PaintScrollBar();
    // Shelves slide to their page (ease-out cubic); play buttons fade.
    bool rows = false;
    const ULONGLONG now = GetTickCount64();
    if (!m_slideKey.empty()) {
        if (now >= m_slideEnd)
            FinishSlide();
        else
            active = true;
    }
    const auto step = [&](float& value, float target, float up, float down) {
        const float next = value < target ? std::min(target, value + up) : std::max(target, value - down);
        if (next == value) return;
        value = next;
        rows = true;
        if (value != target) active = true;
    };
    step(m_playBlend, m_playRow != SIZE_MAX ? 1.0f : 0.0f, 0.2f, 0.2f);
    step(m_playOldBlend, 0.0f, 0.15f, 0.15f);
    step(m_playHotBlend, m_playHot ? 1.0f : 0.0f, 0.25f, 0.12f);
    step(m_shelfPlate, m_hotShelf != SIZE_MAX ? 1.0f : 0.0f, 0.2f, 0.2f);
    step(m_downloadHot, m_hotDownload ? 1.0f : 0.0f, 0.15f, 0.08f);
    step(m_shelfPress,
         m_shelfPressedGroup != SIZE_MAX && m_shelfPressedGroup == m_hotShelf && m_shelfPressedSide == m_hotShelfSide
             ? 1.0f
             : 0.0f,
         0.35f, 0.15f);
    step(m_moreHot, m_moreGroup != SIZE_MAX ? 1.0f : 0.0f, 0.2f, 0.1f);
    step(m_moreOldHot, 0.0f, 0.1f, 0.1f);
    step(m_morePress, m_morePressedGroup != SIZE_MAX && m_morePressedGroup == m_moreGroup ? 1.0f : 0.0f, 0.35f, 0.15f);
    if (m_source && m_source->Groups())
        for (const auto& g : *m_source->Groups())
            if (g.carousel) step(m_shelfShown[g.key], g.key == m_shelfUnder ? 1.0f : 0.0f, 0.15f, 0.1f);
    if (m_playOldBlend == 0) m_playOldRow = SIZE_MAX;
    if (m_glowing) { // about 30 frames a second for the glow
        active = true;
        if (now - m_glowPainted >= 33) {
            m_glowPainted = now;
            rows = true;
        }
    }
    if (rows) PaintRows();
    return active;
}

bool ListView::OnScrollRepeat() {
    const float page = std::max(RowsHeightDip() - 24.0f, 24.0f);
    switch (m_scrollPressed) {
    case 1:
        if (ContentTopDip() <= 0) return false;
        ScrollBy(-kScrollStep);
        return ContentTopDip() > 0;
    case 2:
        if (ContentTopDip() >= MaxScrollDip()) return false;
        ScrollBy(kScrollStep);
        return ContentTopDip() < MaxScrollDip();
    case 3: {
        float top = 0, height = 0;
        if (!ThumbGeometry(&top, &height)) return false;
        const RECT rr = RowsRect();
        const float y = static_cast<float>(m_scrollPt.y - rr.top);
        if (m_scrollTrackDir == 1) {
            if (y >= top) return false;  // the thumb has reached the pointer
            if (ContentTopDip() <= 0) return false;
            ScrollBy(-page);
            return true;
        }
        if (m_scrollTrackDir == 2) {
            if (y < top + height) return false;
            if (ContentTopDip() >= MaxScrollDip()) return false;
            ScrollBy(page);
            return true;
        }
        return false;
    }
    default: return false;
    }
}

void ListView::PaintScrollBar() {
    auto& host = gfx::CompositionHost::Get();
    const RECT rr = RowsRect();
    const int w = ScrollWidthPx(), h = std::max<int>(rr.bottom - rr.top, 1);
    float top = 0, height = 0;
    const bool has = ThumbGeometry(&top, &height);
    const float s = m_scale;
    host.Repaint(m_scroll, w, h, s, [&](ID2D1DeviceContext* dc) {
        wp::ScrollBar(dc, {w / s, h / s}, 0, 0, m_scrollBlend.data()); // the track and the arrows
    });
    PlaceVisual(m_scroll, static_cast<float>(rr.right - m_bounds.left), static_cast<float>(ContentOffsetPx()),
                static_cast<float>(w), static_cast<float>(h));
    m_thumb.IsVisible(has);
    if (has) {
        const int th = std::max(1, static_cast<int>(std::lround(height)));
        host.Repaint(m_thumb, w, th, s, [&](ID2D1DeviceContext* dc) {
            wp::ScrollBar(dc, {w / s, th / s}, 0, th / s, m_scrollBlend.data(), true);
        });
        m_thumb.Size(float2{static_cast<float>(w), static_cast<float>(th)});
    }
    PlaceThumb();
}

void ListView::PlaceThumb() {
    float top = 0, height = 0;
    if (!ThumbGeometry(&top, &height)) return;
    const RECT rr = RowsRect();
    const float arrow = kScrollArrow * m_scale;
    const float trackH = static_cast<float>(std::max<LONG>(rr.bottom - rr.top, 0)) - arrow * 2;
    const float max = MaxScrollDip() * m_scale;
    const float lo = static_cast<float>(ContentOffsetPx()) + arrow;
    m_thumbPlace.InsertScalar(L"x", static_cast<float>(rr.right - m_bounds.left));
    m_thumbPlace.InsertScalar(L"a", lo);
    m_thumbPlace.InsertScalar(L"b", max > 0 ? (trackH - std::round(height)) / max : 0.0f);
    m_thumbPlace.InsertScalar(L"top", 0.0f); // content coordinates are absolute
    m_thumbPlace.InsertScalar(L"lo", lo);
    m_thumbPlace.InsertScalar(L"hi", lo + std::max(0.0f, trackH - std::round(height)));
}

// ---- Input ----------------------------------------------------------------------------------

ListView::Hit ListView::HitAt(POINT pt) const {
    Hit hit;
    if (!PtInRect(&m_bounds, pt)) return hit;
    const float x = (pt.x - m_bounds.left) / m_scale + ScrollX();  // content x
    if (pt.y < m_bounds.top + HeaderHeightPx()) {
        hit.part = Part::HeaderCell;
        hit.col = SIZE_MAX;
        if (!m_source) return hit;
        const auto cols = Columns(RowsWidth());
        for (size_t d = 0; d < cols.size(); ++d)
            if (std::abs(x - (cols[d].x + cols[d].w)) <= kEdgeGrip) {
                hit.part = Part::HeaderEdge;
                hit.col = d;
                return hit;
            }
        for (size_t d = 0; d < cols.size(); ++d)
            if (x >= cols[d].x && x < cols[d].x + cols[d].w) hit.col = d;
        return hit;
    }
    if (pt.y < m_bounds.top + ContentOffsetPx()) {
        hit.part = Part::Status;
        return hit;
    }
    const RECT rr = RowsRect();
    if (pt.x >= rr.right) {
        float top = 0, height = 0;
        const float arrow = kScrollArrow * m_scale;
        const float y = static_cast<float>(pt.y - rr.top);
        if (y < arrow) {
            hit.part = Part::ArrowUp;
            return hit;
        }
        if (y >= static_cast<float>(rr.bottom - rr.top) - arrow) {
            hit.part = Part::ArrowDown;
            return hit;
        }
        hit.part = Part::Track;
        if (ThumbGeometry(&top, &height) && y >= top && y < top + height) hit.part = Part::Thumb;
        return hit;
    }
    hit.part = Part::Empty;
    if (!m_source) return hit;
    // Below the last grouped line is empty space; LineAtDip would clamp it to the last row.
    const float contentY = ContentPoint(pt).y;
    if (Lined() && !m_lineTop.empty() && contentY >= m_lineTop.back()) return hit;
    const size_t line = LineAtDip(contentY);
    switch (m_mode) {
    case Mode::Tiles: {
        const size_t cols = TileColumns();
        // A tile is what it draws (owner, 2026-11-20): the room spread between tiles is empty
        // space, where the page's own menu opens.
        const auto inside = [&](media::TileStyle style, float left, float top) {
            const D2D1_SIZE_F ts = TileSizeOf(style);
            const D2D1_RECT_F box = style == media::TileStyle::Card
                                        ? D2D1_RECT_F{left + kCardInset.x, top + kCardInset.y,
                                                      left + kCardInset.x + CardLayer::kCard.width,
                                                      top + kCardInset.y + CardLayer::kCard.height}
                                        : D2D1_RECT_F{left + 2, top + 3, left + ts.width - 2, top + ts.height - 3};
            return x >= box.left && x < box.right && contentY >= box.top && contentY < box.bottom;
        };
        const auto playAt = [&](size_t idx, media::TileStyle style, float left, float top) {
            if (!inside(style, left, top)) return;
            const D2D1_RECT_F box = TilePlayRect(style, left, top);
            hit.part = m_source->CanPlayTile(idx) && x >= box.left && x < box.right && contentY >= box.top &&
                               contentY < box.bottom
                           ? Part::TilePlay
                           : Part::Row;
            hit.row = idx;
        };
        if (m_tileGroups) {
            // A shelf's arrows lie over its tiles.
            const auto& groups = *m_source->Groups();
            for (size_t gi = 0; gi < groups.size() && gi < m_groupLine.size(); ++gi) {
                const auto& g = groups[gi];
                if (!g.carousel || !g.count) continue;
                const size_t header = m_groupLine[gi];
                const size_t rows = std::min(std::max<size_t>(g.tileLines, 1), g.count);
                if (header + 1 + rows >= m_lineTop.size()) continue;
                const float top = m_lineTop[header + 1], bottom = m_lineTop[header + 1 + rows];
                if (contentY < top || contentY >= bottom) continue;
                for (const bool right : {false, true}) {
                    const D2D1_RECT_F box = ShelfArrow(gi, right, top, bottom);
                    if (ShelfCanPage(gi, right) && x >= box.left && x < box.right && contentY >= box.top &&
                        contentY < box.bottom) {
                        hit.part = Part::Carousel;
                        hit.group = gi;
                        hit.line = right ? 1 : 0;
                        return hit;
                    }
                }
            }
            if (line >= m_lines.size()) return hit;
            const Line& l = m_lines[line];
            if (l.kind == Line::GroupHeader) {
                hit.group = l.group;
                const auto& g = groups[l.group];
                const float right = RowsWidth() - 8;
                const float w = g.carousel ? kShowAll : TextWidth(g.more, 200) + 2;
                if (ShowsMore(l.group) && x >= right - w && x <= right) {
                    hit.part = Part::Link;
                    hit.line = kMoreLink;
                }
                return hit; // the header's title selects nothing here
            }
            float left = 0;
            const size_t idx = TileAtLine(l, x, &left);
            if (idx == SIZE_MAX) return hit;
            playAt(idx, GroupTiles(l.group), left, m_lineTop[line]);
            return hit;
        }
        if (x < kTilePad) return hit;
        const size_t col = static_cast<size_t>((x - kTilePad) / TilePitch());
        const size_t idx = line * cols + col;
        if (col >= cols || idx >= m_source->Count()) return hit;
        playAt(idx, m_source->Tiles(), TileLeft(col), LineTopDip(line));
        return hit;
    }
    case Mode::Grouped: {
        if (line >= m_lines.size()) return hit;
        const Line& l = m_lines[line];
        if (l.kind == Line::GroupHeader) {  // WMP: clicking the group bar selects the group
            hit.part = Part::GroupHeader;
            hit.group = l.group;
            return hit;
        }
        float rowLeft = 0;
        if (AlbumColumn()) {
            const auto cols = Columns(RowsWidth());
            const float albumL = cols[0].x, albumR = cols[0].x + cols[0].w;
            rowLeft = albumR;
            if (l.kind != Line::GroupHeader && x >= albumL && x < albumR) {
                if (l.sub >= kGroupMinLines) return hit;  // below the cover and its lines: empty
                hit.part = Part::AlbumColumn;
                hit.group = l.group;
                const auto& g = (*m_source->Groups())[l.group];
                const size_t k = l.sub;
                const float tx = AlbumTextLeft(albumL);
                if (k < g.lines.size() && g.links[k] && !g.lines[k].empty() && x >= tx &&
                    x <= tx + TextWidth(g.lines[k], albumR - 4 - tx)) {
                    hit.part = Part::Link;
                    hit.line = k;
                }
                return hit;
            }
        }
        if (l.kind != Line::Item || x < rowLeft || x >= ContentWidth()) return hit;
        hit.part = Part::Row;
        hit.row = l.row;
        return hit;
    }
    default:
        if (line < m_source->Count() && x < ContentWidth()) {  // rows end with the last column
            hit.part = Part::Row;
            hit.row = line;
        }
        return hit;
    }
}

LPCWSTR ListView::CursorAt(POINT pt) const {
    if (m_drag == Drag::Resize) return IDC_SIZEWE;
    switch (HitAt(pt).part) {
    case Part::HeaderEdge: return IDC_SIZEWE;
    case Part::Link:
    case Part::Carousel:
    case Part::TilePlay: return IDC_HAND;
    default: return nullptr;
    }
}

void ListView::OnMouseMove(POINT pt) {
    switch (m_drag) {
    case Drag::Thumb: {
        const RECT rr = RowsRect();
        float top = 0, height = 0;
        if (ThumbGeometry(&top, &height)) {
            const float arrow = kScrollArrow * m_scale;
            const float trackH = static_cast<float>(rr.bottom - rr.top) - arrow * 2 - height;
            const float y = static_cast<float>(pt.y - rr.top - arrow - m_dragOffset);
            const float frac = trackH > 0 ? std::clamp(y / trackH, 0.0f, 1.0f) : 0.0f;
            SetScrollDip(frac * MaxScrollDip());
        }
        return;
    }
    case Drag::Resize: {
        const auto cols = Columns(RowsWidth());
        if (m_dragCol < cols.size()) {
            const size_t src = cols[m_dragCol].src;
            if (m_widths.size() <= src) m_widths.resize(src + 1, -1.0f);
            m_widths[src] = std::max(kMinColumn, m_dragStartW + (pt.x - m_dragStartX) / m_scale);
            PaintHeader();
            PaintRows();
        }
        return;
    }
    case Drag::PendingMove:
        if (std::abs(pt.x - m_dragStartX) < kMoveThreshold * m_scale) return;
        m_drag = Drag::Move;
        [[fallthrough]];
    case Drag::Move:
        m_dragX = pt.x;
        PaintHeader();
        return;
    case Drag::Marquee: {
        // Scroll when dragged past the top or bottom (one line per mouse move, no timer).
        const RECT rr = RowsRect();
        if (pt.y < rr.top && m_top > 0) ScrollTo(m_top - 1);
        else if (pt.y >= rr.bottom) ScrollTo(m_top + 1);
        pt.y = std::clamp<LONG>(pt.y, rr.top, rr.bottom - 1);
        m_marqueeTo = ContentPoint(pt);
        UpdateMarquee();
        PaintRows();
        return;
    }
    default:
        break;
    }
    const Hit hit = HitAt(pt);
    const size_t hotHeader = hit.part == Part::HeaderCell ? hit.col : SIZE_MAX;
    if (hotHeader != m_hotHeader) {
        m_hotHeader = hotHeader;
        PaintHeader();
    }
    const size_t hot = hit.part == Part::Row || hit.part == Part::TilePlay ? hit.row : SIZE_MAX;
    // The tile play button: on the hot tile that can play, fading in (AnimateScrollBar).
    const size_t playRow =
        m_mode == Mode::Tiles && hot != SIZE_MAX && !IsCard(hot) && m_source->CanPlayTile(hot) ? hot : SIZE_MAX;
    const bool playHot = hit.part == Part::TilePlay;
    if (playRow != m_playRow || playHot != m_playHot) {
        if (playRow != m_playRow) {
            m_playOldRow = m_playRow;
            m_playOldBlend = m_playBlend;
            m_playRow = playRow;
            m_playBlend = 0;
            m_playHotBlend = 0;
        }
        m_playHot = playHot;
        if (onScrollAnim) onScrollAnim();
    }
    // The shelf under the mouse shows its arrows (fading in); the arrow under it its plate.
    std::wstring shelfKey;
    if (m_mode == Mode::Tiles && m_tileGroups && hit.part != Part::None) {
        const D2D1_POINT_2F c = ContentPoint(pt);
        const auto& groups = *m_source->Groups();
        for (size_t gi = 0; gi < groups.size() && gi < m_groupLine.size(); ++gi) {
            const auto& g = groups[gi];
            if (!g.carousel || !g.count) continue;
            const size_t header = m_groupLine[gi];
            const size_t rows = std::min(std::max<size_t>(g.tileLines, 1), g.count);
            if (header + 1 + rows < m_lineTop.size() && c.y >= m_lineTop[header + 1] &&
                c.y < m_lineTop[header + 1 + rows])
                shelfKey = g.key;
        }
    }
    if (shelfKey != m_shelfUnder) {
        m_shelfUnder = shelfKey;
        if (onScrollAnim) onScrollAnim();
    }
    m_cards->SetHot(m_source, IsCard(hot) ? hot : SIZE_MAX, playHot);
    const size_t hotShelf = hit.part == Part::Carousel ? hit.group : SIZE_MAX;
    const size_t hotShelfSide = hit.part == Part::Carousel ? hit.line : 0;
    if (hotShelf != m_hotShelf || hotShelfSide != m_hotShelfSide) {
        m_hotShelf = hotShelf;
        m_hotShelfSide = hotShelfSide;
        m_shelfPlate = 0;
        if (onScrollAnim) onScrollAnim();
        PaintRows();
    }
    if (const int star = hot != SIZE_MAX ? StarAt(pt) : -1; star != m_hotStar) {
        m_hotStar = star;
        if (hot == m_hot) PaintRows();  // otherwise the hot-row change below repaints
    }
    const size_t hotGroup = hit.part == Part::AlbumColumn || hit.part == Part::Link ? hit.group : SIZE_MAX;
    const size_t hotGroupHeader = hit.part == Part::GroupHeader ? hit.group : SIZE_MAX;
    if (hotGroupHeader != m_hotGroupHeader) {
        m_hotGroupHeader = hotGroupHeader;
        PaintRows();
    }
    const size_t linkGroup = hit.part == Part::Link ? hit.group : SIZE_MAX;
    // A shelf's show-all ball lights under the mouse (fading, AnimateScrollBar).
    const size_t moreGroup = hit.part == Part::Link && hit.line == kMoreLink ? hit.group : SIZE_MAX;
    if (moreGroup != m_moreGroup) {
        m_moreOldGroup = m_moreGroup;
        m_moreOldHot = m_moreHot;
        m_moreGroup = moreGroup;
        m_moreHot = 0;
        if (onScrollAnim) onScrollAnim();
    }
    const size_t linkLine = hit.part == Part::Link ? hit.line : 0;
    // The download mark darkens under the mouse.
    const bool hotDownload = hit.part == Part::Row && CellAt(pt, nullptr) == static_cast<int>(CellKind::Download);
    if (hotDownload != m_hotDownload) {
        m_hotDownload = hotDownload;
        if (onScrollAnim) onScrollAnim(); // the plate fades
    }
    if (hot != m_hot) { // another row's mark starts unlit, and lights if the mouse is on it
        m_downloadHot = 0;
        if (hotDownload && onScrollAnim) onScrollAnim();
    }
    if (hot != m_hot || hotGroup != m_hotGroup || linkGroup != m_hotLinkGroup || linkLine != m_hotLinkLine) {
        m_hot = hot;
        m_hotGroup = hotGroup;
        m_hotLinkGroup = linkGroup;
        m_hotLinkLine = linkLine;
        PaintRows();
    }
    const int scrollHot = hit.part == Part::ArrowUp ? 1
                          : hit.part == Part::ArrowDown ? 2
                          : hit.part == Part::Track   ? 3
                          : hit.part == Part::Thumb   ? 4
                                                      : 0;
    if (scrollHot != m_scrollHot) {
        m_scrollHot = scrollHot;
        if (onScrollAnim) onScrollAnim();
    }
}

void ListView::OnMouseLeave() {
    if (m_drag != Drag::None) return;
    m_cards->SetHot(m_source, SIZE_MAX, false);
    if (m_hotHeader != SIZE_MAX) {
        m_hotHeader = SIZE_MAX;
        PaintHeader();
    }
    if (m_playRow != SIZE_MAX) {
        m_playOldRow = m_playRow;
        m_playOldBlend = m_playBlend;
        m_playRow = SIZE_MAX;
        m_playHot = false;
        if (onScrollAnim) onScrollAnim();
    }
    if (m_moreGroup != SIZE_MAX) {
        m_moreOldGroup = m_moreGroup;
        m_moreOldHot = m_moreHot;
        m_moreGroup = SIZE_MAX;
        if (onScrollAnim) onScrollAnim();
    }
    if (m_hotShelf != SIZE_MAX || !m_shelfUnder.empty()) {
        m_hotShelf = SIZE_MAX;
        m_shelfUnder.clear();
        if (onScrollAnim) onScrollAnim();
        PaintRows();
    }
    m_hotDownload = false;
    m_downloadHot = 0;
    if (m_hot != SIZE_MAX || m_hotGroup != SIZE_MAX || m_hotGroupHeader != SIZE_MAX || m_hotLinkGroup != SIZE_MAX) {
        m_hot = SIZE_MAX;
        m_hotGroup = SIZE_MAX;
        m_hotGroupHeader = SIZE_MAX;
        m_hotLinkGroup = SIZE_MAX;
        PaintRows();
    }
    if (m_scrollHot) {
        m_scrollHot = 0;
        if (onScrollAnim) onScrollAnim();
    }
}

void ListView::OnLButtonDown(POINT pt, bool ctrl, bool shift) {
    const Hit hit = HitAt(pt);
    switch (hit.part) {
    case Part::Thumb: {
        float top = 0, height = 0;
        ThumbGeometry(&top, &height);
        m_drag = Drag::Thumb;
        m_scrollPressed = 4;
        m_scrollPt = pt;
        m_dragOffset = static_cast<int>(pt.y - RowsRect().top - top);
        if (onScrollAnim) onScrollAnim();
        break;
    }
    case Part::Track: {
        float top = 0, height = 0;
        if (ThumbGeometry(&top, &height)) {
            const float page = std::max(RowsHeightDip() - 24.0f, 24.0f);
            if (pt.y - RowsRect().top < top) {
                m_scrollTrackDir = 1;
                ScrollBy(-page);
            } else {
                m_scrollTrackDir = 2;
                ScrollBy(page);
            }
        }
        m_scrollPressed = 3;
        m_scrollPt = pt;
        if (onScrollAnim) onScrollAnim();
        if (onScrollHold) onScrollHold();
        break;
    }
    case Part::ArrowUp: {
        ScrollBy(-kScrollStep);
        m_scrollPressed = 1;
        m_scrollPt = pt;
        if (onScrollAnim) onScrollAnim();
        if (onScrollHold) onScrollHold();
        break;
    }
    case Part::ArrowDown: {
        ScrollBy(kScrollStep);
        m_scrollPressed = 2;
        m_scrollPt = pt;
        if (onScrollAnim) onScrollAnim();
        if (onScrollHold) onScrollHold();
        break;
    }
    case Part::HeaderEdge: {
        const auto cols = Columns(RowsWidth());
        m_drag = Drag::Resize;
        m_dragCol = hit.col;
        m_dragStartX = pt.x;
        m_dragStartW = cols[hit.col].w;
        break;
    }
    case Part::HeaderCell:
        // The album column stays in front; any other column can be dragged elsewhere.
        if (hit.col != SIZE_MAX && !(AlbumColumn() && hit.col == 0)) {
            m_drag = Drag::PendingMove;
            m_dragCol = hit.col;
            m_dragStartX = m_dragX = pt.x;
            PaintHeader();  // pressed look
        } else if (hit.col == 0 && AlbumColumn()) {
            SortBy(0);  // the album column cannot move but still sorts
        }
        break;
    case Part::Row: {
        if (IsCard(hit.row)) { // a card presses in, and opens when released on it
            m_drag = Drag::CardPress;
            m_cardPress = hit.row;
            m_source->SetFocus(hit.row);
            m_cards->SetPressed(m_source, hit.row);
            break;
        }
        // Rating stars and the Favorite star act on the clicked song (and select it).
        const int kind = CellAt(pt, nullptr);
        if (const int star = StarAt(pt); kind == static_cast<int>(CellKind::Rating) && star >= 0)
            m_source->Rate(hit.row, star + 1);
        else if (kind == static_cast<int>(CellKind::Favorite))
            m_source->ToggleFavorite(hit.row);
        else if (kind == static_cast<int>(CellKind::Download))
            m_source->ToggleDownload(hit.row);
        MoveFocus(hit.row, ctrl, shift);
        break;
    }
    case Part::Carousel: // held down: the pressed plate; it pages when released on it
        m_drag = Drag::ShelfButton;
        m_shelfPressedGroup = hit.group;
        m_shelfPressedSide = hit.line;
        if (onScrollAnim) onScrollAnim();
        break;
    case Part::TilePlay: // held: the orb's pressed look; it plays when released on it
        m_drag = Drag::PlayPress;
        m_playPress = hit.row;
        if (IsCard(hit.row)) m_cards->SetPressed(m_source, hit.row);
        PaintRows();
        break;
    case Part::Link:
        if (hit.line == kMoreLink && m_source->Groups() && hit.group < m_source->Groups()->size() &&
            (*m_source->Groups())[hit.group].carousel) {
            // The show-all ball: held while the button is down, acting when it comes up on it.
            m_drag = Drag::MoreButton;
            m_morePressedGroup = hit.group;
            if (onScrollAnim) onScrollAnim();
        } else if (hit.line == kMoreLink)
            m_source->ShowGroupMore(hit.group);
        else if (onLink)
            onLink(hit.group, hit.line);
        break;
    case Part::Empty: {
        // Empty space: start a rubber band. Without Ctrl the click clears the selection.
        if (!m_source || m_source->Count() == 0) break;
        m_drag = Drag::Marquee;
        m_marqueeFrom = m_marqueeTo = ContentPoint(pt);
        m_marqueeBase.clear();
        if (ctrl)
            for (size_t r = 0, n = m_source->Count(); r < n; ++r)
                if (m_source->IsSelected(r)) m_marqueeBase.push_back(r);
        m_source->SetSelection(m_marqueeBase);
        PaintRows();
        break;
    }
    case Part::AlbumColumn:
    case Part::GroupHeader: {  // WMP: the album cell / group bar selects the whole group
        const auto& g = (*m_source->Groups())[hit.group];
        if (g.count == 0) break;
        m_source->SelectRange(g.first, g.first + g.count - 1, ctrl);
        m_source->SetFocus(g.first);
        m_anchor = g.first;
        PaintRows();
        break;
    }
    default:
        break;
    }
}

void ListView::OnLButtonUp(POINT pt) {
    const Drag drag = m_drag;
    m_drag = Drag::None;
    if (m_scrollPressed) {
        m_scrollPressed = 0;
        m_scrollTrackDir = 0;
        if (onScrollAnim) onScrollAnim();
    }
    switch (drag) {
    case Drag::ShelfButton: {
        const size_t group = m_shelfPressedGroup, side = m_shelfPressedSide;
        m_shelfPressedGroup = SIZE_MAX;
        if (onScrollAnim) onScrollAnim();
        const Hit up = HitAt(pt);
        if (up.part == Part::Carousel && up.group == group && up.line == side) PageShelf(group, side == 1);
        break;
    }
    case Drag::MoreButton: {
        const size_t group = m_morePressedGroup;
        m_morePressedGroup = SIZE_MAX;
        if (onScrollAnim) onScrollAnim();
        const Hit up = HitAt(pt);
        if (up.part == Part::Link && up.line == kMoreLink && up.group == group) m_source->ShowGroupMore(group);
        break;
    }
    case Drag::Thumb: PaintScrollBar(); break;
    case Drag::Marquee: PaintRows(); break;
    case Drag::PlayPress: {
        const size_t row = std::exchange(m_playPress, SIZE_MAX);
        m_cards->SetPressed(m_source, SIZE_MAX);
        const Hit up = HitAt(pt);
        PaintRows();
        if (up.part == Part::TilePlay && up.row == row && m_source) m_source->PlayTile(row);
        break;
    }
    case Drag::CardPress: {
        const size_t row = std::exchange(m_cardPress, SIZE_MAX);
        m_cards->SetPressed(m_source, SIZE_MAX);
        const Hit up = HitAt(pt);
        if (up.part == Part::Row && up.row == row && m_source) m_source->Activate(row); // may leave the page
        break;
    }
    case Drag::Resize: SaveColumnLayout(); break;
    case Drag::PendingMove: {  // a click on a header sorts by that column
        const auto cols = Columns(RowsWidth());
        if (m_dragCol < cols.size()) SortBy(cols[m_dragCol].src);
        PaintHeader();
        break;
    }
    case Drag::Move: {
        const auto cols = Columns(RowsWidth());
        const float x = (pt.x - m_bounds.left) / m_scale + ScrollX();
        size_t target = cols.size();
        for (size_t d = 0; d < cols.size(); ++d)
            if (x < cols[d].x + cols[d].w / 2) {
                target = d;
                break;
            }
        if (AlbumColumn()) target = std::max<size_t>(target, 1);
        if (m_dragCol < cols.size() && target != m_dragCol && target != m_dragCol + 1) {
            // Move in m_order, which also holds the hidden columns: before the column now
            // shown at `target`, or after the last shown one.
            const size_t src = cols[m_dragCol].src;
            m_order.erase(std::find(m_order.begin(), m_order.end(), src));
            auto at = target < cols.size() ? std::find(m_order.begin(), m_order.end(), cols[target].src)
                                           : std::find(m_order.begin(), m_order.end(), cols.back().src) + 1;
            m_order.insert(at, src);
            SaveColumnLayout();
        }
        PaintHeader();
        PaintRows();
        break;
    }
    default: break;
    }
}

void ListView::CancelDrag() {
    if (m_drag == Drag::PlayPress) {
        m_playPress = SIZE_MAX;
        m_cards->SetPressed(m_source, SIZE_MAX);
    }
    if (m_drag == Drag::CardPress) {
        m_cardPress = SIZE_MAX;
        m_cards->SetPressed(m_source, SIZE_MAX);
    }
    if (m_drag != Drag::None) m_drag = Drag::None;
    if (m_scrollPressed || m_scrollHot) {
        m_scrollPressed = 0;
        m_scrollTrackDir = 0;
        m_scrollHot = 0;
        if (onScrollAnim) onScrollAnim();
    }
    PaintHeader();
    PaintRows();
    PaintScrollBar();
}

void ListView::OnDoubleClick(POINT pt) {
    const Hit hit = HitAt(pt);
    if (hit.part == Part::AlbumColumn && m_source) {
        // The album's songs only, as the click on the album selected them (owner, 2026-11-15).
        m_source->ActivateGroup(hit.group);
        PaintRows();
    } else if (hit.part == Part::Row && m_source && !IsCard(hit.row)) { // a card opened on the click
        m_source->Activate(hit.row);
        PaintRows();
    }
}

void ListView::OnRButtonUp(HWND owner, POINT pt) {
    if (!m_source) return;
    const Hit hit = HitAt(pt);
    if (hit.part == Part::Status) return;
    if (hit.part == Part::Row && !m_source->IsSelected(hit.row)) {
        m_source->SelectOnly(hit.row);
        m_source->SetFocus(hit.row);
        m_anchor = hit.row;
        PaintRows();
    } else if (hit.part == Part::GroupHeader || hit.part == Part::AlbumColumn || hit.part == Part::Link) {
        const auto& g = (*m_source->Groups())[hit.group];
        if (g.count) {
            // A right-click on a group that is already fully selected keeps the rest of the
            // selection: the menu then acts on everything the user selected. Otherwise the
            // group becomes the selection.
            bool allSelected = true;
            for (size_t r = g.first; allSelected && r < g.first + g.count; ++r)
                if (!m_source->IsSelected(r)) allSelected = false;
            if (!allSelected) {
                m_source->SelectRange(g.first, g.first + g.count - 1, false);
                m_anchor = g.first;
            }
            m_source->SetFocus(g.first);
            PaintRows();
        }
    } else if (hit.part == Part::HeaderCell || hit.part == Part::HeaderEdge) {
        ShowHeaderMenu(owner, pt, hit.col);
        return;
    }
    POINT screen = pt;
    ClientToScreen(owner, &screen);
    if ((hit.part == Part::Empty || hit.part == Part::None) && onEmptyContext) {
        onEmptyContext(screen);
        return;
    }
    m_source->ContextMenu(owner, screen);
    Refresh();
}

void ListView::SortBy(size_t src) {
    if (!m_source || !m_source->CanSort(src)) return;
    // WMP 12 cycles a column: ascending, descending, then back to the default order.
    if (m_source->SortColumn() != src)
        m_source->Sort(src, false);
    else if (!m_source->SortDescending())
        m_source->Sort(src, true);
    else
        m_source->Sort(SIZE_MAX, false);
    Refresh();
    if (onPresentationChanged) onPresentationChanged();
}

void ListView::ShowHeaderMenu(HWND owner, POINT pt, size_t displayCol) {
    if (!m_source) return;
    const auto& srcCols = m_source->Columns();
    const auto cols = Columns(RowsWidth());
    const size_t src = displayCol < cols.size() ? cols[displayCol].src : SIZE_MAX;
    enum : UINT { kSort = 1, kReset = 2, kGroups = 3, kColumn = 100 };
    HMENU menu = CreatePopupMenu();
    HMENU choose = CreatePopupMenu();
    if (src != SIZE_MAX && m_source->CanSort(src) && !srcCols[src].title.empty()) {
        AppendMenuW(menu, MF_STRING, kSort, (Tr(L"Sort by") + L" \u201C" + Tr(srcCols[src].title) + L"\u201D").c_str());
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    }
    if (m_source->CanGroup()) {
        AppendMenuW(menu, MF_STRING | (m_source->Grouping() ? MF_CHECKED : 0), kGroups, Tr(L"Show groups").c_str());
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    }
    for (size_t i = 0; i < srcCols.size(); ++i) {
        if (srcCols[i].title.empty()) continue;
        const bool shown = i < m_hidden.size() ? m_hidden[i] == 0 : !srcCols[i].hidden;
        UINT flags = MF_STRING | (shown ? MF_CHECKED : 0);
        if ((AlbumColumn() && i == 0) || (shown && cols.size() <= 1)) flags |= MF_GRAYED;  // keep one
        AppendMenuW(choose, flags, kColumn + i, srcCols[i].title.c_str());
    }
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(choose), L"Choose &columns");
    AppendMenuW(menu, MF_STRING, kReset, L"&Reset columns");
    POINT screen = pt;
    ClientToScreen(owner, &screen);
    const UINT cmd = TrackWin7Menu(menu, TPM_RETURNCMD | TPM_NONOTIFY, screen.x, screen.y, owner);
    DestroyMenu(menu);  // also destroys the submenu
    if (cmd == kSort) {
        SortBy(src);
    } else if (cmd == kGroups) {
        const bool show = !m_source->Grouping();
        cfg_showGroups.set(show ? 1 : 0);
        m_source->SetGrouping(show);
        Refresh();
        if (onPresentationChanged) onPresentationChanged();
    } else if (cmd == kReset) {
        Store().erase(m_columnKey);
        Persist();
        m_columnKey.clear();
        Refresh();  // reloads the defaults
    } else if (cmd >= kColumn && cmd < kColumn + srcCols.size()) {
        const size_t i = cmd - kColumn;
        if (m_hidden.size() < srcCols.size()) m_hidden.resize(srcCols.size(), 0);
        m_hidden[i] = m_hidden[i] ? 0 : 1;
        SaveColumnLayout();
        Refresh();  // the cover column changes the row height
    }
}

// Text the list shows cut short under the pointer (a tile's line, a cell): its whole text and
// where it stands, in client px, for the window's tooltip (owner, 2026-11-14).
bool ListView::CutTextAt(POINT pt, std::wstring* text, RECT* rect) const {
    if (!m_source || m_drag != Drag::None) return false;
    const Hit hit = HitAt(pt);
    if (hit.part != Part::Row || hit.row >= m_source->Count()) return false;
    const D2D1_POINT_2F c = ContentPoint(pt);
    const RECT rr = RowsRect();
    const auto toClient = [&](D2D1_RECT_F r) {
        return RECT{rr.left + static_cast<LONG>(std::floor((r.left - ScrollX()) * m_scale)),
                    rr.top + static_cast<LONG>(std::floor((r.top - ContentTopDip()) * m_scale)),
                    rr.left + static_cast<LONG>(std::ceil((r.right - ScrollX()) * m_scale)),
                    rr.top + static_cast<LONG>(std::ceil((r.bottom - ContentTopDip()) * m_scale))};
    };
    const auto check = [&](const std::wstring& t, D2D1_RECT_F box) {
        if (t.empty() || c.x < box.left || c.x >= box.right || c.y < box.top || c.y >= box.bottom) return false;
        if (TextWidth(t, 100000) <= box.right - box.left - 1) return false;
        *text = t;
        *rect = toClient(box);
        return true;
    };
    const size_t line = LineOfRow(hit.row);
    const float y = LineTopDip(line);
    if (m_mode == Mode::Tiles) {
        // The tile's style and left edge, wherever its group puts it (a shelf slides).
        media::TileStyle style = m_source->Tiles();
        float x = 0;
        if (m_tileGroups) {
            if (line >= m_lines.size() || TileAtLine(m_lines[line], c.x, &x) != hit.row) return false;
            style = GroupTiles(m_lines[line].group);
        } else {
            x = TileLeft(hit.row % TileColumns());
        }
        const D2D1_SIZE_F ts = TileSizeOf(style);
        const media::TileInfo info = m_source->Tile(hit.row);
        if (style == media::TileStyle::Song) {
            const float tx = x + 8 + kSongTileArt + 10, mid = y + ts.height / 2;
            return check(info.lines[0], {tx, mid - 18, x + ts.width - 6, mid}) ||
                   check(info.lines[1], {tx, mid, x + ts.width - 6, mid + 18});
        }
        if (style == media::TileStyle::Artist) {
            const float a = kArtistTileArt, ay = y + (ts.height - a) / 2, tx = x + 10 + a + 12;
            for (size_t k = 0; k < info.lines.size(); ++k)
                if (check(info.lines[k], {tx, ay + 2 + k * 18, x + ts.width - 6, ay + 2 + (k + 1) * 18})) return true;
            return false;
        }
        const float a = kAlbumTileArt, ay = y + 10;
        for (size_t k = 0; k < 2; ++k)
            if (check(info.lines[k], {x + 6, ay + a + 5 + k * 17, x + ts.width - 6, ay + a + 5 + (k + 1) * 17}))
                return true;
        return false;
    }
    const float rowH = m_mode == Mode::Grouped && line < m_lines.size() ? LineHeightDip(m_lines[line]) : RowHeightPx() / m_scale;
    for (const Col& col : Columns(RowsWidth())) {
        if (c.x < col.x || c.x >= col.x + col.w) continue;
        if (KindOf(m_source->Columns()[col.src]) != CellKind::Text) return false;
        return check(m_source->Cell(hit.row, col.src), {col.x + kCellPad, y, col.x + col.w - kCellPad, y + rowH});
    }
    return false;
}

void ListView::UpdateMarquee() {
    if (!m_source) return;
    const float x0 = std::min(m_marqueeFrom.x, m_marqueeTo.x), x1 = std::max(m_marqueeFrom.x, m_marqueeTo.x);
    const float y0 = std::min(m_marqueeFrom.y, m_marqueeTo.y), y1 = std::max(m_marqueeFrom.y, m_marqueeTo.y);
    // Lines the band touches vertically, then whatever of each line it touches horizontally.
    const size_t first = LineAtDip(std::max(0.0f, y0));
    const size_t last = LineAtDip(std::max(0.0f, y1));
    std::vector<size_t> rows = m_marqueeBase;
    const float right = ContentWidth();
    switch (m_mode) {
    case Mode::Tiles: {
        const size_t cols = TileColumns();
        const float tw = TileSize().width;
        for (size_t line = first; line <= last; ++line)
            for (size_t c = 0; c < cols; ++c) {
                const float tx0 = TileLeft(c), tx1 = tx0 + tw;
                size_t idx = line * cols + c;
                if (m_tileGroups) {
                    if (line >= m_lines.size() || m_lines[line].kind != Line::Item) break;
                    idx = m_lines[line].row + c;
                    if (idx >= TileGroupEnd(m_lines[line].group)) break;
                }
                if (idx < m_source->Count() && x1 >= tx0 && x0 < tx1) rows.push_back(idx);
            }
        break;
    }
    case Mode::Grouped: {
        float left = 0;
        if (AlbumColumn()) {
            const auto cols = Columns(0);
            left = cols[0].x + cols[0].w;
        }
        if (x1 < left || x0 >= right) break;
        for (size_t line = first; line <= last && line < m_lines.size(); ++line)
            if (m_lines[line].kind == Line::Item) rows.push_back(m_lines[line].row);
        break;
    }
    default:
        if (x1 < 0 || x0 >= right) break;
        for (size_t r = first; r <= last && r < m_source->Count(); ++r) rows.push_back(r);
        break;
    }
    std::sort(rows.begin(), rows.end());
    rows.erase(std::unique(rows.begin(), rows.end()), rows.end());
    m_source->SetSelection(rows);
}

void ListView::OnWheel(int delta, bool horizontal) {
    if (horizontal) {  // Shift+wheel or a tilt wheel: sideways, no scrollbar shown
        // Over a shelf it moves the shelf, 100 DIP a notch, gliding (MX Master's thumb wheel).
        if (m_mode == Mode::Tiles && m_tileGroups && !m_shelfUnder.empty() && m_source && m_source->Groups()) {
            const auto& groups = *m_source->Groups();
            for (size_t gi = 0; gi < groups.size(); ++gi) {
                if (groups[gi].key != m_shelfUnder || !groups[gi].carousel) continue;
                const float pitch = TilePitchOf(GroupTiles(gi), true);
                const bool running = m_shelfSlide.group == gi && ShelfShownPos(gi) != ShelfPos(gi);
                const float base = running ? m_shelfSlide.to : ShelfPos(gi);
                SlideShelf(gi, base - static_cast<float>(delta) / WHEEL_DELTA * 100.0f / pitch, kShelfWheelMs);
                return;
            }
        }
        if (m_mode != Mode::Tiles) ScrollXTo(m_scrollX - delta / static_cast<float>(WHEEL_DELTA) * 48.0f);
        return;
    }
    // As browsers do: 100 DIP a notch for Windows' three lines (a page for "one screen"),
    // fractions for fine wheels, added to the glide still running.
    UINT lines = 3;
    SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
    const float notch = lines == WHEEL_PAGESCROLL ? RowsHeightDip() * 0.875f : 100.0f * static_cast<float>(lines) / 3.0f;
    const float from = Gliding() ? m_glide.to : ContentTopDip();
    GlideTo(from - static_cast<float>(delta) / WHEEL_DELTA * notch);
}

void ListView::MoveFocus(size_t row, bool ctrl, bool shift) {
    if (!m_source || row >= m_source->Count()) return;
    if (shift && m_anchor != SIZE_MAX && m_anchor < m_source->Count()) {
        m_source->SelectRange(m_anchor, row, ctrl);
    } else if (ctrl) {
        m_source->Toggle(row);
        m_anchor = row;
    } else {
        m_source->SelectOnly(row);
        m_anchor = row;
    }
    m_source->SetFocus(row);
    EnsureVisible(row);
    PaintRows();
}

bool ListView::OnKeyDown(UINT vk, bool ctrl, bool shift) {
    if (!m_source) return false;
    const size_t n = m_source->Count();
    const size_t focus = m_source->Focus();
    // In the icon view up / down move by a whole row of tiles.
    const size_t step = m_mode == Mode::Tiles ? TileColumns() : 1;
    const size_t page = (std::max<size_t>(VisibleLines(), 2) - 1) * step;
    auto go = [&](size_t row) {
        if (n == 0) return;
        MoveFocus(std::min(row, n - 1), false, shift);
    };
    auto back = [&](size_t by) { return focus == SIZE_MAX ? 0 : (focus > by ? focus - by : 0); };
    auto fwd = [&](size_t by) { return focus == SIZE_MAX ? 0 : focus + by; };
    switch (vk) {
    case VK_UP: go(back(step)); return true;
    case VK_DOWN: go(fwd(step)); return true;
    case VK_LEFT:
        if (m_mode != Mode::Tiles) return false;
        go(back(1));
        return true;
    case VK_RIGHT:
        if (m_mode != Mode::Tiles) return false;
        go(fwd(1));
        return true;
    case VK_PRIOR: go(back(page)); return true;
    case VK_NEXT: go(fwd(page)); return true;
    case VK_HOME: go(0); return true;
    case VK_END: go(n ? n - 1 : 0); return true;
    case VK_RETURN:
        if (focus != SIZE_MAX && focus < n) m_source->Activate(focus);
        PaintRows();
        return true;
    case VK_DELETE:
        if (m_source->DeleteSelected()) Refresh();
        return true;
    case 'A':
        if (ctrl) {
            m_source->SelectAll();
            PaintRows();
            return true;
        }
        return false;
    default:
        return false;
    }
}

}  // namespace glasswave::ui
