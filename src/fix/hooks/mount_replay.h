#pragma once

#include "hooks/hooks.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace ersc_fix::hooks::mount_replay {

void start(const Context& context);

void note_ride_record(uint64_t peer, const uint8_t* payload, size_t len);
void note_block_notice(uint64_t peer, const uint8_t* payload, size_t len);
void note_status(uint64_t peer, const uint8_t* payload, size_t len);
void note_stream(uint64_t peer, const uint8_t* payload, size_t len);

uint64_t owner_mounted_on(uint32_t horse_handle);

int owner_record(uint64_t peer);

std::string redeliver(uint64_t peer);

uint32_t owner_block(uint64_t peer);

bool owner_position(uint64_t peer, float out[3]);

}
