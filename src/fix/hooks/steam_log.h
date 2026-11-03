#pragma once

#include "hooks/hooks.h"

namespace ersc_fix::hooks::steam_log {

void install_async(bool storm_breaker, bool ride_replay);

bool send_raw(uint64_t peer, uint8_t header_channel, uint32_t type, const void* payload, size_t len);

}
