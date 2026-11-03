#include "config.h"
#include "crash_log.h"
#include "guard.h"
#include "common/game_builds.h"
#include "game/build.h"
#include "hooks/hooks.h"
#include "log.h"

#include <windows.h>

namespace {

constexpr DWORD kErscWaitMs = 120'000;

DWORD WINAPI init_thread(LPVOID self) {
    using namespace ersc_fix;

    HMODULE ersc = guard::wait_for_module(L"ersc.dll", kErscWaitMs);
    if (!ersc) return 0;

    auto ersc_path = guard::module_path(ersc);
    log::init(ersc_path.parent_path() / L"ersc_fix_logs");
    log::info("ersc_fix {} loaded from {}", ERSC_FIX_VERSION,
              guard::module_path(static_cast<HMODULE>(self)).string());

    FILETIME created, exited, kernel, user, now;
    if (GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) {
        GetSystemTimeAsFileTime(&now);
        auto ticks = [](FILETIME t) { return (static_cast<uint64_t>(t.dwHighDateTime) << 32) | t.dwLowDateTime; };
        log::info("game process started {:.1f} s before ersc_fix loaded", (ticks(now) - ticks(created)) / 1e7);
    }

    crash_log::install();
    config::load(guard::module_path(static_cast<HMODULE>(self)).parent_path());

    {
        auto ini = (ersc_path.parent_path() / L"ersc_settings.ini").wstring();
        std::string scaling;
        for (const wchar_t* key : {L"enemy_health_scaling", L"enemy_damage_scaling", L"enemy_posture_scaling",
                                   L"boss_health_scaling", L"boss_damage_scaling", L"boss_posture_scaling",
                                   L"allow_summons", L"death_debuffs", L"allow_invaders"}) {

            int v = GetPrivateProfileIntW(L"SCALING", key, -1, ini.c_str());
            if (v == -1) v = GetPrivateProfileIntW(L"GAMEPLAY", key, -1, ini.c_str());
            std::string narrow;
            for (const wchar_t* c = key; *c; ++c) narrow += static_cast<char>(*c);
            scaling += std::format("{}={} ", narrow, v);
        }
        wchar_t pw[128] = {};
        GetPrivateProfileStringW(L"PASSWORD", L"cooppassword", L"", pw, 128, ini.c_str());
        log::info("ersc_settings: {}password_len={}", scaling, std::wcslen(pw));
    }

    auto hash = guard::sha256_file(ersc_path);
    const KnownBuild* build = guard::find_known_build(hash);
    log::info("ersc.dll sha256 {}", hash.empty() ? "<unreadable>" : hash);
    if (!build) {

        const std::string msg = std::format(
            "ersc-fix {} does not recognise this Seamless Co-op build (ersc.dll sha256 {}).\n\nThe game will close. "
            "Use ersc_launcher.exe for stock Seamless Co-op until ersc-fix is updated for it.",
            ERSC_FIX_VERSION, hash.empty() ? "<unreadable>" : hash.substr(0, 16));
        log::error("{}", msg);
        log::flush();
        MessageBoxA(nullptr, msg.c_str(), "ersc-fix", MB_OK | MB_ICONERROR | MB_TOPMOST);
        TerminateProcess(GetCurrentProcess(), 1);
        return 0;
    }
    log::info("recognised build: {}", build->label);

    if (!game::build::detect().known) {
        const std::string msg = std::format(
            "ersc-fix {} does not support Elden Ring {} (supported: {}).\n\nThe game will close. Use "
            "ersc_launcher.exe for stock Seamless Co-op until ersc-fix is updated.",
            ERSC_FIX_VERSION, game::build::current().version, game_builds::supported_versions());
        log::error("{}", msg);
        log::flush();
        MessageBoxA(nullptr, msg.c_str(), "ersc-fix", MB_OK | MB_ICONERROR | MB_TOPMOST);
        TerminateProcess(GetCurrentProcess(), 1);
        return 0;
    }
    hooks::install_all({.game = GetModuleHandleW(nullptr), .ersc = ersc, .build = build});
    return 0;
}

}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);

        if (HANDLE thread = CreateThread(nullptr, 0, init_thread, module, 0, nullptr)) CloseHandle(thread);
    } else if (reason == DLL_PROCESS_DETACH && reserved != nullptr) {

        ersc_fix::hooks::on_process_exit();
        ersc_fix::log::flush();
    }
    return TRUE;
}
