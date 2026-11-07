#pragma once
// ============================================================================
// Lesson — one page of the tour. Every page has the same five parts, in order:
//   1. title + one-line summary            LessonInfo::title / summary
//   2. What / When / Limits                LessonInfo::what / when / limits
//   3. a status line that is never blank   Lesson::GetStatus()
//   4. the live demo                       Lesson::Draw()
//   5. the source chip + Copy snippet      LessonInfo::snippet (tools/gen_snippets.py)
// The shell (Shell.cpp) draws 1, 2, 3 and 5; a lesson writes only its demo.
//
// A lesson also gets the plugin's other callbacks (Tick, Overlay, Event) and its
// own object in settings.json (Load / Save), so each lesson file is complete on
// its own: read one file, and you have read the whole feature.
// ============================================================================
#include <memory>
#include <vector>

#include "App.h"
#include "ui/Ui.h"

namespace ex {

enum class Group : uint8_t { Basics, GameData, Drawing, Interaction, Advanced };
const char* GroupName(Group g);

struct LessonInfo {
    const char* id = "";        // fixed: settings.json "lesson" and the screenshot tool use it
    Group group = Group::Basics;
    const char* icon = "";
    const char* title = "";
    const char* summary = "";   // what this lesson teaches, in one line
    const char* what = "";      // what the API does
    const char* when = "";      // when a plugin should use it
    const char* limits = "";    // the limits and costs the app enforces, in numbers
    const char* snippet = "";   // id of the "// [snippet:<id>]" block shown under the page
};

class Lesson {
public:
    explicit Lesson(const LessonInfo& info) noexcept : m_info(info) {}
    virtual ~Lesson() = default;
    Lesson(const Lesson&) = delete;
    Lesson& operator=(const Lesson&) = delete;

    const LessonInfo& Info() const noexcept { return m_info; }

    // "Live: <the key fact>", or the exact reason the demo is empty right now. Never blank.
    virtual ui::Status GetStatus(const App& app, const P1Frame& f) const = 0;
    // The live demo, inside OnPanel (only while this lesson is open).
    virtual void Draw(App& app, const P1Frame& f) = 0;

    virtual void Tick(App&, const P1Frame&) {}            // every OnTick, open or not
    virtual void Overlay(App&, const P1Frame&) {}         // every OnOverlay, open or not
    virtual void Event(App&, const P1Event&) {}           // every OnEvent
    virtual void Load(App&, const nlohmann::json&) {}     // settings.json "lessons"/<id>
    virtual void Save(const App&, nlohmann::json&) const {}
    virtual void Unload(App&) {}                          // OnUnload: release what you hold

private:
    const LessonInfo& m_info;
};

// One factory per file in lessons/, listed in menu order by MakeAllLessons (Registry.cpp).
std::unique_ptr<Lesson> MakeLifecycleLesson();
std::unique_ptr<Lesson> MakeEventsLesson();
std::unique_ptr<Lesson> MakeLoggingLesson();
std::unique_ptr<Lesson> MakeSettingsLesson();
std::unique_ptr<Lesson> MakeLanguageLesson();
std::unique_ptr<Lesson> MakeSnapshotLesson();
std::unique_ptr<Lesson> MakePlayerLesson();
std::unique_ptr<Lesson> MakeAreaLesson();
std::unique_ptr<Lesson> MakeEntitiesLesson();
std::unique_ptr<Lesson> MakeBuffsLesson();
std::unique_ptr<Lesson> MakeInventoryLesson();
std::unique_ptr<Lesson> MakeMapUiLesson();
std::unique_ptr<Lesson> MakeOverlayLesson();
std::unique_ptr<Lesson> MakeWorldToScreenLesson();
std::unique_ptr<Lesson> MakeTexturesLesson();
std::unique_ptr<Lesson> MakeWidgetsLesson();
std::unique_ptr<Lesson> MakeStyleLesson();
std::unique_ptr<Lesson> MakeKeyboardLesson();
std::unique_ptr<Lesson> MakeInputLesson();
std::unique_ptr<Lesson> MakeClipboardLesson();
std::unique_ptr<Lesson> MakeMemoryLesson();
std::unique_ptr<Lesson> MakePerformanceLesson();
std::unique_ptr<Lesson> MakeThreadsLesson();

std::vector<std::unique_ptr<Lesson>> MakeAllLessons();

}  // namespace ex
