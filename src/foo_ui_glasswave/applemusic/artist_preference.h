#pragma once

#include "core/json.h"

#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace glasswave::applemusic {

// Apple distinguishes a favorite artist from the broader set of artists that
// happen to occur in the user's library.
enum class ArtistPreference {
    Unknown,
    NotFavorite,
    Favorite,
};

enum class ArtistPreferenceAction {
    Favorite,
    Unfavorite,
};

// Only VerifiedArtistCatalogId can create this token. It keeps library ids and
// unverified names or ids out of the current-web favorites endpoint.
class CatalogArtistId final {
  public:
    CatalogArtistId(const CatalogArtistId&) = default;
    CatalogArtistId(CatalogArtistId&&) noexcept = default;
    CatalogArtistId& operator=(const CatalogArtistId&) = default;
    CatalogArtistId& operator=(CatalogArtistId&&) noexcept = default;

    const std::wstring& Value() const noexcept { return m_value; }

  private:
    explicit CatalogArtistId(std::wstring value) : m_value(std::move(value)) {}

    std::wstring m_value;

    friend std::optional<CatalogArtistId> VerifiedArtistCatalogId(
        const core::json::Object& resource, std::wstring_view expectedResourceId) noexcept;
};

struct ArtistPreferenceRequest {
    std::wstring method;
    std::wstring path;
    std::optional<std::string> body; // nullopt means an actually empty body.
};

// Reads the optional attributes.inFavorites field. Missing, null, or malformed
// values remain Unknown; library membership never implies Favorite.
ArtistPreference ParseArtistPreference(const core::json::Object& resource) noexcept;

// The returned resource id must equal expectedResourceId. A catalog artist then
// supplies its own numeric id. A library artist is accepted only when
// relationships.catalog is complete and identifies exactly one catalog artist.
// Name matching and ambiguous relationships are rejected.
std::optional<CatalogArtistId> VerifiedArtistCatalogId(const core::json::Object& resource,
                                                       std::wstring_view expectedResourceId) noexcept;

// Apple's current web contract uses the catalog artist id for both directions:
// POST/DELETE /v1/me/favorites?ids[artists]=<catalog-id>&l=<ui-language>&platform=web,
// with no request body. The path returned here intentionally omits l so the
// normal Apple() client adds the same UI-language parameter to both methods.
// This mirrors the shipped Apple web bundle at
// https://music.apple.com/assets/index~cc245af156.js.
// A successful response acknowledges the request; callers must read
// inFavorites again before treating the new state as final.
std::optional<ArtistPreferenceRequest> BuildArtistPreferenceRequest(const CatalogArtistId& artist,
                                                                    ArtistPreferenceAction action);

} // namespace glasswave::applemusic
