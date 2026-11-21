#pragma once

#include <functional>

namespace glasswave::netease {

// Modeless QR/Cookie login window owned by the main window. `onSuccess` runs on the main
// thread after the account has been refreshed.
void ShowLogin(HWND owner, std::function<void()> onSuccess);

}  // namespace glasswave::netease
