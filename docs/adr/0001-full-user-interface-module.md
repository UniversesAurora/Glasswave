# ADR 0001: Implement Glasswave as a foobar2000 user_interface module

Status: accepted (2026-11-07)

## Context

The UI must control the whole top-level window: non-client area, per-window glass,
shadow, caption buttons and composition target. foobar2000 offers three ways to change
its look: a full `user_interface` module (like Default UI / Columns UI), panels inside an
existing UI (DUI/CUI elements), or script panels (JScript/Spider Monkey Panel, JSplitter).

## Decision

Write a native `user_interface` service (`foo_ui_glasswave.dll`). Other components'
`ui_element` panels can still be hosted inside it later (ADR 0006).

## Consequences

- Full control of the window, input and rendering pipeline; no host UI constraints.
- We must provide what a UI module normally provides: main window, menus, shortcuts,
  status text, layout and persistence.
- Users can switch back to Default UI any time from foobar2000's UI module prompt.
- Script-panel approaches were rejected: no access to DWM/composition, GDI+ rendering,
  and the visual bar cannot be met.
