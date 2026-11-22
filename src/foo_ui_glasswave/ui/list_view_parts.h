#pragma once

// What ListView's source files share (ui/list_view*.cpp only): the includes, aliases,
// measurements, colours and painting helpers.

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

namespace listview {

constexpr float kRowHeight = 22.0f;

constexpr float kScrollArrow = 17.0f;

constexpr float kGutter = 18.0f;  // playing marker column

constexpr float kCellPad = 6.0f;

constexpr size_t kGroupMinLines = 4;  // the album column needs four lines (cover + 4 text lines)

constexpr float kTilePad = 8.0f;

constexpr float kScrollStep = 48.0f; // DIP: a scroll bar arrow's step

constexpr size_t kMoreLink = 100;          // Hit::line of a group header's `more` link

constexpr float kAlbumTileArt = 100;

constexpr float kArtistTileArt = 88;

constexpr float kSongTileArt = 44;

constexpr float kShowAll = 19;         // DIP: a shelf's "show all" (aero's expando button, 19 px at 96 DPI)

// A featured card's tile: the card and room around it for its lifted shadow (ui/card_layer.h).
constexpr D2D1_POINT_2F kCardInset{10, 12}; // 20 DIP between cards, as Apple Music's

constexpr D2D1_SIZE_F kCardTile{CardLayer::kCard.width + 2 * kCardInset.x, CardLayer::kCard.height + 30};

constexpr uint32_t kInk = 0x1E1E1E, kSecondaryInk = 0x4A4A4A, kGreyInk = 0x7A7A7A, kPlayingInk = 0x1E5BA8;

constexpr uint32_t kGroupInk = 0x2E64B4, kGroupRule = 0xC9D7E8, kLinkInk = 0x0066CC;

// {5E8B2C47-9A1D-4F63-B0E2-6C4A7D9F1B38}: column order and widths per column set
extern cfg_var_modern::cfg_string cfg_columns;

// {5E8B2C48-9A1D-4F63-B0E2-6C4A7D9F1B38}: split a sorted Details list into sections
extern cfg_var_modern::cfg_int cfg_showGroups;

// Columns drawn as controls instead of text.
enum class CellKind { Text, Rating, Favorite, Art, Popularity, Progress, Download };

inline CellKind KindOf(const media::Column& c) {
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
    size_t sortColumn = SIZE_MAX;
    bool sortDescending = false;
};

inline std::map<std::wstring, SavedLayout>& Store() {
    static std::map<std::wstring, SavedLayout> store;
    static bool loaded = false;
    if (!loaded) {
        loaded = true;
        // One line per column set: key \t order \t widths \t hidden \t sortCol:desc
        std::wstringstream in(Wide(cfg_columns.get().c_str()));
        std::wstring line;
        while (std::getline(in, line)) {
            const size_t a = line.find(L'\t'), b = line.find(L'\t', a + 1);
            if (a == std::wstring::npos || b == std::wstring::npos) continue;
            const size_t c = line.find(L'\t', b + 1);
            const size_t d = c == std::wstring::npos ? std::wstring::npos : line.find(L'\t', c + 1);
            SavedLayout s;
            std::wstringstream o(line.substr(a + 1, b - a - 1)),
                w(line.substr(b + 1, c == std::wstring::npos ? std::wstring::npos : c - b - 1));
            for (std::wstring v; std::getline(o, v, L',');) s.order.push_back(static_cast<size_t>(_wtoi(v.c_str())));
            for (std::wstring v; std::getline(w, v, L',');) s.widths.push_back(static_cast<float>(_wtof(v.c_str())));
            if (c != std::wstring::npos) {
                const std::wstring hiddenField =
                    line.substr(c + 1, d == std::wstring::npos ? std::wstring::npos : d - c - 1);
                for (wchar_t ch : hiddenField) s.hidden.push_back(ch == L'1' ? 1 : 0);
            }
            if (d != std::wstring::npos) {
                const std::wstring sortField = line.substr(d + 1);
                const size_t colon = sortField.find(L':');
                if (colon != std::wstring::npos && colon > 0) {
                    const int col = _wtoi(sortField.substr(0, colon).c_str());
                    if (col >= 0) {
                        s.sortColumn = static_cast<size_t>(col);
                        s.sortDescending = sortField.substr(colon + 1) == L"1";
                    }
                }
            }
            store[line.substr(0, a)] = std::move(s);
        }
    }
    return store;
}

inline void Persist() {
    std::wstring out;
    for (const auto& [key, s] : Store()) {
        out += key + L'\t';
        for (size_t i = 0; i < s.order.size(); ++i) out += (i ? L"," : L"") + std::to_wstring(s.order[i]);
        out += L'\t';
        for (size_t i = 0; i < s.widths.size(); ++i)
            out += (i ? L"," : L"") + std::to_wstring(static_cast<int>(std::lround(s.widths[i])));
        out += L'\t';
        for (uint8_t h : s.hidden) out += h ? L'1' : L'0';
        out += L'\t';
        if (s.sortColumn != SIZE_MAX)
            out += std::to_wstring(s.sortColumn) + L':' + (s.sortDescending ? L'1' : L'0');
        out += L'\n';
    }
    cfg_columns.set(Utf8(out).c_str());
}

inline D2D1_COLOR_F Hex(uint32_t rgb, float a = 1.0f) {
    return D2D1::ColorF(((rgb >> 16) & 0xFF) / 255.0f, ((rgb >> 8) & 0xFF) / 255.0f, (rgb & 0xFF) / 255.0f, a);
}

inline winrt::com_ptr<ID2D1SolidColorBrush> Brush(ID2D1DeviceContext* dc, uint32_t rgb, float a = 1.0f) {
    winrt::com_ptr<ID2D1SolidColorBrush> b;
    dc->CreateSolidColorBrush(Hex(rgb, a), b.put());
    return b;
}

inline void Text(ID2D1DeviceContext* dc, const std::wstring& text, D2D1_RECT_F box, uint32_t ink,
          DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING,
          DWRITE_FONT_WEIGHT weight = DWRITE_FONT_WEIGHT_NORMAL, bool underline = false) {
    if (text.empty() || box.right - box.left < 4) return;
    auto layout = MakeText(text, 12.0f, weight, box.right - box.left, box.bottom - box.top, align);
    if (underline) layout->SetUnderline(TRUE, {0, static_cast<UINT32>(text.size())});
    dc->DrawTextLayout({box.left, box.top}, layout.get(), Brush(dc, ink).get());
}

inline float TextWidth(const std::wstring& text, float maxW) {
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

} // namespace listview

} // namespace glasswave::ui
