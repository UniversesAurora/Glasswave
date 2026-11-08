#include "pch.h"
#include "ui/taskbar.h"
#include "win7/win7_resources.h"
#include "gfx/composition_host.h"

#include <commctrl.h>
#include <propsys.h>
#pragma comment(lib, "comctl32.lib")

namespace glasswave::ui {

namespace {

enum ButtonId : UINT { kPrev = 1, kPlayPause = 2, kNext = 3, kFavorite = 4 };
// wmploc bitmaps 355 / 356 / 357 (16 / 20 / 24 px glyphs): previous, play, pause, stop,
// next, each followed by its disabled version.
enum Glyph : int { kGlyphPrev = 0, kGlyphPlay = 2, kGlyphPause = 4, kGlyphStop = 6, kGlyphNext = 8 };

// PKEY_Title (FMTID_SummaryInformation, 2), spelled out to avoid linking propsys.
constexpr PROPERTYKEY kTitle{{0xF29F85E0, 0x4FF9, 0x1068, {0xAB, 0x91, 0x08, 0x00, 0x2B, 0x27, 0xB3, 0xD9}}, 2};

// A wmploc PNG scaled to `size` into the image list (premultiplied 32-bpp, as AlphaBlend
// expects); -1 when missing.
int AddPng(HIMAGELIST list, int id, int size) {
    IWICBitmapSource* src = win7::Resources::Get().Image(win7::Module::Wmploc, id);
    if (!src) return -1;
    auto* wic = gfx::CompositionHost::Get().Wic();
    winrt::com_ptr<IWICBitmapScaler> scaler;
    winrt::com_ptr<IWICFormatConverter> conv;
    UINT w = 0, h = 0;
    src->GetSize(&w, &h);
    const int inner = size * 14 / 16;  // the 14 px stars sit in a 16 px cell
    if (FAILED(wic->CreateBitmapScaler(scaler.put())) ||
        FAILED(scaler->Initialize(src, inner, inner, WICBitmapInterpolationModeHighQualityCubic)) ||
        FAILED(wic->CreateFormatConverter(conv.put())) ||
        FAILED(conv->Initialize(scaler.get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                WICBitmapPaletteTypeCustom)))
        return -1;
    BITMAPINFO bi{};
    bi.bmiHeader = {sizeof(BITMAPINFOHEADER), size, -size, 1, 32, BI_RGB};
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bmp) return -1;
    std::memset(bits, 0, static_cast<size_t>(size) * size * 4);
    const int o = (size - inner) / 2;
    const WICRect all{0, 0, inner, inner};
    const size_t offset = (static_cast<size_t>(o) * size + o) * 4;
    conv->CopyPixels(&all, size * 4, static_cast<UINT>(static_cast<size_t>(size) * size * 4 - offset),
                     static_cast<BYTE*>(bits) + offset);
    const int index = ImageList_Add(list, bmp, nullptr);
    DeleteObject(bmp);
    return index;
}

}  // namespace

Taskbar::Taskbar(HWND hwnd) : m_hwnd(hwnd) {
    m_createdMsg = RegisterWindowMessageW(L"TaskbarButtonCreated");
}

Taskbar::~Taskbar() {
    if (m_images) ImageList_Destroy(m_images);
}

bool Taskbar::OnMessage(UINT msg, WPARAM wp, LPARAM /*lp*/) {
    if (msg == m_createdMsg && m_createdMsg) {  // (re)created, e.g. after Explorer restarts
        m_added = false;
        AddButtons();
        return true;
    }
    if (msg == WM_COMMAND && HIWORD(wp) == THBN_CLICKED) {
        auto pc = playback_control::get();
        switch (LOWORD(wp)) {
        case kPrev: pc->previous(); break;
        case kNext: pc->next(); break;
        case kFavorite:
            if (onFavorite) onFavorite();
            break;
        case kPlayPause:
            if (pc->is_playing()) pc->play_or_pause();
            else pc->start();
            break;
        default: return false;
        }
        return true;
    }
    return false;
}

void Taskbar::SetPlayState(bool playing, bool paused) {
    if (playing == m_playing && paused == m_paused) return;
    m_playing = playing;
    m_paused = paused;
    UpdateButtons();
}

void Taskbar::SetFavorite(bool favorite) {
    if (favorite == m_favorite) return;
    m_favorite = favorite;
    UpdateButtons();
}

void Taskbar::AddButtons() {
    if (!m_list) {
        m_list = winrt::try_create_instance<ITaskbarList3>(CLSID_TaskbarList);
        if (!m_list || FAILED(m_list->HrInit())) {
            m_list = nullptr;
            return;
        }
    }
    if (!m_images) {
        // Glyph size follows the small icon size at the window's DPI.
        const int icon = GetSystemMetricsForDpi(SM_CXSMICON, GetDpiForWindow(m_hwnd));
        const int id = icon <= 16 ? 355 : icon <= 20 ? 356 : 357, size = icon <= 16 ? 16 : icon <= 20 ? 20 : 24;
        if (HBITMAP bmp = win7::Resources::Get().DibBitmap(win7::Module::Wmploc, id)) {
            m_images = ImageList_Create(size, size, ILC_COLOR32, 12, 0);
            ImageList_Add(m_images, bmp, nullptr);
            DeleteObject(bmp);
            m_starFull = AddPng(m_images, 5530, size);     // gold star
            m_starOutline = AddPng(m_images, 5531, size);  // its outline
        }
    }
    if (!m_images) return;  // no WMP art: no toolbar rather than blank buttons
    m_list->ThumbBarSetImageList(m_hwnd, m_images);
    THUMBBUTTON buttons[4]{};
    const UINT ids[] = {kPrev, kPlayPause, kNext, kFavorite};
    const int count = m_starFull >= 0 && m_starOutline >= 0 ? 4 : 3;
    for (int i = 0; i < count; ++i) {
        buttons[i].dwMask = THB_BITMAP | THB_TOOLTIP | THB_FLAGS;
        buttons[i].iId = ids[i];
    }
    m_added = SUCCEEDED(m_list->ThumbBarAddButtons(m_hwnd, count, buttons));
    UpdateButtons();
}

void Taskbar::UpdateButtons() {
    if (!m_added || !m_list) return;
    THUMBBUTTON b[4]{};
    const bool showPause = m_playing && !m_paused;
    const bool star = m_starFull >= 0 && m_starOutline >= 0;
    const struct {
        UINT id;
        int image;
        const wchar_t* tip;
        bool enabled;
    } spec[] = {{kPrev, kGlyphPrev + (m_playing ? 0 : 1), L"Previous", m_playing},
                {kPlayPause, showPause ? kGlyphPause : kGlyphPlay, showPause ? L"Pause" : L"Play", true},
                {kNext, kGlyphNext + (m_playing ? 0 : 1), L"Next", m_playing},
                {kFavorite, m_favorite ? m_starFull : m_starOutline,
                 m_favorite ? L"Remove from Favorites" : L"Add to Favorites", m_playing}};
    const int count = star ? 4 : 3;
    for (int i = 0; i < count; ++i) {
        b[i].dwMask = THB_BITMAP | THB_TOOLTIP | THB_FLAGS;
        b[i].iId = spec[i].id;
        b[i].iBitmap = spec[i].image;
        b[i].dwFlags = spec[i].enabled ? THBF_ENABLED : THBF_DISABLED;
        wcsncpy_s(b[i].szTip, spec[i].tip, _TRUNCATE);
    }
    m_list->ThumbBarUpdateButtons(m_hwnd, count, b);
}

void Taskbar::SetupJumpList() {
    // An explicit AppUserModelID may be shared with other foobar2000 installations; their
    // jump lists are not ours to change.
    if (PWSTR id = nullptr; SUCCEEDED(GetCurrentProcessExplicitAppUserModelID(&id))) {
        console::print("Glasswave: jump list tasks skipped (explicit AppUserModelID ", pfc::stringcvt::string_utf8_from_wide(id), ")");
        CoTaskMemFree(id);
        return;
    }
    try {
        auto list = winrt::create_instance<ICustomDestinationList>(CLSID_DestinationList);
        UINT slots = 0;
        winrt::com_ptr<IObjectArray> removed;
        winrt::check_hresult(list->BeginList(&slots, IID_PPV_ARGS(removed.put())));
        auto tasks = winrt::create_instance<IObjectCollection>(CLSID_EnumerableObjectCollection);
        wchar_t exe[MAX_PATH]{};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        // foobar2000's own command-line switches.
        const std::pair<const wchar_t*, const wchar_t*> items[] = {
            {L"Play / Pause", L"/playpause"}, {L"Next track", L"/next"}, {L"Previous track", L"/prev"}, {L"Stop", L"/stop"}};
        for (const auto& [title, args] : items) {
            auto link = winrt::create_instance<IShellLinkW>(CLSID_ShellLink);
            link->SetPath(exe);
            link->SetArguments(args);
            link->SetIconLocation(exe, 0);
            auto store = link.as<IPropertyStore>();
            PROPVARIANT pv{};
            pv.vt = VT_LPWSTR;
            const size_t bytes = (wcslen(title) + 1) * sizeof(wchar_t);
            pv.pwszVal = static_cast<LPWSTR>(CoTaskMemAlloc(bytes));
            if (!pv.pwszVal) continue;
            memcpy(pv.pwszVal, title, bytes);
            store->SetValue(kTitle, pv);
            store->Commit();
            PropVariantClear(&pv);
            tasks->AddObject(link.get());
        }
        winrt::check_hresult(list->AddUserTasks(tasks.as<IObjectArray>().get()));
        winrt::check_hresult(list->CommitList());
    } catch (const winrt::hresult_error& e) {
        console::print("Glasswave: jump list failed: ", pfc::stringcvt::string_utf8_from_wide(e.message().c_str()));
    }
}

}  // namespace glasswave::ui
