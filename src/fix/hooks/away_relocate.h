#pragma once

#include "hooks/hooks.h"

#include <cstddef>
#include <cstdint>

namespace ersc_fix::hooks::away_relocate {

void start(const Context& context);

bool rewrite(uint64_t peer, uint8_t* payload, size_t len);

}
