// SPDX-License-Identifier: MIT
// LRCLIB's public API. No account or token is required.
export function getConfig(config) {
    config.name = "LRCLIB";
    config.version = "1.0";
    config.author = "Glasswave";
}

export function getLyrics(meta, manager) {
    const parameters = {
        track_name: meta.title,
        artist_name: meta.artist,
        album_name: meta.album,
        duration: Math.round(meta.duration)
    };
    const query = Object.entries(parameters)
        .filter(([, value]) => value !== "" && value !== 0)
        .map(([key, value]) => `${key}=${encodeURIComponent(value)}`).join("&");
    let exact = false;
    const add = (item, confidence) => {
        const text = item.syncedLyrics || item.plainLyrics;
        if (!text || manager.checkAbort()) return;
        const lyric = manager.createLyric();
        lyric.title = item.trackName;
        lyric.artist = item.artistName;
        lyric.album = item.albumName;
        lyric.source = "LRCLIB";
        lyric.lyricText = text;
        lyric.confidence = confidence;
        manager.addLyric(lyric);
        exact = true;
    };
    const headers = { "User-Agent": "Glasswave/0.1 (foobar2000 lyrics)" };
    request({ url: `https://lrclib.net/api/get?${query}`, headers }, (error, response, body) => {
        if (!error && response.statusCode === 200) add(JSON.parse(body), 100);
    });
    if (exact || manager.checkAbort()) return;
    const search = `track_name=${encodeURIComponent(meta.title)}&artist_name=${encodeURIComponent(meta.artist)}`;
    request({ url: `https://lrclib.net/api/search?${search}`, headers }, (error, response, body) => {
        if (error || response.statusCode !== 200) return;
        for (const item of JSON.parse(body).slice(0, 12)) add(item, -1);
    });
}
