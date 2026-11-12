/* ============================================================================
 * p1_snapshot.h — the read-only game snapshot a plugin receives, v1.
 *
 * POE1Fixer's worker reads the game; a publisher thread in the app (normal
 * priority, on purpose) converts each new worker snapshot into this flat, pointer-free layout and
 * writes it into a shared-memory section that every plugin process maps
 * READ-ONLY. The plugin host copies the newest complete slot into private
 * memory before each callback, so a P1Snapshot* handed to a plugin is stable
 * for the whole callback.
 *
 * Layout rules (append-only, versioned):
 *   - All offsets are byte offsets from the start of the P1Snapshot.
 *   - Arrays carry their own stride: index with p1_entity_at(s, i) etc., never
 *     with ((P1Entity*)base)[i]. The v1 elements (P1Entity, P1Buff,
 *     P1InventoryItem) can never grow: their accessors compare the stride with
 *     sizeof. Arrays added later (p1_radar.h) compare with a fixed P1_*_V1_BYTES
 *     constant instead, so those elements can grow at the end.
 *   - Strings are p1_str offsets into the string pool; 0 is the empty string.
 *     Read them with p1_str_get(), which bounds-checks.
 *   - header_bytes says how much of P1Snapshot the app wrote. A field added
 *     in a later SDK exists only when P1_SNAP_HAS(s, field) — an older app
 *     never wrote it (the bytes there belong to the arrays).
 *   - Anything marked "gate" is filled only while an app feature that owns
 *     the read (radar, AutoPot) is running; its P1_SNAP_* flag says so. The
 *     radar reads also run while any plugin with the "radar" permission runs.
 * ========================================================================== */
#ifndef P1SDK_P1_SNAPSHOT_H
#define P1SDK_P1_SNAPSHOT_H

#include <stdint.h>
#include <stddef.h>
#include <math.h>

#ifdef __cplusplus
extern "C" {
#endif

#define P1_SNAPSHOT_VERSION 1u

typedef uint32_t p1_str;   /* byte offset into the string pool; 0 = "" */

typedef struct P1Vec3 { float x, y, z; } P1Vec3;

/* ---- game states (index into the game's state table) ---------------------- */
enum {
    P1_STATE_AREA_LOADING = 0, P1_STATE_ESCAPE = 3, P1_STATE_IN_GAME = 4,
    P1_STATE_LOGIN = 6, P1_STATE_WAITING = 7, P1_STATE_SELECT_CHARACTER = 9,
    P1_STATE_LOADING = 11, P1_STATE_NOT_LOADED = 13
};

/* ---- snapshot flags --------------------------------------------------------- */
enum {
    P1_SNAP_ATTACHED       = 1u << 0,   /* the app is attached to a game process         */
    P1_SNAP_ATTACHING      = 1u << 1,   /* initial scan running                          */
    P1_SNAP_WGC_MODE       = 1u << 2,   /* app is in visual (WGC) mode: no game memory    */
    P1_SNAP_IN_GAME        = 1u << 3,   /* state == P1_STATE_IN_GAME                      */
    P1_SNAP_LOADING        = 1u << 4,   /* a loading state                                */
    P1_SNAP_INPUT_FOCUSED  = 1u << 5,   /* a text field in the game owns the keyboard     */
    P1_SNAP_WORKER_FAULT   = 1u << 6,   /* the read faulted this tick: partial data       */
    P1_SNAP_TRADE_BUSY     = 1u << 7,   /* app trade/stash automation owns the mouse      */
    P1_SNAP_RECOVERING     = 1u << 8,   /* app AFK/disconnect recovery is running          */
    P1_SNAP_CAMERA_VALID   = 1u << 9,   /* camera block below is valid                     */
    P1_SNAP_BUFFS_VALID    = 1u << 10,  /* player buffs were read this tick (gate: AutoPot) */
    P1_SNAP_MAP_VALID      = 1u << 11,  /* large_map / mini_map were read (gate: radar)    */
    P1_SNAP_INVENTORY_VALID= 1u << 12,  /* main inventory (12x5) is valid                  */
    P1_SNAP_RADAR_VALID    = 1u << 13   /* the radar reads ran this tick: the radar block's
                                           data is fresh (p1_radar.h; needs a newer app)    */
};

/* ---- entity kinds / component bits (mirror the app's EntityKind/EntityComp) */
enum {
    P1_KIND_PLAYER = 0, P1_KIND_MONSTER, P1_KIND_NPC, P1_KIND_CHEST, P1_KIND_WORLD_ITEM,
    P1_KIND_AREA_TRANSITION, P1_KIND_PORTAL, P1_KIND_WAYPOINT, P1_KIND_EFFECT,
    P1_KIND_MISC_OBJECT, P1_KIND_UNKNOWN
};
enum {
    P1_COMP_LIFE = 1u << 0, P1_COMP_POSITIONED = 1u << 1, P1_COMP_RENDER = 1u << 2,
    P1_COMP_CHEST = 1u << 3, P1_COMP_PLAYER = 1u << 4, P1_COMP_OMP = 1u << 5,
    P1_COMP_MONSTER = 1u << 6, P1_COMP_NPC = 1u << 7, P1_COMP_SHRINE = 1u << 8,
    P1_COMP_AREA_TRANSITION = 1u << 9, P1_COMP_PORTAL = 1u << 10, P1_COMP_WAYPOINT = 1u << 11,
    P1_COMP_MINIMAP_ICON = 1u << 12, P1_COMP_TARGETABLE = 1u << 13,
    P1_COMP_TRANSITIONABLE = 1u << 14, P1_COMP_TRIGGERABLE_BLOCKAGE = 1u << 15,
    P1_COMP_WORLD_ITEM = 1u << 16, P1_COMP_STATE_MACHINE = 1u << 17,
    P1_COMP_DIES_AFTER_TIME = 1u << 18, P1_COMP_BUFFS = 1u << 19, P1_COMP_ACTOR = 1u << 20,
    P1_COMP_STATS = 1u << 21
};

/* ---- one entity (stride = snapshot->entity_stride) ------------------------- */
typedef struct P1Entity {
    uint64_t address;            /* entity pointer in the game (identity together with id) */
    uint32_t id;
    uint32_t flags;              /* raw Entity.Flags, low 32 bits (bits not documented)    */
    uint32_t comp_mask;          /* P1_COMP_*                                              */
    uint8_t  kind;               /* P1_KIND_*                                              */
    uint8_t  rarity;             /* 0 normal, 1 magic, 2 rare, 3 unique, 0xFF none         */
    uint8_t  reaction;           /* 1 friendly, other = hostile/neutral, 0xFF none         */
    uint8_t  targetable;         /* 0 / 1, 0xFF unknown (gate: radar)                      */
    uint8_t  has_life, is_opened, is_strongbox, _pad0;
    int32_t  grid_x, grid_y;     /* Positioned.GridPosition                                */
    P1Vec3   render_pos;         /* Render.Pos (model base, world units)                   */
    P1Vec3   render_bounds;      /* Render.Bounds                                          */
    float    rotation;
    float    terrain_height;     /* terrain height under it, radar (negated) convention   */
    int32_t  hp, hp_max, es, es_max, mana, mana_max;   /* only the local player's are authoritative */
    p1_str   path;               /* metadata path                                          */
    p1_str   minimap_icon;       /* MinimapIcons.dat Id (gate: radar)                      */
    p1_str   transition_target;  /* destination WorldArea Id of an exit (gate: radar)      */
    p1_str   item_path;          /* inner item path of a ground item (gate: radar)         */
} P1Entity;

typedef struct P1Buff {          /* stride = snapshot->buff_stride */
    p1_str   name;               /* BuffDefinitions Id                                    */
    float    timer, max_time;    /* seconds; +inf on an endless buff                       */
    uint16_t charges, flask_slot;
    uint32_t source_id;
} P1Buff;

typedef struct P1InventoryItem { /* stride = snapshot->item_stride (main inventory 12x5) */
    uint64_t entity;
    uint8_t  x, y, w, h;
    p1_str   base_name;
} P1InventoryItem;

typedef struct P1Area {
    p1_str   name, id;           /* localized name, WorldArea Id (e.g. "1_1_town")         */
    int32_t  act, level;
    uint32_t hash, instance_id;
    int32_t  world_area_id;
    uint32_t change_counter;     /* bumps on every real area / instance change            */
    uint8_t  is_town, has_waypoint, is_hideout, _pad0;
    uint32_t _pad1;
} P1Area;

typedef struct P1Player {
    uint64_t address;
    uint32_t id;
    p1_str   name;
    int32_t  hp, hp_max, es, es_max, mana, mana_max;
    int32_t  hp_reserved_flat, hp_reserved_fraction, mana_reserved_flat, mana_reserved_fraction;
    float    hp_regen, mana_regen;
    int32_t  grid_x, grid_y;
    float    world_x, world_y, rotation;
    int16_t  action_id, _pad0;
    int32_t  animation_id;
    uint32_t buff_first, buff_count;  /* range in the buff array (P1_SNAP_BUFFS_VALID)    */
} P1Player;

typedef struct P1Camera {        /* sampled by the worker next to the entities          */
    float    view_proj[16];      /* v = (x,y,z,1) * M  (row-vector, see p1_world_to_screen) */
    int32_t  viewport_w, viewport_h;
} P1Camera;

/* One of the game's two map elements (gate: radar). Units are MIXED, as the game keeps them:
 *   pos_base / size_base: UI base units (2560x1600, height-fit); px = base * client_h/1600.
 *     The MINIMAP is a real rectangle: pos = its top-left, size = its size.
 *     The LARGE MAP is not: its pos is the map CENTRE and its size reads 0x0 (live 2026-11-02).
 *   scale_rel: element scale / root scale (multiply by client_h/1600 for px per base unit).
 *   shift / default_shift: the game's raw sub-map fields, in client PIXELS (not base units).
 *   zoom: the sub-map zoom (0 = unusable).
 * Do not do this math by hand: p1_radar.h has p1_large_map_transform / p1_mini_map_transform,
 * the projection the app's own radar uses (with the user's calibration). */
typedef struct P1MapElement {
    uint8_t  valid, visible, _pad0[2];
    float    pos_base_x, pos_base_y, size_base_x, size_base_y;
    float    scale_rel;
    float    shift_x, shift_y, default_shift_x, default_shift_y;
    float    zoom;
} P1MapElement;

/* ---- render-side block: the game window and keyboard, ~60 Hz ---------------- */
typedef struct P1Viewport {
    uint32_t valid;              /* a game window is known                                 */
    int32_t  client_x, client_y; /* game client area, screen pixels (the overlay covers it) */
    int32_t  client_w, client_h;
    uint32_t game_foreground;    /* the game window is the foreground window               */
    uint32_t app_foreground;     /* the POE1Fixer menu is the foreground window            */
    float    ui_scale;           /* app UI scale (0.8 .. 2.0)                              */
    int32_t  cursor_x, cursor_y; /* cursor, game client pixels                             */
    uint32_t _pad0;
    uint8_t  keys_down[32];      /* bit vk set = key down; sampled ONLY while the game is
                                    foreground and no game text field has focus, else 0  */
} P1Viewport;

/* ---- the snapshot ------------------------------------------------------------ */
typedef struct P1Snapshot {
    uint32_t size_bytes;         /* total bytes (header + arrays + strings)               */
    uint32_t header_bytes;       /* sizeof(P1Snapshot) as the app wrote it                */
    uint32_t version;            /* P1_SNAPSHOT_VERSION                                   */
    uint32_t flags;              /* P1_SNAP_*                                             */
    uint64_t frame;              /* worker frame counter (monotonic within a run)         */
    uint64_t publish_seq;        /* publisher counter (monotonic)                          */
    int64_t  capture_us;         /* steady clock us when positions were sampled (0 = none) */
    int64_t  publish_us;         /* steady clock us when the app published this           */
    uint32_t pid;                /* game process id (0 = none)                             */
    int32_t  game_state;         /* P1_STATE_*                                            */
    float    worker_period_ms;   /* measured worker tick period                           */
    uint32_t _pad0;

    /* raw anchors in the game process (0 = unknown) for P1_PERM_MEMORY plugins.
       Offsets inside these objects change with game patches: prefer the
       typed fields of this snapshot whenever they cover what you need. */
    uint64_t module_base, ingame_state, ingame_data, ingame_ui, server_data,
             local_player, entity_list, camera_ptr;

    P1Area   area;
    P1Player player;
    P1Camera camera;             /* P1_SNAP_CAMERA_VALID                                   */
    P1MapElement large_map, mini_map;   /* P1_SNAP_MAP_VALID                              */

    int32_t  latency_ms, gold, time_in_game_ms;
    uint32_t network_state;
    p1_str   league;
    uint32_t inv_cols, inv_rows; /* 12 x 5 when P1_SNAP_INVENTORY_VALID                    */
    uint32_t _pad1;
    uint64_t inv_occupied;       /* bit (y*cols + x) set = cell occupied                   */

    uint32_t entity_count, entity_stride, entity_offset, entity_truncated;
    uint32_t buff_count, buff_stride, buff_offset, _pad2;
    uint32_t item_count, item_stride, item_offset, _pad3;
    uint32_t string_offset, string_bytes;   /* pool; byte 0 is always '\0'               */

    /* ---- appended (radar data for plugins). Present only when P1_SNAP_HAS(s, radar_bytes):
       an older app never wrote these bytes. Read the block with p1_radar_get() (p1_radar.h). */
    uint32_t radar_offset, radar_bytes;     /* the P1Radar struct itself (offset, its size);
                                               its arrays are placed by its own *_offset
                                               fields. 0 = no radar block                    */
} P1Snapshot;

/* ---- accessors ------------------------------------------------------------- */
#define P1_SNAP_HAS(s, field) \
    ((s) != NULL && (s)->header_bytes >= (uint32_t)(offsetof(P1Snapshot, field) + sizeof(((P1Snapshot*)0)->field)))

static inline const char* p1_str_get(const P1Snapshot* s, p1_str off) {
    if (!s || off == 0 || off >= s->string_bytes) return "";
    return (const char*)s + s->string_offset + off;   /* pool is NUL-terminated per string */
}
static inline const P1Entity* p1_entity_at(const P1Snapshot* s, uint32_t i) {
    if (!s || i >= s->entity_count || s->entity_stride < sizeof(P1Entity)) return NULL;
    return (const P1Entity*)((const char*)s + s->entity_offset + (size_t)i * s->entity_stride);
}
static inline const P1Buff* p1_buff_at(const P1Snapshot* s, uint32_t i) {
    if (!s || i >= s->buff_count || s->buff_stride < sizeof(P1Buff)) return NULL;
    return (const P1Buff*)((const char*)s + s->buff_offset + (size_t)i * s->buff_stride);
}
static inline const P1InventoryItem* p1_item_at(const P1Snapshot* s, uint32_t i) {
    if (!s || i >= s->item_count || s->item_stride < sizeof(P1InventoryItem)) return NULL;
    return (const P1InventoryItem*)((const char*)s + s->item_offset + (size_t)i * s->item_stride);
}
static inline int p1_key_down(const P1Viewport* v, int vk) {
    return (v && vk > 0 && vk < 256) ? ((v->keys_down[vk >> 3] >> (vk & 7)) & 1) : 0;
}

/* World -> game client pixels with the snapshot's camera (the same projection
   the app uses). Returns 0 when behind the camera or the camera is invalid.
   The camera viewport can differ from the client size; the result is scaled
   to client pixels when client_w/h > 0. */
static inline int p1_world_to_screen(const P1Snapshot* s, float wx, float wy, float wz,
                                     float client_w, float client_h, float* sx, float* sy) {
    if (!s || !(s->flags & P1_SNAP_CAMERA_VALID)) return 0;
    const float* m = s->camera.view_proj;
    const float x  = wx * m[0] + wy * m[4] + wz * m[8]  + m[12];
    const float y  = wx * m[1] + wy * m[5] + wz * m[9]  + m[13];
    const float w  = wx * m[3] + wy * m[7] + wz * m[11] + m[15];
    if (!(w > 0.0001f)) return 0;
    const float vw = (float)s->camera.viewport_w, vh = (float)s->camera.viewport_h;
    if (!(vw > 0.0f) || !(vh > 0.0f)) return 0;
    float fx = (x / w + 1.0f) * 0.5f * vw;
    float fy = (1.0f - y / w) * 0.5f * vh;
    if (client_w > 0.0f && client_h > 0.0f) { fx *= client_w / vw; fy *= client_h / vh; }
    if (!isfinite(fx) || !isfinite(fy)) return 0;
    *sx = fx; *sy = fy;
    return 1;
}

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* P1SDK_P1_SNAPSHOT_H */
