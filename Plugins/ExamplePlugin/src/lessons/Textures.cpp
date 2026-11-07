// Lesson "textures": procedural textures, a partial-update animation, and the texture budget.
#include <algorithm>
#include <cmath>

#include "JsonUtil.h"
#include "lessons/Common.h"

namespace ex {

namespace {

constexpr uint64_t kBudgetBytes = 64ull << 20;   // the app's cap per plugin, font atlases included
constexpr int kBudgetCount = 1024;
constexpr int kExtraStep = 16, kExtraMax = 256;   // the "create more" demo stays well inside the budget

ImU32 Rgba(float r, float g, float b) {
    auto c = [](float v) { return static_cast<int>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
    return IM_COL32(c(r), c(g), c(b), 255);   // IM_COL32 is R, G, B, A in memory: RGBA8, as Texture::Create wants
}

// A smooth palette over 0..1 (cosine bands), for the plasma.
ImU32 Palette(float t) {
    constexpr float k = 6.2831853f;
    return Rgba(0.5f + 0.5f * std::cos(k * (t + 0.00f)), 0.5f + 0.5f * std::cos(k * (t + 0.33f)),
                0.5f + 0.5f * std::cos(k * (t + 0.67f)));
}

ImU32 PlasmaPixel(int x, int y, float t) {
    const float fx = float(x), fy = float(y);
    const float dx = fx - 64.0f, dy = fy - 64.0f;
    const float v = std::sin(fx * 0.060f + t) + std::sin(fy * 0.050f - t * 1.3f) +
                    std::sin((fx + fy) * 0.040f + t * 0.7f) + std::sin(std::sqrt(dx * dx + dy * dy) * 0.08f - t * 2.0f);
    return Palette(v * 0.125f + 0.5f + t * 0.05f);
}

Tex Make(const std::vector<ImU32>& px, int w, int h) {
    Tex t;
    t.id = p1::Texture::Create(px.data(), w, h);   // 0 = refused (budget, or not the plugin thread)
    if (t.id) {
        t.w = w;
        t.h = h;
    }
    return t;
}

void Destroy(Tex& t) {
    if (t.id) p1::Texture::Destroy(t.id);
    t = Tex{};
}

}  // namespace

// ---- TextureSet (declared in App.h) ----------------------------------------------------------

bool TextureSet::EnsureBase() {
    if (gradient.id && checker.id && plasma.id) return true;
    if (baseFailed) return false;   // a refusal is not retried every tick: "Recreate" resets it
    if (!gradient.id) {
        std::vector<ImU32> px(256 * 8);
        for (int x = 0; x < 256; ++x)
            for (int y = 0; y < 8; ++y) px[size_t(y) * 256 + size_t(x)] = Palette(float(x) / 255.0f);
        gradient = Make(px, 256, 8);
    }
    if (!checker.id) {
        std::vector<ImU32> px(64 * 64);
        for (int y = 0; y < 64; ++y)
            for (int x = 0; x < 64; ++x)
                px[size_t(y) * 64 + size_t(x)] = ((x / 8 + y / 8) % 2) ? IM_COL32(235, 235, 240, 255) : IM_COL32(60, 64, 76, 255);
        checker = Make(px, 64, 64);
    }
    if (!plasma.id) {
        m_plasma.assign(size_t(kPlasmaSize) * kPlasmaSize, 0);
        for (int y = 0; y < kPlasmaSize; ++y)
            for (int x = 0; x < kPlasmaSize; ++x) m_plasma[size_t(y) * kPlasmaSize + size_t(x)] = PlasmaPixel(x, y, 0.0f);
        plasma = Make(m_plasma, kPlasmaSize, kPlasmaSize);
    }
    baseFailed = !(gradient.id && checker.id && plasma.id);
    if (baseFailed) ++refused;
    return !baseFailed;
}

// [snippet:textures]
// Animates the plasma by rewriting ONE band of rows per call: a partial
// Texture::Update of 128 x 32 pixels (16 KiB) instead of the whole 64 KiB.
// The app uploads at most about 2 MiB per rendered frame across all plugins,
// so small updates reach the screen sooner. Plugin thread only.
void TextureSet::AnimatePlasma(double seconds) {
    if (!plasma.id || m_plasma.size() != size_t(kPlasmaSize) * kPlasmaSize) return;
    const int y0 = m_band * kPlasmaBand;
    const float t = static_cast<float>(std::fmod(seconds, 1000.0));
    for (int y = y0; y < y0 + kPlasmaBand; ++y)
        for (int x = 0; x < kPlasmaSize; ++x) m_plasma[size_t(y) * kPlasmaSize + size_t(x)] = PlasmaPixel(x, y, t);
    const uint32_t* band = m_plasma.data() + size_t(y0) * kPlasmaSize;
    // Update(texture, x, y, w, h, pixels, pitch): pitch is the source row length in BYTES.
    if (p1::Texture::Update(plasma.id, 0, y0, kPlasmaSize, kPlasmaBand, band, kPlasmaSize * 4)) ++updates;
    else ++refused;
    m_band = (m_band + 1) % (kPlasmaSize / kPlasmaBand);
}
// [/snippet]

int TextureSet::AddExtras(int count) {
    std::vector<ImU32> px(32 * 32);
    int made = 0;
    for (int i = 0; i < count; ++i) {
        std::fill(px.begin(), px.end(), Palette(float(extras.size() % 16) / 16.0f));
        Tex t = Make(px, 32, 32);
        if (!t.id) {
            ++refused;
            break;
        }
        extras.push_back(t);
        ++made;
    }
    return made;
}

void TextureSet::DestroyExtras() {
    for (Tex& t : extras) Destroy(t);
    extras.clear();
}

void TextureSet::ReleaseAll() {
    DestroyExtras();
    Destroy(gradient);
    Destroy(checker);
    Destroy(plasma);
    m_plasma.clear();
    m_plasma.shrink_to_fit();
    baseFailed = false;
}

int TextureSet::LiveCount() const {
    return (gradient.id ? 1 : 0) + (checker.id ? 1 : 0) + (plasma.id ? 1 : 0) + static_cast<int>(extras.size());
}

uint64_t TextureSet::LiveBytes() const {
    uint64_t n = gradient.Bytes() + checker.Bytes() + plasma.Bytes();
    for (const Tex& t : extras) n += t.Bytes();
    return n;
}

// ---- the lesson -------------------------------------------------------------------------------

namespace {

constexpr LessonInfo kInfo{
    .id = "textures",
    .group = Group::Drawing,
    .icon = ICON_FA_IMAGE,
    .title = "Textures",
    .summary = "Make your own images: create, update in part, draw in the panel and on the overlay, destroy.",
    .what = "p1::Texture::Create(rgba, w, h) uploads RGBA8 pixels and returns an ImTextureID for ImGui::Image "
            "and ImDrawList::AddImage*; Update rewrites a rectangle of it; Destroy frees it.",
    .when = "Icons, generated images, anything ImGui cannot draw from shapes. Create once, update what "
            "changes, destroy in OnUnload.",
    .limits = "Plugin thread only (elsewhere Create returns 0). At most 4096 x 4096 each; at most 1,024 live "
              "textures and 64 MiB per plugin, ImGui's font atlases included; over a budget Create returns 0 "
              "and the plugin keeps running. The app uploads at most about 2 MiB per frame across all plugins.",
    .snippet = "textures",
};

class TexturesLesson final : public Lesson {
public:
    TexturesLesson() : Lesson(kInfo) {}

    ui::Status GetStatus(const App& app, const P1Frame&) const override {
        const TextureSet& t = app.textures;
        if (t.baseFailed)
            return Reason(ui::Tone::Warn, "Texture::Create returned 0: a budget is full, or the call was not on the "
                                          "plugin thread. Destroy the extras, then Recreate.");
        if (t.LiveCount() == 0)
            return Reason(ui::Tone::Info, "No textures yet: they are made the first time this page or the overlay's "
                                          "sprite layer needs them.");
        return Live("%d textures, %s; the plasma has had %llu partial updates.", t.LiveCount(),
                    FormatBytes(t.LiveBytes()).c_str(), static_cast<unsigned long long>(t.updates));
    }

    // Textures are made lazily, on the plugin thread, only while something shows them.
    void Tick(App& app, const P1Frame& f) override {
        const bool wanted = app.IsShown(this, f) || app.LayerOn(LayerId::Sprite);
        if (!wanted || !app.textures.EnsureBase()) return;
        if (m_animate) app.textures.AnimatePlasma(double(f.now_us) / 1e6);
    }

    void Draw(App& app, const P1Frame&) override {
        DrawBudget(app.textures);
        DrawImages(app);
        DrawControls(app);
    }

    void Unload(App& app) override { app.textures.ReleaseAll(); }

    void Load(App&, const nlohmann::json& j) override { m_animate = js::Bool(j, "animate", true); }
    void Save(const App&, nlohmann::json& j) const override { j["animate"] = m_animate; }

private:
    static void DrawBudget(const TextureSet& t) {
        ui::SectionTitle(ICON_FA_GAUGE_HIGH, "This plugin's textures against the budget");
        const int count = t.LiveCount();
        const uint64_t bytes = t.LiveBytes();
        if (ui::BeginForm("budget", { "Live textures", "Their pixels" })) {
            ui::FormRow("Live textures", "At most 1,024 at once; Create returns 0 past it.");
            ui::Meter(float(count) / float(kBudgetCount), ui::ToneU32(ui::Tone::Info, 0.40f),
                      Fmt("%d of %s", count, FormatCount(kBudgetCount).c_str()).c_str());
            ui::FormRow("Their pixels", "RGBA8: width x height x 4 bytes each, at most 64 MiB in all.");
            ui::Meter(float(double(bytes) / double(kBudgetBytes)), ui::ToneU32(ui::Tone::Info, 0.40f),
                      Fmt("%s of 64 MiB", FormatBytes(bytes).c_str()).c_str());
            ui::EndForm();
        }
        ui::Note("These count only the textures this plugin made. ImGui's font atlases (a few MiB, more once CJK "
                 "text is drawn) count against the same two budgets, so leave room.");
    }

    void DrawImages(App& app) {
        ui::SectionTitle(ICON_FA_IMAGE, "In the panel: ImGui::Image");
        const TextureSet& t = app.textures;
        if (!t.plasma.id) {
            ui::TextDim("Being created...");
            return;
        }
        const float side = std::floor((std::min)(ui::Em(7.0f), (ImGui::GetContentRegionAvail().x - ui::Em(2.0f)) / 3.0f));
        ui::Flow flow;
        // Every image gets the same square tile, so the captions line up whatever its shape.
        auto image = [&](const Tex& tex, const char* caption, float aspect /* height / width */) {
            flow.Next(side);
            ImGui::BeginGroup();
            const ImVec2 at = ImGui::GetCursorPos(), screen = ImGui::GetCursorScreenPos();
            ImGui::GetWindowDrawList()->AddRectFilled(screen, ImVec2(screen.x + side, screen.y + side),
                                                      ImGui::GetColorU32(ImGuiCol_FrameBg), 3.0f);
            const float h = std::floor(side * aspect);
            ImGui::SetCursorPos(ImVec2(at.x, at.y + std::floor((side - h) * 0.5f)));
            ImGui::Image(ImTextureRef(tex.id), ImVec2(side, h));
            ImGui::SetCursorPos(ImVec2(at.x, at.y + side + ImGui::GetStyle().ItemSpacing.y * 0.5f));
            ImGui::TextDisabled("%s", caption);
            ImGui::EndGroup();
        };
        image(t.plasma, Fmt("plasma %d x %d", t.plasma.w, t.plasma.h).c_str(), 1.0f);
        image(t.checker, Fmt("checker %d x %d", t.checker.w, t.checker.h).c_str(), 1.0f);
        image(t.gradient, Fmt("gradient %d x %d", t.gradient.w, t.gradient.h).c_str(), 0.25f);
        if (!t.extras.empty()) {
            ImGui::TextDisabled("Extras:");
            ui::Flow row;
            // (Not "small": a Windows header defines that word as a macro.)
            const float thumb = ImGui::GetFrameHeight();
            for (const Tex& e : t.extras) {
                row.Next(thumb);
                ImGui::Image(ImTextureRef(e.id), ImVec2(thumb, thumb));
            }
        }
    }

    void DrawControls(App& app) {
        ui::SectionTitle(ICON_FA_SLIDERS, "Try it");
        TextureSet& t = app.textures;
        if (ImGui::Checkbox("Animate the plasma (one 128 x 32 band per tick)", &m_animate)) app.MarkDirty();
        bool sprite = app.LayerOn(LayerId::Sprite);
        if (ImGui::Checkbox("Draw the plasma on the overlay (the sprite layer)", &sprite)) app.SetLayer(LayerId::Sprite, sprite);
        ui::Flow flow;
        const std::string more = Fmt(ICON_FA_PLUS "  Create %d more", kExtraStep);
        flow.Next(ui::ButtonWidth(more.c_str()));
        ImGui::BeginDisabled(static_cast<int>(t.extras.size()) >= kExtraMax);
        if (ImGui::Button(more.c_str())) t.AddExtras(kExtraStep);
        ImGui::EndDisabled();
        const char* drop = ICON_FA_TRASH "  Destroy the extras";
        flow.Next(ui::ButtonWidth(drop));
        ImGui::BeginDisabled(t.extras.empty());
        if (ImGui::Button(drop)) t.DestroyExtras();
        ImGui::EndDisabled();
        if (t.extras.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled | ImGuiHoveredFlags_DelayShort))
            ImGui::SetTooltip("There are no extras: \"Create %d more\" makes some.", kExtraStep);   // why it is greyed
        const char* again = ICON_FA_ROTATE "  Recreate all";
        flow.Next(ui::ButtonWidth(again));
        if (ImGui::Button(again)) {
            t.ReleaseAll();   // the next Tick makes them again
        }
        ui::TextDim("Partial updates: %llu. Refusals: %llu.", static_cast<unsigned long long>(t.updates),
                            static_cast<unsigned long long>(t.refused));
    }

    bool m_animate = true;
};

}  // namespace

std::unique_ptr<Lesson> MakeTexturesLesson() { return std::make_unique<TexturesLesson>(); }

}  // namespace ex
