#include "pch.h"
#include "ui/main_menu.h"
#include "ui/win7_menu.h"

namespace glasswave::ui {

void ShowMainMenu(HWND owner, POINT pt, const std::vector<MenuAction>& actions) {
    struct Root {
        const GUID* guid;
        const wchar_t* label;
    };
    static constexpr Root kRoots[] = {
        {&mainmenu_groups::file, L"&File"},       {&mainmenu_groups::edit, L"&Edit"},
        {&mainmenu_groups::view, L"&View"},       {&mainmenu_groups::playback, L"&Playback"},
        {&mainmenu_groups::library, L"&Library"}, {&mainmenu_groups::help, L"&Help"},
    };
    constexpr uint32_t kIdsPerRoot = 0x1000;
    constexpr uint32_t kActionBase = 0x10000;

    HMENU menu = CreatePopupMenu();
    for (size_t i = 0; i < actions.size(); ++i) {
        if (actions[i].label.empty())
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        else
            AppendMenuW(menu, MF_STRING, kActionBase + i, actions[i].label.c_str());
    }
    if (!actions.empty()) AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    std::vector<service_ptr_t<mainmenu_manager>> managers;
    uint32_t base = 1;
    for (const auto& root : kRoots) {
        auto mgr = standard_api_create_t<mainmenu_manager>();
        mgr->instantiate(*root.guid);
        HMENU sub = CreatePopupMenu();
        mgr->generate_menu_win32(sub, base, kIdsPerRoot, mainmenu_manager::flag_show_shortcuts);
        AppendMenuW(menu, MF_POPUP | MF_STRING, reinterpret_cast<UINT_PTR>(sub), root.label);
        managers.push_back(mgr);
        base += kIdsPerRoot;
    }

    SetForegroundWindow(owner);
    const UINT cmd = TrackWin7Menu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, pt.x, pt.y, owner);
    DestroyMenu(menu);  // also destroys the submenus

    if (cmd >= kActionBase) {
        const size_t i = cmd - kActionBase;
        if (i < actions.size() && actions[i].run) actions[i].run();
    } else if (cmd >= 1) {
        const uint32_t index = (cmd - 1) / kIdsPerRoot;
        if (index < managers.size()) managers[index]->execute_command(cmd - (1 + index * kIdsPerRoot));
    }
}

void ShowActionMenu(HWND owner, POINT pt, const std::vector<MenuAction>& actions, size_t checked) {
    if (actions.empty()) return;
    HMENU menu = CreatePopupMenu();
    for (size_t i = 0; i < actions.size(); ++i) {
        if (actions[i].label.empty())
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        else
            AppendMenuW(menu, MF_STRING | (i == checked ? MF_CHECKED : 0), 1 + i, actions[i].label.c_str());
    }
    const UINT cmd = TrackWin7Menu(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_LEFTALIGN | TPM_TOPALIGN, pt.x, pt.y, owner);
    DestroyMenu(menu);
    if (cmd >= 1 && cmd <= actions.size() && actions[cmd - 1].run) actions[cmd - 1].run();
}

}  // namespace glasswave::ui
