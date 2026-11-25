#include "pch.h"
#include "ui/player_window_parts.h"

// The player window's seek bar and times, and the sung line under the song.
namespace glasswave::ui {

using namespace playerwin;

namespace {

constexpr float kSeekInset = 6; // DIP at either end of the seek rectangle: room for the thumb

constexpr float kSeekThumbW = 16, kSeekThumbH = 9; // WMP's seek thumb (4058 / 3884 / 3883)

constexpr int kSeekThumbInMs = 160, kSeekThumbOutMs = 280;

constexpr GlassShape kPillShape{4.5f, 11.5f, 4.5f, 4.5f, 4};

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

// The seek thumb at (0, 0), 16x9 DIP; `state` 0 at rest, 1 lit, 2 held.
void PaintSeekThumb(ID2D1DeviceContext* dc, int state) {
    PaintGlass(dc, kPillShape, kPillLooks[std::clamp(state, 0, 2)]);
}

} // namespace

bool PlayerWindow::LyricLineLive() const {
    return m_shown && m_hwnd && !IsIconic(m_hwnd) && m_playing && !m_paused && m_lyrics && m_lyrics->synced &&
           !m_paneShown && settings::LyricLineShown();
}

// The seek bar's head and the times follow the music on the same frames, a device pixel and a
// second at a time, instead of jumping with the playback time callback once a second. Only
// while the control bar is up: away, the once-a-second callback is all that is drawn.
bool PlayerWindow::SeekLive() const {
    return m_shown && m_hwnd && !IsIconic(m_hwnd) && m_chromeShown && m_playing && !m_paused &&
           m_length > 0 && !Empty(m_layout.seek);
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

// Index of the lyric line being sung (-1: none), and the position in milliseconds.
int PlayerWindow::CurrentLyricLine(int* ms) const {
    double now = m_position;
    try {
        metadb_handle_ptr playing;
        if (auto pc = playback_control::get();
            pc->is_playing() && pc->get_now_playing(playing) && playing == m_track)
            now = pc->playback_get_position();
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
    const auto curve = gfx::Easing({0.3f, 0.0f, 0.1f, 1.0f});
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

void PlayerWindow::OnLyricLineSetting() {
    if (!m_shown || m_client.cx <= 0) return;
    m_lyricLine = -2;
    UpdateLayout(false); // the line's room comes or goes with it
    UpdateMotion();
}

float PlayerWindow::SeekFractionAt(int x) const {
    const RECT& r = m_layout.seek;
    const float x0 = r.left + Px(kSeekInset + 2), x1 = r.right - Px(kSeekInset + 2); // where the head runs
    return std::clamp((x - x0) / std::max(1.0f, x1 - x0), 0.0f, 1.0f);
}

} // namespace glasswave::ui
