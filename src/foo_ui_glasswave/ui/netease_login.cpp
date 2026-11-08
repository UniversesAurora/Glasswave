#include "pch.h"
#include "ui/netease_login.h"
#include "netease/provider.h"
#include "ui/i18n.h"

#include <gdiplus.h>
#include <objidl.h>
#include <wincrypt.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

#pragma comment(lib, "gdiplus.lib")

namespace glasswave::ui {

namespace {

constexpr wchar_t kClassName[] = L"Glasswave.NeteaseLogin";
constexpr UINT_PTR kPollTimer = 1;
constexpr int kPrimary = 1, kSecondary = 2;

HWND g_login = nullptr;

ULONG_PTR GdiplusToken() {
    static ULONG_PTR token = 0;
    static std::once_flag once;
    std::call_once(once, [] {
        Gdiplus::GdiplusStartupInput input;
        GdiplusStartup(&token, &input, nullptr);
    });
    return token;
}

struct LoginState {
    enum class Mode { Starting, Install, Downloading, Qr, Cookie, Error, Done };
    HWND hwnd = nullptr;
    HWND owner = nullptr;
    HWND primary = nullptr;
    HWND secondary = nullptr;
    HWND edit = nullptr;
    HFONT titleFont = nullptr;
    HFONT bodyFont = nullptr;
    std::function<void()> onSuccess;
    Mode mode = Mode::Starting;
    std::wstring status;
    std::wstring key;
    IStream* qrStream = nullptr;
    Gdiplus::Bitmap* qr = nullptr;
    bool polling = false;
    bool closed = false;
};

std::shared_ptr<LoginState> StateOf(HWND hwnd) {
    auto* holder = reinterpret_cast<std::shared_ptr<LoginState>*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    return holder && *holder ? *holder : nullptr;
}

int Dip(HWND hwnd, int value) {
    const UINT dpi = hwnd ? GetDpiForWindow(hwnd) : 96;
    return MulDiv(value, static_cast<int>(dpi), 96);
}

void FreeQr(LoginState& state) {
    delete state.qr;
    state.qr = nullptr;
    if (state.qrStream) {
        state.qrStream->Release();
        state.qrStream = nullptr;
    }
}

bool DecodeQr(const std::wstring& dataUrl, IStream** streamOut, Gdiplus::Bitmap** bitmapOut) {
    const size_t comma = dataUrl.find(L',');
    if (comma == std::wstring::npos) return false;
    const std::wstring base64 = dataUrl.substr(comma + 1);
    DWORD bytes = 0;
    if (!CryptStringToBinaryW(base64.c_str(), static_cast<DWORD>(base64.size()), CRYPT_STRING_BASE64, nullptr, &bytes,
                              nullptr, nullptr) ||
        !bytes)
        return false;
    std::vector<BYTE> data(bytes);
    if (!CryptStringToBinaryW(base64.c_str(), static_cast<DWORD>(base64.size()), CRYPT_STRING_BASE64, data.data(),
                              &bytes, nullptr, nullptr))
        return false;
    IStream* stream = nullptr;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream))) return false;
    if (FAILED(stream->Write(data.data(), bytes, nullptr))) {
        stream->Release();
        return false;
    }
    LARGE_INTEGER zero{};
    stream->Seek(zero, STREAM_SEEK_SET, nullptr);
    auto* bitmap = new Gdiplus::Bitmap(stream, TRUE);
    if (bitmap->GetLastStatus() != Gdiplus::Ok) {
        delete bitmap;
        stream->Release();
        return false;
    }
    *streamOut = stream;
    *bitmapOut = bitmap;
    return true;
}

void Layout(LoginState& state) {
    if (!state.hwnd) return;
    RECT client{};
    GetClientRect(state.hwnd, &client);
    const int w = client.right - client.left;
    const int h = client.bottom - client.top;
    const int buttonW = Dip(state.hwnd, 156);
    const int buttonH = Dip(state.hwnd, 27);
    const int gap = Dip(state.hwnd, 10);
    const int y = h - Dip(state.hwnd, 48);
    const bool two = state.mode == LoginState::Mode::Qr || state.mode == LoginState::Mode::Cookie || state.mode == LoginState::Mode::Install ||
                     state.mode == LoginState::Mode::Error;
    const int total = two ? buttonW * 2 + gap : buttonW;
    int x = (w - total) / 2;
    SetWindowPos(state.primary, nullptr, x, y, buttonW, buttonH, SWP_NOZORDER | SWP_NOACTIVATE);
    if (state.secondary) {
        SetWindowPos(state.secondary, nullptr, x + buttonW + gap, y, buttonW, buttonH,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        ShowWindow(state.secondary, two ? SW_SHOW : SW_HIDE);
    }
    const wchar_t* primaryText = L"Close";
    switch (state.mode) {
    case LoginState::Mode::Install: primaryText = L"Download"; break;
    case LoginState::Mode::Qr: primaryText = L"Use cookie instead"; break;
    case LoginState::Mode::Cookie: primaryText = L"Sign in"; break;
    case LoginState::Mode::Error: primaryText = L"Retry"; break;
    default: primaryText = L"Close"; break;
    }
    SetWindowTextW(state.primary, Tr(primaryText).c_str());
    ShowWindow(state.primary, state.mode == LoginState::Mode::Starting || state.mode == LoginState::Mode::Downloading ? SW_HIDE : SW_SHOW);
    if (state.mode == LoginState::Mode::Cookie) {
        if (!state.edit) {
            state.edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                         WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL,
                                         0, 0, 0, 0, state.hwnd, nullptr, nullptr, nullptr);
            if (state.edit && state.bodyFont) SendMessageW(state.edit, WM_SETFONT, reinterpret_cast<WPARAM>(state.bodyFont), TRUE);
        }
        if (state.edit)
            SetWindowPos(state.edit, nullptr, Dip(state.hwnd, 30), Dip(state.hwnd, 96), w - Dip(state.hwnd, 60),
                         Dip(state.hwnd, 270), SWP_NOZORDER | SWP_NOACTIVATE);
    } else if (state.edit) {
        DestroyWindow(state.edit);
        state.edit = nullptr;
    }
    InvalidateRect(state.hwnd, nullptr, TRUE);
}

void SetMode(const std::shared_ptr<LoginState>& state, LoginState::Mode mode, std::wstring status) {
    if (!state || state->closed) return;
    state->mode = mode;
    state->status = std::move(status);
    if (mode != LoginState::Mode::Qr) {
        KillTimer(state->hwnd, kPollTimer);
        state->polling = false;
    }
    Layout(*state);
}

void CloseWindow(const std::shared_ptr<LoginState>& state) {
    if (!state || state->closed) return;
    state->closed = true;
    KillTimer(state->hwnd, kPollTimer);
    if (state->owner) EnableWindow(state->owner, TRUE);
    if (state->owner) SetForegroundWindow(state->owner);
    DestroyWindow(state->hwnd);
}

void StartServiceOrInstall(const std::shared_ptr<LoginState>& state);

void SetQrImage(const std::shared_ptr<LoginState>& state, const std::wstring& dataUrl) {
    FreeQr(*state);
    IStream* stream = nullptr;
    Gdiplus::Bitmap* bitmap = nullptr;
    if (!DecodeQr(dataUrl, &stream, &bitmap)) {
        SetMode(state, LoginState::Mode::Error, L"The QR code image could not be decoded.");
        return;
    }
    state->qrStream = stream;
    state->qr = bitmap;
    SetMode(state, LoginState::Mode::Qr, L"Scan the QR code with the NetEase Cloud Music app.");
    SetTimer(state->hwnd, kPollTimer, 2000, nullptr);
}

void RequestQr(const std::shared_ptr<LoginState>& state) {
    SetMode(state, LoginState::Mode::Starting, L"Preparing the QR code...");
    netease::Provider::Get().Tasks().Post([state] {
        abort_callback_impl abort;
        std::wstring error;
        netease::QrLogin key;
        netease::QrLogin qr;
        auto& provider = netease::Provider::Get();
        if (!provider.Service().EnsureStarted(abort, &error) || !provider.Api().QrKey(abort, key, error) ||
            !provider.Api().QrCreate(key.key, abort, qr, error)) {
            fb2k::inMainThread([state, error] {
                if (!state->closed) SetMode(state, LoginState::Mode::Error, error);
            });
            return;
        }
        fb2k::inMainThread([state, key = key.key, qr] {
            if (state->closed) return;
            state->key = key;
            SetQrImage(state, qr.imageDataUrl);
        });
    });
}

void PollQr(const std::shared_ptr<LoginState>& state) {
    if (!state || state->closed || state->polling || state->key.empty()) return;
    state->polling = true;
    const std::wstring key = state->key;
    netease::Provider::Get().Tasks().Post([state, key] {
        abort_callback_impl abort;
        const netease::QrStatus status = netease::Provider::Get().Api().QrCheck(key, abort);
        fb2k::inMainThread([state, status] {
            if (state->closed) return;
            state->polling = false;
            switch (status.code) {
            case 800:
                RequestQr(state);
                break;
            case 801:
                state->status = L"Waiting for you to scan the QR code...";
                InvalidateRect(state->hwnd, nullptr, TRUE);
                break;
            case 802:
                state->status = L"Scanned. Confirm the sign-in on your phone.";
                InvalidateRect(state->hwnd, nullptr, TRUE);
                break;
            case 803:
                if (status.cookie.empty()) {
                    SetMode(state, LoginState::Mode::Error, L"The login service returned no session cookie.");
                    break;
                }
                KillTimer(state->hwnd, kPollTimer);
                state->status = L"Signing in...";
                InvalidateRect(state->hwnd, nullptr, TRUE);
                netease::Provider::Get().OnLoginCookie(status.cookie, [state](bool ok, std::wstring error) {
                    if (state->closed) return;
                    if (!ok) {
                        SetMode(state, LoginState::Mode::Error, error.empty() ? L"Sign-in failed." : error);
                        return;
                    }
                    if (state->onSuccess) state->onSuccess();
                    CloseWindow(state);
                });
                break;
            default:
                if (!status.message.empty()) {
                    state->status = status.message;
                    InvalidateRect(state->hwnd, nullptr, TRUE);
                }
                break;
            }
        });
    });
}

void SubmitCookie(const std::shared_ptr<LoginState>& state) {
    if (!state->edit) return;
    const int length = GetWindowTextLengthW(state->edit);
    if (length <= 0) {
        SetMode(state, LoginState::Mode::Error, L"Paste the Cookie header from a signed-in NetEase session.");
        return;
    }
    std::wstring cookie(static_cast<size_t>(length) + 1, L'\0');
    const int got = GetWindowTextW(state->edit, cookie.data(), length + 1);
    cookie.resize(static_cast<size_t>(got));
    state->status = L"Signing in...";
    InvalidateRect(state->hwnd, nullptr, TRUE);
    netease::Provider::Get().OnLoginCookie(cookie, [state](bool ok, std::wstring error) {
        if (state->closed) return;
        if (!ok) {
            SetMode(state, LoginState::Mode::Error, error.empty() ? L"Sign-in failed." : error);
            return;
        }
        if (state->onSuccess) state->onSuccess();
        CloseWindow(state);
    });
}

void DownloadService(const std::shared_ptr<LoginState>& state) {
    SetMode(state, LoginState::Mode::Downloading, L"Downloading the NetEase service (about 65 MB)...");
    netease::Provider::Get().Tasks().Post([state] {
        abort_callback_impl abort;
        std::wstring error;
        const bool ok = netease::Provider::Get().Service().Install(
            abort,
            [state](long long received, long long total) {
                if (!state || state->closed || !total) return;
                const int percent = static_cast<int>(received * 100 / total);
                fb2k::inMainThread([state, percent] {
                    if (state->closed || state->mode != LoginState::Mode::Downloading) return;
                    state->status = L"Downloading the NetEase service... " + std::to_wstring(percent) + L"%";
                    InvalidateRect(state->hwnd, nullptr, TRUE);
                });
            },
            &error);
        fb2k::inMainThread([state, ok, error] {
            if (state->closed) return;
            if (!ok) {
                SetMode(state, LoginState::Mode::Error, error.empty() ? L"Download failed." : error);
                return;
            }
            StartServiceOrInstall(state);
        });
    });
}

void StartServiceOrInstall(const std::shared_ptr<LoginState>& state) {
    if (!state || state->closed) return;
    auto& provider = netease::Provider::Get();
    if (!provider.Service().Available()) {
        SetMode(state, LoginState::Mode::Install,
                L"The NetEase service is not installed. Download it from the NeteaseCloudMusicApi Enhanced "
                L"GitHub release (about 65 MB).");
        return;
    }
    SetMode(state, LoginState::Mode::Starting, L"Starting the NetEase service...");
    provider.StartService([state](bool ok, std::wstring error) {
        if (!state || state->closed) return;
        if (!ok) {
            SetMode(state, LoginState::Mode::Error, error.empty() ? L"The NetEase service did not start." : error);
            return;
        }
        RequestQr(state);
    });
}

void Paint(LoginState& state) {
    PAINTSTRUCT ps{};
    HDC dc = BeginPaint(state.hwnd, &ps);
    RECT client{};
    GetClientRect(state.hwnd, &client);
    FillRect(dc, &client, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(30, 57, 91));

    HFONT old = static_cast<HFONT>(SelectObject(dc, state.titleFont));
    RECT title{0, Dip(state.hwnd, 18), client.right, Dip(state.hwnd, 46)};
    DrawTextW(dc, Tr(L"NetEase Cloud Music").c_str(), -1, &title, DT_CENTER | DT_SINGLELINE);
    SelectObject(dc, state.bodyFont);

    RECT subtitle{ Dip(state.hwnd, 24), Dip(state.hwnd, 52), client.right - Dip(state.hwnd, 24),
                   Dip(state.hwnd, 88) };
    DrawTextW(dc,
              Tr(state.mode == LoginState::Mode::Cookie
                     ? L"Paste the Cookie header from a signed-in NetEase session."
                     : L"Sign in to browse your playlists and play lossless NetEase tracks.")
                  .c_str(),
              -1, &subtitle, DT_CENTER | DT_WORDBREAK);

    if (state.mode != LoginState::Mode::Cookie && state.qr) {
        const int size = Dip(state.hwnd, 260);
        const int x = (client.right - size) / 2;
        const int y = Dip(state.hwnd, 96);
        Gdiplus::Graphics graphics(dc);
        graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        graphics.DrawImage(state.qr, Gdiplus::Rect(x, y, size, size));
    }

    SetTextColor(dc, state.mode == LoginState::Mode::Error ? RGB(160, 32, 32) : RGB(90, 90, 90));
    RECT status{ Dip(state.hwnd, 26), client.bottom - Dip(state.hwnd, 112),
                 client.right - Dip(state.hwnd, 26), client.bottom - Dip(state.hwnd, 52) };
    DrawTextW(dc, Tr(state.status).c_str(), -1, &status, DT_CENTER | DT_WORDBREAK);

    SelectObject(dc, old);
    EndPaint(state.hwnd, &ps);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto state = StateOf(hwnd);
    switch (msg) {
    case WM_PAINT:
        if (state) Paint(*state);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_COMMAND:
        if (!state) return 0;
        switch (LOWORD(wp)) {
        case kPrimary:
            switch (state->mode) {
            case LoginState::Mode::Install: DownloadService(state); break;
            case LoginState::Mode::Error: StartServiceOrInstall(state); break;
            case LoginState::Mode::Qr:
                SetMode(state, LoginState::Mode::Cookie, L"Paste the Cookie header from a signed-in session.");
                break;
            case LoginState::Mode::Cookie: SubmitCookie(state); break;
            default: CloseWindow(state); break;
            }
            break;
        case kSecondary:
            if (state->mode == LoginState::Mode::Cookie)
                RequestQr(state);
            else
                CloseWindow(state);
            break;
        default: break;
        }
        return 0;
    case WM_TIMER:
        if (state && wp == kPollTimer) PollQr(state);
        return 0;
    case WM_CLOSE:
        if (state) CloseWindow(state);
        return 0;
    case WM_NCDESTROY: {
        auto* holder = reinterpret_cast<std::shared_ptr<LoginState>*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (holder) {
            if (*holder) {
                (*holder)->closed = true;
                FreeQr(**holder);
                if ((*holder)->titleFont) DeleteObject((*holder)->titleFont);
                if ((*holder)->bodyFont) DeleteObject((*holder)->bodyFont);
            }
            delete holder;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        }
        if (g_login == hwnd) g_login = nullptr;
        if (state && state->owner) EnableWindow(state->owner, TRUE);
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    default:
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

void RegisterWindowClass() {
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

void ShowNeteaseLogin(HWND owner, std::function<void()> onSuccess) {
    if (g_login) {
        SetForegroundWindow(g_login);
        return;
    }
    RegisterWindowClass();
    GdiplusToken();

    auto state = std::make_shared<LoginState>();
    state->owner = owner;
    state->onSuccess = std::move(onSuccess);

    RECT ownerRect{0, 0, 440, 560};
    if (owner) GetWindowRect(owner, &ownerRect);
    const int width = Dip(owner, 440);
    const int height = Dip(owner, 560);
    const int x = ownerRect.left + ((ownerRect.right - ownerRect.left) - width) / 2;
    const int y = ownerRect.top + ((ownerRect.bottom - ownerRect.top) - height) / 2;

    HWND hwnd = CreateWindowExW(0, kClassName, Tr(L"NetEase Cloud Music").c_str(), WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
                                x, y, width, height, owner, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!hwnd) return;
    g_login = hwnd;
    state->hwnd = hwnd;
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(new std::shared_ptr<LoginState>(state)));

    const UINT dpi = GetDpiForWindow(hwnd) ? GetDpiForWindow(hwnd) : 96;
    state->titleFont = CreateFontW(-MulDiv(15, static_cast<int>(dpi), 72), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                                   DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                   DEFAULT_PITCH, L"Segoe UI");
    state->bodyFont = CreateFontW(-MulDiv(11, static_cast<int>(dpi), 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                  DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                  DEFAULT_PITCH, L"Segoe UI");

    state->primary = CreateWindowW(L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, 0, 0, 0, 0, hwnd,
                                   reinterpret_cast<HMENU>(static_cast<INT_PTR>(kPrimary)), nullptr, nullptr);
    state->secondary = CreateWindowW(L"BUTTON", Tr(L"Close").c_str(), WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0, hwnd,
                                     reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSecondary)), nullptr, nullptr);
    if (state->bodyFont) {
        if (state->primary) SendMessageW(state->primary, WM_SETFONT, reinterpret_cast<WPARAM>(state->bodyFont), TRUE);
        if (state->secondary) SendMessageW(state->secondary, WM_SETFONT, reinterpret_cast<WPARAM>(state->bodyFont), TRUE);
    }

    if (owner) EnableWindow(owner, FALSE);
    Layout(*state);
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    StartServiceOrInstall(state);
}

}  // namespace glasswave::ui
