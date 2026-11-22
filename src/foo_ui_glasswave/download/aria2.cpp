#include "pch.h"
#include "download/aria2.h"
#include "core/file.h"
#include "core/hex.h"
#include "core/text.h"
#include "core/paths.h"

#include "net/http.h"
#include "ui/text.h"

#include <bcrypt.h>
#include <miniz.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winsock2.h>

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <mutex>

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "ws2_32.lib")

namespace glasswave::download::aria2 {

namespace {

using namespace winrt::Windows::Data::Json;
using core::Utf8;
using core::Wide;

// The release installed when there is no aria2c.exe yet: aria2 1.37.0, the official 64-bit
// Windows build. Its RPC interface is stable, so a newer aria2c.exe can simply replace it (or a
// newer release here).
constexpr wchar_t kZipUrl[] =
    L"https://github.com/aria2/aria2/releases/download/release-1.37.0/aria2-1.37.0-win-64bit-build1.zip";
constexpr char kExeInZip[] = "aria2-1.37.0-win-64bit-build1/aria2c.exe";

// g_mutex guards the state below and is held only for short reads and writes, never across the
// install (a 16 MB download) or the engine's start (up to ten seconds): Version() and
// RetryAfterFailure() are called on the main thread. One Ensure installs and starts; the others
// wait on g_started.
std::mutex g_mutex;
std::condition_variable g_started;
bool g_starting = false;
HANDLE g_process = nullptr, g_job = nullptr;
std::wstring g_base; // http://127.0.0.1:<port>/jsonrpc
std::wstring g_secret;
std::wstring g_version;
std::wstring g_lastError;
bool g_failed = false; // until the next download command (RetryAfterFailure)

std::filesystem::path Folder() {
    return core::ProfileDir(L"aria2");
}

std::filesystem::path ExePath() {
    const auto folder = Folder();
    return folder.empty() ? folder : folder / L"aria2c.exe";
}

std::wstring RandomSecret() {
    return Wide(net::RandomHex(16));
}

int PickLoopbackPort() {
    return net::PickLoopbackPort();
}

// Downloads the release, checks it and puts aria2c.exe in place.
bool Install(abort_callback& abort, std::wstring& error) {
    const auto exe = ExePath();
    std::error_code ec;
    if (exe.empty() || (std::filesystem::create_directories(exe.parent_path(), ec), ec)) {
        error = L"Cannot create the download engine's folder.";
        return false;
    }
    net::HttpOptions options;
    options.timeoutMs = 120000;
    options.maxBodyBytes = 16u * 1024u * 1024u;
    const net::HttpResponse response = net::SendHttp(kZipUrl, options, abort);
    if (!response.ok()) {
        error = response.error.empty() ? L"Cannot download aria2 (HTTP " + std::to_wstring(response.status) + L")."
                                       : response.error;
        return false;
    }
    mz_zip_archive zip{};
    if (!mz_zip_reader_init_mem(&zip, response.body.data(), response.body.size(), 0)) {
        error = L"Cannot open the aria2 archive.";
        return false;
    }
    size_t size = 0;
    void* data = mz_zip_reader_extract_file_to_heap(&zip, kExeInZip, &size, 0);
    mz_zip_reader_end(&zip);
    if (!data) {
        error = L"The aria2 archive has no aria2c.exe.";
        return false;
    }
    bool written = true;
    try {
        core::WriteFileAtomic(exe, std::string_view(static_cast<const char*>(data), size));
    } catch (const std::exception&) {
        written = false;
    }
    mz_free(data);
    if (!written) {
        error = L"Cannot install aria2.";
        return false;
    }
    FB2K_console_formatter() << "Glasswave: installed aria2 1.37.0";
    return true;
}

void StopLocked() {
    if (g_job) CloseHandle(std::exchange(g_job, nullptr)); // kill-on-close ends aria2
    if (g_process) {
        WaitForSingleObject(g_process, 2000);
        CloseHandle(std::exchange(g_process, nullptr));
    }
    g_base.clear();
    g_version.clear();
}

bool RunningLocked() { return g_process && WaitForSingleObject(g_process, 0) == WAIT_TIMEOUT; }

// One JSON-RPC call; throws with aria2's message. The base address is read under the lock.
JsonValue Call(const std::wstring& base, const std::wstring& secret, const wchar_t* method, JsonArray params,
               abort_callback& abort) {
    JsonArray all;
    all.Append(JsonValue::CreateStringValue(L"token:" + secret));
    for (const auto& param : params)
        all.Append(param);
    JsonObject request;
    request.SetNamedValue(L"jsonrpc", JsonValue::CreateStringValue(L"2.0"));
    request.SetNamedValue(L"id", JsonValue::CreateStringValue(L"glasswave"));
    request.SetNamedValue(L"method", JsonValue::CreateStringValue(method));
    request.SetNamedValue(L"params", all);
    net::HttpOptions options;
    options.method = L"POST";
    options.noProxy = true;
    options.timeoutMs = 10000;
    options.headers = {{L"Content-Type", L"application/json"}};
    options.body = Utf8(std::wstring(request.Stringify()));
    const net::HttpResponse response = net::SendHttp(base, options, abort);
    if (response.status == 0)
        throw pfc::exception(Utf8(L"The download engine does not answer: " + response.error).c_str());
    JsonObject answer;
    if (!JsonObject::TryParse(Wide(response.body.c_str()), answer))
        throw pfc::exception("The download engine sent an unreadable answer.");
    if (answer.HasKey(L"error")) {
        const auto failure = answer.GetNamedObject(L"error");
        throw pfc::exception(Utf8(std::wstring(failure.GetNamedString(L"message", L"aria2 error"))).c_str());
    }
    return answer.GetNamedValue(L"result");
}

// Starts aria2 and waits until it answers. Not under g_mutex: the lock is taken only to publish
// the process and to read it back while polling.
bool StartEngine(std::wstring& error) {
    {
        std::lock_guard lock(g_mutex);
        StopLocked();
    }
    const int port = PickLoopbackPort();
    if (!port) {
        error = L"Cannot find a free loopback port for aria2.";
        return false;
    }
    const std::wstring secret = RandomSecret();
    const auto exe = ExePath();
    const auto folder = exe.parent_path();
    std::wstring command =
        L"\"" + exe.wstring() + L"\" --no-conf=true --enable-rpc=true --rpc-listen-all=false" +
        L" --rpc-listen-port=" + std::to_wstring(port) + L" --rpc-secret=" + secret +
        L" --stop-with-process=" + std::to_wstring(GetCurrentProcessId()) +
        L" --max-concurrent-downloads=16 --continue=true --file-allocation=none" +
        L" --auto-file-renaming=false --allow-overwrite=true --max-tries=5 --retry-wait=2" +
        L" --disk-cache=16M --summary-interval=0 --show-console-readout=false --console-log-level=error" +
        L" --log-level=warn --log=\"" + (folder / L"aria2.log").wstring() + L"\"";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | CREATE_SUSPENDED,
                        nullptr, folder.c_str(), &startup, &process)) {
        error = L"Cannot start aria2.";
        return false;
    }
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
        info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &info, sizeof(info));
        if (!AssignProcessToJobObject(job, process.hProcess)) CloseHandle(std::exchange(job, nullptr));
    }
    ResumeThread(process.hThread);
    CloseHandle(process.hThread);
    const std::wstring base = L"http://127.0.0.1:" + std::to_wstring(port) + L"/jsonrpc";
    {
        std::lock_guard lock(g_mutex);
        g_process = process.hProcess;
        g_job = job;
        g_secret = secret;
        g_base = base;
    }

    for (int attempt = 0; attempt < 40; ++attempt) { // up to ten seconds
        {
            std::lock_guard lock(g_mutex);
            if (g_process != process.hProcess || !RunningLocked()) break; // exited, or shut down
        }
        try {
            abort_callback_impl local;
            const auto result = Call(base, secret, L"aria2.getVersion", JsonArray(), local);
            const std::wstring version = result.GetObject().GetNamedString(L"version", L"").c_str();
            {
                std::lock_guard lock(g_mutex);
                if (g_process != process.hProcess) break;
                g_version = version;
            }
            FB2K_console_formatter() << "Glasswave: aria2 " << Utf8(version).c_str() << " ready";
            return true;
        } catch (const std::exception&) {
        }
        Sleep(250);
    }
    {
        std::lock_guard lock(g_mutex);
        if (g_process == process.hProcess) StopLocked();
    }
    error = L"aria2 did not start.";
    return false;
}

JsonValue Str(const std::wstring& text) { return JsonValue::CreateStringValue(text); }

} // namespace

bool Ensure(abort_callback& abort, std::wstring* error) {
    {
        std::unique_lock lock(g_mutex);
        for (;;) {
            if (RunningLocked() && !g_base.empty()) return true;
            if (g_failed) {
                if (error) *error = g_lastError;
                return false;
            }
            if (!g_starting) break;
            // Another worker is installing or starting the engine: wait for it, still abortable.
            g_started.wait_for(lock, std::chrono::milliseconds(200));
            abort.check();
        }
        g_starting = true;
    }
    struct Done {
        ~Done() {
            {
                std::lock_guard lock(g_mutex);
                g_starting = false;
            }
            g_started.notify_all();
        }
    } done;

    std::wstring why;
    const bool ok = (std::filesystem::exists(ExePath()) || Install(abort, why)) && StartEngine(why);
    if (ok) {
        std::lock_guard lock(g_mutex);
        g_failed = false;
        g_lastError.clear();
        return true;
    }
    abort.check(); // a pause or exit is not a failure of the engine
    {
        std::lock_guard lock(g_mutex);
        g_failed = true;
        g_lastError = why;
    }
    FB2K_console_formatter() << "Glasswave: aria2 unavailable: " << Utf8(why).c_str();
    if (error) *error = why;
    return false;
}

std::string Add(const stream::DownloadLink& link, const std::wstring& dir, const std::wstring& out, int connections,
                abort_callback& abort) {
    std::wstring base, secret;
    {
        std::lock_guard lock(g_mutex);
        base = g_base;
        secret = g_secret;
    }
    if (base.empty()) throw pfc::exception("The download engine is not running.");
    JsonArray uris;
    uris.Append(Str(link.url));
    JsonArray headers;
    for (const auto& [name, value] : link.headers)
        headers.Append(Str(name + L": " + value));
    const std::wstring n = std::to_wstring(std::clamp(connections, 1, 16));
    JsonObject options;
    options.SetNamedValue(L"dir", Str(dir));
    options.SetNamedValue(L"out", Str(out));
    options.SetNamedValue(L"split", Str(n));
    options.SetNamedValue(L"max-connection-per-server", Str(n));
    options.SetNamedValue(L"min-split-size", Str(L"1M"));
    options.SetNamedValue(L"header", headers);
    JsonArray params;
    params.Append(uris);
    params.Append(options);
    return Utf8(std::wstring(Call(base, secret, L"aria2.addUri", params, abort).GetString()));
}

Status Tell(const std::string& gid, abort_callback& abort) {
    std::wstring base, secret;
    {
        std::lock_guard lock(g_mutex);
        base = g_base;
        secret = g_secret;
    }
    if (base.empty()) throw pfc::exception("The download engine stopped.");
    JsonArray keys;
    for (const wchar_t* key : {L"status", L"totalLength", L"completedLength", L"downloadSpeed", L"connections",
                               L"errorMessage"})
        keys.Append(Str(key));
    JsonArray params;
    params.Append(Str(Wide(gid.c_str())));
    params.Append(keys);
    const auto result = Call(base, secret, L"aria2.tellStatus", params, abort).GetObject();
    const auto number = [&](const wchar_t* key) {
        return std::wcstoull(std::wstring(result.GetNamedString(key, L"0")).c_str(), nullptr, 10);
    };
    Status status;
    const std::wstring kind(result.GetNamedString(L"status", L""));
    status.kind = kind == L"active"     ? Status::Kind::Active
                  : kind == L"paused"   ? Status::Kind::Paused
                  : kind == L"error"    ? Status::Kind::Error
                  : kind == L"complete" ? Status::Kind::Complete
                  : kind == L"removed"  ? Status::Kind::Removed
                                        : Status::Kind::Waiting;
    status.total = number(L"totalLength");
    status.completed = number(L"completedLength");
    status.speed = number(L"downloadSpeed");
    status.connections = static_cast<int>(number(L"connections"));
    status.error = result.GetNamedString(L"errorMessage", L"").c_str();
    return status;
}

void Remove(const std::string& gid) {
    std::wstring base, secret;
    {
        std::lock_guard lock(g_mutex);
        base = g_base;
        secret = g_secret;
    }
    if (base.empty()) return;
    abort_callback_impl local;
    JsonArray params;
    params.Append(Str(Wide(gid.c_str())));
    try {
        Call(base, secret, L"aria2.forceRemove", params, local);
    } catch (const std::exception&) {
    }
    // Wait for aria2 to let the file go (it writes its control file), then drop the record.
    for (int i = 0; i < 20; ++i) {
        try {
            JsonArray statusParams;
            statusParams.Append(Str(Wide(gid.c_str())));
            JsonArray keys;
            keys.Append(Str(L"status"));
            statusParams.Append(keys);
            const auto result = Call(base, secret, L"aria2.tellStatus", statusParams, local).GetObject();
            if (result.GetNamedString(L"status", L"") != L"active") break;
        } catch (const std::exception&) {
            break;
        }
        Sleep(50);
    }
    try {
        Call(base, secret, L"aria2.removeDownloadResult", params, local);
    } catch (const std::exception&) {
    }
}

void RetryAfterFailure() {
    std::lock_guard lock(g_mutex);
    g_failed = false;
}

std::wstring Version() {
    std::lock_guard lock(g_mutex);
    return RunningLocked() ? g_version : std::wstring();
}

void Shutdown() {
    std::lock_guard lock(g_mutex);
    StopLocked();
}

} // namespace glasswave::download::aria2
