// Lesson "lifecycle": which callback runs when, how often, and what it costs.
#include <algorithm>
#include <string>

#include "JsonUtil.h"
#include "lessons/Common.h"

namespace ex {

// Declared in App.h. Every callback of Plugin.cpp is wrapped in a CallbackTimer, which
// calls this when the callback returns (or throws).
void CallbackStats::Record(int64_t startUs, int64_t endUs) noexcept {
    const float us = float(endUs > startUs ? endUs - startUs : 0);
    lastUs = us;
    // An exponential moving average follows recent costs without storing a history.
    avgUs = calls == 0 ? us : avgUs + (us - avgUs) * 0.1f;
    maxUs = (std::max)(maxUs, us);
    ++calls;
    lastAtUs = endUs;
    if (m_windowStartUs == 0) m_windowStartUs = startUs;
    ++m_windowCalls;
    const int64_t span = endUs - m_windowStartUs;
    if (span >= 1'000'000) {
        perSecond = float(m_windowCalls) * 1e6f / float(span);
        m_windowStartUs = endUs;
        m_windowCalls = 0;
        m_hadWindow = true;
    } else if (!m_hadWindow && span >= 300'000) {
        perSecond = float(m_windowCalls) * 1e6f / float(span);   // an early estimate until the first full second
    }
}

namespace {

// [snippet:lifecycle]
// A whole plugin, every callback in the order the host calls it. It uses only
// the SDK and ImGui: copy it into your .cpp and end that file with
//   P1_PLUGIN(MinimalPlugin, "my_id", "My plugin", "1.0.0", "Me", "What it does")
// (it is compiled here, so it cannot rot, but not exported: one DLL is one plugin,
// and this DLL's P1_PLUGIN names ExamplePlugin in Plugin.cpp).
class MinimalPlugin final : public p1::Plugin {
public:
    bool OnLoad() override {                       // once, first; false = refuse to run
        ImTextInitClassifiers();                   // word wrap in this DLL's ImGui copy
        p1::Log(P1_LOG_INFO, "loaded; app language %s", HostApi().language);
        return true;
    }
    void OnEvent(const P1Event& e) override {      // before the OnTick of the same turn
        if (e.type == P1_EV_AREA_CHANGED) m_newArea = true;   // no frame here: note it
    }
    void OnTick(const P1Frame& f) override {       // game logic, no ImGui
        const P1Snapshot& s = *f.snapshot;         // gone when the callback returns:
        if (m_newArea && (s.flags & P1_SNAP_ATTACHED)) {   // COPY what you keep
            m_area = p1::Str(s, s.area.name);
            m_newArea = false;
        }
        ++m_ticks;
    }
    void OnOverlay(const P1Frame&) override {      // ImGui over the game, client pixels
        ImGui::GetWindowDrawList()->AddText(ImVec2(16.0f, 16.0f), IM_COL32_WHITE, m_area.c_str());
    }
    void OnPanel(const P1Frame&) override {        // ImGui in the Plugins tab, while shown
        ImGui::Text("%llu ticks; area: %s", m_ticks, m_area.c_str());
    }
    void OnUnload() override {}                    // last: stop threads (bounded), save
private:
    bool m_newArea = true;
    std::string m_area;
    unsigned long long m_ticks = 0;
};
// [/snippet]

constexpr LessonInfo kInfo{
    .id = "lifecycle",
    .group = Group::Basics,
    .icon = ICON_FA_ARROWS_ROTATE,
    .title = "Lifecycle",
    .summary = "Which callback runs when, how often, and what each one costs.",
    .what = "OnLoad once, then OnEvent, OnTick, OnOverlay and OnPanel on the host's single plugin thread, "
            "then OnUnload once. The process is terminated right after OnUnload.",
    .when = "Game logic and copies of snapshot data in OnTick; ImGui only in OnOverlay and OnPanel; load "
            "settings in OnLoad, save them in OnUnload; slow work on a thread of your own.",
    .limits = "A callback that runs over 5 s (a loading step over 15 s) gets the plugin killed. OnTick: on "
              "every new snapshot, at least 10 times a second. OnOverlay: at most maxFps (60 here). "
              "OnPanel: only while the panel is shown, at most 60 fps. OnUnload: 1.5 s (about 1.3 s, shared "
              "by all plugins, when the app exits), then the process is killed.",
    .snippet = "lifecycle",
};

struct CallbackRow {
    const char* name;
    Cb cb;
    const char* when;
};

constexpr CallbackRow kRows[] = {
    { "OnLoad", Cb::Load, "Once, before anything else. Return false to refuse to run." },
    { "OnEvent", Cb::Event, "Once per event, before the OnTick of the same iteration." },
    { "OnTick", Cb::Tick, "On every new snapshot (about 30 a second), and at least 10 times a second." },
    { "OnOverlay", Cb::Overlay, "On new data, a resize, or the rate asked for with p1::RequestFrames; at most maxFps." },
    { "OnPanel", Cb::Panel, "Only while this panel is visible: about 30 a second idle, up to 60 while used." },
};

class LifecycleLesson final : public Lesson {
public:
    LifecycleLesson() : Lesson(kInfo) {}

    ui::Status GetStatus(const App& app, const P1Frame& f) const override {
        const bool overlay = f.overlay_enabled && f.viewport.valid;
        return Live("%llu ticks so far; the panel runs at %.0f fps; the overlay %s.",
                    static_cast<unsigned long long>(app.Stats(Cb::Tick).calls),
                    double(app.Stats(Cb::Panel).RateAt(f.now_us)),
                    overlay ? Fmt("at %.0f fps", double(app.Stats(Cb::Overlay).RateAt(f.now_us))).c_str()
                            : "is not drawn (no game window, or the overlay is off)");
    }

    void Draw(App& app, const P1Frame& f) override {
        DrawOrder();
        DrawTable(app, f);
        DrawFrame(f);
        DrawFrameRequest(app, f);
    }

    void Load(App& app, const nlohmann::json& j) override {
        app.userFps = static_cast<uint32_t>(js::Int(j, "requestFps", 0, 0, 60));
    }
    void Save(const App& app, nlohmann::json& j) const override { j["requestFps"] = app.userFps; }

private:
    static void DrawOrder() {
        ui::SectionTitle(ICON_FA_ROUTE, "One turn of the plugin thread");
        ui::Flow flow;
        const char* steps[] = { "OnEvent × n", ICON_FA_ARROW_RIGHT, "OnTick", ICON_FA_ARROW_RIGHT,
                                "OnOverlay", ICON_FA_ARROW_RIGHT, "OnPanel" };
        for (const char* s : steps) {
            const bool arrow = s[0] != 'O';
            flow.Next(arrow ? ImGui::CalcTextSize(s).x : ui::ChipWidth(s));
            if (arrow) {
                ImGui::TextDisabled("%s", s);
            } else {
                ui::Chip(s, ui::Tone::Accent);
            }
        }
        ui::Note("OnOverlay and OnPanel run only when a frame is due. The loop sleeps until the app wakes it "
                 "(a new snapshot, input, a control change) and never longer than 250 ms.");
    }

    static void DrawTable(App& app, const P1Frame& f) {
        ui::SectionTitle(ICON_FA_STOPWATCH, "Callbacks of this plugin");
        const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                                      ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoSavedSettings;
        if (ImGui::BeginTable("callbacks", 6, flags)) {
            ImGui::TableSetupColumn("Callback", ImGuiTableColumnFlags_WidthStretch, 1.6f);
            ImGui::TableSetupColumn("Calls", ImGuiTableColumnFlags_WidthStretch, 1.0f);
            ImGui::TableSetupColumn("Per s", ImGuiTableColumnFlags_WidthStretch, 0.8f);
            ImGui::TableSetupColumn("Last", ImGuiTableColumnFlags_WidthStretch, 1.0f);
            ImGui::TableSetupColumn("Average", ImGuiTableColumnFlags_WidthStretch, 1.0f);
            ImGui::TableSetupColumn("Max", ImGuiTableColumnFlags_WidthStretch, 1.0f);
            ui::TableHeadersRowAligned({ false, true, true, true, true, true });   // numbers line up on the right
            for (const CallbackRow& r : kRows) {
                const CallbackStats& s = app.Stats(r.cb);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(r.name);
                ImGui::SetItemTooltip("%s", r.when);
                ImGui::TableNextColumn();
                ui::TextRight("%s", FormatCount(s.calls).c_str());
                ImGui::TableNextColumn();
                ui::TextRight("%.0f", double(s.RateAt(f.now_us)));
                ImGui::TableNextColumn();
                ui::TextRight("%s", FormatCostUs(s.lastUs).c_str());
                ImGui::TableNextColumn();
                ui::TextRight("%s", FormatCostUs(s.avgUs).c_str());
                ImGui::TableNextColumn();
                ui::TextRight("%s", FormatCostUs(s.maxUs).c_str());
            }
            ImGui::EndTable();
        }
        if (ImGui::Button(ICON_FA_ROTATE "  Reset counters")) {
            for (Cb c : { Cb::Event, Cb::Tick, Cb::Overlay, Cb::Panel }) app.Stats(c).Reset();
        }
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ui::TextDim("OnUnload is not listed: nothing runs after it.");
    }

    static void DrawFrame(const P1Frame& f) {
        ui::SectionTitle(ICON_FA_CAMERA, "The P1Frame of this panel frame");
        if (ui::BeginKv("frame", { "delta_s", "now_us", "panel_w x panel_h", "panel_visible", "overlay_enabled",
                                   "permissions", "input_allowed" })) {
            ui::KvText("delta_s", "%.4f s since the previous OnPanel", double(f.delta_s));
            ui::KvText("now_us", "%lld (steady clock, the same as p1::NowUs and the snapshot stamps)",
                       static_cast<long long>(f.now_us));
            ui::KvText("panel_w x panel_h", "%.0f x %.0f px (0 x 0 while hidden)", double(f.panel_w), double(f.panel_h));
            ui::KvText("panel_visible", "%u", f.panel_visible);
            ui::KvText("overlay_enabled", "%u (the app shows this plugin's overlay frames)", f.overlay_enabled);
            ui::KvText("permissions", "%s (effective now; input needs the user's consent too)",
                       PermissionsText(f.permissions).c_str());
            ui::KvText("input_allowed", "%u (%s)", f.input_allowed,
                       f.input_allowed ? "the app would take an input request right now"
                                       : "the app would refuse an input request right now");
            ui::EndKv();
        }
    }

    static void DrawFrameRequest(App& app, const P1Frame& f) {
        ui::SectionTitle(ICON_FA_GAUGE_HIGH, "Overlay frame rate");
        ui::Note("p1::RequestFrames(fps) asks for overlay frames between snapshots, for things that move. "
                 "0 = a frame only when new data arrives. The manifest's maxFps (60) caps it, and building "
                 "frames faster than the app takes them only spends your own CPU share.");
        if (ui::BeginForm("rate", { "RequestFrames", "Requested now", "Observed" })) {
            ui::FormRow("RequestFrames", "Saved in settings.json. Animated overlay layers ask for at least 20: snapshots bring ~30 "
                                           "frames a second in game, so 20 only fills the gaps.");
            int fps = static_cast<int>(app.userFps);
            if (ImGui::SliderInt("##fps", &fps, 0, 60, fps == 0 ? "0 (on new data)" : "%d fps", ImGuiSliderFlags_AlwaysClamp)) {
                app.userFps = static_cast<uint32_t>(fps);
                app.MarkDirty();
            }
            ui::KvText("Requested now", "%u fps: %s", app.requestedFps, app.requestReason);
            ui::KvText("Observed", "%.0f overlay frames a second", double(app.Stats(Cb::Overlay).RateAt(f.now_us)));
            ui::EndForm();
        }
    }
};

}  // namespace

std::unique_ptr<Lesson> MakeLifecycleLesson() { return std::make_unique<LifecycleLesson>(); }

}  // namespace ex
