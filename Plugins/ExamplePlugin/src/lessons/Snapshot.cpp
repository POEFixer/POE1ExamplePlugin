// Lesson "snapshot": the header, every flag and who owns it, the server values and the arrays.
#include "lessons/Common.h"

namespace ex {

namespace {

constexpr LessonInfo kInfo{
    .id = "snapshot",
    .group = Group::GameData,
    .icon = ICON_FA_CAMERA,
    .title = "Snapshot",
    .summary = "The read-only copy of the game every callback gets: what is in it, how fresh it is, who fills what.",
    .what = "f.snapshot points at a flat, pointer-free P1Snapshot that the host copied into this process "
            "before the callback. Strings are offsets into a pool (p1::Str); arrays carry their own stride.",
    .when = "Read it in any callback; copy what you keep: it is valid only until the callback returns. "
            "Test the flags first: they say which parts are filled.",
    .limits = "Append-only: a field added later exists only when P1_SNAP_HAS(s, field); element structs only "
              "grow, so walk arrays by stride (p1::Entities), never as C arrays. Gated parts (buffs: AutoPot, "
              "map rectangles and the radar block: radar) are filled only while that app feature runs (the radar "
              "also while a plugin with the \"radar\" permission runs, as this one). At most 256 buffs, 128 "
              "items; a full 2 MiB slot drops entities and counts them in entity_truncated.",
    .snippet = "snapshot",
};

class SnapshotLesson final : public Lesson {
public:
    SnapshotLesson() : Lesson(kInfo) {}

    ui::Status GetStatus(const App&, const P1Frame& f) const override {
        const P1Snapshot& s = *f.snapshot;
        if (auto gap = GameDataGap(s, Need::Attached)) return *gap;
        return Live("frame %llu, %s old, %u entities, state %s.", static_cast<unsigned long long>(s.frame),
                    FormatSpanUs(f.now_us - s.publish_us).c_str(), s.entity_count, StateName(s.game_state));
    }

    void Draw(App&, const P1Frame& f) override {
        const P1Snapshot& s = *f.snapshot;
        DrawHeader(s, f);
        DrawFlags(s);
        DrawServer(s);
        DrawArrays(s);
    }

private:
    static void DrawHeader(const P1Snapshot& s, const P1Frame& f) {
        ui::SectionTitle(ICON_FA_LIST, "Header");
        if (ui::BeginKv("header", { "version", "header_bytes", "P1_SNAP_HAS", "size_bytes", "frame", "publish_seq",
                                    "capture age", "publish age", "worker period", "pid", "game_state" })) {
            ui::KvText("version", "%u (this plugin was built against %u)", s.version, P1_SNAPSHOT_VERSION);
            ui::KvText("header_bytes", "%u (sizeof(P1Snapshot) here: %zu; %s)", s.header_bytes, sizeof(P1Snapshot),
                       s.header_bytes >= sizeof(P1Snapshot) ? "every field this plugin knows is present"
                                                            : "fields past header_bytes are missing");
            // A field added after v1 is used only when the app's snapshot has it. Every v1 field is
            // always there; string_bytes is the last of them.
            ui::KvText("P1_SNAP_HAS", "P1_SNAP_HAS(&s, string_bytes) = %d. For a field added later: "
                                      "if (P1_SNAP_HAS(&s, new_field)) { use it } else { a fallback }",
                       P1_SNAP_HAS(&s, string_bytes) ? 1 : 0);
            ui::KvText("size_bytes", "%s (header + arrays + strings)", FormatBytes(s.size_bytes).c_str());
            ui::KvText("frame", "%llu (the game reader's tick counter)", static_cast<unsigned long long>(s.frame));
            ui::KvText("publish_seq", "%llu (a different value means new data)",
                       static_cast<unsigned long long>(s.publish_seq));
            ui::KvText("capture age", "%s", s.capture_us ? FormatSpanUs(f.now_us - s.capture_us).c_str() : "never captured");
            // Before the first snapshot the host hands every callback a placeholder: flags 0, publish_us 0.
            ui::KvText("publish age", "%s", s.publish_us ? FormatSpanUs(f.now_us - s.publish_us).c_str()
                                                         : "never published (flags 0, publish_us 0: no snapshot yet)");
            ui::KvText("worker period", "%.1f ms", double(s.worker_period_ms));
            ui::KvText("pid", "%u", s.pid);
            ui::KvText("game_state", "%d = %s", s.game_state, StateName(s.game_state));
            ui::EndKv();
        }
    }

    // The flags grouped by who sets them: a gated flag stays off until its app feature runs.
    static void DrawFlags(const P1Snapshot& s) {
        ui::SectionTitle(ICON_FA_FLAG, "Flags, by who sets them");
        ui::TextDim("flags = 0x%04X. Hover a flag for what it means.", s.flags);
        struct Owner {
            const char* name;
            uint32_t bits[8];   // the flags this owner sets; 0 ends the list
        };
        static constexpr Owner owners[] = {
            { "the game reader", { P1_SNAP_ATTACHED, P1_SNAP_ATTACHING, P1_SNAP_IN_GAME, P1_SNAP_LOADING,
                                   P1_SNAP_INPUT_FOCUSED, P1_SNAP_CAMERA_VALID, P1_SNAP_INVENTORY_VALID,
                                   P1_SNAP_WORKER_FAULT } },
            { "radar gate", { P1_SNAP_MAP_VALID, P1_SNAP_RADAR_VALID } },
            { "AutoPot gate", { P1_SNAP_BUFFS_VALID } },
            { "app state", { P1_SNAP_WGC_MODE, P1_SNAP_TRADE_BUSY, P1_SNAP_RECOVERING } },
        };
        if (!ui::BeginKv("flags", { "the game reader", "radar gate", "AutoPot gate", "app state" })) return;
        for (const Owner& o : owners) {
            ui::KvKeyChips(o.name);
            ui::Flow flow;
            for (const uint32_t bit : o.bits) {
                if (!bit) break;
                const BitName* b = nullptr;
                for (const BitName& n : SnapshotFlagNames())
                    if (n.bit == bit) b = &n;
                if (!b) continue;
                const bool on = (s.flags & bit) != 0;
                const bool gated = bit == P1_SNAP_BUFFS_VALID || bit == P1_SNAP_MAP_VALID || bit == P1_SNAP_RADAR_VALID;
                const ui::Tone tone = on ? ui::Tone::Ok : (gated ? ui::Tone::Info : ui::Tone::Neutral);
                const std::string text = Fmt("%s %s", on ? ICON_FA_CHECK : ICON_FA_MINUS, b->name);
                flow.Next(ui::ChipWidth(text.c_str()));
                ui::Chip(text.c_str(), tone, b->note);
            }
        }
        ui::EndKv();
    }

    static void DrawServer(const P1Snapshot& s) {
        ui::SectionTitle(ICON_FA_SIGNAL, "Server values");
        if (ui::BeginKv("server", { "league", "latency_ms", "gold", "time_in_game_ms", "network_state" })) {
            const char* league = p1::Str(s, s.league);
            ui::KvText("league", "%s", league[0] ? league : "(empty)");
            ui::KvText("latency_ms", "%d ms", s.latency_ms);
            ui::KvText("gold", "%s", FormatCount(uint64_t(s.gold < 0 ? 0 : s.gold)).c_str());
            ui::KvText("time_in_game_ms", "%s", FormatSpanUs(int64_t(s.time_in_game_ms) * 1000).c_str());
            ui::KvText("network_state", "%u (raw)", s.network_state);
            ui::EndKv();
        }
    }

    static void DrawArrays(const P1Snapshot& s) {
        ui::SectionTitle(ICON_FA_TABLE_CELLS, "Arrays");
        const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                                      ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoSavedSettings;
        if (ImGui::BeginTable("arrays", 4, flags)) {
            ImGui::TableSetupColumn("Array");
            ImGui::TableSetupColumn("Count");
            ImGui::TableSetupColumn("Stride");
            ImGui::TableSetupColumn("sizeof here");
            ui::TableHeadersRowAligned({ false, true, true, true });
            auto row = [](const char* name, uint32_t count, uint32_t stride, size_t mine) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(name);
                ImGui::TableNextColumn();
                ui::TextRight("%u", count);
                ImGui::TableNextColumn();
                ui::TextRight("%u", stride);
                ImGui::TableNextColumn();
                ui::TextRight("%zu", mine);
            };
            row("entities", s.entity_count, s.entity_stride, sizeof(P1Entity));
            row("buffs", s.buff_count, s.buff_stride, sizeof(P1Buff));
            row("inventory items", s.item_count, s.item_stride, sizeof(P1InventoryItem));
            ImGui::EndTable();
        }
        ui::TextDim("Strings: %s in the pool. entity_truncated: %u.", FormatBytes(s.string_bytes).c_str(),
                            s.entity_truncated);
        ui::Note("A stride larger than sizeof here means a newer app grew the struct: p1::Entities and "
                 "p1_entity_at still step correctly, and the fields you know keep their offsets.");
    }
};

}  // namespace

std::unique_ptr<Lesson> MakeSnapshotLesson() { return std::make_unique<SnapshotLesson>(); }

}  // namespace ex
