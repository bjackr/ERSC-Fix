#include "hooks/flag_trace.h"

#include "game/mem.h"
#include "game/offsets.h"
#include "game/flag_storage.h"
#include "hooks/flag_trace_internal.h"
#include "log.h"

#include <windows.h>

#include <safetyhook.hpp>

#include <cstring>
#include <format>
#include <vector>

namespace ersc_fix::hooks::flag_trace {

namespace detail {

using namespace ersc_fix::game;
using namespace ersc_fix::game::mem;

uintptr_t g_game_base = 0;
const std::atomic<uint32_t>* g_state = nullptr;
bool g_pack_fix = true;

int read_buffer(const uint8_t* p, size_t n, uint8_t* out) {
    __try {
        std::memcpy(out, p, n);
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

int write_buffer(uint8_t* p, const uint8_t* src, size_t n) {
    __try {
        std::memcpy(p, src, n);
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

Infe parse(uint8_t* p, size_t size) {
    Infe r;
    uint8_t h[kInfeHeader];
    if (!p || !read_buffer(p, kInfeHeader, h)) { r.problem = "unreadable"; return r; }
    uint32_t magic;
    std::memcpy(&magic, h, 4);
    std::memcpy(&r.version, h + 4, 2);
    std::memcpy(&r.flags, h + 6, 2);
    std::memcpy(&r.count, h + 8, 4);
    std::memcpy(&r.bpb, h + 12, 4);
    if (magic != kInfeMagic) { r.problem = std::format("magic {:08X}", magic); return r; }
    if (!r.bpb || r.bpb > 4096 || r.count > 0x394C) { r.problem = std::format("count {} bpb {}", r.count, r.bpb); return r; }
    const size_t need = kInfeHeader + static_cast<size_t>(r.count) * (4 + r.bpb);
    if (size && size < need) { r.problem = std::format("size {} < {}", size, need); return r; }
    for (uint32_t i = 0; i < r.count; ++i) {
        uint8_t* e = p + kInfeHeader + static_cast<size_t>(i) * (4 + r.bpb);
        Entry en;
        if (!read_buffer(e, 4, reinterpret_cast<uint8_t*>(&en.slot))) { r.problem = "entry unreadable"; break; }
        en.bytes = e + 4;
        r.entries.push_back(en);
    }
    return r;
}

using SlotLegacyFn = int(__fastcall*)(void* mgr, int area, int block, int digit);
static int call_slot_legacy(SlotLegacyFn fn, void* mgr, int area, int block, int digit, int* out) {
    __try {
        *out = fn(mgr, area, block, digit);
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

int legacy_slot(uintptr_t mgr, uint32_t key) {
    if (!kFlagSlotLegacy || !mgr || key >= 1000000) return -1;
    int slot = -1;
    if (!call_slot_legacy(reinterpret_cast<SlotLegacyFn>(g_game_base + kFlagSlotLegacy), reinterpret_cast<void*>(mgr), static_cast<int>((key / 1000) % 100),
                          static_cast<int>((key / 10) % 100), static_cast<int>(key % 10), &slot))
        return -1;
    return slot;
}

std::map<int, uint32_t> slot_map(uintptr_t mgr, const std::vector<uint32_t>& keys) {
    auto m = flag_storage::slot_keys();
    for (uint32_t k : keys) {
        const int s = legacy_slot(mgr, k);
        if (s >= 0) m[s] = k;
    }
    return m;
}

}

namespace {

using namespace ersc_fix::game;

SafetyHookInline g_pack_hook, g_pack_volatile_hook;

void apply_pack_fix(void* mgr, const uint32_t* block, uint8_t* out, uint32_t written) {
    if (!detail::g_pack_fix) return;
    flag_storage::Info si;
    if (!flag_storage::info(si)) return;
    uint32_t blk = 0;
    if (block) mem::read(reinterpret_cast<uintptr_t>(block), blk);
    detail::Infe in = detail::parse(out, written);
    if (!in.problem.empty()) return;
    const uint32_t area = blk >> 24, num = (blk >> 16) & 0xFF;
    std::vector<uint32_t> keys;
    if (area != 0x3C) for (uint32_t d = 0; d < 10; ++d) keys.push_back(area * 1000 + num * 10 + d);
    const auto slots = detail::slot_map(reinterpret_cast<uintptr_t>(mgr), keys);
    for (auto& e : in.entries) {
        auto it = slots.find(static_cast<int>(e.slot));
        if (it == slots.end()) continue;
        std::vector<uint8_t> packed(in.bpb), live;
        detail::read_buffer(e.bytes, in.bpb, packed.data());
        if (!flag_storage::read_block(it->second, live) || live.size() != in.bpb) continue;

        bool differs = false;
        for (uint32_t i = 0; i < si.fpb && (i >> 3) < in.bpb && !differs; ++i) differs = detail::get_bit(packed.data(), i) != detail::get_bit(live.data(), i);
        if (differs) detail::write_buffer(e.bytes, live.data(), in.bpb);
    }
}

uint32_t __fastcall pack_detour(void* mgr, uint32_t* block, uint8_t* out) {
    const uint32_t r = g_pack_hook.call<uint32_t>(mgr, block, out);
    if (out && r >= detail::kInfeHeader) {
        apply_pack_fix(mgr, block, out, r);
    }
    return r;
}

uint32_t __fastcall pack_volatile_detour(void* mgr, uint32_t* block, uint8_t* out) {
    const uint32_t r = g_pack_volatile_hook.call<uint32_t>(mgr, block, out);
    if (out && r >= detail::kInfeHeader) {
        apply_pack_fix(mgr, block, out, r);
    }
    return r;
}

}

void start(uintptr_t game_base, const std::atomic<uint32_t>* session_state, bool pack_fix) {
    detail::g_game_base = game_base;
    detail::g_state = session_state;
    detail::g_pack_fix = pack_fix;
    struct Target {
        uint32_t rva;
        void* detour;
        SafetyHookInline* hook;
        const char* name;
    };
    const Target targets[] = {{kPackBlockFlags, reinterpret_cast<void*>(&pack_detour), &g_pack_hook, "the 0x43 packer of non-volatile flag blocks (FUN_1405cf580)"},
                              {kPackVolatileFlags, reinterpret_cast<void*>(&pack_volatile_detour), &g_pack_volatile_hook,
                               "the 0x43 packer of volatile flag blocks (FUN_1405cfad0)"}};
    for (const auto& t : targets) {
        if (!t.rva) {
            log::error("flag trace: {} not found by signature; what this game sends in 0x43 is not corrected", t.name);
            continue;
        }
        auto h = safetyhook::InlineHook::create(reinterpret_cast<char*>(game_base) + t.rva, t.detour);
        if (!h) {
            log::error("flag trace: failed to hook {} at eldenring.exe+0x{:X}", t.name, t.rva);
            continue;
        }
        *t.hook = std::move(*h);
        log::info("flag trace: hooked {} at eldenring.exe+0x{:X}", t.name, t.rva);
    }
    log::info("flag trace: slot mapper {}; outgoing 0x43 flag blocks {}", kFlagSlotLegacy ? std::format("at +0x{:X}", kFlagSlotLegacy) : "NOT found",
              pack_fix ? "corrected to what this game reads when they differ (pack_fix = 1)" : "logged only (pack_fix = 0)");
}

}
