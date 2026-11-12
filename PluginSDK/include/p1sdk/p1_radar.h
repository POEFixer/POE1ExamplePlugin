/* ============================================================================
 * p1_radar.h — radar data in the snapshot, and the map projection, v1.
 *
 * Everything a plugin needs to draw its own radar the way the app's radar does:
 *
 *   P1Radar (one per snapshot, p1_radar_get): why the radar reads are on or
 *     off, how far the area's terrain build is, the projection constants the
 *     app uses, the user's own calibration of the two game maps (Radar tab),
 *     the player's float grid position and height, the icon atlas, and three
 *     arrays:
 *       P1EntityRadar   parallel to the snapshot's entity array: the app's
 *                       radar category of each entity, its icon cells, its
 *                       float grid position and the terrain height under it;
 *       P1QuestState    the game's quest log (Quest.dat Id -> state);
 *       P1QuestTrackerEntry the game's quest list on the right of the screen.
 *   The terrain itself (walkable grid, heights, tiles, rooms) is per area and
 *   much bigger: it arrives separately (p1_terrain.h).
 *
 * Who gets what:
 *   - The radar reads (map elements, minimap icons, exits, ground items,
 *     targetable, quest log, quest list, terrain) run while the app's radar
 *     is on OR while any running plugin has the "radar" permission
 *     (P1_PERM_RADAR). The v1 per-entity fields and large_map / mini_map are
 *     filled whenever they run (P1_SNAP_MAP_VALID, the "unknown" values).
 *   - The P1Radar block and the terrain are for the plugins WITH the
 *     permission: the plugin host removes the block from the snapshot copy of
 *     a plugin without it. P1Radar.gate says whether the reads ran and why
 *     not, P1Radar.flags (P1_RADAR_BY_APP / _BY_PLUGIN) who asked, and
 *     P1_SNAP_RADAR_VALID is set on the ticks the block is there AND the reads
 *     ran (never without the block).
 *
 * Projection (the app's radar, RadarProjection.h, verbatim):
 *   dx = g.x - player.x;  dy = g.y - player.y;  z = dz_world * height_to_grid
 *   screen.x = cx + scale * ((dx - dy) * cos(camera_angle))
 *   screen.y = cy + scale * ((z - (dx + dy)) * sin(camera_angle))
 * where dz_world is the height difference to draw at (heights are negated, so
 * higher ground lifts the point). Use p1_grid_to_map with a transform from
 * p1_radar_large_map / p1_radar_mini_map / p1_panel_transform.
 * ========================================================================== */
#ifndef P1SDK_P1_RADAR_H
#define P1SDK_P1_RADAR_H

#include <stdint.h>
#include <stddef.h>
#include <math.h>
#include <string.h>
#include "p1_snapshot.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- why the radar reads are on / off (P1Radar.gate) ----------------------------- */
enum {
    P1_RGATE_OFF         = 0,  /* nobody wants them (neither the app's radar nor a plugin)  */
    P1_RGATE_ON          = 1,  /* the reads ran this tick                                     */
    P1_RGATE_NOT_IN_GAME = 2,  /* wanted, but the game is not in an area (login, loading...)  */
    P1_RGATE_WGC         = 3   /* the app is in visual (WGC) mode: no game memory at all      */
};

/* ---- P1Radar.flags --------------------------------------------------------------- */
enum {
    P1_RADAR_BY_APP              = 1u << 0,  /* the app's own radar has the reads on          */
    P1_RADAR_BY_PLUGIN           = 1u << 1,  /* a plugin with "radar" has the reads on        */
    P1_RADAR_TERRAIN_REUSED      = 1u << 2,  /* the terrain was re-used (a portal round trip) */
    P1_RADAR_QUESTS_USABLE       = 1u << 3,  /* the quest log is settled and the CURRENT
                                                character's (the app's guide uses it only then) */
    P1_RADAR_QUESTS_PROVISIONAL  = 1u << 4,  /* first read for this character, not settled yet */
    P1_RADAR_LARGE_POS_IS_CENTRE = 1u << 8,  /* large_map.pos_base is the map CENTRE          */
    P1_RADAR_SHIFT_IN_BASE_UNITS = 1u << 9   /* shift / default_shift are base units (else px) */
};

/* ---- stages (mirror the app's Radar:: enums by value) ------------------------------- */
enum {   /* P1Radar.terrain_stage */
    P1_TSTAGE_OFF = 0, P1_TSTAGE_HEADER_BAD, P1_TSTAGE_SHAPE_REJECTED, P1_TSTAGE_READING,
    P1_TSTAGE_DEFERRED /* a trade holds the reads */, P1_TSTAGE_COMPOSING, P1_TSTAGE_READY,
    P1_TSTAGE_READ_FAILED
};
enum {   /* P1Radar.map_ui_stage */
    P1_MAPUI_OFF = 0, P1_MAPUI_OFFSETS_PENDING, P1_MAPUI_MAP_ELEMENT_BAD, P1_MAPUI_OK,
    P1_MAPUI_DEFERRED /* a trade holds the reads: the last state is carried, use it as OK */
};
enum {   /* P1Radar.quest_stage */
    P1_QSTAGE_OFF = 0, P1_QSTAGE_UI_BAD, P1_QSTAGE_NOT_FOUND, P1_QSTAGE_WALK_FAILED, P1_QSTAGE_OK,
    P1_QSTAGE_DEFERRED
};
enum {   /* P1Radar.tracker_stage */
    P1_TRSTAGE_OFF = 0, P1_TRSTAGE_WAITING, P1_TRSTAGE_NO_ELEMENT, P1_TRSTAGE_HIDDEN, P1_TRSTAGE_OK,
    P1_TRSTAGE_DEFERRED
};

/* ---- radar categories (the app's Radar::Category; append-only) ---------------------- */
enum {
    P1_RCAT_SELF = 0, P1_RCAT_PLAYER, P1_RCAT_MINION,
    P1_RCAT_MONSTER_NORMAL, P1_RCAT_MONSTER_MAGIC, P1_RCAT_MONSTER_RARE, P1_RCAT_MONSTER_UNIQUE,
    P1_RCAT_ROGUE_EXILE,
    P1_RCAT_NPC,
    P1_RCAT_CHEST, P1_RCAT_CHEST_MAGIC, P1_RCAT_CHEST_RARE, P1_RCAT_STRONGBOX,
    P1_RCAT_SHRINE, P1_RCAT_AREA_TRANSITION, P1_RCAT_WAYPOINT, P1_RCAT_CHECKPOINT, P1_RCAT_PORTAL,
    P1_RCAT_ESSENCE, P1_RCAT_BREACH, P1_RCAT_ABYSS, P1_RCAT_DELIRIUM, P1_RCAT_LEGION, P1_RCAT_HARVEST,
    P1_RCAT_BLIGHT, P1_RCAT_RITUAL, P1_RCAT_EXPEDITION, P1_RCAT_ULTIMATUM, P1_RCAT_INCURSION,
    P1_RCAT_HEIST, P1_RCAT_BETRAYAL, P1_RCAT_BESTIARY, P1_RCAT_HARBINGER, P1_RCAT_TORMENTED_SPIRIT,
    P1_RCAT_QUEST_OBJECT, P1_RCAT_QUEST_ITEM,
    P1_RCAT_GAME_ICON,
    P1_RCAT_COUNT,
    P1_RCAT_NONE = 0xFF          /* the app's radar does not draw it */
};
enum {
    P1_RGROUP_PLAYERS = 0, P1_RGROUP_MONSTERS, P1_RGROUP_NPCS, P1_RGROUP_CHESTS, P1_RGROUP_WORLD,
    P1_RGROUP_LEAGUES, P1_RGROUP_QUEST,
    P1_RGROUP_NONE = 0xFF
};

/* ---- per-entity radar data (parallel to the snapshot's entity array) ---------------- */
enum {
    P1_ERAD_HIDDEN       = 1u << 0,  /* the app's default filters hide it right now: an opened
                                        chest, a dead or untargetable monster                */
    P1_ERAD_GAME_ICON    = 1u << 1,  /* the app draws it with ITS OWN game icon (game_icon_cell) */
    P1_ERAD_HAS_POSITION = 1u << 2,  /* it has a position (server helpers have none)         */
    P1_ERAD_LOCAL_PLAYER = 1u << 3   /* it is the local player                                */
};
#define P1_ICON_CELL_NONE 0xFFFFu

typedef struct P1EntityRadar {   /* element i describes p1_entity_at(s, i) */
    uint8_t  category;           /* P1_RCAT_*                                                 */
    uint8_t  group;              /* P1_RGROUP_* of the category                               */
    uint8_t  flags;              /* P1_ERAD_*                                                 */
    uint8_t  _pad0;
    uint16_t game_icon_cell;     /* its game minimap icon in the atlas (row * columns + col),
                                    P1_ICON_CELL_NONE = none                                   */
    uint16_t style_icon_cell;    /* the app's default icon for the category, or NONE          */
    float    grid_x, grid_y;     /* float grid position (render_pos / world_per_cell)         */
    float    height;             /* terrain height under it (negated world units; 0 = flat)   */
} P1EntityRadar;

typedef struct P1QuestState {    /* the game's quest log, sorted by id */
    p1_str   id;                 /* Quest.dat Id ("a1q1")                                     */
    int32_t  state;              /* 0 = complete, -1 = not started, else the state's order    */
} P1QuestState;

typedef struct P1QuestTrackerEntry {   /* the game's quest list, top to bottom */
    p1_str   id;                 /* Quest.dat Id of the entry                                 */
    float    min_x, min_y, max_x, max_y;   /* the entry's box, UI base units (px = base*client_h/1600) */
} P1QuestTrackerEntry;

#define P1_ENTITY_RADAR_V1_BYTES 20u
#define P1_QUEST_STATE_V1_BYTES   8u
#define P1_QUEST_TRACKER_V1_BYTES 20u

typedef struct P1Radar {
    uint32_t size_bytes;         /* bytes the app wrote (>= P1_RADAR_V1_BYTES)                */
    uint32_t flags;              /* P1_RADAR_*                                                */
    uint8_t  gate;               /* P1_RGATE_*                                                */
    uint8_t  terrain_stage;      /* P1_TSTAGE_*                                               */
    uint8_t  map_ui_stage;       /* P1_MAPUI_*                                                */
    uint8_t  quest_stage;        /* P1_QSTAGE_*                                               */
    uint8_t  tracker_stage;      /* P1_TRSTAGE_*                                              */
    uint8_t  _pad0[3];
    uint64_t terrain_serial;     /* the CURRENT area's terrain (0 = none published yet)       */
    int32_t  sub_tiles_read, sub_tiles_total, tgt_read, tgt_total;   /* build progress       */
    uint32_t terrain_build_ticks, terrain_ticks_deferred;

    /* projection constants (the app's own values; p1_radar_consts) */
    float    camera_angle;       /* radians                                                   */
    float    height_to_grid;     /* world height -> grid units                                */
    float    world_per_cell;     /* 250/23                                                    */
    float    map_scale_factor;   /* grid units -> map px at zoom 1                            */
    float    large_center_mul_x; /* the large map's centre x multiplier                       */
    float    ui_base_height;     /* 1600                                                      */
    uint32_t cells_per_tile;     /* 23                                                        */
    uint32_t _pad1;

    /* the user's calibration of the game maps (Radar tab); neutral = {1, 0, 0}, offsets px */
    float    large_cal_scale, large_cal_dx, large_cal_dy;
    float    mini_cal_scale, mini_cal_dx, mini_cal_dy;

    /* the local player (independent of entity truncation) */
    float    player_grid_x, player_grid_y;   /* float grid, as the app's radar centres on it */
    float    player_height;                  /* terrain height under it                       */
    uint32_t player_entity_index;            /* its row in the entity array, 0xFFFFFFFF = none */

    /* the icon atlas the cells index (a PNG under the app folder) */
    uint32_t atlas_w, atlas_h;   /* pixels                                                    */
    uint32_t atlas_cell_px;      /* one cell is cell_px x cell_px                              */
    uint32_t atlas_columns;      /* cell n is at column n % columns, row n / columns          */
    p1_str   atlas_path;         /* relative to P1HostApi.app_dir ("Resources/radar/icons.png") */
    uint32_t _pad2;

    uint32_t entity_count, entity_stride, entity_offset, _pad3;   /* count == snapshot entity_count */
    uint64_t quest_serial;       /* changes with the quest log's content                      */
    uint32_t quest_count, quest_stride, quest_offset, _pad4;
    uint64_t tracker_serial;     /* changes with the quest list's content                     */
    uint32_t tracker_count, tracker_stride, tracker_offset, _pad5;
} P1Radar;

#define P1_RADAR_V1_BYTES 208u
#define P1_RADAR_HAS(r, field) \
    ((r) != NULL && (r)->size_bytes >= (uint32_t)(offsetof(P1Radar, field) + sizeof(((P1Radar*)0)->field)))

/* ---- accessors (offsets are from the start of the SNAPSHOT) ------------------------- */
static inline const P1Radar* p1_radar_get(const P1Snapshot* s) {
    if (!P1_SNAP_HAS(s, radar_bytes) || s->radar_offset == 0 || s->radar_bytes < P1_RADAR_V1_BYTES) return NULL;
    if ((uint64_t)s->radar_offset + s->radar_bytes > s->size_bytes) return NULL;
    return (const P1Radar*)((const char*)s + s->radar_offset);
}
static inline const P1EntityRadar* p1_entity_radar_at(const P1Snapshot* s, uint32_t i) {
    const P1Radar* r = p1_radar_get(s);
    if (!r || i >= r->entity_count || r->entity_stride < P1_ENTITY_RADAR_V1_BYTES) return NULL;
    return (const P1EntityRadar*)((const char*)s + r->entity_offset + (size_t)i * r->entity_stride);
}
static inline const P1QuestState* p1_quest_at(const P1Snapshot* s, uint32_t i) {
    const P1Radar* r = p1_radar_get(s);
    if (!r || i >= r->quest_count || r->quest_stride < P1_QUEST_STATE_V1_BYTES) return NULL;
    return (const P1QuestState*)((const char*)s + r->quest_offset + (size_t)i * r->quest_stride);
}
static inline const P1QuestTrackerEntry* p1_tracker_at(const P1Snapshot* s, uint32_t i) {
    const P1Radar* r = p1_radar_get(s);
    if (!r || i >= r->tracker_count || r->tracker_stride < P1_QUEST_TRACKER_V1_BYTES) return NULL;
    return (const P1QuestTrackerEntry*)((const char*)s + r->tracker_offset + (size_t)i * r->tracker_stride);
}
/* The state of quest `id` (binary search); 0 when the log has no such quest. */
static inline int p1_quest_find(const P1Snapshot* s, const char* id, int32_t* state) {
    const P1Radar* r = p1_radar_get(s);
    if (!r || !id) return 0;
    uint32_t lo = 0, hi = r->quest_count;
    while (lo < hi) {
        const uint32_t mid = lo + (hi - lo) / 2;
        const P1QuestState* q = p1_quest_at(s, mid);
        if (!q) return 0;
        const int c = strcmp(p1_str_get(s, q->id), id);
        if (c == 0) { if (state) *state = q->state; return 1; }
        if (c < 0) lo = mid + 1; else hi = mid;
    }
    return 0;
}

/* The stable name of a category ("MonsterRare"; the app's config key). "" for NONE. */
static inline const char* p1_radar_category_key(uint8_t c) {
    static const char* const k[P1_RCAT_COUNT] = {
        "Self", "Player", "Minion", "MonsterNormal", "MonsterMagic", "MonsterRare", "MonsterUnique",
        "RogueExile", "NPC", "Chest", "ChestMagic", "ChestRare", "Strongbox", "Shrine", "AreaTransition",
        "Waypoint", "Checkpoint", "Portal", "Essence", "Breach", "Abyss", "Delirium", "Legion", "Harvest",
        "Blight", "Ritual", "Expedition", "Ultimatum", "Incursion", "Heist", "Betrayal", "Bestiary",
        "Harbinger", "TormentedSpirit", "QuestObject", "QuestItem", "GameIcon" };
    return c < P1_RCAT_COUNT ? k[c] : "";
}

/* ---- projection constants ------------------------------------------------------------ */
typedef struct P1RadarConsts {
    float camera_angle, height_to_grid, world_per_cell, map_scale_factor, large_center_mul_x, ui_base_height;
    int   large_pos_is_centre, shift_in_base_units;
} P1RadarConsts;

/* The app's values from the radar block, or the built-in defaults (the same values) when r
   is NULL (an older app). */
static inline P1RadarConsts p1_radar_consts(const P1Radar* r) {
    P1RadarConsts c;
    c.camera_angle = 0.67544246f;
    c.height_to_grid = 0.092f;
    c.world_per_cell = 250.0f / 23.0f;
    c.map_scale_factor = 2.3633678f;
    c.large_center_mul_x = 0.998184f;
    c.ui_base_height = 1600.0f;
    c.large_pos_is_centre = 1;
    c.shift_in_base_units = 0;
    if (r) {
        c.camera_angle = r->camera_angle;
        c.height_to_grid = r->height_to_grid;
        c.world_per_cell = r->world_per_cell;
        c.map_scale_factor = r->map_scale_factor;
        c.large_center_mul_x = r->large_center_mul_x;
        c.ui_base_height = r->ui_base_height;
        c.large_pos_is_centre = (r->flags & P1_RADAR_LARGE_POS_IS_CENTRE) != 0;
        c.shift_in_base_units = (r->flags & P1_RADAR_SHIFT_IN_BASE_UNITS) != 0;
    }
    return c;
}

/* ---- map transform -------------------------------------------------------------------- */
typedef struct P1MapTransform {
    float cx, cy;            /* map centre, game client px (where the player is drawn)       */
    float scale;             /* px per grid unit, before the cos/sin axis factors           */
    float cos_a, sin_a;      /* of the camera angle                                          */
    float height_to_grid;
    int   valid;
} P1MapTransform;

static inline P1MapTransform p1_make_transform(float cx, float cy, float scale, const P1RadarConsts* k) {
    P1MapTransform t;
    const P1RadarConsts d = p1_radar_consts(NULL);
    if (!k) k = &d;
    t.cx = cx; t.cy = cy; t.scale = scale;
    t.cos_a = cosf(k->camera_angle);
    t.sin_a = sinf(k->camera_angle);
    t.height_to_grid = k->height_to_grid;
    t.valid = isfinite(cx) && isfinite(cy) && isfinite(scale) && scale > 0.0f;
    return t;
}

/* Grid position (gx, gy) -> client px, relative to the player at (pgx, pgy). dz_world is the
   height to draw at minus the player's height (both negated world units); pass
   p1_terrain_height(t, g) - player_height for a point on the ground. */
static inline void p1_grid_to_map(const P1MapTransform* t, float pgx, float pgy, float gx, float gy,
                                  float dz_world, float* sx, float* sy) {
    const float dx = gx - pgx, dy = gy - pgy;
    const float z = dz_world * t->height_to_grid;
    *sx = t->cx + t->scale * ((dx - dy) * t->cos_a);
    *sy = t->cy + t->scale * ((z - (dx + dy)) * t->sin_a);
}

/* Inverse at dz 0 ("which cell is under this pixel"); 0 when it cannot be computed. */
static inline int p1_map_to_grid(const P1MapTransform* t, float pgx, float pgy, float sx, float sy,
                                 float* gx, float* gy) {
    if (!t->valid || !(t->scale > 0.0f) || t->cos_a == 0.0f || t->sin_a == 0.0f) return 0;
    const float a = (sx - t->cx) / (t->scale * t->cos_a);
    const float b = -(sy - t->cy) / (t->scale * t->sin_a);
    const float dx = 0.5f * (a + b), dy = 0.5f * (b - a);
    if (!isfinite(dx) || !isfinite(dy)) return 0;
    *gx = pgx + dx; *gy = pgy + dy;
    return 1;
}

static inline int p1_map_element_usable(const P1MapElement* m, float px_per_base) {
    return m->valid && m->visible && m->zoom > 0.0f && m->scale_rel > 0.0f && px_per_base > 0.0f;
}

/* The game's large map. px_per_base = client_h / ui_base_height. cal_* = the user's calibration
   (1, 0, 0 = none). Invalid when the map is not open. */
static inline P1MapTransform p1_large_map_transform(const P1MapElement* m, float px_per_base, float cal_scale,
                                                    float cal_dx, float cal_dy, const P1RadarConsts* k) {
    P1MapTransform t;
    memset(&t, 0, sizeof t);
    if (!p1_map_element_usable(m, px_per_base)) return t;
    const P1RadarConsts d = p1_radar_consts(NULL);
    if (!k) k = &d;
    float cx = m->pos_base_x * px_per_base, cy = m->pos_base_y * px_per_base;
    if (!k->large_pos_is_centre) {
        cx += m->size_base_x * 0.5f * px_per_base;
        cy += m->size_base_y * 0.5f * px_per_base;
    }
    cx *= k->large_center_mul_x;
    const float mul = k->shift_in_base_units ? px_per_base : 1.0f;
    cx += (m->shift_x + m->default_shift_x) * mul + cal_dx;
    cy += (m->shift_y + m->default_shift_y) * mul + cal_dy;
    return p1_make_transform(cx, cy, m->zoom * k->map_scale_factor * m->scale_rel * px_per_base * cal_scale, k);
}

/* The game's minimap (top-right). Clip your drawing to p1_mini_map_clip. */
static inline P1MapTransform p1_mini_map_transform(const P1MapElement* m, float px_per_base, float cal_scale,
                                                   float cal_dx, float cal_dy, const P1RadarConsts* k) {
    P1MapTransform t;
    memset(&t, 0, sizeof t);
    if (!p1_map_element_usable(m, px_per_base)) return t;
    const P1RadarConsts d = p1_radar_consts(NULL);
    if (!k) k = &d;
    const float mul = k->shift_in_base_units ? px_per_base : 1.0f;
    const float cx = (m->pos_base_x + m->size_base_x * 0.5f) * px_per_base + (m->shift_x + m->default_shift_x) * mul + cal_dx;
    const float cy = (m->pos_base_y + m->size_base_y * 0.5f) * px_per_base + (m->shift_y + m->default_shift_y) * mul + cal_dy;
    return p1_make_transform(cx, cy, m->zoom * k->map_scale_factor * m->scale_rel * px_per_base * cal_scale, k);
}

/* The minimap's client-px rectangle {min_x, min_y, max_x, max_y}. */
static inline void p1_mini_map_clip(const P1MapElement* m, float px_per_base, float out[4]) {
    out[0] = m->pos_base_x * px_per_base;
    out[1] = m->pos_base_y * px_per_base;
    out[2] = (m->pos_base_x + m->size_base_x) * px_per_base;
    out[3] = (m->pos_base_y + m->size_base_y) * px_per_base;
}

/* Snapshot conveniences: the transform the app's radar would use right now, with the user's
   calibration and the app's constants. client_h = P1Frame.viewport.client_h. Invalid when
   the map is closed, the radar reads are off (P1_SNAP_MAP_VALID) or the data is missing. */
static inline P1MapTransform p1_radar_large_map(const P1Snapshot* s, float client_h) {
    P1MapTransform t;
    memset(&t, 0, sizeof t);
    if (!s || !(s->flags & P1_SNAP_MAP_VALID) || !(client_h > 0.0f)) return t;
    const P1Radar* r = p1_radar_get(s);
    const P1RadarConsts k = p1_radar_consts(r);
    return p1_large_map_transform(&s->large_map, client_h / k.ui_base_height, r ? r->large_cal_scale : 1.0f,
                                  r ? r->large_cal_dx : 0.0f, r ? r->large_cal_dy : 0.0f, &k);
}
static inline P1MapTransform p1_radar_mini_map(const P1Snapshot* s, float client_h) {
    P1MapTransform t;
    memset(&t, 0, sizeof t);
    if (!s || !(s->flags & P1_SNAP_MAP_VALID) || !(client_h > 0.0f)) return t;
    const P1Radar* r = p1_radar_get(s);
    const P1RadarConsts k = p1_radar_consts(r);
    return p1_mini_map_transform(&s->mini_map, client_h / k.ui_base_height, r ? r->mini_cal_scale : 1.0f,
                                 r ? r->mini_cal_dx : 0.0f, r ? r->mini_cal_dy : 0.0f, &k);
}

/* A standalone square panel of your own, the app radar's way: side = fsize * client_h (capped
   to the client), top-left at (fx * client_w, fy * client_h) clamped inside the client.
   out = {min_x, min_y, max_x, max_y}; 0 when the client size is not usable. */
static inline int p1_panel_rect(float client_w, float client_h, float fx, float fy, float fsize, float out[4]) {
    if (!(client_w > 0.0f) || !(client_h > 0.0f)) return 0;
    float size = fsize * client_h;
    if (!(size > 0.0f)) return 0;
    const float lim = client_w < client_h ? client_w : client_h;
    if (size > lim) size = lim;
    float x = fx * client_w, y = fy * client_h;
    if (!isfinite(x)) x = 0.0f;
    if (!isfinite(y)) y = 0.0f;
    if (x > client_w - size) x = client_w - size;
    if (x < 0.0f) x = 0.0f;
    if (y > client_h - size) y = client_h - size;
    if (y < 0.0f) y = 0.0f;
    out[0] = x; out[1] = y; out[2] = x + size; out[3] = y + size;
    return 1;
}
/* The panel's transform: centred, zoom * px_per_base * 2 px per grid unit (the app's panel). */
static inline P1MapTransform p1_panel_transform(const float rect[4], float zoom, float px_per_base,
                                                const P1RadarConsts* k) {
    return p1_make_transform((rect[0] + rect[2]) * 0.5f, (rect[1] + rect[3]) * 0.5f, zoom * px_per_base * 2.0f, k);
}

/* An entity's float grid position: render_pos / world_per_cell, else its integer grid. */
static inline void p1_entity_grid(const P1Entity* e, float world_per_cell, float* gx, float* gy) {
    const float rx = e->render_pos.x, ry = e->render_pos.y;
    if ((rx != 0.0f || ry != 0.0f) && isfinite(rx) && isfinite(ry) && world_per_cell > 0.0f) {
        *gx = rx / world_per_cell; *gy = ry / world_per_cell;
    } else {
        *gx = (float)e->grid_x; *gy = (float)e->grid_y;
    }
}

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* P1SDK_P1_RADAR_H */
