#include "pch.h"
#include "core/file.h"

#include "core/text.h"

#include <stdexcept>

namespace glasswave::core {

namespace {

struct Handle {
    HANDLE value = INVALID_HANDLE_VALUE;
    explicit Handle(HANDLE h) : value(h) {}
    ~Handle() { Close(); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    explicit operator bool() const { return value != INVALID_HANDLE_VALUE; }
    bool Close() {
        if (value == INVALID_HANDLE_VALUE) return true;
        const bool ok = CloseHandle(value) != FALSE;
        value = INVALID_HANDLE_VALUE;
        return ok;
    }
};

[[noreturn]] void Fail(const std::filesystem::path& path, const char* what, DWORD error) {
    throw std::runtime_error(std::string(what) + " " + Utf8(path.wstring()) + " (error " +
                             std::to_string(error) + ")");
}

} // namespace

void WriteFileAtomic(const std::filesystem::path& path, std::string_view bytes) {
    std::error_code ec;
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), ec);
        if (ec) throw std::runtime_error("Cannot create the folder of " + Utf8(path.wstring()));
    }
    auto part = path;
    part += L".part";

    DWORD error = ERROR_SUCCESS;
    {
        Handle file(CreateFileW(part.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                                nullptr));
        if (!file) Fail(part, "Cannot create", GetLastError());
        const char* data = bytes.data();
        size_t left = bytes.size();
        while (left > 0 && error == ERROR_SUCCESS) {
            const DWORD chunk = static_cast<DWORD>(std::min<size_t>(left, 1u << 30));
            DWORD written = 0;
            if (!WriteFile(file.value, data, chunk, &written, nullptr) || written == 0) {
                error = GetLastError();
                if (error == ERROR_SUCCESS) error = ERROR_WRITE_FAULT;
                break;
            }
            data += written;
            left -= written;
        }
        if (error == ERROR_SUCCESS && !FlushFileBuffers(file.value)) error = GetLastError();
        if (error == ERROR_SUCCESS && !file.Close()) error = GetLastError();
    }
    if (error == ERROR_SUCCESS &&
        !MoveFileExW(part.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        error = GetLastError();
    }
    if (error != ERROR_SUCCESS) {
        DeleteFileW(part.c_str());
        Fail(path, "Cannot write", error);
    }
}

std::optional<std::string> ReadFileLimited(const std::filesystem::path& path, size_t maxBytes) {
    Handle file(CreateFileW(path.c_str(), GENERIC_READ,
                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    if (!file) return std::nullopt;
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file.value, &size) || size.QuadPart < 0 ||
        static_cast<unsigned long long>(size.QuadPart) > maxBytes) {
        return std::nullopt;
    }
    std::string bytes;
    bytes.resize(static_cast<size_t>(size.QuadPart));
    size_t total = 0;
    while (total < bytes.size()) {
        const DWORD want = static_cast<DWORD>(std::min<size_t>(bytes.size() - total, 1u << 30));
        DWORD read = 0;
        if (!ReadFile(file.value, bytes.data() + total, want, &read, nullptr)) return std::nullopt;
        if (read == 0) break; // shorter than reported: keep what exists
        total += read;
    }
    bytes.resize(total);
    return bytes;
}

} // namespace glasswave::core
