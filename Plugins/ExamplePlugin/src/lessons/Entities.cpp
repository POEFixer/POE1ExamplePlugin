// Lesson "entities": the entity array, filtered, sorted, clipped; one picked entity in full.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

#include "JsonUtil.h"
#include "lessons/Common.h"

namespace ex {

namespace {

constexpr LessonInfo kInfo{
    .id = "entities",
    .group = Group::GameData,
    .icon = ICON_FA_USERS,
    .title = "Entities",
    .summary = "Every entity the game has near the player: monsters, chests, NPCs, exits, items, effects.",
    .what = "The entity array of the snapshot (P1Entity): identity, kind, rarity, reaction, components, grid "
            "and world position, vitals, and the metadata path, plus radar-gated strings.",
    .when = "Anything about the world around the player. Walk it with p1::Entities(s); identify an entity by "
            "address AND id together (the game reuses addresses).",
    .limits = "Up to the 2 MiB snapshot slot: in extreme scenes entities are left out and counted in "
              "entity_truncated. Vitals are authoritative only for the local player. targetable, "
              "minimap_icon, transition_target and item_path are filled only while the app's radar runs.",
    .snippet = "entities",
};

struct Filter {
    int kind = -1;              // -1 = every kind, else P1_KIND_*
    bool hostileOnly = false;   // IsHostile: reaction is neither friendly (1) nor none (0xFF)
    bool aliveOnly = false;     // has life and hp > 0
    int minRarity = 0;          // 0 any, 1 magic or better, 2 rare or better, 3 unique
    char text[64] = "";         // part of the metadata path, any case
};

struct Row {
    uint32_t index;   // position in the entity array of THIS snapshot
    float dist;       // grid units from the player
    float hpPct;      // -1 = no life
};

enum Column : int { kColKind, kColRarity, kColHp, kColDist, kColPath };

// [snippet:entities]
// The rows the table shows. p1::Entities walks the array by its STRIDE (a
// newer app may grow P1Entity). Only plain numbers are kept: the snapshot, and
// every pointer into it, is gone when the callback returns.
std::vector<Row> CollectRows(const P1Snapshot& s, const Filter& flt) {
    std::vector<Row> rows;
    uint32_t index = 0;
    for (const P1Entity& e : p1::Entities(s)) {
        const uint32_t i = index++;
        if (flt.kind >= 0 && e.kind != flt.kind) continue;
        if (flt.hostileOnly && !IsHostile(e)) continue;     // reaction neither 1 (friendly) nor 0xFF
        if (flt.aliveOnly && !(e.has_life && e.hp > 0)) continue;
        if (flt.minRarity > 0 && (e.rarity == 0xFF || e.rarity < flt.minRarity)) continue;
        if (flt.text[0] && !ContainsNoCase(p1::Str(s, e.path), flt.text)) continue;
        const float hp = e.hp_max > 0 ? 100.0f * float(e.hp) / float(e.hp_max) : -1.0f;
        rows.push_back({ i, GridDistance(s, e), hp });
    }
    return rows;
}
// [/snippet]

void SortRows(std::vector<Row>& rows, const P1Snapshot& s, const ImGuiTableSortSpecs* specs) {
    if (!specs || specs->SpecsCount < 1) return;
    const ImGuiTableColumnSortSpecs& spec = specs->Specs[0];
    const bool asc = spec.SortDirection != ImGuiSortDirection_Descending;
    const int column = static_cast<int>(spec.ColumnUserID);
    auto less = [&](const Row& a, const Row& b) {
        const P1Entity& ea = *p1_entity_at(&s, a.index);
        const P1Entity& eb = *p1_entity_at(&s, b.index);
        switch (column) {
            case kColKind:   return ea.kind < eb.kind;
            case kColRarity: return (ea.rarity == 0xFF ? -1 : ea.rarity) < (eb.rarity == 0xFF ? -1 : eb.rarity);
            case kColHp:     return a.hpPct < b.hpPct;
            case kColPath:   return std::strcmp(p1::Str(s, ea.path), p1::Str(s, eb.path)) < 0;
            case kColDist:
            default:         return a.dist < b.dist;
        }
    };
    std::stable_sort(rows.begin(), rows.end(), [&](const Row& a, const Row& b) { return asc ? less(a, b) : less(b, a); });
}

const P1Entity* FindSelected(const App& app, const P1Snapshot& s) {
    for (const P1Entity& e : p1::Entities(s))
        if (app.selection.Is(e)) return &e;
    return nullptr;
}

bool Passes(const P1Snapshot& s, const P1Entity& e, const Filter& flt) {
    if (flt.kind >= 0 && e.kind != flt.kind) return false;
    if (flt.minRarity > 0 && (e.rarity == 0xFF || e.rarity < flt.minRarity)) return false;
    return !flt.text[0] || ContainsNoCase(p1::Str(s, e.path), flt.text);
}

// The nearest living hostile monster the filter lets through.
const P1Entity* NearestHostile(const P1Snapshot& s, const Filter& flt) {
    const P1Entity* nearest = nullptr;
    float best = 0.0f;
    for (const P1Entity& e : p1::Entities(s)) {
        const float d = GridDistance(s, e);
        if (IsHostileAlive(e) && Passes(s, e, flt) && (!nearest || d < best)) {
            nearest = &e;
            best = d;
        }
    }
    return nearest;
}

// What the detail card shows and the selection ring circles: the picked entity, or, until the
// user picks a row, the nearest hostile monster the filter shows. Null when the picked one left
// the snapshot.
const P1Entity* ShownEntity(const App& app, const P1Snapshot& s, const Filter& flt) {
    return app.selection.Any() ? FindSelected(app, s) : NearestHostile(s, flt);
}

class EntitiesLesson final : public Lesson {
public:
    EntitiesLesson() : Lesson(kInfo) {}

    ui::Status GetStatus(const App&, const P1Frame& f) const override {
        const P1Snapshot& s = *f.snapshot;
        if (auto gap = GameDataGap(s, Need::InGame)) return *gap;
        uint32_t hostile = 0;
        for (const P1Entity& e : p1::Entities(s)) hostile += IsHostileAlive(e) ? 1u : 0u;
        return Live("%u entities, %u hostile monsters alive%s.", s.entity_count, hostile,
                    s.entity_truncated ? Fmt(", %u left out (entity_truncated)", s.entity_truncated).c_str() : "");
    }

    void Draw(App& app, const P1Frame& f) override {
        const P1Snapshot& s = *f.snapshot;
        DrawSummary(s);
        DrawFilters(app);
        DrawTable(app, s);
        DrawDetail(app, s);
    }

    void Overlay(App& app, const P1Frame& f) override { DrawSelectionRing(app, f); }

    void Load(App&, const nlohmann::json& j) override {
        m_filter.kind = js::Int(j, "kind", -1, -1, P1_KIND_UNKNOWN);
        m_filter.hostileOnly = js::Bool(j, "hostileOnly", false);
        m_filter.aliveOnly = js::Bool(j, "aliveOnly", false);
        m_filter.minRarity = js::Int(j, "minRarity", 0, 0, 3);
        const std::string text = js::String(j, "pathFilter", "", sizeof m_filter.text - 1);
        std::memcpy(m_filter.text, text.c_str(), text.size() + 1);
    }
    void Save(const App&, nlohmann::json& j) const override {
        j["kind"] = m_filter.kind;
        j["hostileOnly"] = m_filter.hostileOnly;
        j["aliveOnly"] = m_filter.aliveOnly;
        j["minRarity"] = m_filter.minRarity;
        j["pathFilter"] = m_filter.text;
    }

private:
    static void DrawSummary(const P1Snapshot& s) {
        std::array<uint32_t, P1_KIND_UNKNOWN + 1> counts{};
        for (const P1Entity& e : p1::Entities(s)) ++counts[e.kind <= P1_KIND_UNKNOWN ? e.kind : P1_KIND_UNKNOWN];
        ui::Flow flow;
        for (uint8_t k = 0; k <= P1_KIND_UNKNOWN; ++k) {
            if (!counts[k]) continue;
            const std::string text = Fmt("%s %s %u", KindIcon(k), KindName(k), counts[k]);
            flow.Next(ui::ChipWidth(text.c_str()));
            ui::Chip(text.c_str(), ui::Tone::Neutral);
        }
    }

    void DrawFilters(App& app) {
        ui::SectionTitle(ICON_FA_FILTER, "Filter");
        static constexpr const char* kRarities[] = { "Any rarity", "Magic or better", "Rare or better", "Unique" };
        bool changed = false;
        if (ui::BeginForm("filter", { "Kind", "Rarity", "Path contains", "Only" })) {
            ui::FormRow("Kind");
            if (ImGui::BeginCombo("##kind", m_filter.kind < 0 ? "Every kind" : KindName(uint8_t(m_filter.kind)))) {
                if (ImGui::Selectable("Every kind", m_filter.kind < 0)) { m_filter.kind = -1; changed = true; }
                for (int k = 0; k <= P1_KIND_UNKNOWN; ++k)
                    if (ImGui::Selectable(Fmt("%s  %s", KindIcon(uint8_t(k)), KindName(uint8_t(k))).c_str(), m_filter.kind == k)) {
                        m_filter.kind = k;
                        changed = true;
                    }
                ImGui::EndCombo();
            }
            ui::FormRow("Rarity", "Entities without a rarity (0xFF) are left out once a minimum is set.");
            changed |= ImGui::Combo("##rarity", &m_filter.minRarity, kRarities, IM_ARRAYSIZE(kRarities));
            ui::FormRow("Path contains", "Case-insensitive, e.g. \"Monsters/\", \"Strongbox\", \"Transition\".");
            changed |= ImGui::InputTextWithHint("##path", "any path", m_filter.text, sizeof m_filter.text);
            ui::FormRow("Only");
            changed |= ImGui::Checkbox("hostile", &m_filter.hostileOnly);
            ImGui::SameLine();
            changed |= ImGui::Checkbox("alive", &m_filter.aliveOnly);
            ui::EndForm();
        }
        if (changed) app.MarkDirty();
    }

    void DrawTable(App& app, const P1Snapshot& s) {
        if (p1::Entities(s).empty()) {
            ui::TextDim("No entities in this snapshot: the table fills in once a character is in an area.");
            return;
        }
        std::vector<Row> rows = CollectRows(s, m_filter);
        ui::TextDim("%zu of %u shown. Click a column to sort; click a row for every field.", rows.size(), s.entity_count);
        // As tall as the header and its rows, up to 12 rows; more scroll inside the table.
        const float rowH = ImGui::GetTextLineHeight() + ImGui::GetStyle().CellPadding.y * 2.0f;
        const float visibleRows = float((std::min)(rows.size(), size_t(12)) + 1);   // + the header
        const ImGuiTableFlags flags = ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_Sortable |
                                      ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit |
                                      ImGuiTableFlags_NoSavedSettings;
        if (!ImGui::BeginTable("entities", 5, flags, ImVec2(0.0f, rowH * visibleRows + 2.0f))) return;
        ImGui::TableSetupScrollFreeze(0, 1);
        // A fixed column is as wide as its widest content or its header plus the sort arrow.
        auto width = [](const char* header, const char* widest) {
            return (std::max)(ImGui::CalcTextSize(widest).x, ImGui::CalcTextSize(header).x + ImGui::GetFontSize());
        };
        ImGui::TableSetupColumn("Kind", ImGuiTableColumnFlags_WidthFixed, width("Kind", ICON_FA_DOOR_OPEN " Transition"), kColKind);
        ImGui::TableSetupColumn("Rarity", ImGuiTableColumnFlags_WidthFixed, width("Rarity", "Unique"), kColRarity);
        ImGui::TableSetupColumn("HP", ImGuiTableColumnFlags_WidthFixed, width("HP", "100%"), kColHp);
        ImGui::TableSetupColumn("Dist", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultSort,
                                width("Dist", "9999"), kColDist);
        ImGui::TableSetupColumn("Path", ImGuiTableColumnFlags_WidthStretch, 0.0f, kColPath);
        ui::TableHeadersRowAligned({ false, false, true, true, false });   // numbers on the right
        SortRows(rows, s, ImGui::TableGetSortSpecs());   // the data changes every frame: sort every frame

        ImGuiListClipper clipper;   // only the visible rows are submitted, however many there are
        clipper.Begin(static_cast<int>(rows.size()));
        while (clipper.Step()) {
            for (int r = clipper.DisplayStart; r < clipper.DisplayEnd; ++r) {
                const Row& row = rows[size_t(r)];
                const P1Entity& e = *p1_entity_at(&s, row.index);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                const std::string label = Fmt("%s %s##%u", KindIcon(e.kind), KindName(e.kind), row.index);
                if (ImGui::Selectable(label.c_str(), app.selection.Is(e),
                                      ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap))
                    app.selection = Selection{ e.address, e.id };
                ImGui::TableNextColumn();
                if (e.rarity <= 3) ImGui::TextColored(ui::RarityColor(e.rarity), "%s", RarityName(e.rarity));
                else ImGui::TextDisabled("-");
                ImGui::TableNextColumn();
                if (row.hpPct >= 0.0f) {
                    ui::TextRight("%.0f%%", double(row.hpPct));
                } else {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
                    ui::TextRight("-");
                    ImGui::PopStyleColor();
                }
                ImGui::TableNextColumn();
                ui::TextRight("%.0f", double(row.dist));
                ImGui::TableNextColumn();
                // The last two path parts, or only the last one when the column is narrower.
                const char* path = p1::Str(s, e.path);
                std::string brief = ShortPath(path, 2);
                if (ImGui::CalcTextSize(brief.c_str()).x > ImGui::GetContentRegionAvail().x) brief = ShortPath(path, 1);
                ImGui::TextUnformatted(brief.c_str());
                ImGui::SetItemTooltip("%s", path);
            }
        }
        ImGui::EndTable();
    }

    void DrawDetail(App& app, const P1Snapshot& s) const {
        ui::SectionTitle(ICON_FA_MAGNIFYING_GLASS, "The picked entity");
        bool ring = app.LayerOn(LayerId::Selection);
        if (ImGui::Checkbox("Pulsing ring around it on the overlay", &ring)) app.SetLayer(LayerId::Selection, ring);
        // Never an empty card: until the user picks a row, it shows the nearest hostile monster
        // the filter lets through.
        const P1Entity* e = ShownEntity(app, s, m_filter);
        if (!app.selection.Any()) {
            if (!e) {
                ui::TextDim("Click a row in the table to see every field of that entity here.");
                return;
            }
            ui::TextDim("Nothing picked yet: this is the nearest hostile monster the filter shows. Click a row to "
                        "pick another.");
        } else if (!e) {
            ui::StatusLine({ ui::Tone::Info, "The picked entity is not in this snapshot: it died, or left the "
                                             "network bubble around the player." });
            if (ImGui::Button(ICON_FA_XMARK "  Clear")) app.selection = Selection{};
            return;
        }
        ui::BeginCard("##detail");
        DrawFields(s, *e);
        ui::EndCard();
    }

    static void DrawFields(const P1Snapshot& s, const P1Entity& e) {
        auto str = [&](p1_str off) {
            const char* v = p1::Str(s, off);
            return v[0] ? v : "(empty)";
        };
        if (ui::BeginKv("fields", { "address / id", "kind", "rarity", "reaction", "targetable", "life / opened / box",
                                    "flags", "grid", "render_pos", "render_bounds", "rotation", "terrain_height",
                                    "hp / es / mana", "path", "minimap_icon", "transition_target", "item_path" })) {
            ui::KvText("address / id", "0x%016llX / %u", static_cast<unsigned long long>(e.address), e.id);
            ui::KvText("kind", "%u = %s", unsigned(e.kind), KindName(e.kind));
            ui::KvText("rarity", "%u = %s", unsigned(e.rarity), RarityName(e.rarity));
            ui::KvText("reaction", "%u (%s)", unsigned(e.reaction),
                       e.reaction == 1 ? "friendly" : (e.reaction == 0xFF ? "none" : "hostile or neutral"));
            ui::KvText("targetable", "%u%s", unsigned(e.targetable), e.targetable == 0xFF ? " (unknown: radar-gated)" : "");
            ui::KvText("life / opened / box", "%u / %u / %u", unsigned(e.has_life), unsigned(e.is_opened),
                       unsigned(e.is_strongbox));
            ui::KvText("flags", "0x%08X (raw, undocumented: do not depend on them)", e.flags);
            ui::KvText("grid", "%d, %d", e.grid_x, e.grid_y);
            ui::KvText("render_pos", "%.1f, %.1f, %.1f", double(e.render_pos.x), double(e.render_pos.y), double(e.render_pos.z));
            ui::KvText("render_bounds", "%.1f, %.1f, %.1f", double(e.render_bounds.x), double(e.render_bounds.y),
                       double(e.render_bounds.z));
            ui::KvText("rotation", "%.3f rad", double(e.rotation));
            ui::KvText("terrain_height", "%.1f", double(e.terrain_height));
            ui::KvText("hp / es / mana", "%d/%d, %d/%d, %d/%d", e.hp, e.hp_max, e.es, e.es_max, e.mana, e.mana_max);
            ui::KvText("path", "%s", str(e.path));
            ui::KvText("minimap_icon", "%s", str(e.minimap_icon));
            ui::KvText("transition_target", "%s", str(e.transition_target));
            ui::KvText("item_path", "%s", str(e.item_path));
            ui::EndKv();
        }
        ui::TextDim("Components (comp_mask 0x%08X):", e.comp_mask);
        ui::Flow flow;
        for (const BitName& c : ComponentNames()) {
            if (!(e.comp_mask & c.bit)) continue;
            flow.Next(ui::ChipWidth(c.name));
            ui::Chip(c.name, ui::Tone::Accent);
        }
    }

    void DrawSelectionRing(const App& app, const P1Frame& f) const {
        const P1Snapshot& s = *f.snapshot;
        if (!app.LayerOn(LayerId::Selection) || !(s.flags & P1_SNAP_CAMERA_VALID)) return;
        const P1Entity* e = ShownEntity(app, s, m_filter);   // the entity the lesson's card shows
        ImVec2 p;
        if (!e || !p1::WorldToScreen(f, e->render_pos, p)) return;
        const LayerState& layer = app.Layer(LayerId::Selection);
        // A pulse needs frames between snapshots: the layer is "animated", so App asks for 20 fps.
        const float phase = float(f.now_us % 1'000'000) / 1e6f * 6.2831853f;
        const float r = layer.size * UiScale(f) * (1.0f + 0.18f * std::sin(phase));
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImU32 col = ImGui::ColorConvertFloat4ToU32(layer.color);
        ImVec4 faint = layer.color;
        faint.w *= 0.35f;
        dl->AddCircle(p, r * 1.35f, ImGui::ColorConvertFloat4ToU32(faint), 0, 2.0f);
        dl->AddCircle(p, r, col, 0, 3.0f);
    }

    Filter m_filter;
};

}  // namespace

std::unique_ptr<Lesson> MakeEntitiesLesson() { return std::make_unique<EntitiesLesson>(); }

}  // namespace ex
