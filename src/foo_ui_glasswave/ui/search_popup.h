#pragma once

#include <functional>
#include <string>
#include <vector>

// The search box's drop-down, after Windows 7 Explorer's (owner's captures, 2026-11-15): a white
// list of earlier searches or of suggestions under the box, and a footer with the search scopes
// as blue links ("搜索范围:", as Explorer's "添加搜索筛选器" with its filter links). It never takes
// the focus: the box keeps the keyboard, and the window forwards Up, Down, Enter and Escape.
namespace glasswave::ui {

class SearchPopup {
  public:
    struct Row {
        std::wstring text;
        std::wstring detail; // grey, right-aligned (what kind of result, from where)
        bool history = false; // an earlier search: a cross on it deletes it
    };
    struct Link {
        std::wstring text;
        bool current = false; // the chosen scope: plain text, not a link
    };
    struct Model {
        std::vector<Row> rows;
        std::wstring note;      // grey line when there are no rows ("Searching...")
        std::wstring footer;    // the footer's label
        std::vector<Link> links;
        std::wstring clearLink; // non-empty: a link that clears the history
    };

    explicit SearchPopup(HWND owner);
    ~SearchPopup();
    SearchPopup(const SearchPopup&) = delete;
    SearchPopup& operator=(const SearchPopup&) = delete;

    // Shows the drop-down under `anchor` (screen px, the search box) or updates it in place.
    void Show(const RECT& anchor, Model model);
    void Hide();
    bool Visible() const { return m_hwnd && IsWindowVisible(m_hwnd); }
    // Moves the highlight through the rows (Up / Down); -1 none.
    void MoveHot(int delta);
    int HotRow() const { return m_hot; }

    std::function<void(size_t row)> onRow;
    std::function<void(size_t link)> onLink;
    std::function<void()> onClear;
    std::function<void(size_t row)> onDelete; // a history row's cross

  private:
    struct Part {
        RECT rect{};
        int kind = 0; // 1 row, 2 scope link, 3 clear link, 4 a history row's cross
        size_t index = 0;
    };
    static LRESULT CALLBACK Proc(HWND, UINT, WPARAM, LPARAM);
    LRESULT Handle(UINT msg, WPARAM wp, LPARAM lp);
    void Layout(int width);
    void Paint(HDC dc);
    const Part* PartAt(POINT pt) const;
    int Px(float dip) const { return static_cast<int>(std::lround(dip * m_dpi / 96.0f)); }

    HWND m_owner = nullptr, m_hwnd = nullptr;
    HFONT m_font = nullptr, m_underline = nullptr;
    UINT m_dpi = 96;
    Model m_model;
    std::vector<Part> m_parts;
    int m_height = 0, m_footerTop = 0;
    int m_hot = -1;          // highlighted row
    int m_hotLink = -1;      // part index of the link (or cross) under the mouse
    int m_pressed = -1;      // part index of the cross held down
    std::vector<float> m_linkBlend; // each scope button's plate, fading in under the mouse
    bool m_tracking = false;
};

} // namespace glasswave::ui
