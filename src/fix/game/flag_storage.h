#pragma once

#include <cstdint>
#include <map>
#include <vector>

namespace ersc_fix::game::flag_storage {

void init(uintptr_t game_base);

struct Info {
    uintptr_t mgr = 0, arr = 0;
    uint32_t fpb = 0, bpb = 0;
    int mode = 0;
    uint8_t multi = 0, skip_reset = 0;
    uintptr_t alt_storage = 0;
    uint64_t alt_blocks_158 = 0, alt_blocks_170 = 0;
};
bool info(Info& out);

struct BlockLoc {
    int type = 0;
    uintptr_t storage = 0;
    int slot = -1;
};
bool block_of(uint32_t key, BlockLoc& out);

bool read_block(uint32_t key, std::vector<uint8_t>& bytes);

std::map<int, uint32_t> slot_keys();

uint32_t read_range(uint32_t first, uint32_t count, std::vector<uint8_t>& bits);

int set_cleared(const std::vector<uint32_t>& flags, std::vector<uint32_t>* restored);

}
