#pragma once

#include <functional>

namespace glasswave::ui {

// Modeless QR/Cookie login window owned by the main window. `onSuccess` runs on the main
// thread after the account has been refreshed.
void ShowNeteaseLogin(HWND owner, std::function<void()> onSuccess);

}  // namespace glasswave::ui
