#pragma once

#include <SDK/foobar2000.h>

// What a kind of item path that is not an ordinary file or a source's song means to the modules
// that look at items: whether it can play now and whether its tags name it yet. The module that
// owns such paths (cd/ for audio CD tracks) registers one while the component loads; lyrics, play
// statistics and the rest ask here and never name its scheme. Main thread after load.
namespace glasswave::media {

class PathBehaviour {
  public:
    virtual ~PathBehaviour() = default;
    virtual bool Owns(const char* path) const = 0;
    // Whether the item can be played now (a CD track: its disc is in a drive).
    virtual bool Available(const char* path) const = 0;
    // Whether the item's tags say what it is, so it can be looked up by them (lyrics): a CD track
    // is "CD Track 01" until its disc is tagged.
    virtual bool Identified(const metadb_handle_ptr& item) const = 0;
};

void RegisterPathBehaviour(const PathBehaviour& behaviour);

// The answers for any item; an ordinary path is available and identified.
bool ItemAvailable(const char* path);
bool ItemIdentified(const metadb_handle_ptr& item);

} // namespace glasswave::media
