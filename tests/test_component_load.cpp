#include <windows.h>

#include <filesystem>
#include <iostream>

namespace {

int Fail(const char* stage, DWORD error) {
    std::cerr << stage << " failed: Win32 error " << error << '\n';
    return 1;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc != 3) {
        std::cerr << "Usage: test_component_load <component.dll> <runtime-root>\n";
        return 2;
    }

    const std::filesystem::path component = argv[1];
    const std::filesystem::path runtimeRoot = argv[2];
    if (!component.is_absolute() || !runtimeRoot.is_absolute()) {
        std::cerr << "Component and runtime-root paths must be absolute.\n";
        return 2;
    }

    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    if (!SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS))
        return Fail("SetDefaultDllDirectories", GetLastError());

    DLL_DIRECTORY_COOKIE cookie = AddDllDirectory(runtimeRoot.c_str());
    if (!cookie) return Fail("AddDllDirectory", GetLastError());

    HMODULE module =
        LoadLibraryExW(component.c_str(), nullptr,
                       LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!module) {
        const DWORD error = GetLastError();
        RemoveDllDirectory(cookie);
        return Fail("LoadLibraryEx", error);
    }

    const BOOL unloaded = FreeLibrary(module);
    const DWORD unloadError = GetLastError();
    const BOOL cookieRemoved = RemoveDllDirectory(cookie);
    const DWORD cookieError = GetLastError();

    if (!unloaded) return Fail("FreeLibrary", unloadError);
    if (!cookieRemoved) return Fail("RemoveDllDirectory", cookieError);

    std::cout << "component load/unload passed\n";
    return 0;
}
