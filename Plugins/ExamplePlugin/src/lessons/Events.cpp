// Lesson "events": every P1_EV_* decoded, counted, and the last 50 with their age.
#include <algorithm>

#include "lessons/Common.h"

namespace ex {

namespace {

constexpr LessonInfo kInfo{
    .id = "events",
    .group = Group::Basics,
    .icon = ICON_FA_BOLT,
    .title = "Events",
    .summary = "What changed since the last tick: attach, game state, area, character, focus, panel, consent.",
    .what = "OnEvent(const P1Event&) gets a type (P1_EV_*) and two numbers, a and b, whose meaning depends on "
            "the type. The host derives events by comparing the app's controls and each new snapshot with "
            "the previous ones.",
    .when = "React to a change once instead of polling for it: clear per-area caches on AREA_CHANGED, stop "
            "sending input on INPUT_GATE 0, re-read the language on LANGUAGE.",
    .limits = "Events arrive before the OnTick of the same iteration, in a fixed order. OnEvent has no "
              "frame: note the change and read the snapshot in the OnTick that follows. A newer app may "
              "add types: ignore unknown ones (a default: in every switch).",
    .snippet = "events",
};

struct TypeRow {
    uint32_t type;
    const char* payload;
    const char* meaning;
};

constexpr TypeRow kTypes[] = {
    { P1_EV_ATTACHED, "a = pid", "The app attached to a game process." },
    { P1_EV_DETACHED, "a = old pid", "The game process went away, or another one replaced it." },
    { P1_EV_STATE_CHANGED, "a = old, b = new", "Game state (P1_STATE_*). The first one starts from Not loaded." },
    { P1_EV_AREA_CHANGED, "a = counter", "A new area or instance; snapshot->area already describes it." },
    { P1_EV_CHARACTER_CHANGED, "", "The local player's name changed to another non-empty name." },
    { P1_EV_PANEL_SHOWN, "", "This panel became visible in the Plugins tab." },
    { P1_EV_PANEL_HIDDEN, "", "This panel was hidden." },
    { P1_EV_PERMISSIONS, "a = P1_PERM_*", "The user turned input consent on or off (also once at start when consent is off)." },
    { P1_EV_LANGUAGE, "", "The app's UI language changed: re-read HostApi().language." },
    { P1_EV_GAME_FOCUS, "a = 1 / 0", "The game window came to the front, or lost it." },
    { P1_EV_INPUT_GATE, "a = 1 / 0", "The app now accepts, or refuses, input requests." },
    { P1_EV_TERRAIN_CHANGED, "a = serial", "A new area terrain reached P1Frame.terrain (\"radar\" permission)." },
};

// [snippet:events]
// One line per event. The switch has a default: a newer app may send types
// this plugin does not know, and those must be ignored, never treated as errors.
std::string DescribeEvent(const P1Event& e) {
    switch (e.type) {
        case P1_EV_ATTACHED:          return Fmt("attached to the game, pid %lld", static_cast<long long>(e.a));
        case P1_EV_DETACHED:          return Fmt("game process %lld went away", static_cast<long long>(e.a));
        case P1_EV_STATE_CHANGED:     return Fmt("%s -> %s", StateName(int32_t(e.a)), StateName(int32_t(e.b)));
        case P1_EV_AREA_CHANGED:      return Fmt("new area, change counter %lld", static_cast<long long>(e.a));
        case P1_EV_CHARACTER_CHANGED: return "another character";
        case P1_EV_PANEL_SHOWN:       return "panel shown";
        case P1_EV_PANEL_HIDDEN:      return "panel hidden";
        case P1_EV_PERMISSIONS:       return "permissions now: " + PermissionsText(uint32_t(e.a));
        case P1_EV_LANGUAGE:          return "the app's language changed";
        case P1_EV_GAME_FOCUS:        return e.a ? "the game came to the front" : "the game lost the front";
        case P1_EV_INPUT_GATE:        return e.a ? "input requests accepted" : "input requests refused";
        case P1_EV_TERRAIN_CHANGED:   return Fmt("a new area terrain, serial %lld", static_cast<long long>(e.a));
        default:                      return Fmt("unknown type %u (a newer app): ignored", e.type);
    }
}
// [/snippet]

class EventsLesson final : public Lesson {
public:
    EventsLesson() : Lesson(kInfo) {}

    ui::Status GetStatus(const App& app, const P1Frame& f) const override {
        if (app.events.empty())
            return Reason(ui::Tone::Info, "No events yet. They come on attach, a game state or area change, "
                                          "focus changes and when this panel is shown or hidden.");
        const EventRecord& last = app.events.back();
        return Live("%llu events since the plugin started; the last, %s, %s ago.",
                    static_cast<unsigned long long>(app.eventsTotal), EventName(last.type),
                    FormatSpanUs(f.now_us - last.atUs).c_str());
    }

    void Draw(App& app, const P1Frame& f) override {
        DrawTypes(app);
        DrawLog(app, f);
    }

private:
    static void DrawTypes(const App& app) {
        ui::SectionTitle(ICON_FA_LIST, "Event types");
        const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                                      ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoSavedSettings;
        // Fixed-fit columns sit as close as their text: half an em on each side keeps them apart.
        ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(ui::Em(0.5f), ImGui::GetStyle().CellPadding.y));
        const bool open = ImGui::BeginTable("types", 4, flags);
        ImGui::PopStyleVar();   // a table keeps the padding it was begun with
        if (open) {
            ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableSetupColumn("Count", ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableSetupColumn("a, b", ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableSetupColumn("Meaning", ImGuiTableColumnFlags_WidthStretch);
            ui::TableHeadersRowAligned({ false, true, false, false });
            for (const TypeRow& t : kTypes) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(EventName(t.type));
                ImGui::TableNextColumn();
                const uint64_t n = app.eventCounts[EventSlot(t.type)];
                ImGui::PushStyleColor(ImGuiCol_Text, n ? ui::ToneColor(ui::Tone::Accent)
                                                       : ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
                ui::TextRight("%llu", static_cast<unsigned long long>(n));
                ImGui::PopStyleColor();
                ImGui::TableNextColumn();
                ImGui::TextDisabled("%s", t.payload[0] ? t.payload : "-");   // what a and b carry
                ImGui::TableNextColumn();
                ui::TextWrap(t.meaning);
            }
            ImGui::EndTable();
        }
        if (app.eventCounts[0]) ImGui::TextDisabled("Unknown types seen: %llu", static_cast<unsigned long long>(app.eventCounts[0]));
    }

    static void DrawLog(App& app, const P1Frame& f) {
        ui::SectionTitle(ICON_FA_CLOCK, "The last 50, newest first");
        if (app.events.empty()) {
            ui::TextDim("Nothing yet. Hide and show this panel to see PANEL_HIDDEN / PANEL_SHOWN.");
            return;
        }
        // As tall as its rows, up to ten; more scroll inside it. Three columns: age, type, what it says.
        const ImGuiStyle& st = ImGui::GetStyle();
        const float rowH = ImGui::GetTextLineHeight() + st.CellPadding.y * 2.0f;
        const float shown = float((std::min)(app.events.size(), size_t(10)));
        const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit |
                                      ImGuiTableFlags_NoSavedSettings | ImGuiTableFlags_BordersOuter;
        if (ImGui::BeginTable("log", 3, flags, ImVec2(0.0f, rowH * shown + 2.0f))) {
            ImGui::TableSetupColumn("ago", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("888.8 ms ago").x);
            ImGui::TableSetupColumn("type", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("CHARACTER_CHANGED").x);
            ImGui::TableSetupColumn("what", ImGuiTableColumnFlags_WidthStretch);
            for (auto it = app.events.rbegin(); it != app.events.rend(); ++it) {
                const P1Event e{ sizeof(P1Event), it->type, it->a, it->b };
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextDisabled("%s ago", FormatSpanUs(f.now_us - it->atUs).c_str());
                ImGui::TableNextColumn();
                ImGui::TextColored(ui::ToneColor(ui::Tone::Accent), "%s", EventName(it->type));
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(DescribeEvent(e).c_str());
            }
            ImGui::EndTable();
        }
        if (ImGui::Button(ICON_FA_TRASH "  Clear the log")) app.events.clear();
    }
};

}  // namespace

std::unique_ptr<Lesson> MakeEventsLesson() { return std::make_unique<EventsLesson>(); }

}  // namespace ex
