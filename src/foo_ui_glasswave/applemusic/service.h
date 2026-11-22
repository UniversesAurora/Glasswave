#pragma once

#include <filesystem>
#include <mutex>
#include <string>

namespace glasswave::applemusic {

// Only a bridge with an explicit managed runtime opts in to process ownership.
// Calls may block and must run on a worker. An already-running bridge is borrowed.
class ServiceManager {
public:
    static ServiceManager& Get();
    void EnsureStarted(const std::wstring& url, const std::string& token,
                       const std::filesystem::path& runtimeBin, const std::filesystem::path& runtimeData,
                       abort_callback& abort);
    // Terminal for this component lifetime. Also cancels a start currently in flight.
    void Stop();

private:
    ServiceManager() = default;
    ~ServiceManager();
    void StopLocked();

    std::mutex m_mutex;
    abort_callback_impl m_shutdown;
    void* m_process = nullptr;
    void* m_job = nullptr;
    std::wstring m_url;
    std::string m_token;
    std::filesystem::path m_runtimeBin;
    std::filesystem::path m_runtimeData;
    std::filesystem::path m_record;
};

} // namespace glasswave::applemusic
