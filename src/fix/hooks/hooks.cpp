#include "hooks/hooks.h"

#include "config.h"
#include "game/build.h"
#include "game/event_flags.h"
#include "hooks/away_relocate.h"
#include "hooks/block_reclaim.h"
#include "hooks/enemy_probe.h"
#include "hooks/bullet_guard.h"
#include "hooks/flag_probe.h"
#include "hooks/main_thread.h"
#include "hooks/exit_abort.h"
#include "hooks/mount_probe.h"
#include "hooks/mount_replay.h"
#include "hooks/ride_probe.h"
#include "hooks/spawn_repair.h"
#include "hooks/steam_log.h"
#include "hooks/summon_replay.h"
#include "log.h"

#include <windows.h>

#include <format>
#include <string>

namespace ersc_fix::hooks {

void install_all(const Context& context) {
    log::info("eldenring.exe base {}, ersc.dll base {}", static_cast<void*>(context.game),
              static_cast<void*>(context.ersc));

    main_thread::start(context);

    const bool exit_abort_on = GetPrivateProfileIntW(L"hooks", L"exit_abort", 1, config::ini_path().c_str()) != 0;
    const bool steam_log_on = GetPrivateProfileIntW(L"hooks", L"steam_log", 1, config::ini_path().c_str()) != 0;
    const bool storm_breaker_on = GetPrivateProfileIntW(L"hooks", L"storm_breaker", 1, config::ini_path().c_str()) != 0;
    log::info("hooks enabled in ersc_fix.ini: exit_abort={} steam_log={} storm_breaker={}", exit_abort_on, steam_log_on, storm_breaker_on);

    if (exit_abort_on) exit_abort::arm(context);
    if (steam_log_on) steam_log::install_async(storm_breaker_on, false);

    if (!game::build::current().known) {
        log::warn("game-memory features (probes, dead-flag repair, block reclaim, Roundtable relocate) are OFF on this "
                  "game build; only the quit-crash fix, Steam logging and the storm breaker run");
        return;
    }
    game::event_flags::init(reinterpret_cast<uintptr_t>(context.game));

    if (GetPrivateProfileIntW(L"hooks", L"ride_probe", 1, config::ini_path().c_str()) != 0) {

        const bool repair = GetPrivateProfileIntW(L"hooks", L"repair_stuck_state", 0, config::ini_path().c_str()) != 0;

        const bool dead_repair = GetPrivateProfileIntW(L"hooks", L"dead_flag_repair", 1, config::ini_path().c_str()) != 0;

        const bool away_hide = GetPrivateProfileIntW(L"hooks", L"away_hide", 1, config::ini_path().c_str()) != 0;
        ride_probe::start(context, repair, false, dead_repair, away_hide);
    }

    if (steam_log_on && GetPrivateProfileIntW(L"hooks", L"block_reclaim", 1, config::ini_path().c_str()) != 0) {
        block_reclaim::start(context);
    }

    if (GetPrivateProfileIntW(L"hooks", L"away_relocate", 1, config::ini_path().c_str()) != 0) {
        away_relocate::start(context);
    }

    if (steam_log_on && GetPrivateProfileIntW(L"hooks", L"summon_replay", 1, config::ini_path().c_str()) != 0) {
        summon_replay::start(context);
    }

    if (GetPrivateProfileIntW(L"hooks", L"enemy_probe", 1, config::ini_path().c_str()) != 0) enemy_probe::start(context);
    if (GetPrivateProfileIntW(L"hooks", L"flag_probe", 1, config::ini_path().c_str()) != 0) flag_probe::start(context);

    if (GetPrivateProfileIntW(L"hooks", L"bullet_guard", 1, config::ini_path().c_str()) != 0) bullet_guard::start(context);

    if (GetPrivateProfileIntW(L"hooks", L"mount_probe", 1, config::ini_path().c_str()) != 0) {
        mount_probe::start(context);
    }

    const bool mount_replay_on = steam_log_on && GetPrivateProfileIntW(L"hooks", L"mount_replay", 1, config::ini_path().c_str()) != 0;
    if (mount_replay_on) mount_replay::start(context);

    if (mount_replay_on && GetPrivateProfileIntW(L"hooks", L"spawn_repair", 1, config::ini_path().c_str()) != 0 &&
        !spawn_repair::install(context)) {
        const std::string msg = std::format(
            "ersc-fix {}: the bug-003 ride-flag repair could not be installed on this game build.\n\n"
            "The game will close. Set spawn_repair = 0 under [hooks] in SeamlessCoop\\fix\\ersc_fix.ini to run without it.",
            ERSC_FIX_VERSION);
        log::error("{}", msg);
        log::flush();
        MessageBoxA(nullptr, msg.c_str(), "ersc-fix", MB_OK | MB_ICONERROR | MB_TOPMOST);
        TerminateProcess(GetCurrentProcess(), 1);
    }

}

void on_process_exit() {
    exit_abort::on_process_exit();
}

}
