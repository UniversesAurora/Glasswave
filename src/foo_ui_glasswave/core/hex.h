#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace glasswave::core {

// Appends one byte as two hex digits.
inline void AppendHex(std::string& out, unsigned char byte, bool upper = false) {
    const char* digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    out.push_back(digits[byte >> 4]);
    out.push_back(digits[byte & 0x0f]);
}
inline void AppendHexUpper(std::string& out, unsigned char byte) { AppendHex(out, byte, true); }

// Lower-case hex of raw bytes: two digits per byte.
inline std::string HexLower(const void* data, size_t size) {
    const auto* bytes = static_cast<const unsigned char*>(data);
    std::string out;
    out.reserve(size * 2);
    for (size_t i = 0; i < size; ++i) AppendHex(out, bytes[i]);
    return out;
}
inline std::string HexLower(std::string_view bytes) { return HexLower(bytes.data(), bytes.size()); }

}  // namespace glasswave::core
