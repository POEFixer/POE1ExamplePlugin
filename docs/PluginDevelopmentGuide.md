# POE1Fixer plugin development guide

Plugin ABI **v1** (`P1_ABI_VERSION 1`, `P1_SNAPSHOT_VERSION 1`), Dear ImGui **1.92.8** (pinned in the SDK).
Russian version: [PluginDevelopmentGuide_ru.md](PluginDevelopmentGuide_ru.md).

The authoritative contract is the C headers in `PluginSDK\include\p1sdk\` (`p1_abi.h`,
`p1_snapshot.h`). `p1sdk.hpp` is a header-only C++ wrapper over them, and this guide describes
the wrapper. The reference plugin is `Plugins\ExamplePlugin\`. Start from a copy of it.

Contents:
1. [What a plugin is, and what you are guaranteed](#1-what-a-plugin-is-and-what-you-are-guaranteed)
2. [The sandbox](#2-the-sandbox)
3. [Quick start](#3-quick-start)
4. [plugin.json reference](#4-pluginjson-reference)
5. [Lifecycle and threads](#5-lifecycle-and-threads)
6. [The frame and the snapshot](#6-the-frame-and-the-snapshot)
7. [Drawing](#7-drawing)
8. [Settings and files](#8-settings-and-files)
9. [Input requests](#9-input-requests)
10. [Game memory reads](#10-game-memory-reads)
11. [Logging, toasts and the clipboard](#11-logging-toasts-and-the-clipboard)
12. [Debugging](#12-debugging)
13. [ABI and versioning rules](#13-abi-and-versioning-rules)
14. [FAQ and troubleshooting](#14-faq-and-troubleshooting)

---

## 1. What a plugin is, and what you are guaranteed

A plugin is an x64 DLL plus a `plugin.json` manifest in its own folder under
`<POE1Fixer folder>\Plugins\`. When the user enables it, POE1Fixer starts a separate process,
`POE1FixerPluginHost.exe`, for that plugin alone. The host loads your DLL and calls your code. The
app itself never loads plugin code, never calls it and never waits for it.

```
POE1Fixer.exe (the app)                         POE1FixerPluginHost.exe (one per plugin)
  game reader --> read-only snapshot  ------->    your OnEvent / OnTick
                  (shared memory)                 your OnOverlay -> ImGui draw data --+
  overlay window <-- validated draw data <------------------------------------------+
  Plugins tab    <-- your settings panel <------  your OnPanel   (mouse/keys forwarded to you)
  input thread   <-- input requests (gated) <---  p1::Input
```

What you get:
- **A read-only snapshot of the game** (entities, player, area, camera, inventory and more),
  copied into your process before each callback. See [section 6](#6-the-frame-and-the-snapshot).
- **Dear ImGui drawing** in two places: an overlay over the game window (click-through) and a
  settings panel inside the app's **Plugins** tab.
- **Services:** logging, toasts, the clipboard, textures, and with permission, read-only game
  memory reads and keyboard/mouse requests.

What is guaranteed:
- **A crash kills only your plugin.** That covers an access violation, stack overflow,
  `std::terminate`, `abort()` or a pure virtual call, on any of your threads. The app writes a
  crash dump, then restarts your plugin after a back-off of 2, 4, 8, 16 and then 30 s. After
  **4 crashes within 10 minutes** the plugin goes to **Faulted** and stays stopped until the user
  presses **Restart**, or disables and re-enables it. A clean run of 10 minutes resets the back-off
  to 2 s.
- **A hang kills only your plugin.** A callback that runs longer than **5 s** gets the process
  killed. The limit for each loading step is **15 s**. The process is also killed if the host's loop
  stops turning for **10 s**, or if start-up does not finish in **30 s**. The state detail then
  reads, for example, `hung in on_tick`, and the plugin restarts under the same back-off.
- **Your plugin cannot slow the app down.** Your process runs at below-normal priority, inside a
  Job object with a hard CPU cap of **25 % of the whole machine** and a memory cap you choose. It
  may also run only on the **highest-numbered quarter of the logical CPUs** (the same quarter for
  every plugin; on hybrid Intel processors these are the efficiency cores), so even threads you
  raise to the highest priority can never occupy the CPUs the app works on. The game snapshot is
  built once per game-reader tick by a below-normal thread and shared by all plugins. No plugin
  can change the reader's cadence or turn on any of the app's game reads. Each plugin gets at most
  about **30 ms per second** of the app's service thread (the thread that validates your frames
  and reads your requests); past that, your frames and requests wait for the next second. The
  app's render thread only copies draw data the app has already validated, within fixed limits per
  frame and across all plugins (section 7). A slow plugin only loses its own freshness: frames are
  "latest wins", and you always get the newest snapshot, never a backlog.
- **When the app exits, every plugin process ends with it**, even if the app crashes. The Job
  object is closed with kill-on-close.

The app starts plugins once the user is signed in. The Plugins tab is free and is not a paid
feature. It also works in the app's visual (WGC) mode, where the snapshot carries no game data
(`P1_SNAP_WGC_MODE`). At most **16 plugins** run at the same time.

## 2. The sandbox

Your process runs at **low integrity**, with a restricted token: every privilege but one is
removed, and Administrators is deny-only. It also runs inside a **Job object** that allows one
process. In practice this means:

| A plugin CANNOT | Because |
|---|---|
| open the game, the app or any other normal process, so it cannot inject code, write their memory or read their memory directly | they run at a higher integrity level; reading game memory goes through `p1::Memory` (section 10) |
| write files outside its config folder (`P1HostApi::config_dir`) | its own plugin folder and the app folder are read-only to it. The only other writable places are the few per-user locations Windows labels low-integrity, such as `%USERPROFILE%\AppData\LocalLow`; do not rely on them |
| send keyboard or mouse input to the game or the app (`SendInput`, `PostMessage`) | Windows UIPI blocks input into higher-integrity windows. Use `p1::Input` (section 9) |
| open another plugin's process, threads or token: read or write its memory, take its handles, suspend or kill it, run code in it, change its token | every plugin's process, primary thread and token, and every thread it starts, grant other processes of the same user only harmless rights: wait on them, read their security descriptor, query basic information (and query a token) |
| start processes (`CreateProcess`, `ShellExecute`, opening a URL in the browser) | the Job allows one process. `CreateProcess` fails with error 1816 (`ERROR_NOT_ENOUGH_QUOTA`) |
| use the clipboard directly | the Job's UI limits. Use `p1::SetClipboard` (section 11); text the user pastes into your panel arrives through the app |
| use more than its memory cap or more than 25 % of the machine's CPU, or run on more than its quarter of the logical CPUs | Job limits. Over the memory cap, allocations fail; a thread affinity outside the quarter is refused |
| change display settings or system parameters, log off the user, use global atoms, use window handles of processes outside its Job, or switch desktops | the Job's UI limits |

| A plugin CAN | Note |
|---|---|
| **read any file the Windows user can read** | low integrity stops writing up, not reading |
| **use the network** (sockets, HTTP) | not restricted |
| start its own threads | see [section 5](#5-lifecycle-and-threads) |
| read game memory, read-only, with the `memory` permission | the host holds a handle with `PROCESS_VM_READ` only |
| **read and write the data folder of another plugin** | a known limitation, see below |

**Objects your plugin creates** (threads, events, mutexes, sections, ...) get the same protection
as its process: any other process of the same user can only wait on them and query basic
information. That includes your own plugin when it opens one of them again by name or id
(`OpenThread`, `OpenEvent`, ...). Keep and use the handle you got when you created the object: a
creator's handle has full access. Your own process token can be queried and duplicated, not
adjusted.

**Known limitation: data folders are not isolated from each other.** Every plugin runs as the same
Windows user at the same integrity level, so a plugin can read and write the data folder of any
other plugin (`Configs\plugins\data\<id>\`, section 8). Do not keep anything there that another
plugin must not read or change, and treat what you load from it as untrusted input. Users should
install only plugins they trust.

**Be honest about this with your users.** The sandbox protects the game, the app, other plugins'
processes and the user's keyboard and mouse. It is **not** a confidentiality sandbox: a plugin can
read the user's files, write other plugins' data folders and talk to the internet. Users should
install only plugins they trust. Say in your `description` and README what your plugin reads and
what it sends over the network, if anything.

## 3. Quick start

### Project setup

The simplest route is to copy `Plugins\ExamplePlugin\` inside this repository, rename it, give the
`.vcxproj` a new `ProjectGuid`, and change the id in both places (see below). If you build outside
the repository, mirror what `ExamplePlugin.vcxproj` does:

- **x64 DLL**, Unicode, `/EHsc` (the default). **Never `/EHa`**: with it, `catch (...)` would also
  swallow access violations and hide real crashes. The example uses C++20; the SDK headers need at
  least C++17. Use `/utf-8` if your sources contain non-ASCII text.
- **Include directories:** `PluginSDK\include` (for `<p1sdk/...>`) and `PluginSDK\imgui` (for
  `imgui.h` and `IconsFontAwesome6.h`).
- **Compile the SDK's pinned ImGui core into your DLL:** `PluginSDK\imgui\imgui.cpp`,
  `imgui_draw.cpp`, `imgui_tables.cpp`, `imgui_widgets.cpp`. Add no backends, and do not edit
  `PluginSDK\imgui\imconfig.h` or define `IMGUI_*` macros of your own: the host refuses a plugin
  whose ImGui version or struct layout differs from its own (section 7).
- `p1::Settings` uses the bundled `PluginSDK\third_party\nlohmann\json.hpp`. Define
  `P1SDK_NO_JSON` to leave it out.
- Ship a **Release** build. A Debug build (`/MDd`) needs the debug C runtime DLLs, which exist only
  on machines with Visual Studio installed.
- Generate a **PDB in Release too** and keep the PDB of every build you ship: it is what turns a
  user's crash dump into a call stack (section 12). The example links with `/DEBUG` and
  `/PDBALTPATH:%_PDB%`, so the DLL records only the PDB's file name and no path of your machine.

### A minimal plugin

```cpp
#include <p1sdk/p1sdk.hpp>

class HelloPlugin final : public p1::Plugin {
public:
    bool OnLoad() override {
        p1::Log(P1_LOG_INFO, "hello, config folder %s", ConfigDir().c_str());
        return true;                                   // false = refuse to run
    }
    void OnTick(const P1Frame& f) override {           // game logic, no ImGui here
        m_monsters = 0;
        for (const P1Entity& e : p1::Entities(*f.snapshot))
            if (e.kind == P1_KIND_MONSTER && e.has_life && e.hp > 0) ++m_monsters;
    }
    void OnOverlay(const P1Frame&) override {          // over the game, click-through
        char line[64];
        std::snprintf(line, sizeof line, "%u monsters", m_monsters);
        ImGui::GetWindowDrawList()->AddText(ImVec2(16, 16), IM_COL32(255, 255, 255, 255), line);
    }
    void OnPanel(const P1Frame&) override {            // inside the app's Plugins tab
        ImGui::TextUnformatted("Hello plugin");
    }
private:
    uint32_t m_monsters = 0;
};

// Exactly once per DLL, at namespace scope: emits the three exports.
P1_PLUGIN(HelloPlugin, "hello", "Hello", "1.0.0", "you", "Counts living monsters.")
```

The id in `P1_PLUGIN` **must equal** the `id` in `plugin.json`. If it does not, the plugin is
reported Incompatible.

### Folder layout

```
<POE1Fixer folder>\
  POE1Fixer.exe
  POE1FixerPluginHost.exe
  Plugins\                      created by you or the user; the app never creates or deletes it
    hello\                      any folder name, Cyrillic / CJK included
      plugin.json
      Hello.dll
      any-dependency.dll        dependency DLLs go NEXT TO your DLL
      data\...                  your read-only data files
  Configs\plugins\
    plugins.json                the app's enable / consent switches (do not edit while it runs)
    data\
      hello\                    your ONE writable folder, named after your id, made on first start
  CrashReports\plugins\         crash dumps of plugins
```

In this repository, building `POE1ExamplePlugin.sln` writes
`x64\<Configuration>\Plugins\ExamplePlugin\` (DLL + `plugin.json`): the folder to copy into
`<POE1Fixer folder>\Plugins\`.

### Enable it

1. Copy the folder into `<POE1Fixer folder>\Plugins\`.
2. In the app, open the **Plugins** tab and press **Rescan** (or restart the app).
3. Select the plugin and turn on **Enable**. Plugins always start disabled. With the `input`
   permission, the user also has to turn on **Allow input** separately.
4. The state chip goes Starting, then Running. Your panel appears under the plugin's details, and
   your log lines show up in the plugin's log view.

To ship a new build, disable the plugin first: the DLL stays locked while its process runs.
Replace the files, press **Rescan**, then enable it again. A running plugin keeps the manifest it
was started with until it restarts.

## 4. plugin.json reference

```json
{
  "id": "example",
  "name": "Example Plugin",
  "version": "1.0.0",
  "author": "POE1Fixer",
  "description": "What it does, what it reads, what it sends over the network.",
  "dll": "ExamplePlugin.dll",
  "sdk": 1,
  "permissions": ["memory"],
  "overlay": true,
  "panel": true,
  "maxFps": 60,
  "memoryLimitMB": 256
}
```

The file must be at most **64 KiB** of valid UTF-8 and contain one JSON object. Types are strict:
`60.0` or `"60"` where an integer is expected is an error, not a coercion. Unknown keys are
ignored, so a newer SDK can add optional fields.

| Key | Type | Required | Rules | Default |
|---|---|---|---|---|
| `id` | string | yes | 1..48 characters of `a-z 0-9 _ . -`; starts with a letter or digit; no `..`; does not end with `.`; the part before the first `.` is not a Windows device name (`con`, `prn`, `aux`, `nul`, `com0`..`com9`, `lpt0`..`lpt9`, `conin$`, `conout$`, `clock$`). It names your config folder and must equal the id in `P1_PLUGIN` | |
| `name` | string | yes | non-empty, at most 64 bytes | |
| `version` | string | yes | non-empty, at most 32 bytes (free form) | |
| `author` | string | no | at most 64 bytes | `""` |
| `description` | string | no | at most 512 bytes | `""` |
| `dll` | string | yes | a bare file name ending in `.dll` (any case), 5..200 bytes; no path separator, `:`, `*`, `?`, `"`, `<`, `>`, `\|` or `..`; does not start with `.` or a space; no space or `.` right before `.dll`; not a device name. The file must exist in the plugin folder | |
| `sdk` | integer | yes | must equal the app's `P1_ABI_VERSION` (1) | |
| `permissions` | array of strings | no | at most 16 entries, each `"memory"` or `"input"`. Any other string is an error, so a plugin that needs a right this app cannot grant never runs half-working | `[]` |
| `overlay` | bool | no | `false`: your overlay is never shown and `OnOverlay` is never called | `true` |
| `panel` | bool | no | `false`: no settings panel and `OnPanel` is never called | `true` |
| `maxFps` | integer | no | 1..144, the most overlay frames per second | `60` |
| `memoryLimitMB` | integer | no | 128..4096, the Job's memory cap for your process | `1024` |

Every text field must be valid UTF-8 with no control characters.

**Errors.** A bad manifest puts the plugin in the **Invalid** state, and the reason becomes the
state detail. Examples:
`plugin.json is missing`, `plugin.json is larger than 64 KiB`, `plugin.json is not valid JSON`,
`"id" is required`, `"maxFps" must be in 1..144`, `"maxFps" must be an integer`,
`"sdk" 2 is not supported (this app runs sdk 1)`, `unknown permission "net"`,
`the DLL named in plugin.json is missing`,
`duplicate id: another plugin folder already uses "example"`.

**Duplicate ids:** folders are scanned in name order (ordinal, case-insensitive). The first folder
that declares an id wins, and the others are Invalid.

## 5. Lifecycle and threads

### Start, run, stop

```
spawn (suspended, sandboxed), host maps shared memory
  LoadLibraryExW(your.dll)    your DllMain runs              each of your steps at most 15 s
  P1_GetPluginInfo            ABI / snapshot / ImGui / id     ("hung in on_load");
  (host creates its two ImGui contexts and fonts)            the whole start about 30 s
  P1_CreatePlugin             new YourClass()
  OnLoad()                    false or a throw = refuse
Running: loop { OnEvent x n, OnTick, OnOverlay, OnPanel }
stop:    OnUnload(), then ~YourClass(), then the process is terminated
```

- **Keep `DllMain` and global constructors trivial.** They run under the loader lock, before the
  SDK has caught anything.
- **`OnLoad`** runs once, before every other callback. Load your settings here. Returning `false`,
  or throwing, refuses the load: the plugin becomes **Incompatible** (`on_load refused to run`) and
  is not restarted. An exception thrown by your constructor has the same effect.
- **`OnUnload`** runs once, when the user disables the plugin, presses Restart, turns off the
  master switch, or exits the app. Save here. Your object is deleted right after it, and the
  process is then **terminated**: static destructors and `DLL_PROCESS_DETACH` do **not** run. The
  app waits **1.5 s** for a stop and then kills the process; when the app itself exits, the budget
  is about 1.3 s for all plugins together. So never do long work, or join a thread without a bound,
  in `OnUnload` or the destructor.

### One iteration of the plugin thread

The host's main thread is your plugin thread. Each turn of its loop does this:

1. If a stop was requested: `OnUnload`, then exit.
2. Read the app's live controls (permissions, the input gate, overlay on or off, panel size and
   lease, the game window, the language) and queue panel input.
3. Derive the control events: `P1_EV_PERMISSIONS`, `P1_EV_INPUT_GATE`, `P1_EV_LANGUAGE`,
   `P1_EV_GAME_FOCUS`, `P1_EV_PANEL_SHOWN` / `P1_EV_PANEL_HIDDEN`.
4. If the app published a new snapshot, copy it privately, then derive the snapshot events in this
   order: `P1_EV_DETACHED`, `P1_EV_ATTACHED`, `P1_EV_STATE_CHANGED`, `P1_EV_AREA_CHANGED`,
   `P1_EV_CHARACTER_CHANGED`.
5. Call `OnEvent` once per event, in that order.
6. Call `OnTick` if a new snapshot arrived or 100 ms have passed since the last tick.
7. Call `OnOverlay` if an overlay frame is due, then `OnPanel` if a panel frame is due.
8. Publish the frames.

The loop sleeps until the app wakes it (a new snapshot, input, a control change) or the next frame
is due, and never longer than 250 ms.

| Callback | When it runs |
|---|---|
| `OnEvent` | before the `OnTick` of the same iteration, once per event |
| `OnTick` | on every new snapshot (the game reader's cadence: about 30 per second, about 60 while the app's radar runs; plugins never change it), and at least 10 times a second otherwise, for example while detached. Check `snapshot->publish_seq` to tell whether the data is new |
| `OnOverlay` | when the manifest has `"overlay": true`, the plugin is enabled and the game window is known, on a new snapshot, a client-size or UI-scale change, or the rate you asked for with `p1::RequestFrames(fps)`. Never more often than `maxFps` |
| `OnPanel` | only while your panel is visible in the Plugins tab: about 30 times a second when idle, up to about 60 times a second while the user interacts with it |

The app takes overlay frames at most `maxFps` times a second, and panel frames at most 60 times a
second and only while your panel is shown; a frame beyond that is not shown (the next one is).
Building frames faster than that only spends your own CPU share.

### Events

| `P1Event::type` | `a`, `b` | Meaning |
|---|---|---|
| `P1_EV_ATTACHED` | a = game pid | the app attached to a game process |
| `P1_EV_DETACHED` | a = the old pid | the game process went away (or another one replaced it) |
| `P1_EV_STATE_CHANGED` | a = old, b = new `P1_STATE_*` | game state change. The first event has a = `P1_STATE_NOT_LOADED`; a detached snapshot counts as `P1_STATE_NOT_LOADED` |
| `P1_EV_AREA_CHANGED` | a = new `area.change_counter` | a new area or instance. `snapshot->area` already describes it |
| `P1_EV_CHARACTER_CHANGED` | | the local player's name changed to a different, non-empty name |
| `P1_EV_PANEL_SHOWN` / `P1_EV_PANEL_HIDDEN` | | your settings panel became visible or hidden |
| `P1_EV_PERMISSIONS` | a = effective `P1_PERM_*` | the user turned input consent on or off. Also sent once at start when the effective set differs from the manifest's (an `input` plugin without consent) |
| `P1_EV_LANGUAGE` | | the app's UI language changed: re-read `Language()` |
| `P1_EV_GAME_FOCUS` | a = 1 / 0 | the game window became the foreground window, or stopped being it |
| `P1_EV_INPUT_GATE` | a = 1 / 0 | the app now accepts, or refuses, input requests |

```cpp
void OnEvent(const P1Event& e) override {
    switch (e.type) {
        case P1_EV_AREA_CHANGED: m_seen.clear(); break;
        case P1_EV_INPUT_GATE:   m_canAct = e.a != 0; break;
        default: break;                         // a newer app may add event types: ignore them
    }
}
```

### Your own threads

- You may start threads (`std::thread`, a thread pool). Do anything slow there: file I/O, the
  network, heavy computation. A callback that blocks for 5 s kills the plugin.
- The host functions `Log`, `Notify`, `SetClipboard`, `RequestFrames`, `NowUs`, `Memory::*` and
  `Input::*` may be called from **any** thread. The `Texture::*` functions work **only on the
  plugin thread**: elsewhere they return 0 / `false` and do nothing.
- **ImGui only inside `OnOverlay` and `OnPanel`**, on the plugin thread. The host binds its ImGui
  context to your DLL right before each of those two calls. ImGui calls anywhere else touch a
  context that is not in a frame.
- The snapshot pointer is valid only until the callback returns, and only on the plugin thread.
  Copy what your threads need, and share it with your own synchronisation.
- **Catch everything at the top of every thread you start.** An exception that escapes one of your
  threads calls `std::terminate`, which is a crash: dump, back-off, restart.
- The hang check watches callbacks and the host loop. It does **not** notice a deadlock that stays
  inside your own threads. Give your waits time-outs.
- In `OnUnload`, signal your threads to stop and join them with a bound, or detach them. The
  process ends right after, either way.

### Exceptions

| What happens | Result |
|---|---|
| A C++ exception escapes `OnTick`, `OnOverlay`, `OnPanel`, `OnEvent` or `OnUnload` | the SDK catches it and logs `uncaught C++ exception in on_tick: <what>` at error level. The plugin keeps running, and ImGui windows or stacks you left open are repaired by the host |
| A C++ exception escapes `OnLoad` or your constructor | the load is refused (Incompatible) |
| A C++ exception escapes a thread you started | `std::terminate`, which is a crash |
| An access violation, stack overflow, `__fastfail`, `abort()`, a pure call or a CRT invalid-parameter error, on any thread | a crash: the app writes a dump, then restarts the plugin after the back-off |
| An allocation fails at the memory cap | `std::bad_alloc`. If you do not handle it, it is usually a crash, and the state detail says `memory limit N MB reached` |

Do not try to survive an access violation (`__try` around your own code, or `/EHa`). Let the
process die: you get a dump, and the user gets a clean restart.

## 6. The frame and the snapshot

### P1Frame

Every `OnTick`, `OnOverlay` and `OnPanel` gets a `const P1Frame&`:

| Field | Meaning |
|---|---|
| `snapshot` | never null. Valid until the callback returns. Before the first real snapshot it is an all-zero snapshot (`flags == 0`) |
| `viewport` | the game window and keyboard state, sampled at about 60 Hz (below) |
| `now_us` | steady clock in microseconds: the same clock as `snapshot->capture_us`, `publish_us` and `p1::NowUs()` |
| `delta_s` | seconds since the previous callback of the same kind; 0 on the first |
| `panel_w`, `panel_h` | your panel size in pixels, 0 while it is hidden |
| `panel_visible` | 1 while the app shows your panel |
| `overlay_enabled` | 1 when your overlay output will be shown |
| `permissions` | the effective `P1_PERM_*` right now. `P1_PERM_INPUT` is set only when the user also consented. Test with `p1::HasPermission(f, P1_PERM_MEMORY)` |
| `input_allowed` | 1 when `submit_input` would be accepted right now (section 9) |

`HostApi().permissions` (`ManifestPermissions()`) is what the manifest granted. It never changes
while the plugin runs. The live value is the one in `P1Frame::permissions`.

### P1Viewport

| Field | Meaning |
|---|---|
| `valid` | a game window is known. When 0, the client rectangle, cursor and keys are 0 (`ui_scale` and `app_foreground` are still set) |
| `client_x`, `client_y`, `client_w`, `client_h` | the game's client area in screen pixels. The overlay covers exactly this rectangle |
| `game_foreground` | the game window is the foreground window |
| `app_foreground` | the POE1Fixer menu is the foreground window |
| `ui_scale` | the app's UI scale (0.8 .. 2.0) |
| `cursor_x`, `cursor_y` | the mouse cursor in game client pixels; it can be outside the client area |
| `keys_down[32]` | one bit per virtual-key code. **Sampled only while the game is the foreground window and no game text field (such as chat) has focus**; all zero otherwise. Read it with `p1_key_down(&f.viewport, vk)` or `p1::KeyEdge` |

```cpp
p1::KeyEdge m_keys;                                     // a member
void OnTick(const P1Frame& f) override {
    m_keys.Update(f.viewport);                          // once per tick
    if (m_keys.Pressed(VK_F7)) m_show = !m_show;        // edge, not level
}
```

The app samples the keys about 60 times a second, so a tap shorter than one sample can be missed.

### P1Snapshot: header

| Field | Meaning |
|---|---|
| `size_bytes`, `header_bytes`, `version` | total size, the size of the header the app wrote, and `P1_SNAPSHOT_VERSION` |
| `flags` | `P1_SNAP_*`, below |
| `frame` | the game reader's frame counter (rises steadily within one app run) |
| `publish_seq` | the publisher's counter: a different value means new data |
| `capture_us` | steady-clock microseconds when entity positions and the camera were sampled (0 = never) |
| `publish_us` | when the app published this snapshot |
| `pid`, `game_state` | the game process id (0 = none) and its `P1_STATE_*` |
| `worker_period_ms` | the game reader's measured tick period |
| `module_base`, `ingame_state`, `ingame_data`, `ingame_ui`, `server_data`, `local_player`, `entity_list`, `camera_ptr` | raw addresses in the game process, for memory plugins (section 10). 0 = unknown; only filled while attached |
| `area` | `P1Area`, below |
| `player` | `P1Player`, below |
| `camera` | `view_proj[16]` + `viewport_w/h`. Valid with `P1_SNAP_CAMERA_VALID`. Use it through `p1::WorldToScreen` |
| `large_map`, `mini_map` | the game's map UI rectangles (`P1MapElement`). **Gated: radar** (`P1_SNAP_MAP_VALID`) |
| `latency_ms`, `gold`, `time_in_game_ms`, `network_state`, `league` | server values (`league` is a string) |
| `inv_cols`, `inv_rows`, `inv_occupied` | the main inventory grid (12 x 5) and its occupancy bits, bit `y * cols + x`. Valid with `P1_SNAP_INVENTORY_VALID` |
| `entity_count/stride/offset`, `entity_truncated` | the entity array (below). `entity_truncated` is the number of entities left out because the snapshot was full |
| `buff_count/stride/offset`, `item_count/stride/offset` | the buff and inventory-item arrays |
| `string_offset`, `string_bytes` | the string pool |

### Flags

| Flag | Meaning |
|---|---|
| `P1_SNAP_ATTACHED` | the app is attached to a game process |
| `P1_SNAP_ATTACHING` | the initial scan is running |
| `P1_SNAP_WGC_MODE` | the app is in visual (WGC) mode: no game memory, so no game data |
| `P1_SNAP_IN_GAME` | `game_state == P1_STATE_IN_GAME` |
| `P1_SNAP_LOADING` | an area-loading state |
| `P1_SNAP_INPUT_FOCUSED` | a text field in the game (such as chat) owns the keyboard |
| `P1_SNAP_WORKER_FAULT` | the game read faulted this tick: the data may be partial |
| `P1_SNAP_TRADE_BUSY` | the app's trade or stash automation owns the mouse |
| `P1_SNAP_RECOVERING` | the app's AFK or disconnect recovery is running |
| `P1_SNAP_CAMERA_VALID` | `camera` is valid |
| `P1_SNAP_BUFFS_VALID` | the player's buffs were read this tick. **Gated: AutoPot** |
| `P1_SNAP_MAP_VALID` | `large_map` / `mini_map` were read. **Gated: radar** |
| `P1_SNAP_INVENTORY_VALID` | the main inventory is valid |

`flags == 0` means no game data at all: not attached yet, or the very first callback.

**Gated fields.** Some reads cost the app time, so it does them only while one of its own
features needs them. Plugins get these fields as a by-product and can never turn the reads on.
Always test the flag or the "unknown" value, and degrade gracefully:

| Field | Filled only while |
|---|---|
| player buffs (`P1_SNAP_BUFFS_VALID`, `p1::PlayerBuffs`) | AutoPot reads them (it is enabled and running) |
| `large_map`, `mini_map` (`P1_SNAP_MAP_VALID`) | the radar reads the map UI |
| `P1Entity::targetable` (monsters only, 0xFF otherwise) | the radar runs. A value the radar already cached keeps being published |
| `P1Entity::minimap_icon`, `transition_target`, `item_path` | the radar runs; cached values keep being published |

### P1Area and P1Player

`P1Area`: `name` (localized), `id` (WorldArea id such as `"1_1_town"`), `act`, `level`, `hash`,
`instance_id`, `world_area_id`, `change_counter` (rises on every real area or instance change),
`is_town`, `has_waypoint`, `is_hideout`.

`P1Player`: `address`, `id`, `name`, `hp`, `hp_max`, `es`, `es_max`, `mana`, `mana_max`, the four
reservation fields (`hp_reserved_flat`, `hp_reserved_fraction`, `mana_reserved_flat`,
`mana_reserved_fraction`), `hp_regen`, `mana_regen`, `grid_x`, `grid_y`, `world_x`, `world_y`,
`rotation`, `action_id`, `animation_id`, and `buff_first` / `buff_count`, its range in the buff
array. Iterate the buffs with `p1::PlayerBuffs(s)`.

`P1MapElement` (`large_map`, `mini_map`; radar-gated): `valid`, `visible`, `pos_base_x/y`,
`size_base_x/y`, `scale_rel`, `shift_x/y`, `default_shift_x/y`, `zoom`. Positions and sizes are in
the game's UI base units (2560 x 1600, fitted to the height): pixels = base x `client_h` / 1600.

### Arrays: entities, buffs, inventory items

**Never index the arrays as C arrays.** A newer app may grow the element structs, so always go
through the stride, with `p1::Entities(s)` / `p1::PlayerBuffs(s)` / `p1::InventoryItems(s)` or
`p1_entity_at` / `p1_buff_at` / `p1_item_at`, which also bounds-check.

`P1Entity`:

| Field | Meaning |
|---|---|
| `address`, `id` | the entity in the game. Use the pair as its identity |
| `flags` | the raw low 32 bits of the game's entity flags (undocumented bits) |
| `comp_mask` | `P1_COMP_*`: which components the entity has |
| `kind` | `P1_KIND_*` (player, monster, NPC, chest, world item, area transition, portal, waypoint, effect, misc object, unknown) |
| `rarity` | 0 normal, 1 magic, 2 rare, 3 unique, 0xFF none |
| `reaction` | 1 friendly (your minions too), anything else hostile or neutral, 0xFF none |
| `targetable` | 0 / 1, 0xFF unknown (radar-gated, monsters only) |
| `has_life`, `is_opened`, `is_strongbox` | 0 / 1 |
| `grid_x`, `grid_y` | grid position |
| `render_pos`, `render_bounds`, `rotation` | world position of the model's base, its bounds and its rotation. `render_pos` is what you project |
| `terrain_height` | the terrain height under the entity, in the radar's (negated) convention |
| `hp`, `hp_max`, `es`, `es_max`, `mana`, `mana_max` | vitals. Only the local player's are authoritative. Corpses read `hp == 0` |
| `path` | metadata path (string) |
| `minimap_icon`, `transition_target`, `item_path` | the game's minimap icon id, the destination WorldArea id of an exit, and the inner item path of a ground item (strings; radar-gated) |

`P1Buff`: `name` (BuffDefinitions id), `timer` and `max_time` in seconds (`+inf` on an endless
buff), `charges`, `flask_slot`, `source_id`. At most 256 buffs.

`P1InventoryItem`: `entity` (address), `x`, `y`, `w`, `h` (cells), `base_name`. At most 128.

### Strings

Strings are `p1_str` byte offsets into the snapshot's string pool. 0 is the empty string. Read them
with `p1::Str(s, off)` or `p1_str_get(&s, off)`: both are bounds-checked and return `""` for a bad
offset. Every string is UTF-8 and NUL-terminated. Area, player and league strings are cut at
1024 bytes, on a character boundary.

```cpp
const P1Snapshot& s = *f.snapshot;
if (s.flags & P1_SNAP_ATTACHED)
    p1::Log(P1_LOG_INFO, "%s (level %d), %u entities", p1::Str(s, s.area.name), s.area.level, s.entity_count);
for (const P1Entity& e : p1::Entities(s))
    if (e.kind == P1_KIND_CHEST && e.is_strongbox && !e.is_opened) { /* ... */ }
```

### Versioning inside the snapshot

The snapshot is append-only. A field added in a later SDK exists only when
`P1_SNAP_HAS(s, field)` is true: an older app never wrote it, and the bytes at that position belong
to the arrays. Element structs only grow at the end; that is why strides exist. A plugin built
against snapshot version N runs on any app that provides version N or later.

## 7. Drawing

Your process has two Dear ImGui contexts that share nothing. The host owns both: it creates them,
calls `NewFrame`, opens a full-size window, calls you, closes the window, renders, and ships the
draw data to the app. **You never call** `CreateContext`, `DestroyContext`, `SetCurrentContext`,
`NewFrame`, `EndFrame` or `Render`.

### Overlay: `OnOverlay`

- Drawn over the game's client area. **Coordinates are game client pixels**, with (0, 0) at the
  top-left of the client area and `ImGui::GetIO().DisplaySize` = (`client_w`, `client_h`).
- You are inside a full-size, transparent, undecorated window. Draw with
  `ImGui::GetWindowDrawList()`, or `GetForegroundDrawList()` / `GetBackgroundDrawList()`.
- **Click-through:** the overlay never receives clicks or keys. It does get the mouse position
  (`ImGui::GetIO().MousePos` = `f.viewport.cursor_x`, `cursor_y`), but the window the host opens for
  you has `ImGuiWindowFlags_NoInputs`, so ImGui's own hover queries (`IsItemHovered`,
  `IsWindowHovered`, `SetItemTooltip`) never report a hover in it. Test the cursor yourself with
  `ImGui::IsMouseHoveringRect(min, max)` and draw the tooltip with `ImGui::SetTooltip` /
  `BeginTooltip`, or open a window of your own inside `OnOverlay` without `NoInputs` (it still gets
  no clicks). Interactive overlay widgets are not part of v1.

  ```cpp
  const ImVec2 a(p.x - r, p.y - r), b(p.x + r, p.y + r);
  if (ImGui::IsMouseHoveringRect(a, b)) ImGui::SetTooltip("%s", label);   // works in the overlay
  ```
- The style is ImGui's default, with `FontScaleMain` set to the app's UI scale. Scale your own
  sizes by `f.viewport.ui_scale`.
- Frames: on a new snapshot, a client-size or UI-scale change, or at the rate you request (an
  animation). All of these are capped by `maxFps`:

  ```cpp
  bool OnLoad() override { p1::RequestFrames(30); return true; }   // 0 = only on new data (default)
  ```
- When your overlay stops being drawn (for example, the game window is gone), the host sends one
  empty frame, so the app never keeps your last picture. A plugin that stops or crashes disappears
  from the overlay at once.
- Your overlay is drawn above the app's radar and below the app's toasts. A frame built for a
  client size other than the current one is skipped; the next one, built after the resize, shows.

Projecting a world position:

```cpp
void OnOverlay(const P1Frame& f) override {
    const P1Snapshot& s = *f.snapshot;
    if (!(s.flags & P1_SNAP_IN_GAME) || !(s.flags & P1_SNAP_CAMERA_VALID)) return;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float r = 9.0f * (f.viewport.ui_scale > 0.0f ? f.viewport.ui_scale : 1.0f);
    for (const P1Entity& e : p1::Entities(s)) {
        if (e.kind != P1_KIND_MONSTER || !e.has_life || e.hp <= 0 || e.reaction == 1) continue;
        ImVec2 p;
        if (p1::WorldToScreen(f, e.render_pos, p))            // false when behind the camera
            dl->AddCircle(p, r, IM_COL32(240, 90, 80, 220), 0, 2.0f);
    }
}
```

`p1::WorldToScreen` uses the snapshot's camera, captured together with the entity positions, and
scales the result to the client size. The C form is
`p1_world_to_screen(s, x, y, z, client_w, client_h, &sx, &sy)`.

### Settings panel: `OnPanel`

- Drawn inside the app's **Plugins** tab, under your plugin's details. **Coordinates are
  panel-local pixels**, and the size is `f.panel_w` x `f.panel_h`; prefer
  `ImGui::GetContentRegionAvail()`.
- You are inside a full-panel window with no title bar, which scrolls inside itself. It gets
  **full mouse and keyboard input** while the user hovers or focuses it and the app's menu is
  shown: named keys, Ctrl/Shift/Alt, text characters, the mouse wheel, and Ctrl+V / Shift+Insert
  pastes (the app sends its clipboard first). Your mouse cursor shape is applied.
- The style is the app's current theme and UI scale. When the user changes the theme, the host
  replaces the whole style. For your own tweaks, use `PushStyleVar` / `PushStyleColor` around your
  widgets instead of editing `GetStyle()` once.
- `OnPanel` runs only while the panel is visible (`P1_EV_PANEL_SHOWN` / `P1_EV_PANEL_HIDDEN`).
  Popups and combo lists are clipped to the panel's rectangle. IME composition for CJK input is not
  supported in v1, and `ImGui::TextLinkOpenURL` does nothing (the sandbox cannot start a browser):
  copy the link to the clipboard instead.

### Fonts and icons

Both contexts use Segoe UI at 18 px, with Cyrillic, Thai, Japanese and Korean glyphs merged from
the Windows fonts and Font Awesome 6 Solid at 16 px (from `<app dir>\Resources\fonts\`). The host
builds them once at start, so text in any of the app's languages renders, whichever one is selected.
Icons: `#include <IconsFontAwesome6.h>` (in `PluginSDK\imgui`) and write `ICON_FA_BELL "  Text"`.
Adding your own fonts is not part of the v1 contract.

### Textures

```cpp
// The plugin thread only (any callback). RGBA8, rows tightly packed; ImU32 from IM_COL32 is RGBA in memory.
std::vector<ImU32> px(64 * 64, IM_COL32(255, 192, 0, 255));
m_tex = p1::Texture::Create(px.data(), 64, 64);          // 0 = refused
// ... in OnOverlay or OnPanel:
if (m_tex) ImGui::Image(ImTextureRef(m_tex), ImVec2(64, 64));
// later: p1::Texture::Update(m_tex, x, y, w, h, pixels, pitch /* 0 = w*4 */);  p1::Texture::Destroy(m_tex);
```

- At most **4096 x 4096** per texture.
- At most **64 MiB of live textures per plugin**, and at most **1024 live textures** per plugin.
  **Both budgets include the font atlases ImGui builds for your two contexts**: a few MiB, and more
  once CJK glyphs or many font sizes are in use. Each atlas stops growing at 2048 x 2048 (16 MiB; a
  full one drops the font sizes you no longer draw), and an atlas never takes you over either
  budget: when your own textures leave no room, the new atlas waits and the text that needs it is
  not drawn until room frees up. `Create` returns 0 instead of going over either budget, and your
  plugin keeps running.
- `Create` also returns 0 when called off the plugin thread, when more than 80 MiB of texture data
  is still waiting to reach the app, or when your process runs out of memory half-way (the
  half-made texture is then destroyed again; nothing leaks against your budget).
- A frame that uses a new texture is published only after the texture's data has gone out. The app
  uploads at most about 2 MiB per rendered frame across all plugins, so a large texture can take a
  few frames to appear. Draw commands that use a destroyed or unknown texture id are dropped.
- If the app ever sees more than 64 MiB or more than 1024 textures live, or a texture larger than
  4096 px, the plugin is stopped as **Faulted** (`texture budget: ...`). Through `p1::Texture` you
  cannot get there: the host refuses first.

### Limits per frame

| | Overlay | Panel |
|---|---|---|
| vertices | 150,000 | 75,000 |
| indices | 300,000 | 150,000 |
| draw commands | 8,192 | 4,096 |
| display size, each side | at most 16,384 px | at most 8,192 px |

Over a limit, the host keeps the commands in ImGui's order and **drops the rest of the frame from
the first command that does not fit**. The frame is flagged truncated (`kFrameTruncated`), and the
app still shows it. `ImDrawCallback` user callbacks never cross to the app: they are dropped.
Indices are 16-bit (`ImDrawIdx`), with `VtxOffset` support.

**What the app draws.** The table above is what a frame may carry. On top of it the app draws at
most **1024 draw commands** of each frame and at most **6 screens of fill** (the summed area of the
frame's triangles, at most 6 times the display's width x height); the rest of the frame is cut
from the end, and the frame counts as truncated. And when all plugins together would exceed
**2048 draw commands, 300,000 vertices or 8 screens of fill** in one app frame, the app leaves some
plugins' frames out of that app frame, in rotation, so no plugin is starved for good and the app's
own frame time stays bounded. These are limits the app enforces, not targets: draw what you need,
with few large draw calls, and avoid overdraw (big filled shapes on top of each other).

### The pinned ImGui

`PluginSDK\imgui\` holds Dear ImGui **1.92.8** (`IMGUI_VERSION_NUM 19280`): the core files and the
project's `imconfig.h`, which defines `IMGUI_DEFINE_MATH_OPERATORS`, disables the demo windows and
the debug tools, and compiles `IM_ASSERT` out. The host compiles the same copy. On load it compares
the plugin's `IMGUI_VERSION_NUM` and `sizeof` of `ImGuiIO`, `ImGuiStyle`, `ImVec2`, `ImVec4`,
`ImDrawVert`, `ImDrawIdx` and `ImGuiContext` with its own, and refuses any difference
(Incompatible). This copy changes only together with a plugin ABI review, and every plugin is then
rebuilt. The app's own ImGui can be upgraded without touching plugins.

`io.IniFilename` is null, so ImGui saves no window positions; store your own if you need them.
The host repairs Begin/End or Push/Pop stacks you leave unbalanced (for example after an exception),
but do not rely on it.

## 8. Settings and files

| Path (UTF-8, absolute) | Access | What for |
|---|---|---|
| `HostApi().config_dir` / `ConfigDir()` = `<app dir>\Configs\plugins\data\<id>\` | **read and write: your one writable folder.** Created on the first start; it survives updates of your plugin. The process's current directory is this folder. **Other plugins can read and write it too** (section 2) | settings, caches, your own logs |
| `HostApi().plugin_dir` / `PluginDir()` = `<app dir>\Plugins\<folder>\` | read-only | your DLL, dependencies and data files |
| `HostApi().app_dir` / `AppDir()` | read-only | the app folder (`Resources\` lives there) |

Always build absolute paths from these; never rely on the current directory.

### p1::Settings

JSON files in `config_dir`, with nlohmann::json:

```cpp
nlohmann::json j;
std::string error;
switch (p1::Settings::Load("settings.json", j, &error)) {
    case p1::Settings::LoadResult::Loaded:   ApplySettings(j); break;
    case p1::Settings::LoadResult::Missing:  break;                    // first run: defaults
    case p1::Settings::LoadResult::Rejected:                            // kept as settings.json.rejected
        p1::Log(P1_LOG_WARN, "settings unreadable (%s), using defaults", error.c_str()); break;
    case p1::Settings::LoadResult::Error:
        p1::Log(P1_LOG_WARN, "settings not loaded: %s", error.c_str()); break;
}
// ...
if (!p1::Settings::Save("settings.json", BuildJson(), &error))
    p1::Log(P1_LOG_WARN, "settings not saved: %s", error.c_str());
```

- **Names** are plain file names: letters, digits, `_`, `-` and `.`, 1..96 bytes, no leading dot,
  no `..`. `Load` reads files up to 16 MiB; a larger file counts as unreadable.
- **Save is atomic.** It writes `<name>.tmp`, flushes it, and replaces the target with
  `MoveFileExW(REPLACE_EXISTING | WRITE_THROUGH)`. Invalid UTF-8 inside strings is replaced, so it
  never throws.
- **The `.rejected` rule.** A file that cannot be opened, read or parsed is renamed
  `<name>.rejected` (replacing an older one) and `Rejected` is returned, so you start from
  defaults and the broken file stays available for inspection. It is never overwritten. If even
  the rename fails, `Load` returns `Error` and `Save` refuses that name until a later `Load`
  succeeds.
- `Load` and `Save` are thread-safe (one mutex). Do them in `OnLoad` / `OnUnload` or on your own
  thread, and debounce saves; the example saves 1 s after the last change. Keep the work in a
  callback well under the 5 s limit.

### Cyrillic, CJK and other non-ASCII paths

Users install POE1Fixer into folders such as `C:\Игры\POE1Fixer` or `D:\ゲーム\`, and their
Windows account names can be non-ASCII too. Every path the SDK gives you is **UTF-8**:

- Convert with `p1::Widen(utf8)` and call the **W** APIs (`CreateFileW`, `std::filesystem::path`
  built from a `std::wstring`). Convert back with `p1::Narrow(wide)`.
- **Never** pass these paths to an `A` API, `fopen`, `std::ifstream(std::string)`,
  `std::filesystem::path(std::string)` or `path::string()`. They go through the ANSI code page,
  which garbles or throws on characters the code page lacks.
- Size UTF-8 path buffers in bytes: one Cyrillic character takes 2 bytes, one CJK character 3.

```cpp
std::wstring path = p1::Widen(ConfigDir());
if (!path.empty() && path.back() != L'\\') path.push_back(L'\\');
path += L"cache.bin";
HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
```

When your DLL loads, its dependencies are searched for only in your plugin folder, the app folder
and System32, never in the current directory or `PATH`. A later `LoadLibraryW(L"name.dll")` of
your own does not search your plugin folder, so pass the full path built from `PluginDir()`.

## 9. Input requests

A plugin cannot send input itself (section 2). It **asks the app**, which performs the actions on
its own input thread, only when that is safe. You need all of these:

1. `"permissions": ["input"]` in `plugin.json`;
2. the user's consent: the **Allow input** switch for your plugin in the Plugins tab, off by
   default. Until it is on, `P1_PERM_INPUT` is missing from `P1Frame::permissions`;
3. an open gate. The app re-checks it before **every single step**: your plugin is running, the
   game window is known and is the foreground window (no POE1Fixer or plugin window in front), and
   no trade, stash or recovery automation is running (`P1_SNAP_TRADE_BUSY`, `P1_SNAP_RECOVERING`).
   On top of that, at most 60 actions per second per plugin are performed (`Wait` does not count):
   the 61st within one second closes the gate for your plugin, as described below.

`P1Frame::input_allowed` and the `P1_EV_INPUT_GATE` event summarize 2 and 3.

```cpp
void OnTick(const P1Frame& f) override {
    if (!f.input_allowed || !m_wantAction) return;
    const uint32_t queued = p1::Input::Batch()
        .MouseMove(m_target.x, m_target.y)         // game client pixels
        .Wait(50)                                  // ms, at most 1000
        .Click(m_target.x, m_target.y)             // left button, held 30 ms
        .Submit();                                 // all or nothing; 0 = refused
    if (queued) m_wantAction = false;
}
```

| Action | Fields |
|---|---|
| `KeyDown(vk)`, `KeyUp(vk)` | a Windows virtual-key code (keyboard keys only) |
| `KeyPress(vk, holdMs)` | down, hold, up; `holdMs` 0 means 30 ms, at most 1000 ms |
| `MouseMove(x, y)` | game client pixels |
| `MouseDown(vk)`, `MouseUp(vk)` | `VK_LBUTTON`, `VK_RBUTTON`, `VK_MBUTTON`, `VK_XBUTTON1`, `VK_XBUTTON2` |
| `Click(x, y, vk, holdMs)` | move, hover 15 ms, down, hold, up; `holdMs` 0 means 30 ms, at most 1000 ms |
| `Wait(ms)` | at most 1000 ms |

- **A batch holds at most 32 actions** (`P1_INPUT_MAX_BATCH`). `p1::Input::Batch` sends nothing at
  all once it has overflowed (`Overflowed()`). Shortcuts: `p1::Input::KeyPress(vk)`,
  `p1::Input::Click(x, y)`. At most 256 actions wait per plugin; a batch that would go over is
  refused whole.
- **Refused one by one, while the rest of the batch runs:**
  - any key bound to one of the app's own hotkeys (a plugin cannot trigger the app's features);
  - the Windows keys, the Sleep key, and the browser and launch-application keys;
  - keys that would make a system chord: `Space`, `Tab`, `Esc` or `F4` while any Alt key is down
    (Alt+F4 would close the game, Alt+Tab switch away), and `Esc` while Ctrl is down;
  - a mouse button sent as a key (or the reverse).
- **Closes the gate for your plugin**, so the rest of its queue is dropped and every key and mouse
  button it holds is released:
  - a mouse target outside the game's client area, or over a window that covers the game there
    (a `MouseMove` to such a point, or a button pressed while the cursor is there);
  - the 61st action within one second;
  - `SendInput` failing.
- `Submit` returns how many actions were **queued**, or 0 when it was refused on the spot (no
  permission, no consent, a closed gate, a bad batch). Queued is not performed: if the gate closes
  mid-batch, the app drops the rest of the batch and **releases every key and mouse button your
  plugin holds**. The same release happens when your plugin stops, crashes or is disabled. When the
  user turns **Allow input** off, everything your plugin still has queued is dropped at once, before
  another action runs. Design for this: check the result in the next snapshot, and never assume a
  `KeyDown` is still held.
- Actions the app refused are counted in your plugin's telemetry ("refused inputs").
- The app never lets plugin input delay its own trade work: while trade or recovery automation
  runs, plugin input is refused (`Submit` returns 0) and anything still queued is dropped. Nothing
  waits for the automation to end: submit again once `input_allowed` is back.

## 10. Game memory reads

With `"permissions": ["memory"]`, your host process holds a **read-only** handle to the game
(`PROCESS_VM_READ | PROCESS_QUERY_LIMITED_INFORMATION`). No write path exists. This permission
needs no extra user consent, but the Plugins tab shows it to the user.

```cpp
const P1Snapshot& s = *f.snapshot;
if (p1::HasPermission(f, P1_PERM_MEMORY) && s.player.address) {
    uint64_t vtable = 0;
    if (p1::Memory::Read(s.player.address, vtable)) { /* all 8 bytes read */ }

    uint64_t obj = 0;   // p = base; for each offset: p = *(uint64_t*)(p + offset)
    if (p1::Memory::ReadPointerChain(s.ingame_state, { kMyOffsetA, kMyOffsetB }, obj)) { /* ... */ }

    P1ReadRequest reqs[2] = { { addrA, &a, sizeof a, 0 }, { addrB, bufB, sizeof bufB, 0 } };
    const uint32_t complete = p1::Memory::ReadBatch(reqs, 2);   // each request's bytes_read is filled
}
```

- `Memory::ReadBytes` returns the number of bytes read; a partial read reports what it got.
  `Memory::Read<T>` succeeds only when all `sizeof(T)` bytes were read. One request is at most
  16 MiB.
- Every function returns 0 / `false` without the permission, while the app is not attached, and in
  WGC mode. When the game restarts, the app passes the new process's handle automatically.
- Safe from any thread.
- **Anchors:** `module_base`, `ingame_state`, `ingame_data`, `ingame_ui`, `server_data`,
  `local_player`, `entity_list` and `camera_ptr` in the snapshot are the app's own entry points,
  re-derived by the app after every game patch. 0 means unknown.
- **Offsets rot.** Offsets inside game objects change with game patches; every patch can break your
  plugin. Prefer the typed snapshot fields whenever they cover what you need. Keep your offsets in
  one place, and treat every value you read as untrusted: the game changes its memory while you
  read it, so a read can be torn or point at freed memory. Check that pointers are plausible and
  values are in range before you follow or use them. A failed read never crashes, but code that
  trusts garbage can.

## 11. Logging, toasts and the clipboard

```cpp
p1::Log(P1_LOG_INFO, "entered %s", p1::Str(s, s.area.name));        // printf-style, UTF-8
p1::Notify(P1_NOTIFY_SUCCESS, "Loot", "A divine orb dropped.");    // a toast in the app
p1::SetClipboard("https://example.org/build/123");                 // via the app
```

| Service | Limits |
|---|---|
| `p1::Log(level, fmt, ...)` | levels `P1_LOG_DEBUG`, `INFO`, `WARN`, `ERROR`. A line is cut at about 2 KiB; invalid UTF-8 becomes `?`. The app takes **50 lines a second, in bursts of up to 200**; lines over the limit are dropped and counted ("dropped log lines"). **Every** accepted line goes to your plugin's own log view in the Plugins tab (the newest 200 lines). **Only `WARN` and `ERROR` lines** also go to the app's **System Logs** tab, category **Plugins**, as `[<id>] text`, and there at most **one line a second across all plugins together** (the Plugins tab still shows them all) |
| `p1::Notify(level, title, text)` | levels `P1_NOTIFY_INFO`, `SUCCESS`, `WARNING`, `ERROR`. **At most one toast every 2 s per plugin**; extra toasts are dropped. Title at most 128 bytes, text at most 512 bytes. The toast is titled `[<plugin name>] <title>`, so it can never pass for one of the app's own |
| `p1::SetClipboard(text)` | at most about 2 KiB. Only the newest request counts. The app puts it on the clipboard **only while the user is using your panel** (it has focus, and the user's last mouse or keyboard input in it was within the last second), **never while the app's trade automation runs**, and **at most once a second**; a request made at any other time is not applied. So copy in answer to the user: a button in your panel, or ImGui's own copy (Ctrl+C in a text field of your panel), which goes the same way |

None of these calls ever block. When the shared ring they travel through is full, the record is
dropped and counted. Do not log every frame: log changes.

## 12. Debugging

### The Plugins tab

Each plugin shows a **state chip**, a **state detail** line, a **telemetry** line, its settings
panel and a collapsible **log** with the newest 200 lines: your own lines and the app's lines about
your plugin. **Restart** stops the plugin and starts it again at once, and clears Faulted,
Incompatible and SandboxError together with the crash history. Disabling and re-enabling a plugin
clears them too.

| State | Meaning | What happens next |
|---|---|---|
| Disabled | installed, not enabled (or stopped). The detail can say `too many plugins running at once (16 max)` | nothing until the user enables it |
| Starting | the process is up, loading your DLL | Running, or a failure state |
| Running | your callbacks are being called | |
| Stopping | `OnUnload` is running | Disabled, killed after 1.5 s |
| Backoff | it crashed or hung; the detail says why | restarts after 2, 4, 8, 16, then 30 s |
| Faulted | a crash loop (4 crashes in 10 min), or the plugin broke the protocol (`protocol: ...`) or the texture budget (`texture budget: ...`) | **no automatic restart.** Fix it, then Restart |
| Incompatible | it can never load as built: ABI, snapshot or ImGui mismatch, id mismatch, a missing export, a DLL that fails to load (a missing dependency), or `OnLoad` refused | no restart. Rebuild or fix, then Restart |
| Invalid | `plugin.json` is missing or invalid, the DLL is missing, or the id is a duplicate | fix it, then **Rescan** |
| SandboxError | the app could not create the sandbox: `POE1FixerPluginHost.exe` missing, a path longer than 1023 characters, a Windows error | no restart; the detail names the cause |

Typical details: `ACCESS_VIOLATION in MyPlugin.dll+0x1A2B`, `std::terminate`, `abort()`,
`hung in on_tick`, `stopped responding (no heartbeat for 10 s)`,
`did not finish starting in 30 s`, `memory limit 256 MB reached (NO_MEMORY)`,
`the DLL failed to load (missing dependency?): LoadLibrary failed, error 126: ...`,
`built with Dear ImGui 19250, this host uses 19280 (rebuild with the plugin SDK)`.

The **telemetry** line shows: CPU use (percent of one core, averaged over about 1 s, so it can
exceed 100 with several busy threads), working set, the duration of your last `OnTick`,
`OnOverlay` and `OnPanel` in milliseconds, the render-thread cost the app attributes to you,
overlay and panel vertices per frame, live texture bytes, rejected frames, dropped log lines and
refused input actions.

### Logs

- Your plugin's log view in the **Plugins** tab: every line you log (the newest 200) and the app's
  lines about your plugin.
- **System Logs** tab, category **Plugins**: only your `WARN` and `ERROR` lines, as `[<id>] text`
  (at most one plugin line a second across all plugins), and the app's lines about your plugin as
  `plugin <id>: text`, for example:
  - `starting Example Plugin 1.0.0 (pid 1234, instance 3)`, then `running`;
  - `crashed: ACCESS_VIOLATION in ExamplePlugin.dll+0x1A2B; restarting in 2 s`;
  - `hung in on_tick; terminated`, `cannot run: ...`, `stopped: protocol: ...`.
- For a large debug trace, write your own file into `config_dir`.

### Crash dumps

`<app dir>\CrashReports\plugins\<id>_<yyyymmdd-hhmmss>.dmp` and a `.txt` beside it, which lists
the plugin, the time, what happened, the exception code and address, the thread, and the faulting
module with its offset. The newest 5 of each are kept per plugin. Open the `.dmp` in Visual Studio
or WinDbg with **your plugin's PDB** next to the DLL or on the symbol path. The example's Release
configuration writes a PDB too (`/Zi`, `/DEBUG`, `/PDBALTPATH:%_PDB%`: the DLL names only the PDB's
file name, never a folder of your machine). Keep the PDB of every build you ship. The app writes
dumps whether it runs elevated or not.

### Attaching a debugger

You can attach Visual Studio to `POE1FixerPluginHost.exe` (one process per plugin; the pid is in
the log). The app cannot tell a debugger from a hang: a breakpoint that holds a callback for more
than 5 s, or stops the whole process for more than 10 s, gets the process killed. Break briefly,
or debug with logging.

## 13. ABI and versioning rules

What has to match, and when:

| Check | Rule | On mismatch |
|---|---|---|
| `plugin.json` `sdk` | must equal the app's `P1_ABI_VERSION` | Invalid |
| `P1PluginInfo::abi_version` | must equal the host's `P1_ABI_VERSION` | Incompatible |
| `P1PluginInfo::snapshot_version` | 1 or more, and at most the host's `P1_SNAPSHOT_VERSION` (a newer app runs older plugins) | Incompatible |
| `imgui_version_num` and the 7 ImGui layout sizes | must equal the host's | Incompatible |
| `P1PluginInfo::id` | must equal `plugin.json` `id` | Incompatible |
| exports | `P1_GetPluginInfo`, `P1_CreatePlugin`, `P1_BindImGui` (the `P1_PLUGIN` macro emits all three) | Incompatible |

Rules that keep a plugin working across app releases:

- **Plain C crosses the boundary.** No C++ classes, `std::` types, exceptions or vtables between
  your DLL and the host. The SDK wrapper keeps exceptions on your side.
- **Structs only grow at the end.** Every ABI struct starts with `size_bytes`. A side reads a field
  only when `size_bytes` covers it: `P1_HAS_FIELD(p, Type, field)` for `P1HostApi`, `P1Frame`,
  `P1Event` and `P1PluginInfo`, and `P1_SNAP_HAS(s, field)` for the snapshot header. The SDK
  wrapper already checks every host function it calls. Never write `sizeof(your struct)` into
  memory the other side owns.
- **Index arrays by stride** (`p1::Entities`, `p1_entity_at`, ...), never as C arrays.
- **Ignore what you do not know:** unknown event types, `P1_STATE_*` values, `P1_COMP_*` bits and
  `P1_KIND_*` values. Write a `default:` in every switch.
- **Copy what you keep.** No pointer into the snapshot, and no snapshot string, outlives the
  callback.
- **Do not depend on undocumented bits** such as `P1Entity::flags`.
- **Build against the SDK that ships with the app version you target**, with its ImGui copy and
  `imconfig.h` unchanged. When the plugin ABI or the pinned ImGui changes, `P1_ABI_VERSION` or
  `IMGUI_VERSION_NUM` changes, and plugins must be rebuilt. Additions that keep compatibility stay
  inside the same ABI version, as fields appended at the end.

## 14. FAQ and troubleshooting

**Incompatible: "the DLL failed to load (missing dependency?)".** Windows could not load your DLL
or one of its dependencies (error 126 = module not found). Put every dependency DLL **next to your
plugin DLL**; only your plugin folder, the app folder and System32 are searched. Check the list
with `dumpbin /dependents YourPlugin.dll`. The usual culprit is a Debug build: `/MDd` needs
`vcruntime140d.dll` and `ucrtbased.dll`, which exist only where Visual Studio is installed. Ship
Release (`/MD`), or link the C runtime statically (`/MT`).

**Incompatible: "built against another Dear ImGui version or layout".** Rebuild with the ImGui
files from the `PluginSDK\imgui` that ships with the app, with its `imconfig.h` untouched and no
`IMGUI_*` or `ImDrawIdx` defines of your own.

**Incompatible: "the DLL reports another id than plugin.json".** The id in `P1_PLUGIN(...)` and the
`id` in `plugin.json` differ.

**Incompatible: "on_load refused to run" or "P1_CreatePlugin failed".** `OnLoad` returned `false`
or threw, or your constructor threw. The reason is in your plugin's log.

**Invalid.** The detail names the manifest error (section 4). Fix it and press **Rescan**.

**Faulted after a few crashes.** Open the newest `.txt` and `.dmp` in
`CrashReports\plugins\`, fix the crash, ship a new build, press **Restart**.

**Backoff, "hung in on_tick" (or on_overlay, on_panel, on_load).** A callback ran longer than 5 s,
or a loading step longer than 15 s. Move slow work (files, network, heavy computation) to your own
thread.

**"memory limit N MB reached".** Fix the leak, or raise `memoryLimitMB` (at most 4096).

**My overlay does not show.** Check that `"overlay"` is not `false`; that `OnOverlay` is
overridden; that the game window is known (`f.viewport.valid`); that you draw inside
(0, 0)..(`client_w`, `client_h`); that `P1_SNAP_CAMERA_VALID` is set before you project; and that
the plugin is Running.

**My panel is empty.** Check that `"panel"` is not `false` and `OnPanel` is overridden. It is
called only while the panel is visible.

**The snapshot is all zeros or `flags == 0`.** The app is not attached to the game yet, or it is in
WGC mode (`P1_SNAP_WGC_MODE`, no game data).

**Buffs or the map rectangles are always empty.** They are gated (section 6): buffs come only while
AutoPot reads them, the map elements only while the radar runs.

**`entity_truncated` is not 0.** The snapshot slot (2 MiB) was full, and that many entities were
left out. It happens only in extreme scenes.

**`submit_input` returns 0.** Check the `input` permission in `plugin.json`, the user's **Allow
input** switch, `f.input_allowed` (game in the foreground, no trade or recovery automation, menu not
in use), and that the batch has at most 32 valid actions.

**Memory reads return 0.** Check the `memory` permission, that the app is attached
(`P1_SNAP_ATTACHED`), and that it is not in WGC mode.

**I cannot write a file.** Only `config_dir` is writable (section 8).

**I cannot open a URL or start a program.** The sandbox forbids starting processes. Put a "Copy
link" button in your panel that calls `p1::SetClipboard`: the clipboard is set only while the user
is using your panel (section 11).

**`p1::SetClipboard` did nothing.** It applies only while the user is using your panel (focused,
input within the last second), never while the app trades, and at most once a second (section 11).
Call it from your panel, in answer to the user.

**My log lines are not in System Logs.** Only `WARN` and `ERROR` lines go there (and at most one
plugin line a second). Every line is in your plugin's log view in the Plugins tab.

**My overlay tooltip never shows / `IsItemHovered` is always false.** The host's overlay window takes
no input, so ImGui's hover queries never fire in it. Use `ImGui::IsMouseHoveringRect` with your
item's rectangle, then `ImGui::SetTooltip` (section 7).

**Part of my overlay is missing.** The frame was cut: too many vertices or draw commands for the
frame, more than 1024 draw commands or 6 screens of fill for the app, or the app left your frame
out of one app frame because all plugins together drew too much (section 7). Draw less, and avoid
large overlapping filled shapes.

**`OpenThread` / `OpenEvent` on my own object fails with access denied.** Objects your plugin creates
can be opened again by id or name only to wait on or query them (section 2). Keep the handle you
got when you created the object.

**`SetThreadAffinityMask` fails.** Your process may run only on its quarter of the logical CPUs
(section 2); ask for a subset of `GetProcessAffinityMask`'s mask, or leave affinity alone.

**My settings were reset and a `.rejected` file appeared.** The file was not valid JSON (often a
hand edit). The broken file is kept as `<name>.rejected`; nothing overwrote it.

**Text in a Cyrillic or CJK folder fails to load or save.** You passed a UTF-8 path to an ANSI API,
or built `std::filesystem::path` from a `std::string`. Use `p1::Widen` and the W APIs (section 8).

**I changed `plugin.json` but nothing changed.** Press **Rescan**. A running plugin keeps the
manifest it was started with until it restarts.

**Can my plugin break the app, the game or another plugin?** Not by design. A crash, a hang, a leak
or garbage in shared memory stops only your plugin, nothing in the sandbox can write to the game's
or the app's memory, and no plugin can open another plugin's process. Input your plugin asks for
needs the user's consent and passes the app's safety gates. What your plugin does with the user's
files, other plugins' data folders and the network is your responsibility (section 2).
