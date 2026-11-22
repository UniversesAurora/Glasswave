#include "pch.h"
#include "ui/home_store.h"

#include "cache/store.h"
#include "core/text.h"
#include "stream/catalog.h"
#include "ui/text.h"

#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>

#include <fstream>

namespace glasswave::ui::home_store {

namespace {

using winrt::Windows::Data::Json::JsonArray;
using winrt::Windows::Data::Json::JsonObject;
using winrt::Windows::Data::Json::JsonValue;

// 2: a song's badges; 4: stations among a row's items; 5: rows of cards; 6: a card's link
constexpr int kFormat = 6;
constexpr char kHomeSchema[] = "application/vnd.glasswave.home+json";

std::optional<cache::Key> KeyOf(const stream::Provider& provider) {
    const std::string providerKey = core::Utf8(provider.Key());
    if (providerKey.empty()) return std::nullopt;
    const std::string scope = cache::AccountScope(providerKey);
    if (scope.empty()) return std::nullopt;
    return cache::Key{cache::Kind::Data, providerKey, scope, "home", std::to_string(kFormat)};
}

// ---- writing ----

void Put(JsonObject& o, const wchar_t* key, const std::wstring& value) {
    if (!value.empty()) o.SetNamedValue(key, JsonValue::CreateStringValue(value));
}
void Put(JsonObject& o, const wchar_t* key, double value) { o.SetNamedValue(key, JsonValue::CreateNumberValue(value)); }
void PutFlag(JsonObject& o, const wchar_t* key, bool value) {
    if (value) o.SetNamedValue(key, JsonValue::CreateBooleanValue(true));
}

JsonArray ArtistsJson(const std::vector<stream::ArtistRef>& artists) {
    JsonArray list;
    for (const auto& a : artists) {
        JsonObject o;
        Put(o, L"id", a.id);
        Put(o, L"name", a.name);
        list.Append(o);
    }
    return list;
}

JsonObject ItemJson(const stream::HomeShelfItem& item) {
    JsonObject o;
    std::visit(
        [&](const auto& v) {
            using T = std::decay_t<decltype(v)>;
            Put(o, L"id", v.id);
            if constexpr (std::is_same_v<T, stream::Track>) {
                Put(o, L"kind", std::wstring(L"track"));
                Put(o, L"title", v.title);
                Put(o, L"note", v.note);
                // Untranslated keys without separators ("Original"), one string.
                std::wstring badges;
                for (const auto& badge : v.badges) badges += (badges.empty() ? L"" : L"|") + badge;
                Put(o, L"badges", badges);
                Put(o, L"album", v.album);
                Put(o, L"albumId", v.albumId);
                o.SetNamedValue(L"artists", ArtistsJson(v.artists));
                Put(o, L"durationMs", static_cast<double>(v.durationMs));
            } else if constexpr (std::is_same_v<T, stream::Album>) {
                Put(o, L"kind", std::wstring(L"album"));
                Put(o, L"name", v.name);
                Put(o, L"coverUrl", v.coverUrl);
                o.SetNamedValue(L"artists", ArtistsJson(v.artists));
                Put(o, L"trackCount", static_cast<double>(v.trackCount));
                Put(o, L"year", static_cast<double>(v.year));
            } else if constexpr (std::is_same_v<T, stream::Playlist>) {
                Put(o, L"kind", std::wstring(L"playlist"));
                Put(o, L"name", v.name);
                Put(o, L"coverUrl", v.coverUrl);
                Put(o, L"creator", v.creator);
                Put(o, L"trackCount", static_cast<double>(v.trackCount));
                PutFlag(o, L"own", v.own);
                PutFlag(o, L"saved", v.saved);
            } else if constexpr (std::is_same_v<T, stream::Station>) {
                Put(o, L"kind", std::wstring(L"station"));
                Put(o, L"name", v.name);
                Put(o, L"subtitle", v.subtitle);
                Put(o, L"coverUrl", v.coverUrl);
            } else {
                Put(o, L"kind", std::wstring(L"artist"));
                Put(o, L"name", v.name);
                Put(o, L"avatarUrl", v.avatarUrl);
            }
        },
        item);
    return o;
}

JsonObject CardJson(const stream::HomeCard& card) {
    JsonObject c;
    Put(c, L"key", card.key);
    Put(c, L"label", card.label);
    Put(c, L"title", card.title);
    Put(c, L"subtitle", card.subtitle);
    Put(c, L"badge", card.badge);
    Put(c, L"imageUrl", card.imageUrl);
    PutFlag(c, L"imageTall", card.imageTall);
    Put(c, L"videoUrl", card.videoUrl);
    if (card.colors) {
        Put(c, L"background", static_cast<double>(card.background));
        Put(c, L"titleInk", static_cast<double>(card.titleInk));
        Put(c, L"textInk", static_cast<double>(card.textInk));
    }
    Put(c, L"list", card.list);
    Put(c, L"station", card.station);
    Put(c, L"link", card.link);
    if (card.target) c.SetNamedValue(L"target", ItemJson(stream::ToHomeShelfItem(*card.target)));
    return c;
}

JsonObject HomeJson(const stream::Home& home, const std::wstring& account) {
    JsonObject o;
    Put(o, L"format", static_cast<double>(kFormat));
    Put(o, L"account", account);
    Put(o, L"cardsTitle", home.cardsTitle);
    JsonArray cards;
    for (const auto& card : home.cards) cards.Append(CardJson(card));
    o.SetNamedValue(L"cards", cards);
    JsonArray shelves;
    for (const auto& shelf : home.shelves) {
        JsonObject s;
        Put(s, L"id", shelf.id);
        Put(s, L"title", shelf.title);
        JsonArray rowCards;
        for (const auto& card : shelf.cards) rowCards.Append(CardJson(card));
        s.SetNamedValue(L"cards", rowCards);
        JsonArray items;
        for (const auto& item : shelf.items) items.Append(ItemJson(item));
        s.SetNamedValue(L"items", items);
        shelves.Append(s);
    }
    o.SetNamedValue(L"shelves", shelves);
    return o;
}

// ---- reading ----

std::wstring Text(const JsonObject& o, const wchar_t* key) { return std::wstring(o.GetNamedString(key, L"")); }
int Number(const JsonObject& o, const wchar_t* key, int fallback = 0) {
    return static_cast<int>(o.GetNamedNumber(key, fallback));
}
bool Flag(const JsonObject& o, const wchar_t* key) { return o.GetNamedBoolean(key, false); }
std::vector<JsonObject> Objects(const JsonObject& o, const wchar_t* key) {
    std::vector<JsonObject> out;
    for (const auto& value : o.GetNamedArray(key, JsonArray()))
        if (value.ValueType() == winrt::Windows::Data::Json::JsonValueType::Object) out.push_back(value.GetObject());
    return out;
}

std::vector<stream::ArtistRef> ArtistsOf(const JsonObject& o) {
    std::vector<stream::ArtistRef> artists;
    for (const auto& a : Objects(o, L"artists")) artists.push_back({Text(a, L"id"), Text(a, L"name")});
    return artists;
}

std::optional<stream::HomeShelfItem> ItemOf(const JsonObject& o) {
    const std::wstring kind = Text(o, L"kind"), id = Text(o, L"id");
    if (id.empty()) return std::nullopt;
    if (kind == L"track") {
        stream::Track t;
        t.id = id;
        t.title = Text(o, L"title");
        t.note = Text(o, L"note");
        for (std::wstring rest = Text(o, L"badges"); !rest.empty();) {
            const size_t bar = rest.find(L'|');
            t.badges.push_back(rest.substr(0, bar));
            rest = bar == std::wstring::npos ? std::wstring() : rest.substr(bar + 1);
        }
        t.album = Text(o, L"album");
        t.albumId = Text(o, L"albumId");
        t.artists = ArtistsOf(o);
        t.durationMs = Number(o, L"durationMs");
        return stream::HomeShelfItem(std::move(t));
    }
    if (kind == L"album") {
        stream::Album a;
        a.id = id;
        a.name = Text(o, L"name");
        a.coverUrl = Text(o, L"coverUrl");
        a.artists = ArtistsOf(o);
        a.trackCount = Number(o, L"trackCount", stream::kUnknownCount);
        a.year = Number(o, L"year");
        return stream::HomeShelfItem(std::move(a));
    }
    if (kind == L"playlist") {
        stream::Playlist p;
        p.id = id;
        p.name = Text(o, L"name");
        p.coverUrl = Text(o, L"coverUrl");
        p.creator = Text(o, L"creator");
        p.trackCount = Number(o, L"trackCount", stream::kUnknownCount);
        p.own = Flag(o, L"own");
        p.saved = Flag(o, L"saved");
        return stream::HomeShelfItem(std::move(p));
    }
    if (kind == L"artist") {
        stream::Artist a;
        a.id = id;
        a.name = Text(o, L"name");
        a.avatarUrl = Text(o, L"avatarUrl");
        return stream::HomeShelfItem(std::move(a));
    }
    if (kind == L"station") {
        stream::Station s;
        s.id = id;
        s.name = Text(o, L"name");
        s.subtitle = Text(o, L"subtitle");
        s.coverUrl = Text(o, L"coverUrl");
        return stream::HomeShelfItem(std::move(s));
    }
    return std::nullopt;
}

// The records the home page names, given back to the source as a list cache hit would be.
void Restore(stream::Provider& provider, const stream::Home& home) {
    stream::ListPage tracks, albums, playlists, artists;
    tracks.rows = std::vector<stream::Track>{};
    albums.rows = std::vector<stream::Album>{};
    playlists.rows = std::vector<stream::Playlist>{};
    artists.rows = std::vector<stream::Artist>{};
    std::vector<stream::Station> stations;
    const auto add = [&](const stream::HomeShelfItem& item) {
        std::visit(
            [&](const auto& v) {
                using T = std::decay_t<decltype(v)>;
                if constexpr (std::is_same_v<T, stream::Track>)
                    std::get<std::vector<stream::Track>>(tracks.rows).push_back(v);
                else if constexpr (std::is_same_v<T, stream::Album>)
                    std::get<std::vector<stream::Album>>(albums.rows).push_back(v);
                else if constexpr (std::is_same_v<T, stream::Playlist>)
                    std::get<std::vector<stream::Playlist>>(playlists.rows).push_back(v);
                else if constexpr (std::is_same_v<T, stream::Station>)
                    stations.push_back(v);
                else
                    std::get<std::vector<stream::Artist>>(artists.rows).push_back(v);
            },
            item);
    };
    for (const auto& card : home.cards)
        if (card.target) add(stream::ToHomeShelfItem(*card.target));
    for (const auto& shelf : home.shelves) {
        for (const auto& card : shelf.cards)
            if (card.target) add(stream::ToHomeShelfItem(*card.target));
        for (const auto& item : shelf.items) add(item);
    }
    for (const auto* page : {&tracks, &albums, &playlists, &artists}) provider.RestoreListRecords(*page);
    if (stream::Catalog* records = provider.Records()) {
        for (const auto& station : stations) records->Put(station);
    }
}

} // namespace

std::optional<stream::HomeCard> CardOf(const JsonObject& c) {
    stream::HomeCard card;
    card.key = Text(c, L"key");
    card.label = Text(c, L"label");
    card.title = Text(c, L"title");
    card.subtitle = Text(c, L"subtitle");
    card.badge = Text(c, L"badge");
    card.imageUrl = Text(c, L"imageUrl");
    card.imageTall = Flag(c, L"imageTall");
    card.videoUrl = Text(c, L"videoUrl");
    card.colors = c.HasKey(L"background");
    card.background = static_cast<uint32_t>(c.GetNamedNumber(L"background", 0));
    card.titleInk = static_cast<uint32_t>(c.GetNamedNumber(L"titleInk", 0));
    card.textInk = static_cast<uint32_t>(c.GetNamedNumber(L"textInk", 0));
    card.list = Text(c, L"list");
    card.station = Text(c, L"station");
    card.link = Text(c, L"link");
    if (c.HasKey(L"target"))
        if (auto target = ItemOf(c.GetNamedObject(L"target")))
            std::visit(
                [&card](const auto& v) {
                    using T = std::decay_t<decltype(v)>;
                    if constexpr (!std::is_same_v<T, stream::Station>) card.target = stream::CatalogSearchItem(v);
                },
                *target);
    if (card.key.empty()) return std::nullopt;
    return card;
}

std::optional<stream::Home> Load(stream::Provider& provider) {
    const auto key = KeyOf(provider);
    if (!key) return std::nullopt;
    std::string bytes;
    try {
        const auto record = cache::GetStore().Lookup(*key);
        if (!record || record->Info().contentType != kHomeSchema) return std::nullopt;
        std::ifstream input(record->Path(), std::ios::binary);
        if (!input) return std::nullopt;
        bytes.assign((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    } catch (...) {
        return std::nullopt;
    }
    JsonObject o{nullptr};
    if (!JsonObject::TryParse(winrt::to_hstring(bytes), o) || !o) return std::nullopt;
    if (Number(o, L"format") != kFormat) return std::nullopt;
    stream::Home home;
    home.cardsTitle = Text(o, L"cardsTitle");
    for (const auto& c : Objects(o, L"cards"))
        if (auto card = CardOf(c)) home.cards.push_back(std::move(*card));
    for (const auto& s : Objects(o, L"shelves")) {
        stream::HomeShelf shelf;
        shelf.id = Text(s, L"id");
        shelf.title = Text(s, L"title");
        for (const auto& c : Objects(s, L"cards"))
            if (auto card = CardOf(c)) shelf.cards.push_back(std::move(*card));
        for (const auto& i : Objects(s, L"items"))
            if (auto item = ItemOf(i)) shelf.items.push_back(std::move(*item));
        if (!shelf.cards.empty() || !shelf.items.empty()) home.shelves.push_back(std::move(shelf));
    }
    Restore(provider, home);
    return home;
}

void Save(const stream::Provider& provider, const stream::Home& home) {
    const auto key = KeyOf(provider);
    if (!key) return;
    const uint64_t revision = cache::AccountRevision(key->provider);
    const std::string text = winrt::to_string(HomeJson(home, provider.AccountName()).Stringify());
    fb2k::splitTask([key = *key, revision, text] {
        try {
            if (cache::AccountRevision(key.provider) != revision ||
                cache::AccountScope(key.provider) != key.scope)
                return;
            auto writer = cache::GetStore().Begin(key);
            if (!writer) return;
            {
                std::ofstream output(writer->Path(), std::ios::binary | std::ios::trunc);
                if (!output) return;
                output.write(text.data(), static_cast<std::streamsize>(text.size()));
                if (!output) return;
            }
            cache::Metadata meta;
            meta.contentType = kHomeSchema;
            meta.validatedAt = cache::Store::Now();
            writer->Commit(std::move(meta));
        } catch (...) {
        }
    });
}

} // namespace glasswave::ui::home_store
