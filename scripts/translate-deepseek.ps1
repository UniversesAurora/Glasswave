#Requires -Version 7
<#
.SYNOPSIS
    Translate Glasswave's UI strings (and foobar2000's menu items) with DeepSeek.

.DESCRIPTION
    Collects the English strings to translate from
      - <profile>\glasswave\lang\strings-en.txt, written by Organize > "Export strings for
        translation..." (every string the UI has shown, foobar2000's whole main menu and
        the context menu of the selected songs), and
      - the Tr(L"...") / TrC(L"...") literals and menu / column labels in src\,
    sends the ones missing from lang\<Lang>.json to DeepSeek's chat API in batches, and
    merges the answers into lang\<Lang>.json (kept in the repo; run-dev.ps1 deploys it).
    Existing translations are kept, so hand corrections survive re-runs.

    The API key comes from the DEEPSEEK_API_KEY environment variable or, if unset, from
    -KeyFile (default .local\refs\deepseek.txt, never committed); it is never printed or
    written anywhere. The model defaults to the account's "flash" model if /models lists one.
    All strings go to the model in one request so it sees the whole UI as context; only if
    the list is very long is it split, each part still sent with the full list as context.

.PARAMETER DryRun
    List what would be translated without calling the API.
#>
[CmdletBinding()]
param(
    [string]$ProfileDir = (Join-Path $PSScriptRoot '..\.local\fb2k-dev\profile'),
    [string]$Lang = 'zh-CN',
    [string]$Model,
    [string]$KeyFile = (Join-Path $PSScriptRoot '..\.local\refs\deepseek.txt'),
    [int]$BatchSize = 600,
    [switch]$DryRun
)
$ErrorActionPreference = 'Stop'
$root = Resolve-Path (Join-Path $PSScriptRoot '..')
$langDir = Join-Path $root 'lang'
$target = Join-Path $langDir "$Lang.json"
New-Item -ItemType Directory -Force -Path $langDir | Out-Null

# --- Strings to translate -------------------------------------------------------------
$keys = [System.Collections.Generic.SortedSet[string]]::new([StringComparer]::Ordinal)
$export = Join-Path $ProfileDir 'glasswave\lang\strings-en.txt'
if (Test-Path -LiteralPath $export) {
    Get-Content -LiteralPath $export -Encoding utf8 | Where-Object { $_.Trim() } | ForEach-Object { [void]$keys.Add($_) }
} else {
    Write-Warning "No $export yet: run Organize > `"Export strings for translation...`" in Glasswave first (only source literals are used now)."
}
$patterns = @(
    'Tr[C]?\(L"((?:[^"\\]|\\.)+)"\)',           # Tr(L"...") / TrC(L"...")
    '\{L"((?:[^"\\]|\\.)+)",\s*(?:\[|go\(|\d|nullptr|\{)',  # {L"label", [lambda] / go(view) / width / submenu
    '\{"((?:[^"\\]|\\.)+)",\s*(?:"|nullptr)',     # smart playlist presets {"name", "query"
    'mainmenu_groups::\w+, L"((?:[^"\\]|\\.)+)"',  # foobar2000 main menu roots
    'L"((?:[^"\\]|\\.)+)"\s*:\s*L"((?:[^"\\]|\\.)+)"',  # ternary labels (first)
    ':\s*L"((?:[^"\\]|\\.)+)"\);',                 # ternary labels (last)
    'push_back\(\{L"((?:[^"\\]|\\.)+)"',         # menu / crumb / nav items
    'CommandButton\(dc, [^,]+, [^,]+, L"((?:[^"\\]|\\.)+)"',  # command bar labels
    'fit\(L"((?:[^"\\]|\\.)+)"'                 # command bar widths
)
Get-ChildItem (Join-Path $root 'src\foo_ui_glasswave') -Recurse -Include *.cpp, *.h | ForEach-Object {
    $text = Get-Content -LiteralPath $_.FullName -Raw
    foreach ($p in $patterns) {
        foreach ($m in [regex]::Matches($text, $p)) {
            $s = $m.Groups[1].Value -replace '\\n', "`n" -replace '\\"', '"' -replace '\\\\', '\'
            $s = [regex]::Replace($s, '\\u([0-9A-Fa-f]{4})', { param($x) [char][Convert]::ToInt32($x.Groups[1].Value, 16) })
            if ($s -match '[A-Za-z]' -and $s -notmatch '^[#%\[$]' -and $s.Length -lt 200) {
                # menu keys are stored without "&" accelerators
                [void]$keys.Add(($s -replace '&&', "`u{1}" -replace '&', '' -replace "`u{1}", '&'))
            }
        }
    }
}

$existing = [ordered]@{}
if (Test-Path -LiteralPath $target) {
    $json = Get-Content -LiteralPath $target -Raw -Encoding utf8 | ConvertFrom-Json -AsHashtable
    foreach ($k in ($json.Keys | Sort-Object)) { $existing[$k] = $json[$k] }
}
$todo = @($keys | Where-Object { -not $existing.Contains($_) -or -not $existing[$_] })
Write-Host "$($keys.Count) strings, $($existing.Keys.Count) translated, $($todo.Count) to translate"
if ($DryRun -or $todo.Count -eq 0) { $todo | Select-Object -First 50; return }

$apiKey = $env:DEEPSEEK_API_KEY
if (-not $apiKey -and (Test-Path -LiteralPath $KeyFile)) { $apiKey = (Get-Content -LiteralPath $KeyFile -Raw).Trim() }
if (-not $apiKey) { throw 'No DeepSeek key: set DEEPSEEK_API_KEY or pass -KeyFile.' }
$headers = @{ Authorization = "Bearer $apiKey" }
if (-not $Model) {
    $models = (Invoke-RestMethod -Uri 'https://api.deepseek.com/models' -Headers $headers -TimeoutSec 60).data.id
    $Model = ($models | Where-Object { $_ -match 'flash' } | Select-Object -First 1)
    if (-not $Model) { $Model = 'deepseek-chat' }
    Write-Host "Models: $($models -join ', ') -> using $Model"
}

# --- Translate ------------------------------------------------------------------------
$system = @'
You translate the user interface of a music player into Simplified Chinese. The player is
"Glasswave", a Windows 7 / Windows Media Player 12 look-alike built on foobar2000; the
strings are its own labels plus foobar2000's menu commands.

Rules:
- Use the terminology of the Chinese Windows 7 and Windows Media Player 12 (zh-CN): Library
  媒体库, Playlists 播放列表, Music 音乐, Artist 艺术家, Album 唱片集, Album artist 唱片集艺术家,
  Genre 流派, Year 年份, All music 所有音乐, Now Playing 正在播放, Organize 组织,
  Create playlist 创建播放列表, Create auto playlist 创建自动播放列表, Rating 分级, Length 长度,
  Title 标题, Contributing artist 参与创作的艺术家, Composer 作曲者, Size 大小, Search 搜索,
  Play 播放, Pause 暂停, Stop 停止, Previous 上一个, Next 下一个, Shuffle 无序播放, Repeat 重复,
  Mute 静音, Volume 音量, Properties 属性, Preferences 首选项, Options 选项, File 文件,
  Edit 编辑, View 查看, Help 帮助, Remove 删除, Rename 重命名.
- Player features: Favorites 喜爱, Smart playlists 智能播放列表, Play next 下一首播放,
  Add to queue 添加到播放队列, Recently Played 最近播放, Most Played 最常播放,
  Never Played 从未播放, Recently Added 最近添加, Rated Songs 已分级的歌曲,
  Output device 输出设备, Expanded tile 展开的平铺, Details 详细信息, Icon 图标.
- Keep product and technical names as they are: foobar2000, Glasswave, ReplayGain,
  DSP, ASIO, WASAPI, FLAC, MP3, CUE, Unicode, URL, ID3, APEv2, etc.
- Keep placeholders and formatting exactly: %s %d %1 {0} \t, a trailing "...", leading or
  trailing spaces. Do not add "&" accelerators.
- Short, natural UI wording; no explanations.

Answer with a JSON object mapping every input string exactly to its translation.
'@
$uri = 'https://api.deepseek.com/chat/completions'
$context = ($keys | ConvertTo-Json -Compress -AsArray)
for ($i = 0; $i -lt $todo.Count; $i += $BatchSize) {
    $batch = $todo[$i..([Math]::Min($i + $BatchSize, $todo.Count) - 1)]
    $body = @{
        model           = $Model
        temperature     = 0.2
        max_tokens      = 8192
        response_format = @{ type = 'json_object' }
        messages        = @(
            @{ role = 'system'; content = $system },
            @{ role = 'user'; content = "All strings of the UI, for context:`n$context`n`nTranslate these (answer with a JSON object mapping each of them):`n" + ($batch | ConvertTo-Json -Compress -AsArray) }
        )
    } | ConvertTo-Json -Depth 6
    $attempt = 0
    while ($true) {
        try {
            $resp = Invoke-RestMethod -Method Post -Uri $uri -Headers $headers -ContentType 'application/json; charset=utf-8' `
                -Body ([Text.Encoding]::UTF8.GetBytes($body)) -TimeoutSec 180
            break
        } catch {
            if (++$attempt -ge 3) { throw }
            Write-Warning "Request failed ($($_.Exception.Message)), retrying"
            Start-Sleep -Seconds (5 * $attempt)
        }
    }
    $answer = $resp.choices[0].message.content | ConvertFrom-Json -AsHashtable
    $got = 0
    foreach ($k in $batch) {
        if ($answer.ContainsKey($k) -and $answer[$k]) { $existing[$k] = [string]$answer[$k]; ++$got }
    }
    Write-Host ("batch {0}/{1}: {2} of {3} translated" -f ([int]($i / $BatchSize) + 1), [Math]::Ceiling($todo.Count / $BatchSize), $got, $batch.Count)
    # save after every batch so an interrupted run keeps its work
    $sorted = [ordered]@{}
    foreach ($k in ($existing.Keys | Sort-Object -CaseSensitive)) { $sorted[$k] = $existing[$k] }
    [IO.File]::WriteAllText($target, ($sorted | ConvertTo-Json -Depth 2), [Text.UTF8Encoding]::new($false))
}
Write-Host "Wrote $target. Run scripts/run-dev.ps1 to deploy it (Organize > Language picks the language)."
