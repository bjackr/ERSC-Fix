#include "hooks/bullet_guard.h"

#include "config.h"
#include "game/mem.h"
#include "game/offsets.h"
#include "hooks/enemy_probe.h"
#include "hooks/flag_probe.h"
#include "log.h"

#include <windows.h>

#include <intrin.h>
#include <safetyhook.hpp>

#include <atomic>
#include <chrono>
#include <cstring>
#include <format>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace ersc_fix::hooks::bullet_guard {

namespace {

using namespace ersc_fix::game;
using namespace ersc_fix::game::mem;
using clock = std::chrono::steady_clock;

constexpr int kDetailedSpawns = 200;
constexpr auto kSummaryEvery = std::chrono::seconds(10);

struct SpawnInfo {
    uint64_t owner;
    int32_t bullet;
    int32_t dummy_a, dummy_b;
    int32_t field14;
    int32_t dummy_c;
    uint8_t pad[0x44 - 0x1C];
    uint32_t flags;
    uint8_t pad2[0x70 - 0x48];
    float pos[4];
};
static_assert(offsetof(SpawnInfo, bullet) == 0x08);
static_assert(offsetof(SpawnInfo, flags) == 0x44);
static_assert(offsetof(SpawnInfo, pos) == 0x70);

uintptr_t g_game_base = 0, g_game_end = 0, g_ersc_base = 0, g_ersc_end = 0, g_self_base = 0, g_self_end = 0;
bool g_drop = true;
bool g_log = true;
std::set<int32_t> g_dummy_models{120};
std::set<int32_t> g_anchor_models{4510};
std::set<uint32_t> g_blocks{0x0D000000};
SafetyHookInline g_hook;

std::atomic<int> g_detailed{0};
std::atomic<uint64_t> g_spawns{0}, g_dropped{0};
std::mutex g_sum_mutex;
struct Sum {
    int spawns = 0, dropped = 0;
    std::set<int32_t> bullets;
};
std::map<std::string, Sum> g_summary;
clock::time_point g_last_summary{};
std::string g_drop_reason;
bool g_drop_active = false;

using ResolveFn = void* (__fastcall*)(uint64_t handle);

void* call_resolve(ResolveFn fn, uint64_t handle) {
    __try {
        return fn(handle);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

int read_info(const SpawnInfo* src, SpawnInfo* dst) {
    __try {
        std::memcpy(dst, src, sizeof(SpawnInfo));
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

std::string where(uintptr_t a) {
    if (a >= g_game_base && a < g_game_end) return std::format("eldenring.exe+0x{:X}", a - g_game_base);
    if (a >= g_ersc_base && a < g_ersc_end) return std::format("ersc.dll+0x{:X}", a - g_ersc_base);
    if (a >= g_self_base && a < g_self_end) return std::format("ersc_fix.dll+0x{:X}", a - g_self_base);
    return std::format("0x{:X}", a);
}

bool known_code(uintptr_t a) {
    return (a >= g_game_base && a < g_game_end) || (a >= g_ersc_base && a < g_ersc_end) || (a >= g_self_base && a < g_self_end);
}

std::string stack_from(uintptr_t from, int max) {
    std::string s;
    const uintptr_t base = reinterpret_cast<uintptr_t>(reinterpret_cast<NT_TIB*>(NtCurrentTeb())->StackBase);
    const uintptr_t end = std::min(base, from + 200 * sizeof(uintptr_t));
    int n = 0;
    for (uintptr_t p = from & ~static_cast<uintptr_t>(7); p + 8 <= end && n < max; p += 8) {
        uintptr_t v = 0;
        if (!read(p, v)) break;
        if (known_code(v)) {
            s += " " + where(v);
            ++n;
        }
    }
    return s;
}

std::string drop_reason(const enemy_probe::ChrInfo& owner) {
    if (!g_drop) return "";
    if (!g_blocks.empty() && !g_blocks.count(owner.block)) return "";
    if (!g_dummy_models.count(owner.npc)) return "";
    if (flag_probe::reload_verdict() != flag_probe::Reload::unilateral) return "";
    const uint64_t cont = flag_probe::reload_peer();
    for (int32_t anchor : g_anchor_models) {
        uint64_t peer = 0;
        uint32_t sel = 0;
        if (!enemy_probe::peer_holds_disabled_model(anchor, owner.block, &peer, &sel)) continue;
        if (cont && peer != cont) continue;
        return std::format("peer {:016X} (the continuous world since our reload) holds the c{} dragon {:08X}:{:08X} of block {:08X} disabled: its hazard loop has ended",
                           peer, anchor, owner.block, sel, owner.block);
    }
    return "";
}

int* __fastcall spawn_detour(void* manager, int* out_id, SpawnInfo* info, uint64_t arg4) {
    const uintptr_t ret = reinterpret_cast<uintptr_t>(_ReturnAddress());
    const uint64_t n = ++g_spawns;
    SpawnInfo copy{};
    const bool have = info && read_info(info, &copy);
    enemy_probe::ChrInfo owner;
    bool known = false;
    void* inst = nullptr;
    const unsigned type = have ? static_cast<unsigned>((copy.owner >> 28) & 0xF) : 0xFF;

    if (have) known = enemy_probe::describe_by_handle(copy.owner, owner);
    if (have && !known && type == 0 && kFieldInsResolve) {
        inst = call_resolve(reinterpret_cast<ResolveFn>(g_game_base + kFieldInsResolve), copy.owner);
        if (inst) known = enemy_probe::describe(reinterpret_cast<uintptr_t>(inst), owner);
    }
    std::string reason = known ? drop_reason(owner) : "";
    const bool drop = !reason.empty();
    if (known && !drop && g_drop && g_dummy_models.count(owner.npc)) {
        static std::atomic<int> s_misses{0};
        if (s_misses++ < 40) {
            uint64_t peer = 0;
            uint32_t sel = 0;
            bool held = false;
            for (int32_t anchor : g_anchor_models) held = held || enemy_probe::peer_holds_disabled_model(anchor, owner.block, &peer, &sel);
            log::info("bullet guard: dummy c{} spawn not dropped: reload verdict {} (continuous peer {:016X}); a peer {} an anchor dragon of block {:08X} disabled{}",
                      owner.npc, static_cast<int>(flag_probe::reload_verdict()), flag_probe::reload_peer(), held ? "holds" : "does NOT hold", owner.block,
                      held ? std::format(" (peer {:016X}, {:08X})", peer, sel) : "");
        }
    }
    if (drop) ++g_dropped;

    const std::string who = known ? std::format("c{} {:08X}:{:08X} at ({:.0f}, {:.0f}, {:.0f}){}", owner.npc, owner.net_block, owner.net_sel, owner.x, owner.y,
                                                owner.z, (owner.entry_flags & 1) ? " [disabled]" : "")
                            : have ? std::format("handle {:016X} type {}{}", copy.owner, type, inst ? " (not tracked)" : " (unresolved)") : "?";
    if (g_log) {
        bool state_changed = false;
        {
            std::lock_guard lock(g_sum_mutex);
            if (drop != g_drop_active || (drop && reason != g_drop_reason)) {
                g_drop_active = drop;
                g_drop_reason = reason;
                state_changed = true;
            }
        }
        if (state_changed) {
            if (drop) log::warn("bullet guard: dropping bullets from the hazard dummies: {}", reason);
            else log::info("bullet guard: not dropping any more (rule no longer holds)");
        }
        if (g_detailed.load() < kDetailedSpawns || state_changed) {
            ++g_detailed;
            log::info("bullet guard: spawn #{} bullet {} by {}{}; pos ({:.0f}, {:.0f}, {:.0f}) flags {:08X} field14 {} dummies {}/{}/{}; caller {}; stack:{}", n,
                      have ? copy.bullet : -1, who, drop ? " -> DROPPED" : "", have ? copy.pos[0] : 0.f, have ? copy.pos[1] : 0.f, have ? copy.pos[2] : 0.f,
                      have ? copy.flags : 0u, have ? copy.field14 : 0, have ? copy.dummy_a : 0, have ? copy.dummy_b : 0, have ? copy.dummy_c : 0, where(ret),
                      stack_from(reinterpret_cast<uintptr_t>(_AddressOfReturnAddress()) + 8, 5));
        } else {
            std::lock_guard lock(g_sum_mutex);
            auto& sm = g_summary[known ? std::format("c{} {:08X}", owner.npc, owner.net_sel) : std::format("type {}", type)];
            ++sm.spawns;
            sm.dropped += drop;
            if (have) sm.bullets.insert(copy.bullet);
            const auto now = clock::now();
            if (now - g_last_summary >= kSummaryEvery) {
                g_last_summary = now;
                std::string line;
                for (auto& [k, v] : g_summary) {
                    line += std::format(" [{}: {} spawn(s){} bullets", k, v.spawns, v.dropped ? std::format(", {} dropped", v.dropped) : "");
                    int i = 0;
                    for (int32_t b : v.bullets) { if (i++ < 6) line += std::format(" {}", b); }
                    line += "]";
                }
                g_summary.clear();
                log::info("bullet guard: last 10 s:{} (total {} spawn(s), {} dropped)", line, g_spawns.load(), g_dropped.load());
            }
        }
    }
    if (drop) {
        if (out_id) *out_id = -1;
        return out_id;
    }
    return g_hook.call<int*>(manager, out_id, info, arg4);
}

uintptr_t image_end(HMODULE m) {
    if (!m) return 0;
    auto base = reinterpret_cast<uintptr_t>(m);
    auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    auto nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    return base + nt->OptionalHeader.SizeOfImage;
}

std::set<int32_t> parse_models(const wchar_t* s) {
    std::set<int32_t> out;
    const wchar_t* p = s;
    while (*p) {
        while (*p == L',' || *p == L' ' || *p == L';') ++p;
        if (!*p) break;
        wchar_t* e = nullptr;
        const long v = wcstol(p, &e, 10);
        if (e == p) break;
        out.insert(static_cast<int32_t>(v));
        p = e;
    }
    return out;
}

}

void start(const Context& context) {
    g_game_base = reinterpret_cast<uintptr_t>(context.game);
    g_game_end = image_end(context.game);
    g_ersc_base = reinterpret_cast<uintptr_t>(context.ersc);
    g_ersc_end = image_end(context.ersc);
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&start), &self);
    g_self_base = reinterpret_cast<uintptr_t>(self);
    g_self_end = image_end(self);
    const auto ini = config::ini_path();
    g_drop = GetPrivateProfileIntW(L"bullet_guard", L"drop", 1, ini.c_str()) != 0;
    g_log = GetPrivateProfileIntW(L"bullet_guard", L"log_spawns", 1, ini.c_str()) != 0;
    wchar_t buf[512];
    if (GetPrivateProfileStringW(L"bullet_guard", L"dummy_models", L"120", buf, 512, ini.c_str()) > 0) g_dummy_models = parse_models(buf);
    if (GetPrivateProfileStringW(L"bullet_guard", L"anchor_models", L"4510", buf, 512, ini.c_str()) > 0) g_anchor_models = parse_models(buf);
    if (GetPrivateProfileStringW(L"bullet_guard", L"blocks", L"0D000000", buf, 512, ini.c_str()) > 0) {
        g_blocks.clear();
        const wchar_t* q = buf;
        while (*q) {
            while (*q == L',' || *q == L' ' || *q == L';') ++q;
            if (!*q) break;
            wchar_t* e = nullptr;
            const unsigned long v = wcstoul(q, &e, 16);
            if (e == q) break;
            g_blocks.insert(static_cast<uint32_t>(v));
            q = e;
        }
    }
    if (!kBulletSpawn) {
        log::error("bullet guard: CSBulletManager::SpawnBullet not found by signature; the hazard's bullets cannot be seen or dropped");
        return;
    }
    auto h = safetyhook::InlineHook::create(reinterpret_cast<char*>(g_game_base) + kBulletSpawn, reinterpret_cast<void*>(&spawn_detour));
    if (!h) {
        log::error("bullet guard: failed to hook CSBulletManager::SpawnBullet at eldenring.exe+0x{:X}", kBulletSpawn);
        return;
    }
    g_hook = std::move(*h);
    std::string dm, am, bl;
    for (int32_t m : g_dummy_models) dm += std::format(" c{}", m);
    for (int32_t m : g_anchor_models) am += std::format(" c{}", m);
    for (uint32_t b : g_blocks) bl += std::format(" {:08X}", b);
    log::info("bullet guard: hooked CSBulletManager::SpawnBullet at eldenring.exe+0x{:X}; drop {} (dummies{}; anchors{}; blocks{}); spawn log {}", kBulletSpawn,
              g_drop ? "on" : "off (log only)", dm, am, bl.empty() ? " any" : bl, g_log ? "on" : "off");
}

}
