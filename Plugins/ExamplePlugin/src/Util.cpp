#include "Util.h"

#include <cstdarg>
#include <cstdio>

namespace ex {

std::string Fmt(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    va_list again;
    va_copy(again, ap);
    const int n = std::vsnprintf(nullptr, 0, fmt, ap);   // measure first, then write once
    va_end(ap);
    std::string out;
    if (n > 0) {
        out.resize(static_cast<size_t>(n) + 1);
        std::vsnprintf(out.data(), out.size(), fmt, again);
        out.resize(static_cast<size_t>(n));
    }
    va_end(again);
    return out;
}

std::string FormatSpanUs(int64_t us) {
    if (us < 0) us = 0;
    if (us < 1000) return Fmt("%lld µs", static_cast<long long>(us));
    if (us < 10'000) return Fmt("%.1f ms", double(us) / 1e3);
    if (us < 1'000'000) return Fmt("%.0f ms", double(us) / 1e3);
    if (us < 60'000'000) return Fmt("%.1f s", double(us) / 1e6);
    const long long s = static_cast<long long>(us / 1'000'000);
    if (s < 3600) return Fmt("%lld min %lld s", s / 60, s % 60);
    return Fmt("%lld h %lld min", s / 3600, (s / 60) % 60);
}

std::string FormatCostUs(double us) {
    if (!(us >= 0.0)) us = 0.0;   // also catches NaN
    if (us < 1000.0) return Fmt("%.0f µs", us);
    return Fmt("%.2f ms", us / 1000.0);
}

std::string FormatBytes(uint64_t bytes) {
    if (bytes < 1024) return Fmt("%llu B", static_cast<unsigned long long>(bytes));
    if (bytes < (1ull << 20)) return Fmt("%.1f KiB", double(bytes) / 1024.0);
    if (bytes < (1ull << 30)) return Fmt("%.2f MiB", double(bytes) / double(1ull << 20));
    return Fmt("%.2f GiB", double(bytes) / double(1ull << 30));
}

std::string FormatCount(uint64_t n) {
    const std::string digits = std::to_string(n);
    std::string out;
    out.reserve(digits.size() + digits.size() / 3);
    for (size_t i = 0; i < digits.size(); ++i) {
        // A separator before every group of three counted from the right.
        if (i != 0 && (digits.size() - i) % 3 == 0) out.push_back(',');
        out.push_back(digits[i]);
    }
    return out;
}

bool ParseHex(std::string_view text, uint64_t& out) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) text.remove_suffix(1);
    if (text.size() >= 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) text.remove_prefix(2);
    uint64_t value = 0;
    int digits = 0;
    for (const char c : text) {
        if (c == '`' || c == '_') continue;
        int d = -1;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        if (d < 0 || ++digits > 16) return false;
        value = (value << 4) | static_cast<uint64_t>(d);
    }
    if (digits == 0) return false;
    out = value;
    return true;
}

std::string LastPathSegment(std::string_view path) {
    const size_t slash = path.find_last_of('/');
    std::string_view last = slash == std::string_view::npos ? path : path.substr(slash + 1);
    const size_t at = last.find('@');
    if (at != std::string_view::npos) last = last.substr(0, at);
    return std::string(last);
}

std::string ShortPath(std::string_view path, int segments) {
    size_t cut = path.size();
    for (int i = 0; i < segments; ++i) {
        const size_t slash = path.find_last_of('/', cut == 0 ? 0 : cut - 1);
        if (slash == std::string_view::npos || cut == 0) return std::string(path);
        cut = slash;
    }
    return "…" + std::string(path.substr(cut));
}

std::string Utf8Prefix(std::string_view text, int count) {
    size_t end = 0;
    for (int seen = 0; end < text.size(); ++end) {
        const bool lead = (static_cast<unsigned char>(text[end]) & 0xC0u) != 0x80u;   // not a continuation byte
        if (lead && seen++ == count) break;
    }
    return std::string(text.substr(0, end));
}

bool ContainsNoCase(std::string_view text, std::string_view needle) {
    if (needle.empty()) return true;
    if (needle.size() > text.size()) return false;
    auto lower = [](char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; };
    for (size_t i = 0; i + needle.size() <= text.size(); ++i) {
        size_t k = 0;
        while (k < needle.size() && lower(text[i + k]) == lower(needle[k])) ++k;
        if (k == needle.size()) return true;
    }
    return false;
}

}  // namespace ex
