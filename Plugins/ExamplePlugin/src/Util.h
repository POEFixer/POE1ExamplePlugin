#pragma once
// ============================================================================
// Util — formatting and parsing helpers every file of the example uses.
// Nothing here knows about the SDK; copy what you need.
// ============================================================================
#include <sal.h>

#include <cstdint>
#include <string>
#include <string_view>

namespace ex {

// printf into a std::string (UTF-8 in, UTF-8 out).
std::string Fmt(_Printf_format_string_ const char* fmt, ...);

// A time span for people: "850 µs", "12 ms", "3.4 s", "2 min 5 s", "1 h 20 min".
std::string FormatSpanUs(int64_t us);
// A callback cost: "42 µs" below a millisecond, "1.25 ms" above.
std::string FormatCostUs(double us);
// "512 B", "4.0 KiB", "1.50 MiB".
std::string FormatBytes(uint64_t bytes);
// "150,000".
std::string FormatCount(uint64_t n);

// Hexadecimal text to a number: optional "0x", and the ` and _ digit separators that
// debuggers print ("7FF6`12340000"). At most 16 digits. false on anything else.
bool ParseHex(std::string_view text, uint64_t& out);

// "Metadata/Monsters/Zombie/ZombieBasic@83" -> "ZombieBasic" (last segment, no "@...").
std::string LastPathSegment(std::string_view path);
// The same path cut to its last `segments` parts: "…/Zombie/ZombieBasic@83".
std::string ShortPath(std::string_view path, int segments = 2);

// ASCII case-insensitive "does `text` contain `needle`" (paths and ids are ASCII).
bool ContainsNoCase(std::string_view text, std::string_view needle);

// The first `count` characters (code points) of UTF-8 text. Cutting by bytes could split a
// Cyrillic or CJK character in two, which ImGui draws as '?'.
std::string Utf8Prefix(std::string_view text, int count);

}  // namespace ex
