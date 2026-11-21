#include "pch.h"
#include "core/tsv.h"
#include "media/recent_lists.h"
#include "core/text.h"

#include "ui/text.h"

#include <ctime>

namespace glasswave::media {

namespace {

constexpr size_t kLimit = 100;

// One playlist per line: provider \t id \t name \t played (core/tsv.h).
cfg_var_modern::cfg_string cfg_recentLists({0x8d3a51d3, 0x6e2f, 0x4b19, {0xa4, 0x7c, 0x1f, 0x5e, 0x9b, 0x2d, 0x7c, 0x40}},
                                           "");

void Store(const std::vector<RecentList>& lists) {
    const auto field = [](const std::wstring& text) { return core::tsv::Escape(core::Utf8(text)); };
    std::string text;
    for (const auto& list : lists)
        text += field(list.provider) + '\t' + field(list.id) + '\t' + field(list.name) + '\t' +
                std::to_string(list.played) + '\n';
    cfg_recentLists.set(text.c_str());
}

} // namespace

std::vector<RecentList> RecentLists() {
    std::vector<RecentList> lists;
    const std::string text = cfg_recentLists.get().c_str();
    const auto field = [](std::string_view value) { return core::Wide(core::tsv::Unescape(value)); };
    for (const auto line : core::tsv::Split(text, '\n')) {
        const auto parts = core::tsv::Split(line);
        if (parts.size() < 4 || parts[1].empty()) continue;
        lists.push_back({field(parts[0]), field(parts[1]), field(parts[2]), _strtoi64(std::string(parts[3]).c_str(), nullptr, 10)});
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
