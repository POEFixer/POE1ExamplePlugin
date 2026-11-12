// Lesson "mapui": where the game draws its large map and minimap (gate: radar), drawn to scale.
#include <algorithm>
#include <cmath>

#include "lessons/Common.h"

namespace ex {

namespace {

constexpr LessonInfo kInfo{
    .id = "mapui",
    .group = Group::GameData,
    .icon = ICON_FA_MAP,
    .title = "Map UI",
    .summary = "Where the game draws its large map and minimap, to place your own drawing on top of them.",
    .what = "large_map and mini_map (P1MapElement). The minimap is a rectangle: pos is its top-left, size its "
            "size, in the game's UI base units. The large map is not: its pos is the map's CENTRE and its size "
            "reads 0 x 0. shift and default_shift are client pixels; zoom is the sub-map zoom (both maps share it).",
    .when = "Drawing on or next to the game's maps: the minimap's rectangle to clip to, the large map's centre and "
            "scale to place things. For a radar use p1_radar_large_map / p1_radar_mini_map (the Build a radar lesson).",
    .limits = "Gated: read while the app's radar, or a plugin with the \"radar\" permission (this one), runs "
              "(P1_SNAP_MAP_VALID). UI base units are 2560 x 1600 fitted to the height: client pixels = base x "
              "client_h / 1600. The game hides the minimap while the large map is open.",
    .snippet = "mapui",
};

// [snippet:mapui]
// The minimap is a real rectangle: pos is its top-left and size its size, in
// UI base units (2560 x 1600 fitted to the window height: both axes scale by
// client_h / 1600). The large map is NOT: its pos is the map's CENTRE and its
// size reads 0 x 0. Its transform (p1_radar.h) adds the shift, the zoom and the
// user's calibration: the player is drawn at (t.cx, t.cy), and t.scale is the
// pixels per grid cell before the camera's cos / sin.
ImVec4 MiniMapClientRect(const P1MapElement& m, float clientH) {
    const float k = clientH / 1600.0f;
    return ImVec4(m.pos_base_x * k, m.pos_base_y * k, (m.pos_base_x + m.size_base_x) * k,
                  (m.pos_base_y + m.size_base_y) * k);   // x0, y0, x1, y1
}
P1MapTransform LargeMapAt(const P1Snapshot& s, float clientH) {
    return p1_radar_large_map(&s, clientH);   // .valid is 0 while the map is closed
}
// [/snippet]

class MapUiLesson final : public Lesson {
public:
    MapUiLesson() : Lesson(kInfo) {}

    ui::Status GetStatus(const App&, const P1Frame& f) const override {
        const P1Snapshot& s = *f.snapshot;
        if (auto gap = GameDataGap(s, Need::InGame)) return *gap;
        if (!(s.flags & P1_SNAP_MAP_VALID))
            return Reason(ui::Tone::Info, "The map elements are read only while the radar reads run: P1_SNAP_MAP_VALID "
                                          "is off. They run while the app's radar or a plugin with the \"radar\" "
                                          "permission runs.");
        return Live("large map %s, minimap %s.", s.large_map.visible ? "open" : "closed",
                    s.mini_map.visible ? "visible" : "hidden");
    }

    void Draw(App&, const P1Frame& f) override {
        const P1Snapshot& s = *f.snapshot;
        if (!(s.flags & P1_SNAP_MAP_VALID)) {
            ui::TextDim("A table of both elements and a to-scale sketch of the game window appear here while "
                        "the gate is open.");
            return;
        }
        DrawTable(s);
        DrawSketch(s, f);
    }

private:
    static void DrawTable(const P1Snapshot& s) {
        ui::SectionTitle(ICON_FA_TABLE_CELLS, "Both elements");
        const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                                      ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoSavedSettings;
        if (!ImGui::BeginTable("maps", 3, flags)) return;
        ImGui::TableSetupColumn("Field", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("Large map", ImGuiTableColumnFlags_WidthStretch, 1.2f);
        ImGui::TableSetupColumn("Minimap", ImGuiTableColumnFlags_WidthStretch, 1.2f);
        ImGui::TableHeadersRow();
        auto row = [](const char* field, const std::string& a, const std::string& b) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", field);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(a.c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(b.c_str());
        };
        const P1MapElement& lg = s.large_map;
        const P1MapElement& mn = s.mini_map;
        auto pair = [](float x, float y, const char* fmt) { return Fmt(fmt, double(x), double(y)); };
        row("valid / visible", Fmt("%u / %u", unsigned(lg.valid), unsigned(lg.visible)),
            Fmt("%u / %u", unsigned(mn.valid), unsigned(mn.visible)));
        row("pos (base)", pair(lg.pos_base_x, lg.pos_base_y, "%.0f, %.0f: the centre"),
            pair(mn.pos_base_x, mn.pos_base_y, "%.0f, %.0f: top-left"));
        row("size (base)", pair(lg.size_base_x, lg.size_base_y, "%.0f x %.0f (not a size)"),
            pair(mn.size_base_x, mn.size_base_y, "%.0f x %.0f"));
        row("scale_rel", Fmt("%.3f", double(lg.scale_rel)), Fmt("%.3f", double(mn.scale_rel)));
        row("shift (px)", pair(lg.shift_x, lg.shift_y, "%.1f, %.1f"), pair(mn.shift_x, mn.shift_y, "%.1f, %.1f"));
        row("default shift (px)", pair(lg.default_shift_x, lg.default_shift_y, "%.1f, %.1f"),
            pair(mn.default_shift_x, mn.default_shift_y, "%.1f, %.1f"));
        row("zoom", Fmt("%.3f", double(lg.zoom)), Fmt("%.3f", double(mn.zoom)));
        ImGui::EndTable();
    }

    // The game window scaled into the panel: the minimap's rectangle, and the large map's centre with
    // a 150 x 150 cell square around the player projected through its transform (scale and orientation).
    static void DrawSketch(const P1Snapshot& s, const P1Frame& f) {
        ui::SectionTitle(ICON_FA_VECTOR_SQUARE, "To scale");
        const float cw = f.viewport.client_w > 0 ? float(f.viewport.client_w) : 1920.0f;
        const float ch = f.viewport.client_h > 0 ? float(f.viewport.client_h) : 1080.0f;
        const float w = ImGui::GetContentRegionAvail().x;
        const float k = (std::min)(w / cw, ui::Em(14.0f) / ch);   // pixels of the sketch per client pixel
        const ImVec2 o = ImGui::GetCursorScreenPos();
        ImGui::Dummy(ImVec2(cw * k, ch * k));
        // What the snippet gives, beside the sketch when there is room, else under it.
        if (w - cw * k >= ui::Em(15.0f)) {
            ImGui::SameLine(0.0f, ui::Em(1.0f));
            ImGui::BeginGroup();
            DrawClientValues(s, ch);
            ImGui::EndGroup();
        } else {
            DrawClientValues(s, ch);
        }
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(o, ImVec2(o.x + cw * k, o.y + ch * k), ImGui::GetColorU32(ImGuiCol_FrameBg), 3.0f);
        dl->AddRect(o, ImVec2(o.x + cw * k, o.y + ch * k), ImGui::GetColorU32(ImGuiCol_Border), 3.0f);
        auto at = [&](float x, float y) { return ImVec2(o.x + x * k, o.y + y * k); };
        // The minimap: a rectangle.
        if (s.mini_map.valid) {
            const ImVec4 r = MiniMapClientRect(s.mini_map, ch);
            const bool on = s.mini_map.visible != 0;
            dl->AddRectFilled(at(r.x, r.y), at(r.z, r.w), ui::ToneU32(ui::Tone::Ok, on ? 0.30f : 0.10f));
            dl->AddRect(at(r.x, r.y), at(r.z, r.w), ui::ToneU32(ui::Tone::Ok, on ? 1.0f : 0.4f), 0.0f, 0, 1.5f);
            dl->AddText(ImVec2(std::floor(o.x + r.x * k + ui::Em(0.3f)), std::floor(o.y + r.w * k + ui::Em(0.15f))),
                        ui::ToneU32(ui::Tone::Ok, on ? 1.0f : 0.5f), "minimap");
        }
        // The large map: a centre, and how big 50 cells around the player are on it.
        if (s.large_map.valid) {
            const P1MapTransform t = LargeMapAt(s, ch);
            const bool on = t.valid != 0;
            const float pxPerBase = ch / 1600.0f;
            const ImVec2 c = on ? at(t.cx, t.cy) : at(s.large_map.pos_base_x * pxPerBase, s.large_map.pos_base_y * pxPerBase);
            const ImU32 col = ui::ToneU32(ui::Tone::Info, on ? 1.0f : 0.4f);
            if (on) {
                ImVec2 q[4];
                const float g[4][2] = { { -75, -75 }, { 75, -75 }, { 75, 75 }, { -75, 75 } };
                for (int i = 0; i < 4; ++i) {
                    float x = 0.0f, y = 0.0f;
                    p1_grid_to_map(&t, 0.0f, 0.0f, g[i][0], g[i][1], 0.0f, &x, &y);
                    q[i] = at(x, y);
                }
                dl->AddQuadFilled(q[0], q[1], q[2], q[3], ui::ToneU32(ui::Tone::Info, 0.25f));
                dl->AddQuad(q[0], q[1], q[2], q[3], col, 1.5f);
            }
            const float arm = ui::Em(0.5f);
            dl->AddLine(ImVec2(c.x - arm, c.y), ImVec2(c.x + arm, c.y), col, 1.5f);
            dl->AddLine(ImVec2(c.x, c.y - arm), ImVec2(c.x, c.y + arm), col, 1.5f);
            dl->AddText(ImVec2(std::floor(c.x + arm + ui::Em(0.2f)), std::floor(c.y - ui::Em(1.2f))), col,
                        on ? "large map: the centre" : "large map (closed)");
        }
        ui::TextDim("The frame is the game's client area (%.0f x %.0f). The cross is where the large map draws the "
                    "player; the diamond is 150 x 150 grid cells around it. A dim shape: valid, but hidden.",
                    double(cw), double(ch));
    }

    static void DrawClientValues(const P1Snapshot& s, float clientH) {
        ui::TextDim("The snippet's values, in client pixels:");
        if (!ui::BeginKv("client", { "minimap", "large map centre", "large map scale" })) return;
        if (s.mini_map.valid) {
            const ImVec4 r = MiniMapClientRect(s.mini_map, clientH);
            ui::KvTone("minimap", ui::Tone::Ok, "%.0f, %.0f to %.0f, %.0f\n%.0f x %.0f px", double(r.x), double(r.y),
                       double(r.z), double(r.w), double(r.z - r.x), double(r.w - r.y));
        } else {
            ui::KvText("minimap", "not valid");
        }
        const P1MapTransform t = LargeMapAt(s, clientH);
        if (t.valid) {
            ui::KvTone("large map centre", ui::Tone::Info, "%.0f, %.0f", double(t.cx), double(t.cy));
            ui::KvTone("large map scale", ui::Tone::Info, "%.2f px per grid cell", double(t.scale));
        } else {
            ui::KvText("large map centre", "%s", s.large_map.valid ? "closed" : "not valid");
            ui::KvText("large map scale", "-");
        }
        ui::EndKv();
    }
};

}  // namespace

std::unique_ptr<Lesson> MakeMapUiLesson() { return std::make_unique<MapUiLesson>(); }

}  // namespace ex
