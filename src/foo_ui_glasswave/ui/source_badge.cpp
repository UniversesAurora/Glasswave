#include "pch.h"
#include "ui/source_badge.h"

#include "gfx/composition_host.h"
#include "stream/provider.h"
#include "win7/win7_resources.h"

#include <shlobj.h>
#include <shobjidl.h>

namespace glasswave::ui {

namespace {

constexpr int kIconPx = 64; // read once at this size, drawn scaled

// One source's icon: premultiplied BGRA pixels, then a device bitmap made from them.
struct Icon {
    bool asked = false;
    std::vector<uint8_t> pixels; // kIconPx x kIconPx; empty: none found
    winrt::com_ptr<ID2D1Bitmap> bitmap;
    ID2D1Device* device = nullptr;
};
std::map<std::wstring, Icon> g_icons; // by source key; main thread
std::map<int, std::function<void()>> g_listeners;
int g_nextListener = 1;

std::vector<uint8_t> Pixels(HBITMAP bmp) {
    std::vector<uint8_t> out;
    BITMAP info{};
    if (!bmp || !GetObjectW(bmp, sizeof(info), &info) || info.bmWidth != kIconPx || info.bmHeight != kIconPx)
        return out;
    BITMAPINFO bi{};
    bi.bmiHeader = {sizeof(BITMAPINFOHEADER), kIconPx, -kIconPx, 1, 32, BI_RGB};
    out.resize(static_cast<size_t>(kIconPx) * kIconPx * 4);
    HDC dc = GetDC(nullptr);
    const int rows = GetDIBits(dc, bmp, 0, kIconPx, out.data(), &bi, DIB_RGB_COLORS);
    ReleaseDC(nullptr, dc);
    if (rows != kIconPx) out.clear();
    return out;
}

// The service's own desktop app as the Start menu lists it (packaged or not), its icon.
std::vector<uint8_t> AppIcon(const std::vector<std::wstring>& names) {
    winrt::com_ptr<IShellItem> apps;
    if (FAILED(SHGetKnownFolderItem(FOLDERID_AppsFolder, KF_FLAG_DEFAULT, nullptr, IID_PPV_ARGS(apps.put()))))
        return {};
    winrt::com_ptr<IEnumShellItems> items;
    if (FAILED(apps->BindToHandler(nullptr, BHID_EnumItems, IID_PPV_ARGS(items.put())))) return {};
    for (winrt::com_ptr<IShellItem> item; items->Next(1, item.put(), nullptr) == S_OK; item = nullptr) {
        wchar_t* name = nullptr;
        if (FAILED(item->GetDisplayName(SIGDN_NORMALDISPLAY, &name))) continue;
        const bool match = std::find(names.begin(), names.end(), std::wstring(name)) != names.end();
        CoTaskMemFree(name);
        if (!match) continue;
        winrt::com_ptr<IShellItemImageFactory> factory;
        HBITMAP bmp = nullptr;
        if (SUCCEEDED(item->QueryInterface(IID_PPV_ARGS(factory.put()))) &&
            SUCCEEDED(factory->GetImage({kIconPx, kIconPx}, SIIGBF_ICONONLY | SIIGBF_BIGGERSIZEOK, &bmp))) {
            auto pixels = Pixels(bmp);
            DeleteObject(bmp);
            if (!pixels.empty()) return pixels;
        }
    }
    return {};
}

// Windows Media Player's own icon (wmploc 102) for the local library.
std::vector<uint8_t> PlayerIcon() {
    HICON icon = win7::Resources::Get().Icon(win7::Module::Wmploc, 102, kIconPx);
    if (!icon) return {};
    ICONINFO info{};
    std::vector<uint8_t> pixels;
    if (GetIconInfo(icon, &info)) {
        pixels = Pixels(info.hbmColor);
        if (info.hbmColor) DeleteObject(info.hbmColor);
        if (info.hbmMask) DeleteObject(info.hbmMask);
        // Icon colour bitmaps are straight alpha: premultiply for Direct2D.
        for (size_t i = 0; i + 3 < pixels.size(); i += 4)
            for (int c = 0; c < 3; ++c) pixels[i + c] = static_cast<uint8_t>(pixels[i + c] * pixels[i + 3] / 255);
    }
    DestroyIcon(icon);
    return pixels;
}

void Ask(const std::wstring& source) {
    Icon& icon = g_icons[source];
    if (icon.asked) return;
    icon.asked = true;
    if (source == L"local") {
        icon.pixels = PlayerIcon();
        return;
    }
    const auto* provider = stream::FindProvider(source);
    if (!provider) return;
    // The Start menu's apps are listed on a worker (enumerating them takes a moment).
    fb2k::splitTask([source, names = provider->AppNames()] {
        const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        auto pixels = names.empty() ? std::vector<uint8_t>{} : AppIcon(names);
        if (SUCCEEDED(com)) CoUninitialize();
        fb2k::inMainThread([source, pixels = std::move(pixels)]() mutable {
            g_icons[source].pixels = std::move(pixels);
            const auto listeners = g_listeners;
            for (const auto& [token, listener] : listeners)
                if (listener) listener();
        });
    });
}

}  // namespace

void DrawSourceBadge(ID2D1DeviceContext* dc, D2D1_RECT_F box, std::wstring_view source, float opacity) {
    if (source.empty() || opacity <= 0) return;
    const std::wstring key(source);
    Ask(key);
    Icon& icon = g_icons[key];
    if (icon.pixels.empty()) return; // not read yet, or the app is not installed: no mark
    winrt::com_ptr<ID2D1Device> device;
    dc->GetDevice(device.put());
    if (!icon.bitmap || icon.device != device.get()) {
        icon.bitmap = nullptr;
        const D2D1_BITMAP_PROPERTIES props =
            D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        if (FAILED(dc->CreateBitmap({kIconPx, kIconPx}, icon.pixels.data(), kIconPx * 4, props, icon.bitmap.put())))
            return;
        icon.device = device.get();
    }
    dc->DrawBitmap(icon.bitmap.get(), box, opacity, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, nullptr, nullptr);
}

int SubscribeSourceIcons(std::function<void()> ready) {
    const int token = g_nextListener++;
    g_listeners[token] = std::move(ready);
    return token;
}

void UnsubscribeSourceIcons(int token) {
    g_listeners.erase(token);
}

}  // namespace glasswave::ui
