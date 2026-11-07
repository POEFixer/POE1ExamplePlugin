// The lesson list, in menu order. The ids are fixed (settings.json and the screenshot tool use
// them): lifecycle, events, logging, settings, language, snapshot, player, area, entities, buffs,
// inventory, mapui, overlay, w2s, textures, widgets, style, keyboard, input, clipboard, memory,
// performance, threads.
#include "lessons/Lesson.h"

namespace ex {

const char* GroupName(Group g) {
    switch (g) {
        case Group::Basics:      return "Basics";
        case Group::GameData:    return "Game data";
        case Group::Drawing:     return "Drawing";
        case Group::Interaction: return "Interaction";
        case Group::Advanced:    return "Advanced";
        default:                 return "";
    }
}

std::vector<std::unique_ptr<Lesson>> MakeAllLessons() {
    std::vector<std::unique_ptr<Lesson>> v;
    v.reserve(23);
    // Basics
    v.push_back(MakeLifecycleLesson());
    v.push_back(MakeEventsLesson());
    v.push_back(MakeLoggingLesson());
    v.push_back(MakeSettingsLesson());
    v.push_back(MakeLanguageLesson());
    // Game data
    v.push_back(MakeSnapshotLesson());
    v.push_back(MakePlayerLesson());
    v.push_back(MakeAreaLesson());
    v.push_back(MakeEntitiesLesson());
    v.push_back(MakeBuffsLesson());
    v.push_back(MakeInventoryLesson());
    v.push_back(MakeMapUiLesson());
    // Drawing
    v.push_back(MakeOverlayLesson());
    v.push_back(MakeWorldToScreenLesson());
    v.push_back(MakeTexturesLesson());
    v.push_back(MakeWidgetsLesson());
    v.push_back(MakeStyleLesson());
    // Interaction
    v.push_back(MakeKeyboardLesson());
    v.push_back(MakeInputLesson());
    v.push_back(MakeClipboardLesson());
    // Advanced
    v.push_back(MakeMemoryLesson());
    v.push_back(MakePerformanceLesson());
    v.push_back(MakeThreadsLesson());
    return v;
}

}  // namespace ex
