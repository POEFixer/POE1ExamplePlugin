// Lesson "widgets": the recommended settings form, then a gallery of ImGui widgets in a panel.
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>

#include "JsonUtil.h"
#include "lessons/Common.h"

namespace ex {

namespace {

constexpr LessonInfo kInfo{
    .id = "widgets",
    .group = Group::Drawing,
    .icon = ICON_FA_SLIDERS,
    .title = "Widgets",
    .summary = "A settings form laid out the recommended way, and a gallery of what a panel can hold.",
    .what = "OnPanel is a full ImGui frame inside the app's Plugins tab: every ImGui widget works, with full "
            "mouse and keyboard input while the user hovers or focuses the panel.",
    .when = "Your plugin's settings. One control per row, a measured label column, a (?) for anything that "
            "needs explaining, and every change saved (debounced).",
    .limits = "At most 60 panel frames a second, only while shown; per frame 75,000 vertices and 4,096 "
              "commands. Popups and combo lists are clipped to the panel. Ctrl+V pastes (the app sends its "
              "clipboard first); no IME composition; TextLinkOpenURL does nothing (no browser in the sandbox).",
    .snippet = "widgets",
};

struct Form {
    bool enabled = true;
    int mode = 1;
    float radius = 120.0f;
    ImVec4 colour = ImVec4(0.30f, 0.75f, 1.00f, 1.00f);
    char label[64] = "Watch out";
};

constexpr const char* kModes[] = { "Off", "Gentle", "Aggressive" };

// [snippet:widgets]
// A settings form, the recommended way: a measured label column (no width
// typed for English text), one control per row filling the value column, a
// (?) where a setting needs explaining, and every change marked dirty so the
// plugin saves it 1 s later.
bool DrawReferenceForm(Form& form) {
    bool changed = false;
    if (ui::BeginForm("form", { "Enabled", "Mode", "Radius", "Colour", "Label" })) {
        ui::FormRow("Enabled");
        changed |= ImGui::Checkbox("##enabled", &form.enabled);
        ImGui::BeginDisabled(!form.enabled);   // dependent rows stay visible, greyed out
        ui::FormRow("Mode", "Gentle draws only rare and unique monsters; Aggressive draws all of them.");
        changed |= ImGui::Combo("##mode", &form.mode, kModes, IM_ARRAYSIZE(kModes));
        // Ctrl+click types any number: AlwaysClamp keeps a typed value inside the range too.
        ui::FormRow("Radius", "In world units: about 10.9 per grid cell. Ctrl+click to type a value "
                              "(kept within 10 to 300).");
        changed |= ImGui::SliderFloat("##radius", &form.radius, 10.0f, 300.0f, "%.0f", ImGuiSliderFlags_AlwaysClamp);
        ui::FormRow("Colour");
        changed |= ImGui::ColorEdit4("##colour", &form.colour.x, ImGuiColorEditFlags_AlphaBar);
        ui::FormRow("Label", "Any language: the text is UTF-8 end to end.");
        changed |= ImGui::InputText("##label", form.label, sizeof form.label);
        ImGui::EndDisabled();
        ui::EndForm();
    }
    return changed;
}
// [/snippet]

class WidgetsLesson final : public Lesson {
public:
    WidgetsLesson() : Lesson(kInfo) {}

    ui::Status GetStatus(const App& app, const P1Frame&) const override {
        return Live("the form below is saved in settings.json (%s).",
                    app.settings.dirty ? "a change is waiting to be written" : "everything written");
    }

    void Draw(App& app, const P1Frame& f) override {
        ui::SectionTitle(ICON_FA_LIST_CHECK, "A settings form");
        ui::BeginCard("##formcard");
        if (DrawReferenceForm(m_form)) app.MarkDirty();
        ui::EndCard();
        ui::SectionTitle(ICON_FA_SHAPES, "Gallery");
        ui::TextDim("The gallery shows widgets, not settings: only the open tab is saved. Every row names the "
                    "ImGui call that draws it.");
        if (ImGui::BeginTabBar("gallery", ImGuiTabBarFlags_FittingPolicyScroll)) {
            static constexpr const char* kTabs[] = { "Buttons", "Inputs", "Text", "Layout", "Data", "Popups" };
            for (int i = 0; i < IM_ARRAYSIZE(kTabs); ++i) {
                // SetSelected once, after a load: ImGui remembers the open tab by itself afterwards.
                const ImGuiTabItemFlags flags = (m_selectTab && i == m_tab) ? ImGuiTabItemFlags_SetSelected : 0;
                if (!ImGui::BeginTabItem(kTabs[i], nullptr, flags)) continue;
                if (!m_selectTab && i != m_tab) {
                    m_tab = i;
                    app.MarkDirty();
                }
                // Where the open tab's code is: the chip under the page shows the reference form.
                const int line = TabLine(i);
                ui::TextDim("%s src/lessons/Widgets.cpp:%d · %s", ICON_FA_FILE_CODE, line, kTabFunctions[i]);
                switch (i) {
                    case 0: TabButtons(); break;
                    case 1: TabInputs(); break;
                    case 2: TabText(); break;
                    case 3: TabLayout(); break;
                    case 4: TabData(f); break;
                    default: TabPopups(); break;
                }
                ImGui::EndTabItem();
            }
            m_selectTab = false;
            ImGui::EndTabBar();
        }
    }

    void Load(App&, const nlohmann::json& j) override {
        m_tab = js::Int(j, "galleryTab", 0, 0, 5);
        m_selectTab = true;
        m_form.enabled = js::Bool(j, "enabled", m_form.enabled);
        m_form.mode = js::Int(j, "mode", m_form.mode, 0, IM_ARRAYSIZE(kModes) - 1);
        m_form.radius = js::Float(j, "radius", m_form.radius, 10.0f, 300.0f);
        m_form.colour = js::Color(j, "colour", m_form.colour);
        const std::string label = js::String(j, "label", m_form.label, sizeof m_form.label - 1);
        std::memcpy(m_form.label, label.c_str(), label.size() + 1);
    }
    void Save(const App&, nlohmann::json& j) const override {
        j["enabled"] = m_form.enabled;
        j["mode"] = m_form.mode;
        j["radius"] = m_form.radius;
        j["colour"] = js::ColorJson(m_form.colour);
        j["label"] = m_form.label;
        j["galleryTab"] = m_tab;
    }

private:
    // The six tab functions, and the line each starts on (for the chip above the open tab).
    static constexpr const char* kTabFunctions[] = { "TabButtons()", "TabInputs()", "TabText()",
                                                     "TabLayout()", "TabData()", "TabPopups()" };
    static int TabLine(int tab) {
        // Each constant is __LINE__ taken right above its function, so it cannot go stale.
        const int lines[] = { kButtonsLine, kInputsLine, kTextLine, kLayoutLine, kDataLine, kPopupsLine };
        return lines[std::clamp(tab, 0, 5)];
    }

    static constexpr int kButtonsLine = __LINE__ + 1;
    void TabButtons() {
        if (!ui::BeginForm("buttons", { "Button", "SmallButton", "ArrowButton", "Checkbox", "RadioButton",
                                        "Selectable" }))
            return;
        ui::FormRow("Button");
        if (ImGui::Button("Click me")) ++m_clicks;
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::Text("clicks: %d", m_clicks);
        ui::FormRow("SmallButton", "A button without frame padding, for a line of text.");
        if (ImGui::SmallButton("Small")) ++m_clicks;
        ui::FormRow("ArrowButton");
        if (ImGui::ArrowButton("##left", ImGuiDir_Left)) --m_counter;
        ImGui::SameLine();
        if (ImGui::ArrowButton("##right", ImGuiDir_Right)) ++m_counter;
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::Text("counter: %d", m_counter);
        ui::FormRow("Checkbox");
        ImGui::Checkbox("##check", &m_check);
        ui::FormRow("RadioButton");
        ImGui::RadioButton("One", &m_radio, 0);
        ImGui::SameLine();
        ImGui::RadioButton("Two", &m_radio, 1);
        ImGui::SameLine();
        ImGui::RadioButton("Three", &m_radio, 2);
        ui::FormRow("Selectable", "A row that stays highlighted once picked: lists, menus.");
        for (int i = 0; i < 3; ++i)
            if (ImGui::Selectable(Fmt("Selectable %d", i + 1).c_str(), m_selected == i)) m_selected = i;
        ui::EndForm();
    }

    static constexpr int kInputsLine = __LINE__ + 1;
    void TabInputs() {
        if (!ui::BeginForm("inputs", { "SliderInt", "SliderFloat", "SliderAngle", "DragFloat2", "DragIntRange2",
                                       "InputInt", "InputFloat", "Combo", "ColorEdit3" }))
            return;
        // AlwaysClamp on every bounded slider and drag: Ctrl+click (or a double-click on a drag)
        // types any number, and the range holds only with this flag.
        const ImGuiSliderFlags clamp = ImGuiSliderFlags_AlwaysClamp;
        ui::FormRow("SliderInt");
        ImGui::SliderInt("##si", &m_int, 0, 100, "%d of 100", clamp);
        ui::FormRow("SliderFloat", "Ctrl+click a slider to type a value: AlwaysClamp keeps it in range.");
        ImGui::SliderFloat("##sf", &m_float, 0.0f, 1.0f, "ratio %.2f", clamp);
        ui::FormRow("SliderAngle");
        ImGui::SliderAngle("##sa", &m_angle, -180.0f, 180.0f, "%.0f degrees", clamp);
        ui::FormRow("DragFloat2", "Drag sideways; Shift drags faster, Alt slower.");
        ImGui::DragFloat2("##df", m_vec, 0.5f, -100.0f, 100.0f, "%.1f", clamp);
        ui::FormRow("DragIntRange2");
        ImGui::DragIntRange2("##dr", &m_range[0], &m_range[1], 1.0f, 0, 100, "from %d", "to %d", clamp);
        ui::FormRow("InputInt");
        ImGui::InputInt("##ii", &m_int);
        ui::FormRow("InputFloat");
        ImGui::InputFloat("##if", &m_float, 0.01f, 0.1f, "%.3f");
        ui::FormRow("Combo");
        ImGui::Combo("##co", &m_combo, kModes, IM_ARRAYSIZE(kModes));
        ui::FormRow("ColorEdit3");
        ImGui::ColorEdit3("##ce", m_colour);
        ui::EndForm();
    }

    static constexpr int kTextLine = __LINE__ + 1;
    void TabText() {
        if (!ui::BeginForm("text", { "InputTextWithHint", "InputTextMultiline", "ui::TextWrap", "ui::BulletWrapped" }))
            return;
        ui::FormRow("InputTextWithHint", "Ctrl+V pastes: the app forwards its clipboard first.");
        ImGui::InputTextWithHint("##one", "type or paste here", m_line, sizeof m_line);
        ui::FormRow("InputTextMultiline");
        ImGui::InputTextMultiline("##many", m_text, sizeof m_text,
                                  ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * 4.0f + ImGui::GetStyle().FramePadding.y * 2.0f));
        ui::FormRow("ui::TextWrap", "This plugin's wrap (src/ui): unlike ImGui::TextWrapped it never breaks inside "
                                    "a word such as settings.json.");
        ui::TextWrap("Long text wraps at the panel's edge, so a narrow panel grows taller instead of wider: "
                     "settings.json, p1::Settings and 16 MiB stay whole.");
        ui::FormRow("ui::BulletWrapped");
        ui::BulletWrapped("Non-ASCII text works everywhere: Привет, こんにちは, 안녕하세요, สวัสดี.");
        ui::EndForm();
    }

    static constexpr int kLayoutLine = __LINE__ + 1;
    static void TabLayout() {
        if (!ui::BeginForm("layout", { "TreeNode", "CollapsingHeader", "SeparatorText", "BeginTable" })) return;
        // Open the first time they are drawn, so the tab shows what they hold; ImGui remembers
        // the user's choice after that.
        ui::FormRow("TreeNode");
        ImGui::SetNextItemOpen(true, ImGuiCond_Once);
        if (ImGui::TreeNode("A tree node")) {
            ImGui::TextUnformatted("Child row");
            ImGui::SetNextItemOpen(true, ImGuiCond_Once);
            if (ImGui::TreeNode("A nested node")) {
                ImGui::TextUnformatted("Deeper");
                ImGui::TreePop();
            }
            ImGui::TreePop();
        }
        ui::FormRow("CollapsingHeader", "Fold long, rarely used sections of a settings panel.");
        ImGui::SetNextItemOpen(true, ImGuiCond_Once);
        if (ImGui::CollapsingHeader("A collapsing header")) ui::TextWrap("What it folds.");
        ui::FormRow("SeparatorText");
        ImGui::SeparatorText("A titled rule");
        ui::FormRow("BeginTable", "Stretching columns follow the panel's width.");
        const ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_NoSavedSettings;
        if (ImGui::BeginTable("cols", 3, flags)) {
            for (int i = 0; i < 6; ++i) {
                ImGui::TableNextColumn();
                ImGui::Text("Cell %d", i + 1);
            }
            ImGui::EndTable();
        }
        ui::EndForm();
    }

    static constexpr int kDataLine = __LINE__ + 1;
    void TabData(const P1Frame& f) {
        if (!ui::BeginForm("data", { "Sortable table", "PlotLines", "PlotHistogram", "ProgressBar" })) return;
        // A small sortable table: sorted in place, and only when the user changes the order.
        ui::FormRow("Sortable table", "TableGetSortSpecs: click a header to sort, again to reverse.");
        RowData (&rows)[5] = m_rows;
        const ImGuiTableFlags flags = ImGuiTableFlags_Sortable | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                                      ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoHostExtendX |
                                      ImGuiTableFlags_NoSavedSettings;
        if (ImGui::BeginTable("sortable", 2, flags)) {
            // Fixed columns as wide as their content: the sort arrow stays next to its header.
            float nameW = 0.0f;
            for (const RowData& r : rows) nameW = (std::max)(nameW, ImGui::CalcTextSize(r.name).x);
            nameW = (std::max)(nameW, ImGui::CalcTextSize("Name").x) + ImGui::GetFontSize();
            ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultSort, nameW, 0);
            ImGui::TableSetupColumn("Count", ImGuiTableColumnFlags_WidthFixed,
                                    ImGui::CalcTextSize("Count").x + ImGui::GetFontSize(), 1);
            ui::TableHeadersRowAligned({ false, true });   // a number column reads best on the right
            if (ImGuiTableSortSpecs* specs = ImGui::TableGetSortSpecs(); specs && specs->SpecsDirty && specs->SpecsCount > 0) {
                const ImGuiTableColumnSortSpecs s = specs->Specs[0];
                std::sort(std::begin(rows), std::end(rows), [s](const RowData& a, const RowData& b) {
                    const bool less = s.ColumnUserID == 0 ? std::strcmp(a.name, b.name) < 0 : a.count < b.count;
                    const bool more = s.ColumnUserID == 0 ? std::strcmp(b.name, a.name) < 0 : b.count < a.count;
                    return s.SortDirection == ImGuiSortDirection_Ascending ? less : more;
                });
                specs->SpecsDirty = false;   // sort only when the user changes the order
            }
            for (const RowData& r : rows) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(r.name);
                ImGui::TableNextColumn();
                ui::TextRight("%d", r.count);
            }
            ImGui::EndTable();
        }
        // A plot that moves: the last 3 seconds of a wave, sampled from the clock (so it reads
        // the same whatever the frame rate).
        constexpr int kSamples = IM_ARRAYSIZE(m_wave);
        const double now = double(f.now_us % 600'000'000) / 1e6;
        for (int i = 0; i < kSamples; ++i) {
            const double t = now - 3.0 * double(kSamples - 1 - i) / double(kSamples - 1);
            m_wave[i] = float(0.75 * std::sin(t * 2.6) + 0.25 * std::sin(t * 7.1));
        }
        const float h = ImGui::GetFrameHeight() * 2.5f;
        ui::FormRow("PlotLines", "The last 3 s of a wave, sampled from the clock.");
        ImGui::PlotLines("##lines", m_wave, kSamples, 0, nullptr, -1.0f, 1.0f, ImVec2(-FLT_MIN, h));
        static const float bars[] = { 0.2f, 0.5f, 0.9f, 0.6f, 0.3f, 0.8f, 0.4f, 0.7f, 0.1f, 0.5f };
        ui::FormRow("PlotHistogram");
        ImGui::PlotHistogram("##bars", bars, IM_ARRAYSIZE(bars), 0, nullptr, 0.0f, 1.0f, ImVec2(-FLT_MIN, h));
        ui::FormRow("ProgressBar");
        const float progress = float(double(f.now_us % 4'000'000) / 4e6);
        ImGui::ProgressBar(progress, ImVec2(-FLT_MIN, 0.0f));
        ui::EndForm();
    }

    static constexpr int kPopupsLine = __LINE__ + 1;
    void TabPopups() {
        if (!ui::BeginForm("popups", { "SetItemTooltip", "BeginPopup", "BeginPopupModal", "BeginPopupContextItem" }))
            return;
        ui::FormRow("SetItemTooltip");
        ImGui::Button("Hover me");
        ImGui::SetItemTooltip("SetItemTooltip: a tooltip for the item before it.");
        ui::FormRow("BeginPopup");
        if (ImGui::Button("Open a popup")) ImGui::OpenPopup("menu");
        if (ImGui::BeginPopup("menu")) {
            for (const char* m : kModes)
                if (ImGui::Selectable(m)) m_popupChoice = m;
            ImGui::EndPopup();
        }
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ui::TextDim("picked: %s", m_popupChoice);
        ui::FormRow("BeginPopupModal", "A modal dims the panel and takes the input until it closes.");
        if (ImGui::Button("Open a modal")) ImGui::OpenPopup("A modal##modal");
        if (ImGui::BeginPopupModal("A modal##modal", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextUnformatted("A modal dims the panel and takes the input until it closes.");
            if (ImGui::Button("Close")) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        ui::FormRow("BeginPopupContextItem");
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Right-click this line");
        if (ImGui::BeginPopupContextItem("context")) {
            if (ImGui::MenuItem("Count a click")) ++m_clicks;
            if (ImGui::MenuItem("Reset the counter")) m_counter = 0;
            ImGui::EndPopup();
        }
        ui::EndForm();
        ui::Note("Popups, menus and combo lists are clipped to the panel's rectangle: keep them small.");
    }

    struct RowData {
        const char* name;
        int count;
    };

    Form m_form;
    int m_tab = 0;              // the open gallery tab, saved
    bool m_selectTab = true;    // select m_tab on the next frame (after a load)
    RowData m_rows[5] = { { "Chaos Orb", 42 }, { "Divine Orb", 3 }, { "Exalted Orb", 7 },
                          { "Orb of Alchemy", 118 }, { "Vaal Orb", 21 } };
    int m_clicks = 0, m_counter = 0, m_radio = 0, m_selected = -1;
    bool m_check = true;
    int m_int = 25, m_combo = 0;   // off the middle: a slider's knob sits over its centred value there
    int m_range[2] = { 10, 60 };
    float m_float = 0.25f, m_angle = 1.5708f;   // 90 degrees
    float m_vec[2] = { 1.0f, 2.0f };
    float m_colour[3] = { 0.9f, 0.6f, 0.2f };
    char m_line[128] = "";
    char m_text[512] = "Several lines\nof text.";
    float m_wave[90] = {};
    const char* m_popupChoice = "(none yet)";
};

}  // namespace

std::unique_ptr<Lesson> MakeWidgetsLesson() { return std::make_unique<WidgetsLesson>(); }

}  // namespace ex
