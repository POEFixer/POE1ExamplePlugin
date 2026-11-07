// Lesson "mapui": the game's large map and minimap rectangles (gate: radar), drawn to scale.
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
    .what = "large_map and mini_map (P1MapElement): valid, visible, position and size in the game's UI base "
            "units, scale, shift, default shift and zoom.",
    .when = "Drawing on or next to the game's maps: convert to client pixels, then draw in the overlay.",
    .limits = "Gated: read only while the app's radar runs (P1_SNAP_MAP_VALID). UI base units are 2560 x 1600 "
              "fitted to the height: client pixels = base x client_h / 1600.",
    .snippet = "mapui",
};

// [snippet:mapui]
// A map element's rectangle in game CLIENT pixels (the overlay's coordinates).
// UI base units are 2560 x 1600 fitted to the window height, so both axes
// scale by client_h / 1600.
ImVec4 ToClientRect(const P1MapElement& m, float clientH) {
    const float k = clientH / 1600.0f;
    return ImVec4(m.pos_base_x * k, m.pos_base_y * k, (m.pos_base_x + m.size_base_x) * k,
                  (m.pos_base_y + m.size_base_y) * k);   // x0, y0, x1, y1
}
// [/snippet]

class MapUiLesson final : public Lesson {
public:
    MapUiLesson() : Lesson(kInfo) {}

    ui::Status GetStatus(const App&, const P1Frame& f) const override {
        const P1Snapshot& s = *f.snapshot;
        if (auto gap = GameDataGap(s, Need::InGame)) return *gap;
        if (!(s.flags & P1_SNAP_MAP_VALID))
            return Reason(ui::Tone::Info, "The map rectangles are read only while the app's radar runs: "
                                          "P1_SNAP_MAP_VALID is off. Turn the radar on to see them.");
        return Live("large map %s, minimap %s.", s.large_map.visible ? "visible" : "hidden",
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
        row("pos (base)", pair(lg.pos_base_x, lg.pos_base_y, "%.0f, %.0f"), pair(mn.pos_base_x, mn.pos_base_y, "%.0f, %.0f"));
        row("size (base)", pair(lg.size_base_x, lg.size_base_y, "%.0f x %.0f"),
            pair(mn.size_base_x, mn.size_base_y, "%.0f x %.0f"));
        row("scale_rel", Fmt("%.3f", double(lg.scale_rel)), Fmt("%.3f", double(mn.scale_rel)));
        row("shift", pair(lg.shift_x, lg.shift_y, "%.1f, %.1f"), pair(mn.shift_x, mn.shift_y, "%.1f, %.1f"));
        row("default shift", pair(lg.default_shift_x, lg.default_shift_y, "%.1f, %.1f"),
            pair(mn.default_shift_x, mn.default_shift_y, "%.1f, %.1f"));
        row("zoom", Fmt("%.3f", double(lg.zoom)), Fmt("%.3f", double(mn.zoom)));
        ImGui::EndTable();
    }

    // The game window scaled into the panel, with both rectangles where the game draws them.
    static void DrawSketch(const P1Snapshot& s, const P1Frame& f) {
        ui::SectionTitle(ICON_FA_VECTOR_SQUARE, "To scale");
        const float cw = f.viewport.client_w > 0 ? float(f.viewport.client_w) : 1920.0f;
        const float ch = f.viewport.client_h > 0 ? float(f.viewport.client_h) : 1080.0f;
        const float w = ImGui::GetContentRegionAvail().x;
        const float k = (std::min)(w / cw, ui::Em(14.0f) / ch);   // pixels of the sketch per client pixel
        const ImVec2 o = ImGui::GetCursorScreenPos();
        ImGui::Dummy(ImVec2(cw * k, ch * k));
        // What ToClientRect gives, beside the sketch when there is room, else under it.
        if (w - cw * k >= ui::Em(15.0f)) {
            ImGui::SameLine(0.0f, ui::Em(1.0f));
            ImGui::BeginGroup();
            DrawClientRects(s, ch);
            ImGui::EndGroup();
        } else {
            DrawClientRects(s, ch);
        }
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(o, ImVec2(o.x + cw * k, o.y + ch * k), ImGui::GetColorU32(ImGuiCol_FrameBg), 3.0f);
        dl->AddRect(o, ImVec2(o.x + cw * k, o.y + ch * k), ImGui::GetColorU32(ImGuiCol_Border), 3.0f);
        auto draw = [&](const P1MapElement& m, ui::Tone tone, const char* name) {
            if (!m.valid) return;
            const ImVec4 r = ToClientRect(m, ch);
            const ImVec2 a(o.x + r.x * k, o.y + r.y * k), b(o.x + r.z * k, o.y + r.w * k);
            dl->AddRectFilled(a, b, ui::ToneU32(tone, m.visible ? 0.30f : 0.10f));
            dl->AddRect(a, b, ui::ToneU32(tone, m.visible ? 1.0f : 0.4f), 0.0f, 0, 1.5f);
            dl->AddText(ImVec2(std::floor(a.x + ui::Em(0.3f)), std::floor(a.y + ui::Em(0.15f))), ui::ToneU32(tone), name);
        };
        draw(s.large_map, ui::Tone::Info, "large map");
        draw(s.mini_map, ui::Tone::Ok, "minimap");
        ui::TextDim("The frame is the game's client area (%.0f x %.0f). A dim box: valid, but hidden.",
                            double(cw), double(ch));
    }

    static void DrawClientRects(const P1Snapshot& s, float clientH) {
        ui::TextDim("ToClientRect, in client pixels:");
        if (!ui::BeginKv("client", { "large map", "minimap" })) return;
        auto row = [&](const char* name, const P1MapElement& m, ui::Tone tone) {
            if (!m.valid) {
                ui::KvText(name, "not valid");
                return;
            }
            const ImVec4 r = ToClientRect(m, clientH);
            ui::KvTone(name, tone, "%.0f, %.0f to %.0f, %.0f\n%.0f x %.0f px", double(r.x), double(r.y), double(r.z),
                       double(r.w), double(r.z - r.x), double(r.w - r.y));
        };
        row("large map", s.large_map, ui::Tone::Info);
        row("minimap", s.mini_map, ui::Tone::Ok);
        ui::EndKv();
    }
};

}  // namespace

std::unique_ptr<Lesson> MakeMapUiLesson() { return std::make_unique<MapUiLesson>(); }

}  // namespace ex
