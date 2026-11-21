#include "pch.h"
#include "media/recent_lists.h"
#include "core/text.h"

#include "ui/text.h"

#include <ctime>

namespace glasswave::media {

namespace {

constexpr size_t kLimit = 100;

// One playlist per line: provider \t id \t name \t played.
cfg_var_modern::cfg_string cfg_recentLists({0x8d3a51d3, 0x6e2f, 0x4b19, {0xa4, 0x7c, 0x1f, 0x5e, 0x9b, 0x2d, 0x7c, 0x40}},
                                           "");

std::vector<std::wstring> Split(const std::wstring& line, wchar_t sep) {
    std::vector<std::wstring> parts;
    size_t start = 0;
    while (true) {
        const size_t end = line.find(sep, start);
        parts.push_back(line.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start));
        if (end == std::wstring::npos) break;
        start = end + 1;
    }
    return parts;
}

std::wstring Clean(std::wstring text) {
    for (auto& c : text)
        if (c == L'\t' || c == L'\n' || c == L'\r') c = L' ';
    return text;
}

void Store(const std::vector<RecentList>& lists) {
    std::wstring text;
    for (const auto& list : lists)
        text += Clean(list.provider) + L'\t' + Clean(list.id) + L'\t' + Clean(list.name) + L'\t' +
                std::to_wstring(list.played) + L'\n';
    cfg_recentLists.set(core::Utf8(text).c_str());
}

} // namespace

std::vector<RecentList> RecentLists() {
    std::vector<RecentList> lists;
    for (const auto& line : Split(core::Wide(cfg_recentLists.get()), L'\n')) {
        const auto parts = Split(line, L'\t');
        if (parts.size() < 4 || parts[1].empty()) continue;
        lists.push_back({parts[0], parts[1], parts[2], _wtoi64(parts[3].c_str())});
    }
    return lists;
}

void RecordRecentList(const std::wstring& provider, const std::wstring& id, const std::wstring& name) {
    if (id.empty()) return;
    auto lists = RecentLists();
    std::erase_if(lists, [&](const RecentList& l) { return l.provider == provider && l.id == id; });
    lists.insert(lists.begin(), {provider, id, name, static_cast<int64_t>(std::time(nullptr))});
    if (lists.size() > kLimit) lists.resize(kLimit);
    Store(lists);
}

void ForgetRecentList(const std::wstring& provider, const std::wstring& id) {
    auto lists = RecentLists();
    std::erase_if(lists, [&](const RecentList& l) { return l.provider == provider && l.id == id; });
    Store(lists);
}

} // namespace glasswave::media
