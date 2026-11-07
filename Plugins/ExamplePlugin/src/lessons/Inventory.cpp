// Lesson "inventory": the main inventory grid from inv_occupied, and the items in it.
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "lessons/Common.h"

namespace ex {

namespace {

constexpr LessonInfo kInfo{
    .id = "inventory",
    .group = Group::GameData,
    .icon = ICON_FA_BRIEFCASE,
    .title = "Inventory",
    .summary = "The player's main inventory: which of its 12 x 5 cells are taken, and by which item.",
    .what = "inv_cols x inv_rows (12 x 5), inv_occupied (one bit per cell, y * cols + x) and the item array "
            "(P1InventoryItem: entity address, cell x / y, size w / h, base name).",
    .when = "\"Is there room for a 2 x 3 item\", counting items, overlays on the inventory panel.",
    .limits = "Filled while P1_SNAP_INVENTORY_VALID is set (attached and in game). At most 128 items. Base "
              "names are in the game's language: compare items by what you read, not by a translated name.",
    .snippet = "inventory",
};

// [snippet:inventory]
// Is cell (x, y) taken? One bit per cell, row by row. Check the flag and the
// bounds first: the grid is valid only with P1_SNAP_INVENTORY_VALID, and a bit
// index past 63 would be undefined behaviour.
bool CellTaken(const P1Snapshot& s, uint32_t x, uint32_t y) {
    if (!(s.flags & P1_SNAP_INVENTORY_VALID) || x >= s.inv_cols || y >= s.inv_rows) return false;
    const uint32_t bit = y * s.inv_cols + x;
    return bit < 64 && ((s.inv_occupied >> bit) & 1u) != 0;
}

// Free cells, counted the same way.
uint32_t FreeCells(const P1Snapshot& s) {
    uint32_t free = 0;
    for (uint32_t y = 0; y < s.inv_rows; ++y)
        for (uint32_t x = 0; x < s.inv_cols; ++x) free += CellTaken(s, x, y) ? 0u : 1u;
    return free;
}
// [/snippet]

// One rule for every box: its number (the # of the item list below) in the top-left corner, and
// the base name under it, whole words on as many lines as fit, centred, only when every word
// fits. No initials or cut words to decode: hover the box for the name it has no room for.
void DrawItemLabel(ImDrawList* dl, ImVec2 a, ImVec2 b, const char* name, int number) {
    const float pad = ui::Em(0.2f), lh = ImGui::GetFontSize();
    const std::string num = Fmt("%d", number);
    dl->AddText(ImVec2(std::floor(a.x + pad), std::floor(a.y + pad * 0.5f)), ImGui::GetColorU32(ImGuiCol_Text, 0.75f),
                num.c_str());
    a.y += lh;   // the name goes under the number
    const float boxW = b.x - a.x - pad * 2.0f, boxH = b.y - a.y - pad * 2.0f;
    std::vector<std::string> words, lines;
    for (const char* p = name; *p;) {
        while (*p == ' ') ++p;
        const char* q = p;
        while (*q && *q != ' ') ++q;
        if (q > p) words.emplace_back(p, q);
        p = q;
    }
    bool fits = !words.empty();
    for (const std::string& w : words) {
        if (ImGui::CalcTextSize(w.c_str()).x > boxW) fits = false;
        if (!lines.empty() && ImGui::CalcTextSize((lines.back() + " " + w).c_str()).x <= boxW) lines.back() += " " + w;
        else lines.push_back(w);
    }
    if (float(lines.size()) * lh > boxH) fits = false;
    if (!fits) return;
    const ImU32 col = ImGui::GetColorU32(ImGuiCol_Text);
    float y = a.y + (b.y - a.y - lh * float(lines.size())) * 0.5f;
    for (const std::string& l : lines) {
        const float w = ImGui::CalcTextSize(l.c_str()).x;
        dl->AddText(ImVec2(std::floor(a.x + (b.x - a.x - w) * 0.5f), std::floor(y)), col, l.c_str());
        y += lh;
    }
}

class InventoryLesson final : public Lesson {
public:
    InventoryLesson() : Lesson(kInfo) {}

    ui::Status GetStatus(const App&, const P1Frame& f) const override {
        const P1Snapshot& s = *f.snapshot;
        if (auto gap = GameDataGap(s, Need::InGame)) return *gap;
        if (!(s.flags & P1_SNAP_INVENTORY_VALID))
            return Reason(ui::Tone::Info, "The inventory is not valid this tick: P1_SNAP_INVENTORY_VALID is off.");
        return Live("%u items, %u of %u cells free.", p1::InventoryItems(s).size(), FreeCells(s),
                    s.inv_cols * s.inv_rows);
    }

    void Draw(App&, const P1Frame& f) override {
        const P1Snapshot& s = *f.snapshot;
        if (!(s.flags & P1_SNAP_INVENTORY_VALID) || s.inv_cols == 0 || s.inv_rows == 0 || s.inv_cols > 24 ||
            s.inv_rows > 24) {
            ui::TextDim("The 12 x 5 grid is drawn here while the inventory is valid.");
            return;
        }
        DrawGrid(s);
        DrawList(s);
    }

private:
    static void DrawGrid(const P1Snapshot& s) {
        ui::SectionTitle(ICON_FA_TABLE_CELLS, "The grid");
        const float avail = ImGui::GetContentRegionAvail().x;
        const float cell = std::floor((std::min)(ui::Em(3.4f), (avail - 2.0f) / float(s.inv_cols)));
        const ImVec2 o = ImGui::GetCursorScreenPos();
        const ImVec2 size(cell * float(s.inv_cols), cell * float(s.inv_rows));
        ImGui::InvisibleButton("##grid", size);   // an item to hover, so the tooltip below works
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(o, ImVec2(o.x + size.x, o.y + size.y), ImGui::GetColorU32(ImGuiCol_FrameBg), 3.0f);

        // 1. Cells, straight from inv_occupied.
        for (uint32_t y = 0; y < s.inv_rows; ++y)
            for (uint32_t x = 0; x < s.inv_cols; ++x) {
                const ImVec2 a(o.x + float(x) * cell + 1.0f, o.y + float(y) * cell + 1.0f);
                const ImVec2 b(a.x + cell - 2.0f, a.y + cell - 2.0f);
                const ImU32 col = CellTaken(s, x, y) ? ui::ToneU32(ui::Tone::Warn, 0.22f)
                                                     : ImGui::GetColorU32(ImGuiCol_WindowBg, 0.6f);
                dl->AddRectFilled(a, b, col, 2.0f);
            }

        // 2. Items, from the item array: one rectangle over the cells each one covers.
        const P1InventoryItem* hovered = nullptr;
        int hoveredNumber = 0, number = 0;
        const float inset = std::floor(ui::Em(0.17f)), rounding = std::floor(ui::Em(0.17f));
        ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 0.85f);   // labels a little smaller than text
        for (const P1InventoryItem& it : p1::InventoryItems(s)) {
            ++number;
            const ImVec2 a(o.x + float(it.x) * cell + inset, o.y + float(it.y) * cell + inset);
            const ImVec2 b(o.x + float(it.x + it.w) * cell - inset, o.y + float(it.y + it.h) * cell - inset);
            const bool over = ImGui::IsItemHovered() && ImGui::IsMouseHoveringRect(a, b);
            if (over) {
                hovered = &it;
                hoveredNumber = number;
            }
            dl->AddRectFilled(a, b, ui::ToneU32(ui::Tone::Accent, over ? 0.6f : 0.42f), rounding);
            dl->AddRect(a, b, ui::ToneU32(ui::Tone::Accent, 0.9f), rounding);
            DrawItemLabel(dl, a, b, p1::Str(s, it.base_name), number);
        }
        ImGui::PopFont();
        if (hovered && ImGui::BeginTooltip()) {
            ImGui::Text("#%d %s", hoveredNumber, p1::Str(s, hovered->base_name));
            ImGui::TextDisabled("cell %u, %u; %u x %u", unsigned(hovered->x), unsigned(hovered->y), unsigned(hovered->w),
                                unsigned(hovered->h));
            ImGui::EndTooltip();
        }
        ui::TextDim("Tinted cells: inv_occupied. Framed boxes: the item array, each with its # in the list below; "
                    "the base name shows when it fits. Hover a box for its name.");
    }

    static void DrawList(const P1Snapshot& s) {
        ui::SectionTitle(ICON_FA_LIST, "The items");
        const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                                      ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoSavedSettings;
        if (ImGui::BeginTable("items", 4, flags)) {
            ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("888").x);
            ImGui::TableSetupColumn("Base name", ImGuiTableColumnFlags_WidthStretch, 2.5f);
            ImGui::TableSetupColumn("Cell", ImGuiTableColumnFlags_WidthStretch, 0.7f);
            ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthStretch, 0.7f);
            ui::TableHeadersRowAligned({ true, false, false, false });
            int number = 0;
            for (const P1InventoryItem& it : p1::InventoryItems(s)) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
                ui::TextRight("%d", ++number);   // the number on the item's box in the grid
                ImGui::PopStyleColor();
                ImGui::TableNextColumn();
                ui::TextWrap(p1::Str(s, it.base_name));
                ImGui::SetItemTooltip("entity 0x%016llX", static_cast<unsigned long long>(it.entity));
                ImGui::TableNextColumn();
                ImGui::Text("%u, %u", unsigned(it.x), unsigned(it.y));
                ImGui::TableNextColumn();
                ImGui::Text("%u x %u", unsigned(it.w), unsigned(it.h));
            }
            ImGui::EndTable();
        }
    }
};

}  // namespace

std::unique_ptr<Lesson> MakeInventoryLesson() { return std::make_unique<InventoryLesson>(); }

}  // namespace ex
