# Research: reusing foobar2000 components

| Need | Plan | Notes |
| --- | --- | --- |
| Decoding, tags, art, CUE | foobar2000 core (`foo_input_std` etc.) | nothing to build |
| Output WASAPI shared/exclusive | core (2.x has WASAPI built in) | settings via Preferences |
| ASIO | foo_out_asio (official) | user installs as needed |
| DSP | core + DSP components | Preferences > DSP Manager |
| Library | `library_manager`, `metadb`, search filters | WMP-style views drawn by us |
| Playback statistics | foo_playcount | ratings/play counts for WMP columns |
| Lyrics | ESLyric (`foo_uie_eslyric`) as hosted `ui_element` | needs ui_element host (M2) |
| Visualizations | own WMP-style renderer on `visualisation_manager`; optionally host foo_vis_milk2 | own renderer keeps the look |
| Album art | `album_art_manager_v3` | WMC-style grid drawn by us |

Hosting rule (ADR 0006): hosted panels are opaque child windows placed in content areas.
