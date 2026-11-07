# PluginSDK/imgui — the plugin SDK's pinned Dear ImGui

Dear ImGui **1.92.8** (`IMGUI_VERSION_NUM` 19280), the core files only, byte-identical to
the app's own copy at the time the plugin ABI v1 was cut (2026-11-07), including the project
`imconfig.h` (math operators, no demo windows, no debug tools, `IM_ASSERT` compiled out).
No platform/renderer backend is shipped: plugins never talk to a window or a GPU.

## Why it is pinned

A plugin and `POE1FixerPluginHost.exe` each compile THIS copy. The host owns the ImGui
contexts and hands one to the plugin before every `on_overlay` / `on_panel` call
(`P1_BindImGui`); the plugin's ImGui code then reads and writes that context directly. That
only works while both sides agree byte-for-byte on the layout of `ImGuiContext`, `ImGuiIO`,
`ImGuiStyle`, `ImDrawVert`, ... — the host refuses a plugin whose `imgui_version_num` or
`imgui_layout[]` (see `p1_abi.h`) differ from its own.

The app (`POE1Fixer.exe`) never runs plugin ImGui code: it receives finished draw data over
shared memory in a version-independent wire format (`P1DrawWire.h`, `P1WireKeys.h`). So the
app's own ImGui can be upgraded freely, and **an app ImGui upgrade must not touch this folder**.

## Upgrading

Upgrade this copy only together with a P1 ABI review: every plugin must be rebuilt against
the new copy, and the host and the SDK change in the same release. Re-check that
`WireVert` still equals `ImDrawVert`, that `ImDrawIdx` is still 16-bit, and that the
`P1WireImGui.h` key/cursor name tables still compile.
