#pragma once

#include "lyrics/lyrics_service.h"

namespace glasswave::ui {
void ShowLyricsSearch(HWND owner, const metadb_handle_ptr& track, std::wstring source,
                      std::function<void(const lyrics::Candidate&)> selected);
void CloseLyricsSearch();
std::wstring PickLyricsFile(HWND owner);
} // namespace glasswave::ui
