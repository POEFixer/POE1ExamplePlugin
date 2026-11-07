#pragma once
// ============================================================================
// Common — small helpers several lessons use: why game data is missing right
// now, status-line builders, and a few entity questions.
// ============================================================================
#include <optional>

#include "Names.h"
#include "Util.h"
#include "lessons/Lesson.h"

namespace ex {

// What a lesson needs from the snapshot before its demo can show anything.
enum class Need : uint8_t { Attached, InGame, Camera };

// The exact reason the snapshot cannot serve `need` right now, or nullopt when it can.
std::optional<ui::Status> GameDataGap(const P1Snapshot& s, Need need);

// Status-line builders: Live(...) = "Live: ...", Reason(tone, ...) = the text as given.
ui::Status Live(_Printf_format_string_ const char* fmt, ...);
ui::Status Reason(ui::Tone tone, _Printf_format_string_ const char* fmt, ...);

// The local player's own entity (its render_pos carries the height the ring and the grid
// are drawn at), or null.
const P1Entity* FindPlayerEntity(const P1Snapshot& s);

// Not on the player's side, by its reaction: 1 is friendly (the player's minions too), and 0xFF
// means no reaction was read, which this plugin never counts as hostile. Every "hostile" in the
// example uses this one test: the Entities filter, the status lines, the overlay layers.
inline bool IsHostile(const P1Entity& e) { return e.reaction != 1 && e.reaction != 0xFF; }

// A monster that is alive and hostile.
inline bool IsHostileAlive(const P1Entity& e) {
    return e.kind == P1_KIND_MONSTER && e.has_life && e.hp > 0 && IsHostile(e);
}

// The app's UI scale for pixel sizes in the overlay (the overlay's font is already scaled).
inline float UiScale(const P1Frame& f) {
    const float s = f.viewport.ui_scale;
    return (s >= 0.5f && s <= 4.0f) ? s : 1.0f;
}

// Grid distance from the player (grid units: about 10.9 world units each).
float GridDistance(const P1Snapshot& s, const P1Entity& e);

// Text with a dark one-pixel outline, readable over any part of the game.
void OutlinedText(ImDrawList* dl, ImVec2 pos, ImU32 color, const char* text);

}  // namespace ex
