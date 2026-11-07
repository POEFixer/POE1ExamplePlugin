#include "Snippets.h"

namespace ex {

namespace {

constexpr Snippet kSnippets[] = {
#include "generated/Snippets.inc"
};

}  // namespace

std::span<const Snippet> AllSnippets() { return kSnippets; }

const Snippet* FindSnippet(std::string_view id) {
    for (const Snippet& s : kSnippets)
        if (id == s.id) return &s;
    return nullptr;
}

}  // namespace ex
