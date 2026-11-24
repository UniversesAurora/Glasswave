#include "pch.h"
#include "ui/taskbar.h"
#include "media/radio.h"
#include "win7/win7_resources.h"
#include "gfx/composition_host.h"
#include "media/transport.h"
#include "ui/art_cache.h"
#include "ui/i18n.h"

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
    // A cover that finishes loading after DWM asked replaces the thumbnail.
    m_artToken = ArtCache::Get().Subscribe([this] {
        if (!m_coverOn || !m_coverTrack.is_valid() || m_coverSize <= 0) return;
        if (ArtCache::Get().Version(m_coverKey, m_coverTrack, m_coverSize) != m_coverVersion)
            DwmInvalidateIconicBitmaps(m_hwnd);
    });
}

Taskbar::~Taskbar() {
    if (auto* cache = ArtCache::Existing()) cache->Unsubscribe(m_artToken);
    if (m_images) ImageList_Destroy(m_images);
}

void Taskbar::SetCoverTrack(const metadb_handle_ptr& track) {
    const std::wstring key = track.is_valid() ? ArtCache::SourceKey(track) : std::wstring();
    if (track == m_coverTrack && key == m_coverKey) return;
    m_coverTrack = track;
    m_coverKey = key;
    m_coverVersion = 0;
    UseCover(track.is_valid());
    if (m_coverOn) DwmInvalidateIconicBitmaps(m_hwnd);
}

void Taskbar::UseCover(bool on) {
    if (on == m_coverOn) return;
    m_coverOn = on;
    const BOOL value = on;
    DwmSetWindowAttribute(m_hwnd, DWMWA_FORCE_ICONIC_REPRESENTATION, &value, sizeof(value));
    DwmSetWindowAttribute(m_hwnd, DWMWA_HAS_ICONIC_BITMAP, &value, sizeof(value));
}

void Taskbar::SendCoverThumbnail(int maxWidth, int maxHeight) {
    const int side = std::min(maxWidth, maxHeight);
    if (maxWidth <= 0 || maxHeight <= 0 || side <= 0 || !m_coverTrack.is_valid()) return;
    m_coverSize = side;
    winrt::com_ptr<IWICBitmapSource> art;
    const auto state = ArtCache::Get().Find(m_coverKey, m_coverTrack, side, &art);
    if (state == ArtCache::State::Missing) {  // no cover: the window itself again
        UseCover(false);
        return;
    }
    // Always supply a transparent 32-bpp DIB matching DWM's requested maxWidth x maxHeight
    // bounding box with the cover aspect-fitted and centred inside it. Handing DWM a narrower
    // square DIB causes Windows 11's taskbar flyout to stretch the cover horizontally on the
    // first hover before Explorer has cached the iconic bitmap's aspect ratio.
    UINT w = 0, h = 0;
    if (art) art->GetSize(&w, &h);
    const bool loading = !art || w <= 1 || h <= 1;
    if (!loading) m_coverVersion = ArtCache::Get().Version(m_coverKey, m_coverTrack, side);

    BITMAPINFO bi{};
    bi.bmiHeader = {sizeof(BITMAPINFOHEADER), maxWidth, -maxHeight, 1, 32, BI_RGB};
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bmp) return;
    const UINT stride = static_cast<UINT>(maxWidth) * 4;
    std::memset(bits, 0, static_cast<size_t>(stride) * maxHeight);
    bool drawn = loading;
    if (!loading) {
        const double fit = std::min(static_cast<double>(maxWidth) / w, static_cast<double>(maxHeight) / h);
        const int tw = std::clamp(static_cast<int>(std::lround(w * fit)), 1, maxWidth);
        const int th = std::clamp(static_cast<int>(std::lround(h * fit)), 1, maxHeight);
        auto* wic = gfx::CompositionHost::Get().Wic();
        winrt::com_ptr<IWICBitmapScaler> scaler;
        winrt::com_ptr<IWICFormatConverter> conv;
        if (SUCCEEDED(wic->CreateBitmapScaler(scaler.put())) &&
            SUCCEEDED(scaler->Initialize(art.get(), tw, th, WICBitmapInterpolationModeHighQualityCubic)) &&
            SUCCEEDED(wic->CreateFormatConverter(conv.put())) &&
            SUCCEEDED(conv->Initialize(scaler.get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone,
                                       nullptr, 0, WICBitmapPaletteTypeCustom))) {
            const size_t offset =
                static_cast<size_t>((maxHeight - th) / 2) * stride + static_cast<size_t>((maxWidth - tw) / 2) * 4;
            const WICRect all{0, 0, tw, th};
            const UINT room = static_cast<UINT>(static_cast<size_t>(stride) * maxHeight - offset);
            drawn = SUCCEEDED(conv->CopyPixels(&all, stride, room, static_cast<BYTE*>(bits) + offset));
        }
    }
    if (drawn) DwmSetIconicThumbnail(m_hwnd, bmp, 0);
    DeleteObject(bmp);
}

void Taskbar::SendLivePreview() {
    // Peek still shows the window: its current content, composition visuals included.
    RECT rc{};
    if (!GetWindowRect(m_hwnd, &rc) || IsIconic(m_hwnd)) return;
    const int w = rc.right - rc.left, h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return;
    BITMAPINFO bi{};
    bi.bmiHeader = {sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB};
    void* bits = nullptr;
    HDC screen = GetDC(nullptr);
    HDC dc = CreateCompatibleDC(screen);
    ReleaseDC(nullptr, screen);
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (bmp) {
        HGDIOBJ old = SelectObject(dc, bmp);
        if (PrintWindow(m_hwnd, dc, PW_RENDERFULLCONTENT)) {
            // GDI leaves alpha at 0: make the capture opaque.
            auto* px = static_cast<uint32_t*>(bits);
            for (size_t i = 0, n = static_cast<size_t>(w) * h; i < n; ++i) px[i] |= 0xFF000000u;
            SelectObject(dc, old);
            DwmSetIconicLivePreviewBitmap(m_hwnd, bmp, nullptr, 0);
        } else {
            SelectObject(dc, old);
        }
        DeleteObject(bmp);
    }
    DeleteDC(dc);
}

bool Taskbar::OnMessage(UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == m_createdMsg && m_createdMsg) {  // (re)created, e.g. after Explorer restarts
        m_added = false;
        AddButtons();
        return true;
    }
    if (msg == WM_DWMSENDICONICTHUMBNAIL && m_coverOn) {
        SendCoverThumbnail(HIWORD(lp), LOWORD(lp));
        return true;
    }
    if (msg == WM_DWMSENDICONICLIVEPREVIEWBITMAP && m_coverOn) {
        SendLivePreview();
        return true;
    }
    if (msg == WM_COMMAND && HIWORD(wp) == THBN_CLICKED) {
        switch (LOWORD(wp)) {
        case kPrev: media::transport::Previous(); break;
        case kNext: media::transport::Next(); break;
        case kFavorite:
            if (onFavorite) onFavorite();
            break;
        case kPlayPause: media::transport::PlayPause(); break;
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

void Taskbar::RefreshButtons() {
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
    const bool prevOk = m_playing && media::radio::CanGoBack();
    const bool nextOk = m_playing && media::radio::CanGoNext();
    const bool star = m_starFull >= 0 && m_starOutline >= 0;
    const struct {
        UINT id;
        int image;
        const wchar_t* tip;
        bool enabled;
    } spec[] = {{kPrev, kGlyphPrev + (prevOk ? 0 : 1), L"Previous", prevOk},
                {kPlayPause, showPause ? kGlyphPause : kGlyphPlay, showPause ? L"Pause" : L"Play", true},
                {kNext, kGlyphNext + (nextOk ? 0 : 1), L"Next", nextOk},
                {kFavorite, m_favorite ? m_starFull : m_starOutline,
                 m_favorite ? L"Remove from Favorites" : L"Add to Favorites", m_playing}};
    const int count = star ? 4 : 3;
    for (int i = 0; i < count; ++i) {
        b[i].dwMask = THB_BITMAP | THB_TOOLTIP | THB_FLAGS;
        b[i].iId = spec[i].id;
        b[i].iBitmap = spec[i].image;
        b[i].dwFlags = spec[i].enabled ? THBF_ENABLED : THBF_DISABLED;
        wcsncpy_s(b[i].szTip, TrC(spec[i].tip), _TRUNCATE);
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
        for (const auto& [english, args] : items) {
            const wchar_t* title = TrC(english);
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
