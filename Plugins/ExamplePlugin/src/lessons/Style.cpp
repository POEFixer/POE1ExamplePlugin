// Lesson "style": the app's theme as the panel sees it, fonts, icons and non-ASCII text.
#include <algorithm>

#include "lessons/Common.h"

namespace ex {

namespace {

constexpr LessonInfo kInfo{
    .id = "style",
    .group = Group::Drawing,
    .icon = ICON_FA_PALETTE,
    .title = "Style & fonts",
    .summary = "Look like the app: its theme's colours and sizes, its fonts, Font Awesome icons, any script.",
    .what = "The host applies the app's current theme and UI scale to your panel's ImGui style, and replaces "
            "it whole when the user changes theme. The overlay keeps ImGui's default style, with FontScaleMain "
            "set to the UI scale.",
    .when = "Take colours and spacing from ImGui::GetStyle() and sizes from the font size. Define only the "
            "colours that carry a meaning, once (this plugin: ui::ToneColor), in a light and a dark variant.",
    .limits = "Segoe UI at 18 px with Cyrillic, Thai, Japanese and Korean merged in, plus Font Awesome 6 Solid "
              "at 16 px (#include <IconsFontAwesome6.h>). Your own fonts are not part of v1. For local changes "
              "use PushStyleColor / PushStyleVar; never write to GetStyle(): the host replaces it on every theme "
              "change.",
    .snippet = "style",
};

struct IconSample {
    const char* glyph;
    const char* name;
};

constexpr IconSample kIcons[] = {
    { ICON_FA_BELL, "BELL" },           { ICON_FA_GEAR, "GEAR" },
    { ICON_FA_SKULL, "SKULL" },         { ICON_FA_GEM, "GEM" },
    { ICON_FA_FLASK, "FLASK" },         { ICON_FA_MAP, "MAP" },
    { ICON_FA_CROSSHAIRS, "CROSSHAIRS" }, { ICON_FA_EYE, "EYE" },
    { ICON_FA_LOCK, "LOCK" },           { ICON_FA_TRASH, "TRASH" },
    { ICON_FA_COPY, "COPY" },           { ICON_FA_CHECK, "CHECK" },
    { ICON_FA_XMARK, "XMARK" },         { ICON_FA_STAR, "STAR" },
    { ICON_FA_FIRE, "FIRE" },           { ICON_FA_SNOWFLAKE, "SNOWFLAKE" },
};

constexpr const char* kSamples[][2] = {
    { "Русский", "Съешь же ещё этих мягких французских булок." },
    { "Deutsch", "Größe, Übung, Straße." },
    { "Français", "Ça déjà été là : où ?" },
    { "Português", "Ação, não, coração." },
    { "日本語", "エグザイル、ようこそ。" },
    { "한국어", "망명자여, 환영합니다." },
    { "ไทย", "ยินดีต้อนรับ" },
    { "Symbols", "→ ← ↑ ↓  … – ’ “ ” ° ± × µ" },
};

class StyleLesson final : public Lesson {
public:
    StyleLesson() : Lesson(kInfo) {}

    ui::Status GetStatus(const App&, const P1Frame& f) const override {
        return Live("a %s theme, font %.0f px, UI scale %.2f.", ui::IsLightTheme() ? "light" : "dark",
                    double(ImGui::GetFontSize()), double(f.viewport.ui_scale));
    }

    void Draw(App&, const P1Frame& f) override {
        DrawMetrics(f);
        DrawSemantic();
        DrawSwatches();
        DrawIcons();
        DrawText();
    }

private:
    static void DrawMetrics(const P1Frame& f) {
        const ImGuiStyle& st = ImGui::GetStyle();
        ui::SectionTitle(ICON_FA_RULER_COMBINED, "Sizes");
        if (ui::BeginKv("sizes", { "font size", "FontSizeBase x FontScaleMain", "ui_scale", "WindowPadding",
                                   "FramePadding", "ItemSpacing", "rounding" })) {
            ui::KvText("font size", "%.1f px (ImGui::GetFontSize: measure with it, never with constants)",
                       double(ImGui::GetFontSize()));
            ui::KvText("FontSizeBase x FontScaleMain", "%.1f x %.2f", double(st.FontSizeBase), double(st.FontScaleMain));
            ui::KvText("ui_scale", "%.2f (P1Viewport: the app's UI scale, also for the overlay)", double(f.viewport.ui_scale));
            ui::KvText("WindowPadding", "%.0f, %.0f", double(st.WindowPadding.x), double(st.WindowPadding.y));
            ui::KvText("FramePadding", "%.0f, %.0f", double(st.FramePadding.x), double(st.FramePadding.y));
            ui::KvText("ItemSpacing", "%.0f, %.0f", double(st.ItemSpacing.x), double(st.ItemSpacing.y));
            ui::KvText("rounding", "window %.0f, frame %.0f, grab %.0f", double(st.WindowRounding),
                       double(st.FrameRounding), double(st.GrabRounding));
            ui::EndKv();
        }
    }

    static void DrawSemantic() {
        ui::SectionTitle(ICON_FA_SWATCHBOOK, "The plugin's own colours (src/ui/Ui.cpp)");
        if (!ui::BeginKv("palette", { "ui::ToneColor", "ui::RarityColor" })) return;
        const struct { ui::Tone tone; const char* name; } tones[] = {
            { ui::Tone::Neutral, "Neutral" }, { ui::Tone::Accent, "Accent (theme)" }, { ui::Tone::Ok, "Ok" },
            { ui::Tone::Info, "Info" },       { ui::Tone::Warn, "Warn" },             { ui::Tone::Error, "Error" },
        };
        // A row label on the chips' text line: chips are a little taller than text.
        ui::KvKeyChips("ui::ToneColor");
        {
            ui::Flow flow;
            for (const auto& t : tones) {
                const std::string text = Fmt("%s  %s", ui::ToneIcon(t.tone), t.name);
                flow.Next(ui::ChipWidth(text.c_str()));
                ui::Chip(text.c_str(), t.tone);
            }
        }
        ui::KvKeyChips("ui::RarityColor");
        {
            ui::Flow flow;
            for (uint8_t r = 0; r <= 3; ++r) {   // item and monster rarity, a light and a dark variant each
                flow.Next(ui::ChipWidth(RarityName(r)));
                ui::Chip(RarityName(r), ui::RarityColor(r), "ui::RarityColor: item and monster rarity.");
            }
        }
        ui::EndKv();
    }

    static void DrawSwatches() {
        ui::SectionTitle(ICON_FA_PALETTE, "The theme's colours (ImGuiCol_*)");
        ui::TextDim("Hover a swatch for its value. Use these through ImGui::GetStyle().Colors or GetColorU32.");
        // As many columns as the LONGEST name allows: measured, so no name runs into the next column.
        const float box = ImGui::GetTextLineHeight();
        float widest = 0.0f;
        for (int i = 0; i < ImGuiCol_COUNT; ++i) widest = (std::max)(widest, ImGui::CalcTextSize(ImGui::GetStyleColorName(i)).x);
        const float cell = box + ImGui::GetStyle().ItemSpacing.x * 2.0f + widest + ImGui::GetStyle().CellPadding.x * 2.0f;
        const int columns = std::clamp(int(ImGui::GetContentRegionAvail().x / cell), 1, 4);
        if (!ImGui::BeginTable("swatches", columns, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_NoSavedSettings))
            return;
        for (int i = 0; i < ImGuiCol_COUNT; ++i) {
            ImGui::TableNextColumn();
            const ImVec4 c = ImGui::GetStyle().Colors[i];
            ImGui::PushID(i);
            ImGui::ColorButton("##sw", c, ImGuiColorEditFlags_AlphaPreviewHalf, ImVec2(box, box));
            // A hairline around every swatch: a colour close to the page's own would vanish without it.
            ImGui::GetWindowDrawList()->AddRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(),
                                                ImGui::GetColorU32(ImGuiCol_Text, 0.25f), 2.0f);
            ImGui::PopID();
            ImGui::SameLine();
            ImGui::TextUnformatted(ImGui::GetStyleColorName(i));
            ImGui::SetItemTooltip("ImGuiCol_%s = %.2f, %.2f, %.2f, %.2f", ImGui::GetStyleColorName(i), double(c.x),
                                  double(c.y), double(c.z), double(c.w));
        }
        ImGui::EndTable();
    }

    static void DrawIcons() {
        ui::SectionTitle(ICON_FA_ICONS, "Font Awesome 6 Solid");
        ui::TextDim("Write ICON_FA_<NAME> next to text: ICON_FA_BELL \"  Text\". The glyphs come merged into the font.");
        ui::Flow flow;
        for (const IconSample& i : kIcons) {
            const std::string text = Fmt("%s %s", i.glyph, i.name);
            flow.Next(ui::ChipWidth(text.c_str()));
            ui::Chip(text.c_str(), ui::Tone::Neutral);
        }
    }

    static void DrawText() {
        ui::SectionTitle(ICON_FA_FONT, "Any script");
        // The label column is measured over every label, in its own script.
        float labelW = 0.0f;
        for (const auto& s : kSamples) labelW = (std::max)(labelW, ImGui::CalcTextSize(s[0]).x);
        if (!ImGui::BeginTable("scripts", 2, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoSavedSettings)) return;
        ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthFixed, labelW + ImGui::GetStyle().ItemInnerSpacing.x);
        ImGui::TableSetupColumn("text", ImGuiTableColumnFlags_WidthStretch);
        for (const auto& s : kSamples) ui::KvText(s[0], "%s", s[1]);
        ImGui::EndTable();
    }
};

}  // namespace

std::unique_ptr<Lesson> MakeStyleLesson() { return std::make_unique<StyleLesson>(); }

}  // namespace ex
