#include "pch.h"
#include "stream/atmos_renderer.h"

#include <chrono>

namespace glasswave::stream::atmos {
namespace {

constexpr uint32_t kAbiVersion = 1;
constexpr uint32_t kFloat32Pcm = 1;
constexpr uint32_t kStatusOk = 0;
constexpr uint32_t kStatusPending = 1;
constexpr uint32_t kStatusEnd = 2;

struct NativeInfo {
    uint32_t structSize;
    uint32_t abiVersion;
    uint32_t sampleRate;
    uint32_t channels;
    uint32_t channelMask;
    uint32_t outputMode;
    uint32_t layout;
    uint32_t pcmFormat;
    uint32_t flags;
    uint32_t dynamicObjects;
};

using ReadCallback = int64_t(__cdecl*)(void*, uint8_t*, uint64_t);
using AbiVersionFn = uint32_t(__cdecl*)();
using CreateFn = uint32_t(__cdecl*)(uint32_t, uint32_t, uint32_t, uint32_t, void*, ReadCallback, void**);
using GetInfoFn = uint32_t(__cdecl*)(void*, NativeInfo*);
using RenderFn = uint32_t(__cdecl*)(void*, float*, uint32_t, uint32_t*, int32_t*);
using CancelFn = uint32_t(__cdecl*)(void*);
using DestroyFn = uint32_t(__cdecl*)(void*);

std::wstring AdjacentRendererPath() {
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&AdjacentRendererPath), &self))
        return {};
    wchar_t path[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(self, path, static_cast<DWORD>(std::size(path)));
    if (!length || length >= std::size(path)) return {};
    std::wstring result(path, length);
    const auto slash = result.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return {};
    result.resize(slash + 1);
    return result + L"glasswave_cavern.dll";
}

void WaitBriefly(abort_callback& abort) {
    abort.check();
    if (const HANDLE event = abort.get_abort_event())
        WaitForSingleObject(event, 5);
    else
        Sleep(5);
    abort.check();
}

class NativeApi {
  public:
    static NativeApi& Get() {
        // NativeAOT shared libraries do not support safe unloading after runtime initialization.
        // Keep one export table for the life of this component process.
        static auto* api = new NativeApi();
        return *api;
    }

    bool ready() const noexcept { return m_ready; }

    AbiVersionFn abi = nullptr;
    CreateFn create = nullptr;
    GetInfoFn getInfo = nullptr;
    RenderFn render = nullptr;
    CancelFn cancel = nullptr;
    DestroyFn destroy = nullptr;

  private:
    NativeApi() noexcept {
        const std::wstring path = AdjacentRendererPath();
        if (path.empty()) return;
        m_module = LoadLibraryExW(path.c_str(), nullptr,
                                  LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!m_module) return;

        abi = Bind<AbiVersionFn>("gw_atmos_abi_version");
        create = Bind<CreateFn>("gw_atmos_create");
        getInfo = Bind<GetInfoFn>("gw_atmos_get_info");
        render = Bind<RenderFn>("gw_atmos_render");
        cancel = Bind<CancelFn>("gw_atmos_cancel");
        destroy = Bind<DestroyFn>("gw_atmos_destroy");
        m_ready = abi && create && getInfo && render && cancel && destroy && abi() == kAbiVersion;
    }

    template <typename T> T Bind(const char* name) noexcept {
        const auto value = reinterpret_cast<T>(GetProcAddress(m_module, name));
        return value;
    }

    HMODULE m_module = nullptr;
    bool m_ready = false;
};

struct NativeTarget {
    uint32_t mode;
    uint32_t layout;
};

NativeTarget MapTarget(OutputTarget target) {
    switch (target) {
    case OutputTarget::StereoSpeakers:
        return {0, 0};
    case OutputTarget::Headphones:
        return {1, 0};
    case OutputTarget::Speakers5_1:
        return {0, 1};
    case OutputTarget::Speakers7_1:
        return {0, 2};
    }
    throw exception_io_data("Unsupported Apple Music Atmos output target.");
}

} // namespace

class Renderer::Impl {
  public:
    struct ReadBridge {
        file::ptr source;
        abort_callback_impl abort;
        mutable std::mutex mutex;
        std::exception_ptr error;

        static int64_t __cdecl Pull(void* userdata, uint8_t* destination, uint64_t capacity) noexcept {
            auto& self = *static_cast<ReadBridge*>(userdata);
            try {
                self.abort.check();
                uint64_t total = 0;
                while (total < capacity) {
                    self.abort.check();
                    const auto count = self.source->read(destination + total,
                                                         static_cast<t_size>(capacity - total), self.abort);
                    if (!count) break;
                    total += count;
                }
                return static_cast<int64_t>(total);
            } catch (...) {
                std::lock_guard lock(self.mutex);
                if (!self.error) self.error = std::current_exception();
                return -1;
            }
        }

        void StoreCallFailure() {
            try {
                abort.check();
            } catch (...) {
                std::lock_guard lock(mutex);
                if (!error) error = std::current_exception();
            }
        }

        [[noreturn]] void ThrowStored() const {
            std::exception_ptr stored;
            {
                std::lock_guard lock(mutex);
                stored = error;
            }
            if (stored) std::rethrow_exception(stored);
            throw exception_io_data("The Apple Music Atmos renderer failed.");
        }
    };

    Impl(file::ptr source, OutputTarget target) : m_api(NativeApi::Get()), m_target(MapTarget(target)) {
        if (!m_api.ready()) throw exception_io_not_found();
        m_bridge = std::make_unique<ReadBridge>();
        m_bridge->source = std::move(source);
        const uint32_t status = m_api.create(kAbiVersion, m_target.mode, m_target.layout, static_cast<uint32_t>(kFramesPerRead),
                                             m_bridge.get(), &ReadBridge::Pull, &m_handle);
        if (status != kStatusOk || !m_handle) {
            m_bridge->StoreCallFailure();
            m_bridge->ThrowStored();
        }
    }

    ~Impl() { DestroyNoThrow(); }

    RenderInfo WaitForInfo(abort_callback& abort) {
        for (;;) {
            NativeInfo nativeInfo{};
            const uint32_t status = m_api.getInfo(m_handle, &nativeInfo);
            if (status == kStatusOk) {
                if (nativeInfo.structSize != sizeof(NativeInfo) || nativeInfo.abiVersion != kAbiVersion ||
                    nativeInfo.pcmFormat != kFloat32Pcm || !nativeInfo.channels || !nativeInfo.sampleRate ||
                    nativeInfo.outputMode != m_target.mode || nativeInfo.layout != m_target.layout ||
                    !(nativeInfo.flags & 1))
                    throw exception_io_data("Invalid Apple Music Atmos renderer format.");
                RenderInfo result;
                result.sampleRate = nativeInfo.sampleRate;
                result.channels = nativeInfo.channels;
                result.channelMask = nativeInfo.channelMask;
                result.hasJocObjects = (nativeInfo.flags & 1) != 0;
                result.dynamicObjects = nativeInfo.dynamicObjects;
                info = result;
                return result;
            }
            if (status != kStatusPending) {
                m_bridge->StoreCallFailure();
                m_bridge->ThrowStored();
            }
            WaitBriefly(abort);
        }
    }

    size_t Read(audio_sample* output, size_t maxFrames, abort_callback& abort) {
        if (!maxFrames) return 0;
        if (!info.channels) throw exception_io_data("Query Atmos renderer info before reading.");
        m_scratch.resize(maxFrames * info.channels);
        for (;;) {
            uint32_t written = 0;
            int32_t atEnd = 0;
            const uint32_t status =
                m_api.render(m_handle, m_scratch.data(), static_cast<uint32_t>(maxFrames), &written, &atEnd);
            if (status == kStatusOk) {
                const size_t samples = static_cast<size_t>(written) * info.channels;
                std::copy_n(m_scratch.begin(), samples, output);
                return written;
            }
            if (status == kStatusEnd && atEnd) return 0;
            if (status != kStatusPending) {
                m_bridge->StoreCallFailure();
                m_bridge->ThrowStored();
            }
            WaitBriefly(abort);
        }
    }

    void Cancel() noexcept {
        m_bridge->abort.abort();
        if (m_handle) m_api.cancel(m_handle);
    }

  private:
    void DestroyNoThrow() noexcept {
        if (!m_bridge) return;
        Cancel();
        if (m_handle) {
            m_api.destroy(m_handle);
            m_handle = nullptr;
        }
    }

    NativeApi& m_api;
    NativeTarget m_target;
    void* m_handle = nullptr;
    RenderInfo info;
    std::unique_ptr<ReadBridge> m_bridge;
    std::vector<float> m_scratch;
};

bool Renderer::Available() noexcept {
    return NativeApi::Get().ready();
}

Renderer::Renderer(file::ptr source, OutputTarget target)
    : m_impl(std::make_unique<Impl>(std::move(source), target)) {}

Renderer::~Renderer() = default;

RenderInfo Renderer::WaitForInfo(abort_callback& abort) {
    return m_impl->WaitForInfo(abort);
}

size_t Renderer::Read(audio_sample* output, size_t maxFrames, abort_callback& abort) {
    return m_impl->Read(output, maxFrames, abort);
}

void Renderer::Cancel() noexcept {
    m_impl->Cancel();
}

} // namespace glasswave::stream::atmos
