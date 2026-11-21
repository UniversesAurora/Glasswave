#include "pch.h"
#include "ui/lyrics_tools.h"
#include "core/text.h"

#include "lyrics/lyrics_service.h"
#include "stream/provider.h"
#include "ui/i18n.h"

#include <commdlg.h>
#include <filesystem>
#include <fstream>

namespace glasswave::ui::lyrics_tools {

namespace {

using core::Utf8;

std::wstring SafeName(std::wstring name) {
    for (auto& c : name)
        if (wcschr(L"\\/:*?\"<>|", c) || c < 32) c = L'_';
    while (!name.empty() && (name.back() == L'.' || name.back() == L' ')) name.pop_back();
    return name.empty() ? L"Lyrics" : name;
}

// "Artist - Title.lrc", as the profile folder and the save dialog name it.
std::wstring NamedFile(const metadb_handle_ptr& track) {
    const auto q = lyrics::Service::QueryFor(track);
    return SafeName(q.artist.empty() ? q.title : q.artist + L" - " + q.title) + L".lrc";
}

std::wstring NativePath(const metadb_handle_ptr& track) {
    if (!track.is_valid() || stream::ProviderOf(track)) return {};
    return lyrics::Service::QueryFor(track).nativePath;
}

// Next to the track: the same-name .lrc the lyrics are looked for in; a cue sheet's song by
// its artist and title.
std::wstring TrackFolderFile(const metadb_handle_ptr& track) {
    const auto native = NativePath(track);
    if (native.empty()) return {};
    std::filesystem::path path(native);
    if (track->get_subsong_index() != 0) return (path.parent_path() / NamedFile(track)).wstring();
    return path.replace_extension(L".lrc").wstring();
}

bool WriteUtf8(const std::wstring& path, const std::wstring& text) {
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
    std::ofstream out(std::filesystem::path(path), std::ios::binary | std::ios::trunc);
    if (!out) return false;
    const std::string bytes = "\xEF\xBB\xBF" + Utf8(text);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(out);
}

std::wstring PickSaveFile(HWND owner, const metadb_handle_ptr& track) {
    wchar_t file[MAX_PATH]{};
    wcsncpy_s(file, NamedFile(track).c_str(), _TRUNCATE);
    std::wstring folder;
    if (const auto native = NativePath(track); !native.empty())
        folder = std::filesystem::path(native).parent_path().wstring();
    const std::wstring filter = Tr(L"Lyrics") + L" (*.lrc)" + L'\0' + L"*.lrc" + L'\0' + Tr(L"Text") + L" (*.txt)" +
                                L'\0' + L"*.txt" + L'\0' + L'\0';
    OPENFILENAMEW ofn{sizeof(ofn)};
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = filter.c_str();
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrInitialDir = folder.empty() ? nullptr : folder.c_str();
    ofn.lpstrDefExt = L"lrc";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    return GetSaveFileNameW(&ofn) ? std::wstring(file) : std::wstring();
}

bool SaveTag(HWND owner, const metadb_handle_ptr& track, const std::wstring& document) {
    auto ref = track->get_info_ref();
    if (!ref.is_valid()) return false;
    file_info_impl info(ref->info());
    // The field the embedded lyrics are read from first, if the file has it; else LYRICS.
    const char* field = info.meta_get("SYNCEDLYRICS", 0) ? "SYNCEDLYRICS" : "LYRICS";
    info.meta_set(field, Utf8(document).c_str());
    const file_info* infos[] = {&info};
    metadb_io_v2::get()->update_info_async_simple(pfc::list_single_ref_t<metadb_handle_ptr>(track),
                                                  pfc::list_const_array_t<const file_info*, const file_info**>(infos, 1),
                                                  owner, metadb_io_v2::op_flag_delay_ui, nullptr);
    return true;
}

}  // namespace

bool CanSave(const metadb_handle_ptr& track, SaveTarget target) {
    if (!track.is_valid()) return false;
    switch (target) {
    case SaveTarget::Tag: return !NativePath(track).empty();
    case SaveTarget::TrackFolder: return !TrackFolderFile(track).empty();
    case SaveTarget::Profile: return !lyrics::Service::ProfileLyricsFolder().empty();
    default: return true;
    }
}

bool Save(HWND owner, const metadb_handle_ptr& track, const std::wstring& document, SaveTarget target) {
    if (!track.is_valid() || document.empty()) return false;
    if (target == SaveTarget::Default) target = CanSave(track, SaveTarget::TrackFolder) ? SaveTarget::TrackFolder : SaveTarget::Profile;
    if (target == SaveTarget::Tag) {
        if (CanSave(track, target) && SaveTag(owner, track, document)) return true;
        MessageBoxW(owner, Tr(L"The lyrics could not be written to the track's tags.").c_str(), Tr(L"Lyrics").c_str(),
                    MB_OK | MB_ICONWARNING);
        return false;
    }
    std::wstring path;
    if (target == SaveTarget::TrackFolder) path = TrackFolderFile(track);
    else if (target == SaveTarget::Profile)
        path = (std::filesystem::path(lyrics::Service::ProfileLyricsFolder()) / NamedFile(track)).wstring();
    else path = PickSaveFile(owner, track);
    if (path.empty()) return false;
    if (WriteUtf8(path, document)) {
        console::print("Glasswave lyrics: saved to ", Utf8(path).c_str());
        return true;
    }
    MessageBoxW(owner, (Tr(L"The lyrics could not be saved to") + L"\n" + path).c_str(), Tr(L"Lyrics").c_str(),
                MB_OK | MB_ICONWARNING);
    return false;
}

bool CopyText(HWND owner, const std::wstring& text) {
    if (!OpenClipboard(owner)) return false;
    EmptyClipboard();
    const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    bool done = false;
    if (memory) {
        if (void* p = GlobalLock(memory)) {
            memcpy(p, text.c_str(), bytes);
            GlobalUnlock(memory);
            done = SetClipboardData(CF_UNICODETEXT, memory) != nullptr;
        }
        if (!done) GlobalFree(memory);
    }
    CloseClipboard();
    return done;
}

std::vector<std::pair<unsigned, std::wstring>> CodePages() {
    return {{936, L"Simplified Chinese (GBK) [936]"},
            {65001, L"Unicode (UTF-8) [65001]"},
            {1200, L"Unicode (UTF-16 LE) [1200]"},
            {1201, L"Unicode (UTF-16 BE) [1201]"},
            {1252, L"Western European (Windows) [1252]"},
            {1254, L"Turkish (Windows) [1254]"},
            {54936, L"Chinese Simplified (GB18030) [54936]"},
            {932, L"ANSI/OEM Japanese (Shift-JIS) [932]"},
            {949, L"ANSI/OEM Korean (Unified Hangul Code) [949]"},
            {950, L"Traditional Chinese (Big5) [950]"}};
}

// ---- Small modal windows -----------------------------------------------------------------

namespace {

constexpr wchar_t kClass[] = L"Glasswave.LyricsTool";

enum Id : int {
    kOk = IDOK,
    kCancel = IDCANCEL,
    kCopy = 100,
    kText,
    kMetadata,
    kReferences,
    kTimestamps,
    kStandard,
    kMerge,
    kGroup,
    kEdit,
    kLabel
};

enum class Kind { Copy, Text, Offset };

struct Dialog {
    Kind kind = Kind::Copy;
    HWND hwnd = nullptr;
    HFONT font = nullptr;
    bool running = true, accepted = false;
    bool karaoke = false;
    lyrics::WriteOptions options;
    std::wstring header, text, number;
    int dpi = 96;
    int Dip(int v) const { return MulDiv(v, dpi, 96); }
};

HWND Item(const Dialog& d, int id) { return GetDlgItem(d.hwnd, id); }
bool Checked(const Dialog& d, int id) { return SendMessageW(Item(d, id), BM_GETCHECK, 0, 0) == BST_CHECKED; }

// "Copy as standard lyrics" and "merge" mean something only with timestamps; standard only
// for word-timed lyrics.
void UpdateCopyStates(const Dialog& d) {
    const bool timestamps = Checked(d, kTimestamps);
    EnableWindow(Item(d, kStandard), timestamps && d.karaoke);
    EnableWindow(Item(d, kMerge), timestamps);
}

void Layout(Dialog& d) {
    RECT rc{};
    GetClientRect(d.hwnd, &rc);
    const int w = rc.right, h = rc.bottom, m = d.Dip(14), bw = d.Dip(96), bh = d.Dip(28), gap = d.Dip(10);
    const int by = h - m - bh;
    MoveWindow(Item(d, kCancel), w - m - bw, by, bw, bh, TRUE);
    MoveWindow(Item(d, d.kind == Kind::Copy ? kCopy : d.kind == Kind::Text ? kCopy : kOk), w - m - 2 * bw - gap, by, bw,
               bh, TRUE);
    if (d.kind == Kind::Copy) {
        const int top = m, half = (w - 2 * m) / 2, row = d.Dip(30), x0 = m + d.Dip(16), y0 = top + d.Dip(24);
        MoveWindow(Item(d, kGroup), m, top, w - 2 * m, by - gap - top, TRUE);
        const int cw = half - d.Dip(24), ch = d.Dip(22);
        MoveWindow(Item(d, kMetadata), x0, y0, cw, ch, TRUE);
        MoveWindow(Item(d, kReferences), m + half + d.Dip(8), y0, cw, ch, TRUE);
        MoveWindow(Item(d, kTimestamps), x0, y0 + row, cw, ch, TRUE);
        MoveWindow(Item(d, kStandard), m + half + d.Dip(8), y0 + row, cw, ch, TRUE);
        MoveWindow(Item(d, kMerge), x0, y0 + 2 * row, cw, ch, TRUE);
    } else if (d.kind == Kind::Text) {
        const int headerH = d.header.empty() ? 0 : d.Dip(18) * static_cast<int>(1 + std::count(d.header.begin(), d.header.end(), L'\n'));
        MoveWindow(Item(d, kLabel), m, m, w - 2 * m, headerH, TRUE);
        MoveWindow(Item(d, kEdit), m, m + headerH + (headerH ? d.Dip(6) : 0), w - 2 * m,
                   by - gap - m - headerH - (headerH ? d.Dip(6) : 0), TRUE);
    } else {
        MoveWindow(Item(d, kLabel), m, m, w - 2 * m, d.Dip(40), TRUE);
        MoveWindow(Item(d, kEdit), m, m + d.Dip(44), w - 2 * m, d.Dip(26), TRUE);
    }
}

std::wstring TextOf(HWND control) {
    std::wstring s(static_cast<size_t>(GetWindowTextLengthW(control)) + 1, L'\0');
    s.resize(static_cast<size_t>(GetWindowTextW(control, s.data(), static_cast<int>(s.size()))));
    return s;
}

LRESULT CALLBACK Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* d = reinterpret_cast<Dialog*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
    case WM_SIZE:
        if (d) Layout(*d);
        return 0;
    case WM_GETMINMAXINFO:
        if (d && d->kind == Kind::Text) {
            auto* mm = reinterpret_cast<MINMAXINFO*>(lp);
            mm->ptMinTrackSize = {d->Dip(360), d->Dip(240)};
        }
        return 0;
    case WM_CTLCOLORSTATIC:
        // Labels and check boxes on the window's white; the read-only text stays white too.
        SetBkColor(reinterpret_cast<HDC>(wp), GetSysColor(COLOR_WINDOW));
        return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
    case WM_COMMAND:
        if (!d) return 0;
        switch (LOWORD(wp)) {
        case kTimestamps: UpdateCopyStates(*d); break;
        case kCopy:
            if (d->kind == Kind::Text) {
                CopyText(hwnd, d->text);
                break;
            }
            [[fallthrough]];
        case kOk:
            if (d->kind == Kind::Copy) {
                d->options.metadata = Checked(*d, kMetadata);
                d->options.references = Checked(*d, kReferences);
                d->options.timestamps = Checked(*d, kTimestamps);
                d->options.standard = Checked(*d, kStandard);
                d->options.mergeSame = Checked(*d, kMerge);
            } else if (d->kind == Kind::Offset) {
                d->number = TextOf(Item(*d, kEdit));
            }
            d->accepted = d->kind != Kind::Text;
            d->running = false;
            break;
        case kCancel: d->running = false; break;
        default: break;
        }
        return 0;
    case WM_CLOSE:
        if (d) d->running = false;
        return 0;
    default:
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

void Register() {
    static std::once_flag once;
    std::call_once(once, [] {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = Proc;
        wc.hInstance = core_api::get_my_instance();
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
        wc.lpszClassName = kClass;
        RegisterClassExW(&wc);
    });
}

HWND Control(Dialog& d, const wchar_t* cls, const std::wstring& text, DWORD style, int id, DWORD exStyle = 0) {
    HWND h = CreateWindowExW(exStyle, cls, text.c_str(), WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, d.hwnd,
                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), core_api::get_my_instance(), nullptr);
    if (h && d.font) SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(d.font), TRUE);
    return h;
}

// Runs `d` modally over `owner` until it is closed.
void Run(Dialog& d, HWND owner, const std::wstring& title, int wDip, int hDip, bool resizable) {
    Register();
    d.dpi = owner ? static_cast<int>(GetDpiForWindow(owner)) : 96;
    if (d.dpi <= 0) d.dpi = 96;
    RECT ownerRect{0, 0, 800, 600};
    if (owner) GetWindowRect(owner, &ownerRect);
    DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN;
    if (resizable) style |= WS_THICKFRAME | WS_MAXIMIZEBOX;
    RECT r{0, 0, d.Dip(wDip), d.Dip(hDip)};
    AdjustWindowRectExForDpi(&r, style, FALSE, WS_EX_DLGMODALFRAME, static_cast<UINT>(d.dpi));
    const int w = r.right - r.left, h = r.bottom - r.top;
    const int x = ownerRect.left + (ownerRect.right - ownerRect.left - w) / 2;
    const int y = ownerRect.top + (ownerRect.bottom - ownerRect.top - h) / 2;
    d.hwnd = CreateWindowExW(WS_EX_DLGMODALFRAME, kClass, title.c_str(), style, x, y, w, h, owner, nullptr,
                             core_api::get_my_instance(), nullptr);
    if (!d.hwnd) return;
    SetWindowLongPtrW(d.hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&d));
    NONCLIENTMETRICSW ncm{sizeof(ncm)};
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, static_cast<UINT>(d.dpi));
    d.font = CreateFontIndirectW(&ncm.lfMessageFont);

    const DWORD tab = WS_TABSTOP;
    HWND first = nullptr;
    if (d.kind == Kind::Copy) {
        Control(d, L"BUTTON", Tr(L"Copy options"), BS_GROUPBOX, kGroup);
        const auto check = [&](int id, const wchar_t* label, bool on) {
            HWND h = Control(d, L"BUTTON", Tr(label), BS_AUTOCHECKBOX | tab, id);
            SendMessageW(h, BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0);
            if (!first) first = h;
        };
        check(kMetadata, L"Include metadata", d.options.metadata);
        check(kReferences, L"Include reference lines", d.options.references);
        check(kTimestamps, L"Include timestamps", d.options.timestamps);
        check(kStandard, L"Copy as standard lyrics", d.options.standard);
        check(kMerge, L"Merge timestamps of identical lines", d.options.mergeSame);
        Control(d, L"BUTTON", Tr(L"Copy"), BS_DEFPUSHBUTTON | tab, kCopy);
        UpdateCopyStates(d);
    } else if (d.kind == Kind::Text) {
        Control(d, L"STATIC", d.header, SS_LEFT | SS_NOPREFIX, kLabel);
        std::wstring text;  // the edit control wants CR LF
        for (size_t i = 0; i < d.text.size(); ++i) {
            if (d.text[i] == L'\n' && (i == 0 || d.text[i - 1] != L'\r')) text += L'\r';
            text += d.text[i];
        }
        first = Control(d, L"EDIT", text, ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL | WS_HSCROLL |
                                              ES_AUTOHSCROLL | tab,
                        kEdit, WS_EX_CLIENTEDGE);
        Control(d, L"BUTTON", Tr(L"Copy"), BS_PUSHBUTTON | tab, kCopy);
    } else {
        Control(d, L"STATIC", d.header, SS_LEFT | SS_NOPREFIX, kLabel);
        first = Control(d, L"EDIT", d.number, ES_AUTOHSCROLL | tab, kEdit, WS_EX_CLIENTEDGE);
        Control(d, L"BUTTON", Tr(L"OK"), BS_DEFPUSHBUTTON | tab, kOk);
        SendMessageW(first, EM_SETSEL, 0, -1);
    }
    Control(d, L"BUTTON", Tr(d.kind == Kind::Text ? L"Close" : L"Cancel"), BS_PUSHBUTTON | tab, kCancel);
    Layout(d);

    if (owner) EnableWindow(owner, FALSE);
    ShowWindow(d.hwnd, SW_SHOW);
    if (first) SetFocus(first);
    MSG msg{};
    bool quit = false;
    while (d.running) {
        const BOOL got = GetMessageW(&msg, nullptr, 0, 0);
        if (got <= 0) {
            quit = got == 0;
            break;
        }
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE && (msg.hwnd == d.hwnd || IsChild(d.hwnd, msg.hwnd))) {
            d.running = false;
            continue;
        }
        if (!IsDialogMessageW(d.hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    if (owner) EnableWindow(owner, TRUE);
    SetWindowLongPtrW(d.hwnd, GWLP_USERDATA, 0);
    DestroyWindow(d.hwnd);
    if (d.font) DeleteObject(d.font);
    if (owner) SetActiveWindow(owner);
    if (quit) PostQuitMessage(static_cast<int>(msg.wParam));
}

}  // namespace

bool ShowCopyDialog(HWND owner, lyrics::WriteOptions& options, bool karaoke) {
    Dialog d;
    d.kind = Kind::Copy;
    d.options = options;
    d.karaoke = karaoke;
    Run(d, owner, Tr(L"Copy lyrics"), 460, 190, false);
    if (d.accepted) options = d.options;
    return d.accepted;
}

void ShowTextWindow(HWND owner, const std::wstring& title, const std::wstring& header, const std::wstring& text) {
    Dialog d;
    d.kind = Kind::Text;
    d.header = header;
    d.text = text;
    Run(d, owner, title, 560, 520, true);
}

bool AskOffset(HWND owner, int& ms) {
    Dialog d;
    d.kind = Kind::Offset;
    d.header = Tr(L"Offset in milliseconds: positive shows the lyrics earlier, negative later.");
    d.number = std::to_wstring(ms);
    Run(d, owner, Tr(L"Adjust offset"), 380, 140, false);
    if (!d.accepted) return false;
    std::wstring s;
    for (const wchar_t c : d.number)
        if (!iswspace(c) && c != L'+') s += c;
    if (s.empty()) return false;
    wchar_t* end = nullptr;
    const long value = wcstol(s.c_str(), &end, 10);
    if (!end || *end != L'\0') return false;
    ms = static_cast<int>(std::clamp<long>(value, -600000, 600000));
    return true;
}

}  // namespace glasswave::ui::lyrics_tools
