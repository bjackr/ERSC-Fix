#include "hooks/mount_probe.h"

#include "game/mem.h"
#include "game/offsets.h"
#include "hooks/mount_probe_internal.h"
#include "hooks/mount_replay.h"
#include "log.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <format>
#include <map>
#include <mutex>
#include <string>

namespace ersc_fix::hooks::mount_probe {

namespace detail {

using namespace ersc_fix::game;
using namespace ersc_fix::game::mem;

uintptr_t g_game_base = 0;
std::atomic<int> g_dropped{0}, g_passed{0};

namespace {
std::atomic<bool> g_req_offset_reported{false};
}

bool Rider::riding() const { return node_state == kRideNodeMounting || node_state == kRideNodeRiding || req_state == kRideReqMount; }
bool Rider::riding_by_node() const { return node_state == kRideNodeMounting || node_state == kRideNodeRiding; }

std::string describe(const Rider& r) {
    return std::format("node {} req[state={} horse={:016X}] load {:X} entry {} pos ({:.1f} {:.1f} {:.1f})", r.node_state, r.req_state, r.req_handle,
                       r.load, r.entry, r.pos[0], r.pos[1], r.pos[2]);
}

std::string describe(const Horse& h) {
    return std::format("load {:X} blk {:08X} model {} node {} entry {} act {} alpha {:.2f} pos ({:.1f} {:.1f} {:.1f})", h.load, h.block,
                       h.model ? "yes" : "NULL", h.node_state, h.entry, h.activation, h.alpha, h.pos[0], h.pos[1], h.pos[2]);
}

void sample_position(uintptr_t chr, float* pos) {
    if (uintptr_t physics = deref(deref(chr + kChrModules) + kModulesPhysics)) {
        read(physics + kPhysicsPosition, pos[0]);
        read(physics + kPhysicsPosition + 4, pos[1]);
        read(physics + kPhysicsPosition + 8, pos[2]);
    }
}

bool sample_rider(uintptr_t chr, Rider& r) {
    read(chr + kChrHandle, r.h0);
    read(chr + kChrHandle + 4, r.h1);
    if (uintptr_t req = deref(chr + kChrRideRequestObj)) {
        read(req + kRideReqState, r.req_state);
        read(req + kRideReqHandle, r.req_handle);
        if (r.req_state > 8 && !g_req_offset_reported.exchange(true)) {
            log::error("mount probe: kRideReqState (ChrIns+0x{:X} -> +0x{:X}) reads 0x{:X}, not a ride request state: the offset has moved; the ride "
                       "repair and the horse-type drop act on garbage until offsets.h is updated",
                       kChrRideRequestObj, kRideReqState, r.req_state);
        }
    }
    read(chr + kChrLoadState, r.load);
    if (uintptr_t entry = deref(chr + kChrSetEntryPtr)) read(entry + kEntryBlockState, r.entry);
    sample_position(chr, r.pos);
    uintptr_t ride = deref(deref(chr + kChrModules) + kModulesRide);
    if (!ride) return false;
    if (uintptr_t node = deref(ride + kRideNode)) read(node + kRideNodeState, r.node_state);
    return true;
}

bool sample_horse(uintptr_t chr, Horse& h) {
    int32_t npc = -1;
    if (!read(chr + kChrNpcId, npc) || npc != kTorrentNpcId) return false;
    read(chr + kChrHandle, h.h0);
    read(chr + kChrHandle + 4, h.h1);
    read(chr + kChrLoadState, h.load);
    read(chr + kChrBlockId, h.block);
    read(chr + kChrActivationState, h.activation);
    read(chr + kChrFadeAlpha, h.alpha);
    if (uintptr_t entry = deref(chr + kChrSetEntryPtr)) read(entry + kEntryBlockState, h.entry);
    sample_position(chr, h.pos);
    h.model = deref(chr + kChrModelIns) != 0;
    if (uintptr_t ride = deref(deref(chr + kChrModules) + kModulesRide)) {
        if (uintptr_t node = deref(ride + kRideNode)) read(node + kRideNodeState, h.node_state);
    }
    return true;
}

void collect(uintptr_t wcm, uintptr_t me, std::map<uintptr_t, Rider>& riders, std::map<uintptr_t, Horse>* horses) {
    uint32_t cap = 0;
    uintptr_t entries = 0;
    if (read(wcm + kWcmPlayerSetCapacity, cap) && read(wcm + kWcmPlayerSetEntries, entries) && cap <= 64) {
        for (uint32_t k = 0; k < cap; ++k) {
            uintptr_t chr = deref(entries + 0x10ull * k);
            Rider r;
            if (chr && chr != me && sample_rider(chr, r)) riders[chr] = r;
        }
    }
    if (!horses) return;
    if (uintptr_t buddy_man = deref(wcm + kWcmSummonBuddyManager)) {
        uintptr_t set = deref(buddy_man + kBuddyManChrSet);
        if (set && read(set + kChrSetCapacity, cap) && read(set + kChrSetEntries, entries) && cap <= 4096 && entries) {
            for (uint32_t i = 0; i < cap; ++i) {
                uintptr_t chr = deref(entries + 0x10ull * i);
                Horse h;
                if (chr && sample_horse(chr, h)) (*horses)[chr] = h;
            }
        }
    }
}

}

namespace {

using namespace ersc_fix::game;
using namespace ersc_fix::game::mem;
using detail::describe;
using detail::Horse;
using detail::Rider;

std::mutex g_type_mutex;
std::map<uint64_t, uint8_t> g_last_type;

}

bool should_drop_horse_reset(uint64_t peer, uint8_t horse_type, std::string& why) {
    uint8_t last = 0;
    bool seen = false;
    {
        std::lock_guard lock(g_type_mutex);
        auto it = g_last_type.find(peer);
        if (it != g_last_type.end()) {
            seen = true;
            last = it->second;
        }
        g_last_type[peer] = horse_type;
    }
    if (seen && last != horse_type) {
        why = std::format("horse type changed {:02X} -> {:02X}", last, horse_type);
        ++detail::g_passed;
        return false;
    }

    uintptr_t wcm = deref(detail::g_game_base + kWorldChrManPtr);
    uintptr_t me = wcm ? deref(wcm + kWcmMainPlayer) : 0;
    std::map<uintptr_t, Rider> riders;
    std::map<uintptr_t, Horse> horses;
    if (me) detail::collect(wcm, me, riders, &horses);

    const int record = mount_replay::owner_record(peer);
    const char* record_text = record == 1 ? "mounted" : record == 0 ? "dismounted" : "none";
    for (auto& [chr, r] : riders) {
        if (!r.riding()) continue;
        std::string horse = "no horse object";
        for (auto& [hchr, h] : horses) {
            if (h.handle() == r.req_handle || horse == "no horse object") horse = std::format("horse {:X}:{:08X} {}", h.h0, h.h1, describe(h));
            if (h.handle() == r.req_handle) break;
        }
        if (!r.riding_by_node() && record == 0) {

            why = std::format("copy {:X}:{:08X} has only a stale mount request ({}) and the owner's ride records say dismounted; {}", r.h0, r.h1,
                              describe(r), horse);
            ++detail::g_passed;
            return false;
        }
        why = std::format("type {:02X} unchanged and copy {:X}:{:08X} rides ({}; owner's records: {}); {}", horse_type, r.h0, r.h1, describe(r),
                          record_text, horse);
        ++detail::g_dropped;
        return true;
    }
    why = std::format("{}; owner's records: {}", riders.empty() ? "no partner copy" : "no partner copy rides", record_text);
    ++detail::g_passed;
    return false;
}

void start(const Context& context) {
    detail::g_game_base = reinterpret_cast<uintptr_t>(context.game);
    log::info("mount probe: redundant horse-type messages dropped while a partner rides");
}

}
