#include "pch.h"

#include "ui/album_wall.h"

#include "ui/art_cache.h"
#include "win7/win7_resources.h"

#include <winrt/Windows.Foundation.Collections.h>

#include <array>
#include <unordered_set>

namespace glasswave::ui {

namespace wuc = winrt::Windows::UI::Composition;
using winrt::com_ptr;
using winrt::Windows::Foundation::Numerics::float2;
using winrt::Windows::Foundation::Numerics::float3;

namespace {

// Measured from the owner's WMC recordings (docs/design/player-window.md).
constexpr float kTileOfHeight = 0.162f; // tile side / monitor height
constexpr float kMinTile = 84;          // DIP, small monitors
constexpr float kDriftSeconds = 68;     // one tile height of drift
// The entrance (enter.mp4 at 30 fps): tiles land one by one at random places over about 2.7 s;
// each appears about 6 % larger and faint, the blue showing through it, is at its size in
// about 0.3 s and solid in about 0.5 s. The wall itself does not move.
constexpr float kRevealSpread = 2.6f; // s from the first tile's start to the last one's
constexpr float kFillSpread = 0.8f;   // s, places a larger window uncovers
// The owner found WMC's 6 % too close to the wall and the accelerating fall stiff: the tiles
// fall from higher (18 % larger) on one soft curve that eases in and settles without a snap.
constexpr int kFallMs = 900;          // the fall, the settling and the fade
constexpr float kFallFrom = 1.18f;    // scale at the start of the fall
// A cover arriving on a tile fades in over what it showed (its colour); the sharp cover over its
// blurred stand-in, the same picture, a little quicker (owner, 2026-11-14).
constexpr int kArriveMs = 600;
constexpr int kSharpenMs = 400;
// The swell: two slow waves across the wall (periods of about 35 s and 55 s; the owner slowed
// them from 21 s and 33 s). A tile's shadow on its lower and right neighbours follows how far it
// stands above them.
// Paused, the wall does not stop: drift and swell go on at this fraction of their pace (WMC).
constexpr float kPausedPace = 0.25f;
// A bound against absurd playlists, not a choice: a cover is painted only while it is shown.
constexpr size_t kMaxAlbums = 5000;
// A round of the deck has at least this many cards; fewer albums are made up with the coloured
// tiles. It keeps a picture from meeting itself a row further down on a wide wall.
constexpr size_t kMinDeck = 24;
// WMC's tiles for albums without art (the colours of AUDIO.NOCOVER.1 - 6).
constexpr uint32_t kColors[] = {0x2C8CC6, 0x3BBD8C, 0xD8432E, 0xC8BEA6, 0x7A69B6, 0xE2C01E};

std::chrono::duration<int64_t, std::ratio<1, 10000000>> Seconds(float s) {
    return std::chrono::duration<int64_t, std::ratio<1, 10000000>>(static_cast<int64_t>(s * 1e7f));
}

int64_t Cell(int col, int row) {
    return (static_cast<int64_t>(row) << 32) ^ static_cast<uint32_t>(col);
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

// The height of the water under the tile at (c, row), -1..1, as an expression of the clock.
std::wstring Wave(const std::wstring& c, const std::wstring& row) {
    return L"(0.6 * Sin(clock.t * 0.18 + " + c + L" * 0.55 + " + row + L" * 0.36) + 0.4 * Sin(clock.t * 0.114 - " + c +
           L" * 0.27 + " + row + L" * 0.61))";
}

} // namespace

AlbumWall::AlbumWall(const wuc::ContainerVisual& parent) {
    const auto& comp = gfx::CompositionHost::Get().Compositor();
    m_root = comp.CreateContainerVisual();
    m_root.Clip(comp.CreateInsetClip());
    m_grid = comp.CreateContainerVisual();
    m_root.Children().InsertAtTop(m_grid);
    m_drift = comp.CreateContainerVisual();
    m_grid.Children().InsertAtTop(m_drift);
    parent.Children().InsertAtTop(m_root);
    m_clock = comp.CreatePropertySet();
    m_clock.InsertScalar(L"t", 0.0f);
    m_clock.InsertScalar(L"amp", 0.0f);
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
    if (albums.size() > kMaxAlbums) {
        for (size_t i = albums.size(); i > 1; --i) std::swap(albums[i - 1], albums[Random() % i]);
        albums.resize(kMaxAlbums);
    }
    constexpr int kColorCount = static_cast<int>(std::size(kColors));
    m_slots.clear();
    m_slots.reserve(albums.size() + kColorCount);
    for (auto& track : albums) {
        Slot slot;
        slot.track = std::move(track);
        slot.color = static_cast<int>(Random() % kColorCount); // shown while its cover loads
        m_slots.push_back(std::move(slot));
    }
    for (int i = 0; i < kColorCount; ++i) {
        Slot slot;
        slot.color = i;
        slot.ready = true;
        m_slots.push_back(std::move(slot));
    }
    BuildDeck();
    m_hand.clear();
    m_lastDealt = SIZE_MAX;
    m_cells.clear();
    for (auto& row : m_rows)
        for (auto& tile : row) {
            tile.slot = SIZE_MAX;
            tile.cell = INT64_MIN;
            tile.shown = nullptr;
        }
    if (m_tile > 0) Rebuild(false);
}

void AlbumWall::SetGeometry(SIZE px, SIZE monitor, float scale) {
    const int monitorHeight = monitor.cy;
    m_monitorWidth = monitor.cx;
    if (px.cx == m_size.cx && px.cy == m_size.cy && monitorHeight == m_monitorHeight && scale == m_scale) return;
    // A window that grows uncovers places that fill with falling tiles.
    const bool grew = m_size.cx > 0 && (px.cx > m_size.cx || px.cy > m_size.cy);
    if (scale != m_scale)
        for (auto& slot : m_slots) slot.surface = nullptr;
    m_size = px;
    m_monitorHeight = monitorHeight;
    m_scale = scale;
    m_root.Size(float2{static_cast<float>(px.cx), static_cast<float>(px.cy)});
    Rebuild(grew);
}

void AlbumWall::Rebuild(bool fillByFalling) {
    if (m_size.cx <= 0 || m_size.cy <= 0) return;
    const int tile = std::max(static_cast<int>(std::lround(m_monitorHeight * kTileOfHeight)),
                              static_cast<int>(std::lround(kMinTile * m_scale)));
    const bool tileChanged = tile != m_tile;
    if (tileChanged) {
        m_tile = tile;
        m_cells.clear(); // another monitor: another wall
        for (auto& row : m_rows)
            for (auto& t : row) t.cell = INT64_MIN;
        for (auto& slot : m_slots) { // covers are decoded for the new size as they are dealt
            slot.surface = nullptr;
            slot.painted = nullptr;
            slot.ready = slot.requested = false;
        }
    }
    if (!m_shadeBrush) PaintShadows();

    const auto& comp = gfx::CompositionHost::Get().Compositor();
    // The grid starts at the window's corner, so sizing the window never shifts the tiles.
    const size_t cols = static_cast<size_t>((m_size.cx + tile - 1) / tile);
    const size_t rows = static_cast<size_t>((m_size.cy + tile - 1) / tile) + 1; // one more for the drift
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
            t.holder.Opacity(m_revealed ? 1.0f : 0.0f);
            t.drop = comp.CreateSpriteVisual();
            t.drop.Brush(m_dropBrush);
            t.drop.Opacity(0.0f);
            t.visual = comp.CreateSpriteVisual();
            t.visual.RelativeSizeAdjustment(float2{1, 1});
            t.shade = comp.CreateSpriteVisual();
            t.shade.RelativeSizeAdjustment(float2{1, 1});
            t.shade.Brush(m_shadeBrush);
            t.dim = comp.CreateSpriteVisual();
            t.dim.RelativeSizeAdjustment(float2{1, 1});
            t.dim.Brush(comp.CreateColorBrush(winrt::Windows::UI::ColorHelper::FromArgb(255, 0, 0, 0)));
            t.dim.Opacity(0.0f);
            for (const wuc::Visual& v : {wuc::Visual(t.drop), wuc::Visual(t.visual), wuc::Visual(t.shade), wuc::Visual(t.dim)})
                t.holder.Children().InsertAtTop(v);
            m_drift.Children().InsertAtTop(t.holder);
            row.push_back(std::move(t));
            AttachSwell(row.back(), r, row.size() - 1);
        }
        for (size_t c = 0; c < cols; ++c) {
            auto& t = row[c];
            t.holder.Size(float2{side, side});
            // The first row waits above the window for the drift to bring it down.
            t.holder.Offset(float3{static_cast<float>(c) * side, static_cast<float>(r) * side - side, 0});
            t.holder.CenterPoint(float3{side / 2, side / 2, 0});
            t.drop.Size(float2{side * 1.5f, side * 1.5f});
            t.drop.Offset(float3{-side * 0.25f, -side * 0.25f, 0});
            if (tileChanged) AttachSwell(t, r, c);
        }
    }

    // Every tile shows its place's cover; a place seen for the first time gets one. Places the
    // window did not show a moment ago fill by falling when it has grown.
    std::unordered_set<int64_t> shown;
    if (fillByFalling)
        for (const auto& row : m_rows)
            for (const auto& t : row)
                if (t.cell != INT64_MIN) shown.insert(t.cell);
    std::vector<Tile*> fresh;
    for (size_t r = 0; r < rows; ++r)
        for (size_t c = 0; c < cols; ++c) {
            auto& t = m_rows[r][c];
            const int gc = static_cast<int>(c), gr = static_cast<int>(r) + m_driftRows;
            const int64_t cell = Cell(gc, gr);
            if (t.cell == cell && t.slot < m_slots.size()) {
                // Its cover may have been painted again (another scale) since it was shown.
                if (t.shown != m_slots[t.slot].surface) Assign(t, t.slot);
                continue;
            }
            t.cell = cell;
            if (fillByFalling && m_revealed && !shown.contains(cell)) fresh.push_back(&t);
            if (const auto it = m_cells.find(cell); it != m_cells.end()) {
                Assign(t, it->second);
                continue;
            }
            const size_t slot = PickSlot(gc, gr);
            m_cells[cell] = slot;
            Assign(t, slot);
        }
    for (size_t i = fresh.size(); i > 1; --i) std::swap(fresh[i - 1], fresh[Random() % i]);
    const float spread = std::min(kFillSpread, 0.05f * static_cast<float>(fresh.size()));
    for (size_t i = 0; i < fresh.size(); ++i)
        Drop(*fresh[i], spread * static_cast<float>(i) / std::max<size_t>(1, fresh.size() - 1),
             kFallMs + static_cast<int>(Random() % 120));
    LoadCovers();
    Preload();
    if (tileChanged && m_running) {
        m_drifting = false;
        StartDrift();
    }
}

// One round of cards: every album that may have a cover, made up with the coloured tiles to at
// least kMinDeck cards when there are few.
void AlbumWall::BuildDeck() {
    m_deck.clear();
    size_t firstColor = m_slots.size();
    for (size_t i = 0; i < m_slots.size(); ++i) {
        if (!m_slots[i].track.is_valid()) {
            firstColor = std::min(firstColor, i);
            continue;
        }
        if (!m_slots[i].missing) m_deck.push_back(i);
    }
    const size_t colors = m_slots.size() - firstColor;
    if (colors == 0) return;
    for (size_t k = Random() % colors; m_deck.size() < kMinDeck; ++k)
        m_deck.push_back(firstColor + k % colors);
}

void AlbumWall::ShuffleIn() {
    std::vector<size_t> round = m_deck;
    for (size_t i = round.size(); i > 1; --i) std::swap(round[i - 1], round[Random() % i]);
    m_hand.insert(m_hand.begin(), round.begin(), round.end()); // under what is left
}

// The tile's own size in pixels: the cover is decoded (and, from NetEase, fetched in the
// rendition) for exactly what the wall shows.
int AlbumWall::CoverPx() const { return m_tile; }

// Asks for a cover (it loads in the background) and notes what is known of it already.
void AlbumWall::RequestCover(Slot& slot) {
    if (!slot.track.is_valid() || slot.missing || slot.ready || m_tile <= 0) return;
    com_ptr<IWICBitmapSource> art;
    const auto state = ArtCache::Get().Find(slot.track, CoverPx(), &art);
    slot.requested = true;
    if (state == ArtCache::State::Missing) slot.missing = true;
    if (art) slot.ready = true;
}

// The cards next in line (about two screens of places) load before they are dealt, so the
// rows the drift brings in show their covers at once. Not the whole deck: a long playlist
// does not decode hundreds of covers it will show only minutes later.
void AlbumWall::Preload() {
    if (m_tile <= 0 || m_deck.empty()) return;
    size_t places = 0;
    for (const auto& row : m_rows) places += row.size();
    const size_t ahead = std::max<size_t>(kMinDeck, places * 2);
    while (m_hand.size() < ahead && m_hand.size() < m_deck.size() * 4) ShuffleIn();
    std::vector<size_t> gone;
    for (size_t i = m_hand.size(), n = 0; i-- > 0 && n < ahead; ++n) {
        Slot& slot = m_slots[m_hand[i]];
        RequestCover(slot);
        if (slot.missing) gone.push_back(m_hand[i]);
    }
    for (const size_t slot : gone) Retire(slot);
}

// An album found to have no cover leaves the deck and every place showing it.
void AlbumWall::Retire(size_t slot) {
    if (std::find(m_deck.begin(), m_deck.end(), slot) == m_deck.end()) return;
    BuildDeck();
    std::erase(m_hand, slot);
    for (size_t r = 0; r < m_rows.size(); ++r)
        for (size_t c = 0; c < m_rows[r].size(); ++c) {
            Tile& t = m_rows[r][c];
            if (t.slot != slot) continue;
            const int gc = static_cast<int>(c), gr = static_cast<int>(r) + m_driftRows;
            m_cells.erase(Cell(gc, gr));
            const size_t next = PickSlot(gc, gr);
            m_cells[Cell(gc, gr)] = next;
            Assign(t, next);
        }
    for (auto it = m_cells.begin(); it != m_cells.end();)
        it = it->second == slot ? m_cells.erase(it) : std::next(it);
}

// The next card of the deck for a new place. Every round deals each card once, in a shuffled
// order. A card whose cover has loaded goes before one still loading; a card showing the same
// picture as the place's neighbours (or the card dealt just before) is passed over for the next
// one that does not, and is dealt later in the round. When no card left in the round fits, the
// next round is shuffled in underneath and searched too.
size_t AlbumWall::PickSlot(int col, int row) {
    if (m_deck.empty()) return SIZE_MAX;
    std::array<size_t, 9> around{};
    size_t count = 0;
    for (int dr = -1; dr <= 1; ++dr)
        for (int dc = -1; dc <= 1; ++dc) {
            if (dr == 0 && dc == 0) continue;
            if (const auto it = m_cells.find(Cell(col + dc, row + dr)); it != m_cells.end())
                around[count++] = it->second;
        }
    if (m_lastDealt != SIZE_MAX) around[count++] = m_lastDealt;
    const auto end = around.begin() + static_cast<std::ptrdiff_t>(count);
    const auto clashes = [&](size_t slot) { return std::find(around.begin(), end, slot) != end; };
    // Cards found to have no cover since they were shuffled leave the hand here.
    std::erase_if(m_hand, [this](size_t slot) { return m_slots[slot].missing; });
    if (m_hand.empty()) ShuffleIn();
    const auto take = [this, &clashes](bool readyOnly) {
        for (size_t i = m_hand.size(); i-- > 0;) {
            const Slot& slot = m_slots[m_hand[i]];
            if (clashes(m_hand[i]) || (readyOnly && !slot.ready)) continue;
            std::swap(m_hand[i], m_hand.back());
            return true;
        }
        return false;
    };
    // A loaded cover first; a cover still loading only when no loaded one fits (it shows its
    // colour until it arrives). Only then is the next round searched.
    if (!take(true) && !take(false)) {
        ShuffleIn();
        if (!take(true)) take(false);
    }
    // Too few pictures to keep them apart (only the coloured tiles): the top card it is.
    const size_t slot = m_hand.back();
    m_hand.pop_back();
    m_lastDealt = slot;
    RequestCover(m_slots[slot]);
    return slot;
}

void AlbumWall::Assign(Tile& tile, size_t slot) {
    tile.slot = slot;
    if (tile.previous && tile.previous.Brush()) { // a fade still running ends with another picture
        tile.previous.StopAnimation(L"Opacity");
        tile.previous.Brush(nullptr);
    }
    if (slot < m_slots.size() && !m_slots[slot].surface && m_tile > 0) PaintSlot(m_slots[slot]);
    tile.shown = slot < m_slots.size() ? m_slots[slot].surface : nullptr;
    if (!tile.shown) {
        tile.visual.Brush(nullptr);
        return;
    }
    auto brush = gfx::CompositionHost::Get().Compositor().CreateSurfaceBrush(m_slots[slot].surface);
    brush.Stretch(wuc::CompositionStretch::Fill);
    tile.visual.Brush(brush);
}

// The cover as Media Center's wall shows it: the picture alone, edge to edge. The tile's
// relief comes from the shadows its neighbours cast (PaintShadows).
void AlbumWall::PaintSlot(Slot& slot) {
    auto& host = gfx::CompositionHost::Get();
    const int px = CoverPx();
    const bool fresh = !slot.surface;
    com_ptr<IWICBitmapSource> art;
    if (slot.track.is_valid() && !slot.missing) {
        const auto state = ArtCache::Get().Find(slot.track, px, &art);
        slot.requested = true;
        if (state == ArtCache::State::Missing) slot.missing = true;
        if (art) slot.ready = true;
    }
    if (!fresh && (!art || art.get() == slot.painted.get())) return; // nothing new to show
    // A cover arriving for a tile already on the wall (its colour, or a smaller stand-in) goes
    // to a surface of its own, so the tiles can fade from the old picture to it.
    const auto before = slot.surface;
    const bool sharpening = slot.painted != nullptr; // the same picture, only sharper
    slot.surface = host.CreateSurface(px, px);
    slot.painted = art;
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
    });
    if (before) CrossFade(static_cast<size_t>(&slot - m_slots.data()), before, sharpening);
}

// The tiles showing `slot` change to its new surface with the old picture fading out over it
// (owner, 2026-11-14: a cover replacing the coloured tile, or the sharp cover replacing its
// blurred stand-in, must not jump). The old surface is let go once the fade has ended.
void AlbumWall::CrossFade(size_t slot, const wuc::CompositionDrawingSurface& before, bool sharpening) {
    if (slot >= m_slots.size()) return;
    const auto& comp = gfx::CompositionHost::Get().Compositor();
    for (auto& row : m_rows)
        for (auto& t : row) {
            if (t.slot != slot || t.shown != before) continue;
            const auto old = t.visual.Brush();
            Assign(t, slot);
            if (!old) continue;
            if (!t.previous) {
                t.previous = comp.CreateSpriteVisual();
                t.previous.RelativeSizeAdjustment(float2{1, 1});
                t.holder.Children().InsertAbove(t.previous, t.visual);
            }
            t.previous.Brush(old);
            auto batch = comp.CreateScopedBatch(wuc::CompositionBatchTypes::Animation);
            auto fade = comp.CreateScalarKeyFrameAnimation();
            fade.InsertKeyFrame(0.0f, 1.0f);
            fade.InsertKeyFrame(1.0f, 0.0f, comp.CreateCubicBezierEasingFunction({0.4f, 0.0f}, {0.2f, 1.0f}));
            fade.Duration(std::chrono::milliseconds(sharpening ? kSharpenMs : kArriveMs));
            t.previous.StartAnimation(L"Opacity", fade);
            batch.End();
            batch.Completed([previous = t.previous, old](const auto&, const auto&) {
                if (previous.Brush() == old) previous.Brush(nullptr);
            });
        }
}

// Two shadows shared by every tile, painted once and stretched to the tile:
//   shade  what the tiles above and to the left cast on this one (the light comes from the
//          upper left): dark along its top edge and, less, its left edge, fading inwards over a
//          tenth of the tile (PixPin_2026-11-11_23-23-39.png);
//   drop   a soft square under a falling tile, larger and fainter the higher it is.
void AlbumWall::PaintShadows() {
    auto& host = gfx::CompositionHost::Get();
    const auto& comp = host.Compositor();
    constexpr int kPx = 256;
    const auto shade = host.CreateSurface(kPx, kPx);
    host.Draw(shade, 1.0f, [&](ID2D1DeviceContext* dc) {
        // change.mp4 at 295 px a tile: the shade runs about 20 px in from the top and 18 px
        // from the left, with a darker seam of a pixel or two right at the edge.
        const float s = kPx, top = s * 0.075f, left = s * 0.065f, seam = s * 0.008f;
        Gradient(dc, {0, 0, s, top}, {0, 0}, {0, top}, D2D1::ColorF(0, 0, 0, 0.52f), D2D1::ColorF(0, 0, 0, 0.0f));
        Gradient(dc, {0, 0, left, s}, {0, 0}, {left, 0}, D2D1::ColorF(0, 0, 0, 0.44f), D2D1::ColorF(0, 0, 0, 0.0f));
        com_ptr<ID2D1SolidColorBrush> line;
        dc->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 0.30f), line.put());
        dc->FillRectangle({0, 0, s, seam}, line.get());
        dc->FillRectangle({0, seam, seam, s}, line.get());
    });
    m_shadeBrush = comp.CreateSurfaceBrush(shade);
    m_shadeBrush.Stretch(wuc::CompositionStretch::Fill);
    const auto drop = host.CreateSurface(kPx, kPx);
    host.Draw(drop, 1.0f, [&](ID2D1DeviceContext* dc) {
        // A tile's square (the middle two thirds) blurred over a sixth of the sprite.
        constexpr int kSteps = 24;
        const float s = kPx, inner = s / 6, feather = s / 6;
        const float each = 1.0f - std::pow(1.0f - 0.9f, 1.0f / kSteps);
        com_ptr<ID2D1SolidColorBrush> brush;
        dc->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, each), brush.put());
        for (int i = 0; i < kSteps; ++i) {
            const float d = feather * (1.0f - static_cast<float>(i) / (kSteps - 1));
            dc->FillRoundedRectangle({{inner - d, inner - d, s - inner + d, s - inner + d}, d, d}, brush.get());
        }
    });
    m_dropBrush = comp.CreateSurfaceBrush(drop);
    m_dropBrush.Stretch(wuc::CompositionStretch::Fill);
}

// New covers have arrived: paint the ones on the wall (also a sharp cover replacing the
// thumbnail that stood in for it), note the ones waiting in the deck, and take albums found to
// have none out of it.
void AlbumWall::LoadCovers() {
    if (m_tile <= 0) return;
    std::vector<size_t> gone;
    for (size_t i = 0; i < m_slots.size(); ++i) {
        Slot& slot = m_slots[i];
        if (!slot.track.is_valid() || slot.missing) continue;
        if (slot.surface)
            PaintSlot(slot);
        else if (slot.requested && !slot.ready)
            RequestCover(slot);
        if (slot.missing) gone.push_back(i);
    }
    for (const size_t slot : gone) Retire(slot);
}

// Covers no tile shows any more give their surfaces back; a later deal paints them again.
void AlbumWall::ReleaseHidden() {
    std::vector<bool> shown(m_slots.size(), false);
    for (const auto& row : m_rows)
        for (const auto& t : row)
            if (t.slot < shown.size()) shown[t.slot] = true;
    for (size_t i = 0; i < m_slots.size(); ++i)
        if (!shown[i] && m_slots[i].surface) {
            m_slots[i].surface = nullptr;
            m_slots[i].painted = nullptr;
        }
}

// A tile falls straight down onto the wall: from above it looks larger and is still faint; it
// leaves gently, gathers speed and eases onto the wall on one curve, dips a hair into the water
// and comes up again while it turns solid. Its shadow on the wall closes in under it on the way.
void AlbumWall::Drop(Tile& tile, float delaySeconds, int durationMs) {
    const auto& comp = gfx::CompositionHost::Get().Compositor();
    // What falls later lies on what is already there.
    m_drift.Children().Remove(tile.holder);
    m_drift.Children().InsertAtTop(tile.holder);
    const float side = static_cast<float>(m_tile);
    const auto fall = comp.CreateCubicBezierEasingFunction({0.38f, 0.0f}, {0.22f, 1.0f});
    const auto settle = comp.CreateCubicBezierEasingFunction({0.33f, 0.0f}, {0.67f, 1.0f});
    const auto soft = comp.CreateCubicBezierEasingFunction({0.45f, 0.0f}, {0.55f, 1.0f});
    const auto start = [&](const wuc::Visual& v, const wchar_t* property, const wuc::KeyFrameAnimation& a) {
        a.Duration(std::chrono::milliseconds(durationMs));
        a.DelayTime(Seconds(delaySeconds));
        a.DelayBehavior(wuc::AnimationDelayBehavior::SetInitialValueBeforeDelay);
        v.StartAnimation(property, a);
    };
    // On the wall at 62 % of the time; the dip and the rise after it are as soft.
    auto scale = comp.CreateVector3KeyFrameAnimation();
    scale.InsertKeyFrame(0.0f, float3{kFallFrom, kFallFrom, 1});
    scale.InsertKeyFrame(0.62f, float3{1, 1, 1}, fall);
    scale.InsertKeyFrame(0.76f, float3{0.993f, 0.993f, 1}, settle);
    scale.InsertKeyFrame(0.95f, float3{1, 1, 1}, soft);
    scale.InsertKeyFrame(1.0f, float3{1, 1, 1});
    start(tile.holder, L"Scale", scale);
    auto fade = comp.CreateScalarKeyFrameAnimation();
    fade.InsertKeyFrame(0.0f, 0.0f);
    fade.InsertKeyFrame(0.05f, 0.16f);
    fade.InsertKeyFrame(0.6f, 1.0f, comp.CreateCubicBezierEasingFunction({0.3f, 0.0f}, {0.5f, 1.0f}));
    fade.InsertKeyFrame(1.0f, 1.0f);
    start(tile.holder, L"Opacity", fade);
    // The higher it starts, the farther its shadow lies from it.
    const float lift = (kFallFrom - 1) / 0.08f;
    auto shift = comp.CreateVector3KeyFrameAnimation();
    shift.InsertKeyFrame(0.0f, float3{-side * 0.25f + side * 0.035f * lift, -side * 0.25f + side * 0.05f * lift, 0});
    shift.InsertKeyFrame(0.62f, float3{-side * 0.25f, -side * 0.25f, 0}, fall);
    shift.InsertKeyFrame(1.0f, float3{-side * 0.25f, -side * 0.25f, 0});
    start(tile.drop, L"Offset", shift);
    auto shadow = comp.CreateScalarKeyFrameAnimation();
    shadow.InsertKeyFrame(0.0f, 0.0f);
    shadow.InsertKeyFrame(0.35f, 0.34f, soft);
    shadow.InsertKeyFrame(0.62f, 0.24f);
    shadow.InsertKeyFrame(0.9f, 0.0f, soft);
    shadow.InsertKeyFrame(1.0f, 0.0f);
    start(tile.drop, L"Opacity", shadow);
}

// The entrance: tile after tile at random places, a little faster at first.
void AlbumWall::Reveal() {
    m_revealed = true;
    std::vector<Tile*> tiles;
    for (auto& row : m_rows)
        for (auto& t : row) tiles.push_back(&t);
    for (size_t i = tiles.size(); i > 1; --i) std::swap(tiles[i - 1], tiles[Random() % i]);
    // The full screen's pace in every window: tiles land as often as when the whole monitor
    // fills in kRevealSpread, so the mini window (a few of the same tiles) is not sparse and slow.
    float spread = kRevealSpread;
    if (m_tile > 0 && m_monitorWidth > 0 && m_monitorHeight > 0) {
        const float full = std::ceil(static_cast<float>(m_monitorWidth) / m_tile) *
                           (std::ceil(static_cast<float>(m_monitorHeight) / m_tile) + 1);
        spread = kRevealSpread * std::min(1.0f, static_cast<float>(tiles.size()) / std::max(1.0f, full));
    }
    float last = 0;
    for (size_t i = 0; i < tiles.size(); ++i) {
        const float u = static_cast<float>(i) / std::max<size_t>(1, tiles.size() - 1);
        const float delay = spread * std::pow(u, 1.15f) + 0.08f * Unit();
        const int ms = kFallMs + static_cast<int>(Random() % 160);
        Drop(*tiles[i], delay, ms);
        last = std::max(last, delay + ms / 1000.0f);
    }
    RaiseSwell(last + 0.4f);
}

// A tile's shadow on its neighbours and its own shade follow the water at its place on the
// screen (its row moves with the drift). Expressions of the shared clock: the compositor
// evaluates them, nothing runs here.
void AlbumWall::AttachSwell(Tile& tile, size_t row, size_t col) {
    const auto& comp = gfx::CompositionHost::Get().Compositor();
    // Rows on the screen: row r lies one tile above its place in the grid (see Rebuild).
    const std::wstring y = L"(r - 1 + drift.Offset.Y / tile)", above = L"(r - 2 + drift.Offset.Y / tile)";
    const std::wstring self = Wave(L"c", y);
    // Standing lower than the tiles above and to the left, a tile lies deeper in their shadow.
    auto shade = comp.CreateExpressionAnimation(L"Clamp(0.72 + clock.amp * 1.1 * ((" + Wave(L"c", above) + L" + " +
                                                Wave(L"(c - 1)", y) + L") * 0.5 - " + self + L"), 0, 1)");
    auto dim = comp.CreateExpressionAnimation(L"clock.amp * 0.05 * (1 - " + self + L")");
    for (const auto& animation : {shade, dim}) {
        animation.SetReferenceParameter(L"clock", m_clock);
        animation.SetReferenceParameter(L"drift", m_drift);
        animation.SetScalarParameter(L"c", static_cast<float>(col));
        animation.SetScalarParameter(L"r", static_cast<float>(row));
        animation.SetScalarParameter(L"tile", static_cast<float>(std::max(1, m_tile)));
    }
    tile.shade.StartAnimation(L"Opacity", shade);
    tile.dim.StartAnimation(L"Opacity", dim);
}

// The swell rises gently out of a still wall.
void AlbumWall::RaiseSwell(float delaySeconds) {
    const auto& comp = gfx::CompositionHost::Get().Compositor();
    auto raise = comp.CreateScalarKeyFrameAnimation();
    raise.InsertKeyFrame(1.0f, 1.0f, comp.CreateCubicBezierEasingFunction({0.4f, 0.0f}, {0.6f, 1.0f}));
    raise.Duration(Seconds(5.0f));
    raise.DelayTime(Seconds(delaySeconds));
    m_clock.StartAnimation(L"amp", raise);
}

// The clock only runs while the wall does; stopped, the tiles rest where the swell left them.
void AlbumWall::RunClock(bool run) {
    if (run == m_clockRunning) return;
    m_clockRunning = run;
    const auto& comp = gfx::CompositionHost::Get().Compositor();
    if (run) {
        m_clockStarted = std::chrono::steady_clock::now();
        auto tick = comp.CreateScalarKeyFrameAnimation();
        tick.InsertKeyFrame(0.0f, m_clockSeconds);
        tick.InsertKeyFrame(1.0f, m_clockSeconds + 3600.0f, comp.CreateLinearEasingFunction());
        tick.Duration(Seconds(3600.0f));
        m_clock.StartAnimation(L"t", tick);
        ApplyPace();
    } else {
        m_clockSeconds +=
            m_pace * std::chrono::duration<float>(std::chrono::steady_clock::now() - m_clockStarted).count();
        m_clock.StopAnimation(L"t");
        m_clock.InsertScalar(L"t", m_clockSeconds);
    }
}

void AlbumWall::SetPaused(bool paused) {
    const float pace = paused ? kPausedPace : 1.0f;
    if (pace == m_pace) return;
    if (m_clockRunning) { // the clock's own count of seconds goes on at the old pace up to now
        const auto now = std::chrono::steady_clock::now();
        m_clockSeconds += m_pace * std::chrono::duration<float>(now - m_clockStarted).count();
        m_clockStarted = now;
    }
    m_pace = pace;
    ApplyPace();
}

// Both running animations, the drift and the clock, play at the current pace.
void AlbumWall::ApplyPace() {
    try {
        if (m_drifting)
            if (auto controller = m_drift.TryGetAnimationController(L"Offset")) controller.PlaybackRate(m_pace);
        if (m_clockRunning)
            if (auto controller = m_clock.TryGetAnimationController(L"t")) controller.PlaybackRate(m_pace);
    } catch (...) {
    }
}

void AlbumWall::SetRunning(bool running) {
    if (running == m_running) return;
    m_running = running;
    RunClock(running);
    if (running) {
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
    anim.InsertKeyFrame(1.0f, float3{0, static_cast<float>(m_tile), 0}, comp.CreateLinearEasingFunction());
    anim.Duration(Seconds(kDriftSeconds));
    m_drift.StartAnimation(L"Offset", anim);
    batch.End();
    ApplyPace();
    batch.Completed([this, generation, alive = std::weak_ptr<int>(m_alive)](const auto&, const auto&) {
        if (alive.expired() || generation != m_driftGeneration) return;
        OnDriftEnd();
    });
}

void AlbumWall::OnDriftEnd() {
    // The bottom row has left the screen: every tile now shows the place above it, the first
    // row (above the window) places nobody has seen yet, and the drift starts again.
    m_drifting = false;
    --m_driftRows;
    const int gone = m_driftRows + static_cast<int>(m_rows.size()) + 1;
    for (auto it = m_cells.begin(); it != m_cells.end();)
        it = static_cast<int>(it->first >> 32) > gone ? m_cells.erase(it) : std::next(it);
    m_drift.Offset(float3{0, 0, 0});
    Rebuild(false);
    ReleaseHidden();
    StartDrift();
}

} // namespace glasswave::ui
