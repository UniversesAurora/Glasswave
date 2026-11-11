#include "pch.h"

#include "ui/album_wall.h"

#include "ui/art_cache.h"
#include "win7/win7_resources.h"

#include <winrt/Windows.Foundation.Collections.h>

namespace glasswave::ui {

namespace wuc = winrt::Windows::UI::Composition;
using winrt::com_ptr;
using winrt::Windows::Foundation::Numerics::float2;
using winrt::Windows::Foundation::Numerics::float3;

namespace {

// Measured from the owner's WMC recordings (docs/design/player-window.md).
constexpr float kTileOfHeight = 0.162f; // tile side / screen height
constexpr float kMinTile = 84;          // DIP, small windows
constexpr float kDriftSeconds = 68;     // one tile height of drift
// The entrance: a ring from the middle; each tile springs forward out of the depth.
constexpr float kRevealPerTile = 0.085f; // s of delay per tile of distance from the middle
constexpr float kRevealJitter = 0.16f;   // s of random delay on top
constexpr int kRevealMs = 760;
constexpr float kRevealFrom = 0.58f;     // start scale
constexpr float kRiseTiles = 0.9f;       // the wall settles upwards by this many tiles
constexpr int kRiseMs = 2600;
// The swell: every tile sinks back once per period, later the further right and down it is.
constexpr float kSwellSeconds = 11;
constexpr float kSwellDepth = 0.925f;    // scale at the bottom of the swell
constexpr float kSwellDim = 0.80f;       // opacity there
constexpr float kSwellPerCol = 0.42f, kSwellPerRow = 0.66f; // s of phase per tile
constexpr size_t kMaxAlbums = 40;
constexpr size_t kFewAlbums = 14; // below this the coloured tiles join in
// WMC's tiles for albums without art (the colours of AUDIO.NOCOVER.1 - 6).
constexpr uint32_t kColors[] = {0x2C8CC6, 0x3BBD8C, 0xD8432E, 0xC8BEA6, 0x7A69B6, 0xE2C01E};

std::chrono::duration<int64_t, std::ratio<1, 10000000>> Seconds(float s) {
    return std::chrono::duration<int64_t, std::ratio<1, 10000000>>(static_cast<int64_t>(s * 1e7f));
}

void Gradient(ID2D1DeviceContext* dc, D2D1_RECT_F r, D2D1_POINT_2F from, D2D1_POINT_2F to, D2D1_COLOR_F a,
              D2D1_COLOR_F b) {
    const D2D1_GRADIENT_STOP stops[] = {{0, a}, {1, b}};
    com_ptr<ID2D1GradientStopCollection> collection;
    dc->CreateGradientStopCollection(stops, 2, collection.put());
    com_ptr<ID2D1LinearGradientBrush> brush;
    dc->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(from, to), collection.get(), brush.put());
    dc->FillRectangle(r, brush.get());
}

} // namespace

AlbumWall::AlbumWall(const wuc::ContainerVisual& parent) {
    const auto& comp = gfx::CompositionHost::Get().Compositor();
    m_root = comp.CreateContainerVisual();
    m_root.Clip(comp.CreateInsetClip());
    m_rise = comp.CreateContainerVisual();
    m_root.Children().InsertAtTop(m_rise);
    m_drift = comp.CreateContainerVisual();
    m_rise.Children().InsertAtTop(m_drift);
    parent.Children().InsertAtTop(m_root);
    m_random ^= static_cast<uint32_t>(GetTickCount64());
    m_artToken = ArtCache::Get().Subscribe([this] { LoadCovers(); });
}

AlbumWall::~AlbumWall() {
    if (auto* cache = ArtCache::Existing()) cache->Unsubscribe(m_artToken);
    if (m_root) {
        if (auto parent = m_root.Parent()) parent.Children().Remove(m_root);
    }
}

uint32_t AlbumWall::Random() {
    m_random ^= m_random << 13;
    m_random ^= m_random >> 17;
    m_random ^= m_random << 5;
    return m_random;
}

void AlbumWall::SetAlbums(std::vector<metadb_handle_ptr> albums) {
    // A random selection, so every visit shows another part of the library.
    for (size_t i = albums.size(); i > 1; --i) std::swap(albums[i - 1], albums[Random() % i]);
    if (albums.size() > kMaxAlbums) albums.resize(kMaxAlbums);
    m_albums = std::move(albums);
    m_slots.clear();
    int color = 0;
    for (const auto& track : m_albums) {
        Slot slot;
        slot.track = track;
        slot.color = color++ % static_cast<int>(std::size(kColors));
        m_slots.push_back(std::move(slot));
    }
    if (m_slots.size() < kFewAlbums) {
        for (int i = 0; i < static_cast<int>(std::size(kColors)); ++i) {
            Slot slot;
            slot.color = i;
            m_slots.push_back(std::move(slot));
        }
    }
    for (auto& row : m_rows)
        for (auto& tile : row) tile.slot = SIZE_MAX;
    if (m_tile > 0) {
        for (auto& slot : m_slots) PaintSlot(slot);
        for (size_t r = 0; r < m_rows.size(); ++r)
            for (size_t c = 0; c < m_rows[r].size(); ++c) Assign(m_rows[r][c], PickSlot(r, c));
        LoadCovers();
    }
}

void AlbumWall::SetSize(SIZE px, float scale) {
    if (px.cx == m_size.cx && px.cy == m_size.cy && scale == m_scale) return;
    const bool scaleChanged = scale != m_scale;
    m_size = px;
    m_scale = scale;
    m_root.Size(float2{static_cast<float>(px.cx), static_cast<float>(px.cy)});
    if (scaleChanged)
        for (auto& slot : m_slots) slot.surface = nullptr;
    Rebuild();
}

void AlbumWall::Rebuild() {
    if (m_size.cx <= 0 || m_size.cy <= 0) return;
    const int tile = std::max(static_cast<int>(std::lround(m_size.cy * kTileOfHeight)),
                              static_cast<int>(std::lround(kMinTile * m_scale)));
    const size_t cols = static_cast<size_t>((m_size.cx + tile - 1) / tile);
    const size_t rows = static_cast<size_t>((m_size.cy + tile - 1) / tile) + 1; // one more for the drift
    const bool tileChanged = tile != m_tile;
    const bool gridChanged = tileChanged || rows != m_rows.size() || (!m_rows.empty() && cols != m_rows[0].size());
    m_tile = tile;
    for (auto& slot : m_slots)
        if (!slot.surface) PaintSlot(slot);

    const auto& comp = gfx::CompositionHost::Get().Compositor();
    // Centre the columns so a resize grows the wall on both sides.
    const float x0 = std::floor((m_size.cx - static_cast<float>(cols * tile)) / 2);
    const float side = static_cast<float>(tile);
    while (m_rows.size() > rows) {
        for (auto& t : m_rows.back()) m_drift.Children().Remove(t.holder);
        m_rows.pop_back();
    }
    m_rows.resize(rows);
    for (size_t r = 0; r < rows; ++r) {
        auto& row = m_rows[r];
        while (row.size() > cols) {
            m_drift.Children().Remove(row.back().holder);
            row.pop_back();
        }
        while (row.size() < cols) {
            Tile t;
            t.holder = comp.CreateContainerVisual();
            t.visual = comp.CreateSpriteVisual();
            t.visual.RelativeSizeAdjustment(float2{1, 1});
            t.visual.Opacity(m_revealed ? 1.0f : 0.0f);
            t.holder.Children().InsertAtTop(t.visual);
            m_drift.Children().InsertAtTop(t.holder);
            row.push_back(std::move(t));
        }
        for (size_t c = 0; c < cols; ++c) {
            auto& t = row[c];
            t.holder.Size(float2{side, side});
            t.holder.Offset(float3{x0 + static_cast<float>(c) * side, static_cast<float>(r) * side, 0});
            t.holder.CenterPoint(float3{side / 2, side / 2, 0});
            t.visual.CenterPoint(float3{side / 2, side / 2, 0});
            if (t.slot == SIZE_MAX || t.slot >= m_slots.size()) Assign(t, PickSlot(r, c));
        }
    }
    LoadCovers();
    if (tileChanged && m_running) {
        m_drifting = false;
        StartDrift();
    }
    if (gridChanged && m_swelling) {
        StopSwell();
        StartSwell(0.5f);
    }
}

size_t AlbumWall::PickSlot(size_t row, size_t col) {
    if (m_slots.empty()) return SIZE_MAX;
    const size_t left = col > 0 && row < m_rows.size() && col - 1 < m_rows[row].size() ? m_rows[row][col - 1].slot
                                                                                         : SIZE_MAX;
    const size_t up = row > 0 && col < m_rows[row - 1].size() ? m_rows[row - 1][col].slot : SIZE_MAX;
    size_t pick = 0;
    for (int attempt = 0; attempt < 6; ++attempt) {
        pick = Random() % m_slots.size();
        if (pick != left && pick != up) break;
    }
    return pick;
}

void AlbumWall::Assign(Tile& tile, size_t slot) {
    tile.slot = slot;
    if (slot >= m_slots.size() || !m_slots[slot].surface) {
        tile.visual.Brush(nullptr);
        return;
    }
    auto brush = gfx::CompositionHost::Get().Compositor().CreateSurfaceBrush(m_slots[slot].surface);
    brush.Stretch(wuc::CompositionStretch::Fill);
    tile.visual.Brush(brush);
}

void AlbumWall::PaintSlot(Slot& slot) {
    auto& host = gfx::CompositionHost::Get();
    const int px = std::min(384, static_cast<int>(std::lround(192 * m_scale)));
    const bool fresh = !slot.surface;
    if (fresh) slot.surface = host.CreateSurface(px, px);
    com_ptr<IWICBitmapSource> art;
    if (slot.track.is_valid() && !slot.missing) {
        const auto state = ArtCache::Get().Find(slot.track, px, &art);
        if (state == ArtCache::State::Missing) slot.missing = true;
    }
    if (!fresh && !art) return; // the coloured tile is already there
    slot.loaded = art != nullptr;
    const float side = px / m_scale;
    host.Draw(slot.surface, m_scale, [&](ID2D1DeviceContext* dc) {
        const D2D1_RECT_F box{0, 0, side, side};
        com_ptr<ID2D1Bitmap> bitmap;
        if (art) dc->CreateBitmapFromWicBitmap(art.get(), bitmap.put());
        wchar_t name[40];
        swprintf_s(name, L"AUDIO.NOCOVER.%d.LARGE.PNG", slot.color + 1);
        if (bitmap) {
            const D2D1_SIZE_F s = bitmap->GetSize();
            const float k = std::max(side / std::max(1.0f, s.width), side / std::max(1.0f, s.height));
            const float w = s.width * k, h = s.height * k;
            const D2D1_RECT_F dst{(side - w) / 2, (side - h) / 2, (side + w) / 2, (side + h) / 2};
            dc->DrawBitmap(bitmap.get(), &dst, 1.0f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, nullptr, nullptr);
        } else if (!win7::DrawStretched(dc, win7::Module::Ehres, name, box)) {
            // Media Center's no-cover tile is missing: its colour, lit from the upper left.
            com_ptr<ID2D1SolidColorBrush> base;
            dc->CreateSolidColorBrush(D2D1::ColorF(kColors[slot.color]), base.put());
            dc->FillRectangle(box, base.get());
            Gradient(dc, box, {0, 0}, {side, side}, D2D1::ColorF(1, 1, 1, 0.18f), D2D1::ColorF(0, 0, 0, 0.16f));
        }
        // The pillowed block of WMC's wall: lit along the top and left, falling into shadow
        // towards the bottom and right, with a soft dark seam where it meets its neighbours.
        const float lit = side * 0.07f, shade = side * 0.11f;
        const auto white = [](float a) { return D2D1::ColorF(1, 1, 1, a); };
        const auto black = [](float a) { return D2D1::ColorF(0, 0, 0, a); };
        Gradient(dc, {0, 0, side, lit}, {0, 0}, {0, lit}, white(0.22f), white(0.0f));
        Gradient(dc, {0, 0, lit, side}, {0, 0}, {lit, 0}, white(0.16f), white(0.0f));
        Gradient(dc, {0, side - shade, side, side}, {0, side - shade}, {0, side}, black(0.0f), black(0.42f));
        Gradient(dc, {side - shade, 0, side, side}, {side - shade, 0}, {side, 0}, black(0.0f), black(0.36f));
        // A sheen across the face, as if the block were slightly domed.
        Gradient(dc, box, {0, 0}, {side * 0.9f, side}, white(0.05f), black(0.12f));
        com_ptr<ID2D1SolidColorBrush> seam, glint;
        dc->CreateSolidColorBrush(black(0.38f), seam.put());
        dc->CreateSolidColorBrush(white(0.34f), glint.put());
        const float line = std::max(1.0f, std::floor(m_scale)) / m_scale;
        dc->FillRectangle({0, side - line, side, side}, seam.get());
        dc->FillRectangle({side - line, 0, side, side}, seam.get());
        dc->FillRectangle({0, 0, side - line, line}, glint.get());
        dc->FillRectangle({0, 0, line, side - line}, glint.get());
    });
}

void AlbumWall::LoadCovers() {
    if (m_tile <= 0) return;
    for (auto& slot : m_slots)
        if (slot.track.is_valid() && !slot.loaded && !slot.missing && slot.surface) PaintSlot(slot);
}

// A ring spreading from the middle of the wall. Every tile comes forward out of the depth,
// overshoots a little and settles; the wall as a whole rises into place and slows down.
void AlbumWall::Reveal() {
    m_revealed = true;
    const auto& comp = gfx::CompositionHost::Get().Compositor();
    const auto spring = comp.CreateCubicBezierEasingFunction({0.22f, 1.28f}, {0.36f, 1.0f});
    const auto soft = comp.CreateCubicBezierEasingFunction({0.2f, 0.0f}, {0.0f, 1.0f});
    const float rows = static_cast<float>(m_rows.size()), cols = m_rows.empty() ? 0.0f : static_cast<float>(m_rows[0].size());
    float last = 0;
    for (size_t r = 0; r < m_rows.size(); ++r) {
        for (size_t c = 0; c < m_rows[r].size(); ++c) {
            auto& tile = m_rows[r][c];
            const float dx = static_cast<float>(c) - (cols - 1) / 2, dy = static_cast<float>(r) - (rows - 2) / 2;
            const float delay = std::sqrt(dx * dx + dy * dy) * kRevealPerTile +
                                static_cast<float>(Random() % 1000) / 1000.0f * kRevealJitter;
            last = std::max(last, delay);
            tile.visual.Opacity(0.0f);
            auto fade = comp.CreateScalarKeyFrameAnimation();
            fade.InsertKeyFrame(0.0f, 0.0f);
            fade.InsertKeyFrame(0.55f, 1.0f, soft);
            fade.InsertKeyFrame(1.0f, 1.0f);
            fade.Duration(std::chrono::milliseconds(kRevealMs));
            fade.DelayTime(Seconds(delay));
            fade.DelayBehavior(wuc::AnimationDelayBehavior::SetInitialValueBeforeDelay);
            tile.visual.StartAnimation(L"Opacity", fade);
            auto grow = comp.CreateVector3KeyFrameAnimation();
            grow.InsertKeyFrame(0.0f, float3{kRevealFrom, kRevealFrom, 1.0f});
            grow.InsertKeyFrame(1.0f, float3{1.0f, 1.0f, 1.0f}, spring);
            grow.Duration(std::chrono::milliseconds(kRevealMs));
            grow.DelayTime(Seconds(delay));
            grow.DelayBehavior(wuc::AnimationDelayBehavior::SetInitialValueBeforeDelay);
            tile.visual.StartAnimation(L"Scale", grow);
        }
    }
    auto rise = comp.CreateVector3KeyFrameAnimation();
    rise.InsertKeyFrame(0.0f, float3{0, static_cast<float>(m_tile) * kRiseTiles, 0});
    rise.InsertKeyFrame(1.0f, float3{0, 0, 0}, comp.CreateCubicBezierEasingFunction({0.12f, 0.82f}, {0.18f, 1.0f}));
    rise.Duration(std::chrono::milliseconds(kRiseMs));
    m_rise.StartAnimation(L"Offset", rise);
    if (m_running) {
        StopSwell();
        StartSwell(last + kRevealMs / 1000.0f + 0.6f);
    }
}

// One period per tile: at rest most of the time, then a slow sink and return. The phase grows
// to the right and downwards, so the swell crosses the wall as a slanted front.
void AlbumWall::StartSwell(float startDelay) {
    if (m_swelling) return;
    m_swelling = true;
    const auto& comp = gfx::CompositionHost::Get().Compositor();
    const auto ease = comp.CreateCubicBezierEasingFunction({0.37f, 0.0f}, {0.63f, 1.0f});
    for (size_t r = 0; r < m_rows.size(); ++r) {
        for (size_t c = 0; c < m_rows[r].size(); ++c) {
            auto& tile = m_rows[r][c];
            const float delay = startDelay + std::fmod(static_cast<float>(c) * kSwellPerCol +
                                                           static_cast<float>(r) * kSwellPerRow,
                                                       kSwellSeconds);
            auto sink = comp.CreateVector3KeyFrameAnimation();
            sink.InsertKeyFrame(0.0f, float3{1, 1, 1});
            sink.InsertKeyFrame(0.16f, float3{kSwellDepth, kSwellDepth, 1}, ease);
            sink.InsertKeyFrame(0.34f, float3{1, 1, 1}, ease);
            sink.InsertKeyFrame(1.0f, float3{1, 1, 1});
            sink.Duration(Seconds(kSwellSeconds));
            sink.DelayTime(Seconds(delay));
            sink.IterationBehavior(wuc::AnimationIterationBehavior::Forever);
            tile.holder.StartAnimation(L"Scale", sink);
            auto dim = comp.CreateScalarKeyFrameAnimation();
            dim.InsertKeyFrame(0.0f, 1.0f);
            dim.InsertKeyFrame(0.16f, kSwellDim, ease);
            dim.InsertKeyFrame(0.34f, 1.0f, ease);
            dim.InsertKeyFrame(1.0f, 1.0f);
            dim.Duration(Seconds(kSwellSeconds));
            dim.DelayTime(Seconds(delay));
            dim.IterationBehavior(wuc::AnimationIterationBehavior::Forever);
            tile.holder.StartAnimation(L"Opacity", dim);
        }
    }
}

void AlbumWall::StopSwell() {
    if (!m_swelling) return;
    m_swelling = false;
    auto& host = gfx::CompositionHost::Get();
    for (auto& row : m_rows) {
        for (auto& tile : row) {
            host.AnimateScale(tile.holder, 1.0f, 700); // back to rest, wherever the swell was
            host.AnimateOpacity(tile.holder, 1.0f, 700);
        }
    }
}

void AlbumWall::SetRunning(bool running) {
    if (running == m_running) return;
    m_running = running;
    if (running) {
        if (m_revealed) StartSwell(0.8f);
        if (m_drifting) {
            try {
                if (auto controller = m_drift.TryGetAnimationController(L"Offset")) {
                    controller.Resume();
                    return;
                }
            } catch (...) {
            }
            m_drifting = false;
        }
        StartDrift();
    } else {
        StopSwell();
        if (!m_drifting) return;
        try {
            if (auto controller = m_drift.TryGetAnimationController(L"Offset")) {
                controller.Pause();
                return;
            }
        } catch (...) {
        }
        ++m_driftGeneration; // no controller: the drift starts over next time
        m_drift.StopAnimation(L"Offset");
        m_drifting = false;
    }
}

void AlbumWall::StartDrift() {
    if (!m_running || m_drifting || m_tile <= 0) return;
    const auto& comp = gfx::CompositionHost::Get().Compositor();
    m_drifting = true;
    const uint64_t generation = ++m_driftGeneration;
    auto batch = comp.CreateScopedBatch(wuc::CompositionBatchTypes::Animation);
    auto anim = comp.CreateVector3KeyFrameAnimation();
    anim.InsertKeyFrame(0.0f, float3{0, 0, 0});
    anim.InsertKeyFrame(1.0f, float3{0, -static_cast<float>(m_tile), 0}, comp.CreateLinearEasingFunction());
    anim.Duration(Seconds(kDriftSeconds));
    m_drift.StartAnimation(L"Offset", anim);
    batch.End();
    batch.Completed([this, generation, alive = std::weak_ptr<int>(m_alive)](const auto&, const auto&) {
        if (alive.expired() || generation != m_driftGeneration) return;
        OnDriftEnd();
    });
}

void AlbumWall::OnDriftEnd() {
    // The top row has left the screen: every row takes the covers of the one below it, the
    // last row gets new ones, and the drift starts again from the top.
    m_drifting = false;
    for (size_t r = 0; r + 1 < m_rows.size(); ++r)
        for (size_t c = 0; c < m_rows[r].size() && c < m_rows[r + 1].size(); ++c)
            Assign(m_rows[r][c], m_rows[r + 1][c].slot);
    if (!m_rows.empty()) {
        const size_t last = m_rows.size() - 1;
        for (size_t c = 0; c < m_rows[last].size(); ++c) Assign(m_rows[last][c], PickSlot(last, c));
    }
    m_drift.Offset(float3{0, 0, 0});
    StartDrift();
}

} // namespace glasswave::ui
