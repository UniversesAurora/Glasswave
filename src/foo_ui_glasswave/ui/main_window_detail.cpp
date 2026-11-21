#include "pch.h"
#include "ui/main_window_parts.h"

// The detail band over artist, album and playlist pages: its painting, facts and the
// description's scroll bar.
namespace glasswave::ui {

using namespace mainwin;

namespace {

// The subject's facts as Windows 7's details pane lists properties: "Label: value" rows of the
// band's 12 DIP type, in up to three columns of at least this width, filled downwards.
constexpr float kDetailFactRowH = 18, kDetailFactColumnMinW = 220, kDetailFactGap = 6, kDetailFactColumnGap = 18;

constexpr float kDetailTextTop = 62; // below the name and the stats line

constexpr float kDetailStatsLine = 20; // the stats line's share of it (none: the facts move up)

} // namespace

void MainWindow::PaintDetailHeader() {
    if (!m_detailHeader || !m_detail.visible) return;
    const RECT rect = m_layout.detailHeader;
    const int w = rect.right - rect.left, h = rect.bottom - rect.top;
    if (w <= 0 || h <= 0) return;
    m_detailHeaderSize = {w, h};

    winrt::com_ptr<IWICBitmapSource> art;
    if (!m_detail.artKey.empty() && m_detail.artItem.is_valid())
        ArtCache::Get().Find(m_detail.artKey, m_detail.artItem, PxI(kDetailArt), &art);

    auto& host = gfx::CompositionHost::Get();
    const float s = m_scale;
    host.Repaint(m_detailHeader, w, h, s, [&](ID2D1DeviceContext* dc) {
        const float W = w / s, H = h / s;
        const float px = LinePx() / s;
        dc->FillRectangle({-1, 0, W + 1, H}, VGrad(dc, 0, H, {{0.0f, 0xF6FBFF}, {1.0f, 0xEBF3FC}}).get());
        com_ptr<ID2D1SolidColorBrush> rule;
        dc->CreateSolidColorBrush(Hex(0xC3CCCF), rule.put());
        dc->FillRectangle({0, H - px, W, H}, rule.get());

        const float pad = 18, side = kDetailArt;
        const float ax = pad, ay = 14; // the artwork stays at the top-left of the band
        const bool made = !m_detail.artBadge.empty() || !m_detail.artCaption.empty();
        if (art) {
            winrt::com_ptr<ID2D1Bitmap> bitmap;
            if (SUCCEEDED(dc->CreateBitmapFromWicBitmap(art.get(), nullptr, bitmap.put())))
                dc->DrawBitmap(bitmap.get(), D2D1::RectF(ax, ay, ax + side, ay + side), 1.0f,
                               D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        } else if (made) {
            dc->FillRectangle({ax, ay, ax + side, ay + side},
                              VGrad(dc, ay, ay + side, {{0.0f, 0x5B7DB8}, {1.0f, 0x2A3F66}}).get());
        } else if (!win7::DrawStretched(dc, win7::Module::Wmploc, L"ALBUM_0.PNG", {ax, ay, ax + side, ay + side})) {
            // WMP's own "no album art" picture, as the tiles show it; a plain plate without it.
            wp::RowPlate(dc, {ax, ay, ax + side, ay + side}, wp::RowState::Normal, false);
        }
        if (made) { // the made cover: a shade from the top and the foot, the mark and the caption
            const D2D1_GRADIENT_STOP stops[] = {{0.0f, D2D1::ColorF(0, 0.38f)}, {0.42f, D2D1::ColorF(0, 0.0f)},
                                                {0.58f, D2D1::ColorF(0, 0.0f)}, {1.0f, D2D1::ColorF(0, 0.55f)}};
            com_ptr<ID2D1GradientStopCollection> list;
            com_ptr<ID2D1LinearGradientBrush> shade;
            dc->CreateGradientStopCollection(stops, 4, list.put());
            if (list && SUCCEEDED(dc->CreateLinearGradientBrush({{0, ay}, {0, ay + side}}, list.get(), shade.put())))
                dc->FillRectangle({ax, ay, ax + side, ay + side}, shade.get());
            com_ptr<ID2D1SolidColorBrush> white, shadow;
            dc->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), white.put());
            dc->CreateSolidColorBrush(D2D1::ColorF(0, 0.35f), shadow.put());
            if (!m_detail.artBadge.empty()) {
                auto badge = MakeText(m_detail.artBadge, side * 0.36f, DWRITE_FONT_WEIGHT_SEMI_BOLD, side, side * 0.5f);
                dc->DrawTextLayout({ax + 8, ay + 3}, badge.get(), shadow.get());
                dc->DrawTextLayout({ax + 7, ay + 2}, badge.get(), white.get());
            }
            if (!m_detail.artCaption.empty()) {
                auto caption = MakeText(m_detail.artCaption, 12.0f, DWRITE_FONT_WEIGHT_BOLD, side - 14, 18);
                dc->DrawTextLayout({ax + 7, ay + side - 22}, caption.get(), white.get());
            }
        }
        com_ptr<ID2D1SolidColorBrush> edge;
        dc->CreateSolidColorBrush(Hex(0x9AA7B8), edge.put());
        dc->DrawRectangle({ax + 0.5f, ay + 0.5f, ax + side - 0.5f, ay + side - 0.5f}, edge.get(), 1.0f);

        com_ptr<ID2D1SolidColorBrush> ink, sub, faint;
        dc->CreateSolidColorBrush(Hex(0x1E395B), ink.put());
        dc->CreateSolidColorBrush(Hex(0x3E5878), sub.put());
        dc->CreateSolidColorBrush(Hex(0x6B7B8E), faint.put());
        const float textX = ax + side + 18;
        const float textW = DetailHeaderTextWidth();
        auto nameLayout = MakeText(m_detail.title, 20.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, textW, 30);
        dc->DrawTextLayout({textX, 10}, nameLayout.get(), ink.get());
        if (!m_detail.subtitle.empty()) {
            auto statsLayout = MakeText(m_detail.subtitle, 12.0f, DWRITE_FONT_WEIGHT_NORMAL, textW, 18);
            dc->DrawTextLayout({textX, 42}, statsLayout.get(), sub.get());
        }
        DrawDetailFacts(dc, textX, DetailTextTop(), textW);
        if (!m_detail.tagline.empty()) {
            const float top = DetailTextTop() + DetailFactsHeight(textW);
            auto tagline = MakeWrappedText(m_detail.tagline, 12.0f, DWRITE_FONT_WEIGHT_NORMAL, textW,
                                           DetailTaglineHeight(textW));
            dc->DrawTextLayout({textX, top}, tagline.get(), sub.get());
        }
        const float briefTop = m_detailBriefTop;
        if (!m_detail.brief.empty()) {
            const float drawW = m_detailBriefTextW > 0 ? m_detailBriefTextW : textW;
            if (m_detailBriefScrollable) {
                const D2D1_RECT_F clip{textX, briefTop, textX + drawW, briefTop + m_detailBriefViewH};
                dc->PushAxisAlignedClip(clip, D2D1_ANTIALIAS_MODE_ALIASED);
                auto full = MakeWrappedText(m_detail.brief, 12.0f, DWRITE_FONT_WEIGHT_NORMAL, drawW, 4000.0f);
                dc->DrawTextLayout({textX, briefTop - m_detailBriefScroll}, full.get(), faint.get());
                dc->PopAxisAlignedClip();
            } else {
                // The collapsed text is already three lines at most: it gets their whole height.
                // A 48 DIP box dropped the third line (each is a hair over 16 DIP), and Show more
                // then stood alone after the line that was not drawn (owner, 2026-11-14).
                const bool collapsed = !m_detailBriefExpanded && !m_detailBriefCollapsed.empty();
                const float maxH = m_detailBriefExpanded ? std::max(24.0f, m_detailBriefViewH) : collapsed ? 4000.0f : 48.0f;
                const std::wstring& shown = collapsed ? m_detailBriefCollapsed : m_detail.brief;
                auto briefLayout = MakeWrappedText(shown, 12.0f, DWRITE_FONT_WEIGHT_NORMAL, drawW, maxH);
                dc->DrawTextLayout({textX, briefTop}, briefLayout.get(), faint.get());
            }
            if (m_detailBriefScrollable && m_layout.detailBriefBar.right > m_layout.detailBriefBar.left) {
                const RECT& bar = m_layout.detailBriefBar;
                const float barX = (bar.left - rect.left) / s;
                const float barY = (bar.top - rect.top) / s;
                const float barH = (bar.bottom - bar.top) / s;
                float thumbTop = 0, thumbH = 0;
                if (DetailBriefThumb(&thumbTop, &thumbH)) {
                    // Keep the surface's DPI transform; replacing it would draw the bar at
                    // device scale and make it jump around at 125 %/150 %.
                    D2D1_MATRIX_3X2_F base{};
                    dc->GetTransform(&base);
                    const D2D1::Matrix3x2F baseM(base._11, base._12, base._21, base._22, base._31, base._32);
                    dc->SetTransform(D2D1::Matrix3x2F::Translation(barX, barY) * baseM);
                    wp::ScrollBar(dc, {kDetailScrollWidth, barH}, thumbTop, thumbH,
                                  m_briefScroll.Blend());
                    dc->SetTransform(baseM);
                }
            }
        }
    });
    Place(m_detailHeader, rect); // Repaint resets the visual to its surface origin
    LogDetail(art != nullptr);
}

// The band's text, written to the log when it changes, so what a page shows from a service can
// be read back without looking at the screen (see ui/dev_commands.cpp).
void MainWindow::LogDetail(bool art) {
    std::wstring text = L"page: " + m_detail.title + (art ? L" [art]" : L" [no art]");
    if (!m_detail.subtitle.empty()) text += L" | " + m_detail.subtitle;
    for (const auto& fact : m_detail.facts) text += L" | " + fact.label + L": " + fact.value;
    if (!m_detail.tagline.empty()) text += L" | tagline: " + m_detail.tagline;
    if (m_source) {
        text += L" | rows: " + std::to_wstring(m_source->Count());
        if (const auto* groups = m_source->Groups())
            for (const auto& group : *groups)
                text += L" | " + group.header + L" (" + std::to_wstring(group.count) + L")";
    }
    if (!m_detail.brief.empty())
        text += L" | brief: " + std::to_wstring(m_detail.brief.size()) + L" chars, \"" +
                m_detail.brief.substr(0, 24) + L"...\"";
    if (text == m_loggedDetail) return;
    m_loggedDetail = text;
    core::Log(text);
}

void MainWindow::RepaintDetailHeader() {
    if (!m_detailHeader) return;
    m_detail = m_page ? m_page->Detail() : DetailInfo{};
    if (!m_detail.visible) return;
    PaintDetailHeader();
    if (Widget* action = WidgetById(kDetailAction)) action->Invalidate();
    if (Widget* brief = WidgetById(kDetailBriefToggle)) brief->Invalidate();
}

// Without a stats line (an artist the service gives no counts for) the facts close up under
// the name instead of leaving its row empty (owner, 2026-11-14).
float MainWindow::DetailTextTop() const {
    return m_detail.subtitle.empty() ? kDetailTextTop - kDetailStatsLine : kDetailTextTop;
}

float MainWindow::DetailHeaderTextWidth() const {
    const Layout& L = m_layout;
    const float s = m_scale;
    const float textX = 18 + kDetailArt + 18;
    const float right = m_detail.action != DetailInfo::Action::None
                            ? (L.detailAction.left - L.detailHeader.left) / s - 18.0f
                            : (L.detailHeader.right - L.detailHeader.left) / s - 18.0f;
    return std::max(120.0f, right - textX);
}

size_t MainWindow::DetailFactColumns(float textW) const {
    const size_t n = m_detail.facts.size();
    if (n == 0) return 0;
    const auto fit = static_cast<size_t>(std::clamp(static_cast<int>(textW / kDetailFactColumnMinW), 1, 3));
    return std::min(n, fit);
}

float MainWindow::DetailFactsHeight(float textW) const {
    const size_t columns = DetailFactColumns(textW);
    if (columns == 0) return 0;
    const size_t rows = (m_detail.facts.size() + columns - 1) / columns;
    return rows * kDetailFactRowH + 4;
}

float MainWindow::DetailTaglineHeight(float textW) const {
    if (m_detail.tagline.empty()) return 0;
    auto layout = MakeWrappedText(m_detail.tagline, 12.0f, DWRITE_FONT_WEIGHT_NORMAL, textW, 2 * kDetailFactRowH);
    DWRITE_TEXT_METRICS metrics{};
    layout->GetMetrics(&metrics);
    return std::min(std::ceil(metrics.height), 2 * kDetailFactRowH) + 6;
}

// The facts in columns: each column's labels start at one line and its values at another
// (owner, 2026-11-14: right-aligned labels made the rows look crooked); long values end in an
// ellipsis. A column takes the width its text needs; when they do not all fit, the narrow
// ones keep theirs and the wide ones share the rest equally.
void MainWindow::DrawDetailFacts(ID2D1DeviceContext* dc, float x, float y, float textW) {
    m_detailCut.clear();
    const size_t columns = DetailFactColumns(textW);
    if (columns == 0) return;
    const size_t n = m_detail.facts.size();
    const size_t rows = (n + columns - 1) / columns;
    const auto width = [](const std::wstring& text) {
        DWRITE_TEXT_METRICS metrics{};
        MakeText(text, 12.0f)->GetMetrics(&metrics);
        return std::ceil(metrics.widthIncludingTrailingWhitespace);
    };
    std::vector<float> labelW(columns, 0), need(columns, 0);
    for (size_t c = 0; c < columns; ++c) {
        float valueW = 0;
        for (size_t i = c * rows; i < std::min(n, (c + 1) * rows); ++i) {
            labelW[c] = std::max(labelW[c], width(m_detail.facts[i].label + L":"));
            valueW = std::max(valueW, width(m_detail.facts[i].value));
        }
        need[c] = labelW[c] + kDetailFactGap + valueW;
    }
    // Water-filling: share what is left among the columns that need more than their share.
    std::vector<float> columnW = need;
    float room = textW - kDetailFactColumnGap * (columns - 1);
    std::vector<bool> settled(columns, false);
    for (size_t open = columns; open > 0;) {
        const float share = room / open;
        bool changed = false;
        for (size_t c = 0; c < columns; ++c)
            if (!settled[c] && need[c] <= share) {
                settled[c] = true;
                room -= need[c];
                --open;
                changed = true;
            }
        if (!changed) {
            for (size_t c = 0; c < columns; ++c)
                if (!settled[c]) columnW[c] = share;
            break;
        }
    }
    // A label is never cut (owner, 2026-11-14: "唱片公..." said nothing); its value keeps at
    // least a few characters.
    for (size_t c = 0; c < columns; ++c)
        columnW[c] = std::max(columnW[c], std::min(need[c], labelW[c] + kDetailFactGap + 60));
    com_ptr<ID2D1SolidColorBrush> label, value;
    dc->CreateSolidColorBrush(Hex(0x6B7B8E), label.put());
    dc->CreateSolidColorBrush(Hex(0x1E395B), value.put());
    float cx = x;
    for (size_t c = 0; c < columns; ++c) {
        const float nameW = labelW[c];
        const float valueW = std::max(1.0f, columnW[c] - nameW - kDetailFactGap);
        for (size_t i = c * rows; i < std::min(n, (c + 1) * rows); ++i) {
            const float top = y + (i - c * rows) * kDetailFactRowH;
            auto name = MakeText(m_detail.facts[i].label + L":", 12.0f, DWRITE_FONT_WEIGHT_NORMAL, nameW, kDetailFactRowH);
            dc->DrawTextLayout({cx, top}, name.get(), label.get());
            const float vx = cx + nameW + kDetailFactGap;
            auto text = MakeText(m_detail.facts[i].value, 12.0f, DWRITE_FONT_WEIGHT_NORMAL, valueW, kDetailFactRowH);
            dc->DrawTextLayout({vx, top}, text.get(), value.get());
            if (width(m_detail.facts[i].value) > valueW + 0.5f) {
                // Cut short: the tooltip shows it whole (UpdateDetailTip).
                const RECT& band = m_layout.detailHeader;
                m_detailCut.push_back({{band.left + PxI(vx), band.top + PxI(top), band.left + PxI(vx + valueW),
                                        band.top + PxI(top + kDetailFactRowH)},
                                       m_detail.facts[i].value});
            }
        }
        cx += columnW[c] + kDetailFactColumnGap;
    }
}

void MainWindow::OnDetailAction() {
    if (m_page) m_page->DetailAction();
}

// The description's bar measures in DIP, its arrows 17 tall, its thumb at least 24.
ScrollBarModel::Geometry MainWindow::DetailBriefGeometry() const {
    return {m_detailBriefViewH, 17.0f, 24.0f, m_detailBriefContentH, m_detailBriefViewH, m_detailBriefScroll};
}

bool MainWindow::DetailBriefThumb(float* top, float* height) const {
    return m_detailBriefScrollable && ScrollBarModel::Thumb(DetailBriefGeometry(), top, height);
}

ScrollBarModel::Part MainWindow::DetailBriefPartAt(POINT pt) const {
    if (!m_detailBriefScrollable || !PtInRect(&m_layout.detailBriefBar, pt)) return ScrollBarModel::kNone;
    return ScrollBarModel::PartAt(DetailBriefGeometry(), (pt.y - m_layout.detailBriefBar.top) / m_scale);
}

void MainWindow::ScrollDetailBrief(float delta) {
    const float maxScroll = std::max(0.0f, m_detailBriefContentH - m_detailBriefViewH);
    const float next = std::clamp(m_detailBriefScroll + delta, 0.0f, maxScroll);
    if (next == m_detailBriefScroll) return;
    m_detailBriefScroll = next;
    RepaintDetailHeader();
}

void MainWindow::UpdateDetailBriefHover(POINT pt) {
    if (m_briefScroll.SetHot(DetailBriefPartAt(pt))) StartScrollAnimation();
}

// The description scrolls a line of 54 DIP per arrow step and its height less two lines per page.
float MainWindow::DetailBriefPage() const { return std::max(18.0f, m_detailBriefViewH - 36.0f); }

void MainWindow::OnDetailBriefBarDown(POINT pt) {
    const ScrollBarModel::Part part = DetailBriefPartAt(pt);
    const float y = (pt.y - m_layout.detailBriefBar.top) / m_scale;
    if (const float step = m_briefScroll.Press(part, y, DetailBriefGeometry(), 54.0f, DetailBriefPage()); step != 0)
        ScrollDetailBrief(step);
    if (float top = 0, height = 0; part == ScrollBarModel::kThumb && DetailBriefThumb(&top, &height))
        m_detailBriefDragOffset = pt.y - m_layout.detailBriefBar.top - PxI(top);
    StartScrollAnimation();
    if (part == ScrollBarModel::kUp || part == ScrollBarModel::kDown || part == ScrollBarModel::kTrack)
        StartScrollRepeat();
}

void MainWindow::OnDetailBriefBarMove(POINT pt) {
    if (m_briefScroll.Pressed() != ScrollBarModel::kThumb) return;
    float top = 0, height = 0;
    if (!DetailBriefThumb(&top, &height)) return;
    const float arrow = 17.0f;
    const float trackH = m_detailBriefViewH - arrow * 2;
    if (trackH <= height) return;
    const float y =
        (pt.y - m_layout.detailBriefBar.top) / m_scale - arrow - m_detailBriefDragOffset / m_scale;
    const float maxScroll = std::max(0.0f, m_detailBriefContentH - m_detailBriefViewH);
    const float frac = std::clamp(y / (trackH - height), 0.0f, 1.0f);
    const float next = frac * maxScroll;
    if (next != m_detailBriefScroll) {
        m_detailBriefScroll = next;
        RepaintDetailHeader();
    }
}

void MainWindow::OnDetailBriefBarUp() {
    if (m_briefScroll.Release()) StartScrollAnimation();
}

bool MainWindow::AnimateDetailScrollBar() {
    bool changed = false;
    const bool active = m_briefScroll.Animate(&changed);
    if (changed) RepaintDetailHeader();
    return active;
}

bool MainWindow::OnDetailBriefRepeat() {
    if (!m_detailBriefScrollable) return false;
    const float step = m_briefScroll.Repeat(DetailBriefGeometry(), 54.0f, DetailBriefPage());
    if (step == 0) return false;
    ScrollDetailBrief(step);
    return true;
}

// A detail value cut short shows in full in the tooltip once the pointer rests on it for the
// system's tooltip delay (the double-click time, as Windows' own tooltips).
// The list's own text cut short (a tile's title, a cell) shows the same way.
void MainWindow::UpdateDetailTip(POINT pt) {
    std::wstring text;
    RECT rect{};
    if (m_detail.visible)
        for (const auto& cut : m_detailCut)
            if (PtInRect(&cut.rect, pt)) {
                text = cut.text;
                rect = cut.rect;
            }
    bool above = false;
    for (const auto* cuts : {&m_npCut, &m_lyricCut, &m_streamCut})
        for (const auto& cut : *cuts)
            if (text.empty() && PtInRect(&cut.rect, pt)) {
                text = cut.text;
                rect = cut.rect;
                above = true; // the control bar is at the window's bottom
            }
    if (text.empty() && m_list && m_list->HitTest(pt)) m_list->CutTextAt(pt, &text, &rect);
    if (text == m_tipText && EqualRect(&rect, &m_tipRect)) return;
    m_tipText = std::move(text);
    m_tipRect = rect;
    m_tipAbove = above;
    KillTimer(m_hwnd, kDetailTipTimer);
    if (!m_volumeHover) HideTip();
    if (!m_tipText.empty()) {
        m_detailTipAt = pt;
        SetTimer(m_hwnd, kDetailTipTimer, GetDoubleClickTime(), nullptr);
    }
}

} // namespace glasswave::ui
