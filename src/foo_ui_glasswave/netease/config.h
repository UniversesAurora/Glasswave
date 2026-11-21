#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace glasswave::netease {

enum class Quality { Auto, Lossless, Hires, Exhigh, Standard };

Quality GetQuality();
void SetQuality(Quality quality);
// The quality songs are downloaded at (its own setting, as in NetEase's client).
Quality GetDownloadQuality();
void SetDownloadQuality(Quality quality);
// The service's levels to ask for, best first, for one quality setting.
std::vector<std::wstring> QualityLevels(Quality quality);


// Non-empty: use an externally managed NeteaseCloudMusicApi service instead of our
// downloaded helper process.
std::wstring GetServiceUrlOverride();
void SetServiceUrlOverride(std::wstring_view url);

int64_t GetAccountId();
std::wstring GetAccountName();
void SetAccount(int64_t userId, std::wstring_view nickname);
void ClearAccount();

}  // namespace glasswave::netease
