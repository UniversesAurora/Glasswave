#include "pch.h"
#include "ui/win7_menu.h"
#include "ui/i18n.h"
#include "ui/shell_glue.h"
#include "gfx/composition_host.h"
#include "win7/win7_resources.h"
#include "media/special_playlists.h"

#include <commctrl.h>
#include <gdiplus.h>
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "msimg32.lib")
#pragma comment(lib, "gdiplus.lib")

namespace glasswave::ui {

namespace {

// aero.msstyles Menu images (Win7, 96 DPI design sizes).
enum : int {
    kBackground = 974,       // 1x1 #F0F0F0
    kBorder = 975,           // 3x3 #979797
    kCheck = 976,            // 16x64: check, check disabled, bullet, bullet disabled
    kCheckBackground = 978,  // 7x21: 3 states of 7x7, sizing 3
    kGutter = 979,           // 3x1: #F1F1F1 #E2E3E3 #FFFFFF
    kItem = 980,             // 7x84: normal, hot, disabled, disabled hot (7x21), sizing 3
    kSeparator = 981,        // 1x6: rows 3 #E0E0E0, 4 #FFFFFF
    kSubmenu = 982,          // 9x18: normal, disabled (9x9)
};
constexpr COLORREF kBackColor = RGB(0xF0, 0xF0, 0xF0), kBorderColor = RGB(0x97, 0x97, 0x97);
constexpr COLORREF kText = RGB(0, 0, 0), kDisabledText = RGB(0x6D, 0x6D, 0x6D);
// Layout at 96 DPI.
constexpr int kItemHeight = 22, kSeparatorHeight = 7, kGutterX = 27, kTextLeft = 34, kArrowArea = 26,
              kShortcutGap = 24, kCheckBox = 22;

struct Dib {
    HBITMAP bmp = nullptr;
    int w = 0, h = 0;
};

// Premultiplied 32-bpp DIB of an aero (or wmploc) image, cached for the process (GDI frees it at exit).
const Dib* Image(int id, win7::Module module = win7::Module::Aero) {
    static std::map<std::pair<int, int>, Dib> cache;
    const auto key = std::make_pair(static_cast<int>(module), id);
    if (auto it = cache.find(key); it != cache.end()) return it->second.bmp ? &it->second : nullptr;
    Dib& d = cache[key];
    IWICBitmapSource* src = win7::Resources::Get().Image(module, id);
    if (!src) return nullptr;
    winrt::com_ptr<IWICFormatConverter> conv;
    auto* wic = gfx::CompositionHost::Get().Wic();
    if (FAILED(wic->CreateFormatConverter(conv.put())) ||
        FAILED(conv->Initialize(src, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                WICBitmapPaletteTypeCustom)))
        return nullptr;
    UINT w = 0, h = 0;
    conv->GetSize(&w, &h);
    BITMAPINFO bi{};
    bi.bmiHeader = {sizeof(BITMAPINFOHEADER), static_cast<LONG>(w), -static_cast<LONG>(h), 1, 32, BI_RGB};
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bmp) return nullptr;
    if (FAILED(conv->CopyPixels(nullptr, w * 4, w * h * 4, static_cast<BYTE*>(bits)))) {
        DeleteObject(bmp);
        return nullptr;
    }
    d = {bmp, static_cast<int>(w), static_cast<int>(h)};
    return &d;
}

void Blit(HDC dst, const Dib& img, RECT src, RECT to) {
    if (to.right <= to.left || to.bottom <= to.top || src.right <= src.left || src.bottom <= src.top) return;
    HDC mem = CreateCompatibleDC(dst);
    HGDIOBJ old = SelectObject(mem, img.bmp);
    const BLENDFUNCTION bf{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    AlphaBlend(dst, to.left, to.top, to.right - to.left, to.bottom - to.top, mem, src.left, src.top,
               src.right - src.left, src.bottom - src.top, bf);
    SelectObject(mem, old);
    DeleteDC(mem);
}

// Nine-grid with the borders 1:1 in device pixels, like the theme engine draws them.
void NineGrid(HDC dc, const Dib& img, RECT s, int m, RECT d) {
    const int sx[] = {s.left, s.left + m, s.right - m, s.right}, sy[] = {s.top, s.top + m, s.bottom - m, s.bottom};
    const int dx[] = {d.left, d.left + m, d.right - m, d.right}, dy[] = {d.top, d.top + m, d.bottom - m, d.bottom};
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            Blit(dc, img, {sx[c], sy[r], sx[c + 1], sy[r + 1]}, {dx[c], dy[r], dx[c + 1], dy[r + 1]});
}

struct ItemData {
    std::wstring text;
    bool separator = false, radio = false, submenu = false;
    int icon = -1;  // slot in wmploc's 16 px icon strip 331, shown instead of a check
};

struct MenuState {
    std::vector<std::unique_ptr<ItemData>> items;
    std::set<const ItemData*> mine;
    UINT dpi = 96;
    HFONT font = nullptr, bold = nullptr;  // bold: the default item (e.g. the current view)
    HBRUSH back = nullptr;
    const std::map<UINT, int>* icons = nullptr;  // command id -> icon slot
    int Px(float v) const { return static_cast<int>(std::lround(v * dpi / 96.0f)); }
};
MenuState* g_menu = nullptr;  // menus are modal: one at a time
HHOOK g_hook = nullptr;

void Convert(HMENU menu, MenuState& s) {
    MENUINFO mi{sizeof(mi)};
    mi.fMask = MIM_BACKGROUND;
    mi.hbrBack = s.back;
    SetMenuInfo(menu, &mi);
    for (int i = 0, n = GetMenuItemCount(menu); i < n; ++i) {
        wchar_t buf[512]{};
        MENUITEMINFOW ii{sizeof(ii)};
        ii.fMask = MIIM_FTYPE | MIIM_SUBMENU | MIIM_STRING | MIIM_ID;
        ii.dwTypeData = buf;
        ii.cch = static_cast<UINT>(std::size(buf) - 1);
        if (!GetMenuItemInfoW(menu, i, TRUE, &ii) || (ii.fType & MFT_OWNERDRAW)) continue;
        auto d = std::make_unique<ItemData>();
        d->separator = (ii.fType & MFT_SEPARATOR) != 0;
        d->radio = (ii.fType & MFT_RADIOCHECK) != 0;
        d->submenu = ii.hSubMenu != nullptr;
        if (s.icons)
            if (auto it = s.icons->find(ii.wID); it != s.icons->end()) d->icon = it->second;
        if (!d->separator) d->text = TrMenu(buf);  // ours and foobar2000's items alike
        MENUITEMINFOW set{sizeof(set)};
        set.fMask = MIIM_FTYPE | MIIM_DATA;
        set.fType = (ii.fType & (MFT_SEPARATOR | MFT_RADIOCHECK | MFT_MENUBARBREAK | MFT_MENUBREAK)) | MFT_OWNERDRAW;
        set.dwItemData = reinterpret_cast<ULONG_PTR>(d.get());
        SetMenuItemInfoW(menu, i, TRUE, &set);
        s.mine.insert(d.get());
        s.items.push_back(std::move(d));
        if (ii.hSubMenu) Convert(ii.hSubMenu, s);
    }
}

std::pair<std::wstring, std::wstring> Split(const std::wstring& text) {
    const size_t tab = text.find(L'\t');
    return tab == std::wstring::npos ? std::pair{text, std::wstring{}} : std::pair{text.substr(0, tab), text.substr(tab + 1)};
}

ULONG_PTR GdiplusToken() {
    static const ULONG_PTR token = [] {
        Gdiplus::GdiplusStartupInput input;
        ULONG_PTR out = 0;
        Gdiplus::GdiplusStartup(&out, &input, nullptr);
        return out;
    }();
    return token;
}

// The theme's 9x9 submenu bitmap is too coarse once the menu is drawn at 200 %. A vector
// triangle stays crisp at any DPI.
bool DrawSubmenuArrow(HDC dc, int ax, int ay, int a, bool disabled) {
    if (!GdiplusToken()) return false;
    Gdiplus::Graphics g(dc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    const Gdiplus::REAL x = ax + a * (3.0f / 9.0f), y = ay + a * (1.0f / 9.0f);
    const Gdiplus::REAL w = a * (4.0f / 9.0f), h = a * (7.0f / 9.0f);
    const Gdiplus::PointF points[3] = {{x, y}, {x, y + h}, {x + w, y + h / 2}};
    Gdiplus::SolidBrush brush(disabled ? Gdiplus::Color(255, 109, 109, 109) : Gdiplus::Color(255, 10, 10, 10));
    return g.FillPolygon(&brush, points, 3) == Gdiplus::Ok;
}

// The radio bullet is a 13 px glass sphere in the theme bitmap; drawing it as a small
// gradient sphere keeps the highlight and the dark rim without magnifying the 96-DPI art.
bool DrawRadioBullet(HDC dc, int cx, int cy, int size, bool disabled) {
    if (!GdiplusToken()) return false;
    Gdiplus::Graphics g(dc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    const Gdiplus::REAL r = size * 0.25f;  // the theme's bullet occupies 8 of its 16 px cell
    Gdiplus::GraphicsPath path;
    path.AddEllipse(cx - r, cy - r, 2 * r, 2 * r);
    Gdiplus::PathGradientBrush brush(&path);
    brush.SetCenterPoint(Gdiplus::PointF(cx - r * 0.30f, cy - r * 0.35f));
    brush.SetCenterColor(disabled ? Gdiplus::Color(255, 250, 250, 250) : Gdiplus::Color(255, 226, 233, 255));
    Gdiplus::Color surround[1] = {disabled ? Gdiplus::Color(255, 130, 130, 130) : Gdiplus::Color(255, 24, 30, 104)};
    int surroundCount = 1;
    brush.SetSurroundColors(surround, &surroundCount);
    Gdiplus::Status status = g.FillPath(&brush, &path);
    Gdiplus::SolidBrush highlight(Gdiplus::Color(190, 255, 255, 255));
    status = status == Gdiplus::Ok ? g.FillEllipse(&highlight, cx - r * 0.55f, cy - r * 0.62f, r * 0.70f, r * 0.42f)
                                   : status;
    Gdiplus::Pen rim(disabled ? Gdiplus::Color(255, 105, 105, 105) : Gdiplus::Color(255, 24, 30, 92),
                     std::max(1.0f, size / 16.0f));
    if (status == Gdiplus::Ok) status = g.DrawPath(&rim, &path);
    return status == Gdiplus::Ok;
}

// The theme's check mark is a 16 px bitmap that goes soft once the menu is drawn at
// 200 %. Draw the same two-stroke mark as vectors (shape and colours sampled from
// IMAGE 976's first cell).
bool DrawCheckMark(HDC dc, int x, int y, int size, bool disabled) {
    if (!GdiplusToken()) return false;
    Gdiplus::Graphics g(dc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    const Gdiplus::REAL k = size / 16.0f;
    const Gdiplus::PointF points[3] = {
        {x + 4.0f * k, y + 7.6f * k}, {x + 6.6f * k, y + 11.6f * k}, {x + 12.6f * k, y + 2.2f * k}};
    Gdiplus::Pen pen(disabled ? Gdiplus::Color(255, 0x84, 0x85, 0x89) : Gdiplus::Color(255, 0x0C, 0x12, 0xA1),
                     std::max(1.0f, 2.0f * k));
    pen.SetLineJoin(Gdiplus::LineJoinMiter);
    pen.SetLineCap(Gdiplus::LineCapFlat, Gdiplus::LineCapFlat, Gdiplus::DashCapFlat);
    return g.DrawLines(&pen, points, 3) == Gdiplus::Ok;
}

void Measure(MEASUREITEMSTRUCT& mis, const ItemData& d, const MenuState& s) {
    if (d.separator) {
        mis.itemWidth = 0;
        mis.itemHeight = s.Px(kSeparatorHeight);
        return;
    }
    HDC dc = GetDC(nullptr);
    HGDIOBJ old = SelectObject(dc, s.bold);  // room for a bold default item
    const auto [label, shortcut] = Split(d.text);
    RECT a{}, b{};
    DrawTextW(dc, label.c_str(), -1, &a, DT_SINGLELINE | DT_CALCRECT);
    if (!shortcut.empty()) DrawTextW(dc, shortcut.c_str(), -1, &b, DT_SINGLELINE | DT_CALCRECT | DT_NOPREFIX);
    SelectObject(dc, old);
    ReleaseDC(nullptr, dc);
    int w = s.Px(kTextLeft) + a.right + (shortcut.empty() ? 0 : s.Px(kShortcutGap) + b.right) + s.Px(kArrowArea);
    // The system adds the width of a check mark to owner-drawn menu items.
    w -= GetSystemMetricsForDpi(SM_CXMENUCHECK, s.dpi) - 1;
    mis.itemWidth = static_cast<UINT>(std::max(w, 0));
    mis.itemHeight = static_cast<UINT>(std::max<int>(s.Px(kItemHeight), a.bottom + s.Px(6)));
}

void Draw(const DRAWITEMSTRUCT& dis, const ItemData& d, const MenuState& s) {
    HDC dc = dis.hDC;
    const RECT rc = dis.rcItem;
    const bool hot = (dis.itemState & ODS_SELECTED) != 0, disabled = (dis.itemState & (ODS_DISABLED | ODS_GRAYED)) != 0;
    FillRect(dc, &rc, s.back);
    const int gx = rc.left + s.Px(kGutterX);
    if (const Dib* g = Image(kGutter))  // the gutter runs through separators too
        Blit(dc, *g, {0, 0, 3, 1}, {gx, rc.top, gx + 3, rc.bottom});
    if (d.separator) {
        if (const Dib* sep = Image(kSeparator)) {
            const int y = (rc.top + rc.bottom) / 2 - 3;
            Blit(dc, *sep, {0, 0, 1, 6}, {gx + 3 + s.Px(2), y, rc.right - s.Px(1), y + 6});
        }
        return;
    }
    if (hot)
        if (const Dib* item = Image(kItem)) {
            const int state = disabled ? 3 : 1;
            NineGrid(dc, *item, {0, 21 * state, 7, 21 * state + 21}, 3,
                     {rc.left, rc.top, rc.right, rc.bottom});  // close to the menu edge, as in Win7
        }
    if (d.icon >= 0) {  // an icon in the check column (WMP's view menu) instead of a mark
        if (const Dib* strip = Image(331, win7::Module::Wmploc)) {
            const int g = s.Px(16), cx = rc.left + s.Px(3) + (s.Px(kCheckBox) - g) / 2, cy = (rc.top + rc.bottom - g) / 2;
            Blit(dc, *strip, {16 * d.icon, 0, 16 * d.icon + 16, 16}, {cx, cy, cx + g, cy + g});
        }
    } else if (dis.itemState & ODS_CHECKED) {
        const int box = s.Px(kCheckBox), bx = rc.left + s.Px(3), by = (rc.top + rc.bottom - box) / 2;
        if (const Dib* back = Image(kCheckBackground))
            NineGrid(dc, *back, {0, disabled ? 7 : 0, 7, disabled ? 14 : 7}, 3, {bx, by, bx + box, by + box});
        const int g = s.Px(16);
        const int cx = bx + (box - g) / 2, cy = by + (box - g) / 2;
        if (d.radio) {
            if (!DrawRadioBullet(dc, cx + g / 2, cy + g / 2, g, disabled)) {
                if (const Dib* check = Image(kCheck)) {
                    const int cell = 2 + (disabled ? 1 : 0);
                    Blit(dc, *check, {0, 16 * cell, 16, 16 * cell + 16}, {cx, cy, cx + g, cy + g});
                }
            }
        } else if (!DrawCheckMark(dc, cx, cy, g, disabled)) {
            if (const Dib* check = Image(kCheck)) {
                const int cell = disabled ? 1 : 0;
                Blit(dc, *check, {0, 16 * cell, 16, 16 * cell + 16}, {cx, cy, cx + g, cy + g});
            }
        }
    }
    const auto [label, shortcut] = Split(d.text);
    HGDIOBJ old = SelectObject(dc, (dis.itemState & ODS_DEFAULT) ? s.bold : s.font);  // default item bold
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, disabled ? kDisabledText : kText);
    RECT tr{rc.left + s.Px(kTextLeft), rc.top, rc.right - s.Px(kArrowArea), rc.bottom};
    const UINT prefix = (dis.itemState & ODS_NOACCEL) ? DT_HIDEPREFIX : 0;
    DrawTextW(dc, label.c_str(), -1, &tr, DT_SINGLELINE | DT_VCENTER | DT_LEFT | prefix);
    if (!shortcut.empty()) DrawTextW(dc, shortcut.c_str(), -1, &tr, DT_SINGLELINE | DT_VCENTER | DT_RIGHT | DT_NOPREFIX);
    SelectObject(dc, old);
    if (d.submenu) {
        const int a = s.Px(9), ax = rc.right - s.Px(kArrowArea) + (s.Px(kArrowArea) - a) / 2 + s.Px(2);
        const int ay = (rc.top + rc.bottom - a) / 2;
        if (!DrawSubmenuArrow(dc, ax, ay, a, disabled))
            if (const Dib* arrow = Image(kSubmenu))
                Blit(dc, *arrow, {0, 9 * (disabled ? 1 : 0), 9, 9 * (disabled ? 1 : 0) + 9}, {ax, ay, ax + a, ay + a});
        // Keep the system from drawing its own submenu arrow over ours.
        ExcludeClipRect(dc, rc.left, rc.top, rc.right, rc.bottom);
    }
}

LRESULT CALLBACK OwnerProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    if (g_menu && (msg == WM_MEASUREITEM || msg == WM_DRAWITEM)) {
        if (msg == WM_MEASUREITEM) {
            auto& mis = *reinterpret_cast<MEASUREITEMSTRUCT*>(lp);
            const auto* d = reinterpret_cast<const ItemData*>(mis.itemData);
            if (mis.CtlType == ODT_MENU && g_menu->mine.count(d)) {
                Measure(mis, *d, *g_menu);
                return TRUE;
            }
        } else {
            const auto& dis = *reinterpret_cast<const DRAWITEMSTRUCT*>(lp);
            const auto* d = reinterpret_cast<const ItemData*>(dis.itemData);
            if (dis.CtlType == ODT_MENU && g_menu->mine.count(d)) {
                Draw(dis, *d, *g_menu);
                return TRUE;
            }
        }
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

// Our popup windows: square corners and Win7's one-pixel border over the menu colour.
LRESULT CALLBACK PopupProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR) {
    if (msg == WM_NCPAINT) {
        HDC dc = GetWindowDC(hwnd);
        RECT wr{}, cr{};
        GetWindowRect(hwnd, &wr);
        GetClientRect(hwnd, &cr);
        MapWindowPoints(hwnd, nullptr, reinterpret_cast<POINT*>(&cr), 2);
        OffsetRect(&cr, -wr.left, -wr.top);
        OffsetRect(&wr, -wr.left, -wr.top);
        ExcludeClipRect(dc, cr.left, cr.top, cr.right, cr.bottom);
        HBRUSH back = CreateSolidBrush(kBackColor), border = CreateSolidBrush(kBorderColor);
        FillRect(dc, &wr, back);
        FrameRect(dc, &wr, border);
        DeleteObject(back);
        DeleteObject(border);
        ReleaseDC(hwnd, dc);
        return 0;
    }
    if (msg == WM_NCDESTROY) RemoveWindowSubclass(hwnd, PopupProc, id);
    return DefSubclassProc(hwnd, msg, wp, lp);
}

LRESULT CALLBACK HookProc(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION) {
        const auto* cwp = reinterpret_cast<const CWPSTRUCT*>(lp);
        if (cwp->message == WM_CREATE) {
            wchar_t cls[16]{};
            GetClassNameW(cwp->hwnd, cls, static_cast<int>(std::size(cls)));
            if (wcscmp(cls, L"#32768") == 0) {
                DWM_WINDOW_CORNER_PREFERENCE corner = DWMWCP_DONOTROUND;
                DwmSetWindowAttribute(cwp->hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
                COLORREF none = DWMWA_COLOR_NONE;
                DwmSetWindowAttribute(cwp->hwnd, DWMWA_BORDER_COLOR, &none, sizeof(none));
                SetWindowSubclass(cwp->hwnd, PopupProc, 1, 0);
            }
        }
    }
    return CallNextHookEx(g_hook, code, wp, lp);
}

}  // namespace

UINT TrackWin7Menu(HMENU menu, UINT flags, int x, int y, HWND owner, const std::map<UINT, int>* icons,
                   const RECT* exclude) {
    TPMPARAMS params{sizeof(params)};
    if (exclude) params.rcExclude = *exclude;
    TPMPARAMS* const keepOut = exclude ? &params : nullptr;
    const bool art = Image(kItem) && Image(kGutter);
    if (!art || g_menu) return TrackPopupMenuEx(menu, flags, x, y, owner, keepOut);
    MenuState state;
    state.dpi = GetDpiForWindow(owner);
    NONCLIENTMETRICSW ncm{sizeof(ncm)};
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, state.dpi);
    state.font = CreateFontIndirectW(&ncm.lfMenuFont);
    LOGFONTW bold = ncm.lfMenuFont;
    bold.lfWeight = FW_BOLD;
    state.bold = CreateFontIndirectW(&bold);
    state.icons = icons;
    state.back = CreateSolidBrush(kBackColor);
    Convert(menu, state);
    g_menu = &state;
    SetWindowSubclass(owner, OwnerProc, 0x67770001, 0);
    g_hook = SetWindowsHookExW(WH_CALLWNDPROC, HookProc, nullptr, GetCurrentThreadId());
    const UINT result = TrackPopupMenuEx(menu, flags, x, y, owner, keepOut);
    if (g_hook) UnhookWindowsHookEx(g_hook);
    g_hook = nullptr;
    RemoveWindowSubclass(owner, OwnerProc, 0x67770001);
    g_menu = nullptr;
    DeleteObject(state.font);
    DeleteObject(state.bold);
    DeleteObject(state.back);
    return result;
}

namespace {

// Queue and Favorites commands above foobar2000's own context menu (ids past its range).
enum : UINT { kPlay = 0x8001, kPlayNext, kAddToQueue, kClearQueue, kSaveArt };

std::function<std::vector<MenuAction>(metadb_handle_list_cref)> g_contextActions;

std::vector<MenuAction> ContextActionsFor(metadb_handle_list_cref items) {
    if (!g_contextActions || items.get_count() == 0) return {};
    return g_contextActions(items);
}

void AppendQueueCommands(HMENU menu, bool inQueue) {
    if (inQueue) return;  // the queue page acts on the playing list itself
    AppendMenuW(menu, MF_STRING, kPlay, TrC(L"Play"));
    AppendMenuW(menu, MF_STRING, kPlayNext, TrC(L"Play next"));
    AppendMenuW(menu, MF_STRING, kAddToQueue, TrC(L"Add to queue"));
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
}

bool RunQueueCommand(UINT cmd, metadb_handle_list_cref items) {
    switch (cmd) {
    case kPlay: media::PlayQueue(items, 0); return true;
    case kPlayNext: media::PlayNext(items); return true;
    case kAddToQueue: media::AddToQueue(items); return true;
    default: return false;
    }
}

}  // namespace

void SetContextActionProvider(std::function<std::vector<MenuAction>(metadb_handle_list_cref)> provider) {
    g_contextActions = std::move(provider);
}

void RunContextMenu(HWND owner, metadb_handle_list_cref items, POINT pt, bool nowPlaying) {
    if (items.get_count() == 0) return;
    auto cm = contextmenu_manager::get();
    cm->init_context(items, contextmenu_manager::flag_show_shortcuts);
    constexpr int kBase = 1;
    constexpr UINT kActionBase = 0x8200;
    HMENU menu = CreatePopupMenu();
    AppendQueueCommands(menu, nowPlaying);
    MenuBuilder custom(kActionBase);
    custom.Append(menu, ContextActionsFor(items));
    if (items.get_count() == 1) AppendMenuW(menu, MF_STRING, kSaveArt, TrC(L"Save album art..."));
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    cm->win32_build_menu(menu, kBase, 0x7FFF);
    const UINT cmd = TrackWin7Menu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, pt.x, pt.y, owner);
    DestroyMenu(menu);
    if (cmd == kSaveArt) {
        SaveAlbumArt(owner, items[0]);
        return;
    }
    if (custom.Run(cmd)) return;
    if (RunQueueCommand(cmd, items)) return;
    if (cmd >= kBase) cm->execute_by_id(cmd - kBase);
}

void RunPlaylistContextMenu(HWND owner, POINT pt) {
    auto pm = playlist_manager::get();
    const t_size playlist = pm->get_active_playlist();
    const bool queue = media::IsQueue(playlist);
    metadb_handle_list items;
    pm->activeplaylist_get_selected_items(items);
    auto cm = contextmenu_manager::get();
    cm->init_context_playlist(contextmenu_manager::flag_show_shortcuts);
    constexpr int kBase = 1;
    constexpr UINT kActionBase = 0x8200;
    HMENU menu = CreatePopupMenu();
    if (queue) {
        AppendMenuW(menu, MF_STRING, kClearQueue, TrC(L"Clear Now Playing"));
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    }
    if (items.get_count()) AppendQueueCommands(menu, queue);
    const auto actions = ContextActionsFor(items);
    MenuBuilder custom(kActionBase);
    custom.Append(menu, actions);
    if (!actions.empty()) AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    cm->win32_build_menu(menu, kBase, 0x7FFF);
    const UINT cmd = TrackWin7Menu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, pt.x, pt.y, owner);
    DestroyMenu(menu);
    if (cmd == kClearQueue) {
        if (queue) {
            t_size playingPlaylist = SIZE_MAX, playingIndex = SIZE_MAX;
            const bool playing =
                pm->get_playing_item_location(&playingPlaylist, &playingIndex) && playingPlaylist == playlist;
            pm->playlist_undo_backup(playlist);
            pm->playlist_clear(playlist);
            if (playing) playback_control::get()->stop();
        }
        return;
    }
    if (custom.Run(cmd)) return;
    if (RunQueueCommand(cmd, items)) return;
    if (cmd >= kBase) cm->execute_by_id(cmd - kBase);
}

}  // namespace glasswave::ui
