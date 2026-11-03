#include "hooks/spawn_repair.h"

#include "game/mem.h"
#include "game/offsets.h"
#include "hooks/mount_replay.h"
#include "config.h"
#include "log.h"

#include <windows.h>

#include <safetyhook.hpp>

#include <cstdint>
#include <format>
#include <string>
#include <vector>

namespace ersc_fix::hooks::spawn_repair {

namespace {

using namespace ersc_fix::game;
using namespace ersc_fix::game::mem;

constexpr uint32_t kFlagsRiding = 0x0D, kFlagBlocked = 0x02, kFlagRequired = 0x08, kFlagRiding = 0x01;

SafetyHookInline g_hook;
uintptr_t g_game_base = 0;
bool g_preplace = true;

struct HorseSeen {
    bool found = false;
    uintptr_t chr = 0;
    uint32_t load = 0;
    bool model = false;
};

HorseSeen find_horse(uint64_t handle) {
    HorseSeen h;
    uintptr_t wcm = deref(g_game_base + kWorldChrManPtr);
    uintptr_t buddy_man = wcm ? deref(wcm + kWcmSummonBuddyManager) : 0;
    uintptr_t set = buddy_man ? deref(buddy_man + kBuddyManChrSet) : 0;
    uint32_t cap = 0;
    uintptr_t entries = 0;
    if (!set || !read(set + kChrSetCapacity, cap) || !read(set + kChrSetEntries, entries) || cap > 4096 || !entries) return h;
    for (uint32_t i = 0; i < cap; ++i) {
        uintptr_t chr = deref(entries + 0x10ull * i);
        uint64_t hd = 0;
        if (!chr || !read(chr + kChrHandle, hd) || hd != handle) continue;
        h.found = true;
        h.chr = chr;
        read(chr + kChrLoadState, h.load);
        h.model = deref(chr + kChrModelIns) != 0;
        break;
    }
    return h;
}

uintptr_t copy_for(uintptr_t pgd) {
    uintptr_t wcm = deref(g_game_base + kWorldChrManPtr);
    uint32_t cap = 0;
    uintptr_t entries = 0;
    if (!wcm || !read(wcm + kWcmPlayerSetCapacity, cap) || !read(wcm + kWcmPlayerSetEntries, entries) || cap > 64) return 0;
    for (uint32_t k = 0; k < cap; ++k) {
        uintptr_t chr = deref(entries + 0x10ull * k);
        if (chr && deref(chr + kPlayerGameData) == pgd) return chr;
    }
    return 0;
}

void detour(void* self, void* member_data, void* member) {
    const auto pgd = reinterpret_cast<uintptr_t>(member_data);
    uint64_t horse = 0;
    uint32_t flags = 0;
    const bool have_handle = pgd && read(pgd + kPgdRideHorseHandle, horse);
    const uintptr_t ride_state = pgd ? deref(pgd + kPgdRideState) : 0;
    const bool have_flags = ride_state && read(ride_state + kRideStateFlags, flags);

    const uint64_t owner = have_handle ? mount_replay::owner_mounted_on(static_cast<uint32_t>(horse)) : 0;
    std::string repaired;
    if (have_flags && have_handle && !(flags & kFlagBlocked) && (flags & kFlagsRiding) != kFlagsRiding && owner) {
        const uint32_t fixed = flags | kFlagsRiding;
        if (write(ride_state + kRideStateFlags, fixed)) {
            repaired = std::format(" | REPAIRED flags {:08X} -> {:08X} (owner's records say mounted on this horse)", flags, fixed);
            flags = fixed;
        } else {
            repaired = " | repair FAILED (flags not writable)";
        }
    }
    const bool riding = have_flags && (flags & kFlagRiding) && !(flags & kFlagBlocked) && (flags & kFlagRequired);

    std::string delivered;
    if (owner) delivered = mount_replay::redeliver(owner);

    std::string preplaced;
    if (owner && g_preplace && have_handle) {
        const HorseSeen before = find_horse(horse);
        uint32_t blk = 0xFFFFFFFFu;
        if (before.found) read(before.chr + kChrBlockId, blk);
        const uint32_t want = mount_replay::owner_block(owner);
        if (before.found && blk == 0xFFFFFFFFu && want != 0xFFFFFFFFu) {
            if (write(before.chr + kChrBlockId, want)) preplaced = std::format(" | horse PRE-PLACED in block {:08X} (owner's stream) before the pairing", want);
            else preplaced = " | horse pre-placement FAILED (block field not writable)";
        } else if (before.found && blk != 0xFFFFFFFFu) {
            preplaced = std::format(" | horse already in block {:08X}", blk);
        } else if (!before.found) {
            preplaced = " | horse object not created yet (will be born with the block from the table)";
        } else {
            preplaced = " | no block known for the owner (no stream, no notice): horse left unplaced";
        }
    }

    g_hook.call<void>(self, member_data, member);

    uint32_t h0 = 0, req = 0, node = 0;
    uintptr_t req_obj = 0, node_obj = 0, ride_mod = 0;
    const uintptr_t copy = copy_for(pgd);
    if (copy) {
        read(copy + kChrHandle, h0);
        req_obj = deref(copy + kChrRideRequestObj);
        if (req_obj) read(req_obj + kRideReqState, req);
        ride_mod = deref(deref(copy + kChrModules) + kModulesRide);
        if (ride_mod) {
            node_obj = deref(ride_mod + kRideNode);
            if (node_obj) read(node_obj + kRideNodeState, node);
        }
    }
    const HorseSeen h = have_handle ? find_horse(horse) : HorseSeen{};

    if (owner && g_preplace && h.chr) {
        uint32_t blk = 0xFFFFFFFFu;
        read(h.chr + kChrBlockId, blk);
        const uint32_t want = mount_replay::owner_block(owner);
        if (blk == 0xFFFFFFFFu && want != 0xFFFFFFFFu) {
            if (write(h.chr + kChrBlockId, want)) preplaced += std::format(" | horse PRE-PLACED in block {:08X} (owner's stream) right after the spawn, before the pairing", want);
            else preplaced += " | horse pre-placement FAILED after the spawn (block field not writable)";

            float pos[3];
            const uintptr_t physics = deref(deref(h.chr + kChrModules) + kModulesPhysics);
            if (physics && mount_replay::owner_position(owner, pos)) {
                bool ok = true;
                for (int i = 0; i < 3; ++i) {
                    ok = write(physics + kPhysicsPosition + 4 * i, pos[i]) && ok;
                    ok = write(physics + kPhysicsPositionPrev + 4 * i, pos[i]) && ok;
                }
                preplaced += ok ? std::format(" and at the owner's position ({:.1f} {:.1f} {:.1f})", pos[0], pos[1], pos[2])
                                : " (position write FAILED)";
            } else {
                preplaced += physics ? " (no owner position streamed yet: left at the origin)" : " (no physics module yet: position left at the origin)";
            }
        } else if (blk != 0xFFFFFFFFu) {
            preplaced += std::format(" | horse born in block {:08X}", blk);
        } else {
            preplaced += " | no block known for the owner after the spawn: horse left unplaced";
        }
    }
}

}

bool install(const Context& context) {
    g_game_base = reinterpret_cast<uintptr_t>(context.game);
    g_preplace = GetPrivateProfileIntW(L"hooks", L"spawn_preplace", 1, config::ini_path().c_str()) != 0;
    if (!kNetPlayerSpawn) {
        log::error("spawn repair: the net-player spawn was not found by the signature scan");
        return false;
    }
    auto result = safetyhook::InlineHook::create(reinterpret_cast<char*>(context.game) + kNetPlayerSpawn, reinterpret_cast<void*>(&detour));
    if (!result) {
        log::error("spawn repair: failed to hook the net-player spawn at eldenring.exe+0x{:X}", kNetPlayerSpawn);
        return false;
    }
    g_hook = std::move(*result);
    log::info("spawn repair: hooked the net-player spawn at eldenring.exe+0x{:X} (sets a partner's stale ride flags at creation when their "
              "ride records say mounted, re-delivers their mount burst there, pre-places their horse in the owner's block {})",
              kNetPlayerSpawn, g_preplace ? "ON" : "off");
    return true;
}

}
