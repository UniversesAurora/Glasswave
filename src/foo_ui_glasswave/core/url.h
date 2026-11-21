#pragma once

#include <string>
#include <string_view>

namespace glasswave::core {

// Percent-encodes the UTF-8 bytes of `value`; only unreserved characters (RFC 3986) pass through.
std::string UrlEncode(std::wstring_view value);

}  // namespace glasswave::core
