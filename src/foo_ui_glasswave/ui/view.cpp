#include "pch.h"
#include "ui/view.h"

namespace glasswave::ui {

std::wstring ViewKey(const View& view) {
    std::wstring key = std::to_wstring(static_cast<int>(view.library));
    key += L'|';
    key += std::to_wstring(static_cast<int>(view.kind));
    key += L'|';
    key += std::to_wstring(view.playlist);
    key += L'|';
    key += std::to_wstring(static_cast<int>(view.group));
    key += L'|';
    key += view.key;
    key += L'|';
    key += view.provider;
    key += L'|';
    key += view.remoteId;
    key += L'|';
    key += view.sectionId;
    key += L'|';
    key += view.groupId;
    key += L'|';
    key += std::to_wstring(static_cast<int>(view.shelf));
    key += view.shelfKnown ? L"|s|" : L"|u|";
    return key;
}

}  // namespace glasswave::ui
