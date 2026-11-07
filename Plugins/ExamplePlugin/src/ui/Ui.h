#pragma once
// ============================================================================
// ui — the example's mini UI kit. Nothing in it is specific to this example:
// copy the two files into your own plugin if you like the look.
//
// The rules it follows, and why:
//   - No width is sized for English text. Label columns are MEASURED with
//     CalcTextSize, so a longer label (or a translation) never clips.
//   - Every gap and size comes from ImGui::GetStyle() or the font size (Em);
//     only 1 px hairlines are fixed. The host gives the panel the app's theme
//     and UI scale, so the panel follows both.
//   - Colours come from the theme, except a few SEMANTIC ones (OK / info /
//     warning / error, item rarity), defined once in Ui.cpp with a variant
//     for light and for dark themes.
//   - Text that can be long wraps. Rows of chips and buttons wrap too (Flow),
//     so a 760 px panel never scrolls sideways.
// ============================================================================
#include <imgui.h>
#include <IconsFontAwesome6.h>

#include <cstdint>
#include <initializer_list>
#include <string>

namespace ui {

// ---- semantic colours ------------------------------------------------------------
enum class Tone : uint8_t { Neutral, Accent, Ok, Info, Warn, Error };

struct Status {
    Tone tone = Tone::Neutral;
    std::string text;
};

bool        IsLightTheme();                         // the theme's window background is light
ImVec4      ToneColor(Tone t);                      // text / icon colour for the theme in use
ImU32       ToneU32(Tone t, float alpha = 1.0f);
const char* ToneIcon(Tone t);
ImVec4      RarityColor(uint8_t rarity);            // 0 normal .. 3 unique; anything else: text colour

// ---- sizes -------------------------------------------------------------------------
float Em(float n = 1.0f);                           // n times the current font size
float ButtonWidth(const char* label);               // what ImGui::Button(label) will take

// Lays items out left to right and starts a new line when the next one does not fit.
//   ui::Flow flow;
//   for (...) { flow.Next(ui::ButtonWidth(label)); ImGui::Button(label); }
class Flow {
public:
    Flow();
    void Next(float itemWidth);
private:
    float m_right;
    bool m_first = true;
};

// ---- text --------------------------------------------------------------------------
void PageTitle(const char* icon, const char* title);
// An accent icon in a fixed-width box, the title, and a rule to the right edge. The fixed box
// keeps every section's title at the same x, whatever the icon's own width.
void SectionTitle(const char* icon, const char* text);
float IconBoxWidth();                                   // that box: one place for every icon column

// Wrapped text that breaks only at blanks and line feeds. ImGui's TextWrapped also breaks
// after '.' and ',', which splits "settings.json" or "p1::Settings" across two lines; a word
// wider than the whole line is still cut, where ImGui would cut it. `color` null = Text colour.
void TextWrap(const char* text, const ImVec4* color = nullptr);
void TextDim(const char* fmt, ...) IM_FMTARGS(1);       // wrapped, the theme's disabled text colour
void Note(const char* text);                            // an info icon and a wrapped, dimmed text
void BulletWrapped(const char* fmt, ...) IM_FMTARGS(1); // a bullet and a wrapped text
void HelpMarker(const char* text);                      // a (?) that shows `text` on hover
void TextRight(const char* fmt, ...) IM_FMTARGS(1);     // right-aligned in what is left of the line (a table cell)

// ---- containers ------------------------------------------------------------------
// A framed group that grows with its content. Always call EndCard, whatever BeginCard returned.
bool BeginCard(const char* id);
void EndCard();

// A two-column key / value table whose key column is as wide as the widest label.
//   if (ui::BeginKv("area", { "Name", "Level" })) { ui::KvText("Name", "%s", name); ...; ui::EndKv(); }
bool BeginKv(const char* id, std::initializer_list<const char*> labels);
// Between these two, every BeginKv / BeginForm uses ONE key column: the widest one of the
// group, as measured in the previous frame, so values line up down a page. `id` names the
// group (a new id starts from scratch). The shell wraps each lesson's demo in one.
void BeginAlignedGroup(const char* id);
void EndAlignedGroup();
void KvKey(const char* label);                          // starts a row; draw the value next
void KvKeyChips(const char* label);                     // the same, for a row of chips (a little taller than text)
void KvText(const char* label, const char* fmt, ...) IM_FMTARGS(2);
void KvTone(const char* label, Tone tone, const char* fmt, ...) IM_FMTARGS(3);
void EndKv();

// A settings form: the same measured label column, room for a help marker, and the
// widget filling the value column. One control per row.
//   if (ui::BeginForm("form", { "Radius", "Colour" })) {
//       ui::FormRow("Radius", "What the radius is for.");  ImGui::SliderFloat("##radius", ...);
//       ui::EndForm();
//   }
bool BeginForm(const char* id, std::initializer_list<const char*> labels);
void FormRow(const char* label, const char* help = nullptr);
void EndForm();

// The header row of a table whose numeric columns are right-aligned: pass one flag per column,
// in order (true = right). Call it where you would call ImGui::TableHeadersRow().
void TableHeadersRowAligned(std::initializer_list<bool> rightAligned);

// ---- small widgets ---------------------------------------------------------------
float ChipWidth(const char* text);
float ChipPadY();                                       // a chip's padding above (and below) its text
// A pill with text. Returns true while hovered; shows `tooltip` (wrapped) on hover.
bool  Chip(const char* text, Tone tone, const char* tooltip = nullptr);
bool  Chip(const char* text, const ImVec4& color, const char* tooltip = nullptr);   // any colour (rarity)
// A tinted line with an icon: the "Live: ..." / "why it is empty" line of every lesson.
void  StatusLine(const Status& status);
// A bar: `fraction` filled in `fill`, and an optional `reserved` part hatched at the right end.
// `label` sits at the left end, `value` is right-aligned where the free part ends, and `note`
// is right-aligned inside the hatched part when it fits ("177 reserved"). Text is outlined
// only where the theme's text colour would not read on the bar by itself.
// width 0 = the whole available width.
void  Meter(float fraction, ImU32 fill, const char* label, const char* value = nullptr, float reserved = 0.0f,
            const char* note = nullptr, float width = 0.0f);
// A thin bar under a value. A budget (more is worse) is green, amber past 60 %, red past 90 %;
// a rate or a target (`budget` false: nearer the cap is the goal) is drawn in the accent colour.
void  UsageBar(float fraction, float width = 0.0f, bool budget = true);
// A framed block of text that scrolls sideways inside itself instead of widening the page.
void  CodeBlock(const char* id, const char* text, int maxLines = 20);

}  // namespace ui
