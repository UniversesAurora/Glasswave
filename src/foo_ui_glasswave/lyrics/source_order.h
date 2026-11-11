#pragma once

#include <algorithm>
#include <cctype>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace glasswave::lyrics::source_order {

struct Entry {
    std::wstring key;
    std::wstring orderKey;
    bool enabled = true;
    bool applicable = true;
};

inline std::vector<std::wstring> Tokens(std::wstring_view text) {
    std::vector<std::wstring> result;
    size_t start = 0;
    while (start <= text.size()) {
        const size_t end = text.find(L',', start);
        const size_t length = end == std::wstring_view::npos ? text.size() - start : end - start;
        std::wstring token(text.substr(start, length));
        token.erase(token.begin(),
                    std::find_if(token.begin(), token.end(), [](wchar_t ch) { return !iswspace(ch); }));
        token.erase(
            std::find_if(token.rbegin(), token.rend(), [](wchar_t ch) { return !iswspace(ch); }).base(),
            token.end());
        if (!token.empty()) result.push_back(std::move(token));
        if (end == std::wstring_view::npos) break;
        start = end + 1;
    }
    return result;
}

inline std::vector<size_t> OrderedIndexes(const std::vector<Entry>& entries, std::wstring_view orderText) {
    const auto tokens = Tokens(orderText);
    std::vector<size_t> indexes;
    std::vector<bool> included(entries.size(), false);
    for (size_t i = 0; i < entries.size(); ++i) {
        const auto& entry = entries[i];
        if (entry.enabled && entry.applicable &&
            std::find(tokens.begin(), tokens.end(), entry.orderKey) != tokens.end()) {
            indexes.push_back(i);
            included[i] = true;
        }
    }
    std::stable_sort(indexes.begin(), indexes.end(), [&](size_t left, size_t right) {
        return std::find(tokens.begin(), tokens.end(), entries[left].orderKey) <
               std::find(tokens.begin(), tokens.end(), entries[right].orderKey);
    });
    // The order string is priority, not an allow-list. Newly installed providers remain
    // discoverable and runnable unless their explicit checkbox disables them.
    for (size_t i = 0; i < entries.size(); ++i)
        if (entries[i].enabled && entries[i].applicable && !included[i]) indexes.push_back(i);
    return indexes;
}

inline std::optional<size_t> CachedSourceIndex(const std::vector<Entry>& entries,
                                               const std::vector<size_t>& ordered,
                                               std::wstring_view sourceKey, bool cacheable) {
    if (!cacheable) return std::nullopt;
    for (size_t position = 0; position < ordered.size(); ++position)
        if (entries[ordered[position]].key == sourceKey) return position;
    return std::nullopt;
}

} // namespace glasswave::lyrics::source_order
