#pragma once
// ============================================================================
// Snippets — the code excerpts behind every lesson's "Copy snippet" button.
//
// They cannot drift from the code: an excerpt IS the code between a
// "// [snippet:<id>]" and a "// [/snippet]" line somewhere under src/, copied
// verbatim into src/generated/Snippets.inc by tools/gen_snippets.py.
// "python tools/gen_snippets.py --check" fails when the copy is stale.
// ============================================================================
#include <span>
#include <string_view>

namespace ex {

struct Snippet {
    const char* id;        // "input", "textures", ...
    const char* file;      // "src/lessons/Input.cpp" (relative to the plugin folder)
    const char* function;  // "SubmitSafeDemo()" or "class Worker"
    int line;              // first line of the excerpt in `file`
    const char* text;      // the excerpt, dedented, at most 2000 bytes (the app's clipboard limit)
};

std::span<const Snippet> AllSnippets();
const Snippet* FindSnippet(std::string_view id);

}  // namespace ex
