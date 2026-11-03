#pragma once

#include "hooks/hooks.h"

namespace ersc_fix::hooks::ride_probe {

void start(const Context& context, bool repair_stuck_state, bool update_pin, bool dead_flag_repair, bool away_hide);

int64_t ms_since_partner_respawn();

}
