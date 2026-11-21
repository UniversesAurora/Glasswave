#include "pch.h"
#include "core/tsv.h"

namespace glasswave::core::tsv {

std::string Escape(std::string_view utf8) {
    std::string out;
    out.reserve(utf8.size());
    for (const char c : utf8) {
        switch (c) {
        case '%': out += "%25"; break;
        case '\t': out += "%09"; break;
        case '\n': out += "%0A"; break;
        case '\r': out += "%0D"; break;
        default: out += c; break;
        }
    }
    return out;
}

std::string Unescape(std::string_view field) {
    const auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        return -1;
    };
    std::string out;
    out.reserve(field.size());
    for (size_t i = 0; i < field.size(); ++i) {
        if (field[i] == '%' && i + 2 < field.size()) {
            const int hi = hex(field[i + 1]), lo = hex(field[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out += static_cast<char>(hi * 16 + lo);
                i += 2;
                continue;
            }
        }
        out += field[i];
    }
    return out;
}

std::vector<std::string_view> Split(std::string_view text, char separator) {
    std::vector<std::string_view> parts;
    size_t start = 0;
    while (true) {
        const size_t end = text.find(separator, start);
        parts.push_back(text.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start));
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    return parts;
}

} // namespace glasswave::core::tsv
