#include "pch.h"
#include "ui/shell_glue.h"
#include "gfx/composition_host.h"

namespace glasswave::ui {

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
    if (!data.is_valid() || data->get_size() == 0) return nullptr;
    auto* wic = gfx::CompositionHost::Get().Wic();
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
    UINT w = 0, h = 0;
    frame->GetSize(&w, &h);
    if (!w || !h) return nullptr;
    const double k = std::min(1.0, static_cast<double>(sizePx) / std::max(w, h));
    const UINT tw = std::max(1u, static_cast<UINT>(w * k)), th = std::max(1u, static_cast<UINT>(h * k));
    if (FAILED(wic->CreateBitmapScaler(scaler.put())) ||
        FAILED(scaler->Initialize(frame.get(), tw, th, WICBitmapInterpolationModeHighQualityCubic)) ||
        FAILED(wic->CreateFormatConverter(conv.put())) ||
        FAILED(conv->Initialize(scaler.get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                WICBitmapPaletteTypeMedianCut)) ||
        FAILED(wic->CreateBitmapFromSource(conv.get(), WICBitmapCacheOnLoad, owned.put())))
        return nullptr;
    return owned;  // owns its pixels; the album_art_data buffer may go away
}

}  // namespace glasswave::ui
