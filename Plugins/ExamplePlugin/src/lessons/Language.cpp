// Lesson "language": the app's UI language, P1_EV_LANGUAGE, and a string table with a fallback.
#include <array>
#include <string_view>

#include "JsonUtil.h"
#include "lessons/Common.h"

namespace ex {

namespace {

constexpr LessonInfo kInfo{
    .id = "language",
    .group = Group::Basics,
    .icon = ICON_FA_LANGUAGE,
    .title = "Language",
    .summary = "Show your panel in the user's language, and fall back to English for anything else.",
    .what = "HostApi().language is the app's UI language code: en, ru, de, es, fr, ja, ko, pt_BR or th. "
            "P1_EV_LANGUAGE says it changed; re-read it then (do not cache the pointer).",
    .when = "Pick your own strings by that code. Keep English as the fallback for a code you do not know.",
    .limits = "A newer app may add languages. Every script of the nine is in the font from the start "
              "(Cyrillic and Latin in Segoe UI; Thai, Japanese and Korean merged from Windows fonts), so any "
              "language renders whichever one is selected; the language at start only decides which CJK "
              "font wins for shared ideographs. No IME composition in v1.",
    .snippet = "language",
};

enum class Str : uint8_t { Greeting, Monsters, AreaLevel, Saved, Enabled, Copy, Count };
constexpr size_t kStrCount = static_cast<size_t>(Str::Count);
constexpr const char* kKeys[kStrCount] = { "greeting", "monsters", "area_level", "saved", "enabled", "copy" };

struct Language {
    const char* code;
    const char* name;   // in its own language
    std::array<const char*, kStrCount> text;
};

const Language kLanguages[] = {
    { "en", "English", { "Hello, exile!", "Monsters nearby", "Area level", "Settings saved", "Enabled", "Copy to clipboard" } },
    { "ru", "Русский", { "Привет, изгнанник!", "Монстры рядом", "Уровень области", "Настройки сохранены", "Включено", "Копировать в буфер обмена" } },
    { "de", "Deutsch", { "Hallo, Verbannter!", "Monster in der Nähe", "Gebietsstufe", "Einstellungen gespeichert", "Aktiviert", "In die Zwischenablage kopieren" } },
    { "es", "Español", { "¡Hola, exiliado!", "Monstruos cerca", "Nivel del área", "Ajustes guardados", "Activado", "Copiar al portapapeles" } },
    { "fr", "Français", { "Bonjour, exilé !", "Monstres à proximité", "Niveau de la zone", "Paramètres enregistrés", "Activé", "Copier dans le presse-papiers" } },
    { "ja", "日本語", { "こんにちは、エグザイル！", "近くのモンスター", "エリアレベル", "設定を保存しました", "有効", "クリップボードにコピー" } },
    { "ko", "한국어", { "안녕하세요, 망명자여!", "근처의 몬스터", "지역 레벨", "설정이 저장되었습니다", "사용", "클립보드에 복사" } },
    { "pt_BR", "Português (Brasil)", { "Olá, exilado!", "Monstros por perto", "Nível da área", "Configurações salvas", "Ativado", "Copiar para a área de transferência" } },
    { "th", "ไทย", { "สวัสดี ผู้ถูกเนรเทศ!", "มอนสเตอร์ใกล้เคียง", "เลเวลพื้นที่", "บันทึกการตั้งค่าแล้ว", "เปิดใช้งาน", "คัดลอกไปยังคลิปบอร์ด" } },
};

// [snippet:language]
// A string in the app's language, or in English when the code is unknown: a
// newer app may add languages, and the panel must stay readable in all of them.
const char* Tr(std::string_view language, Str id) {
    for (const Language& l : kLanguages)
        if (language == l.code) return l.text[static_cast<size_t>(id)];
    return kLanguages[0].text[static_cast<size_t>(id)];   // English
}

std::string_view AppLanguage() {
    const P1HostApi* h = p1::Host();       // re-read every time: the host swaps the pointer
    return (h && h->language) ? std::string_view(h->language) : std::string_view("en");
}
// [/snippet]

const Language* Known(std::string_view code) {
    for (const Language& l : kLanguages)
        if (code == l.code) return &l;
    return nullptr;
}

class LanguageLesson final : public Lesson {
public:
    LanguageLesson() : Lesson(kInfo) {}

    ui::Status GetStatus(const App&, const P1Frame&) const override {
        const std::string_view code = AppLanguage();
        const Language* l = Known(code);
        if (!l)
            return Reason(ui::Tone::Warn, "The app reports \"%.*s\", a language this plugin does not know: its "
                                          "strings fall back to English.", int(code.size()), code.data());
        return Live("the app is in %s (%s); %u language change%s since the plugin started.", l->name, l->code,
                    m_changes, m_changes == 1 ? "" : "s");
    }

    void Event(App&, const P1Event& e) override {
        if (e.type == P1_EV_LANGUAGE) ++m_changes;
    }

    void Draw(App& app, const P1Frame&) override {
        DrawPreview(app);
        DrawAllScripts();
        ui::Note("The host merges every script once, at start. If you ship your own strings, test them in "
                 "all nine languages: a missing glyph renders as '?'. Text a user types or pastes into your "
                 "panel arrives as UTF-8 through ImGui.");
    }

    void Load(App&, const nlohmann::json& j) override { m_preview = js::String(j, "preview", "", 16); }
    void Save(const App&, nlohmann::json& j) const override {
        if (!m_preview.empty()) j["preview"] = m_preview;
    }

private:
    static constexpr float kColumnWeights[3] = { 0.7f, 1.3f, 1.5f };   // both tables of the page

    void DrawPreview(App& app) {
        ui::SectionTitle(ICON_FA_LANGUAGE, "Your strings, with an English fallback");
        const std::string appCode(AppLanguage());
        // By default the right column follows the app; an English app would show English twice,
        // so it then shows Russian instead.
        const std::string automatic = appCode == "en" ? "ru" : appCode;
        const std::string shown = m_preview.empty() ? automatic : m_preview;
        const std::string autoLabel = appCode == "en" ? "ru (the app is in English)" : "the app's language (" + appCode + ")";
        if (ui::BeginForm("preview", { "Compare with" })) {
            ui::FormRow("Compare with", "Only picks the right-hand column. \"xx\" shows what a language code this "
                                        "plugin does not know gets: English.");
            const std::string current = m_preview.empty() ? autoLabel : m_preview;
            if (ImGui::BeginCombo("##lang", current.c_str())) {
                if (ImGui::Selectable(autoLabel.c_str(), m_preview.empty())) Set(app, "");
                for (const Language& l : kLanguages)
                    if (ImGui::Selectable(Fmt("%s – %s", l.code, l.name).c_str(), m_preview == l.code)) Set(app, l.code);
                if (ImGui::Selectable("xx – unknown: falls back to English", m_preview == "xx")) Set(app, "xx");
                ImGui::EndCombo();
            }
            ui::EndForm();
        }
        const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                                      ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoSavedSettings;
        if (ImGui::BeginTable("strings", 3, flags)) {
            // The same three weights as the table below, so both tables' columns line up.
            ImGui::TableSetupColumn("Key", ImGuiTableColumnFlags_WidthStretch, kColumnWeights[0]);
            ImGui::TableSetupColumn("en", ImGuiTableColumnFlags_WidthStretch, kColumnWeights[1]);
            ImGui::TableSetupColumn(shown.c_str(), ImGuiTableColumnFlags_WidthStretch, kColumnWeights[2]);
            ImGui::TableHeadersRow();
            for (size_t i = 0; i < kStrCount; ++i) {
                const Str id = static_cast<Str>(i);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextDisabled("%s", kKeys[i]);
                ImGui::TableNextColumn();
                ui::TextWrap(Tr("en", id));
                ImGui::TableNextColumn();
                ui::TextWrap(Tr(shown, id));
            }
            ImGui::EndTable();
        }
    }

    static void DrawAllScripts() {
        ui::SectionTitle(ICON_FA_FONT, "Every language, one line each");
        const ImGuiTableFlags flags = ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoSavedSettings;
        if (ImGui::BeginTable("scripts", 3, flags)) {
            ImGui::TableSetupColumn("code", ImGuiTableColumnFlags_WidthStretch, kColumnWeights[0]);
            ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthStretch, kColumnWeights[1]);
            ImGui::TableSetupColumn("text", ImGuiTableColumnFlags_WidthStretch, kColumnWeights[2]);
            for (const Language& l : kLanguages) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextDisabled("%s", l.code);
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(l.name);
                ImGui::TableNextColumn();
                ui::TextWrap(l.text[0]);
            }
            ImGui::EndTable();
        }
    }

    void Set(App& app, const char* code) {
        m_preview = code;
        app.MarkDirty();
    }

    std::string m_preview;   // "" = follow the app
    unsigned m_changes = 0;
};

}  // namespace

std::unique_ptr<Lesson> MakeLanguageLesson() { return std::make_unique<LanguageLesson>(); }

}  // namespace ex
