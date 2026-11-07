#include "pch.h"
#include "win7/win7_resources.h"
#include "gfx/composition_host.h"

namespace glasswave::win7 {

namespace {

std::unique_ptr<Resources> g_resources;

struct ModuleInfo {
    const wchar_t* file;
    const wchar_t* type;  // resource type holding the PNGs
};

// wmploc.dll keeps WMP 12's PNGs under numeric type 257, aero.msstyles under "IMAGE",
// ehres.dll (Media Center) under RT_RCDATA.
const ModuleInfo kModules[] = {
    {L"wmploc.dll", MAKEINTRESOURCEW(257)},
    {L"aero.msstyles", L"IMAGE"},
    {L"ehres.dll", MAKEINTRESOURCEW(10)},
};
static_assert(std::size(kModules) == static_cast<size_t>(Module::Count));

std::wstring ProfileFolder() {
    pfc::string8 native;
    try {
        if (!filesystem::g_get_native_path(core_api::get_profile_path(), native)) return {};
    } catch (...) {
        return {};
    }
    std::wstring path = pfc::stringcvt::string_wide_from_utf8(native).get_ptr();
    if (!path.empty() && path.back() != L'\\') path += L'\\';
    return path + L"glasswave\\";
}

}  // namespace

Resources& Resources::Get() {
    if (!g_resources) g_resources.reset(new Resources());
    return *g_resources;
}

void Resources::Shutdown() { g_resources.reset(); }

Resources::Resources() : m_folder(ProfileFolder()) {
    if (m_folder.empty()) return;
    for (size_t i = 0; i < m_modules.size(); ++i) {
        const std::wstring path = m_folder + kModules[i].file;
        m_modules[i] = LoadLibraryExW(path.c_str(), nullptr,
                                      LOAD_LIBRARY_AS_IMAGE_RESOURCE | LOAD_LIBRARY_AS_DATAFILE);
        if (m_modules[i]) {
            FB2K_console_formatter() << "Glasswave: using Win7 artwork from "
                                     << pfc::stringcvt::string_utf8_from_wide(path.c_str());
        }
    }
}

Resources::~Resources() {
    m_device.clear();
    m_decoded.clear();
    for (HMODULE& m : m_modules)
        if (m) FreeLibrary(std::exchange(m, nullptr));
}

IWICBitmapSource* Resources::Decode(Module module, const std::wstring& name) {
    const auto key = std::make_pair(static_cast<int>(module), name);
    if (auto it = m_decoded.find(key); it != m_decoded.end()) return it->second.get();
    auto& slot = m_decoded[key];  // stays nullptr on any failure below

    HMODULE mod = Handle(module);
    if (!mod) return nullptr;
    HRSRC res = FindResourceW(mod, name.c_str(), kModules[static_cast<size_t>(module)].type);
    if (!res) return nullptr;
    HGLOBAL data = LoadResource(mod, res);
    const DWORD size = SizeofResource(mod, res);
    auto* bytes = data ? static_cast<BYTE*>(LockResource(data)) : nullptr;
    if (!bytes || size == 0) return nullptr;

    auto* wic = gfx::CompositionHost::Get().Wic();
    winrt::com_ptr<IWICStream> stream;
    winrt::com_ptr<IWICBitmapDecoder> decoder;
    winrt::com_ptr<IWICBitmapFrameDecode> frame;
    winrt::com_ptr<IWICFormatConverter> converter;
    winrt::com_ptr<IWICBitmap> bitmap;
    if (FAILED(wic->CreateStream(stream.put())) || FAILED(stream->InitializeFromMemory(bytes, size)) ||
        FAILED(wic->CreateDecoderFromStream(stream.get(), nullptr, WICDecodeMetadataCacheOnDemand, decoder.put())) ||
        FAILED(decoder->GetFrame(0, frame.put())) || FAILED(wic->CreateFormatConverter(converter.put())) ||
        FAILED(converter->Initialize(frame.get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr,
                                     0, WICBitmapPaletteTypeMedianCut)) ||
        FAILED(wic->CreateBitmapFromSource(converter.get(), WICBitmapCacheOnLoad, bitmap.put())))
        return nullptr;
    slot = bitmap;
    return slot.get();
}

bool Resources::Has(Module module, const std::wstring& name) { return Decode(module, name) != nullptr; }

D2D1_SIZE_U Resources::Size(Module module, const std::wstring& name) {
    UINT w = 0, h = 0;
    if (auto* src = Decode(module, name)) src->GetSize(&w, &h);
    return {w, h};
}

ID2D1Bitmap* Resources::Bitmap(ID2D1DeviceContext* dc, Module module, const std::wstring& name) {
    const auto key = std::make_pair(static_cast<int>(module), name);
    // Every drawing surface comes from the one D2D device, so device bitmaps can be shared.
    if (auto it = m_device.find(key); it != m_device.end()) return it->second.get();
    IWICBitmapSource* src = Decode(module, name);
    if (!src) return nullptr;
    const auto props = D2D1::BitmapProperties(
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);
    winrt::com_ptr<ID2D1Bitmap> bmp;
    if (FAILED(dc->CreateBitmapFromWicBitmap(src, &props, bmp.put()))) return nullptr;
    m_device[key] = bmp;
    return bmp.get();
}

bool DrawPart(ID2D1DeviceContext* dc, Module module, int id, D2D1_RECT_F src, D2D1_POINT_2F at, float opacity) {
    ID2D1Bitmap* bmp = Resources::Get().Bitmap(dc, module, id);
    if (!bmp) return false;
    const D2D1_RECT_F dst{at.x, at.y, at.x + (src.right - src.left), at.y + (src.bottom - src.top)};
    dc->DrawBitmap(bmp, &dst, opacity, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, &src, nullptr);
    return true;
}

bool DrawStretched(ID2D1DeviceContext* dc, Module module, const std::wstring& name, D2D1_RECT_F dst,
                   float opacity) {
    ID2D1Bitmap* bmp = Resources::Get().Bitmap(dc, module, name);
    if (!bmp) return false;
    dc->DrawBitmap(bmp, &dst, opacity, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, nullptr, nullptr);
    return true;
}

bool Draw(ID2D1DeviceContext* dc, Module module, int id, D2D1_POINT_2F at, float opacity) {
    const D2D1_SIZE_U s = Resources::Get().Size(module, id);
    if (s.width == 0) return false;
    return DrawPart(dc, module, id, D2D1::RectF(0, 0, static_cast<float>(s.width), static_cast<float>(s.height)), at,
                    opacity);
}

}  // namespace glasswave::win7
