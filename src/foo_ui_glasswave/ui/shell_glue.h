#pragma once

namespace glasswave::ui {

// OLE drop target that accepts files from Explorer (CF_HDROP).
winrt::com_ptr<IDropTarget> MakeFileDropTarget(std::function<void(IDataObject*)> onDrop);

// Decode foobar2000 album art into a premultiplied BGRA bitmap of at most sizePx square.
winrt::com_ptr<IWICBitmapSource> DecodeArt(const album_art_data::ptr& data, int sizePx);

}  // namespace glasswave::ui
