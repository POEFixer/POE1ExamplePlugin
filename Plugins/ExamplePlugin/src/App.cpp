#include "App.h"

#include <IconsFontAwesome6.h>

#include "JsonUtil.h"
#include "lessons/Lesson.h"

namespace ex {

namespace {

// The overlay layers. Sizes are in the unit of `sizeFormat`; pixel sizes are scaled by the
// app's UI scale when drawn.
const LayerInfo kLayers[] = {
    { "markers", "Entity markers", ICON_FA_LOCATION_CROSSHAIRS, "overlay",
      "A circle on every living hostile monster. Magic, rare and unique ones use their rarity colour; "
      "the colour here is for normal ones.",
      ImVec4(0.95f, 0.36f, 0.30f, 0.90f), 8.0f, 3.0f, 24.0f, "%.0f px", false },
    { "health", "Health bars", ICON_FA_HEART, "overlay",
      "A bar above each living hostile monster. Only the local player's life is authoritative: a monster "
      "the game has not synced yet can read full.",
      ImVec4(0.32f, 0.85f, 0.42f, 0.95f), 46.0f, 24.0f, 120.0f, "%.0f px wide", false },
    { "ring", "Player ring", ICON_FA_CIRCLE, "overlay",
      "A circle ON THE GROUND around the player: 64 world points, each projected, joined by lines.",
      ImVec4(0.36f, 0.80f, 1.00f, 0.80f), 180.0f, 50.0f, 1000.0f, "%.0f world units", false },
    { "lines", "Lines to nearest", ICON_FA_ROUTE, "overlay",
      "Lines from the player to the nearest living hostile monsters.",
      ImVec4(1.00f, 0.86f, 0.30f, 0.70f), 3.0f, 1.0f, 10.0f, "%.0f targets", false },
    { "arrows", "Edge arrows", ICON_FA_LOCATION_ARROW, "overlay",
      "Rare and unique monsters that are off screen get an arrow on the screen edge, pointing at them: "
      "filled in their rarity colour, labelled with the name and the distance. The colour here is the "
      "arrow's outline.",
      ImVec4(0.95f, 0.95f, 0.95f, 0.90f), 14.0f, 8.0f, 32.0f, "%.0f px", false },
    { "hud", "HUD card", ICON_FA_TABLE_LIST, "overlay",
      "A small card: area, entity counts, and what this overlay costs. The legend (a checkbox above the "
      "table) joins it as a second section.",
      ImVec4(0.50f, 0.74f, 1.00f, 1.00f), 1.0f, 0.8f, 1.6f, "%.2fx", false },
    { "labels", "Outlined labels", ICON_FA_TAG, "overlay",
      "Names of exits, portals, waypoints, NPCs, strongboxes and rare or unique monsters, nearest first. "
      "Outlined so they read on any background; a dot marks the point of every label that has no marker.",
      ImVec4(0.96f, 0.96f, 0.96f, 1.00f), 12.0f, 1.0f, 40.0f, "%.0f labels", false },
    { "tooltip", "Hover tooltip", ICON_FA_HAND_POINTER, "overlay",
      "Point at a marker in the game: ImGui::IsMouseHoveringRect, then BeginTooltip placed with "
      "SetNextWindowPos. Item hover never fires in the click-through overlay window.",
      ImVec4(0.96f, 0.96f, 0.96f, 1.00f), 14.0f, 6.0f, 40.0f, "%.0f px hit radius", false },
    { "radar", "Mini radar", ICON_FA_SATELLITE_DISH, "overlay",
      "Entities around the player, at the right edge, oriented like the screen: monsters in their marker "
      "colours, NPCs teal, chests as squares, exits as diamonds. The colour here is the ring's. The inner "
      "ring is the screen's side edge; the outer ring is half as far again.",
      ImVec4(0.72f, 0.62f, 0.98f, 0.95f), 90.0f, 60.0f, 200.0f, "%.0f px radius", false },
    { "sprite", "Textured sprite", ICON_FA_IMAGE, "overlay",
      "The Textures lesson's animated plasma texture, drawn as a rotating quad above the player.",
      ImVec4(1.00f, 1.00f, 1.00f, 0.95f), 48.0f, 24.0f, 128.0f, "%.0f px", true },
    { "grid", "World grid", ICON_FA_TABLE_CELLS, "w2s",
      "A 5 x 5 grid on the ground around the player, every line projected from world to screen. Thin and "
      "neutral: a reference behind the other layers.",
      ImVec4(1.00f, 1.00f, 1.00f, 0.22f), 100.0f, 25.0f, 400.0f, "%.0f world units", false },
    { "selection", "Selection ring", ICON_FA_BULLSEYE, "entities",
      "A pulsing ring around the entity the Entities lesson's card shows: the row you picked, or, until you "
      "pick one, the nearest hostile monster.",
      ImVec4(0.95f, 0.40f, 0.95f, 0.95f), 20.0f, 10.0f, 60.0f, "%.0f px", true },
};
static_assert(std::size(kLayers) == kLayerCount, "one LayerInfo per LayerId");

}  // namespace

std::span<const LayerInfo> Layers() { return kLayers; }
const LayerInfo& LayerOf(LayerId id) { return kLayers[static_cast<size_t>(id)]; }

App::App() {
    for (size_t i = 0; i < kLayerCount; ++i) {
        layers[i].color = kLayers[i].color;
        layers[i].size = kLayers[i].size;
    }
}

App::~App() = default;   // here, where Lesson is a complete type

Lesson* App::Find(std::string_view id) const {
    for (const auto& l : lessons)
        if (id == l->Info().id) return l.get();
    return nullptr;
}

Lesson* App::Current() const {
    if (Lesson* l = Find(lessonId)) return l;
    return lessons.empty() ? nullptr : lessons.front().get();
}

void App::Select(std::string_view id) {
    if (!Find(id) || id == lessonId) return;
    lessonId = std::string(id);
    MarkDirty();
}

bool App::IsShown(const Lesson* lesson, const P1Frame& f) const {
    return f.panel_visible != 0 && lesson != nullptr && Current() == lesson;
}

void App::ApplySettings(const nlohmann::json& j) {
    if (!j.is_object()) return;
    // An unknown or missing lesson id falls back to the first lesson.
    const std::string id = js::String(j, "lesson", "lifecycle", 64);
    lessonId = Find(id) ? id : "lifecycle";

    if (const nlohmann::json* l = js::Find(j, "layers"); l && l->is_object()) {
        // "all": true is a documented developer switch (the screenshot tool uses it): every layer
        // on. It is applied once; the next save writes the individual switches instead.
        const bool all = js::Bool(*l, "all", false);
        for (size_t i = 0; i < kLayerCount; ++i) layers[i].on = all || js::Bool(*l, kLayers[i].id, layers[i].on);
    }
    if (const nlohmann::json* st = js::Find(j, "layerStyle"); st && st->is_object()) {
        for (size_t i = 0; i < kLayerCount; ++i) {
            const nlohmann::json* one = js::Find(*st, kLayers[i].id);
            if (!one) continue;
            layers[i].color = js::Color(*one, "color", layers[i].color);
            layers[i].size = js::Float(*one, "size", layers[i].size, kLayers[i].sizeMin, kLayers[i].sizeMax);
        }
    }
    const nlohmann::json* per = js::Find(j, "lessons");
    for (const auto& lesson : lessons) {
        const nlohmann::json* mine = per ? js::Find(*per, lesson->Info().id) : nullptr;
        if (mine && mine->is_object()) lesson->Load(*this, *mine);
    }
}

nlohmann::json App::SettingsJson() const {
    nlohmann::json j = nlohmann::json::object();
    j["lesson"] = lessonId;
    nlohmann::json on = nlohmann::json::object(), style = nlohmann::json::object();
    for (size_t i = 0; i < kLayerCount; ++i) {
        on[kLayers[i].id] = layers[i].on;
        style[kLayers[i].id] = { { "color", js::ColorJson(layers[i].color) }, { "size", layers[i].size } };
    }
    j["layers"] = std::move(on);
    j["layerStyle"] = std::move(style);
    nlohmann::json per = nlohmann::json::object();
    for (const auto& lesson : lessons) {
        nlohmann::json mine = nlohmann::json::object();
        lesson->Save(*this, mine);
        if (!mine.empty()) per[lesson->Info().id] = std::move(mine);
    }
    j["lessons"] = std::move(per);
    return j;
}

void App::MarkDirty() {
    settings.dirty = true;
    settings.changedUs = p1::NowUs();
}

bool App::SaveDue(int64_t nowUs, int64_t debounceUs) const {
    // Debounced: a slider dragged for 3 s writes the file once, 1 s after the user lets go.
    return settings.dirty && !settings.saving && nowUs - settings.changedUs >= debounceUs;
}

void App::SetLayer(LayerId id, bool on) {
    if (Layer(id).on == on) return;
    Layer(id).on = on;
    MarkDirty();
}

int App::ActiveLayers() const {
    int n = 0;
    for (const LayerState& l : layers) n += l.on ? 1 : 0;
    return n;
}

void App::UpdateFrameRequest() {
    // 0 = an overlay frame only when new data arrives (the snapshot cadence, ~30/s in game).
    // N = a frame whenever N per second would be due since the last one, for things that move
    // between snapshots. The two add up: in game, snapshots already bring ~30 frames a second,
    // so asking for 30 more would build up to 60. 20 only fills the gaps (a menu, a loading
    // screen, the app's idle 10 Hz) and costs nothing extra while snapshots flow.
    uint32_t want = userFps;
    requestReason = userFps ? "the Lifecycle lesson's slider" : "nothing animates: frames follow new snapshots";
    bool animated = false;
    for (size_t i = 0; i < kLayerCount; ++i) animated |= layers[i].on && kLayers[i].animated;
    if (animated && want < 20) {
        want = 20;
        requestReason = "an animated layer is on (sprite or selection ring): 20 fill the gaps between snapshots";
    }
    if (!fpsRequestSent || want != requestedFps) {
        p1::RequestFrames(want);   // cheap, but only on a change: it is a request, not a per-frame call
        requestedFps = want;
        fpsRequestSent = true;
    }
}

void App::RecordEvent(const P1Event& e, int64_t nowUs) {
    events.push_back({ nowUs, e.type, e.a, e.b });
    while (events.size() > kEventLogSize) events.pop_front();
    ++eventCounts[EventSlot(e.type)];
    ++eventsTotal;
}

}  // namespace ex
