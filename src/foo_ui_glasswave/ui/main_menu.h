#pragma once

namespace glasswave::ui {

struct MenuAction {
    std::wstring label;  // empty = separator
    std::function<void()> run;
    std::vector<MenuAction> children;  // non-empty: a submenu
    bool checked = false;              // radio-style check mark
    bool checkbox = false;             // checked draws a check mark instead of a bullet
    bool disabled = false;             // greyed: shown, not chosen
    // Optional: subscribes while the menu is open; call the updater with fresh submenu actions
    // whenever items change, and return a cleanup callback run when the menu closes.
    std::function<std::function<void()>(std::function<void(std::vector<MenuAction>)>)> onLiveSubmenu;
};

// Ends a group of actions: a separator, unless there is nothing before it or a separator already.
inline void AppendSeparator(std::vector<MenuAction>& actions) {
    if (!actions.empty() && !actions.back().label.empty()) actions.push_back({});
}

// Turns actions into a Win32 popup's items and runs the one chosen. Command ids are numbered
// from `base`; separators at either end of a menu or next to another are left out.
class MenuBuilder {
  public:
    explicit MenuBuilder(UINT base) : m_base(base) {}
    void Append(HMENU menu, const std::vector<MenuAction>& actions); // submenus included
    // Activates any onLiveSubmenu subscriptions; returns a cleanup handle to call before DestroyMenu.
    std::function<void()> AttachLiveSubmenus();
    // Runs the action behind `cmd`: false when `cmd` is not one of these.
    bool Run(UINT cmd) const;

  private:
    struct LiveSubmenu {
        HMENU parent = nullptr;
        HMENU sub = nullptr;
        decltype(MenuAction::onLiveSubmenu) subscribe;
    };
    UINT m_base;
    std::vector<std::function<void()>> m_runs; // per id from m_base
    std::vector<LiveSubmenu> m_live;
};

// Pop up a menu at screen point pt: optional custom actions first, then the complete
// foobar2000 main menu (File/Edit/View/Playback/Library/Help) built by mainmenu_manager,
// so preferences, components, DSP and output settings are always reachable.
void ShowMainMenu(HWND owner, POINT screenPt, const std::vector<MenuAction>& actions = {});

// A plain popup of `actions` at screen point pt (top-left aligned); `checked` gets a check.
void ShowActionMenu(HWND owner, POINT screenPt, const std::vector<MenuAction>& actions, size_t checked = SIZE_MAX);
// The same popup for what was clicked (`anchor`, screen px): below it, or above it where there is
// no room, never over it. A menu opened on the button's press that came up over the pointer took
// the release as a choice (owner, 2026-11-21: the output device menu picked a device by itself).
void ShowActionMenu(HWND owner, const RECT& anchor, const std::vector<MenuAction>& actions,
                    size_t checked = SIZE_MAX);

}  // namespace glasswave::ui
