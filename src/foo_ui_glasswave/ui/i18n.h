#pragma once

#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

// UI language (English / Simplified Chinese).
//
// Every user-visible string goes through Tr(): English text is the key, the translation comes
// from `<profile>\glasswave\lang\<code>.json` (a flat JSON object "English": "translation"),
// falling back to the English text. foobar2000's own menu items are translated the same way
// when Glasswave draws its menus. Tr() also records each key it is asked for, so
// ExportStrings() can hand the full list to scripts/translate-deepseek.ps1.
namespace glasswave::ui {

enum class Language { Auto, English, Chinese };

Language CurrentLanguageSetting();
void SetLanguageSetting(Language language);  // reloads the table; the caller repaints
bool UsingChinese();

// Translation of `english` (the English text itself when there is none).
const std::wstring& Tr(const std::wstring& english);
// Source-specific keys use "<context> | <english>" and fall back to Tr(english).
const std::wstring& Tr(const std::wstring& english, std::wstring_view context);
inline const wchar_t* TrC(const wchar_t* english) { return Tr(english).c_str(); }

// Menu text: translates the label and keeps an "\t" shortcut and "&" accelerator working.
std::wstring TrMenu(const std::wstring& text);

// A translated template with "{0}", "{1}"... replaced by `values`. Where a value meets a Han
// character of the template, or the template meets a Han character of a value, a Latin letter or
// digit on the other side is set off by a space, as written Chinese does ("登录 Apple Music 后").
std::wstring Fill(std::wstring_view text, std::initializer_list<std::wstring_view> values);
// "A", "A or B", "A, B or C" in the UI language ("{0} or {1}", "{0}, {1}" translated).
std::wstring JoinOr(const std::vector<std::wstring>& items);

// Writes every string seen so far, plus foobar2000's whole main menu and the context menu
// for `sample` (if any), to <profile>\glasswave\lang\strings-en.txt, one per line.
// Returns the file path.
std::wstring ExportStrings(metadb_handle_list_cref sample);

}  // namespace glasswave::ui
