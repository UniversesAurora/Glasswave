#include "pch.h"
#include "media/path_behaviour.h"

#include <vector>

namespace glasswave::media {

namespace {

std::vector<const PathBehaviour*>& Registry() {
    static std::vector<const PathBehaviour*> behaviours;
    return behaviours;
}

const PathBehaviour* Find(const char* path) {
    if (!path) return nullptr;
    for (const PathBehaviour* behaviour : Registry())
        if (behaviour->Owns(path)) return behaviour;
    return nullptr;
}

} // namespace

void RegisterPathBehaviour(const PathBehaviour& behaviour) {
    Registry().push_back(&behaviour);
}

bool ItemAvailable(const char* path) {
    const PathBehaviour* behaviour = Find(path);
    return !behaviour || behaviour->Available(path);
}

bool ItemIdentified(const metadb_handle_ptr& item) {
    if (!item.is_valid()) return false;
    const PathBehaviour* behaviour = Find(item->get_path());
    return !behaviour || behaviour->Identified(item);
}

} // namespace glasswave::media
