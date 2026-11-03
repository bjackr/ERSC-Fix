#pragma once

#include "hooks/steam_types.h"

#include <cstddef>
#include <cstdint>

namespace ersc_fix::hooks::inject {

void note_channel(uint8_t header_channel, int local_channel);

void note_peer(const SteamNetworkingIdentity& peer);
bool known_peer(uint64_t steam_id);

size_t peers(uint64_t* out, size_t max);

void queue(uint64_t peer, uint8_t header_channel, uint32_t type, const void* payload, size_t len);

int inject(int local_channel, SteamNetworkingMessage** out, int capacity);

}
