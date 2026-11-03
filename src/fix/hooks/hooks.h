#pragma once

#include "known_builds.h"

#include <windows.h>

namespace ersc_fix::hooks {

struct Context {
    HMODULE game;
    HMODULE ersc;
    const KnownBuild* build;
};

void install_all(const Context& context);

void on_process_exit();

}
