/* ============================================================================
 * p1_abi.h — POE1Fixer plugin ABI (plugin DLL <-> plugin host process), v1.
 *
 * A plugin is a DLL that runs inside its OWN process, POE1FixerPluginHost.exe,
 * started by POE1Fixer for that plugin alone. The app never loads plugin code.
 * The app and the plugin host talk over shared memory (see the app-side
 * POE1Fixer/plugins/ipc/ headers, which are not part of this SDK); this header
 * is only the contract between the plugin DLL and the plugin host:
 *
 *   plugin exports   P1_GetPluginInfo   identity + build layout, read first
 *                    P1_CreatePlugin    fills a P1PluginApi table of callbacks
 *                    P1_BindImGui       binds the plugin's ImGui copy to the
 *                                       host's context + allocator
 *   host provides    P1HostApi          services (log, memory reads, input
 *                                       requests, textures, notify, clipboard)
 *
 * Rules that keep this ABI stable:
 *   - Plain C. No C++ classes, std:: types, exceptions or vtables cross it.
 *   - Every struct starts with size_bytes. A side reads a field only when
 *     size_bytes covers it (P1_HAS_FIELD); structs only ever grow at the END.
 *     Nobody ever writes sizeof(their struct) into memory the other side owns.
 *   - Strings are UTF-8, NUL-terminated. Paths are UTF-8 too (convert with
 *     MultiByteToWideChar(CP_UTF8) before calling a W API).
 *   - All callbacks run on the plugin host's single plugin thread, in order.
 *     Host API functions may be called from ANY plugin thread unless noted.
 *
 * Process facts a plugin must know (the sandbox):
 *   - The plugin host runs at LOW integrity with a restricted token: it cannot
 *     open other processes, cannot write files outside its config directory
 *     (P1HostApi.config_dir), cannot send input to the game window, and cannot
 *     start child processes. Game memory is readable through read_memory (the
 *     host holds a read-only handle when the plugin has P1_PERM_MEMORY);
 *     keyboard/mouse actions go through submit_input (P1_PERM_INPUT, user
 *     consent required) and the app performs them only when it is safe.
 *   - A crash or a hang kills only this process. The app restarts it with a
 *     back-off and gives up on a crash loop. Do not try to survive an access
 *     violation; do catch your own C++ exceptions (the SDK wrapper does).
 * ========================================================================== */
#ifndef P1SDK_P1_ABI_H
#define P1SDK_P1_ABI_H

#include <stdint.h>
#include <stddef.h>
#include "p1_snapshot.h"
#include "p1_radar.h"
#include "p1_terrain.h"

#ifdef __cplusplus
extern "C" {
#endif

#define P1_ABI_VERSION 1u

#if defined(_WIN32)
#  define P1_EXPORT __declspec(dllexport)
#  define P1_CALL   __cdecl
#else
#  define P1_EXPORT
#  define P1_CALL
#endif

/* size_bytes-guarded field access: true when struct *p (whose first member is
 * uint32_t size_bytes) was written by a side that knew `field`. */
#define P1_HAS_FIELD(p, type, field) \
    ((p) != NULL && (p)->size_bytes >= (uint32_t)(offsetof(type, field) + sizeof(((type*)0)->field)))

/* ---- permissions (manifest "permissions": ["memory", "input", "radar"]) - */
enum {
    P1_PERM_MEMORY = 1u << 0,   /* read_memory / read_memory_batch work        */
    P1_PERM_INPUT  = 1u << 1,   /* submit_input works (needs user consent too)  */
    P1_PERM_RADAR  = 1u << 2    /* the app runs its radar reads for you while you
                                   run (map elements, minimap icons, exits, quest
                                   log: p1_radar.h) and maps the area terrain into
                                   your process (P1Frame.terrain, p1_terrain.h).
                                   Needs an app that knows it: an older app refuses
                                   the manifest (set the catalog's min_app_version). */
};

/* ---- log levels ----------------------------------------------------------- */
enum { P1_LOG_DEBUG = 0, P1_LOG_INFO = 1, P1_LOG_WARN = 2, P1_LOG_ERROR = 3 };

/* ---- notification levels -------------------------------------------------- */
enum { P1_NOTIFY_INFO = 0, P1_NOTIFY_SUCCESS = 1, P1_NOTIFY_WARNING = 2, P1_NOTIFY_ERROR = 3 };

/* ---- plugin host exit codes (the app classifies a stopped plugin by these;
 *      an exception code such as 0xC0000005 means the plugin crashed) ------- */
#define P1_EXIT_OK               0x00000000u
#define P1_EXIT_BAD_ARGS         0xE1F10001u
#define P1_EXIT_CHANNEL          0xE1F10002u  /* shared memory missing / wrong version */
#define P1_EXIT_DLL_LOAD         0xE1F10003u  /* LoadLibrary failed (missing dependency?) */
#define P1_EXIT_MISSING_EXPORT   0xE1F10004u
#define P1_EXIT_ABI_MISMATCH     0xE1F10005u
#define P1_EXIT_IMGUI_MISMATCH   0xE1F10006u
#define P1_EXIT_LOAD_REFUSED     0xE1F10007u  /* on_load returned 0 */
#define P1_EXIT_ID_MISMATCH      0xE1F10008u  /* DLL id != plugin.json id */
#define P1_EXIT_PARENT_GONE      0xE1F10009u
#define P1_EXIT_FONTS            0xE1F1000Au
#define P1_EXIT_PROTOCOL         0xE1F1000Bu
#define P1_EXIT_INIT             0xE1F1000Cu  /* any other host start-up failure */
#define P1_EXIT_CPP_TERMINATE    0xE1F1DE01u  /* std::terminate (uncaught C++ exception) */
#define P1_EXIT_PURECALL         0xE1F1DE02u
#define P1_EXIT_INVALID_PARAM    0xE1F1DE03u
#define P1_EXIT_ABORT            0xE1F1DE04u

/* ---- events (P1PluginApi.on_event) ---------------------------------------- */
enum {
    P1_EV_ATTACHED          = 1,  /* the app attached to a game process (a = pid)            */
    P1_EV_DETACHED          = 2,  /* the game process went away                               */
    P1_EV_STATE_CHANGED     = 3,  /* a = old P1_STATE_*, b = new                              */
    P1_EV_AREA_CHANGED      = 4,  /* a = new area change counter (snapshot->area is the new) */
    P1_EV_CHARACTER_CHANGED = 5,  /* the local player's name changed                          */
    P1_EV_PANEL_SHOWN       = 6,  /* your settings panel became visible in the app            */
    P1_EV_PANEL_HIDDEN      = 7,
    P1_EV_PERMISSIONS       = 8,  /* a = new effective P1_PERM_* bits (input consent toggled) */
    P1_EV_LANGUAGE          = 9,  /* P1HostApi.language changed (re-read it)                  */
    P1_EV_GAME_FOCUS        = 10, /* a = 1 game window became foreground, 0 lost it           */
    P1_EV_INPUT_GATE        = 11, /* a = 1 the app accepts input requests now, 0 refuses      */
    P1_EV_TERRAIN_CHANGED   = 12  /* a new area terrain arrived (P1_PERM_RADAR; a = its serial):
                                     rebuild what you derive from P1Frame.terrain             */
};

typedef struct P1Event {
    uint32_t size_bytes;
    uint32_t type;      /* P1_EV_* */
    int64_t  a, b;
} P1Event;

/* ---- the per-callback frame ----------------------------------------------- */
typedef struct P1Frame {
    uint32_t size_bytes;
    uint32_t _pad0;
    const P1Snapshot* snapshot;     /* never NULL; valid until the callback returns.
                                       Copy what you keep. flags==0 => no game data. */
    P1Viewport viewport;            /* game window + key state, sampled ~60 Hz        */
    int64_t  now_us;                /* steady clock, microseconds (same clock as
                                       snapshot->capture_us / publish_us)             */
    float    delta_s;               /* seconds since the previous callback of this kind */
    float    panel_w, panel_h;      /* settings panel size in pixels (0 when hidden)   */
    uint32_t panel_visible;         /* 1 while the app shows your panel                */
    uint32_t overlay_enabled;       /* 1 when your overlay output will be shown        */
    uint32_t permissions;           /* effective P1_PERM_* right now                    */
    uint32_t input_allowed;         /* 1 when submit_input would be accepted right now */
    /* ---- appended: check P1_HAS_FIELD(f, P1Frame, terrain) (an older host has none) -- */
    uint32_t _pad1;
    const P1Terrain* terrain;       /* the newest area terrain (P1_PERM_RADAR), NULL before
                                       the first one or without the permission. Valid until
                                       the callback returns; it can be the PREVIOUS area's
                                       while the new one is built: p1_terrain_current().  */
} P1Frame;

/* The frame's terrain when it is the snapshot's CURRENT area's (its serial matches the radar
   block's terrain_serial), else NULL. */
static inline const P1Terrain* p1_terrain_current(const P1Frame* f) {
    if (!P1_HAS_FIELD(f, P1Frame, terrain) || !f->terrain) return NULL;
    const P1Radar* r = p1_radar_get(f->snapshot);
    if (!r || r->terrain_serial == 0 || r->terrain_serial != f->terrain->serial) return NULL;
    return f->terrain;
}

/* ---- input requests (P1_PERM_INPUT + the user's consent) ------------------
 * The app performs the actions in order, on its own input thread, ONLY while:
 * the game window is foreground, no trade / stash / recovery automation runs,
 * the user is not typing in the app, and the plugin's consent is on. When the
 * gate closes mid-batch the rest is dropped and every key/button the plugin
 * holds is released. Coordinates are game CLIENT pixels. */
enum {
    P1_IN_KEY_DOWN   = 1,   /* vk                                   */
    P1_IN_KEY_UP     = 2,   /* vk                                   */
    P1_IN_KEY_PRESS  = 3,   /* vk, hold_ms (0 => 30 ms)             */
    P1_IN_MOUSE_MOVE = 4,   /* x, y                                 */
    P1_IN_MOUSE_DOWN = 5,   /* vk = VK_LBUTTON/VK_RBUTTON/VK_MBUTTON/VK_XBUTTON1/2 */
    P1_IN_MOUSE_UP   = 6,   /* vk                                   */
    P1_IN_CLICK      = 7,   /* vk, x, y, hold_ms (0 => 30 ms)       */
    P1_IN_WAIT       = 8    /* hold_ms (<= 1000)                    */
};
#define P1_INPUT_MAX_BATCH 32u

typedef struct P1InputAction {
    uint16_t type;          /* P1_IN_* */
    uint16_t vk;            /* Windows virtual-key code */
    int32_t  x, y;          /* game client pixels (MOUSE_MOVE / CLICK) */
    uint32_t hold_ms;
} P1InputAction;

/* ---- batched memory reads (P1_PERM_MEMORY) -------------------------------- */
typedef struct P1ReadRequest {
    uint64_t address;
    void*    dst;
    uint32_t size;          /* <= 16 MiB per request */
    uint32_t bytes_read;    /* out */
} P1ReadRequest;

/* ---- identity, exported by the plugin (P1_GetPluginInfo) ------------------ */
typedef struct P1PluginInfo {
    uint32_t size_bytes;
    uint32_t abi_version;        /* P1_ABI_VERSION the plugin was built against   */
    uint32_t snapshot_version;   /* P1_SNAPSHOT_VERSION it was built against      */
    uint32_t imgui_version_num;  /* IMGUI_VERSION_NUM of the SDK's ImGui copy      */
    /* sizeof(ImGuiIO), sizeof(ImGuiStyle), sizeof(ImVec2), sizeof(ImVec4),
       sizeof(ImDrawVert), sizeof(ImDrawIdx), sizeof(ImGuiContext) — the host
       refuses a plugin whose ImGui layout differs from its own. */
    uint32_t imgui_layout[7];
    const char* id;              /* must equal plugin.json "id"                   */
    const char* name;
    const char* version;
    const char* author;
    const char* description;
} P1PluginInfo;

/* ---- services provided by the plugin host --------------------------------- */
typedef struct P1HostApi {
    uint32_t size_bytes;
    uint32_t abi_version;               /* P1_ABI_VERSION of the host               */
    void*    host;                      /* opaque, pass back to every function      */

    const char* plugin_dir;             /* UTF-8, absolute, read-only for you       */
    const char* config_dir;             /* UTF-8, absolute, the ONE writable folder */
    const char* app_dir;                /* UTF-8, absolute (Resources/ lives here)  */
    const char* language;               /* app UI language: "en","ru","de","es","fr",
                                           "ja","ko","pt_BR","th" (re-read on P1_EV_LANGUAGE) */
    uint32_t permissions;               /* granted by the manifest (consent is live in P1Frame) */
    uint32_t _pad0;

    /* logging — rate-limited by the app (excess lines are dropped and counted) */
    void     (P1_CALL *log)(void* host, int level, const char* utf8);
    /* a toast in the app (rate-limited, ~1 per 2 s) */
    void     (P1_CALL *notify)(void* host, int level, const char* title_utf8, const char* text_utf8);

    /* game memory, read-only. 0 / no-op without P1_PERM_MEMORY or when detached. */
    uint32_t (P1_CALL *read_memory)(void* host, uint64_t address, void* dst, uint32_t size);
    uint32_t (P1_CALL *read_memory_batch)(void* host, P1ReadRequest* reqs, uint32_t count);

    /* input requests. Returns how many actions were queued (0 = refused). */
    uint32_t (P1_CALL *submit_input)(void* host, const P1InputAction* actions, uint32_t count);

    /* textures for ImGui::Image(). RGBA8, tightly packed (pitch = w*4 when 0).
       Returns an ImTextureID value (0 on failure). Callable from the plugin
       thread only. Max 4096x4096; the app caps total texture memory (64 MiB) and
       the count of live textures (1024) per plugin, ImGui's font atlases included:
       over either cap this returns 0 and the plugin keeps running. */
    uint64_t (P1_CALL *texture_create)(void* host, const void* rgba, int32_t w, int32_t h);
    int      (P1_CALL *texture_update)(void* host, uint64_t tex, int32_t x, int32_t y,
                                       int32_t w, int32_t h, const void* rgba, int32_t pitch);
    void     (P1_CALL *texture_destroy)(void* host, uint64_t tex);

    /* clipboard (the sandbox has no clipboard of its own; the app sets it, only
       while the user is using this plugin's panel, never while the app trades,
       at most once a second; the newest request wins) */
    void     (P1_CALL *set_clipboard)(void* host, const char* utf8);

    /* overlay frame rate while nothing changes: 0 = build an overlay frame only
       on new data (default), N = also at least N frames/s (animations), capped
       by the manifest maxFps. */
    void     (P1_CALL *request_frames)(void* host, uint32_t overlay_fps);

    int64_t  (P1_CALL *now_us)(void* host);

    /* ---- appended: check P1SDK_HOST_HAS / P1_HAS_FIELD(api, P1HostApi, terrain_hold) ----
       The newest terrain (P1_PERM_RADAR) with one reference taken, or NULL. It stays valid
       until you pass it to terrain_release, on ANY thread (build a texture on a worker).
       Every hold needs exactly one release. A held terrain counts against your process's
       memory limit: release the old one when P1_EV_TERRAIN_CHANGED arrives. */
    const P1Terrain* (P1_CALL *terrain_hold)(void* host);
    void     (P1_CALL *terrain_release)(void* host, const P1Terrain* terrain);
} P1HostApi;

/* ---- callbacks, filled by the plugin in P1_CreatePlugin ------------------- */
typedef struct P1PluginApi {
    uint32_t size_bytes;                /* set by the HOST to sizeof(its P1PluginApi); the
                                           plugin fills only fields that fit             */
    uint32_t _pad0;
    void*    self;
    int      (P1_CALL *on_load)(void* self);                     /* 0 = refuse to run */
    void     (P1_CALL *on_unload)(void* self);
    void     (P1_CALL *on_tick)(void* self, const P1Frame* f);    /* logic, every frame */
    void     (P1_CALL *on_overlay)(void* self, const P1Frame* f); /* ImGui frame over the game */
    void     (P1_CALL *on_panel)(void* self, const P1Frame* f);   /* ImGui frame in the app menu */
    void     (P1_CALL *on_event)(void* self, const P1Event* e);
} P1PluginApi;

/* ---- the three exports ----------------------------------------------------- */
typedef const P1PluginInfo* (P1_CALL *P1_GetPluginInfo_t)(void);
typedef int  (P1_CALL *P1_CreatePlugin_t)(const P1HostApi* host, P1PluginApi* out);
/* Called before every on_overlay / on_panel with the context that frame uses.
   imgui_context is an ImGuiContext*, alloc/free are ImGuiMemAllocFunc /
   ImGuiMemFreeFunc. The plugin must call ImGui::SetAllocatorFunctions and
   ImGui::SetCurrentContext in ITS ImGui copy (the SDK macro does this). */
typedef void (P1_CALL *P1_BindImGui_t)(void* imgui_context, void* alloc_fn, void* free_fn, void* user);

#define P1_EXPORT_GET_INFO    "P1_GetPluginInfo"
#define P1_EXPORT_CREATE      "P1_CreatePlugin"
#define P1_EXPORT_BIND_IMGUI  "P1_BindImGui"

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* P1SDK_P1_ABI_H */
