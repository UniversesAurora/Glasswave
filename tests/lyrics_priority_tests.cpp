#include "lyrics/cache_format.h"
#include "lyrics/source_order.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

using glasswave::lyrics::source_order::CachedSourceIndex;
using glasswave::lyrics::source_order::Entry;
using glasswave::lyrics::source_order::OrderedIndexes;

size_t checks = 0;

void Check(bool condition, const char* message) {
    ++checks;
    if (condition) return;
    std::cerr << message << '\n';
    std::exit(1);
}

std::vector<Entry> Sources(bool sourceEnabled = true) {
    return {
        {L"native:source:file", L"source", sourceEnabled, true},
        {L"native:sidecar", L"sidecar", true, true},
        {L"qqmusic.js", L"qqmusic.js", true, true},
        {L"applemusic", L"applemusic", true, true},
    };
}

void CheckOrderingAndCacheFallback() {
    auto sources = Sources();
    auto ordered = OrderedIndexes(sources, L"qqmusic.js,source,sidecar,applemusic");
    Check(ordered.size() == 4 && ordered[0] == 2 && ordered[1] == 0,
          "A preferred plugin did not move above the source slot");
    Check(CachedSourceIndex(sources, ordered, L"native:source:file", false) == std::nullopt,
          "A non-cacheable local source produced a cache fallback");
    Check(CachedSourceIndex(sources, ordered, L"qqmusic.js", true) == 0,
          "A low-priority cached plugin was not bound to its current rank");
    auto originalOrder = OrderedIndexes(sources, L"source,qqmusic.js");
    Check(CachedSourceIndex(sources, originalOrder, L"qqmusic.js", true) == 1,
          "A cached fallback did not initially retain its low priority");
    auto preferredOrder = OrderedIndexes(sources, L"qqmusic.js,source");
    Check(CachedSourceIndex(sources, preferredOrder, L"qqmusic.js", true) == 0,
          "A cached fallback did not follow the edited current order");

    sources = Sources(false);
    ordered = OrderedIndexes(sources, L"qqmusic.js,source");
    Check(ordered.size() == 3 && ordered[0] == 2 &&
              std::find(ordered.begin(), ordered.end(), 0) == ordered.end(),
          "A disabled source remained enabled or cached");
    Check(CachedSourceIndex(sources, ordered, L"native:source:file", true) == std::nullopt,
          "A disabled source retained a cache fallback");

    sources = {
        {L"native:provider:ncm", L"source", true, true},
        {L"qqmusic.js", L"qqmusic.js", true, true},
    };
    ordered = OrderedIndexes(sources, L"qqmusic.js,source");
    Check(ordered.size() == 2 && ordered[0] == 1 && ordered[1] == 0,
          "Provider-native lyrics could not be reordered below a plugin with the source token");

    sources = Sources();
    sources[1].applicable = false;
    ordered = OrderedIndexes(sources, L"qqmusic.js,sidecar,source");
    Check(std::find(ordered.begin(), ordered.end(), 1) == ordered.end(),
          "An inapplicable source remained in the current-provider order");
    ordered = OrderedIndexes(Sources(), L"qqmusic.js");
    Check(ordered.size() == 4 && ordered[0] == 2 && ordered[1] == 0 && ordered[2] == 1 && ordered[3] == 3,
          "An enabled source omitted from the priority text was not appended stably");
    ordered = OrderedIndexes(Sources(), L"");
    Check(ordered.size() == 4 && ordered[0] == 0 && ordered[1] == 1 && ordered[2] == 2 && ordered[3] == 3,
          "An empty priority did not retain registry order");
    ordered = OrderedIndexes(Sources(), L"missing,qqmusic.js,qqmusic.js,source");
    Check(ordered.size() == 4 && ordered[0] == 2 && ordered[1] == 0,
          "Unknown or duplicate priority tokens destabilized enabled source ordering");

    auto withNewPlugin = Sources();
    withNewPlugin.push_back({L"newplugin.js", L"newplugin.js", true, true});
    ordered = OrderedIndexes(withNewPlugin, L"source,qqmusic.js");
    Check(ordered.size() == 5 && ordered.back() == 4,
          "A newly discovered enabled plugin was not appended for discoverability");
}

void CheckManualSelectionIsIndependent() {
    Check(glasswave::lyrics::cache_format::ManualSelectionApplies(false) &&
              glasswave::lyrics::cache_format::ManualSelectionApplies(true),
          "Automatic-cache policy suppressed the explicit manual selection");
    const std::wstring stored = L"[glasswave-source:QQ Music]\n[00:01.00]Chosen line";
    const auto headers = glasswave::lyrics::cache_format::Parse(stored);
    Check(headers.source == L"QQ Music" && stored.substr(headers.body) == L"[00:01.00]Chosen line",
          "The current manual selection format did not round-trip");
    Check(headers.provider.empty() && headers.scope.empty() && headers.sourceKey.empty(),
          "Manual selection gained automatic-cache policy fields");
    Check(glasswave::lyrics::cache_format::Manual(headers.source, stored.substr(headers.body)) == stored,
          "Reorder or account-independent manual selection text changed");
}

void CheckAutomaticBinding() {
    const std::wstring stored = glasswave::lyrics::cache_format::Automatic(
        L"Apple Music", L"applemusic", L"apple", L"scope-1", L"[00:01.00]Cached");
    const auto headers = glasswave::lyrics::cache_format::Parse(stored);
    Check(headers.sourceKey == L"applemusic" && headers.provider == L"apple" && headers.scope == L"scope-1",
          "Automatic cache lost its stable source or durable scope binding");
    Check(stored.substr(headers.body) == L"[00:01.00]Cached",
          "Automatic cache headers changed the lyric body");
}

} // namespace

int main() {
    CheckOrderingAndCacheFallback();
    CheckManualSelectionIsIndependent();
    CheckAutomaticBinding();
    std::cout << checks << " lyrics priority checks passed.\n";
}
