#include "pch.h"
#include "ui/shell_glue.h"
#include "core/text.h"
#include "cache/images.h"
#include "gfx/composition_host.h"
#include "ui/i18n.h"
#include "ui/text.h"

#include <commdlg.h>
#include <filesystem>
#include <fstream>
#include <thread>

namespace glasswave::ui {

using core::Wide;


namespace {

// The picture's own type from its first bytes; the file keeps the provider's encoding.
const wchar_t* ArtExtension(const album_art_data::ptr& data) {
    const auto* b = static_cast<const uint8_t*>(data->get_ptr());
    const size_t n = data->get_size();
    if (n >= 8 && b[0] == 0x89 && b[1] == 'P' && b[2] == 'N' && b[3] == 'G') return L"png";
    if (n >= 12 && std::memcmp(b, "RIFF", 4) == 0 && std::memcmp(b + 8, "WEBP", 4) == 0) return L"webp";
    if (n >= 4 && std::memcmp(b, "GIF8", 4) == 0) return L"gif";
    if (n >= 2 && b[0] == 'B' && b[1] == 'M') return L"bmp";
    return L"jpg";
}

void SaveArtData(HWND owner, const album_art_data::ptr& data, std::wstring name) {
    if (!IsWindow(owner)) return;
    if (!data.is_valid() || data->get_size() == 0) {
        MessageBoxW(owner, Tr(L"This song has no album art.").c_str(), Tr(L"Save album art").c_str(),
                    MB_OK | MB_ICONINFORMATION);
        return;
    }
    const std::wstring ext = ArtExtension(data);
    wchar_t file[MAX_PATH]{};
    wcsncpy_s(file, (name + L"." + ext).c_str(), _TRUNCATE);
    const std::wstring filter = Tr(L"Picture") + L" (*." + ext + L")" + L'\0' + L"*." + ext + L'\0' + L'\0';
    OPENFILENAMEW ofn{sizeof(ofn)};
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = filter.c_str();
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrDefExt = ext.c_str();
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetSaveFileNameW(&ofn)) return;
    std::ofstream out(std::filesystem::path(file), std::ios::binary | std::ios::trunc);
    out.write(static_cast<const char*>(data->get_ptr()), static_cast<std::streamsize>(data->get_size()));
    if (!out)
        MessageBoxW(owner, Tr(L"The picture could not be saved.").c_str(), Tr(L"Save album art").c_str(),
                    MB_OK | MB_ICONWARNING);
}

}  // namespace

void SaveAlbumArt(HWND owner, const metadb_handle_ptr& track) {
    if (!track.is_valid()) return;
    static titleformat_object::ptr script;
    if (!script)
        titleformat_compiler::get()->compile_safe(script, "$if2(%album artist%,%artist%)$if(%album%, - %album%,)");
    pfc::string8 text;
    track->format_title(nullptr, text, script, nullptr);
    std::wstring name = Wide(text.c_str());
    for (auto& c : name)
        if (wcschr(L"\\/:*?\"<>|", c) || c < 32) c = L'_';
    while (!name.empty() && (name.back() == L'.' || name.back() == L' ')) name.pop_back();
    if (name.empty()) name = L"Cover";
    // The cover may come from a provider over the network: never on the main thread.
    std::thread([owner, track, name = std::move(name)]() mutable {
        album_art_data::ptr data;
        try {
            const cache::SizeHintScope original(cache::kOriginalSize); // the provider's largest picture
            abort_callback_dummy abort;
            auto extractor = album_art_manager_v2::get()->open(pfc::list_single_ref_t<metadb_handle_ptr>(track),
                                                               pfc::list_single_ref_t<GUID>(album_art_ids::cover_front),
                                                               abort);
            data = extractor->query(album_art_ids::cover_front, abort);
        } catch (...) {
            // No cover: said so below.
        }
        fb2k::inMainThread([owner, data, name = std::move(name)]() mutable { SaveArtData(owner, data, std::move(name)); });
    }).detach();
}

namespace {

struct FileDropTarget : winrt::implements<FileDropTarget, ::IDropTarget> {
    explicit FileDropTarget(std::function<void(IDataObject*)> onDrop) : m_onDrop(std::move(onDrop)) {}

    static bool HasFiles(IDataObject* data) {
        FORMATETC fmt{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
        return data && data->QueryGetData(&fmt) == S_OK;
    }

    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* data, DWORD, POINTL, DWORD* effect) noexcept override {
        m_accept = HasFiles(data);
        *effect = m_accept ? DROPEFFECT_COPY : DROPEFFECT_NONE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL, DWORD* effect) noexcept override {
        *effect = m_accept ? DROPEFFECT_COPY : DROPEFFECT_NONE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragLeave() noexcept override { return S_OK; }
    HRESULT STDMETHODCALLTYPE Drop(IDataObject* data, DWORD, POINTL, DWORD* effect) noexcept override {
        *effect = DROPEFFECT_NONE;
        if (!HasFiles(data)) return S_OK;
        *effect = DROPEFFECT_COPY;
        try {
            m_onDrop(data);
        } catch (...) {
        }
        return S_OK;
    }

    std::function<void(IDataObject*)> m_onDrop;
    bool m_accept = false;
};

}  // namespace

winrt::com_ptr<IDropTarget> MakeFileDropTarget(std::function<void(IDataObject*)> onDrop) {
    return winrt::make<FileDropTarget>(std::move(onDrop)).as<IDropTarget>();
}

winrt::com_ptr<IWICBitmapSource> DecodeArt(const album_art_data::ptr& data, int sizePx) {
    return DecodeArt(gfx::CompositionHost::Get().Wic(), data, sizePx);
}

winrt::com_ptr<IWICBitmapSource> DecodeArt(IWICImagingFactory* wic, const album_art_data::ptr& data, int sizePx) {
    if (!data.is_valid() || data->get_size() == 0) return nullptr;
    winrt::com_ptr<IWICStream> stream;
    winrt::com_ptr<IWICBitmapDecoder> decoder;
    winrt::com_ptr<IWICBitmapFrameDecode> frame;
    winrt::com_ptr<IWICBitmapScaler> scaler;
    winrt::com_ptr<IWICFormatConverter> conv;
    winrt::com_ptr<IWICBitmap> owned;
    if (FAILED(wic->CreateStream(stream.put())) ||
        FAILED(stream->InitializeFromMemory(static_cast<BYTE*>(const_cast<void*>(data->get_ptr())),
                                            static_cast<DWORD>(data->get_size()))) ||
        FAILED(wic->CreateDecoderFromStream(stream.get(), nullptr, WICDecodeMetadataCacheOnDemand, decoder.put())) ||
        FAILED(decoder->GetFrame(0, frame.put())))
        return nullptr;
    // A large JPEG often carries an EXIF thumbnail. When it is at least the requested size,
    // scale from it instead of decoding the full image: the visible difference at list-cover
    // sizes is nil, and large scans stop stalling the art worker.
    winrt::com_ptr<IWICBitmapSource> source = frame;
    winrt::com_ptr<IWICBitmapSource> thumbnail;
    if (SUCCEEDED(frame->GetThumbnail(thumbnail.put())) && thumbnail) {
        UINT tw = 0, th = 0;
        if (SUCCEEDED(thumbnail->GetSize(&tw, &th)) && std::max(tw, th) >= static_cast<UINT>(sizePx))
            source = thumbnail;
    }
    UINT w = 0, h = 0;
    source->GetSize(&w, &h);
    if (!w || !h) return nullptr;
    const double k = std::min(1.0, static_cast<double>(sizePx) / std::max(w, h));
    const UINT tw = std::max(1u, static_cast<UINT>(w * k)), th = std::max(1u, static_cast<UINT>(h * k));
    if (FAILED(wic->CreateBitmapScaler(scaler.put())) ||
        FAILED(scaler->Initialize(source.get(), tw, th, WICBitmapInterpolationModeHighQualityCubic)) ||
        FAILED(wic->CreateFormatConverter(conv.put())) ||
        FAILED(conv->Initialize(scaler.get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                WICBitmapPaletteTypeMedianCut)) ||
        FAILED(wic->CreateBitmapFromSource(conv.get(), WICBitmapCacheOnLoad, owned.put())))
        return nullptr;
    return owned;  // owns its pixels; the album_art_data buffer may go away
}

}  // namespace glasswave::ui
