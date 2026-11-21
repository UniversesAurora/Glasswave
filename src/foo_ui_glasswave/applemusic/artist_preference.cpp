#include "pch.h"

#include "applemusic/artist_preference.h"

#include <algorithm>
#include <cctype>

namespace glasswave::applemusic {

namespace {

using winrt::Windows::Data::Json::IJsonValue;
using winrt::Windows::Data::Json::JsonValueType;

IJsonValue Lookup(const core::json::Object& object, const wchar_t* key) {
    if (!object) return nullptr;
    const winrt::hstring name(key);
    if (!object.HasKey(name)) return nullptr;
    return object.Lookup(name);
}

std::optional<std::wstring> String(const core::json::Object& object, const wchar_t* key) {
    const IJsonValue value = Lookup(object, key);
    if (!value || value.ValueType() != JsonValueType::String) return std::nullopt;
    return std::wstring(value.GetString().c_str());
}

std::optional<core::json::Object> Object(const core::json::Object& object, const wchar_t* key) {
    const IJsonValue value = Lookup(object, key);
    if (!value || value.ValueType() != JsonValueType::Object) return std::nullopt;
    return value.GetObject();
}

bool IsCatalogArtistId(std::wstring_view id) {
    return !id.empty() && std::all_of(id.begin(), id.end(), [](wchar_t c) { return c >= L'0' && c <= L'9'; });
}

std::optional<std::wstring> CatalogArtistIdFromLibraryArtist(const core::json::Object& resource) {
    const auto relationships = Object(resource, L"relationships");
    if (!relationships) return std::nullopt;
    const auto catalog = Object(*relationships, L"catalog");
    if (!catalog || !String(*catalog, L"next").value_or(L"").empty()) return std::nullopt;

    const IJsonValue dataValue = Lookup(*catalog, L"data");
    if (!dataValue || dataValue.ValueType() != JsonValueType::Array) return std::nullopt;
    const auto data = dataValue.GetArray();
    if (data.Size() != 1) return std::nullopt;

    const IJsonValue itemValue = data.GetAt(0);
    if (!itemValue || itemValue.ValueType() != JsonValueType::Object) return std::nullopt;
    const auto item = itemValue.GetObject();
    const auto type = String(item, L"type");
    const auto id = String(item, L"id");
    if (!type || *type != L"artists" || !id || !IsCatalogArtistId(*id)) return std::nullopt;
    return id;
}

} // namespace

ArtistPreference ParseArtistPreference(const core::json::Object& resource) noexcept {
    try {
        const auto type = String(resource, L"type");
        if (!type || (*type != L"artists" && *type != L"library-artists")) return ArtistPreference::Unknown;

        const auto attributes = Object(resource, L"attributes");
        if (!attributes) return ArtistPreference::Unknown;
        const IJsonValue value = Lookup(*attributes, L"inFavorites");
        if (!value || value.ValueType() != JsonValueType::Boolean) return ArtistPreference::Unknown;
        return value.GetBoolean() ? ArtistPreference::Favorite : ArtistPreference::NotFavorite;
    } catch (...) {
        return ArtistPreference::Unknown;
    }
}

std::optional<CatalogArtistId> VerifiedArtistCatalogId(const core::json::Object& resource,
                                                       std::wstring_view expectedResourceId) noexcept {
    try {
        const auto type = String(resource, L"type");
        const auto resourceId = String(resource, L"id");
        if (!type || !resourceId || *resourceId != expectedResourceId) return std::nullopt;
        if (*type == L"artists") {
            if (!IsCatalogArtistId(*resourceId)) return std::nullopt;
            return CatalogArtistId(std::move(*resourceId));
        }
        if (*type == L"library-artists") {
            const auto catalogId = CatalogArtistIdFromLibraryArtist(resource);
            if (!catalogId) return std::nullopt;
            return CatalogArtistId(std::move(*catalogId));
        }
    } catch (...) {
    }
    return std::nullopt;
}

std::optional<ArtistPreferenceRequest> BuildArtistPreferenceRequest(const CatalogArtistId& artist,
                                                                    ArtistPreferenceAction action) {
    std::wstring method;
    switch (action) {
    case ArtistPreferenceAction::Favorite:
        method = L"POST";
        break;
    case ArtistPreferenceAction::Unfavorite:
        method = L"DELETE";
        break;
    default:
        return std::nullopt;
    }

    return ArtistPreferenceRequest{
        std::move(method),
        L"/v1/me/favorites?ids[artists]=" + artist.Value() + L"&platform=web",
        std::nullopt,
    };
}

} // namespace glasswave::applemusic
