#include "pch.h"
#include "ui/nav_pane.h"
#include "gfx/composition_host.h"
#include "ui/text.h"
#include "win7/win7_resources.h"

namespace glasswave::ui {

namespace wp = wmp::paint;
using winrt::Windows::Foundation::Numerics::float2;
using winrt::Windows::Foundation::Numerics::float3;

namespace {
constexpr float kRowHeight = 22.0f;
constexpr float kIndent = 16.0f;

// WMP 12 navigation icons: wmploc icon groups (see docs/design/win7-asset-map.md).
int WmpIconId(wmp::paint::NavIcon icon, bool autoPlaylist) {
    using I = wmp::paint::NavIcon;
    switch (icon) {
    case I::Library: return 29527;
    case I::Playlist: return autoPlaylist ? 29572 : 29522;
    case I::NowPlaying: return 29522;
    case I::Music: return 29511;
    case I::Artist: return 29514;
    case I::Album: return 29510;
    case I::Genre: return 29517;
    default: return 0;
    }
}

// Icon as a WIC bitmap at `px` pixels, cached; nullptr when the art is missing.
std::map<std::pair<int, int>, winrt::com_ptr<IWICBitmapSource>>& IconCache() {
    static std::map<std::pair<int, int>, winrt::com_ptr<IWICBitmapSource>> cache;
    return cache;
}

IWICBitmapSource* WmpIcon(int id, int px) {
    auto& cache = IconCache();
    const auto key = std::make_pair(id, px);
    if (auto it = cache.find(key); it != cache.end()) return it->second.get();
    winrt::com_ptr<IWICBitmapSource> result;
    if (HICON h = win7::Resources::Get().Icon(win7::Module::Wmploc, id, px)) {
        auto* wic = gfx::CompositionHost::Get().Wic();
        winrt::com_ptr<IWICBitmap> bmp;
        winrt::com_ptr<IWICFormatConverter> conv;
        if (SUCCEEDED(wic->CreateBitmapFromHICON(h, bmp.put())) && SUCCEEDED(wic->CreateFormatConverter(conv.put())) &&
            SUCCEEDED(conv->Initialize(bmp.get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                       WICBitmapPaletteTypeMedianCut)))
            result = conv;
        DestroyIcon(h);
    }
    cache[key] = result;
    return result.get();
}

bool DrawWmpIcon(ID2D1DeviceContext* dc, D2D1_POINT_2F center, wmp::paint::NavIcon icon, bool autoPlaylist, float scale,
                 int overrideId = 0) {
    const int id = overrideId ? overrideId : WmpIconId(icon, autoPlaylist);
    if (!id) return false;
    const int px = static_cast<int>(std::lround(16 * scale));
    IWICBitmapSource* src = WmpIcon(id, px);
    if (!src) return false;
    winrt::com_ptr<ID2D1Bitmap> bmp;
    if (FAILED(dc->CreateBitmapFromWicBitmap(src, nullptr, bmp.put()))) return false;
    const float x = std::round((center.x - 8) * scale) / scale, y = std::round((center.y - 8) * scale) / scale;
    dc->DrawBitmap(bmp.get(), D2D1::RectF(x, y, x + px / scale, y + px / scale), 1.0f,
                   D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
    return true;
}
}  // namespace

NavPane::NavPane(const winrt::Windows::UI::Composition::ContainerVisual& parent) {
    m_visual = gfx::CompositionHost::Get().Compositor().CreateSpriteVisual();
    parent.Children().InsertAtTop(m_visual);
    Rebuild();
}

NavPane::~NavPane() { IconCache().clear(); }  // WIC objects go before COM does

int NavPane::RowHeightPx() const { return static_cast<int>(std::lround(kRowHeight * m_scale)); }

void NavPane::Rebuild() {
    m_items.clear();
    m_items.push_back({Target::PlaylistsHeader, 0, 0, L"Playlists", wp::NavIcon::Playlist, true});
    if (m_playlistsExpanded) {
        auto pm = playlist_manager::get();
        pfc::string8 name;
        for (t_size i = 0, n = pm->get_playlist_count(); i < n; ++i) {
            pm->playlist_get_name(i, name);
            m_items.push_back({Target::Playlist, i, 1, Wide(name), wp::NavIcon::Playlist, false});
        }
    }
    m_items.push_back({Target::Music, 0, 0, L"Music", wp::NavIcon::Music, true});
    if (m_musicExpanded) {
        m_items.push_back({Target::Artists, 0, 1, L"Artist", wp::NavIcon::Artist, false});
        m_items.push_back({Target::Albums, 0, 1, L"Album", wp::NavIcon::Album, false});
        m_items.push_back({Target::Genres, 0, 1, L"Genre", wp::NavIcon::Genre, false});
    }
    m_hot = SIZE_MAX;
    Paint();
}

void NavPane::SetBounds(const RECT& px, float scale) {
    const bool changed = (px.right - px.left) != (m_bounds.right - m_bounds.left) ||
                         (px.bottom - px.top) != (m_bounds.bottom - m_bounds.top) || scale != m_scale;
    m_bounds = px;
    m_scale = scale;
    m_visual.Offset(float3{static_cast<float>(px.left), static_cast<float>(px.top), 0});
    if (changed) Paint();
}

void NavPane::Select(Target target, size_t playlist) {
    m_selTarget = target;
    m_selPlaylist = playlist;
    Paint();
}

const NavPane::Item* NavPane::ItemAt(POINT pt) const {
    if (!HitTest(pt)) return nullptr;
    const size_t i = m_top + static_cast<size_t>((pt.y - m_bounds.top - std::lround(6 * m_scale)) /
                                                 std::max(RowHeightPx(), 1));
    return (pt.y - m_bounds.top) >= std::lround(6 * m_scale) && i < m_items.size() ? &m_items[i] : nullptr;
}

const NavPane::Item* NavPane::OnLButtonDown(POINT pt) {
    const Item* item = ItemAt(pt);
    if (!item) return nullptr;
    // Clicking the disclosure triangle toggles; clicking the label also selects.
    const float x = (pt.x - m_bounds.left) / m_scale;
    if (item->expandable && x < 16 + item->level * kIndent) {
        if (item->target == Target::PlaylistsHeader) m_playlistsExpanded = !m_playlistsExpanded;
        if (item->target == Target::Music) m_musicExpanded = !m_musicExpanded;
        Rebuild();
        return nullptr;
    }
    return item;
}

void NavPane::OnMouseMove(POINT pt) {
    const Item* item = ItemAt(pt);
    const size_t hot = item ? static_cast<size_t>(item - m_items.data()) : SIZE_MAX;
    if (hot != m_hot) {
        m_hot = hot;
        Paint();
    }
}

void NavPane::OnMouseLeave() {
    if (m_hot != SIZE_MAX) {
        m_hot = SIZE_MAX;
        Paint();
    }
}

void NavPane::OnWheel(int delta) {
    const size_t visible = static_cast<size_t>((m_bounds.bottom - m_bounds.top) / std::max(RowHeightPx(), 1));
    const size_t maxTop = m_items.size() > visible ? m_items.size() - visible : 0;
    const int lines = 3 * (-delta / WHEEL_DELTA);
    m_top = lines < 0 ? (m_top > static_cast<size_t>(-lines) ? m_top + lines : 0) : std::min(m_top + lines, maxTop);
    Paint();
}

void NavPane::Paint() {
    auto& host = gfx::CompositionHost::Get();
    const int w = std::max<int>(m_bounds.right - m_bounds.left, 1), h = std::max<int>(m_bounds.bottom - m_bounds.top, 1);
    const float s = m_scale;
    host.Repaint(m_visual, w, h, s, [&](ID2D1DeviceContext* dc) {
        const float width = w / s;
        const float rowH = RowHeightPx() / s;
        winrt::com_ptr<ID2D1SolidColorBrush> ink, sep;
        dc->CreateSolidColorBrush(D2D1::ColorF(0x1E395B), ink.put());
        dc->CreateSolidColorBrush(D2D1::ColorF(0xD5DFE9), sep.put());
        // Divider against the list
        const float L = std::max(1.0f, std::floor(s)) / s;
        dc->FillRectangle({width - L, 0, width, h / s}, sep.get());
        float y = 6;
        for (size_t i = m_top; i < m_items.size(); ++i, y += rowH) {
            const Item& it = m_items[i];
            const bool selected = it.target == m_selTarget &&
                                  (it.target != Target::Playlist || it.playlist == m_selPlaylist);
            const bool hot = i == m_hot;
            if (selected || hot)
                wp::RowPlate(dc, {4, y, width - 6, y + rowH},
                             selected ? (hot ? wp::RowState::SelectedHot : wp::RowState::Selected) : wp::RowState::Hot,
                             true);
            const float x = 8 + it.level * kIndent;
            if (it.expandable) {
                const bool expanded = it.target == Target::PlaylistsHeader ? m_playlistsExpanded : m_musicExpanded;
                // Win7 tree expander (aero Explorer::TreeView glyph 928: closed / open, 16x16).
                const float gy = std::round((y + rowH / 2 - 8) * s) / s, gx = std::round((x - 6) * s) / s;
                if (!win7::DrawPartTo(dc, win7::Module::Aero, 928, D2D1::RectU(0, expanded ? 16 : 0, 16, expanded ? 32 : 16),
                                      {gx, gy, gx + 16, gy + 16}))
                    wp::Disclosure(dc, {x + 2, y + rowH / 2}, expanded);
            }
            const bool autoPlaylist = it.target == Target::Playlist && autoplaylist_manager::get()->is_client_present(it.playlist);
            const int special = it.target == Target::Playlist && it.text == L"Favorites" ? 29526 : 0;  // star
            if (!DrawWmpIcon(dc, {x + 16, y + rowH / 2}, it.icon, autoPlaylist, s, special))
                wp::NavGlyph(dc, {x + 16, y + rowH / 2}, it.icon);
            auto layout = MakeText(it.text, 12.0f, DWRITE_FONT_WEIGHT_NORMAL, width - x - 34, rowH);
            dc->DrawTextLayout({x + 26, y}, layout.get(), ink.get());
        }
    });
}

}  // namespace glasswave::ui
