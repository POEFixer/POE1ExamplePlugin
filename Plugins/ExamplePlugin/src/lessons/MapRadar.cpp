// Lesson "maprad": a radar of your own on the game's maps, from the radar data the app hands a
// plugin with the "radar" permission (the area's terrain, each entity's category, the projection).
#include <algorithm>
#include <cmath>

#include "JsonUtil.h"
#include "lessons/Common.h"

namespace ex {

namespace {

constexpr LessonInfo kInfo{
    .id = "maprad",
    .group = Group::Advanced,
    .icon = ICON_FA_MAP_LOCATION_DOT,
    .title = "Build a radar",
    .summary = "Your own radar on the game's large map, on its minimap or in a panel: the area's walls, the "
               "entities by kind, the player, the exits.",
    .what = "With \"radar\" in plugin.json the app runs its radar reads while this plugin runs and hands it two "
            "things: each area's terrain (P1Frame.terrain: walkable grid, heights, tiles, rooms; p1_terrain.h) and "
            "a radar block in every snapshot (p1_radar.h: why the reads are on, how far the terrain is, each "
            "entity's category and grid position, the user's map calibration, the quest log). p1::TerrainTexture "
            "turns the terrain into a texture on a thread; p1::Radar::LargeMap / MiniMap give the transform the "
            "app's own radar uses.",
    .when = "Anything drawn on the game's maps: walls, monsters, exits, a path of your own. Use the block's "
            "category instead of guessing from metadata paths, and the SDK's transform instead of your own math.",
    .limits = "Free for every key. A new area's terrain arrives seconds after the area is entered; until then the "
              "frame keeps the previous one, so draw only p1_terrain_current. A terrain is often 10 to 30 MB (at most "
              "64 MiB) and counts "
              "against memoryLimitMB (512 here). The walls are ONE AddImageQuad; shapes of one texture and one clip "
              "batch into one draw call: the overlay takes 150,000 vertices and the app draws at most 1,024 commands "
              "a frame. An app older than this SDK refuses a plugin.json with \"radar\".",
    .snippet = "maprad",
};

// ---- options ------------------------------------------------------------------------------------
enum class Where : int { Auto, Large, Mini, Panel };
constexpr const char* kWhereIds[] = { "auto", "large", "mini", "panel" };   // settings.json values
constexpr const char* kWhereNames[] = { "Auto: the large map, else the minimap, else a panel", "The large map only",
                                        "The minimap only", "A panel of its own" };

struct RadarOptions {
    bool on = false;            // draw the radar over the game (off by default, like every overlay layer)
    Where where = Where::Auto;
    bool filled = false;        // false: the walls (outline); true: every walkable cell
    int border = 1;             // brush radius in cells, 0..3: thicker walls
    ImVec4 color{ 0.58f, 0.76f, 1.0f, 0.60f };   // the walls' tint (the image is white)
    float zoom = 1.0f;          // the panel's zoom, as the app's own radar panel has it
    bool monsters = true, chests = true, league = true;
};

P1TerrainStyle StyleOf(const RadarOptions& o) {
    // White, tinted when drawn: a colour change costs nothing, only the shape options rebuild.
    return P1TerrainStyle{ 0xFFFFFFFFu, o.border, o.filled ? 1 : 0, 1 };
}

bool GroupShown(const RadarOptions& o, uint8_t group) {
    if (group == P1_RGROUP_MONSTERS) return o.monsters;
    if (group == P1_RGROUP_CHESTS) return o.chests;
    if (group == P1_RGROUP_LEAGUES) return o.league;
    return true;
}

ImU32 GroupColor(uint8_t group) {
    switch (group) {
        case P1_RGROUP_PLAYERS:  return IM_COL32(110, 215, 255, 255);
        case P1_RGROUP_MONSTERS: return IM_COL32(240, 84, 72, 255);
        case P1_RGROUP_NPCS:     return IM_COL32(64, 214, 196, 255);
        case P1_RGROUP_CHESTS:   return IM_COL32(222, 178, 110, 255);
        case P1_RGROUP_WORLD:    return IM_COL32(150, 200, 255, 255);
        case P1_RGROUP_LEAGUES:  return IM_COL32(205, 125, 255, 255);
        case P1_RGROUP_QUEST:    return IM_COL32(125, 235, 120, 255);
        default:                 return IM_COL32(205, 205, 205, 255);
    }
}

const char* GroupName(uint8_t group) {
    switch (group) {
        case P1_RGROUP_PLAYERS:  return "players";
        case P1_RGROUP_MONSTERS: return "monsters";
        case P1_RGROUP_NPCS:     return "NPCs";
        case P1_RGROUP_CHESTS:   return "chests";
        case P1_RGROUP_WORLD:    return "world: exits, waypoints, shrines";
        case P1_RGROUP_LEAGUES:  return "league mechanics";
        case P1_RGROUP_QUEST:    return "quest objects";
        default:                 return "other";
    }
}

// One entity's shape: its group picks the shape and the colour, its category the size. Few
// segments: a map has hundreds of these.
void DrawShape(ImDrawList* dl, ImVec2 p, const P1EntityRadar& r, float scale) {
    const ImU32 col = GroupColor(r.group), edge = IM_COL32(0, 0, 0, 190);
    float s = 2.6f;
    if (r.category == P1_RCAT_MONSTER_MAGIC) s = 3.1f;
    if (r.category == P1_RCAT_MONSTER_RARE || r.category == P1_RCAT_ROGUE_EXILE) s = 3.9f;
    if (r.category == P1_RCAT_MONSTER_UNIQUE || r.category == P1_RCAT_STRONGBOX) s = 4.7f;
    if (r.group == P1_RGROUP_WORLD || r.group == P1_RGROUP_LEAGUES || r.group == P1_RGROUP_QUEST) s = 4.2f;
    s *= scale;
    switch (r.group) {
        case P1_RGROUP_CHESTS:
            dl->AddRectFilled(ImVec2(p.x - s - 1, p.y - s - 1), ImVec2(p.x + s + 1, p.y + s + 1), edge);
            dl->AddRectFilled(ImVec2(p.x - s, p.y - s), ImVec2(p.x + s, p.y + s), col);
            break;
        case P1_RGROUP_WORLD:
            dl->AddQuadFilled(ImVec2(p.x, p.y - s - 1.5f), ImVec2(p.x + s + 1.5f, p.y), ImVec2(p.x, p.y + s + 1.5f),
                              ImVec2(p.x - s - 1.5f, p.y), edge);
            dl->AddQuadFilled(ImVec2(p.x, p.y - s), ImVec2(p.x + s, p.y), ImVec2(p.x, p.y + s), ImVec2(p.x - s, p.y), col);
            break;
        case P1_RGROUP_LEAGUES:
        case P1_RGROUP_QUEST:
            dl->AddTriangleFilled(ImVec2(p.x, p.y - s - 2), ImVec2(p.x + s + 2, p.y + s + 1), ImVec2(p.x - s - 2, p.y + s + 1), edge);
            dl->AddTriangleFilled(ImVec2(p.x, p.y - s), ImVec2(p.x + s, p.y + s), ImVec2(p.x - s, p.y + s), col);
            break;
        default:
            dl->AddCircleFilled(p, s + 1.0f, edge, 8);
            dl->AddCircleFilled(p, s, col, 8);
            if (r.category == P1_RCAT_MONSTER_RARE || r.category == P1_RCAT_MONSTER_UNIQUE)
                dl->AddCircle(p, s + 2.5f * scale, col, 10, 1.5f);   // the ones that matter, ringed
            break;
    }
}

// [snippet:maprad]
// One radar frame on transform t (p1::Radar::LargeMap / MiniMap, or a panel's).
// The walls first, as ONE textured quad under everything; then each entity the
// app's radar would draw, as a small shape: same texture and clip, so they all
// batch into one draw call. Exits get their target area's id. The player last,
// at the map centre. me = the player as the app's radar centres on it.
int DrawRadar(ImDrawList* under, ImDrawList* over, const P1Frame& f, const P1MapTransform& t,
              const p1::Radar::Player& me, const p1::TerrainTexture& walls, const RadarOptions& o,
              float scale) {
    walls.Draw(under, f, t, me, ImGui::ColorConvertFloat4ToU32(o.color));
    const P1Snapshot& s = *f.snapshot;
    int drawn = 0;
    uint32_t i = 0;
    for (const P1EntityRadar& r : p1::Radar::Entities(s)) {   // parallel to p1::Entities(s)
        const P1Entity* e = p1_entity_at(&s, i++);
        if (!e || r.category == P1_RCAT_NONE) continue;         // the app's radar skips it
        if (r.flags & (P1_ERAD_HIDDEN | P1_ERAD_LOCAL_PLAYER)) continue;   // opened, dead; the player
        if (!GroupShown(o, r.group)) continue;
        const ImVec2 p = p1::Radar::ToMap(t, me, r.grid_x, r.grid_y, r.height);
        DrawShape(over, p, r, scale);
        const char* target = p1::Str(s, e->transition_target);
        if (r.category == P1_RCAT_AREA_TRANSITION && target[0])
            OutlinedText(over, ImVec2(p.x + 7.0f * scale, p.y - 7.0f * scale), IM_COL32(205, 228, 255, 255), target);
        ++drawn;
    }
    const ImVec2 c(t.cx, t.cy);                                  // ToMap of the player itself
    over->AddCircleFilled(c, 5.0f * scale, IM_COL32(0, 0, 0, 220), 12);
    over->AddCircleFilled(c, 3.5f * scale, IM_COL32(255, 255, 255, 255), 12);
    return drawn;
}
// [/snippet]

// Where the radar goes this frame, and the clip rectangle when it has one.
enum class Shown : uint8_t { None, Large, Mini, Panel };
struct Target {
    Shown shown = Shown::None;
    P1MapTransform t{};
    bool clip = false;
    ImVec2 mn, mx;
};

// The app radar's default standalone panel: a square at the top right, 30 % of the client height.
constexpr float kPanelX = 0.78f, kPanelY = 0.08f, kPanelSize = 0.30f;

Target PickTarget(const P1Frame& f, const RadarOptions& o) {
    Target out;
    if (o.where == Where::Auto || o.where == Where::Large) {
        out.t = p1::Radar::LargeMap(f);
        if (out.t.valid) {
            out.shown = Shown::Large;
            return out;
        }
    }
    if (o.where == Where::Auto || o.where == Where::Mini) {
        out.t = p1::Radar::MiniMap(f);
        if (out.t.valid && p1::Radar::MiniMapClip(f, out.mn, out.mx)) {
            out.shown = Shown::Mini;
            out.clip = true;
            return out;
        }
    }
    if (o.where == Where::Auto || o.where == Where::Panel) {
        const P1RadarConsts k = p1_radar_consts(p1::Radar::Get(*f.snapshot));
        float r[4];
        if (p1_panel_rect(float(f.viewport.client_w), float(f.viewport.client_h), kPanelX, kPanelY, kPanelSize, r)) {
            out.t = p1_panel_transform(r, o.zoom, float(f.viewport.client_h) / k.ui_base_height, &k);
            out.shown = Shown::Panel;
            out.clip = true;
            out.mn = ImVec2(r[0], r[1]);
            out.mx = ImVec2(r[2], r[3]);
            return out;
        }
    }
    out.t = P1MapTransform{};
    return out;
}

const char* ShownText(Shown s) {
    switch (s) {
        case Shown::Large: return "on the large map";
        case Shown::Mini:  return "on the minimap";
        case Shown::Panel: return "in its own panel";
        default:           return "nowhere";
    }
}

const char* GateText(uint8_t gate) {
    switch (gate) {
        case P1_RGATE_OFF:         return "OFF: nobody asked for the radar reads";
        case P1_RGATE_ON:          return "ON: the radar reads ran this tick";
        case P1_RGATE_NOT_IN_GAME: return "NOT_IN_GAME: wanted, but the game is not in an area";
        case P1_RGATE_WGC:         return "WGC: the app is in visual mode and reads no game memory";
        default:                   return "unknown (a newer app)";
    }
}

const char* TerrainStageText(uint8_t stage) {
    switch (stage) {
        case P1_TSTAGE_OFF:            return "off";
        case P1_TSTAGE_HEADER_BAD:     return "the terrain header is unreadable";
        case P1_TSTAGE_SHAPE_REJECTED: return "the terrain's shape was rejected (offsets moved?)";
        case P1_TSTAGE_READING:        return "reading";
        case P1_TSTAGE_DEFERRED:       return "paused: a trade holds the reads";
        case P1_TSTAGE_COMPOSING:      return "composing the heights";
        case P1_TSTAGE_READY:          return "ready";
        case P1_TSTAGE_READ_FAILED:    return "a read failed: retried soon";
        default:                       return "unknown (a newer app)";
    }
}

const char* MapUiStageText(uint8_t stage) {
    switch (stage) {
        case P1_MAPUI_OFF:              return "off";
        case P1_MAPUI_OFFSETS_PENDING:  return "the map offsets are not derived for this game version";
        case P1_MAPUI_MAP_ELEMENT_BAD:  return "the map element is unreadable";
        case P1_MAPUI_OK:               return "ok";
        case P1_MAPUI_DEFERRED:         return "deferred (a trade): the last state, use it as ok";
        default:                        return "unknown (a newer app)";
    }
}

class MapRadarLesson final : public Lesson {
public:
    MapRadarLesson() : Lesson(kInfo), m_walls(StyleOf(RadarOptions{})) {}

    ui::Status GetStatus(const App&, const P1Frame& f) const override {
        const P1Snapshot& s = *f.snapshot;
        if (!(f.permissions & P1_PERM_RADAR))
            return Reason(ui::Tone::Warn, "No \"radar\" permission: plugin.json does not list it, so the app turns no "
                                          "radar read on for this plugin and P1Frame.terrain stays NULL. Add \"radar\" "
                                          "to \"permissions\".");
        if (auto gap = GameDataGap(s, Need::InGame)) return *gap;
        const P1Radar* r = p1::Radar::Get(s);
        if (!r)
            return Reason(ui::Tone::Warn, "No radar block in the snapshot: this app is older than the radar SDK "
                                          "(p1_radar_get is NULL). Nothing is drawn.");
        if (r->gate != P1_RGATE_ON) return Reason(ui::Tone::Info, "The radar reads are not running: gate %s.", GateText(r->gate));
        const P1Terrain* t = p1_terrain_current(&f);
        if (!t) {
            if (r->terrain_stage == P1_TSTAGE_READING)
                return Reason(ui::Tone::Info, "Building this area's terrain: %d of %d sub-tiles, %d of %d tile files read "
                                              "(terrain_stage READING).", r->sub_tiles_read, r->sub_tiles_total, r->tgt_read,
                              r->tgt_total);
            if (r->terrain_stage == P1_TSTAGE_READY)
                return Reason(ui::Tone::Info, "The terrain is ready (serial %llu); it reaches this plugin in a moment "
                                              "(P1_EV_TERRAIN_CHANGED).", static_cast<unsigned long long>(r->terrain_serial));
            return Reason(ui::Tone::Info, "No terrain for this area yet: terrain_stage is %s.", TerrainStageText(r->terrain_stage));
        }
        if (!m_opt.on)
            return Reason(ui::Tone::Info, "The terrain is here (%u x %u tiles); the preview below draws it. Tick \"Draw "
                                          "the radar\" to draw it over the game too.", t->tiles_x, t->tiles_y);
        const Target target = PickTarget(f, m_opt);
        if (target.shown == Shown::None)
            return Reason(ui::Tone::Info, "The radar has nowhere to go: \"%s\" is chosen and that map is closed.",
                          kWhereNames[static_cast<int>(m_opt.where)]);
        return Live("terrain %u x %u tiles (serial %llu), drawn %s, %d entities on it.", t->tiles_x, t->tiles_y,
                    static_cast<unsigned long long>(t->serial), ShownText(target.shown), m_lastDrawn);
    }

    void Tick(App& app, const P1Frame& f) override {
        // The texture only while someone looks at it: the overlay radar, or this page's preview.
        if (!m_opt.on && !app.IsShown(this, f)) return;
        m_walls.SetStyle(StyleOf(m_opt));   // a new outline / fill / border rebuilds the image
        m_walls.Update(f);                  // off-thread build, upload when ready, nothing when unchanged
    }

    void Event(App&, const P1Event& e) override {
        if (e.type != P1_EV_TERRAIN_CHANGED) return;
        ++m_terrainEvents;
        m_lastEventSerial = static_cast<uint64_t>(e.a);
    }

    void Overlay(App&, const P1Frame& f) override {
        if (!m_opt.on) return;
        const P1Snapshot& s = *f.snapshot;
        if (!(s.flags & P1_SNAP_IN_GAME) || !p1::Radar::Get(s)) return;   // an older app: nothing to draw
        const p1::Radar::Player me = p1::Radar::PlayerOf(f, &m_motion);
        const Target target = PickTarget(f, m_opt);
        if (!me.valid || target.shown == Shown::None) return;
        const float scale = UiScale(f);
        ImDrawList* under = ImGui::GetBackgroundDrawList();   // under every other layer
        ImDrawList* over = ImGui::GetWindowDrawList();
        if (target.clip) {
            under->PushClipRect(target.mn, target.mx, true);
            over->PushClipRect(target.mn, target.mx, true);
        }
        if (target.shown == Shown::Panel) {
            under->AddRectFilled(target.mn, target.mx, IM_COL32(8, 11, 16, 200), 6.0f * scale);
            under->AddRect(target.mn, target.mx, IM_COL32(255, 255, 255, 40), 6.0f * scale);
        }
        m_lastDrawn = DrawRadar(under, over, f, target.t, me, m_walls, m_opt, scale);
        if (target.clip) {
            over->PopClipRect();
            under->PopClipRect();
        }
    }

    void Draw(App& app, const P1Frame& f) override {
        DrawWhy(f);
        DrawPreview(f);
        if (DrawOptions()) app.MarkDirty();
        DrawBudget(app, f);
    }

    void Load(App&, const nlohmann::json& j) override {
        m_opt.on = js::Bool(j, "on", false);
        const std::string where = js::String(j, "map", "auto", 16);
        m_opt.where = Where::Auto;
        for (int i = 0; i < 4; ++i)
            if (where == kWhereIds[i]) m_opt.where = static_cast<Where>(i);
        m_opt.filled = js::Bool(j, "filled", false);
        m_opt.border = js::Int(j, "border", 1, 0, 3);
        m_opt.color = js::Color(j, "color", RadarOptions{}.color);
        m_opt.zoom = js::Float(j, "zoom", 1.0f, 0.25f, 4.0f);
        m_opt.monsters = js::Bool(j, "monsters", true);
        m_opt.chests = js::Bool(j, "chests", true);
        m_opt.league = js::Bool(j, "league", true);
    }

    void Save(const App&, nlohmann::json& j) const override {
        j["on"] = m_opt.on;
        j["map"] = kWhereIds[static_cast<int>(m_opt.where)];
        j["filled"] = m_opt.filled;
        j["border"] = m_opt.border;
        j["color"] = js::ColorJson(m_opt.color);
        j["zoom"] = m_opt.zoom;
        j["monsters"] = m_opt.monsters;
        j["chests"] = m_opt.chests;
        j["league"] = m_opt.league;
    }

    void Unload(App&) override { m_walls.Release(); }   // the texture, after a running build ends

private:
    // Why there is data, or not: every condition on the way, in order.
    void DrawWhy(const P1Frame& f) const {
        ui::SectionTitle(ICON_FA_LIST_CHECK, "Why there is data, or not");
        const P1Snapshot& s = *f.snapshot;
        const P1Radar* r = p1::Radar::Get(s);
        const P1Terrain* frameTerrain = P1_HAS_FIELD(&f, P1Frame, terrain) ? f.terrain : nullptr;
        const P1Terrain* t = p1_terrain_current(&f);
        if (!ui::BeginKv("why", { "permission", "radar block", "gate", "terrain build", "map elements", "terrain",
                                  "quests", "events" }))
            return;
        if (f.permissions & P1_PERM_RADAR) ui::KvTone("permission", ui::Tone::Ok, "\"radar\" (P1_PERM_RADAR)");
        else ui::KvTone("permission", ui::Tone::Warn, "missing: plugin.json has no \"radar\"");
        if (!r) {
            ui::KvTone("radar block", ui::Tone::Warn, "none: an older app, or no plugin with \"radar\" runs");
            ui::EndKv();
            return;
        }
        ui::KvText("radar block", "%u bytes, %u entities, flags 0x%X%s%s", r->size_bytes, r->entity_count, r->flags,
                   (r->flags & P1_RADAR_BY_APP) ? ", by the app's radar" : "",
                   (r->flags & P1_RADAR_BY_PLUGIN) ? ", by a plugin" : "");
        ui::KvTone("gate", r->gate == P1_RGATE_ON ? ui::Tone::Ok : ui::Tone::Info, "%s", GateText(r->gate));
        ui::KvTone("terrain build", r->terrain_stage == P1_TSTAGE_READY ? ui::Tone::Ok : ui::Tone::Info,
                   "%s; %d / %d sub-tiles, %d / %d tile files", TerrainStageText(r->terrain_stage), r->sub_tiles_read,
                   r->sub_tiles_total, r->tgt_read, r->tgt_total);
        ui::KvTone("map elements", r->map_ui_stage == P1_MAPUI_OK ? ui::Tone::Ok : ui::Tone::Info, "%s; large map %s, minimap %s",
                   MapUiStageText(r->map_ui_stage), s.large_map.visible ? "open" : "closed",
                   s.mini_map.visible ? "visible" : "hidden");
        if (t)
            ui::KvTone("terrain", ui::Tone::Ok, "%s, %u x %u tiles (%u x %u cells), serial %llu, %s; %u tile files, %u rooms",
                       p1_terrain_str(t, t->area_id), t->tiles_x, t->tiles_y, t->walk_width, t->walk_height,
                       static_cast<unsigned long long>(t->serial), FormatBytes(t->size_bytes).c_str(), t->tgt_count,
                       t->room_count);
        else if (frameTerrain)
            ui::KvTone("terrain", ui::Tone::Info, "the previous area's (serial %llu): the new one is on its way",
                       static_cast<unsigned long long>(frameTerrain->serial));
        else
            ui::KvTone("terrain", ui::Tone::Info, "none yet");
        ui::KvText("quests", "%u in the quest log%s, %u in the quest list", r->quest_count,
                   (r->flags & P1_RADAR_QUESTS_USABLE) ? " (settled)" : "", r->tracker_count);
        ui::KvText("events", "%llu P1_EV_TERRAIN_CHANGED, the last for serial %llu",
                   static_cast<unsigned long long>(m_terrainEvents), static_cast<unsigned long long>(m_lastEventSerial));
        ui::EndKv();
    }

    // The same DrawRadar, into this panel: a square of its own, with a transform of its own scale.
    void DrawPreview(const P1Frame& f) {
        ui::SectionTitle(ICON_FA_EYE, "Preview");
        const P1Snapshot& s = *f.snapshot;
        const p1::Radar::Player me = p1::Radar::PlayerOf(f, &m_motion);
        if (!p1::Radar::Get(s) || !me.valid || !(s.flags & P1_SNAP_IN_GAME)) {
            ui::TextDim("The radar draws here once the snapshot carries the radar block in game.");
            return;
        }
        const float side = std::floor((std::min)(ImGui::GetContentRegionAvail().x, ui::Em(20.0f)));
        const ImVec2 a = ImGui::GetCursorScreenPos(), b(a.x + side, a.y + side);
        ImGui::Dummy(ImVec2(side, side));
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(a, b, IM_COL32(8, 11, 16, 255), ui::Em(0.3f));
        dl->PushClipRect(a, b, true);
        // p1_make_transform: the radar's projection at any scale you like (here about 380 cells across).
        const P1RadarConsts k = p1_radar_consts(p1::Radar::Get(s));
        const P1MapTransform t = p1_make_transform((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f, side / 300.0f, &k);
        m_previewDrawn = DrawRadar(dl, dl, f, t, me, m_walls, m_opt, 1.0f);
        dl->PopClipRect();
        dl->AddRect(a, b, ImGui::GetColorU32(ImGuiCol_Border), ui::Em(0.3f));
        if (!m_walls.Ready(f))
            dl->AddText(ImVec2(a.x + ui::Em(0.5f), a.y + ui::Em(0.4f)), IM_COL32(200, 200, 200, 255),
                        p1_terrain_current(&f) ? "building the terrain image..." : "no terrain yet");
        // The legend: one line per group, in its colour.
        ImGui::SameLine(0.0f, ui::Em(1.0f));
        ImGui::BeginGroup();
        ui::TextDim("%d entities in the preview (player, hidden and filtered ones left out).", m_previewDrawn);
        for (uint8_t g = P1_RGROUP_PLAYERS; g <= P1_RGROUP_QUEST; ++g) {
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(GroupColor(g)), "%s", ICON_FA_CIRCLE);
            ImGui::SameLine();
            ImGui::TextUnformatted(GroupName(g));
        }
        ui::TextDim("Exits carry their target area's id.");
        ImGui::EndGroup();
    }

    // The options. Returns true when one changed (the caller marks the settings dirty).
    bool DrawOptions() {
        ui::SectionTitle(ICON_FA_SLIDERS, "Options");
        bool changed = false;
        if (!ui::BeginForm("radar", { "Draw the radar", "Where", "Walls", "Wall width", "Wall colour", "Panel zoom",
                                      "Show" }))
            return false;
        ui::FormRow("Draw the radar", "Over the game, every overlay frame. The preview above draws either way.");
        changed |= ImGui::Checkbox("##on", &m_opt.on);
        ui::FormRow("Where", "Auto follows the game: the large map while it is open (the game then hides the minimap), "
                             "else the minimap, else a panel at the top right.");
        int where = static_cast<int>(m_opt.where);
        if (ImGui::Combo("##where", &where, kWhereNames, IM_ARRAYSIZE(kWhereNames))) {
            m_opt.where = static_cast<Where>(where);
            changed = true;
        }
        ui::FormRow("Walls", "Outline: the cells at the edge of the walkable ground, as the app's radar draws them. "
                             "Filled: every walkable cell.");
        int filled = m_opt.filled ? 1 : 0;
        changed |= ImGui::RadioButton("outline", &filled, 0);
        ImGui::SameLine();
        changed |= ImGui::RadioButton("filled", &filled, 1);
        m_opt.filled = filled != 0;
        ui::FormRow("Wall width", "Extra cells of brush around each painted cell (the image is rebuilt).");
        changed |= ImGui::SliderInt("##border", &m_opt.border, 0, 3, "%d", ImGuiSliderFlags_AlwaysClamp);
        ui::FormRow("Wall colour", "The tint of the white image: changing it costs nothing.");
        changed |= ImGui::ColorEdit4("##color", &m_opt.color.x, ImGuiColorEditFlags_AlphaBar);
        ui::FormRow("Panel zoom", "Only for the panel of its own: px per grid cell = zoom x client_h / 1600 x 2.");
        changed |= ImGui::SliderFloat("##zoom", &m_opt.zoom, 0.25f, 4.0f, "%.2fx", ImGuiSliderFlags_AlwaysClamp);
        ui::FormRow("Show", "Players, NPCs, exits and quest objects always show.");
        changed |= ImGui::Checkbox("monsters", &m_opt.monsters);
        ImGui::SameLine();
        changed |= ImGui::Checkbox("chests", &m_opt.chests);
        ImGui::SameLine();
        changed |= ImGui::Checkbox("league", &m_opt.league);
        ui::EndForm();
        return changed;
    }

    void DrawBudget(const App& app, const P1Frame& f) const {
        ui::SectionTitle(ICON_FA_GAUGE, "What it costs");
        const P1Terrain* t = p1_terrain_current(&f);
        if (t && m_walls.Ready(f)) {
            int w = 0, h = 0;
            p1_terrain_image_size(t, m_walls.CellsPerTexel(), &w, &h);
            ui::BulletWrapped("The walls are one %d x %d texture (%s, %d cell per texel), built on a thread from the "
                              "terrain (p1_terrain_pixels) and drawn as ONE AddImageQuad: the image is flat, the relief "
                              "is baked into its pixels.", w, h, FormatBytes(uint64_t(w) * uint64_t(h) * 4u).c_str(),
                              m_walls.CellsPerTexel());
        } else {
            ui::BulletWrapped("The walls are one texture, built on a thread from the terrain (p1_terrain_pixels) and "
                              "drawn as ONE AddImageQuad: the image is flat, the relief is baked into its pixels. "
                              "None is built yet.");
        }
        ui::BulletWrapped("Every shape and label shares the font texture and one clip rectangle, so a map's hundreds of "
                          "entities stay one draw call. This plugin's last overlay frame: %s vertices, %d draw %s.",
                          FormatCount(uint64_t(app.overlayMetrics.vertices)).c_str(), app.overlayMetrics.commands,
                          app.overlayMetrics.commands == 1 ? "command" : "commands");
        ui::BulletWrapped("Per frame the overlay takes 150,000 vertices; the app draws at most 1,024 commands and 6 "
                          "screens of fill. A clip rectangle (the minimap, a panel) starts a new command: push it once.");
        ui::BulletWrapped("The radar moves every overlay frame: p1::Radar::PlayerOf with a PlayerMotion predicts the "
                          "player between two snapshots, as the app's radar does.");
    }

    RadarOptions m_opt;
    p1::TerrainTexture m_walls;
    p1::Radar::PlayerMotion m_motion;
    int m_lastDrawn = 0, m_previewDrawn = 0;
    uint64_t m_terrainEvents = 0, m_lastEventSerial = 0;
};

}  // namespace

std::unique_ptr<Lesson> MakeMapRadarLesson() { return std::make_unique<MapRadarLesson>(); }

}  // namespace ex
