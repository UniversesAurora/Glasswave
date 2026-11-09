#pragma once

#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace glasswave::lyrics {

struct ScriptConfig {
    std::filesystem::path path;
    std::string name, fileType;
    bool rawMeta = false, plainText = true;
};

struct ScriptRequest {
    std::string url, method = "GET", body;
    std::map<std::string, std::string> headers;
    int timeoutMs = 6000;
};

struct ScriptResponse {
    int status = 0;
    std::string message, body, error;
    std::map<std::string, std::string> headers;
};

struct ScriptLyric {
    std::string title, artist, album, source, text;
    int confidence = -1;
};

// No foobar2000 dependency. Tests supply deterministic HTTP and storage callbacks.
struct ScriptEnvironment {
    std::vector<std::filesystem::path> roots;
    std::vector<ScriptConfig> parsers;
    std::function<bool()> aborted;
    std::function<ScriptResponse(const ScriptRequest&)> request;
    std::function<std::string(const std::string&)> getData;
    std::function<void(const std::string&, const std::string&)> setData;
    std::function<void(const std::string&)> log;
    std::function<void(ScriptLyric)> found;
};

class ScriptHost {
  public:
    explicit ScriptHost(ScriptEnvironment environment);
    ~ScriptHost();
    ScriptHost(const ScriptHost&) = delete;
    ScriptHost& operator=(const ScriptHost&) = delete;

    ScriptConfig Configure(const std::filesystem::path& path);
    void Search(const ScriptConfig& script, const std::map<std::string, std::string>& meta, unsigned subsong,
                double duration);
    std::string Parse(const ScriptConfig& parser, std::string text, double duration);

  private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace glasswave::lyrics
