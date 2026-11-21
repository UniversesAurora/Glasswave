#pragma once

#include "stream/list_session.h"

#include <memory>
#include <utility>
#include <vector>

namespace glasswave::stream {

// The rows of one kind from a page; empty when the page holds another kind.
template <class Row> std::vector<Row> RowsOf(ListPage& page) {
    if (auto* rows = std::get_if<std::vector<Row>>(&page.rows)) return std::move(*rows);
    return {};
}

// Reads a whole list the way the UI does (a ListSession loading every page, the provider's
// metadata enrichment included) and completes once the rows are final or the read failed.
// The disk cache is bypassed, so the result is what the service answers now. For probes and
// diagnostics; main thread, `done` runs once on the main thread.
inline void ReadList(Provider& provider, ListQuery query, ListPageDone done) {
    struct Reading {
        std::shared_ptr<ListSession> session;
        ListPageDone done;
        bool finished = false;
    };
    query.cache = ListCachePolicy::None;
    auto reading = std::make_shared<Reading>();
    reading->done = std::move(done);
    reading->session = ListSession::Open(
        provider, std::move(query),
        [reading](const ListState& state) {
            if (reading->finished || state.loading) return;
            if (state.error.empty() && !(state.complete && !state.page.enrichmentPending)) return;
            reading->finished = true;
            ListPage page = state.page;
            std::wstring error = state.error;
            if (reading->session) {
                reading->session->Cancel();
                reading->session.reset();
            }
            if (reading->done) std::exchange(reading->done, nullptr)(std::move(page), std::move(error));
        },
        ListLoadMode::All);
    if (reading->finished && reading->session) {
        reading->session->Cancel();
        reading->session.reset();
    }
}

} // namespace glasswave::stream
