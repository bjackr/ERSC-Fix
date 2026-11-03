#include "hooks/flag_probe.h"

#include "config.h"
#include "game/flag_storage.h"
#include "hooks/flag_trace.h"
#include "hooks/flag_probe_internal.h"
#include "game/event_flags.h"
#include "game/mem.h"
#include "game/offsets.h"
#include "hooks/inject.h"
#include "hooks/main_thread.h"
#include "hooks/steam_log.h"
#include "log.h"

#include <windows.h>

#include <safetyhook.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <format>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace ersc_fix::hooks::flag_probe {

using detail::bit;
using detail::g_generation;
using detail::kFlagsPerBlock;
using detail::Snapshot;

namespace detail {
std::atomic<uint32_t> g_generation{0};
}

namespace {

using namespace ersc_fix::game;
using namespace ersc_fix::game::mem;
using clock = std::chrono::steady_clock;

constexpr uint32_t kBitmapMagic = 0x33474C46;
constexpr size_t kHeader = 24;

uintptr_t g_game_base = 0;
std::vector<uint32_t> g_extra;
bool g_reconcile = true;
std::set<int> g_classes{2, 3, 5};
std::set<uint32_t> g_deny;

std::mutex g_peer_mutex;
std::map<uint64_t, Snapshot> g_peers;
Snapshot g_last_own;
uint32_t g_last_own_block = 0;
std::mutex g_last_own_mutex;

std::mutex g_state_mutex;
std::atomic<bool> g_reloading{false};
std::string g_reloading_how;
struct PeerState {
    bool reloading = false;
    uint32_t last_generation = 0;
};
std::map<uint64_t, PeerState> g_peer_state;
std::set<uint32_t> g_awaiting;
std::atomic<int> g_verdict{static_cast<int>(Reload::none)};
std::atomic<uint64_t> g_reload_peer{0};
struct Adopted {
    std::vector<uint32_t> ids;
    uint64_t from = 0;
};
Adopted g_adopted;
bool g_rebuild_pending_verdict = false;

std::atomic<uint32_t> g_session_state{0};
bool g_guard_enabled = true;
std::mutex g_guard_mutex;
std::set<uint32_t> g_guard;
uint64_t g_guard_peer = 0;
uint32_t g_guard_base = 0;
bool g_guard_mirror_clear = true;

std::set<uint32_t> g_guard_adopted;
SafetyHookInline g_reset_hook, g_apply_hook;

SafetyHookInline g_set_hook;
std::atomic<bool> g_in_set_detour{false};
std::atomic<int> g_set_countered{0};
std::atomic<uint64_t> g_set_calls{0}, g_set_clears{0};

void __fastcall set_flag_detour(void* manager, int* flag_id, char value, void* source, char queue_relay) {
    g_set_hook.call<void>(manager, flag_id, value, source, queue_relay);
    ++g_set_calls;
    if (value != 0 || !flag_id || g_in_set_detour.load(std::memory_order_relaxed)) return;
    ++g_set_clears;
    const uint32_t id = static_cast<uint32_t>(*flag_id);
    {
        std::lock_guard lock(g_guard_mutex);
        if (!g_guard.count(id)) return;
    }
    g_in_set_detour.store(true);
    int again = static_cast<int>(id);
    g_set_hook.call<void>(manager, &again, 1, source, 0);
    g_in_set_detour.store(false);
    const int n = ++g_set_countered;
    if (n <= 5) log::warn("flag probe: guard: a setter call cleared guarded flag {} (session state {}); set again in the same call (#{})", id, g_session_state.load(), n);
}
std::atomic<int> g_restore_calls_reset{0}, g_restore_calls_apply{0};
std::atomic<int> g_restored_reset{0}, g_restored_apply{0};

std::atomic<int> g_quiet_reset{0}, g_quiet_apply{0};
std::atomic<bool> g_say_next_reset{true}, g_say_next_apply{true};

void guard_arm(const std::vector<uint32_t>& ids, uint64_t peer, uint32_t base, bool adopted, const char* why) {
    if (!g_guard_enabled || (!g_reset_hook && !g_apply_hook && !g_set_hook)) return;
    std::lock_guard lock(g_guard_mutex);
    if (g_guard_peer != peer || g_guard_base != base) {
        g_guard.clear();
        g_guard_adopted.clear();
    }
    g_guard_peer = peer;
    g_guard_base = base;
    g_say_next_reset.store(true);
    g_say_next_apply.store(true);
    const size_t before = g_guard.size();
    for (uint32_t id : ids) {
        g_guard.insert(id);
        if (adopted) g_guard_adopted.insert(id);
    }
    log::info("flag probe: guard ({}): {} flag(s) now follow peer {:016X}'s world for range {} ({} new, {} written by us): the reload's clears of them are "
              "undone while that peer holds them set", why, g_guard.size(), peer, base, g_guard.size() - before, g_guard_adopted.size());
}

void guard_restore(const char* writer, std::atomic<int>& calls, std::atomic<int>& total, std::atomic<int>& quiet, std::atomic<bool>& say_next) {
    std::vector<uint32_t> ids;
    {
        std::lock_guard lock(g_guard_mutex);
        if (g_guard.empty()) return;
        ids.assign(g_guard.begin(), g_guard.end());
    }
    ++calls;
    std::vector<uint32_t> restored;
    const int resolved = flag_storage::set_cleared(ids, &restored);
    total += static_cast<int>(restored.size());
    std::string list;
    for (size_t i = 0; i < restored.size() && i < 30; ++i) list += std::format(" {}", restored[i]);
    const int skipped = quiet.exchange(0);
    const std::string quiet_text = skipped ? std::format(" ({} earlier call(s) that cleared nothing were not logged)", skipped) : std::string();
    if (!restored.empty() || resolved < static_cast<int>(ids.size())) {
        say_next.store(true);
        log::warn("flag probe: guard: {} left {} of {} guarded flag(s) at 0 (session state {}): set again before it returned to its caller{}{}{}{}{}", writer,
                  restored.size(), ids.size(), g_session_state.load(), list.empty() ? "" : ":", list, restored.size() > 30 ? " .." : "",
                  resolved < static_cast<int>(ids.size()) ? std::format("; {} flag(s) not resolvable in the storage right now", ids.size() - resolved) : "",
                  quiet_text);
    } else if (say_next.exchange(false)) {
        log::info("flag probe: guard: {} ran with {} guarded flag(s); none were cleared (session state {}){}", writer, ids.size(), g_session_state.load(),
                  quiet_text);
    } else {
        quiet.store(skipped + 1);
    }
}

uint32_t range_base(uint32_t block);

void __fastcall volatile_reset_detour(void* manager) {
    g_reset_hook.call<void>(manager);
    guard_restore("the game's volatile reset", g_restore_calls_reset, g_restored_reset, g_quiet_reset, g_say_next_reset);
}

int __fastcall flag_block_apply_detour(void* manager, void* data, uint64_t size, uint64_t arg4) {
    const int r = g_apply_hook.call<int>(manager, data, size, arg4);
    guard_restore("the 0x43 flag-block copy", g_restore_calls_apply, g_restored_apply, g_quiet_apply, g_say_next_apply);
    return r;
}

void guard_disarm(const char* why) {
    std::lock_guard lock(g_guard_mutex);
    if (g_guard.empty()) return;
    log::info("flag probe: guard released ({}): {} flag(s) were protected; undone after the writers: volatile reset {} call(s), {} flag(s) set again; "
              "0x43 block copy {} call(s), {} flag(s) set again; setter clears countered {} (setter calls so far {}, {} clears)", why, g_guard.size(),
              g_restore_calls_reset.load(), g_restored_reset.load(), g_restore_calls_apply.load(), g_restored_apply.load(), g_set_countered.exchange(0),
              g_set_calls.load(), g_set_clears.load());
    g_restore_calls_reset.store(0);
    g_restored_reset.store(0);
    g_restore_calls_apply.store(0);
    g_restored_apply.store(0);
    g_quiet_reset.store(0);
    g_quiet_apply.store(0);
    g_guard.clear();
    g_guard_adopted.clear();
    g_guard_peer = 0;
    g_guard_base = 0;
}

void guard_tick(const std::vector<uint8_t>& own_bits, uint32_t own_base, const std::map<uint64_t, Snapshot>& peers) {
    std::vector<uint32_t> snapshot;
    uint64_t peer;
    uint32_t base;
    {
        std::lock_guard lock(g_guard_mutex);
        if (g_guard.empty()) return;
        snapshot.assign(g_guard.begin(), g_guard.end());
        peer = g_guard_peer;
        base = g_guard_base;
    }

    if (own_base && own_base != base) { guard_disarm(std::format("we left the block (range {} -> {})", base, own_base).c_str()); return; }
    {
        std::lock_guard lock(g_state_mutex);
        auto st = g_peer_state.find(peer);
        if (st != g_peer_state.end() && st->second.reloading) {
            guard_disarm("the peer announced a reload; its world is no longer the reference");
            return;
        }
    }
    auto ps = peers.find(peer);
    if (ps == peers.end() || ps->second.base != base) { guard_disarm("the peer no longer reports this block's range"); return; }
    if (clock::now() - ps->second.at > std::chrono::seconds(10)) { guard_disarm("the peer's bitmaps stopped arriving"); return; }
    std::vector<uint32_t> released, stray;
    for (uint32_t id : snapshot) {
        if (id < base || id >= base + kFlagsPerBlock) continue;
        if (!bit(ps->second.bits, id - base)) released.push_back(id);
        else if (own_base == base && !own_bits.empty() && !bit(own_bits, id - base)) stray.push_back(id);
    }

    if (!stray.empty()) {
        std::string list;
        for (size_t i = 0; i < stray.size() && i < 40; ++i) list += std::format(" {}", stray[i]);
        log::warn("flag probe: guard: {} guarded flag(s) read 0 here while peer {:016X} holds them set, cleared by a writer neither hook covers (session "
                  "state {}); setting them again on the game thread:{}{}", stray.size(), peer, g_session_state.load(), list, stray.size() > 40 ? " .." : "");
        main_thread::post([stray] {
            int ok = 0;
            for (uint32_t id : stray) ok += event_flags::set(id, true);
            log::info("flag probe: guard: {} of {} stray-cleared flag(s) set again", ok, stray.size());
        });
    }
    if (released.empty()) return;
    std::vector<uint32_t> mirror;
    {
        std::lock_guard lock(g_guard_mutex);
        for (uint32_t id : released) {
            g_guard.erase(id);
            if (g_guard_adopted.erase(id)) mirror.push_back(id);
        }
    }
    for (uint32_t id : released) {
        const bool m = g_guard_mirror_clear && std::find(mirror.begin(), mirror.end(), id) != mirror.end();
        log::info("flag probe: guard: peer {:016X} now holds flag {} at 0: released{}", peer, id, m ? ", mirroring the clear here (we had written it)" : "");
    }
    if (g_guard_mirror_clear && !mirror.empty()) {
        main_thread::post([mirror] {
            for (uint32_t id : mirror) {
                const auto v = event_flags::get(id);
                if (v && *v) log::warn("flag probe: guard: mirror clear of flag {}: {}", id, event_flags::set(id, false) ? "written" : "FAILED");
            }
        });
    }
}

bool guard_install() {
    if (!g_guard_enabled || !g_game_base) return false;

    struct Target {
        uint32_t rva;
        void* detour;
        SafetyHookInline* hook;
        const char* name;
    };
    const Target targets[] = {{kVolatileReset, reinterpret_cast<void*>(&volatile_reset_detour), &g_reset_hook, "the game's volatile reset (FUN_1405d29e0)"},
                              {kFlagBlockApply, reinterpret_cast<void*>(&flag_block_apply_detour), &g_apply_hook,
                               "the 0x43 flag-block deserializer (FUN_1405d5bf0)"},
                              {kSetEventFlag, reinterpret_cast<void*>(&set_flag_detour), &g_set_hook, "the low-level flag setter CSFD4VirtualMemoryFlag::SetFlag (FUN_1405d2f60)"}};
    for (const auto& t : targets) {
        if (!t.rva) {
            log::error("flag probe: guard: {} not found by signature; its clears of guarded flags cannot be undone", t.name);
            continue;
        }
        auto h = safetyhook::InlineHook::create(reinterpret_cast<char*>(g_game_base) + t.rva, t.detour);
        if (!h) {
            log::error("flag probe: guard: failed to hook {} at eldenring.exe+0x{:X}; its clears of guarded flags cannot be undone", t.name, t.rva);
            continue;
        }
        *t.hook = std::move(*h);
        log::info("flag probe: guard: hooked {} at eldenring.exe+0x{:X}", t.name, t.rva);
    }
    return g_reset_hook || g_apply_hook || g_set_hook;
}

uint32_t range_base(uint32_t block) {
    const uint32_t area = block >> 24, sub = (block >> 16) & 0xFF, sub2 = (block >> 8) & 0xFF;
    if (area == 0x3C) return 1000000000u + sub * 1000000u + sub2 * 10000u;
    if (area == 0 || area == 0xFF) return 0;
    return area * 1000000u + sub * 10000u;
}

int tail_class(uint32_t id) { return static_cast<int>((id / 1000) % 10); }

std::vector<uint32_t> parse_ids(const std::wstring& s, std::vector<std::pair<uint32_t, bool>>* assignments) {
    std::vector<uint32_t> ids;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && (s[i] == L',' || s[i] == L' ' || s[i] == L';')) ++i;
        size_t j = i;
        while (j < s.size() && s[j] != L',' && s[j] != L' ' && s[j] != L';') ++j;
        if (j > i) {
            const std::wstring tok = s.substr(i, j - i);
            const size_t eq = tok.find(L'=');
            const uint32_t id = static_cast<uint32_t>(wcstoul(tok.substr(0, eq).c_str(), nullptr, 10));
            if (id || tok == L"0") {
                if (assignments && eq != std::wstring::npos) assignments->push_back({id, wcstoul(tok.substr(eq + 1).c_str(), nullptr, 10) != 0});
                else ids.push_back(id);
            }
        }
        i = j;
    }
    return ids;
}

Snapshot scan_range(uint32_t base) {
    Snapshot s;
    s.base = base;
    s.generation = g_generation.load();
    s.at = clock::now();
    if (!base) return s;

    if (flag_storage::read_range(base, kFlagsPerBlock, s.bits) == 0) {
        s.bits.assign((kFlagsPerBlock + 7) / 8, 0);
        for (uint32_t i = 0; i < kFlagsPerBlock; ++i) {
            auto v = event_flags::get(base + i);
            if (v && *v) s.bits[i / 8] |= static_cast<uint8_t>(1u << (i % 8));
        }
    }
    return s;
}

std::vector<uint8_t> encode(const Snapshot& s, uint32_t handoff_block) {
    std::vector<uint8_t> msg(kHeader + s.bits.size() + s.extra.size() * 5);
    uint32_t hdr[6] = {kBitmapMagic, s.base, kFlagsPerBlock, static_cast<uint32_t>(s.extra.size()), s.generation, handoff_block};
    std::memcpy(msg.data(), hdr, kHeader);
    std::memcpy(msg.data() + kHeader, s.bits.data(), s.bits.size());
    size_t o = kHeader + s.bits.size();
    for (auto& [id, v] : s.extra) {
        std::memcpy(msg.data() + o, &id, 4);
        msg[o + 4] = v ? 1 : 0;
        o += 5;
    }
    return msg;
}

void send_periodic(const Snapshot& s) {
    const auto msg = encode(s, 0);
    uint64_t peers[16];
    const size_t n = inject::peers(peers, 16);
    for (size_t i = 0; i < n; ++i) steam_log::send_raw(peers[i], 1, kFlagBitmapType, msg.data(), msg.size());
}

void adopt(const std::vector<uint32_t>& ids, uint64_t peer, uint32_t block) {
    {
        std::lock_guard lock(g_state_mutex);
        g_adopted.ids = ids;
        g_adopted.from = peer;
    }
    guard_arm(ids, peer, range_base(block), true, "adoption");
    const bool posted = main_thread::post([ids, peer, block] {
        int ok = 0;
        for (uint32_t id : ids) {
            const auto before = event_flags::get(id);
            if (before && *before) continue;
            const bool done = event_flags::set(id, true);
            ok += done;
            log::warn("flag probe: handoff: set flag {} (was {}) from peer {:016X} for block {:08X}: {}", id, before ? (*before ? "1" : "0") : "?", peer,
                      block, done ? "written on the game thread" : "FAILED");
        }
        log::info("flag probe: handoff: {} of {} flags written", ok, ids.size());
    });
    if (!posted) log::warn("flag probe: handoff: not posted, game-thread hook not installed");
}

void poll_session_state() {
    static uint32_t prev_state = 0xFFFFFFFF;
    uintptr_t sess = deref(g_game_base + kSessionManPtr);
    if (!sess) return;
    uint32_t connected = 0, role = 0, state = 0;
    if (!read(sess + kSessMyId, connected) || !read(sess + kSessRole, role) || !read(sess + kSessState, state)) return;
    g_session_state.store(state);
    if (state == prev_state) return;
    if (state == 5) {
        std::lock_guard lock(g_state_mutex);
        g_rebuild_pending_verdict = prev_state == 6 && connected;
        if (!g_reloading.load()) {
            g_reloading.store(true);
            g_reloading_how = "session state -> 5 (no 0x19/0x0A seen first)";
            log::info("flag probe: session state {} -> 5 (connected {}, role {}) without our own 0x19/0x0A: marking RELOADING", prev_state, connected, role);
        } else {
            log::info("flag probe: session state {} -> 5 (connected {}, role {}); RELOADING since {}", prev_state, connected, role, g_reloading_how);
        }
    } else if (state == 6 && prev_state == 5) {
        std::lock_guard lock(g_state_mutex);
        g_generation.fetch_add(1);
        g_reloading.store(false);
        if (g_rebuild_pending_verdict) {
            g_rebuild_pending_verdict = false;
            if (g_verdict.load() == static_cast<int>(Reload::pending)) {
                g_verdict.store(static_cast<int>(Reload::none));
                log::info("flag probe: world rebuilt (generation {}) with no flag handoff received: nothing reconciled (joint reload, or no peer had the block loaded)",
                          g_generation.load());
            }
        }
        log::info("flag probe: session state 5 -> 6: world rebuilt (generation {}); SETTLED again{}", g_generation.load(),
                  g_awaiting.empty() ? "" : std::format("; still awaiting a handoff for {} block(s)", g_awaiting.size()));
    } else if (state == 0 && prev_state != 0xFFFFFFFF) {

        guard_disarm("the session ended; no peer world is the reference");
    }
    prev_state = state;
}

DWORD WINAPI thread(LPVOID) {
    auto last_send = clock::now();
    int sub = 0;
    for (;;) {
        Sleep(250);
        poll_session_state();
        if (++sub % 4) continue;
        if (!event_flags::available()) continue;
        uintptr_t wcm = deref(g_game_base + kWorldChrManPtr);
        uint32_t block = 0;
        if (!wcm || !read(wcm + kWcmCurrentBlock, block)) continue;

        {
            static int s_bad = 0;
            const uint32_t area = block >> 24;
            const bool plausible = block == 0 || block == 0xFFFFFFFFu || ((block & 0xFF) == 0 && area >= 0x0A && area <= 0x6F);
            if (!plausible && s_bad++ < 3) {
                log::error("flag probe: WorldChrMan+0x{:X} (kWcmCurrentBlock) reads {:08X}, not a block id: the hand offset is probably stale for this "
                           "game build; see re/game_builds.md", kWcmCurrentBlock, block);
            }
        }
        Snapshot cur = scan_range(range_base(block));
        for (uint32_t id : g_extra) {
            auto v = event_flags::get(id);
            if (v) cur.extra[id] = *v;
        }
        {
            std::lock_guard lock(g_last_own_mutex);
            g_last_own = cur;
            g_last_own_block = block;
        }
        std::map<uint64_t, Snapshot> peers;
        {
            std::lock_guard lock(g_peer_mutex);
            peers = g_peers;
        }
        guard_tick(cur.bits, cur.base, peers);
        if (cur.at - last_send >= std::chrono::seconds(2)) {
            last_send = cur.at;
            send_periodic(cur);
        }
    }
}

void apply_handoff(uint64_t peer, uint32_t block, const Snapshot& theirs) {
    Snapshot own = scan_range(theirs.base);
    std::vector<uint32_t> ids;
    int differ = 0, skipped_class = 0, skipped_deny = 0, cleared_here = 0;
    for (uint32_t i = 0; i < kFlagsPerBlock; ++i) {
        const bool ours = bit(own.bits, i), th = bit(theirs.bits, i);
        if (ours == th) continue;
        ++differ;
        if (ours && !th) { ++cleared_here; continue; }
        const uint32_t id = theirs.base + i;
        if (!g_classes.count(tail_class(id))) { ++skipped_class; continue; }
        if (g_deny.count(id)) { ++skipped_deny; continue; }
        ids.push_back(id);
    }
    const bool rebuild = g_reloading.load();
    log::warn("flag probe: handoff from peer {:016X} for block {:08X} (range {}, their generation {}) while we {}: {} flags differ, {} they have cleared "
              "left alone, {} outside classes, {} denied; {} to set{}",
              peer, block, theirs.base, theirs.generation, rebuild ? "rebuild our world" : "load the block", differ, cleared_here, skipped_class, skipped_deny,
              ids.size(), g_reconcile ? "" : " [reconcile_after_reload = 0: dry run, nothing written]");
    {
        std::lock_guard lock(g_state_mutex);
        g_verdict.store(static_cast<int>(Reload::unilateral));
        g_reload_peer.store(peer);
        g_rebuild_pending_verdict = false;
    }

    if (g_reconcile) {
        std::vector<uint32_t> held;
        for (uint32_t i = 0; i < kFlagsPerBlock; ++i) {
            if (!bit(theirs.bits, i)) continue;
            const uint32_t id = theirs.base + i;
            if (g_classes.count(tail_class(id)) && !g_deny.count(id)) held.push_back(id);
        }
        if (!held.empty()) guard_arm(held, peer, theirs.base, false, rebuild ? "handoff during our rebuild" : "handoff for a block we load");
    }
    if (g_reconcile && !ids.empty()) adopt(ids, peer, block);
}

}

void start(const Context& context) {
    g_game_base = reinterpret_cast<uintptr_t>(context.game);
    const auto ini = config::ini_path();
    wchar_t buf[2048];
    if (GetPrivateProfileStringW(L"flag_probe", L"watch_extra", L"", buf, 2048, ini.c_str()) > 0) g_extra = parse_ids(buf, nullptr);
    g_reconcile = GetPrivateProfileIntW(L"flag_probe", L"reconcile_after_reload", 1, ini.c_str()) != 0;
    if (GetPrivateProfileStringW(L"flag_probe", L"reconcile_classes", L"", buf, 2048, ini.c_str()) > 0) {
        g_classes.clear();
        for (uint32_t c : parse_ids(buf, nullptr)) if (c < 10) g_classes.insert(static_cast<int>(c));
    }
    if (GetPrivateProfileStringW(L"flag_probe", L"reconcile_deny", L"", buf, 2048, ini.c_str()) > 0) {
        for (uint32_t id : parse_ids(buf, nullptr)) g_deny.insert(id);
    }
    g_guard_enabled = GetPrivateProfileIntW(L"flag_probe", L"guard", 1, ini.c_str()) != 0;
    g_guard_mirror_clear = GetPrivateProfileIntW(L"flag_probe", L"guard_mirror_clear", 1, ini.c_str()) != 0;
    flag_storage::init(g_game_base);
    const bool guarded = guard_install();
    flag_trace::start(g_game_base, &g_session_state, GetPrivateProfileIntW(L"flag_probe", L"pack_fix", 1, ini.c_str()) != 0);
    if (HANDLE t = CreateThread(nullptr, 0, thread, nullptr, 0, nullptr)) CloseHandle(t);
    log::info("flag probe: guard {}", guarded ? "on: the reload's two flag-block writers and the flag setter are hooked; their clears of guarded flags are undone before they return"
                                              : (g_guard_enabled ? "requested but NOT installed (no writer found)" : "off"));
    std::string classes;
    for (int c : g_classes) classes += std::format("{}xxx ", c);
    log::info("flag probe: started; flag handoff on block load {} (classes {}, {} denied), {} extra flags watched, setter {}",
              g_reconcile ? "on" : "off (dry run)", classes, g_deny.size(), g_extra.size(),
              event_flags::can_set() ? std::format("at +0x{:X}", kSetEventFlag) : "not resolved");
}

void note_peer_flags(uint64_t peer, const uint8_t* payload, size_t len) {
    if (!peer || !payload || len < kHeader) return;
    uint32_t hdr[6];
    std::memcpy(hdr, payload, kHeader);
    if (hdr[0] != kBitmapMagic || hdr[2] != kFlagsPerBlock) return;
    const size_t nbits = (kFlagsPerBlock + 7) / 8;
    if (len < kHeader + nbits + 5ull * hdr[3] || hdr[3] > 4096) return;
    Snapshot s;
    s.base = hdr[1];
    s.generation = hdr[4];
    s.at = clock::now();
    s.bits.assign(payload + kHeader, payload + kHeader + nbits);
    size_t o = kHeader + nbits;
    for (uint32_t i = 0; i < hdr[3]; ++i, o += 5) {
        uint32_t id;
        std::memcpy(&id, payload + o, 4);
        s.extra[id] = payload[o + 4] != 0;
    }
    const uint32_t handoff_block = hdr[5];
    bool peer_reloading;
    {
        std::lock_guard lock(g_state_mutex);
        auto& st = g_peer_state[peer];
        if (st.reloading && s.generation != st.last_generation) {
            st.reloading = false;
            log::info("flag probe: peer {:016X} finished its reload (generation {} -> {})", peer, st.last_generation, s.generation);
        }
        st.last_generation = s.generation;
        peer_reloading = st.reloading;
    }
    {
        std::lock_guard lock(g_peer_mutex);
        auto& cur = g_peers[peer];
        if (cur.base != s.base) log::info("flag probe: peer {:016X} now reports flag range {} (generation {})", peer, s.base, s.generation);
        cur = s;
    }
    if (!handoff_block) return;

    bool wanted;
    {
        std::lock_guard lock(g_state_mutex);
        wanted = g_awaiting.erase(handoff_block) > 0;
    }
    if (!wanted) {
        log::info("flag probe: handoff for block {:08X} from peer {:016X} ignored: we did not request that block", handoff_block, peer);
        return;
    }
    if (peer_reloading) {
        log::info("flag probe: handoff for block {:08X} from peer {:016X} ignored: that peer is reloading too (joint reload)", handoff_block, peer);
        return;
    }
    if (s.base != range_base(handoff_block)) {
        log::info("flag probe: handoff for block {:08X} from peer {:016X} ignored: range {} does not match the block", handoff_block, peer, s.base);
        return;
    }
    apply_handoff(peer, handoff_block, s);
}

void note_peer_reload(uint64_t peer, const char* how) {
    if (!peer) return;
    bool first;
    {
        std::lock_guard lock(g_state_mutex);
        auto& st = g_peer_state[peer];
        first = !st.reloading;
        st.reloading = true;
    }
    if (!first) return;
    log::info("flag probe: peer {:016X} announced a world reload ({})", peer, how);

    const bool joint = g_reloading.load();
    std::vector<uint32_t> written;
    bool armed = false;
    {
        std::lock_guard lock(g_guard_mutex);
        armed = !g_guard.empty() && g_guard_peer == peer;
        if (armed && joint) {
            if (g_restored_reset.load() || g_restored_apply.load()) written.assign(g_guard.begin(), g_guard.end());
            else written.assign(g_guard_adopted.begin(), g_guard_adopted.end());
        }
    }
    if (armed) {
        guard_disarm(joint ? "the peer announced a reload while ours is in progress: joint reload, both worlds go to vanilla"
                           : "the peer announced a reload; its world is no longer the reference");
    }
    if (!written.empty()) {
        log::warn("flag probe: guard: joint reload: {} flag(s) this guard had set again are cleared on the game thread (the vanilla reset would have left them 0)",
                  written.size());
        main_thread::post([written] {
            int ok = 0;
            for (uint32_t id : written) {
                const auto v = event_flags::get(id);
                if (v && *v) ok += event_flags::set(id, false);
            }
            log::info("flag probe: guard: joint reload: {} of {} flag(s) cleared", ok, written.size());
        });
    }

    uint32_t block;
    {
        std::lock_guard lock(g_last_own_mutex);
        block = g_last_own_block;
    }
    if (block) note_peer_block_request(peer, block, std::format("second chance on their {} reload notice", how).c_str());
}

void note_peer_block_request(uint64_t peer, uint32_t block, const char* how) {
    if (!peer || !block) return;
    const uint32_t base = range_base(block);
    if (!base) return;
    const uint32_t state = g_session_state.load();
    const bool reloading = g_reloading.load();
    uint32_t own_block = 0;
    {
        std::lock_guard lock(g_last_own_mutex);
        own_block = g_last_own.base;
    }
    const bool have_block = own_block == base;
    if (state != 6 || reloading || !have_block) {
        log::info("flag probe: peer {:016X} is building block {:08X} ({}); not answering: {}{}{}", peer, block, how,
                  state != 6 ? std::format("our session state is {} ", state) : "", reloading ? "we are RELOADING ourselves " : "",
                  have_block ? "" : std::format("we are not in that block (ours {})", own_block));
        return;
    }
    Snapshot s = scan_range(base);
    int set = 0;
    for (uint32_t i = 0; i < kFlagsPerBlock; ++i) set += bit(s.bits, i);
    const auto msg = encode(s, block);
    const bool ok = steam_log::send_raw(peer, 1, kFlagBitmapType, msg.data(), msg.size());
    log::info("flag probe: peer {:016X} is building block {:08X} ({}); we are SETTLED there: handed off our {} set flags of range {} (generation {}): {}",
              peer, block, how, set, base, s.generation, ok ? "sent" : "SEND FAILED");
}

void note_own_reload(const char* how) {
    std::lock_guard lock(g_state_mutex);
    if (!g_reloading.load()) {
        g_reloading.store(true);
        g_reloading_how = how;
        g_verdict.store(static_cast<int>(Reload::pending));
        g_reload_peer.store(0);
        log::info("flag probe: our game announced a world reload ({}): RELOADING; we will not answer block requests until the world is rebuilt", how);
    }
}

void note_own_block_request(uint32_t block, const char* how) {
    if (!block) return;
    {
        std::lock_guard lock(g_state_mutex);
        g_awaiting.insert(block);
    }
    log::info("flag probe: our game is building block {:08X} ({}){}: a flag handoff for it will be accepted", block, how,
              g_reloading.load() ? " during our world rebuild" : "");

    const uint32_t base = range_base(block);
    if (!base) return;
    std::map<uint64_t, Snapshot> peers;
    {
        std::lock_guard lock(g_peer_mutex);
        peers = g_peers;
    }
    const auto now = clock::now();
    for (auto& [peer, ps] : peers) {
        if (ps.base != base || now - ps.at > std::chrono::seconds(4)) continue;
        bool reloading;
        {
            std::lock_guard lock(g_state_mutex);
            auto it = g_peer_state.find(peer);
            reloading = it != g_peer_state.end() && it->second.reloading;
        }
        if (reloading) {
            log::info("flag probe: peer {:016X} has a bitmap for block {:08X} but is reloading itself: not pre-positioning from it", peer, block);
            continue;
        }
        log::info("flag probe: pre-positioning block {:08X} from peer {:016X}'s periodic bitmap ({:.1f} s old) before the game builds it", block, peer,
                  std::chrono::duration<double>(now - ps.at).count());
        apply_handoff(peer, block, ps);
        return;
    }
}

Reload reload_verdict() { return static_cast<Reload>(g_verdict.load()); }
uint64_t reload_peer() { return g_reload_peer.load(); }

}
