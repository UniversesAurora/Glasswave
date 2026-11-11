#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>

#include <winrt/Windows.Data.Json.h>

#include "applemusic/artist_preference.h"

namespace {

using glasswave::applemusic::ArtistPreference;
using glasswave::applemusic::ArtistPreferenceAction;
using glasswave::applemusic::BuildArtistPreferenceRequest;
using glasswave::applemusic::ParseArtistPreference;
using glasswave::applemusic::VerifiedArtistCatalogId;
using winrt::Windows::Data::Json::JsonObject;

size_t checks = 0;

void Check(bool condition, const char* message) {
    ++checks;
    if (condition) return;
    std::cerr << message << '\n';
    std::exit(1);
}

JsonObject Parse(const char* text) {
    return JsonObject::Parse(winrt::to_hstring(text));
}

void CheckPreferenceState() {
    Check(ParseArtistPreference(Parse(R"({
        "id":"42","type":"artists","attributes":{"inFavorites":true}
    })")) == ArtistPreference::Favorite,
          "A true inFavorites value was not Favorite");
    Check(ParseArtistPreference(Parse(R"({
        "id":"r.library","type":"library-artists","attributes":{"inFavorites":false}
    })")) == ArtistPreference::NotFavorite,
          "A false inFavorites value was not NotFavorite");

    for (const char* text : {
             R"({"id":"42","type":"artists","attributes":{}})",
             R"({"id":"42","type":"artists","attributes":{"inFavorites":null}})",
             R"({"id":"42","type":"artists","attributes":{"inFavorites":"true"}})",
             R"({"id":"42","type":"artists","attributes":{"inFavorites":1}})",
             R"({"id":"42","type":"albums","attributes":{"inFavorites":true}})",
         }) {
        Check(ParseArtistPreference(Parse(text)) == ArtistPreference::Unknown,
              "Missing or invalid favorite state was not Unknown");
    }
}

void CheckCatalogArtistMapping() {
    const JsonObject catalog = Parse(R"({"id":"1542902428","type":"artists"})");
    const auto catalogId = VerifiedArtistCatalogId(catalog, L"1542902428");
    Check(catalogId && catalogId->Value() == L"1542902428", "A valid catalog artist id was rejected");

    const JsonObject library = Parse(R"({
        "id":"r.De4w4q",
        "type":"library-artists",
        "relationships":{"catalog":{"data":[
            {"id":"1542902428","type":"artists"}
        ]}}
    })");
    const auto libraryId = VerifiedArtistCatalogId(library, L"r.De4w4q");
    Check(libraryId && libraryId->Value() == L"1542902428",
          "A unique verified library-to-catalog artist mapping was rejected");

    for (const char* text : {
             R"({"id":"r.no","type":"library-artists","relationships":{}})",
             R"({"id":"r.empty","type":"library-artists","relationships":{"catalog":{"data":[]}}})",
             R"({"id":"r.multi","type":"library-artists","relationships":{"catalog":{"data":[
                 {"id":"1","type":"artists"},{"id":"2","type":"artists"}
             ]}}})",
             R"({"id":"r.null","type":"library-artists","relationships":{"catalog":{"data":[
                 {"id":"1","type":"artists"},null
             ]}}})",
             R"({"id":"r.type","type":"library-artists","relationships":{"catalog":{"data":[
                 {"id":"1","type":"library-albums"}
             ]}}})",
             R"({"id":"r.next","type":"library-artists","relationships":{"catalog":{
                 "next":"/v1/me/library/artists/r.next/catalog?offset=1",
                 "data":[{"id":"1","type":"artists"}]}}})",
             R"({"id":"r.bad","type":"library-artists","relationships":{"catalog":{"data":[
                 {"id":"not-numeric","type":"artists"}
             ]}}})",
             R"({"id":"l.library-album","type":"library-albums"})",
             R"({"id":"","type":"artists"})",
         }) {
        const JsonObject resource = Parse(text);
        const std::wstring requestedId =
            resource.HasKey(L"id") && resource.GetNamedValue(L"id").ValueType() ==
                                          winrt::Windows::Data::Json::JsonValueType::String
                ? std::wstring(resource.GetNamedString(L"id").c_str())
                : std::wstring();
        Check(!VerifiedArtistCatalogId(resource, requestedId).has_value(),
              "An unverified artist mapping was accepted");
    }
}

void CheckRequestBuilderAndAliasRetention() {
    const JsonObject library = Parse(R"({
        "id":"r.owner",
        "type":"library-artists",
        "relationships":{"catalog":{"data":[
            {"id":"1542902428","type":"artists"}
        ]}}
    })");
    const auto id = VerifiedArtistCatalogId(library, L"r.owner");
    Check(id.has_value(), "Alias test mapping was rejected");

    const auto favorite = BuildArtistPreferenceRequest(*id, ArtistPreferenceAction::Favorite);
    Check(favorite.has_value(), "Favorite request was not built");
    Check(favorite->method == L"POST", "Favorite request method was not POST");
    // Apple() appends the current UI locale to this path. Both favorite and
    // unfavorite therefore end up with the l and platform parameters used by
    // the shipped Apple web client.
    Check(favorite->path == L"/v1/me/favorites?ids[artists]=1542902428&platform=web",
          "Favorite request path was not exact");
    Check(!favorite->body.has_value(), "Favorite request unexpectedly has a body");

    const auto unfavorite = BuildArtistPreferenceRequest(*id, ArtistPreferenceAction::Unfavorite);
    Check(unfavorite.has_value(), "Unfavorite request was not built");
    Check(unfavorite->method == L"DELETE", "Unfavorite request method was not DELETE");
    Check(unfavorite->path == L"/v1/me/favorites?ids[artists]=1542902428&platform=web",
          "Unfavorite request path was not exact");
    Check(!unfavorite->body.has_value(), "Unfavorite request unexpectedly has a body");

    // The original library resource id is metadata-only and never becomes the
    // catalog favorite id, including after favorite membership changes.
    Check(library.GetNamedString(L"id") == L"r.owner",
          "The original library identity changed during request construction");

    const JsonObject wrongIdentity = Parse(R"({
        "id":"1542902429",
        "type":"artists"
    })");
    Check(!VerifiedArtistCatalogId(wrongIdentity, L"1542902428"),
          "A mismatched requested artist identity was accepted");
}

} // namespace

int main() {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    CheckPreferenceState();
    CheckCatalogArtistMapping();
    CheckRequestBuilderAndAliasRetention();
    std::cout << "Apple artist preference checks passed: " << checks << '\n';
    return 0;
}
