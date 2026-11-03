#include "game/build.h"

#include "common/game_builds.h"
#include "game/mem.h"
#include "game/offsets.h"
#include "game/signatures.h"
#include "log.h"

#include <windows.h>

#include <format>

namespace ersc_fix::game::build {

namespace {

Info g_info;

void apply(uint32_t wcm, uint32_t ev, uint32_t sess, uint32_t get_flag, uint32_t vt_net, uint32_t vt_pad, uint32_t vt_netai,
           uint32_t vt_ride) {
    kWorldChrManPtr = wcm;
    kCsEventFlagManPtr = ev;
    kSessionManPtr = sess;
    kGetEventFlag = get_flag;
    kVtNetworkManipulator = vt_net;
    kVtPadManipulator = vt_pad;
    kVtNetAIManipulator = vt_netai;
    kVtRideManipulator = vt_ride;
}

}

const Info& detect() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    g_info.version = game_builds::file_version(path);
    const auto* table = game_builds::find(g_info.version);
    const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    const signatures::Resolved sig = signatures::resolve(base);
    log::info("game signatures: {}", sig.report);
    kRideMount = sig.ride_mount;
    kRideDismount = sig.ride_dismount;
    kNetPlayerSpawn = sig.net_player_spawn;
    kChrSetDisabled = sig.chr_set_disabled;
    kSetEventFlag = sig.set_event_flag;
    kVolatileReset = sig.volatile_reset;
    kFlagBlockApply = sig.flag_block_apply;
    kPackBlockFlags = sig.pack_block_flags;
    kPackVolatileFlags = sig.pack_volatile_flags;
    kFlagSlotLegacy = sig.flag_slot_legacy;
    kBulletSpawn = sig.bullet_spawn;
    kFieldInsResolve = sig.field_ins_resolve;

    if (table) {

        apply(table->world_chr_man, table->cs_event_flag_man, table->session_man, table->get_event_flag, table->vt_network,
              table->vt_pad, table->vt_netai, table->vt_ride);
        g_info.known = true;
        g_info.label = table->label;
        const bool agree = !sig.complete || (sig.world_chr_man == table->world_chr_man && sig.session_man == table->session_man &&
                                             sig.cs_event_flag_man == table->cs_event_flag_man &&
                                             (!sig.get_event_flag || sig.get_event_flag == table->get_event_flag));
        log::info("game build: {} -> {} (table); signatures {}", g_info.version, g_info.label,
                  !sig.complete ? "incomplete" : (agree ? "agree" : "DISAGREE: report this before the next patch"));
        if (!sig.get_event_flag) {

            uint8_t b[16];
            std::string hex;
            if (mem::copy(base + table->get_event_flag, b, sizeof(b))) {
                for (uint8_t x : b) hex += std::format("{:02x}", x);
            }
            log::info("game signatures: GetFlag at table +0x{:X} reads {} in memory (pattern 448b411c448bda33d2...)",
                      table->get_event_flag, hex.empty() ? "<unreadable>" : hex);
        }
        return g_info;
    }
    if (sig.complete) {

        apply(sig.world_chr_man, sig.cs_event_flag_man, sig.session_man, sig.get_event_flag, sig.vt_network, sig.vt_pad,
              sig.vt_netai, 0);
        g_info.known = true;
        g_info.label = "resolved by signatures (not in the verified table)";
        log::warn("game build: eldenring.exe {} is not in the verified table; addresses resolved by signature scan. "
                  "Structure offsets are assumed unchanged; check the probe lines look sane.",
                  g_info.version.empty() ? "<no version>" : g_info.version);
        return g_info;
    }
    log::error("game build: eldenring.exe {} is not in the table and the signature scan did not resolve every address",
               g_info.version.empty() ? "<no version>" : g_info.version);
    return g_info;
}

const Info& current() {
    return g_info;
}

}
