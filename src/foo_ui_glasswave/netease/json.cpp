#include "pch.h"
#include "netease/json.h"

#include <climits>

namespace glasswave::netease::json {

using winrt::Windows::Data::Json::IJsonValue;
using winrt::Windows::Data::Json::JsonValueType;

namespace {

IJsonValue Lookup(const Object& object, const wchar_t* key) {
    const winrt::hstring name(key);
    if (!object || !object.HasKey(name)) return nullptr;
    return object.Lookup(name);
}

}  // namespace

std::optional<Object> Parse(std::string_view utf8, std::wstring* error) {
    try {
        return Object::Parse(winrt::to_hstring(utf8));
    } catch (const winrt::hresult_error& e) {
        if (error) *error = e.message().c_str();
        return std::nullopt;
    }
}

std::wstring GetString(const Object& object, const wchar_t* key, std::wstring fallback) {
    const IJsonValue value = Lookup(object, key);
    if (!value || value.ValueType() != JsonValueType::String) return fallback;
    return std::wstring(value.GetString().c_str());
}

int64_t GetInt64(const Object& object, const wchar_t* key, int64_t fallback) {
    const IJsonValue value = Lookup(object, key);
    if (!value || value.ValueType() != JsonValueType::Number) return fallback;
    return static_cast<int64_t>(value.GetNumber());
}

int GetInt(const Object& object, const wchar_t* key, int fallback) {
    const int64_t value = GetInt64(object, key, fallback);
    if (value < INT_MIN) return INT_MIN;
    if (value > INT_MAX) return INT_MAX;
    return static_cast<int>(value);
}

bool GetBool(const Object& object, const wchar_t* key, bool fallback) {
    const IJsonValue value = Lookup(object, key);
    if (!value || value.ValueType() != JsonValueType::Boolean) return fallback;
    return value.GetBoolean();
}

std::optional<Object> GetObject(const Object& object, const wchar_t* key) {
    const IJsonValue value = Lookup(object, key);
    if (!value || value.ValueType() != JsonValueType::Object) return std::nullopt;
    return value.GetObject();
}

std::optional<Array> GetArray(const Object& object, const wchar_t* key) {
    const IJsonValue value = Lookup(object, key);
    if (!value || value.ValueType() != JsonValueType::Array) return std::nullopt;
    return value.GetArray();
}

std::vector<Object> Objects(const Array& array) {
    std::vector<Object> out;
    if (!array) return out;
    const uint32_t size = array.Size();
    out.reserve(size);
    for (uint32_t i = 0; i < size; ++i) {
        const IJsonValue value = array.GetAt(i);
        if (value && value.ValueType() == JsonValueType::Object) out.push_back(value.GetObject());
    }
    return out;
}

std::wstring Stringify(const Object& object) {
    if (!object) return {};
    return std::wstring(object.Stringify().c_str());
}

}  // namespace glasswave::netease::json
