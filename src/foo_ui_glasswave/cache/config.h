#pragma once

namespace glasswave::cache {
inline constexpr int kAudioMinMiB = 64, kAudioMaxMiB = 262144, kAudioDefaultMiB = 10240;
inline constexpr int kImagesMinMiB = 32, kImagesMaxMiB = 8192, kImagesDefaultMiB = 512;
inline constexpr int kListsMinMiB = 16, kListsMaxMiB = 8192, kListsDefaultMiB = 128;
int AudioLimitMiB();
int ImageLimitMiB();
int ListLimitMiB();
void SetAudioLimitMiB(int value);
void SetImageLimitMiB(int value);
void SetListLimitMiB(int value);
void ApplyLimits();
} // namespace glasswave::cache
