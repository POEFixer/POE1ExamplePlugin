// Lesson "overlay": a gallery of overlay layers, each one switchable, with its own colour and size.
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <vector>

#include "JsonUtil.h"
#include "lessons/Common.h"

namespace ex {

namespace {

constexpr float kTwoPi = 6.2831853f;

constexpr LessonInfo kInfo{
    .id = "overlay",
    .group = Group::Drawing,
    .icon = ICON_FA_LAYER_GROUP,
    .title = "Overlay gallery",
    .summary = "Drawing over the game: markers, bars, world-space shapes, edge arrows, cards, labels, a "
               "tooltip, a radar and a sprite.",
    .what = "OnOverlay is an ImGui frame over the game's client area: (0, 0) is its top-left corner, sizes are "
            "client pixels. Draw with ImGui::GetWindowDrawList() (or the foreground / background lists).",
    .when = "Anything the player should see in the game. Project world positions with p1::WorldToScreen; "
            "scale your pixel sizes by f.viewport.ui_scale.",
    .limits = "Click-through: no clicks or keys; item hover never fires (test the cursor with "
              "ImGui::IsMouseHoveringRect). Per frame: 150,000 vertices and 8,192 commands from the host; the "
              "app then draws at most 1,024 commands and 6 screens of fill. At most maxFps (60) frames a second.",
    .snippet = "overlay",
};

// The radar's own colours for what is not a monster. Each kind also has its own shape, so
// nothing depends on telling two similar colours apart.
constexpr ImU32 kRadarNpc = IM_COL32(64, 214, 196, 255);     // teal, circle
constexpr ImU32 kRadarChest = IM_COL32(214, 172, 108, 235);  // tan, square
constexpr ImU32 kRadarExit = IM_COL32(120, 200, 255, 255);   // light blue, diamond

ImU32 U32(const ImVec4& c) { return ImGui::ColorConvertFloat4ToU32(c); }

ImU32 Opaque(ImVec4 c) {
    c.w = 1.0f;
    return U32(c);
}

// A monster's colour everywhere on the overlay: its rarity colour, or the markers layer's
// colour for a normal one. The radar, the labels and the legend use the same rule.
ImU32 MarkerColor(const P1Entity& e, ImU32 normal) {
    return (e.rarity >= 1 && e.rarity <= 3) ? U32(ui::RarityColor(e.rarity)) : normal;
}

// [snippet:overlay]
// Markers: project each monster's world position to client pixels, draw a
// circle there. WorldToScreen returns false behind the camera: skip those.
// Pixel sizes follow the app's UI scale; colours carry the rarity.
void DrawMarkers(ImDrawList* dl, const P1Frame& f, const LayerState& layer) {
    const float r = layer.size * UiScale(f);
    const ImU32 normal = ImGui::ColorConvertFloat4ToU32(layer.color);
    for (const P1Entity& e : p1::Entities(*f.snapshot)) {
        // Living hostile monsters: reaction 1 is friendly, 0xFF means none was read.
        if (e.kind != P1_KIND_MONSTER || !e.has_life || e.hp <= 0 || e.reaction == 1 || e.reaction == 0xFF) continue;
        ImVec2 p;
        if (!p1::WorldToScreen(f, e.render_pos, p)) continue;
        const bool rarityColour = e.rarity >= 1 && e.rarity <= 3;   // magic, rare, unique
        const ImU32 col = rarityColour ? ImGui::ColorConvertFloat4ToU32(ui::RarityColor(e.rarity)) : normal;
        dl->AddCircleFilled(p, r * 0.35f, col);
        dl->AddCircle(p, e.rarity == 3 ? r * 1.3f : r, col, 0, e.rarity >= 2 ? 2.5f : 1.5f);
    }
}
// [/snippet]

void DrawHealthBars(ImDrawList* dl, const P1Frame& f, const LayerState& layer, float markerRadius) {
    const float scale = UiScale(f);
    const float w = layer.size * scale, h = (std::max)(3.0f, w * 0.11f);
    const ImU32 fill = U32(layer.color);
    for (const P1Entity& e : p1::Entities(*f.snapshot)) {
        if (!IsHostileAlive(e) || e.hp_max <= 0) continue;
        ImVec2 p;
        if (!p1::WorldToScreen(f, e.render_pos, p)) continue;
        const ImVec2 a(p.x - w * 0.5f, p.y - markerRadius - 6.0f * scale - h);
        const ImVec2 b(a.x + w, a.y + h);
        const float frac = std::clamp(float(e.hp) / float(e.hp_max), 0.0f, 1.0f);
        dl->AddRectFilled(a, b, IM_COL32(0, 0, 0, 170));
        dl->AddRectFilled(a, ImVec2(a.x + w * frac, b.y), fill);
        dl->AddRect(ImVec2(a.x - 1.0f, a.y - 1.0f), ImVec2(b.x + 1.0f, b.y + 1.0f), IM_COL32(0, 0, 0, 220));
    }
}

// A circle ON THE GROUND: 64 world points around the player, each projected. Points
// behind the camera split the outline into separate runs.
void DrawRing(ImDrawList* dl, const P1Frame& f, const LayerState& layer, const P1Entity& me) {
    const ImU32 col = U32(layer.color);
    const float thickness = 2.0f * UiScale(f);
    ImVec2 run[65];
    int n = 0;
    for (int i = 0; i <= 64; ++i) {
        const float a = float(i) * kTwoPi / 64.0f;
        ImVec2 p;
        if (p1::WorldToScreen(f, me.render_pos.x + layer.size * std::cos(a), me.render_pos.y + layer.size * std::sin(a),
                              me.render_pos.z, p)) {
            run[n++] = p;
            continue;
        }
        if (n > 1) dl->AddPolyline(run, n, col, ImDrawFlags_None, thickness);
        n = 0;
    }
    if (n > 1) dl->AddPolyline(run, n, col, ImDrawFlags_None, thickness);
}

struct Near {
    const P1Entity* e;
    float dist;
};

// A count from a layer's size: at least 1, whatever a settings file or a typed value says.
size_t CountOf(const LayerState& layer) { return size_t((std::max)(1.0f, layer.size)); }

std::vector<Near> Nearest(const P1Snapshot& s, size_t count, bool (*keep)(const P1Entity&)) {
    std::vector<Near> v;
    for (const P1Entity& e : p1::Entities(s))
        if (keep(e)) v.push_back({ &e, GridDistance(s, e) });
    const size_t n = (std::min)(count, v.size());
    std::partial_sort(v.begin(), v.begin() + std::ptrdiff_t(n), v.end(), [](const Near& a, const Near& b) { return a.dist < b.dist; });
    v.resize(n);
    return v;
}

void DrawLines(ImDrawList* dl, const P1Frame& f, const LayerState& layer, ImVec2 from) {
    const P1Snapshot& s = *f.snapshot;
    const std::vector<Near> targets = Nearest(s, CountOf(layer), &IsHostileAlive);
    for (size_t i = 0; i < targets.size(); ++i) {
        ImVec2 p;
        if (!p1::WorldToScreen(f, targets[i].e->render_pos, p)) continue;
        ImVec4 c = layer.color;
        c.w *= 1.0f - 0.6f * float(i) / float(targets.size());   // nearer = stronger
        dl->AddLine(from, p, U32(c), 1.5f * UiScale(f));
    }
}

// Off-screen rare and unique monsters: an arrow on the screen's edge, filled in the rarity colour,
// outlined in the layer's colour, with the name and the grid distance on the screen side of it.
void DrawArrows(ImDrawList* dl, const P1Frame& f, const LayerState& layer, ImVec2 client) {
    const P1Snapshot& s = *f.snapshot;
    const float scale = UiScale(f), size = layer.size * scale, margin = size * 1.6f;
    const ImVec2 c(client.x * 0.5f, client.y * 0.5f);
    for (const P1Entity& e : p1::Entities(s)) {
        if (!IsHostileAlive(e) || e.rarity < 2 || e.rarity > 3) continue;
        ImVec2 p;
        if (!p1::WorldToScreen(f, e.render_pos, p)) continue;
        if (p.x >= margin && p.x <= client.x - margin && p.y >= margin && p.y <= client.y - margin) continue;   // on screen
        const float dx = p.x - c.x, dy = p.y - c.y, len = std::sqrt(dx * dx + dy * dy);
        if (len < 1.0f) continue;
        const ImVec2 d(dx / len, dy / len);
        // Walk from the centre along d until the inset rectangle's edge.
        const float t = (std::min)((c.x - margin) / (std::max)(std::fabs(d.x), 1e-4f),
                                   (c.y - margin) / (std::max)(std::fabs(d.y), 1e-4f));
        const ImVec2 tip(c.x + d.x * t, c.y + d.y * t);
        const ImVec2 back(tip.x - d.x * size, tip.y - d.y * size), side(-d.y * size * 0.55f, d.x * size * 0.55f);
        const ImVec2 l(back.x + side.x, back.y + side.y), r(back.x - side.x, back.y - side.y);
        const ImU32 col = U32(ui::RarityColor(e.rarity));
        dl->AddTriangleFilled(tip, l, r, col);
        dl->AddTriangle(tip, l, r, U32(layer.color), 1.5f * scale);
        // The label: behind the arrow, towards the centre, kept inside the screen.
        const std::string text = Fmt("%s · %.0f", LastPathSegment(p1::Str(s, e.path)).c_str(), double(GridDistance(s, e)));
        const ImVec2 ts = ImGui::CalcTextSize(text.c_str());
        const float away = 4.0f * scale + 0.5f * (std::fabs(d.x) * ts.x + std::fabs(d.y) * ts.y);
        ImVec2 at(back.x - d.x * away - ts.x * 0.5f, back.y - d.y * away - ts.y * 0.5f);
        at.x = std::clamp(at.x, 4.0f, (std::max)(4.0f, client.x - ts.x - 4.0f));
        at.y = std::clamp(at.y, 4.0f, (std::max)(4.0f, client.y - ts.y - 4.0f));
        OutlinedText(dl, ImVec2(std::floor(at.x), std::floor(at.y)), col, text.c_str());
    }
}

std::string LabelFor(const P1Snapshot& s, const P1Entity& e) {
    switch (e.kind) {
        case P1_KIND_AREA_TRANSITION: {
            const char* to = p1::Str(s, e.transition_target);   // radar-gated: may be empty
            return to[0] ? Fmt("Exit " ICON_FA_ARROW_RIGHT " %s", to) : std::string("Exit");
        }
        case P1_KIND_PORTAL:   return "Portal";
        case P1_KIND_WAYPOINT: return "Waypoint";
        case P1_KIND_CHEST:    return "Strongbox";
        default:               return LastPathSegment(p1::Str(s, e.path));
    }
}

bool WantsLabel(const P1Entity& e) {
    switch (e.kind) {
        case P1_KIND_AREA_TRANSITION:
        case P1_KIND_PORTAL:
        case P1_KIND_WAYPOINT:
        case P1_KIND_NPC:     return true;
        case P1_KIND_CHEST:   return e.is_strongbox && !e.is_opened;
        case P1_KIND_MONSTER: return IsHostileAlive(e) && (e.rarity == 2 || e.rarity == 3);
        default:              return false;
    }
}

// Under the marker when the markers layer drew one there; anything else gets a small dot at its
// point first, so no label floats unanchored.
void DrawLabels(ImDrawList* dl, const P1Frame& f, const LayerState& layer, float markerRadius, bool markersOn) {
    const P1Snapshot& s = *f.snapshot;
    const float scale = UiScale(f), dot = 3.0f * scale;
    for (const Near& n : Nearest(s, CountOf(layer), &WantsLabel)) {
        ImVec2 p;
        if (!p1::WorldToScreen(f, n.e->render_pos, p)) continue;
        const std::string text = LabelFor(s, *n.e);
        const ImVec2 ts = ImGui::CalcTextSize(text.c_str());
        const bool monster = n.e->kind == P1_KIND_MONSTER;
        const ImU32 col = monster ? MarkerColor(*n.e, U32(layer.color)) : U32(layer.color);
        float below = markerRadius;
        if (!(monster && markersOn)) {
            dl->AddCircleFilled(p, dot + 1.0f, IM_COL32(0, 0, 0, 210));
            dl->AddCircleFilled(p, dot, col);
            below = dot;
        }
        OutlinedText(dl, ImVec2(std::floor(p.x - ts.x * 0.5f), std::floor(p.y + below + 3.0f * scale)), col, text.c_str());
    }
}

// Hover in a click-through window: ImGui's item hover never fires there, so test the
// cursor against each marker's rectangle and open the tooltip by hand.
void DrawHoverTooltip(const P1Frame& f, const LayerState& layer, float markerRadius) {
    const P1Snapshot& s = *f.snapshot;
    const float r = layer.size * UiScale(f);
    for (const P1Entity& e : p1::Entities(s)) {
        if (!IsHostileAlive(e)) continue;
        ImVec2 p;
        if (!p1::WorldToScreen(f, e.render_pos, p) ||
            !ImGui::IsMouseHoveringRect(ImVec2(p.x - r, p.y - r), ImVec2(p.x + r, p.y + r), false))
            continue;
        // Up and to the side of the marker, so it covers neither the label under the marker nor the
        // health bar above it; to the left on the right half of the screen.
        const bool left = p.x > float(f.viewport.client_w) * 0.5f;
        const float gap = markerRadius + 10.0f * UiScale(f);
        ImGui::SetNextWindowPos(ImVec2(left ? p.x - gap : p.x + gap, p.y - markerRadius), ImGuiCond_Always,
                                ImVec2(left ? 1.0f : 0.0f, 1.0f));
        if (!ImGui::BeginTooltip()) return;
        ImGui::TextColored(ui::RarityColor(e.rarity), "%s", LastPathSegment(p1::Str(s, e.path)).c_str());
        ImGui::Text("%s, life %d / %d", RarityName(e.rarity), e.hp, e.hp_max);
        ImGui::TextDisabled("%s", ShortPath(p1::Str(s, e.path), 3).c_str());
        ImGui::EndTooltip();
        return;   // one tooltip
    }
}

void DrawSprite(ImDrawList* dl, const P1Frame& f, const LayerState& layer, const Tex& tex, ImVec2 player) {
    if (!tex.id) return;   // the Textures lesson creates it once this layer is on
    const float half = layer.size * UiScale(f) * 0.5f;
    const ImVec2 c(player.x, player.y - half * 3.2f);
    const float a = float(double(f.now_us % 60'000'000) / 1e6) * 0.8f;   // radians: needs frames between snapshots
    const float cs = std::cos(a) * half, sn = std::sin(a) * half;
    // The four corners of a square of side 2 * half, turned by `a` around c.
    const ImVec2 q1(c.x - cs + sn, c.y - sn - cs), q2(c.x + cs + sn, c.y + sn - cs);
    const ImVec2 q3(c.x + cs - sn, c.y + sn + cs), q4(c.x - cs - sn, c.y - sn + cs);
    dl->AddImageQuad(ImTextureRef(tex.id), q1, q2, q3, q4, ImVec2(0, 0), ImVec2(1, 0), ImVec2(1, 1), ImVec2(0, 1),
                     U32(layer.color));
}

// A top-down radar oriented like the screen: the camera says how one world unit along x and
// along y moves on screen, and the radar uses exactly that (no hard-coded angle). Monsters use
// the markers' colours; NPCs, chests and exits each have a shape and a colour of their own.
void DrawRadar(ImDrawList* dl, const P1Frame& f, const LayerState& layer, ImU32 normalMonster, const P1Entity& me,
               ImVec2 client) {
    const float scale = UiScale(f), R = layer.size * scale;
    const ImVec2 c(client.x - R - 24.0f * scale, client.y * 0.42f);
    ImVec2 o, ax, ay;
    const P1Vec3 m = me.render_pos;
    if (!p1::WorldToScreen(f, m, o) || !p1::WorldToScreen(f, m.x + 100.0f, m.y, m.z, ax) ||
        !p1::WorldToScreen(f, m.x, m.y + 100.0f, m.z, ay))
        return;
    const ImVec2 ux((ax.x - o.x) / 100.0f, (ax.y - o.y) / 100.0f), uy((ay.x - o.x) / 100.0f, (ay.y - o.y) / 100.0f);
    // The inner ring is the screen's left / right edge (half a screen width from the player), the
    // outer ring 1.5 times as far: what is on screen fills the inner disc, what is near the
    // screen shows between the rings.
    const float edge = client.x * 0.5f, k = R / (edge * 1.5f);

    dl->AddCircleFilled(c, R, IM_COL32(10, 14, 20, 170));
    dl->AddCircle(c, R, U32(layer.color), 0, 1.5f * scale);   // the layer's colour: the ring only
    dl->AddCircle(c, edge * k, IM_COL32(255, 255, 255, 40));
    dl->AddLine(ImVec2(c.x - R, c.y), ImVec2(c.x + R, c.y), IM_COL32(255, 255, 255, 25));
    dl->AddLine(ImVec2(c.x, c.y - R), ImVec2(c.x, c.y + R), IM_COL32(255, 255, 255, 25));
    for (const P1Entity& e : p1::Entities(*f.snapshot)) {
        if (&e == &me) continue;
        enum class Shape { None, Dot, Square, Diamond } shape = Shape::None;
        ImU32 col = 0;
        if (IsHostileAlive(e)) {
            shape = Shape::Dot;
            col = MarkerColor(e, normalMonster);
        } else if (e.kind == P1_KIND_NPC) {
            shape = Shape::Dot;
            col = kRadarNpc;
        } else if (e.kind == P1_KIND_CHEST && !e.is_opened) {
            shape = Shape::Square;
            col = kRadarChest;
        } else if (e.kind == P1_KIND_AREA_TRANSITION || e.kind == P1_KIND_PORTAL || e.kind == P1_KIND_WAYPOINT) {
            shape = Shape::Diamond;
            col = kRadarExit;
        }
        if (shape == Shape::None) continue;
        const float dx = e.render_pos.x - m.x, dy = e.render_pos.y - m.y;
        const ImVec2 d((dx * ux.x + dy * uy.x) * k, (dx * ux.y + dy * uy.y) * k);
        if (d.x * d.x + d.y * d.y > R * R) continue;
        const ImVec2 at(c.x + d.x, c.y + d.y);
        const float s = (e.rarity >= 2 && e.rarity <= 3 && shape == Shape::Dot ? 3.5f : 2.5f) * scale;
        if (shape == Shape::Dot) dl->AddCircleFilled(at, s, col);
        else if (shape == Shape::Square) dl->AddRectFilled(ImVec2(at.x - s, at.y - s), ImVec2(at.x + s, at.y + s), col);
        else dl->AddQuadFilled(ImVec2(at.x, at.y - s * 1.4f), ImVec2(at.x + s * 1.4f, at.y), ImVec2(at.x, at.y + s * 1.4f),
                               ImVec2(at.x - s * 1.4f, at.y), col);
    }
    dl->AddCircleFilled(c, 3.5f * scale, IM_COL32(255, 255, 255, 255));
}

// ---- the HUD and the legend: one card ------------------------------------------------------
// One helper for both, so they share padding, rounding, the accent bar and the title colour. When
// both are on they are two sections of the same card, as wide as the wider of the two.
struct CardRow {
    enum class Kind : uint8_t { Title, Text, Section, Layer, RarityKey } kind;
    std::string text;
    ImU32 color = 0;
    const char* icon = nullptr;
};

void DrawCard(ImDrawList* dl, ImVec2 at, ImU32 accent, const std::vector<CardRow>& rows, ImU32 normalMonster, float scale) {
    const float lh = ImGui::GetTextLineHeight(), pad = std::floor(lh * 0.5f), bar = std::floor(4.0f * scale);
    const float iconBox = lh * 1.3f, gap = std::floor(lh * 0.3f), dot = std::floor(lh * 0.22f);
    static constexpr const char* kRarity[] = { "normal", "magic", "rare", "unique" };
    auto rarityKeyWidth = [&]() {
        float w = iconBox;
        for (const char* r : kRarity) w += dot * 2.0f + gap * 0.6f + ImGui::CalcTextSize(r).x + gap;
        return w - gap;
    };
    // Measure.
    float w = 0.0f, h = 0.0f;
    for (const CardRow& r : rows) {
        switch (r.kind) {
            case CardRow::Kind::Layer:     w = (std::max)(w, iconBox + ImGui::CalcTextSize(r.text.c_str()).x); break;
            case CardRow::Kind::RarityKey: w = (std::max)(w, rarityKeyWidth()); break;
            default:                       w = (std::max)(w, ImGui::CalcTextSize(r.text.c_str()).x); break;
        }
        h += lh + (r.kind == CardRow::Kind::Section ? pad : 0.0f);
    }
    const ImVec2 b(at.x + bar + pad * 2.0f + w, at.y + pad * 2.0f + h);
    const float rounding = pad * 0.6f;
    dl->AddRectFilled(at, b, IM_COL32(12, 16, 22, 200), rounding);
    dl->AddRectFilled(at, ImVec2(at.x + bar, b.y), accent, rounding, ImDrawFlags_RoundCornersLeft);
    // Draw.
    const float x = at.x + bar + pad;
    float y = at.y + pad;
    const ImU32 text = IM_COL32(235, 235, 235, 255), dim = IM_COL32(160, 170, 185, 255);
    for (const CardRow& r : rows) {
        switch (r.kind) {
            case CardRow::Kind::Title:
                dl->AddText(ImVec2(x, y), accent, r.text.c_str());
                break;
            case CardRow::Kind::Text:
                dl->AddText(ImVec2(x, y), text, r.text.c_str());
                break;
            case CardRow::Kind::Section: {   // a hairline, then the section's title
                const float ly = std::floor(y + pad * 0.5f) + 0.5f;
                dl->AddLine(ImVec2(x, ly), ImVec2(b.x - pad, ly), IM_COL32(255, 255, 255, 40));
                y += pad;
                dl->AddText(ImVec2(x, y), accent, r.text.c_str());
                break;
            }
            case CardRow::Kind::Layer: {     // the layer's own icon, in the layer's colour
                const float iw = ImGui::CalcTextSize(r.icon).x;
                dl->AddText(ImVec2(std::floor(x + (iconBox - gap - iw) * 0.5f), y), r.color, r.icon);
                dl->AddText(ImVec2(x + iconBox, y), text, r.text.c_str());
                break;
            }
            case CardRow::Kind::RarityKey: { // what a marker's colour means
                float kx = x + iconBox;
                for (uint8_t i = 0; i < 4; ++i) {
                    const ImU32 col = i == 0 ? normalMonster : U32(ui::RarityColor(i));
                    dl->AddCircleFilled(ImVec2(kx + dot, y + lh * 0.5f), dot, col);
                    kx += dot * 2.0f + gap * 0.6f;
                    dl->AddText(ImVec2(kx, y), dim, kRarity[i]);
                    kx += ImGui::CalcTextSize(kRarity[i]).x + gap;
                }
                break;
            }
        }
        y += lh;
    }
}

class OverlayLesson final : public Lesson {
public:
    OverlayLesson() : Lesson(kInfo) {}

    ui::Status GetStatus(const App& app, const P1Frame& f) const override {
        if (!f.viewport.valid)
            return Reason(ui::Tone::Warn, "No game window: OnOverlay runs only while the app knows the game window "
                                          "(viewport.valid is 0).");
        if (!f.overlay_enabled)
            return Reason(ui::Tone::Warn, "The app does not show this plugin's overlay: overlay_enabled is 0.");
        const int on = app.ActiveLayers();
        if (on == 0) return Reason(ui::Tone::Info, "No layer is on: tick one in the table below.");
        if (auto gap = GameDataGap(*f.snapshot, Need::Camera))
            return Reason(ui::Tone::Info, "%d layers on; the HUD and the legend draw, the world layers wait. %s", on,
                          gap->text.c_str());
        return Live("%d of %zu layers on; the last overlay frame had %s vertices and %d draw commands.", on, kLayerCount,
                    FormatCount(uint64_t(app.overlayMetrics.vertices)).c_str(), app.overlayMetrics.commands);
    }

    void Draw(App& app, const P1Frame&) override {
        DrawLayerTable(app);
        DrawNotes();
    }

    void Overlay(App& app, const P1Frame& f) override {
        const P1Snapshot& s = *f.snapshot;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 client(float(f.viewport.client_w), float(f.viewport.client_h));
        const float scale = UiScale(f);
        const bool markersOn = app.LayerOn(LayerId::Markers);
        const float markerR = app.Layer(LayerId::Markers).size * scale;
        const ImU32 normalMonster = Opaque(app.Layer(LayerId::Markers).color);
        const bool world = (s.flags & P1_SNAP_IN_GAME) && (s.flags & P1_SNAP_CAMERA_VALID);
        const P1Entity* me = world ? FindPlayerEntity(s) : nullptr;
        ImVec2 player;
        const bool havePlayer = me && p1::WorldToScreen(f, me->render_pos, player);

        if (world) {
            if (me && app.LayerOn(LayerId::Ring)) DrawRing(dl, f, app.Layer(LayerId::Ring), *me);
            if (havePlayer && app.LayerOn(LayerId::Lines)) DrawLines(dl, f, app.Layer(LayerId::Lines), player);
            if (markersOn) DrawMarkers(dl, f, app.Layer(LayerId::Markers));
            if (app.LayerOn(LayerId::Health)) DrawHealthBars(dl, f, app.Layer(LayerId::Health), markerR);
            if (app.LayerOn(LayerId::Arrows)) DrawArrows(dl, f, app.Layer(LayerId::Arrows), client);
            if (app.LayerOn(LayerId::Labels)) DrawLabels(dl, f, app.Layer(LayerId::Labels), markerR, markersOn);
            if (havePlayer && app.LayerOn(LayerId::Sprite))
                DrawSprite(dl, f, app.Layer(LayerId::Sprite), app.textures.plasma, player);
            if (me && app.LayerOn(LayerId::Radar))
                DrawRadar(dl, f, app.Layer(LayerId::Radar), normalMonster, *me, client);
        }
        DrawHudCard(dl, app, f, ImVec2(16.0f * scale, client.y * 0.16f), normalMonster);
        if (world && app.LayerOn(LayerId::Tooltip)) DrawHoverTooltip(f, app.Layer(LayerId::Tooltip), markerR);
    }

    void Load(App&, const nlohmann::json& j) override { m_legend = js::Bool(j, "legend", true); }
    void Save(const App&, nlohmann::json& j) const override { j["legend"] = m_legend; }

private:
    // The HUD (a layer) and the legend (a checkbox) as one card: either one, or both as two sections.
    void DrawHudCard(ImDrawList* dl, const App& app, const P1Frame& f, ImVec2 at, ImU32 normalMonster) const {
        const bool hud = app.LayerOn(LayerId::Hud), legend = m_legend && app.ActiveLayers() > 0;
        if (!hud && !legend) return;
        const LayerState& hudLayer = app.Layer(LayerId::Hud);
        std::vector<CardRow> rows;
        if (hud) {
            const P1Snapshot& s = *f.snapshot;
            uint32_t hostile = 0;
            for (const P1Entity& e : p1::Entities(s)) hostile += IsHostileAlive(e) ? 1u : 0u;
            rows.push_back({ CardRow::Kind::Title, "SDK Example" });
            rows.push_back({ CardRow::Kind::Text, (s.flags & P1_SNAP_ATTACHED)
                                                      ? Fmt("%s, level %d", p1::Str(s, s.area.name), s.area.level)
                                                      : std::string("not attached") });
            rows.push_back({ CardRow::Kind::Text, Fmt("%u entities, %u hostile alive", s.entity_count, hostile) });
            rows.push_back({ CardRow::Kind::Text, Fmt("overlay %.0f fps, %s a frame",
                                                      double(app.Stats(Cb::Overlay).RateAt(f.now_us)),
                                                      FormatCostUs(app.Stats(Cb::Overlay).avgUs).c_str()) });
        }
        if (legend) {
            rows.push_back({ hud ? CardRow::Kind::Section : CardRow::Kind::Title, "Overlay layers" });
            for (size_t i = 0; i < kLayerCount; ++i) {
                if (!app.layers[i].on) continue;
                rows.push_back({ CardRow::Kind::Layer, Layers()[i].name, Opaque(app.layers[i].color), Layers()[i].icon });
                if (static_cast<LayerId>(i) == LayerId::Markers) rows.push_back({ CardRow::Kind::RarityKey, "" });
            }
        }
        // The HUD layer's size scales the whole card; its colour is the card's accent.
        ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * (hud ? hudLayer.size : 1.0f));
        DrawCard(dl, at, Opaque(hudLayer.color), rows, normalMonster, UiScale(f));
        ImGui::PopFont();
    }

    void DrawLayerTable(App& app) {
        ui::SectionTitle(ICON_FA_LAYER_GROUP, "Layers");
        ui::Flow flow;
        const char* all = ICON_FA_EYE "  All on";
        const char* none = ICON_FA_EYE_SLASH "  All off";
        flow.Next(ui::ButtonWidth(all));
        if (ImGui::Button(all))
            for (size_t i = 0; i < kLayerCount; ++i) app.SetLayer(static_cast<LayerId>(i), true);
        flow.Next(ui::ButtonWidth(none));
        if (ImGui::Button(none))
            for (size_t i = 0; i < kLayerCount; ++i) app.SetLayer(static_cast<LayerId>(i), false);
        flow.Next(ImGui::GetFrameHeight() + ImGui::CalcTextSize("Legend card on the overlay").x + ui::Em(0.5f));
        if (ImGui::Checkbox("Legend card on the overlay", &m_legend)) app.MarkDirty();

        // Size: drags, not sliders (a slider's knob sits on its value text), as wide as the widest
        // value any layer can show, measured. Half an em of cell padding keeps the columns apart.
        const ImGuiStyle& st = ImGui::GetStyle();
        float sizeW = ImGui::CalcTextSize("Size").x;
        for (const LayerInfo& info : Layers())
            sizeW = (std::max)(sizeW, ImGui::CalcTextSize(Fmt(info.sizeFormat, double(info.sizeMax)).c_str()).x);
        sizeW += st.FramePadding.x * 2.0f + ui::Em(1.0f);
        const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                                      ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoSavedSettings;
        ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(ui::Em(0.4f), st.CellPadding.y));
        const bool open = ImGui::BeginTable("layers", 4, flags);
        ImGui::PopStyleVar();
        if (!open) return;
        const float box = ImGui::GetFrameHeight();
        ImGui::TableSetupColumn("On", ImGuiTableColumnFlags_WidthFixed, (std::max)(box, ImGui::CalcTextSize("On").x));
        ImGui::TableSetupColumn("Layer", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("Colour", ImGuiTableColumnFlags_WidthFixed, (std::max)(box, ImGui::CalcTextSize("Colour").x));
        ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, sizeW);
        ImGui::TableHeadersRow();
        for (size_t i = 0; i < kLayerCount; ++i) {
            const LayerInfo& info = Layers()[i];
            LayerState& st2 = app.layers[i];
            ImGui::PushID(info.id);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            bool on = st2.on;
            if (ImGui::Checkbox("##on", &on)) app.SetLayer(static_cast<LayerId>(i), on);
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::Text("%s  %s", info.icon, info.name);
            ImGui::SameLine();
            if (std::string_view(info.lesson) != "overlay")
                ui::HelpMarker(Fmt("%s\n\nDrawn by the %s lesson; listed here so that one switch turns every layer on.",
                                   info.help, info.lesson).c_str());
            else
                ui::HelpMarker(info.help);
            ImGui::TableNextColumn();
            if (ImGui::ColorEdit4("##color", &st2.color.x, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel |
                                                              ImGuiColorEditFlags_AlphaBar))
                app.MarkDirty();
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-FLT_MIN);
            const float speed = (info.sizeMax - info.sizeMin) / 300.0f;
            if (ImGui::DragFloat("##size", &st2.size, speed, info.sizeMin, info.sizeMax, info.sizeFormat,
                                 ImGuiSliderFlags_AlwaysClamp))
                app.MarkDirty();
            ImGui::SetItemTooltip("Drag sideways, or double-click to type: the value is clamped to %.0f .. %.0f.",
                                  double(info.sizeMin), double(info.sizeMax));
            ImGui::PopID();
        }
        ImGui::EndTable();
        ui::TextDim("World grid and Selection ring belong to the World " ICON_FA_ARROW_RIGHT " screen and Entities "
                    "lessons; they are listed here too, so one switch turns every layer on.");
    }

    static void DrawNotes() {
        ui::SectionTitle(ICON_FA_LIGHTBULB, "How the layers are made");
        ui::BulletWrapped("Screen-space: markers, bars, arrows, labels and the HUD are drawn at a projected point, "
                          "in pixels scaled by the UI scale.");
        ui::BulletWrapped("World-space: the ring is 64 points on the ground, each projected; it bends with the "
                          "camera like the game's own ground effects.");
        ui::BulletWrapped("One colour, one meaning: a monster is its rarity colour (normal ones the markers' "
                          "colour) on the markers, the labels, the arrows and the radar alike.");
        ui::BulletWrapped("The radar asks the camera how a step along world x and y moves on screen, so it turns "
                          "with the view without a hard-coded angle.");
        ui::BulletWrapped("The tooltip tests the cursor itself (IsMouseHoveringRect): the overlay window is "
                          "click-through and never reports item hover.");
        ui::BulletWrapped("Overdraw costs the app: avoid big filled shapes on top of each other. The HUD card is "
                          "small on purpose.");
    }

    bool m_legend = true;
};

}  // namespace

std::unique_ptr<Lesson> MakeOverlayLesson() { return std::make_unique<OverlayLesson>(); }

}  // namespace ex
