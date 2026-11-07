// Lesson "keyboard": reading keys with p1::KeyEdge, and a plugin hotkey that toggles a layer.
#include <algorithm>
#include <cfloat>
#include <cmath>

#include "JsonUtil.h"
#include "lessons/Common.h"

namespace ex {

namespace {

constexpr LessonInfo kInfo{
    .id = "keyboard",
    .group = Group::Interaction,
    .icon = ICON_FA_KEYBOARD,
    .title = "Keyboard",
    .summary = "Read the keys the player holds in the game, and react to a press of your own hotkey.",
    .what = "P1Viewport::keys_down has one bit per virtual-key code (mouse buttons included). p1::KeyEdge turns "
            "two samples into Down / Pressed / Released; call Update once per OnTick.",
    .when = "Plugin hotkeys and \"while a key is held\" features. React to an edge (Pressed), not to the level.",
    .limits = "keys_down is refreshed about 60 times a second, ONLY while the game is the foreground window and no "
              "game text field (chat) has focus; all zero otherwise. You see it once per OnTick (about 30 a "
              "second, at least 10), so a tap shorter than one tick can be missed. Reading keys never blocks "
              "them: the game and the app's own hotkeys still see every key.",
    .snippet = "keyboard",
};

struct Cap {
    const char* label;
    int vk;
    float width;   // in key units
};

// A compact keyboard, every row 15 units wide like a real one, so the right edge is straight.
// A cap without a label is a gap. Shift has a cap per side (VK_LSHIFT / VK_RSHIFT); Ctrl and
// Alt have one cap each, lit by the generic code (either side).
constexpr float kRowUnits = 15.0f;
const Cap kRow0[] = { { "Esc", VK_ESCAPE, 1 }, { nullptr, 0, 1 }, { "F1", VK_F1, 1 }, { "F2", VK_F2, 1 },
                      { "F3", VK_F3, 1 }, { "F4", VK_F4, 1 }, { nullptr, 0, 0.5f }, { "F5", VK_F5, 1 },
                      { "F6", VK_F6, 1 }, { "F7", VK_F7, 1 }, { "F8", VK_F8, 1 }, { nullptr, 0, 0.5f },
                      { "F9", VK_F9, 1 }, { "F10", VK_F10, 1 }, { "F11", VK_F11, 1 }, { "F12", VK_F12, 1 } };
const Cap kRow1[] = { { "`", VK_OEM_3, 1 }, { "1", '1', 1 }, { "2", '2', 1 }, { "3", '3', 1 }, { "4", '4', 1 },
                      { "5", '5', 1 }, { "6", '6', 1 }, { "7", '7', 1 }, { "8", '8', 1 }, { "9", '9', 1 },
                      { "0", '0', 1 }, { "-", VK_OEM_MINUS, 1 }, { "=", VK_OEM_PLUS, 1 }, { "Bksp", VK_BACK, 2 } };
const Cap kRow2[] = { { "Tab", VK_TAB, 1.5f }, { "Q", 'Q', 1 }, { "W", 'W', 1 }, { "E", 'E', 1 }, { "R", 'R', 1 },
                      { "T", 'T', 1 }, { "Y", 'Y', 1 }, { "U", 'U', 1 }, { "I", 'I', 1 }, { "O", 'O', 1 },
                      { "P", 'P', 1 }, { "[", VK_OEM_4, 1 }, { "]", VK_OEM_6, 1 }, { "\\", VK_OEM_5, 1.5f } };
const Cap kRow3[] = { { "Caps", VK_CAPITAL, 1.75f }, { "A", 'A', 1 }, { "S", 'S', 1 }, { "D", 'D', 1 }, { "F", 'F', 1 },
                      { "G", 'G', 1 }, { "H", 'H', 1 }, { "J", 'J', 1 }, { "K", 'K', 1 }, { "L", 'L', 1 },
                      { ";", VK_OEM_1, 1 }, { "'", VK_OEM_7, 1 }, { "Enter", VK_RETURN, 2.25f } };
const Cap kRow4[] = { { "Shift", VK_LSHIFT, 2.25f }, { "Z", 'Z', 1 }, { "X", 'X', 1 }, { "C", 'C', 1 }, { "V", 'V', 1 },
                      { "B", 'B', 1 }, { "N", 'N', 1 }, { "M", 'M', 1 }, { ",", VK_OEM_COMMA, 1 },
                      { ".", VK_OEM_PERIOD, 1 }, { "/", VK_OEM_2, 1 }, { "Shift", VK_RSHIFT, 2.75f } };
const Cap kRow5[] = { { "Ctrl", VK_CONTROL, 1.5f }, { "Alt", VK_MENU, 1.5f }, { "Space", VK_SPACE, 8 },
                      { ICON_FA_ARROW_LEFT, VK_LEFT, 1 }, { ICON_FA_ARROW_UP, VK_UP, 1 }, { ICON_FA_ARROW_DOWN, VK_DOWN, 1 },
                      { ICON_FA_ARROW_RIGHT, VK_RIGHT, 1 } };
const Cap kRow6[] = { { "LMB", VK_LBUTTON, 3 }, { "RMB", VK_RBUTTON, 3 }, { "MMB", VK_MBUTTON, 3 },
                      { "M4", VK_XBUTTON1, 3 }, { "M5", VK_XBUTTON2, 3 } };

bool IsMouseVk(int vk) {
    return vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON || vk == VK_XBUTTON1 || vk == VK_XBUTTON2;
}

class KeyboardLesson final : public Lesson {
public:
    KeyboardLesson() : Lesson(kInfo) {}

    ui::Status GetStatus(const App&, const P1Frame& f) const override {
        if (!f.viewport.valid)
            return Reason(ui::Tone::Warn, "No game window: keys are sampled only while the app knows the game window.");
        if (f.snapshot->flags & P1_SNAP_INPUT_FOCUSED)
            return Reason(ui::Tone::Info, "A game text field (chat) has the keyboard: P1_SNAP_INPUT_FOCUSED is on, so "
                                          "keys_down is all zero.");
        if (!f.viewport.game_foreground)
            return Reason(ui::Tone::Info, "The game is not the foreground window, so keys_down is all zero. Click into "
                                          "the game and press keys: they light up here.");
        // Shift, Ctrl and Alt come twice (the generic code and the side's code): count each key once.
        int down = 0;
        for (int vk = 1; vk < 256; ++vk)
            if (vk != VK_SHIFT && vk != VK_CONTROL && vk != VK_MENU) down += m_keys.Down(vk) ? 1 : 0;
        return Live("keys_down is sampled; %d key%s down now. %s", down, down == 1 ? "" : "s",
                    m_hotkey ? Fmt("%s toggles the %s layer.", VkName(m_hotkey).c_str(), LayerOf(m_target).name).c_str()
                             : "No hotkey is set.");
    }

    // [snippet:keyboard]
    // Key edges, once per tick. keys_down is all zero while the game is not in
    // front or a game text field has focus, so nothing below fires while the
    // player types in chat. React to Pressed (an edge), not to Down (a level).
    void Tick(App& app, const P1Frame& f) override {
        m_keys.Update(f.viewport);
        for (int vk = 1; vk < 256; ++vk) {
            if (m_keys.Pressed(vk)) {
                ++m_pressed;
                m_lastVk = vk;
                m_lastUs = f.now_us;
                if (m_capturing && !IsMouseVk(vk)) {   // "press a key to bind it"
                    m_hotkey = vk;
                    m_capturing = false;
                    app.MarkDirty();
                    return;
                }
            }
            if (m_keys.Released(vk)) ++m_released;
        }
        if (m_hotkey && !m_capturing && m_keys.Pressed(m_hotkey)) {
            app.SetLayer(m_target, !app.LayerOn(m_target));
            ++m_hotkeyHits;
        }
    }
    // [/snippet]

    void Draw(App& app, const P1Frame& f) override {
        DrawKeyboard();
        DrawCounters(f);
        DrawHotkey(app);
    }

    void Load(App&, const nlohmann::json& j) override {
        m_hotkey = js::Int(j, "hotkey", VK_F7, 0, 254);
        if (IsMouseVk(m_hotkey)) m_hotkey = 0;
        const std::string target = js::String(j, "toggles", "hud", 32);
        for (size_t i = 0; i < kLayerCount; ++i)
            if (target == Layers()[i].id) m_target = static_cast<LayerId>(i);
    }
    void Save(const App&, nlohmann::json& j) const override {
        j["hotkey"] = m_hotkey;
        j["toggles"] = LayerOf(m_target).id;
    }

private:
    void DrawKeyboard() const {
        ui::SectionTitle(ICON_FA_KEYBOARD, "keys_down, live");
        const ImGuiStyle& st = ImGui::GetStyle();
        const float gap = (std::max)(2.0f, st.ItemSpacing.x * 0.5f);
        // A row of U units is unit * U + gap * (U - 1) wide, whatever its keys: 15 units for every row.
        const float unit = std::floor((std::min)(ui::Em(2.5f),
                                                 (ImGui::GetContentRegionAvail().x - gap * (kRowUnits - 1.0f)) / kRowUnits));
        const float rounding = st.FrameRounding + 2.0f;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        // Each row is drawn with the draw list, then one Dummy reserves its rectangle.
        auto row = [&](const Cap* caps, size_t n) {
            const ImVec2 o = ImGui::GetCursorScreenPos();
            float x = o.x;
            for (size_t i = 0; i < n; ++i) {
                const Cap& c = caps[i];
                if (!c.label) {   // a gap between key groups
                    x += (unit + gap) * c.width;
                    continue;
                }
                const ImVec2 a(x, o.y), b(x + unit * c.width + gap * (c.width - 1.0f), o.y + unit);
                const bool down = m_keys.Down(c.vk);
                dl->AddRectFilled(a, b, down ? ui::ToneU32(ui::Tone::Accent, 0.85f) : ImGui::GetColorU32(ImGuiCol_FrameBg),
                                  rounding);
                if (down) dl->AddRect(a, b, ImGui::GetColorU32(ImGuiCol_Text), rounding);   // outlines cost vertices: only where it matters
                const ImVec2 ts = ImGui::CalcTextSize(c.label);
                const ImU32 text = down ? ImGui::GetColorU32(ImGuiCol_WindowBg) : ImGui::GetColorU32(ImGuiCol_Text);
                dl->AddText(ImVec2(a.x + (b.x - a.x - ts.x) * 0.5f, a.y + (unit - ts.y) * 0.5f), text, c.label);
                x = b.x + gap;
            }
            ImGui::Dummy(ImVec2(x - o.x - gap, unit));
        };
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(st.ItemSpacing.x, gap));   // rows sit `gap` apart
        row(kRow0, std::size(kRow0));
        row(kRow1, std::size(kRow1));
        row(kRow2, std::size(kRow2));
        row(kRow3, std::size(kRow3));
        row(kRow4, std::size(kRow4));
        row(kRow5, std::size(kRow5));
        ImGui::Dummy(ImVec2(0.0f, gap));
        row(kRow6, std::size(kRow6));
        ImGui::PopStyleVar();
    }

    void DrawCounters(const P1Frame& f) const {
        if (ui::BeginKv("counts", { "pressed / released", "last pressed", "also down" })) {
            ui::KvText("pressed / released", "%llu / %llu (edges seen by KeyEdge since the plugin started)",
                       static_cast<unsigned long long>(m_pressed), static_cast<unsigned long long>(m_released));
            if (m_lastVk)
                ui::KvText("last pressed", "%s, %s ago", VkName(m_lastVk).c_str(), FormatSpanUs(f.now_us - m_lastUs).c_str());
            else
                ui::KvText("last pressed", "-");
            std::string others;
            for (int vk = 1; vk < 256; ++vk)
                if (m_keys.Down(vk) && !OnKeyboard(vk)) others += (others.empty() ? "" : ", ") + VkName(vk);
            ui::KvText("also down", "%s", others.empty() ? "-" : others.c_str());
            ui::EndKv();
        }
    }

    void DrawHotkey(App& app) {
        ui::SectionTitle(ICON_FA_BOLT, "A plugin hotkey");
        if (ui::BeginForm("hotkey", { "Hotkey", "Toggles", "Pressed" })) {
            ui::FormRow("Hotkey", "Click, then press a key IN THE GAME: keys are sampled only while the game is in "
                                  "front. Saved in settings.json.");
            const std::string label = m_capturing ? std::string(ICON_FA_KEYBOARD "  press a key in the game...")
                                                  : (m_hotkey ? VkName(m_hotkey) : std::string("(none)"));
            // A button that looks like the other fields of the form: the frame colours, the text on the left.
            const ImGuiStyle& st = ImGui::GetStyle();
            ImGui::PushStyleColor(ImGuiCol_Button, st.Colors[ImGuiCol_FrameBg]);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, st.Colors[ImGuiCol_FrameBgHovered]);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, st.Colors[ImGuiCol_FrameBgActive]);
            ImGui::PushStyleColor(ImGuiCol_Text, m_capturing ? ui::ToneColor(ui::Tone::Warn) : st.Colors[ImGuiCol_Text]);
            ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.0f, 0.5f));
            if (ImGui::Button((label + "##bind").c_str(), ImVec2(-FLT_MIN, 0.0f))) m_capturing = !m_capturing;
            ImGui::PopStyleVar();
            ImGui::PopStyleColor(4);
            ui::FormRow("Toggles", "The overlay layer the hotkey switches on and off.");
            if (ImGui::BeginCombo("##target", LayerOf(m_target).name)) {
                for (size_t i = 0; i < kLayerCount; ++i)
                    if (ImGui::Selectable(Layers()[i].name, static_cast<size_t>(m_target) == i)) {
                        m_target = static_cast<LayerId>(i);
                        app.MarkDirty();
                    }
                ImGui::EndCombo();
            }
            ui::KvText("Pressed", "%llu times", static_cast<unsigned long long>(m_hotkeyHits));
            ui::EndForm();
        }
        if (m_hotkey && ImGui::Button(ICON_FA_XMARK "  Clear the hotkey")) {
            m_hotkey = 0;
            app.MarkDirty();
        }
        ui::Note("A plugin's hotkey is read, not owned: the game and the app see the same key press. Pick a key "
                 "nothing else uses. (The input arbiter never PRESSES a key bound to one of the app's hotkeys.)");
    }

    static bool OnKeyboard(int vk) {
        // Drawn already: Shift as its two sides; either side of Ctrl / Alt lights the generic cap.
        if (vk == VK_SHIFT || vk == VK_LCONTROL || vk == VK_RCONTROL || vk == VK_LMENU || vk == VK_RMENU) return true;
        auto in = [vk](const Cap* b, const Cap* e) { return std::any_of(b, e, [vk](const Cap& c) { return c.vk == vk; }); };
        return in(std::begin(kRow0), std::end(kRow0)) || in(std::begin(kRow1), std::end(kRow1)) ||
               in(std::begin(kRow2), std::end(kRow2)) || in(std::begin(kRow3), std::end(kRow3)) ||
               in(std::begin(kRow4), std::end(kRow4)) || in(std::begin(kRow5), std::end(kRow5)) ||
               in(std::begin(kRow6), std::end(kRow6));
    }

    p1::KeyEdge m_keys;
    int m_hotkey = VK_F7;
    LayerId m_target = LayerId::Hud;
    bool m_capturing = false;
    uint64_t m_pressed = 0, m_released = 0, m_hotkeyHits = 0;
    int m_lastVk = 0;
    int64_t m_lastUs = 0;
};

}  // namespace

std::unique_ptr<Lesson> MakeKeyboardLesson() { return std::make_unique<KeyboardLesson>(); }

}  // namespace ex
