#include "pch.h"
#include "ui/main_window_parts.h"

// The library window's now-playing band: the song's text, cover, sung line and stream
// information, the output device, the lyrics pane's reveal, and the song on show.
namespace glasswave::ui {

using namespace mainwin;

namespace {

constexpr UINT kArtHoldMs = 1500;

constexpr int kPaneRevealMs = 300;

// Text rows of the transport band (title + artist and the lyric line on the left, stream
// format and output device on the right): 12 DIP Segoe UI on 16 DIP rows, both pairs
// centred on the band like the time between them.
constexpr float kInfoRow = 16.0f;

} // namespace

float MainWindow::NowPlayingWidth() const {
    const float w = (m_layout.nowPlaying.right - m_layout.nowPlaying.left) / m_scale;
    return std::clamp(std::floor(w) - 4, 0.0f, 350.0f);
}

void MainWindow::RefreshNowPlayingArt(bool refreshText) {
    if (m_held.track.is_valid()) { // a song waiting for its cover: shown with it once it has come
        ReleaseHeldTrack(false);
        return;
    }
    auto pc = playback_control::get();
    metadb_handle_ptr now;
    winrt::com_ptr<IWICBitmapSource> art;
    if (pc->get_now_playing(now)) {
        const std::wstring identity = lyrics::Service::TrackIdentity(now);
        // Another song than the one shown comes in through ShowTrack (with its text), never here.
        if (identity != m_artTrackIdentity) return;
        const bool identityChanged = false;
        const std::uint64_t generation = ++m_artGeneration;
        const int sizePx = std::max(PxI(TransportHeight()), PxI(64));
        ArtCache::Get().Find(now, sizePx, &art);
        metadb_handle_ptr current;
        if (generation != m_artGeneration || !pc->get_now_playing(current) ||
            lyrics::Service::TrackIdentity(current) != identity)
            return;
        if (!identityChanged && !refreshText && art.get() == m_art.get()) return;
        m_art = std::move(art);
        m_lyricsArt = m_art;
    } else {
        // Between songs (or an output restart) the last one stays shown whole; a stop clears it
        // (ClearShownTrack).
        if (m_lyricsTrack.is_valid() || !m_artTrackIdentity.empty()) return;
        if (!refreshText && !m_art) return;
        m_art = nullptr;
        m_lyricsArt = nullptr;
        ++m_artGeneration;
    }
    if (m_pane) m_pane->SetTrack(m_trackTitle, m_trackArtist, m_lyricsArt);
    {
        // The cover's colour: under the lyrics at once (clear while it loads, the window's
        // tint showing), and on the glass when the tint follows the cover - there the last
        // cover's colour stays until the next one has loaded.
        const std::optional<uint32_t> tint = CoverTint(m_art.get());
        if (m_pane) m_pane->SetCoverTint(tint);
        if (tint && tint != m_windowCoverRgb) {
            m_windowCoverRgb = tint;
            ApplyTint(true);
        }
    }
    RepaintNowPlaying();
    const metadb_handle_ptr cover = m_artTrackIdentity.empty() ? metadb_handle_ptr() : now;
    if (m_taskbar) m_taskbar->SetCoverTrack(cover);
    if (m_playerTaskbar) m_playerTaskbar->SetCoverTrack(cover);
}

bool MainWindow::PlaybackPaintAllowed() const {
    return m_hwnd && IsWindowVisible(m_hwnd) && !IsIconic(m_hwnd);
}

void MainWindow::FlushPlaybackPaints() {
    if (!PlaybackPaintAllowed()) return;
    if (m_seekPaint.Take(true))
        if (auto* seek = WidgetById(kSeek)) seek->Invalidate();
    if (m_nowPlayingPaint.Pending()) RepaintNowPlaying();
}

void MainWindow::UpdatePlaybackPresentation() {
    const bool allowed = PlaybackPaintAllowed();
    if (!allowed) {
        m_playbackPresentationSuspended = true;
        if (m_frameClock) m_frameClock->Stop();
        return;
    }
    if (!m_playbackPresentationSuspended) return;
    m_playbackPresentationSuspended = false;
    FlushPlaybackPaints();
    bool more = m_pane && m_pane->Tick();
    more |= TickNowPlayingLyric();
    if (more) StartLyricsFrames();
    StartSeekFrames();
}

void MainWindow::RepaintNowPlaying() {
    m_nowPlayingPaint.Request();
    if (!m_nowPlayingPaint.Take(PlaybackPaintAllowed())) return;
    m_npPaintedWidth = NowPlayingWidth();
    auto& host = gfx::CompositionHost::Get();
    const int w = PxI(360), h = PxI(TransportHeight() - 10);
    host.Repaint(m_nowPlaying, m_nowPlayingBacking, w, h, m_scale, [&](ID2D1DeviceContext* dc) {
        if (m_trackTitle.empty()) return;
        const float H = h / m_scale;
        const float side = H - 2;
        const D2D1_RECT_F coverBox{2, 1, 2 + side, 1 + side};
        com_ptr<ID2D1Bitmap> bmp;
        if (m_art) {
            if (SUCCEEDED(dc->CreateBitmapFromWicBitmap(m_art.get(), nullptr, bmp.put()))) {
                const D2D1_SIZE_F sz = bmp->GetSize();
                const float k = side / std::max(sz.width, sz.height);
                const D2D1_RECT_F dst{2, 1 + (side - sz.height * k) / 2, 2 + sz.width * k,
                                      1 + (side + sz.height * k) / 2};
                // Same edge as the covers in the library (soft shadow, one-pixel 18 % black line).
                const float m = (dst.right - dst.left) * 8.0f / 184.0f;
                win7::DrawStretched(
                    dc, win7::Module::Wmploc, L"ALBUMDROPSHADOW_200X200.PNG",
                    {dst.left - m + 1, dst.top - m + 2, dst.right + m + 1, dst.bottom + m + 2}, 0.45f);
                dc->DrawBitmap(bmp.get(), &dst, 1.0f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, nullptr,
                               nullptr);
                com_ptr<ID2D1SolidColorBrush> frame;
                dc->CreateSolidColorBrush(D2D1::ColorF(0, 0.18f), frame.put());
                const float px = 1.0f / m_scale;
                dc->DrawRectangle(
                    {dst.left + px / 2, dst.top + px / 2, dst.right - px / 2, dst.bottom - px / 2},
                    frame.get(), px);
            }
        }
        if (!bmp && !win7::DrawStretched(dc, win7::Module::Wmploc, L"ALBUM_0.PNG", coverBox))
            pane::DrawCover(dc, nullptr, coverBox, 4.0f);
        if (m_npHot == 5) {
            // Like Apple Music's cover: a veil and the glyph of what a click does.
            com_ptr<ID2D1SolidColorBrush> veil;
            dc->CreateSolidColorBrush(D2D1::ColorF(0x0A1E46, 0.55f), veil.put());
            dc->FillRectangle(coverBox, veil.get());
            const float cx = (coverBox.left + coverBox.right) / 2, cy = (coverBox.top + coverBox.bottom) / 2;
            if (!win7::Draw(dc, win7::Module::Wmploc, 4601, {std::round(cx - 12.5f), std::round(cy - 12.5f)}))
                pane::DrawIcon(dc, L'\xE740', 14, {cx, cy}, pane::White(dc, 0.95f).get());
        }
        const float x = coverBox.right + 8;
        m_npArtRect = {m_layout.nowPlaying.left + PxI(coverBox.left), m_layout.nowPlaying.top + PxI(coverBox.top),
                       m_layout.nowPlaying.left + PxI(coverBox.right), m_layout.nowPlaying.top + PxI(coverBox.bottom)};
        com_ptr<ID2D1SolidColorBrush> dark, grey, link;
        dc->CreateSolidColorBrush(Hex(0x1E1E1E), dark.put());
        dc->CreateSolidColorBrush(Hex(0x5B5B5B), grey.put());
        dc->CreateSolidColorBrush(Hex(0x0066CC), link.put());
        // Row 1: title and artist (links: the song in the list, the artist), then the Favorite
        // star. Row 2: the current lyric line (PaintNowPlayingLyric). Without synced lyrics, or
        // with the line hidden, row 1 holds the title and the star and row 2 the artist.
        const bool lyricRow = m_npLyrics && m_npLyrics->synced && settings::LyricLineShown();
        const float top = std::round(H / 2 - kInfoRow);
        m_npTextX = x;
        m_npLyricTop = top + kInfoRow;
        const auto toClient = [&](D2D1_RECT_F r) {
            return RECT{m_layout.nowPlaying.left + PxI(r.left), m_layout.nowPlaying.top + PxI(r.top),
                        m_layout.nowPlaying.left + PxI(r.right), m_layout.nowPlaying.top + PxI(r.bottom)};
        };
        const auto natural = [](const std::wstring& text) {
            if (text.empty()) return 0.0f;
            DWRITE_TEXT_METRICS tm{};
            MakeText(text, 12.0f, DWRITE_FONT_WEIGHT_NORMAL, 2000, kInfoRow + 2)->GetMetrics(&tm);
            return std::ceil(tm.widthIncludingTrailingWhitespace);
        };
        constexpr float kStar = 22, kGap = 10;
        const float limit = m_npPaintedWidth;
        float titleW = natural(m_trackTitle), artistW = natural(m_trackArtist);
        const auto text = [&](const std::wstring& t, float at, float rowTop, float width, ID2D1SolidColorBrush* ink,
                              bool hot) {
            auto layout = MakeText(t, 12.0f, DWRITE_FONT_WEIGHT_NORMAL, std::max(width, 1.0f), kInfoRow + 2);
            if (hot) layout->SetUnderline(TRUE, {0, static_cast<UINT32>(t.size())});
            dc->DrawTextLayout({at, rowTop - 1}, layout.get(), hot ? link.get() : ink);
            return D2D1_RECT_F{at, rowTop, at + width, rowTop + kInfoRow};
        };
        float end = x;
        m_npArtistRect = {};
        m_npCut.clear();
        const float fullTitleW = titleW, fullArtistW = artistW;
        if (lyricRow) {
            // Share the row: the title first, the artist keeps at least 40 % when both are long.
            // Too narrow for both to stay readable, the artist leaves the row to the title
            // (owner, 2026-11-14: in the narrowest window the two were drawn over each other).
            const float room = std::max(0.0f, limit - x - kStar);
            constexpr float kBothMin = 140;
            if (artistW > 0 && titleW + kGap + artistW > room) {
                const float share = room - kGap;
                if (share < kBothMin) {
                    artistW = 0;
                } else {
                    artistW = std::min(artistW, std::max(std::min(artistW, 60.0f), share * 0.4f));
                    titleW = std::min(titleW, share - artistW);
                    artistW = std::min(artistW, share - titleW);
                }
            }
            titleW = std::min(titleW, room);
            m_npTitleRect = toClient(text(m_trackTitle, x, top, titleW, dark.get(), m_npHot == 1));
            end = x + titleW;
            if (artistW > 0) {
                m_npArtistRect = toClient(text(m_trackArtist, end + kGap, top, artistW, grey.get(), m_npHot == 2));
                end += kGap + artistW;
            }
        } else {
            titleW = std::min(titleW, std::max(0.0f, limit - x - kStar));
            m_npTitleRect = toClient(text(m_trackTitle, x, top, titleW, dark.get(), m_npHot == 1));
            end = x + titleW;
            artistW = std::min(artistW, std::max(0.0f, limit - x));
            if (artistW > 0)
                m_npArtistRect = toClient(text(m_trackArtist, x, top + kInfoRow, artistW, grey.get(), m_npHot == 2));
        }
        // Cut short (or left out for the title): the tooltip shows it whole (UpdateDetailTip).
        if (titleW + 0.5f < fullTitleW && m_npTitleRect.right > m_npTitleRect.left)
            m_npCut.push_back({m_npTitleRect, m_trackTitle});
        if (artistW > 0 && artistW + 0.5f < fullArtistW) m_npCut.push_back({m_npArtistRect, m_trackArtist});
        // WMP's stars: gold (5530) in Favorites, gold outline (5531) on hover, empty (5533).
        const float sx = std::round((end + 6) * m_scale) / m_scale;
        const float sy = std::round((top + (kInfoRow - 14) / 2) * m_scale) / m_scale;
        win7::Draw(dc, win7::Module::Wmploc, m_npFavorite ? 5530 : m_npHot == 4 ? 5531 : 5533, {sx, sy});
        m_npStarRect = toClient({sx - 3, sy - 3, sx + 17, sy + 17});
    });
    if (m_trackTitle.empty()) m_npTitleRect = m_npArtistRect = m_npStarRect = {};
    if (!m_trackTitle.empty()) {
        metadb_handle_ptr now;
        if (playback_control::get()->get_now_playing(now)) {
            auto* provider = stream::ProviderOf(now);
            if (provider && !provider->CanSetFavorite(!m_npFavorite)) m_npStarRect = {};
        }
    }
    m_npLyricLine = -2;
    PaintNowPlayingLyric();
    // Time beside the controls, painted at its box's width (the widest its text gets).
    m_timePaintedW = std::max<LONG>(1, m_layout.time.right - m_layout.time.left);
    const int timeW = static_cast<int>(m_timePaintedW);
    host.Repaint(m_time, m_timeBacking, timeW, PxI(20), m_scale, [&](ID2D1DeviceContext* dc) {
        if (m_length <= 0 && m_trackTitle.empty()) return;
        const double position = m_seeking ? m_seekPreview * m_length : m_position;
        const bool remaining = m_length > 0 && settings::PlayerTimeRemaining();
        std::wstring text =
            remaining ? L"-" + FormatTime(std::max(0.0, m_length - position)) : FormatTime(position);
        if (m_length > 0) text += L" / " + FormatTime(m_length);
        com_ptr<ID2D1SolidColorBrush> grey;
        dc->CreateSolidColorBrush(Hex(0x4A4A4A), grey.put());
        // Left of the capsule (a narrow window) it ends at the capsule instead.
        auto layout = MakeText(text, 12.0f, DWRITE_FONT_WEIGHT_NORMAL, timeW / m_scale + 20, 20,
                               m_layout.timeLeft ? DWRITE_TEXT_ALIGNMENT_TRAILING : DWRITE_TEXT_ALIGNMENT_LEADING);
        if (m_layout.timeLeft) layout->SetMaxWidth(timeW / m_scale);
        dc->DrawTextLayout({0, 0}, layout.get(), grey.get());
    });
    UpdateLayout();
}

// The lyric line under the title. Word-timed lines turn blue word by word as they are sung;
// line-timed ones are blue as a whole.
void MainWindow::PaintNowPlayingLyric() {
    auto& host = gfx::CompositionHost::Get();
    const int w = PxI(360), h = PxI(TransportHeight() - 10);
    if (!m_npLyricSurface || m_npLyricSize.cx != w || m_npLyricSize.cy != h) {
        m_npLyricSurface = host.CreateSurface(w, h);
        m_npLyricSize = {w, h};
        auto brush = host.Compositor().CreateSurfaceBrush(m_npLyricSurface);
        brush.Stretch(wuc::CompositionStretch::None);
        brush.HorizontalAlignmentRatio(0);
        brush.VerticalAlignmentRatio(0);
        m_npLyric.Brush(brush);
    }
    double now = m_position;
    try {
        if (auto pc = playback_control::get(); pc->is_playing()) now = pc->playback_get_position();
    } catch (...) {
    }
    const int ms = static_cast<int>(now * 1000);
    int index = -1;
    if (m_npLyrics && m_npLyrics->synced && !m_trackTitle.empty() && settings::LyricLineShown())
        for (size_t i = 0; i < m_npLyrics->lines.size() && m_npLyrics->lines[i].start <= ms; ++i)
            index = static_cast<int>(i);
    m_npLyricLine = index;
    m_lyricCut.clear();
    host.Draw(m_npLyricSurface, m_scale, [&](ID2D1DeviceContext* dc) {
        if (index < 0) return;
        const lyrics::Line& line = m_npLyrics->lines[static_cast<size_t>(index)];
        const float width = std::max(1.0f, NowPlayingWidth() - m_npTextX);
        auto layout = MakeText(line.text, 12.0f, DWRITE_FONT_WEIGHT_NORMAL, width, kInfoRow + 2);
        {
            DWRITE_TEXT_METRICS natural{};
            MakeText(line.text, 12.0f, DWRITE_FONT_WEIGHT_NORMAL, 10000, kInfoRow + 2)->GetMetrics(&natural);
            if (natural.widthIncludingTrailingWhitespace > width + 0.5f) {
                const RECT& np = m_layout.nowPlaying;
                m_lyricCut.push_back({{np.left + PxI(m_npTextX), np.top + PxI(m_npLyricTop),
                                       np.left + PxI(m_npTextX + width), np.top + PxI(m_npLyricTop + kInfoRow)},
                                      line.text});
            }
        }
        com_ptr<ID2D1SolidColorBrush> grey, blue;
        dc->CreateSolidColorBrush(Hex(0x6E6E6E), grey.put());
        dc->CreateSolidColorBrush(Hex(0x1F5FB8), blue.put());
        const D2D1_POINT_2F at{m_npTextX, m_npLyricTop - 1};
        if (line.words.empty()) {
            dc->DrawTextLayout(at, layout.get(), blue.get());
            return;
        }
        // Sung part: words before the current one whole, the current one by its progress.
        // Measured on the whole line: a line cut short with an ellipsis hit-tests its hidden
        // words at odd places, and the fill jumped back and forth. Past the cut the visible
        // line, ellipsis included, stays blue.
        auto whole = MakeText(line.text, 12.0f, DWRITE_FONT_WEIGHT_NORMAL, 10000, kInfoRow + 2);
        float fill = 0;
        UINT32 pos = 0;
        for (const auto& word : line.words) {
            const UINT32 len = static_cast<UINT32>(word.text.size());
            if (word.start > ms || pos >= line.text.size()) break;
            DWRITE_HIT_TEST_METRICS hit{};
            UINT32 count = 0;
            whole->HitTestTextRange(pos, std::min<UINT32>(len, static_cast<UINT32>(line.text.size()) - pos), 0, 0,
                                    &hit, 1, &count);
            const float p =
                word.end > word.start
                    ? std::clamp(static_cast<float>(ms - word.start) / (word.end - word.start), 0.0f, 1.0f)
                    : 1.0f;
            if (count > 0) fill = std::max(fill, hit.left + hit.width * p);
            pos += len;
        }
        DWRITE_TEXT_METRICS full{}, shown{};
        whole->GetMetrics(&full);
        layout->GetMetrics(&shown);
        if (full.widthIncludingTrailingWhitespace > width + 0.5f) {
            const float visible = shown.left + shown.width;
            constexpr float kEllipsis = 12; // the trimming sign and the room before it
            if (fill >= visible - kEllipsis) fill = visible + 1;
        }
        dc->DrawTextLayout(at, layout.get(), grey.get());
        if (fill > 0) {
            dc->PushAxisAlignedClip({at.x, 0, at.x + fill, h / m_scale}, D2D1_ANTIALIAS_MODE_ALIASED);
            dc->DrawTextLayout(at, layout.get(), blue.get());
            dc->PopAxisAlignedClip();
        }
    });
}

bool MainWindow::TickNowPlayingLyric() {
    if (!m_npLyrics || !m_npLyrics->synced || !m_playing || !settings::LyricLineShown()) return false;
    const double now = playback_control::get()->playback_get_position();
    const int ms = static_cast<int>(now * 1000);
    int index = -1;
    for (size_t i = 0; i < m_npLyrics->lines.size() && m_npLyrics->lines[i].start <= ms; ++i)
        index = static_cast<int>(i);
    // Repaint when the line changes, and every frame while a word-timed line fills.
    const bool filling = index >= 0 && !m_npLyrics->lines[static_cast<size_t>(index)].words.empty() &&
                         ms <= m_npLyrics->lines[static_cast<size_t>(index)].end + 50;
    if (index != m_npLyricLine || filling) PaintNowPlayingLyric();
    return !m_paused;
}

void MainWindow::RepaintStreamInfo() {
    // Right-aligned: stream format on top, the output device (a menu) below.
    auto& host = gfx::CompositionHost::Get();
    // Painted at the block's real width (narrow windows shrink it); long names get an ellipsis.
    const int w = std::max<int>(1, m_layout.stream.right - m_layout.stream.left),
              h = PxI(TransportHeight() - 12);
    if (m_deviceName.empty()) RefreshDeviceName();
    const std::wstring device = m_deviceName.empty() ? Tr(L"Output device") : m_deviceName;
    float deviceLeft = 0, deviceTop = 0, deviceRight = 0;
    m_streamCut.clear();
    host.Repaint(m_streamVisual, w, h, m_scale, [&](ID2D1DeviceContext* dc) {
        const float W = w / m_scale, H = h / m_scale;
        com_ptr<ID2D1SolidColorBrush> grey, link;
        dc->CreateSolidColorBrush(Hex(0x5B5B5B), grey.put());
        dc->CreateSolidColorBrush(Hex(m_npHot == 3 ? 0x0066CC : 0x2E4E7A), link.put());
        const float lh = kInfoRow, top = std::round(H / 2 - lh);
        if (!m_streamInfo.empty()) {
            auto info = MakeText(m_streamInfo, 11.5f, DWRITE_FONT_WEIGHT_NORMAL, W, lh + 2);
            info->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
            dc->DrawTextLayout({0, top - 1}, info.get(), grey.get());
            DWRITE_TEXT_METRICS natural{};
            MakeText(m_streamInfo, 11.5f, DWRITE_FONT_WEIGHT_NORMAL, 10000, lh + 2)->GetMetrics(&natural);
            if (natural.widthIncludingTrailingWhitespace > W + 0.5f) {
                const RECT& r = m_layout.stream;
                m_streamCut.push_back({{r.left, r.top + PxI(top), r.right, r.top + PxI(top + lh)}, m_streamInfo});
            }
        }
        const std::wstring label = device + L"  ▾";
        auto dev = MakeText(label, 11.5f, DWRITE_FONT_WEIGHT_NORMAL, W, lh + 2);
        dev->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
        dev->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        const DWRITE_TRIMMING ellipsis{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        com_ptr<IDWriteInlineObject> sign;
        if (com_ptr<IDWriteFactory> f; SUCCEEDED(DWriteCreateFactory(
                DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(f.put()))))
            f->CreateEllipsisTrimmingSign(dev.get(), sign.put());
        dev->SetTrimming(&ellipsis, sign.get());
        if (m_npHot == 3) dev->SetUnderline(TRUE, {0, static_cast<UINT32>(device.size())});
        dc->DrawTextLayout({0, top + lh - 1}, dev.get(), link.get());
        DWRITE_TEXT_METRICS tm{};
        dev->GetMetrics(&tm);
        {
            DWRITE_TEXT_METRICS natural{};
            MakeText(label, 11.5f, DWRITE_FONT_WEIGHT_NORMAL, 10000, lh + 2)->GetMetrics(&natural);
            if (natural.widthIncludingTrailingWhitespace > W + 0.5f) {
                const RECT& r = m_layout.stream;
                m_streamCut.push_back({{r.left, r.top + PxI(top + lh), r.right, r.top + PxI(top + 2 * lh)}, device});
            }
        }
        deviceLeft = tm.left;
        deviceRight = tm.left + tm.width;
        deviceTop = top + lh;
    });
    m_deviceText = {deviceLeft, deviceTop, deviceRight, deviceTop + kInfoRow};
}

void MainWindow::RefreshDeviceName() {
    m_deviceName.clear();
    try {
        const auto cfg = output_manager::get()->getCoreConfig();
        output_manager_v2::get()->listDevices([&](const char* name, const GUID& output, const GUID& dev) {
            if (output == cfg.m_output && dev == cfg.m_device) m_deviceName = Wide(name);
        });
    } catch (...) {
    }
}

RECT MainWindow::DeviceRect() const {
    const RECT& s = m_layout.stream;
    if (m_deviceText.right <= m_deviceText.left) return {};
    return {s.left + PxI(m_deviceText.left), s.top + PxI(m_deviceText.top), s.left + PxI(m_deviceText.right),
            s.top + PxI(m_deviceText.bottom)};
}

void MainWindow::ToggleLyrics() {
    KillTimer(m_hwnd, kPaneRevealTimer);
    const bool open = !m_lyricsOpen;
    cfg_lyricsOpen.set(open ? 1 : 0);
    const int paneW = PxI(kLyricsPaneWidth);
    if (IsWindowMaximized() || IsIconic(m_hwnd) || !IsWindowVisible(m_hwnd)) {
        // A maximized window gives the pane part of its width at once.
        m_lyricsOpen = open;
        m_paneReveal = -1;
        UpdateLayout();
    } else {
        // A restored window grows to the right by the pane, eased, and shrinks back the
        // same way; the pane's width follows the window so the library never moves.
        RECT wr{};
        GetWindowRect(m_hwnd, &wr);
        const int shown = m_paneReveal >= 0 ? m_paneReveal : (m_lyricsOpen ? paneW : 0);
        m_paneRevealBase = wr.right - wr.left - shown; // the window's width without any pane
        m_paneRevealFrom = shown;
        m_paneRevealTo = open ? paneW : 0;
        m_paneReveal = shown;
        m_paneRevealStart = GetTickCount64();
        m_lyricsOpen = true; // the pane stays in the layout until it has slid shut
        SetTimer(m_hwnd, kPaneRevealTimer, 8, nullptr);
        StepPaneReveal();
    }
    if (auto* w = WidgetById(kLyricsToggle)) w->Invalidate();
    RepaintNowPlaying();
}

void MainWindow::StepPaneReveal() {
    const float t = std::clamp(static_cast<float>(GetTickCount64() - m_paneRevealStart) / kPaneRevealMs, 0.0f, 1.0f);
    const float eased = 1.0f - std::pow(1.0f - t, 4.0f); // quartic ease-out: quick start, soft landing
    const bool done = t >= 1.0f;
    m_paneReveal = static_cast<int>(std::lround(m_paneRevealFrom + (m_paneRevealTo - m_paneRevealFrom) * eased));
    RECT wr{};
    GetWindowRect(m_hwnd, &wr);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromWindow(m_hwnd, MONITOR_DEFAULTTONEAREST), &mi);
    const RECT& work = mi.rcWork;
    const int width = std::min<int>(m_paneRevealBase + m_paneReveal, work.right - work.left);
    int left = wr.left;
    if (left + width > work.right) left = std::max<int>(work.left, work.right - width);
    if (done) {
        KillTimer(m_hwnd, kPaneRevealTimer);
        m_lyricsOpen = m_paneRevealTo > 0;
        m_paneReveal = -1;
    }
    SetWindowPos(m_hwnd, nullptr, left, wr.top, width, wr.bottom - wr.top, SWP_NOZORDER | SWP_NOACTIVATE);
    UpdateLayout();
    if (done) {
        if (auto* w = WidgetById(kLyricsToggle)) w->Invalidate();
        RepaintNowPlaying();
    }
}

void MainWindow::SetLyricLineShown(bool shown) {
    settings::SetLyricLineShown(shown);
    m_npLyricLine = -2;
    RepaintNowPlaying();
    if (m_player) m_player->OnLyricLineSetting();
}

int MainWindow::NowPlayingPartAt(POINT pt) {
    if (PtInRect(&m_npTitleRect, pt)) return 1;
    if (PtInRect(&m_npArtistRect, pt)) return NowPlayingArtistLinks() ? 2 : 0;
    if (PtInRect(&m_npStarRect, pt)) return 4;
    if (const RECT d = DeviceRect(); PtInRect(&d, pt)) return 3;
    if (PtInRect(&m_npArtRect, pt)) return 5; // like Apple Music: the cover opens the mini player
    if (m_length > 0 && PtInRect(&m_layout.time, pt)) return 6;
    return 0;
}

bool MainWindow::NowPlayingArtistLinks() {
    if (!m_npArtistLinks) {
        metadb_handle_ptr now;
        metadb_handle_list items;
        if (playback_control::get()->get_now_playing(now)) items.add_item(now);
        m_npArtistLinks = !BuildArtistLinks(items, true).empty();
    }
    return *m_npArtistLinks;
}

void MainWindow::UpdateNowPlayingHover(POINT pt) {
    if (!PtInRect(&m_npArtistRect, pt)) m_npArtistLinks.reset(); // asked again on the next hover
    const int part = NowPlayingPartAt(pt);
    if (part == m_npHot) return;
    const int old = m_npHot;
    m_npHot = part;
    if (old == 3 || part == 3) RepaintStreamInfo();
    if (old == 1 || old == 2 || old == 4 || old == 5 || part == 1 || part == 2 || part == 4 || part == 5)
        RepaintNowPlaying();
}

void MainWindow::OnNowPlayingClick(int part) {
    switch (part) {
    case 5:
        ShowPlayer(false);
        break;
    case 1:
        JumpToPlaying();
        break;
    case 2: { // the playing song's artist: its provider's artist page, local group, or search
        metadb_handle_ptr now;
        if (!playback_control::get()->get_now_playing(now)) break;
        metadb_handle_list items;
        items.add_item(now);
        auto links = BuildArtistLinks(items, true);
        if (links.size() == 1) {
            links.front().run();
        } else if (links.size() > 1) {
            RECT anchor = m_npArtistRect;
            MapWindowPoints(m_hwnd, nullptr, reinterpret_cast<POINT*>(&anchor), 2);
            ShowActionMenu(m_hwnd, anchor, links);
        }
        break;
    }
    case 3:
        ShowDeviceMenu();
        break;
    case 4: { // Favorite star next to the title
        metadb_handle_ptr now;
        if (playback_control::get()->get_now_playing(now)) {
            auto* provider = stream::ProviderOf(now);
            const bool next = !media::IsFavorite(now);
            if (!provider || provider->CanSetFavorite(next)) media::ToggleFavorite(now);
        }
        UpdateFavoriteState();
        break;
    }
    case 6: // Time progress: click to toggle elapsed / remaining (shared with mini & full-screen)
        settings::SetPlayerTimeRemaining(!settings::PlayerTimeRemaining());
        m_timeMeasuredFor = -1;
        UpdateLayout();
        RepaintNowPlaying();
        break;
    default:
        break;
    }
}

void MainWindow::JumpToPlaying() {
    metadb_handle_ptr now;
    if (!playback_control::get()->get_now_playing(now)) return;
    size_t row = m_source ? m_source->FindRow(now) : SIZE_MAX;
    if (row == SIZE_MAX) { // not on this page: open the playlist it plays from
        t_size playlist = SIZE_MAX, index = SIZE_MAX;
        if (!playlist_manager::get()->get_playing_item_location(&playlist, &index)) return;
        View v;
        v.library = View::Library::All;
        v.kind = View::Kind::Playlist;
        v.playlist = playlist;
        Navigate(v, true);
        row = m_source ? m_source->FindRow(now) : SIZE_MAX;
    }
    if (row == SIZE_MAX) return;
    m_source->SelectOnly(row);
    m_source->SetFocus(row);
    m_list->EnsureVisible(row);
    m_list->Refresh();
}

void MainWindow::ShowDeviceMenu() {
    struct Device {
        GUID output, device;
    };
    std::vector<Device> devices;
    std::vector<MenuAction> actions;
    size_t current = SIZE_MAX;
    try {
        const auto cfg = output_manager::get()->getCoreConfig();
        output_manager_v2::get()->listDevices([&](const char* name, const GUID& output, const GUID& device) {
            if (output == cfg.m_output && device == cfg.m_device) current = devices.size();
            devices.push_back({output, device});
            actions.push_back({Wide(name), [this, output, device] {
                                   output_manager_v2::get()->setCoreConfigDevice(output, device);
                                   RefreshDeviceName();
                                   RepaintStreamInfo();
                               }});
        });
    } catch (const std::exception& e) {
        popup_message::g_show(e.what(), "Output device");
        return;
    }
    RECT d = DeviceRect();
    MapWindowPoints(m_hwnd, nullptr, reinterpret_cast<POINT*>(&d), 2);
    ShowActionMenu(m_hwnd, d, actions, current);
}

// A track's title and artist as the lists show them: the provider's record, a repaired cue sheet.
void MainWindow::TrackText(const metadb_handle_ptr& track, std::wstring& title, std::wstring& artist) const {
    if (auto* provider = stream::ProviderOf(track)) {
        if (const auto record = provider->FindTrack(provider->TrackId(track))) {
            if (!record->title.empty()) title = record->title;
            if (!record->artists.empty()) artist = stream::JoinArtists(record->artists);
        }
    }
    if (const media::RepairedText* r = media::CueCatalog::Get().Repair(track)) { // mis-decoded cue sheet
        if (!r->title.empty()) title = r->title;
        if (!r->artist.empty()) artist = r->artist;
    }
}

// Shows a song: with its cover at once when the cover is at hand (or known missing), else the
// song shown before stays whole until the cover has come or kArtHoldMs has passed.
void MainWindow::ShowTrack(const metadb_handle_ptr& track, std::wstring title, std::wstring artist, bool starting) {
    HeldTrack next{track, lyrics::Service::TrackIdentity(track), std::move(title), std::move(artist)};
    if (next.identity == m_artTrackIdentity && !starting) return;
    winrt::com_ptr<IWICBitmapSource> art;
    const auto state = ArtCache::Get().Find(track, std::max(PxI(TransportHeight()), PxI(64)), &art);
    const bool shownBefore = !m_artTrackIdentity.empty();
    if (state == ArtCache::State::Pending && !art && shownBefore) {
        const bool already = m_held.track.is_valid();
        m_held = std::move(next);
        if (!already) SetTimer(m_hwnd, kArtHoldTimer, kArtHoldMs, nullptr);
        return;
    }
    if (m_held.track.is_valid()) {
        KillTimer(m_hwnd, kArtHoldTimer);
        m_held = {};
    }
    ApplyShownTrack(next, state == ArtCache::State::Missing ? nullptr : art);
}

void MainWindow::ReleaseHeldTrack(bool timedOut) {
    if (!m_held.track.is_valid()) return;
    winrt::com_ptr<IWICBitmapSource> art;
    const auto state = ArtCache::Get().Find(m_held.track, std::max(PxI(TransportHeight()), PxI(64)), &art);
    if (state == ArtCache::State::Pending && !art && !timedOut) return;
    KillTimer(m_hwnd, kArtHoldTimer);
    const HeldTrack held = std::move(m_held);
    m_held = {};
    ApplyShownTrack(held, state == ArtCache::State::Missing ? nullptr : art); // its cover, or the placeholder
}

void MainWindow::ApplyShownTrack(const HeldTrack& track, winrt::com_ptr<IWICBitmapSource> art) {
    m_trackTitle = track.title;
    m_trackArtist = track.artist;
    m_npArtistLinks.reset();
    m_artTrackIdentity = track.identity;
    ++m_artGeneration;
    m_art = std::move(art);
    m_lyricsArt = m_art;
    if (m_pane) m_pane->SetTrack(m_trackTitle, m_trackArtist, m_lyricsArt);
    if (m_paneStartingPreview && m_paneStartingPreview->track == track.track) RefreshPaneQueue(true);
    const std::optional<uint32_t> tint = CoverTint(m_art.get());
    if (m_pane) m_pane->SetCoverTint(tint);
    if (tint && tint != m_windowCoverRgb) {
        m_windowCoverRgb = tint;
        ApplyTint(true);
    }
    RepaintNowPlaying();
    if (m_taskbar) m_taskbar->SetCoverTrack(track.track);
    if (m_playerTaskbar) m_playerTaskbar->SetCoverTrack(track.track);
    if (m_player) m_player->OnPlayback();
    std::wstring caption = m_trackTitle.empty() ? std::wstring(kAppName)
                                                : m_trackTitle + (m_trackArtist.empty() ? L"" : L" - " + m_trackArtist) +
                                                      L" - " + kAppName;
    SetWindowTextW(m_hwnd, caption.c_str());
}

// Playback is starting towards a song (Next, Previous, a list started): it shows now, while its
// audio is still being opened, and the queue / history sidebar moves with it immediately.
void MainWindow::PreviewStartingTrack(play_control::t_track_command command) {
    auto* scheduler = media::PlaybackScheduler::Existing();
    auto preview = scheduler ? scheduler->PreviewStarting(command) : std::nullopt;
    const metadb_handle_ptr track =
        preview ? preview->track : (scheduler ? scheduler->StartingTrack(command) : metadb_handle_ptr());
    if (!track.is_valid()) return;
    static titleformat_object::ptr s_title, s_artist;
    if (!s_title) {
        titleformat_compiler::get()->compile_safe(s_title, "$if2(%title%,%filename%)");
        titleformat_compiler::get()->compile_safe(s_artist, "[%artist%]");
    }
    pfc::string8 title, artist;
    track->format_title(nullptr, title, s_title, nullptr);
    track->format_title(nullptr, artist, s_artist, nullptr);
    std::wstring t = Wide(title), a = Wide(artist);
    TrackText(track, t, a);
    if (lyrics::Service::TrackIdentity(track) != m_artTrackIdentity) m_startingFrom = m_artTrackIdentity;
    m_paneStartingPreview = std::move(preview);
    ShowTrack(track, std::move(t), std::move(a), true);
}

// Stopped: the song shown goes with its cover and text (owner, 2026-11-20: no placeholder under
// the last song's name).
void MainWindow::ClearShownTrack() {
    KillTimer(m_hwnd, kArtHoldTimer);
    m_held = {};
    m_startingFrom.clear();
    m_paneStartingPreview.reset();
    m_panePreviewedTrack.release();
    ResetLyrics();
    m_trackTitle.clear();
    m_trackArtist.clear();
    m_artTrackIdentity.clear();
    ++m_artGeneration;
    m_art = nullptr;
    m_lyricsArt = nullptr;
    if (m_pane) m_pane->SetTrack({}, {}, nullptr);
    if (m_taskbar) m_taskbar->SetCoverTrack({});
    if (m_playerTaskbar) m_playerTaskbar->SetCoverTrack({});
    RepaintNowPlaying();
}

void MainWindow::OnTrackChanged(bool playbackChanged) {
    struct NotifyPlayer { // after this window's own state is current
        MainWindow& window;
        ~NotifyPlayer() {
            if (window.m_player) window.m_player->OnPlayback();
        }
    } notifyPlayer{*this};
    static titleformat_object::ptr s_title, s_artist;
    if (!s_title) {
        titleformat_compiler::get()->compile_safe(s_title, "$if2(%title%,%filename%)");
        titleformat_compiler::get()->compile_safe(s_artist, "[%artist%]");
    }
    auto pc = playback_control::get();
    metadb_handle_ptr now;
    const bool loaded = pc->get_now_playing(now);
    const std::wstring trackIdentity = loaded ? lyrics::Service::TrackIdentity(now) : std::wstring();
    // Output-device restarts and playlist rebuilds can briefly unload the current item.
    // Keep the last complete presentation until the core reports the same or another track.
    const bool retainStoppedTrack = !loaded && m_lyricsTrack.is_valid();
    pfc::string8 title, artist;
    if (loaded) {
        pc->playback_format_title(nullptr, title, s_title, nullptr, playback_control::display_level_all);
        pc->playback_format_title(nullptr, artist, s_artist, nullptr, playback_control::display_level_all);
    } else if (!retainStoppedTrack) {
        m_art = nullptr;
        m_lyricsArt = nullptr;
    }
    std::wstring newTitle = Wide(title), newArtist = Wide(artist);
    if (loaded) TrackText(now, newTitle, newArtist);
    if (!loaded && !retainStoppedTrack) {
        m_trackTitle.clear();
        m_trackArtist.clear();
    } else if (loaded && trackIdentity == m_artTrackIdentity) { // the song shown: its text may have changed
        m_trackTitle = newTitle;
        m_trackArtist = newArtist;
        m_npArtistLinks.reset(); // its record may have been completed
        m_startingFrom.clear();
    } else if (loaded && trackIdentity != m_startingFrom) {
        // Another song (not the last one still reported while the next one starts).
        ShowTrack(now, newTitle, newArtist, false);
    }
    if (loaded || !retainStoppedTrack) {
        m_length = loaded ? pc->playback_get_length() : 0;
        m_position = loaded ? pc->playback_get_position() : 0;
    }

    // Stream format: codec, bit rate, sample rate, bit depth, channels. A paused resumed
    // track still has all of this; gate on a loaded track, not on is_playing().
    static titleformat_object::ptr s_stream;
    if (!s_stream)
        titleformat_compiler::get()->compile_safe(
            s_stream, "[%codec%]|[%bitrate%]|[%samplerate%]|[$info(bitspersample)]|[%channels%]");
    if (!retainStoppedTrack) m_streamInfo.clear();
    if (loaded) {
        pfc::string8 raw;
        pc->playback_format_title(nullptr, raw, s_stream, nullptr, playback_control::display_level_all);
        std::vector<std::wstring> f;
        std::wstringstream in(Wide(raw));
        for (std::wstring part; std::getline(in, part, L'|');)
            f.push_back(part);
        f.resize(5);
        auto add = [&](const std::wstring& s) {
            if (s.empty()) return;
            if (!m_streamInfo.empty()) m_streamInfo += L"   ";
            m_streamInfo += s;
        };
        add(f[0]);
        if (!f[1].empty()) add(f[1] + L" kbps");
        if (!f[2].empty()) {
            wchar_t khz[32];
            const double rate = _wtof(f[2].c_str()) / 1000.0;
            swprintf_s(khz, std::fmod(rate, 1.0) == 0 ? L"%.0f kHz" : L"%.1f kHz", rate);
            add(khz);
        }
        if (!f[3].empty()) add(f[3] + L" bit");
        if (!f[4].empty()) {
            std::wstring ch = f[4];
            ch[0] = static_cast<wchar_t>(::towupper(ch[0]));
            add(ch);
        }
    }
    const bool lyricsTrackChanged = loaded && trackIdentity != m_lyricsTrackIdentity;
    const bool trackChanged = loaded && (playbackChanged || lyricsTrackChanged);
    RefreshNowPlayingArt(true);
    if (lyricsTrackChanged)
        RequestLyrics(now, false);
    else if (loaded)
        m_lyricsTrack = now; // a new handle object can represent the same logical track
    SchedulePaneQueueRefresh(trackChanged);
    RepaintStreamInfo();
    UpdateFavoriteState();
    m_seekPaint.Request();
    FlushPlaybackPaints();
    if (m_list) m_list->Refresh();

    std::wstring caption =
        m_trackTitle.empty()
            ? std::wstring(kAppName)
            : m_trackTitle + (m_trackArtist.empty() ? L"" : L" - " + m_trackArtist) + L" - " + kAppName;
    SetWindowTextW(m_hwnd, caption.c_str());
}

} // namespace glasswave::ui
