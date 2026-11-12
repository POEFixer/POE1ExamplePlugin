// ============================================================================
// SDK Example — a guided tour of the POE1Fixer plugin SDK, one lesson per page.
//
// This file is the plugin itself: the p1::Plugin subclass, the P1_PLUGIN
// exports, routing every callback to the lessons, and settings.json (loaded in
// OnLoad, saved 1 s after the last change on the worker thread, and once more
// in OnUnload). Everything a lesson teaches lives in src/lessons/<Lesson>.cpp.
//
// The plugin runs in its own sandboxed process (POE1FixerPluginHost.exe): a
// bug here can stop this plugin, never the app or the game.
// ============================================================================
#include <stdexcept>

#include <imgui_internal.h>   // ImGuiContext::Windows, for CountCommands

#include "App.h"
#include "Names.h"
#include "Shell.h"
#include "lessons/Lesson.h"

namespace ex {
namespace {

constexpr const char* kSettingsFile = "settings.json";
constexpr int64_t kSaveDebounceUs = 1'000'000;   // save 1 s after the last change
// OnUnload has 1.5 s for one plugin (about 1.3 s, shared by all plugins, when the app exits):
// never wait longer than this for the worker.
constexpr int kWorkerStopBoundMs = 300;

// What p1::Settings::Load gave: read on any thread, applied on the plugin thread.
struct SettingsFile {
    p1::Settings::LoadResult result = p1::Settings::LoadResult::Missing;
    nlohmann::json json;
    std::string error;
};

class ExamplePlugin final : public p1::Plugin {
public:
    bool OnLoad() override {
        CallbackTimer timer(m_app.Stats(Cb::Load));
        // ImGui keeps its word-wrap character classes (blank / punctuation / other) in statics
        // of each ImGui copy, and fills them only when that copy builds a font atlas. The host
        // builds the fonts, so this DLL's copy must fill its own once, or every TextWrapped
        // would break lines in the middle of words. Idempotent, needs no context.
        ImTextInitClassifiers();
        m_app.lessons = MakeAllLessons();
        ApplySettingsFile(m_app, ReadSettings());   // small, once: fine on the plugin thread here
        if (!m_app.worker.Start()) p1::Log(P1_LOG_WARN, "the worker thread did not start: saves run inline");
        p1::Log(P1_LOG_INFO, "SDK Example started: %zu lessons, language %s, permissions %s",
                m_app.lessons.size(), Language().c_str(), PermissionsText(ManifestPermissions()).c_str());
        return true;   // false (or a throw) would refuse the load: the plugin becomes Incompatible
    }

    void OnUnload() override {
        // The process is terminated right after this returns: static destructors never run.
        // Stop the thread first (bounded), release what we hold, then write unsaved settings.
        m_app.worker.Stop(kWorkerStopBoundMs);
        for (auto& lesson : m_app.lessons) lesson->Unload(m_app);
        if (m_app.settings.dirty || m_app.settings.saving) SaveNow();
    }

    void OnEvent(const P1Event& e) override {
        CallbackTimer timer(m_app.Stats(Cb::Event));
        m_app.RecordEvent(e, p1::NowUs());
        if (e.type == P1_EV_PANEL_SHOWN) m_app.shell.scrollNavToSelected = true;
        for (auto& lesson : m_app.lessons) lesson->Event(m_app, e);
    }

    // OnTick: the game logic of one iteration, on the plugin thread, after the OnEvent calls of
    // the same iteration. No ImGui here (there is no frame). The Lifecycle lesson's snippet is a
    // whole minimal plugin instead (lessons/Lifecycle.cpp): this one is the example's plumbing.
    void OnTick(const P1Frame& f) override {
        CallbackTimer timer(m_app.Stats(Cb::Tick));    // feeds the Lifecycle table
        m_app.worker.Drain(m_app);                     // results of background jobs land here
        for (auto& lesson : m_app.lessons) lesson->Tick(m_app, f);
        m_app.UpdateFrameRequest();                    // p1::RequestFrames, on a change only
        if (m_app.settings.reloadRequested) {
            m_app.settings.reloadRequested = false;
            ReloadOnWorker();                          // file I/O off the plugin thread
        }
        if (m_app.settings.saveNowRequested || m_app.SaveDue(f.now_us, kSaveDebounceUs)) {
            m_app.settings.saveNowRequested = false;
            SaveOnWorker();                            // file I/O off the plugin thread
        }
    }

    void OnOverlay(const P1Frame& f) override {
        CallbackTimer timer(m_app.Stats(Cb::Overlay));
        CaptureMetrics(m_app.overlayMetrics);          // ImGui's counts of the PREVIOUS overlay frame
        for (auto& lesson : m_app.lessons) lesson->Overlay(m_app, f);
        m_app.overlayMetrics.commands = CountCommands();
    }

    void OnPanel(const P1Frame& f) override {
        CallbackTimer timer(m_app.Stats(Cb::Panel));
        CaptureMetrics(m_app.panelMetrics);
        DrawPanel(m_app, f);
        m_app.panelMetrics.commands = CountCommands();
    }

private:
    // [snippet:settings]
    // Read the file (any thread: OnLoad, or the worker for a reload), then apply
    // it on the plugin thread. Every LoadResult needs its own answer:
    //   Loaded    apply it, reading defensively (types, ranges);
    //   Missing   a first run: the defaults stay;
    //   Rejected  the file could not be read or parsed: it is kept as
    //             "settings.json.rejected" (never overwritten), the defaults stay;
    //   Error     no config folder, a bad name, or a bad file that could not be
    //             moved aside (Save then refuses that name until a Load succeeds).
    static SettingsFile ReadSettings() {
        SettingsFile r;
        r.result = p1::Settings::Load(kSettingsFile, r.json, &r.error);
        return r;
    }

    static void ApplySettingsFile(App& app, const SettingsFile& r) {
        app.settings.loadResult = r.result;
        app.settings.loadError = r.error;
        app.settings.loadedUs = p1::NowUs();
        switch (r.result) {
            case p1::Settings::LoadResult::Loaded:
                app.ApplySettings(r.json);
                break;
            case p1::Settings::LoadResult::Missing:
                break;
            case p1::Settings::LoadResult::Rejected:
                p1::Log(P1_LOG_WARN, "settings.json rejected (%s): kept as settings.json.rejected", r.error.c_str());
                break;
            case p1::Settings::LoadResult::Error:
            default:
                p1::Log(P1_LOG_WARN, "settings.json not loaded: %s", r.error.c_str());
                break;
        }
        app.settings.dirty = false;
    }
    // [/snippet]

    // The Settings lesson's "Reload from disk": the read runs on the worker (it can be up to
    // 16 MiB and a rename), the result is applied back here in Drain().
    void ReloadOnWorker() {
        const bool posted = m_app.worker.Post(
            "reload settings.json",
            []() -> Worker::Done {
                SettingsFile r = ReadSettings();
                return [r = std::move(r)](App& app) { ApplySettingsFile(app, r); };
            },
            [](App& app, const std::string& error) {
                p1::Log(P1_LOG_WARN, "reloading settings.json failed: %s", error.c_str());
                app.settings.loadError = "the reload failed: " + error;
            });
        if (!posted) ApplySettingsFile(m_app, ReadSettings());   // no worker: a one-off read here
    }

    // Debounced save, on the worker: the JSON is built here (plugin thread, no lock needed),
    // the file is written there, and the result comes back through Drain().
    void SaveOnWorker() {
        nlohmann::json j = m_app.SettingsJson();
        m_app.settings.dirty = false;
        m_app.settings.saving = true;
        const bool posted = m_app.worker.Post(
            "save settings.json",
            [j = std::move(j)]() -> Worker::Done {
                std::string error;
                const bool ok = p1::Settings::Save(kSettingsFile, j, &error);
                return [ok, error](App& app) { FinishSave(app, ok, error); };
            },
            [](App& app, const std::string& error) { FinishSave(app, false, "the save job failed: " + error); });
        if (!posted) SaveNow();
    }

    void SaveNow() {
        std::string error;
        const bool ok = p1::Settings::Save(kSettingsFile, m_app.SettingsJson(), &error);
        FinishSave(m_app, ok, error);
    }

    static void FinishSave(App& app, bool ok, const std::string& error) {
        app.settings.saving = false;
        if (ok) {
            ++app.settings.saves;
            app.settings.savedUs = p1::NowUs();
            app.settings.saveError.clear();
            return;
        }
        ++app.settings.saveFailures;
        app.settings.saveError = error;
        app.MarkDirty();   // retried after another debounce period, not every tick
        p1::Log(P1_LOG_WARN, "settings.json not saved: %s", error.c_str());
    }

    static void CaptureMetrics(FrameMetrics& m) {
        const ImGuiIO& io = ImGui::GetIO();   // the context of THIS callback (overlay or panel)
        m.vertices = io.MetricsRenderVertices;
        m.indices = io.MetricsRenderIndices;
        m.windows = io.MetricsRenderWindows;
    }

    // The draw commands of this frame so far, in every window of this context (the panel's child
    // windows, cards and code blocks, popups, tooltips) plus the foreground and background lists:
    // what the app receives for the frame. Empty commands are left out, as ImGui drops them too.
    static int CountCommands() {
        int n = 0;
        auto count = [&n](const ImDrawList* dl) {
            for (const ImDrawCmd& c : dl->CmdBuffer) n += c.ElemCount > 0 ? 1 : 0;
        };
        for (const ImGuiWindow* w : ImGui::GetCurrentContext()->Windows)
            if (w->Active && !w->Hidden) count(w->DrawList);
        count(ImGui::GetForegroundDrawList());
        count(ImGui::GetBackgroundDrawList());
        return n;
    }

    App m_app;
};

}  // namespace
}  // namespace ex

P1_PLUGIN(ex::ExamplePlugin, "example", "SDK Example", "2.1.0", "POE1Fixer",
          "A guided tour of the plugin SDK: 24 lessons with live data, limits and copyable snippets.")
