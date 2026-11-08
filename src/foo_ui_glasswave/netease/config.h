#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace glasswave::netease {

enum class Quality { Auto, Lossless, Hires, Exhigh, Standard };

Quality GetQuality();
void SetQuality(Quality quality);

// Total size allowed for downloaded album covers, in MB.
int CoverCacheLimitMb();

// Non-empty: use an externally managed NeteaseCloudMusicApi service instead of our
// downloaded helper process.
std::wstring GetServiceUrlOverride();
void SetServiceUrlOverride(std::wstring_view url);

int64_t GetAccountId();
std::wstring GetAccountName();
void SetAccount(int64_t userId, std::wstring_view nickname);
void ClearAccount();

}  // namespace glasswave::netease
