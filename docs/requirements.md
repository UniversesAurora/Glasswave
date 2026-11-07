# Requirements

The owner's original requirements are kept verbatim below (Chinese). Summary in English first.

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
