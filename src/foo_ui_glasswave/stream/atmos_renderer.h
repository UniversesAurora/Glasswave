#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

#include <SDK/foobar2000.h>

namespace glasswave::stream::atmos {

enum class OutputTarget : uint32_t {
    StereoSpeakers = 0, // FL, FR; mask 0x00003
    Headphones = 1,     // HRTF-rendered stereo; mask 0x00003
    Speakers5_1 = 2,    // FL, FR, FC, LFE, SL, SR; mask 0x0060F
    Speakers7_1 = 3,    // FL, FR, FC, LFE, BL, BR, SL, SR; mask 0x0063F
};

// Frames the renderer produces per call; consumers read in chunks of this size.
inline constexpr size_t kFramesPerRead = 1024;

struct RenderInfo {
    uint32_t sampleRate = 0;
    uint32_t channels = 0;
    uint32_t channelMask = 0;
    uint32_t dynamicObjects = 0;
    bool hasJocObjects = false;
};

// The source must return decrypted, elementary E-AC-3 syncframes in sequential order. The
// managed renderer owns the decode thread and pulls from this file through a non-throwing
// C ABI callback. Seeking is intentionally unavailable until a verified EC-3 index exists.
class Renderer {
  public:
    // Loads and validates the adjacent NativeAOT DLL once for this process. No source,
    // decoder context, or managed render thread is created.
    static bool Available() noexcept;

    explicit Renderer(file::ptr source, OutputTarget target = OutputTarget::StereoSpeakers);
    ~Renderer();

    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    RenderInfo WaitForInfo(abort_callback& abort);

    // Returns rendered frames. Output is interleaved float PCM converted to foobar's
    // audio_sample type, with channels in the order documented by OutputTarget.
    size_t Read(audio_sample* output, size_t maxFrames, abort_callback& abort);
    void Cancel() noexcept;

    static bool CanSeek() { return false; }

  private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace glasswave::stream::atmos
