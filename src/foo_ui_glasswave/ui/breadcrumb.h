#pragma once

#include "ui/main_menu.h"

#include <functional>
#include <string>
#include <vector>

#include <winrt/Windows.UI.Composition.h>

// The address bar's breadcrumb (WMP 12, Windows 7's Explorer): a path of levels, each a name
// that goes there and an arrow that lists the levels below it. The window decides only the
// path (MainWindow::BuildCrumbPath: the opener's path, then the page's own level, rooted at the
// navigation pane); how it looks and behaves is decided here, the same for every page:
//  - every level but the last has an arrow, and its menu lists the levels below (when the path
//    gives none, at least the next one), so arrows never stand without a menu;
//  - hovering a level lights its name and its arrow together (the part under the pointer hot,
//    the other in WMP's "grey" state), fading in quickly and out slowly;
//  - a path wider than the bar keeps its last levels and hands the first ones to the root
//    arrow's menu, as Explorer does.
namespace glasswave::ui {

class Breadcrumb {
  public:
    struct Level {
        std::wstring text;                                 // empty: the leading root arrow
        std::function<void()> open;                        // click on the name
        std::function<std::vector<MenuAction>()> children; // the arrow's menu
        bool literal = false;                              // a user's name: shown untranslated
    };

    // `frames` keeps calling Animate() (the window's animation timer) until it returns false.
    explicit Breadcrumb(std::function<void()> frames) : m_frames(std::move(frames)) {}

    void SetVisual(winrt::Windows::UI::Composition::SpriteVisual visual) { m_visual = std::move(visual); }
    void SetPath(std::vector<Level> path);
    // Where it stands in the window (client px) and the window's scale; repaints.
    void SetBounds(const RECT& bounds, float scale);

    void Paint();
    bool Animate(); // one step of the hover fades; true while one runs

    void OnMouseMove(POINT client); // a point outside the bar unlights it
    void OnMouseLeave() { OnMouseMove({-1, -1}); }
    // A click in the bar (client px): goes to a level or opens its arrow's menu.
    void OnClick(HWND owner, POINT client);

  private:
    struct Placed {
        Level level;
        bool arrow = false;
        float x0 = 0, split = 0, x1 = 0; // DIP: name [x0, split), arrow [split, x1)
    };
    void Layout();
    bool HitTest(POINT client, int* index, int* part) const;

    std::function<void()> m_frames;
    winrt::Windows::UI::Composition::SpriteVisual m_visual{nullptr};
    std::vector<Level> m_path;
    std::vector<Placed> m_placed; // what shows: the root, then the levels that fit
    RECT m_bounds{};
    float m_scale = 1.0f;
    int m_hot = -1, m_hotPart = 0; // part 0 = name, 1 = arrow
    int m_open = -1;               // level whose arrow menu is open
    std::vector<float> m_blend;    // hover fade of each level, 0..1
    std::vector<int> m_blendPart;  // the part last under the pointer, kept while fading out
};

} // namespace glasswave::ui
