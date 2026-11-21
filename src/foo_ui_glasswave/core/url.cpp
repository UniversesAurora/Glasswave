#include "pch.h"
#include "core/url.h"

#include "core/hex.h"
#include "core/text.h"

namespace glasswave::core {

std::string UrlEncode(std::wstring_view value) {
    const std::string utf8 = Utf8(value);
    std::string out;
    out.reserve(utf8.size() * 3);
    for (const unsigned char c : utf8) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' ||
            c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            AppendHexUpper(out, c);
        }
    }
    return out;
}

} // namespace glasswave::core
