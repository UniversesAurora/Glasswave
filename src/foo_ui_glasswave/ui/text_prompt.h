#pragma once

#include <functional>
#include <string>

namespace glasswave::ui {

// A small modal input box for names (the NetEase "new playlist" action). The owner is
// disabled until the box closes; onOk receives the typed text (possibly empty).
void PromptText(HWND owner, std::wstring title, std::wstring label, std::wstring initial,
                std::function<void(std::wstring)> onOk);

}  // namespace glasswave::ui
