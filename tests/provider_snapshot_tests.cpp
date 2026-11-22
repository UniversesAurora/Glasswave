#include "cache/provider_snapshot.h"
#include "cache/store.h"

#include <atomic>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;
using glasswave::cache::Kind;
using glasswave::cache::Store;
using glasswave::cache::provider_snapshot::Snapshot;

constexpr std::size_t kStringLimit = 1024 * 1024;
std::atomic_size_t checkCount = 0;

class TestFailure final : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};

void Check(bool condition, std::string_view message) {
    ++checkCount;
    if (condition) return;
    throw TestFailure(std::string(message));
}

[[noreturn]] void Fail(std::string_view message) {
    throw TestFailure(std::string(message));
}

fs::path Normalized(const fs::path& path) {
    std::error_code error;
    const auto absolute = fs::absolute(path, error);
    if (error) Fail("Unable to make a test path absolute");
    const auto canonical = fs::weakly_canonical(absolute, error);
    if (error) Fail("Unable to resolve a test path");
    return canonical.lexically_normal();
}

fs::path ValidateRootArgument(const fs::path& argument, const fs::path& expectedParent) {
    const auto parent = Normalized(expectedParent);
    const auto root = Normalized(argument);
    Check(root.parent_path() == parent, "Provider snapshot root must be a direct child of .local/tests");
    const auto name = root.filename().string();
    constexpr std::string_view prefix = "provider-snapshot-";
    Check(name.size() > prefix.size() && name.starts_with(prefix),
          "Provider snapshot root must use the provider-snapshot- prefix");
    return root;
}

std::string FailureMessage(std::exception_ptr failure) {
    try {
        std::rethrow_exception(failure);
    } catch (const std::exception& error) {
        return error.what();
    } catch (...) {
        return "Unknown provider snapshot failure";
    }
}

template <typename Function>
std::string RunCase(const fs::path& root, std::string_view name, Function&& function) {
    const auto directory = Normalized(root / name);
    try {
        Check(directory.parent_path() == Normalized(root), "Provider snapshot case escaped its root");
        fs::create_directories(directory);
        function(directory);
        return {};
    } catch (...) {
        return FailureMessage(std::current_exception());
    }
}

Snapshot Fixture() {
    Snapshot snapshot;
    snapshot.provider = "apple";
    snapshot.sourceOrigin = "external:https://127.0.0.1:45871/fixture";
    snapshot.context = "provider-context-v1;6:apple-v16:Japan0:";
    snapshot.locale = "zh-Hans-CN";
    snapshot.accountId = "dsid-100000000001";
    snapshot.accountName = "Glasswave Listener";
    snapshot.sourceStorefront = "US";
    snapshot.displayRegion = 1;
    // Named fields: stream::Playlist grows details between the count and the flags.
    snapshot.created = {
        {.id = L"p.created-1", .name = L"My Playlist", .coverUrl = L"https://example.invalid/cover-1",
         .trackCount = 12, .own = true},
        {.id = L"p.created-2", .name = L"朝のプレイリスト", .coverUrl = L"https://example.invalid/cover-2",
         .trackCount = -1, .own = true},
    };
    snapshot.saved = {
        {.id = L"p.saved-1", .name = L"Saved Playlist", .coverUrl = L"https://example.invalid/cover-3",
         .trackCount = 24, .saved = true},
    };
    snapshot.bindings = {{"favorites", "p.favorite-system-playlist"}};
    return snapshot;
}

void RoundTrip(const fs::path& root) {
    Store store(root);
    const std::string accountScope = store.Scope("apple");
    const Snapshot expected = Fixture();
    Check(glasswave::cache::provider_snapshot::Save(store, accountScope, expected),
          "Valid provider snapshot did not save");
    const auto loaded = glasswave::cache::provider_snapshot::Load(store, accountScope, expected.provider,
                                                                  expected.sourceOrigin, expected.context);
    Check(loaded == expected, "Provider snapshot did not round-trip exactly");

    Snapshot missingIdentity = expected;
    missingIdentity.accountId.clear();
    Check(!glasswave::cache::provider_snapshot::Save(store, accountScope, missingIdentity),
          "Provider snapshot without a stable account id was accepted");
    Check(glasswave::cache::provider_snapshot::Load(store, accountScope, expected.provider,
                                                    expected.sourceOrigin, expected.context) == expected,
          "Rejected account identity replaced the previous snapshot");

    Snapshot duplicate = expected;
    duplicate.bindings.push_back(expected.bindings.front());
    Check(!glasswave::cache::provider_snapshot::Save(store, accountScope, duplicate),
          "Duplicate navigation bindings were accepted");

    Snapshot emptyResource = expected;
    emptyResource.bindings.front().resourceId.clear();
    Check(!glasswave::cache::provider_snapshot::Save(store, accountScope, emptyResource),
          "An empty navigation target binding was accepted");

    Snapshot tooManyBindings = expected;
    tooManyBindings.bindings.clear();
    for (int i = 0; i < 65; ++i) {
        tooManyBindings.bindings.push_back({"binding-" + std::to_string(i), "resource-" + std::to_string(i)});
    }
    Check(!glasswave::cache::provider_snapshot::Save(store, accountScope, tooManyBindings),
          "Navigation binding count limit was not enforced");
}

void IdentityIsolation(const fs::path& root) {
    Store store(root);
    const std::string accountScope = store.Scope("apple");

    Snapshot first = Fixture();
    first.sourceOrigin = "external:https://bridge.example/root;context/cached";
    first.context = "provider-context-v1;11:ja-JP0:";
    first.accountName = "First identity";

    Snapshot second = first;
    second.sourceOrigin = "external:https://bridge.example/root";
    second.context = "cached;context/provider-context-v1;11:ja-JP0:";
    second.accountName = "Second identity";

    Check(glasswave::cache::provider_snapshot::Save(store, accountScope, first),
          "First key-isolation snapshot did not save");
    Check(!glasswave::cache::provider_snapshot::Load(store, accountScope, second.provider,
                                                     second.sourceOrigin, second.context),
          "A delimiter-bearing source/context pair collided across provider identities");

    Check(glasswave::cache::provider_snapshot::Save(store, accountScope, second),
          "Second key-isolation snapshot did not save");
    const auto firstLoaded = glasswave::cache::provider_snapshot::Load(store, accountScope, first.provider,
                                                                       first.sourceOrigin, first.context);
    const auto secondLoaded = glasswave::cache::provider_snapshot::Load(store, accountScope, second.provider,
                                                                        second.sourceOrigin, second.context);
    Check(firstLoaded == first && secondLoaded == second,
          "Distinct provider identities did not remain independently addressable");

    glasswave::cache::provider_snapshot::Erase(store, accountScope, first.provider, first.sourceOrigin,
                                               first.context);
    Check(!glasswave::cache::provider_snapshot::Load(store, accountScope, first.provider, first.sourceOrigin,
                                                     first.context),
          "Provider snapshot erase removed the wrong identity");
    Check(glasswave::cache::provider_snapshot::Load(store, accountScope, second.provider, second.sourceOrigin,
                                                    second.context) == second,
          "Provider snapshot erase removed a neighboring identity");
}

void AggregateBudget(const fs::path& root) {
    Store store(root);
    const std::string accountScope = store.Scope("apple");
    Snapshot oversized = Fixture();
    oversized.created.clear();
    oversized.saved.clear();
    oversized.created.resize(9);
    for (auto& playlist : oversized.created) {
        playlist.id = L"p.large";
        playlist.name.assign(kStringLimit, L'x');
        playlist.trackCount = 1;
        playlist.own = true;
    }

    Check(!glasswave::cache::provider_snapshot::Save(store, accountScope, oversized),
          "Aggregate provider snapshot budget accepted more than 8 MiB");
    Check(store.CurrentUsage(Kind::Data).entries == 0,
          "Rejected aggregate provider snapshot left a committed cache entry");
    Check(!glasswave::cache::provider_snapshot::Load(store, accountScope, oversized.provider,
                                                     oversized.sourceOrigin, oversized.context),
          "Rejected aggregate provider snapshot remained readable");

    Snapshot valid = Fixture();
    Check(glasswave::cache::provider_snapshot::Save(store, accountScope, valid),
          "Store did not remain usable after a bounded aggregate rejection");
    Check(glasswave::cache::provider_snapshot::Load(store, accountScope, valid.provider, valid.sourceOrigin,
                                                    valid.context) == valid,
          "Valid snapshot after aggregate rejection did not round-trip");
}

void Utf8Boundary(const fs::path& root) {
    Store store(root);
    const std::string accountScope = store.Scope("apple");
    Snapshot accepted = Fixture();
    accepted.created[0].name.assign(kStringLimit, L'x');
    Check(glasswave::cache::provider_snapshot::Save(store, accountScope, accepted),
          "A field at the exact UTF-8 string boundary was rejected");
    Check(glasswave::cache::provider_snapshot::Load(store, accountScope, accepted.provider,
                                                    accepted.sourceOrigin, accepted.context) == accepted,
          "A field at the exact UTF-8 string boundary did not round-trip");

    Snapshot oversized = accepted;
    oversized.created[0].name.push_back(L'x');
    Check(!glasswave::cache::provider_snapshot::Save(store, accountScope, oversized),
          "A field one byte beyond the UTF-8 string boundary was accepted");
    Check(glasswave::cache::provider_snapshot::Load(store, accountScope, accepted.provider,
                                                    accepted.sourceOrigin, accepted.context) == accepted,
          "Rejected UTF-8 overflow replaced the previous committed snapshot");

    Snapshot malformed = accepted;
    malformed.created[0].name = L"valid";
    malformed.created[0].name.push_back(static_cast<wchar_t>(0xd800));
    Check(!glasswave::cache::provider_snapshot::Save(store, accountScope, malformed),
          "An unpaired UTF-16 surrogate was accepted");
    Check(glasswave::cache::provider_snapshot::Load(store, accountScope, accepted.provider,
                                                    accepted.sourceOrigin, accepted.context) == accepted,
          "Malformed UTF-16 replaced the previous committed snapshot");
}

} // namespace

int main(int argc, char** argv) {
    try {
        Check(argc == 2, "Usage: provider_snapshot_tests.exe <unique-test-root>");
        const auto root = ValidateRootArgument(argv[1], fs::current_path() / ".local/tests");
        Check(!fs::exists(root), "Unique provider snapshot test root already exists");
        fs::create_directories(root);

        std::vector<std::string> failures;
        const auto run = [&](std::string_view name, auto function) {
            auto failure = RunCase(root, name, function);
            if (!failure.empty()) failures.push_back(std::string(name) + ": " + failure);
        };
        run("round-trip", RoundTrip);
        run("identity", IdentityIsolation);
        run("aggregate", AggregateBudget);
        run("utf8", Utf8Boundary);

        if (!failures.empty()) {
            for (const auto& failure : failures)
                std::cerr << failure << '\n';
            return 1;
        }
        std::cout << "Provider snapshot checks passed: " << checkCount.load() << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
