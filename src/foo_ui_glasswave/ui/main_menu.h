#pragma once

namespace glasswave::ui {

struct MenuAction {
    std::wstring label;  // empty = separator
    std::function<void()> run;
    std::vector<MenuAction> children;  // non-empty: a submenu
    bool checked = false;              // radio-style check mark
    bool checkbox = false;             // checked draws a check mark instead of a bullet
    bool disabled = false;             // greyed: shown, not chosen
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
