#pragma once

#include "game/offsets.h"

#include <cstdint>
#include <vector>

namespace ersc_fix::hooks::ride_probe::detail {

struct ChrInfo {
    uintptr_t chr = 0;
    ersc_fix::game::FieldInsHandle handle{-1, 0xFFFFFFFFu};
    int32_t npc_id = 0;
};

extern uintptr_t g_game_base;

std::vector<ChrInfo> chr_set(uintptr_t set, uint32_t cap_off, uint32_t entries_off);

}
