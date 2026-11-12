#pragma once
// ============================================================================
// App — what the lessons share, owned by the plugin object (Plugin.cpp).
//
// One plugin runs in one process and every callback runs on the host's single
// plugin thread, so App needs no locks: its members are touched only from the
// callbacks. The one door to another thread is Worker, whose own mutex guards
// everything the worker thread sees (lessons/Threads.cpp).
//
// A shared piece is implemented in the lesson that teaches it:
//   CallbackStats -> lessons/Lifecycle.cpp   TextureSet -> lessons/Textures.cpp
//   Worker        -> lessons/Threads.cpp     settings I/O -> Plugin.cpp
// ============================================================================
#include <p1sdk/p1sdk.hpp>

#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace ex {

class App;
class Lesson;

// ---- callback timing (lessons/Lifecycle.cpp) -----------------------------------------
enum class Cb : uint8_t { Load, Event, Tick, Overlay, Panel, Count };

struct CallbackStats {
    uint64_t calls = 0;
    float lastUs = 0.0f, avgUs = 0.0f, maxUs = 0.0f;   // avg: exponential moving average
    float perSecond = 0.0f;                            // calls during the last full second
    int64_t lastAtUs = 0;                              // when the last call ended
    void Record(int64_t startUs, int64_t endUs) noexcept;
    void Reset() noexcept { *this = CallbackStats{}; }
    // The rate, or 0 once the callback has stopped coming (a hidden panel, no game window).
    float RateAt(int64_t nowUs) const noexcept { return nowUs - lastAtUs > 2'000'000 ? 0.0f : perSecond; }
private:
    int64_t m_windowStartUs = 0;
    uint32_t m_windowCalls = 0;
    bool m_hadWindow = false;   // a full one-second window has been counted
};

// Times one callback from construction to destruction, even when the callback throws.
class CallbackTimer {
public:
    explicit CallbackTimer(CallbackStats& s) noexcept : m_stats(s), m_startUs(p1::NowUs()) {}
    ~CallbackTimer() { m_stats.Record(m_startUs, p1::NowUs()); }
    CallbackTimer(const CallbackTimer&) = delete;
    CallbackTimer& operator=(const CallbackTimer&) = delete;
private:
    CallbackStats& m_stats;
    int64_t m_startUs;
};

// ImGui's own counts for the previous frame of one context (ImGuiIO::Metrics*), plus the
// draw commands of this frame in every window of the context (Plugin.cpp CountCommands).
struct FrameMetrics {
    int vertices = 0, indices = 0, windows = 0, commands = 0;
};

// ---- overlay layers --------------------------------------------------------------------
// Ids are fixed: settings.json "layers" uses them, and so does the screenshot tool.
enum class LayerId : uint8_t {
    Markers, Health, Ring, Lines, Arrows, Hud, Labels, Tooltip, Radar, Sprite, Grid, Selection, Count
};
inline constexpr size_t kLayerCount = static_cast<size_t>(LayerId::Count);

struct LayerInfo {
    const char* id;          // settings key
    const char* name;
    const char* icon;
    const char* lesson;      // the lesson that draws it
    const char* help;
    ImVec4 color;            // default colour
    float size, sizeMin, sizeMax;
    const char* sizeFormat;  // slider format
    bool animated;           // moves between snapshots: needs p1::RequestFrames
};
std::span<const LayerInfo> Layers();
const LayerInfo& LayerOf(LayerId id);

struct LayerState {
    bool on = false;
    ImVec4 color{};
    float size = 0.0f;
};

// ---- events and areas ------------------------------------------------------------------
struct EventRecord {
    int64_t atUs = 0;
    uint32_t type = 0;
    int64_t a = 0, b = 0;
};
inline constexpr size_t kEventLogSize = 50;
// One counter per known P1_EV_* type (1 .. P1_EV_TERRAIN_CHANGED); slot 0 counts every other type,
// so a type a newer app adds shows up as "unknown" instead of hiding in a slot of its own.
inline constexpr uint32_t kEventSlots = P1_EV_TERRAIN_CHANGED + 1u;
inline constexpr uint32_t EventSlot(uint32_t type) { return type >= 1u && type < kEventSlots ? type : 0u; }

struct AreaVisit {
    std::string name, id;
    int32_t level = 0;
    uint32_t counter = 0;
    int64_t enteredUs = 0;
};
inline constexpr size_t kAreaHistorySize = 10;

// The entity picked in the Entities lesson. Address AND id: the game reuses addresses.
struct Selection {
    uint64_t address = 0;
    uint32_t id = 0;
    bool Any() const noexcept { return address != 0; }
    bool Is(const P1Entity& e) const noexcept { return address != 0 && e.address == address && e.id == id; }
};

// ---- settings.json status (Plugin.cpp does the I/O) -------------------------------------
struct SettingsState {
    p1::Settings::LoadResult loadResult = p1::Settings::LoadResult::Missing;
    std::string loadError;
    int64_t loadedUs = 0;
    bool dirty = false;            // changed since the last save
    bool saving = false;           // a save is on the worker thread
    bool saveNowRequested = false; // the Settings lesson's "Save now"
    bool reloadRequested = false;  // the Settings lesson's "Reload"
    int64_t changedUs = 0, savedUs = 0;
    uint32_t saves = 0, saveFailures = 0;
    std::string saveError;
};

// ---- textures (lessons/Textures.cpp) -------------------------------------------------------
struct Tex {
    ImTextureID id = 0;
    int w = 0, h = 0;
    uint64_t Bytes() const noexcept { return id ? uint64_t(w) * uint64_t(h) * 4u : 0u; }
};

class TextureSet {
public:
    static constexpr int kPlasmaSize = 128;   // the animated texture, 128 x 128 RGBA
    static constexpr int kPlasmaBand = 32;    // rows rewritten per update (a partial Update)

    bool EnsureBase();                        // gradient, checker, plasma; plugin thread only
    void AnimatePlasma(double seconds);       // rewrites one band of the plasma
    int  AddExtras(int count);                // small solid textures, to watch the budget move
    void DestroyExtras();
    void ReleaseAll();                        // every texture this plugin made
    int  LiveCount() const;
    uint64_t LiveBytes() const;

    Tex gradient, checker, plasma;
    std::vector<Tex> extras;
    uint64_t updates = 0, refused = 0;
    bool baseFailed = false;                  // a create returned 0: not retried every tick
private:
    std::vector<uint32_t> m_plasma;           // CPU copy of the plasma pixels (RGBA8)
    int m_band = 0;
};

// ---- the worker thread (lessons/Threads.cpp) ------------------------------------------------
class Worker {
public:
    using Done = std::function<void(App&)>;   // runs later on the PLUGIN thread, in Drain()
    using Job = std::function<Done()>;        // runs on the WORKER thread
    // Runs on the PLUGIN thread, in Drain(), when the job threw: clear "busy" flags here, or a
    // failed job would leave its demo waiting for a result that never comes.
    using Failed = std::function<void(App&, const std::string& error)>;
    static constexpr size_t kMaxQueued = 64;

    struct Stats {
        uint64_t posted = 0, done = 0, failed = 0, refused = 0;
        size_t queued = 0;
        bool running = false;
        float lastJobMs = 0.0f;
        std::string current, lastName, lastError;
    };

    Worker();
    ~Worker();
    Worker(const Worker&) = delete;
    Worker& operator=(const Worker&) = delete;

    bool Start();
    void Stop(int boundMs);                   // signal, wait at most boundMs, then join or detach
    bool Post(const char* name, Job job, Failed failed = nullptr);   // false: stopped, or the queue is full
    void Drain(App& app);                     // plugin thread: apply finished jobs' results
    Stats GetStats() const;
    uint32_t ThreadId() const;                // Win32 thread id, 0 when not running

private:
    struct Shared;
    static void Run(std::shared_ptr<Shared> sh) noexcept;
    static void Loop(Shared& sh);
    std::shared_ptr<Shared> m_sh;             // shared with the thread: outlives a detached thread
    std::thread m_thread;
    uint32_t m_threadId = 0;
};

// ---- App ---------------------------------------------------------------------------------------
class App {
public:
    App();
    ~App();
    App(const App&) = delete;
    App& operator=(const App&) = delete;

    // lessons, in menu order (lessons/Registry.cpp)
    std::vector<std::unique_ptr<Lesson>> lessons;
    std::string lessonId = "lifecycle";
    Lesson* Find(std::string_view id) const;
    Lesson* Current() const;                       // never null once lessons exist
    void Select(std::string_view id);
    bool IsShown(const Lesson* lesson, const P1Frame& f) const;   // panel visible AND this lesson open

    // persisted settings: { "lesson", "layers", "layerStyle", "lessons": { "<id>": {...} } }
    void ApplySettings(const nlohmann::json& j);
    nlohmann::json SettingsJson() const;
    void MarkDirty();
    bool SaveDue(int64_t nowUs, int64_t debounceUs) const;
    SettingsState settings;

    // overlay layers
    std::array<LayerState, kLayerCount> layers;
    LayerState& Layer(LayerId id) { return layers[static_cast<size_t>(id)]; }
    const LayerState& Layer(LayerId id) const { return layers[static_cast<size_t>(id)]; }
    bool LayerOn(LayerId id) const { return Layer(id).on; }
    void SetLayer(LayerId id, bool on);
    int ActiveLayers() const;

    // callback costs and frames
    std::array<CallbackStats, static_cast<size_t>(Cb::Count)> stats;
    CallbackStats& Stats(Cb c) { return stats[static_cast<size_t>(c)]; }
    const CallbackStats& Stats(Cb c) const { return stats[static_cast<size_t>(c)]; }
    FrameMetrics overlayMetrics, panelMetrics;
    uint32_t userFps = 0;                 // the Lifecycle lesson's RequestFrames slider
    uint32_t requestedFps = 0;            // what p1::RequestFrames was last told
    bool fpsRequestSent = false;
    const char* requestReason = "";
    void UpdateFrameRequest();
    int stressMarkers = 0;                // the Performance lesson's stress slider (not saved)

    // events and areas
    std::deque<EventRecord> events;       // newest at the back
    std::array<uint64_t, kEventSlots> eventCounts{};
    uint64_t eventsTotal = 0;
    void RecordEvent(const P1Event& e, int64_t nowUs);
    std::deque<AreaVisit> areas;          // newest at the back
    bool areaPending = false;             // AREA_CHANGED seen; the next OnTick records the area

    Selection selection;
    TextureSet textures;
    Worker worker;
    uint64_t panelThrows = 0;             // the Threads & errors lesson's deliberate exceptions

    // panel shell
    struct ShellState {
        std::string lastPage;             // to scroll a newly opened page to its top
        bool scrollNavToSelected = true;  // after load and when the panel is shown again
        bool scrollToSource = false;      // the title's source chip was clicked: show part 5
        bool codeOpen = false;
        int64_t copiedUs = 0;
        std::string copiedLesson;
    } shell;
};

}  // namespace ex
