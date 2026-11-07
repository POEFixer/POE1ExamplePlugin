#include "ui/Ui.h"

#include <imgui_internal.h>   // FindRenderedTextEnd ("##id" suffixes), ItemSize / ItemAdd, ImTextCharFromUtf8

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace ui {

namespace {

float Luminance(const ImVec4& c) { return 0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z; }

ImU32 ToU32(ImVec4 c, float alpha) {
    c.w *= alpha;
    return ImGui::ColorConvertFloat4ToU32(c);
}

ImVec4 Dimmed(ImVec4 c, float alpha) {
    c.w *= alpha;
    return c;
}

float Measure(const char* a, const char* b) {
    return ImGui::GetFont()->CalcTextSizeA(ImGui::GetFontSize(), FLT_MAX, 0.0f, a, b).x;
}

bool IsBlank(char c) { return c == ' ' || c == '\t'; }

// "16" + "MiB", "1.5" + "s,", "60" + "fps": a number and the short unit after it stay on one line.
bool NumberAndUnit(const char* n0, const char* n1, const char* u0, const char* u1) {
    if (n1 <= n0 || n1[-1] < '0' || n1[-1] > '9' || u1 - u0 > 4) return false;
    for (const char* c = n0; c < n1; ++c)
        if (!((*c >= '0' && *c <= '9') || *c == '.' || *c == ',' || *c == '+' || *c == '-')) return false;
    const unsigned char first = static_cast<unsigned char>(*u0);
    return (first >= 'a' && first <= 'z') || (first >= 'A' && first <= 'Z') || first == '%' || first >= 0x80;
}

// Calls emit(begin, end) for each line of [text, end) at most `width` wide, breaking only at
// blanks and '\n' (see TextWrap). Widths add up word by word: one pass over the text.
template <class Emit>
void WrapLines(const char* text, const char* end, float width, Emit&& emit) {
    width = (std::max)(width, ImGui::GetFontSize());   // at least one character a line
    const char* s = text;
    for (;;) {
        const char* nl = static_cast<const char*>(std::memchr(s, '\n', size_t(end - s)));
        const char* pe = nl ? nl : end;   // this paragraph: [s, pe)
        const char* lineBegin = s;        // a paragraph keeps its leading blanks
        const char* lineEnd = s;
        const char* lastBegin = s;        // the last word on the line ...
        const char* beforeLast = nullptr; // ... and the end of the word before it (null: it is the first)
        float lineW = 0.0f;
        bool empty = true;
        const char* p = s;
        while (p < pe) {
            const char* w0 = p;
            while (w0 < pe && IsBlank(*w0)) ++w0;
            if (w0 == pe) break;
            const char* w1 = w0;
            while (w1 < pe && !IsBlank(*w1)) ++w1;
            float add = Measure(empty ? lineBegin : lineEnd, w1);   // the blanks before the word, and the word
            if (!empty && lineW + add > width) {
                if (beforeLast && NumberAndUnit(lastBegin, lineEnd, w0, w1) &&
                    Measure(lastBegin, w1) <= width) {
                    emit(lineBegin, beforeLast);   // the number moves down with its unit
                    lineBegin = lastBegin;
                    lineW = Measure(lastBegin, lineEnd);
                    beforeLast = nullptr;
                } else {
                    emit(lineBegin, lineEnd);
                    lineBegin = w0;
                    empty = true;
                    add = Measure(w0, w1);
                }
            }
            if (empty && add > width) {
                // One word wider than a whole line: cut it where ImGui would, a line at a time.
                const char* q = lineBegin;
                while (Measure(q, w1) > width) {
                    const char* cut = ImGui::GetFont()->CalcWordWrapPosition(ImGui::GetFontSize(), q, w1, width);
                    if (cut <= q) {
                        unsigned int c = 0;
                        cut = q + (std::max)(1, ImTextCharFromUtf8(&c, q, w1));
                    }
                    emit(q, cut);
                    q = cut;
                }
                lineBegin = q;
                add = Measure(q, w1);
            }
            beforeLast = empty ? nullptr : lineEnd;
            lastBegin = empty ? lineBegin : w0;
            lineW = (empty ? 0.0f : lineW) + add;
            lineEnd = w1;
            empty = false;
            p = w1;
        }
        emit(lineBegin, empty ? lineBegin : lineEnd);   // the paragraph's last line ("" for an empty one)
        if (!nl) break;
        s = nl + 1;
    }
}

struct Line {
    const char* b;
    const char* e;
};

// Draws wrapped lines at `pos` and returns the block's size; `measureOnly` skips the drawing.
ImVec2 DrawWrapped(ImDrawList* dl, ImVec2 pos, ImU32 color, const char* text, float width, bool measureOnly) {
    const char* end = text + std::strlen(text);
    const float lh = ImGui::GetFontSize();
    float maxW = 0.0f;
    int n = 0;
    const ImVec4 clip = dl->_ClipRectStack.Size ? dl->_ClipRectStack.back() : ImVec4(-FLT_MAX, -FLT_MAX, FLT_MAX, FLT_MAX);
    WrapLines(text, end, width, [&](const char* b, const char* e) {
        const float y = pos.y + lh * float(n++);
        maxW = (std::max)(maxW, Measure(b, e));
        if (!measureOnly && b != e && y + lh >= clip.y && y <= clip.w)
            dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(pos.x, y), color, b, e);
    });
    return ImVec2(maxW, lh * float(n));
}

}  // namespace

bool IsLightTheme() { return Luminance(ImGui::GetStyle().Colors[ImGuiCol_WindowBg]) > 0.5f; }

// [snippet:style]
// The ONLY colours this plugin defines itself. Everything else comes from the
// app's theme through ImGui::GetStyle(), so the panel follows the theme; these
// carry a meaning (ok / warning / error), so each has a variant that stays
// readable on a light and on a dark background.
ImVec4 ToneColor(Tone t) {
    const bool light = IsLightTheme();
    switch (t) {
        case Tone::Ok:     return light ? ImVec4(0.10f, 0.50f, 0.22f, 1.0f) : ImVec4(0.42f, 0.84f, 0.50f, 1.0f);
        case Tone::Info:   return light ? ImVec4(0.10f, 0.38f, 0.80f, 1.0f) : ImVec4(0.50f, 0.74f, 1.00f, 1.0f);
        case Tone::Warn:   return light ? ImVec4(0.66f, 0.40f, 0.00f, 1.0f) : ImVec4(1.00f, 0.76f, 0.32f, 1.0f);
        case Tone::Error:  return light ? ImVec4(0.76f, 0.12f, 0.12f, 1.0f) : ImVec4(1.00f, 0.45f, 0.45f, 1.0f);
        case Tone::Accent: return ImGui::GetStyle().Colors[ImGuiCol_SliderGrab];  // the theme's primary colour
        case Tone::Neutral:
        default:           return ImGui::GetStyle().Colors[ImGuiCol_Text];
    }
}
// [/snippet]

ImU32 ToneU32(Tone t, float alpha) { return ToU32(ToneColor(t), alpha); }

const char* ToneIcon(Tone t) {
    switch (t) {
        case Tone::Ok:     return ICON_FA_CIRCLE_CHECK;
        case Tone::Info:   return ICON_FA_CIRCLE_INFO;
        case Tone::Warn:   return ICON_FA_TRIANGLE_EXCLAMATION;
        case Tone::Error:  return ICON_FA_CIRCLE_XMARK;
        case Tone::Accent: return ICON_FA_STAR;
        case Tone::Neutral:
        default:           return ICON_FA_CIRCLE_INFO;
    }
}

ImVec4 RarityColor(uint8_t rarity) {
    const bool light = IsLightTheme();
    switch (rarity) {
        case 1:  return light ? ImVec4(0.22f, 0.30f, 0.85f, 1.0f) : ImVec4(0.56f, 0.62f, 1.00f, 1.0f);   // magic
        case 2:  return light ? ImVec4(0.58f, 0.48f, 0.00f, 1.0f) : ImVec4(1.00f, 0.92f, 0.45f, 1.0f);   // rare
        case 3:  return light ? ImVec4(0.66f, 0.33f, 0.08f, 1.0f) : ImVec4(0.93f, 0.56f, 0.26f, 1.0f);   // unique
        default: return ImGui::GetStyle().Colors[ImGuiCol_Text];                                          // normal
    }
}

float Em(float n) { return ImGui::GetFontSize() * n; }

float ButtonWidth(const char* label) {
    return ImGui::CalcTextSize(label, nullptr, true).x + ImGui::GetStyle().FramePadding.x * 2.0f;
}

Flow::Flow() : m_right(ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x) {}

void Flow::Next(float itemWidth) {
    if (m_first) {
        m_first = false;
        return;
    }
    // GetItemRectMax is the previous item's right edge: stay on its line when this one fits.
    const float x = ImGui::GetItemRectMax().x + ImGui::GetStyle().ItemSpacing.x;
    if (x + itemWidth <= m_right + 0.5f) ImGui::SameLine();
}

void PageTitle(const char* icon, const char* title) {
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.4f);   // 1.92: same font, bigger size
    ImGui::PushStyleColor(ImGuiCol_Text, ToneColor(Tone::Accent));
    ImGui::TextUnformatted(icon);
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::TextUnformatted(title);
    ImGui::PopFont();
}

float IconBoxWidth() { return Em(1.4f); }

void SectionTitle(const char* icon, const char* text) {
    const ImGuiStyle& st = ImGui::GetStyle();
    ImGui::Spacing();
    const float lh = ImGui::GetTextLineHeight(), padY = st.SeparatorTextPadding.y;
    const float avail = ImGui::GetContentRegionAvail().x;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(avail, lh + padY * 2.0f));
    if (!ImGui::IsItemVisible()) return;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float y = p.y + padY;
    // The icon on the content edge, like every other line of the page; the box only keeps the
    // titles at one x, whatever the icon's own width.
    const float box = (std::max)(IconBoxWidth(), ImGui::CalcTextSize(icon).x);
    dl->AddText(ImVec2(p.x, y), ToneU32(Tone::Accent), icon);
    const float tx = p.x + box + st.ItemInnerSpacing.x;
    dl->AddText(ImVec2(tx, y), ImGui::GetColorU32(ImGuiCol_Text), text);
    const float lx = tx + ImGui::CalcTextSize(text).x + st.ItemSpacing.x;
    const float ly = IM_TRUNC(y + lh * 0.5f) + 0.5f;
    if (lx < p.x + avail)
        dl->AddLine(ImVec2(lx, ly), ImVec2(p.x + avail, ly), ImGui::GetColorU32(ImGuiCol_Separator),
                    std::clamp(st.SeparatorTextBorderSize, 1.0f, 2.0f));
}

void TextWrap(const char* text, const ImVec4* color) {
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (window->SkipItems) return;
    // Like ImGui::TextEx: honour the line's text baseline (AlignTextToFramePadding, SameLine).
    const ImVec2 pos(window->DC.CursorPos.x, window->DC.CursorPos.y + window->DC.CurrLineTextBaseOffset);
    const ImU32 col = color ? ImGui::ColorConvertFloat4ToU32(*color) : ImGui::GetColorU32(ImGuiCol_Text);
    const ImVec2 size = DrawWrapped(window->DrawList, pos, col, text, ImGui::GetContentRegionAvail().x, false);
    ImGui::ItemSize(size, 0.0f);
    ImGui::ItemAdd(ImRect(pos, ImVec2(pos.x + size.x, pos.y + size.y)), 0);
}

void TextDim(const char* fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    TextWrap(buf, &ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
}

void Note(const char* text) {
    const ImVec4& dim = ImGui::GetStyle().Colors[ImGuiCol_TextDisabled];
    ImGui::TextColored(dim, "%s", ICON_FA_CIRCLE_INFO);
    ImGui::SameLine();
    TextWrap(text, &dim);   // the wrapped block stays to the right of the icon
}

void BulletWrapped(const char* fmt, ...) {
    ImGui::Bullet();   // keeps the cursor on the same line
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    TextWrap(buf);
}

void TextRight(const char* fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    const float w = ImGui::CalcTextSize(buf).x, avail = ImGui::GetContentRegionAvail().x;
    if (avail > w) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + avail - w);
    ImGui::TextUnformatted(buf);
}

void HelpMarker(const char* text) {
    ImGui::TextDisabled("%s", ICON_FA_CIRCLE_QUESTION);
    if (ImGui::BeginItemTooltip()) {
        ImGui::PushTextWrapPos(Em(24.0f));
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

bool BeginCard(const char* id) {
    const ImGuiStyle& st = ImGui::GetStyle();
    ImGui::PushStyleColor(ImGuiCol_ChildBg, Dimmed(st.Colors[ImGuiCol_FrameBg], 0.55f));
    ImGui::PushStyleColor(ImGuiCol_Border, Dimmed(st.Colors[ImGuiCol_Border], 0.6f));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, (std::max)(st.FrameRounding, 4.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.0f);
    const bool open = ImGui::BeginChild(id, ImVec2(0.0f, 0.0f),
                                        ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding |
                                            ImGuiChildFlags_Borders);
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
    return open;
}

void EndCard() { ImGui::EndChild(); }

namespace {

// One key column for every BeginKv / BeginForm of a group (BeginAlignedGroup): the widest of
// the previous frame. Only the panel uses key / value tables, so one group at a time is enough.
struct AlignedGroup {
    ImGuiID id = 0;
    int frame = -1;      // the frame BeginAlignedGroup ran in; any other frame: no group
    bool open = false;
    float last = 0.0f;   // the widest key column of the previous frame
    float now = 0.0f;    // ... and of this one, so far
};
AlignedGroup g_group;

float WidestLabel(std::initializer_list<const char*> labels) {
    float w = 0.0f;
    for (const char* l : labels) w = (std::max)(w, ImGui::CalcTextSize(l, nullptr, true).x);
    return w;
}

bool BeginTwoColumns(const char* id, float keyWidth) {
    if (g_group.open && g_group.frame == ImGui::GetFrameCount()) {
        g_group.now = (std::max)(g_group.now, keyWidth);
        // Never more than 45 % of the width for the floor: a narrow side table keeps its values.
        keyWidth = (std::max)(keyWidth, (std::min)(g_group.last, ImGui::GetContentRegionAvail().x * 0.45f));
    }
    if (!ImGui::BeginTable(id, 2, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoSavedSettings)) return false;
    ImGui::TableSetupColumn("key", ImGuiTableColumnFlags_WidthFixed, keyWidth);
    ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch);
    return true;
}

}  // namespace

bool BeginKv(const char* id, std::initializer_list<const char*> labels) {
    return BeginTwoColumns(id, WidestLabel(labels));
}

void BeginAlignedGroup(const char* id) {
    const ImGuiID h = ImHashStr(id);
    g_group.last = h == g_group.id ? g_group.now : 0.0f;   // a new group starts from scratch
    g_group.id = h;
    g_group.now = 0.0f;
    g_group.frame = ImGui::GetFrameCount();
    g_group.open = true;
}

void EndAlignedGroup() { g_group.open = false; }

void KvKey(const char* label) {
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::TextDisabled("%s", label);
    ImGui::TableSetColumnIndex(1);
}

void KvKeyChips(const char* label) {
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + ChipPadY());   // on the chips' text line
    ImGui::TextDisabled("%s", label);
    ImGui::TableSetColumnIndex(1);
}

void KvText(const char* label, const char* fmt, ...) {
    KvKey(label);
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    TextWrap(buf);
}

void KvTone(const char* label, Tone tone, const char* fmt, ...) {
    KvKey(label);
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    const ImVec4 col = ToneColor(tone);
    TextWrap(buf, &col);
}

void EndKv() { ImGui::EndTable(); }

bool BeginForm(const char* id, std::initializer_list<const char*> labels) {
    // Room for the label, a gap and the (?) marker, measured, never guessed.
    const float marker = ImGui::CalcTextSize(ICON_FA_CIRCLE_QUESTION).x + ImGui::GetStyle().ItemSpacing.x;
    return BeginTwoColumns(id, WidestLabel(labels) + marker);
}

void FormRow(const char* label, const char* help) {
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::AlignTextToFramePadding();   // the label sits on the widget's text baseline
    ImGui::TextUnformatted(label);
    if (help) {
        ImGui::SameLine();
        HelpMarker(help);
    }
    ImGui::TableSetColumnIndex(1);
    ImGui::SetNextItemWidth(-FLT_MIN);  // the widget fills the value column
}

void EndForm() { ImGui::EndTable(); }

void TableHeadersRowAligned(std::initializer_list<bool> rightAligned) {
    const ImGuiTable* table = ImGui::GetCurrentTable();
    ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
    int column = 0;
    for (const bool right : rightAligned) {
        if (ImGui::TableSetColumnIndex(column)) {
            const char* name = ImGui::TableGetColumnName(column);
            ImGui::PushID(column);
            // TableHeader draws its label at the cursor: move the cursor, the cell stays whole. A
            // sortable column keeps room for its arrow at the right, as TableHeader does, or the
            // label would be cut to "...".
            if (right) {
                const bool sortable = table && (table->Flags & ImGuiTableFlags_Sortable) &&
                                      !(ImGui::TableGetColumnFlags(column) & ImGuiTableColumnFlags_NoSort);
                const float arrow = sortable ? IM_TRUNC(ImGui::GetFontSize() * 0.65f + ImGui::GetStyle().FramePadding.x) : 0.0f;
                const float w = ImGui::CalcTextSize(name, nullptr, true).x;
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (std::max)(0.0f, ImGui::GetContentRegionAvail().x - w - arrow));
            }
            ImGui::TableHeader(name);
            ImGui::PopID();
        }
        ++column;
    }
}

float ChipWidth(const char* text) {
    return ImGui::CalcTextSize(text, nullptr, true).x + ImGui::GetStyle().FramePadding.x * 1.6f;
}

float ChipPadY() { return (std::max)(Em(0.11f), ImGui::GetStyle().FramePadding.y * 0.5f); }

namespace {

bool ChipImpl(const char* text, const ImVec4& color, float fillAlpha, const char* tooltip) {
    const ImGuiStyle& st = ImGui::GetStyle();
    const ImVec2 textSize = ImGui::CalcTextSize(text, nullptr, true);
    const ImVec2 pad(st.FramePadding.x * 0.8f, ChipPadY());
    const ImVec2 size(textSize.x + pad.x * 2.0f, textSize.y + pad.y * 2.0f);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::Dummy(size);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float r = size.y * 0.5f;
    dl->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), ToU32(color, fillAlpha), r);
    dl->AddRect(p, ImVec2(p.x + size.x, p.y + size.y), ToU32(color, 0.40f), r);
    const char* end = ImGui::FindRenderedTextEnd(text);
    dl->AddText(ImVec2(p.x + pad.x, p.y + pad.y), ToU32(color, 1.0f), text, end);
    const bool hovered = ImGui::IsItemHovered();
    if (hovered && tooltip && ImGui::BeginTooltip()) {
        ImGui::PushTextWrapPos(Em(24.0f));
        ImGui::TextUnformatted(tooltip);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
    return hovered;
}

}  // namespace

bool Chip(const char* text, Tone tone, const char* tooltip) {
    return ChipImpl(text, ToneColor(tone), tone == Tone::Neutral ? 0.08f : 0.16f, tooltip);
}

bool Chip(const char* text, const ImVec4& color, const char* tooltip) { return ChipImpl(text, color, 0.16f, tooltip); }

void StatusLine(const Status& status) {
    const ImGuiStyle& st = ImGui::GetStyle();
    const float width = ImGui::GetContentRegionAvail().x;
    const ImVec2 pad(st.FramePadding.x + Em(0.22f), st.FramePadding.y + Em(0.11f));
    const char* icon = ToneIcon(status.tone);
    const float iconW = ImGui::CalcTextSize(icon).x;
    const float textX = pad.x + iconW + st.ItemSpacing.x;
    const float wrapW = (std::max)(Em(4.0f), width - textX - pad.x);
    const char* text = status.text.empty() ? "(no status)" : status.text.c_str();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const ImVec2 textSize = DrawWrapped(dl, ImVec2(0.0f, 0.0f), 0, text, wrapW, true);
    const float height = (std::max)(textSize.y, ImGui::GetTextLineHeight()) + pad.y * 2.0f;

    ImGui::Dummy(ImVec2(width, height));
    if (!ImGui::IsItemVisible()) return;
    const ImVec2 q(p.x + width, p.y + height);
    const float rounding = (std::max)(st.FrameRounding, 3.0f);
    dl->AddRectFilled(p, q, ToneU32(status.tone, status.tone == Tone::Neutral ? 0.08f : 0.14f), rounding);
    const float bar = (std::max)(2.0f, IM_TRUNC(Em(0.17f)));   // the accent bar at the left edge
    dl->AddRectFilled(p, ImVec2(p.x + bar, q.y), ToneU32(status.tone, 0.9f), rounding, ImDrawFlags_RoundCornersLeft);
    dl->AddText(ImVec2(p.x + pad.x, p.y + pad.y), ToneU32(status.tone), icon);
    DrawWrapped(dl, ImVec2(p.x + textX, p.y + pad.y), ImGui::GetColorU32(ImGuiCol_Text), text, wrapW, false);
}

void Meter(float fraction, ImU32 fill, const char* label, const char* value, float reserved, const char* note,
           float width) {
    const ImGuiStyle& st = ImGui::GetStyle();
    const float w = width > 0.0f ? width : ImGui::GetContentRegionAvail().x;
    const float h = ImGui::GetFrameHeight();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(w, h));
    if (!ImGui::IsItemVisible()) return;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 q(p.x + w, p.y + h);
    const float rounding = st.FrameRounding;
    dl->AddRectFilled(p, q, ImGui::GetColorU32(ImGuiCol_FrameBg), rounding);

    const float f = std::clamp(fraction, 0.0f, 1.0f);
    const float r = std::clamp(reserved, 0.0f, 1.0f);
    const float freeEnd = q.x - w * r;   // where the reserved part starts
    if (f > 0.0f) dl->AddRectFilled(p, ImVec2(p.x + w * f, q.y), fill, rounding);
    if (r > 0.0f) {
        // Reserved capacity: hatched at the right end, clipped to its own rectangle.
        const ImVec2 r0(freeEnd, p.y);
        dl->PushClipRect(r0, q, true);
        dl->AddRectFilled(r0, q, ImGui::GetColorU32(ImGuiCol_Text, 0.10f), rounding);
        for (float x = r0.x - h; x < q.x; x += (std::max)(4.0f, Em(0.33f)))
            dl->AddLine(ImVec2(x, q.y), ImVec2(x + h, p.y), ImGui::GetColorU32(ImGuiCol_Text, 0.30f), 1.0f);
        dl->PopClipRect();
    }
    if (st.FrameBorderSize > 0.0f) dl->AddRect(p, q, ImGui::GetColorU32(ImGuiCol_Border), rounding);

    // An outline only where the text colour alone would not read: on a light fill in a dark theme,
    // on a dark fill in a light one. A dark enough fill keeps the digits crisp.
    const ImVec4 text = st.Colors[ImGuiCol_Text], bg = st.Colors[ImGuiCol_FrameBg];
    const ImVec4 fc = ImGui::ColorConvertU32ToFloat4(fill);
    const ImVec4 under(fc.x * fc.w + bg.x * (1.0f - fc.w), fc.y * fc.w + bg.y * (1.0f - fc.w),
                       fc.z * fc.w + bg.z * (1.0f - fc.w), 1.0f);
    const float lt = Luminance(text);
    const bool outline = (f > 0.0f && std::fabs(lt - Luminance(under)) < 0.45f) || std::fabs(lt - Luminance(bg)) < 0.45f;
    const ImU32 outlineCol = IsLightTheme() ? IM_COL32(255, 255, 255, 170) : IM_COL32(0, 0, 0, 170);
    const float ty = IM_TRUNC(p.y + (h - ImGui::GetTextLineHeight()) * 0.5f), pad = IM_TRUNC(Em(0.4f));
    auto put = [&](float x, const char* s, ImU32 col) {
        x = IM_TRUNC(x);
        if (outline) {
            static constexpr float kOffsets[4][2] = { { -1, 0 }, { 1, 0 }, { 0, -1 }, { 0, 1 } };
            for (const auto& o : kOffsets) dl->AddText(ImVec2(x + o[0], ty + o[1]), outlineCol, s);
        }
        dl->AddText(ImVec2(x, ty), col, s);
    };
    const bool hasLabel = label && *label, hasValue = value && *value;
    const float lw = hasLabel ? ImGui::CalcTextSize(label).x : 0.0f, vw = hasValue ? ImGui::CalcTextSize(value).x : 0.0f;
    // The value ends where the free part ends; with no room there, at the bar's end.
    float vx = freeEnd - pad - vw;
    if (hasValue && vx < p.x + pad + lw + pad) vx = q.x - pad - vw;
    const bool showLabel = hasLabel && (!hasValue || p.x + pad + lw + pad <= vx);   // the value wins a tie
    if (showLabel) put(p.x + pad, label, ImGui::GetColorU32(ImGuiCol_Text));
    if (hasValue) put((std::max)(vx, p.x + pad), value, ImGui::GetColorU32(ImGuiCol_Text));
    if (note && *note && r > 0.0f) {
        const float nw = ImGui::CalcTextSize(note).x, nx = q.x - pad - nw;
        const float taken = hasValue ? vx + vw : (showLabel ? p.x + pad + lw : p.x);
        if (nx >= freeEnd + pad * 0.5f && nx >= taken + pad) put(nx, note, ImGui::GetColorU32(ImGuiCol_Text, 0.85f));
    }
}

void UsageBar(float fraction, float width, bool budget) {
    const float w = width > 0.0f ? width : ImGui::GetContentRegionAvail().x;
    const float h = (std::max)(3.0f, IM_TRUNC(Em(0.2f)));
    // Tucked under the value above it: half the usual item gap.
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() - ImGui::GetStyle().ItemSpacing.y * 0.5f);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(w, h));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), ImGui::GetColorU32(ImGuiCol_FrameBg), h * 0.5f);
    const float f = std::clamp(fraction, 0.0f, 1.0f);
    if (f <= 0.0f) return;
    const Tone tone = !budget ? Tone::Accent : (f > 0.9f ? Tone::Error : (f > 0.6f ? Tone::Warn : Tone::Ok));
    dl->AddRectFilled(p, ImVec2(p.x + (std::max)(h, w * f), p.y + h), ToneU32(tone, 0.85f), h * 0.5f);
}

void CodeBlock(const char* id, const char* text, int maxLines) {
    const ImGuiStyle& st = ImGui::GetStyle();
    int lines = 1;
    for (const char* c = text; *c; ++c)
        if (*c == '\n') ++lines;
    const int shown = (std::min)(lines, (std::max)(1, maxLines));
    const ImVec2 textSize = ImGui::CalcTextSize(text);
    const float avail = ImGui::GetContentRegionAvail().x;
    const bool scrollsX = textSize.x + st.FramePadding.x * 2.0f > avail;
    const float height = float(shown) * ImGui::GetTextLineHeight() + st.FramePadding.y * 2.0f +
                         (scrollsX ? st.ScrollbarSize : 0.0f) + (lines > shown ? 0.0f : 2.0f);
    ImGui::BeginChild(id, ImVec2(0.0f, height), ImGuiChildFlags_FrameStyle, ImGuiWindowFlags_HorizontalScrollbar);
    ImGui::TextUnformatted(text);
    ImGui::EndChild();
}

}  // namespace ui
