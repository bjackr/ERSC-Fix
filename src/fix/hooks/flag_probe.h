#pragma once

#include "hooks/hooks.h"

#include <cstddef>
#include <cstdint>

namespace ersc_fix::hooks::flag_probe {

constexpr uint32_t kFlagBitmapType = 0x7C;

void start(const Context& context);

void note_peer_flags(uint64_t peer, const uint8_t* payload, size_t len);

void note_peer_reload(uint64_t peer, const char* how);

void note_peer_block_request(uint64_t peer, uint32_t block, const char* how);

void note_own_reload(const char* how);

void note_own_block_request(uint32_t block, const char* how);

enum class Reload { none, pending, joint, unilateral };
Reload reload_verdict();

uint64_t reload_peer();

}
