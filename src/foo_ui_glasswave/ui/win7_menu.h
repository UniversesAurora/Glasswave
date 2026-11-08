#pragma once

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
UINT TrackWin7Menu(HMENU menu, UINT flags, int x, int y, HWND owner);

// foobar2000's context menu for `items` at screen point `pt`, as a Win7 menu.
void RunContextMenu(HWND owner, metadb_handle_list_cref items, POINT pt);
// foobar2000's context menu for the active playlist's selection (playlist commands included).
void RunPlaylistContextMenu(HWND owner, POINT pt);

}  // namespace glasswave::ui
