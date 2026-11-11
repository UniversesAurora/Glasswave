#pragma once

#include "netease/json.h"

#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace glasswave::applemusic {

using Object = netease::json::Object;

struct AccountState {
    bool signedIn = false;
    std::wstring accountId;
    std::wstring storefront;
    std::wstring name;
};

struct DrmState {
    bool available = false;
    std::wstring manager;
    std::wstring process;
    std::wstring authentication;
    std::wstring fairplay;
    std::wstring session;
};

struct LibraryPlaybackMapping {
    std::wstring libraryId;
    std::wstring catalogId;
    std::wstring reportingId;
};

// Non-2xx bridge response. The message retains the previous runtime_error text; callers
// that need selective recovery inspect status(). Every 2xx, including an empty 202/204,
// remains a successful Request result.
class BridgeHttpError : public std::runtime_error {
  public:
    explicit BridgeHttpError(int status);

    int status() const noexcept;

  private:
    int m_status;
};

// Diagnostic isolation for the reserved input fixture. Construction requires a mock
// bridge descriptor; the transaction header is bound to that exact URL and token.
class ScopedFixtureProbeTransaction {
  public:
    ScopedFixtureProbeTransaction();
    ~ScopedFixtureProbeTransaction();
    ScopedFixtureProbeTransaction(const ScopedFixtureProbeTransaction&) = delete;
    ScopedFixtureProbeTransaction& operator=(const ScopedFixtureProbeTransaction&) = delete;

  private:
    std::wstring m_previousUrl;
    std::string m_previousToken;
    std::string m_previousTag;
};

std::string ProbeTransactionTag();

// Blocking calls to the token-protected loopback helper. The bridge file is re-read for
// every request so setup and a rotated token take effect without restarting foobar2000.
// Exceptions contain neither the token nor a request or response body.
Object Request(std::wstring_view path, abort_callback& abort, std::wstring_view method = L"GET",
               std::string body = {});
Object Apple(std::wstring_view path, abort_callback& abort, std::wstring_view method = L"GET",
             const Object* body = nullptr);

// Fetches every page in an Apple "data"/"next" collection. The initial path and every
// next link must name the same API path; next links may only use Apple's Music API origin.
std::vector<Object> Collection(std::wstring_view path, abort_callback& abort);

// The provider calls this from the explicit UI-language change notification. Apple()
// otherwise discovers the current language once on its first request.
void SetAppleLocale(bool chinese);

// Resolves the current catalog identity explicitly supplied by Apple for one library
// song. This is used only when the library item's stored catalogId no longer resolves.
std::optional<LibraryPlaybackMapping> ResolveLibraryPlaybackMapping(std::wstring_view libraryId,
                                                                    std::wstring_view catalogId,
                                                                    abort_callback& abort);

// Both throw when <profile>/glasswave/applemusic/bridge.json is missing or invalid.
std::wstring BridgeUrl();
std::string BridgeToken();
// Stable origin for persisted provider state. Managed runtime directory, not its ephemeral
// loopback listener; exact loopback URL for an unmanaged external origin.
std::string BridgeSourceIdentity();

// UTF-8 percent encoding for an opaque path segment.
std::wstring Encode(std::wstring_view value);

// Verified account and DRM snapshots from the helper.
AccountState GetAccount(abort_callback& abort);
DrmState GetDrmState(abort_callback& abort);

} // namespace glasswave::applemusic
