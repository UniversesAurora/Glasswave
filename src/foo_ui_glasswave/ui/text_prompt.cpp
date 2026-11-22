#include "pch.h"
#include "ui/text_prompt.h"
#include "ui/i18n.h"
#include "ui/win32.h"

#include <commctrl.h>

#pragma comment(lib, "comctl32.lib")

namespace glasswave::ui {

namespace {

constexpr wchar_t kClassName[] = L"Glasswave.TextPrompt";
constexpr int kEdit = 100, kOk = IDOK, kCancel = IDCANCEL;
HWND g_prompt = nullptr;

struct PromptState {
    HWND hwnd = nullptr;
    HWND owner = nullptr;
    HWND edit = nullptr;
    HFONT font = nullptr;
    std::wstring label;
    std::function<void(std::wstring)> onOk;
    bool closed = false;
};

std::shared_ptr<PromptState> StateOf(HWND hwnd) {
    auto* holder = reinterpret_cast<std::shared_ptr<PromptState>*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    return holder && *holder ? *holder : nullptr;
}


void LayoutPrompt(PromptState& state) {
    RECT client{};
    GetClientRect(state.hwnd, &client);
    const int w = client.right - client.left;
    const int h = client.bottom - client.top;
    const int margin = Dip(state.hwnd, 16);
    const int fieldH = Dip(state.hwnd, 24);
    const int buttonW = Dip(state.hwnd, 84);
    const int buttonH = Dip(state.hwnd, 26);
    const int gap = Dip(state.hwnd, 8);
    MoveWindow(state.edit, margin, Dip(state.hwnd, 44), w - margin * 2, fieldH, TRUE);
    const int buttonsY = h - margin - buttonH;
    const int cancelX = w - margin - buttonW;
    const int okX = cancelX - gap - buttonW;
    if (HWND cancel = GetDlgItem(state.hwnd, kCancel))
        MoveWindow(cancel, cancelX, buttonsY, buttonW, buttonH, TRUE);
    if (HWND ok = GetDlgItem(state.hwnd, kOk)) MoveWindow(ok, okX, buttonsY, buttonW, buttonH, TRUE);
}

void ClosePrompt(const std::shared_ptr<PromptState>& state) {
    if (!state || state->closed) return;
    state->closed = true;
    // Re-enable and activate the owner before destroying the modal popup, otherwise
    // Windows cannot return focus to the disabled owner and sends the app behind other windows.
    if (state->owner && IsWindow(state->owner)) {
        EnableWindow(state->owner, TRUE);
        SetActiveWindow(state->owner);
        SetForegroundWindow(state->owner);
    }
    DestroyWindow(state->hwnd);
}

void Submit(const std::shared_ptr<PromptState>& state) {
    if (!state || state->closed) return;
    std::wstring text;
    if (state->edit) text = WindowText(state->edit);
    auto callback = std::move(state->onOk);
    state->onOk = {};
    ClosePrompt(state);
    if (callback) callback(std::move(text));
}

LRESULT CALLBACK EditProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    if (msg == WM_KEYDOWN) {
        if (wp == VK_RETURN) {
            PostMessageW(GetParent(hwnd), WM_COMMAND, kOk, 0);
            return 0;
        }
        if (wp == VK_ESCAPE) {
            PostMessageW(GetParent(hwnd), WM_COMMAND, kCancel, 0);
            return 0;
        }
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

void PaintPrompt(PromptState& state) {
    PAINTSTRUCT ps{};
    HDC dc = BeginPaint(state.hwnd, &ps);
    RECT client{};
    GetClientRect(state.hwnd, &client);
    FillRect(dc, &client, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(30, 57, 91));
    HFONT old = static_cast<HFONT>(SelectObject(dc, state.font));
    RECT label{Dip(state.hwnd, 16), Dip(state.hwnd, 14), client.right - Dip(state.hwnd, 16), Dip(state.hwnd, 40)};
    DrawTextW(dc, state.label.c_str(), -1, &label, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    SelectObject(dc, old);
    EndPaint(state.hwnd, &ps);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto state = StateOf(hwnd);
    switch (msg) {
    case WM_PAINT:
        if (state) PaintPrompt(*state);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_SIZE:
        if (state) LayoutPrompt(*state);
        return 0;
    case WM_COMMAND:
        if (!state) return 0;
        switch (LOWORD(wp)) {
        case kOk: Submit(state); break;
        case kCancel: ClosePrompt(state); break;
        default: break;
        }
        return 0;
    case WM_CLOSE:
        if (state) ClosePrompt(state);
        return 0;
    case WM_NCDESTROY: {
        auto* holder = reinterpret_cast<std::shared_ptr<PromptState>*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (holder) {
            if (*holder) {
                (*holder)->closed = true;
                if ((*holder)->edit) RemoveWindowSubclass((*holder)->edit, EditProc, 1);
                if ((*holder)->font) DeleteObject((*holder)->font);
                if ((*holder)->owner && IsWindow((*holder)->owner)) EnableWindow((*holder)->owner, TRUE);
            }
            delete holder;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        }
        if (g_prompt == hwnd) g_prompt = nullptr;
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    default:
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

void RegisterClass() {
    static std::once_flag once;
    std::call_once(once, [] {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = WndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH));
        wc.lpszClassName = kClassName;
        RegisterClassExW(&wc);
    });
}

}  // namespace

void PromptText(HWND owner, std::wstring title, std::wstring label, std::wstring initial,
                std::function<void(std::wstring)> onOk) {
    if (g_prompt) {
        SetForegroundWindow(g_prompt);
        return;
    }
    RegisterClass();

    auto state = std::make_shared<PromptState>();
    state->owner = owner;
    state->label = std::move(label);
    state->onOk = std::move(onOk);

    RECT ownerRect{0, 0, 420, 180};
    if (owner) GetWindowRect(owner, &ownerRect);
    const int width = Dip(owner, 400);
    const int height = Dip(owner, 160);
    const int x = ownerRect.left + ((ownerRect.right - ownerRect.left) - width) / 2;
    const int y = ownerRect.top + ((ownerRect.bottom - ownerRect.top) - height) / 2;
    HWND hwnd = CreateWindowExW(0, kClassName, title.c_str(), WS_POPUP | WS_CAPTION | WS_SYSMENU, x, y, width, height,
                                owner, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!hwnd) return;
    g_prompt = hwnd;
    state->hwnd = hwnd;
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(new std::shared_ptr<PromptState>(state)));

    const UINT dpi = DpiOf(hwnd);
    state->font = CreateFontW(-MulDiv(12, static_cast<int>(dpi), 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                              DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                              DEFAULT_PITCH, L"Segoe UI");
    state->edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", initial.c_str(),
                                  WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, 0, 0, 0, 0, hwnd,
                                  reinterpret_cast<HMENU>(static_cast<INT_PTR>(kEdit)), nullptr, nullptr);
    HWND ok = CreateWindowW(L"BUTTON", Tr(L"OK").c_str(), WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, 0, 0,
                            0, 0, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kOk)), nullptr, nullptr);
    HWND cancel = CreateWindowW(L"BUTTON", Tr(L"Cancel").c_str(), WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 0, 0,
                                hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCancel)), nullptr, nullptr);
    auto setFont = [&](HWND control) {
        if (control && state->font) SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(state->font), TRUE);
    };
    setFont(state->edit);
    setFont(ok);
    setFont(cancel);
    if (state->edit) SetWindowSubclass(state->edit, EditProc, 1, 0);

    if (owner) EnableWindow(owner, FALSE);
    LayoutPrompt(*state);
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    if (state->edit) {
        SetFocus(state->edit);
        SendMessageW(state->edit, EM_SETSEL, 0, -1);
    }
}

}  // namespace glasswave::ui
