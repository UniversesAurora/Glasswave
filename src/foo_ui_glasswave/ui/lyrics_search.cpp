#include "pch.h"
#include "ui/lyrics_search.h"
#include "lyrics/scripts.h"
#include "ui/i18n.h"

#include <commctrl.h>
#include <shobjidl.h>

namespace glasswave::ui {
namespace {
constexpr wchar_t kClass[] = L"Glasswave.LyricsSearch";
enum {
    kTitle = 101,
    kArtist,
    kSource,
    kSearch,
    kResults,
    kPreview,
    kStatus,
    kLoad
};
HWND g_window = nullptr;

struct State {
    HWND window = nullptr;
    HFONT font = nullptr;
    lyrics::Query query;
    std::vector<std::wstring> sourceKeys;
    std::vector<lyrics::Candidate> candidates;
    std::function<void(const lyrics::Candidate&)> selected;
    bool registered = false;
};

std::shared_ptr<State> StateOf(HWND window) {
    const auto* holder = reinterpret_cast<std::shared_ptr<State>*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    return holder ? *holder : nullptr;
}

int Dip(HWND window, int value) {
    return MulDiv(value, static_cast<int>(GetDpiForWindow(window)), 96);
}
std::wstring Text(HWND window, int id) {
    HWND edit = GetDlgItem(window, id);
    std::wstring text(static_cast<size_t>(GetWindowTextLengthW(edit)) + 1, 0);
    text.resize(static_cast<size_t>(GetWindowTextW(edit, text.data(), static_cast<int>(text.size()))));
    return text;
}

int Selection(const State& state) {
    return ListView_GetNextItem(GetDlgItem(state.window, kResults), -1, LVNI_SELECTED);
}

void Preview(const State& state) {
    const int index = Selection(state);
    const bool valid = index >= 0 && static_cast<size_t>(index) < state.candidates.size();
    std::wstring text;
    if (valid) {
        for (wchar_t ch : state.candidates[index].lrc) {
            if (ch == L'\r') continue;
            if (ch == L'\n') text += L'\r';
            text += ch;
        }
    }
    SetWindowTextW(GetDlgItem(state.window, kPreview), text.c_str());
    EnableWindow(GetDlgItem(state.window, kLoad), valid);
}

void Layout(HWND window) {
    RECT rect{};
    GetClientRect(window, &rect);
    const int margin = Dip(window, 16), gap = Dip(window, 8), h = Dip(window, 24);
    const int width = rect.right - margin * 2, button = Dip(window, 92), half = (width - gap) / 2;
    const auto place = [&](int id, int x, int y, int w, int height) {
        MoveWindow(GetDlgItem(window, id), x, y, std::max(w, 1), std::max(height, 1), TRUE);
    };
    place(201, margin, margin, half, h);
    place(202, margin + half + gap, margin, half, h);
    place(kTitle, margin, margin + h, half, h);
    place(kArtist, margin + half + gap, margin + h, half, h);
    const int sourceY = margin + h * 2 + gap;
    place(kSource, margin, sourceY, width - button - gap, Dip(window, 200));
    place(kSearch, rect.right - margin - button, sourceY, button, h);
    const int listY = sourceY + h + gap, bottom = rect.bottom - margin - h;
    const int contentH = bottom - gap - listY;
    const int listH = contentH / 2;
    place(kResults, margin, listY, width, listH);
    place(kPreview, margin, listY + listH + gap, width, contentH - listH - gap);
    place(kStatus, margin, bottom, width - button * 2 - gap * 2, h);
    place(kLoad, rect.right - margin - button * 2 - gap, bottom, button, h);
    place(IDCANCEL, rect.right - margin - button, bottom, button, h);
}

void Search(const std::shared_ptr<State>& state) {
    state->query.title = Text(state->window, kTitle);
    state->query.artist = Text(state->window, kArtist);
    const auto index = static_cast<size_t>(SendDlgItemMessageW(state->window, kSource, CB_GETCURSEL, 0, 0));
    const std::wstring key = index < state->sourceKeys.size() ? state->sourceKeys[index] : L"";
    state->candidates.clear();
    ListView_DeleteAllItems(GetDlgItem(state->window, kResults));
    Preview(*state);
    SetWindowTextW(GetDlgItem(state->window, kStatus), Tr(L"Searching for lyrics...").c_str());
    lyrics::Service::Get().FindCandidates(
        state->query, key,
        [weak = std::weak_ptr<State>(state)](std::vector<lyrics::Candidate> candidates, bool finished) {
            const auto state = weak.lock();
            if (!state || !IsWindow(state->window)) return;
            const int previous = Selection(*state);
            std::wstring selectedText, selectedSource;
            if (previous >= 0 && static_cast<size_t>(previous) < state->candidates.size()) {
                selectedText = state->candidates[previous].lrc;
                selectedSource = state->candidates[previous].source;
            }
            state->candidates = std::move(candidates);
            HWND list = GetDlgItem(state->window, kResults);
            SendMessageW(list, WM_SETREDRAW, FALSE, 0);
            ListView_DeleteAllItems(list);
            int selection = 0;
            for (size_t i = 0; i < state->candidates.size(); ++i) {
                auto& candidate = state->candidates[i];
                const auto parsed = lyrics::ParseLrc(candidate.lrc);
                LVITEMW item{};
                item.mask = LVIF_TEXT;
                item.iItem = static_cast<int>(i);
                item.pszText = candidate.source.data();
                const int row = ListView_InsertItem(list, &item);
                const std::wstring title = candidate.title.empty() ? parsed.title : candidate.title;
                const std::wstring artist = candidate.artist.empty() ? parsed.artist : candidate.artist;
                const std::wstring timing = Tr(parsed.karaoke  ? L"Word timed"
                                               : parsed.synced ? L"Line timed"
                                                               : L"Plain text");
                ListView_SetItemText(list, row, 1, const_cast<wchar_t*>(title.c_str()));
                ListView_SetItemText(list, row, 2, const_cast<wchar_t*>(artist.c_str()));
                ListView_SetItemText(list, row, 3, const_cast<wchar_t*>(timing.c_str()));
                if (!selectedText.empty() && candidate.lrc == selectedText && candidate.source == selectedSource)
                    selection = row;
            }
            if (!state->candidates.empty())
                ListView_SetItemState(list, selection, LVIS_SELECTED | LVIS_FOCUSED,
                                      LVIS_SELECTED | LVIS_FOCUSED);
            SendMessageW(list, WM_SETREDRAW, TRUE, 0);
            InvalidateRect(list, nullptr, TRUE);
            const auto message =
                finished ? (state->candidates.empty()
                                ? Tr(L"No lyrics found")
                                : std::to_wstring(state->candidates.size()) + Tr(L" lyrics found"))
                         : Tr(L"Searching for lyrics...");
            SetWindowTextW(GetDlgItem(state->window, kStatus), message.c_str());
            Preview(*state);
        });
}

void Load(const std::shared_ptr<State>& state) {
    const int index = Selection(*state);
    if (index < 0 || static_cast<size_t>(index) >= state->candidates.size()) return;
    const auto candidate = state->candidates[index];
    const auto selected = state->selected;
    DestroyWindow(state->window);
    selected(candidate);
}

LRESULT CALLBACK Proc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    const auto state = StateOf(window);
    switch (message) {
    case WM_SIZE:
        if (state) Layout(window);
        return 0;
    case WM_CTLCOLORSTATIC:
        SetBkColor(reinterpret_cast<HDC>(wp), GetSysColor(COLOR_WINDOW));
        SetTextColor(reinterpret_cast<HDC>(wp), GetSysColor(COLOR_WINDOWTEXT));
        return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
    case WM_GETMINMAXINFO: {
        auto* info = reinterpret_cast<MINMAXINFO*>(lp);
        info->ptMinTrackSize = {Dip(window, 520), Dip(window, 400)};
        return 0;
    }
    case WM_COMMAND:
        if (!state) break;
        switch (LOWORD(wp)) {
        case kSearch:
            Search(state);
            break;
        case kLoad:
            Load(state);
            break;
        case IDCANCEL:
            DestroyWindow(window);
            break;
        }
        return 0;
    case WM_NOTIFY:
        if (state && reinterpret_cast<NMHDR*>(lp)->idFrom == kResults) {
            const UINT code = reinterpret_cast<NMHDR*>(lp)->code;
            if (code == LVN_ITEMCHANGED) Preview(*state);
            if (code == NM_DBLCLK) Load(state);
        }
        return 0;
    case WM_CLOSE:
        DestroyWindow(window);
        return 0;
    case WM_NCDESTROY: {
        if (state && state->registered) modeless_dialog_manager::g_remove(window);
        lyrics::Service::Get().CancelSearch();
        if (state && state->font) DeleteObject(state->font);
        delete reinterpret_cast<std::shared_ptr<State>*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        if (g_window == window) g_window = nullptr;
        break;
    }
    }
    return DefWindowProcW(window, message, wp, lp);
}

LRESULT CALLBACK EditProc(HWND window, UINT message, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    if (message == WM_KEYDOWN && (wp == VK_RETURN || wp == VK_ESCAPE)) {
        SendMessageW(GetParent(window), WM_COMMAND, wp == VK_RETURN ? kSearch : IDCANCEL, 0);
        return 0;
    }
    return DefSubclassProc(window, message, wp, lp);
}
} // namespace

void ShowLyricsSearch(HWND owner, const metadb_handle_ptr& track, std::wstring source,
                      std::function<void(const lyrics::Candidate&)> selected) {
    if (g_window) {
        SetForegroundWindow(g_window);
        return;
    }
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = Proc;
    wc.hInstance = core_api::get_my_instance();
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);
    auto state = std::make_shared<State>();
    state->query = lyrics::Service::QueryFor(track);
    state->selected = std::move(selected);
    RECT ownerRect{};
    GetWindowRect(owner, &ownerRect);
    const int width = Dip(owner, 640), height = Dip(owner, 540);
    state->window =
        CreateWindowExW(WS_EX_CONTROLPARENT, kClass, Tr(L"Search lyrics").c_str(), WS_OVERLAPPEDWINDOW,
                        ownerRect.left + (ownerRect.right - ownerRect.left - width) / 2,
                        ownerRect.top + (ownerRect.bottom - ownerRect.top - height) / 2, width, height, owner,
                        nullptr, core_api::get_my_instance(), nullptr);
    if (!state->window) return;
    g_window = state->window;
    SetWindowLongPtrW(g_window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(new std::shared_ptr<State>(state)));
    state->font = CreateFontW(-MulDiv(9, static_cast<int>(GetDpiForWindow(g_window)), 72), 0, 0, 0, FW_NORMAL,
                              FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0, 0, 0, L"Segoe UI");
    const auto make = [&](const wchar_t* type, const std::wstring& text, DWORD style, int id,
                          DWORD extended = 0) {
        HWND control = CreateWindowExW(extended, type, text.c_str(), WS_CHILD | WS_VISIBLE | style, 0, 0, 0,
                                       0, g_window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                       core_api::get_my_instance(), nullptr);
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(state->font), TRUE);
        return control;
    };
    make(L"STATIC", Tr(L"Title"), 0, 201);
    make(L"STATIC", Tr(L"Artist"), 0, 202);
    for (const auto& [id, text] : {std::pair{kTitle, state->query.title}, {kArtist, state->query.artist}}) {
        HWND edit = make(L"EDIT", text, WS_TABSTOP | ES_AUTOHSCROLL, id, WS_EX_CLIENTEDGE);
        SetWindowSubclass(edit, EditProc, 1, 0);
    }
    HWND combo = make(L"COMBOBOX", L"", WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST, kSource);
    SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(Tr(L"All lyric sources").c_str()));
    state->sourceKeys.push_back(L"");
    int selection = 0;
    for (const auto& searcher : lyrics::scripts::Ordered(state->query.provider)) {
        const int index = static_cast<int>(
            SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(searcher->Name().c_str())));
        state->sourceKeys.push_back(searcher->Key());
        if (source == searcher->Key()) selection = index;
    }
    SendMessageW(combo, CB_SETCURSEL, selection, 0);
    make(L"BUTTON", Tr(L"Search"), WS_TABSTOP | BS_DEFPUSHBUTTON, kSearch);
    HWND list = make(WC_LISTVIEWW, L"", WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS, kResults,
                     WS_EX_CLIENTEDGE);
    ListView_SetExtendedListViewStyle(list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    int columnIndex = 0;
    for (const auto* label : {L"Source", L"Title", L"Artist", L"Timing"}) {
        auto text = Tr(label);
        LVCOLUMNW column{};
        column.mask = LVCF_TEXT | LVCF_WIDTH;
        column.pszText = text.data();
        column.cx = Dip(g_window, columnIndex == 1 ? 180 : 130);
        ListView_InsertColumn(list, columnIndex++, &column);
    }
    make(L"EDIT", L"", WS_TABSTOP | ES_MULTILINE | ES_READONLY | WS_VSCROLL | ES_AUTOVSCROLL, kPreview,
         WS_EX_CLIENTEDGE);
    make(L"STATIC", L"", SS_LEFT | SS_ENDELLIPSIS, kStatus);
    make(L"BUTTON", Tr(L"Load lyrics"), WS_TABSTOP, kLoad);
    make(L"BUTTON", Tr(L"Close"), WS_TABSTOP, IDCANCEL);
    Layout(g_window);
    modeless_dialog_manager::g_add(g_window);
    state->registered = true;
    ShowWindow(g_window, SW_SHOW);
    SetForegroundWindow(g_window);
    Search(state);
}

void CloseLyricsSearch() {
    if (g_window) DestroyWindow(g_window);
}

std::wstring PickLyricsFile(HWND owner) {
    winrt::com_ptr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(dialog.put()))))
        return {};
    const std::wstring name = Tr(L"Lyrics files"), title = Tr(L"Load lyrics from file");
    const COMDLG_FILTERSPEC filters[] = {{name.c_str(), L"*.lrc;*.txt"}};
    dialog->SetFileTypes(1, filters);
    dialog->SetTitle(title.c_str());
    if (FAILED(dialog->Show(owner))) return {};
    winrt::com_ptr<IShellItem> item;
    if (FAILED(dialog->GetResult(item.put()))) return {};
    wchar_t* path = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) return {};
    std::wstring result(path);
    CoTaskMemFree(path);
    return result;
}
} // namespace glasswave::ui
