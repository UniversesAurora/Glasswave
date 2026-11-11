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

struct Headers {
    std::wstring source;
    std::wstring sourceKey;
    std::wstring provider;
    std::wstring scope;
    std::optional<long long> notFoundAt;
    size_t body = 0;
};

// The automatic-cache flag never suppresses owner-authored per-track selections.
inline bool ManualSelectionApplies(bool useAutomaticCache) {
    (void)useAutomaticCache;
    return true;
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
        else
            break;
        offset = close + 1;
        if (offset < text.size() && text[offset] == L'\r') ++offset;
        if (offset < text.size() && text[offset] == L'\n') ++offset;
        headers.body = offset;
    }
    return headers;
}

inline std::wstring Manual(std::wstring_view source, std::wstring_view lrc) {
    return source.empty() ? std::wstring(lrc)
                          : kSourceTag + std::wstring(source) + L"]\n" + std::wstring(lrc);
}

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
