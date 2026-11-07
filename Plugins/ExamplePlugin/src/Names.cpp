#include "Names.h"

#include <IconsFontAwesome6.h>
#include <windows.h>

#include "Util.h"

namespace ex {

const char* StateName(int32_t state) {
    switch (state) {
        case P1_STATE_AREA_LOADING:     return "Area loading";
        case P1_STATE_ESCAPE:           return "Escape menu";
        case P1_STATE_IN_GAME:          return "In game";
        case P1_STATE_LOGIN:            return "Login";
        case P1_STATE_WAITING:          return "Waiting";
        case P1_STATE_SELECT_CHARACTER: return "Select character";
        case P1_STATE_LOADING:          return "Loading";
        case P1_STATE_NOT_LOADED:       return "Not loaded";
        default:                        return "Unknown state";
    }
}

const char* EventName(uint32_t type) {
    switch (type) {
        case P1_EV_ATTACHED:          return "ATTACHED";
        case P1_EV_DETACHED:          return "DETACHED";
        case P1_EV_STATE_CHANGED:     return "STATE_CHANGED";
        case P1_EV_AREA_CHANGED:      return "AREA_CHANGED";
        case P1_EV_CHARACTER_CHANGED: return "CHARACTER_CHANGED";
        case P1_EV_PANEL_SHOWN:       return "PANEL_SHOWN";
        case P1_EV_PANEL_HIDDEN:      return "PANEL_HIDDEN";
        case P1_EV_PERMISSIONS:       return "PERMISSIONS";
        case P1_EV_LANGUAGE:          return "LANGUAGE";
        case P1_EV_GAME_FOCUS:        return "GAME_FOCUS";
        case P1_EV_INPUT_GATE:        return "INPUT_GATE";
        default:                      return "UNKNOWN";
    }
}

const char* KindName(uint8_t kind) {
    switch (kind) {
        case P1_KIND_PLAYER:          return "Player";
        case P1_KIND_MONSTER:         return "Monster";
        case P1_KIND_NPC:             return "NPC";
        case P1_KIND_CHEST:           return "Chest";
        case P1_KIND_WORLD_ITEM:      return "Item";
        case P1_KIND_AREA_TRANSITION: return "Transition";
        case P1_KIND_PORTAL:          return "Portal";
        case P1_KIND_WAYPOINT:        return "Waypoint";
        case P1_KIND_EFFECT:          return "Effect";
        case P1_KIND_MISC_OBJECT:     return "Object";
        default:                      return "Unknown";
    }
}

const char* KindIcon(uint8_t kind) {
    switch (kind) {
        case P1_KIND_PLAYER:          return ICON_FA_USER;
        case P1_KIND_MONSTER:         return ICON_FA_SKULL;
        case P1_KIND_NPC:             return ICON_FA_USERS;
        case P1_KIND_CHEST:           return ICON_FA_BOX_OPEN;
        case P1_KIND_WORLD_ITEM:      return ICON_FA_GEM;
        case P1_KIND_AREA_TRANSITION: return ICON_FA_DOOR_OPEN;
        case P1_KIND_PORTAL:          return ICON_FA_CIRCLE;
        case P1_KIND_WAYPOINT:        return ICON_FA_LOCATION_ARROW;
        case P1_KIND_EFFECT:          return ICON_FA_WAND_MAGIC_SPARKLES;
        case P1_KIND_MISC_OBJECT:     return ICON_FA_CUBE;
        default:                      return ICON_FA_CIRCLE_QUESTION;
    }
}

const char* RarityName(uint8_t rarity) {
    switch (rarity) {
        case 0:  return "Normal";
        case 1:  return "Magic";
        case 2:  return "Rare";
        case 3:  return "Unique";
        default: return "-";
    }
}

const char* LogLevelName(int level) {
    switch (level) {
        case P1_LOG_DEBUG: return "Debug";
        case P1_LOG_INFO:  return "Info";
        case P1_LOG_WARN:  return "Warn";
        case P1_LOG_ERROR: return "Error";
        default:           return "?";
    }
}

const char* NotifyLevelName(int level) {
    switch (level) {
        case P1_NOTIFY_INFO:    return "Info";
        case P1_NOTIFY_SUCCESS: return "Success";
        case P1_NOTIFY_WARNING: return "Warning";
        case P1_NOTIFY_ERROR:   return "Error";
        default:                return "?";
    }
}

namespace {

constexpr BitName kFlags[] = {
    { P1_SNAP_ATTACHED, "ATTACHED", "The app is attached to a game process. Set by the app's game reader." },
    { P1_SNAP_ATTACHING, "ATTACHING", "The app's first scan of a new game process is running." },
    { P1_SNAP_WGC_MODE, "WGC_MODE", "The app is in visual (WGC) mode: it reads no game memory, so no game data." },
    { P1_SNAP_IN_GAME, "IN_GAME", "game_state is P1_STATE_IN_GAME: a character is in an area." },
    { P1_SNAP_LOADING, "LOADING", "An area-loading state." },
    { P1_SNAP_INPUT_FOCUSED, "INPUT_FOCUSED", "A text field in the game (chat) owns the keyboard: keys_down is all zero." },
    { P1_SNAP_WORKER_FAULT, "WORKER_FAULT", "The game read faulted this tick: the data may be partial." },
    { P1_SNAP_TRADE_BUSY, "TRADE_BUSY", "The app's trade or stash automation owns the mouse. Plugin input is refused (Submit returns 0) and "
      "anything queued is dropped." },
    { P1_SNAP_RECOVERING, "RECOVERING", "The app's AFK or disconnect recovery is running. Plugin input is refused (Submit returns 0) "
      "and anything queued is dropped." },
    { P1_SNAP_CAMERA_VALID, "CAMERA_VALID", "camera is valid: world positions can be projected to the screen." },
    { P1_SNAP_BUFFS_VALID, "BUFFS_VALID", "Gate: AutoPot. Player buffs are read only while the app's AutoPot runs." },
    { P1_SNAP_MAP_VALID, "MAP_VALID", "Gate: radar. large_map / mini_map are read only while the app's radar runs." },
    { P1_SNAP_INVENTORY_VALID, "INVENTORY_VALID", "The main inventory (12 x 5) is valid." },
};

constexpr BitName kComponents[] = {
    { P1_COMP_LIFE, "Life", "" },
    { P1_COMP_POSITIONED, "Positioned", "" },
    { P1_COMP_RENDER, "Render", "" },
    { P1_COMP_CHEST, "Chest", "" },
    { P1_COMP_PLAYER, "Player", "" },
    { P1_COMP_OMP, "ObjectMagicProperties", "" },
    { P1_COMP_MONSTER, "Monster", "" },
    { P1_COMP_NPC, "NPC", "" },
    { P1_COMP_SHRINE, "Shrine", "" },
    { P1_COMP_AREA_TRANSITION, "AreaTransition", "" },
    { P1_COMP_PORTAL, "Portal", "" },
    { P1_COMP_WAYPOINT, "Waypoint", "" },
    { P1_COMP_MINIMAP_ICON, "MinimapIcon", "" },
    { P1_COMP_TARGETABLE, "Targetable", "" },
    { P1_COMP_TRANSITIONABLE, "Transitionable", "" },
    { P1_COMP_TRIGGERABLE_BLOCKAGE, "TriggerableBlockage", "" },
    { P1_COMP_WORLD_ITEM, "WorldItem", "" },
    { P1_COMP_STATE_MACHINE, "StateMachine", "" },
    { P1_COMP_DIES_AFTER_TIME, "DiesAfterTime", "" },
    { P1_COMP_BUFFS, "Buffs", "" },
    { P1_COMP_ACTOR, "Actor", "" },
    { P1_COMP_STATS, "Stats", "" },
};

}  // namespace

std::span<const BitName> SnapshotFlagNames() { return kFlags; }
std::span<const BitName> ComponentNames() { return kComponents; }

std::string PermissionsText(uint32_t perms) {
    const bool mem = (perms & P1_PERM_MEMORY) != 0, in = (perms & P1_PERM_INPUT) != 0;
    if (mem && in) return "memory + input";
    if (mem) return "memory";
    if (in) return "input";
    return "none";
}

std::string VkName(int vk) {
    if (vk >= 'A' && vk <= 'Z') return std::string(1, static_cast<char>(vk));
    if (vk >= '0' && vk <= '9') return std::string(1, static_cast<char>(vk));
    if (vk >= VK_F1 && vk <= VK_F24) return Fmt("F%d", vk - VK_F1 + 1);
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) return Fmt("Num %d", vk - VK_NUMPAD0);
    switch (vk) {
        case VK_LBUTTON:  return "Left mouse";
        case VK_RBUTTON:  return "Right mouse";
        case VK_MBUTTON:  return "Middle mouse";
        case VK_XBUTTON1: return "Mouse 4";
        case VK_XBUTTON2: return "Mouse 5";
        case VK_BACK:     return "Backspace";
        case VK_TAB:      return "Tab";
        case VK_RETURN:   return "Enter";
        case VK_SHIFT:    return "Shift";
        case VK_CONTROL:  return "Ctrl";
        case VK_MENU:     return "Alt";
        case VK_PAUSE:    return "Pause";
        case VK_CAPITAL:  return "Caps Lock";
        case VK_ESCAPE:   return "Esc";
        case VK_SPACE:    return "Space";
        case VK_PRIOR:    return "Page Up";
        case VK_NEXT:     return "Page Down";
        case VK_END:      return "End";
        case VK_HOME:     return "Home";
        case VK_LEFT:     return "Left";
        case VK_UP:       return "Up";
        case VK_RIGHT:    return "Right";
        case VK_DOWN:     return "Down";
        case VK_INSERT:   return "Insert";
        case VK_DELETE:   return "Delete";
        case VK_LWIN:     return "Left Win";
        case VK_RWIN:     return "Right Win";
        case VK_LSHIFT:   return "Left Shift";
        case VK_RSHIFT:   return "Right Shift";
        case VK_LCONTROL: return "Left Ctrl";
        case VK_RCONTROL: return "Right Ctrl";
        case VK_LMENU:    return "Left Alt";
        case VK_RMENU:    return "Right Alt";
        case VK_OEM_3:    return "`";
        case VK_OEM_MINUS: return "-";
        case VK_OEM_PLUS: return "=";
        case VK_OEM_4:    return "[";
        case VK_OEM_6:    return "]";
        case VK_OEM_1:    return ";";
        case VK_OEM_7:    return "'";
        case VK_OEM_COMMA: return ",";
        case VK_OEM_PERIOD: return ".";
        case VK_OEM_2:    return "/";
        case VK_OEM_5:    return "\\";
        default:          return Fmt("VK 0x%02X", static_cast<unsigned>(vk));
    }
}

}  // namespace ex
