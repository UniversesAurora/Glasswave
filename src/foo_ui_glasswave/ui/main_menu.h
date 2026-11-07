#pragma once

namespace glasswave::ui {

// Pop up the foobar2000 main menu (File/Edit/View/Playback/Library/Help) at screen point pt
// and run the chosen command. The content comes from mainmenu_manager, so preferences,
// components, DSP and output settings are all reachable from here.
void ShowMainMenu(HWND owner, POINT screenPt);

}  // namespace glasswave::ui
