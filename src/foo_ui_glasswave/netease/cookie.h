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

// Keeps only non-empty name=value pairs, drops Set-Cookie attributes. The QR service
// returns Set-Cookie lines joined together; forwarding them verbatim can leave an empty
// MUSIC_U as the last value.
std::wstring NormalizeCookie(std::wstring_view raw);
bool CookieHasMusicU(std::wstring_view cookie);

}  // namespace glasswave::netease
