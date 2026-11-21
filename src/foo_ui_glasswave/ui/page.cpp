#include "pch.h"
#include "ui/page.h"
#include "ui/song_table.h"

namespace glasswave::ui {

void Page::OnFavoritesChanged() {
    if (auto* source = SongTable()) {
        source->RefreshFavorites();
        m_host.ListChanged();
    }
}

SongTableSource* Page::SongTable() const {
    return dynamic_cast<SongTableSource*>(m_host.Source());
}

SongsFetch Page::AllSongs(ReadAllSongs read) {
    return Guard([this, read = std::move(read)](std::function<void(metadb_handle_list)> ready) {
        read(Guard([this, ready = std::move(ready)](bool complete) {
            if (!complete) return;
            if (auto* source = SongTable()) ready(source->VisibleItems());
        }));
    });
}

void Page::PlayWhenRead(ReadAllSongs read, std::wstring identity) {
    read(Guard([this, identity = std::move(identity)](bool) {
        auto* source = SongTable();
        if (!source) return;
        if (const size_t row = source->FindRowByIdentity(identity); row != SIZE_MAX) source->PlayFrom(row);
    }));
}

} // namespace glasswave::ui
