// Lesson "area": the current area, and the last 10 areas visited (from P1_EV_AREA_CHANGED).
#include "lessons/Common.h"

namespace ex {

namespace {

constexpr LessonInfo kInfo{
    .id = "area",
    .group = Group::GameData,
    .icon = ICON_FA_MAP_LOCATION_DOT,
    .title = "Area",
    .summary = "Where the player is: the area's name, id, level and kind, and a history built from events.",
    .what = "snapshot->area (P1Area): the localized name, the WorldArea id (\"1_1_town\"), act, level, hash, "
            "instance, town / waypoint / hideout flags, and change_counter, which rises on every real area or "
            "instance change.",
    .when = "Reset per-area state when P1_EV_AREA_CHANGED arrives (a = the new change_counter), then read the "
            "new area in the OnTick that follows: OnEvent has no frame.",
    .limits = "Filled while attached. The name is in the game's language, cut at 1024 bytes on a character "
              "boundary; compare areas by id or hash, never by name.",
    .snippet = "area",
};

class AreaLesson final : public Lesson {
public:
    AreaLesson() : Lesson(kInfo) {}

    ui::Status GetStatus(const App& app, const P1Frame& f) const override {
        const P1Snapshot& s = *f.snapshot;
        if (auto gap = GameDataGap(s, Need::Attached)) return *gap;
        const char* name = p1::Str(s, s.area.name);
        if (!name[0]) return Reason(ui::Tone::Info, "Attached, but no area yet (game state: %s).", StateName(s.game_state));
        return Live("%s, level %d; %zu area%s recorded since the plugin started.", name, s.area.level,
                    app.areas.size(), app.areas.size() == 1 ? "" : "s");
    }

    void Event(App& app, const P1Event& e) override {
        if (e.type == P1_EV_AREA_CHANGED) app.areaPending = true;   // read it in the next OnTick
    }

    // [snippet:area]
    // OnEvent noted the change (it has no snapshot); this OnTick reads the new
    // area. Strings are COPIED: the snapshot is gone when the callback returns.
    void Tick(App& app, const P1Frame& f) override {
        const P1Snapshot& s = *f.snapshot;
        if (!(s.flags & P1_SNAP_ATTACHED)) return;
        const bool first = !m_seeded && s.area.change_counter != 0;   // where the plugin started
        if (!app.areaPending && !first) return;
        app.areaPending = false;
        m_seeded = true;   // once: clearing the history must not bring the current area back
        AreaVisit v;
        v.name = p1::Str(s, s.area.name);
        v.id = p1::Str(s, s.area.id);
        v.level = s.area.level;
        v.counter = s.area.change_counter;
        v.enteredUs = f.now_us;
        app.areas.push_back(std::move(v));
        while (app.areas.size() > kAreaHistorySize) app.areas.pop_front();
    }
    // [/snippet]

    void Draw(App& app, const P1Frame& f) override {
        const P1Snapshot& s = *f.snapshot;
        DrawCurrent(s);
        DrawHistory(app, f);
    }

private:
    static void DrawCurrent(const P1Snapshot& s) {
        ui::SectionTitle(ICON_FA_MAP_LOCATION_DOT, "The current area");
        if (!(s.flags & P1_SNAP_ATTACHED)) {
            ui::TextDim("Shown once the app is attached to the game.");
            return;
        }
        const P1Area& a = s.area;
        if (ui::BeginKv("area", { "name", "id", "act / level", "hash", "instance_id", "world_area_id",
                                  "change_counter" })) {
            ui::KvText("name", "%s", p1::Str(s, a.name));
            ui::KvText("id", "%s", p1::Str(s, a.id));
            ui::KvText("act / level", "act %d, level %d", a.act, a.level);
            ui::KvText("hash", "0x%08X", a.hash);
            ui::KvText("instance_id", "%u", a.instance_id);
            ui::KvText("world_area_id", "%d", a.world_area_id);
            ui::KvText("change_counter", "%u", a.change_counter);
            ui::EndKv();
        }
        // A tick when the flag is set, a dash when it is not: the same look as the Snapshot lesson's flags.
        ui::Flow flow;
        const struct { bool on; const char* name; const char* field; } chips[] = {
            { a.is_town != 0, "town", "is_town" },
            { a.has_waypoint != 0, "waypoint", "has_waypoint" },
            { a.is_hideout != 0, "hideout", "is_hideout" },
        };
        for (const auto& c : chips) {
            const std::string text = Fmt("%s  %s", c.on ? ICON_FA_CHECK : ICON_FA_MINUS, c.name);
            const std::string tip = Fmt("%s = %d", c.field, c.on ? 1 : 0);
            flow.Next(ui::ChipWidth(text.c_str()));
            ui::Chip(text.c_str(), c.on ? ui::Tone::Ok : ui::Tone::Neutral, tip.c_str());
        }
    }

    static void DrawHistory(App& app, const P1Frame& f) {
        ui::SectionTitle(ICON_FA_CLOCK_ROTATE_LEFT, "The last 10 areas, newest first");
        if (app.areas.empty()) {
            ui::TextDim("Nothing recorded yet: change area in the game.");
            return;
        }
        const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                                      ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoSavedSettings;
        if (ImGui::BeginTable("history", 4, flags)) {
            ImGui::TableSetupColumn("Area", ImGuiTableColumnFlags_WidthStretch, 2.2f);
            ImGui::TableSetupColumn("Level", ImGuiTableColumnFlags_WidthStretch, 0.6f);
            ImGui::TableSetupColumn("Entered", ImGuiTableColumnFlags_WidthStretch, 1.0f);
            ImGui::TableSetupColumn("Stayed", ImGuiTableColumnFlags_WidthStretch, 1.0f);
            ImGui::TableHeadersRow();
            for (size_t i = app.areas.size(); i-- > 0;) {
                const AreaVisit& v = app.areas[i];
                const int64_t left = i + 1 < app.areas.size() ? app.areas[i + 1].enteredUs : f.now_us;
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ui::TextWrap(v.name.empty() ? "(no name)" : v.name.c_str());
                ImGui::SetItemTooltip("id %s, change counter %u", v.id.c_str(), v.counter);
                ImGui::TableNextColumn();
                ImGui::Text("%d", v.level);
                ImGui::TableNextColumn();
                ImGui::TextUnformatted((FormatSpanUs(f.now_us - v.enteredUs) + " ago").c_str());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(FormatSpanUs(left - v.enteredUs).c_str());
            }
            ImGui::EndTable();
        }
        if (ImGui::Button(ICON_FA_TRASH "  Clear the history")) app.areas.clear();
    }

    bool m_seeded = false;   // the area the plugin started in is recorded
};

}  // namespace

std::unique_ptr<Lesson> MakeAreaLesson() { return std::make_unique<AreaLesson>(); }

}  // namespace ex
