#include "pch.h"
#include "media/playback_scheduler.h"
#include "ui/i18n.h"
#include "ui/main_menu.h"
#include "ui/main_window.h"
#include "ui/win7_menu.h"

#include <cwctype>
#include <map>
#include <set>
#include <string_view>

namespace glasswave::ui {

namespace {

using CommandActions = std::map<UINT, std::function<void()>>;

enum : UINT {
    kOrderDefault = 0x8000,
    kOrderShuffleTracks,
    kOrderRepeatOff,
    kOrderRepeatPlaylist,
    kOrderRepeatTrack,
    kOrderRandom,
};

std::wstring MenuLabel(HMENU menu, UINT position) {
    const int length = GetMenuStringW(menu, position, nullptr, 0, MF_BYPOSITION);
    if (length <= 0) return {};
    std::wstring label(static_cast<size_t>(length) + 1, L'\0');
    const int copied =
        GetMenuStringW(menu, position, label.data(), static_cast<int>(label.size()), MF_BYPOSITION);
    label.resize(static_cast<size_t>(std::max(copied, 0)));
    return label;
}

std::wstring NormalizeMenuLabel(std::wstring text) {
    text = text.substr(0, text.find(L'\t'));
    std::wstring out;
    out.reserve(text.size());
    bool space = false;
    for (wchar_t c : text) {
        if (c == L'&') continue;
        if (std::iswspace(static_cast<wint_t>(c))) {
            space = !out.empty();
            continue;
        }
        if (space) out.push_back(L' ');
        space = false;
        out.push_back(static_cast<wchar_t>(std::towlower(static_cast<wint_t>(c))));
    }
    return out;
}

bool LabelIs(std::wstring_view normalized, const wchar_t* key) {
    return normalized == NormalizeMenuLabel(key) || normalized == NormalizeMenuLabel(Tr(key));
}

bool LooksLikePlaybackOrder(HMENU menu, std::wstring_view label) {
    if (LabelIs(label, L"Order")) return true;
    auto pm = playlist_manager::get();
    std::set<std::wstring> orders;
    for (t_size i = 0; i < pm->playback_order_get_count(); ++i)
        if (const char* name = pm->playback_order_get_name(i))
            orders.insert(NormalizeMenuLabel(pfc::stringcvt::string_wide_from_utf8(name).get_ptr()));
    size_t matches = 0;
    const int count = GetMenuItemCount(menu);
    for (int i = 0; i < count; ++i) {
        if (orders.count(NormalizeMenuLabel(MenuLabel(menu, static_cast<UINT>(i))))) ++matches;
    }
    return matches >= 2;
}

void SetPlaybackMode(bool shuffle, int repeat) {
    if (auto* window = MainWindow::Instance()) window->SelectPlaybackMode(shuffle, repeat);
}

int CurrentRepeat() {
    if (auto* scheduler = media::PlaybackScheduler::Existing()) return scheduler->QueueRepeatMode();
    return 0;
}

bool CurrentShuffle() {
    if (auto* scheduler = media::PlaybackScheduler::Existing()) return scheduler->QueueShuffleEnabled();
    return false;
}

HMENU BuildOrderMenu(CommandActions& commands) {
    const bool shuffle = CurrentShuffle();
    const int repeat = CurrentRepeat();
    HMENU order = CreatePopupMenu();
    AppendMenuW(order, MF_STRING, kOrderDefault, Tr(L"Default").c_str());
    AppendMenuW(order, MF_STRING, kOrderShuffleTracks, Tr(L"Shuffle (tracks)").c_str());
    AppendMenuW(order, MF_SEPARATOR, 0, nullptr);
    HMENU repeatMenu = CreatePopupMenu();
    AppendMenuW(repeatMenu, MF_STRING, kOrderRepeatOff, Tr(L"Off").c_str());
    AppendMenuW(repeatMenu, MF_STRING, kOrderRepeatPlaylist, Tr(L"Repeat (playlist)").c_str());
    AppendMenuW(repeatMenu, MF_STRING, kOrderRepeatTrack, Tr(L"Repeat (track)").c_str());
    CheckMenuRadioItem(repeatMenu, kOrderRepeatOff, kOrderRepeatTrack,
                       repeat == 1   ? kOrderRepeatPlaylist
                       : repeat == 2 ? kOrderRepeatTrack
                                     : kOrderRepeatOff,
                       MF_BYCOMMAND);
    AppendMenuW(order, MF_POPUP | MF_STRING, reinterpret_cast<UINT_PTR>(repeatMenu), Tr(L"Repeat").c_str());

    if (!shuffle && repeat == 0) CheckMenuItem(order, kOrderDefault, MF_BYCOMMAND | MF_CHECKED);
    if (shuffle) CheckMenuItem(order, kOrderShuffleTracks, MF_BYCOMMAND | MF_CHECKED);

    commands[kOrderDefault] = [] { SetPlaybackMode(false, 0); };
    commands[kOrderShuffleTracks] = [] { SetPlaybackMode(!CurrentShuffle(), CurrentRepeat()); };
    commands[kOrderRepeatOff] = [] { SetPlaybackMode(CurrentShuffle(), 0); };
    commands[kOrderRepeatPlaylist] = [] { SetPlaybackMode(CurrentShuffle(), 1); };
    commands[kOrderRepeatTrack] = [] { SetPlaybackMode(CurrentShuffle(), 2); };
    return order;
}

void EnhancePlaybackMenu(HMENU menu, CommandActions& commands) {
    int orderPosition = -1, randomPosition = -1;
    std::wstring orderLabel, randomLabel;
    for (int i = 0; i < GetMenuItemCount(menu); ++i) {
        MENUITEMINFOW info{sizeof(info)};
        info.fMask = MIIM_FTYPE | MIIM_SUBMENU;
        if (!GetMenuItemInfoW(menu, static_cast<UINT>(i), TRUE, &info) || (info.fType & MFT_SEPARATOR))
            continue;
        const std::wstring label = MenuLabel(menu, static_cast<UINT>(i));
        const std::wstring normalized = NormalizeMenuLabel(label);
        if (info.hSubMenu && orderPosition < 0 && LooksLikePlaybackOrder(info.hSubMenu, normalized)) {
            orderPosition = i;
            orderLabel = label;
        }
        if (!info.hSubMenu && randomPosition < 0 && LabelIs(normalized, L"Random")) {
            randomPosition = i;
            randomLabel = label;
        }
    }

    const int insertAt = orderPosition >= 0    ? orderPosition
                         : randomPosition >= 0 ? randomPosition
                                               : GetMenuItemCount(menu);
    HMENU order = BuildOrderMenu(commands);
    if (orderPosition >= 0) {
        InsertMenuW(menu, static_cast<UINT>(insertAt), MF_BYPOSITION | MF_POPUP | MF_STRING,
                    reinterpret_cast<UINT_PTR>(order), orderLabel.c_str());
        DeleteMenu(menu, static_cast<UINT>(insertAt + 1), MF_BYPOSITION);
    } else {
        AppendMenuW(menu, MF_POPUP | MF_STRING, reinterpret_cast<UINT_PTR>(order), Tr(L"Order").c_str());
    }

    if (randomPosition >= 0) {
        ModifyMenuW(menu, static_cast<UINT>(randomPosition), MF_BYPOSITION | MF_STRING, kOrderRandom,
                    randomLabel.c_str());
    } else {
        AppendMenuW(menu, MF_STRING, kOrderRandom, Tr(L"Random").c_str());
    }
    commands[kOrderRandom] = [] {
        SetPlaybackMode(true, CurrentRepeat());
        playback_control::get()->next();
    };
}

void SnapshotNativeCommands(HMENU menu, const service_ptr_t<mainmenu_manager>& manager, UINT base, UINT count,
                            CommandActions& commands) {
    for (int i = 0; i < GetMenuItemCount(menu); ++i) {
        MENUITEMINFOW info{sizeof(info)};
        info.fMask = MIIM_ID | MIIM_FTYPE | MIIM_SUBMENU;
        if (!GetMenuItemInfoW(menu, static_cast<UINT>(i), TRUE, &info) || (info.fType & MFT_SEPARATOR))
            continue;
        if (info.hSubMenu) {
            SnapshotNativeCommands(info.hSubMenu, manager, base, count, commands);
        } else if (info.wID >= base && info.wID - base < count) {
            const UINT relative = info.wID - base;
            commands[info.wID] = [manager, relative] { manager->execute_command(relative); };
        }
    }
}

// Appends `actions` (submenus included) to `menu`; ids are base + index into `flat`.
void Append(HMENU menu, const std::vector<MenuAction>& actions, UINT base,
            std::vector<const MenuAction*>& flat) {
    for (const auto& a : actions) {
        if (a.label.empty()) {
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        } else if (!a.children.empty()) {
            HMENU sub = CreatePopupMenu();
            Append(sub, a.children, base, flat);
            AppendMenuW(menu, MF_POPUP | MF_STRING | (a.disabled ? MF_GRAYED : 0u), reinterpret_cast<UINT_PTR>(sub),
                        a.label.c_str());
        } else {
            flat.push_back(&a);
            const UINT id = base + static_cast<UINT>(flat.size() - 1);
            AppendMenuW(menu, MF_STRING | (a.disabled ? MF_GRAYED : 0u), id, a.label.c_str());
            if (a.checked && a.checkbox)
                CheckMenuItem(menu, id, MF_BYCOMMAND | MF_CHECKED);
            else if (a.checked)
                CheckMenuRadioItem(menu, id, id, id, MF_BYCOMMAND);
        }
    }
}

} // namespace

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
    std::vector<const MenuAction*> flat;
    CommandActions commands;
    Append(menu, actions, kActionBase, flat);
    for (size_t i = 0; i < flat.size(); ++i)
        if (flat[i]->run) commands[kActionBase + static_cast<UINT>(i)] = flat[i]->run;
    if (!actions.empty()) AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    uint32_t base = 1;
    for (const auto& root : kRoots) {
        auto mgr = standard_api_create_t<mainmenu_manager>();
        mgr->instantiate(*root.guid);
        HMENU sub = CreatePopupMenu();
        mgr->generate_menu_win32(sub, base, kIdsPerRoot, mainmenu_manager::flag_show_shortcuts);
        if (&mainmenu_groups::playback == root.guid) EnhancePlaybackMenu(sub, commands);
        SnapshotNativeCommands(sub, mgr, base, kIdsPerRoot, commands);
        AppendMenuW(menu, MF_POPUP | MF_STRING, reinterpret_cast<UINT_PTR>(sub), root.label);
        base += kIdsPerRoot;
    }

    SetForegroundWindow(owner);
    const UINT cmd = TrackWin7Menu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, pt.x, pt.y, owner);
    DestroyMenu(menu); // also destroys the submenus

    const auto command = commands.find(cmd);
    if (command != commands.end() && command->second) command->second();
}

void ShowActionMenu(HWND owner, POINT pt, const std::vector<MenuAction>& actions, size_t checked) {
    if (actions.empty()) return;
    std::vector<MenuAction> copy = actions;
    if (checked < copy.size()) copy[checked].checked = true;
    HMENU menu = CreatePopupMenu();
    std::vector<const MenuAction*> flat;
    Append(menu, copy, 1, flat);
    CommandActions commands;
    for (size_t i = 0; i < flat.size(); ++i)
        if (flat[i]->run) commands[1 + static_cast<UINT>(i)] = flat[i]->run;
    const UINT cmd =
        TrackWin7Menu(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_LEFTALIGN | TPM_TOPALIGN, pt.x, pt.y, owner);
    DestroyMenu(menu);
    const auto command = commands.find(cmd);
    if (command != commands.end() && command->second) command->second();
}

} // namespace glasswave::ui
