#pragma once

#include "hooks/hooks.h"

#include <cstdint>
#include <string>

namespace ersc_fix::hooks::mount_probe {

void start(const Context& context);

bool should_drop_horse_reset(uint64_t peer, uint8_t horse_type, std::string& why);

}
