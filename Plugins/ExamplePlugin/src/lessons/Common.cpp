#include "lessons/Common.h"

#include <cmath>
#include <cstdarg>
#include <cstdio>

namespace ex {

namespace {

std::string VFmt(const char* fmt, va_list ap) {
    char buf[1024];
    const int n = std::vsnprintf(buf, sizeof buf, fmt, ap);
    return n < 0 ? std::string() : std::string(buf);
}

}  // namespace

// [snippet:snapshot]
// Why can't the snapshot serve this demo right now? Test the flags in this
// order: the earliest missing step is the one the user can act on.
std::optional<ui::Status> GameDataGap(const P1Snapshot& s, Need need) {
    using ui::Tone;
    if (s.flags == 0)
        return ui::Status{ Tone::Warn, "No game data yet: the snapshot's flags are 0 (the app is not attached "
                                       "to the game, or this is the very first callback)." };
    if (s.flags & P1_SNAP_WGC_MODE)
        return ui::Status{ Tone::Info, "Visual (WGC) mode: P1_SNAP_WGC_MODE is on, so the app reads no game "
                                       "memory and the snapshot carries no game data." };
    if (s.flags & P1_SNAP_ATTACHING)
        return ui::Status{ Tone::Info, "Attaching: P1_SNAP_ATTACHING is on, the app's first scan of the game "
                                       "is still running." };
    if (!(s.flags & P1_SNAP_ATTACHED))
        return ui::Status{ Tone::Warn, "Not attached: P1_SNAP_ATTACHED is off. Start the game; the app finds "
                                       "it on its own." };
    if (need == Need::Attached) return std::nullopt;
    if (!(s.flags & P1_SNAP_IN_GAME))
        return Reason(Tone::Info, "Not in game: P1_SNAP_IN_GAME is off (game state: %s). Enter an area.",
                      StateName(s.game_state));
    if (need == Need::Camera && !(s.flags & P1_SNAP_CAMERA_VALID))
        return ui::Status{ Tone::Info, "No camera: P1_SNAP_CAMERA_VALID is off this tick, so world positions "
                                       "cannot be projected to the screen." };
    return std::nullopt;
}
// [/snippet]

ui::Status Live(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::string text = "Live: " + VFmt(fmt, ap);
    va_end(ap);
    return ui::Status{ ui::Tone::Ok, std::move(text) };
}

ui::Status Reason(ui::Tone tone, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::string text = VFmt(fmt, ap);
    va_end(ap);
    return ui::Status{ tone, std::move(text) };
}

const P1Entity* FindPlayerEntity(const P1Snapshot& s) {
    if (s.player.address == 0) return nullptr;
    for (const P1Entity& e : p1::Entities(s))
        if (e.address == s.player.address) return &e;
    return nullptr;
}

float GridDistance(const P1Snapshot& s, const P1Entity& e) {
    const float dx = float(e.grid_x - s.player.grid_x);
    const float dy = float(e.grid_y - s.player.grid_y);
    return std::sqrt(dx * dx + dy * dy);
}

void OutlinedText(ImDrawList* dl, ImVec2 pos, ImU32 color, const char* text) {
    const ImU32 outline = IM_COL32(0, 0, 0, 210);
    static constexpr float kOffsets[4][2] = { { -1, 0 }, { 1, 0 }, { 0, -1 }, { 0, 1 } };
    for (const auto& o : kOffsets) dl->AddText(ImVec2(pos.x + o[0], pos.y + o[1]), outline, text);
    dl->AddText(pos, color, text);
}

}  // namespace ex
