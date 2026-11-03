#pragma once

#include "hooks/hooks.h"

namespace ersc_fix::hooks::exit_abort {

void arm(const Context& context);

void on_process_exit();

}
