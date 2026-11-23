#include "pch.h"
#include "ui/win32.h"

#include <commctrl.h>

#pragma comment(lib, "comctl32.lib")

namespace glasswave::ui {

UINT DpiOf(HWND hwnd) {
    const UINT dpi = hwnd ? GetDpiForWindow(hwnd) : 0;
    return dpi ? dpi : 96;
}

int Dip(HWND hwnd, int dip) {
    return MulDiv(dip, static_cast<int>(DpiOf(hwnd)), 96);
}

std::wstring WindowText(HWND window) {
    std::wstring text(static_cast<size_t>(std::max(GetWindowTextLengthW(window), 0)) + 1, L'\0');
    const int count = GetWindowTextW(window, text.data(), static_cast<int>(text.size()));
    text.resize(static_cast<size_t>(std::max(count, 0)));
    return text;
}

std::wstring ItemText(HWND dialog, int id) {
    return WindowText(GetDlgItem(dialog, id));
}

void Notice(HWND owner, const std::wstring& title, const std::wstring& text, bool warning) {
    MessageBoxW(owner, text.c_str(), title.c_str(), MB_OK | (warning ? MB_ICONWARNING : MB_ICONINFORMATION));
}

bool Confirm(HWND owner, const std::wstring& title, const std::wstring& text, bool irreversible) {
    return MessageBoxW(owner, text.c_str(), title.c_str(),
                       MB_YESNO | MB_DEFBUTTON2 | (irreversible ? MB_ICONWARNING : MB_ICONQUESTION)) == IDYES;
}

bool ConfirmOption(HWND owner, const std::wstring& title, const std::wstring& text,
                   const std::wstring& optionLabel, bool& optionChecked, bool irreversible) {
    TASKDIALOGCONFIG cfg{sizeof(cfg)};
    cfg.hwndParent = owner;
    cfg.dwFlags = TDF_POSITION_RELATIVE_TO_WINDOW | TDF_ALLOW_DIALOG_CANCELLATION |
                  (optionChecked ? TDF_VERIFICATION_FLAG_CHECKED : 0);
    cfg.dwCommonButtons = TDCBF_YES_BUTTON | TDCBF_NO_BUTTON;
    cfg.pszWindowTitle = title.c_str();
    cfg.pszMainIcon = (irreversible || optionChecked) ? TD_WARNING_ICON : TD_INFORMATION_ICON;
    cfg.pszContent = text.c_str();
    cfg.pszVerificationText = optionLabel.c_str();
    cfg.nDefaultButton = IDNO;
    cfg.lpCallbackData = static_cast<LONG_PTR>(irreversible ? 1 : 0);
    cfg.pfCallback = [](HWND hwnd, UINT msg, WPARAM wp, LPARAM, LONG_PTR data) -> HRESULT {
        if (msg == TDN_VERIFICATION_CLICKED) {
            const bool warn = data != 0 || wp != 0;
            SendMessageW(hwnd, TDM_UPDATE_ICON, TDIE_ICON_MAIN,
                         reinterpret_cast<LPARAM>(warn ? TD_WARNING_ICON : TD_INFORMATION_ICON));
        }
        return S_OK;
    };
    int button = IDNO;
    BOOL verified = optionChecked ? TRUE : FALSE;
    if (SUCCEEDED(TaskDialogIndirect(&cfg, &button, nullptr, &verified))) {
        optionChecked = verified != FALSE;
        return button == IDYES;
    }
    optionChecked = false;
    return Confirm(owner, title, text, irreversible);
}

} // namespace glasswave::ui
