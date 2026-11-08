#include "pch.h"
#include "netease/config.h"
#include "netease/http.h"
#include "netease/log.h"
#include "netease/profile.h"
#include "netease/service.h"

#include <bcrypt.h>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <winsock2.h>

#pragma comment(lib, "ws2_32.lib")

namespace glasswave::netease {

namespace {

constexpr wchar_t kServiceVersion[] = L"4.41.1";
constexpr wchar_t kServiceUrl[] =
    L"https://github.com/NeteaseCloudMusicApiEnhanced/api-enhanced/releases/download/"
    L"v4.41.1/ncm-api-win-x64.exe";
constexpr char kServiceSha256[] = "f196774ab85dc1039ec3ca6b5297cbc3e086b0bbbfc744498e6064ea94ceaa02";

std::wstring Wide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string Utf8(std::wstring_view s) {
    if (s.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring Win32Error(DWORD code) {
    wchar_t* buffer = nullptr;
    const DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                       FORMAT_MESSAGE_IGNORE_INSERTS,
                                   nullptr, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                                   reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
    std::wstring text = n && buffer ? std::wstring(buffer, n) : L"Win32 error " + std::to_wstring(code);
    if (buffer) LocalFree(buffer);
    while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n' || text.back() == L' ')) text.pop_back();
    return text;
}

std::string Sha256Hex(const std::string& data) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) return {};
    unsigned char digest[32]{};
    const NTSTATUS status = BCryptHash(alg, nullptr, 0, reinterpret_cast<PUCHAR>(const_cast<char*>(data.data())),
                                       static_cast<ULONG>(data.size()), digest, sizeof(digest));
    BCryptCloseAlgorithmProvider(alg, 0);
    if (status < 0) return {};
    static constexpr char hex[] = "0123456789abcdef";
    std::string out;
    out.reserve(64);
    for (unsigned char byte : digest) {
        out.push_back(hex[byte >> 4]);
        out.push_back(hex[byte & 0x0f]);
    }
    return out;
}

int PickLoopbackPort() {
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return 0;
    SOCKET socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket == INVALID_SOCKET) {
        WSACleanup();
        return 0;
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    int port = 0;
    if (bind(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0) {
        int length = sizeof(address);
        if (getsockname(socket, reinterpret_cast<sockaddr*>(&address), &length) == 0)
            port = ntohs(address.sin_port);
    }
    closesocket(socket);
    WSACleanup();
    return port;
}

std::vector<wchar_t> EnvironmentBlock(int port) {
    std::vector<wchar_t> block;
    LPWCH environment = GetEnvironmentStringsW();
    if (environment) {
        for (LPWCH p = environment; *p; p += wcslen(p) + 1) {
            if (_wcsnicmp(p, L"HOST=", 5) == 0 || _wcsnicmp(p, L"PORT=", 5) == 0) continue;
            const size_t length = wcslen(p) + 1;
            block.insert(block.end(), p, p + length);
        }
        FreeEnvironmentStringsW(environment);
    }
    const std::wstring host = L"HOST=127.0.0.1";
    const std::wstring portValue = L"PORT=" + std::to_wstring(port);
    block.insert(block.end(), host.begin(), host.end());
    block.push_back(L'\0');
    block.insert(block.end(), portValue.begin(), portValue.end());
    block.push_back(L'\0');
    block.push_back(L'\0');
    return block;
}

std::wstring NormalizeBaseUrl(std::wstring url) {
    while (!url.empty() && (url.back() == L'/' || url.back() == L'\\')) url.pop_back();
    if (url.empty()) return {};
    if (_wcsnicmp(url.c_str(), L"http://", 7) != 0 && _wcsnicmp(url.c_str(), L"https://", 8) != 0)
        url = L"http://" + url;
    return url;
}

}  // namespace

ServiceManager& ServiceManager::Get() {
    static ServiceManager instance;
    return instance;
}

ServiceManager::ServiceManager() = default;
ServiceManager::~ServiceManager() { Stop(); }

void ServiceManager::Initialize() {
    std::lock_guard lock(m_mutex);
    m_externalUrl = NormalizeBaseUrl(GetServiceUrlOverride());
    m_externalConfigured = !m_externalUrl.empty();
    if (!m_externalUrl.empty()) {
        m_state = ServiceState::External;
        m_baseUrl = Utf8(m_externalUrl);
    }
}

ServiceState ServiceManager::State() const {
    std::lock_guard lock(m_mutex);
    return m_state;
}

bool ServiceManager::External() const {
    std::lock_guard lock(m_mutex);
    return !m_externalUrl.empty();
}

bool ServiceManager::IsInstalled() const {
    const auto path = ExePath();
    std::error_code ec;
    return !path.empty() && std::filesystem::is_regular_file(path, ec) &&
           std::filesystem::file_size(path, ec) > 1024u * 1024u;
}

bool ServiceManager::Available() const {
    // Must not take m_mutex: a worker can hold it while probing the helper for up to
    // 30 seconds, and the login window calls this on the main thread.
    return m_externalConfigured.load() || IsInstalled();
}

std::wstring ServiceManager::ExePath() const {
    const auto dir = ServiceDir();
    return dir.empty() ? std::wstring() : (dir / L"ncm-api.exe").wstring();
}

std::wstring ServiceManager::LogPath() const {
    const auto dir = ProfileDir();
    return dir.empty() ? std::wstring() : (dir / L"service.log").wstring();
}

std::wstring ServiceManager::LastError() const {
    std::lock_guard lock(m_mutex);
    return m_lastError;
}

std::string ServiceManager::BaseUrl() const {
    std::lock_guard lock(m_mutex);
    return m_baseUrl;
}

void ServiceManager::SetErrorLocked(std::wstring error) {
    m_lastError = std::move(error);
    m_state = ServiceState::Failed;
}

bool ServiceManager::Install(abort_callback& abort, const std::function<void(long long, long long)>& progress,
                             std::wstring* error) {
    {
        std::lock_guard lock(m_mutex);
        if (!m_externalUrl.empty()) {
            if (error) *error = L"An external NetEase service is configured.";
            return false;
        }
    }
    const auto dir = ServiceDir();
    if (dir.empty() || !EnsureDir(dir)) {
        if (error) *error = L"Cannot create the NetEase service folder.";
        return false;
    }
    Stop();  // a running helper would keep the old executable locked
    Log(L"service: downloading " + std::wstring(kServiceUrl));

    HttpOptions options;
    options.noProxy = false;
    options.timeoutMs = 120000;
    options.maxBodyBytes = 100u * 1024u * 1024u;
    options.headers = {{L"Accept", L"application/octet-stream"}};
    options.progress = progress;
    const HttpResponse response = SendHttp(kServiceUrl, options, abort);
    if (!response.ok()) {
        if (error) *error = response.error.empty() ? L"Download failed (HTTP " + std::to_wstring(response.status) + L")."
                                                   : response.error;
        Log(L"service: download failed: " + (error ? *error : L"unknown"));
        return false;
    }
    if (response.body.size() < 1024u * 1024u) {
        if (error) *error = L"Downloaded service is unexpectedly small.";
        return false;
    }
    const std::string hash = Sha256Hex(response.body);
    if (hash != kServiceSha256) {
        if (error) *error = L"Downloaded service failed the SHA-256 check.";
        Log(L"service: SHA-256 mismatch");
        return false;
    }

    const auto exe = std::filesystem::path(ExePath());
    auto tmp = exe;
    tmp += L".part";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            if (error) *error = L"Cannot write the NetEase service.";
            return false;
        }
        out.write(response.body.data(), static_cast<std::streamsize>(response.body.size()));
        if (!out) {
            if (error) *error = L"Cannot write the NetEase service.";
            return false;
        }
    }
    if (!MoveFileExW(tmp.c_str(), exe.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        std::error_code ec;
        std::filesystem::remove(tmp, ec);
        if (error) *error = L"Cannot install the NetEase service: " + Win32Error(GetLastError());
        return false;
    }
    Log(L"service: installed " + std::wstring(kServiceVersion) + L" at " + exe.wstring());
    return true;
}

bool ServiceManager::EnsureStarted(abort_callback& abort, std::wstring* error) {
    std::lock_guard lock(m_mutex);
    if (!m_externalUrl.empty()) {
        m_state = ServiceState::External;
        m_baseUrl = Utf8(m_externalUrl);
        HttpOptions probe;
        probe.noProxy = true;
        probe.timeoutMs = 3000;
        probe.maxBodyBytes = 64u * 1024u;
        const HttpResponse response = SendHttp(m_externalUrl + L"/", probe, abort);
        if (response.status == 0) {
            SetErrorLocked(L"External NetEase service is not reachable: " + response.error);
            if (error) *error = m_lastError;
            return false;
        }
        m_lastError.clear();
        return true;
    }
    if (m_state == ServiceState::Ready && !m_baseUrl.empty()) return true;
    if (!IsInstalled()) {
        SetErrorLocked(L"NetEase service is not installed.");
        if (error) *error = m_lastError;
        return false;
    }
    return StartLocked(abort, error);
}

bool ServiceManager::StartLocked(abort_callback& abort, std::wstring* error) {
    StopLocked();
    m_state = ServiceState::Starting;
    m_lastError.clear();

    const int port = PickLoopbackPort();
    if (!port) {
        SetErrorLocked(L"Cannot find a free loopback port.");
        if (error) *error = m_lastError;
        return false;
    }
    const auto exe = ExePath();
    const auto workdir = ServiceDir();
    const auto logPath = LogPath();
    if (exe.empty() || workdir.empty()) {
        SetErrorLocked(L"Cannot resolve the NetEase service path.");
        if (error) *error = m_lastError;
        return false;
    }

    SECURITY_ATTRIBUTES attributes{sizeof(attributes), nullptr, TRUE};
    HANDLE log = CreateFileW(logPath.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, &attributes,
                             OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    HANDLE nullInput = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &attributes, OPEN_EXISTING,
                                   FILE_ATTRIBUTE_NORMAL, nullptr);
    if (log == INVALID_HANDLE_VALUE) log = nullptr;
    if (nullInput == INVALID_HANDLE_VALUE) nullInput = nullptr;

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    if (log || nullInput) {
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdInput = nullInput ? nullInput : GetStdHandle(STD_INPUT_HANDLE);
        startup.hStdOutput = log ? log : GetStdHandle(STD_OUTPUT_HANDLE);
        startup.hStdError = log ? log : GetStdHandle(STD_ERROR_HANDLE);
    }
    PROCESS_INFORMATION process{};
    std::wstring command = L"\"" + exe + L"\"";
    std::vector<wchar_t> environment = EnvironmentBlock(port);
    const BOOL created = CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, TRUE,
                                        CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED,
                                        environment.data(), workdir.c_str(), &startup, &process);
    if (nullInput) CloseHandle(nullInput);
    if (log) CloseHandle(log);
    if (!created) {
        SetErrorLocked(L"Cannot start the NetEase service: " + Win32Error(GetLastError()));
        if (error) *error = m_lastError;
        return false;
    }

    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
        info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &info, sizeof(info));
        if (!AssignProcessToJobObject(job, process.hProcess)) {
            Log(L"service: cannot assign the helper to a job object");
            CloseHandle(job);
            job = nullptr;
        }
    }
    if (ResumeThread(process.hThread) == static_cast<DWORD>(-1)) {
        SetErrorLocked(L"Cannot resume the NetEase service: " + Win32Error(GetLastError()));
        TerminateProcess(process.hProcess, 1);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        if (job) CloseHandle(job);
        if (error) *error = m_lastError;
        return false;
    }
    CloseHandle(process.hThread);

    m_process = process.hProcess;
    m_job = job;
    m_port = port;
    m_baseUrl = "http://127.0.0.1:" + std::to_string(port);
    Log(L"service: started on port " + std::to_wstring(port));

    for (int attempt = 0; attempt < 120; ++attempt) {
        abort.check();
        if (WaitForSingleObject(process.hProcess, 0) == WAIT_OBJECT_0) {
            DWORD code = 0;
            GetExitCodeProcess(process.hProcess, &code);
            Log(L"service: helper exited during start-up, code " + std::to_wstring(code));
            StopLocked();
            SetErrorLocked(L"The NetEase service exited during start-up (code " + std::to_wstring(code) + L").");
            if (error) *error = m_lastError;
            return false;
        }
        HttpOptions probe;
        probe.noProxy = true;
        probe.timeoutMs = 1000;
        probe.maxBodyBytes = 64u * 1024u;
        const HttpResponse response = SendHttp(Wide(m_baseUrl) + L"/", probe, abort);
        if (response.status != 0) {
            m_state = ServiceState::Ready;
            m_lastError.clear();
            Log(L"service: ready");
            return true;
        }
        if (attempt == 0 || attempt == 20 || attempt == 60 || attempt == 119) {
            Log(L"service: probe " + std::to_wstring(attempt) + L" failed: " +
                (response.error.empty() ? L"no HTTP response" : response.error));
        }
        Sleep(250);
    }

    StopLocked();
    SetErrorLocked(L"The NetEase service did not become ready in time.");
    if (error) *error = m_lastError;
    return false;
}

void ServiceManager::Stop() {
    std::lock_guard lock(m_mutex);
    StopLocked();
}

void ServiceManager::StopLocked() {
    if (m_job) {
        CloseHandle(static_cast<HANDLE>(m_job));  // kill-on-close terminates the helper
        m_job = nullptr;
    }
    if (m_process) {
        HANDLE process = static_cast<HANDLE>(m_process);
        if (WaitForSingleObject(process, 3000) == WAIT_TIMEOUT) TerminateProcess(process, 0);
        CloseHandle(process);
        m_process = nullptr;
    }
    m_port = 0;
    m_baseUrl.clear();
    if (m_state == ServiceState::Ready || m_state == ServiceState::Starting) m_state = ServiceState::Stopped;
}

}  // namespace glasswave::netease
