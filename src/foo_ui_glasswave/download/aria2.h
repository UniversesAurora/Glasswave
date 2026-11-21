#pragma once

#include "stream/provider.h"

#include <cstdint>
#include <string>

// aria2 (https://aria2.github.io/) as the download engine (owner, 2026-11-15: "use a mature
// engine"): segmented downloads over several connections, resumed from its control files.
// The official Windows release is downloaded on first use into <profile>\glasswave\aria2
// (a newer aria2c.exe can replace it) and runs as a child process in a kill-on-close job, talking
// JSON-RPC on 127.0.0.1 with a random secret; it also exits with foobar2000
// (--stop-with-process). Every call blocks: download worker threads only.
namespace glasswave::download::aria2 {

struct Status {
    enum class Kind { Active, Waiting, Paused, Error, Complete, Removed };
    Kind kind = Kind::Waiting;
    uint64_t completed = 0, total = 0; // bytes; total 0: not known yet
    uint64_t speed = 0;                // bytes per second
    int connections = 0;
    std::wstring error; // Error: aria2's message
};

// Installs and starts the engine when needed. False (with the reason) when it cannot run;
// after a failure it answers the same until RetryAfterFailure (the next download command).
bool Ensure(abort_callback& abort, std::wstring* error);
void RetryAfterFailure();
// Starts fetching `link` into dir\out with up to `connections` connections; returns its id.
// An existing out file with its ".aria2" control file continues where it stopped.
std::string Add(const stream::DownloadLink& link, const std::wstring& dir, const std::wstring& out,
                int connections, abort_callback& abort);
Status Tell(const std::string& gid, abort_callback& abort);
// Stops a download and forgets it; its files stay (they resume with the next Add).
void Remove(const std::string& gid);
// The engine's version while it runs ("1.37.0"), else empty.
std::wstring Version();
void Shutdown();

} // namespace glasswave::download::aria2
