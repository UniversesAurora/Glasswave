#pragma once

#include "core/signal.h"

namespace glasswave::ui {

// Self-drawn WMP 12 search field. Text input arrives as WM_CHAR. The owner forwards IME
// composition here so it is drawn in place; the IME's candidate list sits under the box.
// It edits as a Windows edit box does: a caret anywhere in the text (arrows, Home, End, Ctrl for
// words), a selection (Shift, the mouse, Ctrl+A, double click for a word) that typing replaces,
// Delete, and the clipboard (Ctrl+C / X / V).
class SearchBox {
public:
    explicit SearchBox(const winrt::Windows::UI::Composition::ContainerVisual& parent);

    void SetBounds(const RECT& px, float scale);
    const RECT& Bounds() const { return m_bounds; }
    bool HitTest(POINT pt) const { return PtInRect(&m_bounds, pt) != FALSE; }
    void SetFocused(bool focused);
    bool Focused() const { return m_focused; }

    bool OnChar(wchar_t ch, HWND owner);
    bool OnKeyDown(UINT vk, bool ctrl, HWND owner);
    void Clear();
    const std::wstring& Text() const { return m_text; }
    // The text with the IME's uncommitted part (what suggestions are asked for).
    std::wstring Typed() const { return m_text + m_composition; }
    // Sets the text without onChange (a search page shows its term).
    void SetText(std::wstring text);
    // The search scope as Windows 7 shows a search filter: blue text at the start of the box
    // ("NetEase Cloud Music:"). Empty: none. Backspace in an empty box removes it (onTokenRemoved).
    void SetToken(std::wstring token);
    const std::wstring& Token() const { return m_token; }
    // The cross that replaces the magnifier while the box holds a search.
    bool HitClear(POINT pt) const;
    void SetClearHot(bool hot);
    void SetClearPressed(bool pressed);
    // The cross's plate fading between rest, hot and pressed; true while it still changes.
    bool Animate();
    bool Animating() const;
    POINT CaretClientPoint() const;
    // The mouse in the box (client px): a click puts the caret there (Shift: extends the
    // selection), a drag selects, a double click selects a word.
    void OnMouseDown(POINT pt, bool shift);
    void OnMouseDrag(POINT pt);
    void OnDoubleClick(POINT pt);
    // The IME's uncommitted text and its cursor; committed text arrives through InsertText.
    void SetComposition(std::wstring text, size_t cursor);
    void InsertText(std::wstring_view text);

    std::function<void(const std::wstring&)> onChange;
    std::function<void()> onTokenRemoved;
    std::function<void()> onCompose; // the IME's uncommitted text changed

private:
    void Paint();
    void Changed();
    // The shown text (scope token, text, composition at the caret) as one layout.
    struct Shown {
        winrt::com_ptr<IDWriteTextLayout> layout;
        size_t prefix = 0; // characters before the text (the token and its space)
        bool placeholder = false;
    };
    Shown BuildLayout(float height) const;
    size_t IndexAt(POINT pt) const;  // text index under a client point
    bool HasSelection() const { return m_anchor != m_cursor; }
    std::pair<size_t, size_t> Selection() const { return {std::min(m_anchor, m_cursor), std::max(m_anchor, m_cursor)}; }
    void DeleteSelection();
    void Replace(std::wstring_view text); // types `text` over the selection
    void MoveTo(size_t index, bool extend);
    size_t WordLeft(size_t from) const;
    size_t WordRight(size_t from) const;
    void Copy(HWND owner) const;

    winrt::Windows::UI::Composition::SpriteVisual m_visual{nullptr}, m_caret{nullptr};
    core::Signal<>::Connection m_deviceReplaced; // its surface is empty after a device loss
    std::wstring m_text;
    std::wstring m_token;
    std::wstring m_composition;
    size_t m_compositionCursor = 0;
    size_t m_cursor = 0, m_anchor = 0; // caret and selection anchor in m_text
    float m_scroll = 0;                // DIP the text is scrolled left, keeping the caret seen
    RECT m_bounds{};
    float m_scale = 1.0f;
    float m_caretX = 0;  // DIP, relative to the box
    bool m_focused = false;
    bool m_clearHot = false, m_clearPressed = false;
    float m_clearHotBlend = 0, m_clearPressBlend = 0;
};

}  // namespace glasswave::ui
