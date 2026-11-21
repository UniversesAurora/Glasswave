#include "pch.h"
#include "ui/list_view_parts.h"

// The list view's icon views: tiles, shelves with their arrows, the card layer's place and
// the covers.
namespace glasswave::ui {

using namespace listview;

namespace {

constexpr float kSourceBadge = 18;  // DIP: a mixed result's source mark on its cover

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

} // namespace

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

} // namespace glasswave::ui
