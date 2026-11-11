#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "applemusic/artwork_url.h"

namespace {

size_t checks = 0;

void Check(bool condition, const char* message) {
    ++checks;
    if (condition) return;
    std::cerr << message << '\n';
    std::exit(1);
}

void Accepted() {
    const std::vector<std::wstring_view> urls{
        L"https://is1-ssl.mzstatic.com/image/thumb/Music/source.jpg?token=a%2Fb",
        L"HTTPS://IS1-SSL.MZSTATIC.COM/image/thumb/Music/source.jpg",
        L"https://store-035.blobstore.apple.com/uploaded/cover?signature=a%2Fb%3D",
        L"https://store-033.blobstore.apple.com/uploaded/cover?X-Amz-Credential=a%2Fb&X-Amz-Signature=abc",
        L"https://a.b.mzstatic.com/i?x=@value#fragment",
    };
    for (size_t i = 0; i < urls.size(); ++i) {
        const std::string label = "Allowed artwork URL " + std::to_string(i + 1) + " was rejected";
        Check(glasswave::applemusic::IsAllowedAppleArtworkUrl(urls[i]), label.c_str());
    }
}

void Rejected() {
    const std::vector<std::wstring_view> urls{
        L"",
        L"http://store-035.blobstore.apple.com/x",
        L"https://user@store-035.blobstore.apple.com/x",
        L"https://user:pass@store-035.blobstore.apple.com/x",
        L"https://user%40store-035.blobstore.apple.com/x",
        L"https://store-035.blobstore.apple.com:443/x",
        L"https://store-035.blobstore.apple.com:8443/x",
        L"https://store-035.blobstore.apple.com.evil.test/x",
        L"https://store-035.blobstore.apple.com@evil.test/x",
        L"https://store-035.blobstore.apple.co/x",
        L"https://store-035.blobstore.apple.com./x",
        L"https://evilblobstore.apple.com/x",
        L"https://blobstore.apple.com/x",
        L"https://is1-ssl.mzstatic.com.evil.test/x",
        L"https://evil-mzstatic.com/x",
        L"https://mzstatic.com/x",
        L"https://-bad.blobstore.apple.com/x",
        L"https://bad-.blobstore.apple.com/x",
        L"https://bad_name.blobstore.apple.com/x",
        L"https://bad..blobstore.apple.com/x",
        L"https://store-035.blobstore.apple.com/x%0A",
        L"https://store-035.blobstore.apple.com/x%7F",
        L"https://store-035.blobstore.apple.com/x%GG",
        L"https://store-035.blobstore.apple.com/x\\@evil.test",
        L"https://store-035.blobstore.apple.com/a b",
        L"https://store-035.blobstore.apple.com/a\tb",
    };
    for (size_t i = 0; i < urls.size(); ++i) {
        const std::string label = "Rejected artwork URL " + std::to_string(i + 1) + " was accepted";
        Check(!glasswave::applemusic::IsAllowedAppleArtworkUrl(urls[i]), label.c_str());
    }

    std::wstring newline = L"https://store-035.blobstore.apple.com/x";
    newline.push_back(L'\n');
    Check(!glasswave::applemusic::IsAllowedAppleArtworkUrl(newline), "Raw newline was accepted");
    std::wstring nul = L"https://store-035.blobstore.apple.com/x";
    nul.push_back(L'\0');
    Check(!glasswave::applemusic::IsAllowedAppleArtworkUrl(nul), "Raw NUL was accepted");
}

void SignedQueryIsUnchanged() {
    std::wstring signedUrl =
        L"https://store-035.blobstore.apple.com/uploaded/cover"
        L"?X-Amz-Credential=a%2Fb%3D&X-Amz-Signature=abc%2Bdef&Expires=123";
    const std::wstring before = signedUrl;
    Check(glasswave::applemusic::IsAllowedAppleArtworkUrl(signedUrl), "Signed blobstore URL was rejected");
    Check(signedUrl == before, "Signed URL changed during validation");
}

} // namespace

int main() {
    Accepted();
    Rejected();
    SignedQueryIsUnchanged();
    std::cout << "Apple artwork URL checks passed: " << checks << '\n';
    return 0;
}
