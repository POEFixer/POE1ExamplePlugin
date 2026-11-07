// Lesson "buffs": the player's buffs with timers, charges and flask slots (gate: AutoPot).
#include <algorithm>
#include <cmath>

#include "lessons/Common.h"

namespace ex {

namespace {

constexpr LessonInfo kInfo{
    .id = "buffs",
    .group = Group::GameData,
    .icon = ICON_FA_FLASK,
    .title = "Buffs",
    .summary = "The buffs and debuffs on the player, with time left, charges and the flask that gave them.",
    .what = "p1::PlayerBuffs(s) walks the player's range of the buff array (P1Buff): the BuffDefinitions id, "
            "timer and max_time in seconds (+inf on an endless buff), charges, flask_slot and source_id.",
    .when = "Flask and buff overlays, uptime trackers, \"is this aura on\" checks.",
    .limits = "Gated: buffs are read only while the app's AutoPot runs (P1_SNAP_BUFFS_VALID). A plugin can "
              "never turn that read on; test the flag and say why the list is empty. At most 256 buffs.",
    .snippet = "buffs",
};

struct BuffView {
    std::string name;
    float left = 0.0f, total = 0.0f;   // seconds; total <= 0 or infinite = endless
    bool endless = false;
    bool fromFlask = false;            // flaskSlot means something only then
    unsigned charges = 0, flaskSlot = 0, source = 0;
};

// [snippet:buffs]
// One buff, copied out of the snapshot. timer / max_time are +inf on an endless
// buff: test with std::isfinite before doing arithmetic with them.
BuffView ToView(const P1Snapshot& s, const P1Buff& b) {
    BuffView v;
    v.name = p1::Str(s, b.name);
    v.endless = !std::isfinite(b.timer) || !std::isfinite(b.max_time) || b.max_time <= 0.0f;
    v.left = v.endless ? 0.0f : std::fmax(0.0f, b.timer);
    v.total = v.endless ? 0.0f : b.max_time;
    v.charges = b.charges;
    v.flaskSlot = b.flask_slot;
    v.source = b.source_id;
    // A flask's own buff has the player as its source; on any other buff flask_slot is just 0.
    v.fromFlask = s.player.id != 0 && b.source_id == s.player.id;
    return v;
}
// [/snippet]

class BuffsLesson final : public Lesson {
public:
    BuffsLesson() : Lesson(kInfo) {}

    ui::Status GetStatus(const App&, const P1Frame& f) const override {
        const P1Snapshot& s = *f.snapshot;
        if (auto gap = GameDataGap(s, Need::InGame)) return *gap;
        if (!(s.flags & P1_SNAP_BUFFS_VALID))
            return Reason(ui::Tone::Info, "Buffs are read only while the app's AutoPot runs: P1_SNAP_BUFFS_VALID "
                                          "is off. Turn AutoPot on in the app to see them here.");
        const uint32_t n = p1::PlayerBuffs(s).size();
        return n ? Live("%u buffs on the player.", n) : Live("no buffs on the player right now.");
    }

    void Draw(App&, const P1Frame& f) override {
        const P1Snapshot& s = *f.snapshot;
        ui::SectionTitle(ICON_FA_FLASK, "On the player now");
        const p1::BuffRange buffs = p1::PlayerBuffs(s);   // empty unless P1_SNAP_BUFFS_VALID
        if (buffs.empty()) {
            ui::TextDim("%s", (s.flags & P1_SNAP_BUFFS_VALID) ? "No buffs right now."
                                                              : "Nothing to show while the gate is closed.");
            DrawNotes();
            return;
        }
        // PadOuterX: the right-aligned numbers keep the same gap from the table's edge as from each other.
        const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_PadOuterX |
                                      ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoSavedSettings;
        if (ImGui::BeginTable("buffs", 4, flags)) {
            // The two number columns are as wide as their header or their widest cell, measured.
            auto fit = [](const char* header, const char* widest) {
                return (std::max)(ImGui::CalcTextSize(header).x, ImGui::CalcTextSize(widest).x);
            };
            ImGui::TableSetupColumn("Buff", ImGuiTableColumnFlags_WidthStretch, 1.0f);
            ImGui::TableSetupColumn("Time left", ImGuiTableColumnFlags_WidthStretch, 1.0f);
            ImGui::TableSetupColumn("Charges", ImGuiTableColumnFlags_WidthFixed, fit("Charges", "999"));
            ImGui::TableSetupColumn("Flask slot", ImGuiTableColumnFlags_WidthFixed, fit("Flask slot", ICON_FA_FLASK " 9"));
            ui::TableHeadersRowAligned({ false, false, true, true });
            const ImVec4 dim = ImGui::GetStyle().Colors[ImGuiCol_TextDisabled];
            for (const P1Buff& b : buffs) {
                const BuffView v = ToView(s, b);
                ImGui::TableNextRow();
                // Every cell lines its text up with the meter's text (a frame-high row).
                ImGui::TableNextColumn();
                ImGui::AlignTextToFramePadding();
                ui::TextWrap(v.name.empty() ? "(no id)" : v.name.c_str());
                ImGui::SetItemTooltip("source_id %u", v.source);
                ImGui::TableNextColumn();
                if (v.endless) {
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextColored(ui::ToneColor(ui::Tone::Accent), ICON_FA_INFINITY "  endless");
                } else {
                    // A dark fill: the theme's text reads on it without an outline.
                    ui::Meter(v.left / v.total, ui::ToneU32(ui::Tone::Info, 0.40f),
                              Fmt("%.1f s left", double(v.left)).c_str(), Fmt("of %.1f s", double(v.total)).c_str());
                }
                ImGui::TableNextColumn();
                ImGui::AlignTextToFramePadding();
                if (v.charges) {
                    ui::TextRight("%u", v.charges);
                } else {
                    ImGui::PushStyleColor(ImGuiCol_Text, dim);
                    ui::TextRight("-");
                    ImGui::PopStyleColor();
                }
                ImGui::TableNextColumn();
                ImGui::AlignTextToFramePadding();
                if (v.fromFlask) {
                    ui::TextRight(ICON_FA_FLASK " %u", v.flaskSlot);
                } else {
                    ImGui::PushStyleColor(ImGuiCol_Text, dim);
                    ui::TextRight("-");
                    ImGui::PopStyleColor();
                }
            }
            ImGui::EndTable();
        }
        DrawNotes();
    }

private:
    static void DrawNotes() {
        ui::Note("Ids are BuffDefinitions ids, the same in every game language. flask_slot is the 0-based belt "
                 "slot of the flask that applied the buff, and means something only on a flask's own buff "
                 "(its source_id is the player); on other buffs it is just 0, shown here as \"-\".");
    }
};

}  // namespace

std::unique_ptr<Lesson> MakeBuffsLesson() { return std::make_unique<BuffsLesson>(); }

}  // namespace ex
