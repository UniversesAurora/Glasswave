#pragma once

#include <functional>
#include <string>
#include <vector>

// One row of the preferences page. Glasswave's own settings and each streaming provider
// describe their options this way, so a new provider adds its section without touching
// the page. Labels are English source text (translated when shown).
namespace glasswave::settings {

struct Field {
    enum class Kind {
        Number,  // edit box with arrows: min..max, `unit` after it
        Choice,  // drop-down list: the index into `choices`
        Check,   // checkbox: 0 unchecked, 1 checked (get/set)
        Text,    // free text
        Button,  // runs `press` at once (not part of Apply)
        Note     // read-only text from `note`, refreshed after every button
    };
    Kind kind = Kind::Number;
    std::wstring label;
    std::wstring unit;
    int min = 0, max = 0, defaultValue = 0;
    std::vector<std::wstring> choices;
    std::wstring defaultText;
    // Optional label of a Choice field in the same section: this control is enabled only while
    // that Choice field's live selection index is non-zero.
    std::wstring dependsOnChoice;
    bool restart = false;  // takes effect after foobar2000 restarts

    std::function<int()> get;  // Number, Choice
    std::function<void(int)> set;
    std::function<std::wstring()> getText;  // Text
    std::function<void(const std::wstring&)> setText;
    std::function<void(HWND owner)> press;  // Button
    std::function<std::wstring()> note;     // Note
};

struct Section {
    std::wstring title;
    std::vector<Field> fields;
};

}  // namespace glasswave::settings
