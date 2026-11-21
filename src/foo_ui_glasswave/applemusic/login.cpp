#include "pch.h"
#include "applemusic/login.h"
#include "core/text.h"
#include "applemusic/client.h"
#include "core/json.h"
#include "stream/provider.h"
#include "ui/i18n.h"

#include <atomic>

namespace glasswave::applemusic {
namespace {
namespace json = core::json;
using Value = winrt::Windows::Data::Json::JsonValue;
constexpr wchar_t kClass[] = L"Glasswave.AppleMusicLogin";
constexpr UINT kResult = WM_APP + 77;
constexpr int kEmail = 201, kPassword = 202, kCode = 203, kLogin = 204, kVerify = 205, kStatus = 206;
HWND g_window = nullptr;

using core::Utf8;

std::wstring Text(HWND window, int id) {
    HWND edit = GetDlgItem(window, id);
    std::wstring value(static_cast<size_t>(GetWindowTextLengthW(edit)) + 1, L'\0');
    const int count = GetWindowTextW(edit, value.data(), static_cast<int>(value.size()));
    value.resize(static_cast<size_t>(std::max(0, count)));
    return value;
}
template <typename String> void Erase(String& text) {
    if (!text.empty()) SecureZeroMemory(text.data(), text.size() * sizeof(typename String::value_type));
    text.clear();
}

struct LoginState {
    HWND hwnd = nullptr;
    HFONT font = nullptr;
    abort_callback_impl abort;
    std::thread worker;
    std::mutex mutex;
    std::condition_variable wake;
    std::wstring status = L"Sign in with your Apple Account. Your password is not saved by Glasswave.";
    std::string command;
    std::wstring endpoint;
    bool stopping = false, active = false, pending = false, verified = false;
    std::function<void()> changed;

    ~LoginState() { Stop(); }
    void Stop() {
        {
            std::lock_guard lock(mutex);
            stopping = true;
            Erase(command);
        }
        abort.abort();
        wake.notify_all();
        if (worker.joinable()) worker.join();
    }
    void Publish(std::wstring text, bool signedIn = false) {
        {
            std::lock_guard lock(mutex);
            status = std::move(text);
            verified = signedIn;
        }
        PostMessageW(hwnd, kResult, 0, 0);
    }
    void Run() {
        while (!abort.is_aborting()) {
            std::string body;
            std::wstring path;
            {
                std::unique_lock lock(mutex);
                const auto ready = [this] { return stopping || pending; };
                // Polled once a second only while an authorization is under way; otherwise the
                // thread sleeps until a command arrives or the dialog closes.
                if (active)
                    wake.wait_for(lock, std::chrono::seconds(1), ready);
                else
                    wake.wait(lock, ready);
                if (stopping) return;
                if (pending) {
                    body.swap(command);
                    path.swap(endpoint);
                    pending = false;
                } else if (!active) {
                    continue;
                }
            }
            try {
                if (!path.empty()) {
                    Request(path, abort, L"POST", std::move(body)); // wiped by the request
                    Erase(body);
                    active = true;
                }
                const Object snapshot = Request(L"/api/v1/drm/status", abort);
                const auto state = json::GetObject(snapshot, L"state");
                const auto auth = state ? json::GetString(*state, L"authentication") : std::wstring();
                if (auth == L"challenging") {
                    const auto challenge = json::GetObject(snapshot, L"challenge");
                    const std::wstring description = challenge ? json::GetString(*challenge, L"description") : std::wstring();
                    Publish(description.empty() ? L"Approve the sign-in on your Apple device, then enter its verification code." : description);
                } else if (auth == L"failed") {
                    active = false;
                    Publish(L"Apple Account sign-in failed. Check the account and try again.");
                } else if (auth == L"logged_in") {
                    const auto account = Request(L"/api/v1/glasswave/account", abort);
                    if (json::GetBool(account, L"signedIn")) {
                        active = false;
                        Publish(L"Signed in to Apple Music.", true);
                    } else {
                        Publish(L"Completing Apple Music authorization...");
                    }
                } else {
                    Publish(L"Waiting for Apple Account authorization...");
                }
            } catch (const exception_aborted&) {
                Erase(body);
                return;
            } catch (const std::exception&) {
                Erase(body);
                active = false;
                Publish(L"Cannot complete Apple Music sign-in. Check that the local helper is running and try again.");
            }
        }
    }
};

LoginState* State(HWND hwnd) { return reinterpret_cast<LoginState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA)); }
int Dip(HWND hwnd, int value) { return MulDiv(value, static_cast<int>(GetDpiForWindow(hwnd)), 96); }
void Control(LoginState& state, const wchar_t* type, const wchar_t* label, int id, DWORD style,
             int x, int y, int width, int height) {
    const auto text = ui::Tr(label);
    HWND control = CreateWindowExW(wcscmp(type, L"EDIT") == 0 ? WS_EX_CLIENTEDGE : 0, type, text.c_str(),
                                    WS_CHILD | WS_VISIBLE | style, Dip(state.hwnd, x), Dip(state.hwnd, y),
                                    Dip(state.hwnd, width), Dip(state.hwnd, height), state.hwnd,
                                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), nullptr, nullptr);
    SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(state.font), TRUE);
    if (wcscmp(type, L"EDIT") == 0) SendMessageW(control, EM_SETLIMITTEXT, id == kCode ? 64 : 512, 0);
}
void Submit(LoginState& state, bool verify) {
    Object request;
    auto password = Text(state.hwnd, kPassword);
    if (verify) {
        auto code = Text(state.hwnd, kCode);
        if (code.empty()) return;
        request.Insert(L"reply", Value::CreateStringValue(code));
        Erase(code);
        SetDlgItemTextW(state.hwnd, kCode, L"");
    } else {
        auto email = Text(state.hwnd, kEmail);
        if (email.empty() || password.empty()) { Erase(password); return; }
        request.Insert(L"email", Value::CreateStringValue(email));
        request.Insert(L"password", Value::CreateStringValue(password));
        SetDlgItemTextW(state.hwnd, kPassword, L"");
    }
    Erase(password);
    {
        std::lock_guard lock(state.mutex);
        Erase(state.command);
        state.command = Utf8(json::Stringify(request));
        state.endpoint = verify ? L"/api/v1/drm/challenge" : L"/api/v1/drm/authenticate";
        state.pending = true;
        state.status = L"Waiting for Apple Account authorization...";
    }
    state.wake.notify_one();
    PostMessageW(state.hwnd, kResult, 0, 0);
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
    auto* state = State(hwnd);
    if (message == WM_COMMAND && state) {
        if (LOWORD(wp) == kLogin) Submit(*state, false);
        else if (LOWORD(wp) == kVerify) Submit(*state, true);
        else if (LOWORD(wp) == IDCANCEL) DestroyWindow(hwnd);
        return 0;
    }
    if (message == kResult && state) {
        std::wstring status;
        bool verified;
        {
            std::lock_guard lock(state->mutex);
            status = state->status;
            verified = state->verified;
        }
        SetDlgItemTextW(hwnd, kStatus, ui::Tr(status.c_str()).c_str());
        if (verified && state->changed) {
            auto changed = std::move(state->changed);
            changed();
        }
        return 0;
    }
    if (message == WM_DESTROY && state) {
        state->Stop();
        DeleteObject(state->font);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        g_window = nullptr;
        delete state;
        return 0;
    }
    return DefWindowProcW(hwnd, message, wp, lp);
}

class LoginLifecycle : public initquit {
public:
    void on_init() override {}
    void on_quit() override { CloseLogin(); }
};
FB2K_SERVICE_FACTORY(LoginLifecycle);

class LoginCommand : public mainmenu_commands {
public:
    t_uint32 get_command_count() override { return 1; }
    GUID get_command(t_uint32) override {
        return {0xd69db3d7, 0x6e55, 0x42e9, {0xa6, 0x05, 0xaa, 0x8b, 0xd9, 0xdb, 0x47, 0x32}};
    }
    void get_name(t_uint32, pfc::string_base& out) override { out = "Sign in to Apple Music"; }
    bool get_description(t_uint32, pfc::string_base& out) override {
        out = "Authorize the local Apple Music helper with your Apple Account.";
        return true;
    }
    GUID get_parent() override { return mainmenu_groups::file; }
    void execute(t_uint32, ctx_t) override {
        if (auto* provider = stream::FindProvider(L"apple")) provider->SignIn(core_api::get_main_window());
    }
};
FB2K_SERVICE_FACTORY(LoginCommand);

}  // namespace

void ShowLogin(HWND owner, std::function<void()> onChanged) {
    if (g_window) { SetForegroundWindow(g_window); return; }
    WNDCLASSW wc{};
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = core_api::get_my_instance();
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    wc.lpszClassName = kClass;
    static const ATOM registered = RegisterClassW(&wc); // once; the class outlives the dialog
    (void)registered;
    RECT rect{0, 0, 460, 330};
    const UINT dpi = owner ? GetDpiForWindow(owner) : 96;
    rect.right = MulDiv(rect.right, static_cast<int>(dpi), 96);
    rect.bottom = MulDiv(rect.bottom, static_cast<int>(dpi), 96);
    AdjustWindowRectExForDpi(&rect, WS_CAPTION | WS_SYSMENU, FALSE, WS_EX_CONTROLPARENT, dpi);
    g_window = CreateWindowExW(WS_EX_CONTROLPARENT, kClass, ui::Tr(L"Sign in to Apple Music").c_str(),
                                 WS_CAPTION | WS_SYSMENU, CW_USEDEFAULT, CW_USEDEFAULT,
                                 rect.right - rect.left, rect.bottom - rect.top, owner, nullptr, wc.hInstance, nullptr);
    if (!g_window) return;
    auto* state = new LoginState;
    state->hwnd = g_window;
    state->changed = std::move(onChanged);
    state->font = CreateFontW(-MulDiv(9, static_cast<int>(dpi), 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                DEFAULT_PITCH, L"Segoe UI");
    SetWindowLongPtrW(g_window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    Control(*state, L"STATIC", L"Apple Account", 0, 0, 16, 16, 428, 20);
    Control(*state, L"EDIT", L"", kEmail, WS_TABSTOP | ES_AUTOHSCROLL, 16, 40, 428, 24);
    Control(*state, L"STATIC", L"Password", 0, 0, 16, 76, 428, 20);
    Control(*state, L"EDIT", L"", kPassword, WS_TABSTOP | ES_AUTOHSCROLL | ES_PASSWORD, 16, 100, 310, 24);
    Control(*state, L"BUTTON", L"Sign in", kLogin, WS_TABSTOP, 338, 100, 106, 24);
    Control(*state, L"STATIC", L"Verification code", 0, 0, 16, 136, 428, 20);
    Control(*state, L"EDIT", L"", kCode, WS_TABSTOP | ES_AUTOHSCROLL, 16, 160, 310, 24);
    Control(*state, L"BUTTON", L"Verify", kVerify, WS_TABSTOP, 338, 160, 106, 24);
    Control(*state, L"STATIC", state->status.c_str(), kStatus, 0, 16, 204, 428, 64);
    Control(*state, L"BUTTON", L"Close", IDCANCEL, WS_TABSTOP, 338, 286, 106, 28);
    state->worker = std::thread([state] {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        state->Run();
        winrt::uninit_apartment();
    });
    ShowWindow(g_window, SW_SHOW);
    SetFocus(GetDlgItem(g_window, kEmail));
}

void CloseLogin() { if (g_window) DestroyWindow(g_window); }

}  // namespace glasswave::applemusic
