# Win7 asset map (lookup index)

Lookup index for the user's Windows 7 resource files (ADR 0007: images are loaded at run time
from `<profile>\glasswave\`, never committed). This file only holds IDs and text descriptions.
Companion to [win7-assets.md](win7-assets.md), which documents the art already in use and how
it is placed.

Sources: `wmploc.dll` 12.0.7601.24382, `aero.msstyles`, `ehres.dll` (Win7 SP1). The preview
catalog is produced by `scripts/catalog-art.ps1` into `.local/catalog/` (contact sheets plus
`index.tsv`).

Conventions used below:

- "N states, vertical" means N equal frames stacked top to bottom; "horizontal" means left to
  right. Frame size = image size / N.
- wmploc ID prefixes: `1xxxx` = high-contrast (white glyphs on black), `2xxxx` = high-contrast
  (black glyphs on white), `34xxx`/`35xxx` = light grey glyph sets (dark backgrounds),
  `45xxx` = misc. The un-prefixed ID is the normal Aero art.
- Identification is visual unless the text says "verified" (pixel-compared or proven by theme
  data). Treat "candidate" entries as guesses until checked against a WMP 12 reference capture.
- Catalog caveat: the catalog renders 32-bpp `RT_BITMAP` (wmploc type 2) without its alpha
  channel (e.g. 355-357 show a grey background), and icon groups at their largest frame
  upscaled to 256 px. Read the resource itself for real pixels.

## Quick lookup

| UI need | Best candidate(s) | Layout / notes |
| --- | --- | --- |
| Back/forward orb groove (capsule recess) | wmploc 257 **5360** (5361 is byte-identical) | 56x29, 1 state, translucent peanut-shaped recess. Verified: 5350 drawn at offset (1, 1) sits exactly in it. Dark-skin variant: type 2 5230-5233 (59x35, orbs baked into a dark capsule, normal / hot / pressed / disabled). |
| Back/forward orbs | wmploc 257 5350 / 5352 / 5351 / 5353 | in use (see win7-assets.md). Explorer's own orb: aero 1098-1103 (`Navigation` parts 1/2). |
| Nav pane: Library | wmploc 14 **29527** (device with film + note) | candidate; icon groups 2955x have 16/20/24/32 px frames. |
| Nav pane: Playlists | 14 **29513** (page + play + orbit ring) | same image as file icon 612. |
| Nav pane: a playlist item | 14 **29522** (page + play); auto playlist **29572** (page + play + sparkle) | |
| Nav pane: Music | 14 **29511** (note) | |
| Nav pane: Artist | 14 **29514** (person + note) | contributing artist candidate: 29537 / 29546 (person). |
| Nav pane: Album | 14 **29510** (CD) | |
| Nav pane: Genre | 14 **29517** (three discs) | candidate. |
| Nav pane: Videos | 14 **29538** (film strip) | video folder 29544, video download 29540, stacked films 29543. |
| Nav pane: Pictures | 14 **29531** (framed landscape) | also 29534; picture folder 29533. |
| Nav pane: Recorded TV | 14 **29548** (flat TV) | series/multiple 29549 / 29553, TV folder 29552. |
| Nav pane: Other libraries | 14 **29578** (server + film + globe) | other media: 29556 (film + note), its folder 29557. |
| Extras: year | 14 **29530** (calendar) | |
| Extras: folder | 14 **29519** (folder + note) | |
| Extras: composer | 14 29515 / 29512 (disc + person) | uncertain which one WMP uses for composer. |
| Extras: rating / favourites | 14 **29526** (star) | no heart glyph exists in wmploc or ehres. |
| Extras: other | 29521 lock (parental rating?), 29532 page "1", 29573 recently added (clock-like), 29574 tag/keywords, 29560 audio CD, 29564 data/DVD disc, 29563 portable device, 29568 sync, 29524 radio waves, 29575 online store | candidates. |
| Large (icon view) versions of the above | wmploc 257 **297NN** = 192x192 PNG of icon group 295NN (29710 = 29510, ...) | verified by appearance for 29710-29775. |
| View options icons | 257 **331** strip slots 55-58 (16x16 at x = 880 / 896 / 912 / 928: details, expanded tile, icon, tile); icon groups **7700-7703** (details, icon, expanded tile, tile; 16/20/24/32) and **29656-29659**; grey 16x16 bitmaps type 2 **7600-7603** | |
| Split-button dropdown arrow | 257 **WW_SPLIT.PNG** 42x20 (3 states horizontal, 14x20 each: arrow only, hot plate, pressed plate); `_120` 51x25, `_144` 63x30; `_BW` / `_WB` high contrast | |
| Command bar button hot/pressed | 257 **WMP_TOOLBARBUTTON.PNG** 51x22 (3 states horizontal, 17x22: normal (empty), hot, pressed); background **COMMAND_BAR_BG.PNG** 20x32 (white -> #DCE6F4 -> #A0AFC3 bottom rule) | Explorer equivalent: aero `CommandModule` 891-898. |
| Star rating | 257 **STARS.PNG** 429x16 = 33 cells of 13x16 (see wmploc 257 table); single 14x14 stars 5524-5533 | |
| Breadcrumb separator arrow | 257 **5235 / 5236 / 5237** (9x9: right, left (RTL), down = open), **5225-5227** (11x11), **5255-5257** (13x13); overflow "«" 5228 (9x9), 5229 (11x11), 5258 (13x13) | three DPI sizes; colour change for hot/pressed is not in these bitmaps. Explorer's: aero 872 (`BB::Toolbar` glyph, 8 states). |
| Breadcrumb item hover/pressed | 257 **WMP_BREADCRUMB_MAINBUTTON.PNG** 64x22 (4 states horizontal, 16x22: normal (empty), hot, pressed, grey "other part hot"); **WMP_BREADCRUMB_SPLITBUTTON.PNG** 40x22 (4 states, 10x22, same order) | 9-grid sources. |
| Seek bar | 257 track **4159** (171x9), fill **3881** (171x9), secondary fill 2137 (lighter, likely buffered/downloaded), head gradient **3882** (16x9, dark blue -> light cyan going right), thumbs **4058** normal / **3884** hot / **3883** pressed (16x9) | candidate set; alternative thumbs 2169 / 2171 / 2170. Mini-mode: 446/447 (185x9), 448-450, head 456. |
| Blue gradient at progress head | 257 **3882** (and 456, 17x9) | |
| Volume tooltip bubble | aero **881** (`Tooltip` part 1 TTP_STANDARD, 9x22, 1 state, sizing margins 4,4,4,4) | balloon 882, stem 883. |
| Speaker hover/pressed plate | 257 **4723 / 4725 / 4724** (25x25: normal (empty) / hot / pressed) candidate; 5241 / 5242 / 5243 (31x35, plate with speaker baked in: normal / hot / pressed) | speaker glyphs 5244-5248 in use. |
| Menu art | aero `Menu` 972-986 (see aero table) | verified mapping through VARIANT data. |
| Library background | 257 **LIBRARY_BACKGROUND7.PNG** (800x600, light blue, diagonal streaks) best match; LIBRARY_BACKGROUND5 (similar, greyer) | 2-6 are alternates (3 and 4 carry "FPO" placeholder marks). LBG_TOP / LBG_BOTTOM 50x150 gradient strips. |
| List column header | 257 LISTVIEWHEADER.PNG (in use); aero `Header` part 1 **554** (6x288, 12 states vertical) / `ItemsView::Header` 575 | |
| List selection / hot row | aero **601-604** (`Explorer::ListView` part 1: 602 hot, 601 selected, 604 selected-not-focused, 603 hot-selected; 7x74, sizing 3,3,3,3); ItemsView variants 629-632; WMP tile selection 257 **5339 / 5340 / 5341** (225x78: selected-focused, selected, hot) | |
| Now-playing row marker | 257 **5346** (16x16 blue play arrow), 5347 (24x16 double arrow) | candidate. |
| WMP application icon | wmploc 14 **102** (16, 24, 32, 48, 64, 256) | WMP document icons: 466, 606, 954. |
| Taskbar thumbnail toolbar | wmploc type 2 **355** (16 px), **356** (20 px), **357** (24 px): 10 glyphs each, pairs of normal + disabled (alpha ~40 %): previous, play, pause, stop, next | verified 32-bpp with alpha. |
| Heart / favourite glyph | not found; nearest: star 14 29526 / 257 5530 | |
| Shuffle / repeat | 257 4174/4175 shuffle on/off, 4176/4177 repeat on/off (in use); no repeat-one variant found | |
| Scrollbar | aero `ScrollBar` 717-733 (see aero table) | |
| Tree expand/collapse glyph | aero 928 (`Explorer::TreeView` glyph) / 931 (hot glyph), 16x32, 2 states (closed, open) | |

## wmploc.dll type 257 (PNG)

All IDs covered by range; every sheet (00-14) was viewed.

| ID / range | Size | What it is | Use in Glasswave |
| --- | --- | --- | --- |
| 331 / 332 / 350 | 1024x16 | 64 toolbar icons of 16x16 each: normal / disabled (grey) / third set (identical to 331 visually, probably hot). Slots: 0 media file, 1 new playlist (folder + star), 3 info, 4 plus, 5 delete X, 6-8 arrows up/down/left, 9 rip disc, 10 no-entry, 11 info, 13 check, 14 plus, 16 layout, 17 no-entry, 18-20 arrows, 21 X, 22 refresh, 23 home, 25 add to now playing, 26 no-entry, 28 sync list, 29 cancel rip, 30 cancel sync, 33 blank page, 34 lock, 35-50 globe animation, 52 X, 53/54 arrows, 55-58 view options, 59 music, 60 video, 61 TV, 62 picture, 63 other media | view options (55-58); candidates for command icons |
| 431-433 | 154x22 | 7 small task icons per strip (burn, stop, rip, play, ...) in 3 sets | not zoomed |
| 436-439 | 142x39 | compact (mini mode) transport, 4 states | |
| 440 | 48x47 | orb glow ring (mini mode) | |
| 446 / 447 | 185x9 | mini-mode seek track / fill | candidate |
| 448 / 449 / 450 | 17x9 | mini seek thumbs (normal / hot / pressed) | |
| 451-453 | 38x39 | pause orbs (mini) normal / pressed / hot | |
| 455 / 456 | 17x9 | empty / seek head gradient | |
| 520-532 | 75x75, 200x200 | no-art placeholders (video, music note, DVD) small + large | |
| 544 | 236x236 | large faded music note | |
| 1703-1705 | 18x18 | EQ preset icons | |
| 1740 | 33x23 | orange glow frame | |
| 1741-1743, 1745 | 25x15 | red close button normal / hot / pressed, X glyph | |
| 1750 | 64x64 | clock icon | |
| 1770-1787, 2020-2024, 2030 | 32x15, 30x13 | dark LCD status chiclets (buffering frames, play, pause, stop, disc, HDCD) | |
| 2031 / 2032 | 170x129, 170x51 | dark gradient panels | |
| 2033-2037 | 155x25 | mini transport layouts | |
| 2043-2045 | 24x25 | pause orbs | |
| 2066-2072 | 15x25, 25x15 | mini-mode restore / switch buttons | |
| 2100-2114 | various | horizontal / vertical mini sliders, thumbs | |
| 2117-2119 | 25x24 | pause orb with glow ring (dark skin) | |
| 2137 | 171x9 | lighter seek fill (buffer?) | candidate |
| 2138, 5609 | | Dolby Digital / Dolby Digital Plus logos | |
| 2151 | 32x15 | status chiclet | |
| 2164 / 2165 | 558x68, 274x68 | transport capsule full / half | 2164 in use |
| 2168 | 80x23 | volume track (grey) | candidate |
| 2169-2171 | 16x9 | alternative seek thumbs normal / pressed / hot | |
| 2174 / 2175 | 170x25, 25x170 | dark mini bar horizontal / vertical | |
| 2200 / 2201 | 558x69, 274x68 | capsule variants | |
| 2564-2566 | 16x16 | shield badges (blue, dark, orange) | |
| 2958 / 2968 | 48, 128 | media server icon | |
| 3881 / 3882 | 171x9, 16x9 | seek fill / head gradient | candidate |
| 3883 / 3884 / 4058 | 16x9 | seek thumb pressed / hot / normal | candidate |
| 3885-3887 | 14x23 | round thumbs (volume style) | |
| 4004, 4059, 5343 | | empty images | |
| 4153-4156 | 182x51 | transport cluster | in use |
| 4159 | 171x9 | seek track | candidate |
| 4160 | 52x52 | orb glow ring | in use |
| 4161 | 200x200 | album drop shadow | |
| 4162-4180 | | transport glyphs (play, pause, next, prev, rew, ff, shuffle, repeat, DVD, stop) | in use |
| 4181-4184 | 24x20, 41x20 | caption glyphs minimize / maximize / restore / close | |
| 4185 | 40x25 | DVD glyph disabled | |
| 4190 / 4191 | 25x25 | switch-view glyph (grey / blue) | |
| 4200-4203 | 169x51 | cluster without stop plate | |
| 4205-4212 | 47x25 | white transport glyphs | |
| 4601-4603 | 25x25 | pop-out / switch to Now Playing glyphs | |
| 4616 | 102x44 | white rounded panel with groove | |
| 4619-4630 | 39x25 | split-button plates with dropdown arrow (normal arrow only, pressed, hot, ...; 4626-4630 dark-background set) | |
| 4723-4725 | 25x25 | square button plate normal (empty) / pressed / hot | speaker plate candidate |
| 4731-4741 | 55x25 | wider split-button plates, same scheme | |
| 5030-5032, 45032 | 7x31 | vertical slider / divider pieces | |
| 5225-5229, 5235-5237, 5255-5258 | 9-13 px | navy triangles right / left / down and "«", three DPI sizes | breadcrumb candidate |
| 5239 | 7x6 | white down arrow | |
| 5240 | 80x23 | volume fill variant | |
| 5241-5243 | 31x35 | speaker on plate normal / hot / pressed | |
| 5244-5248 | 25x25 | speaker glyphs | in use |
| 5264-5266 | 192x192 | large play arrow, CD, sync arrows | |
| 5270 / 5300 | 8x11 | tiny blue right arrow | |
| 5271 / 5272 | 25x25 | grey refresh / back glyphs | |
| 5280-5282 | 69x25 | button plate normal (empty) / pressed / hot | |
| 5290 | 142x145 | media (film + note) icon | |
| 5305 / 5310 / 5315 | small | blue layout icons | |
| 5306-5309 | 25x25 | stop-record, list, rip glyphs | |
| 5320-5338, 5344 | large | Now Playing backgrounds, gradients, reflections, shadows (5321 dark blue, 5334 colour swirl) | |
| 5339-5342 | 225x78 | tile selection: selected-focused / selected / hot / white card | list selection candidate |
| 5346 / 5347 | 16x16, 24x16 | blue play arrow / double arrow | playing-row marker candidate |
| 5350-5353 | 54x27 | back / forward orbs | in use |
| 5360 / 5361 | 56x29 | orb groove (identical) | groove |
| 5401 | 9x36 | light blue gradient | |
| 5444 | 20x8 | dark block | |
| 5504-5515, 5545-5547 | 96x96 | disc and device icons (CD, CD-R, CD-ROM, CD-RW, DVD-R, DVD-RAM, DVD-ROM, DVD-RW, DVD, media file, sync, player) | |
| 5520-5523, 5534 | 48x8 | capacity bar fills (empty, grey, red, teal, green) | |
| 5524-5533 | 14x14 | single stars: 5524 white full, 5525 white half, 5526 faint empty, 5527 faint half, 5528 white half, 5529 faint, 5530 gold full, 5531 gold outline, 5532 bright gold, 5533 empty | rating |
| 5535 / 5536 | 5x110, 5x130 | thin vertical lines | |
| 5540-5543 | 49x15 | small button normal / pressed / hot / disabled | |
| 5600-5626 | 256x15, 640x15 | status-area animation strips, 8 or 20 frames of 32x15 (HDCD, Dolby, ff, rew, connecting rings, buffering waves); 5600-5602 / 5608 empty | |
| 5650-5665, 5670, 5672 | 17x21, 10x21 | button plates and end caps normal / pressed / hot (Play / Burn / Sync tab pieces?) | candidate |
| 5678-5681 | | volume fill and thumbs | in use |
| 7777-7794 | 55x23, 39x23 | split-button plates (23 px tall set) | |
| 7800-7802 | 23x23 | square plates normal / pressed / hot | |
| 7805 | 51x51 | glow ring | |
| 11741 / 11745, 12164-45223 | | high-contrast and white-glyph variants of the above (see prefix rule) | |
| 29710-29775 | 192x192 | large nav-pane category icons (= icon groups 295NN) | |
| 45216 | 16x16 | empty | |
| 45217 / 45219 / 45221 / 45223 | 304x34 | 19-frame 16x16 activity animations: sync, burn, download, rip (with error / done end frames) | |
| ALBUM_0-6, ALBUMDROPSHADOW_200X200 | 200x200 | no-art covers, drop shadow | in use |
| BASKET_* | | list pane (basket) backgrounds and separators | candidate for list pane |
| BLANK_16X16 | 16x16 | empty | |
| CHECKBOXES | 64x16 | 4 checkboxes horizontal: unchecked, unchecked hot, checked, checked hot | |
| CHICKLETS / _BW / _WB | 512x16 | 32 status chiclets of 16x16 (info, error, media types) normal + framed | |
| COMMAND_BAR_BG / _BW / _WB | 20x32, 15x32 | command bar background | candidate |
| FOLDER, GENERIC_DRAG_*, JEWELCASE, PHOTO, VIDEO, NO_ART_DVD, SPROCKETS_256 | large | placeholder art | |
| LBG_TOP / LBG_BOTTOM (+ _TRANSPARENCY) | 50x150 | library background top / bottom gradient strips and alpha masks | |
| LIBRARY_BACKGROUND2-7 | 800x600-1600x1200 | library backgrounds (7 best match) | |
| LISL_BUTTONS / _HC | 300x16 | list pane buttons, 6 states horizontal (50x16) | |
| LISTVIEWHEADER | 6x92 | list header, 4 states | in use |
| LOADING | 360x20 | loading animation strip | |
| NP_GLYPH | 5x5 | tiny blue arrow | |
| PREVIEWEQ / PREVIEWSTOP | 50x50 | preview overlay | |
| PROGRESS | 144x16 | green progress fill + empty, 3 frames of 48 | |
| SERVICE* | | online store tiles | |
| SPLITTER_TOP / _BOTTOM | 3x20, 3x80 | pane splitter | |
| STARS | 429x16 | 33 cells of 13x16: 0-4 faint empty, 5-9 gold full, 10-14 gold outline, 15-19 bright gold (hover), 20-24 faint empty (2nd set), 25-29 white full (selected row?), 30-32 white partial / half | rating |
| SYNCDLGICONS | 32x16 | green plus normal / framed | |
| WMP_BREADCRUMB_MAINBUTTON / _SPLITBUTTON | 64x22, 40x22 | breadcrumb backgrounds | candidate |
| WMP_PUSHBUTTON | 345x21 | push button, 5 states horizontal (69x21): normal, hot, pressed, disabled, default | |
| WMP_TOOLBARBUTTON | 51x22 | command bar button | candidate |
| WW_CLEAR / WW_SEARCH (+ _120 / _144) | 78x20 ... | search box clear / search buttons, 3 states horizontal | |
| WW_SPLIT* | 42x20 ... | view options dropdown arrow, 3 states | candidate |

## wmploc.dll type 14 (icon groups)

All sheets (00-04) viewed. Frame sizes read from the group directories.

| ID / range | Sizes | What it is |
| --- | --- | --- |
| 102 | 16-256 | **Windows Media Player application icon** |
| 218 | 16-48 | PDA / device |
| 460-465 | legacy 4/8 bpp | sync list, doc list, globe + film (streaming), CD + note (464, 16-255) |
| 466, 606, 954 | 16-256 | WMP document |
| 467 / 468 | | DVD / blank disc |
| 470, 576-578, 8700-8703 | 32 | badges: no-entry, check, X, warning |
| 471-476 | | network, rip (472/473), plus, filter funnel, schedule |
| 503, 615 | | film strip |
| 550 | | pie chart (disk usage) |
| 575 | | clock + note |
| 579 | | gear |
| 600-621 | | radio, notes, magnifier (602, 16-256), disc + person, disc, media/playlist files (612 playlist, 613/616 media file), stacked discs, WMP tile, sync to device, disc + page, stop sign, info with cursor |
| 700-711 | | error-badged variants (disc, sync, rip, burn, media, WMP, note, globe, lock, globe-play), portable player, player sync |
| 718-720 | | sync device, green arrows, stop document |
| 730-739 | 16-256 | file type icons: generic, AVI, MP3, MPEG, WAV, WMA, WMV, M4V, M4A, MP4V |
| 814-817 | | license keys (download, backup, ok, warning) |
| 865 / 866 / 890 / 891 | | shield warning / error, info, warning |
| 1050, 1110, 1151, 1152, 1171, 1193, 1210 | | media, plug-in box, lock, keys, lock, speaker, monitor |
| 2956, 2957, 2969-2972, 23304, 29303 | | network / media server icons (2969, 23304: server + play = Play To; 2970: server + globe) |
| 7700-7703 | 16/20/24/32 | view options: details, icon, expanded tile, tile (colour) |
| 29510-29578 | 16/20/24/32 | nav-pane category icons (see Quick lookup); 29577 empty |
| 29600-29661 | mostly 16 (some 16-32) | command icons: 29601 help, 29602 new folder, 29603 rip, 29604/29612 info, 29605 save, 29606/29653 delete X, 29607 sync, 29608 dark "»" overflow, 29610 burn, 29611/29618/29627 stop, 29614 checklist, 29617 layout, 29626 new auto playlist, 29629 sync list, 29630 cancel burn, 29631 cancel sync, 29632 blank page, 29635 globe + note (store, 30 px), 29636 lock, 29637-29651 globe animation, 29654/29655 arrows, 29656-29659 view options, 29660 Play To device, 29661 white "»"; 29600, 29609, 29613, 29615, 29616, 29619-29625, 29628, 29633, 29634, 29652 render empty |

## wmploc.dll type 2 (bitmaps)

All sheets (00-08) viewed. Many bitmaps are magenta-keyed (RGB) or 8-bpp; 32-bpp ones carry
alpha that the catalog drops. Flat-colour images (magenta / green / red / cyan blocks) are
skin hit-test colour maps for the art of the same size.

| ID / range | Size | What it is |
| --- | --- | --- |
| 101, 105, 213, 216-218, 328, 333/334 | small | misc: orange play icon, cursor X, up/down arrows, return-to-library arrows (82x68 x3), SRS logo |
| 339 / 348 / 349 | 696x14, 580x14 | WMP 11 rating star strips (magenta key) |
| 351 | 154x22 | colour map |
| 352 | 1024x16 | 24-bpp, all white (mask) |
| **355 / 356 / 357** | 160x16, 200x20, 240x24 | **taskbar thumbnail glyphs**, 10 x (16/20/24 px): prev, prev disabled, play, play dis., pause, pause dis., stop, stop dis., next, next dis. (32-bpp alpha) |
| 365-398 | 22x23, 29x29, 21x27, 28x15 | small transport buttons on plates, 4 states each (normal, hot, pressed, disabled): volume, play, stop, prev, rew, ff, next |
| 400-430 | | slider lines, tracks, thumbs, pause orbs (older compact controls) |
| 435, 454, 490, 457 | | colour maps / black block |
| 491-494 | 36x17 | dark prev/next pairs (WMP 11 deskband) |
| 506 / 509 / 512 | 18x18 | EQ preset icons |
| 522-524, 556-574, 1000/1001 | 14-20 px | small deskband / dialog icons (save, up/down arrows, play) |
| 1010 | 208x16 | 13 WMP 11-style 16 px library icons |
| 1011-1013 | 96x16, 96x12 | dropdown arrows, checkboxes, +/- boxes, warning, no-entry |
| 1014 / 1015 | 22x22 | rip / sync orbs |
| 1230, 1400 | | WMP 11 mini transport bar, WMP tile logo |
| 1680-1735 | 16-18 px | battery-like icons, splitter lines (1706-1709), back arrows, list options, DVD, switch-to, page arrows |
| 1792-1868, 2541-2579 | | WMP 11 mini player / deskband art (round play/pause, blue strips), 1800 / 1845 / 1860 WMP wordmark logo (138x118) |
| 2034, 2042, 2065, 2069, 2106, 2116 | | colour maps |
| 2061, 3961 | 121x25 | dark button group |
| 2080, 3960, 3995, 3997, 5078, 5238 | | lines / dividers |
| 2120-2127 | 13x13 | radio buttons and checkboxes |
| 2136-2158, 2167, 2220-2222, 2243/2244 | | black/white masks and long strips |
| 2223-2249 | | older transport (volume, pause orbs on plates, 72x27 / 100x27 mini transports, thumbs, slider tracks) |
| 3952-3954 | 41x69 | dark tab pieces |
| 3999, 4148, 4157, 4204, 4585, 4625, 4726, 4736, 5013, 5033, 5064, 5234, 5270, 5354, 5544, 7776, 7789, 1826, 2230, 2235 | | colour maps (5354 = orbs 5350, 4157 = cluster, 4585 = speaker plate 5241, ...) |
| 4008-4011, 14008, 24008 | 95x20 | caption button group (dark skin), HC |
| 4012 / 4013 | 25x25 | visualisation icons |
| 4617 / 4618 | 14x10 | small dark buttons |
| 5010-5025, 45010-45014, 5060-5101 | 66x36, 19x36, 31x36 | dark / blue glass button plates (full-screen controls) |
| 5053-5098, 5224, 5516, 5630/5631 | | white masks |
| 5230-5233 | 59x35 | back/forward orbs in dark capsule: normal / hot / pressed / disabled |
| 5402 | 500x60 | light gradient |
| 5632-5635 | 48x8, 46x6 | capacity bar fills |
| 7600-7603 | 16x16 | grey view-option icons: details, icon, expanded tile, tile |
| 14051 ... 32165 | | high-contrast / dark variants |
| IDB_NOWPLAYINGVISUALS_BUTTON_* | 19x18 | visualisation menu button up / hover / down / disabled |
| IDB_OLDSTRBMP_* | | AMG logo, DVD menu map, WindowsMedia.com logo |
| MINI_BTNGROUP_* | 43x16 | WMP 11 mini-mode prev/next group (up, hover, down, disabled, colour map) |

## aero.msstyles IMAGE

The 600 IMAGE ids (508-1108) were mapped to class / part / state from the theme data; only the
classes marked "viewed" below were also checked visually.

How the mapping works (verified): `VARIANT/NORMAL` is a list of records with a 32-byte header
of eight int32 `(property, type, class, part, state, flag, reserved, size)`. When `flag` is
non-zero the record has no payload and `flag` is the value; otherwise `size` bytes follow,
padded to 8. Class names are the non-empty UTF-16 strings of the `CMAP` resource, indexed in
order (index 202 = `DWMWindow`, matching the existing atlas data). Image properties are short
records whose `flag` is the IMAGE resource id: 3001 `TMT_IMAGEFILE`, 3002-3005
`TMT_IMAGEFILE1-4` (per-DPI: 96 / 120 / 144 / 192), 3008 `TMT_GLYPHIMAGEFILE`. Check:
`Button` part 3 (checkbox) -> 508 13x260 = 20 states of 13x13. `TMT_IMAGELAYOUT` (4011) is 0
(vertical stack) for all but three parts. A full dump (class, part, state, images, image
count, sizing margins) can be regenerated into `.local/catalog/scratch/aero_images.tsv`.

| Class (part) | IDs | Size, states | Notes |
| --- | --- | --- | --- |
| Menu 7 BARBACKGROUND | 972 | 3x50, 2 (active, inactive) | viewed |
| Menu 8 BARITEM | 973 | 9x114, 6 (normal, hot, pushed, disabled, disabled hot, disabled pushed) | sizing 4,4,4,4 |
| Menu 9 POPUPBACKGROUND | 974 | 1x1 | |
| Menu 10 POPUPBORDERS | 975 | 3x3 | sizing 1,1,1,1 |
| Menu 11 POPUPCHECK | 976 (96 dpi), 977 (120) | 16x64, 4 (check, check disabled, bullet, bullet disabled) | |
| Menu 12 POPUPCHECKBACKGROUND | 978 | 7x21, 3 | sizing 3,3,3,3 |
| Menu 13 POPUPGUTTER | 979 | 3x1 | |
| Menu 14 POPUPITEM | 980 | 7x84, 4 (normal, hot, disabled, disabled hot) | sizing 3,3,3,3 |
| Menu 15 POPUPSEPARATOR | 981 | 1x6 | |
| Menu 16 POPUPSUBMENU | 982 | 9x18, 2 (normal, disabled) | |
| Menu 17-20 SYSTEM close / max / min / restore | 983-986 | 11x20, 2 | |
| Tooltip 1 STANDARD | 881 | 9x22, 1 | sizing 4,4,4,4; viewed |
| Tooltip 3 BALLOON | 882 | 9x22 | |
| Tooltip 5 CLOSE | 878 / 879 / 880 | 17x48 (96 dpi), 3 | |
| Tooltip 6 BALLOONSTEM | 883 / 884 / 885 | 21x126, 6 | |
| Tooltip 7 WRENCH | 886 / 887 / 888 | 17x48, 3 | |
| ScrollBar 1 ARROWBTN | 717 (base), 718 / 719 / 720 (96/120/144) | 13x260 at 96 dpi, 20 (up, down, left, right x normal / hot / pressed / disabled, then hover set) | viewed |
| ScrollBar 2 / 3 THUMB horz / vert | 732 / 733 | 20x85 / 17x110, 5 (normal, hot, pressed, disabled, hover) | |
| ScrollBar 4-7 TRACK | 727 (horz), 728 (vert) | 1x40 / 14x5, 5 | |
| ScrollBar 8 / 9 GRIPPER | 721-723 / 724-726 | 5 | |
| ScrollBar 10 SIZEBOX | 729-731 | 16x128, 8 | also Status 3 |
| Toolbar 1/2 BUTTON / DROPDOWN | 849 | 7x208, 8 (normal, hot, pressed, disabled, checked, hot checked, near hot, other side hot) | grey glass plates; viewed |
| Toolbar 3 / 4 SPLITBUTTON / DROPDOWN | 852 / 853, glyph 854-856 | 6x208 / 5x208, 8 | |
| Toolbar 5 / 6 SEPARATOR | 850 / 851 | | |
| Media::Toolbar 4 | 853, glyph 857 | 7x32 glyph | WMP / Media class |
| Media::Rebar 6 BACKGROUND | 683 | 3x36 dark gradient | viewed |
| BreadcrumbBar 1 | 866, glyph 867 ("«" overflow, 10x144, 8) | 6x168, 8 | viewed |
| BB::Toolbar 1 / 3 / 4 | 870 / 871 / 866, glyph 872-874 (right arrow -> down arrow, 10x72, 8) | 21 px frames, 8 | Explorer breadcrumb button and separator arrow |
| Inactive / Max breadcrumb variants | 868 (glyph), 866 | | |
| Address::ComboBox 1 | 875, glyph 876 | 3 states | |
| SearchButton::Toolbar | 870, 871, 866, glyph 877 | 8 | |
| Header 1 HEADERITEM | 554 | 6x288, 12 (normal, hot, pressed, sorted x3, icon x6) | viewed |
| Header 0 / 4 / 5 / 6 / 7 | 574 / 570-573 / 555-559 / 560-564 / 565-569 | background, sort arrow (2), dropdown, dropdown filter, overflow | |
| ItemsView::Header | 575, 595, 576-594 | same layout | |
| ListView 6 (group header) | 601-604, 620 | 7x74 | |
| ListView 7 / 8 / 9 / 10 | 621 / 622-624 / 625-627 / 628 | group line, expand / collapse buttons (3), column detail | viewed |
| Explorer::ListView 1 | 602 hot, 601 selected, 604 selected not focused, 603 hot selected | 7x74, sizing 3,3,3,3 | viewed |
| ItemsView::ListView 1 / 6 | 630 hot, 629 selected, 631, 632 | 7x74 | |
| ItemsView | 640, 641, 642-644, 645, 646 | | |
| Progress 1-12 | 648-667 | bar 648, chunk 650, fill 656 (normal) / 657 (error) / 658 (paused) / 659 (partial), move overlay 664, pulse overlay 666, vertical and transparent variants | viewed (648, 650, 656, 664, 666) |
| Indeterminate::Progress | 668-670 | | |
| TrackBar 1 / 2 TRACK | 924 | 3x3 | |
| TrackBar 3-8 THUMB variants | 910 (6x95), 911-913 bottom, 920-922 top, 923 vert, 914-916 left, 917-919 right | 5 (normal, hot, pressed, focused, disabled) | viewed |
| Navigation 1 / 2 / 3 | 1098-1100 back, 1101-1103 forward, 1104-1106 menu button | 30x120, 4 (normal, hot, pressed, disabled) | Explorer orbs; viewed |
| CommandModule 1-12 | 891-900 | background 3x31, button 7x104 (4), split 6x130 (5), split dropdown 5x130 (5), arrow glyph 13x10, chevron 16x16, 13x20 (2), 898 button (4), 899 / 900 large backgrounds | viewed (891-898) |
| Explorer::TreeView 2 / 4 | 928-930 / 931-933 | 16x32, 2 (closed, open) | |
| TreeView 2 | 925-927 | 9x18, 2 | |
| Button 1-7 | 508-519 | push 516 (13x138, 6), radio 517-519 (8), checkbox 508-510 (20), group box 515, command link 511 / glyph 512-514 | |
| Combobox, Edit, DatePicker, DropListControl, Listbox, Spin | 520-532, 611-617, 712-716, 734-749 | standard controls | not viewed |
| Window 1-31 | 934-970 | classic (non-DWM) caption, frame, buttons | not viewed |
| AltTab, ExplorerBar, MonthCal, Rebar*, ControlPanel, AeroWizard, TaskDialog | 533-553, 596-613, 671-711 | | not viewed |
| StartPanel, StartPanelPriv, TaskBand*, TaskBar*, TrayNotify*, ShowDesktop | 750-848, 989-1075, 1107, 1108 | Start menu and taskbar | not viewed |
| Tab, PreviewPane, InfoBar, SearchBox, AddressBand, Flyout, TextGlow, DragDrop, Placesbar, ProperTree, ExplorerMenu | 818-830, 858-865, 901-909, 987-995, 1076-1097 | | not viewed |

### aero IMAGE visual notes (all 13 contact sheets viewed)

Alpha is shown on a checkerboard here, so shapes are readable. DPI sets repeat the same art at
about 100 / 125 / 150 % (e.g. 508 / 509 / 510 = 13x260 / 16x320 / 20x400; 1098 / 1099 / 1100 =
30x120 / 38x152 / 45x180). Mapping check: nothing looked wrong; two corrections below.

- **Correction:** ScrollBar ARROWBTN 717 is 17x340 (a fourth, larger set, probably the 175 / 200 %
  size), not the base; 718 = 13x260 (100 %), 719 = 16x320 (125 %), 720 = 20x400 (150 %).
- Button family (508-532, 611-619 area): 508-510 checkbox sets (blue boxes, check, mixed, red X
  states), 511 command-link strip, 512-514 green and grey command-link arrows, 516 push button
  (rounded, light blue hot, blue pressed), 517-519 radio buttons (light blue discs, dark bullet),
  520 / 524 / 525 / 527 slim tracks and thumbs (blue glass), 521-523 black dropdown arrows
  (8 states, three sizes), 526 / 529-532 / 538-541 tiny tree-style plus boxes (white plus on blue /
  grey), 533 focus-ring frame (dark outline, pale blue fill), 534-536 7x74 tab or header strips
  (grey / pale blue glass), 543 red-X close buttons on a plate (3 states: normal, hot, pressed
  dark), 544 small pin / minimize glyphs, 545-552 round "»" / "«" / chevron-up / chevron-down
  overflow buttons on a grey plate (3 states: glyph only, plate, plate pressed).
- 556-559, 577-580: black / steel dropdown chevrons (double triangles). 561-564, 582-585: check
  marks (3 states, black / steel). 566-569, 587-590: "»" glyphs in 4 sizes. 570-573, 591-594:
  light-blue diamond (sort arrow background) in four sizes; **these are the header sort arrow
  glyphs** (diamond with a split line), see Header table.
- 601-604 / 629-632 / 620: 7x74 selection capsules: pale blue (selected), grey (hot / unfocused),
  lighter blue (hot selected); rounded outer line. Use as the 9-grid for list selection.
- 605-610: tiny black / blue triangles right (605-607) and left (608-610), 3 sizes, 5 states
  (the blue one is the hot state). 611-613: grid icon + blue dropdown arrow on grey plate (view
  button, 3 sizes). 614-618: grey vertical slider capsules (614 thumb, 618 11x105 thumb states).
- 622-627: round blue-navy expand / collapse chevron buttons (down 622-624, up 625-627), 3 sizes
  x 3 states (glyph, plate, hot plate); 634-639 / 642-644: blue triangle expanders (outline grey
  = normal, cyan = hot) for the group header and tree views, 640 / 641 amber dots (focus
  indicators), 645 7x7 selection corner.
- Progress (648-670): 648 / 649 / 652 / 668 grey glass bars (horizontal / vertical), 656 green,
  657 red, 658 yellow, 659 teal fills (51x14); 660-662 vertical green / red / yellow, 663 teal;
  664 / 666 soft grey glow overlays; 670 green glow; 671 vertical white highlight.
- 674 19x69 grey two-button plate (scroll button), 675-677 navy "»" glyphs (overflow), 680-683:
  rebar / toolbar background strips (683 is the dark grey -> black Media::Rebar gradient, 682
  blue).
- 693-710: round scroll-style arrow buttons (down 693-701, up 702-710), three sizes, each with
  states: grey (normal), light-blue (hot), solid blue (pressed). Candidate for Explorer-style
  overflow / expander buttons in the nav pane.
- 712-716 tiny grips; 717-733 scrollbar parts (721-726: grip lines, blue = hot, grey = normal);
  734-749: vertical / horizontal scrollbar thumbs and arrow pieces for the older (classic) form.
- 750-848: Start menu / taskbar art (753 / 754: orange sleep / lock / power buttons, 757-760 /
  773-774 black arrows, 783 / 784 / 813 / 814 white scroll plates, 803 grey pill, 831 / 832
  orange-glow taskbar button, 767-791 taskbar tray plates with grey three-state squares).
  Nothing here is useful for WMP.
- 849-857: toolbar plates (thin, mostly invisible strips: the plate is drawn from 7x208 with 8
  states, nearly transparent normal state); 854-856 black dropdown triangles in 8 states,
  857 white ones. 858-865: 1-px-wide status / SearchBox pieces. 867 / 868: tiny "«" overflow
  marks. 872-874: black breadcrumb arrows (right, then down for open), 875 address combobox
  button (blue glass), 876 blue arrow, 877 tiny black dropdown arrows (8 states).
- 878-880: close "X" tooltips (red X on plate, 3 sizes); 881 / 882: tooltip rounded white
  gradient bubbles; 883-885: balloon stems (white corner triangles, 6 states, 3 sizes); 886-888:
  wrench buttons.
- 891-898: command module. 893-894 are thin 6x130 / 5x130 split plates (almost transparent
  until hot), 895 dark navy dropdown arrow (13x10), 896 navy "»" chevron, 897 blue arrows (2
  states), 898 button plate; 899 / 900 are 830x55 / 500x55 gradients (CommandModule backgrounds,
  white to pale). 901: pale vertical strip.
- 905 3-state grey square (pinned plate). 910-923: trackbar thumbs, pale-blue arrow-shaped
  thumbs pointing left / right / up / down with 5 states (grey, blue, dark blue, cyan, white);
  these are directly usable as a WMP-like slider thumb only in the pointer shape, not as the WMP
  seek thumb. 925-927: tree plus / minus boxes (white plate, blue sign) at 3 sizes.
- 928-933: explorer tree glyph triangles (closed outline, open black filled; blue-cyan on hot).
- 934-1000: classic window parts: 934 / 935 caption button plates (blue gradient and its black
  mask), 936 / 966 orange-red close glow set (7 states), 937-950 / 957-963 caption glyphs (X,
  box, dash, restore, help "?") in 3 sizes, 952 / 960 / 964 caption button plates (blue glass,
  white outline), 965 white frame, 967 checker-X mask, 976 / 977 menu check and bullet (dark navy
  check, grey check, navy bullet, grey bullet), 978 check background (blue-grey), 982 submenu
  arrow, 983-986 system menu glyphs, 987 grey squares.
- 1000-1036: window / task-band tabs. 1014-1017 / 1024-1027: tall 60-px stacked taskbar
  buttons (grey glass, orange fill at the bottom = pinned-progress pulse), 1018-1021 coloured
  progress strips. 1033: soft rounded white glow (thumbnail frame). 1036-1039: teal / black
  tooltip pieces.
- 1040-1075: more caption glyphs, "»" white chevrons (1047-1053), pinned dots (1054-1059),
  1062-1075 grey rounded scroll buttons (up / down / left / right) with 3 sizes. 1078-1095:
  blue plus (1078-1080), blue arrow (1081-1083), blue tag (1084-1086), blue return arrow
  (1087-1089), yellow warning triangle (1090-1092), red no-entry (1093-1095); each in 3 sizes
  (19x24 / 24x30 / 29x36). These are Explorer "Details pane" badges (add tag, link, error).
- 1096 108x108 grey translucent square; 1098-1103 back / forward orbs (verified: blue orb with
  arrow, order normal / hot / pressed / disabled grey), 1104-1106 history dropdown (tiny blue
  arrow, grey plate, dark pressed), 1107 / 1108 ShowDesktop strips.

## ehres.dll

Type 10 (RCDATA, 2287) has descriptive names, so the groups below come from the names, not
from the images. 1298 of them are high-contrast copies (`HC1.*`, `HC2.*`).

| Name prefix (count) | What it is | Relevant items |
| --- | --- | --- |
| COMMON.* (79) | backgrounds (layers, shards, gradients), buttons (251x51 focus / no focus / disabled), scroll arrows (27x27, 4 states), stars, transport feedback, logos | **COMMON.STAR / STAREMPTY / STARHALF** 23x22 (+ .SHADOW): rating stars incl. half |
| SEEKBAR.* (25) | Media Center seek bar | SEEKBAR.PAST / FUTURE 14x24, POSITION.ACTIVE 28x28 / DORMANT 26x26 / SHADOW, PROGRESS.BACKGROUND, shuttle and time-code pieces |
| TWOFOOT.* (125) | windowed-mode controls: transport, volume, mute, caption buttons, coloured keys, battery, full-screen bars; states DISABLED / FOCUS / NOFOCUS / PRESSED | play / pause 42x43, prev / next / stop / mute 38x31 |
| OVERLAYS.* (23) | on-screen overlays | OVERLAYS.VOLUME.BAR.ON / OFF 150x10, VOLUME.MUTE |
| CONTEXTMENU.* (3), NOWPLAYING.* (13), INFOPANEL.* (7), INFOBAR.* (2) | context menu background and buttons, Now Playing art, info panel | |
| SELECTOR.* (7), BROWSE.* (58), DETAILS.* (39), PIVOT.* (8), SPINNER.* (13), AUTOSCROLL.* (12) | focus loop glow, gallery browse, details page, pivot arrows, spinners, scroll chevrons | |
| STARTMENU.* (85), SKITTLE.* (7), FIRSTRUN.* (76), GLOBALSETTINGS.* (27), OPTIONS.* (26) | Media Center start menu, quick links, setup | |
| AUDIO.* (29), VIDEO.* (50), PHOTO.* (25), MOVIE.* (13), TV.* (9), EPG.* (31), GUIDE.* (18), MYRADIO.* (15), RADIO.* (5), LISTMAKER.* (24), NOTIFICATIONS.* (31) | media-type pages | AUDIO.NOCOVER.* no-art covers |
| others (< 7 each) | OSK, TOAST, SLIDE, TENFOOT, EXTENDER, GRACEFUL, CHANGEREJECT, BUSY, ... | |

Type 14 (15 icon groups): 1 Media Center (green Windows orb), 2 Media Center document,
3 headphones, 4 TV, 5 glass panel, 6 film, 7 / 8 TV with red dots, 9 puzzle piece, 10 list
layout, `EHTRAY.ICON.*` download / recording / shutdown / warning, `EHTRAY.LOG.ICON`.

### ehres type 10: range notes (sheets 00-07, 36-47 viewed; 08-35 are `HC1.` / `HC2.` copies)

The catalog sorts names alphabetically, 48 images per sheet; sheet N covers the names listed.
Alpha is shown on a checkerboard. High-contrast sheets 08-35 (`HC1.*` yellow / black / grey
glyphs, `HC2.*` black / white) repeat the normal art in flat colours; sheet 09 was sampled and
matches (AUTOSCROLL chevrons, BROWSE.CMD icons in yellow outlines). No separate catalogue.

| Sheets | Names | What it looks like |
| --- | --- | --- |
| 00 | ALTERNATE.TWOFOOT.*, AMBIENT*, AUDIO.*, AUTOSCROLL.*, BOBSLED | Windowed top-bar piece, white glow, grey "+" orb; CD / disc icons, check mark, equaliser bars; **AUDIO.NOCOVER.1-7 no-art covers** (blue, green, red, tan, purple, yellow swirl backgrounds, small + large) and a note icon; **AUTOSCROLL** light-blue chevrons (down / left / right / up, normal / focus / disabled) |
| 01 | BROWSE.AUDIO*, BROWSE.CMD.* | Music note tiles, small player devices; **BROWSE.CMD.*: command icons (add "+", play triangle, queue, network, X, list, magnifier), each in 3 states: cyan glyph (dormant), glyph on a glass plate with blue bottom edge (focus), white (disabled)**; inserted-media discs (CD, BD, HD DVD, WMV) and card icons |
| 02 | BROWSE.NOIMAGE*, BROWSE.TITLE*, BUSY, CHANGEREJECT.*, COMMON.BACKGROUND.* | Grey-blue placeholder art; film tile (blue, play arrow); round ring spinner; **CHANGEREJECT eject triangle on a blue-grey rounded plate (focus / no focus)**; white-blue star; dark-blue backgrounds, soft blue light shards and glows (backgrounds for the Media Center look) |
| 03 | COMMON.BUTTON.*, COMMON.SCROLL.*, COMMON.STAR*, COMMON.TRANSPORTFEEDBACK.* | 251x51 wide button states (translucent glass bars), focus overlays, glow dots; **COMMON.SCROLL arrows 27x27 (glass square with white chevron: no focus / focus / pressed / disabled)**; **white stars: full, empty, half (23x22) + shadow set**; **round grey transport feedback orbs 53x53 (skip back / forward, rew, ff, pause, play, step) with white glyphs** |
| 04 | COMMON.TRANSPORTFEEDBACK (prev, stop), CONTEXTMENU.*, DETAILS.* | Blue context-menu background and button strips; **DETAILS.CMD.* command glyphs (plus, disc, no-entry, eject, undo, play, record, record series, X) in 3 states: cyan on transparent / white on a dark-navy square plate (focus) / white (disabled)**; DETAILS pager chevrons, thumbnail drop shadow |
| 05 | EDIT.*, EPG.*, EXTENDER.*, FIND.* | Text caret, light mirrored strips, **EPG cell backgrounds in genre colours** (teal, purple, olive, orange, green, navy, black) with focus variants; TV and Xbox 360 extender renders; info / error / warning icons; lock; search edit box |
| 06-07 | FIRSTRUN.*, GLOBALSETTINGS.* | Setup wizard: big translucent check boxes and radio buttons (large / small, glass bullet, white dot, selected state), **round dark-blue orbs with gear, arrow and Windows logo**, signal-strength bars (green / yellow / red), device illustrations, hero images; **GLOBALSETTINGS.TILE.* grey glass square / check / down-arrow tiles (up / focus / select / disabled)** |
| 36 | LISTMAKER.*, MORE, MOVIE.*, MYRADIO.* | **LISTMAKER.GALLERY: grey rounded-square checkboxes (empty / checked, focus / no focus)**; dark-blue list pieces with notched corners; globe, HD badges; **MOVIE.NOCOVER.1-7 no-art covers** (same colours as AUDIO); MYRADIO chevrons on glass plates (down, up), 3 states |
| 37 | MYRADIO.REMOVE, NOTIFICATIONS.*, NOWPLAYING.* | White X on a glass plate; **notification boxes: blue, red, orange gradient plates + pill glows**, icons (key, power, spinner, green Windows orb, camera, disc, memory card, warning, phone, check mark); dark navy gradient / shadow pieces |
| 38 | NOWPLAYING.*, OPTIONS.*, OSK.* | Now Playing shadows and gradients (blue), large grey play orb, faint note; A/V cable plugs (red / green / blue / yellow / white) and ghost line-art devices for calibration; lock; on-screen keyboard blue backgrounds |
| 39 | OVERLAYS.*, PHOTO.MOREINFO.* | Overlay panel backgrounds (blue gradients), **volume mute icon, overlay close X / check (ghosted), blue-white 3D arrows (down / left / right / up), zoom magnifiers +/-, rotate arrows, crop** (3 states each: dormant / focus / disabled), media-card and disc glyphs |
| 40 | PIVOT.*, RADIO.*, SEEKBAR.*, SELECTOR.* | **PIVOT chevrons (cyan = focus, light = dormant, white = disabled)**; FM radio tuner art (blue); **SEEKBAR: glow knob (active: white glow; dormant: faint ring; shadow: dark blob), white marker with notch, time-code arrows (rew / ff 1-4 chevrons, pause bars, play), shuttle pieces, empty dark strip**; **SELECTOR.FOCUSLOOP.* pale glow corners (top-left, bottom-left, top-right, glass) + dark square outline 32x32 (SELECTOR.PNG)**; UAC shields; Media Center logo |
| 41 | SKITTLE.*, SLIDE.*, SPEECH.*, SPINNER.*, STARTMENU.* (start) | Green Windows orb in several sizes, blurred / glowing variants, ring; slide-show "<" ">" chevrons and shadows; microphone; **SPINNER +/- glass squares (3 states)**; start-menu rainbow arcs, cyan sparkles, logo and wordmark pieces |
| 42-43 | STARTMENU.* (quick links, tiles) | Media Center start-menu content: blue backdrop, TV render, coloured tile grids (pictures, music, video, TV, radio, extender, search variants with magnifier overlay), disc / DVD box-art mini-collages, TV colour bars; icons (clock, sync, shield / power, check box); no use for WMP |
| 44 | TENFOOT.*, TV.* | **Ten-foot status dots (green / red / yellow / blue) on glass plates, list / guide / help glyphs**; check marks (white / cyan); glass tile with white-blue gradient; TV infobar pieces, down / up chevrons (cyan glow), rewind |
| 45 | TWOFOOT.CONTROL.* (A-R) | Not opened; by name: windowed-mode controls (backward, battery, close, forward, mute, pause, play, record, ...) |
| 46 | TWOFOOT.CONTROL.* (R-Z), TWOFOOT.FULLSCREEN.*, VIDEO.CONFLICT / DRM | **Windowed-mode transport / commands in up to 4 states each (disabled, focus = navy glyph on a glass plate with blue edge, no focus = bare dark-navy glyph, pressed)**; actually spans sheet 44-46: navy rew / ff "<<" ">>", battery (empty .. full, plug, warning), blue record dot, "d" logo, dropdown triangle, close X (orange plate when pressed), grey / blue glossy back orb, green dot, list glyphs, red record dot, restore / maximise (teal plate), help list, stop (navy square on white plate), yellow dot, teletext; full-screen top / bottom bars; many amber warning triangles (video conflict / DRM) |
| 47 | VIDEO.EPG.*, NOTIFICATIONS, RECORD, TELETEXT, ZOOMPANEL | EPG row backgrounds (blue / grey / dark), info "i" and red record bubbles, teletext coloured buttons (blue / green / red / yellow), silver bar and button bodies (video control strips), zoom panel frame |

Note: the TWOFOOT.CONTROL art described in the rows 45-46 was read from the sheets labelled
44 and 46 in the catalog (sheet 45 itself was not opened).

Things from ehres that match a Glasswave need (32-bpp PNG, so alpha is intact in the
resource):

- Focus / selection art: **SELECTOR.FOCUSLOOP.*** (glow corners 168x80 / 167x66 / 81x121,
  glass 178x114, hot-spot lines 48x4 / 4x48) and **COMMON.BUTTON.LEFT.FOCUS / RIGHT.FOCUS**
  (251x51 / 611x51 bright rounded bar). Reference for a Media Center focus ring.
- Menu / command strips: **CONTEXTMENU.BACKGROUND** 281x352 (blue panel), **CONTEXTMENU.BUTTON**
  251x50.
- Transport: **COMMON.TRANSPORTFEEDBACK.*** 53x53 (grey glossy round orbs: play, pause, ff, rew,
  skip, step); **TWOFOOT.CONTROL.*** 38x31 / 29x24 / 42x43 (windowed controls, 4 states).
- Seek bar: SEEKBAR.PAST / FUTURE 14x24, POSITION.ACTIVE 28x28 (white glow), DORMANT 26x26.
- Backgrounds: COMMON.BACKGROUND.* blue shards and glows, NOWPLAYING.* gradients, large
  `AUDIO.NOCOVER.*` swirl covers (200 / 400 px).
- Scroll arrows: COMMON.SCROLL.* 27x27 and AUTOSCROLL.* (light-blue glass chevrons).
- Stars: COMMON.STAR / STAREMPTY / STARHALF 23x22 (+ .SHADOW).

## Not yet catalogued

- **aero IMAGE**: all 13 sheets viewed (see "aero IMAGE visual notes"). Not checked: which
  three parts use horizontal layout; the standard-control and Start / taskbar mappings were
  only sanity-checked by appearance.
- **ehres type 10**: coarse range notes only; HC sheets 08-35 are not described individually,
  sheet 45 (TWOFOOT.CONTROL, middle part) was not opened, single images were not zoomed.
- **wmploc 257**: all sheets viewed; not zoomed: 431-433 strips (icon order), 2105-2111,
  BASKET_* details, LOADING, LISL_BUTTONS state names, 5650-5672 role.
- **wmploc 2**: all sheets (00-08) viewed; colour maps not paired with their art one by one.
- **wmploc 14**: all sheets viewed; nav-pane names are visual guesses, not checked against a
  WMP 12 capture. 29600, 29609, 29613, 29615, 29616, 29619-29625, 29628, 29633, 29634 and
  29652 render empty in the catalog (not investigated). Composer: 29512 (CD behind a large
  person) and 29515 (large CD, smaller person) are both disc + person; which one WMP uses for
  composer vs contributing artist is still unknown.
- Still no heart / favourite glyph and no repeat-one glyph (rechecked wmploc names and ehres
  names / sheets).
- wmploc types 256 (`RT_?` 256), 1/3/12 (cursors, icons), AVI, JPGTYPE, XML: not looked at.
- The `hd\wmploc` folder under the profile (the @2x artwork) was not catalogued.
