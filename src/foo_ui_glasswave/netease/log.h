#pragma once

#include <string>
#include <string_view>

namespace glasswave::netease {

// Appends one line to <profile>\glasswave\netease\glasswave-netease.log.
// Never pass cookies or other credentials in `message`.
void Log(std::wstring_view message);
void LogHttp(std::wstring_view method, std::wstring_view path, unsigned long status, long long elapsedMs);
std::wstring LogPath();

}  // namespace glasswave::netease
