#pragma once
#include "ui/main_menu.h"

#include <functional>
#include <string>
#include <vector>

namespace glasswave::ui {

// Popup menus in Windows 7 style, drawn with aero.msstyles' own Menu parts (background
// 974, border 975, check / bullet 976, check background 978, gutter 979, item 980,
// separator 981, submenu arrow 982; docs/design/win7-asset-map.md).
//
// The menu and its submenus become owner-drawn for the call; the owner window is
// subclassed only while the menu is up. A thread-local hook catches our own popup
// windows to drop Windows 11's rounded corners and paint the Win7 border. Without the
// user's aero.msstyles the menu is shown unchanged.
//
// Same as TrackPopupMenuEx; pass TPM_RETURNCMD to get the chosen command.
// icons (optional) maps command ids to slots of wmploc's 16 px icon strip 331, drawn in
// place of a check mark; a default item (SetMenuDefaultItem) is drawn bold. exclude (optional,
// screen px) is an area the menu must not cover (TPMPARAMS::rcExclude).
UINT TrackWin7Menu(HMENU menu, UINT flags, int x, int y, HWND owner, const std::map<UINT, int>* icons = nullptr,
                   const RECT* exclude = nullptr);

// foobar2000's context menu for `items` at screen point `pt`, as a Win7 menu. One song also
// offers "Save album art...". nowPlaying: the playing song's own menu (on its title and cover),
// without the play and queue commands that make no sense for it.
void RunContextMenu(HWND owner, metadb_handle_list_cref items, POINT pt, bool nowPlaying = false,
                    std::vector<MenuAction> extra = {});
// foobar2000's context menu for the active playlist's selection (playlist commands included).
void RunPlaylistContextMenu(HWND owner, POINT pt);

// Extra context-menu commands supplied by the main window for the selected items.
void SetContextActionProvider(std::function<std::vector<MenuAction>(metadb_handle_list_cref)> provider);

}  // namespace glasswave::ui
