#include "pch.h"
#include "applemusic/service.h"
#include "net/http.h"
#include "core/json.h"

#include <fstream>
#include <utility>

namespace glasswave::applemusic {
namespace {
namespace fs = std::filesystem;
using Value = winrt::Windows::Data::Json::JsonValue;

class Handle {
public:
    explicit Handle(HANDLE value = nullptr) : m_value(value) {}
    ~Handle() { if (*this) CloseHandle(m_value); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    HANDLE get() const { return m_value; }
    HANDLE release() { return std::exchange(m_value, nullptr); }
    explicit operator bool() const { return m_value && m_value != INVALID_HANDLE_VALUE; }
private:
    HANDLE m_value;
};

[[noreturn]] void Fail(const char* message) { throw std::runtime_error(message); }

struct EnvironmentLess {
    bool operator()(const std::wstring& a, const std::wstring& b) const {
        return _wcsicmp(a.c_str(), b.c_str()) < 0;
    }
};

std::string ForwardSlashesUtf8(const fs::path& path) {
    std::wstring wide = path.lexically_normal().wstring();
    for (wchar_t& ch : wide) {
        if (ch == L'\\') ch = L'/';
    }
    return std::string(pfc::stringcvt::string_utf8_from_wide(wide.c_str()).get_ptr());
}

void EnsureRuntimeConfigYaml(const fs::path& runtimeBin, const fs::path& runtimeData) {
    const fs::path configPath = runtimeData / L"config.yaml";
    std::error_code ec;
    const bool sameDir = runtimeBin.lexically_normal() == runtimeData.lexically_normal();
    if (sameDir && fs::is_regular_file(configPath, ec) && !ec) return;
    const std::string content =
        "drm-binary-path: \"" + ForwardSlashesUtf8(runtimeBin) + "\"\n"
        "drm-base-dir: \"" + ForwardSlashesUtf8(runtimeData / L"files") + "\"\n"
        "alac-max: 192000\n"
        "get-m3u8-mode: hires\n"
        "get-m3u8-from-device: true\n"
        "aac-type: aac-lc\n"
        "exit-on-error: true\n";
    std::ofstream file(configPath, std::ios::binary | std::ios::trunc);
    file.write(content.data(), static_cast<std::streamsize>(content.size()));
    if (!file) Fail("Cannot write the Apple Music runtime configuration.");
}

std::vector<wchar_t> Environment(const fs::path& runtimeBin, const fs::path& runtimeData, const std::string& token) {
    std::map<std::wstring, std::wstring, EnvironmentLess> vars;
    wchar_t* inherited = GetEnvironmentStringsW();
    if (!inherited) Fail("Cannot prepare the Apple Music service environment.");
    for (const wchar_t* at = inherited; *at; at += wcslen(at) + 1) {
        const std::wstring entry(at);
        const auto split = entry.find(L'=', 1); // Preserve per-drive working directories too.
        if (split != std::wstring::npos) vars[entry.substr(0, split)] = entry.substr(split + 1);
    }
    FreeEnvironmentStringsW(inherited);
    vars[L"GLASSWAVE_HEADLESS"] = L"1";
    vars[L"ADAM_API_TOKEN"] = pfc::stringcvt::string_wide_from_utf8(token.c_str()).get_ptr();
    vars[L"ADAM_ADAMPLAY_DIR"] = (runtimeData / L"widevine").wstring();
    vars[L"APPDATA"] = (runtimeData / L"config").wstring();
    vars[L"LOCALAPPDATA"] = (runtimeData / L"localappdata").wstring();
    vars[L"TMP"] = vars[L"TEMP"] = (runtimeData / L"tmp").wstring();
    vars[L"PATH"] = runtimeBin.wstring() + L";" + vars[L"PATH"];
    std::vector<wchar_t> block;
    for (const auto& [key, value] : vars) {
        const std::wstring entry = key + L"=" + value;
        block.insert(block.end(), entry.begin(), entry.end());
        block.push_back(L'\0');
    }
    block.push_back(L'\0');
    return block;
}

unsigned Port(const std::wstring& url) {
    // client.cpp validates the complete loopback URL before calling this manager.
    const auto colon = url.find(L':', 7);
    return colon == std::wstring::npos ? 80u : static_cast<unsigned>(std::stoul(url.substr(colon + 1)));
}

net::HttpResponse Probe(const std::wstring& url, const std::string& token, abort_callback& abort) {
    net::HttpOptions options;
    options.noProxy = true;
    options.timeoutMs = 500;
    options.maxBodyBytes = 64u * 1024u;
    options.headers = {{L"X-API-Token", pfc::stringcvt::string_wide_from_utf8(token.c_str()).get_ptr()}};
    return net::SendHttp(url + L"/api/v1/status", options, abort);
}

void WriteRecord(const fs::path& path, HANDLE process, const fs::path& engine, unsigned port) {
    FILETIME created{}, exited{}, kernel{}, user{};
    SYSTEMTIME time{};
    if (!GetProcessTimes(process, &created, &exited, &kernel, &user) ||
        !FileTimeToSystemTime(&created, &time)) return;
    wchar_t utc[40]{};
    swprintf_s(utc, L"%04u-%02u-%02uT%02u:%02u:%02u.%03uZ", time.wYear, time.wMonth, time.wDay,
               time.wHour, time.wMinute, time.wSecond, time.wMilliseconds);
    core::json::Object record;
    record.Insert(L"enginePid", Value::CreateNumberValue(GetProcessId(process)));
    record.Insert(L"enginePath", Value::CreateStringValue(engine.wstring()));
    record.Insert(L"startedUtc", Value::CreateStringValue(utc));
    record.Insert(L"port", Value::CreateNumberValue(port));
    record.Insert(L"ownerPid", Value::CreateNumberValue(GetCurrentProcessId()));
    const auto json = core::json::Stringify(record);
    const std::string bytes = pfc::stringcvt::string_utf8_from_wide(json.c_str()).get_ptr();
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

class ServiceLifecycle : public initquit {
public:
    void on_init() override {}
    void on_quit() override { ServiceManager::Get().Stop(); }
};
FB2K_SERVICE_FACTORY(ServiceLifecycle);
} // namespace

ServiceManager& ServiceManager::Get() {
    static ServiceManager instance;
    return instance;
}

ServiceManager::~ServiceManager() { Stop(); }

void ServiceManager::EnsureStarted(const std::wstring& url, const std::string& token,
                                   const fs::path& runtimeBin, const fs::path& runtimeData,
                                   abort_callback& abort) {
    if (runtimeBin.empty() || runtimeData.empty()) return; // Explicit fixtures and external endpoints are never launched.
    abort.check();
    m_shutdown.check();
    std::unique_lock lock(m_mutex, std::defer_lock);
    const HANDLE cancel[] = {abort.get_abort_event(), m_shutdown.get_abort_event()};
    while (!lock.try_lock()) {
        if (WaitForMultipleObjects(2, cancel, FALSE, 25) != WAIT_TIMEOUT) {
            abort.check();
            m_shutdown.check();
        }
    }
    abort.check();
    m_shutdown.check();
    if (m_process && m_url == url && m_token == token && m_runtimeBin == runtimeBin &&
        m_runtimeData == runtimeData &&
        WaitForSingleObject(static_cast<HANDLE>(m_process), 0) == WAIT_TIMEOUT) return;
    StopLocked();

    // This request does not replay any user operation. It only determines ownership.
    const auto existing = Probe(url, token, m_shutdown);
    abort.check();
    if (existing.status == 200 && existing.error.empty()) return;
    if (existing.status != 0) Fail("The Apple Music service address is occupied or authorization has changed.");
    if (!runtimeBin.is_absolute() || !runtimeData.is_absolute())
        Fail("The managed Apple Music runtime path must be absolute.");
    const auto engine = runtimeBin / L"engine.exe";
    for (const auto* name : {L"engine.exe", L"libdrm_client.dll", L"libhybris-core.dll", L"hybris-linker/q.so"}) {
        std::error_code ec;
        if (!fs::is_regular_file(runtimeBin / name, ec) || ec)
            Fail("The native Apple Music runtime is incomplete. Reinstall Glasswave or run Apple Music setup.");
    }
    {
        std::error_code ec;
        if (!fs::is_regular_file(runtimeData / L"rootfs/system/lib64/libandroidappmusic.so", ec) || ec)
            Fail("Apple Music Android libraries are missing. Place the Android .so files in "
                 "<profile>\\glasswave\\applemusic\\runtime\\rootfs\\system\\lib64\\.");
    }
    std::error_code ec;
    for (const auto* name : {L"logs", L"config", L"localappdata", L"tmp", L"widevine", L"files/mpl_db"}) {
        fs::create_directories(runtimeData / name, ec);
        if (ec) Fail("Cannot prepare the native Apple Music runtime folders.");
    }
    EnsureRuntimeConfigYaml(runtimeBin, runtimeData);

    Handle job(CreateJobObjectW(nullptr, nullptr));
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!job || !SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
        Fail("Cannot create Apple Music service lifetime protection.");
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    Handle log(CreateFileW((runtimeData / L"logs/component-service.log").c_str(), FILE_APPEND_DATA,
                          FILE_SHARE_READ | FILE_SHARE_WRITE, &security, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    Handle input(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                            &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!log || !input) Fail("Cannot open Apple Music service output.");
    SIZE_T attributeBytes = 0;
    InitializeProcThreadAttributeList(nullptr, 2, 0, &attributeBytes);
    std::vector<unsigned char> attributes(attributeBytes);
    auto* list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
    if (!InitializeProcThreadAttributeList(list, 2, 0, &attributeBytes))
        Fail("Cannot initialize Apple Music service handles.");
    struct AttributeCleanup {
        LPPROC_THREAD_ATTRIBUTE_LIST list;
        ~AttributeCleanup() { DeleteProcThreadAttributeList(list); }
    } cleanup{list};
    HANDLE inherited[] = {input.get(), log.get()};
    if (!UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited,
                                   sizeof(inherited), nullptr, nullptr))
        Fail("Cannot restrict Apple Music service handle inheritance.");
    // Assign atomically during CreateProcess, closing the suspended-orphan window
    // if foobar2000 is terminated between process creation and user-mode assignment.
    HANDLE jobs[] = {job.get()};
    if (!UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_JOB_LIST, jobs,
                                   sizeof(jobs), nullptr, nullptr))
        Fail("Cannot bind Apple Music service creation to this player instance.");
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = input.get();
    startup.StartupInfo.hStdOutput = startup.StartupInfo.hStdError = log.get();
    startup.lpAttributeList = list;
    const unsigned port = Port(url);
    std::wstring command = L"\"" + engine.wstring() + L"\" --api " + std::to_wstring(port);
    auto environment = Environment(runtimeBin, runtimeData, token);
    PROCESS_INFORMATION child{};
    const BOOL created = CreateProcessW(engine.c_str(), command.data(), nullptr, nullptr, TRUE,
        CREATE_SUSPENDED | CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT,
        environment.data(), runtimeData.c_str(), &startup.StartupInfo, &child);
    SecureZeroMemory(environment.data(), environment.size() * sizeof(wchar_t));
    if (!created) Fail("Cannot start the native Apple Music service.");
    Handle process(child.hProcess), thread(child.hThread);
    // Job membership is already established before any child code can execute.
    if (ResumeThread(thread.get()) == static_cast<DWORD>(-1)) {
        TerminateProcess(process.get(), 1);
        WaitForSingleObject(process.get(), 3000);
        Fail("Cannot bind the Apple Music service to this player instance.");
    }
    m_process = process.release();
    m_job = job.release();
    m_url = url;
    m_token = token;
    m_runtimeBin = runtimeBin;
    m_runtimeData = runtimeData;
    m_record = runtimeData / (L"engine-" + std::to_wstring(port) + L".pid.json");
    try {
        WriteRecord(m_record, static_cast<HANDLE>(m_process), engine, port);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
        while (std::chrono::steady_clock::now() < deadline) {
            abort.check();
            m_shutdown.check();
            if (WaitForSingleObject(static_cast<HANDLE>(m_process), 0) != WAIT_TIMEOUT)
                Fail("The native Apple Music service exited during startup. Check its private log.");
            const auto response = Probe(url, token, m_shutdown);
            if (response.status == 200 && response.error.empty()) return;
            if (response.status != 0) Fail("The Apple Music service did not accept its configured authorization.");
            WaitForMultipleObjects(2, cancel, FALSE, 100);
        }
        Fail("The native Apple Music service did not become ready in time.");
    } catch (...) {
        StopLocked();
        throw;
    }
}

void ServiceManager::Stop() {
    m_shutdown.abort(); // Do this before locking, including when another thread is starting.
    std::lock_guard lock(m_mutex);
    StopLocked();
}

void ServiceManager::StopLocked() {
    if (m_job) {
        CloseHandle(static_cast<HANDLE>(m_job));
        m_job = nullptr;
    }
    if (m_process) {
        WaitForSingleObject(static_cast<HANDLE>(m_process), 3000);
        CloseHandle(static_cast<HANDLE>(m_process));
        m_process = nullptr;
        if (!m_record.empty()) DeleteFileW(m_record.c_str());
    }
    m_record.clear();
    m_runtimeBin.clear();
    m_runtimeData.clear();
    m_url.clear();
    if (!m_token.empty()) SecureZeroMemory(m_token.data(), m_token.size());
    m_token.clear();
}

} // namespace glasswave::applemusic
