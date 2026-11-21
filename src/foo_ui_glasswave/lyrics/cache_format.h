#pragma once

#include <cstddef>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>

namespace glasswave::lyrics::cache_format {

inline constexpr wchar_t kSourceTag[] = L"[glasswave-source:";
inline constexpr wchar_t kSourceKeyTag[] = L"[glasswave-source-key:";
inline constexpr wchar_t kNotFoundTag[] = L"[glasswave-notfound:";
inline constexpr wchar_t kCacheProviderTag[] = L"[glasswave-cache-provider:";
inline constexpr wchar_t kCacheScopeTag[] = L"[glasswave-cache-scope:";
inline constexpr wchar_t kFileTag[] = L"[glasswave-file:";          // a selection read from a file
inline constexpr wchar_t kCodePageTag[] = L"[glasswave-codepage:";  // ... in this code page
inline constexpr wchar_t kNoneTag[] = L"[glasswave-none:";          // the owner chose no lyrics

struct Headers {
    std::wstring source;
    std::wstring sourceKey;
    std::wstring provider;
    std::wstring scope;
    std::wstring file;
    unsigned codePage = 0;
    bool none = false;
    std::optional<long long> notFoundAt;
    size_t body = 0;
};

// The automatic-cache flag never suppresses owner-authored per-track selections.
inline bool ManualSelectionApplies(bool useAutomaticCache) {
    (void)useAutomaticCache;
    return true;
}

// A header value runs to the first ']': a path's '%' and ']' are written as %25 and %5D.
inline std::wstring EscapeValue(std::wstring_view value) {
    std::wstring out;
    for (const wchar_t c : value) {
        if (c == L'%') out += L"%25";
        else if (c == L']') out += L"%5D";
        else if (c == L'\r' || c == L'\n') continue;
        else out += c;
    }
    return out;
}

inline std::wstring UnescapeValue(std::wstring_view value) {
    std::wstring out;
    for (size_t i = 0; i < value.size(); ++i) {
        if (value.substr(i, 3) == L"%25") {
            out += L'%';
            i += 2;
        } else if (value.substr(i, 3) == L"%5D") {
            out += L']';
            i += 2;
        } else {
            out += value[i];
        }
    }
    return out;
}

inline std::optional<std::wstring> HeaderValue(std::wstring_view line, std::wstring_view prefix) {
    if (!line.starts_with(prefix) || line.empty() || line.back() != L']') return std::nullopt;
    return std::wstring(line.substr(prefix.size(), line.size() - prefix.size() - 1));
}

inline Headers Parse(std::wstring_view text) {
    Headers headers;
    size_t offset = 0;
    while (offset < text.size() && text[offset] == L'[') {
        const size_t close = text.find(L']', offset);
        if (close == std::wstring_view::npos) break;
        const std::wstring_view line = text.substr(offset, close - offset + 1);
        if (const auto source = HeaderValue(line, kSourceTag))
            headers.source = *source;
        else if (const auto sourceKey = HeaderValue(line, kSourceKeyTag))
            headers.sourceKey = *sourceKey;
        else if (const auto provider = HeaderValue(line, kCacheProviderTag))
            headers.provider = *provider;
        else if (const auto scope = HeaderValue(line, kCacheScopeTag))
            headers.scope = *scope;
        else if (const auto notFound = HeaderValue(line, kNotFoundTag))
            headers.notFoundAt = _wtoi64(notFound->c_str());
        else if (const auto file = HeaderValue(line, kFileTag))
            headers.file = UnescapeValue(*file);
        else if (const auto codePage = HeaderValue(line, kCodePageTag))
            headers.codePage = static_cast<unsigned>(_wtoi(codePage->c_str()));
        else if (HeaderValue(line, kNoneTag))
            headers.none = true;
        else
            break;
        offset = close + 1;
        if (offset < text.size() && text[offset] == L'\r') ++offset;
        if (offset < text.size() && text[offset] == L'\n') ++offset;
        headers.body = offset;
    }
    return headers;
}

inline std::wstring Manual(std::wstring_view source, std::wstring_view lrc, std::wstring_view sourceKey = {},
                           std::wstring_view file = {}, unsigned codePage = 0) {
    std::wstring text;
    if (!source.empty()) text += kSourceTag + std::wstring(source) + L"]\n";
    if (!sourceKey.empty()) text += kSourceKeyTag + std::wstring(sourceKey) + L"]\n";
    if (!file.empty()) text += kFileTag + EscapeValue(file) + L"]\n";
    if (codePage != 0) text += kCodePageTag + std::to_wstring(codePage) + L"]\n";
    return text + std::wstring(lrc);
}

// A selection of no lyrics at all for the track.
inline std::wstring ManualNone() { return std::wstring(kNoneTag) + L"1]\n"; }

inline std::wstring Automatic(std::wstring_view source, std::wstring_view sourceKey,
                              std::wstring_view provider, std::wstring_view scope, std::wstring_view lrc) {
    std::wstring text;
    if (!source.empty()) text += kSourceTag + std::wstring(source) + L"]\n";
    if (!sourceKey.empty()) text += kSourceKeyTag + std::wstring(sourceKey) + L"]\n";
    if (!provider.empty() && !scope.empty()) {
        text += kCacheProviderTag + std::wstring(provider) + L"]\n";
        text += kCacheScopeTag + std::wstring(scope) + L"]\n";
    }
    text += lrc;
    return text;
}

} // namespace glasswave::lyrics::cache_format
