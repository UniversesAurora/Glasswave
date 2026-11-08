#include "pch.h"
#include "netease/cookie.h"
#include "netease/log.h"
#include "netease/profile.h"

#include <dpapi.h>
#include <filesystem>
#include <fstream>
#include <mutex>

namespace glasswave::netease {

namespace {

std::mutex g_mutex;
std::wstring g_cookie;
bool g_loaded = false;

const wchar_t kEntropy[] = L"Glasswave.Netease.Session.v1";

std::filesystem::path SessionPath() {
    const auto dir = ProfileDir();
    return dir.empty() ? std::filesystem::path() : dir / L"session.dat";
}

DATA_BLOB Entropy() {
    return DATA_BLOB{static_cast<DWORD>(sizeof(kEntropy)),
                     reinterpret_cast<BYTE*>(const_cast<wchar_t*>(kEntropy))};
}

bool WriteFileBytes(const std::filesystem::path& path, const std::vector<BYTE>& bytes) {
    if (!EnsureDir(path.parent_path())) return false;
    auto tmp = path;
    tmp += L".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!out) return false;
    }
    if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        std::error_code ec;
        std::filesystem::remove(tmp, ec);
        return false;
    }
    return true;
}

bool ReadFileBytes(const std::filesystem::path& path, std::vector<BYTE>& bytes) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return !bytes.empty();
}

}  // namespace

bool LoadSessionCookie() {
    const auto path = SessionPath();
    if (path.empty()) return false;

    std::vector<BYTE> encrypted;
    if (!ReadFileBytes(path, encrypted)) return false;

    DATA_BLOB in{static_cast<DWORD>(encrypted.size()), encrypted.data()};
    DATA_BLOB entropy = Entropy();
    DATA_BLOB out{};
    if (!CryptUnprotectData(&in, nullptr, &entropy, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) {
        Log(L"session: cannot decrypt the saved cookie");
        return false;
    }
    const size_t chars = out.cbData / sizeof(wchar_t);
    std::wstring cookie(reinterpret_cast<const wchar_t*>(out.pbData), chars);
    LocalFree(out.pbData);

    std::lock_guard lock(g_mutex);
    g_cookie = std::move(cookie);
    g_loaded = true;
    return true;
}

std::wstring SessionCookie() {
    std::lock_guard lock(g_mutex);
    return g_cookie;
}

void SaveSessionCookie(std::wstring_view cookie) {
    {
        std::lock_guard lock(g_mutex);
        g_cookie.assign(cookie);
        g_loaded = true;
    }
    const auto path = SessionPath();
    if (path.empty()) return;

    DATA_BLOB in{static_cast<DWORD>(cookie.size() * sizeof(wchar_t)),
                 reinterpret_cast<BYTE*>(const_cast<wchar_t*>(cookie.data()))};
    DATA_BLOB entropy = Entropy();
    DATA_BLOB out{};
    if (!CryptProtectData(&in, L"Glasswave NetEase session", &entropy, nullptr, nullptr,
                          CRYPTPROTECT_UI_FORBIDDEN, &out)) {
        Log(L"session: cannot encrypt the cookie");
        return;
    }
    const std::vector<BYTE> bytes(out.pbData, out.pbData + out.cbData);
    LocalFree(out.pbData);
    if (!WriteFileBytes(path, bytes)) Log(L"session: cannot write the cookie file");
}

void ClearSessionCookie() {
    {
        std::lock_guard lock(g_mutex);
        g_cookie.clear();
        g_loaded = true;
    }
    const auto path = SessionPath();
    if (path.empty()) return;
    std::error_code ec;
    std::filesystem::remove(path, ec);
    auto tmp = path;
    tmp += L".tmp";
    std::filesystem::remove(tmp, ec);
}

std::wstring SessionCookiePath() {
    const auto path = SessionPath();
    return path.empty() ? std::wstring() : path.wstring();
}

}  // namespace glasswave::netease
