#include "pch.h"
#include "ui/win32.h"

namespace glasswave::ui {

UINT DpiOf(HWND hwnd) {
    const UINT dpi = hwnd ? GetDpiForWindow(hwnd) : 0;
    return dpi ? dpi : 96;
}

int Dip(HWND hwnd, int dip) { return MulDiv(dip, static_cast<int>(DpiOf(hwnd)), 96); }

std::wstring WindowText(HWND window) {
    std::wstring text(static_cast<size_t>(std::max(GetWindowTextLengthW(window), 0)) + 1, L'\0');
    const int count = GetWindowTextW(window, text.data(), static_cast<int>(text.size()));
    text.resize(static_cast<size_t>(std::max(count, 0)));
    return text;
}

std::wstring ItemText(HWND dialog, int id) { return WindowText(GetDlgItem(dialog, id)); }

void Notice(HWND owner, const std::wstring& title, const std::wstring& text, bool warning) {
    MessageBoxW(owner, text.c_str(), title.c_str(), MB_OK | (warning ? MB_ICONWARNING : MB_ICONINFORMATION));
}

bool Confirm(HWND owner, const std::wstring& title, const std::wstring& text, bool irreversible) {
    return MessageBoxW(owner, text.c_str(), title.c_str(),
                       MB_YESNO | MB_DEFBUTTON2 | (irreversible ? MB_ICONWARNING : MB_ICONQUESTION)) == IDYES;
}

} // namespace glasswave::ui
