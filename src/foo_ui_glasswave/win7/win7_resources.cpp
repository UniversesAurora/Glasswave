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
    {L"aero.msstyles", L"STREAM"},
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
    m_pieces.clear();
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
    // The theme's IMAGE resources (aero.msstyles) store premultiplied colour in plain PNGs
    // (73 % white is B9B9B9 at alpha B9); taken as straight alpha they came out grey and dark.
    // Their pixels are read unconverted and labelled premultiplied. WMP's PNGs and the DWM
    // atlas use straight alpha and are converted as usual.
    const bool premultiplied = module == Module::Aero;
    if (FAILED(wic->CreateStream(stream.put())) || FAILED(stream->InitializeFromMemory(bytes, size)) ||
        FAILED(wic->CreateDecoderFromStream(stream.get(), nullptr, WICDecodeMetadataCacheOnDemand, decoder.put())) ||
        FAILED(decoder->GetFrame(0, frame.put())) || FAILED(wic->CreateFormatConverter(converter.put())) ||
        FAILED(converter->Initialize(frame.get(),
                                     premultiplied ? GUID_WICPixelFormat32bppBGRA : GUID_WICPixelFormat32bppPBGRA,
                                     WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeMedianCut)))
        return nullptr;
    if (premultiplied) {
        UINT w = 0, h = 0;
        converter->GetSize(&w, &h);
        std::vector<BYTE> px(static_cast<size_t>(w) * h * 4);
        if (FAILED(converter->CopyPixels(nullptr, w * 4, static_cast<UINT>(px.size()), px.data())) ||
            FAILED(wic->CreateBitmapFromMemory(w, h, GUID_WICPixelFormat32bppPBGRA, w * 4,
                                               static_cast<UINT>(px.size()), px.data(), bitmap.put())))
            return nullptr;
    } else if (FAILED(wic->CreateBitmapFromSource(converter.get(), WICBitmapCacheOnLoad, bitmap.put()))) {
        return nullptr;
    }
    slot = bitmap;
    return slot.get();
}

bool Resources::Has(Module module, const std::wstring& name) { return Decode(module, name) != nullptr; }

D2D1_SIZE_U Resources::Size(Module module, const std::wstring& name) {
    UINT w = 0, h = 0;
    if (auto* src = Decode(module, name)) src->GetSize(&w, &h);
    return {w, h};
}

winrt::com_ptr<IWICBitmapSource> Resources::LoadHd(Module module, const std::wstring& name, IWICBitmapSource* base) {
    static const wchar_t* const kDirs[] = {L"wmploc", L"aero", L"atlas", L"ehres"};
    static_assert(std::size(kDirs) == static_cast<size_t>(Module::Count));
    std::wstring file = name[0] == L'#' ? name.substr(1) : name;
    const std::wstring path = m_folder + L"hd\\" + kDirs[static_cast<size_t>(module)] + L"\\" + file + L"@2x.png";
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) return nullptr;
    auto* wic = gfx::CompositionHost::Get().Wic();
    winrt::com_ptr<IWICBitmapDecoder> decoder;
    winrt::com_ptr<IWICBitmapFrameDecode> frame;
    winrt::com_ptr<IWICFormatConverter> converter;
    if (FAILED(wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand,
                                              decoder.put())) ||
        FAILED(decoder->GetFrame(0, frame.put())) || FAILED(wic->CreateFormatConverter(converter.put())) ||
        FAILED(converter->Initialize(frame.get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                     WICBitmapPaletteTypeMedianCut)))
        return nullptr;
    UINT w = 0, h = 0, bw = 0, bh = 0;
    converter->GetSize(&w, &h);
    base->GetSize(&bw, &bh);
    if (w != 2 * bw || h != 2 * bh) return nullptr;  // must be exactly twice the original
    return converter;
}

ID2D1Bitmap* Resources::Bitmap(ID2D1DeviceContext* dc, Module module, const std::wstring& name) {
    const auto key = std::make_pair(static_cast<int>(module), name);
    // Every drawing surface comes from the one D2D device, so device bitmaps can be shared.
    if (auto it = m_device.find(key); it != m_device.end()) return it->second.get();
    IWICBitmapSource* src = Decode(module, name);
    if (!src) return nullptr;
    // An optional high-resolution replacement, hd\<module>\<name>@2x.png in the artwork
    // folder (made locally from the user's own files, never shipped), is used at 192 DPI so
    // every coordinate stays in original bitmap pixels.
    float dpi = 96.0f;
    winrt::com_ptr<IWICBitmapSource> hd = LoadHd(module, name, src);
    if (hd) {
        src = hd.get();
        dpi = 192.0f;
    }
    const auto props = D2D1::BitmapProperties(
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), dpi, dpi);
    winrt::com_ptr<ID2D1Bitmap> bmp;
    if (FAILED(dc->CreateBitmapFromWicBitmap(src, &props, bmp.put()))) return nullptr;
    m_device[key] = bmp;
    return bmp.get();
}

ID2D1Bitmap* Resources::Piece(ID2D1DeviceContext* dc, Module module, const std::wstring& id, const D2D1_RECT_U& src) {
    const std::wstring key = std::to_wstring(static_cast<int>(module)) + L":" + id + L":" +
                             std::to_wstring(src.left) + L"," + std::to_wstring(src.top) + L"," +
                             std::to_wstring(src.right) + L"," + std::to_wstring(src.bottom);
    if (auto it = m_pieces.find(key); it != m_pieces.end()) return it->second.get();
    ID2D1Bitmap* whole = Bitmap(dc, module, id);
    if (!whole || src.right <= src.left || src.bottom <= src.top) return nullptr;
    const auto props = D2D1::BitmapProperties(
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);
    winrt::com_ptr<ID2D1Bitmap> piece;
    const D2D1_POINT_2U origin{0, 0};
    // A 2x replacement (192 DPI) holds twice the pixels; copy the matching area.
    float bdpi = 96, unused = 96;
    whole->GetDpi(&bdpi, &unused);
    const UINT32 k = bdpi > 96.0f ? 2 : 1;
    const D2D1_RECT_U s{src.left * k, src.top * k, src.right * k, src.bottom * k};
    const auto pieceProps = D2D1::BitmapProperties(
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f * k, 96.0f * k);
    if (FAILED(dc->CreateBitmap(D2D1::SizeU(s.right - s.left, s.bottom - s.top), nullptr, 0, k == 1 ? props : pieceProps,
                                piece.put())) ||
        FAILED(piece->CopyFromBitmap(&origin, whole, &s)))
        return nullptr;
    m_pieces[key] = piece;
    return piece.get();
}

bool DrawPart(ID2D1DeviceContext* dc, Module module, int id, D2D1_RECT_F src, D2D1_POINT_2F at, float opacity) {
    ID2D1Bitmap* bmp = Resources::Get().Bitmap(dc, module, id);
    if (!bmp) return false;
    const D2D1_RECT_F dst{at.x, at.y, at.x + (src.right - src.left), at.y + (src.bottom - src.top)};
    dc->DrawBitmap(bmp, &dst, opacity, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, &src, nullptr);
    return true;
}

namespace {
// Nearest-neighbour when a piece maps whole bitmap pixels onto whole device pixels
// (crisp, e.g. 1:1 or 2:1 at 200 %), high-quality cubic otherwise.
D2D1_INTERPOLATION_MODE ModeFor(ID2D1DeviceContext* dc, const D2D1_RECT_F& src, const D2D1_RECT_F& dst) {
    float dpiX = 96, dpiY = 96;
    dc->GetDpi(&dpiX, &dpiY);
    const float sx = (dst.right - dst.left) * dpiX / 96.0f / std::max(src.right - src.left, 1e-3f);
    const float sy = (dst.bottom - dst.top) * dpiY / 96.0f / std::max(src.bottom - src.top, 1e-3f);
    auto whole = [](float v) { return v >= 1.0f && std::abs(v - std::round(v)) < 1e-3f; };
    return whole(sx) && whole(sy) ? D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR
                                  : D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC;
}
}  // namespace

bool DrawNineGrid(ID2D1DeviceContext* dc, Module module, const std::wstring& id, D2D1_RECT_F src, D2D1_RECT_F m,
                  D2D1_RECT_F dst, float opacity) {
    if (!Resources::Get().Bitmap(dc, module, id)) return false;
    float dpiX = 96, dpiY = 96;
    dc->GetDpi(&dpiX, &dpiY);
    const float px = 96.0f / dpiX;  // DIP per device pixel
    // Fixed borders shrink proportionally when the target is smaller than they are.
    const float kx = std::min(px, (dst.right - dst.left) / std::max(m.left + m.right, 1e-3f));
    const float ky = std::min(px, (dst.bottom - dst.top) / std::max(m.top + m.bottom, 1e-3f));
    const float sx[4] = {src.left, src.left + m.left, src.right - m.right, src.right};
    const float sy[4] = {src.top, src.top + m.top, src.bottom - m.bottom, src.bottom};
    const float dx[4] = {dst.left, dst.left + m.left * kx, dst.right - m.right * kx, dst.right};
    const float dy[4] = {dst.top, dst.top + m.top * ky, dst.bottom - m.bottom * ky, dst.bottom};
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) {
            const D2D1_RECT_F s{sx[c], sy[r], sx[c + 1], sy[r + 1]}, d{dx[c], dy[r], dx[c + 1], dy[r + 1]};
            if (s.right - s.left <= 0 || s.bottom - s.top <= 0 || d.right - d.left <= 0 || d.bottom - d.top <= 0)
                continue;
            const D2D1_RECT_U su{static_cast<UINT32>(s.left), static_cast<UINT32>(s.top), static_cast<UINT32>(s.right),
                                 static_cast<UINT32>(s.bottom)};
            if (ID2D1Bitmap* piece = Resources::Get().Piece(dc, module, id, su))
                dc->DrawBitmap(piece, &d, opacity, ModeFor(dc, s, d), nullptr, nullptr);
        }
    return true;
}
bool DrawStretched(ID2D1DeviceContext* dc, Module module, const std::wstring& name, D2D1_RECT_F dst,
                   float opacity) {
    ID2D1Bitmap* bmp = Resources::Get().Bitmap(dc, module, name);
    if (!bmp) return false;
    dc->DrawBitmap(bmp, &dst, opacity, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, nullptr, nullptr);
    return true;
}

HICON Resources::Icon(Module module, int id, int size) const {
    HMODULE mod = Handle(module);
    if (!mod) return nullptr;
    return static_cast<HICON>(LoadImageW(mod, MAKEINTRESOURCEW(id), IMAGE_ICON, size, size, LR_DEFAULTCOLOR));
}

HBITMAP Resources::DibBitmap(Module module, int id) const {
    HMODULE mod = Handle(module);
    if (!mod) return nullptr;
    return static_cast<HBITMAP>(LoadImageW(mod, MAKEINTRESOURCEW(id), IMAGE_BITMAP, 0, 0, LR_CREATEDIBSECTION));
}

bool DrawPartTo(ID2D1DeviceContext* dc, Module module, const std::wstring& name, D2D1_RECT_U src, D2D1_RECT_F dst,
                float opacity) {
    ID2D1Bitmap* piece = Resources::Get().Piece(dc, module, name, src);
    if (!piece) return false;
    dc->DrawBitmap(piece, &dst, opacity, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, nullptr, nullptr);
    return true;
}

bool Draw(ID2D1DeviceContext* dc, Module module, int id, D2D1_POINT_2F at, float opacity) {
    const D2D1_SIZE_U s = Resources::Get().Size(module, id);
    if (s.width == 0) return false;
    return DrawPart(dc, module, id, D2D1::RectF(0, 0, static_cast<float>(s.width), static_cast<float>(s.height)), at,
                    opacity);
}

}  // namespace glasswave::win7
