// Lesson "player": the local player's vitals, reservation, position and actions.
#include <algorithm>
#include <cmath>

#include "lessons/Common.h"

namespace ex {

namespace {

constexpr LessonInfo kInfo{
    .id = "player",
    .group = Group::GameData,
    .icon = ICON_FA_USER,
    .title = "Player",
    .summary = "The local player: life, energy shield, mana and their reservation, position, current action.",
    .what = "snapshot->player (P1Player): vitals and their maximums, the four reservation fields, regeneration, "
            "grid and world position, rotation, action and animation ids, and its range in the buff array.",
    .when = "Anything about the user's own character. Its vitals are the only authoritative ones in the "
            "snapshot: a monster the game has not synced yet can read full or zero.",
    .limits = "Filled while attached and in game; address 0 = no player this tick. The reservation fraction is "
              "in 1/10000 of the maximum (the app reads it the same way, unverified on the current client).",
    .snippet = "player",
};

struct Vital {
    int current = 0, max = 0, reserved = 0;
    float Fraction() const { return max > 0 ? float(current) / float(max) : 0.0f; }
    float ReservedFraction() const { return max > 0 ? float(reserved) / float(max) : 0.0f; }
};

// [snippet:player]
// Life with its reservation. A fraction is in 1/10000 of the maximum; values
// out of range read as "none" (a bad read after a game patch must not turn
// into a negative pool).
Vital MakeVital(int current, int max, int reservedFlat, int reservedFraction) {
    Vital v;
    v.current = std::max(0, current);
    v.max = std::max(0, max);
    const int64_t fraction = (reservedFraction >= 0 && reservedFraction <= 10000) ? reservedFraction : 0;
    const int64_t flat = (reservedFlat >= 0 && reservedFlat <= v.max) ? reservedFlat : 0;
    const int64_t reserved = (fraction * v.max + 9999) / 10000 + flat;   // the fraction rounds up
    v.reserved = static_cast<int>(std::min<int64_t>(reserved, v.max));
    return v;
}
// [/snippet]

class PlayerLesson final : public Lesson {
public:
    PlayerLesson() : Lesson(kInfo) {}

    ui::Status GetStatus(const App&, const P1Frame& f) const override {
        const P1Snapshot& s = *f.snapshot;
        if (auto gap = GameDataGap(s, Need::InGame)) return *gap;
        if (s.player.address == 0)
            return Reason(ui::Tone::Info, "No local player this tick: player.address is 0 (a loading screen?).");
        const char* name = p1::Str(s, s.player.name);
        return Live("%s, life %d / %d, at grid %d, %d.", name[0] ? name : "(no name)", s.player.hp, s.player.hp_max,
                    s.player.grid_x, s.player.grid_y);
    }

    void Draw(App&, const P1Frame& f) override {
        const P1Snapshot& s = *f.snapshot;
        if (!(s.flags & P1_SNAP_ATTACHED) || s.player.address == 0) {
            ui::TextDim("The demo shows the player's bars, position and action here once a character is in game.");
            return;
        }
        const P1Player& p = s.player;
        DrawIdentity(s, p);
        DrawVitals(p);
        DrawPosition(p);
    }

private:
    static void DrawIdentity(const P1Snapshot& s, const P1Player& p) {
        ui::SectionTitle(ICON_FA_ID_CARD, "Who");
        if (ui::BeginKv("who", { "name", "id", "address" })) {
            ui::KvText("name", "%s", p1::Str(s, p.name));
            ui::KvText("id", "%u", p.id);
            ui::KvText("address", "0x%016llX (the entity in the game)", static_cast<unsigned long long>(p.address));
            ui::EndKv();
        }
    }

    // The name at the left, the numbers where the free part ends, the reservation inside its hatch.
    static void Bar(const char* label, const Vital& v, ImU32 color) {
        const std::string value = Fmt("%d / %d", v.current, v.max);
        const std::string note = v.reserved > 0 ? Fmt("%d reserved", v.reserved) : std::string();
        ui::Meter(v.Fraction(), color, label, value.c_str(), v.ReservedFraction(), note.c_str());
    }

    static void DrawVitals(const P1Player& p) {
        ui::SectionTitle(ICON_FA_HEART_PULSE, "Vitals");
        Bar("Life", MakeVital(p.hp, p.hp_max, p.hp_reserved_flat, p.hp_reserved_fraction), IM_COL32(196, 52, 52, 230));
        if (p.es_max > 0) Bar("Energy shield", MakeVital(p.es, p.es_max, 0, 0), IM_COL32(48, 110, 160, 230));
        Bar("Mana", MakeVital(p.mana, p.mana_max, p.mana_reserved_flat, p.mana_reserved_fraction), IM_COL32(52, 92, 210, 230));
        if (ui::BeginKv("regen", { "life regeneration", "mana regeneration", "reservation (raw)" })) {
            ui::KvText("life regeneration", "%.1f / s", double(p.hp_regen));
            ui::KvText("mana regeneration", "%.1f / s", double(p.mana_regen));
            ui::KvText("reservation (raw)", "life flat %d, fraction %d; mana flat %d, fraction %d", p.hp_reserved_flat,
                       p.hp_reserved_fraction, p.mana_reserved_flat, p.mana_reserved_fraction);
            ui::EndKv();
        }
        ui::Note("The hatched end of a bar is reserved: it cannot be filled. Energy shield appears only when "
                 "the character has any.");
    }

    static void DrawPosition(const P1Player& p) {
        ui::SectionTitle(ICON_FA_COMPASS, "Where, and doing what");
        // A small compass: the facing direction from `rotation` (radians, world space).
        const float size = ui::Em(4.2f);
        const ImVec2 o = ImGui::GetCursorScreenPos();
        ImGui::Dummy(ImVec2(size, size));
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 c(o.x + size * 0.5f, o.y + size * 0.5f);
        dl->AddCircleFilled(c, size * 0.48f, ImGui::GetColorU32(ImGuiCol_FrameBg));
        dl->AddCircle(c, size * 0.48f, ImGui::GetColorU32(ImGuiCol_Border));
        const float r = size * 0.40f;
        const ImVec2 tip(c.x + std::cos(p.rotation) * r, c.y - std::sin(p.rotation) * r);
        dl->AddLine(c, tip, ui::ToneU32(ui::Tone::Accent), 2.5f);
        dl->AddCircleFilled(tip, ui::Em(0.2f), ui::ToneU32(ui::Tone::Accent));
        ImGui::SameLine();
        ImGui::BeginGroup();
        if (ui::BeginKv("pos", { "grid", "world", "rotation", "action_id", "animation_id", "buffs" })) {
            ui::KvText("grid", "%d, %d", p.grid_x, p.grid_y);
            ui::KvText("world", "%.1f, %.1f", double(p.world_x), double(p.world_y));
            ui::KvText("rotation", "%.3f rad (%.0f deg)", double(p.rotation), double(p.rotation) * 57.29578);
            ui::KvText("action_id", "%d (0x%04X)", int(p.action_id), unsigned(uint16_t(p.action_id)));
            ui::KvText("animation_id", "%d", p.animation_id);
            ui::KvText("buffs", "%u from index %u (filled with P1_SNAP_BUFFS_VALID)", p.buff_count, p.buff_first);
            ui::EndKv();
        }
        ImGui::EndGroup();
    }
};

}  // namespace

std::unique_ptr<Lesson> MakePlayerLesson() { return std::make_unique<PlayerLesson>(); }

}  // namespace ex
