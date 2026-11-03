#include "hooks/ride_probe.h"

#include "game/mem.h"
#include "game/offsets.h"
#include "hooks/ride_probe_internal.h"
#include "log.h"

#include <windows.h>
#include <intrin.h>

#include <atomic>
#include <cstdint>
#include <format>
#include <map>

namespace ersc_fix::hooks::ride_probe {

namespace detail {

using namespace ersc_fix::game;
using namespace ersc_fix::game::mem;

uintptr_t g_game_base = 0;

std::vector<ChrInfo> chr_set(uintptr_t set, uint32_t cap_off, uint32_t entries_off) {
    std::vector<ChrInfo> out;
    uint32_t capacity = 0;
    uintptr_t entries = 0;
    if (!set || !read(set + cap_off, capacity) || !read(set + entries_off, entries)) return out;
    if (capacity > 4096 || !entries) return out;
    for (uint32_t i = 0; i < capacity; ++i) {
        ChrInfo c;
        c.chr = deref(entries + 0x10ull * i);
        if (!c.chr) continue;
        read(c.chr + kChrHandle, c.handle);
        read(c.chr + kChrNpcId, c.npc_id);
        out.push_back(c);
    }
    return out;
}

}

namespace {

using namespace ersc_fix::game;
using namespace ersc_fix::game::mem;
using detail::chr_set;
using detail::ChrInfo;
using detail::g_game_base;

bool g_dead_repair = false;
struct DeadWatch {
    bool saw_swap = false;
    int dead_samples = 0;
};
std::map<uintptr_t, DeadWatch> g_dead_watch;
constexpr int kDeadFallbackSamples = 120;
std::atomic<uint64_t> g_partner_respawn_ms{0};

void repair_dead_flags(uintptr_t chr) {
    uint32_t load = 0;
    uintptr_t ctrl = deref(chr + kChrCtrl);
    auto& w = g_dead_watch[chr];
    if (!ctrl || !read(chr + kChrLoadState, load)) return;
    if ((load & kLoadStateRemoteBit) == 0) {
        w.saw_swap = true;
        w.dead_samples = 0;
        return;
    }
    uint32_t ctrl_dead = 0, state_dead = 0;
    uintptr_t state = deref(ctrl + kCtrlStateObj);
    read(ctrl + kCtrlDeadFlags, ctrl_dead);
    if (state) read(state + kCtrlStateDeadWord, state_dead);
    const bool dead = (ctrl_dead & 1) || (state_dead & 1);
    if (!dead) {
        if (w.saw_swap) {
            log::info("dead-flag repair: partner copy {:X} came back from the swap with the dead bits already clear "
                      "(after {} dead samples); nothing to do", chr, w.dead_samples);
            g_partner_respawn_ms.store(GetTickCount64(), std::memory_order_relaxed);
        }
        w.saw_swap = false;
        w.dead_samples = 0;
        return;
    }
    ++w.dead_samples;

    if (!(w.saw_swap && w.dead_samples >= 2) && w.dead_samples < kDeadFallbackSamples) return;
    const bool via_swap = w.saw_swap;
    w.saw_swap = false;
    w.dead_samples = 0;
    if (ctrl_dead & 1) _InterlockedAnd(reinterpret_cast<volatile long*>(ctrl + kCtrlDeadFlags), ~1L);
    if (state && (state_dead & 1)) _InterlockedAnd(reinterpret_cast<volatile long*>(state + kCtrlStateDeadWord), ~1L);
    log::warn("dead-flag repair: partner copy {:X} {} but still dead (ctrl+0xF0={:X}, state+0x24={:X}); cleared "
              "bit0 of both",
              chr, via_swap ? "restored after the respawn swap" : "dead for 30 s without a swap", ctrl_dead, state_dead);
    g_partner_respawn_ms.store(GetTickCount64(), std::memory_order_relaxed);
}

bool g_away_hide = false;
std::map<uintptr_t, int> g_away_samples;

void hide_away_copy(uintptr_t chr) {
    uint32_t load = 0, fade = 0;
    float alpha = 1.0f;
    if (!read(chr + kChrLoadState, load) || !read(chr + kChrActivationState, fade) || !read(chr + kChrFadeAlpha, alpha)) return;
    const bool away = fade == 0 && (load & kLoadStateRemoteBit) != 0 && (load & kLoadStateAwayClearedBit) == 0;
    if (!away) {
        g_away_samples.erase(chr);
        return;
    }
    if (alpha == 0.0f) return;
    if (++g_away_samples[chr] < 2) return;
    *reinterpret_cast<volatile float*>(chr + kChrFadeAlpha) = 0.0f;
    *reinterpret_cast<volatile float*>(chr + kChrFadeDelta) = 0.0f;
    log::warn("away hide: partner copy {:X} is faded out (+0x1C0=0, load 0x{:X}) but drawn at alpha {:.2f}; set alpha 0",
              chr, load, alpha);
}

DWORD WINAPI repair_thread(LPVOID) {
    for (;;) {
        Sleep(250);
        uintptr_t wcm = deref(g_game_base + kWorldChrManPtr);
        if (!wcm) continue;
        uintptr_t main_player = deref(wcm + kWcmMainPlayer);
        for (auto& p : chr_set(wcm, kWcmPlayerSetCapacity, kWcmPlayerSetEntries)) {
            if (p.chr == main_player) continue;
            if (g_dead_repair) repair_dead_flags(p.chr);
            if (g_away_hide) hide_away_copy(p.chr);
        }
    }
}

}

void start(const Context& context, bool repair_stuck_state, bool update_pin, bool dead_flag_repair, bool away_hide) {
    detail::g_game_base = reinterpret_cast<uintptr_t>(context.game);
    g_dead_repair = dead_flag_repair;
    g_away_hide = away_hide;
    if (dead_flag_repair || away_hide) {
        if (HANDLE t = CreateThread(nullptr, 0, repair_thread, nullptr, 0, nullptr)) CloseHandle(t);
    }
    log::info("ride probe: dead-flag repair {}; away hide {} (250 ms loop)",
              dead_flag_repair ? "on" : "off", away_hide ? "on" : "off");
}

int64_t ms_since_partner_respawn() {
    const uint64_t at = g_partner_respawn_ms.load(std::memory_order_relaxed);
    if (!at) return -1;
    return static_cast<int64_t>(GetTickCount64() - at);
}

}
