#pragma once

#include <cstdint>
#include <string>

namespace ersc_fix::game::signatures {

struct Resolved {
    bool complete = false;
    uint32_t world_chr_man = 0, cs_event_flag_man = 0, session_man = 0, get_event_flag = 0;
    uint32_t vt_network = 0, vt_pad = 0, vt_netai = 0;
    uint32_t ride_mount = 0, ride_dismount = 0;
    uint32_t net_player_spawn = 0;
    uint32_t chr_set_disabled = 0;
    uint32_t set_event_flag = 0;
    uint32_t volatile_reset = 0;
    uint32_t flag_block_apply = 0;
    uint32_t pack_block_flags = 0, pack_volatile_flags = 0;
    uint32_t flag_slot_legacy = 0;
    uint32_t bullet_spawn = 0;
    uint32_t field_ins_resolve = 0;
    std::string report;
};

Resolved resolve(uintptr_t game_base);

}
