#include "pch.h"

#include "ui/breadcrumb.h"

#include "gfx/composition_host.h"
#include "ui/i18n.h"
#include "ui/text.h"
#include "win7/win7_resources.h"
#include "wmp/wmp_painter.h"

namespace glasswave::ui {
namespace {

using winrt::com_ptr;
namespace wp = wmp::paint;

constexpr float kPlateH = 21;  // DIP (owner: 17 was too low), centred with the text in the bar
constexpr float kArrowW = 12;  // DIP
constexpr float kNamePad = 8;  // DIP around a name
constexpr float kMenuDrop = 23; // DIP from the bar's top to where an arrow's menu opens
constexpr float kFadeIn = 0.25f, kFadeOut = 0.1f; // per frame: quicker in than out, as Explorer

D2D1_COLOR_F Hex(uint32_t rgb, float a = 1.0f) {
    return D2D1::ColorF(((rgb >> 16) & 0xFF) / 255.0f, ((rgb >> 8) & 0xFF) / 255.0f, (rgb & 0xFF) / 255.0f, a);
}

float NameWidth(const Breadcrumb::Level& level) {
    if (level.text.empty()) return 0;
    auto t = MakeText(level.literal ? level.text : Tr(level.text), 12.0f, DWRITE_FONT_WEIGHT_NORMAL, 400, 20);
    DWRITE_TEXT_METRICS tm{};
    t->GetMetrics(&tm);
    return std::ceil(tm.widthIncludingTrailingWhitespace) + kNamePad;
}

} // namespace

void Breadcrumb::SetVisual(winrt::Windows::UI::Composition::SpriteVisual visual) {
    m_visual = std::move(visual);
    m_deviceReplaced = gfx::CompositionHost::Get().OnDeviceReplaced([this] { Paint(); });
}

void Breadcrumb::SetPath(std::vector<Level> path) {
    // An arrow always has a menu: a level the path gives none lists at least the next one.
    for (size_t i = 0; i + 1 < path.size(); ++i) {
        if (path[i].children) continue;
        const Level& next = path[i + 1];
        path[i].children = [text = next.literal ? next.text : Tr(next.text), open = next.open] {
            return std::vector<MenuAction>{{text, open}};
        };
    }
    m_path = std::move(path);
    m_open = -1;
    Layout();
    Paint();
}

void Breadcrumb::SetBounds(const RECT& bounds, float scale) {
    if (EqualRect(&bounds, &m_bounds) && scale == m_scale) return;
    m_bounds = bounds;
    m_scale = scale;
    Layout();
    Paint();
}

// The root, then as many of the last levels as fit; the first ones that do not go to the
// root arrow's menu (before what it lists itself).
void Breadcrumb::Layout() {
    m_placed.clear();
    if (m_path.empty()) return;
    const float room = std::max<LONG>(m_bounds.right - m_bounds.left, 0) / m_scale;
    std::vector<float> widths;
    for (size_t i = 0; i < m_path.size(); ++i) {
        const bool arrow = m_path[i].children || i + 1 < m_path.size();
        widths.push_back(NameWidth(m_path[i]) + (arrow ? kArrowW : 0));
    }
    float total = 0;
    for (float w : widths) total += w;
    size_t first = 1; // the first level shown after the root
    while (room > 0 && total > room && first + 1 < m_path.size()) total -= widths[first++];
    Level root = m_path.front();
    if (first > 1) {
        std::vector<MenuAction> hidden;
        for (size_t i = 1; i < first; ++i)
            hidden.push_back({m_path[i].literal ? m_path[i].text : Tr(m_path[i].text), m_path[i].open});
        root.children = [hidden, below = root.children] {
            auto items = hidden;
            if (below) {
                items.push_back({});
                for (auto& item : below()) items.push_back(std::move(item));
            }
            return items;
        };
    }
    float x = 0;
    const auto place = [&](const Level& level, bool last) {
        Placed p;
        p.level = level;
        p.arrow = level.children || !last;
        p.x0 = x;
        x += NameWidth(level);
        p.split = x;
        if (p.arrow) x += kArrowW;
        p.x1 = x;
        m_placed.push_back(std::move(p));
    };
    place(root, m_path.size() == 1);
    for (size_t i = first; i < m_path.size(); ++i) place(m_path[i], i + 1 == m_path.size());
    if (m_hot >= static_cast<int>(m_placed.size())) m_hot = -1;
    m_blend.assign(m_placed.size(), 0.0f); // a new path starts unlit
    m_blendPart.assign(m_placed.size(), 0);
    if (m_hot >= 0) {
        m_blend[static_cast<size_t>(m_hot)] = 1.0f;
        m_blendPart[static_cast<size_t>(m_hot)] = m_hotPart;
    }
}

void Breadcrumb::Paint() {
    if (!m_visual) return;
    auto& host = gfx::CompositionHost::Get();
    const int w = std::max<int>(1, m_bounds.right - m_bounds.left), h = std::max<int>(1, m_bounds.bottom - m_bounds.top);
    const float s = m_scale;
    host.Repaint(m_visual, w, h, s, [&](ID2D1DeviceContext* dc) {
        const float H = h / s, top = std::round((H - kPlateH) / 2 * s) / s, bottom = top + kPlateH;
        com_ptr<ID2D1SolidColorBrush> ink, divider, arrowInk;
        dc->CreateSolidColorBrush(Hex(0x1E395B), ink.put());
        dc->CreateSolidColorBrush(Hex(0x8DB2E3, 0.8f), divider.put());
        dc->CreateSolidColorBrush(Hex(0x1E3250), arrowInk.put()); // WMP's dark navy, as a vector
        const bool art = win7::Resources::Get().Has(win7::Module::Wmploc, L"WMP_BREADCRUMB_MAINBUTTON.PNG");
        for (int i = 0; i < static_cast<int>(m_placed.size()); ++i) {
            const Placed& c = m_placed[static_cast<size_t>(i)];
            const size_t at = static_cast<size_t>(i);
            const bool open = i == m_open;
            const float blend = open ? 1.0f : m_blend[at];
            const bool lit = blend > 0.0f; // lit, fading in or out
            const int part = i == m_hot ? m_hotPart : m_blendPart[at];
            const bool faded = lit && blend < 1.0f;
            if (faded)
                dc->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                                     D2D1::IdentityMatrix(), blend),
                              nullptr);
            // The whole level lights: WMP 12's split button (WMP_BREADCRUMB_MAINBUTTON /
            // _SPLITBUTTON, states empty / hot / pressed / grey), the part under the pointer
            // hot and the other grey, the arrow pressed while its menu is open.
            if ((lit || open) && art) {
                const int nameState = open || part == 1 ? 3 : 1;
                const int arrowState = open ? 2 : part == 1 ? 1 : 3;
                if (!c.level.text.empty()) {
                    // MAINBUTTON is the left half of a split button with no right edge; a level
                    // without an arrow is closed with the split part's last 3 px.
                    const float px = 1.0f / s, edge = c.arrow ? 0 : 3 * px;
                    win7::DrawNineGrid(dc, win7::Module::Wmploc, L"WMP_BREADCRUMB_MAINBUTTON.PNG",
                                       {16.0f * nameState, 0, 16.0f * nameState + 16, 22}, {3, 3, 3, 3},
                                       {c.x0, top, c.split - edge, bottom});
                    if (!c.arrow)
                        win7::DrawNineGrid(dc, win7::Module::Wmploc, L"WMP_BREADCRUMB_SPLITBUTTON.PNG",
                                           {10.0f * nameState + 7, 0, 10.0f * nameState + 10, 22}, {0, 3, 0, 3},
                                           {c.split - edge, top, c.split, bottom});
                }
                if (c.arrow)
                    win7::DrawNineGrid(dc, win7::Module::Wmploc, L"WMP_BREADCRUMB_SPLITBUTTON.PNG",
                                       {10.0f * arrowState, 0, 10.0f * arrowState + 10, 22}, {3, 3, 3, 3},
                                       {c.split, top, c.x1, bottom});
            } else if (lit || open) {
                wp::RowPlate(dc, {c.x0, top, c.x1, bottom}, wp::RowState::Hot, true);
                if (open || part == 1) {
                    if (c.arrow) wp::RowPlate(dc, {c.split, top, c.x1, bottom}, wp::RowState::Selected, true);
                } else if (!c.level.text.empty()) {
                    wp::RowPlate(dc, {c.x0, top, c.split, bottom}, wp::RowState::SelectedHot, true);
                }
                if (!c.level.text.empty() && c.arrow)
                    dc->FillRectangle({c.split, top + 1, c.split + 1.0f / s, bottom - 1}, divider.get());
            }
            if (faded) dc->PopLayer();
            if (!c.level.text.empty()) {
                auto t = MakeText(c.level.literal ? c.level.text : Tr(c.level.text), 12.0f, DWRITE_FONT_WEIGHT_NORMAL,
                                  c.split - c.x0, H);
                t->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER); // centred in the bar, like WMP
                dc->DrawTextLayout({c.x0 + 4, 0}, t.get(), ink.get());
            }
            if (c.arrow) { // small triangle: right, or down while the menu is open
                const float cx = std::round((c.split + c.x1) / 2 * s) / s, cy = std::round(H / 2 * s) / s;
                com_ptr<ID2D1Factory> f;
                dc->GetFactory(f.put());
                com_ptr<ID2D1PathGeometry> tri;
                com_ptr<ID2D1GeometrySink> sink;
                if (FAILED(f->CreatePathGeometry(tri.put())) || FAILED(tri->Open(sink.put()))) continue;
                if (open) {
                    sink->BeginFigure({cx - 3.5f, cy - 1.5f}, D2D1_FIGURE_BEGIN_FILLED);
                    sink->AddLine({cx + 3.5f, cy - 1.5f});
                    sink->AddLine({cx, cy + 2.5f});
                } else {
                    sink->BeginFigure({cx - 1.5f, cy - 3.5f}, D2D1_FIGURE_BEGIN_FILLED);
                    sink->AddLine({cx + 2.5f, cy});
                    sink->AddLine({cx - 1.5f, cy + 3.5f});
                }
                sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                sink->Close();
                dc->FillGeometry(tri.get(), arrowInk.get());
            }
        }
    });
}

bool Breadcrumb::HitTest(POINT client, int* index, int* part) const {
    if (!PtInRect(&m_bounds, client)) return false;
    const float x = (client.x - m_bounds.left) / m_scale;
    for (int i = 0; i < static_cast<int>(m_placed.size()); ++i) {
        const Placed& c = m_placed[static_cast<size_t>(i)];
        if (x >= c.x0 && x < c.x1) {
            *index = i;
            *part = x >= c.split ? 1 : 0;
            return true;
        }
    }
    return false;
}

void Breadcrumb::OnMouseMove(POINT client) {
    int index = -1, part = 0;
    if (!HitTest(client, &index, &part)) index = -1;
    if (index == m_hot && (index < 0 || part == m_hotPart)) return;
    m_hot = index;
    m_hotPart = part;
    if (index >= 0) m_blendPart[static_cast<size_t>(index)] = part;
    Paint();
    if (m_frames) m_frames(); // the fade
}

bool Breadcrumb::Animate() {
    bool active = false, changed = false;
    for (size_t i = 0; i < m_blend.size(); ++i) {
        const float target = static_cast<int>(i) == m_hot || static_cast<int>(i) == m_open ? 1.0f : 0.0f;
        float& blend = m_blend[i];
        const float next = blend < target ? std::min(target, blend + kFadeIn) : std::max(target, blend - kFadeOut);
        if (next != blend) {
            blend = next;
            changed = true;
        }
        if (blend != target) active = true;
    }
    if (changed) Paint();
    return active;
}

void Breadcrumb::OnClick(HWND owner, POINT client) {
    int index = -1, part = 0;
    if (!HitTest(client, &index, &part)) return;
    const Placed c = m_placed[static_cast<size_t>(index)]; // going somewhere replaces the path
    if (part == 0 && c.level.open) {
        c.level.open();
        return;
    }
    if (!c.level.children) return;
    m_open = index;
    Paint();
    // Dropped from the chevron, never over the bar (it opens on the press).
    const LONG x = m_bounds.left + static_cast<LONG>(std::lround(c.split * m_scale));
    RECT anchor{x, m_bounds.top, x + 1, m_bounds.top + static_cast<LONG>(std::lround(kMenuDrop * m_scale))};
    MapWindowPoints(owner, nullptr, reinterpret_cast<POINT*>(&anchor), 2);
    const auto items = c.level.children();
    ShowActionMenu(owner, anchor, items); // runs the chosen action
    m_open = -1;
    m_hot = -1;
    Paint();
    if (m_frames) m_frames(); // the plates fade out
}

} // namespace glasswave::ui
