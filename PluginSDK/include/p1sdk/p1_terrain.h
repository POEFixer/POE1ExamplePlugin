/* ============================================================================
 * p1_terrain.h — the current area's terrain, for plugins that build a radar, v1.
 *
 * What it is: the same per-area data the app's own radar draws from — the
 * walkable grid (the game's movement layer), the height of every grid cell,
 * the TGT tile files and the generated rooms. One P1Terrain per area, built by
 * the app's worker once per area (seconds after entering it) and never changed
 * afterwards.
 *
 * Who gets it: only a plugin whose plugin.json lists the "radar" permission
 * (P1_PERM_RADAR). Such a plugin also turns the app's radar reads on while it
 * runs (map elements, minimap icons, exits, the quest log: p1_radar.h).
 *
 * How it arrives (p1_abi.h):
 *   - P1Frame.terrain: the newest terrain, valid until the callback returns
 *     (NULL without the permission or before the first one). Check that it is
 *     the CURRENT area's with p1_terrain_current(frame) (the serial must match
 *     the snapshot's radar block): on an area change the old one stays until
 *     the new one is built.
 *   - P1_EV_TERRAIN_CHANGED: a new terrain arrived (a = its serial).
 *   - P1HostApi.terrain_hold / terrain_release: keep a terrain alive past the
 *     callback, e.g. to build a texture on your own worker thread.
 *
 * Grid coordinates: x right, y down in the game's grid; one cell = 250/23 world
 * units; one tile = 23 x 23 cells. An entity's grid position is
 * render_pos / (250/23) (p1_radar.h: p1_entity_grid).
 *
 * Layout rules (append-only, versioned) as in p1_snapshot.h: offsets are bytes
 * from the start of the P1Terrain, arrays carry their own stride, strings are
 * p1_str offsets into this terrain's own pool (read them with p1_terrain_str).
 * The plugin host validated every offset, count and size before handing it out.
 * ========================================================================== */
#ifndef P1SDK_P1_TERRAIN_H
#define P1SDK_P1_TERRAIN_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "p1_snapshot.h"

#ifdef __cplusplus
extern "C" {
#endif

#define P1_TERRAIN_VERSION       1u
#define P1_TERRAIN_CELLS_PER_TILE 23

/* ---- flags ------------------------------------------------------------------ */
enum {
    P1_TERRAIN_SUBTILE_HEIGHTS = 1u << 0,  /* heights include the per-cell sub-tile detail
                                              (else whole tiles are flat steps)           */
    P1_TERRAIN_HEIGHTS_OMITTED = 1u << 1,  /* the area was too large: no height grid     */
    P1_TERRAIN_TGT_OMITTED     = 1u << 2,  /* the area was too large: no TGT records     */
    P1_TERRAIN_ROOMS_OMITTED   = 1u << 3,  /* the area was too large: no rooms           */
    P1_TERRAIN_WALK_OMITTED    = 1u << 4,  /* the area was too large: no walkable grid   */
    P1_TERRAIN_TGT_RAW_KEYS    = 1u << 5   /* some TGT keys did not parse: file holds the
                                              whole key and key_x/key_y read 0xFF          */
};

/* ---- height grid formats ------------------------------------------------------ */
enum {
    P1_HEIGHT_NONE = 0,   /* flat terrain (no height grid)                                */
    P1_HEIGHT_I16  = 1,   /* int16 per cell; height = value * height_scale (exact)       */
    P1_HEIGHT_F32  = 2    /* float per cell; height = value * height_scale (scale 1)     */
};

/* ---- rooms read outcome ------------------------------------------------------------ */
enum { P1_ROOMS_NOT_READ = 0, P1_ROOMS_OK = 1, P1_ROOMS_FAILED = 2 };

/* One tile that names a TGT file (tile art). The app's own key for a tile is
 * "<file>:x:<key_x>-y:<key_y>" (p1_terrain_tgt_key), the format the radar's
 * targets and quest-guide exits are written in. Its grid box is
 * [tile * 23, tile * 23 + 23). stride = terrain->tgt_stride. */
typedef struct P1TerrainTgt {
    uint16_t tile_x, tile_y;
    uint8_t  key_x, key_y;       /* the tile's cell inside its TGT (already rotation-swapped) */
    uint16_t _pad0;
    p1_str   file;               /* UTF-8 TGT path as the game names it                       */
} P1TerrainTgt;

/* One room the level generator placed: its .arm file and its tile box. Bounds
 * are INCLUSIVE tiles; the grid box is [min * 23, (max + 1) * 23).
 * stride = terrain->room_stride. */
typedef struct P1TerrainRoom {
    p1_str   name;               /* UTF-8 .arm path ("Metadata/Terrain/.../x.arm")          */
    int16_t  min_x, min_y, max_x, max_y;
} P1TerrainRoom;

typedef struct P1Terrain {
    uint32_t size_bytes;         /* total bytes (header + grids + arrays + strings)          */
    uint32_t header_bytes;       /* sizeof(P1Terrain) as the app wrote it                     */
    uint32_t version;            /* P1_TERRAIN_VERSION                                        */
    uint32_t flags;              /* P1_TERRAIN_*                                               */
    uint64_t serial;             /* unique per terrain the app built; equals the snapshot radar
                                    block's terrain_serial while this is the current area's   */
    uint32_t area_hash;          /* the area hash when it was BUILT (a terrain re-used for a
                                    portal round trip keeps it): match on serial, not hash   */
    p1_str   area_id;            /* WorldArea Id when it was built ("1_1_2")                 */
    uint32_t tiles_x, tiles_y;   /* size in tiles                                             */
    uint32_t cells_per_tile;     /* 23                                                        */
    uint32_t build_ms;           /* how long the app took to build it (diagnostic)            */

    /* walkable grid: packed nibbles, two cells per byte, row-major, EVEN x in the LOW
       nibble. Value 0 = blocked, 1 = walkable next to an obstacle, 2+ = walkable interior. */
    uint32_t walk_width;         /* cells = walk_bytes_per_row * 2                            */
    uint32_t walk_height;        /* cells                                                     */
    uint32_t walk_bytes_per_row;
    uint32_t walk_offset;
    uint32_t walk_bytes;         /* = walk_bytes_per_row * walk_height (0 = omitted)          */

    /* height grid: one value per cell, row-major, (tiles_x*23) x (tiles_y*23). It can be
       0..2 cells narrower than the walkable grid (the walkable rows are padded to bytes).
       World units, NEGATED: higher ground is MORE NEGATIVE (the game's convention). */
    uint32_t height_format;      /* P1_HEIGHT_*                                               */
    uint32_t height_width, height_height;
    uint32_t height_offset, height_bytes;
    float    height_scale;       /* world height = stored value * height_scale                */
    uint32_t _pad0;

    uint32_t tgt_count, tgt_stride, tgt_offset, _pad1;
    uint32_t room_count, room_stride, room_offset;
    uint32_t room_state;         /* P1_ROOMS_*                                                */
    p1_str   rooms_reason;       /* why the room read failed ("" when ok)                     */
    uint32_t string_offset, string_bytes;   /* pool; byte 0 is always '\0'                   */
    uint32_t _pad2;
} P1Terrain;

#define P1_TERRAIN_HAS(t, field) \
    ((t) != NULL && (t)->header_bytes >= (uint32_t)(offsetof(P1Terrain, field) + sizeof(((P1Terrain*)0)->field)))

/* v1 element sizes: an accessor accepts any stride at least this big, so a newer
   app may append fields to an element without breaking a plugin built today. */
#define P1_TERRAIN_TGT_V1_BYTES  12u
#define P1_TERRAIN_ROOM_V1_BYTES 12u

/* ---- accessors ------------------------------------------------------------------------ */
static inline const char* p1_terrain_str(const P1Terrain* t, p1_str off) {
    if (!t || off == 0 || off >= t->string_bytes) return "";
    return (const char*)t + t->string_offset + off;
}

/* The walkable value of cell (x, y): 0..15, 0 outside the grid or when omitted. */
static inline uint8_t p1_terrain_cell(const P1Terrain* t, int x, int y) {
    if (!t || t->walk_bytes == 0 || x < 0 || y < 0 || (uint32_t)x >= t->walk_width || (uint32_t)y >= t->walk_height)
        return 0;
    const size_t idx = (size_t)y * t->walk_bytes_per_row + (size_t)(x >> 1);
    if (idx >= t->walk_bytes) return 0;
    const uint8_t b = ((const uint8_t*)t + t->walk_offset)[idx];
    return (uint8_t)((x & 1) == 0 ? (b & 0x0F) : (b >> 4));
}
static inline int p1_terrain_walkable(const P1Terrain* t, int x, int y) { return p1_terrain_cell(t, x, y) != 0; }

/* The height of cell (x, y) in world units (negated: higher = more negative); 0 outside
   the grid or on flat terrain. */
static inline float p1_terrain_height_at(const P1Terrain* t, int x, int y) {
    if (!t || t->height_format == P1_HEIGHT_NONE || x < 0 || y < 0 ||
        (uint32_t)x >= t->height_width || (uint32_t)y >= t->height_height) return 0.0f;
    const size_t i = (size_t)y * t->height_width + (size_t)x;
    const uint8_t* base = (const uint8_t*)t + t->height_offset;
    if (t->height_format == P1_HEIGHT_I16) {
        if ((i + 1) * 2 > t->height_bytes) return 0.0f;
        int16_t q; memcpy(&q, base + i * 2, 2);
        return (float)q * t->height_scale;
    }
    if (t->height_format == P1_HEIGHT_F32) {
        if ((i + 1) * 4 > t->height_bytes) return 0.0f;
        float f; memcpy(&f, base + i * 4, 4);
        return f * t->height_scale;
    }
    return 0.0f;
}

/* The height under a float grid position — the app's radar uses exactly this (the cell the
   position truncates into; 0 for a negative or NaN position). */
static inline float p1_terrain_height(const P1Terrain* t, float gx, float gy) {
    if (!(gx >= 0.0f) || !(gy >= 0.0f)) return 0.0f;
    return p1_terrain_height_at(t, (int)gx, (int)gy);
}

static inline const P1TerrainTgt* p1_terrain_tgt_at(const P1Terrain* t, uint32_t i) {
    if (!t || i >= t->tgt_count || t->tgt_stride < P1_TERRAIN_TGT_V1_BYTES) return NULL;
    return (const P1TerrainTgt*)((const char*)t + t->tgt_offset + (size_t)i * t->tgt_stride);
}
static inline const P1TerrainRoom* p1_terrain_room_at(const P1Terrain* t, uint32_t i) {
    if (!t || i >= t->room_count || t->room_stride < P1_TERRAIN_ROOM_V1_BYTES) return NULL;
    return (const P1TerrainRoom*)((const char*)t + t->room_offset + (size_t)i * t->room_stride);
}

/* The app's key of a TGT tile: "<file>:x:<key_x>-y:<key_y>" (or the raw file text when the
   key did not parse). Writes at most cap bytes, NUL-terminated; returns the key's length. */
static inline int p1_terrain_tgt_key(const P1Terrain* t, const P1TerrainTgt* g, char* buf, size_t cap) {
    if (!g || !buf || cap == 0) return 0;
    const char* file = p1_terrain_str(t, g->file);
    int n;
    if (g->key_x == 0xFF && g->key_y == 0xFF && t && (t->flags & P1_TERRAIN_TGT_RAW_KEYS))
        n = snprintf(buf, cap, "%s", file);
    else
        n = snprintf(buf, cap, "%s:x:%u-y:%u", file, (unsigned)g->key_x, (unsigned)g->key_y);
    return n < 0 ? 0 : n;
}

/* ---- terrain image (the app radar's own outline / fill, as RGBA pixels) ----------------- */
typedef struct P1TerrainStyle {
    uint32_t color;          /* IM_COL32 layout: byte 0 = R, 1 = G, 2 = B, 3 = A (0xFFFFFFFF = white;
                                draw white and tint the image with the vertex colour)               */
    int32_t  border;         /* square brush radius in cells, 0..3                                  */
    int32_t  filled;         /* 0 = outline (the walls), 1 = every walkable cell                    */
    int32_t  lift_by_height; /* 1 = shift each cell by its height, the way the game map shows it    */
} P1TerrainStyle;

/* The image size for `cells_per_texel` cells per texel (1 = full size; 2 halves each side). */
static inline void p1_terrain_image_size(const P1Terrain* t, int cells_per_texel, int* w, int* h) {
    const int ds = cells_per_texel < 1 ? 1 : cells_per_texel;
    const int W = t ? (int)t->walk_width : 0, H = t ? (int)t->walk_height : 0;
    if (w) *w = W > 0 ? (W + ds - 1) / ds : 0;
    if (h) *h = H > 0 ? (H + ds - 1) / ds : 0;
}

/* Paints the terrain into rgba (out_w x out_h texels, 4 bytes each, row-major, the caller
   zero-fills it first). A texel is painted when ANY cell it covers is painted, so thin walls
   survive a downscale. With cells_per_texel 1 the result is byte-identical to the app radar's
   own texture (Radar::BuildWalkablePixels). Texel (i, j) covers grid cells
   [i*ds, (i+1)*ds) x [j*ds, (j+1)*ds). Returns 0 when there is nothing to paint (no walkable
   grid, a buffer too small). Pure CPU: call it on any thread (hold the terrain meanwhile). */
static inline int p1_terrain_pixels(const P1Terrain* t, const P1TerrainStyle* st, int cells_per_texel,
                                    uint8_t* rgba, int out_w, int out_h) {
    if (!t || !st || !rgba || t->walk_bytes == 0) return 0;
    const int width = (int)t->walk_width, height = (int)t->walk_height;
    const int ds = cells_per_texel < 1 ? 1 : cells_per_texel;
    int need_w = 0, need_h = 0;
    p1_terrain_image_size(t, ds, &need_w, &need_h);
    if (width <= 0 || height <= 0 || out_w < need_w || out_h < need_h) return 0;

    const int has_height = st->lift_by_height && t->height_format != P1_HEIGHT_NONE &&
                           t->height_width > 0 && t->height_height > 0;
    const int height_w = has_height ? (int)t->height_width : width;
    const int height_h = has_height ? (int)t->height_height : height;
    /* height (world units) / (2 * world units per cell) -> whole cells (the app's constant expression) */
    const float world_per_cell = 250.0f / (float)P1_TERRAIN_CELLS_PER_TILE;
    const float inv_height_mult = 1.0f / (world_per_cell * 2.0f);
    const int loop_max_y = height < height_h ? height : height_h;
    const int loop_max_x = width < height_w ? width : height_w;
    const int border = st->border < 0 ? 0 : (st->border > 3 ? 3 : st->border);
    const uint8_t cr = (uint8_t)(st->color & 0xFF), cg = (uint8_t)((st->color >> 8) & 0xFF);
    const uint8_t cb = (uint8_t)((st->color >> 16) & 0xFF), ca = (uint8_t)((st->color >> 24) & 0xFF);
    const int x_begin = st->filled ? 0 : 1;
    const int x_end = st->filled ? loop_max_x : loop_max_x - 1;

    for (int y = 0; y < loop_max_y; ++y) {
        for (int x = x_begin; x < x_end; ++x) {
            const uint8_t v = p1_terrain_cell(t, x, y);
            if (st->filled) {
                if (v == 0) continue;
            } else {
                if (v != 1 && v != 0) continue;              /* interior walkable cell */
                if (!(p1_terrain_cell(t, x, y + 1) || p1_terrain_cell(t, x, y - 1) ||
                      p1_terrain_cell(t, x - 1, y) || p1_terrain_cell(t, x + 1, y))) continue;
            }
            int off = 0;
            if (has_height) off = (int)(p1_terrain_height_at(t, x, y) * inv_height_mult);
            const int image_x = x - off, image_y = y - off;
            for (int dy = -border; dy <= border; ++dy) {
                const int py = image_y + dy;
                if ((unsigned)py >= (unsigned)height) continue;
                for (int dx = -border; dx <= border; ++dx) {
                    const int pxx = image_x + dx;
                    if ((unsigned)pxx >= (unsigned)width) continue;
                    uint8_t* p = rgba + ((size_t)(py / ds) * (size_t)out_w + (size_t)(pxx / ds)) * 4;
                    p[0] = cr; p[1] = cg; p[2] = cb; p[3] = ca;
                }
            }
        }
    }
    return 1;
}

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* P1SDK_P1_TERRAIN_H */
