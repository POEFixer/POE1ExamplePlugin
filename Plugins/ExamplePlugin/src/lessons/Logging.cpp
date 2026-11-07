// Lesson "logging": log lines and toasts, and where each one ends up.
#include <array>

#include "lessons/Common.h"

namespace ex {

namespace {

constexpr int64_t kToastIntervalUs = 2'000'000;   // the app's limit: one toast per 2 s per plugin
constexpr int kBurstLines = 250;                   // more than the burst of 200, on purpose

constexpr LessonInfo kInfo{
    .id = "logging",
    .group = Group::Basics,
    .icon = ICON_FA_TERMINAL,
    .title = "Log & toasts",
    .summary = "p1::Log writes a line to this plugin's log; p1::Notify shows a toast in the app.",
    .what = "p1::Log(level, fmt, ...) is printf-style UTF-8. p1::Notify(level, title, text) shows a toast "
            "titled \"[SDK Example] <title>\", so it can never pass for one of the app's own.",
    .when = "Log changes, not frames. A toast is for something the user must notice now (an error, a "
            "finished long task); everything else belongs in the log or the panel.",
    .limits = "50 lines a second, bursts of 200; extra lines are dropped and counted. Every accepted line goes "
              "to this plugin's log view in the Plugins tab (the newest 200). Only warn and error lines are "
              "mirrored to the app's System Logs, at most about 1 line a second across ALL plugins. One "
              "toast per 2 s per plugin; title 128 bytes, text 512 bytes; a line is cut at about 2 KiB.",
    .snippet = "logging",
};

// [snippet:logging]
// A log line at each level, and a toast. Neither call blocks: both go into a
// ring the app drains, and the app applies the rate limits on its side.
void SendLine(int level) {
    p1::Log(level, "a %s line from the SDK Example (the time is %lld us)", LogLevelName(level),
            static_cast<long long>(p1::NowUs()));
}

void SendToast(int level) {
    p1::Notify(level, "Toast demo", "One toast per 2 s per plugin; the app drops the rest.");
}
// [/snippet]

class LoggingLesson final : public Lesson {
public:
    LoggingLesson() : Lesson(kInfo) {}

    ui::Status GetStatus(const App&, const P1Frame& f) const override {
        const int64_t wait = kToastIntervalUs - (f.now_us - m_lastToastUs);
        if (m_lines == 0 && m_toasts == 0)
            return Reason(ui::Tone::Info, "Nothing sent yet: press a button below, then open this plugin's log in "
                                          "the Plugins tab.");
        return Live("%llu lines and %llu toasts sent; %s.", static_cast<unsigned long long>(m_lines),
                    static_cast<unsigned long long>(m_toasts),
                    m_lastToastUs == 0 || wait <= 0 ? "the next toast would be shown"
                                                    : Fmt("a toast now would be dropped (wait %s)", FormatSpanUs(wait).c_str()).c_str());
    }

    void Draw(App&, const P1Frame& f) override {
        DrawLogButtons();
        DrawToastButtons(f);
        DrawWhereLinesGo();
    }

private:
    void DrawLogButtons() {
        ui::SectionTitle(ICON_FA_TERMINAL, "Log a line");
        ui::Flow flow;
        for (int level = P1_LOG_DEBUG; level <= P1_LOG_ERROR; ++level) {
            const std::string label = Fmt("%s##log%d", LogLevelName(level), level);
            flow.Next(ui::ButtonWidth(label.c_str()));
            if (ImGui::Button(label.c_str())) {
                SendLine(level);
                ++m_lines;
                ++m_byLevel[size_t(level)];
            }
        }
        const std::string burst = Fmt(ICON_FA_BOLT "  Send %d debug lines at once", kBurstLines);
        flow.Next(ui::ButtonWidth(burst.c_str()));
        if (ImGui::Button(burst.c_str())) {
            for (int i = 0; i < kBurstLines; ++i) p1::Log(P1_LOG_DEBUG, "burst line %d of %d", i + 1, kBurstLines);
            m_lines += kBurstLines;
            m_byLevel[P1_LOG_DEBUG] += kBurstLines;
        }
        ui::TextDim("Sent: debug %llu, info %llu, warn %llu, error %llu",
                            static_cast<unsigned long long>(m_byLevel[0]), static_cast<unsigned long long>(m_byLevel[1]),
                            static_cast<unsigned long long>(m_byLevel[2]), static_cast<unsigned long long>(m_byLevel[3]));
        ui::Note("The burst sends 250 lines in one click: about 200 are admitted, the rest are dropped and "
                 "counted as \"dropped log lines\" on this plugin's telemetry line in the Plugins tab.");
    }

    void DrawToastButtons(const P1Frame& f) {
        ui::SectionTitle(ICON_FA_BELL, "Show a toast");
        ui::Flow flow;
        for (int level = P1_NOTIFY_INFO; level <= P1_NOTIFY_ERROR; ++level) {
            const std::string label = Fmt("%s##toast%d", NotifyLevelName(level), level);
            flow.Next(ui::ButtonWidth(label.c_str()));
            if (ImGui::Button(label.c_str())) {
                if (m_lastToastUs != 0 && f.now_us - m_lastToastUs < kToastIntervalUs) ++m_toastsTooSoon;
                else m_lastToastUs = f.now_us;
                SendToast(level);
                ++m_toasts;
            }
        }
        // How long until the app takes the next toast: a meter that fills over 2 s.
        if (ui::BeginForm("toastwindow", { "Next toast" })) {
            ui::FormRow("Next toast", "The app shows one toast per 2 s per plugin and drops the others.");
            const int64_t since = f.now_us - m_lastToastUs;
            const float ready = m_lastToastUs == 0 ? 1.0f : float(since) / float(kToastIntervalUs);
            const bool open = ready >= 1.0f;
            const std::string text = open ? std::string("ready: the app would show it")
                                          : Fmt("dropped by the app for %s more", FormatSpanUs(kToastIntervalUs - since).c_str());
            ui::Meter(ready, ui::ToneU32(open ? ui::Tone::Ok : ui::Tone::Warn, 0.45f), text.c_str());
            ui::EndForm();
        }
        if (m_toastsTooSoon)
            ui::TextDim("%llu toasts were sent inside the 2 s window (the app dropped them).",
                                static_cast<unsigned long long>(m_toastsTooSoon));
    }

    static void DrawWhereLinesGo() {
        ui::SectionTitle(ICON_FA_ROUTE, "Where a line ends up");
        const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                                      ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoSavedSettings;
        if (ImGui::BeginTable("where", 3, flags)) {
            ImGui::TableSetupColumn("Level", ImGuiTableColumnFlags_WidthStretch, 0.7f);
            ImGui::TableSetupColumn("This plugin's log (Plugins tab)", ImGuiTableColumnFlags_WidthStretch, 1.4f);
            ImGui::TableSetupColumn("System Logs tab", ImGuiTableColumnFlags_WidthStretch, 1.4f);
            ImGui::TableHeadersRow();
            for (int level = P1_LOG_DEBUG; level <= P1_LOG_ERROR; ++level) {
                const bool mirrored = level >= P1_LOG_WARN;
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(LogLevelName(level));
                ImGui::TableNextColumn();
                ImGui::TextColored(ui::ToneColor(ui::Tone::Ok), ICON_FA_CHECK "  yes");
                ImGui::TableNextColumn();
                if (mirrored)
                    ImGui::TextColored(ui::ToneColor(ui::Tone::Ok), ICON_FA_CHECK "  as [example] text");
                else
                    ImGui::TextDisabled(ICON_FA_MINUS "  no");
            }
            ImGui::EndTable();
        }
        ui::Note("System Logs takes plugin lines through ONE limiter for all plugins together (about 1 a "
                 "second, short bursts of 10), so plugins can never push the app's own lines out of its log.");
    }

    uint64_t m_lines = 0, m_toasts = 0, m_toastsTooSoon = 0;
    std::array<uint64_t, 4> m_byLevel{};
    int64_t m_lastToastUs = 0;
};

}  // namespace

std::unique_ptr<Lesson> MakeLoggingLesson() { return std::make_unique<LoggingLesson>(); }

}  // namespace ex
