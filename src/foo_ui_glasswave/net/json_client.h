#pragma once

#include "core/json.h"
#include "net/http.h"

#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace glasswave::net {

// The request never produced a reply (status 0) or the reply was cut short ("ended early",
// "too large"). `reason()` is the transport's own text; it holds no token or header.
class TransportError : public std::runtime_error {
  public:
    explicit TransportError(std::wstring reason);
    const std::wstring& reason() const noexcept { return m_reason; }

  private:
    std::wstring m_reason;
};

// A complete reply whose status is not 2xx. The body is kept for the caller that needs to
// tell failures apart; it is never part of what().
class HttpStatusError : public std::runtime_error {
  public:
    HttpStatusError(int status, std::string message, std::string body = {});
    int status() const noexcept { return m_status; }
    const std::string& body() const noexcept { return m_body; }

  private:
    int m_status;
    std::string m_body;
};

struct JsonCall {
    std::wstring method = L"GET";
    std::wstring url;
    // The line written to the log for this call; the URL's own path is used when empty. The
    // path must carry no query, token or cookie.
    std::wstring logMethod;
    std::wstring logPath;
    std::vector<std::pair<std::wstring, std::wstring>> headers;
    std::string body;
    bool noProxy = true;
    int timeoutMs = 30000;
    size_t maxBody = 32u * 1024u * 1024u;
    // Throw HttpStatusError for a non-2xx reply. A source whose error answers carry a JSON
    // envelope of their own clears this and reads the status from the reply.
    bool throwOnStatus = true;
};

struct JsonReply {
    unsigned long status = 0;
    bool bodyEmpty = false;
    // Empty when the body is empty or is not a JSON object; `parseError` then says why.
    std::optional<core::json::Object> json;
    std::wstring parseError;
    std::vector<std::wstring> setCookies;
};

// One JSON-over-HTTP call: timing, one log line per call (failures included), then
// TransportError / HttpStatusError, then core::json::Parse. Body-level envelopes (NetEase's
// "code", the Apple bridge's "error") stay with the provider. The request body is moved into
// the request and zeroed once the call returns (a sign-in body carries a password).
JsonReply CallJson(JsonCall call, abort_callback& abort);

} // namespace glasswave::net
