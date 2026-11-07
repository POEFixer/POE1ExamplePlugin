// Lesson "clipboard": copying through the app, and pasting into the panel.
#include <cfloat>
#include <string>

#include "lessons/Common.h"

namespace ex {

namespace {

constexpr int64_t kClipboardIntervalUs = 1'000'000;   // the app applies at most one request a second

constexpr LessonInfo kInfo{
    .id = "clipboard",
    .group = Group::Interaction,
    .icon = ICON_FA_CLIPBOARD,
    .title = "Clipboard",
    .summary = "Copy text for the user (a build link, a summary) and accept text they paste into your panel.",
    .what = "p1::SetClipboard(text) asks the app to put UTF-8 text on the clipboard; ImGui's own copy (Ctrl+C in "
            "a text field, ImGui::SetClipboardText) goes the same way. Ctrl+V in your panel pastes: the app sends "
            "its clipboard first.",
    .when = "In answer to the user: a Copy button in your panel. The sandbox cannot open a browser, so \"Copy "
            "link\" is how a plugin hands out a URL.",
    .limits = "The app applies a request only while the user is using this panel (it has focus, and the user's "
              "last mouse or keyboard input in it was within the last second), never while the app trades, at "
              "most once a second; the newest request wins. At most about 2 KiB of text.",
    .snippet = "clipboard",
};

std::string Summary(const P1Snapshot& s) {
    if (!(s.flags & P1_SNAP_ATTACHED)) return "SDK Example: not attached to the game.";
    uint32_t hostile = 0;
    for (const P1Entity& e : p1::Entities(s)) hostile += IsHostileAlive(e) ? 1u : 0u;
    const char* player = p1::Str(s, s.player.name);
    return Fmt("%s in %s (level %d, %s league): life %d/%d, %u entities, %u hostile monsters alive.",
               player[0] ? player : "The player", p1::Str(s, s.area.name), s.area.level, p1::Str(s, s.league),
               s.player.hp, s.player.hp_max, s.entity_count, hostile);
}

// [snippet:clipboard]
// A Copy button. The click is what makes it work: the app applies a clipboard
// request only while the user is using this panel, so call SetClipboard in
// answer to a click, never from OnTick or a timer.
bool CopyButton(const char* label, const std::string& text) {
    if (!ImGui::Button(label)) return false;
    p1::SetClipboard(text.c_str());      // the same as ImGui::SetClipboardText(text.c_str()) here
    return true;
}
// [/snippet]

class ClipboardLesson final : public Lesson {
public:
    ClipboardLesson() : Lesson(kInfo) {}

    ui::Status GetStatus(const App&, const P1Frame& f) const override {
        if (f.snapshot->flags & P1_SNAP_TRADE_BUSY)
            return Reason(ui::Tone::Warn, "The app's trade automation is running: clipboard requests made now are "
                                          "dropped; copy again when it stops.");
        if (!m_copiedUs) return Reason(ui::Tone::Info, "Nothing copied yet: press Copy below, then paste anywhere.");
        return Live("last copy request %s ago (%zu bytes).", FormatSpanUs(f.now_us - m_copiedUs).c_str(), m_copiedBytes);
    }

    void Draw(App&, const P1Frame& f) override {
        const P1Snapshot& s = *f.snapshot;
        ui::SectionTitle(ICON_FA_COPY, "Copy");
        const std::string summary = Summary(s);
        ui::BeginCard("##summary");
        ui::TextWrap(summary.c_str());
        ui::EndCard();
        ui::Flow flow;
        const char* copy = ICON_FA_COPY "  Copy this summary";
        flow.Next(ui::ButtonWidth(copy));
        if (CopyButton(copy, summary)) Remember(f, summary.size());
        const char* link = ICON_FA_LINK "  Copy a link";
        const std::string url = "https://www.pathofexile.com/trade";
        flow.Next(ui::ButtonWidth(link));
        if (CopyButton(link, url)) Remember(f, url.size());
        if (m_copiedUs) {
            const int64_t since = f.now_us - m_copiedUs;
            ui::TextDim("Requested %s ago. %s", FormatSpanUs(since).c_str(),
                                since < kClipboardIntervalUs ? "Another request now would wait for the 1 s limit."
                                                             : "The next request would be applied.");
        }

        ui::SectionTitle(ICON_FA_PASTE, "Paste");
        ui::TextDim("Click into the box and press Ctrl+V (or Shift+Insert): the app sends its clipboard into the panel "
                    "first, so ImGui's paste works as usual. Ctrl+A, Ctrl+C and Ctrl+X work too.");
        const ImVec2 at = ImGui::GetCursorScreenPos();
        ImGui::InputTextMultiline("##paste", m_paste, sizeof m_paste,
                                  ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * 5.0f + ImGui::GetStyle().FramePadding.y * 2.0f));
        // InputTextMultiline has no hint text: draw one while the box is empty and not being typed in.
        if (!m_paste[0] && !ImGui::IsItemActive()) {
            const ImVec2 pad = ImGui::GetStyle().FramePadding;
            ImGui::GetWindowDrawList()->AddText(ImVec2(at.x + pad.x, at.y + pad.y), ImGui::GetColorU32(ImGuiCol_TextDisabled),
                                                "Paste here: click, then Ctrl+V");
        }
        const size_t len = std::char_traits<char>::length(m_paste);
        ui::TextDim("%zu bytes of UTF-8 in the box.", len);
        ui::Note("Treat pasted text as untrusted input: parse it defensively, and cap its size.");
    }

private:
    void Remember(const P1Frame& f, size_t bytes) {
        m_copiedUs = f.now_us;
        m_copiedBytes = bytes;
    }

    int64_t m_copiedUs = 0;
    size_t m_copiedBytes = 0;
    char m_paste[2048] = "";
};

}  // namespace

std::unique_ptr<Lesson> MakeClipboardLesson() { return std::make_unique<ClipboardLesson>(); }

}  // namespace ex
