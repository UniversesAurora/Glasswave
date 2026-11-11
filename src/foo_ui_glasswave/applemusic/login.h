#pragma once

#include <functional>

namespace glasswave::applemusic {

// Native account dialog. Secrets stay in the dialog/helper and never enter logs or cfg.
void ShowLogin(HWND owner, std::function<void()> onChanged);
void CloseLogin();

}  // namespace glasswave::applemusic
