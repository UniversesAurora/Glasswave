#pragma once

#include <string>
#include <utility>

namespace glasswave::stream {

// Where a provider stands with its account, kept the same way by every source: nothing is
// known yet, a cached snapshot is shown while the session is checked (restoring), or the
// service has confirmed who is signed in (verified). The reads of a restoring provider stop at
// the list session's gate; its cached rows stay visible.
//
// No locking: the provider keeps it next to the rest of its account state and guards both with
// the same mutex, so a transition changes the gate and that state together.
struct AccountGate {
    bool verified = false;    // the service answered with this account
    bool authPending = false; // a sign-in or a start-up check is under way
    bool restoring = false;   // a cached snapshot of `cachedId` is shown until it is verified
    std::wstring cachedId;    // the account the snapshot belongs to

    // The state of a session that knows nothing yet; `pending` while a check is expected.
    void Reset(bool pending = false) {
        verified = false;
        authPending = pending;
        restoring = false;
        cachedId.clear();
    }
    // A snapshot of `accountId` is shown while the session is verified.
    void BeginRestore(std::wstring accountId) {
        cachedId = std::move(accountId);
        restoring = true;
    }
    // The service confirmed the account.
    void Verify() {
        Reset(false);
        verified = true;
    }
    // What Provider::SessionRestoring reports.
    bool Waiting() const { return restoring || authPending; }
};

} // namespace glasswave::stream
