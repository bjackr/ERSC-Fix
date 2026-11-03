#pragma once

#include "hooks/hooks.h"

#include <cstddef>
#include <cstdint>

namespace ersc_fix::hooks::summon_replay {

void start(const Context& context);

void note_spawn(uint64_t peer, const uint8_t* payload, size_t len);
void note_despawn(uint64_t peer, const uint8_t* payload, size_t len);

void note_position(uint64_t peer, const uint8_t* payload, size_t len);

bool own_stream_position(uint32_t& block, float& x, float& y, float& z);

void note_owner_state(uint64_t peer, const uint8_t* payload, size_t len);

void note_chr_status(uint64_t peer, const uint8_t* payload, size_t len);

void note_game_packet(uint64_t peer, uint32_t type, const uint8_t* payload, size_t len);

}
