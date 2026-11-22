#include "pch.h"
#include "ui/i18n.h"
#include "core/paths.h"
#include "core/text.h"
#include "stream/provider.h"
#include "win7/win7_resources.h"

#include <cwctype>
#include <filesystem>
#include <fstream>

namespace glasswave::ui {

namespace {

// {7E1A4C92-3D5B-4F08-A6C1-9B2E4D7F0A35}
cfg_var_modern::cfg_int cfg_language({0x7e1a4c92, 0x3d5b, 0x4f08, {0xa6, 0xc1, 0x9b, 0x2e, 0x4d, 0x7f, 0x0a, 0x35}}, 0);

std::wstring LangFolder() { return win7::Resources::Get().Folder() + L"lang\\"; }

// Minimal reader for a flat JSON object of string pairs (what the translate script writes).
std::map<std::wstring, std::wstring> ReadJson(const std::wstring& path) {
    std::map<std::wstring, std::wstring> out;
    std::ifstream in{std::filesystem::path(path), std::ios::binary};
    if (!in) return out;
    std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const std::wstring s = core::Wide(bytes.rfind("\xEF\xBB\xBF", 0) == 0 ? bytes.substr(3) : bytes);
    size_t i = 0;
    auto readString = [&](std::wstring& v) {
        while (i < s.size() && s[i] != L'"') ++i;
        if (i >= s.size()) return false;
        ++i;
        for (; i < s.size() && s[i] != L'"'; ++i) {
            if (s[i] != L'\\' || i + 1 >= s.size()) {
                v += s[i];
                continue;
            }
            const wchar_t e = s[++i];
            switch (e) {
            case L'n': v += L'\n'; break;
            case L't': v += L'\t'; break;
            case L'r': v += L'\r'; break;
            case L'u':
                if (i + 4 < s.size()) {
                    v += static_cast<wchar_t>(std::wcstoul(s.substr(i + 1, 4).c_str(), nullptr, 16));
                    i += 4;
                }
                break;
            default: v += e; break;
            }
        }
        ++i;
        return true;
    };
    while (i < s.size()) {
        std::wstring key, value;
        if (!readString(key)) break;
        while (i < s.size() && s[i] != L':') ++i;
        if (!readString(value)) break;
        if (!key.empty()) out[key] = value;
    }
    return out;
}

struct Catalog {
    std::map<std::wstring, std::wstring> table;
    std::set<std::wstring> seen;
    bool chinese = false;
    bool loaded = false;
};

Catalog& Get() {
    static Catalog c;
    if (!c.loaded) {
        c.loaded = true;
        const auto setting = static_cast<Language>(cfg_language.get());
        c.chinese = setting == Language::Chinese ||
                    (setting == Language::Auto && PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_CHINESE);
        if (c.chinese) {
            c.table = ReadJson(LangFolder() + L"zh-CN.json");
            if (c.table.empty()) {
                const auto component = core::ComponentRoot();
                if (!component.empty()) c.table = ReadJson((component / L"lang" / L"zh-CN.json").wstring());
            }
        }
    }
    return c;
}

}  // namespace

Language CurrentLanguageSetting() { return static_cast<Language>(std::clamp<int>(static_cast<int>(cfg_language.get()), 0, 2)); }
void SetLanguageSetting(Language language) {
    const bool wasChinese = UsingChinese();
    cfg_language = static_cast<int>(language);
    auto& c = Get();
    c.loaded = false;  // reload on next use; keys seen so far stay recorded
    c.table.clear();
    if (UsingChinese() == wasChinese) return;
    // This runs from the settings/action path, never from a painter or const provider read.
    // Default providers re-read their cached snapshot asynchronously after language changes.
    for (auto* provider : stream::Providers()) {
        try {
            provider->OnLanguageChanged();
        } catch (...) {
        }
    }
}
bool UsingChinese() { return Get().chinese; }

const std::wstring& Tr(const std::wstring& english) {
    auto& c = Get();
    auto [it, fresh] = c.seen.insert(english);
    if (!c.chinese) return *it;
    if (auto t = c.table.find(english); t != c.table.end() && !t->second.empty()) return t->second;
    return *it;
}

const std::wstring& Tr(const std::wstring& english, std::wstring_view context) {
    auto& c = Get();
    const auto [key, fresh] = c.seen.insert(std::wstring(context) + L" | " + english);
    (void)fresh;
    if (c.chinese) {
        if (const auto translated = c.table.find(*key);
            translated != c.table.end() && !translated->second.empty())
            return translated->second;
    }
    return Tr(english);
}

std::wstring TrMenu(const std::wstring& text) {
    // "&Open...\tCtrl+O": translate the label without "&" (keys are plain English), keep the
    // shortcut; the accelerator letter is appended as "(&O)" the way Chinese menus show it.
    const size_t tab = text.find(L'\t');
    const std::wstring label = text.substr(0, tab), shortcut = tab == std::wstring::npos ? L"" : text.substr(tab);
    std::wstring plain;
    wchar_t accel = 0;
    for (size_t i = 0; i < label.size(); ++i) {
        if (label[i] == L'&' && i + 1 < label.size()) {
            if (label[i + 1] == L'&') {
                plain += L'&';
                ++i;
                continue;
            }
            accel = label[i + 1];
            continue;
        }
        plain += label[i];
    }
    const std::wstring& translated = Tr(plain);
    if (&translated == &*Get().seen.find(plain) || translated == plain) return text;  // untranslated
    std::wstring out = translated;
    if (accel) out += std::wstring(L"(&") + static_cast<wchar_t>(::towupper(accel)) + L")";
    return out + shortcut;
}

namespace {

bool IsHan(wchar_t c) {
    return (c >= 0x4E00 && c <= 0x9FFF) || (c >= 0x3400 && c <= 0x4DBF) || (c >= 0xF900 && c <= 0xFAFF);
}

bool IsLatinOrDigit(wchar_t c) {
    return (c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z') || (c >= L'0' && c <= L'9');
}

// Whether two characters that end up side by side want a space between them.
bool NeedsSpace(wchar_t left, wchar_t right) {
    return (IsHan(left) && IsLatinOrDigit(right)) || (IsLatinOrDigit(left) && IsHan(right));
}

}  // namespace

std::wstring Fill(std::wstring_view text, std::initializer_list<std::wstring_view> values) {
    std::wstring out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size();) {
        if (text[i] == L'{' && i + 2 < text.size() && text[i + 2] == L'}' && text[i + 1] >= L'0' &&
            static_cast<size_t>(text[i + 1] - L'0') < values.size()) {
            const std::wstring_view value = *(values.begin() + (text[i + 1] - L'0'));
            if (!value.empty() && !out.empty() && NeedsSpace(out.back(), value.front())) out += L' ';
            out += value;
            i += 3;
            if (!value.empty() && i < text.size() && NeedsSpace(value.back(), text[i])) out += L' ';
            continue;
        }
        out += text[i++];
    }
    return out;
}

std::wstring JoinOr(const std::vector<std::wstring>& items) {
    if (items.empty()) return {};
    std::wstring joined = items.front();
    for (size_t i = 1; i < items.size(); ++i)
        joined = Fill(Tr(i + 1 == items.size() ? L"{0} or {1}" : L"{0}, {1}"), {joined, items[i]});
    return joined;
}

std::wstring ExportStrings(metadb_handle_list_cref sample) {
    auto& c = Get();
    std::set<std::wstring> all = c.seen;
    auto addMenu = [&](HMENU menu, auto&& self) -> void {
        for (int i = 0, n = GetMenuItemCount(menu); i < n; ++i) {
            wchar_t buf[512]{};
            MENUITEMINFOW ii{sizeof(ii)};
            ii.fMask = MIIM_STRING | MIIM_SUBMENU | MIIM_FTYPE;
            ii.dwTypeData = buf;
            ii.cch = static_cast<UINT>(std::size(buf) - 1);
            if (!GetMenuItemInfoW(menu, i, TRUE, &ii) || (ii.fType & MFT_SEPARATOR)) continue;
            std::wstring label = buf;
            label = label.substr(0, label.find(L'\t'));
            std::wstring plain;
            for (size_t k = 0; k < label.size(); ++k)
                if (label[k] != L'&' || (k + 1 < label.size() && label[k + 1] == L'&' && (++k, true))) plain += label[k];
            if (!plain.empty()) all.insert(plain);
            if (ii.hSubMenu) self(ii.hSubMenu, self);
        }
    };
    for (const GUID* root : {&mainmenu_groups::file, &mainmenu_groups::edit, &mainmenu_groups::view,
                             &mainmenu_groups::playback, &mainmenu_groups::library, &mainmenu_groups::help}) {
        auto mgr = standard_api_create_t<mainmenu_manager>();
        mgr->instantiate(*root);
        HMENU m = CreatePopupMenu();
        mgr->generate_menu_win32(m, 1, 0x1000, 0);
        addMenu(m, addMenu);
        DestroyMenu(m);
    }
    if (sample.get_count()) {
        auto cm = contextmenu_manager::get();
        cm->init_context(sample, contextmenu_manager::flag_view_full);
        HMENU m = CreatePopupMenu();
        cm->win32_build_menu(m, 1, 0x7FFF);
        addMenu(m, addMenu);
        DestroyMenu(m);
    }
    const std::wstring folder = LangFolder();
    CreateDirectoryW(folder.c_str(), nullptr);
    const std::wstring path = folder + L"strings-en.txt";
    std::ofstream out{std::filesystem::path(path), std::ios::binary};
    for (const auto& s : all)
        if (!s.empty() && s.find(L'\n') == std::wstring::npos) out << core::Utf8(s) << '\n';
    return path;
}

}  // namespace glasswave::ui
