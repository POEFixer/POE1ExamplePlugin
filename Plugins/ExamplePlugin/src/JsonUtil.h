#pragma once
// ============================================================================
// JsonUtil — reading settings.json defensively.
//
// A settings file is untrusted input: a user edits it by hand, an older or a
// newer build of the plugin wrote it, another plugin may even have written it
// (data folders are not isolated from each other). So every read here checks
// the type, clamps the range, and falls back to the default instead of
// throwing. A file nlohmann cannot PARSE never gets this far: p1::Settings
// renames it to settings.json.rejected (see the Settings lesson).
// ============================================================================
#include <p1sdk/p1sdk.hpp>

#include <cmath>
#include <string>

namespace ex::js {

inline const nlohmann::json* Find(const nlohmann::json& j, const char* key) {
    if (!j.is_object()) return nullptr;
    const auto it = j.find(key);
    return it == j.end() ? nullptr : &*it;
}

inline bool Bool(const nlohmann::json& j, const char* key, bool def) {
    const nlohmann::json* v = Find(j, key);
    return (v && v->is_boolean()) ? v->get<bool>() : def;
}

inline float Float(const nlohmann::json& j, const char* key, float def, float lo, float hi) {
    const nlohmann::json* v = Find(j, key);
    if (!v || !v->is_number()) return def;
    const double d = v->get<double>();
    if (!std::isfinite(d)) return def;
    return static_cast<float>(d < lo ? lo : (d > hi ? hi : d));
}

inline int Int(const nlohmann::json& j, const char* key, int def, int lo, int hi) {
    const nlohmann::json* v = Find(j, key);
    if (!v || !v->is_number_integer()) return def;
    const long long n = v->get<long long>();
    return static_cast<int>(n < lo ? lo : (n > hi ? hi : n));
}

// At most `maxBytes` bytes; a longer string is refused, not cut (a cut can split a
// UTF-8 character).
inline std::string String(const nlohmann::json& j, const char* key, const std::string& def, size_t maxBytes) {
    const nlohmann::json* v = Find(j, key);
    if (!v || !v->is_string()) return def;
    const std::string& s = v->get_ref<const std::string&>();
    return s.size() <= maxBytes ? s : def;
}

inline ImVec4 Color(const nlohmann::json& j, const char* key, const ImVec4& def) {
    const nlohmann::json* v = Find(j, key);
    if (!v || !v->is_array() || v->size() != 4) return def;
    float c[4] = { def.x, def.y, def.z, def.w };
    for (size_t i = 0; i < 4; ++i) {
        const nlohmann::json& e = (*v)[i];
        if (!e.is_number()) return def;
        const double d = e.get<double>();
        if (!std::isfinite(d)) return def;
        c[i] = static_cast<float>(d < 0.0 ? 0.0 : (d > 1.0 ? 1.0 : d));
    }
    return ImVec4(c[0], c[1], c[2], c[3]);
}

inline nlohmann::json ColorJson(const ImVec4& c) { return nlohmann::json::array({ c.x, c.y, c.z, c.w }); }

}  // namespace ex::js
