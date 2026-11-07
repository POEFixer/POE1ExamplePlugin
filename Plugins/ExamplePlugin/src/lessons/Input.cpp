// Lesson "input": asking the app for keyboard / mouse actions, with a SAFE demo (one mouse move).
#include <cmath>

#include "lessons/Common.h"

namespace ex {

namespace {

constexpr int64_t kArmWindowUs = 10'000'000;   // the demo waits this long for the game to come to the front
constexpr int64_t kDemoDelayUs = 3'000'000;    // three Wait(1000) actions before the move

constexpr LessonInfo kInfo{
    .id = "input",
    .group = Group::Interaction,
    .icon = ICON_FA_COMPUTER_MOUSE,
    .title = "Input requests",
    .summary = "A plugin never sends input itself: it asks the app, which acts only when it is safe.",
    .what = "p1::Input::Batch collects up to 32 actions (key down / up / press, mouse move / down / up / click, "
            "wait) and Submit() hands them to the app's input thread. It returns how many were queued; 0 = refused.",
    .when = "Only with all three: \"input\" in plugin.json, the user's \"Allow input\" in the Plugins tab, and an "
            "open gate (P1Frame::input_allowed). Check the result in a later snapshot: queued is not performed.",
    .limits = "The gate is re-checked before every action: the game window in front, no POE1Fixer window in front, "
              "no trade, stash or recovery automation. 256 actions queued at most. Refused one by one (the rest "
              "of the batch runs): keys bound to the app's hotkeys, Win / Sleep / browser / launch keys, "
              "Alt+Space, Alt+Tab, Alt+Esc, Alt+F4, Ctrl+Esc, a button sent as a key. Closing the gate instead "
              "(the queue is dropped, every held key and button released): a mouse target outside the client "
              "area, the 61st action within one second (waits do not count), SendInput failing, and consent "
              "turned off.",
    .snippet = "input",
};

// [snippet:input]
// The SAFE demo: three one-second waits, then one mouse move to the centre of
// the game's client area. No click and no key: nothing happens in the game,
// the cursor just moves. Coordinates are game CLIENT pixels.
uint32_t SubmitSafeDemo(const P1Viewport& vp) {
    if (!vp.valid || vp.client_w <= 0 || vp.client_h <= 0) return 0;
    p1::Input::Batch batch;
    batch.Wait(1000).Wait(1000).Wait(1000)                 // a Wait is at most 1000 ms
         .MouseMove(vp.client_w / 2, vp.client_h / 2);
    return batch.Submit();                                 // all or nothing; 0 = refused now
}
// [/snippet]

// Why Submit would be refused right now, in the order the app checks.
std::string WhyClosed(const P1Frame& f) {
    const P1HostApi* h = p1::Host();
    if (!h || !(h->permissions & P1_PERM_INPUT)) return "plugin.json does not ask for \"input\"";
    if (!p1::HasPermission(f, P1_PERM_INPUT)) return "\"Allow input\" is off for this plugin in the Plugins tab";
    if (!f.viewport.valid) return "no game window";
    if (f.viewport.app_foreground) return "a POE1Fixer window is in front (you are using the app)";
    if (!f.viewport.game_foreground) return "the game window is not in front";
    if (f.snapshot->flags & P1_SNAP_TRADE_BUSY) return "the app's trade or stash automation owns the mouse";
    if (f.snapshot->flags & P1_SNAP_RECOVERING) return "the app's recovery is running";
    return f.input_allowed ? "" : "the app's gate is closed (input_allowed is 0)";
}

class InputLesson final : public Lesson {
public:
    InputLesson() : Lesson(kInfo) {}

    ui::Status GetStatus(const App&, const P1Frame& f) const override {
        const P1HostApi* h = p1::Host();
        if (!h || !(h->permissions & P1_PERM_INPUT))
            return Reason(ui::Tone::Error, "Needs the \"input\" permission in plugin.json.");
        if (!p1::HasPermission(f, P1_PERM_INPUT))
            return Reason(ui::Tone::Warn, "\"Allow input\" is off for this plugin in the Plugins tab: P1_PERM_INPUT is "
                                          "missing from P1Frame::permissions, so every Submit returns 0.");
        if (m_armed)
            return Reason(ui::Tone::Info, "Armed: switch to the game window. The batch is sent the moment "
                                          "input_allowed turns 1.");
        if (!f.input_allowed)
            return Reason(ui::Tone::Info, "Consent is on; the gate is closed now: %s.", WhyClosed(f).c_str());
        return Live("the gate is open: Submit would be accepted.");
    }

    void Event(App&, const P1Event& e) override {
        if (e.type == P1_EV_INPUT_GATE) (e.a ? m_gateOpened : m_gateClosed)++;
        if (e.type == P1_EV_PERMISSIONS) ++m_permissionEvents;
    }

    // Sent from OnTick, not from the button: while the user clicks this panel the app's window
    // is in front, so the gate is closed. The demo waits for the game to come to the front.
    void Tick(App&, const P1Frame& f) override {
        if (!m_armed) return;
        if (f.now_us - m_armedUs > kArmWindowUs) {
            m_armed = false;
            m_result = { ui::Tone::Warn, "Not sent: the game did not come to the front within 10 s." };
            return;
        }
        if (!f.input_allowed) return;
        m_armed = false;
        const uint32_t queued = SubmitSafeDemo(f.viewport);
        if (queued) {
            m_sentUs = f.now_us;
            m_result = { ui::Tone::Ok, Fmt("Queued %u actions: the cursor moves to the centre of the game in 3 s.", queued) };
        } else {
            m_result = { ui::Tone::Warn, "Refused on the spot: " + WhyClosed(f) + "." };
        }
    }

    void Draw(App&, const P1Frame& f) override {
        DrawChecklist(f);
        DrawDemo(f);
        DrawRules();
    }

private:
    static void Check(bool ok, const char* text, const char* detail) {
        ImGui::TextColored(ui::ToneColor(ok ? ui::Tone::Ok : ui::Tone::Warn), "%s", ok ? ICON_FA_CIRCLE_CHECK : ICON_FA_CIRCLE_XMARK);
        ImGui::SameLine();
        ui::TextWrap(Fmt("%s %s", text, detail).c_str());
    }

    void DrawChecklist(const P1Frame& f) const {
        ui::SectionTitle(ICON_FA_LIST_CHECK, "The three conditions");
        const P1HostApi* h = p1::Host();
        const bool asked = h && (h->permissions & P1_PERM_INPUT);
        const bool consent = p1::HasPermission(f, P1_PERM_INPUT);
        Check(asked, "1. plugin.json lists \"input\".", asked ? "(it does)" : "(it does not: Submit always returns 0)");
        Check(consent, "2. The user ticked \"Allow input\" for this plugin.",
              consent ? "(on)" : "(off: the default; only the user can turn it on)");
        Check(f.input_allowed != 0, "3. The gate is open (P1Frame::input_allowed).",
              f.input_allowed ? "(open)" : ("(closed: " + WhyClosed(f) + ")").c_str());
        ui::TextDim("Gate events: opened %u, closed %u. Permission events: %u.", m_gateOpened, m_gateClosed,
                            m_permissionEvents);
    }

    void DrawDemo(const P1Frame& f) {
        ui::SectionTitle(ICON_FA_HAND_POINTER, "The safe demo");
        ui::TextDim("Waits 3 s (three Wait actions: one Wait is at most 1 s), then moves the mouse to the centre of "
                    "the game window. It never clicks and never presses a key.");
        const bool consent = p1::HasPermission(f, P1_PERM_INPUT);
        ImGui::BeginDisabled(!consent || m_armed);
        if (ImGui::Button(ICON_FA_PLAY "  Arm the demo, then switch to the game")) {
            m_armed = true;
            m_armedUs = f.now_us;
            m_result = {};
        }
        ImGui::EndDisabled();
        if (!consent) {
            ImGui::SameLine();
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("(needs \"Allow input\")");
        }
        if (m_armed) {
            const float left = 1.0f - float(f.now_us - m_armedUs) / float(kArmWindowUs);
            ui::Meter(left, ui::ToneU32(ui::Tone::Info, 0.40f), "waiting for the game to come to the front",
                      Fmt("%.0f s left", std::ceil(double(left) * double(kArmWindowUs) / 1e6)).c_str());
            if (ImGui::Button(ICON_FA_XMARK "  Disarm")) m_armed = false;
        } else if (m_sentUs && f.now_us - m_sentUs < kDemoDelayUs) {
            const float done = float(f.now_us - m_sentUs) / float(kDemoDelayUs);
            ui::Meter(done, ui::ToneU32(ui::Tone::Ok, 0.40f), "the app is running the three waits",
                      Fmt("%.1f of 3 s", double(f.now_us - m_sentUs) / 1e6).c_str());
        }
        if (!m_result.text.empty()) ui::StatusLine(m_result);
    }

    static void DrawRules() {
        ui::SectionTitle(ICON_FA_SHIELD_HALVED, "What the app refuses, and why");
        ui::TextDim("Refused one by one; the rest of the batch still runs:");
        ui::BulletWrapped("Any key bound to one of the app's own hotkeys: a plugin cannot trigger the app's features.");
        ui::BulletWrapped("The Windows keys, Sleep, and the browser and launch-application keys.");
        ui::BulletWrapped("System chords: Space, Tab, Esc or F4 while an Alt key is down (Alt+F4 would close the "
                          "game), and Esc while Ctrl is down.");
        ui::BulletWrapped("A mouse button sent as a key, or a key sent as a mouse button.");
        ui::TextDim("Closes the gate for this plugin: the rest of its queue is dropped and every key and button "
                    "it holds is released:");
        ui::BulletWrapped("A mouse target outside the client area, or over a window that covers the game there.");
        ui::BulletWrapped("The 61st action within one second (waits do not count), and SendInput failing.");
        ui::BulletWrapped("Trade, stash or recovery automation starting, the game leaving the front, or the user "
                          "turning consent off. While the gate is closed, Submit returns 0: nothing waits for it "
                          "to open again, and plugin input never delays the app's own work.");
        ui::Note("Refused actions are counted on this plugin's telemetry line (\"refused inputs\"). Never assume a "
                 "KeyDown is still held: a closed gate releases it.");
    }

    bool m_armed = false;
    int64_t m_armedUs = 0, m_sentUs = 0;
    ui::Status m_result;
    unsigned m_gateOpened = 0, m_gateClosed = 0, m_permissionEvents = 0;
};

}  // namespace

std::unique_ptr<Lesson> MakeInputLesson() { return std::make_unique<InputLesson>(); }

}  // namespace ex
