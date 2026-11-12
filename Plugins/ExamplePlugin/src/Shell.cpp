#include "Shell.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "Names.h"
#include "Snippets.h"
#include "Util.h"
#include "lessons/Lesson.h"
#include "ui/Ui.h"

namespace ex {

namespace {

constexpr int64_t kCopiedNoticeUs = 3'000'000;

// One chip of the status strip, with a shorter text for a panel too narrow for the full one.
// `order`: when the strip does not fit, chips go short in this order (1 first); 0 = one text.
struct StripChip {
    std::string full, compact;
    ui::Tone tone;
    const char* tooltip;
    int order;
};

// One line of chips: what is true right now, before any lesson is read.
void StatusStrip(const App& app, const P1Frame& f) {
    const P1Snapshot& s = *f.snapshot;
    std::vector<StripChip> chips;
    auto add = [&](std::string full, std::string compact, ui::Tone tone, const char* tooltip, int order) {
        chips.push_back({ std::move(full), std::move(compact), tone, tooltip, order });
    };
    auto same = [&](const std::string& text, ui::Tone tone, const char* tooltip) { add(text, text, tone, tooltip, 0); };

    if (s.flags == 0)
        same(ICON_FA_PLUG "  no game data", ui::Tone::Warn,
             "The snapshot's flags are 0: the app is not attached to the game yet.");
    else if (s.flags & P1_SNAP_WGC_MODE)
        same(ICON_FA_EYE "  WGC mode", ui::Tone::Info,
             "The app is in visual (WGC) mode: it reads no game memory, so the snapshot has no game data.");
    else if (!(s.flags & P1_SNAP_ATTACHED))
        same(ICON_FA_PLUG "  not attached", ui::Tone::Warn, "P1_SNAP_ATTACHED is off.");
    else
        same(Fmt(ICON_FA_GAMEPAD "  %s", StateName(s.game_state)), (s.flags & P1_SNAP_IN_GAME) ? ui::Tone::Ok : ui::Tone::Info,
             "snapshot->game_state (P1_STATE_*). P1_SNAP_IN_GAME is set while it is In game.");

    if (s.publish_us > 0) {
        const int64_t age = f.now_us - s.publish_us;
        const bool fresh = age < (std::max)(int64_t(250'000), int64_t(s.worker_period_ms * 3000.0f));
        add(Fmt(ICON_FA_CLOCK "  snapshot %s old · worker %.0f ms", FormatSpanUs(age).c_str(), double(s.worker_period_ms)),
            Fmt(ICON_FA_CLOCK "  %s old", FormatSpanUs(age).c_str()), fresh ? ui::Tone::Neutral : ui::Tone::Warn,
            "How old the snapshot is (now_us - publish_us), and the worker: the app's game reader reads the game "
            "every worker_period_ms, about 33 ms, about 17 ms while the app's radar runs.",
            3);
    }

    const bool overlayShown = f.overlay_enabled && f.viewport.valid;
    add(overlayShown ? ICON_FA_LAYER_GROUP "  overlay on" : ICON_FA_LAYER_GROUP "  overlay off",
        overlayShown ? ICON_FA_LAYER_GROUP "  overlay" : ICON_FA_LAYER_GROUP "  no overlay",
        overlayShown ? ui::Tone::Ok : ui::Tone::Neutral,
        "P1Frame::overlay_enabled (the plugin is enabled and its manifest has \"overlay\": true) and "
        "viewport.valid (the app knows the game window). OnOverlay runs only while both hold.",
        1);

    const bool memory = p1::HasPermission(f, P1_PERM_MEMORY);
    same(memory ? ICON_FA_MEMORY "  memory" : ICON_FA_MEMORY "  no memory", memory ? ui::Tone::Ok : ui::Tone::Neutral,
         "The \"memory\" permission (plugin.json): read-only game memory reads through p1::Memory.");

    const P1HostApi* host = p1::Host();
    const bool asked = host && (host->permissions & P1_PERM_INPUT);
    const bool consent = p1::HasPermission(f, P1_PERM_INPUT);
    if (!asked)
        same(ICON_FA_COMPUTER_MOUSE "  no input", ui::Tone::Neutral, "plugin.json does not ask for \"input\".");
    else if (!consent)
        add(ICON_FA_COMPUTER_MOUSE "  input: consent off", ICON_FA_COMPUTER_MOUSE "  input off", ui::Tone::Warn,
            "plugin.json asks for \"input\", but \"Allow input\" is off for this plugin in the Plugins tab.", 2);
    else
        add(f.input_allowed ? ICON_FA_COMPUTER_MOUSE "  input: open" : ICON_FA_COMPUTER_MOUSE "  input: gate closed",
            f.input_allowed ? ICON_FA_COMPUTER_MOUSE "  input open" : ICON_FA_COMPUTER_MOUSE "  input closed",
            f.input_allowed ? ui::Tone::Ok : ui::Tone::Info,
            "P1Frame::input_allowed: the app takes input requests only while the game window is in front "
            "and no trade or recovery automation runs. While you use this panel the app is in front.", 2);

    same(Fmt(ICON_FA_LANGUAGE "  %s", host && host->language ? host->language : "?"), ui::Tone::Neutral,
         "HostApi().language: the app's UI language. P1_EV_LANGUAGE says when it changes.");

    // Each value keeps its own unit once one of them crosses a millisecond, so 7 µs never reads as
    // 0.01 ms; while all three are below it, one "µs" at the end is enough.
    const float tick = app.Stats(Cb::Tick).avgUs, overlay = app.Stats(Cb::Overlay).avgUs, panel = app.Stats(Cb::Panel).avgUs;
    const bool allMicro = tick < 999.5f && overlay < 999.5f && panel < 999.5f;
    add(allMicro ? Fmt(ICON_FA_GAUGE_HIGH "  tick %.0f · overlay %.0f · panel %.0f µs", double(tick), double(overlay),
                       double(panel))
                 : Fmt(ICON_FA_GAUGE_HIGH "  tick %s · overlay %s · panel %s", FormatCostUs(tick).c_str(),
                       FormatCostUs(overlay).c_str(), FormatCostUs(panel).c_str()),
        allMicro ? Fmt(ICON_FA_GAUGE_HIGH "  t/o/p %.0f/%.0f/%.0f µs", double(tick), double(overlay), double(panel))
                 : Fmt(ICON_FA_GAUGE_HIGH "  t/o/p %s/%s/%s", FormatCostUs(tick).c_str(), FormatCostUs(overlay).c_str(),
                       FormatCostUs(panel).c_str()),
        ui::Tone::Neutral, "t/o/p in the short form. This plugin's average cost (a moving average) of OnTick, "
        "OnOverlay and OnPanel.", 4);

    // The full texts when they fit on one line; else the chips go short one by one, in their
    // order, until the line fits (and only when even the shortest do not fit, it wraps).
    const float gap = ImGui::GetStyle().ItemSpacing.x, avail = ImGui::GetContentRegionAvail().x;
    int level = 0;
    for (; level <= 4; ++level) {
        float total = -gap;
        for (const StripChip& c : chips)
            total += ui::ChipWidth((c.order && c.order <= level ? c.compact : c.full).c_str()) + gap;
        if (total <= avail) break;
    }
    ui::Flow flow;
    for (const StripChip& c : chips) {
        const char* text = (c.order && c.order <= level ? c.compact : c.full).c_str();
        flow.Next(ui::ChipWidth(text));
        ui::Chip(text, c.tone, c.tooltip);
    }
}

float NavWidth(const App& app, float avail) {
    const ImGuiStyle& st = ImGui::GetStyle();
    float widest = 0.0f;
    for (const auto& l : app.lessons) widest = (std::max)(widest, ImGui::CalcTextSize(l->Info().title).x);
    const float want = ui::IconBoxWidth() + st.ItemInnerSpacing.x + widest + st.FramePadding.x * 2.0f +
                       st.WindowPadding.x * 2.0f + st.ScrollbarSize;
    return std::clamp(want, ui::Em(8.0f), avail * 0.36f);
}

void Nav(App& app, float width, bool compact) {
    ImGui::BeginChild("##nav", ImVec2(width, 0.0f), ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding);
    const ImGuiStyle& st = ImGui::GetStyle();
    // A tight list: rows a sixth of an em apart, so 23 lessons and 5 group headers fit a 720 px
    // panel whole (the 24th scrolls into view when it is the open one). The highlight of a
    // Selectable still covers the gap, so rows read as one list.
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(st.ItemSpacing.x, (std::max)(2.0f, std::floor(ui::Em(0.17f)))));
    // Rows are clipped to the padded area: a scrolled row never shows half of itself in the
    // padding along the top or bottom border.
    const ImVec2 wp = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();
    ImGui::PushClipRect(ImVec2(wp.x, wp.y + st.WindowPadding.y), ImVec2(wp.x + ws.x, wp.y + ws.y - st.WindowPadding.y), true);
    const float box = ui::IconBoxWidth(), gap = st.ItemInnerSpacing.x;
    int group = -1;
    float groupTop = 0.0f, selTop = -1.0f, selBottom = 0.0f, selGroupTop = 0.0f;   // window-local y
    std::vector<float> rowTops;   // every header's and row's top: the places a scroll may stop at
    for (const auto& l : app.lessons) {
        const LessonInfo& in = l->Info();
        if (static_cast<int>(in.group) != group) {
            if (group >= 0) ImGui::Dummy(ImVec2(0.0f, std::floor(ui::Em(0.3f))));   // air between groups
            groupTop = ImGui::GetCursorPosY();
            rowTops.push_back(groupTop);
            if (compact && group >= 0) ImGui::Separator();
            else if (!compact) ImGui::TextDisabled("%s", GroupName(in.group));
            group = static_cast<int>(in.group);
        }
        const bool selected = app.lessonId == in.id;
        rowTops.push_back(ImGui::GetCursorPosY());
        // The icon centred in a box of one width and every title at one x, whatever the icon.
        const ImVec2 p = ImGui::GetCursorScreenPos();
        if (ImGui::Selectable(Fmt("##%s", in.id).c_str(), selected)) app.Select(in.id);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImU32 text = ImGui::GetColorU32(ImGuiCol_Text);
        const float iconW = ImGui::CalcTextSize(in.icon).x;
        dl->AddText(ImVec2(std::floor(p.x + (box - iconW) * 0.5f), p.y), text, in.icon);
        if (!compact) dl->AddText(ImVec2(p.x + box + gap, p.y), text, in.title);
        if (compact) ImGui::SetItemTooltip("%s", in.title);
        if (selected) {
            const float base = ImGui::GetScrollY() - ImGui::GetWindowPos().y;   // screen y -> window-local y
            selTop = ImGui::GetItemRectMin().y + base;
            selBottom = ImGui::GetItemRectMax().y + base;
            selGroupTop = groupTop;
        }
    }
    // A list that scrolls gets one row of room after the last lesson, so it can stop with a row
    // start at the top edge even when the last lesson is open.
    if (ImGui::GetCursorPosY() + st.WindowPadding.y > ImGui::GetWindowHeight())
        ImGui::Dummy(ImVec2(0.0f, ImGui::GetTextLineHeight()));
    // After a load, and when the panel is shown again: scroll only if the open lesson is not whole
    // in view. A scroll stops only where a header or a row starts at the top edge, so no row is
    // ever cut in half there: the lesson's group header if that works, else the nearest row start
    // that keeps the open lesson whole and does not scroll past the end of the list.
    if (app.shell.scrollNavToSelected && selTop >= 0.0f) {
        const float padY = st.WindowPadding.y, viewTop = ImGui::GetScrollY(), viewH = ImGui::GetWindowHeight();
        // "Whole" inside the padded area: the clip above hides what reaches into the padding.
        if (selTop < viewTop + padY || selBottom > viewTop + viewH - padY) {
            const float listEnd = ImGui::GetCursorPosY() - ImGui::GetStyle().ItemSpacing.y;   // the last row's bottom
            const float maxScroll = (std::max)(0.0f, listEnd + padY - viewH);
            const float lower = selBottom + padY - viewH, upper = (std::min)(selTop - padY, maxScroll);
            const float header = selGroupTop - padY;
            float target = header;
            if (header < lower || header > upper) {
                // The group is taller than the view: the first stop that shows the lesson whole.
                // The group is at the end of the list: the last stop before the end.
                const bool tooHigh = header < lower;
                bool found = false;
                for (const float top : rowTops) {   // rows in list order: their tops only grow
                    const float t = top - padY;
                    if (t < lower - 0.5f || t > upper + 0.5f) continue;
                    target = t;
                    found = true;
                    if (tooHigh) break;
                }
                if (!found) target = std::clamp(header, (std::min)(lower, upper), upper);
            }
            ImGui::SetScrollY((std::max)(0.0f, target));
        }
        app.shell.scrollNavToSelected = false;
    }
    ImGui::PopClipRect();
    ImGui::PopStyleVar();
    ImGui::EndChild();
}

// "src/lessons/Events.cpp" -> "Events.cpp".
const char* FileName(const char* path) {
    const char* slash = std::strrchr(path, '/');
    return slash ? slash + 1 : path;
}

// A chip on the line of a frame-high item (a button), its text on the button's text line.
float ChipOffsetY() {
    return (std::max)(0.0f, std::floor((ImGui::GetFrameHeight() - ImGui::GetTextLineHeight()) * 0.5f - ui::ChipPadY()));
}

// Part 1, the title, with a compact copy of part 5 at the right end of its line: a newcomer
// sees at once that the lesson is one file, and one click shows its code.
void TitleRow(App& app, const LessonInfo& in) {
    ui::PageTitle(in.icon, in.title);
    const Snippet* sn = FindSnippet(in.snippet);
    if (!sn) return;
    const std::string chip = Fmt(ICON_FA_FILE_CODE "  %s:%d", FileName(sn->file), sn->line);
    const float titleH = ImGui::GetItemRectSize().y, w = ui::ChipWidth(chip.c_str());
    const float chipH = ImGui::GetTextLineHeight() + ui::ChipPadY() * 2.0f;
    ImGui::SameLine();
    const float avail = ImGui::GetContentRegionAvail().x;
    if (avail < w + ImGui::GetStyle().ItemSpacing.x) {   // no room: part 5 is at the bottom as always
        ImGui::NewLine();
        return;
    }
    ImGui::SetCursorPos(ImVec2(ImGui::GetCursorPosX() + avail - w,
                               ImGui::GetCursorPosY() + (std::max)(0.0f, std::floor((titleH - chipH) * 0.5f))));
    const std::string tip = Fmt("This lesson's code: %s:%d, %s. Click to show it at the bottom of the page, where "
                                "Copy snippet puts it on the clipboard.", sn->file, sn->line, sn->function);
    ui::Chip(chip.c_str(), ui::Tone::Neutral, tip.c_str());
    if (ImGui::IsItemClicked()) {
        app.shell.codeOpen = true;
        app.shell.scrollToSource = true;
    }
}

void WhatWhenLimits(const LessonInfo& in) {
    ui::BeginCard("##wwl");
    if (ui::BeginKv("wwl", { "What", "When", "Limits" })) {
        ui::KvText("What", "%s", in.what);
        ui::KvText("When", "%s", in.when);
        ui::KvText("Limits", "%s", in.limits);
        ui::EndKv();
    }
    ui::EndCard();
}

// Part 5 of every page: where the code is, and a copy of it.
void SourceRow(App& app, const LessonInfo& in, const P1Frame& f) {
    ImGui::Spacing();
    if (app.shell.scrollToSource) {   // the title's chip was clicked: this part at the top of the view
        ImGui::SetScrollHereY(0.0f);
        app.shell.scrollToSource = false;
    }
    ImGui::Separator();
    ImGui::Spacing();
    const Snippet* sn = FindSnippet(in.snippet);
    if (!sn) {
        ui::StatusLine({ ui::Tone::Error, Fmt("Snippet \"%s\" is missing: run tools/gen_snippets.py.", in.snippet) });
        return;
    }
    const ImGuiStyle& st = ImGui::GetStyle();
    const std::string where = Fmt(ICON_FA_FILE_CODE "  %s:%d · %s", sn->file, sn->line, sn->function);
    const char* copy = ICON_FA_COPY "  Copy snippet";
    const char* toggle = app.shell.codeOpen ? ICON_FA_EYE_SLASH "  Hide code" : ICON_FA_CODE "  Show code";
    // The chip and both buttons on one line when they fit; else the chip alone, then both buttons
    // together on the next line: never one button left alone on a line of its own.
    const float buttons = ui::ButtonWidth(copy) + st.ItemSpacing.x + ui::ButtonWidth(toggle);
    const bool oneLine = ui::ChipWidth(where.c_str()) + st.ItemSpacing.x + buttons <= ImGui::GetContentRegionAvail().x;
    // In a group with a spacer above it, so the chip's text sits on the buttons' text line while
    // the line itself still starts at the buttons' top.
    ImGui::BeginGroup();
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(st.ItemSpacing.x, 0.0f));
    if (oneLine) ImGui::Dummy(ImVec2(0.0f, ChipOffsetY()));
    ui::Chip(where.c_str(), ui::Tone::Neutral,
             "Where this lesson's code lives. The snippet is a verbatim excerpt of that code, copied into the DLL "
             "by tools/gen_snippets.py (--check fails when the copy is stale).");
    ImGui::PopStyleVar();
    ImGui::EndGroup();
    if (oneLine) ImGui::SameLine();
    ui::Flow flow;
    flow.Next(ui::ButtonWidth(copy));
    if (ImGui::Button(copy)) {
        // The app applies a clipboard request only while the user is using this panel: a click is.
        p1::SetClipboard(sn->text);
        app.shell.copiedUs = f.now_us;
        app.shell.copiedLesson = in.id;
    }
    flow.Next(ui::ButtonWidth(toggle));
    if (ImGui::Button(toggle)) app.shell.codeOpen = !app.shell.codeOpen;

    if (app.shell.copiedLesson == in.id && f.now_us - app.shell.copiedUs < kCopiedNoticeUs) {
        const char* done = ICON_FA_CHECK "  copy requested";
        flow.Next(ImGui::CalcTextSize(done).x);
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(ui::ToneColor(ui::Tone::Ok), "%s", done);
        ImGui::SetItemTooltip("The app puts it on the clipboard because you clicked in this panel "
                              "(at most once a second, never while it trades).");
    }
    if (app.shell.codeOpen) {
        ImGui::Spacing();
        ui::CodeBlock("##code", sn->text);
    }
}

void Page(App& app, Lesson& lesson, const P1Frame& f) {
    const LessonInfo& in = lesson.Info();
    ImGui::BeginChild("##page", ImVec2(0.0f, 0.0f), ImGuiChildFlags_AlwaysUseWindowPadding);
    if (app.shell.lastPage != in.id) {   // a newly opened lesson starts at its top
        ImGui::SetScrollY(0.0f);
        app.shell.lastPage = in.id;
    }
    TitleRow(app, in);
    ui::TextDim("%s", in.summary);
    ImGui::Spacing();
    WhatWhenLimits(in);
    ImGui::Spacing();
    ui::StatusLine(lesson.GetStatus(app, f));
    ImGui::Spacing();
    ImGui::PushID(in.id);
    ui::BeginAlignedGroup(in.id);   // one key column for every key / value table of the demo
    lesson.Draw(app, f);
    ui::EndAlignedGroup();
    ImGui::PopID();
    SourceRow(app, in, f);
    ImGui::Spacing();
    ImGui::EndChild();
}

}  // namespace

void DrawPanel(App& app, const P1Frame& f) {
    Lesson* lesson = app.Current();
    if (!lesson) {
        ui::StatusLine({ ui::Tone::Error, "No lessons were created (see this plugin's log)." });
        return;
    }
    StatusStrip(app, f);
    ImGui::Spacing();
    const float avail = ImGui::GetContentRegionAvail().x;
    // A narrow panel (or a large UI scale) gets an icon-only lesson list, so the page keeps its width.
    const bool compact = avail < ui::Em(38.0f);
    const ImGuiStyle& st = ImGui::GetStyle();
    const float navWidth = compact ? ui::IconBoxWidth() + (st.FramePadding.x + st.WindowPadding.x) * 2.0f
                                   : NavWidth(app, avail);
    Nav(app, navWidth, compact);
    ImGui::SameLine();
    Page(app, *lesson, f);
}

}  // namespace ex
