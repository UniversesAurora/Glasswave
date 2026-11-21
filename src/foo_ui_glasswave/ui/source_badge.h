#pragma once

#include <functional>
#include <string_view>

namespace glasswave::ui {

// The mark of a result's source on a cover in results mixed from several sources (owner,
// 2026-11-15): the icon of the service's own desktop app as the Start menu shows it
// (stream::Provider::AppNames), Windows Media Player's icon for the local library. Read at run
// time from what is installed; nothing is drawn when the app is not. `source` is a provider
// key or "local"; `box` the mark's square in DIP.
void DrawSourceBadge(ID2D1DeviceContext* dc, D2D1_RECT_F box, std::wstring_view source, float opacity = 1.0f);

// Called when an icon was read (the Start menu is read on a worker): repaint the marks.
int SubscribeSourceIcons(std::function<void()> ready);
void UnsubscribeSourceIcons(int token);

}  // namespace glasswave::ui
