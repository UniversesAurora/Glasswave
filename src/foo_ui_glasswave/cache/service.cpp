#include "pch.h"
#include "cache/config.h"
#include "cache/store.h"
#include "core/paths.h"
#include "core/text.h"

namespace glasswave::cache {
namespace {
cfg_var_modern::cfg_int audioLimit(
    {0xb83be422, 0xe106, 0x48b8, {0xa2, 0xcd, 0x9a, 0x93, 0x77, 0xfa, 0x62, 0x11}}, kAudioDefaultMiB);
cfg_var_modern::cfg_int imageLimit(
    {0xb83be423, 0xe106, 0x48b8, {0xa2, 0xcd, 0x9a, 0x93, 0x77, 0xfa, 0x62, 0x11}}, kImagesDefaultMiB);
cfg_var_modern::cfg_int listLimit(
    {0xb83be424, 0xe106, 0x48b8, {0xa2, 0xcd, 0x9a, 0x93, 0x77, 0xfa, 0x62, 0x11}}, kListsDefaultMiB);
cfg_var_modern::cfg_string cacheFolder(
    {0xb83be425, 0xe106, 0x48b8, {0xa2, 0xcd, 0x9a, 0x93, 0x77, 0xfa, 0x62, 0x11}}, "");
std::mutex scopeMutex;
std::map<std::string, uint64_t> revisions;
std::set<std::string> revoked;

std::wstring TrimPath(std::wstring_view value) {
    while (!value.empty() && (value.front() == L' ' || value.front() == L'\t' || value.front() == L'\r' ||
                              value.front() == L'\n'))
        value.remove_prefix(1);
    while (!value.empty() && (value.back() == L' ' || value.back() == L'\t' || value.back() == L'\r' ||
                              value.back() == L'\n'))
        value.remove_suffix(1);
    return std::wstring(value);
}
} // namespace

std::wstring CustomCacheFolder() {
    return TrimPath(core::Wide(cacheFolder.get().c_str()));
}

std::wstring CacheFolder() {
    const std::wstring custom = CustomCacheFolder();
    if (!custom.empty()) return std::filesystem::path(custom).lexically_normal().wstring();
    const auto root = core::ProfileRoot();
    return root.empty() ? std::wstring() : (root / L"cache").wstring();
}

void SetCacheFolder(std::wstring_view path) {
    const std::wstring trimmed = TrimPath(path);
    cacheFolder.set(trimmed.empty() ? "" : core::Utf8(std::filesystem::path(trimmed).lexically_normal().wstring()).c_str());
}

int AudioLimitMiB() {
    return static_cast<int>(std::clamp<int64_t>(audioLimit.get(), kAudioMinMiB, kAudioMaxMiB));
}
int ImageLimitMiB() {
    return static_cast<int>(std::clamp<int64_t>(imageLimit.get(), kImagesMinMiB, kImagesMaxMiB));
}
int ListLimitMiB() {
    return static_cast<int>(std::clamp<int64_t>(listLimit.get(), kListsMinMiB, kListsMaxMiB));
}
void SetAudioLimitMiB(int value) {
    audioLimit.set(std::clamp(value, kAudioMinMiB, kAudioMaxMiB));
    ApplyLimits();
}
void SetImageLimitMiB(int value) {
    imageLimit.set(std::clamp(value, kImagesMinMiB, kImagesMaxMiB));
    ApplyLimits();
}
void SetListLimitMiB(int value) {
    listLimit.set(std::clamp(value, kListsMinMiB, kListsMaxMiB));
    ApplyLimits();
}

Store& GetStore() {
    static Store store([] {
        const std::wstring configured = CacheFolder();
        if (configured.empty()) throw std::runtime_error("The cache profile is unavailable.");
        const std::filesystem::path dir(configured);
        core::EnsureDir(dir);
        return dir / L"v1";
    }());
    static const bool configured = [&] {
        store.SetLimit(Kind::Audio, static_cast<uint64_t>(AudioLimitMiB()) * 1024 * 1024);
        store.SetLimit(Kind::Image, static_cast<uint64_t>(ImageLimitMiB()) * 1024 * 1024);
        store.SetLimit(Kind::Data, static_cast<uint64_t>(ListLimitMiB()) * 1024 * 1024);
        return true;
    }();
    (void)configured;
    return store;
}
void ApplyLimits() {
    try {
        auto& store = GetStore();
        store.SetLimit(Kind::Audio, static_cast<uint64_t>(AudioLimitMiB()) * 1024 * 1024);
        store.SetLimit(Kind::Image, static_cast<uint64_t>(ImageLimitMiB()) * 1024 * 1024);
        store.SetLimit(Kind::Data, static_cast<uint64_t>(ListLimitMiB()) * 1024 * 1024);
    } catch (...) { /* Cache availability must not prevent preferences or playback. */
    }
}
std::string AccountScope(const std::string& provider) {
    std::lock_guard lock(scopeMutex);
    if (revoked.contains(provider)) throw std::runtime_error("The cache account scope is unavailable.");
    return GetStore().Scope(provider);
}
bool SelectAccountScope(const std::string& provider, const std::string& accountId) {
    if (accountId.empty()) {
        RotateAccountScope(provider);
        return true;
    }
    std::lock_guard lock(scopeMutex);
    const bool wasRevoked = revoked.contains(provider);
    bool changed = false;
    try {
        changed = GetStore().SelectAccountScope(provider, accountId);
        revoked.erase(provider);
    } catch (...) {
        revoked.insert(provider);
        ++revisions[provider];
        return true;
    }
    if (changed || wasRevoked) ++revisions[provider];
    return changed || wasRevoked;
}
void RotateAccountScope(const std::string& provider) {
    // Revocation must complete before the provider publishes its new account state.
    // Scope readers share this lock so a failed disk revocation cannot grant the old scope.
    std::lock_guard lock(scopeMutex);
    try {
        GetStore().SignOutScope(provider);
        revoked.erase(provider);
    } catch (...) {
        // Signing out must still succeed when the cache disk/index is unavailable.
        // Fail closed for all subsequent account-scoped cache access in this process.
        revoked.insert(provider);
    }
    ++revisions[provider];
}
uint64_t AccountRevision(const std::string& provider) {
    std::lock_guard lock(scopeMutex);
    const auto found = revisions.find(provider);
    return found == revisions.end() ? 0 : found->second;
}
} // namespace glasswave::cache
