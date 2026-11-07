// Lesson "settings": settings.json with p1::Settings, the debounced save, and the .rejected rule.
#include <algorithm>

#include "lessons/Common.h"

namespace ex {

namespace {

constexpr LessonInfo kInfo{
    .id = "settings",
    .group = Group::Basics,
    .icon = ICON_FA_FLOPPY_DISK,
    .title = "Settings",
    .summary = "Keep settings in JSON files in your one writable folder, and never lose a user's file.",
    .what = "p1::Settings::Load / Save read and write JSON (nlohmann::json) in HostApi().config_dir. Save is "
            "atomic: it writes <name>.tmp, flushes it, then replaces the file with MoveFileExW.",
    .when = "Load in OnLoad; save a little after the last change (this plugin: 1 s, on its worker thread) "
            "and once more in OnUnload.",
    .limits = "File names: letters, digits, '_', '-' and '.'; 1 to 96 bytes. Load reads files up to 16 MiB. A "
              "file that cannot be read or parsed is renamed <name>.rejected and never overwritten. Load and "
              "Save share one mutex, so any thread may call them. Other plugins can read and write this folder "
              "too.",
    .snippet = "settings",
};

const char* LoadResultName(p1::Settings::LoadResult r) {
    switch (r) {
        case p1::Settings::LoadResult::Loaded:   return "Loaded";
        case p1::Settings::LoadResult::Missing:  return "Missing";
        case p1::Settings::LoadResult::Rejected: return "Rejected";
        case p1::Settings::LoadResult::Error:    return "Error";
        default:                                 return "?";
    }
}

// Every path the SDK hands out is UTF-8 and may hold Cyrillic or CJK: widen it and use the
// W APIs. std::filesystem::path(std::string) or an A API would garble or throw on it.
std::wstring ConfigPath(const wchar_t* name) {
    const P1HostApi* h = p1::Host();
    std::wstring dir = p1::Widen(h && h->config_dir ? h->config_dir : "");
    if (dir.empty()) return {};
    if (dir.back() != L'\\' && dir.back() != L'/') dir.push_back(L'\\');
    return dir + name;
}

struct FileRow {
    std::string name;
    uint64_t size = 0;
};

std::vector<FileRow> ListConfigFolder() {
    std::vector<FileRow> rows;
    const std::wstring pattern = ConfigPath(L"*");
    if (pattern.empty()) return rows;
    WIN32_FIND_DATAW fd{};
    HANDLE find = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
    if (find == INVALID_HANDLE_VALUE) return rows;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        rows.push_back({ p1::Narrow(std::wstring(fd.cFileName)), (uint64_t(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow });
    } while (rows.size() < 64 && FindNextFileW(find, &fd));
    FindClose(find);
    std::sort(rows.begin(), rows.end(), [](const FileRow& a, const FileRow& b) { return a.name < b.name; });
    return rows;
}

bool WriteRawFile(const std::wstring& path, std::string_view bytes) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool ok = WriteFile(f, bytes.data(), DWORD(bytes.size()), &written, nullptr) && written == bytes.size();
    CloseHandle(f);
    return ok;
}

enum class DemoOp : uint8_t { List, WriteValid, Corrupt, Load, Delete };

struct DemoResult {
    bool hasMessage = false;
    bool listed = true;              // `files` is a fresh listing (false: the job failed, keep the old one)
    ui::Status message;
    std::vector<FileRow> files;
};

// Runs on the WORKER thread: only files and its own data, nothing of the plugin's.
DemoResult RunDemoOp(DemoOp op) {
    DemoResult r;
    r.hasMessage = op != DemoOp::List;
    switch (op) {
        case DemoOp::WriteValid: {
            const nlohmann::json j = { { "greeting", "hello" }, { "count", 3 } };
            std::string error;
            r.message = p1::Settings::Save("demo.json", j, &error)
                            ? ui::Status{ ui::Tone::Ok, "demo.json written: valid JSON, saved atomically." }
                            : ui::Status{ ui::Tone::Error, "Save refused: " + error };
            break;
        }
        case DemoOp::Corrupt:
            r.message = WriteRawFile(ConfigPath(L"demo.json"), "{ \"greeting\": \"hel")
                            ? ui::Status{ ui::Tone::Warn, "demo.json now holds broken JSON. Load it next." }
                            : ui::Status{ ui::Tone::Error, "Could not write demo.json." };
            break;
        case DemoOp::Load: {
            nlohmann::json j;
            std::string error;
            const p1::Settings::LoadResult res = p1::Settings::Load("demo.json", j, &error);
            if (res == p1::Settings::LoadResult::Loaded)
                r.message = { ui::Tone::Ok, "Loaded: " + j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace) };
            else if (res == p1::Settings::LoadResult::Missing)
                r.message = { ui::Tone::Info, "Missing: there is no demo.json. Write one first." };
            else if (res == p1::Settings::LoadResult::Rejected)
                r.message = { ui::Tone::Warn, "Rejected (" + error + "). The broken file is now demo.json.rejected: "
                                              "kept for inspection, never overwritten. Defaults apply." };
            else
                r.message = { ui::Tone::Error, std::string("Error: ") + error };
            break;
        }
        case DemoOp::Delete:
            DeleteFileW(ConfigPath(L"demo.json").c_str());
            DeleteFileW(ConfigPath(L"demo.json.rejected").c_str());
            r.message = { ui::Tone::Info, "demo.json and demo.json.rejected deleted." };
            break;
        case DemoOp::List:
        default:
            break;
    }
    r.files = ListConfigFolder();
    return r;
}

class SettingsLesson final : public Lesson {
public:
    SettingsLesson() : Lesson(kInfo) {}

    ui::Status GetStatus(const App& app, const P1Frame& f) const override {
        const SettingsState& st = app.settings;
        if (st.loadResult == p1::Settings::LoadResult::Rejected)
            return Reason(ui::Tone::Warn, "settings.json was not valid JSON at start: it is kept as "
                                          "settings.json.rejected and the defaults are in use.");
        if (st.loadResult == p1::Settings::LoadResult::Error)
            return Reason(ui::Tone::Error, "settings.json could not be loaded: %s", st.loadError.c_str());
        return Live("settings.json %s at start; %s.",
                    st.loadResult == p1::Settings::LoadResult::Loaded ? "was loaded" : "did not exist (a first run: defaults)",
                    SaveState(st, f.now_us).c_str());
    }

    void Draw(App& app, const P1Frame& f) override {
        if (!m_listed) Post(app, DemoOp::List);
        DrawOwnFile(app, f);
        DrawRejectedDemo(app);
    }

    // Applies a demo result on the plugin thread (called from Worker::Drain).
    void Apply(DemoResult r) {
        m_busy = false;
        m_listed = true;
        if (r.hasMessage) m_message = std::move(r.message);
        if (r.listed) m_files = std::move(r.files);
    }

private:
    static std::string SaveState(const SettingsState& st, int64_t nowUs) {
        if (st.saving) return "a save is running on the worker thread";
        if (st.dirty) return "a change is waiting: it is written 1 s after the last change";
        if (st.savedUs) return "last saved " + FormatSpanUs(nowUs - st.savedUs) + " ago";
        return "nothing changed since";
    }

    void Post(App& app, DemoOp op) {
        if (m_busy) return;
        const bool posted = app.worker.Post(
            "settings demo",
            [op]() -> Worker::Done {
                DemoResult r = RunDemoOp(op);
                // The result goes back by value; the lesson is looked up again on the plugin thread.
                return [r = std::move(r)](App& a) mutable {
                    if (Lesson* l = a.Find(kInfo.id)) static_cast<SettingsLesson*>(l)->Apply(std::move(r));
                };
            },
            [](App& a, const std::string& error) {   // the job threw: report it, and the buttons work again
                DemoResult r;
                r.hasMessage = true;
                r.listed = false;
                r.message = { ui::Tone::Error, "The demo job failed on the worker: " + error };
                if (Lesson* l = a.Find(kInfo.id)) static_cast<SettingsLesson*>(l)->Apply(std::move(r));
            });
        m_busy = posted;
        if (!posted) m_message = { ui::Tone::Error, "The worker thread is not running." };
    }

    void DrawOwnFile(App& app, const P1Frame& f) {
        const SettingsState& st = app.settings;
        ui::SectionTitle(ICON_FA_FILE_CODE, "This plugin's settings.json");
        const P1HostApi* h = p1::Host();
        if (ui::BeginKv("own", { "Folder", "At start", "Now", "Saves" })) {
            ui::KvText("Folder", "%s", h && h->config_dir ? h->config_dir : "(none)");
            ui::KvText("At start", "%s%s%s", LoadResultName(st.loadResult), st.loadError.empty() ? "" : ": ",
                       st.loadError.c_str());
            ui::KvText("Now", "%s", SaveState(st, f.now_us).c_str());
            ui::KvText("Saves", "%u written, %u failed%s%s", st.saves, st.saveFailures,
                       st.saveError.empty() ? "" : "; last error: ", st.saveError.c_str());
            ui::EndKv();
        }
        ui::Flow flow;
        const char* save = ICON_FA_FLOPPY_DISK "  Save now";
        flow.Next(ui::ButtonWidth(save));
        if (ImGui::Button(save)) app.settings.saveNowRequested = true;
        const char* reload = ICON_FA_ROTATE "  Reload from disk";
        flow.Next(ui::ButtonWidth(reload));
        if (ImGui::Button(reload)) app.settings.reloadRequested = true;
        ui::Note("What is saved: the open lesson, every overlay layer with its colour and size, and each "
                 "lesson's own options under \"lessons\". \"layers\": {\"all\": true} is a developer switch "
                 "that turns every layer on once; the next save writes the single switches instead.");
    }

    void DrawRejectedDemo(App& app) {
        ui::SectionTitle(ICON_FA_TRIANGLE_EXCLAMATION, "The .rejected rule, step by step");
        ui::TextDim("A user's settings are never silently thrown away: a file the plugin cannot parse is moved "
                    "aside, and the plugin starts from its defaults. Try it on demo.json:");
        ui::Flow flow;
        struct Step { const char* label; DemoOp op; };
        static constexpr Step kSteps[] = {
            { "1. Write a valid demo.json", DemoOp::WriteValid },
            { "2. Corrupt it", DemoOp::Corrupt },
            { "3. Load it", DemoOp::Load },
            { ICON_FA_TRASH "  Clean up", DemoOp::Delete },
        };
        ImGui::BeginDisabled(m_busy);
        for (const Step& s : kSteps) {
            flow.Next(ui::ButtonWidth(s.label));
            if (ImGui::Button(s.label)) Post(app, s.op);
        }
        ImGui::EndDisabled();
        if (!m_message.text.empty()) ui::StatusLine(m_message);

        if (m_files.empty()) {
            ui::TextDim("Files in the folder: %s", m_listed ? "none yet" : "reading...");
            return;
        }
        // A table as wide as its columns: a name and a right-aligned size read as one row.
        float nameW = ImGui::CalcTextSize("demo.json.rejected").x;
        for (const FileRow& row : m_files) nameW = (std::max)(nameW, ImGui::CalcTextSize(row.name.c_str()).x);
        nameW = (std::min)(nameW + ui::Em(2.0f), ImGui::GetContentRegionAvail().x * 0.7f);
        const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoHostExtendX |
                                      ImGuiTableFlags_NoSavedSettings | ImGuiTableFlags_BordersInnerH;
        if (ImGui::BeginTable("files", 2, flags)) {
            ImGui::TableSetupColumn("Files in the folder", ImGuiTableColumnFlags_WidthFixed, nameW);
            ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, ui::Em(5.0f));
            ui::TableHeadersRowAligned({ false, true });
            for (const FileRow& row : m_files) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                const bool rejected = row.name.size() > 9 && row.name.ends_with(".rejected");
                if (rejected) ImGui::TextColored(ui::ToneColor(ui::Tone::Warn), "%s", row.name.c_str());
                else ImGui::TextUnformatted(row.name.c_str());
                ImGui::TableNextColumn();
                ui::TextRight("%s", FormatBytes(row.size).c_str());
            }
            ImGui::EndTable();
        }
    }

    bool m_busy = false, m_listed = false;
    ui::Status m_message;
    std::vector<FileRow> m_files;
};

}  // namespace

std::unique_ptr<Lesson> MakeSettingsLesson() { return std::make_unique<SettingsLesson>(); }

}  // namespace ex
