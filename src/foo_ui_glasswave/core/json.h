#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>

namespace glasswave::core::json {

using Object = winrt::Windows::Data::Json::JsonObject;
using Array = winrt::Windows::Data::Json::JsonArray;

std::optional<Object> Parse(std::string_view utf8, std::wstring* error = nullptr);

std::wstring GetString(const Object& object, const wchar_t* key, std::wstring fallback = {});
int64_t GetInt64(const Object& object, const wchar_t* key, int64_t fallback = 0);
int GetInt(const Object& object, const wchar_t* key, int fallback = 0);
bool GetBool(const Object& object, const wchar_t* key, bool fallback = false);
std::optional<Object> GetObject(const Object& object, const wchar_t* key);
std::optional<Array> GetArray(const Object& object, const wchar_t* key);
std::vector<Object> Objects(const Array& array);
std::wstring Stringify(const Object& object);

} // namespace glasswave::core::json
