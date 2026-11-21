#include "pch.h"
#include "applemusic/client.h"
#include "core/text.h"
#include "core/url.h"
#include "applemusic/service.h"
#include "core/json.h"
#include "core/paths.h"
#include "ui/i18n.h"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <set>

namespace glasswave::applemusic {

namespace {

namespace json = core::json;
using winrt::Windows::Data::Json::JsonValue;

constexpr uintmax_t kBridgeFileLimit = 64u * 1024u;
constexpr size_t kBridgeTokenLimit = 16u * 1024u;
constexpr size_t kCollectionPageLimit = 256;
constexpr size_t kCollectionItemLimit = 100'000;

std::atomic_bool g_chineseLocale = false;
std::atomic_bool g_localeKnown = false;
thread_local std::wstring g_probeUrl;
thread_local std::string g_probeToken;
thread_local std::string g_probeTag;

struct BridgeConfig {
    std::wstring url;
    std::string token;
    std::filesystem::path runtime;
    std::string fixtureTransactionTag;
};

[[noreturn]] void Fail(const char* message) {
    throw std::runtime_error(message);
}

using core::Utf8;
using core::Wide;

bool AsciiLetter(wchar_t c) {
    return (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z');
}

bool HasControl(std::wstring_view text) {
    return std::any_of(text.begin(), text.end(), [](wchar_t c) { return c < 0x20 || c == 0x7f; });
}

using core::LowerAscii;

bool ValidPort(std::wstring_view port) {
    if (port.empty() || port.size() > 5) return false;
    unsigned value = 0;
    for (wchar_t c : port) {
        if (c < L'0' || c > L'9') return false;
        value = value * 10 + static_cast<unsigned>(c - L'0');
    }
    return value > 0 && value <= 65535;
}

std::wstring ValidateBridgeUrl(std::wstring_view value) {
    static constexpr std::wstring_view prefix = L"http://";
    if (value.size() <= prefix.size() || _wcsnicmp(value.data(), prefix.data(), prefix.size()) != 0 ||
        HasControl(value) || value.find_first_of(L"/?#@\\", prefix.size()) != std::wstring_view::npos) {
        Fail("The Apple Music bridge URL must use http://127.0.0.1.");
    }

    const std::wstring_view authority = value.substr(prefix.size());
    static constexpr std::wstring_view loopback = L"127.0.0.1";
    if (authority == loopback) return std::wstring(value);
    if (authority.size() <= loopback.size() || authority.substr(0, loopback.size()) != loopback ||
        authority[loopback.size()] != L':' || !ValidPort(authority.substr(loopback.size() + 1))) {
        Fail("The Apple Music bridge URL must use http://127.0.0.1.");
    }
    return std::wstring(value);
}

bool ValidJsonToken(std::wstring_view token) {
    if (token.empty() || token.size() > kBridgeTokenLimit || HasControl(token)) return false;
    return true;
}

bool ValidCatalogId(std::wstring_view id) {
    if (id.empty() || id.size() > 256 || HasControl(id) || id.find(L'/') != std::wstring_view::npos ||
        id.find(L'\\') != std::wstring_view::npos || id.find(L'?') != std::wstring_view::npos ||
        id.find(L'#') != std::wstring_view::npos) {
        return false;
    }
    return std::all_of(id.begin(), id.end(), [](wchar_t c) { return c >= L'0' && c <= L'9'; });
}

BridgeConfig ReadBridge() {
    const auto dir = core::ProfileDir(L"applemusic");
    if (dir.empty()) Fail("The foobar2000 profile is unavailable.");
    const auto path = dir / L"bridge.json";

    std::error_code ec;
    const auto status = std::filesystem::symlink_status(path, ec);
    if (ec || status.type() != std::filesystem::file_type::regular) {
        Fail("The Apple Music bridge is not configured.");
    }
    const uintmax_t size = std::filesystem::file_size(path, ec);
    if (ec || size == 0 || size > kBridgeFileLimit) Fail("The Apple Music bridge file is invalid.");

    // Every request reads the bridge configuration. The file changes only when the bridge
    // is set up again, so its parsed form is reused while its size and write time stand.
    const auto written = std::filesystem::last_write_time(path, ec);
    if (ec) Fail("The Apple Music bridge file cannot be read.");
    static std::mutex cacheMutex;
    static std::optional<BridgeConfig> cached;
    static uintmax_t cachedSize = 0;
    static std::filesystem::file_time_type cachedWritten{};
    {
        std::lock_guard lock(cacheMutex);
        if (cached && cachedSize == size && cachedWritten == written) {
            const BridgeConfig& config = *cached;
            if (!g_probeTag.empty() && (config.url != g_probeUrl || config.token != g_probeToken ||
                                        config.fixtureTransactionTag != g_probeTag))
                Fail("The Apple Music fixture bridge changed during the probe.");
            return config;
        }
    }

    std::ifstream input(path, std::ios::binary);
    if (!input) Fail("The Apple Music bridge file cannot be read.");
    std::string bytes(static_cast<size_t>(size), '\0');
    input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!input || static_cast<size_t>(input.gcount()) != bytes.size()) {
        Fail("The Apple Music bridge file cannot be read.");
    }
    if (input.peek() != std::char_traits<char>::eof()) Fail("The Apple Music bridge file is invalid.");
    if (bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xef &&
        static_cast<unsigned char>(bytes[1]) == 0xbb && static_cast<unsigned char>(bytes[2]) == 0xbf) {
        bytes.erase(0, 3);
    }
    if (bytes.empty()) Fail("The Apple Music bridge file is invalid.");

    std::wstring parseError;
    const auto root = json::Parse(bytes, &parseError);
    if (!root) Fail("The Apple Music bridge file is not valid JSON.");
    BridgeConfig config;
    config.url = ValidateBridgeUrl(json::GetString(*root, L"url"));
    const std::wstring token = json::GetString(*root, L"token");
    if (!ValidJsonToken(token)) Fail("The Apple Music bridge token is invalid.");
    config.token = Utf8(token);
    if (config.token.empty() || config.token.size() > kBridgeTokenLimit) {
        Fail("The Apple Music bridge token is invalid.");
    }
    if (const auto managed = json::GetObject(*root, L"managed")) {
        const auto directory = json::GetString(*managed, L"runtimeDirectory");
        if (directory.empty() || HasControl(directory))
            Fail("The managed Apple Music runtime path is invalid.");
        config.runtime = directory;
        if (!config.runtime.is_absolute()) Fail("The managed Apple Music runtime path must be absolute.");
    }
    const auto fixtureAsset = json::GetString(*root, L"fixturePlaybackAssetId");
    if (config.runtime.empty() && json::GetString(*root, L"fixtureAssetId") == L"glasswave-test" &&
        (fixtureAsset == L"glasswave-test" || fixtureAsset == L"1237064886")) {
        const auto tag = json::GetString(*root, L"fixtureTransactionTag");
        if (tag.size() == 32 && std::all_of(tag.begin(), tag.end(), [](wchar_t c) {
                return (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f');
            }))
            config.fixtureTransactionTag = Utf8(tag);
    }
    if (!g_probeTag.empty() && (config.url != g_probeUrl || config.token != g_probeToken ||
                                config.fixtureTransactionTag != g_probeTag))
        Fail("The Apple Music fixture bridge changed during the probe.");
    {
        std::lock_guard lock(cacheMutex);
        cached = config;
        cachedSize = size;
        cachedWritten = written;
    }
    return config;
}

std::wstring Method(std::wstring_view method) {
    if (method.empty() || method.size() > 16 || HasControl(method)) Fail("The HTTP method is invalid.");
    std::wstring out(method);
    for (auto& c : out) {
        if (!AsciiLetter(c)) Fail("The HTTP method is invalid.");
        if (c >= L'a' && c <= L'z') c = static_cast<wchar_t>(c + L'A' - L'a');
    }
    return out;
}

void ValidateApiPath(std::wstring_view path) {
    static constexpr std::wstring_view prefix = L"/api/v1/";
    if (path.size() <= prefix.size() || path.substr(0, prefix.size()) != prefix || HasControl(path) ||
        path.find(L'#') != std::wstring_view::npos || path.find(L'\\') != std::wstring_view::npos) {
        Fail("The Apple Music bridge path is invalid.");
    }
}

void ValidateApplePath(std::wstring_view path) {
    static constexpr std::wstring_view prefix = L"/v1/";
    if (path.size() <= prefix.size() || path.substr(0, prefix.size()) != prefix || HasControl(path) ||
        path.find(L'#') != std::wstring_view::npos || path.find(L'\\') != std::wstring_view::npos) {
        Fail("The Apple Music API path is invalid.");
    }
}

std::wstring PathOnly(std::wstring_view path) {
    const size_t query = path.find(L'?');
    return std::wstring(path.substr(0, query));
}

std::wstring NormalizeNext(std::wstring_view initial, std::wstring_view next) {
    if (next.empty() || HasControl(next) || next.find(L'#') != std::wstring_view::npos ||
        next.find(L'\\') != std::wstring_view::npos) {
        Fail("Apple Music returned an invalid pagination link.");
    }

    std::wstring path;
    if (next[0] == L'/') {
        path.assign(next);
    } else {
        const size_t scheme = next.find(L"://");
        if (scheme == std::wstring_view::npos) Fail("Apple Music returned an invalid pagination link.");
        const std::wstring protocol = LowerAscii(next.substr(0, scheme));
        const size_t pathStart = next.find(L'/', scheme + 3);
        if (protocol != L"https" || pathStart == std::wstring_view::npos) {
            Fail("Apple Music returned an unexpected pagination origin.");
        }
        const std::wstring_view authority = next.substr(scheme + 3, pathStart - scheme - 3);
        const std::wstring host = LowerAscii(authority);
        if (host != L"amp-api.music.apple.com" && host != L"api.music.apple.com") {
            Fail("Apple Music returned an unexpected pagination origin.");
        }
        path.assign(next.substr(pathStart));
    }
    ValidateApplePath(path);
    // The service writes the storefront in lower case ("/catalog/us/") whatever case was asked.
    if (LowerAscii(PathOnly(path)) != LowerAscii(PathOnly(initial)))
        Fail("Apple Music returned an unexpected pagination path.");
    return path;
}

bool HasLocaleParameter(std::wstring_view path) {
    const size_t query = path.find(L'?');
    if (query == std::wstring_view::npos) return false;
    size_t at = query + 1;
    while (at <= path.size()) {
        const size_t end = path.find(L'&', at);
        const std::wstring_view parameter =
            path.substr(at, end == std::wstring_view::npos ? std::wstring_view::npos : end - at);
        const size_t equals = parameter.find(L'=');
        const std::wstring_view name = parameter.substr(0, equals);
        if (name == L"l") return true;
        if (end == std::wstring_view::npos) break;
        at = end + 1;
    }
    return false;
}

bool HasQueryParameter(std::wstring_view path, std::wstring_view name) {
    const size_t query = path.find(L'?');
    if (query == std::wstring_view::npos) return false;
    for (size_t at = query + 1; at < path.size();) {
        const size_t end = std::min(path.find(L'&', at), path.size());
        const std::wstring_view parameter = path.substr(at, end - at);
        if (parameter.substr(0, parameter.find(L'=')) == name) return true;
        at = end + 1;
    }
    return false;
}

std::wstring WithoutQueryParameter(std::wstring_view path, std::wstring_view name) {
    const size_t query = path.find(L'?');
    if (query == std::wstring_view::npos) return std::wstring(path);
    std::wstring out(path.substr(0, query));
    wchar_t separator = L'?';
    for (size_t at = query + 1; at < path.size();) {
        const size_t end = std::min(path.find(L'&', at), path.size());
        const std::wstring_view parameter = path.substr(at, end - at);
        if (!parameter.empty() && parameter.substr(0, parameter.find(L'=')) != name) {
            out += separator;
            out += parameter;
            separator = L'&';
        }
        at = end + 1;
    }
    return out;
}

bool ChineseLocale() {
    if (!g_localeKnown.load(std::memory_order_acquire)) SetAppleLocale(ui::UsingChinese());
    return g_chineseLocale.load(std::memory_order_relaxed);
}

std::wstring WithLocale(std::wstring path) {
    if (HasLocaleParameter(path)) return path;
    path += path.find(L'?') == std::wstring::npos ? L'?' : L'&';
    path += ChineseLocale() ? L"l=zh-Hans-CN" : L"l=en-US";
    return path;
}

Object EmptyObject() {
    return Object();
}

} // namespace

BridgeHttpError::BridgeHttpError(int status, bool refused)
    : net::HttpStatusError(
          status, refused ? "Apple Music refused this library item (HTTP " + std::to_string(status) + ")."
                          : "Apple Music bridge request failed (HTTP " + std::to_string(status) + ")."),
      m_refused(refused) {}

bool BridgeHttpError::refused() const noexcept {
    return m_refused;
}

ScopedFixtureProbeTransaction::ScopedFixtureProbeTransaction() {
    auto config = ReadBridge();
    if (config.fixtureTransactionTag.empty()) Fail("The Apple Music probe requires a mock fixture bridge.");
    // All validation and allocation precede these nonthrowing swaps.
    m_previousUrl.swap(g_probeUrl);
    m_previousToken.swap(g_probeToken);
    m_previousTag.swap(g_probeTag);
    g_probeUrl.swap(config.url);
    g_probeToken.swap(config.token);
    g_probeTag.swap(config.fixtureTransactionTag);
}

ScopedFixtureProbeTransaction::~ScopedFixtureProbeTransaction() {
    g_probeUrl.swap(m_previousUrl);
    g_probeToken.swap(m_previousToken);
    g_probeTag.swap(m_previousTag);
}

std::string ProbeTransactionTag() {
    if (!g_probeTag.empty()) (void)ReadBridge();
    return g_probeTag;
}

std::wstring BridgeUrl() {
    return ReadBridge().url;
}

std::string BridgeToken() {
    return ReadBridge().token;
}

std::string BridgeSourceIdentity() {
    const BridgeConfig config = ReadBridge();
    if (!config.runtime.empty()) return "managed:" + Utf8(config.runtime.lexically_normal().wstring());
    return "external:" + Utf8(config.url);
}

void SetAppleLocale(bool chinese) {
    g_chineseLocale.store(chinese, std::memory_order_relaxed);
    g_localeKnown.store(true, std::memory_order_release);
}

std::wstring Encode(std::wstring_view value) { return Wide(core::UrlEncode(value)); }

namespace {

// `logMethod` / `logPath` name the call in the log; an Apple API call logs the API request it
// carries, not the bridge's one generic path.
Object BridgeRequest(std::wstring_view path, abort_callback& abort, std::wstring_view method,
                     std::string body, std::wstring logMethod, std::wstring logPath) {
    ValidateApiPath(path);
    const std::wstring verb = Method(method);
    const BridgeConfig config = ReadBridge();
    ServiceManager::Get().EnsureStarted(config.url, config.token, config.runtime, abort);

    net::JsonCall call;
    call.method = verb;
    call.url = config.url + std::wstring(path);
    call.logMethod = std::move(logMethod);
    call.logPath = logPath.empty() ? std::wstring(path) : std::move(logPath);
    call.body = std::move(body);
    call.noProxy = true;
    call.timeoutMs = 30000;
    call.maxBody = 32u * 1024u * 1024u;
    call.headers = {
        {L"Accept", L"application/json"},
        {L"X-API-Token", Wide(config.token)},
    };
    if (!g_probeTag.empty()) call.headers.emplace_back(L"X-Glasswave-Probe-Transaction", Wide(g_probeTag));
    if (!call.body.empty()) call.headers.emplace_back(L"Content-Type", L"application/json");

    net::JsonReply reply;
    try {
        reply = net::CallJson(std::move(call), abort);
    } catch (const net::TransportError& error) {
        // The transport's reason (a WinHTTP code, a truncated body); it holds no token or header.
        std::string message = "Cannot reach the Apple Music bridge.";
        if (!error.reason().empty()) message += " " + Utf8(error.reason());
        Fail(message.c_str());
    } catch (const net::HttpStatusError& error) {
        throw BridgeHttpError(error.status(),
                              error.body().find("webPlayback refused the library item") != std::string::npos);
    }
    if (reply.bodyEmpty) return EmptyObject();

    if (!reply.json) Fail("The Apple Music bridge returned invalid JSON.");
    const Object& parsed = *reply.json;
    if (!json::GetString(parsed, L"error").empty()) Fail("The Apple Music bridge reported an error.");
    if (const auto errors = json::GetArray(parsed, L"errors"); errors && errors->Size() != 0)
        Fail("The Apple Music bridge returned errors.");
    return parsed;
}

} // namespace

Object Request(std::wstring_view path, abort_callback& abort, std::wstring_view method, std::string body) {
    return BridgeRequest(path, abort, method, std::move(body), {}, {});
}

std::optional<LibraryPlaybackMapping> ResolveLibraryPlaybackMapping(std::wstring_view libraryId,
                                                                    std::wstring_view catalogId,
                                                                    abort_callback& abort) {
    if (libraryId.empty() || catalogId.empty()) return std::nullopt;
    try {
        const Object response =
            Apple(L"/v1/me/library/songs/" + Encode(libraryId) + L"?include=catalog", abort);
        const auto data = json::GetArray(response, L"data");
        if (!data || data->Size() != 1) return std::nullopt;
        const auto items = json::Objects(*data);
        if (items.size() != 1) return std::nullopt;

        const Object& item = items.front();
        if (json::GetString(item, L"id") != libraryId || json::GetString(item, L"type") != L"library-songs") {
            return std::nullopt;
        }
        const auto attributes = json::GetObject(item, L"attributes");
        const auto playback = attributes ? json::GetObject(*attributes, L"playParams") : std::nullopt;
        if (!playback || !json::GetBool(*playback, L"isLibrary") ||
            json::GetString(*playback, L"id") != libraryId ||
            json::GetString(*playback, L"kind") != L"song" ||
            json::GetString(*playback, L"catalogId") != catalogId ||
            !json::GetBool(*playback, L"reporting")) {
            return std::nullopt;
        }

        const std::wstring reportingId = json::GetString(*playback, L"reportingId");
        if (!ValidCatalogId(reportingId) || reportingId == catalogId ||
            (reportingId.size() > 2 && reportingId[1] == L'.')) {
            return std::nullopt;
        }
        return LibraryPlaybackMapping{std::wstring(libraryId), std::wstring(catalogId), reportingId};
    } catch (const exception_aborted&) {
        throw;
    } catch (...) {
        return std::nullopt;
    }
}

Object Apple(std::wstring_view path, abort_callback& abort, std::wstring_view method, const Object* body) {
    ValidateApplePath(path);
    const bool libraryDelete = Method(method) == L"DELETE" && path.starts_with(L"/v1/me/library/");
    std::wstring localizedPath = libraryDelete ? std::wstring(path) : WithLocale(std::wstring(path));
    const auto send = [&](const std::wstring& requestPath) {
        Object request;
        request.Insert(L"method", JsonValue::CreateStringValue(Method(method)));
        request.Insert(L"path", JsonValue::CreateStringValue(requestPath));
        request.Insert(L"body", body && *body ? static_cast<winrt::Windows::Data::Json::IJsonValue>(*body)
                                              : JsonValue::CreateNullValue());
        return BridgeRequest(L"/api/v1/glasswave/apple", abort, L"POST", Utf8(json::Stringify(request)),
                             L"apple " + Method(method), requestPath);
    };

    // The catalog host has been observed (2026-11) to answer every request that carries
    // "platform=web" with 400 "Platform web is not supported", while the same request
    // without the parameter succeeds; the library host accepts it. A catalog request that
    // is refused is sent once more without the parameter, and once that has worked the
    // parameter is left out of catalog requests for the rest of the run, so a refusal costs
    // one extra request per run instead of failing every catalog read.
    static std::atomic_bool catalogRefusesPlatform = false;
    const bool catalogWithPlatform =
        path.starts_with(L"/v1/catalog/") && HasQueryParameter(localizedPath, L"platform");
    if (!catalogWithPlatform) return send(localizedPath);
    const std::wstring withoutPlatform = WithoutQueryParameter(localizedPath, L"platform");
    if (catalogRefusesPlatform.load(std::memory_order_relaxed)) return send(withoutPlatform);
    try {
        return send(localizedPath);
    } catch (const BridgeHttpError& error) {
        if (error.status() != 400) throw;
    }
    Object response = send(withoutPlatform);
    catalogRefusesPlatform.store(true, std::memory_order_relaxed);
    return response;
}

std::vector<Object> Collection(std::wstring_view firstPath, abort_callback& abort) {
    ValidateApplePath(firstPath);
    std::vector<Object> out;
    std::set<std::wstring> seen;
    std::wstring path(firstPath);

    for (size_t page = 0; !path.empty(); ++page) {
        abort.check();
        if (page >= kCollectionPageLimit || !seen.emplace(path).second) {
            Fail("Apple Music pagination did not terminate.");
        }
        const Object response = Apple(path, abort);
        if (const auto data = json::GetArray(response, L"data")) {
            const auto objects = json::Objects(*data);
            if (out.size() + objects.size() > kCollectionItemLimit)
                Fail("Apple Music returned too many items.");
            out.insert(out.end(), objects.begin(), objects.end());
        }
        const std::wstring next = json::GetString(response, L"next");
        path = next.empty() ? std::wstring() : NormalizeNext(firstPath, next);
    }
    return out;
}

AccountState GetAccount(abort_callback& abort) {
    const Object response = Request(L"/api/v1/glasswave/account", abort);
    AccountState account;
    account.signedIn = json::GetBool(response, L"signedIn");
    account.accountId = json::GetString(response, L"accountId");
    account.name = json::GetString(response, L"name");
    if (!account.signedIn) return account;
    if (account.accountId.empty()) Fail("The Apple Music account has no stable identifier.");

    account.storefront = json::GetString(response, L"storefront");
    if (account.storefront.size() != 2 ||
        !std::all_of(account.storefront.begin(), account.storefront.end(), AsciiLetter)) {
        Fail("The Apple Music account returned an invalid storefront.");
    }
    for (auto& c : account.storefront)
        if (c >= L'a' && c <= L'z') c = static_cast<wchar_t>(c + L'A' - L'a');
    return account;
}

DrmState GetDrmState(abort_callback& abort) {
    const Object response = Request(L"/api/v1/drm/status", abort);
    DrmState out;
    out.available = json::GetBool(response, L"available");
    if (const auto state = json::GetObject(response, L"state")) {
        out.manager = json::GetString(*state, L"manager");
        out.process = json::GetString(*state, L"process");
        out.authentication = json::GetString(*state, L"authentication");
        out.fairplay = json::GetString(*state, L"fairplay");
        out.session = json::GetString(*state, L"session");
    }
    return out;
}

} // namespace glasswave::applemusic
