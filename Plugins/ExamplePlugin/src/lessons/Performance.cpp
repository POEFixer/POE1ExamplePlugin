// Lesson "performance": what this plugin costs, every budget the app enforces, and a stress slider.
#include <algorithm>
#include <bit>
#include <cmath>

#include "lessons/Common.h"   // brings <windows.h> (through the SDK) before <psapi.h> needs it

#include <psapi.h>            // K32GetProcessMemoryInfo lives in kernel32: no extra library to link

namespace ex {

namespace {

constexpr int kStressMax = 5000;

constexpr LessonInfo kInfo{
    .id = "performance",
    .group = Group::Advanced,
    .icon = ICON_FA_GAUGE_HIGH,
    .title = "Performance & limits",
    .summary = "What this plugin costs, and every limit the app enforces, side by side.",
    .what = "The app measures each plugin (its telemetry line in the Plugins tab) and caps it: CPU and memory "
            "by the Job object, frames by count and size, its share of the app's own service thread.",
    .when = "Before shipping, and whenever an overlay looks cut: draw what you need, with few large draw "
            "calls, and do heavy work on a thread of your own.",
    .limits = "Per overlay frame 150,000 vertices, 300,000 indices, 8,192 commands; the app draws at most 1,024 "
              "commands and 6 screens of fill. Textures 1,024 / 64 MiB. Overlay at most maxFps, panel at most 60 "
              "fps while shown. 30 ms a second of the app's service thread. CPU: 25 % of the machine, on a "
              "quarter of the logical processors. Memory: memoryLimitMB from plugin.json (512 here).",
    .snippet = "performance",
};

uint64_t U64(const FILETIME& t) { return (uint64_t(t.dwHighDateTime) << 32) | t.dwLowDateTime; }

// What this process looks like from the inside: CPU time, memory, and its Job's limits.
struct ProcessSample {
    float cpuPercent = 0.0f;               // of one logical processor, over the last second
    uint64_t workingSet = 0, privateBytes = 0;
    uint64_t memoryLimit = 0;              // the Job's cap (0 = could not query)
    uint32_t cpuRatePercent = 0;           // the Job's CPU rate cap (0 = could not query)
    int cpus = 0, systemCpus = 0;          // affinity: the CPUs this process may run on
};

// [snippet:performance]
// The stress test: `count` small hexagons over the screen, in ONE draw call
// (same texture, same clip rectangle) while the window's draw list stays under
// 65,536 vertices (16-bit indices; past that a new command starts). 6 points
// each: 12 vertices and 48 indices with anti-aliasing, so 5000 stay under the
// overlay's 150,000 vertices and 300,000 indices. Big overlapping shapes would
// cost fill instead.
void DrawStress(ImDrawList* dl, ImVec2 client, int count, float now) {
    const ImVec2 c(client.x * 0.5f, client.y * 0.5f);
    const float spread = (std::min)(client.x, client.y) * 0.46f / std::sqrt(float(kStressMax));
    for (int i = 0; i < count; ++i) {
        const float a = float(i) * 2.39996323f + now * 0.2f;      // the golden angle: an even spiral
        const float r = spread * std::sqrt(float(i));
        const ImVec2 p(c.x + std::cos(a) * r, c.y + std::sin(a) * r);
        dl->AddCircleFilled(p, 3.0f, IM_COL32(120, 200, 255, 160), 6);
    }
}
// [/snippet]

class PerformanceLesson final : public Lesson {
public:
    PerformanceLesson() : Lesson(kInfo) {}

    ui::Status GetStatus(const App& app, const P1Frame& f) const override {
        const float share = CallbackShare(app, f.now_us);
        return Live("callbacks use about %.1f %% of one CPU; the last overlay frame had %s vertices.", double(share),
                    FormatCount(uint64_t(app.overlayMetrics.vertices)).c_str());
    }

    void Tick(App& app, const P1Frame& f) override {
        if (app.IsShown(this, f) && f.now_us - m_sampleUs >= 1'000'000) Sample(f.now_us);
    }

    void Overlay(App& app, const P1Frame& f) override {
        const int count = std::clamp(app.stressMarkers, 0, kStressMax);   // bounded where it is used, too
        if (count <= 0) return;
        DrawStress(ImGui::GetWindowDrawList(), ImVec2(float(f.viewport.client_w), float(f.viewport.client_h)), count,
                   float(double(f.now_us % 60'000'000) / 1e6));
    }

    void Unload(App& app) override { app.stressMarkers = 0; }

    void Draw(App& app, const P1Frame& f) override {
        DrawCosts(app, f);
        DrawBudgets(app, f);
        DrawStressControl(app, f);
    }

private:
    static float CallbackShare(const App& app, int64_t nowUs) {
        float us = 0.0f;
        for (Cb c : { Cb::Event, Cb::Tick, Cb::Overlay, Cb::Panel }) us += app.Stats(c).avgUs * app.Stats(c).RateAt(nowUs);
        return us / 1e6f * 100.0f;   // microseconds per second -> percent of one CPU
    }

    void Sample(int64_t nowUs) {
        FILETIME created{}, exited{}, kernel{}, user{};
        if (GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) {
            const uint64_t cpu = U64(kernel) + U64(user);   // 100 ns units, all threads
            if (m_sampleUs && nowUs > m_sampleUs)
                m_proc.cpuPercent = float(double(cpu - m_cpu) / 10.0 / double(nowUs - m_sampleUs) * 100.0);
            m_cpu = cpu;
        }
        PROCESS_MEMORY_COUNTERS_EX pmc{};
        pmc.cb = sizeof pmc;
        if (K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof pmc)) {
            m_proc.workingSet = pmc.WorkingSetSize;
            m_proc.privateBytes = pmc.PrivateUsage;
        }
        // A null job handle means "the job this process is in": the sandbox's own limits, read live.
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
        if (QueryInformationJobObject(nullptr, JobObjectExtendedLimitInformation, &li, sizeof li, nullptr)) {
            if (li.BasicLimitInformation.LimitFlags & JOB_OBJECT_LIMIT_PROCESS_MEMORY) m_proc.memoryLimit = li.ProcessMemoryLimit;
            else if (li.BasicLimitInformation.LimitFlags & JOB_OBJECT_LIMIT_JOB_MEMORY) m_proc.memoryLimit = li.JobMemoryLimit;
        }
        JOBOBJECT_CPU_RATE_CONTROL_INFORMATION rate{};
        if (QueryInformationJobObject(nullptr, JobObjectCpuRateControlInformation, &rate, sizeof rate, nullptr) &&
            (rate.ControlFlags & JOB_OBJECT_CPU_RATE_CONTROL_ENABLE))
            m_proc.cpuRatePercent = rate.CpuRate / 100;   // CpuRate is in 1/100 of a percent
        DWORD_PTR mine = 0, system = 0;
        if (GetProcessAffinityMask(GetCurrentProcess(), &mine, &system)) {
            m_proc.cpus = std::popcount(uint64_t(mine));
            m_proc.systemCpus = std::popcount(uint64_t(system));
        }
        m_sampleUs = nowUs;
    }

    static void DrawCosts(const App& app, const P1Frame& f) {
        ui::SectionTitle(ICON_FA_STOPWATCH, "This plugin, measured from the inside");
        if (ui::BeginKv("costs", { "OnTick", "OnOverlay", "OnPanel", "callbacks in all", "last overlay frame",
                                   "last panel frame" })) {
            auto cost = [&](const char* name, Cb c) {
                const CallbackStats& s = app.Stats(c);
                ui::KvText(name, "%s on average, %s at most, %.0f a second", FormatCostUs(s.avgUs).c_str(),
                           FormatCostUs(s.maxUs).c_str(), double(s.RateAt(f.now_us)));
            };
            cost("OnTick", Cb::Tick);
            cost("OnOverlay", Cb::Overlay);
            cost("OnPanel", Cb::Panel);
            ui::KvText("callbacks in all", "about %.2f %% of one CPU", double(CallbackShare(app, f.now_us)));
            auto frame = [](const char* name, const FrameMetrics& m) {
                ui::KvText(name, "%s vertices, %s indices, %d window%s, %d draw command%s",
                           FormatCount(uint64_t(m.vertices)).c_str(), FormatCount(uint64_t(m.indices)).c_str(), m.windows,
                           m.windows == 1 ? "" : "s", m.commands, m.commands == 1 ? "" : "s");
            };
            frame("last overlay frame", app.overlayMetrics);
            frame("last panel frame", app.panelMetrics);
            ui::EndKv();
        }
        ui::Note("Vertices, indices and windows are ImGui's own counts (ImGuiIO::Metrics*) of the PREVIOUS frame of "
                 "the same context. Draw commands are counted at the end of this callback over every window of the "
                 "context (child windows, cards, popups, tooltips) and the foreground and background lists.");
    }

    void DrawBudgets(const App& app, const P1Frame& f) const {
        ui::SectionTitle(ICON_FA_SCALE_BALANCED, "Every budget, and where this plugin stands");
        const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                                      ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoSavedSettings;
        if (!ImGui::BeginTable("budgets", 3, flags)) return;
        ImGui::TableSetupColumn("Budget", ImGuiTableColumnFlags_WidthStretch, 1.1f);
        ImGui::TableSetupColumn("Limit", ImGuiTableColumnFlags_WidthStretch, 1.4f);
        ImGui::TableSetupColumn("Now", ImGuiTableColumnFlags_WidthStretch, 1.3f);
        ImGui::TableHeadersRow();
        // `used` < 0: nothing to measure against; otherwise a bar under the value shows the share.
        // A rate (`rate`): running near its cap is the goal, so its bar carries no warning colour.
        auto row = [](const char* budget, const char* limit, const std::string& now, float used = -1.0f,
                      ui::Tone tone = ui::Tone::Neutral, bool rate = false) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ui::TextWrap(budget);
            ImGui::TableNextColumn();
            ui::TextWrap(limit);
            ImGui::TableNextColumn();
            const ImVec4 col = ui::ToneColor(tone);
            ui::TextWrap(now.c_str(), &col);
            if (used >= 0.0f) ui::UsageBar(used, 0.0f, !rate);
        };
        const FrameMetrics& o = app.overlayMetrics;
        // (Not "near": <windows.h> defines that word away as a macro.)
        auto close = [](double used, double limit) { return used > limit * 0.8 ? ui::Tone::Warn : ui::Tone::Neutral; };
        auto share = [](double used, double limit) { return limit > 0.0 ? float(used / limit) : 0.0f; };
        row("Overlay vertices / frame", "150,000 (more: the rest of the frame is cut)",
            FormatCount(uint64_t(o.vertices)), share(o.vertices, 150000), close(o.vertices, 150000));
        row("Overlay indices / frame", "300,000", FormatCount(uint64_t(o.indices)), share(o.indices, 300000),
            close(o.indices, 300000));
        row("Draw commands / frame", "8,192 from the host; the app draws the first 1,024",
            Fmt("%d (every window)", o.commands), share(o.commands, 1024), close(o.commands, 1024));
        row("Fill / frame", "6 screens (summed triangle area); 8 for all plugins together", "not measured here");
        row("Panel vertices / frame", "75,000", FormatCount(uint64_t(app.panelMetrics.vertices)),
            share(app.panelMetrics.vertices, 75000), close(app.panelMetrics.vertices, 75000));
        const double overlayRate = app.Stats(Cb::Overlay).RateAt(f.now_us), panelRate = app.Stats(Cb::Panel).RateAt(f.now_us);
        row("Overlay frames", "at most maxFps (60 here); fewer can mean throttled frames (see below)",
            Fmt("%.0f a second", overlayRate), share(overlayRate, 60), ui::Tone::Neutral, true);
        row("Panel frames", "at most 60 a second, only while shown", Fmt("%.0f a second", panelRate), share(panelRate, 60),
            ui::Tone::Neutral, true);
        row("Textures", "1,024 live and 64 MiB, font atlases included",
            Fmt("%d, %s (own)", app.textures.LiveCount(), FormatBytes(app.textures.LiveBytes()).c_str()),
            (std::max)(share(app.textures.LiveCount(), 1024), share(double(app.textures.LiveBytes()), double(64u << 20))));
        row("App service thread", "30 ms per second for this plugin (100 ms for all); over it, frames, textures "
            "and requests wait for the next second",
            "measured by the app: a \"throttled\" line in this plugin's log, and its telemetry line in the Plugins tab");
        const float slowest = (std::max)({ app.Stats(Cb::Tick).maxUs, app.Stats(Cb::Overlay).maxUs, app.Stats(Cb::Panel).maxUs });
        row("One callback", "5 s, then the plugin is killed", Fmt("slowest so far: %s", FormatCostUs(slowest).c_str()),
            share(slowest, 5e6));
        // The Job's cap is a share of the whole machine: 25 % of 32 CPUs is 8 CPUs' worth.
        const double capCpus = double(m_proc.cpuRatePercent) / 100.0 * double(m_proc.systemCpus);
        row("CPU", "25 % of the whole machine, on the top quarter of the logical CPUs",
            m_sampleUs ? Fmt("%.1f %% of one CPU; %d of %d CPUs; Job cap %s", double(m_proc.cpuPercent), m_proc.cpus,
                             m_proc.systemCpus, m_proc.cpuRatePercent ? Fmt("%u %%", m_proc.cpuRatePercent).c_str() : "unreadable")
                       : std::string("sampling..."),
            m_sampleUs && capCpus > 0.0 ? share(m_proc.cpuPercent / 100.0, capCpus) : -1.0f);
        row("Memory", "memoryLimitMB in plugin.json (512 here); over it allocations fail",
            m_sampleUs ? Fmt("working set %s, private %s; Job cap %s", FormatBytes(m_proc.workingSet).c_str(),
                             FormatBytes(m_proc.privateBytes).c_str(),
                             m_proc.memoryLimit ? FormatBytes(m_proc.memoryLimit).c_str() : "unreadable")
                       : std::string("sampling..."),
            m_sampleUs && m_proc.memoryLimit ? share(double(m_proc.privateBytes), double(m_proc.memoryLimit)) : -1.0f);
        ImGui::EndTable();
    }

    static void DrawStressControl(App& app, const P1Frame& f) {
        ui::SectionTitle(ICON_FA_FIRE, "Stress the overlay");
        if (ui::BeginForm("stress", { "Markers" })) {
            ui::FormRow("Markers", "Not saved: the overlay is back to normal after a restart.");
            // AlwaysClamp: Ctrl+click lets the user type any number; this one sizes an overlay loop.
            ImGui::SliderInt("##stress", &app.stressMarkers, 0, kStressMax, app.stressMarkers ? "%d hexagons" : "off",
                             ImGuiSliderFlags_AlwaysClamp);
            ui::EndForm();
        }
        if (app.stressMarkers > 0 && !(f.overlay_enabled && f.viewport.valid))
            ui::StatusLine({ ui::Tone::Info, "The overlay is not drawn right now (no game window), so the counts do not move." });
        ui::Note("Watch \"last overlay frame\" above: each hexagon adds 12 vertices and 48 indices, all in one "
                 "draw command while the window's draw list stays under 65,536 vertices.");
    }

    ProcessSample m_proc;
    uint64_t m_cpu = 0;
    int64_t m_sampleUs = 0;
};

}  // namespace

std::unique_ptr<Lesson> MakePerformanceLesson() { return std::make_unique<PerformanceLesson>(); }

}  // namespace ex
