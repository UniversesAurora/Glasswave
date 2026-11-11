# Requirements

## Unified lyric source priority (2026-11-11)

- Preserve all existing explicit per-track lyric selections, their associations and stored
  content. Automatic source priority, enablement, cache policy and account changes must not
  replace, rewrite or invalidate an owner's manual selection.
- Use one ordered/enabled source pipeline for embedded lyrics, adjacent same-name lyric
  files, native streaming-provider lyrics and general metadata-search plugins.
- The portable order token `source` means the current track's own origin: embedded/tag
  lyrics for a local file, or exact-ID native lyrics for the current streaming provider.
- The independent token `sidecar` means a same-directory, same-name lyric file when a real
  local backing file exists. It can be ranked before or after `source` and network sources.
- Native provider and file entries use a localized `(Source)` display suffix to distinguish
  them from general search plugins. Provider-native entries apply only to their provider.
- Apple Music has independent exact-ID native and general metadata-search entries. The
  latter is usable for local files and other providers through the existing signed-in helper.
- Automatic lyric caches must respect the effective source order/enablement and current
  file contents; a cached old winner must not override a newly preferred or enabled source.

The owner's original requirements are kept verbatim below (Chinese). Summary in English first.

## Follow-up display and verification requirements (2026-11-10)

- Publish usable streaming rows before optional related metadata; only views requiring
  exact album relationships wait for their metadata-ready prefix.
- Missing artwork must immediately show the existing placeholder, never retain the previous
  song's cover. Cache confirmed absence and revalidate in the background without blanking.
- Provide an Apple Music display-name region setting covering song, album and artist names,
  with Account default, Japan/ja-JP and China/zh-Hans-CN options. Apply the region to all
  available presentation metadata, including descriptions. Account authorization, library
  identities and playback storefront remain unchanged; unavailable regional counterparts
  retain original names. Preserve personally uploaded/library-only metadata. The owner has
  manually confirmed Account -> Japan KAF -> 花譜; rapid China/Japan switching and refresh
  behavior remain pending, independently of this accepted example.
- Ask the owner to verify official-client behavior and listening/original-file evidence
  when needed, before presenting conflicting metadata as a confirmed upstream defect.
- Prefer owner testing for appearance and interaction; do not proactively automate the UI
  or capture screenshots. Continue builds and functional automated checks independently.

## Owner follow-up requirements and open reports (since 21:40, 2026-11-10)

- Resolve catalog/library authority without silently substituting song identity. The owner
  reports Colors Of Happiness (Yui Makino) changing from catalog `1237064886` to reported
  `1865440142` / library `i.ZOMrromt4P51o6r`, ISRC `JPTE01701820`, ALAC 16/44.1. Mapping
  corrections and early-session RAII cleanup require native acceptance.
- Use authoritative identities for artist pages and song-row album links, including
  multi-artist credits. Fix the reported artist 400, album-link 404, romanized credits and
  unavailable links; display-name changes must not change navigation targets.
- China/Japan selects metadata presentation, not UI language. Keep canonical raw metadata,
  cancel obsolete generations and coalesce visible-first display work. Switching must avoid
  UI lag and stale names without requiring Refresh. The revision is implemented with passing
  mock region contracts; live rapid-toggle acceptance remains pending.
- Now Playing double-click must preserve stable row identity with shuffle off or on and on
  playback failure. Keep physical playlist projection independent of playback roles. After
  an explicit new selection, preserve the user's Play Next entry as the next track. A
  revision that clears this priority is unacceptable. Use a single new current schema and
  explicitly upgrade development data; do not add compatibility readers.
- Provide Apple Favorite Artists separately from all library-artist membership. Use official
  `inFavorites`; absent values are unknown, not false. Official reads are validated, but the
  separate page and real-account behavior still need acceptance. POST/DELETE and stale-read
  reconciliation now pass the mock phase; it performed no real-account writes.
- Name album membership actions Add to Library / Remove from Library. A successful remote
  acknowledgment must survive a later Refresh error instead of rolling back or showing stale
  Saved state. Use a generic mutation epoch and separate acknowledged state from eventual
  reconciliation for albums and playlists. The 61-operation mock scenario now passes;
  acceptance of the original owner-facing Refresh failure remains pending.
- Retain composition layers and coalesce hidden/minimized paints without adding timers.
  Suspend/resume FrameClock after default visibility/window-position processing. Four
  composition-policy cases and the separate four-case FrameClock regression are accepted
  logical evidence; actual GPU allocation/refcount behavior needs independent acceptance.
- Native probes must restore empty active/playing SDK state (`SIZE_MAX`) as well as ordinary
  state. Seventeen scheduler behavior cases passed, but restoration failed; the corrected
  18-case run is pending. Final owner-state restoration must use the authoritative
  post-upgrade backup after all probes, without compatibility readers.
- Treat the captured 18:02:59 automatic-playback event as a Glasswave playback-chain memory
  fault: stopping the engine/player reduced global commit from about 85 GB to 48 GB, with
  FOOBAR Shared at 39.646 GB. The exact allocation path and original seek trigger remain
  unconfirmed. Watchdog repair and a stable Frida baseline do not establish a root fix.
  Resolve current monitor ownership through `.local/tests/memory-watch-live.json`, retain
  8 GiB private/shared limits and separate GPU monitoring. The owner now requests continuous
  monitoring/protection, with further allocator analysis only if another fault occurs.
  This investigation must not block the remaining development.

Provisional phase acceptance (2026-11-11): Release at 09:29:48 local has zero warnings/errors;
provider baseline 50/50, mutation 61/61 and helper 29/29 pass. The mutation scenario records
20 empty-body 202/204 acknowledgments and two nonempty create-201 responses, zero unexpected
calls/contract errors, Japan 1 -> 0 -> 1 in both scenarios, bridge restoration and no
real-account writes. Evidence: `.local/tests/finalNov11-provider-summary.json`.
This paragraph records the earlier phase only. Current implementation, test evidence and
remaining acceptance are maintained in `docs/plan/status.md`. The owner checks visible UI
and interactions manually; no proactive UI automation/screenshots.

## Summary

- **Goal**: a foobar2000-based player UI that faithfully reproduces Windows 7 Aero Glass,
  Windows Media Player 12 and Windows Media Center — visuals *and* light effects, motion and
  interaction — on Windows 11. WMC's dynamic glow/breathing/transition style is a favorite.
- **Glass**: real transparency with live blur, tint, highlights, reflection and shadow;
  Win7 caption, frame and caption buttons; Win7 control styles; active/inactive/hover/press
  feedback. **Only our window** — no global theme or DWM changes.
- **Keep foobar2000**: decoding (FLAC/WAV/ALAC/MP3/AAC, CUE, art, tags), playlists, library,
  DSP, WASAPI shared/exclusive, ASIO, component ecosystem. Do not rewrite the audio core.
- **Features**: local browsing, transport, volume, seek, playlist management, WMP-like
  visualizations. Later: research Apple Music integration (library, control, lossless,
  exclusive output) without rewriting the audio core.
- **Performance**: native and efficient (no Electron), high refresh rate animations but no
  pointless rendering when idle, high DPI, multi-monitor, resizing, foobar2000 stability.
- **Reuse** mature, maintained, popular foobar2000 components for complex features where they
  exist — without lowering the visual bar.
- **Engineering**: agent-friendly repository structure, project management, git.

Later clarifications from the owner (2026-11-07):

- Use a freshly downloaded latest foobar2000 for development, never the old install in
  `E:\music\foobar2000`.
- Commit messages: detailed multi-line Linux-kernel style.
- Artifacts (comments, docs, commits) in English; conversation in Chinese.
- Fidelity must be 1:1 with Win7 (caption buttons, frame lines and highlights, shadow, WMP
  transport), but crisper than the low-resolution originals — no copied pixelation.

Apple Music follow-up (2026-11-09):

- The helper must run natively on Windows; WSL is not an acceptable runtime dependency.
- Reuse Adam's Go service and upstream structure with a small maintained adapter patch;
  remove unused client/output layers rather than rewriting the service in another language.
- Play progressively while the original encoded song is arriving. Do not wait for the
  entire song before starting; foobar2000 must still own decoding, DSP and output.
- Library lists must reuse loaded data, resolve library/catalog identities for artist
  artwork and albums, report real counts, and preserve track names in now-playing views.

Apple Music follow-up (2026-11-10):

- Resolve mid-song decoding failures through the complete segmented-stream lifecycle,
  including key changes, seeking, reconnection, error propagation and stale cache versions.
- Own the configured native Apple Music helper from inside the component: start it for
  login/account/library/playback requests, wait for readiness off the UI thread, retain
  authentication across direct player restarts, and stop owned processes on normal or
  abnormal player exit without terminating external services.
- Use one provider-neutral persistent cache for original encoded songs and all artwork.
  Playback must remain progressive; cached audio is reusable at the same actual quality.
  Show cached art immediately and retain that same artwork identity's pixels while
  background validation runs. A different song without artwork must immediately show the
  existing placeholder. Guard account/page lifetimes and old/new request races. Keep bounded
  memory/disk use and reserve the same storage model for future list caching.
- Prefetch the next actual track before the current track ends. Queue edits, shuffle,
  repeat, manual navigation, source quality, account changes and cancellation must not
  advance or corrupt the core's playback state or cause an unrelated track to play.
- Preserve paused lyrics through output-device reloads and unrelated list/navigation work.
  Provider root context menus must expose sign-in/sign-out for both initial providers.
- Expanded song tiles must group each real album once through shared provider-neutral rules.
- Play personal iCloud uploads in their actual available encoding, with library metadata,
  album artwork and library-only artist navigation independent of catalog availability.
- Add real Apple Music Dolby Atmos source delivery and object-aware rendering inside the
  foobar2000 input; the helper must continue to transport encoded audio only.
- Provide an Atmos on/off setting and common listening/output choices. The owner's current
  setup is a USB DAC feeding one pair of left/right speakers, not headphones or HDMI.
- Stream list pages in server order on demand. Sorting, expanded album grouping and local
  page search require the full logical source, but publish useful results after every page.
  Keep loading visible above the list and retain selection, focus and scroll position.
- Make list caching optional per provider/query, with cached-first background validation.
  Cached prefixes and network pagination must never mix different list generations.
- Own predictable shuffle scheduling across every UI entry point, Up Next and prefetch.
  Play Next takes immediate priority; ordinary additions enter the unplayed random order.
  Shuffle-off restores surviving canonical order, including additions, deletions and manual
  drag intent. Replacing Now Playing invalidates the previous list's restoration state.

## Original text

> # foobar2000 Aero Glass 自定义界面项目需求
>
> ## 一、项目背景
>
> 希望基于 foobar2000 开发一款高度还原 Windows 7 Aero Glass 风格的自定义播放器界面。
>
> 目标运行环境为 Windows 11，主要参考 Windows 7 时代的 Windows Media Player 12（WMP 12）和 Windows Media Center（WMC）。
>
> 希望不仅在视觉上还原当年的设计，也能够还原其细腻的光效、动态交互和操作体验。
>
> 项目以 foobar2000 为基础，保留其现有音频播放核心、解码能力、播放列表、媒体库、DSP 和音频输出等功能，而不是从零开发音频播放器。
>
> ## 二、界面与视觉需求
>
> ### 1. Windows 7 Aero Glass
>
> 希望尽可能完整、准确地还原 Windows 7 原生 Aero Glass 的视觉效果，而非简单模仿其颜色和布局。
>
> 包括：
>
> - 真正的窗口透明，可以实时透过播放器窗口看到后面的桌面和其他应用程序。
> - 实时背景模糊、玻璃着色、渐变、高光、反射和阴影。
> - Windows 7 原生 Aero 风格的标题栏、边框及窗口控制按钮。
> - Windows 7 经典控件样式，包括按钮、滑块、进度条、滚动条、菜单、列表等。
> - 窗口激活、失焦、鼠标悬停、按下和选中时的视觉反馈。
> - 整体视觉语言应与 Windows 7 原生软件保持一致，而不是现代 Windows 11 Fluent Design 风格。
>
> 仅播放器自身使用 Aero Glass，不希望修改 Windows 11 的全局主题、其他程序的窗口效果或系统 DWM 行为。
>
> ### 2. Windows Media Player 12
>
> 希望参考 WMP 12 的布局、控件及交互设计，包括：
>
> - 经典的音乐库、播放列表、专辑封面和播放控制界面。
> - 具有玻璃质感、立体渐变和蓝色高光的播放按钮。
> - 原生 Aero 风格的音量滑块和播放进度条。
> - 歌曲列表、导航区域、工具栏及菜单的经典视觉设计。
> - 细腻、自然的控件状态变化与反馈。
>
> ### 3. Windows Media Center
>
> 希望参考 Windows Media Center 的动态视觉表现，包括：
>
> - 按钮和选中项目的动态发光效果。
> - 鼠标悬停时的柔和光晕、高亮和渐变。
> - 具有呼吸感、脉冲感的焦点动画。
> - 专辑封面及媒体库内容的平滑移动、缩放和过渡。
> - 页面切换、列表操作时流畅自然的动画。
> - 富有玻璃质感、光泽及空间层次的整体视觉体验。
>
> 希望还原真正的动态交互，而非仅使用静态贴图模拟。
>
> ## 三、功能需求
>
> - 基于 foobar2000 SDK 实现自定义用户界面，尽可能完整地控制播放器窗口与界面。
> - 保留 foobar2000 原有的音频解码、播放列表、媒体库、DSP 和插件能力。
> - 保留 FLAC、WAV、ALAC、MP3、AAC 等格式，以及 CUE 分轨、专辑封面和元数据支持。
> - 保留 WASAPI 共享、WASAPI 独占及可用的 ASIO 输出能力。
> - 支持本地音乐浏览、播放控制、音量调整、播放进度及播放列表管理。
> - 希望具备类似 WMP 的动态音乐可视化效果。
> - 后续希望研究 Apple Music 接入的可行性，包括曲库访问、播放控制、无损音质和独占输出；但不要求为此重写 foobar2000 音频核心。
>
> ## 四、性能与兼容性需求
>
> - 主要面向 Windows 11。
> - 希望使用原生、高效的技术实现，尽可能降低 CPU、GPU 和内存占用。
> - 动画应流畅，支持高刷新率，但不希望静止界面持续进行无意义的高频渲染。
> - 支持高 DPI、多显示器及正常的窗口缩放。
> - 尽可能保持 foobar2000 原有的稳定性与播放性能。
> - 不希望使用 Electron。
> - 倾向于 C++、Win32、Direct2D、DirectComposition 等原生技术，但具体技术方案由 Agent 自行调查和评估，不预先限定。
> - 对于 Windows 11 下 Aero Glass 的还原，希望优先考虑仅作用于应用自身、长期稳定且易维护的实现方式。
> - 尽可能使用成熟的、持续更新的、广为欢迎的foobar2000组件来实现一些复杂而且已经有人实现好的功能，避免自己造轮子。但对视觉上的要求不能降低
>
> ## 五、整体设计原则
>
> 本项目最重要的是 **Windows 7 Aero Glass 的高保真还原，以及 WMP 12、Windows Media Center 的经典视觉与动态交互体验**。尤其是我非常喜欢Windows media center的效果，整个窗口也希望尽可能复原Windows7的窗口效果，包括玻璃、透明等
>
> 不希望仅仅开发一个使用蓝色渐变、半透明背景的普通播放器主题，而是尽可能接近 Windows 7 时代微软原生桌面软件的设计品质。
>
> 同时，项目应充分利用 foobar2000 已有的成熟音频能力，将开发重点放在自定义界面、窗口效果、控件设计和动画体验上。
>
> 以上是项目需求和技术偏好，不代表已经确定具体实现架构。希望 Agent 基于这些需求自行研究合适的技术方案。然后在本目录下进行开发，建立Agent编程的最佳实践的目录结构，并进行项目管理。创建git版本管理。
