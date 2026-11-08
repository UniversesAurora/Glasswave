#include "pch.h"
#include "ui/i18n.h"
#include "win7/win7_resources.h"

#include <cwctype>
#include <filesystem>
#include <fstream>

namespace glasswave::ui {

namespace {

// {7E1A4C92-3D5B-4F08-A6C1-9B2E4D7F0A35}
cfg_var_modern::cfg_int cfg_language({0x7e1a4c92, 0x3d5b, 0x4f08, {0xa6, 0xc1, 0x9b, 0x2e, 0x4d, 0x7f, 0x0a, 0x35}}, 0);

std::wstring Widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string Narrow(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring LangFolder() { return win7::Resources::Get().Folder() + L"lang\\"; }

// Minimal reader for a flat JSON object of string pairs (what the translate script writes).
std::map<std::wstring, std::wstring> ReadJson(const std::wstring& path) {
    std::map<std::wstring, std::wstring> out;
    std::ifstream in{std::filesystem::path(path), std::ios::binary};
    if (!in) return out;
    std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const std::wstring s = Widen(bytes.rfind("\xEF\xBB\xBF", 0) == 0 ? bytes.substr(3) : bytes);
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
        if (c.chinese) c.table = ReadJson(LangFolder() + L"zh-CN.json");
    }
    return c;
}

}  // namespace

Language CurrentLanguageSetting() { return static_cast<Language>(std::clamp<int>(static_cast<int>(cfg_language.get()), 0, 2)); }
void SetLanguageSetting(Language language) {
    cfg_language = static_cast<int>(language);
    auto& c = Get();
    c.loaded = false;  // reload on next use; keys seen so far stay recorded
    c.table.clear();
}
bool UsingChinese() { return Get().chinese; }

const std::wstring& Tr(const std::wstring& english) {
    auto& c = Get();
    auto [it, fresh] = c.seen.insert(english);
    if (!c.chinese) return *it;
    if (auto t = c.table.find(english); t != c.table.end() && !t->second.empty()) return t->second;
    return *it;
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
        if (!s.empty() && s.find(L'\n') == std::wstring::npos) out << Narrow(s) << '\n';
    return path;
}

}  // namespace glasswave::ui
