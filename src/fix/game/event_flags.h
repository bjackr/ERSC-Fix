#pragma once

#include <cstdint>
#include <optional>

namespace ersc_fix::game::event_flags {

void init(uintptr_t game_base);
bool available();

std::optional<bool> get(uint32_t flag_id);

bool set(uint32_t flag_id, bool value);
bool can_set();

}
