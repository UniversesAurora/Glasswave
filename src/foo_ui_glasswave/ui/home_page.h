#pragma once

#include "ui/page.h"

// Home (owner, 2026-11-15): the home pages of the signed-in sources with
// stream::Capability::Home, in one. On top each source's featured cards in a shelf of its own,
// Apple Music's first, then NetEase Cloud Music's, drawn as Apple Music's Top Picks cards
// (ui/card_layer.h). Below them the sources' recommended rows, taken in turn (the first of each,
// then the second of each...), as covers or song cards marked with their source.
//
// A visit shows the last home page read this session at once and reads it again when it is
// older than a few minutes; Refresh reads it now.
namespace glasswave::ui {

std::unique_ptr<Page> MakeHomePage(PageHost& host, const View& view);

} // namespace glasswave::ui
