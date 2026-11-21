#pragma once

#include <string>
#include <string_view>

namespace glasswave::core {

// UTF-16 <-> UTF-8 with explicit lengths: embedded NULs survive, invalid sequences become
// U+FFFD, and the functions never throw.
std::string Utf8(std::wstring_view text);
std::wstring Wide(std::string_view utf8);
// For C strings from the SDK; null is the empty string.
std::wstring Wide(const char* utf8);

// Same conversions, but invalid input is an error: throws std::runtime_error. For validating
// data read from disk or the network, where substituting characters would hide corruption.
std::string Utf8Strict(std::wstring_view text);
std::wstring WideStrict(std::string_view utf8);

// Folds A-Z only; every other character is kept as is.
std::wstring LowerAscii(std::wstring_view text);
std::string LowerAscii(std::string_view text);

} // namespace glasswave::core
