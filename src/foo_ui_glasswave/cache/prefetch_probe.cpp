#include "pch.h"

#include "cache/prefetch_host.h"

#include <SDK/initquit.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace glasswave::cache::prefetch_probe {
namespace {

namespace fs = std::filesystem;
namespace json = winrt::Windows::Data::Json;

using JsonArray = json::JsonArray;
using JsonObject = json::JsonObject;
using JsonValue = json::JsonValue;

constexpr std::string_view kProbe = "prefetch-host";
constexpr std::string_view kProvider = "glasswave-prefetch-test";
constexpr size_t kMaxMarkerBytes = 4096;
constexpr auto kMainCallTimeout = std::chrono::seconds(10);

struct Paths {
    fs::path request;
    fs::path claimed;
    fs::path result;
};

struct Marker {
    bool valid = false;
    std::string runId;
};

struct CaseResult {
    std::string name;
    bool ok = false;
    std::string detail;
};

struct ProbeResult {
    std::string runId;
    std::string error;
    bool hookInstalled = false;
    bool hookRestored = false;
    uint64_t activeAfter = 0;
    std::vector<CaseResult> cases;
    std::vector<PrefetchHostProbeEvent> events;
};

struct MainCall;

struct RunControl {
    std::mutex mutex;
    std::shared_ptr<MainCall> activeCall;
    std::atomic_bool stopping = false;
};

struct MainCall {
    std::mutex mutex;
    std::condition_variable changed;
    bool done = false;
    bool value = false;
};

bool SameName(const fs::path& left, const fs::path& right) {
    return _wcsicmp(left.c_str(), right.c_str()) == 0;
}

std::optional<fs::path> FinalPath(const fs::path& path, bool directory) {
    const DWORD flags = directory ? FILE_FLAG_BACKUP_SEMANTICS : FILE_ATTRIBUTE_NORMAL;
    HANDLE handle = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                OPEN_EXISTING, flags, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return std::nullopt;

    std::array<wchar_t, 32768> buffer{};
    const DWORD length = GetFinalPathNameByHandleW(handle, buffer.data(), static_cast<DWORD>(buffer.size()),
                                                   FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    CloseHandle(handle);
    if (!length || length >= buffer.size()) return std::nullopt;

    std::wstring text(buffer.data(), length);
    if (text.rfind(L"\\\\?\\UNC\\", 0) == 0)
        text = L"\\\\" + text.substr(8);
    else if (text.rfind(L"\\\\?\\", 0) == 0)
        text.erase(0, 4);
    if (directory) {
        const size_t rootLength = fs::path(text).root_path().native().size();
        while (text.size() > rootLength && (text.back() == L'\\' || text.back() == L'/'))
            text.pop_back();
    }
    return fs::path(text).lexically_normal();
}

std::optional<fs::path> NativeProfileDirectory() {
    pfc::string8 native;
    try {
        if (!filesystem::g_get_native_path(core_api::get_profile_path(), native)) return std::nullopt;
    } catch (...) {
        return std::nullopt;
    }
    return FinalPath(fs::path(pfc::stringcvt::string_wide_from_utf8(native.get_ptr()).get_ptr()), true);
}

std::optional<Paths> ProbePaths() {
    const auto profile = NativeProfileDirectory();
    if (!profile || !SameName(profile->filename(), fs::path(L"profile")) ||
        !SameName(profile->parent_path().filename(), fs::path(L"fb2k-dev")) ||
        !SameName(profile->parent_path().parent_path().filename(), fs::path(L".local")))
        return std::nullopt;

    const fs::path directory = *profile / L"glasswave" / L"prefetch-probe";
    return Paths{
        directory / L"request.json",
        directory / L"request.claimed.json",
        directory / L"result.json",
    };
}

std::optional<std::string> ReadBounded(const fs::path& path, size_t limit) {
    std::error_code error;
    const uintmax_t size = fs::file_size(path, error);
    if (error || size == 0 || size > limit) return std::nullopt;
    std::ifstream input(path, std::ios::binary);
    if (!input) return std::nullopt;
    std::string bytes(static_cast<size_t>(size), '\0');
    input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!input || input.gcount() != static_cast<std::streamsize>(bytes.size())) return std::nullopt;
    return bytes;
}

bool ValidRunId(std::string_view value) {
    if (value.size() < 8 || value.size() > 64) return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-';
    });
}

Marker ReadMarker(std::string_view bytes) {
    Marker marker;
    try {
        const JsonObject object = JsonObject::Parse(winrt::to_hstring(std::string(bytes)));
        const auto probe = object.GetNamedString(L"probe");
        marker.runId = winrt::to_string(object.GetNamedString(L"runId"));
        marker.valid = wcscmp(probe.c_str(), L"prefetch-host") == 0 && ValidRunId(marker.runId);
    } catch (...) {
    }
    return marker;
}

bool OnMain(RunControl& control, abort_callback& abort, std::function<bool()> action) {
    auto call = std::make_shared<MainCall>();
    {
        std::lock_guard lock(control.mutex);
        if (control.stopping.load(std::memory_order_acquire) || abort.is_aborting() || control.activeCall)
            return false;
        control.activeCall = call;
    }
    try {
        fb2k::inMainThread([call, action = std::move(action)]() mutable {
            bool value = false;
            try {
                value = action();
            } catch (...) {
            }
            {
                std::lock_guard lock(call->mutex);
                call->value = value;
                call->done = true;
            }
            call->changed.notify_all();
        });
    } catch (...) {
        std::lock_guard lock(control.mutex);
        if (control.activeCall == call) control.activeCall.reset();
        return false;
    }

    std::unique_lock lock(call->mutex);
    const auto finished =
        call->changed.wait_until(lock, std::chrono::steady_clock::now() + kMainCallTimeout, [&] {
            return call->done || control.stopping.load(std::memory_order_acquire) || abort.is_aborting();
        });
    {
        std::lock_guard controlLock(control.mutex);
        if (control.activeCall == call) control.activeCall.reset();
    }
    return finished && call->done && call->value && !control.stopping.load(std::memory_order_acquire) &&
           !abort.is_aborting();
}

PrefetchHostProbeSnapshot Snapshot(double remaining, std::string candidateToken, std::string itemPath,
                                   std::string contextFingerprint, bool stopAfterCurrent = false) {
    PrefetchHostProbeSnapshot snapshot;
    snapshot.playback.playing = true;
    snapshot.playback.stopAfterCurrent = stopAfterCurrent;
    snapshot.playback.networkAllowed = true;
    snapshot.playback.speculativePrefetchAllowed = true;
    snapshot.playback.remainingSeconds = remaining;
    snapshot.playback.next.itemPath = std::move(itemPath);
    snapshot.playback.next.proof =
        snapshot.playback.next.itemPath.empty() ? CandidateProof::None : CandidateProof::CoreLookahead;
    snapshot.currentToken = "prefetch-probe-current";
    snapshot.candidateToken = std::move(candidateToken);
    snapshot.contextFingerprint = std::move(contextFingerprint);
    return snapshot;
}

bool DispatchSnapshot(RunControl& control, abort_callback& abort, PrefetchHostProbeSnapshot snapshot) {
    return OnMain(control, abort, [snapshot = std::move(snapshot)] {
        return PrefetchHostProbeAccess::SetSnapshot(snapshot) &&
               PrefetchHostProbeAccess::DispatchPlaybackTime();
    });
}

std::vector<PrefetchHostProbeEvent> Events() {
    return PrefetchHostProbeAccess::Events();
}

uint64_t LastStarted(const std::vector<PrefetchHostProbeEvent>& events) {
    for (auto it = events.rbegin(); it != events.rend(); ++it)
        if (it->kind == PrefetchHostProbeEventKind::Started) return it->requestId;
    return 0;
}

uint64_t LastCancelled(const std::vector<PrefetchHostProbeEvent>& events) {
    for (auto it = events.rbegin(); it != events.rend(); ++it)
        if (it->kind == PrefetchHostProbeEventKind::Cancelled) return it->requestId;
    return 0;
}

void AddCase(ProbeResult& result, std::string name, bool ok, std::string detail = {}) {
    result.cases.push_back({std::move(name), ok, std::move(detail)});
}

std::string EventName(PrefetchHostProbeEventKind kind) {
    return kind == PrefetchHostProbeEventKind::Started ? "started" : "cancelled";
}

std::string ProofName(CandidateProof proof) {
    switch (proof) {
    case CandidateProof::LinearPlaylist:
        return "linear";
    case CandidateProof::RepeatOne:
        return "repeat-one";
    case CandidateProof::CoreQueue:
        return "core-queue";
    case CandidateProof::CoreLookahead:
        return "core-lookahead";
    case CandidateProof::None:
        return "none";
    }
    return "unknown";
}

JsonObject ResultJson(const ProbeResult& result, bool ok) {
    JsonObject object;
    object.Insert(L"probe", JsonValue::CreateStringValue(L"prefetch-host"));
    object.Insert(L"provider", JsonValue::CreateStringValue(winrt::to_hstring(std::string(kProvider))));
    object.Insert(L"runId", JsonValue::CreateStringValue(winrt::to_hstring(result.runId)));
    object.Insert(L"ok", JsonValue::CreateBooleanValue(ok));
    object.Insert(L"error", result.error.empty()
                                ? JsonValue::CreateNullValue()
                                : JsonValue::CreateStringValue(winrt::to_hstring(result.error)));
    object.Insert(L"hookInstalled", JsonValue::CreateBooleanValue(result.hookInstalled));
    object.Insert(L"hookRestored", JsonValue::CreateBooleanValue(result.hookRestored));
    object.Insert(L"activeAfter", JsonValue::CreateNumberValue(static_cast<double>(result.activeAfter)));

    JsonArray cases;
    for (const auto& item : result.cases) {
        JsonObject row;
        row.Insert(L"name", JsonValue::CreateStringValue(winrt::to_hstring(item.name)));
        row.Insert(L"ok", JsonValue::CreateBooleanValue(item.ok));
        row.Insert(L"detail", item.detail.empty()
                                  ? JsonValue::CreateNullValue()
                                  : JsonValue::CreateStringValue(winrt::to_hstring(item.detail)));
        cases.Append(row);
    }
    object.Insert(L"cases", cases);
    object.Insert(L"caseCount", JsonValue::CreateNumberValue(static_cast<double>(result.cases.size())));

    JsonArray events;
    for (const auto& item : result.events) {
        JsonObject row;
        row.Insert(L"kind", JsonValue::CreateStringValue(winrt::to_hstring(EventName(item.kind))));
        row.Insert(L"requestId", JsonValue::CreateNumberValue(static_cast<double>(item.requestId)));
        row.Insert(L"contextGeneration",
                   JsonValue::CreateNumberValue(static_cast<double>(item.contextGeneration)));
        row.Insert(L"trackGeneration",
                   JsonValue::CreateNumberValue(static_cast<double>(item.trackGeneration)));
        row.Insert(L"candidateGeneration",
                   JsonValue::CreateNumberValue(static_cast<double>(item.candidateGeneration)));
        row.Insert(L"itemPath", JsonValue::CreateStringValue(winrt::to_hstring(item.itemPath)));
        row.Insert(L"proof", JsonValue::CreateStringValue(winrt::to_hstring(ProofName(item.proof))));
        events.Append(row);
    }
    object.Insert(L"events", events);
    object.Insert(L"eventCount", JsonValue::CreateNumberValue(static_cast<double>(result.events.size())));
    return object;
}

bool WriteAll(HANDLE file, std::string_view bytes) {
    size_t offset = 0;
    while (offset < bytes.size()) {
        const DWORD count =
            static_cast<DWORD>(std::min<size_t>(bytes.size() - offset, std::numeric_limits<DWORD>::max()));
        DWORD written = 0;
        if (!WriteFile(file, bytes.data() + offset, count, &written, nullptr) || written == 0) return false;
        offset += written;
    }
    return true;
}

bool WriteResult(const fs::path& resultPath, const ProbeResult& result, bool ok) {
    const fs::path temporary = resultPath.wstring() + L".tmp";
    const std::string bytes = winrt::to_string(ResultJson(result, ok).Stringify());
    HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    const bool written = WriteAll(file, bytes) && FlushFileBuffers(file);
    CloseHandle(file);
    if (!written || !MoveFileExW(temporary.c_str(), resultPath.c_str(),
                                 MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temporary.c_str());
        return false;
    }
    return true;
}

void RunCases(const Paths& paths, RunControl& control, abort_callback& abort, ProbeResult& result) {
    const auto requestBytes = ReadBounded(paths.claimed, kMaxMarkerBytes);
    DeleteFileW(paths.claimed.c_str());
    if (!requestBytes) {
        result.error = "request_read_failed";
        return;
    }

    const Marker marker = ReadMarker(*requestBytes);
    if (!marker.valid) {
        result.error = "invalid_probe_marker";
        return;
    }
    result.runId = marker.runId;

    if (!OnMain(control, abort, [] {
            return PrefetchHostProbeAccess::Install(
                Snapshot(30.001, "candidate-a", "prefetch-probe-next-a", "context-a"));
        })) {
        result.error = "hook_install_failed";
        return;
    }
    result.hookInstalled = true;
    AddCase(result, "callback_host_installed", true);

    if (!DispatchSnapshot(control, abort,
                          Snapshot(30.001, "candidate-a", "prefetch-probe-next-a", "context-a"))) {
        result.error = "threshold_before_dispatch_failed";
        return;
    }
    auto events = Events();
    AddCase(result, "threshold_before_30_seconds", events.empty(),
            events.empty() ? std::string() : "prefetched_before_threshold");
    if (!events.empty()) return;

    if (!DispatchSnapshot(control, abort,
                          Snapshot(30.0, "candidate-a", "prefetch-probe-next-a", "context-a"))) {
        result.error = "threshold_dispatch_failed";
        return;
    }
    events = Events();
    const bool thresholdStarted = events.size() == 1 &&
                                  events[0].kind == PrefetchHostProbeEventKind::Started &&
                                  events[0].itemPath == "prefetch-probe-next-a";
    AddCase(result, "threshold_at_30_seconds", thresholdStarted,
            thresholdStarted ? std::string() : "expected_one_start_at_threshold");
    if (!thresholdStarted) return;

    if (!DispatchSnapshot(control, abort,
                          Snapshot(29.9, "candidate-a", "prefetch-probe-next-a", "context-a"))) {
        result.error = "progress_dispatch_failed";
        return;
    }
    events = Events();
    AddCase(result, "progress_single_flight", events.size() == 1,
            events.size() == 1 ? std::string() : "duplicate_start_on_progress");

    const uint64_t firstRequest = events.front().requestId;
    if (!OnMain(control, abort, [] { return PrefetchHostProbeAccess::NotifyContextChanged(); })) {
        result.error = "context_change_failed";
        return;
    }
    events = Events();
    const bool contextCancelled = events.size() == 2 &&
                                  events[1].kind == PrefetchHostProbeEventKind::Cancelled &&
                                  events[1].requestId == firstRequest;
    AddCase(result, "context_change_cancel", contextCancelled,
            contextCancelled ? std::string() : "context_change_did_not_cancel");
    if (!contextCancelled) return;

    if (!OnMain(control, abort,
                [firstRequest] { return PrefetchHostProbeAccess::CompletePrime(firstRequest); })) {
        result.error = "context_cancel_completion_failed";
        return;
    }
    events = Events();
    const bool contextReplacement = events.size() == 3 &&
                                    events[2].kind == PrefetchHostProbeEventKind::Started &&
                                    events[2].contextGeneration > events[0].contextGeneration;
    AddCase(result, "context_change_replacement", contextReplacement,
            contextReplacement ? std::string() : "replacement_generation_not_advanced");
    if (!contextReplacement) return;

    const uint64_t secondRequest = events[2].requestId;
    if (!DispatchSnapshot(control, abort,
                          Snapshot(29.8, "candidate-b", "prefetch-probe-next-b", "context-a"))) {
        result.error = "candidate_change_dispatch_failed";
        return;
    }
    events = Events();
    const bool candidateCancelled = events.size() == 4 &&
                                    events[3].kind == PrefetchHostProbeEventKind::Cancelled &&
                                    events[3].requestId == secondRequest;
    AddCase(result, "candidate_change_cancel", candidateCancelled,
            candidateCancelled ? std::string() : "candidate_change_did_not_cancel");
    if (!candidateCancelled) return;

    if (!OnMain(control, abort,
                [secondRequest] { return PrefetchHostProbeAccess::CompletePrime(secondRequest); })) {
        result.error = "candidate_cancel_completion_failed";
        return;
    }
    events = Events();
    const bool candidateReplacement = events.size() == 5 &&
                                      events[4].kind == PrefetchHostProbeEventKind::Started &&
                                      events[4].itemPath == "prefetch-probe-next-b" &&
                                      events[4].candidateGeneration > events[2].candidateGeneration;
    AddCase(result, "candidate_change_replacement", candidateReplacement,
            candidateReplacement ? std::string() : "candidate_replacement_missing");
    if (!candidateReplacement) return;

    const uint64_t thirdRequest = events[4].requestId;
    if (!DispatchSnapshot(control, abort, Snapshot(29.7, {}, {}, "context-a", true))) {
        result.error = "stop_after_current_dispatch_failed";
        return;
    }
    events = Events();
    const bool stopCancelled = events.size() == 6 &&
                               events[5].kind == PrefetchHostProbeEventKind::Cancelled &&
                               events[5].requestId == thirdRequest;
    AddCase(result, "stop_after_current_cancel", stopCancelled,
            stopCancelled ? std::string() : "stop_after_current_did_not_cancel");
    if (!stopCancelled) return;

    if (!OnMain(control, abort,
                [thirdRequest] { return PrefetchHostProbeAccess::CompletePrime(thirdRequest); })) {
        result.error = "stop_cancel_completion_failed";
        return;
    }
    events = Events();
    result.events = events;
    result.activeAfter = PrefetchHostProbeAccess::ActivePrimeRequestId();
    AddCase(result, "no_replacement_after_stop", result.activeAfter == 0 && events.size() == 6,
            result.activeAfter == 0 ? std::string() : "active_prime_after_stop");
}

void RunProbe(const Paths& paths, RunControl& control, abort_callback& abort) {
    SetThreadDescription(GetCurrentThread(), L"Glasswave prefetch host probe");
    winrt::init_apartment(winrt::apartment_type::multi_threaded);

    ProbeResult result;
    bool installed = false;
    try {
        RunCases(paths, control, abort, result);
        installed = result.hookInstalled;
    } catch (const exception_aborted&) {
        result.error = "cancelled";
    } catch (...) {
        result.error = "probe_exception";
    }

    if (installed) {
        if (OnMain(control, abort, [] { return PrefetchHostProbeAccess::Uninstall(); })) {
            result.hookRestored = true;
        } else {
            result.error = result.error.empty() ? "hook_restore_failed" : result.error;
        }
    }

    const bool casesPass =
        !result.cases.empty() &&
        std::all_of(result.cases.begin(), result.cases.end(), [](const CaseResult& item) { return item.ok; });
    AddCase(result, "hooks_restored", result.hookRestored,
            result.hookRestored ? std::string() : "hook_not_restored");
    const bool ok = result.error.empty() && result.hookInstalled && result.hookRestored &&
                    result.activeAfter == 0 && casesPass && result.events.size() == 6;
    WriteResult(paths.result, result, ok);
    winrt::uninit_apartment();
}

class PrefetchProbeLifecycle : public initquit {
  public:
    ~PrefetchProbeLifecycle() { Stop(); }

    void on_init() override {
        const auto paths = ProbePaths();
        if (!paths) return;
        std::error_code error;
        if (!fs::is_regular_file(paths->request, error) || error) return;

        DeleteFileW(paths->result.c_str());
        if (!MoveFileExW(paths->request.c_str(), paths->claimed.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            return;

        m_abort = std::make_unique<abort_callback_impl>();
        m_control = std::make_shared<RunControl>();
        auto* abort = m_abort.get();
        auto control = m_control;
        m_worker = std::thread([paths = *paths, control, abort] { RunProbe(paths, *control, *abort); });
    }

    void on_quit() override { Stop(); }

  private:
    void Stop() noexcept {
        std::shared_ptr<MainCall> activeCall;
        if (m_control) {
            {
                std::lock_guard lock(m_control->mutex);
                m_control->stopping.store(true, std::memory_order_release);
                activeCall = m_control->activeCall;
            }
            if (activeCall) activeCall->changed.notify_all();
        }
        if (m_abort) m_abort->abort();
        if (m_worker.joinable()) m_worker.join();
        if (PrefetchHost::Existing()) PrefetchHostProbeAccess::Uninstall();
        m_control.reset();
        m_abort.reset();
    }

    std::shared_ptr<RunControl> m_control;
    std::unique_ptr<abort_callback_impl> m_abort;
    std::thread m_worker;
};

FB2K_SERVICE_FACTORY(PrefetchProbeLifecycle);

} // namespace
} // namespace glasswave::cache::prefetch_probe
