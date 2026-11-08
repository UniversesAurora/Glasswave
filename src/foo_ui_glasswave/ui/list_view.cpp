#include "pch.h"
#include "ui/list_view.h"
#include "gfx/composition_host.h"
#include "ui/art_cache.h"
#include "ui/text.h"
#include "win7/win7_resources.h"
#include "ui/win7_menu.h"
#include "media/favorites.h"
#include "netease/handle.h"
#include "ui/i18n.h"
#include "wmp/wmp_painter.h"

namespace glasswave::ui {

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
constexpr D2D1_SIZE_F kAlbumTile{132, 160};
constexpr float kAlbumTileArt = 100;
constexpr D2D1_SIZE_F kArtistTile{240, 108};
constexpr float kArtistTileArt = 88;
constexpr size_t kMaxArtBitmaps = 128;  // decoded covers kept for scrolling and view switches

constexpr uint32_t kInk = 0x1E1E1E, kSecondaryInk = 0x4A4A4A, kGreyInk = 0x7A7A7A, kPlayingInk = 0x1E5BA8;
constexpr uint32_t kGroupInk = 0x2E64B4, kGroupRule = 0xC9D7E8, kLinkInk = 0x0066CC;

// {5E8B2C47-9A1D-4F63-B0E2-6C4A7D9F1B38}: column order and widths per column set
cfg_var_modern::cfg_string cfg_columns({0x5e8b2c47, 0x9a1d, 0x4f63, {0xb0, 0xe2, 0x6c, 0x4a, 0x7d, 0x9f, 0x1b, 0x38}},
                                       "");
// {5E8B2C48-9A1D-4F63-B0E2-6C4A7D9F1B38}: split a sorted Details list into sections
cfg_var_modern::cfg_int cfg_showGroups({0x5e8b2c48, 0x9a1d, 0x4f63, {0xb0, 0xe2, 0x6c, 0x4a, 0x7d, 0x9f, 0x1b, 0x38}},
                                       1);

// Columns drawn as controls instead of text.
enum class CellKind { Text, Rating, Favorite, Art };
CellKind KindOf(const media::Column& c) {
    if (!c.format) return CellKind::Text;
    if (std::strcmp(c.format, "[%rating%]") == 0) return CellKind::Rating;
    if (std::strcmp(c.format, "<favorite>") == 0) return CellKind::Favorite;
    if (std::strcmp(c.format, media::kArtFormat) == 0) return CellKind::Art;
    return CellKind::Text;
}
constexpr float kStar = 13;  // STARS.PNG cell width (13x16)

// Ratings go through foobar2000's own Playback Statistics context command
// ("Playback Statistics" / "Rating" / "1".."5"), so they land where %rating% reads them.
bool RateTrack(const metadb_handle_ptr& track, int stars) {
    metadb_handle_list items;
    items.add_item(track);
    auto cm = contextmenu_manager::get();
    cm->init_context(items, contextmenu_manager::flag_view_full);
    contextmenu_node* node = cm->get_root();
    const std::string value = stars > 0 ? std::to_string(stars) : "<not set>";
    for (const char* want : {"Playback Statistics", "Rating", value.c_str()}) {
        contextmenu_node* next = nullptr;
        for (t_size i = 0, n = node ? node->get_num_children() : 0; i < n && !next; ++i) {
            contextmenu_node* c = node->get_child(i);
            if (!c || !c->get_name()) continue;
            std::string name = c->get_name();
            name.erase(std::remove(name.begin(), name.end(), '&'), name.end());
            if (pfc::stricmp_ascii(name.c_str(), want) == 0) next = c;
        }
        if (!next) return false;
        node = next;
    }
    node->execute();
    return true;
}

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
}  // namespace

ListView::ListView(const wuc::ContainerVisual& parent) {
    const auto& comp = gfx::CompositionHost::Get().Compositor();
    m_root = comp.CreateContainerVisual();
    m_header = comp.CreateSpriteVisual();
    m_rows = comp.CreateSpriteVisual();
    m_scroll = comp.CreateSpriteVisual();
    m_root.Children().InsertAtTop(m_rows);
    m_root.Children().InsertAtTop(m_header);
    m_root.Children().InsertAtTop(m_scroll);
    parent.Children().InsertAtTop(m_root);
    m_artToken = ArtCache::Get().Subscribe([this] {
        if (m_mode != Mode::Plain) PaintRows();
    });
}

ListView::~ListView() {
    if (auto* cache = ArtCache::Existing()) cache->Unsubscribe(m_artToken);
}

// ---- Geometry -------------------------------------------------------------------------------

int ListView::RowHeightPx() const {
    return static_cast<int>(std::lround((ArtColumnVisible() ? kArtRowHeight : kRowHeight) * m_scale));
}
int ListView::HeaderHeightPx() const {
    return m_mode == Mode::Tiles ? 0 : static_cast<int>(std::lround(kHeaderHeight * m_scale));
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
    return m_source && m_source->Tiles() == media::TileStyle::Artist ? kArtistTile : kAlbumTile;
}

size_t ListView::TileColumns() const {
    const float w = RowsWidth() - 2 * kTilePad;
    return static_cast<size_t>(std::max(1.0f, std::floor(w / TileSize().width)));
}

RECT ListView::RowsRect() const {
    return {m_bounds.left, m_bounds.top + HeaderHeightPx(), m_bounds.right - ScrollWidthPx(), m_bounds.bottom};
}

float ListView::RowsWidth() const {
    const RECT r = RowsRect();
    return std::max<LONG>(r.right - r.left, 0) / m_scale;
}

float ListView::ArtSize() const { return kGroupMinLines * (RowHeightPx() / m_scale) - 8; }
float ListView::AlbumTextLeft(float albumLeft) const { return albumLeft + 3 + ArtSize() + 8; }

float ListView::LineHeightDip(const Line& line) const {
    if (m_mode == Mode::Grouped && !m_groupArt && line.kind == Line::GroupHeader)
        return static_cast<float>(std::lround(kRowHeight * m_scale)) / m_scale;
    return RowHeightPx() / m_scale;
}

float ListView::LineTopDip(size_t line) const {
    if (m_mode == Mode::Grouped) return m_lineTop.empty() ? 0 : m_lineTop[std::min(line, m_lineTop.size() - 1)];
    return static_cast<float>(line) * (LineHeightPx() / m_scale);
}

size_t ListView::LineAtDip(float y) const {
    if (m_mode == Mode::Grouped) {
        if (m_lineTop.size() < 2) return 0;
        auto it = std::upper_bound(m_lineTop.begin(), m_lineTop.end(), y);
        const size_t line = it == m_lineTop.begin() ? 0 : static_cast<size_t>(it - m_lineTop.begin() - 1);
        return std::min(line, m_lineTop.size() - 2);
    }
    return y > 0 ? static_cast<size_t>(y / std::max(LineHeightPx() / m_scale, 0.001f)) : 0;
}

float ListView::RowsHeightDip() const {
    const RECT r = RowsRect();
    return std::max<LONG>(r.bottom - r.top, 0) / m_scale;
}

size_t ListView::LineCount() const {
    const size_t n = m_source ? m_source->Count() : 0;
    switch (m_mode) {
    case Mode::Grouped: return m_lines.size();
    case Mode::Tiles: {
        const size_t cols = TileColumns();
        return (n + cols - 1) / cols;
    }
    default: return n;
    }
}

size_t ListView::LineOfRow(size_t row) const {
    switch (m_mode) {
    case Mode::Grouped: return row < m_rowLine.size() ? m_rowLine[row] : 0;
    case Mode::Tiles: return row / TileColumns();
    default: return row;
    }
}

size_t ListView::VisibleLines() const {
    const float viewH = RowsHeightDip();
    if (m_mode == Mode::Grouped && !m_lineTop.empty()) {
        size_t n = 0;
        float y = 0;
        for (size_t i = m_top; i < m_lines.size() && y < viewH; ++i) {
            y += LineHeightDip(m_lines[i]);
            ++n;
        }
        return n;
    }
    return static_cast<size_t>(viewH / std::max(LineHeightPx() / m_scale, 0.001f));
}

size_t ListView::MaxTop() const {
    const size_t n = LineCount();
    if (m_mode == Mode::Grouped && !m_lineTop.empty()) {
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
    return {(pt.x - rr.left) / m_scale + ScrollX(), (pt.y - rr.top) / m_scale + LineTopDip(m_top)};
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
    if (!m_source) return;
    if (m_source->Tiles() != media::TileStyle::None) {
        m_mode = Mode::Tiles;
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
    if (m_mode == Mode::Grouped) {
        m_lineTop.reserve(m_lines.size() + 1);
        float y = 0;
        for (const Line& line : m_lines) {
            m_lineTop.push_back(y);
            y += LineHeightDip(line);
        }
        m_lineTop.push_back(y);
    }
}

void ListView::SetSource(media::ListSource* source, bool resetScroll) {
    m_source = source;
    if (m_source && m_source->CanGroup()) m_source->SetGrouping(cfg_showGroups.get() != 0);
    m_hot = SIZE_MAX;
    m_hotGroup = SIZE_MAX;
    m_hotGroupHeader = SIZE_MAX;
    m_hotLinkGroup = SIZE_MAX;
    m_anchor = source ? source->Focus() : SIZE_MAX;
    if (resetScroll) {
        m_top = 0;
        m_scrollX = 0;
    }
    BuildLines();
    PaintHeader();
    Refresh();
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
    BuildLines();  // rows, groups or the view style may have changed
    m_top = std::min(m_top, MaxTop());
    m_scrollX = std::clamp(m_scrollX, 0.0f, std::max(0.0f, ContentWidth() + 4 - RowsWidth()));
    if (m_source && m_hot != SIZE_MAX && m_hot >= m_source->Count()) m_hot = SIZE_MAX;
    m_hotGroup = SIZE_MAX;
    m_hotGroupHeader = SIZE_MAX;
    m_hotLinkGroup = SIZE_MAX;
    PaintHeader();
    PaintRows();
    PaintScrollBar();
    CheckNearEnd();
}

void ListView::ScrollTo(size_t top) {
    top = std::min(top, MaxTop());
    if (top == m_top) return;
    m_top = top;
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
    if (m_mode == Mode::Grouped && !m_lineTop.empty()) {
        const float viewH = RowsHeightDip();
        const float top = LineTopDip(m_top);
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
    case Mode::Tiles: return std::min(m_top * TileColumns(), m_source->Count() - 1);
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

// ---- Painting -------------------------------------------------------------------------------

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
            const float line = 1.0f / s * std::max(1.0f, std::floor(s));
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
    const RECT rr = RowsRect();
    const int w = std::max<int>(rr.right - rr.left, 1), h = std::max<int>(rr.bottom - rr.top, 1);
    if (!m_rowsSurface || m_rowsSurfaceSize.cx != w || m_rowsSurfaceSize.cy != h) {
        m_rowsSurface = host.CreateSurface(w, h);
        m_rowsSurfaceSize = {w, h};
        auto brush = host.Compositor().CreateSurfaceBrush(m_rowsSurface);
        brush.Stretch(wuc::CompositionStretch::None);
        brush.HorizontalAlignmentRatio(0);
        brush.VerticalAlignmentRatio(0);
        m_rows.Brush(brush);
    }
    PlaceVisual(m_rows, 0, static_cast<float>(HeaderHeightPx()), static_cast<float>(w), static_cast<float>(h));
    const float s = m_scale;
    host.Draw(m_rowsSurface, s, [&](ID2D1DeviceContext* dc) {
        const D2D1_SIZE_F size{w / s, h / s};
        if (!m_source) return;
        if (m_source->Count() == 0) {
            Text(dc, Tr(m_source->EmptyText()), {20, 20, size.width - 20, 60}, kGreyInk, DWRITE_TEXT_ALIGNMENT_CENTER);
            return;
        }
        // Favorite column shown: which tracks are in the Favorites playlist.
        m_favorites.clear();
        if (m_mode != Mode::Tiles) {
            const auto& srcCols = m_source->Columns();
            bool shown = false;
            for (const Col& c : Columns(0)) shown = shown || KindOf(srcCols[c.src]) == CellKind::Favorite;
            if (const t_size p = shown ? media::FavoritesPlaylist(false) : SIZE_MAX; p != SIZE_MAX) {
                metadb_handle_list items;
                playlist_manager::get()->playlist_get_all_items(p, items);
                for (t_size i = 0; i < items.get_count(); ++i) m_favorites.insert(items[i].get_ptr());
            }
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
            const float line = std::max(1.0f, std::floor(s)) / s;
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
            const float top = LineTopDip(m_top);
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
            const metadb_handle_ptr item = m_source->Item(row);
            const bool fav = item.is_valid() && [&] {
                int64_t remoteId = 0;
                if (netease::ParseSongPath(item->get_path(), remoteId)) return media::IsFavorite(item);
                return m_favorites.count(item.get_ptr()) != 0;
            }();
            const int id = fav ? 5530 : row == m_hot ? 5531 : 5533;
            const float fx = std::round((c.x + (c.w - 14) / 2) * m_scale) / m_scale;
            const float fy = std::round((y + (rowH - 14) / 2) * m_scale) / m_scale;
            if (win7::Draw(dc, win7::Module::Wmploc, id, {fx, fy})) continue;
            if (fav) Text(dc, L"★", {c.x + kCellPad, y + 0.5f, c.x + c.w - kCellPad, y + 0.5f + rowH}, 0xD4A017,
                          DWRITE_TEXT_ALIGNMENT_CENTER);
            continue;
        }
        const bool primary = col.flex;
        Text(dc, m_source->Cell(row, c.src), {c.x + kCellPad, y + 0.5f, c.x + c.w - kCellPad, y + 0.5f + rowH},
             isPlaying ? kPlayingInk : (primary ? kInk : kSecondaryInk),
             col.rightAlign ? DWRITE_TEXT_ALIGNMENT_TRAILING : DWRITE_TEXT_ALIGNMENT_LEADING,
             isPlaying && primary ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL);
    }
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
        const float y = i * rowH;
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
    float y = 0;
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
            const float line1 = std::floor(m_scale) / m_scale;
            const float ly = std::round((y + rowH / 2 + 1) * m_scale) / m_scale;
            if (lx < viewRight - 6)
                dc->FillRectangle({lx, ly, viewRight - 6, ly + line1}, Brush(dc, kGroupRule).get());
            // Hovering the bar shows only its outline, as WMP does (no selection fill).
            if (line.group == m_hotGroupHeader) {
                const float px = std::max(1.0f, std::floor(m_scale)) / m_scale;
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
        const float top = LineTopDip(m_groupLine[gi] + 1) - LineTopDip(m_top);
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
        DrawArt(dc, g.art, {albumL + 3, top + 4, albumL + 3 + artSize, top + 4 + artSize});
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
    const size_t cols = TileColumns();
    const D2D1_SIZE_F ts = TileSize();
    const bool beside = m_source->Tiles() == media::TileStyle::Artist;
    const size_t focus = m_source->Focus();
    const float lineH = LineHeightPx() / m_scale;
    const size_t visible = VisibleLines() + 1;
    for (size_t i = 0; i < visible; ++i) {
        const float y = i * lineH;
        for (size_t c = 0; c < cols; ++c) {
            const size_t idx = (m_top + i) * cols + c;
            if (idx >= n) return;
            const float x = kTilePad + c * ts.width;
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
            if (beside) {
                const float a = kArtistTileArt, ay = y + (ts.height - a) / 2;
                DrawArt(dc, info.art, {x + 10, ay, x + 10 + a, ay + a});
                const float tx = x + 10 + a + 12;
                for (size_t k = 0; k < info.lines.size(); ++k)
                    Text(dc, info.lines[k], {tx, ay + 2 + k * 18, x + ts.width - 6, ay + 2 + (k + 1) * 18},
                         k == 0 ? kInk : kGreyInk);
            } else {
                const float a = kAlbumTileArt, ax = x + (ts.width - a) / 2, ay = y + 10;
                if (info.icon)  // playlists page: WMP's large playlist icon instead of a cover
                    win7::DrawStretched(dc, win7::Module::Wmploc, L"#" + std::to_wstring(info.icon), {ax + 10, ay + 10, ax + a - 10, ay + a - 10});
                else
                    DrawArt(dc, info.art, {ax, ay, ax + a, ay + a});
                for (size_t k = 0; k < 2; ++k)
                    Text(dc, info.lines[k], {x + 6, ay + a + 5 + k * 17, x + ts.width - 6, ay + a + 5 + (k + 1) * 17},
                         k == 0 ? kInk : kGreyInk, DWRITE_TEXT_ALIGNMENT_CENTER);
            }
        }
    }
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
        if (src) {
            src->GetSize(&sw, &sh);
            if (SUCCEEDED(dc->CreateBitmapFromWicBitmap(src.get(), nullptr, bmp.put()))) {
                m_artBitmaps[cacheKey] = {bmp, ++m_artClock, version};
                if (m_artBitmaps.size() > kMaxArtBitmaps) {
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
        } else if (!bmp) {
            if (state == ArtCache::State::Pending) return;  // painted when it arrives
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

bool ListView::ThumbGeometry(float* top, float* height) const {
    const size_t n = LineCount(), v = VisibleLines();
    const float arrow = kScrollArrow * m_scale;
    const float trackH = static_cast<float>(std::max<LONG>(RowsRect().bottom - RowsRect().top, 0)) - arrow * 2;
    if (n <= v || trackH <= 0) return false;
    const float minH = 24.0f * m_scale;
    *height = std::min(trackH, std::max(minH, trackH * v / n));
    *top = arrow + (trackH - *height) * m_top / std::max<size_t>(MaxTop(), 1);
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
    return active;
}

bool ListView::OnScrollRepeat() {
    const size_t page = std::max<size_t>(VisibleLines(), 1) - 1;
    switch (m_scrollPressed) {
    case 1:
        if (m_top == 0) return false;
        ScrollTo(m_top > 3 ? m_top - 3 : 0);
        return m_top > 0;
    case 2:
        if (m_top >= MaxTop()) return false;
        ScrollTo(m_top + 3);
        return m_top < MaxTop();
    case 3: {
        float top = 0, height = 0;
        if (!ThumbGeometry(&top, &height)) return false;
        const RECT rr = RowsRect();
        const float y = static_cast<float>(m_scrollPt.y - rr.top);
        if (m_scrollTrackDir == 1) {
            if (y >= top) return false;  // the thumb has reached the pointer
            if (m_top == 0) return false;
            ScrollTo(m_top > page ? m_top - page : 0);
            return true;
        }
        if (m_scrollTrackDir == 2) {
            if (y < top + height) return false;
            if (m_top >= MaxTop()) return false;
            ScrollTo(m_top + page);
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
        wp::ScrollBar(dc, {w / s, h / s}, top / s, has ? height / s : 0, m_scrollBlend.data());
    });
    PlaceVisual(m_scroll, static_cast<float>(rr.right - m_bounds.left), static_cast<float>(HeaderHeightPx()),
                static_cast<float>(w), static_cast<float>(h));
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
    const size_t line = LineAtDip(ContentPoint(pt).y);
    switch (m_mode) {
    case Mode::Tiles: {
        const size_t cols = TileColumns();
        if (x < kTilePad) return hit;
        const size_t col = static_cast<size_t>((x - kTilePad) / TileSize().width);
        const size_t idx = line * cols + col;
        if (col >= cols || idx >= m_source->Count()) return hit;
        hit.part = Part::Row;
        hit.row = idx;
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
                if (k < g.lines.size() && !g.linkKeys[k].empty() && !g.lines[k].empty() && x >= tx &&
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
    case Part::Link: return IDC_HAND;
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
            ScrollTo(static_cast<size_t>(std::lround(frac * MaxTop())));
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
    const size_t hot = hit.part == Part::Row ? hit.row : SIZE_MAX;
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
    const size_t linkLine = hit.part == Part::Link ? hit.line : 0;
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
    if (m_hotHeader != SIZE_MAX) {
        m_hotHeader = SIZE_MAX;
        PaintHeader();
    }
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
            const size_t page = std::max<size_t>(VisibleLines(), 1) - 1;
            if (pt.y - RowsRect().top < top) {
                m_scrollTrackDir = 1;
                ScrollTo(m_top > page ? m_top - page : 0);
            } else {
                m_scrollTrackDir = 2;
                ScrollTo(m_top + page);
            }
        }
        m_scrollPressed = 3;
        m_scrollPt = pt;
        if (onScrollAnim) onScrollAnim();
        if (onScrollHold) onScrollHold();
        break;
    }
    case Part::ArrowUp: {
        ScrollTo(m_top > 3 ? m_top - 3 : 0);
        m_scrollPressed = 1;
        m_scrollPt = pt;
        if (onScrollAnim) onScrollAnim();
        if (onScrollHold) onScrollHold();
        break;
    }
    case Part::ArrowDown: {
        ScrollTo(m_top + 3);
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
        // Rating stars and the Favorite star act on the clicked song (and select it).
        const int kind = CellAt(pt, nullptr);
        if (kind == static_cast<int>(CellKind::Rating) && StarAt(pt) >= 0) {
            if (const auto item = m_source->Item(hit.row); item.is_valid() && !RateTrack(item, StarAt(pt) + 1))
                popup_message::g_show("Ratings need foobar2000's Playback Statistics (Library > Playback Statistics).",
                                      "Rating");
        } else if (kind == static_cast<int>(CellKind::Favorite)) {
            if (const auto item = m_source->Item(hit.row); item.is_valid()) media::ToggleFavorite(item);
        }
        MoveFocus(hit.row, ctrl, shift);
        break;
    }
    case Part::Link:
        if (onLink) onLink(hit.group, hit.line);
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
    case Drag::Thumb: PaintScrollBar(); break;
    case Drag::Marquee: PaintRows(); break;
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
        const auto& g = (*m_source->Groups())[hit.group];
        if (g.count) m_source->Activate(g.first);
        PaintRows();
    } else if (hit.part == Part::Row && m_source) {
        m_source->Activate(hit.row);
        PaintRows();
    }
}

void ListView::OnRButtonUp(HWND owner, POINT pt) {
    if (!m_source) return;
    const Hit hit = HitAt(pt);
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
                const float tx0 = kTilePad + c * tw, tx1 = tx0 + tw;
                const size_t idx = line * cols + c;
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
        if (m_mode != Mode::Tiles) ScrollXTo(m_scrollX - delta / static_cast<float>(WHEEL_DELTA) * 48.0f);
        return;
    }
    const int step = m_mode == Mode::Tiles ? 1 : 3;
    const int lines = step * (-delta / WHEEL_DELTA);
    if (lines < 0)
        ScrollTo(m_top > static_cast<size_t>(-lines) ? m_top + lines : 0);
    else
        ScrollTo(m_top + lines);
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
