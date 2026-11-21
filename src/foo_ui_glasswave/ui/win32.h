#pragma once

#include <string>

// Small Win32 pieces Glasswave's own windows and dialogs share: DPI, control text, the
// confirmations and notices, and the window procedure guard.

namespace glasswave::ui {

// The window's DPI; 96 without a window.
UINT DpiOf(HWND hwnd);
// `dip` (1/96 inch) in the window's pixels.
int Dip(HWND hwnd, int dip);

// A control's text, and a dialog item's.
std::wstring WindowText(HWND window);
std::wstring ItemText(HWND dialog, int id);

// One look for Glasswave's message boxes (`title` and `text` already translated).
// A notice with OK; `warning`: something failed.
void Notice(HWND owner, const std::wstring& title, const std::wstring& text, bool warning = false);
// A Yes / No question, No the default; `irreversible` (it cannot be undone): the warning icon.
bool Confirm(HWND owner, const std::wstring& title, const std::wstring& text, bool irreversible = false);

// Runs a window object's message handler: an exception is reported on the console and the
// message goes to DefWindowProc instead of unwinding into user32.
template <class Handler>
LRESULT GuardedWindowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, Handler&& handler) {
    try {
        return handler();
    } catch (const std::exception& e) {
        console::complain("Glasswave", e.what());
    } catch (const winrt::hresult_error& e) {
        console::complain("Glasswave", pfc::stringcvt::string_utf8_from_wide(e.message().c_str()));
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace glasswave::ui
