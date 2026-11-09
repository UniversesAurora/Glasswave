#pragma once

#include "app/setting_fields.h"
#include "lyrics/lyrics_service.h"

namespace glasswave::lyrics::scripts {

void Start();
void Reload();
void Shutdown();
std::vector<std::shared_ptr<Searcher>> Ordered(std::wstring_view provider);
std::vector<settings::Field> SettingFields();

} // namespace glasswave::lyrics::scripts
