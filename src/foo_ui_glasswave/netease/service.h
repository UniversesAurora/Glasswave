#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>

namespace glasswave::netease {

enum class ServiceState { Stopped, Starting, Ready, Failed, External };

// Owns the precompiled NeteaseCloudMusicApi Enhanced helper process. The helper is
// downloaded from the pinned release into <profile>\glasswave\netease\service and talks
// only to 127.0.0.1. An external `api_base` setting bypasses the managed process.
class ServiceManager {
public:
    static ServiceManager& Get();

    void Initialize();
    ServiceState State() const;
    bool Available() const;  // external URL configured, or the helper file exists
    bool IsInstalled() const;
    std::wstring ExePath() const;
    std::wstring LogPath() const;
    std::wstring LastError() const;
    std::string BaseUrl() const;

    // Blocking; call from a worker thread. Downloads and verifies the pinned helper.
    bool Install(abort_callback& abort, const std::function<void(long long, long long)>& progress,
                 std::wstring* error);

    // Blocking; starts the helper if needed and waits until it answers.
    bool EnsureStarted(abort_callback& abort, std::wstring* error);

    void Stop();
    // Makes a start-up that is waiting for the helper give up (foobar2000 is closing).
    void Cancel() { m_cancel = true; }

private:
    ServiceManager();
    ~ServiceManager();
    ServiceManager(const ServiceManager&) = delete;
    ServiceManager& operator=(const ServiceManager&) = delete;

    bool StartLocked(abort_callback& abort, std::wstring* error);
    void StopLocked();
    void SetErrorLocked(std::wstring error);
    bool External() const;

    mutable std::mutex m_mutex;
    ServiceState m_state = ServiceState::Stopped;
    std::wstring m_externalUrl;
    std::atomic_bool m_externalConfigured{false};
    std::atomic_bool m_cancel{false};
    std::wstring m_lastError;
    std::string m_baseUrl;
    void* m_process = nullptr;  // HANDLE
    void* m_job = nullptr;      // HANDLE
    int m_port = 0;
};

}  // namespace glasswave::netease
