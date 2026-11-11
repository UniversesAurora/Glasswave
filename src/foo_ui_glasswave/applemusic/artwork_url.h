#pragma once

#include <string_view>

namespace glasswave::applemusic {

namespace detail {

inline bool EqualsAsciiIgnoreCase(std::wstring_view a, std::wstring_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        wchar_t left = a[i], right = b[i];
        if (left >= L'A' && left <= L'Z') left = static_cast<wchar_t>(left + L'a' - L'A');
        if (right >= L'A' && right <= L'Z') right = static_cast<wchar_t>(right + L'a' - L'A');
        if (left != right) return false;
    }
    return true;
}

inline bool EndsWithAsciiIgnoreCase(std::wstring_view text, std::wstring_view suffix) {
    return text.size() >= suffix.size() &&
           EqualsAsciiIgnoreCase(text.substr(text.size() - suffix.size()), suffix);
}

inline bool IsHexDigit(wchar_t c) {
    return (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f') || (c >= L'A' && c <= L'F');
}

inline int HexValue(wchar_t c) {
    if (c >= L'0' && c <= L'9') return c - L'0';
    if (c >= L'a' && c <= L'f') return c - L'a' + 10;
    return c - L'A' + 10;
}

inline bool HasValidEncoding(std::wstring_view url) {
    for (size_t i = 0; i < url.size(); ++i) {
        const wchar_t c = url[i];
        if (c < 0x21 || c > 0x7e || c == L'\\') return false;
        if (c != L'%') continue;
        if (i + 2 >= url.size() || !IsHexDigit(url[i + 1]) || !IsHexDigit(url[i + 2])) return false;
        const int decoded = (HexValue(url[i + 1]) << 4) | HexValue(url[i + 2]);
        if (decoded < 0x20 || decoded == 0x7f) return false;
        i += 2;
    }
    return true;
}

inline bool IsValidDnsHost(std::wstring_view host) {
    if (host.empty() || host.size() > 253 || host.front() == L'.' || host.back() == L'.') return false;
    size_t labelSize = 0;
    for (size_t i = 0; i <= host.size(); ++i) {
        if (i < host.size() && host[i] != L'.') {
            const wchar_t c = host[i];
            if (!((c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9') ||
                  c == L'-'))
                return false;
            if (c == L'-' && labelSize == 0) return false;
            if (++labelSize > 63) return false;
            continue;
        }
        if (labelSize == 0 || host[i - 1] == L'-') return false;
        labelSize = 0;
    }
    return true;
}

} // namespace detail

// Validates Apple's image origins without normalizing the generated or signed URL. Both public
// mzstatic artwork and user-uploaded blobstore artwork are accepted. Query data is never logged.
inline bool IsAllowedAppleArtworkUrl(std::wstring_view url) {
    constexpr std::wstring_view scheme = L"https://";
    if (url.size() <= scheme.size() || !detail::EqualsAsciiIgnoreCase(url.substr(0, scheme.size()), scheme))
        return false;
    if (!detail::HasValidEncoding(url)) return false;

    const size_t authorityStart = scheme.size();
    const size_t authorityEnd = url.find_first_of(L"/?#", authorityStart);
    const std::wstring_view host =
        url.substr(authorityStart, authorityEnd == std::wstring_view::npos ? std::wstring_view::npos
                                                                           : authorityEnd - authorityStart);
    if (host.empty() || host.find(L'@') != std::wstring_view::npos || !detail::IsValidDnsHost(host))
        return false;
    return detail::EndsWithAsciiIgnoreCase(host, L".mzstatic.com") ||
           detail::EndsWithAsciiIgnoreCase(host, L".blobstore.apple.com");
}

} // namespace glasswave::applemusic
