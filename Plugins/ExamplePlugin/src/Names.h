#pragma once
// ============================================================================
// Names — readable text for the SDK's numbers: game states, events, entity
// kinds and components, snapshot flags, permissions and virtual keys.
//
// Every table answers "unknown" for a value it does not list: a newer app may
// add states, kinds, components or events, and a plugin must keep running.
// ============================================================================
#include <p1sdk/p1_abi.h>

#include <cstdint>
#include <span>
#include <string>

namespace ex {

const char* StateName(int32_t state);          // P1_STATE_*
const char* EventName(uint32_t type);          // P1_EV_* without the prefix
const char* KindName(uint8_t kind);            // P1_KIND_*
const char* KindIcon(uint8_t kind);            // a Font Awesome glyph per kind
const char* RarityName(uint8_t rarity);        // 0..3, 0xFF = none
const char* LogLevelName(int level);           // P1_LOG_*
const char* NotifyLevelName(int level);        // P1_NOTIFY_*

struct BitName {
    uint32_t    bit;
    const char* name;
    const char* note;   // what it means, and who sets it
};
std::span<const BitName> SnapshotFlagNames();  // P1_SNAP_*
std::span<const BitName> ComponentNames();     // P1_COMP_*

// "memory + input", "memory", "none".
std::string PermissionsText(uint32_t perms);
// "F7", "A", "Left mouse", ... ; "VK 0x%02X" for keys without a short name.
std::string VkName(int vk);

}  // namespace ex
