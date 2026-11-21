#pragma once

#include "stream/provider.h"

#include <string>
#include <vector>

// Where the search box searches (owner, 2026-11-15): the list on screen, filtered as WMP does,
// or a catalog, as Windows 7 Explorer's search filters pick one ("搜索范围:" links under the
// drop-down, the chosen one shown as a blue token at the start of the box). Every source at
// once is the mixed search: the local library and each provider that can search.
namespace glasswave::ui {

struct SearchScope {
    enum class Kind { List, All, Local, Provider };
    Kind kind = Kind::List;
    std::wstring provider; // Provider: its key
    bool operator==(const SearchScope&) const = default;
    bool Catalog() const { return kind != Kind::List; }
};

// The scopes offered, in order: the list, every source, the local library, each provider that
// can search.
std::vector<SearchScope> SearchScopes();
std::wstring ScopeLabel(const SearchScope& scope); // translated
stream::Provider* ScopeProvider(const SearchScope& scope);

// What was searched before, newest first (catalog searches only; the list filter is not kept).
std::vector<std::wstring> SearchHistory();
void RememberSearch(const std::wstring& term);
void ForgetSearch(const std::wstring& term);
void ClearSearchHistory();

} // namespace glasswave::ui
