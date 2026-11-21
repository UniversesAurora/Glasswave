#include "pch.h"
#include "ui/search_scope.h"
#include "core/text.h"

#include "ui/i18n.h"
#include "ui/text.h"

namespace glasswave::ui {

using core::Utf8;
using core::Wide;

namespace {

constexpr size_t kHistoryLimit = 20;

// One term per line, newest first.
cfg_var_modern::cfg_string cfg_searchHistory({0x8d3a51d2, 0x6e2f, 0x4b19, {0xa4, 0x7c, 0x1f, 0x5e, 0x9b, 0x2d, 0x7c, 0x40}},
                                             "");

void Store(const std::vector<std::wstring>& terms) {
    std::wstring text;
    for (const auto& term : terms) {
        if (!text.empty()) text += L'\n';
        text += term;
    }
    cfg_searchHistory.set(Utf8(text).c_str());
}

} // namespace

std::vector<SearchScope> SearchScopes() {
    std::vector<SearchScope> scopes{{SearchScope::Kind::List, {}},
                                    {SearchScope::Kind::All, {}},
                                    {SearchScope::Kind::Local, {}}};
    for (auto* provider : stream::Providers())
        if (provider->Can(stream::Capability::CatalogSearch))
            scopes.push_back({SearchScope::Kind::Provider, std::wstring(provider->Key())});
    return scopes;
}

std::wstring ScopeLabel(const SearchScope& scope) {
    switch (scope.kind) {
    case SearchScope::Kind::List: return Tr(L"Current list");
    case SearchScope::Kind::All: return Tr(L"All sources");
    case SearchScope::Kind::Local: return Tr(L"Local Music");
    case SearchScope::Kind::Provider:
        if (auto* provider = ScopeProvider(scope)) return Tr(provider->Name());
        return scope.provider;
    }
    return {};
}

stream::Provider* ScopeProvider(const SearchScope& scope) {
    return scope.kind == SearchScope::Kind::Provider ? stream::FindProvider(scope.provider) : nullptr;
}

std::vector<std::wstring> SearchHistory() {
    std::vector<std::wstring> terms;
    const std::wstring text = Wide(cfg_searchHistory.get());
    size_t start = 0;
    while (start < text.size()) {
        const size_t end = text.find(L'\n', start);
        std::wstring term = text.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start);
        if (!term.empty()) terms.push_back(std::move(term));
        if (end == std::wstring::npos) break;
        start = end + 1;
    }
    return terms;
}

void RememberSearch(const std::wstring& term) {
    if (term.empty()) return;
    auto terms = SearchHistory();
    std::erase(terms, term);
    terms.insert(terms.begin(), term);
    if (terms.size() > kHistoryLimit) terms.resize(kHistoryLimit);
    Store(terms);
}

void ForgetSearch(const std::wstring& term) {
    auto terms = SearchHistory();
    std::erase(terms, term);
    Store(terms);
}

void ClearSearchHistory() {
    cfg_searchHistory.set("");
}

} // namespace glasswave::ui
