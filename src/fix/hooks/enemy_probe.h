#pragma once

#include "hooks/hooks.h"

namespace ersc_fix::hooks::enemy_probe {

void start(const Context& context);

void note_packet(bool send, uint64_t peer, uint8_t channel, uint32_t type, const uint8_t* payload, size_t len);

constexpr uint32_t kDisabledSetType = 0x7D;
void note_disabled_set(uint64_t peer, const uint8_t* payload, size_t len);

struct ChrInfo {
    int32_t npc = -1;
    uint32_t net_block = 0, net_sel = 0, block = 0;
    uint8_t entry_flags = 0;
    bool model = false;
    float x = 0, y = 0, z = 0;
};
bool describe(uintptr_t chr, ChrInfo& out);

bool describe_by_handle(uint64_t handle, ChrInfo& out);
bool peer_holds_disabled_model(int32_t npc, uint32_t block, uint64_t* peer_out, uint32_t* sel_out);

}
