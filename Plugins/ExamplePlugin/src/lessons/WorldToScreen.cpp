// Lesson "w2s": world positions to game-client pixels with the snapshot's camera.
#include <cfloat>
#include <cmath>

#include "lessons/Common.h"

namespace ex {

namespace {

constexpr LessonInfo kInfo{
    .id = "w2s",
    .group = Group::Drawing,
    .icon = ICON_FA_CROSSHAIRS,
    .title = "World " ICON_FA_ARROW_RIGHT " screen",
    .summary = "Turn a position in the game world into the pixel where the game shows it.",
    .what = "p1::WorldToScreen(f, x, y, z, out) multiplies by the snapshot's view-projection matrix, divides "
            "by w and scales the camera's viewport to the client size. It returns false behind the camera.",
    .when = "Every overlay drawing tied to something in the world: use render_pos for entities (the model's "
            "base) and the player entity's render_pos for the ground under the player.",
    .limits = "Valid only with P1_SNAP_CAMERA_VALID. The camera was sampled with the entity positions "
              "(capture_us), so both describe the same moment. The camera viewport can differ from the client "
              "size; the helper scales for you.",
    .snippet = "w2s",
};

// [snippet:w2s]
// A 5 x 5 grid on the ground around `centre`. A straight line in the world is a
// straight line on screen, so projecting both ends of each line is enough. A
// line with an end behind the camera is skipped (WorldToScreen returns false).
void DrawGrid(ImDrawList* dl, const P1Frame& f, P1Vec3 centre, float cell, ImU32 col) {
    if (!(cell >= 1.0f)) return;                     // a typed 0 (or NaN) would divide by zero
    centre.x = std::round(centre.x / cell) * cell;   // snapped: the grid stays put in the world
    centre.y = std::round(centre.y / cell) * cell;
    const float half = cell * 2.5f;
    for (int i = 0; i <= 5; ++i) {
        const float o = -half + cell * float(i);
        ImVec2 a, b;
        if (p1::WorldToScreen(f, centre.x + o, centre.y - half, centre.z, a) &&
            p1::WorldToScreen(f, centre.x + o, centre.y + half, centre.z, b))
            dl->AddLine(a, b, col, 1.0f);
        if (p1::WorldToScreen(f, centre.x - half, centre.y + o, centre.z, a) &&
            p1::WorldToScreen(f, centre.x + half, centre.y + o, centre.z, b))
            dl->AddLine(a, b, col, 1.0f);
    }
}
// [/snippet]

class WorldToScreenLesson final : public Lesson {
public:
    WorldToScreenLesson() : Lesson(kInfo) {}

    ui::Status GetStatus(const App&, const P1Frame& f) const override {
        const P1Snapshot& s = *f.snapshot;
        if (auto gap = GameDataGap(s, Need::Camera)) return *gap;
        const P1Entity* me = FindPlayerEntity(s);
        if (!me) return Reason(ui::Tone::Info, "The camera is valid, but the player's entity is not in the snapshot.");
        ImVec2 p;
        if (!p1::WorldToScreen(f, me->render_pos, p))
            return Reason(ui::Tone::Info, "The player is behind the camera this frame: WorldToScreen returned false.");
        return Live("the player at world %.0f, %.0f is at %.0f, %.0f on screen.", double(me->render_pos.x),
                    double(me->render_pos.y), double(p.x), double(p.y));
    }

    void Draw(App& app, const P1Frame& f) override {
        const P1Snapshot& s = *f.snapshot;
        DrawCamera(s, f);
        DrawProbe(s, f);
        DrawGridControls(app);
        DrawMatrix(s);
    }

    void Overlay(App& app, const P1Frame& f) override {
        const P1Snapshot& s = *f.snapshot;
        if (!app.LayerOn(LayerId::Grid) || !(s.flags & P1_SNAP_CAMERA_VALID)) return;
        const P1Entity* me = FindPlayerEntity(s);
        if (!me) return;
        const LayerState& layer = app.Layer(LayerId::Grid);
        // The background list: the grid sits under every other layer.
        ImDrawList* dl = ImGui::GetBackgroundDrawList();
        DrawGrid(dl, f, me->render_pos, layer.size, ImGui::ColorConvertFloat4ToU32(layer.color));
        // The axis labels in the grid's hue at full alpha, outlined: the lines are faint on purpose,
        // the labels must still read.
        ImVec4 hue = layer.color;
        hue.w = 1.0f;
        const ImU32 label = ImGui::ColorConvertFloat4ToU32(hue);
        ImVec2 px, py;
        const float half = layer.size * 2.5f, gap = 4.0f * UiScale(f);
        if (p1::WorldToScreen(f, me->render_pos.x + half, me->render_pos.y, me->render_pos.z, px))
            OutlinedText(dl, ImVec2(px.x + gap, px.y - ImGui::GetFontSize() * 0.5f), label, "+x");
        if (p1::WorldToScreen(f, me->render_pos.x, me->render_pos.y + half, me->render_pos.z, py))
            OutlinedText(dl, ImVec2(py.x + gap, py.y - ImGui::GetFontSize() * 0.5f), label, "+y");
    }

private:
    static void DrawCamera(const P1Snapshot& s, const P1Frame& f) {
        ui::SectionTitle(ICON_FA_CAMERA, "Camera and window");
        if (ui::BeginKv("camera", { "camera", "camera viewport", "game client", "scale to client", "cursor" })) {
            ui::KvTone("camera", (s.flags & P1_SNAP_CAMERA_VALID) ? ui::Tone::Ok : ui::Tone::Warn, "%s",
                       (s.flags & P1_SNAP_CAMERA_VALID) ? "valid (P1_SNAP_CAMERA_VALID)" : "not valid this tick");
            ui::KvText("camera viewport", "%d x %d", s.camera.viewport_w, s.camera.viewport_h);
            ui::KvText("game client", "%d x %d at screen %d, %d", f.viewport.client_w, f.viewport.client_h,
                       f.viewport.client_x, f.viewport.client_y);
            if (s.camera.viewport_w > 0 && s.camera.viewport_h > 0)
                ui::KvText("scale to client", "%.3f x %.3f", double(f.viewport.client_w) / s.camera.viewport_w,
                           double(f.viewport.client_h) / s.camera.viewport_h);
            else
                ui::KvText("scale to client", "-");
            ui::KvText("cursor", "%d, %d (client pixels; can be outside the window)", f.viewport.cursor_x,
                       f.viewport.cursor_y);
            ui::EndKv();
        }
    }

    // Project a point next to the player: move it with the sliders and watch the pixel move.
    void DrawProbe(const P1Snapshot& s, const P1Frame& f) {
        ui::SectionTitle(ICON_FA_LOCATION_CROSSHAIRS, "Try a point");
        const P1Entity* me = FindPlayerEntity(s);
        if (!me || !(s.flags & P1_SNAP_CAMERA_VALID)) {
            ui::TextDim("Needs a valid camera and the player's entity.");
            return;
        }
        if (ui::BeginForm("probe", { "x offset", "y offset", "z offset", "world", "screen" })) {
            // Drags, not sliders: the value reads in the middle, with no grab knob on top of it.
            ui::FormRow("x offset", "World units from the player along x (about 10.9 per grid cell). Drag, or "
                                    "double-click to type.");
            ImGui::DragFloat("##dx", &m_offset[0], 2.0f, -600.0f, 600.0f, "%+.0f world units", ImGuiSliderFlags_AlwaysClamp);
            ui::FormRow("y offset", "World units from the player along y.");
            ImGui::DragFloat("##dy", &m_offset[1], 2.0f, -600.0f, 600.0f, "%+.0f world units", ImGuiSliderFlags_AlwaysClamp);
            ui::FormRow("z offset", "Height. Try it to find which way is up in this camera.");
            ImGui::DragFloat("##dz", &m_offset[2], 1.0f, -300.0f, 300.0f, "%+.0f world units", ImGuiSliderFlags_AlwaysClamp);
            const P1Vec3 w{ me->render_pos.x + m_offset[0], me->render_pos.y + m_offset[1], me->render_pos.z + m_offset[2] };
            ui::KvText("world", "%.1f, %.1f, %.1f", double(w.x), double(w.y), double(w.z));
            ImVec2 p;
            if (p1::WorldToScreen(f, w, p))
                ui::KvTone("screen", ui::Tone::Ok, "%.1f, %.1f (client pixels)", double(p.x), double(p.y));
            else
                ui::KvTone("screen", ui::Tone::Warn, "false: behind the camera");
            ui::EndForm();
        }
    }

    static void DrawGridControls(App& app) {
        ui::SectionTitle(ICON_FA_TABLE_CELLS, "The grid layer");
        LayerState& layer = app.Layer(LayerId::Grid);
        if (ui::BeginForm("grid", { "Draw the grid", "Cell size", "Colour" })) {
            ui::FormRow("Draw the grid", "The \"grid\" overlay layer: 5 x 5 cells on the ground, snapped to world "
                                         "coordinates so it stays put while the player moves.");
            bool on = layer.on;
            if (ImGui::Checkbox("##on", &on)) app.SetLayer(LayerId::Grid, on);
            ui::FormRow("Cell size", "One cell's side on the ground, in world units.");
            if (ImGui::SliderFloat("##cell", &layer.size, LayerOf(LayerId::Grid).sizeMin, LayerOf(LayerId::Grid).sizeMax,
                                   "%.0f world units", ImGuiSliderFlags_AlwaysClamp))   // no typed 0: it divides
                app.MarkDirty();
            ui::FormRow("Colour", "The lines' colour; alpha makes the grid fainter.");
            if (ImGui::ColorEdit4("##color", &layer.color.x, ImGuiColorEditFlags_AlphaBar)) app.MarkDirty();
            ui::EndForm();
        }
    }

    static void DrawMatrix(const P1Snapshot& s) {
        ui::SectionTitle(ICON_FA_TABLE, "camera.view_proj (row-vector convention)");
        ui::TextDim("v = (x, y, z, 1) x M; then x / w and y / w are in -1..1 across the viewport.");
        // Right-aligned, so the decimal points line up down each column.
        const ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchSame |
                                      ImGuiTableFlags_NoSavedSettings;
        if (ImGui::BeginTable("matrix", 4, flags)) {
            for (int r = 0; r < 4; ++r) {
                ImGui::TableNextRow();
                for (int c = 0; c < 4; ++c) {
                    ImGui::TableNextColumn();
                    ui::TextRight("%.4f", double(s.camera.view_proj[r * 4 + c]));
                }
            }
            ImGui::EndTable();
        }
    }

    float m_offset[3] = { 0.0f, 0.0f, 0.0f };
};

}  // namespace

std::unique_ptr<Lesson> MakeWorldToScreenLesson() { return std::make_unique<WorldToScreenLesson>(); }

}  // namespace ex
