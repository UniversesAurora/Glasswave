#include "pch.h"
#include "ui/search_box.h"
#include "gfx/composition_host.h"
#include "ui/text.h"
#include "ui/i18n.h"
#include "wmp/wmp_painter.h"

namespace glasswave::ui {

using winrt::Windows::Foundation::Numerics::float2;
using winrt::Windows::Foundation::Numerics::float3;

namespace {
constexpr float kTextLeft = 6.0f;
constexpr float kIconRoom = 25.0f; // the clear button plate (WMP 12 WW_CLEAR)
}  // namespace

SearchBox::SearchBox(const winrt::Windows::UI::Composition::ContainerVisual& parent) {
    const auto& comp = gfx::CompositionHost::Get().Compositor();
    m_visual = comp.CreateSpriteVisual();
    m_caret = comp.CreateSpriteVisual();
    m_caret.Brush(comp.CreateColorBrush(winrt::Windows::UI::Color{255, 0, 0, 0}));
    m_caret.IsVisible(false);
    parent.Children().InsertAtTop(m_visual);
    parent.Children().InsertAtTop(m_caret);

    // Blinking caret as a compositor animation: no timers, no CPU while idle.
    auto blink = comp.CreateScalarKeyFrameAnimation();
    blink.InsertKeyFrame(0.0f, 1.0f);
    blink.InsertKeyFrame(0.5f, 1.0f, comp.CreateStepEasingFunction());
    blink.InsertKeyFrame(0.51f, 0.0f, comp.CreateStepEasingFunction());
    blink.InsertKeyFrame(1.0f, 0.0f);
    blink.Duration(std::chrono::milliseconds(1060));
    blink.IterationBehavior(winrt::Windows::UI::Composition::AnimationIterationBehavior::Forever);
    m_caret.StartAnimation(L"Opacity", blink);
}

void SearchBox::SetBounds(const RECT& px, float scale) {
    m_bounds = px;
    m_scale = scale;
    m_visual.Offset(float3{static_cast<float>(px.left), static_cast<float>(px.top), 0});
    Paint();  // the caret is positioned in window coordinates
}

void SearchBox::SetFocused(bool focused) {
    if (m_focused == focused) return;
    m_focused = focused;
    if (!focused) m_composition.clear();
    Paint();
}

SearchBox::Shown SearchBox::BuildLayout(float height) const {
    Shown out;
    // The IME's composition is drawn in place at the caret, underlined.
    const std::wstring typed = m_text.substr(0, m_cursor) + m_composition + m_text.substr(m_cursor);
    out.placeholder = typed.empty() && m_token.empty();
    const std::wstring& placeholderText = Tr(L"Search");
    // The scope token, then the text: one layout so the caret and the scrolling see both.
    const std::wstring prefix = m_token.empty() ? std::wstring() : m_token + L" ";
    out.prefix = prefix.size();
    const std::wstring shown = out.placeholder ? placeholderText : prefix + typed;
    out.layout = MakeText(shown, 12.0f, DWRITE_FONT_WEIGHT_NORMAL, 4096, height);
    if (out.placeholder)
        out.layout->SetFontStyle(DWRITE_FONT_STYLE_ITALIC, {0, static_cast<UINT32>(placeholderText.size())});
    if (!m_composition.empty())
        out.layout->SetUnderline(TRUE, {static_cast<UINT32>(prefix.size() + m_cursor),
                                        static_cast<UINT32>(m_composition.size())});
    return out;
}

void SearchBox::Paint() {
    auto& host = gfx::CompositionHost::Get();
    const int w = std::max<int>(m_bounds.right - m_bounds.left, 1), h = std::max<int>(m_bounds.bottom - m_bounds.top, 1);
    const float s = m_scale;
    host.Repaint(m_visual, w, h, s, [&](ID2D1DeviceContext* dc) {
        const D2D1_SIZE_F size{w / s, h / s};
        const bool clear = !m_text.empty() || !m_token.empty();
        wmp::paint::SearchFrame(dc, size, m_focused, clear, clear ? m_clearHotBlend : 0.0f,
                                clear ? m_clearPressBlend : 0.0f);
        Shown shown = BuildLayout(size.height);
        auto* layout = shown.layout.get();
        winrt::com_ptr<ID2D1SolidColorBrush> ink, blue, white, select;
        dc->CreateSolidColorBrush(shown.placeholder ? D2D1::ColorF(0x8D8D8D) : D2D1::ColorF(0x1E1E1E), ink.put());
        dc->CreateSolidColorBrush(D2D1::ColorF(0x0070C0), blue.put());  // Explorer's filter blue (measured)
        dc->CreateSolidColorBrush(D2D1::ColorF(0xFFFFFF), white.put());
        // The selection in the system's highlight (Windows 7: #3399FF, white text), only while
        // the box has the focus, as an edit box shows it.
        const COLORREF hl = GetSysColor(COLOR_HIGHLIGHT);
        dc->CreateSolidColorBrush(D2D1::ColorF(GetRValue(hl) / 255.0f, GetGValue(hl) / 255.0f, GetBValue(hl) / 255.0f),
                                  select.put());
        if (!m_token.empty() && !shown.placeholder)
            layout->SetDrawingEffect(blue.get(), {0, static_cast<UINT32>(m_token.size())});
        const bool selecting = m_focused && HasSelection() && m_composition.empty();
        if (selecting) {
            const auto [from, to] = Selection();
            layout->SetDrawingEffect(white.get(), {static_cast<UINT32>(shown.prefix + from), static_cast<UINT32>(to - from)});
        }
        // The caret, and the scroll that keeps it in view.
        float caret = 0;
        if (!shown.placeholder) {
            const auto index = static_cast<UINT32>(shown.prefix + m_cursor + std::min(m_compositionCursor, m_composition.size()));
            float x = 0, y = 0;
            DWRITE_HIT_TEST_METRICS hit{};
            if (SUCCEEDED(layout->HitTestTextPosition(index, FALSE, &x, &y, &hit))) caret = x;
        }
        DWRITE_TEXT_METRICS tm{};
        layout->GetMetrics(&tm);
        const float room = size.width - kTextLeft - kIconRoom;
        if (shown.placeholder || tm.widthIncludingTrailingWhitespace <= room) {
            m_scroll = 0;
        } else {
            if (caret - m_scroll > room - 1) m_scroll = caret - room + 1;
            if (caret - m_scroll < 0) m_scroll = std::max(0.0f, caret - room / 3);
            m_scroll = std::clamp(m_scroll, 0.0f, tm.widthIncludingTrailingWhitespace - room + 1);
        }
        dc->PushAxisAlignedClip({kTextLeft, 0, kTextLeft + room, size.height}, D2D1_ANTIALIAS_MODE_ALIASED);
        if (selecting) {
            const auto [from, to] = Selection();
            UINT32 count = 0;
            layout->HitTestTextRange(static_cast<UINT32>(shown.prefix + from), static_cast<UINT32>(to - from), 0, 0,
                                     nullptr, 0, &count);
            std::vector<DWRITE_HIT_TEST_METRICS> ranges(count);
            if (count && SUCCEEDED(layout->HitTestTextRange(static_cast<UINT32>(shown.prefix + from),
                                                            static_cast<UINT32>(to - from), 0, 0, ranges.data(),
                                                            count, &count)))
                for (const auto& r : ranges) {
                    const float top = (size.height - 15) / 2;
                    dc->FillRectangle({kTextLeft - m_scroll + r.left, top, kTextLeft - m_scroll + r.left + r.width, top + 15},
                                      select.get());
                }
        }
        dc->DrawTextLayout({kTextLeft - m_scroll, 0}, layout, ink.get());
        dc->PopAxisAlignedClip();
        m_caretX = kTextLeft + caret - m_scroll;
    });
    const float caretH = std::round(14.0f * s);
    m_caret.Size(float2{std::max(1.0f, std::floor(s)), caretH});
    m_caret.Offset(float3{std::round(m_bounds.left + m_caretX * s), std::round(m_bounds.top + (h - caretH) / 2), 0});
    m_caret.IsVisible(m_focused);
}

POINT SearchBox::CaretClientPoint() const {
    return {static_cast<LONG>(m_bounds.left + m_caretX * m_scale), m_bounds.top};
}

void SearchBox::SetComposition(std::wstring text, size_t cursor) {
    if (text == m_composition && cursor == m_compositionCursor) return;
    const bool changed = text != m_composition;
    m_composition = std::move(text);
    m_compositionCursor = cursor;
    Paint();  // the filter follows committed text only; suggestions follow the composition too
    if (changed && onCompose) onCompose();
}

void SearchBox::InsertText(std::wstring_view text) {
    m_composition.clear();
    m_compositionCursor = 0;
    Replace(text);
}

void SearchBox::Changed() {
    Paint();
    if (onChange) onChange(m_text);
}

void SearchBox::Clear() {
    if (m_text.empty()) return;
    m_text.clear();
    m_cursor = m_anchor = 0;
    Changed();
}

void SearchBox::SetText(std::wstring text) {
    if (text == m_text) return;
    m_text = std::move(text);
    m_cursor = m_anchor = m_text.size();
    Paint();
}

void SearchBox::SetToken(std::wstring token) {
    if (token == m_token) return;
    m_token = std::move(token);
    Paint();
}

void SearchBox::DeleteSelection() {
    const auto [from, to] = Selection();
    m_text.erase(from, to - from);
    m_cursor = m_anchor = from;
}

void SearchBox::Replace(std::wstring_view text) {
    DeleteSelection();
    std::wstring clean;
    for (const wchar_t c : text)
        if (c >= 0x20) clean.push_back(c);
    m_text.insert(m_cursor, clean);
    m_cursor = m_anchor = m_cursor + clean.size();
    Changed();
}

void SearchBox::MoveTo(size_t index, bool extend) {
    index = std::min(index, m_text.size());
    // Never between the halves of a surrogate pair.
    if (index > 0 && index < m_text.size() && IS_LOW_SURROGATE(m_text[index]) && IS_HIGH_SURROGATE(m_text[index - 1]))
        index += index > m_cursor ? 1 : -1;
    m_cursor = index;
    if (!extend) m_anchor = index;
    Paint();
}

size_t SearchBox::WordLeft(size_t from) const {
    size_t i = from;
    while (i > 0 && iswspace(m_text[i - 1])) --i;
    while (i > 0 && !iswspace(m_text[i - 1])) --i;
    return i;
}

size_t SearchBox::WordRight(size_t from) const {
    size_t i = from;
    while (i < m_text.size() && !iswspace(m_text[i])) ++i;
    while (i < m_text.size() && iswspace(m_text[i])) ++i;
    return i;
}

void SearchBox::Copy(HWND owner) const {
    if (!HasSelection() || !OpenClipboard(owner)) return;
    const auto [from, to] = Selection();
    const std::wstring text = m_text.substr(from, to - from);
    EmptyClipboard();
    const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    if (HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
        if (void* p = GlobalLock(memory)) {
            memcpy(p, text.c_str(), bytes);
            GlobalUnlock(memory);
            if (!SetClipboardData(CF_UNICODETEXT, memory)) GlobalFree(memory);
        } else {
            GlobalFree(memory);
        }
    }
    CloseClipboard();
}

size_t SearchBox::IndexAt(POINT pt) const {
    const float height = (m_bounds.bottom - m_bounds.top) / m_scale;
    Shown shown = BuildLayout(height);
    if (shown.placeholder) return 0;
    const float x = (pt.x - m_bounds.left) / m_scale - kTextLeft + m_scroll;
    BOOL trailing = FALSE, inside = FALSE;
    DWRITE_HIT_TEST_METRICS hit{};
    if (FAILED(shown.layout->HitTestPoint(x, height / 2, &trailing, &inside, &hit))) return m_text.size();
    const size_t at = hit.textPosition + (trailing ? hit.length : 0);
    return at < shown.prefix ? 0 : std::min(at - shown.prefix, m_text.size());
}

void SearchBox::OnMouseDown(POINT pt, bool shift) {
    MoveTo(IndexAt(pt), shift);
}

void SearchBox::OnMouseDrag(POINT pt) {
    MoveTo(IndexAt(pt), true);
}

void SearchBox::OnDoubleClick(POINT pt) {
    const size_t at = IndexAt(pt);
    m_anchor = at > 0 && at <= m_text.size() && !iswspace(m_text[at - 1]) && (at == m_text.size() || iswspace(m_text[at]))
                   ? WordLeft(at)
                   : WordLeft(std::min(at + 1, m_text.size()));
    size_t end = m_anchor;
    while (end < m_text.size() && !iswspace(m_text[end])) ++end;
    m_cursor = end;
    Paint();
}

bool SearchBox::HitClear(POINT pt) const {
    if (m_text.empty() && m_token.empty()) return false;
    const LONG left = m_bounds.right - static_cast<LONG>(std::lround(kIconRoom * m_scale));
    return pt.x >= left && pt.x < m_bounds.right && pt.y >= m_bounds.top && pt.y < m_bounds.bottom;
}

void SearchBox::SetClearHot(bool hot) { m_clearHot = hot; }

void SearchBox::SetClearPressed(bool pressed) { m_clearPressed = pressed; }

bool SearchBox::Animating() const {
    return m_clearHotBlend != (m_clearHot ? 1.0f : 0.0f) ||
           m_clearPressBlend != (m_clearPressed && m_clearHot ? 1.0f : 0.0f);
}

// In about 120 ms, out in about 250 ms; pressed in at once nearly, out at the hot pace.
bool SearchBox::Animate() {
    const auto step = [](float& value, float target, float up, float down) {
        const float next = value < target ? std::min(target, value + up) : std::max(target, value - down);
        const bool changed = next != value;
        value = next;
        return changed;
    };
    bool changed = step(m_clearHotBlend, m_clearHot ? 1.0f : 0.0f, 0.14f, 0.07f);
    changed |= step(m_clearPressBlend, m_clearPressed && m_clearHot ? 1.0f : 0.0f, 0.35f, 0.12f);
    if (changed) Paint();
    return Animating();
}

bool SearchBox::OnChar(wchar_t ch, HWND) {
    if (!m_focused) return false;
    if (ch == L'\b') {
        if (m_text.empty() && !m_token.empty()) { // Backspace takes the scope away, as in Explorer
            if (onTokenRemoved) onTokenRemoved();
            return true;
        }
        if (!HasSelection() && m_cursor > 0) {
            m_anchor = m_cursor - 1;
            if (m_anchor > 0 && IS_LOW_SURROGATE(m_text[m_anchor]) && IS_HIGH_SURROGATE(m_text[m_anchor - 1])) --m_anchor;
        }
        if (HasSelection()) {
            DeleteSelection();
            Changed();
        }
        return true;
    }
    if (ch == 0x7F) { // Ctrl+Backspace: the word before the caret
        if (!HasSelection()) m_anchor = WordLeft(m_cursor);
        if (HasSelection()) {
            DeleteSelection();
            Changed();
        }
        return true;
    }
    if (ch < 0x20) return true; // other control characters (Ctrl+A, C, X, V are keys)
    Replace(std::wstring_view(&ch, 1));
    return true;
}

bool SearchBox::OnKeyDown(UINT vk, bool ctrl, HWND owner) {
    if (!m_focused) return false;
    const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    switch (vk) {
    case VK_ESCAPE:
        Clear();
        return true;
    case VK_LEFT:
        if (!shift && HasSelection() && !ctrl)
            MoveTo(Selection().first, false);
        else
            MoveTo(ctrl ? WordLeft(m_cursor) : m_cursor ? m_cursor - 1 : 0, shift);
        return true;
    case VK_RIGHT:
        if (!shift && HasSelection() && !ctrl)
            MoveTo(Selection().second, false);
        else
            MoveTo(ctrl ? WordRight(m_cursor) : m_cursor + 1, shift);
        return true;
    case VK_HOME:
        MoveTo(0, shift);
        return true;
    case VK_END:
        MoveTo(m_text.size(), shift);
        return true;
    case VK_DELETE:
        if (!HasSelection() && m_cursor < m_text.size()) {
            m_anchor = ctrl ? WordRight(m_cursor) : m_cursor + 1;
            if (m_anchor < m_text.size() && IS_LOW_SURROGATE(m_text[m_anchor])) ++m_anchor;
        }
        if (HasSelection()) {
            DeleteSelection();
            Changed();
        }
        return true;
    default: break;
    }
    if (ctrl && vk == 'A') {
        m_anchor = 0;
        m_cursor = m_text.size();
        Paint();
        return true;
    }
    if (ctrl && (vk == 'C' || vk == 'X')) {
        Copy(owner);
        if (vk == 'X' && HasSelection()) {
            DeleteSelection();
            Changed();
        }
        return true;
    }
    if (ctrl && vk == 'V') {
        if (OpenClipboard(owner)) {
            std::wstring text;
            if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
                if (auto* p = static_cast<const wchar_t*>(GlobalLock(h))) {
                    text = p;
                    GlobalUnlock(h);
                }
            }
            CloseClipboard();
            Replace(text);
        }
        return true;
    }
    // Let navigation keys through to the list (Down moves into the results).
    return vk != VK_DOWN && vk != VK_UP && vk != VK_RETURN && vk != VK_TAB;
}

}  // namespace glasswave::ui
