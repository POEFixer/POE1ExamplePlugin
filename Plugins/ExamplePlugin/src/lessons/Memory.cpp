// Lesson "memory": read-only game memory reads: the PE header, the snapshot's anchors, a hex
// viewer and a pointer chain. No game offset is hard-coded anywhere in this file.
#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cstring>
#include <ctime>

#include "JsonUtil.h"
#include "lessons/Common.h"

namespace ex {

namespace {

constexpr int64_t kRefreshUs = 500'000;   // re-read twice a second while this page is open
constexpr int kHexBytes = 256;

constexpr LessonInfo kInfo{
    .id = "memory",
    .group = Group::Advanced,
    .icon = ICON_FA_MEMORY,
    .title = "Memory",
    .summary = "Read the game's memory directly, read-only, for what the snapshot does not carry.",
    .what = "p1::Memory::Read<T>, ReadBytes, ReadBatch and ReadPointerChain read the game through a read-only "
            "handle the host holds (PROCESS_VM_READ plus query rights; no write or operation rights). The snapshot's anchors (module_base, "
            "ingame_state, ...) are the app's own entry points, re-derived after every game patch.",
    .when = "Only for data the snapshot lacks. Prefer typed snapshot fields: they keep working across patches.",
    .limits = "Needs \"memory\" in plugin.json (shown to the user, no consent switch). 0 / false without it, while "
              "detached, and in WGC mode. One request at most 16 MiB; a partial read reports what it got. Offsets "
              "inside game objects change with game patches: never hard-code one in a published plugin without a "
              "fallback, and treat every value read as untrusted (it can be torn or stale).",
    .snippet = "memory",
};

struct PeInfo {
    bool read = false, mz = false, pe = false;
    uint16_t machine = 0, sections = 0;
    uint32_t timestamp = 0, imageSize = 0;
    int64_t us = 0;
};

// [snippet:memory]
// A read that survives every game patch: the PE headers at the module base.
// "MZ" at offset 0, e_lfanew at 0x3C, "PE\0\0" at e_lfanew. None of this is an
// offset into a game object, so none of it rots. Check what you read before
// you follow it: e_lfanew comes from another process's memory.
PeInfo CheckPeHeader(uint64_t moduleBase) {
    PeInfo r;
    IMAGE_DOS_HEADER dos{};
    if (!moduleBase || !p1::Memory::Read(moduleBase, dos)) return r;     // all 64 bytes, or false
    r.read = true;
    r.mz = dos.e_magic == IMAGE_DOS_SIGNATURE;                            // 'MZ'
    if (!r.mz || dos.e_lfanew <= 0 || dos.e_lfanew > 4096) return r;
    IMAGE_NT_HEADERS64 nt{};
    if (!p1::Memory::Read(moduleBase + uint64_t(dos.e_lfanew), nt)) return r;
    r.pe = nt.Signature == IMAGE_NT_SIGNATURE;                            // 'PE\0\0'
    r.machine = nt.FileHeader.Machine;                                    // 0x8664 = x64
    r.sections = nt.FileHeader.NumberOfSections;
    r.timestamp = nt.FileHeader.TimeDateStamp;                            // link time: changes per game build
    r.imageSize = nt.OptionalHeader.SizeOfImage;
    return r;
}
// [/snippet]

struct Anchor {
    const char* name;
    uint64_t P1Snapshot::*field;
};

constexpr Anchor kAnchors[] = {
    { "module_base", &P1Snapshot::module_base },   { "ingame_state", &P1Snapshot::ingame_state },
    { "ingame_data", &P1Snapshot::ingame_data },   { "ingame_ui", &P1Snapshot::ingame_ui },
    { "server_data", &P1Snapshot::server_data },   { "local_player", &P1Snapshot::local_player },
    { "entity_list", &P1Snapshot::entity_list },   { "camera_ptr", &P1Snapshot::camera_ptr },
};
constexpr size_t kAnchorCount = std::size(kAnchors);

// A user-mode address a pointer could plausibly hold on x64 Windows.
bool Plausible(uint64_t p) { return p >= 0x10000 && p < 0x00007FFF'FFFF0000ull; }

// Every address and qword on this page in one format: 0x and 16 digits, so columns line up.
std::string Hex64(uint64_t v) { return Fmt("0x%016llX", static_cast<unsigned long long>(v)); }

std::string BuildTime(uint32_t stamp) {
    const std::time_t t = static_cast<std::time_t>(stamp);
    std::tm tm{};
    if (gmtime_s(&tm, &t) != 0) return "?";
    char buf[32];
    return std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M UTC", &tm) ? std::string(buf) : std::string("?");
}

// Hex offsets separated by spaces or commas: "0x10, 28 0x8".
bool ParseOffsets(const char* text, std::vector<uint32_t>& out) {
    out.clear();
    std::string token;
    auto flush = [&]() {
        if (token.empty()) return true;
        uint64_t v = 0;
        if (!ParseHex(token, v) || v > 0xFFFFFFFFull || out.size() >= 16) return false;
        out.push_back(static_cast<uint32_t>(v));
        token.clear();
        return true;
    };
    for (const char* c = text; *c; ++c) {
        if (*c == ' ' || *c == ',' || *c == '\t') {
            if (!flush()) return false;
        } else {
            token.push_back(*c);
        }
    }
    return flush();
}

class MemoryLesson final : public Lesson {
public:
    MemoryLesson() : Lesson(kInfo) {}

    ui::Status GetStatus(const App&, const P1Frame& f) const override {
        const P1HostApi* h = p1::Host();
        if (!h || !(h->permissions & P1_PERM_MEMORY) || !p1::HasPermission(f, P1_PERM_MEMORY))
            return Reason(ui::Tone::Error, "Needs the \"memory\" permission in plugin.json.");
        if (auto gap = GameDataGap(*f.snapshot, Need::Attached))
            return Reason(gap->tone, "%s Memory reads return 0 until then.", gap->text.c_str());
        if (!f.snapshot->module_base) return Reason(ui::Tone::Info, "Attached, but module_base is 0 (unknown) this tick.");
        if (!m_everRead) return Reason(ui::Tone::Info, "Reading the PE header at module_base...");
        if (!m_pe.read) return Reason(ui::Tone::Warn, "module_base is known, but reading it failed (a read returned 0).");
        return Live("module_base reads %s and %s: machine 0x%04X, %u sections, linked %s.", m_pe.mz ? "MZ" : "no MZ",
                    m_pe.pe ? "PE" : "no PE", unsigned(m_pe.machine), unsigned(m_pe.sections), BuildTime(m_pe.timestamp).c_str());
    }

    void Draw(App& app, const P1Frame& f) override {
        const P1Snapshot& s = *f.snapshot;
        if (!p1::HasPermission(f, P1_PERM_MEMORY)) {
            ui::TextDim("Add \"memory\" to \"permissions\" in plugin.json to try this page.");
            return;
        }
        const bool refresh = f.now_us - m_readUs >= kRefreshUs;
        if (refresh) {
            m_readUs = f.now_us;
            ReadAll(s);
            m_everRead = true;
        }
        DrawPe(s);
        DrawAnchors(s);
        DrawHex(app, s);
        DrawChain(app);
    }

    void Load(App&, const nlohmann::json& j) override {
        CopyText(m_addr, js::String(j, "address", "", sizeof m_addr - 1));
        CopyText(m_offsets, js::String(j, "offsets", "", sizeof m_offsets - 1));
        m_chainBase = js::Int(j, "chainBase", 0, 0, int(kAnchorCount) - 1);
    }
    void Save(const App&, nlohmann::json& j) const override {
        j["address"] = m_addr;
        j["offsets"] = m_offsets;
        j["chainBase"] = m_chainBase;
    }

private:
    template <size_t N>
    static void CopyText(char (&dst)[N], const std::string& src) {
        const size_t n = (std::min)(src.size(), N - 1);
        std::memcpy(dst, src.data(), n);
        dst[n] = '\0';
    }

    void ReadAll(const P1Snapshot& s) {
        int64_t t0 = p1::NowUs();
        m_pe = CheckPeHeader(s.module_base);
        m_pe.us = p1::NowUs() - t0;

        // One host call for all eight anchors: each request gets its own bytes_read.
        P1ReadRequest reqs[kAnchorCount] = {};
        for (size_t i = 0; i < kAnchorCount; ++i) {
            m_anchorValue[i] = 0;
            reqs[i] = { s.*kAnchors[i].field, &m_anchorValue[i], 8, 0 };
        }
        t0 = p1::NowUs();
        m_anchorComplete = p1::Memory::ReadBatch(reqs, uint32_t(kAnchorCount));
        m_anchorUs = p1::NowUs() - t0;
        for (size_t i = 0; i < kAnchorCount; ++i) m_anchorRead[i] = reqs[i].bytes_read;

        if (!m_addr[0] && s.module_base) CopyText(m_addr, Fmt("%llX", static_cast<unsigned long long>(s.module_base)));
        uint64_t addr = 0;
        m_hexValid = ParseHex(m_addr, addr);
        m_hexAddr = addr;
        t0 = p1::NowUs();
        m_hexRead = m_hexValid ? p1::Memory::ReadBytes(addr, m_hex, kHexBytes) : 0;
        m_hexUs = p1::NowUs() - t0;

        ReadChain(s);
    }

    void ReadChain(const P1Snapshot& s) {
        m_steps.clear();
        m_chainError.clear();
        std::vector<uint32_t> offsets;
        if (!ParseOffsets(m_offsets, offsets)) {
            m_chainError = "Offsets: up to 16 hex numbers, e.g. \"0x10 0x28\".";
            return;
        }
        // The same walk as p1::Memory::ReadPointerChain(base, { ... }, out), one step at a time.
        uint64_t p = s.*kAnchors[size_t(m_chainBase)].field;
        for (const uint32_t off : offsets) {
            Step st{ p + off, 0, false };
            st.ok = p != 0 && p1::Memory::Read(p + off, st.value);
            m_steps.push_back(st);
            if (!st.ok || !st.value) return;
            p = st.value;
        }
        m_chainEnd = p;
        m_endRead = p ? p1::Memory::ReadBytes(p, m_endBytes, sizeof m_endBytes) : 0;
    }

    void DrawPe(const P1Snapshot& s) {
        ui::SectionTitle(ICON_FA_SHIELD_HALVED, "A patch-proof read: the PE header");
        if (ui::BeginKv("pe", { "module_base", "MZ / PE", "machine", "sections", "linked", "image size", "read time" })) {
            ui::KvText("module_base", "%s", Hex64(s.module_base).c_str());
            if (!m_pe.read) {
                ui::KvTone("MZ / PE", ui::Tone::Warn, "not read (no module_base, or the read returned 0)");
                ui::EndKv();
                return;
            }
            ui::KvTone("MZ / PE", m_pe.mz && m_pe.pe ? ui::Tone::Ok : ui::Tone::Warn, "%s / %s", m_pe.mz ? "MZ" : "no MZ",
                       m_pe.pe ? "PE\\0\\0" : "no PE");
            if (m_pe.pe) {
                ui::KvText("machine", "0x%04X%s", unsigned(m_pe.machine), m_pe.machine == 0x8664 ? " (x64)" : "");
                ui::KvText("sections", "%u", unsigned(m_pe.sections));
                ui::KvText("linked", "%s (the game build: it changes with every patch)", BuildTime(m_pe.timestamp).c_str());
                ui::KvText("image size", "%s", FormatBytes(m_pe.imageSize).c_str());
            }
            ui::KvText("read time", "%s for two reads", FormatCostUs(double(m_pe.us)).c_str());
            ui::EndKv();
        }
    }

    void DrawAnchors(const P1Snapshot& s) {
        ui::SectionTitle(ICON_FA_ANCHOR, "The anchors, in one ReadBatch");
        ui::TextDim("%u of %zu requests complete in %s. First qword of each object: a C++ object starts "
                            "with its vtable, which points into the game's module.",
                            m_anchorComplete, kAnchorCount, FormatCostUs(double(m_anchorUs)).c_str());
        const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                                      ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoSavedSettings;
        if (!ImGui::BeginTable("anchors", 3, flags)) return;
        ImGui::TableSetupColumn("Anchor", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthStretch, 1.4f);
        ImGui::TableSetupColumn("First qword (hex)", ImGuiTableColumnFlags_WidthStretch, 1.8f);
        ImGui::TableHeadersRow();
        const uint64_t lo = s.module_base, hi = s.module_base + m_pe.imageSize;
        for (size_t i = 0; i < kAnchorCount; ++i) {
            const uint64_t addr = s.*kAnchors[i].field;
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(kAnchors[i].name);
            ImGui::TableNextColumn();
            if (addr) ImGui::TextUnformatted(Hex64(addr).c_str());
            else ImGui::TextDisabled("0 (unknown)");
            ImGui::TableNextColumn();
            if (m_anchorRead[i] != 8) {
                ImGui::TextDisabled("%s", addr ? "unreadable" : "-");
                continue;
            }
            const uint64_t v = m_anchorValue[i];
            const bool inModule = m_pe.imageSize && v >= lo && v < hi;
            ImGui::TextUnformatted(Hex64(v).c_str());
            if (inModule) {
                ImGui::SameLine();
                ImGui::TextColored(ui::ToneColor(ui::Tone::Info), "%s", "vtable?");   // a guess: Info, not Ok
                ImGui::SetItemTooltip("It points into the game's module (+0x%llX): very likely a vtable.",
                                      static_cast<unsigned long long>(v - lo));
            }
        }
        ImGui::EndTable();
    }

    void DrawHex(App& app, const P1Snapshot& s) {
        ui::SectionTitle(ICON_FA_TABLE_CELLS, "Hex viewer");
        if (ui::BeginForm("hexform", { "Address (hex)", "Jump to" })) {
            ui::FormRow("Address (hex)", "Any address in the game process. 256 bytes are read twice a second.");
            if (ImGui::InputText("##addr", m_addr, sizeof m_addr, ImGuiInputTextFlags_CharsHexadecimal)) {
                app.MarkDirty();
                m_readUs = 0;   // read again on the next frame
            }
            ui::FormRow("Jump to");
            if (ImGui::BeginCombo("##jump", "an anchor...")) {
                for (const Anchor& a : kAnchors) {
                    const uint64_t v = s.*a.field;
                    if (ImGui::Selectable(a.name) && v) {
                        CopyText(m_addr, Fmt("%llX", static_cast<unsigned long long>(v)));
                        app.MarkDirty();
                        m_readUs = 0;
                    }
                }
                ImGui::EndCombo();
            }
            ui::EndForm();
        }
        if (!m_hexValid) {
            ui::TextDim("Type a hexadecimal address.");
            return;
        }
        ui::TextDim("%u of %d bytes read in %s.", m_hexRead, kHexBytes, FormatCostUs(double(m_hexUs)).c_str());
        DrawHexRows();
    }

    // The byte columns are computed, not left to the proportional font: every byte has its own
    // column. The characters on the right are one string per row, so words stay readable.
    // Always 16 rows: 16 bytes a row when that fits, else 8 (the first 128 bytes).
    void DrawHexRows() const {
        constexpr int kRows = 16;
        float hexDigit = 0.0f;   // the widest of 0-9 A-F: a proportional font's glyphs differ
        for (const char* d = "0123456789ABCDEF"; *d; ++d) hexDigit = (std::max)(hexDigit, ImGui::CalcTextSize(d, d + 1).x);
        const float byteW = hexDigit * 2.0f + ui::Em(0.4f);
        float addrW = 0.0f;      // the widest address actually shown
        for (int row = 0; row < kRows; ++row)
            addrW = (std::max)(addrW, ImGui::CalcTextSize(Fmt("%016llX", static_cast<unsigned long long>(m_hexAddr + uint64_t(row) * 16u)).c_str()).x);
        addrW += ui::Em(1.0f);
        const float charW = ImGui::CalcTextSize("0").x;   // the text column's width, per byte, about
        const float gap = ui::Em(0.6f);
        const int perRow = addrW + 16.0f * (byteW + charW) + gap <= ImGui::GetContentRegionAvail().x ? 16 : 8;
        const float lh = std::floor(ImGui::GetTextLineHeight() * 1.15f);
        const float textX = addrW + float(perRow) * byteW + gap;
        const ImVec2 o = ImGui::GetCursorScreenPos();
        ImGui::Dummy(ImVec2(textX + float(perRow) * charW, lh * float(kRows)));   // the whole block, one item
        if (!ImGui::IsItemVisible()) return;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImU32 dim = ImGui::GetColorU32(ImGuiCol_TextDisabled), text = ImGui::GetColorU32(ImGuiCol_Text);
        for (int row = 0; row < kRows; ++row) {
            const float y = o.y + float(row) * lh;
            const uint64_t at = m_hexAddr + uint64_t(row) * uint64_t(perRow);
            dl->AddText(ImVec2(o.x, y), dim, Fmt("%016llX", static_cast<unsigned long long>(at)).c_str());
            char chars[17] = {};
            for (int i = 0; i < perRow; ++i) {
                const int k = row * perRow + i;
                const bool ok = uint32_t(k) < m_hexRead;
                dl->AddText(ImVec2(o.x + addrW + float(i) * byteW, y), ok && m_hex[k] ? text : dim,
                            ok ? Fmt("%02X", unsigned(m_hex[k])).c_str() : "??");
                chars[i] = ok && std::isprint(m_hex[k]) ? static_cast<char>(m_hex[k]) : '.';
            }
            dl->AddText(ImVec2(o.x + textX, y), text, chars);
        }
    }

    void DrawChain(App& app) {
        ui::SectionTitle(ICON_FA_LINK, "Pointer chain");
        ui::TextDim("p = base; then for each offset: p = *(uint64_t*)(p + offset). Exactly what "
                    "p1::Memory::ReadPointerChain(base, { ... }, out) does, shown step by step.");
        if (ui::BeginForm("chainform", { "Base", "Offsets (hex)" })) {
            ui::FormRow("Base");
            if (ImGui::BeginCombo("##base", kAnchors[size_t(m_chainBase)].name)) {
                for (int i = 0; i < int(kAnchorCount); ++i)
                    if (ImGui::Selectable(kAnchors[i].name, m_chainBase == i)) {
                        m_chainBase = i;
                        app.MarkDirty();
                        m_readUs = 0;
                    }
                ImGui::EndCombo();
            }
            ui::FormRow("Offsets (hex)", "Your own offsets, e.g. \"0 8\". This plugin hard-codes none: offsets inside "
                                         "game objects change with game patches.");
            if (ImGui::InputTextWithHint("##offsets", "e.g. 0 8", m_offsets, sizeof m_offsets)) {
                app.MarkDirty();
                m_readUs = 0;
            }
            ui::EndForm();
        }
        if (!m_chainError.empty()) {
            ui::StatusLine({ ui::Tone::Warn, m_chainError });
            return;
        }
        if (m_steps.empty()) {
            ui::TextDim("Enter one or more offsets to follow the chain.");
            return;
        }
        const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                                      ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoSavedSettings;
        if (ImGui::BeginTable("chain", 3, flags)) {
            ImGui::TableSetupColumn("Step", ImGuiTableColumnFlags_WidthStretch, 0.5f);
            ImGui::TableSetupColumn("Reads at", ImGuiTableColumnFlags_WidthStretch, 1.3f);
            ImGui::TableSetupColumn("Got", ImGuiTableColumnFlags_WidthStretch, 1.6f);
            ImGui::TableHeadersRow();
            for (size_t i = 0; i < m_steps.size(); ++i) {
                const Step& st = m_steps[i];
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::Text("%zu", i + 1);
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(Hex64(st.at).c_str());
                ImGui::TableNextColumn();
                if (!st.ok) ImGui::TextColored(ui::ToneColor(ui::Tone::Warn), "read failed");
                else if (!st.value) ImGui::TextColored(ui::ToneColor(ui::Tone::Warn), "null: the chain stops");
                else if (!Plausible(st.value))
                    ImGui::TextColored(ui::ToneColor(ui::Tone::Warn), "%s (not a plausible pointer)", Hex64(st.value).c_str());
                else ImGui::TextUnformatted(Hex64(st.value).c_str());
            }
            ImGui::EndTable();
        }
        const Step& last = m_steps.back();
        if (last.ok && last.value && m_endRead == sizeof m_endBytes) {
            uint64_t q = 0;
            uint32_t d = 0;
            float fl = 0.0f;
            std::memcpy(&q, m_endBytes, 8);
            std::memcpy(&d, m_endBytes, 4);
            std::memcpy(&fl, m_endBytes, 4);
            ui::TextDim("At the end, %s: qword %s, dword %u, float %g", Hex64(m_chainEnd).c_str(), Hex64(q).c_str(), d,
                        double(fl));
        }
    }

    struct Step {
        uint64_t at;
        uint64_t value;
        bool ok;
    };

    PeInfo m_pe;
    bool m_everRead = false;
    int64_t m_readUs = 0, m_anchorUs = 0, m_hexUs = 0;
    uint64_t m_anchorValue[kAnchorCount] = {};
    uint32_t m_anchorRead[kAnchorCount] = {};
    uint32_t m_anchorComplete = 0;
    char m_addr[32] = "";
    bool m_hexValid = false;
    uint64_t m_hexAddr = 0;
    unsigned char m_hex[kHexBytes] = {};
    uint32_t m_hexRead = 0;
    int m_chainBase = 0;
    char m_offsets[128] = "";
    std::vector<Step> m_steps;
    std::string m_chainError;
    uint64_t m_chainEnd = 0;
    unsigned char m_endBytes[8] = {};
    uint32_t m_endRead = 0;
};

}  // namespace

std::unique_ptr<Lesson> MakeMemoryLesson() { return std::make_unique<MemoryLesson>(); }

}  // namespace ex
