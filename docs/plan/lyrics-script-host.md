# Lyrics script host (ESLyric-compatible searchers)

Implementation work order. Read [ADR 0010](../adr/0010-lyrics.md) first. The lyrics
pane, the LRC model and `lyrics::Service` already exist; this task only adds searchers. Do not
change the view, the service's public API or the LRC model without asking the owner.

## Goal

Glasswave runs ESLyric's lyric source scripts (`searcher`) and parser scripts (`parser`)
unchanged, on an embedded JavaScript engine, and registers each searcher script as a
`lyrics::Searcher`. With ESLyric's own scripts and the community's word-timed QQ Music /
Kugou / NetEase scripts installed, playing a local file shows word-timed lyrics with
translations in the pane.

## What exists (do not rewrite)

- `lyrics/lyrics.h`, `lyrics/lrc.cpp`: `Lyrics` model, `ParseLrc` (standard and enhanced
  LRC, same-timestamp translation lines, `[offset:]`), `DecodeText`.
- `lyrics/lyrics_service.{h,cpp}`: `Query`, `Candidate`, `Searcher`, `RegisterSearcher`,
  `Service`. The service already runs every registered searcher on its own thread with a
  20 s budget, scores candidates (`BestCandidate`), caches the winner and delivers it.
- `stream::Provider::Lyrics`: streaming tracks (NetEase) get their lyrics before searchers
  run; searchers mostly matter for local files.

## Contract to implement

The ESLyric wiki documents it: <https://github.com/ESLyric/release/wiki/歌词源及解析器脚本>.
Reference scripts to test against (not committed; `.local/refs/lyrics`):

- ESLyric 1.0.7.0 built-ins (extracted from the DLL's resources): `searcher/kugou.js`,
  `qqmusic.js`, `netease.js`, `musixmatch.js`; `parser/krc.js`, `qrc.js`; libraries
  `lib/crypto-es/**` (MIT), `lib/querystring/querystring.min.js` (MIT).
- Robotxm/ESLyric-LyricsSource `current/` (GPL-3): `qrc/searcher/qqmusic_ex.js`,
  `qrc/parser/qrcjson.js`, `qrc/lib/qrc-decryptor/qrc-decryptor.js`,
  `yrc/searcher/netease_ex.js`, `yrc/parser/yrcjson.js`, `krc/parser/krc.js`.

Everything these scripts use, and therefore the complete required surface:

| Name | Behaviour |
| --- | --- |
| ES modules | `export function getConfig/getLyrics/parseLyric`; `import x from "path"` resolved against the importing file's folder, then `scripts/lib/`. |
| `getConfig(cfg)` | Searcher: `name`, `version`, `author`, `useRawMeta`. Parser: also `fileType`, `parsePlainText` (default true). |
| `getLyrics(meta, man)` | `meta`: `title`, `artist`, `album` (cleaned for search unless `useRawMeta`), `rawTitle`, `rawArtist`, `rawAlbum`, `path`, `rawPath`, `subSong`, `duration` (seconds). |
| `man.createLyric()` | Object with `title`, `artist`, `album`, `source`, `location`, `lyricText` (string) or `lyricData` (ArrayBuffer), `fileType` (default `"lrc"`), `isLocal`, `confidence` (0..100). Scripts reuse one object for several `addLyric` calls: copy on add. |
| `man.addLyric(obj)` | Converts with the parser for `obj.fileType` (none needed for `lrc`), then hands a `lyrics::Candidate` to the service. |
| `man.checkAbort()` | True when the `abort_callback` is set. |
| `man.getSvcData(key)` / `setSvcData(key, value)` | Persistent strings per script (musixmatch keeps its token here). Store in foobar2000 configuration or a JSON file under `<profile>\glasswave\lyrics`. Missing key returns `""`. |
| `request(urlOrOptions, cb)` | Options: `url`, `method` (`get`/`post`), `headers` (object), `body` (string), `raw` (body as ArrayBuffer), `timeout` (ms, default 6000). **The callback runs before `request` returns** (scripts depend on it: `qqmusic_ex.js` reads results right after the call). `cb(err, res, body)`: `err` is null or a message, `res` has `statusCode`, `statusMessage`, `headers`. Uses `net::SendHttp` (WinHTTP). |
| `messageLoop(ms)` | Runs pending promise jobs (`JS_ExecutePendingJob`) until none are left. |
| `evalLib(path)` | Evaluates `scripts/lib/<path>` as a classic script in the global scope (defines `querystring`). |
| `mxml.loadString(xml)` | Returns the document node or null. Nodes: `findElement(name[, mode])` (first descendant element with that name; `mxml.MXML_DESCEND` is the default mode; the result is also iterable over every match in document order, because scripts write `for (const s of doc.findElement('songinfo') \|\| [])`), `getFirstChild()`, `getCDATA()` (text of a CDATA node), `getText()`, `getAttr(name)` (null if absent). Write a small XML reader; there is no need for a full DOM. |
| `zlib.uncompress(buf)` | ArrayBuffer or typed array in, ArrayBuffer out (zlib stream), null on error. Use miniz. |
| `atob`, `btoa` | UTF-8 strings. Original QQ scripts pass Unicode metadata directly, and KRC parsers decode UTF-8 JSON / translations. |
| `arrayBufferToString(buf)` | UTF-8 decode. Add `stringToArrayBuffer(str)` (UTF-8 encode) too. |
| `console.log(...)` | foobar2000 console, prefixed `Glasswave lyrics [script name]:`. |
| `utils.createProfiler`, `debugWriteArrayBuffer` | Stubs (no-ops) are enough. |
| `import * as decoder from "parser_ext.so"` | `decodeQrc(ArrayBuffer)` returns decrypted zlib data. When the user's community `qrc-decryptor.js` is installed, a small compatibility module delegates to its exported `decryptQrcBytes`. It removes the original script's `[offset:0]` binary prefix and adapts bytes to hex. The GPL decryptor stays external and unchanged. Without it, return null. No decryption algorithm is ported into the component. |

Parsers convert `lyricText` (or `lyricData` when `parsePlainText` is false) to LRC in
`context.lyricText`; `context.duration` is the track length. Map `fileType` -> parser by the
parsers' `getConfig`. A parser's output goes into `Candidate::lrc`; the service parses and
scores it.

`meta.title` / `meta.artist` cleaning when `useRawMeta` is false: ESLyric strips bracketed
suffixes ("(Live)", "【...】", "feat. ..."); take the first artist of a multi-value tag.

## Where scripts come from

Scan, in this order, merging by file name (later folders override earlier ones):

1. `<profile>\eslyric-data\scripts` — written by ESLyric itself when the owner installs it,
   kept current by ESLyric updates. This is the main source.
2. `<profile>\glasswave\lyrics\scripts` — Glasswave's own folder for the community scripts
   and ours.

Each folder has `searcher\`, `parser\`, `lib\`. Load once at start-up on a worker thread; a
"Reload scripts" button in the preferences reloads. Never copy, bundle or commit ESLyric's or
the community's scripts (unknown / GPL licences); the repo only carries scripts we write.

## Engine and dependencies

Add both to `third_party/deps.lock.json`, `scripts/bootstrap.ps1` (extract into
`third_party/<name>`), `third_party/README.md`, and compile them in the project with
warnings off for those files only (the build must stay warning-free at /W4 for our code).

| Dependency | Version | URL | SHA-256 | Licence |
| --- | --- | --- | --- | --- |
| quickjs-ng (amalgamation: `quickjs-amalgam.c`, `quickjs.h`) | 0.18.0 | https://github.com/quickjs-ng/quickjs/releases/download/v0.18.0/quickjs-amalgam.zip | `6C05066A6B62B483E2BA0F83D7A16688C6958182B214BFD693529434CEF84A9E` | MIT |
| miniz (`miniz.c`, `miniz.h`) | 3.1.2 | https://github.com/richgel999/miniz/releases/download/3.1.2/miniz-3.1.2.zip | `F0446D863F9C19926AD9483C523FDC42E42B8D4A6A431D27E09D49C79A140D9A` | MIT |

Do not compile `quickjs-libc` (no file or process access for scripts).

## Threading and lifetime

- One `JSRuntime` + `JSContext` per search (per searcher per track). QuickJS is single-
  threaded per runtime; the service already calls `Search` on its own thread. Creating a
  runtime and loading a script takes a few milliseconds; cache compiled bytecode per script
  (`JS_WriteObject` / `JS_ReadObject`) if profiling shows it matters.
- Set `JS_SetInterruptHandler` to stop scripts when `abort` is set or after the 20 s budget,
  and a memory limit (`JS_SetMemoryLimit`, 64 MB).
- Parsers run in the same context as the searcher that produced the lyric (load them into it
  on demand).
- No global state shared between runtimes except the read-only script list.

## Files

```
src/foo_ui_glasswave/lyrics/script_host.{h,cpp}   // runtime setup, module loader, built-ins
src/foo_ui_glasswave/lyrics/script_xml.{h,cpp}    // the mxml subset
src/foo_ui_glasswave/lyrics/script_searcher.{h,cpp} // lyrics::Searcher over one script
src/foo_ui_glasswave/lyrics/scripts.{h,cpp}       // discovery, registration, reload
```

Keep object file names unique across folders (two `service.cpp` files once broke the link:
the project compiles every folder into one output directory).

## Preferences

Add a "Lyrics" section to `ui/settings_page.cpp` through `settings::Field` rows: a note
listing the script folders found and how many searchers loaded, a "Reload scripts" button, an
"Open script folder" button, one on/off choice per searcher (stored by script file name), and
"Clear lyrics cache" (deletes `<profile>\glasswave\lyrics\*.lrc`). Strings go through `Tr`
and into `lang/zh-CN.json`.

## Our own scripts (second step, after the host works)

Written by us, MIT, committed under `scripts/lyrics/` and copied into
`<profile>\glasswave\lyrics\scripts` by `run-dev.ps1`:

- `lrclib.js`: <https://lrclib.net/api/get?track_name=&artist_name=&album_name=&duration=>,
  falling back to `/api/search`; returns `syncedLyrics` (line-timed) or `plainLyrics`.
- `applemusic.js`: Apple Music syllable lyrics (TTML with `<span begin end>` per syllable,
  `ttm:agent` for duets, translations in `<iTunesMetadata><translations>`). Needs the user's
  `media-user-token` (from music.apple.com cookies) and a developer token; both go into
  `getSvcData`, entered through a preferences text field. Convert TTML to enhanced LRC.
- `spotify.js`: Spotify's color-lyrics endpoint needs a web access token derived from the
  user's `sp_dc` cookie; Spotify changes the token scheme often, so isolate it in one function.

Ask the owner before starting the Spotify / Apple Music scripts; they require accounts.

## Acceptance

1. Build warning-free; `scripts/run-dev.ps1` deploys.
2. With ESLyric's built-in scripts copied into `<profile>\eslyric-data\scripts` of the dev
   instance and Robotxm's `current/` scripts in `<profile>\glasswave\lyrics\scripts`:
   - a local Chinese pop file without lyrics gets word-timed QQ Music or Kugou lyrics with
     the translation, shown in the pane within a few seconds;
   - a local English file gets Musixmatch (or LRCLIB) lyrics;
   - the foobar2000 console lists each loaded script once and no script errors.
3. Skipping tracks quickly does not leave searches running (watch the console and CPU).
4. Closing foobar2000 during a search exits promptly.
5. Unit-check the pieces with no network: the module loader (relative and `lib/` imports),
   `mxml` against the QQ Music sample XML, `zlib.uncompress`, `request` callback ordering.

## Hand-back

Commit in Linux-kernel style ([commit-style](../agent/commit-style.md)), one logical change
per commit, English. Update `docs/plan/status.md` (what works, what was tested, what not)
and this file's checklist below. Claude reviews the result against this document.

## Checklist

- [x] Dependencies fetched and verified by bootstrap
- [x] Engine + module loader + built-ins
- [x] mxml subset (Windows MSXML tree, DTD / external resolution disabled)
- [x] Searcher wrapper + registration + parser dispatch
- [x] Script discovery + reload + preferences rows
- [x] Offline host contract checks, including 11 unmodified reference script configs
- [x] Live NetEase, Kugou word timing and LRCLIB search / selection
- [x] Original QQ and QQ Ex live probes each returned 10 word-timed candidates
- [x] Own LRCLIB script
- [ ] Full acceptance across Chinese / English local libraries and Musixmatch endpoint
- [ ] Apple Music / Spotify scripts (owner account setup still required)

2026-11-09 implementation notes. The owner's subsequent request authorizes source-specific
priority and manual result selection, extending the service API while retaining the model
and existing rendering behavior. Search, source-specific search, LRC / text loading and
automatic-selection reset are available from both lyric regions. Automatic fallback cache
does not outrank the track's own lyrics. Explicit selections are separate `.selected.lrc`
files and survive automatic-cache clearing. No third-party scripts are bundled or tracked.
The dev profile contains the owner's reference scripts for integration verification.
