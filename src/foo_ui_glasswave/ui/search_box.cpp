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
constexpr float kIconRoom = 22.0f;
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
    Paint();
}

void SearchBox::Paint() {
    auto& host = gfx::CompositionHost::Get();
    const int w = std::max<int>(m_bounds.right - m_bounds.left, 1), h = std::max<int>(m_bounds.bottom - m_bounds.top, 1);
    const float s = m_scale;
    host.Repaint(m_visual, w, h, s, [&](ID2D1DeviceContext* dc) {
        const D2D1_SIZE_F size{w / s, h / s};
        wmp::paint::SearchFrame(dc, size, m_focused);
        winrt::com_ptr<ID2D1SolidColorBrush> ink;
        const bool placeholder = m_text.empty();
        const std::wstring& placeholderText = Tr(L"Search");
        dc->CreateSolidColorBrush(placeholder ? D2D1::ColorF(0x8D8D8D) : D2D1::ColorF(0x1E1E1E), ink.put());
        auto layout = MakeText(placeholder ? std::wstring_view(placeholderText) : std::wstring_view(m_text), 12.0f,
                               DWRITE_FONT_WEIGHT_NORMAL, 4096, size.height);
        if (placeholder)
            layout->SetFontStyle(DWRITE_FONT_STYLE_ITALIC, {0, static_cast<UINT32>(placeholderText.size())});
        // Keep the end of long text visible.
        DWRITE_TEXT_METRICS tm{};
        layout->GetMetrics(&tm);
        const float room = size.width - kTextLeft - kIconRoom;
        const float shift = (!placeholder && tm.widthIncludingTrailingWhitespace > room)
                                ? tm.widthIncludingTrailingWhitespace - room
                                : 0.0f;
        dc->PushAxisAlignedClip({kTextLeft, 0, kTextLeft + room, size.height}, D2D1_ANTIALIAS_MODE_ALIASED);
        dc->DrawTextLayout({kTextLeft - shift, 0}, layout.get(), ink.get());
        dc->PopAxisAlignedClip();
        m_caretX = kTextLeft + (placeholder ? 0.0f : tm.widthIncludingTrailingWhitespace - shift);
    });
    const float caretH = std::round(14.0f * s);
    m_caret.Size(float2{std::max(1.0f, std::floor(s)), caretH});
    m_caret.Offset(float3{std::round(m_bounds.left + m_caretX * s), std::round(m_bounds.top + (h - caretH) / 2), 0});
    m_caret.IsVisible(m_focused);
}

POINT SearchBox::CaretClientPoint() const {
    return {static_cast<LONG>(m_bounds.left + m_caretX * m_scale), m_bounds.top};
}

void SearchBox::Changed() {
    Paint();
    if (onChange) onChange(m_text);
}

void SearchBox::Clear() {
    if (m_text.empty()) return;
    m_text.clear();
    Changed();
}

bool SearchBox::OnChar(wchar_t ch, HWND) {
    if (!m_focused) return false;
    if (ch == L'\b') {
        if (!m_text.empty()) {
            m_text.pop_back();
            if (!m_text.empty() && IS_HIGH_SURROGATE(m_text.back())) m_text.pop_back();
            Changed();
        }
        return true;
    }
    if (ch == 0x7F) {  // Ctrl+Backspace
        m_text.clear();
        Changed();
        return true;
    }
    if (ch < 0x20) return true;  // swallow other control characters
    m_text.push_back(ch);
    Changed();
    return true;
}

bool SearchBox::OnKeyDown(UINT vk, bool ctrl, HWND owner) {
    if (!m_focused) return false;
    if (vk == VK_ESCAPE) {
        Clear();
        return true;
    }
    if (ctrl && vk == 'V') {
        if (OpenClipboard(owner)) {
            if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
                if (auto* p = static_cast<const wchar_t*>(GlobalLock(h))) {
                    for (const wchar_t* c = p; *c; ++c)
                        if (*c >= 0x20) m_text.push_back(*c);
                    GlobalUnlock(h);
                }
            }
            CloseClipboard();
            Changed();
        }
        return true;
    }
    if (ctrl && vk == 'A') return true;
    // Let navigation keys through to the list (Down moves into the results).
    return vk != VK_DOWN && vk != VK_UP && vk != VK_RETURN && vk != VK_TAB;
}

}  // namespace glasswave::ui
