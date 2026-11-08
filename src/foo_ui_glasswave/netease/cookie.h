#pragma once

#include <string>
#include <string_view>

namespace glasswave::netease {

// Session cookie encrypted with DPAPI (current Windows user). The plain cookie is only
// kept in memory.
bool LoadSessionCookie();
std::wstring SessionCookie();
void SaveSessionCookie(std::wstring_view cookie);
void ClearSessionCookie();
std::wstring SessionCookiePath();

}  // namespace glasswave::netease
