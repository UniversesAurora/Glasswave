#include "pch.h"
#include "ui/main_window.h"
#include "gfx/motion.h"
#include "gfx/d2d_util.h"

#include "gfx/composition_host.h"
#include "ui/remote_pages.h"
#include "ui/search_page.h"
#include "ui/text.h"

#include <unordered_set>

#include <winrt/Windows.Foundation.Collections.h>

// The search box's scopes, drop-down and suggestions, and the page tabs over the list.
namespace glasswave::ui {

using winrt::com_ptr;
using winrt::Windows::Foundation::Numerics::float2;
using winrt::Windows::Foundation::Numerics::float3;

namespace {

constexpr UINT_PTR kSuggestTimer = 7;  // shared with main_window.cpp's timer ids
constexpr UINT kSuggestDelayMs = 200;  // typing pauses this long before suggestions are asked for
constexpr size_t kSuggestTerms = 8, kSuggestItems = 4, kLocalTerms = 4, kHistoryRows = 10;
// The page tabs (owner, 2026-11-15: Media Center's large text tabs): new UI values.
constexpr float kTabBand = 44, kTabLeft = 20, kTabGap = 28, kTabText = 20, kTabBar = 3;
// The bar's slide: Windows 8.1's fast-out curve (cubic-bezier 0.1, 0.9, 0.2, 1).
constexpr int kTabSlideMs = 500;

D2D1_COLOR_F Hex(uint32_t rgb, float a = 1.0f) {
    return D2D1::ColorF(rgb, a);
}

std::wstring Trimmed(std::wstring text) {
    const size_t first = text.find_first_not_of(L" \t\r\n");
    if (first == std::wstring::npos) return {};
    const size_t last = text.find_last_not_of(L" \t\r\n");
    return text.substr(first, last - first + 1);
}

// Names containing `needle`, those starting with it first, each once.
void AddMatches(std::vector<std::wstring>& out, std::vector<std::wstring>& later,
                std::unordered_set<std::wstring>& seen, const std::wstring& name, const std::wstring& needle) {
    if (name.empty()) return;
    const std::wstring lower = media::Lower(name);
    const size_t at = lower.find(needle);
    if (at == std::wstring::npos || !seen.insert(lower).second) return;
    (at == 0 ? out : later).push_back(name);
}

} // namespace

// ---- scope and text --------------------------------------------------------------------------

void MainWindow::BeginSearch(const SearchScope& scope) {
    SetSearchScope(scope);
    SetSearchFocus(true);
}

void MainWindow::SetSearchScope(const SearchScope& scope) {
    const bool wasList = !m_searchScope.Catalog();
    m_searchScope = scope;
    m_search->SetToken(scope.Catalog() ? ScopeLabel(scope) + L":" : std::wstring());
    // The list filter belongs to the list scope: a catalog scope leaves the list whole, and the
    // list scope filters again with what the box holds.
    const std::wstring filter = scope.Catalog() ? std::wstring() : media::Lower(m_search->Text());
    if ((wasList || !scope.Catalog()) && filter != m_filter) {
        m_filter = filter;
        if (m_source) {
            m_source->SetFilter(m_filter);
            m_list->SetSource(m_source.get(), true);
            if (m_page) m_page->OnPresentationChanged();
        }
    }
    OnSearchTextChanged(m_search->Text());
}

void MainWindow::SyncSearchBox(const View& view) {
    if (view.kind == View::Kind::Search) {
        m_searchScope = SearchViewScope(view);
        m_search->SetToken(ScopeLabel(m_searchScope) + L":");
        m_search->SetText(view.remoteId);
        m_filter.clear();
    } else if (m_searchScope.Catalog()) {
        m_searchScope = {};
        m_search->SetToken({});
        m_search->SetText({});
    }
    HideSearchPopup();
}

void MainWindow::OnSearchTextChanged(const std::wstring& text) {
    if (!m_searchScope.Catalog() && media::Lower(text) != m_filter) { // the composition only suggests
        m_filter = media::Lower(text);
        if (m_source) {
            m_source->SetFilter(m_filter);
            m_list->SetSource(m_source.get(), true);
            if (m_page) m_page->OnPresentationChanged();
        }
    }
    if (m_suggestCancel) *m_suggestCancel = true;
    m_suggestCancel.reset();
    m_suggestAnswers.clear();
    m_suggestPending = 0;
    m_suggestions.clear();
    KillTimer(m_hwnd, kSuggestTimer);
    (void)text;
    const std::wstring typed = m_search->Typed(); // with the IME's uncommitted text
    if (Trimmed(typed).empty()) {
        if (m_search->Focused()) ShowSearchPopup();
        return;
    }
    if (!m_searchScope.Catalog()) {
        ListSuggestions();
        if (m_search->Focused()) ShowSearchPopup();
        return;
    }
    if (m_search->Focused()) ShowSearchPopup();
    SetTimer(m_hwnd, kSuggestTimer, kSuggestDelayMs, nullptr);
}

void MainWindow::SubmitSearch(std::wstring term) {
    term = Trimmed(std::move(term));
    if (term.empty()) return;
    RememberSearch(term);
    HideSearchPopup();
    if (!m_searchScope.Catalog()) return; // the list is filtered already
    // A search from a search page keeps its tab.
    const std::wstring tab = m_view.kind == View::Kind::Search ? m_view.sectionId : std::wstring();
    SetSearchFocus(false);
    Navigate(SearchView(m_searchScope, std::move(term), tab), true);
}

void MainWindow::Search(const std::wstring& term) {
    if (Trimmed(term).empty()) return;
    SetSearchScope({SearchScope::Kind::All, {}});
    m_search->SetText(term);
    SubmitSearch(term);
}

void MainWindow::OnSearchClear() {
    if (!m_search->Text().empty()) {
        m_search->Clear();
    } else if (m_searchScope.Catalog()) {
        SetSearchScope({});
    }
    SetSearchFocus(true);
}

bool MainWindow::OnSearchKey(WPARAM key) {
    const bool open = m_searchPopup && m_searchPopup->Visible();
    switch (key) {
    case VK_DOWN:
    case VK_UP:
        if (!open) {
            if (!m_searchScope.Catalog() && key == VK_DOWN && m_search->Text().empty()) return false; // into the list
            ShowSearchPopup();
            return true;
        }
        if (!m_suggestions.empty()) {
            m_searchPopup->MoveHot(key == VK_DOWN ? 1 : -1);
            return true;
        }
        return m_searchScope.Catalog();
    case VK_RETURN:
        if (open && m_searchPopup->HotRow() >= 0) {
            PickSuggestion(static_cast<size_t>(m_searchPopup->HotRow()));
            return true;
        }
        if (m_searchScope.Catalog()) {
            SubmitSearch(m_search->Text());
            return true;
        }
        SubmitSearch(m_search->Text()); // remembered; the list keeps its filter
        return false;
    case VK_ESCAPE:
        if (open) {
            HideSearchPopup();
            return true;
        }
        return false;
    default: return false;
    }
}

// ---- the drop-down -----------------------------------------------------------------------------

void MainWindow::ShowSearchPopup() {
    if (!m_searchPopup) {
        m_searchPopup = std::make_unique<SearchPopup>(m_hwnd);
        m_searchPopup->onRow = [this](size_t row) { PickSuggestion(row); };
        m_searchPopup->onLink = [this](size_t link) {
            const auto scopes = SearchScopes();
            if (link < scopes.size()) BeginSearch(scopes[link]);
        };
        m_searchPopup->onClear = [this] {
            ClearSearchHistory();
            ShowSearchPopup();
        };
        m_searchPopup->onDelete = [this](size_t row) {
            if (row >= m_suggestions.size() || !m_suggestions[row].history) return;
            ForgetSearch(m_suggestions[row].text);
            ShowSearchPopup();
        };
    }
    SearchPopup::Model model;
    const bool empty = Trimmed(m_search->Typed()).empty();
    if (empty) {
        m_suggestions.clear();
        const auto history = SearchHistory();
        for (const auto& term : history) {
            if (m_suggestions.size() >= kHistoryRows) break;
            m_suggestions.push_back({term, {}, true, [this, term] {
                                         m_search->SetText(term);
                                         OnSearchTextChanged(term);
                                         SubmitSearch(term);
                                     }});
        }
        if (!history.empty()) model.clearLink = Tr(L"Clear search history");
    } else if (m_suggestions.empty() && m_suggestPending) {
        model.note = Tr(L"Searching...");
    }
    for (const auto& s : m_suggestions) model.rows.push_back({s.text, s.detail, s.history});
    model.footer = Tr(L"Search in:");
    for (const auto& scope : SearchScopes()) model.links.push_back({ScopeLabel(scope), scope == m_searchScope});
    RECT anchor = m_search->Bounds();
    MapWindowPoints(m_hwnd, nullptr, reinterpret_cast<POINT*>(&anchor), 2);
    m_searchPopup->Show(anchor, std::move(model));
}

void MainWindow::HideSearchPopup() {
    if (m_searchPopup) m_searchPopup->Hide();
}

void MainWindow::PickSuggestion(size_t row) {
    if (row >= m_suggestions.size()) return;
    const auto pick = m_suggestions[row].pick; // it may rebuild the rows
    if (pick) pick();
}

// The list on screen suggests its own titles, artists and albums.
void MainWindow::ListSuggestions() {
    m_suggestions.clear();
    if (!m_source) return;
    const std::wstring needle = media::Lower(Trimmed(m_search->Typed()));
    std::vector<size_t> columns;
    const auto& cols = m_source->Columns();
    for (size_t c = 0; c < cols.size(); ++c)
        if (cols[c].title == L"Title" || cols[c].title == L"Artist" || cols[c].title == L"Album" ||
            cols[c].title == L"Name")
            columns.push_back(c);
    std::vector<std::wstring> first, later;
    std::unordered_set<std::wstring> seen;
    // The source is filtered by the same text: its rows are the matches.
    const size_t rows = std::min<size_t>(m_source->Count(), 5000);
    for (size_t r = 0; r < rows && first.size() < kSuggestTerms; ++r)
        for (size_t c : columns) AddMatches(first, later, seen, m_source->Cell(r, c), needle);
    first.insert(first.end(), later.begin(), later.end());
    if (first.size() > kSuggestTerms) first.resize(kSuggestTerms);
    for (const auto& term : first)
        m_suggestions.push_back({term, {}, false, [this, term] {
                                     m_search->SetText(term);
                                     OnSearchTextChanged(term);
                                     HideSearchPopup();
                                 }});
}

void MainWindow::FetchSuggestions() {
    const std::wstring term = Trimmed(m_search->Typed());
    if (term.empty() || !m_searchScope.Catalog()) return;
    m_suggestCancel = stream::MakeCancelToken();
    m_suggestAnswers.clear();
    m_suggestPending = 0;
    const bool local = m_searchScope.kind == SearchScope::Kind::All || m_searchScope.kind == SearchScope::Kind::Local;
    if (local) {
        // The library's own names: artists, albums, then titles.
        stream::Provider::CatalogSearchSuggestions answer;
        const std::wstring needle = media::Lower(term);
        std::vector<std::wstring> first, later;
        std::unordered_set<std::wstring> seen;
        for (const auto kind : {media::GroupKind::Artist, media::GroupKind::Album})
            for (const auto& group : m_library.Groups(kind)) AddMatches(first, later, seen, group.name, needle);
        for (const auto& track : m_library.Tracks()) {
            if (first.size() >= kLocalTerms) break;
            AddMatches(first, later, seen, track.title, needle);
        }
        first.insert(first.end(), later.begin(), later.end());
        for (size_t i = 0; i < first.size() && i < kLocalTerms; ++i)
            answer.ordered.emplace_back(stream::Provider::CatalogSearchSuggestionTerm{first[i], first[i]});
        m_suggestAnswers.push_back({L"local", std::move(answer)});
    }
    std::vector<stream::Provider*> providers;
    if (m_searchScope.kind == SearchScope::Kind::All) {
        for (auto* provider : stream::Providers())
            if (provider->Can(stream::Capability::SearchSuggestions)) providers.push_back(provider);
    } else if (auto* provider = ScopeProvider(m_searchScope);
               provider && provider->Can(stream::Capability::SearchSuggestions)) {
        providers.push_back(provider);
    }
    for (auto* provider : providers) {
        ++m_suggestPending;
        const auto cancel = m_suggestCancel;
        provider->FetchCatalogSearchSuggestions(
            {term, cancel},
            [this, alive = std::weak_ptr<int>(m_alive), cancel, key = std::wstring(provider->Key())](
                stream::Provider::CatalogSearchSuggestions answer, std::wstring) {
                if (alive.expired() || *cancel || cancel != m_suggestCancel) return;
                if (m_suggestPending) --m_suggestPending;
                m_suggestAnswers.push_back({key, std::move(answer)});
                BuildSuggestions();
                if (m_search->Focused()) ShowSearchPopup();
            });
    }
    BuildSuggestions();
    if (m_search->Focused()) ShowSearchPopup();
}

// The answers so far as rows: the terms of every source by rank (the library's first), then the
// sources' top items by rank, each marked with its kind and, in the mixed scope, its source.
void MainWindow::BuildSuggestions() {
    m_suggestions.clear();
    // Sources in scope order: local, then the providers as registered.
    std::vector<const std::pair<std::wstring, stream::Provider::CatalogSearchSuggestions>*> answers;
    for (const auto& answer : m_suggestAnswers)
        if (answer.first == L"local") answers.push_back(&answer);
    for (auto* provider : stream::Providers())
        for (const auto& answer : m_suggestAnswers)
            if (answer.first == provider->Key()) answers.push_back(&answer);
    using Term = stream::Provider::CatalogSearchSuggestionTerm;
    std::unordered_set<std::wstring> seen;
    std::vector<std::vector<const Term*>> terms(answers.size());
    std::vector<std::vector<const stream::CatalogSearchItem*>> items(answers.size());
    for (size_t a = 0; a < answers.size(); ++a)
        for (const auto& entry : answers[a]->second.ordered) {
            if (const auto* term = std::get_if<Term>(&entry))
                terms[a].push_back(term);
            else
                items[a].push_back(&std::get<stream::CatalogSearchItem>(entry));
        }
    const auto submit = [this](const std::wstring& term) {
        return [this, term] {
            m_search->SetText(term);
            SubmitSearch(term);
        };
    };
    for (size_t rank = 0; m_suggestions.size() < kSuggestTerms; ++rank) {
        bool any = false;
        for (size_t a = 0; a < answers.size() && m_suggestions.size() < kSuggestTerms; ++a) {
            if (rank >= terms[a].size()) continue;
            any = true;
            const Term& term = *terms[a][rank];
            if (!seen.insert(media::Lower(term.searchTerm)).second) continue;
            m_suggestions.push_back({term.displayTerm, {}, false, submit(term.searchTerm)});
        }
        if (!any) break;
    }
    const bool mixed = m_searchScope.kind == SearchScope::Kind::All;
    size_t added = 0;
    for (size_t rank = 0; added < kSuggestItems; ++rank) {
        bool any = false;
        for (size_t a = 0; a < answers.size() && added < kSuggestItems; ++a) {
            if (rank >= items[a].size()) continue;
            any = true;
            auto* provider = stream::FindProvider(answers[a]->first);
            if (!provider) continue;
            const stream::CatalogSearchItem item = *items[a][rank];
            std::wstring text, kind;
            std::function<void()> open;
            std::visit(
                [&](const auto& v) {
                    using T = std::decay_t<decltype(v)>;
                    if constexpr (std::is_same_v<T, stream::Track>) {
                        const std::wstring artists = stream::JoinArtists(v.artists);
                        text = artists.empty() ? v.title : v.title + L" — " + artists;
                        kind = Tr(L"Song");
                        if (provider->Can(stream::Capability::SongDetails))
                            open = [this, provider, v] { Navigate(SongView(*provider, v.id, v.title), true); };
                    } else if constexpr (std::is_same_v<T, stream::Artist>) {
                        text = v.name;
                        kind = Tr(L"Artist");
                        open = [this, provider, v] { Navigate(ArtistView(*provider, v.id, v.name), true); };
                    } else if constexpr (std::is_same_v<T, stream::Album>) {
                        const std::wstring artists = stream::JoinArtists(v.artists);
                        text = artists.empty() ? v.name : v.name + L" — " + artists;
                        kind = Tr(L"Album");
                        open = [this, provider, v] { Navigate(AlbumView(*provider, v.id, v.name), true); };
                    } else {
                        text = v.name;
                        kind = provider->Text(L"Playlist");
                        open = [this, provider, v] {
                            Navigate(PlaylistView(*provider, v.id, v.name, std::nullopt), true);
                        };
                    }
                },
                item);
            if (!open) continue;
            if (mixed) kind += L" · " + Tr(provider->Name());
            m_suggestions.push_back({std::move(text), std::move(kind), false, [this, open] {
                                         HideSearchPopup();
                                         SetSearchFocus(false);
                                         open();
                                     }});
            ++added;
        }
        if (!any) break;
    }
}

// ---- page tabs -----------------------------------------------------------------------------------

void MainWindow::LayoutTabs() {
    const auto tabs = m_page ? m_page->Tabs() : std::vector<PageTab>{};
    const std::wstring current = m_page ? m_page->Tab() : std::wstring();
    const bool changed = tabs.size() != m_tabs.size() || current != m_tabCurrent ||
                         !std::equal(tabs.begin(), tabs.end(), m_tabs.begin(), [](const PageTab& a, const PageTab& b) {
                             return a.key == b.key && a.label == b.label;
                         });
    if (changed) {
        // Another tab of the same page: its bar slides over from the one it left.
        const bool sameTabs = tabs.size() == m_tabs.size() &&
                              std::equal(tabs.begin(), tabs.end(), m_tabs.begin(),
                                         [](const PageTab& a, const PageTab& b) { return a.key == b.key; });
        m_tabSlide = sameTabs && current != m_tabCurrent && m_tabBarLeft >= 0;
        if (!sameTabs) m_tabBarLeft = m_tabBarRight = -1;
        m_tabs = tabs;
        m_tabCurrent = current;
        m_tabBlend.assign(m_tabs.size(), 0.0f);
        m_tabHot = -1;
    }
    if (!m_tabStrip) {
        const auto& comp = gfx::CompositionHost::Get().Compositor();
        m_tabStrip = comp.CreateSpriteVisual();
        m_contentLayer.Children().InsertAtTop(m_tabStrip);
        // The bar: a vertical gradient in its own visual over the strip.
        auto gradient = comp.CreateLinearGradientBrush();
        gradient.StartPoint({0, 0});
        gradient.EndPoint({0, 1});
        gradient.ColorStops().Append(comp.CreateColorGradientStop(0, {255, 0x6B, 0xB7, 0xF5}));
        gradient.ColorStops().Append(comp.CreateColorGradientStop(1, {255, 0x2F, 0x7F, 0xD0}));
        m_tabBar = comp.CreateSpriteVisual();
        m_tabBar.Brush(gradient);
        m_tabStrip.Children().InsertAtTop(m_tabBar);
    }
    const RECT& r = m_layout.tabs;
    const bool show = !m_tabs.empty() && r.right > r.left;
    m_tabStrip.IsVisible(show);
    if (!show) return;
    const SIZE size{r.right - r.left, r.bottom - r.top};
    m_tabStrip.Offset(float3{static_cast<float>(r.left), static_cast<float>(r.top), 0});
    if (changed || size.cx != m_tabStripSize.cx || size.cy != m_tabStripSize.cy) PaintTabs();
}

// Media Center's tabs on the library's light ground: large light text, the chosen tab dark with
// a thick blue bar on the band's bottom rule under it, the others dimmed until the mouse
// brightens them. The bar slides from tab to tab.
void MainWindow::PaintTabs() {
    const RECT& r = m_layout.tabs;
    const int w = std::max<int>(r.right - r.left, 1), h = std::max<int>(r.bottom - r.top, 1);
    m_tabStripSize = {w, h};
    m_tabX.clear();
    gfx::CompositionHost::Get().Repaint(m_tabStrip, w, h, m_scale, [&](ID2D1DeviceContext* dc) {
        const float H = h / m_scale, W = w / m_scale;
        com_ptr<ID2D1SolidColorBrush> rule;
        dc->CreateSolidColorBrush(Hex(0xD5DFEB), rule.put());
        const float line = gfx::Hairline(m_scale);
        dc->FillRectangle({0, H - line, W, H}, rule.get());
        float x = kTabLeft, barLeft = -1, barRight = -1;
        for (size_t i = 0; i < m_tabs.size(); ++i) {
            const bool current = m_tabs[i].key == m_tabCurrent;
            auto layout = MakeText(m_tabs[i].label, kTabText, DWRITE_FONT_WEIGHT_LIGHT, 600, H, DWRITE_TEXT_ALIGNMENT_LEADING,
                                   L"Segoe UI");
            DWRITE_TEXT_METRICS tm{};
            layout->GetMetrics(&tm);
            const float blend = i < m_tabBlend.size() ? m_tabBlend[i] : 0.0f;
            const float alpha = current ? 1.0f : 0.45f + 0.4f * blend;
            com_ptr<ID2D1SolidColorBrush> ink;
            dc->CreateSolidColorBrush(Hex(0x1E395B, alpha), ink.put());
            const float top = std::round((H - kTabBar - tm.height) / 2 * m_scale) / m_scale;
            dc->DrawTextLayout({x, top}, layout.get(), ink.get());
            if (current) {
                barLeft = x;
                barRight = x + tm.width;
            }
            m_tabX.push_back(x);
            x += tm.width + kTabGap;
        }
        m_tabX.push_back(x - kTabGap);
        if (barLeft >= 0) PlaceTabBar(barLeft, barRight);
    });
}

// The bar under the current tab: placed at once, or (another tab of the same page) slid there on
// the compositor, which keeps the motion smooth whatever the UI thread does meanwhile.
void MainWindow::PlaceTabBar(float left, float right) {
    if (!m_tabBar) return;
    const float H = (m_layout.tabs.bottom - m_layout.tabs.top) / m_scale;
    const float barH = std::max(1.0f, std::round(kTabBar * m_scale));
    const float3 offset{std::round(left * m_scale), std::round(H * m_scale) - barH, 0};
    const float2 size{std::round((right - left) * m_scale), barH};
    const bool moved = left != m_tabBarLeft || right != m_tabBarRight;
    if (!moved && !m_tabSlide && m_tabBar.Size().x > 0) return; // a hover repaint: leave a slide running
    const bool slide = m_tabSlide && m_tabBarLeft >= 0 && moved;
    m_tabSlide = false;
    m_tabBarLeft = left;
    m_tabBarRight = right;
    if (!slide) {
        m_tabBar.StopAnimation(L"Offset");
        m_tabBar.StopAnimation(L"Size");
        m_tabBar.Offset(offset);
        m_tabBar.Size(size);
        return;
    }
    const auto& comp = gfx::CompositionHost::Get().Compositor();
    const auto ease = gfx::Easing(gfx::curve::kFastOut);
    auto move = comp.CreateVector3KeyFrameAnimation();
    move.InsertExpressionKeyFrame(0.0f, L"this.StartingValue");
    move.InsertKeyFrame(1.0f, offset, ease);
    move.Duration(std::chrono::milliseconds(kTabSlideMs));
    auto grow = comp.CreateVector2KeyFrameAnimation();
    grow.InsertExpressionKeyFrame(0.0f, L"this.StartingValue");
    grow.InsertKeyFrame(1.0f, size, ease);
    grow.Duration(std::chrono::milliseconds(kTabSlideMs));
    m_tabBar.StartAnimation(L"Offset", move);
    m_tabBar.StartAnimation(L"Size", grow);
}

int MainWindow::TabAt(POINT pt) const {
    if (!PtInRect(&m_layout.tabs, pt) || m_tabX.size() < 2) return -1;
    const float x = (pt.x - m_layout.tabs.left) / m_scale;
    for (size_t i = 0; i + 1 < m_tabX.size(); ++i) {
        const float right = i + 2 < m_tabX.size() ? m_tabX[i + 1] - kTabGap / 2 : m_tabX[i + 1] + kTabGap / 2;
        if (x >= m_tabX[i] - kTabGap / 2 && x < right) return static_cast<int>(i);
    }
    return -1;
}

void MainWindow::UpdateTabHover(POINT pt) {
    const int hot = TabAt(pt);
    if (hot == m_tabHot) return;
    m_tabHot = hot;
    StartScrollAnimation(); // the tabs brighten and dim on the same timer as the breadcrumb
}

void MainWindow::OnTabClick(POINT pt) {
    const int index = TabAt(pt);
    if (index < 0 || !m_page || static_cast<size_t>(index) >= m_tabs.size()) return;
    const std::wstring key = m_tabs[static_cast<size_t>(index)].key;
    m_page->SelectTab(key); // navigates
}

bool MainWindow::AnimateTabs() {
    bool active = false, changed = false;
    for (size_t i = 0; i < m_tabBlend.size(); ++i) {
        const float target = static_cast<int>(i) == m_tabHot ? 1.0f : 0.0f;
        float& blend = m_tabBlend[i];
        const float next = blend < target ? std::min(target, blend + 0.25f) : std::max(target, blend - 0.1f);
        if (next != blend) {
            blend = next;
            changed = true;
        }
        if (blend != target) active = true;
    }
    if (changed && m_tabStrip && m_tabStrip.IsVisible()) PaintTabs();
    return active;
}

} // namespace glasswave::ui
