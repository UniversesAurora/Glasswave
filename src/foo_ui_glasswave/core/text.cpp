#include "core/text.h"

#include <windows.h>

#include <climits>
#include <stdexcept>

namespace glasswave::core {

namespace {

// `strict` rejects invalid sequences instead of replacing them.
std::string ToUtf8(std::wstring_view text, bool strict) {
    if (text.empty()) return {};
    if (text.size() > static_cast<size_t>(INT_MAX)) {
        if (strict) throw std::runtime_error("Text is too long for UTF-8 conversion.");
        return {};
    }
    const DWORD flags = strict ? WC_ERR_INVALID_CHARS : 0;
    const int length = static_cast<int>(text.size());
    const int size = WideCharToMultiByte(CP_UTF8, flags, text.data(), length, nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        if (strict) throw std::runtime_error("Text is not valid UTF-16.");
        return {};
    }
    std::string out(static_cast<size_t>(size), '\0');
    if (WideCharToMultiByte(CP_UTF8, flags, text.data(), length, out.data(), size, nullptr, nullptr) !=
        size) {
        if (strict) throw std::runtime_error("UTF-8 conversion failed.");
        return {};
    }
    return out;
}

std::wstring ToWide(std::string_view utf8, bool strict) {
    if (utf8.empty()) return {};
    if (utf8.size() > static_cast<size_t>(INT_MAX)) {
        if (strict) throw std::runtime_error("Text is too long for UTF-16 conversion.");
        return {};
    }
    const DWORD flags = strict ? MB_ERR_INVALID_CHARS : 0;
    const int length = static_cast<int>(utf8.size());
    const int size = MultiByteToWideChar(CP_UTF8, flags, utf8.data(), length, nullptr, 0);
    if (size <= 0) {
        if (strict) throw std::runtime_error("Text is not valid UTF-8.");
        return {};
    }
    std::wstring out(static_cast<size_t>(size), L'\0');
    if (MultiByteToWideChar(CP_UTF8, flags, utf8.data(), length, out.data(), size) != size) {
        if (strict) throw std::runtime_error("UTF-16 conversion failed.");
        return {};
    }
    return out;
}

template <class String>
String Fold(typename String::value_type a, typename String::value_type z, String text) {
    for (auto& c : text)
        if (c >= a && c <= z) c = static_cast<typename String::value_type>(c + ('a' - 'A'));
    return text;
}

} // namespace

std::string Utf8(std::wstring_view text) {
    return ToUtf8(text, false);
}
std::wstring Wide(std::string_view utf8) {
    return ToWide(utf8, false);
}
std::wstring Wide(const char* utf8) {
    return utf8 ? ToWide(utf8, false) : std::wstring();
}
std::string Utf8Strict(std::wstring_view text) {
    return ToUtf8(text, true);
}
std::wstring WideStrict(std::string_view utf8) {
    return ToWide(utf8, true);
}

std::wstring LowerAscii(std::wstring_view text) {
    return Fold<std::wstring>(L'A', L'Z', std::wstring(text));
}
std::string LowerAscii(std::string_view text) {
    return Fold<std::string>('A', 'Z', std::string(text));
}

} // namespace glasswave::core
