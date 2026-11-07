// ============================================================================
// p1sdk.hpp — header-only C++ wrapper over the POE1Fixer plugin ABI (p1_abi.h).
//
//   #include <p1sdk/p1sdk.hpp>
//   class MyPlugin : public p1::Plugin {
//       bool OnLoad() override { p1::Log(P1_LOG_INFO, "hello"); return true; }
//       void OnOverlay(const P1Frame& f) override { ... ImGui ... }
//   };
//   P1_PLUGIN(MyPlugin, "my_plugin", "My Plugin", "1.0.0", "me", "What it does")
//
// What the wrapper guarantees:
//   - A C++ exception never crosses the ABI. Every callback thunk catches
//     std::exception and (...) and logs it through the host; the plugin keeps
//     running (an exception thrown out of on_load refuses the load instead).
//   - std::terminate / abort / pure calls / invalid CRT parameters in THIS
//     DLL's C runtime are handed to the plugin host's crash guard (its
//     unhandled-exception filter) with the matching P1_EXIT_* code, so the
//     app can show what killed the plugin and restart it. (A real access
//     violation needs no help: it reaches that filter directly.) Do not compile plugins with /EHa:
//     catch (...) would then swallow access violations and hide real crashes.
//   - ImGui is usable ONLY inside OnOverlay / OnPanel: the host binds its
//     context to this DLL's ImGui copy right before each of those calls.
//
// Thread rules: every Plugin callback runs on the host's single plugin thread.
// Log / Notify / Memory / Input / SetClipboard may be called from any thread;
// Texture functions only from the plugin thread.
// ============================================================================
#pragma once

#ifndef NOMINMAX
#  define NOMINMAX
#  define P1SDK_UNDEF_NOMINMAX
#endif
#include <windows.h>
#ifdef P1SDK_UNDEF_NOMINMAX
#  undef NOMINMAX
#  undef P1SDK_UNDEF_NOMINMAX
#endif

#include <csignal>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <initializer_list>
#include <mutex>
#include <string>
#include <type_traits>
#include <vector>

#include "p1_abi.h"
#include "../../imgui/imgui.h"
#include "../../imgui/imgui_internal.h"   // sizeof(ImGuiContext) for the layout check

#ifndef P1SDK_NO_JSON
#  include "../../third_party/nlohmann/json.hpp"
#endif

namespace p1 {

class Plugin;

// ---------------------------------------------------------------------------
// detail — process-wide state of this DLL (one plugin per DLL, one DLL per
// plugin host process, so plain statics are exactly the right scope).
// ---------------------------------------------------------------------------
namespace detail {

struct State {
    const P1HostApi* host = nullptr;
    Plugin* plugin = nullptr;
};
inline State& GetState() noexcept {
    static State s;
    return s;
}

// True when the host's table is large enough to hold `field`.
#define P1SDK_HOST_HAS(h, field) P1_HAS_FIELD((h), P1HostApi, field)

// Hands a fatal CRT event (P1_EXIT_* code) to the plugin host's process-wide
// unhandled-exception filter, which maps the code to a crash kind, records it
// in shared memory and terminates the process with `code`. The filter is
// CALLED, not reached by RaiseException: the C runtime runs a terminate
// handler inside __try/__except, which would swallow a raised exception and
// turn every std::terminate into a plain abort.
[[noreturn]] inline void RaiseFatal(DWORD code) noexcept {
    static volatile LONG claimed = 0;
    if (InterlockedCompareExchange(&claimed, 1, 0) != 0) {   // another thread is already dying
        Sleep(10'000);
        TerminateProcess(GetCurrentProcess(), code);
        ExitProcess(code);
    }
    // Static: the faulting thread may have no stack to spare, and only one thread gets here.
    static CONTEXT context;
    static EXCEPTION_RECORD record;
    static EXCEPTION_POINTERS pointers;
    RtlCaptureContext(&context);
    record = EXCEPTION_RECORD{};
    record.ExceptionCode = code;
    record.ExceptionFlags = EXCEPTION_NONCONTINUABLE;
#if defined(_M_X64)
    record.ExceptionAddress = reinterpret_cast<PVOID>(static_cast<uintptr_t>(context.Rip));
#endif
    pointers.ExceptionRecord = &record;
    pointers.ContextRecord = &context;
    const LPTOP_LEVEL_EXCEPTION_FILTER filter = SetUnhandledExceptionFilter(nullptr);
    SetUnhandledExceptionFilter(filter);
    if (filter) filter(&pointers);
    // Only reached without a host filter (or one that returned).
    TerminateProcess(GetCurrentProcess(), code);
    ExitProcess(code);
}

// Hooks THIS DLL's C runtime. When the plugin and the host share one CRT these
// replace the host's equivalent hooks with ones that end up in the same place.
inline void InstallCrashHooks() noexcept {
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);   // no dialog, no WER fast-fail
    std::set_terminate([]() { RaiseFatal(P1_EXIT_CPP_TERMINATE); });   // per thread in MSVC: the plugin thread
    _set_purecall_handler([]() { RaiseFatal(P1_EXIT_PURECALL); });
    _set_invalid_parameter_handler(
        [](const wchar_t*, const wchar_t*, const wchar_t*, unsigned int, uintptr_t) {
            RaiseFatal(P1_EXIT_INVALID_PARAM);
        });
    // Other plugin threads reach abort() through the default terminate handler.
    std::signal(SIGABRT, [](int) { RaiseFatal(P1_EXIT_ABORT); });
}

inline void HostLog(int level, const char* utf8) noexcept {
    const P1HostApi* h = GetState().host;
    if (h && P1SDK_HOST_HAS(h, log) && h->log && utf8) h->log(h->host, level, utf8);
}

inline void ReportException(const char* where, const char* what) noexcept {
    char buf[512];
    std::snprintf(buf, sizeof buf, "uncaught C++ exception in %s: %s", where ? where : "?",
                  what ? what : "(not a std::exception)");
    HostLog(P1_LOG_ERROR, buf);
}

} // namespace detail

// ---------------------------------------------------------------------------
// Host services
// ---------------------------------------------------------------------------
inline const P1HostApi* Host() noexcept { return detail::GetState().host; }

// printf-style log line (UTF-8). Lines are cut at 2048 bytes; the app rate-limits.
inline void LogV(int level, const char* fmt, va_list ap) noexcept {
    if (!fmt) return;
    char buf[2048];
    const int n = std::vsnprintf(buf, sizeof buf, fmt, ap);
    if (n < 0) return;
    detail::HostLog(level, buf);
}
inline void Log(int level, _Printf_format_string_ const char* fmt, ...) noexcept {
    va_list ap;
    va_start(ap, fmt);
    LogV(level, fmt, ap);
    va_end(ap);
}

// A toast in the app (rate-limited to about one per two seconds).
inline void Notify(int level, const char* titleUtf8, const char* textUtf8) noexcept {
    const P1HostApi* h = Host();
    if (h && P1SDK_HOST_HAS(h, notify) && h->notify)
        h->notify(h->host, level, titleUtf8 ? titleUtf8 : "", textUtf8 ? textUtf8 : "");
}

inline void SetClipboard(const char* utf8) noexcept {
    const P1HostApi* h = Host();
    if (h && P1SDK_HOST_HAS(h, set_clipboard) && h->set_clipboard) h->set_clipboard(h->host, utf8 ? utf8 : "");
}

// Ask for overlay frames at least `fps` times a second (0 = only on new data).
inline void RequestFrames(uint32_t fps) noexcept {
    const P1HostApi* h = Host();
    if (h && P1SDK_HOST_HAS(h, request_frames) && h->request_frames) h->request_frames(h->host, fps);
}

// Steady clock, microseconds — the same clock as P1Frame::now_us and the snapshot stamps.
inline int64_t NowUs() noexcept {
    const P1HostApi* h = Host();
    if (h && P1SDK_HOST_HAS(h, now_us) && h->now_us) return h->now_us(h->host);
    return 0;
}

inline bool HasPermission(const P1Frame& f, uint32_t perm) noexcept { return (f.permissions & perm) == perm; }

// ---------------------------------------------------------------------------
// Snapshot access
// ---------------------------------------------------------------------------
inline const char* Str(const P1Snapshot& s, p1_str off) noexcept { return p1_str_get(&s, off); }

// Range over a strided array of the snapshot (never index the arrays as C arrays:
// a newer app may have grown the element structs).
template <class T, const T* (*At)(const P1Snapshot*, uint32_t)>
class StridedRange {
public:
    class iterator {
    public:
        iterator(const P1Snapshot* s, uint32_t i) noexcept : m_s(s), m_i(i) {}
        const T& operator*() const noexcept { return *At(m_s, m_i); }
        const T* operator->() const noexcept { return At(m_s, m_i); }
        iterator& operator++() noexcept { ++m_i; return *this; }
        bool operator!=(const iterator& o) const noexcept { return m_i != o.m_i; }
        bool operator==(const iterator& o) const noexcept { return m_i == o.m_i; }
    private:
        const P1Snapshot* m_s;
        uint32_t m_i;
    };
    StridedRange(const P1Snapshot* s, uint32_t first, uint32_t count) noexcept : m_s(s), m_first(first), m_count(count) {}
    iterator begin() const noexcept { return iterator(m_s, m_first); }
    iterator end() const noexcept { return iterator(m_s, m_first + m_count); }
    uint32_t size() const noexcept { return m_count; }
    bool empty() const noexcept { return m_count == 0; }
private:
    const P1Snapshot* m_s;
    uint32_t m_first, m_count;
};

using EntityRange = StridedRange<P1Entity, &p1_entity_at>;
using BuffRange   = StridedRange<P1Buff, &p1_buff_at>;
using ItemRange   = StridedRange<P1InventoryItem, &p1_item_at>;

// for (const P1Entity& e : p1::Entities(snapshot)) { ... }
inline EntityRange Entities(const P1Snapshot& s) noexcept {
    const uint32_t n = (s.entity_stride >= sizeof(P1Entity)) ? s.entity_count : 0u;
    return EntityRange(&s, 0, n);
}
// The local player's buffs (empty unless P1_SNAP_BUFFS_VALID).
inline BuffRange PlayerBuffs(const P1Snapshot& s) noexcept {
    if (!(s.flags & P1_SNAP_BUFFS_VALID) || s.buff_stride < sizeof(P1Buff)) return BuffRange(&s, 0, 0);
    const uint64_t end = uint64_t(s.player.buff_first) + s.player.buff_count;
    if (end > s.buff_count) return BuffRange(&s, 0, 0);
    return BuffRange(&s, s.player.buff_first, s.player.buff_count);
}
// Main inventory items (empty unless P1_SNAP_INVENTORY_VALID).
inline ItemRange InventoryItems(const P1Snapshot& s) noexcept {
    const uint32_t n = ((s.flags & P1_SNAP_INVENTORY_VALID) && s.item_stride >= sizeof(P1InventoryItem)) ? s.item_count : 0u;
    return ItemRange(&s, 0, n);
}

// World position -> game client pixels (overlay space) with the frame's client size.
inline bool WorldToScreen(const P1Frame& f, float wx, float wy, float wz, ImVec2& out) noexcept {
    if (!f.snapshot) return false;
    float sx = 0.0f, sy = 0.0f;
    if (!p1_world_to_screen(f.snapshot, wx, wy, wz, float(f.viewport.client_w), float(f.viewport.client_h), &sx, &sy))
        return false;
    out = ImVec2(sx, sy);
    return true;
}
inline bool WorldToScreen(const P1Frame& f, const P1Vec3& w, ImVec2& out) noexcept {
    return WorldToScreen(f, w.x, w.y, w.z, out);
}

// Edge detection over P1Viewport::keys_down (the app samples it only while the
// game is foreground and no game text field has focus). Call Update once per tick.
class KeyEdge {
public:
    void Update(const P1Viewport& v) noexcept {
        std::memcpy(m_prev, m_cur, sizeof m_cur);
        std::memcpy(m_cur, v.keys_down, sizeof m_cur);
    }
    bool Down(int vk) const noexcept { return Bit(m_cur, vk); }
    bool Pressed(int vk) const noexcept { return Bit(m_cur, vk) && !Bit(m_prev, vk); }
    bool Released(int vk) const noexcept { return !Bit(m_cur, vk) && Bit(m_prev, vk); }
    void Reset() noexcept { std::memset(m_prev, 0, sizeof m_prev); std::memset(m_cur, 0, sizeof m_cur); }
private:
    static bool Bit(const uint8_t* k, int vk) noexcept {
        return vk > 0 && vk < 256 && ((k[vk >> 3] >> (vk & 7)) & 1) != 0;
    }
    uint8_t m_prev[32] = {};
    uint8_t m_cur[32] = {};
};

// ---------------------------------------------------------------------------
// Game memory (P1_PERM_MEMORY). Read-only by construction: the host holds a
// handle with PROCESS_VM_READ plus query rights (PROCESS_QUERY_LIMITED_INFORMATION),
// no write or operation rights. Every function returns 0 / false without
// the permission or while the app is not attached to a game.
// ---------------------------------------------------------------------------
namespace Memory {

inline uint32_t ReadBytes(uint64_t address, void* dst, uint32_t size) noexcept {
    const P1HostApi* h = Host();
    if (!h || !P1SDK_HOST_HAS(h, read_memory) || !h->read_memory || !dst || size == 0) return 0;
    return h->read_memory(h->host, address, dst, size);
}

// True only when all sizeof(T) bytes were read.
template <class T>
bool Read(uint64_t address, T& out) noexcept {
    static_assert(std::is_trivially_copyable_v<T>, "Memory::Read needs a trivially copyable type");
    T tmp{};
    if (ReadBytes(address, &tmp, uint32_t(sizeof(T))) != sizeof(T)) return false;
    out = tmp;
    return true;
}

template <class T>
T Read(uint64_t address) noexcept {
    T v{};
    Read(address, v);
    return v;
}

// Follows a pointer chain: p = base; for each offset: p = *(uint64_t*)(p + offset).
// Returns the final pointer in `out`; false when any step failed or read null.
inline bool ReadPointerChain(uint64_t base, std::initializer_list<uint32_t> offsets, uint64_t& out) noexcept {
    uint64_t p = base;
    for (const uint32_t off : offsets) {
        uint64_t next = 0;
        if (!p || !Read(p + off, next) || !next) return false;
        p = next;
    }
    out = p;
    return true;
}

// Batched reads (one host call for many requests). Returns how many completed fully.
inline uint32_t ReadBatch(P1ReadRequest* reqs, uint32_t count) noexcept {
    const P1HostApi* h = Host();
    if (!h || !P1SDK_HOST_HAS(h, read_memory_batch) || !h->read_memory_batch || !reqs || !count) return 0;
    return h->read_memory_batch(h->host, reqs, count);
}

} // namespace Memory

// ---------------------------------------------------------------------------
// Input requests (P1_PERM_INPUT + the user's consent). The app performs them on
// its own input thread only while it is safe (game foreground, no trade or
// recovery automation, user not typing in the app) and releases everything the
// plugin holds when that stops being true. Coordinates are game client pixels.
// ---------------------------------------------------------------------------
namespace Input {

class Batch {
public:
    Batch& KeyDown(uint16_t vk) noexcept { return Add(P1_IN_KEY_DOWN, vk, 0, 0, 0); }
    Batch& KeyUp(uint16_t vk) noexcept { return Add(P1_IN_KEY_UP, vk, 0, 0, 0); }
    Batch& KeyPress(uint16_t vk, uint32_t holdMs = 0) noexcept { return Add(P1_IN_KEY_PRESS, vk, 0, 0, holdMs); }
    Batch& MouseMove(int32_t x, int32_t y) noexcept { return Add(P1_IN_MOUSE_MOVE, 0, x, y, 0); }
    Batch& MouseDown(uint16_t vk = VK_LBUTTON) noexcept { return Add(P1_IN_MOUSE_DOWN, vk, 0, 0, 0); }
    Batch& MouseUp(uint16_t vk = VK_LBUTTON) noexcept { return Add(P1_IN_MOUSE_UP, vk, 0, 0, 0); }
    Batch& Click(int32_t x, int32_t y, uint16_t vk = VK_LBUTTON, uint32_t holdMs = 0) noexcept {
        return Add(P1_IN_CLICK, vk, x, y, holdMs);
    }
    Batch& Wait(uint32_t ms) noexcept { return Add(P1_IN_WAIT, 0, 0, 0, ms > 1000u ? 1000u : ms); }

    uint32_t Count() const noexcept { return m_n; }
    bool Overflowed() const noexcept { return m_overflow; }
    void Clear() noexcept { m_n = 0; m_overflow = false; }

    // All or nothing: a batch that overflowed P1_INPUT_MAX_BATCH is never sent.
    // Returns how many actions the host queued (0 = refused).
    uint32_t Submit() const noexcept {
        const P1HostApi* h = Host();
        if (m_overflow || m_n == 0 || !h || !P1SDK_HOST_HAS(h, submit_input) || !h->submit_input) return 0;
        return h->submit_input(h->host, m_a, m_n);
    }

private:
    Batch& Add(uint16_t type, uint16_t vk, int32_t x, int32_t y, uint32_t holdMs) noexcept {
        if (m_n >= P1_INPUT_MAX_BATCH) { m_overflow = true; return *this; }
        P1InputAction& a = m_a[m_n++];
        a.type = type; a.vk = vk; a.x = x; a.y = y; a.hold_ms = holdMs;
        return *this;
    }
    P1InputAction m_a[P1_INPUT_MAX_BATCH] = {};
    uint32_t m_n = 0;
    bool m_overflow = false;
};

inline uint32_t KeyPress(uint16_t vk, uint32_t holdMs = 0) noexcept { return Batch().KeyPress(vk, holdMs).Submit(); }
inline uint32_t Click(int32_t x, int32_t y, uint16_t vk = VK_LBUTTON, uint32_t holdMs = 0) noexcept {
    return Batch().Click(x, y, vk, holdMs).Submit();
}

} // namespace Input

// ---------------------------------------------------------------------------
// Textures for ImGui::Image (plugin thread only). RGBA8, max 4096x4096; the app
// caps the live texture memory per plugin (64 MiB by default).
// ---------------------------------------------------------------------------
namespace Texture {

// Returns 0 on failure.
inline ImTextureID Create(const void* rgba, int w, int h) noexcept {
    const P1HostApi* hp = Host();
    if (!hp || !P1SDK_HOST_HAS(hp, texture_create) || !hp->texture_create || !rgba || w <= 0 || h <= 0) return 0;
    return ImTextureID(hp->texture_create(hp->host, rgba, int32_t(w), int32_t(h)));
}
// pitch 0 = tightly packed (w * 4).
inline bool Update(ImTextureID tex, int x, int y, int w, int h, const void* rgba, int pitch = 0) noexcept {
    const P1HostApi* hp = Host();
    if (!hp || !P1SDK_HOST_HAS(hp, texture_update) || !hp->texture_update || !tex || !rgba) return false;
    return hp->texture_update(hp->host, uint64_t(tex), int32_t(x), int32_t(y), int32_t(w), int32_t(h), rgba,
                              int32_t(pitch)) != 0;
}
inline void Destroy(ImTextureID tex) noexcept {
    const P1HostApi* hp = Host();
    if (hp && P1SDK_HOST_HAS(hp, texture_destroy) && hp->texture_destroy && tex) hp->texture_destroy(hp->host, uint64_t(tex));
}

} // namespace Texture

// ---------------------------------------------------------------------------
// UTF-8 <-> UTF-16. Every path the host gives you is UTF-8; convert it before
// calling a W API (never pass it to an A API or std::filesystem::path(std::string):
// both go through the ANSI code page and break on Cyrillic / CJK folders).
// Invalid input yields an empty string.
// ---------------------------------------------------------------------------
inline std::wstring Widen(const char* utf8, size_t len) noexcept {
    try {
        if (!utf8 || len == 0 || len > 0x7FFFFFFFu) return {};
        const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, int(len), nullptr, 0);
        if (n <= 0) return {};
        std::wstring w(size_t(n), L'\0');
        if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, int(len), w.data(), n) != n) return {};
        return w;
    } catch (...) {
        return {};
    }
}
inline std::wstring Widen(const std::string& utf8) noexcept { return Widen(utf8.data(), utf8.size()); }
inline std::wstring Widen(const char* utf8) noexcept { return utf8 ? Widen(utf8, std::strlen(utf8)) : std::wstring(); }

inline std::string Narrow(const wchar_t* w, size_t len) noexcept {
    try {
        if (!w || len == 0 || len > 0x7FFFFFFFu) return {};
        const int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, w, int(len), nullptr, 0, nullptr, nullptr);
        if (n <= 0) return {};
        std::string s(size_t(n), '\0');
        if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, w, int(len), s.data(), n, nullptr, nullptr) != n) return {};
        return s;
    } catch (...) {
        return {};
    }
}
inline std::string Narrow(const std::wstring& w) noexcept { return Narrow(w.data(), w.size()); }

#ifndef P1SDK_NO_JSON
// ---------------------------------------------------------------------------
// Settings — JSON files in P1HostApi::config_dir (your ONE writable folder).
//   Load: Missing on first run; an existing file that cannot be read or parsed
//         is renamed "<name>.rejected" (replacing an older one) and Rejected is
//         returned, so a corrupt file is kept for inspection, never overwritten.
//         If even the rename fails, Save refuses that name until a Load works.
//   Save: atomic — writes "<name>.tmp", flushes it, then replaces the target
//         with MoveFileExW(REPLACE_EXISTING | WRITE_THROUGH).
// `name` is a plain file name: letters, digits, '_', '-', '.', 1..96 bytes, no "..".
// ---------------------------------------------------------------------------
namespace Settings {

enum class LoadResult { Loaded, Missing, Rejected, Error };

namespace impl {

inline std::mutex& Lock() noexcept {
    static std::mutex m;
    return m;
}
inline std::vector<std::string>& Blocked() noexcept {
    static std::vector<std::string> v;
    return v;
}
inline bool IsBlocked(const std::string& name) noexcept {
    for (const std::string& b : Blocked())
        if (b == name) return true;
    return false;
}
inline void SetBlocked(const std::string& name, bool on) noexcept {
    try {
        std::vector<std::string>& v = Blocked();
        for (size_t i = 0; i < v.size(); ++i)
            if (v[i] == name) { if (!on) v.erase(v.begin() + std::ptrdiff_t(i)); return; }
        if (on) v.push_back(name);
    } catch (...) {
    }
}
inline bool ValidName(const char* name) noexcept {
    if (!name) return false;
    const size_t n = std::strlen(name);
    if (n == 0 || n > 96 || std::strstr(name, "..") != nullptr || name[0] == '.') return false;
    for (size_t i = 0; i < n; ++i) {
        const char c = name[i];
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
                        c == '-' || c == '.';
        if (!ok) return false;
    }
    return true;
}
inline std::wstring PathFor(const char* name) noexcept {
    try {
        const P1HostApi* h = Host();
        if (!h || !P1SDK_HOST_HAS(h, config_dir) || !h->config_dir || !ValidName(name)) return {};
        std::wstring dir = Widen(h->config_dir);
        if (dir.empty()) return {};
        if (dir.back() != L'\\' && dir.back() != L'/') dir.push_back(L'\\');
        return dir + Widen(name);
    } catch (...) {
        return {};
    }
}
inline void SetError(std::string* error, const char* text) noexcept {
    if (!error) return;
    try { *error = text; } catch (...) {}
}

} // namespace impl

inline LoadResult Load(const char* name, nlohmann::json& out, std::string* error = nullptr) noexcept {
    try {
        std::lock_guard<std::mutex> lock(impl::Lock());
        const std::wstring path = impl::PathFor(name);
        if (path.empty()) { impl::SetError(error, "invalid settings name or no config directory"); return LoadResult::Error; }
        const std::string key(name);
        const std::wstring rejected = path + L".rejected";
        auto reject = [&](const char* why) {
            if (MoveFileExW(path.c_str(), rejected.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
                impl::SetBlocked(key, false);
                impl::SetError(error, why);
                return LoadResult::Rejected;
            }
            impl::SetBlocked(key, true);   // could not move it away: never overwrite it
            impl::SetError(error, "settings file unreadable and could not be renamed; saving is disabled for it");
            return LoadResult::Error;
        };

        HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f == INVALID_HANDLE_VALUE) {
            const DWORD e = GetLastError();
            if (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND) {
                impl::SetBlocked(key, false);
                return LoadResult::Missing;
            }
            return reject("settings file could not be opened");
        }
        LARGE_INTEGER size{};
        std::string text;
        bool readOk = GetFileSizeEx(f, &size) != FALSE && size.QuadPart >= 0 && size.QuadPart <= (16ll << 20);
        if (readOk) {
            text.resize(size_t(size.QuadPart));
            size_t got = 0;
            while (got < text.size()) {
                DWORD n = 0;
                const DWORD want = DWORD((std::min)(text.size() - got, size_t(1u << 20)));
                if (!ReadFile(f, text.data() + got, want, &n, nullptr) || n == 0) { readOk = false; break; }
                got += n;
            }
        }
        CloseHandle(f);
        if (!readOk) return reject("settings file could not be read");
        size_t start = 0;
        if (text.size() >= 3 && uint8_t(text[0]) == 0xEF && uint8_t(text[1]) == 0xBB && uint8_t(text[2]) == 0xBF) start = 3;
        nlohmann::json j = nlohmann::json::parse(text.begin() + std::ptrdiff_t(start), text.end(), nullptr, false);
        if (j.is_discarded()) return reject("settings file is not valid JSON");
        out = std::move(j);
        impl::SetBlocked(key, false);
        return LoadResult::Loaded;
    } catch (const std::exception& e) {
        impl::SetError(error, e.what());
    } catch (...) {
        impl::SetError(error, "unknown error");
    }
    return LoadResult::Error;
}

inline bool Save(const char* name, const nlohmann::json& value, std::string* error = nullptr) noexcept {
    try {
        std::lock_guard<std::mutex> lock(impl::Lock());
        const std::wstring path = impl::PathFor(name);
        if (path.empty()) { impl::SetError(error, "invalid settings name or no config directory"); return false; }
        if (impl::IsBlocked(name)) { impl::SetError(error, "an unreadable settings file is in the way; not overwriting it"); return false; }
        // replace: invalid UTF-8 in a string must never make dump() throw (type_error.316).
        const std::string text = value.dump(2, ' ', false, nlohmann::json::error_handler_t::replace);
        const std::wstring tmp = path + L".tmp";
        HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f == INVALID_HANDLE_VALUE) { impl::SetError(error, "cannot create the temporary file"); return false; }
        bool ok = true;
        size_t put = 0;
        while (ok && put < text.size()) {
            DWORD n = 0;
            const DWORD want = DWORD((std::min)(text.size() - put, size_t(1u << 20)));
            ok = WriteFile(f, text.data() + put, want, &n, nullptr) != FALSE && n == want;
            put += n;
        }
        ok = ok && FlushFileBuffers(f) != FALSE;
        CloseHandle(f);
        if (ok) ok = MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
        if (!ok) {
            DeleteFileW(tmp.c_str());
            impl::SetError(error, "writing the settings file failed");
        }
        return ok;
    } catch (const std::exception& e) {
        impl::SetError(error, e.what());
    } catch (...) {
        impl::SetError(error, "unknown error");
    }
    return false;
}

} // namespace Settings
#endif // P1SDK_NO_JSON

// ---------------------------------------------------------------------------
// The plugin base class. Override what you need; every callback runs on the
// host's plugin thread. ImGui calls belong in OnOverlay / OnPanel only.
//   OnLoad     once, before anything else. Return false to refuse to run.
//   OnTick     game logic: on every new snapshot (and ~10 Hz while none arrives).
//   OnOverlay  an ImGui frame drawn over the game (click-through, game client
//              pixels). You are inside a full-screen transparent window.
//   OnPanel    your settings panel inside the app's Plugins tab (only while shown).
//   OnEvent    P1_EV_* notifications, before the OnTick of the same iteration.
//   OnUnload   once, before the process exits (save settings here). The object
//              is deleted right after it.
// ---------------------------------------------------------------------------
class Plugin {
public:
    Plugin() = default;
    virtual ~Plugin() = default;
    Plugin(const Plugin&) = delete;
    Plugin& operator=(const Plugin&) = delete;

    virtual bool OnLoad() { return true; }
    virtual void OnUnload() {}
    virtual void OnTick(const P1Frame&) {}
    virtual void OnOverlay(const P1Frame&) {}
    virtual void OnPanel(const P1Frame&) {}
    virtual void OnEvent(const P1Event&) {}

    // The host's service table (never null once the plugin exists).
    const P1HostApi& HostApi() const noexcept { return *detail::GetState().host; }
    // The frame of the callback running now (OnTick / OnOverlay / OnPanel), else null.
    const P1Frame* CurrentFrame() const noexcept { return m_frame; }
    // The snapshot of the callback running now, else null.
    const P1Snapshot* CurrentSnapshot() const noexcept { return m_frame ? m_frame->snapshot : nullptr; }

    std::string PluginDir() const { return Field(HostApi().plugin_dir); }
    std::string ConfigDir() const { return Field(HostApi().config_dir); }
    std::string AppDir() const { return Field(HostApi().app_dir); }
    std::string Language() const { return Field(HostApi().language); }
    uint32_t ManifestPermissions() const noexcept { return HostApi().permissions; }

private:
    static std::string Field(const char* s) { return s ? std::string(s) : std::string(); }
    friend struct detail_access;
    const P1Frame* m_frame = nullptr;
};

struct detail_access {
    static void SetFrame(Plugin& p, const P1Frame* f) noexcept { p.m_frame = f; }
};

namespace detail {

inline Plugin* Self(void* self) noexcept { return static_cast<Plugin*>(self); }

template <class Fn>
inline void Guard(const char* where, Fn&& fn) noexcept {
    try {
        fn();
    } catch (const std::exception& e) {
        ReportException(where, e.what());
    } catch (...) {
        ReportException(where, nullptr);
    }
}

inline int P1_CALL OnLoadThunk(void* self) noexcept {
    if (!self) return 0;
    try {
        return Self(self)->OnLoad() ? 1 : 0;
    } catch (const std::exception& e) {
        ReportException("on_load", e.what());
    } catch (...) {
        ReportException("on_load", nullptr);
    }
    return 0;
}

inline void P1_CALL OnUnloadThunk(void* self) noexcept {
    if (!self) return;
    Plugin* p = Self(self);
    Guard("on_unload", [&] { p->OnUnload(); });
    Guard("plugin destructor", [&] { delete p; });
    GetState().plugin = nullptr;
}

inline void P1_CALL OnTickThunk(void* self, const P1Frame* f) noexcept {
    if (!self || !f) return;
    Plugin* p = Self(self);
    detail_access::SetFrame(*p, f);
    Guard("on_tick", [&] { p->OnTick(*f); });
    detail_access::SetFrame(*p, nullptr);
}

inline void P1_CALL OnOverlayThunk(void* self, const P1Frame* f) noexcept {
    if (!self || !f) return;
    Plugin* p = Self(self);
    detail_access::SetFrame(*p, f);
    Guard("on_overlay", [&] { p->OnOverlay(*f); });
    detail_access::SetFrame(*p, nullptr);
}

inline void P1_CALL OnPanelThunk(void* self, const P1Frame* f) noexcept {
    if (!self || !f) return;
    Plugin* p = Self(self);
    detail_access::SetFrame(*p, f);
    Guard("on_panel", [&] { p->OnPanel(*f); });
    detail_access::SetFrame(*p, nullptr);
}

inline void P1_CALL OnEventThunk(void* self, const P1Event* e) noexcept {
    if (!self || !e) return;
    Plugin* p = Self(self);
    Guard("on_event", [&] { p->OnEvent(*e); });
}

constexpr P1PluginInfo MakeInfo(const char* id, const char* name, const char* version, const char* author,
                                const char* description) noexcept {
    P1PluginInfo i{};
    i.size_bytes = uint32_t(sizeof(P1PluginInfo));
    i.abi_version = P1_ABI_VERSION;
    i.snapshot_version = P1_SNAPSHOT_VERSION;
    i.imgui_version_num = uint32_t(IMGUI_VERSION_NUM);
    i.imgui_layout[0] = uint32_t(sizeof(ImGuiIO));
    i.imgui_layout[1] = uint32_t(sizeof(ImGuiStyle));
    i.imgui_layout[2] = uint32_t(sizeof(ImVec2));
    i.imgui_layout[3] = uint32_t(sizeof(ImVec4));
    i.imgui_layout[4] = uint32_t(sizeof(ImDrawVert));
    i.imgui_layout[5] = uint32_t(sizeof(ImDrawIdx));
    i.imgui_layout[6] = uint32_t(sizeof(ImGuiContext));
    i.id = id;
    i.name = name;
    i.version = version;
    i.author = author;
    i.description = description;
    return i;
}

template <class T>
inline int CreatePlugin(const P1HostApi* host, P1PluginApi* out) noexcept {
    static_assert(std::is_base_of_v<Plugin, T>, "the P1_PLUGIN class must derive from p1::Plugin");
    if (!host || !out || !P1SDK_HOST_HAS(host, log) || !P1_HAS_FIELD(out, P1PluginApi, on_load)) return 0;
    GetState().host = host;
    InstallCrashHooks();
    Plugin* p = nullptr;
    try {
        p = new T();
    } catch (const std::exception& e) {
        ReportException("plugin constructor", e.what());
        return 0;
    } catch (...) {
        ReportException("plugin constructor", nullptr);
        return 0;
    }
    GetState().plugin = p;
    // Fill only the fields the host's table has room for (append-only ABI).
    out->self = p;
    out->on_load = &OnLoadThunk;
    if (P1_HAS_FIELD(out, P1PluginApi, on_unload))  out->on_unload  = &OnUnloadThunk;
    if (P1_HAS_FIELD(out, P1PluginApi, on_tick))    out->on_tick    = &OnTickThunk;
    if (P1_HAS_FIELD(out, P1PluginApi, on_overlay)) out->on_overlay = &OnOverlayThunk;
    if (P1_HAS_FIELD(out, P1PluginApi, on_panel))   out->on_panel   = &OnPanelThunk;
    if (P1_HAS_FIELD(out, P1PluginApi, on_event))   out->on_event   = &OnEventThunk;
    return 1;
}

inline void BindImGui(void* ctx, void* allocFn, void* freeFn, void* user) noexcept {
    // Allocator first: everything this DLL's ImGui copy allocates inside the
    // host's context must come from (and go back to) the host's heap.
    if (allocFn && freeFn)
        ImGui::SetAllocatorFunctions(reinterpret_cast<ImGuiMemAllocFunc>(allocFn),
                                     reinterpret_cast<ImGuiMemFreeFunc>(freeFn), user);
    ImGui::SetCurrentContext(static_cast<ImGuiContext*>(ctx));
}

} // namespace detail
} // namespace p1

// Emits the three exports. Use exactly once per plugin DLL, at namespace scope.
#define P1_PLUGIN(Class, id, name, version, author, description)                                              \
    extern "C" P1_EXPORT const P1PluginInfo* P1_CALL P1_GetPluginInfo(void) {                                  \
        static const P1PluginInfo p1_info_ = ::p1::detail::MakeInfo(id, name, version, author, description);   \
        return &p1_info_;                                                                                      \
    }                                                                                                          \
    extern "C" P1_EXPORT int P1_CALL P1_CreatePlugin(const P1HostApi* p1_host_, P1PluginApi* p1_out_) {        \
        return ::p1::detail::CreatePlugin<Class>(p1_host_, p1_out_);                                           \
    }                                                                                                          \
    extern "C" P1_EXPORT void P1_CALL P1_BindImGui(void* p1_ctx_, void* p1_alloc_, void* p1_free_, void* p1_user_) { \
        ::p1::detail::BindImGui(p1_ctx_, p1_alloc_, p1_free_, p1_user_);                                       \
    }
