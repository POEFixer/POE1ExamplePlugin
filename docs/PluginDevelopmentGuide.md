# POE1Fixer plugin development guide

Plugin ABI **v1** (`P1_ABI_VERSION 1`, `P1_SNAPSHOT_VERSION 1`), Dear ImGui **1.92.8** (pinned in the SDK).
Russian version: [PluginDevelopmentGuide_ru.md](PluginDevelopmentGuide_ru.md).

The authoritative contract is the C headers in `PluginSDK\include\p1sdk\` (`p1_abi.h`,
`p1_snapshot.h`, and for radar data `p1_radar.h` and `p1_terrain.h`). `p1sdk.hpp` is a header-only
C++ wrapper over them, and this guide describes the wrapper. The reference plugin is
`Plugins\ExamplePlugin\`. Start from a copy of it.

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
11. [Building a radar](#11-building-a-radar)
12. [Logging, toasts and the clipboard](#12-logging-toasts-and-the-clipboard)
13. [Debugging](#13-debugging)
14. [ABI and versioning rules](#14-abi-and-versioning-rules)
15. [FAQ and troubleshooting](#15-faq-and-troubleshooting)

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
- **Radar data**, with the `radar` permission: the current area's terrain and everything the
  app's own radar draws from, so you can build a radar of your own. See
  [section 11](#11-building-a-radar).

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
  built once per game-reader tick by one app thread and shared by all plugins. No plugin can
  change the reader's cadence or turn on any of the app's game reads, with one exception: while a
  plugin with the `radar` permission runs, the app performs the reads of its own radar, at the
  radar's cadence, exactly as when the user's radar is on (section 11). Each plugin gets at most
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
| use the clipboard directly | the Job's UI limits. Use `p1::SetClipboard` (section 12); text the user pastes into your panel arrives through the app |
| use more than its memory cap or more than 25 % of the machine's CPU, or run on more than its quarter of the logical CPUs | Job limits. Over the memory cap, allocations fail; a thread affinity outside the quarter is refused |
| change display settings or system parameters, log off the user, use global atoms, use window handles of processes outside its Job, or switch desktops | the Job's UI limits |

| A plugin CAN | Note |
|---|---|
| **read any file the Windows user can read** | low integrity stops writing up, not reading |
| **use the network** (sockets, HTTP) | not restricted |
| start its own threads | see [section 5](#5-lifecycle-and-threads) |
| read game memory, read-only, with the `memory` permission | the host holds a handle with `PROCESS_VM_READ` only |
| receive the current area's terrain, with the `radar` permission | a second shared section, mapped read-only, that the app hands only to plugins with that permission (section 11) |
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
  user's crash dump into a call stack (section 13). The example links with `/DEBUG` and
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
  Plugins\                      the user's; the app creates it only on a user action, never deletes it
    hello\                      any folder name, Cyrillic / CJK included
      plugin.json
      Hello.dll
      any-dependency.dll        dependency DLLs go NEXT TO your DLL
      data\...                  your read-only data files
      .catalog.json             only in a folder the catalog installed (its marker)
    .staging\  .trash\          the catalog installer's work folders, never listed as plugins
  Configs\plugins\
    plugins.json                the app's enable / consent switches (do not edit while it runs)
    data\
      hello\                    your ONE writable folder, named after your id, made on first start
  CrashReports\plugins\         crash dumps of plugins
```

In this repository, building `POE1ExamplePlugin.sln` writes
`x64\<Configuration>\Plugins\ExamplePlugin\` (DLL + `plugin.json`): the folder to copy into
`<POE1Fixer folder>\Plugins\`.

### Install and enable it

**Users** install the plugins of POE1Fixer's catalog from the app: **Plugins** tab, **Catalog**,
**Install**. The app downloads the plugin's release zip from GitHub, checks it against the catalog,
puts it in `Plugins\<folder>\` and turns on its **Enable** switch (it runs while **Run plugins**
is on). **Allow input** stays off until the user ticks it.
**Update to X** replaces the installed copy and keeps its **Enable** and **Allow input** switches.
**Remove** deletes the folder, keeps `Configs\plugins\data\<id>\` and the **Enable** switch, and
turns **Allow input** off: whatever build comes next under that id has to be granted input again.

**While you develop**, copy your build in by hand:

1. Copy the folder into `<POE1Fixer folder>\Plugins\`.
2. In the app, open the **Plugins** tab and press **Rescan** (the icon button with the folder tree,
   top right; its tooltip starts with "Rescan"), or restart the app.
3. Select the plugin and turn on **Enable**. A plugin placed by hand starts disabled, unless its id
   was enabled before (the app keeps the switches by id). With the `input` permission, the user
   also has to turn on **Allow input** separately.
4. The state chip goes Starting, then Running. Your panel fills the **Plugin settings** tab of the
   plugin's page, and your log lines show up in its **Recent log** tab.

The catalog never updates or removes a folder placed by hand. Only a folder the catalog installed
carries its marker `.catalog.json`, naming that folder and the plugin's id. If the catalog lists
your id, its card reads **Installed separately** and offers no Update or Remove, and on the
**Installed** page your plugin has an "Open its folder" button where a catalog plugin has
**Remove**. Do not copy your build over a folder the catalog installed: its marker stays, and the
catalog then treats the folder as its own (an update from the catalog replaces your build, and its
**Remove** deletes it). Use a folder of your own, or delete `.catalog.json` from it.

To ship a new build, disable the plugin first: the DLL stays locked while its process runs.
Replace the files, press **Rescan**, then enable it again. A running plugin keeps the manifest it
was started with until it restarts.

### Publishing a plugin in the catalog

For the POE1Fixer maintainers. The catalog is `catalog.json` in the GitHub repository
`POEFixer/POE1PluginCatalog`. The app reads it from that repository's `HEAD` over HTTPS and
installs only what it lists. Nothing is signed: a download is trusted because it comes over HTTPS
from a GitHub release of a repository of the `POEFixer` account, and its size and SHA-256 match the
catalog entry. So only plugins released from a `POEFixer/<repo>` repository can be listed; the app
skips any other entry. The tools are in the POE1Fixer source repository, under `tools/plugin_sdk/`
(Python 3.8+, standard library only, no network).

1. Build the plugin in Release (a Dbg DLL names drive-letter paths, which `package_plugin.py`
   refuses). In its `plugin.json`, `version` must be `X.Y.Z` (each part 0..99999, no leading zero)
   and `sdk` the app's `P1_ABI_VERSION`.
2. Package it: `python tools/plugin_sdk/package_plugin.py <build folder> <out.zip>`, with
   `--extra REL` for each further file. The zip is deterministic (the same files give the same
   bytes). The tool prints its size and SHA-256.
3. Publish a GitHub release of the plugin's `POEFixer/<repo>` with the tag `v` + the version
   (`v1.2.0`), and attach that zip as an asset, byte-identical.
4. Add or update the plugin's entry in `tools/plugin_sdk/catalog_sources.json` (`repo`, `tag`,
   `folder`, `asset`, `zip`, `min_app_version`, optional `changes` and `i18n`), then run
   `python tools/plugin_sdk/gen_catalog.py --out <your clone of POE1PluginCatalog>`. It takes
   every field the app shows from the zip itself and writes `catalog.json` and `README.md`.
   `--check` only compares. A plugin that lists the `radar` permission needs an app that knows
   it: an older app refuses its manifest (`unknown permission "radar"`) and its catalog reader
   skips the entry. Set `min_app_version` to the first POE1Fixer version that has radar data for
   plugins.
5. Check the result with the app's own code: run a Dbg build of the app (its version at least the
   entry's `min_app_version`) as `POE1Fixer.exe --plugincatalogcheck <catalog.json> <zip> [<zip> ...]`.
   It parses the catalog, matches each zip to its entry by size and SHA-256, extracts it the way
   the installer does and checks its `plugin.json`. The exit code is the number of failures.
6. Commit and push the catalog repository. An app fetches the catalog the first time its Plugins
   tab is shown in a session, and on **Refresh**; GitHub's file cache can lag a few minutes behind
   a push.

What the installer accepts (it checks the size and SHA-256 first, then every entry, before it
extracts a file):

- a zip of at most 64 MiB, at most 256 entries and at most 256 MiB unpacked;
- `plugin.json` and the DLL it names at the root of the zip, with `id`, `version` and `sdk` equal
  to the catalog entry's;
- regular files only, stored or deflated, not encrypted: no directory entries, links or reparse
  points;
- names of 1..240 bytes of UTF-8, relative and `/`-separated, without `\`, `:`, a control
  character or `* ? " < > |`; no empty, `.` or `..` component, no component with a leading space
  or a trailing dot or space, no Windows device name, and no two names that differ only in ASCII
  case.

`package_plugin.py` writes zips that pass, and refuses (writing nothing) a build that would not.

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
| `permissions` | array of strings | no | at most 16 entries, each `"memory"`, `"input"` or `"radar"` (below). Any other string is an error, so a plugin that needs a right this app cannot grant never runs half-working | `[]` |
| `overlay` | bool | no | `false`: your overlay is never shown and `OnOverlay` is never called | `true` |
| `panel` | bool | no | `false`: no settings panel and `OnPanel` is never called | `true` |
| `maxFps` | integer | no | 1..144, the most overlay frames per second | `60` |
| `memoryLimitMB` | integer | no | 128..4096, the Job's memory cap for your process | `1024` |

Every text field must be valid UTF-8 with no control characters.

**Permissions.** Each one is shown to the user as a chip on the plugin's card in the Plugins tab
and in the catalog:

| Permission | Constant | What it gives | Chip | Needs the user's consent |
|---|---|---|---|---|
| `"memory"` | `P1_PERM_MEMORY` | read-only reads of game memory (section 10) | Reads game memory | no |
| `"input"` | `P1_PERM_INPUT` | keyboard and mouse requests (section 9) | Keyboard and mouse | yes: **Allow input**, off by default |
| `"radar"` | `P1_PERM_RADAR` | while your plugin runs, the app performs its radar reads; you receive the area terrain and the radar data (section 11). Free whatever licence the user has | Map and radar data | no |

`"radar"` exists only in apps that ship the radar SDK (`p1_radar.h`, `p1_terrain.h`). An older app
refuses a manifest that lists it, with `unknown permission "radar"`, so publish such a plugin with
the catalog's `min_app_version` set (section 3).

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
5. With the `radar` permission: if the app published a new area terrain, copy it privately and
   queue `P1_EV_TERRAIN_CHANGED` (section 11).
6. Call `OnEvent` once per event, in that order.
7. Call `OnTick` if a new snapshot arrived or 100 ms have passed since the last tick.
8. Call `OnOverlay` if an overlay frame is due, then `OnPanel` if a panel frame is due.
9. Publish the frames.

The loop sleeps until the app wakes it (a new snapshot, input, a control change) or the next frame
is due, and never longer than 250 ms.

| Callback | When it runs |
|---|---|
| `OnEvent` | before the `OnTick` of the same iteration, once per event |
| `OnTick` | on every new snapshot (the game reader's cadence: about 30 per second, about 60 while the radar reads run, that is while the app's radar is on or a plugin with the `radar` permission runs; nothing else a plugin does changes it), and at least 10 times a second otherwise, for example while detached. Check `snapshot->publish_seq` to tell whether the data is new |
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
| `P1_EV_TERRAIN_CHANGED` | a = the terrain's `serial` | `radar` permission only: a new area terrain arrived and `P1Frame::terrain` now points at it. Rebuild what you derive from it (section 11). Needs an app with radar data |

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
- The host functions `Log`, `Notify`, `SetClipboard`, `RequestFrames`, `NowUs`, `Memory::*`,
  `Input::*` and the terrain references (`p1::TerrainRef`, section 11) may be called from **any**
  thread. The `Texture::*` functions work **only on the plugin thread**: elsewhere they return 0 /
  `false` and do nothing.
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
| `terrain` | appended field: test `P1_HAS_FIELD(&f, P1Frame, terrain)` first, an older host has none. The newest area terrain (`radar` permission only), null before the first one and without the permission. Valid until the callback returns; while a new area's terrain is still being built it is the PREVIOUS area's, so read it through `p1_terrain_current(&f)` (section 11) |

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
| `large_map`, `mini_map` | the game's two map elements (`P1MapElement`, below). **Gated: radar** (`P1_SNAP_MAP_VALID`) |
| `latency_ms`, `gold`, `time_in_game_ms`, `network_state`, `league` | server values (`league` is a string) |
| `inv_cols`, `inv_rows`, `inv_occupied` | the main inventory grid (12 x 5) and its occupancy bits, bit `y * cols + x`. Valid with `P1_SNAP_INVENTORY_VALID` |
| `entity_count/stride/offset`, `entity_truncated` | the entity array (below). `entity_truncated` is the number of entities left out because the snapshot was full |
| `buff_count/stride/offset`, `item_count/stride/offset` | the buff and inventory-item arrays |
| `string_offset`, `string_bytes` | the string pool |
| `radar_offset`, `radar_bytes` | appended fields: present only when `P1_SNAP_HAS(s, radar_bytes)`. Where the radar block (`P1Radar`) is; 0 = none. Written while a plugin with the `radar` permission runs. Read it with `p1_radar_get(&s)` or `p1::Radar::Get(s)`, which test all of this (section 11) |

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
| `P1_SNAP_RADAR_VALID` | the radar reads ran this tick, so the radar block's data is fresh (section 11). Never set by an app without radar data |

`flags == 0` means no game data at all: not attached yet, or the very first callback.

**Gated fields.** Some reads cost the app time, so it does them only while one of its own
features needs them. Plugins get these fields as a by-product. The one set of reads a plugin can
turn on is the radar's: a running plugin with the `radar` permission makes the app perform them
exactly as for its own radar (section 11). Always test the flag or the "unknown" value, and
degrade gracefully:

| Field | Filled only while |
|---|---|
| player buffs (`P1_SNAP_BUFFS_VALID`, `p1::PlayerBuffs`) | AutoPot reads them (it is enabled and running) |
| `large_map`, `mini_map` (`P1_SNAP_MAP_VALID`) | the radar reads run: the user's radar is on, or a plugin with the `radar` permission runs |
| `P1Entity::targetable` (monsters only, 0xFF otherwise) | the radar reads run. A value the radar already cached keeps being published |
| `P1Entity::minimap_icon`, `transition_target`, `item_path` | the radar reads run; cached values keep being published |
| the radar block (`p1_radar_get`) and its arrays | your plugin has the `radar` permission (the host removes the block for any other plugin); its data is fresh on ticks with `P1_SNAP_RADAR_VALID` |

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
`size_base_x/y`, `scale_rel`, `shift_x/y`, `default_shift_x/y`, `zoom`. The units are mixed, the
way the game keeps them:

- `pos_base` and `size_base` are in the game's UI base units (2560 x 1600, fitted to the height):
  pixels = base x `client_h` / 1600.
- The **minimap** is a real rectangle: `pos_base` is its top-left corner, `size_base` its size.
- The **large map** is not: its `pos_base` is the map's **centre**, and its `size_base` reads 0 x 0
  (the radar block says so with `P1_RADAR_LARGE_POS_IS_CENTRE`).
- `shift_x/y` and `default_shift_x/y` are the game's own sub-map offsets in **client pixels**, not
  base units.
- `scale_rel` is the element's scale relative to the UI root (multiply by `client_h` / 1600 for
  pixels per base unit); `zoom` is the sub-map zoom, 0 = unusable.

Do not do this math by hand: `p1::Radar::LargeMap(f)` / `p1::Radar::MiniMap(f)` (C:
`p1_radar_large_map`, `p1_radar_mini_map`) return the transform the app's own radar uses, with
the user's calibration (section 11).

### Arrays: entities, buffs, inventory items

**Never index the arrays as C arrays.** Always go through the stride, with `p1::Entities(s)` /
`p1::PlayerBuffs(s)` / `p1::InventoryItems(s)` or `p1_entity_at` / `p1_buff_at` / `p1_item_at`,
which also bounds-check. Which element structs may still grow, and why `P1Entity` may not, is in
section 14.

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

While the radar block is written, a parallel array of `P1EntityRadar` describes each entity the
way the app's radar sees it (its category, icon cells, float grid position, terrain height):
element i belongs to entity i. Read it with `p1::Radar::Entities(s)` or `p1_entity_radar_at(&s, i)`
(section 11). Each entity then costs 20 more bytes of the snapshot slot.

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
to the arrays. `radar_offset` and `radar_bytes` are such fields: the header grew from 528 to 536
bytes, and `p1_radar_get` returns null on an app that wrote 528. Element structs only grow at the
end; that is why strides exist. A plugin built against snapshot version N runs on any app that
provides version N or later. The rules for the element sizes are in section 14.

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

- Drawn inside the app's **Plugins** tab, in the **Plugin settings** tab of your plugin's page:
  it fills the card below the plugin's header, never less than 330 px high at 100 % UI scale.
  **Coordinates are panel-local pixels**, and the size is `f.panel_w` x `f.panel_h`; prefer
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
- An image as big as an area's terrain has its own helper, `p1::TerrainTexture`, which builds it
  off the plugin thread and keeps it inside these limits (section 11).

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

## 11. Building a radar

With the `radar` permission a plugin gets everything the app's own radar draws from, so it can
draw a complete radar of its own: the area's walkable outline and relief, entity icons, exits and
quest state, on the game's large map, on its minimap or in a panel of your own. It is **free**: it
works whatever licence the user has (the app's built-in Radar stays a paid feature). The headers
are `p1_radar.h` (the radar block and the projection) and `p1_terrain.h` (the area terrain);
`p1sdk.hpp` wraps both in `p1::Radar`, `p1::TerrainRef` and `p1::TerrainTexture`.

The SDK Example's lesson 24, **"Build a radar"** (`maprad`), is the full worked example: every piece
of this section with live data and code to copy.

### What the permission does

In `plugin.json`, add `"radar"` to `permissions` (next to any others you need) and give the
process room: `"memoryLimitMB": 512` or more (see the last point).

- **The app performs its radar reads while your plugin runs:** the map elements, the minimap
  icons, the exit targets, the ground-item paths, `targetable`, the quest log, the quest list and
  the area terrain, exactly as for its own radar, at the radar's cadence (snapshots about 60 times
  a second). The terrain is then built in towns and hideouts too: your plugin decides what to draw
  there. Never in WGC mode. A plugin cannot make the app read more than that, and it never makes
  the app's built-in radar draw or do work, nor moves the progress of its quest guide.
- **Your process receives the area terrain** (`P1Frame::terrain`, `P1_EV_TERRAIN_CHANGED`,
  `p1::TerrainRef`) through a second read-only shared section. The app hands that section only to
  plugins with the permission: there is no other way to get the terrain.
- **Your snapshot carries a radar block** (`P1Radar`). The app writes it while at least one running
  plugin has the permission, and the plugin host removes it from the snapshot of every plugin that
  does not have the permission: without `"radar"`, `p1_radar_get` returns `NULL` even while another
  plugin's radar runs. (The v1 per-entity fields such as `minimap_icon` are filled whenever the
  radar reads run, for every plugin, as before.)
- **No consent is needed.** The Plugins tab shows the permission as a "Map and radar data" chip.
- **It needs an app with radar data.** An older app refuses the manifest (`unknown permission
  "radar"`, the plugin is Invalid). Set the catalog entry's `min_app_version` (section 3).
- **Ask for memory.** Your process holds a private copy of each area's terrain (often 10 to
  30 MB, up to 64 MiB), the terrain image while it is built, and the texture data on its way to
  the app. 512 MB is a sound `memoryLimitMB`; the default 1024 is fine too.

### What arrives, and when

| Piece | Where | Read it with |
|---|---|---|
| radar block `P1Radar`: gate, stages, projection constants, the user's map calibration, the player, the icon atlas | the snapshot (`radar_offset`) | `p1::Radar::Get(s)`, `p1_radar_get(&s)` |
| per-entity `P1EntityRadar` | parallel to the entity array | `p1::Radar::Entities(s)`, `p1_entity_radar_at(&s, i)` |
| quest log `P1QuestState` | the snapshot | `p1::Radar::Quests(s)`, `p1_quest_at`, `p1_quest_find` |
| quest list `P1QuestTrackerEntry` | the snapshot | `p1::Radar::Tracker(s)`, `p1_tracker_at` |
| area terrain `P1Terrain` | `P1Frame::terrain`, `radar` plugins only | `p1_terrain_current(&f)`, `p1::TerrainRef`, `p1::TerrainTexture` |
| `P1_EV_TERRAIN_CHANGED` (a = serial) | `OnEvent`, `radar` plugins only | |

**The gate.** `P1Radar::gate` says whether the reads ran, and why not:

| `gate` | Meaning |
|---|---|
| `P1_RGATE_ON` | the reads ran this tick; `P1_SNAP_RADAR_VALID` is set |
| `P1_RGATE_NOT_IN_GAME` | wanted, but the game is not in an area: login, character select, a loading screen |
| `P1_RGATE_WGC` | the app is in visual (WGC) mode: no game memory at all |
| `P1_RGATE_OFF` | nobody wants them (for example just before the app has noticed your plugin) |

`P1Radar::flags` adds who asked (`P1_RADAR_BY_APP`, `P1_RADAR_BY_PLUGIN`), `P1_RADAR_TERRAIN_REUSED`
(the terrain was re-used after a portal round trip), the quest-log flags (below), and two
projection switches (`P1_RADAR_LARGE_POS_IS_CENTRE`, `P1_RADAR_SHIFT_IN_BASE_UNITS`) that the
transform helpers read for you.

**The stages** mirror the app's own radar status:

| Field | Values |
|---|---|
| `terrain_stage` | `P1_TSTAGE_OFF`, `READING`, `COMPOSING` (heights being combined off-thread), `READY`; `DEFERRED` while the app's trade automation holds the reads; failures `HEADER_BAD`, `SHAPE_REJECTED`, `READ_FAILED`. Progress: `sub_tiles_read` / `sub_tiles_total`, `tgt_read` / `tgt_total`, `terrain_build_ticks`, `terrain_ticks_deferred` |
| `map_ui_stage` | `P1_MAPUI_OK`; `DEFERRED` = a trade holds the reads and the last state is carried: use it like OK; `OFFSETS_PENDING`, `MAP_ELEMENT_BAD`, `OFF` |
| `quest_stage` | `P1_QSTAGE_OK`; `UI_BAD`, `NOT_FOUND`, `WALK_FAILED`, `DEFERRED`, `OFF` |
| `tracker_stage` | `P1_TRSTAGE_OK`; `WAITING` (the quest log is not read yet), `NO_ELEMENT`, `HIDDEN`, `DEFERRED`, `OFF` |

**Entering an area.**

1. `P1_EV_AREA_CHANGED` arrives and `snapshot->area` describes the new area. The block's
   `terrain_serial` reads 0 while the new terrain is built, and `terrain_stage` moves through
   `READING` and `COMPOSING`.
2. Some seconds later the build is done: `terrain_stage` is `READY` and `terrain_serial` names the
   new terrain.
3. Shortly after, the terrain reaches your process: `P1_EV_TERRAIN_CHANGED` with a = its serial, and
   `P1Frame::terrain` points at it.

Until step 3, `P1Frame::terrain` still points at the **previous** area's terrain. Use
`p1_terrain_current(&f)`: it returns the frame's terrain only when its serial equals the block's
`terrain_serial`, and null otherwise, so you never draw the old area's walls over the new area.
A portal round trip (map, hideout, map) can re-adopt an earlier terrain without reading it again
(`P1_RADAR_TERRAIN_REUSED`): its serial comes back and the terrain arrives again. Serials are unique
per build but do not only grow: compare them for equality. Match a terrain on its serial, never on
`area_hash` (a re-used terrain keeps the hash it was built with).

### The terrain (`P1Terrain`)

One `P1Terrain` per area, built once by the app's game reader and never changed afterwards. The
host has validated every offset, count and size before you see it.

- **Grid.** x to the right, y down, in the game's grid cells. One cell is 250/23 world units
  (`P1Radar::world_per_cell`); one tile is 23 x 23 cells (`cells_per_tile`); the area is `tiles_x`
  x `tiles_y` tiles. `area_id` is the WorldArea id when it was built, `build_ms` how long the app
  took (diagnostic).
- **Walkable grid** (the game's movement layer): `walk_width` x `walk_height` cells, packed two per
  byte, row-major, `walk_bytes_per_row` bytes a row, **even x in the low nibble**. Value 0 =
  blocked, 1 = walkable next to an obstacle, 2 or more = walkable interior. Read a cell with
  `p1_terrain_cell(t, x, y)` (0..15, 0 outside the grid) or `p1_terrain_walkable(t, x, y)`.
  `walk_bytes` 0 means the grid was left out.
- **Heights:** one value per cell, row-major, `height_width` x `height_height` = `tiles_x*23` x
  `tiles_y*23`. This grid can be **0..2 cells narrower** than the walkable one (walkable rows are
  padded to whole bytes), so never index one grid with the other's width. The values are world
  units, **negated**: higher ground is more negative, the game's own convention. `height_format`:
  `P1_HEIGHT_NONE` (flat, no grid), `P1_HEIGHT_I16` (int16 per cell, height = value x
  `height_scale`, exact), `P1_HEIGHT_F32` (float per cell, `height_scale` 1). The app writes int16
  whenever that is lossless, so both give you the app's values bit for bit. Read with
  `p1_terrain_height_at(t, x, y)`, or `p1_terrain_height(t, gx, gy)` for a float position (the cell
  it truncates into; the app's radar uses exactly this). `P1_TERRAIN_SUBTILE_HEIGHTS` says the
  heights carry per-cell detail; without it every tile is a flat step.
- **TGT tiles** (the tile art): `tgt_count` records `P1TerrainTgt` {`tile_x`, `tile_y`, `key_x`,
  `key_y`, `file`}, read with `p1_terrain_tgt_at(t, i)`. A record covers the grid box
  `[tile*23, tile*23 + 23)` on each axis. `p1_terrain_tgt_key(t, g, buf, cap)` writes the app's own key for the
  tile, `"<file>:x:<key_x>-y:<key_y>"`, the format the app's radar targets and quest-guide exits are
  written in. With `P1_TERRAIN_TGT_RAW_KEYS`, some keys did not parse: such a record has `key_x` =
  `key_y` = 0xFF and `file` holds the whole key, which `p1_terrain_tgt_key` then returns as it is.
  Tile names are hints, not truth: a wall decoration can carry an "entrance" name.
- **Rooms** (what the level generator placed): `room_state` is `P1_ROOMS_OK`, `P1_ROOMS_NOT_READ` or
  `P1_ROOMS_FAILED` (`rooms_reason` says why). `room_count` records `P1TerrainRoom` {`name` (the
  `.arm` path), `min_x`, `min_y`, `max_x`, `max_y`}, read with `p1_terrain_room_at(t, i)`. The
  bounds are **inclusive tiles**: the grid box is `[min*23, (max+1)*23)` on each axis.
- **Strings** live in the terrain's own pool: read them with `p1_terrain_str(t, off)`, never with
  `p1_str_get`.
- **Parts left out.** The terrain travels through one 64 MiB slot. The Coast (84 x 84 tiles) takes
  about 9.4 MB and a 150 x 150-tile map about 30 MB; the slot holds about 225 x 225 tiles with
  heights. A bigger area loses parts, in this order, each with its flag: the heights
  (`P1_TERRAIN_HEIGHTS_OMITTED`), then the TGT records and the rooms (`P1_TERRAIN_TGT_OMITTED`,
  `P1_TERRAIN_ROOMS_OMITTED`), then the walkable grid (`P1_TERRAIN_WALK_OMITTED`). Test the counts
  and sizes, never assume a part is there.

### Holding a terrain on your own thread

`P1Frame::terrain` is valid until the callback returns (the host replaces it only between
callbacks). To use a terrain on a thread of your own, take a reference: `p1::TerrainRef::HoldNewest()`
(C: `terrain_hold`) returns the newest terrain with one reference taken, valid on **any** thread
until the `TerrainRef` is reset or destroyed (C: one `terrain_release` per `terrain_hold`). It is
empty without the permission, before the first terrain and on an app without radar data. The
newest terrain can already be newer than the one you looked at, so compare `serial`. Every terrain
you hold counts against your `memoryLimitMB`: drop the old reference when `P1_EV_TERRAIN_CHANGED`
arrives. Releasing a pointer the host never gave you does nothing.

```cpp
// OnTick: analyse each area's terrain once, on a thread of your own.
void OnTick(const P1Frame& f) override {
    if (m_job.valid() && m_job.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
        m_exits = m_job.get();                                   // your own result type
    const P1Terrain* cur = p1_terrain_current(&f);
    if (!cur || cur->serial == m_analysed || m_job.valid()) return;
    auto held = std::make_shared<p1::TerrainRef>(p1::TerrainRef::HoldNewest());
    if (!*held || (*held)->serial != cur->serial) return;       // a newer one is on its way
    m_analysed = cur->serial;
    m_job = std::async(std::launch::async, [held] { return FindExits(*held->get()); });
}
```

### The projection

The app's radar maps a grid position onto the screen like this (`p1_radar.h`):

```
dx = g.x - player.x;  dy = g.y - player.y;  z = dz_world * height_to_grid
screen.x = cx + scale * ((dx - dy) * cos(camera_angle))
screen.y = cy + scale * ((z - (dx + dy)) * sin(camera_angle))
```

(`cx`, `cy`) is where the player is drawn, `scale` the pixels per grid unit, and `dz_world` the
height to draw at minus the player's height, both negated world units, so higher ground lifts the
point. A `P1MapTransform` holds `cx`, `cy`, `scale` and the angle; `valid` is 0 when it cannot be
used. Get one from:

| Where you draw | Transform | Notes |
|---|---|---|
| the game's large map (the full-screen map) | `p1::Radar::LargeMap(f)`, C `p1_radar_large_map(s, client_h)` | invalid while the map is closed |
| the game's minimap (top right) | `p1::Radar::MiniMap(f)`, C `p1_radar_mini_map(s, client_h)` | clip to `p1::Radar::MiniMapClip(f, min, max)` (C `p1_mini_map_clip`) |
| a square panel of your own | `p1_panel_rect(client_w, client_h, fx, fy, fsize, rect)` + `p1_panel_transform(rect, zoom, px_per_base, &k)` | the app radar's panel: side = `fsize` x `client_h` (capped to the client), top-left (`fx` x `client_w`, `fy` x `client_h`) kept inside the client; `zoom` x `px_per_base` x 2 pixels per grid unit |
| anything else | `p1_make_transform(cx, cy, scale, &k)` | |

The two map transforms are invalid while `P1_SNAP_MAP_VALID` is not set. They apply the user's
**calibration**: the corrections set in the app's Radar tab for each game map (`large_cal_scale`,
`large_cal_dx`, `large_cal_dy`, `mini_cal_*` in the block; neutral = scale 1, offsets 0; offsets in
client pixels). With your own map element math, pass them to `p1_large_map_transform(m,
px_per_base, cal_scale, cal_dx, cal_dy, &k)` / `p1_mini_map_transform`. `px_per_base` is always
`client_h / ui_base_height`.

Apply a transform with `p1::Radar::ToMap(t, me, gx, gy, height)` (C:
`p1_grid_to_map(&t, pgx, pgy, gx, gy, dz_world, &sx, &sy)`): pass the terrain height at the point,
and `ToMap` subtracts the player's. The inverse, at height 0, is `p1_map_to_grid(&t, pgx, pgy, sx,
sy, &gx, &gy)`: for example the cell under the cursor.

**Constants.** `p1_radar_consts(r)` returns the app's own values from the block: `camera_angle`,
`height_to_grid`, `world_per_cell`, `map_scale_factor`, `large_center_mul_x`, `ui_base_height` and
the two projection switches. With `r` null (an app without radar data) it returns built-in
defaults, the same values. Read them from the block rather than hard-coding them: two of them
(`map_scale_factor` and the large map's centre multiplier) are the app's best estimates, which is
what the calibration corrects, and a later app may refine them.

**The player.** `p1::Radar::PlayerOf(f, &motion)` returns a `p1::Radar::Player` {`gx`, `gy`,
`height`, `valid`}: the block's `player_grid_x` / `player_grid_y` (the player as the app's radar
centres on it, read from the full entity list, so a truncated entity array never loses it), the
height from `p1_terrain_current` when there is one (else `player_height`), extrapolated with
`motion` (pass `nullptr` for the raw position). `player_entity_index` is the player's row in the
entity array, 0xFFFFFFFF = none.

**Entities** carry their float grid position and the terrain height under it in `P1EntityRadar`
(`grid_x`, `grid_y`, `height`). For a bare `P1Entity`, `p1_entity_grid(e, world_per_cell, &gx, &gy)`
gives the same position (`render_pos` / `world_per_cell`, else the integer grid) and
`p1_terrain_height(t, gx, gy)` the height.

**Player motion.** The game reader samples the player 30 to 60 times a second, but the game
scrolls its map every game frame. `p1::Radar::PlayerMotion` closes that gap: `PlayerOf` feeds it
each snapshot's position and predicts the position at `f.now_us`. Its rule is the app radar's: a
velocity from two samples 5 to 120 ms apart and at most 12 cells apart, a horizon of
min(1.25 x the sample interval, 50 ms), a sample older than 250 ms used as it is, and a fresh start
on an area change. The prediction only shows when you build overlay frames between snapshots:
`p1::RequestFrames(fps)`, capped by your `maxFps`, and only while a map is on screen.

### Drawing the terrain

`p1::TerrainTexture` does all of it the app radar's way:

- `Update(f)` on the plugin thread (`OnTick`). Cheap when nothing changed. When the current area's
  terrain arrives, it holds it, builds the image on a background thread, and uploads it on a later
  `OnTick`. It frees the previous texture before creating the new one (two area-sized textures may
  not fit the budget together); when the budget is still full it tries again 2 s later, and an image
  finished for an area you have already left is dropped.
- `Draw(dl, f, t, me, tint)` in `OnOverlay` lays the image under the map with the corners the app
  uses (one `AddImageQuad`). It draws only while the texture is the current area's and returns
  `false` otherwise. The image is flat: the player's height moves it, and the per-cell relief is
  baked into the pixels by `lift_by_height`.
- `P1TerrainStyle` {`color` (`IM_COL32` layout), `border` (square brush radius in cells, 0..3),
  `filled` (0 = the outline, the walls; 1 = every walkable cell), `lift_by_height` (1 = shift each
  cell by its height, the way the game map shows it)}. The default is `{0xFFFFFFFF, 0, 0, 1}` (white walls,
  no border, lifted): choose the colour with `Draw`'s tint. `SetStyle` rebuilds the image. `Release()` frees the texture after
  waiting for a running build; the destructor calls it.

One level down, `p1::BuildTerrainImage(t, style, image, maxBytes)` fills a `p1::TerrainImage`
{`serial`, `width`, `height`, `cellsPerTexel`, `rgba`}, and in C `p1_terrain_image_size(t, ds, &w,
&h)` + `p1_terrain_pixels(t, &style, ds, rgba, w, h)` paint into your own zero-filled buffer. Both
are pure CPU: run them on any thread while you hold the terrain.

- **Downscaling.** `BuildTerrainImage` tries 1, 2, 3, ... cells per texel until each side is at
  most 4096 texels and the image at most `maxBytes` (24 MiB by default); it fails past 64. A texel
  is painted when **any** cell it covers is painted, so thin walls survive a downscale. Texel
  (i, j) covers cells `[i*ds, (i+1)*ds)` x `[j*ds, (j+1)*ds)`, so the image spans grid (0, 0) to
  `(width*ds, height*ds)`. At one cell per texel the pixels are byte-identical to the
  app radar's own texture.
- **Texture limits** (section 7): 4096 x 4096 per texture and **64 MiB of live textures per plugin,
  ImGui's font atlases included**. A full 4096 x 4096 image would be the whole budget, hence the
  24 MiB default. Budget the icon atlas too (below).
- **Upload pacing.** The app uploads at most about 2 MiB per rendered frame across all plugins, so
  a 24 MiB terrain image takes about a dozen frames to arrive, and a frame that draws a new texture
  is shown only after its data arrived: your previous frame stays on screen meanwhile. Build once
  per area, never per frame.
- Drawing the grid cell by cell (`AddRectFilled` per walkable cell) is far over the per-frame
  vertex limit on any real area: use the texture.

### Entities and icons

`P1EntityRadar` element i describes entity i (`p1_entity_at(&s, i)`). `p1::Radar::Entities(s)` is
empty when there is no block. Its fields:

| Field | Meaning |
|---|---|
| `category` | the app radar's classification, `P1_RCAT_*` (`SELF`, `PLAYER`, `MINION`, the four monster rarities, `ROGUE_EXILE`, `NPC`, the chests and `STRONGBOX`, `SHRINE`, `AREA_TRANSITION`, `WAYPOINT`, `CHECKPOINT`, `PORTAL`, the league mechanics, `QUEST_OBJECT`, `QUEST_ITEM`, `GAME_ICON`). `P1_RCAT_NONE` (0xFF) = the app's radar does not draw it. The list only grows: treat a value you do not know as unknown |
| `group` | `P1_RGROUP_*` of the category: `PLAYERS`, `MONSTERS`, `NPCS`, `CHESTS`, `WORLD`, `LEAGUES`, `QUEST`, or `NONE` |
| `flags` | `P1_ERAD_HIDDEN` (the app's default filters hide it right now: an opened chest, a dead or untargetable monster), `P1_ERAD_GAME_ICON` (the app draws it with its own game minimap icon, `game_icon_cell`), `P1_ERAD_HAS_POSITION`, `P1_ERAD_LOCAL_PLAYER` |
| `game_icon_cell` | the entity's game minimap icon in the atlas, `P1_ICON_CELL_NONE` (0xFFFF) = none |
| `style_icon_cell` | the app's default icon for the category (not the user's own radar styles), or none |
| `grid_x`, `grid_y`, `height` | float grid position and the terrain height under it |

To read both arrays together, walk the index:

```cpp
const P1Snapshot& s = *f.snapshot;
for (uint32_t i = 0; i < s.entity_count; ++i) {
    const P1Entity* e = p1_entity_at(&s, i);
    const P1EntityRadar* r = p1_entity_radar_at(&s, i);
    if (!e || !r || r->category == P1_RCAT_NONE) continue;
    if (r->group == P1_RGROUP_MONSTERS && e->rarity == 3) { /* a unique monster */ }
}
```

`p1_radar_category_key(category)` returns the category's stable name (`"MonsterRare"`), the key the
app uses in its own config: use it as the key of per-category settings in your settings file.

**The icon atlas** is a PNG under the app folder: `AppDir()` + `\` + the block's `atlas_path`
(`"Resources/radar/icons.png"`; the `/` works in Windows file APIs). It is `atlas_w` x `atlas_h`
pixels of square `atlas_cell_px` cells, `atlas_columns` to a row; cell n sits at column
n % `atlas_columns`, row n / `atlas_columns`. Read the sizes from the block, never hard-code them: the
atlas changes with the app. The SDK has no image decoder: decode the PNG yourself (a PNG decoder
compiled into your DLL, or WIC) on a thread of your own, then upload it with `p1::Texture::Create`
on the plugin thread. The whole atlas (896 x 3392 today) is about 11.6 MiB of your 64 MiB texture
budget, so with a terrain texture next to it consider copying only the cells you draw into a
smaller texture.

```cpp
// The UV rectangle of an atlas cell (game_icon_cell / style_icon_cell); false = no icon.
bool CellUv(const P1Radar& r, uint16_t cell, ImVec2& uv0, ImVec2& uv1) {
    if (cell == P1_ICON_CELL_NONE || r.atlas_columns == 0 || r.atlas_w == 0 || r.atlas_h == 0) return false;
    const uint32_t x = (cell % r.atlas_columns) * r.atlas_cell_px, y = (cell / r.atlas_columns) * r.atlas_cell_px;
    if (x + r.atlas_cell_px > r.atlas_w || y + r.atlas_cell_px > r.atlas_h) return false;
    uv0 = ImVec2(float(x) / r.atlas_w, float(y) / r.atlas_h);
    uv1 = ImVec2(float(x + r.atlas_cell_px) / r.atlas_w, float(y + r.atlas_cell_px) / r.atlas_h);
    return true;
}
// The atlas file, once the radar block is there. A UTF-8 path: widen it for the W APIs (section 8).
if (const P1Radar* rb = p1::Radar::Get(s)) {
    std::wstring path = p1::Widen(AppDir());
    if (!path.empty() && path.back() != L'\\') path.push_back(L'\\');
    path += p1::Widen(p1::Str(s, rb->atlas_path));
    // decode it on your own thread, then p1::Texture::Create on the plugin thread
}
```

### The quest log and the quest list

- **Quest log:** `P1QuestState` {`id` (the Quest.dat id, `"a1q1"`), `state` (0 = complete, -1 = not
  started, else the order of the state the quest is in)}, sorted by id. `p1_quest_find(&s, "a1q1",
  &state)` finds one (0 = the log has no such quest); `p1::Radar::Quests(s)` walks them all.
  `quest_serial` changes whenever the content changes. `P1_RADAR_QUESTS_USABLE` = the log is
  settled and belongs to the current character (the app's own guide uses it only then);
  `P1_RADAR_QUESTS_PROVISIONAL` = the first read for this character, not settled yet: right after
  a character switch the game can still hold the previous character's log.
- **Quest list** (the game's list of tracked quests on the right of the screen):
  `P1QuestTrackerEntry` {`id`, `min_x`, `min_y`, `max_x`, `max_y`}, top to bottom. The box is in UI
  base units: pixels = base x `client_h` / `ui_base_height`. An entry that an open game window
  covers is left out, and the list is not read in hideouts (the game shows none there).
  `tracker_serial` changes with the content.

### Performance and budgets

- **The app's side** is the radar's own reads at the radar's cadence, nothing more: the terrain is
  built once per area (in steps, and it waits while the app trades), exported once per area and
  copied once into your process. While the app's trade automation runs, the radar reads wait
  (`DEFERRED` stages) and the last map state is carried.
- **Your frame** carries at most 150,000 vertices, and the app draws at most **1024 draw commands**
  and 6 screens of fill of it (section 7). A radar is a few hundred icons, which fits if you
  batch: every change of texture or clip rectangle starts a new draw command, so draw in passes
  (the terrain quad, then all shapes, then all atlas icons, then text) instead of shape, icon and
  text per entity, and push the minimap's clip rectangle once around everything.
- **Skip what is off the map** before you add it, and keep shapes cheap: a small `AddRectFilled` is
  4 vertices, a circle with the default segment count many more.
- **Frames.** By default an overlay frame is built on every new snapshot. Call
  `p1::RequestFrames(N)` only while a map is open, and 0 again when it closes.
- **Threads.** Build images and analyse terrains off the plugin thread (`p1::TerrainTexture` and
  `p1::TerrainRef` above): a callback that blocks for 5 s kills the plugin.

### A minimal radar

`plugin.json`:

```json
{
  "id": "mini_radar",
  "name": "Mini radar",
  "version": "1.0.0",
  "dll": "MiniRadar.dll",
  "sdk": 1,
  "permissions": ["radar"],
  "panel": false,
  "memoryLimitMB": 512
}
```

The plugin: the area's walls and a dot per entity, on the large map while it is open and on the
minimap otherwise.

```cpp
#include <p1sdk/p1sdk.hpp>

class MiniRadar final : public p1::Plugin {
public:
    void OnTick(const P1Frame& f) override {
        m_terrain.Update(f);                  // a new area: builds the image off-thread, uploads it later
    }

    void OnOverlay(const P1Frame& f) override {
        const P1Snapshot& s = *f.snapshot;
        if (!(s.flags & P1_SNAP_IN_GAME) || !(s.flags & P1_SNAP_RADAR_VALID)) return;

        ImDrawList* dl = ImGui::GetBackgroundDrawList();
        P1MapTransform t = p1::Radar::LargeMap(f);              // the large map, while it is open
        const bool mini = !t.valid;
        if (mini) {                                             // else the minimap, clipped to it
            ImVec2 clipMin, clipMax;
            t = p1::Radar::MiniMap(f);
            if (!t.valid || !p1::Radar::MiniMapClip(f, clipMin, clipMax)) return;
            dl->PushClipRect(clipMin, clipMax, true);
        }

        const p1::Radar::Player me = p1::Radar::PlayerOf(f, &m_motion);
        if (me.valid) {
            m_terrain.Draw(dl, f, t, me, IM_COL32(150, 200, 255, 150));   // the walls: one draw command
            const float r = 3.0f * (f.viewport.ui_scale > 0.0f ? f.viewport.ui_scale : 1.0f);
            for (const P1EntityRadar& e : p1::Radar::Entities(s)) {       // then every dot
                if (e.category == P1_RCAT_NONE || !(e.flags & P1_ERAD_HAS_POSITION) ||
                    (e.flags & (P1_ERAD_HIDDEN | P1_ERAD_LOCAL_PLAYER))) continue;
                const ImVec2 p = p1::Radar::ToMap(t, me, e.grid_x, e.grid_y, e.height);
                dl->AddRectFilled(ImVec2(p.x - r, p.y - r), ImVec2(p.x + r, p.y + r), ColorOf(e.group));
            }
            dl->AddCircleFilled(ImVec2(t.cx, t.cy), r * 1.5f, IM_COL32(255, 255, 255, 255));   // you
        }
        if (mini) dl->PopClipRect();
    }

private:
    static ImU32 ColorOf(uint8_t group) {
        switch (group) {
            case P1_RGROUP_MONSTERS: return IM_COL32(240, 80, 70, 230);
            case P1_RGROUP_CHESTS:   return IM_COL32(240, 200, 60, 230);
            case P1_RGROUP_WORLD:    return IM_COL32(90, 200, 255, 230);
            case P1_RGROUP_QUEST:    return IM_COL32(120, 240, 120, 230);
            default:                 return IM_COL32(200, 200, 200, 200);
        }
    }
    p1::TerrainTexture m_terrain;             // white walls with the relief baked in, tinted in Draw
    p1::Radar::PlayerMotion m_motion;         // keeps the centre moving between snapshots
};

P1_PLUGIN(MiniRadar, "mini_radar", "Mini radar", "1.0.0", "you", "A minimal radar on the game's maps.")
```

The SDK Example's lesson 24, "Build a radar" (`maprad`), goes further on the same foundations.

## 12. Logging, toasts and the clipboard

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

## 13. Debugging

### The Plugins tab

The tab has two pages, switched at its top left: **Installed** and **Catalog** (a badge on Catalog
counts the updates it offers). At the top right are **Run plugins** (every plugin on or off) and
two icon buttons that name themselves in their tooltips: open the plugins folder, and **Rescan**
(the folder tree).

**Installed** lists the plugins on the left, in a narrow column (150 to 190 px at 100 % UI scale).
A row shows the name, then the **state chip** and the installed version, or the update the catalog
offers. The selected plugin fills the rest of the page:

- a compact **header card**: the name, version, author and permission chips on one row (Reads game
  memory, Keyboard and mouse, Map and radar data, Draws over the game, Settings panel; each explains
  itself in a tooltip), the description on one line (all of it in the tooltip), then **Enable**,
  **Allow input** (only with
  the `input` permission), **Restart**, **Update to X** or **Repair** when the catalog offers one,
  and **Remove** (a plugin the catalog installed) or "Open its folder" (a plugin placed by hand).
  Below them: the **state detail**, the restart countdown, the crash count of the last 10 minutes,
  and this session's install, update or removal (its progress, that it is done, or why and when it
  failed);
- the **main card**, with three tabs: **Plugin settings** (your panel, shown first), **Recent log**
  (the newest 200 lines: your own lines and the app's lines about your plugin) and **Resource use**
  (the telemetry). Without your panel on screen, the settings tab says that the plugin has no
  settings page, that **Run plugins** is off, or that its settings appear there while it runs,
  with an **Enable** button when the plugin is disabled.

**Catalog** shows **Refresh**, the time of the last check, and one card per catalog plugin with its
action: **Install**, **Update to X**, **Repair**, or a chip (Installed, Incompatible, Needs a newer
POE1Fixer, Folder in use, Installed separately), **Settings** for an installed plugin (it opens
the plugin on the Installed page), and **Remove** for a plugin the catalog installed.

**Restart** stops the plugin and starts it again at once, and clears Faulted, Incompatible and
SandboxError together with the crash history. Disabling and re-enabling a plugin clears them too.

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

The **Resource use** tab shows: CPU use (percent of one core, averaged over about 1 s, so it can
exceed 100 with several busy threads), working set, the duration of your last `OnTick`,
`OnOverlay` and `OnPanel` in milliseconds, the render-thread cost the app attributes to you,
overlay and panel vertices per frame, live texture bytes, rejected frames, dropped log lines and
refused input actions.

### Logs

- Your plugin's **Recent log** tab in the **Plugins** tab: every line you log (the newest 200) and
  the app's lines about your plugin.
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

## 14. ABI and versioning rules

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
  `P1Event` and `P1PluginInfo` (`P1SDK_HOST_HAS(h, field)` is the same test for `P1HostApi`),
  `P1_SNAP_HAS(s, field)` for the snapshot header, `P1_RADAR_HAS(r, field)` for the radar block
  and `P1_TERRAIN_HAS(t, field)` for the terrain header. The SDK wrapper already checks every host
  function it calls. Never write `sizeof(your struct)` into memory the other side owns.
- **Test appended fields before you use them.** The radar data was added inside ABI v1, with no
  change to `P1_ABI_VERSION` or `P1_SNAPSHOT_VERSION`: a plugin built with this SDK still loads on
  an older app (as long as its manifest does not list `"radar"`) and finds none of it there.

  | Appended | Present when | Already tested by |
  |---|---|---|
  | `P1Snapshot::radar_offset`, `radar_bytes` (header 528 -> 536 bytes) | `P1_SNAP_HAS(s, radar_bytes)` | `p1_radar_get`, `p1::Radar::*` |
  | `P1Frame::terrain` (136 -> 144 bytes) | `P1_HAS_FIELD(&f, P1Frame, terrain)` | `p1_terrain_current` |
  | `P1HostApi::terrain_hold`, `terrain_release` (144 -> 160 bytes) | `P1SDK_HOST_HAS(&HostApi(), terrain_hold)` | `p1::TerrainRef` |
  | `P1_PERM_RADAR`, `P1_EV_TERRAIN_CHANGED`, `P1_SNAP_RADAR_VALID` | the app has radar data | |
- **Index arrays by stride** (`p1::Entities`, `p1_entity_at`, ...), never as C arrays.
- **Element sizes.** The arrays that came with the radar data are read through v1 element sizes,
  never `sizeof`: `P1_ENTITY_RADAR_V1_BYTES` (20), `P1_QUEST_STATE_V1_BYTES` (8),
  `P1_QUEST_TRACKER_V1_BYTES` (20), `P1_TERRAIN_TGT_V1_BYTES` (12), `P1_TERRAIN_ROOM_V1_BYTES` (12).
  Their accessors accept any stride at least that big, so a later app can append fields to these
  elements without breaking a plugin built today. The three original arrays cannot grow:
  `p1_entity_at`, `p1_buff_at` and `p1_item_at` (and `p1::Entities` and the other ranges) compare
  the stride with the `sizeof` your plugin was compiled with, so a `P1Entity` grown in a later SDK
  would leave a plugin built with it seeing no entities at all on an app that writes the old size.
  `P1Entity`, `P1Buff` and `P1InventoryItem` are therefore frozen, and new per-entity data comes in
  parallel arrays such as `P1EntityRadar`.
- **Ignore what you do not know:** unknown event types, `P1_STATE_*` values, `P1_COMP_*` bits and
  `P1_KIND_*` values. Write a `default:` in every switch.
- **Copy what you keep.** No pointer into the snapshot, and no snapshot string, outlives the
  callback.
- **Do not depend on undocumented bits** such as `P1Entity::flags`.
- **Build against the SDK that ships with the app version you target**, with its ImGui copy and
  `imconfig.h` unchanged. When the plugin ABI or the pinned ImGui changes, `P1_ABI_VERSION` or
  `IMGUI_VERSION_NUM` changes, and plugins must be rebuilt. Additions that keep compatibility stay
  inside the same ABI version, as fields appended at the end.

## 15. FAQ and troubleshooting

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
AutoPot reads them, the map elements only while the radar reads run (the user's radar is on, or a
plugin with the `radar` permission runs: list `"radar"` in your manifest to turn them on yourself).

**`entity_truncated` is not 0.** The snapshot slot (2 MiB) was full, and that many entities were
left out. It happens only in extreme scenes. While the radar block is written, each entity takes
20 bytes more of the slot.

**Why is my radar empty?** Go down the list (section 11):
- `"radar"` is in `permissions` and the plugin was restarted after you edited `plugin.json`:
  `p1::HasPermission(f, P1_PERM_RADAR)` is true. An app without radar data marks the plugin
  Invalid with `unknown permission "radar"`.
- The app is not in WGC mode (`P1Radar::gate` reads `P1_RGATE_WGC`) and the game is in an area
  (`P1_RGATE_NOT_IN_GAME` on login, character select and loading screens).
- The terrain is there: right after entering an area it is still being built (`terrain_stage`
  `READING` or `COMPOSING`; `DEFERRED` while the app trades), and until it arrives
  `p1_terrain_current(&f)` is null even though `P1Frame::terrain` still holds the previous area's.
- A map is open: `p1::Radar::LargeMap(f)` is invalid while the large map is closed, and both map
  transforms need `P1_SNAP_MAP_VALID` (`map_ui_stage` `P1_MAPUI_OK` or `DEFERRED`).
- `p1::TerrainTexture::Update(f)` runs in every `OnTick`, and your texture budget has room
  (section 7): with a full budget the upload waits.
- Everything is drawn through the same transform and player (`p1::Radar::ToMap`), inside the
  minimap's clip rectangle when you draw on the minimap.

**Can I get the terrain without the permission?** No. The app gives the terrain section only to a
plugin whose manifest lists `"radar"`: for any other plugin `P1Frame::terrain` stays null,
`P1_EV_TERRAIN_CHANGED` never comes and `terrain_hold` returns null. The radar block in the
snapshot can be visible to you while another plugin has the permission, but never count on that.

**Does a radar plugin slow the game or the app?** The app then does what it does when the user's
own radar is on: the radar's read-only memory reads at the radar's cadence (about 60 snapshots a
second instead of 30), the terrain built once per area, exported once and copied once into your
process. Nothing is ever written to the game, the app's trade work keeps priority (the radar
reads wait while it runs), and your own drawing and CPU time stay inside your plugin's limits
(sections 1 and 7).

**My radar is offset from the game's map.** Use `p1::Radar::LargeMap(f)` / `MiniMap(f)` instead of
your own map math (the large map's `pos_base` is its centre, the shifts are pixels: section 6).
Two of the projection constants are estimates; the user corrects them with the calibration in
the app's Radar tab, and the transforms apply it.

**`submit_input` returns 0.** Check the `input` permission in `plugin.json`, the user's **Allow
input** switch, `f.input_allowed` (game in the foreground, no trade or recovery automation, menu not
in use), and that the batch has at most 32 valid actions.

**Memory reads return 0.** Check the `memory` permission, that the app is attached
(`P1_SNAP_ATTACHED`), and that it is not in WGC mode.

**I cannot write a file.** Only `config_dir` is writable (section 8).

**I cannot open a URL or start a program.** The sandbox forbids starting processes. Put a "Copy
link" button in your panel that calls `p1::SetClipboard`: the clipboard is set only while the user
is using your panel (section 12).

**`p1::SetClipboard` did nothing.** It applies only while the user is using your panel (focused,
input within the last second), never while the app trades, and at most once a second (section 12).
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
