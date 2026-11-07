#pragma once

namespace glasswave::ui {

// Self-drawn WMP 12 search field. Text input arrives as WM_CHAR (IME composition is
// handled by the system IME window, positioned via CaretScreenPoint).
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
    POINT CaretClientPoint() const;

    std::function<void(const std::wstring&)> onChange;

private:
    void Paint();
    void Changed();

    winrt::Windows::UI::Composition::SpriteVisual m_visual{nullptr}, m_caret{nullptr};
    std::wstring m_text;
    RECT m_bounds{};
    float m_scale = 1.0f;
    float m_caretX = 0;  // DIP, relative to the box
    bool m_focused = false;
};

}  // namespace glasswave::ui
