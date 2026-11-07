// Lesson "threads": a worker thread done right, what is thread-safe in the SDK, and what an
// exception inside a callback does (the SDK catches it; the plugin keeps running).
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <system_error>

#include "JsonUtil.h"
#include "lessons/Common.h"

namespace ex {

// ---- Worker (declared in App.h) ------------------------------------------------------------

struct Worker::Shared {
    struct Item {
        std::string name;
        Job job;
        Failed failed;
    };
    std::mutex mutex;
    std::condition_variable wake;       // a job arrived, or stop
    std::condition_variable finished;   // the thread left its loop
    std::deque<Item> queue;
    std::vector<Done> outbox;           // results waiting for Drain() on the plugin thread
    bool stop = false, exited = false;
    Stats stats;
};

Worker::Worker() = default;

Worker::~Worker() {
    // Stop() has joined or detached the thread already; this only covers a plugin that never
    // reached OnUnload. A joinable std::thread destroyed here would call std::terminate.
    if (m_thread.joinable()) m_thread.detach();
}

bool Worker::Start() {
    if (m_thread.joinable()) return true;
    m_sh = std::make_shared<Shared>();
    try {
        m_thread = std::thread(&Worker::Run, m_sh);
    } catch (const std::system_error&) {
        return false;
    }
    m_threadId = GetThreadId(m_thread.native_handle());
    std::lock_guard lock(m_sh->mutex);
    m_sh->stats.running = true;
    return true;
}

// [snippet:threads]
// The worker's loop. The rules for every thread a plugin starts:
//  1. wait on a condition variable, never spin, and check `stop` after every
//     wake (Stop() sets it and notifies, so this wait needs no time-out);
//  2. never run a job while holding the lock;
//  3. touch nothing of the plugin's: a job returns a Done, and the plugin thread
//     runs it in Drain(), where no lock is needed any more;
//  4. catch EVERYTHING: an exception that leaves a thread is std::terminate,
//     a crash of the whole plugin (Run() wraps this loop in try / catch);
//  5. a failure comes back too (Failed), or a "busy" flag would wait forever.
void Worker::Loop(Shared& sh) {
    std::unique_lock lock(sh.mutex);
    while (!sh.stop) {
        if (sh.queue.empty()) {
            sh.wake.wait(lock);
            continue;
        }
        Shared::Item item = std::move(sh.queue.front());
        sh.queue.pop_front();
        sh.stats.current = item.name;
        lock.unlock();
        const auto t0 = std::chrono::steady_clock::now();
        Done done;
        std::string error;
        try {
            done = item.job();
        } catch (const std::exception& e) {
            error = e.what();
        } catch (...) {
            error = "unknown exception";
        }
        if (!error.empty() && item.failed)
            done = [failed = std::move(item.failed), error](App& app) { failed(app, error); };
        const float ms = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t0).count();
        lock.lock();
        sh.stats.current.clear();
        sh.stats.lastName = std::move(item.name);
        sh.stats.lastJobMs = ms;
        if (error.empty()) ++sh.stats.done;
        else { ++sh.stats.failed; sh.stats.lastError = error; }
        if (done) sh.outbox.push_back(std::move(done));
    }
}
// [/snippet]

void Worker::Run(std::shared_ptr<Shared> sh) noexcept {
    try {
        Loop(*sh);
    } catch (...) {
        // Nothing may leave a thread. (Loop already catches what a job throws; this is the
        // lock or an allocation failing.)
    }
    try {
        std::lock_guard lock(sh->mutex);
        sh->exited = true;
        sh->stats.running = false;
    } catch (...) {
    }
    sh->finished.notify_all();
}

void Worker::Stop(int boundMs) {
    if (!m_thread.joinable()) return;
    bool exited = false;
    {
        std::unique_lock lock(m_sh->mutex);
        m_sh->stop = true;
        m_sh->queue.clear();   // queued jobs are dropped: the plugin is going away
        m_sh->wake.notify_all();
        exited = m_sh->finished.wait_for(lock, std::chrono::milliseconds(boundMs), [&] { return m_sh->exited; });
    }
    // OnUnload has 1.5 s (about 1.3 s, shared by all plugins, when the app exits): join only a
    // thread that is known to be done. A thread still in a job is detached; it holds `m_sh` by
    // shared_ptr, so nothing it touches is freed under it.
    if (exited) m_thread.join();
    else m_thread.detach();
    m_threadId = 0;
}

bool Worker::Post(const char* name, Job job, Failed failed) {
    if (!m_sh || !m_thread.joinable()) return false;
    std::lock_guard lock(m_sh->mutex);
    if (m_sh->stop || m_sh->queue.size() >= kMaxQueued) {
        ++m_sh->stats.refused;
        return false;
    }
    m_sh->queue.push_back({ name ? name : "", std::move(job), std::move(failed) });
    ++m_sh->stats.posted;
    m_sh->wake.notify_one();
    return true;
}

void Worker::Drain(App& app) {
    if (!m_sh) return;
    std::vector<Done> ready;
    {
        std::lock_guard lock(m_sh->mutex);
        ready.swap(m_sh->outbox);
    }
    for (Done& d : ready) {
        try {
            d(app);   // the plugin thread: App needs no lock here
        } catch (const std::exception& e) {
            p1::Log(P1_LOG_ERROR, "a worker result failed to apply: %s", e.what());
        }
    }
}

Worker::Stats Worker::GetStats() const {
    if (!m_sh) return {};
    std::lock_guard lock(m_sh->mutex);
    Stats s = m_sh->stats;
    s.queued = m_sh->queue.size();
    return s;
}

uint32_t Worker::ThreadId() const { return m_threadId; }

// ---- the lesson -------------------------------------------------------------------------------

namespace {

constexpr LessonInfo kInfo{
    .id = "threads",
    .group = Group::Advanced,
    .icon = ICON_FA_GEARS,
    .title = "Threads & errors",
    .summary = "Do slow work on a thread of your own, and see what happens when a callback throws.",
    .what = "Plugins may start threads. The SDK wraps every callback in try / catch: an exception thrown out of "
            "OnTick, OnOverlay, OnPanel, OnEvent or OnUnload is logged and the plugin keeps running.",
    .when = "File I/O, the network, heavy computation: anything that could keep a callback busy. This plugin "
            "writes settings.json and runs the demos below on its worker.",
    .limits = "A callback over 5 s kills the plugin, and the hang check does not see a deadlock inside your "
              "own threads: in a callback, never wait on another thread's lock or join without a bound. An "
              "exception that leaves a thread of yours is a crash. "
              "Log, Notify, SetClipboard, RequestFrames, NowUs, Memory and Input work from any thread; Texture "
              "only on the plugin thread; ImGui only inside OnOverlay / OnPanel.",
    .snippet = "threads",
};

constexpr int kPrimesMin = 10'000, kPrimesMax = 5'000'000;

// Runs on the worker: plain computation on its own data. The bound is clamped again here: a
// value from settings.json or a typed one is never trusted with an allocation.
uint32_t CountPrimes(uint32_t below) {
    below = std::clamp<uint32_t>(below, kPrimesMin, kPrimesMax);
    std::vector<bool> composite(below, false);
    uint32_t n = 0;
    for (uint32_t i = 2; i < below; ++i) {
        if (composite[i]) continue;
        ++n;
        for (uint64_t k = uint64_t(i) * i; k < below; k += i) composite[size_t(k)] = true;
    }
    return n;
}

struct ThreadRule {
    const char* call;
    bool anyThread;
    const char* note;
};

constexpr ThreadRule kRules[] = {
    { "p1::Log, p1::Notify", true, "rate-limited by the app" },
    { "p1::SetClipboard", true, "applied only while the user uses the panel" },
    { "p1::RequestFrames, p1::NowUs", true, "" },
    { "p1::Memory::*", true, "with the memory permission" },
    { "p1::Input::*", true, "with the input permission and consent" },
    { "p1::Texture::*", false, "elsewhere they return 0 / false" },
    { "ImGui::*", false, "only inside OnOverlay and OnPanel" },
    { "the P1Frame and the snapshot", false, "valid until the callback returns" },
};

class ThreadsLesson final : public Lesson {
public:
    ThreadsLesson() : Lesson(kInfo) {}

    ui::Status GetStatus(const App& app, const P1Frame&) const override {
        const Worker::Stats st = app.worker.GetStats();
        if (!st.running) return Reason(ui::Tone::Warn, "The worker thread is not running (it failed to start, or stopped).");
        return Live("worker thread %u: %llu job%s done, %llu failed, %zu waiting.", app.worker.ThreadId(),
                    static_cast<unsigned long long>(st.done), st.done == 1 ? "" : "s",
                    static_cast<unsigned long long>(st.failed), st.queued);
    }

    // Posts the prime counter once a second while this page is open, the demo is on and no run
    // is in flight. A demo nobody looks at should cost nothing.
    void Tick(App& app, const P1Frame& f) override {
        if (!m_primesOn || m_primesBusy || !app.IsShown(this, f) || f.now_us - m_primesUs < 1'000'000) return;
        m_primesUs = f.now_us;
        const uint32_t below = static_cast<uint32_t>(std::clamp(m_primesBelow, kPrimesMin, kPrimesMax));
        m_primesBusy = app.worker.Post(
            "count primes",
            [below]() -> Worker::Done {
                const auto t0 = std::chrono::steady_clock::now();
                const uint32_t n = CountPrimes(below);
                const float ms = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t0).count();
                return [below, n, ms](App& a) {   // back on the plugin thread
                    if (auto* l = static_cast<ThreadsLesson*>(a.Find(kInfo.id))) l->OnPrimes(below, n, ms);
                };
            },
            [](App& a, const std::string& error) {   // the job threw (bad_alloc, ...): not busy any more
                if (auto* l = static_cast<ThreadsLesson*>(a.Find(kInfo.id))) l->OnPrimesFailed(error);
            });
    }

    void Draw(App& app, const P1Frame&) override {
        DrawWorker(app);
        DrawRules();
        DrawExceptions(app);
    }

    void Load(App&, const nlohmann::json& j) override {
        m_primesOn = js::Bool(j, "primes", true);
        m_primesBelow = js::Int(j, "primesBelow", 1'000'000, kPrimesMin, kPrimesMax);
    }
    void Save(const App&, nlohmann::json& j) const override {
        j["primes"] = m_primesOn;
        j["primesBelow"] = m_primesBelow;
    }

    void OnPrimes(uint32_t below, uint32_t count, float ms) {
        m_primesBusy = false;
        m_resultOk = true;
        m_result = Fmt("%s primes below %s, counted in %.1f ms on the worker", FormatCount(count).c_str(),
                       FormatCount(below).c_str(), double(ms));
    }

    void OnPrimesFailed(const std::string& error) {
        m_primesBusy = false;   // the next run is posted a second later, as usual
        m_resultOk = false;
        m_result = "The job threw on the worker (" + error + "): Failed brought it back to the plugin thread.";
    }

private:
    void DrawWorker(App& app) {
        ui::SectionTitle(ICON_FA_GEARS, "The worker thread");
        const Worker::Stats st = app.worker.GetStats();
        if (ui::BeginKv("worker", { "thread", "jobs", "now", "last job", "last error" })) {
            ui::KvText("thread", "%u (the plugin thread is %lu)", app.worker.ThreadId(), GetCurrentThreadId());
            ui::KvText("jobs", "%llu posted, %llu done, %llu failed, %llu refused, %zu waiting",
                       static_cast<unsigned long long>(st.posted), static_cast<unsigned long long>(st.done),
                       static_cast<unsigned long long>(st.failed), static_cast<unsigned long long>(st.refused), st.queued);
            ui::KvText("now", "%s", st.current.empty() ? "idle (waiting on a condition variable: no CPU)" : st.current.c_str());
            ui::KvText("last job", "%s, %.2f ms", st.lastName.empty() ? "-" : st.lastName.c_str(), double(st.lastJobMs));
            ui::KvText("last error", "%s", st.lastError.empty() ? "-" : st.lastError.c_str());
            ui::EndKv();
        }
        if (ui::BeginForm("primes", { "Count primes", "Below" })) {
            ui::FormRow("Count primes", "Once a second while this page is open, on the worker. The result comes "
                                        "back through Drain().");
            if (ImGui::Checkbox("##on", &m_primesOn)) app.MarkDirty();
            ui::FormRow("Below");
            // AlwaysClamp: Ctrl+click lets the user type any number, and the bound sizes an allocation.
            if (ImGui::SliderInt("##below", &m_primesBelow, kPrimesMin, kPrimesMax, "%d",
                                 ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_AlwaysClamp))
                app.MarkDirty();
            ui::EndForm();
        }
        if (!m_result.empty()) ui::StatusLine({ m_resultOk ? ui::Tone::Ok : ui::Tone::Warn, m_result });

        ui::Flow flow;
        const char* log = ICON_FA_TERMINAL "  Log from the worker";
        flow.Next(ui::ButtonWidth(log));
        if (ImGui::Button(log))
            app.worker.Post("log a line", []() -> Worker::Done {
                p1::Log(P1_LOG_INFO, "hello from the worker thread %lu (p1::Log is thread-safe)", GetCurrentThreadId());
                return nullptr;
            });
        const char* tex = ICON_FA_IMAGE "  Texture::Create on the worker";
        flow.Next(ui::ButtonWidth(tex));
        if (ImGui::Button(tex))
            app.worker.Post("texture off-thread", []() -> Worker::Done {
                const ImU32 pixel = IM_COL32_WHITE;
                const ImTextureID id = p1::Texture::Create(&pixel, 1, 1);   // refused: not the plugin thread
                return [id](App& a) {
                    if (id) p1::Texture::Destroy(id);   // never expected; cleaned up on the right thread anyway
                    if (auto* l = static_cast<ThreadsLesson*>(a.Find(kInfo.id)))
                        l->m_textureResult = id ? "Create returned a texture (unexpected)."
                                                : "Create returned 0 on the worker, as documented: textures belong "
                                                  "to the plugin thread.";
                };
            });
        if (!m_textureResult.empty()) ui::TextDim("%s", m_textureResult.c_str());
    }

    static void DrawRules() {
        ui::SectionTitle(ICON_FA_LIST_CHECK, "Which calls work from which thread");
        const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                                      ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoSavedSettings;
        if (!ImGui::BeginTable("rules", 3, flags)) return;
        ImGui::TableSetupColumn("Call", ImGuiTableColumnFlags_WidthStretch, 1.4f);
        ImGui::TableSetupColumn("From", ImGuiTableColumnFlags_WidthStretch, 0.9f);
        ImGui::TableSetupColumn("Note", ImGuiTableColumnFlags_WidthStretch, 1.5f);
        ImGui::TableHeadersRow();
        for (const ThreadRule& r : kRules) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ui::TextWrap(r.call);
            ImGui::TableNextColumn();
            ImGui::TextColored(ui::ToneColor(r.anyThread ? ui::Tone::Ok : ui::Tone::Warn), "%s",
                               r.anyThread ? "any thread" : "plugin thread");
            ImGui::TableNextColumn();
            ui::TextWrap(r.note);
        }
        ImGui::EndTable();
    }

    static void DrawExceptions(App& app) {
        ui::SectionTitle(ICON_FA_BUG, "An exception inside a callback");
        ui::TextDim("The button throws a std::runtime_error from inside OnPanel. The SDK's guard catches it and "
                    "logs \"uncaught C++ exception in on_panel: ...\" (an error line: it also reaches System "
                    "Logs); the host repairs the ImGui stacks the throw left open, and the plugin keeps running. "
                    "This frame of the panel ends early; the next one is whole.");
        if (app.panelThrows)
            ui::TextDim("Thrown %llu time%s so far: see this plugin's log in the Plugins tab.",
                                static_cast<unsigned long long>(app.panelThrows), app.panelThrows == 1 ? "" : "s");
        if (ImGui::Button(ICON_FA_BUG "  Throw inside OnPanel")) {
            ++app.panelThrows;
            throw std::runtime_error("thrown on purpose by the Threads & errors lesson");
        }
        ui::Note("There is no crash button on purpose. A crash (an access violation, std::terminate) kills only "
                 "this plugin's process: the app writes a dump and restarts it after 2, 4, 8, 16 then 30 s, and "
                 "stops after 4 crashes in 10 minutes.");
    }

    bool m_primesOn = true, m_primesBusy = false, m_resultOk = true;
    int m_primesBelow = 1'000'000;
    int64_t m_primesUs = 0;
    std::string m_result, m_textureResult;
};

}  // namespace

std::unique_ptr<Lesson> MakeThreadsLesson() { return std::make_unique<ThreadsLesson>(); }

}  // namespace ex
