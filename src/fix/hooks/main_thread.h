#pragma once

#include "hooks/hooks.h"

#include <cstdint>
#include <functional>

namespace ersc_fix::hooks::main_thread {

void start(const Context& context);

bool post(std::function<void()> task);

uint32_t thread_id();

}
