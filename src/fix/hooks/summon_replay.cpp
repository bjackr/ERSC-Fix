#include "hooks/summon_replay.h"

#include "game/mem.h"
#include "game/offsets.h"
#include "hooks/inject.h"
#include "log.h"

#include <windows.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <mutex>
#include <format>
#include <string>
#include <vector>

namespace ersc_fix::hooks::summon_replay {

namespace {

using namespace ersc_fix::game;
using namespace ersc_fix::game::mem;
using clock = std::chrono::steady_clock;

constexpr uint32_t kTypeSpawn = 0x15;
constexpr uint8_t kHeaderChannelMod = 1;
constexpr size_t kBlockOffset = 2;
constexpr size_t kHandleOffset = 6;
constexpr size_t kStreamPosOffset = 7;
constexpr auto kPositionFresh = std::chrono::seconds(5);

constexpr auto kMinAge = std::chrono::seconds(5);
constexpr auto kSettled = std::chrono::seconds(3);
constexpr auto kRetryInterval = std::chrono::seconds(10);
constexpr auto kForget = std::chrono::minutes(10);
constexpr int kMaxReplays = 30;
constexpr uint32_t kSessInSession = 6;

constexpr auto kBurstWindow = std::chrono::seconds(2);
constexpr size_t kMaxBurst = 24;
constexpr uint8_t kHeaderChannelGame = 3;
constexpr uint32_t kTypeChrEvent = 0x24, kTypeSenderEvent = 0x26;
constexpr size_t kChrEventFrame = 8;

uintptr_t g_game_base = 0;
std::mutex g_mutex;

struct Packet {
    uint32_t type;
    std::vector<uint8_t> payload;
};

struct Spawn {
    uint64_t peer = 0;
    uint32_t block = 0;
    std::vector<uint8_t> payload;
    clock::time_point received;
    clock::time_point last_replay;
    int replays = 0;
    std::vector<Packet> burst;
};
std::map<uint64_t, uint32_t> g_frames;
std::map<uint32_t, Spawn> g_spawns;

struct Position {
    float x = 0, y = 0, z = 0;
    uint32_t block = 0;
    clock::time_point at;
};
std::map<uint64_t, Position> g_positions;

struct Seen {
    clock::time_point owner_state;
    uint8_t state_flag = 0;
    clock::time_point status;
};
std::map<uint64_t, std::map<uint8_t, Seen>> g_seen;

bool summon_slot(uint32_t block, uint32_t selector, uint8_t& slot) {
    if (block != 0xFFFFFFFFu || (selector >> 8) != 0x388) return false;
    slot = static_cast<uint8_t>(selector & 0xFF);
    return true;
}

std::string ago(clock::time_point t, clock::time_point now) {
    if (t == clock::time_point{}) return "never";
    return std::format("{} s ago", std::chrono::duration_cast<std::chrono::seconds>(now - t).count());
}

DWORD WINAPI thread(LPVOID) {
    clock::time_point in_session_since{};
    for (;;) {
        Sleep(1000);

        uint32_t state = 0;
        if (uintptr_t sess = deref(g_game_base + kSessionManPtr)) read(sess + kSessState, state);
        if (state != kSessInSession) in_session_since = {};
        else if (in_session_since == clock::time_point{}) in_session_since = clock::now();
        const bool settled = in_session_since != clock::time_point{} && clock::now() - in_session_since >= kSettled;
        {
            std::lock_guard lock(g_mutex);
            if (g_spawns.empty()) continue;
        }
        uintptr_t wcm = deref(g_game_base + kWorldChrManPtr);
        if (!wcm) continue;

        std::vector<uint32_t> present;
        if (uintptr_t buddy_man = deref(wcm + kWcmSummonBuddyManager)) {
            uintptr_t set = deref(buddy_man + kBuddyManChrSet);
            uint32_t cap = 0;
            uintptr_t entries = 0;
            if (set && read(set + kChrSetCapacity, cap) && read(set + kChrSetEntries, entries) && cap <= 4096 && entries) {
                for (uint32_t i = 0; i < cap; ++i) {
                    uintptr_t chr = deref(entries + 0x10ull * i);
                    uint32_t h = 0;
                    if (chr && read(chr + kChrHandle, h)) present.push_back(h);
                }
            }
        }

        const auto now = clock::now();
        std::lock_guard lock(g_mutex);
        const auto me = g_positions.find(0);
        const bool have_me = me != g_positions.end() && now - me->second.at < kPositionFresh;
        for (auto it = g_spawns.begin(); it != g_spawns.end();) {
            auto& [handle, sp] = *it;
            if (now - sp.received > kForget) {
                log::info("summon replay: forgetting announcement {:08X} (10 min old, {} replay(s))", handle, sp.replays);
                it = g_spawns.erase(it);
                continue;
            }
            bool exists = false;
            for (uint32_t h : present) if (h == handle) exists = true;
            if (exists) {
                if (sp.replays) {

                    std::string types;
                    const auto frame = g_frames.find(sp.peer);
                    for (auto& p : sp.burst) {
                        std::vector<uint8_t> payload = p.payload;
                        if (p.type == kTypeChrEvent && payload.size() >= kChrEventFrame + 4 && frame != g_frames.end()) {
                            std::memcpy(payload.data() + kChrEventFrame, &frame->second, 4);
                        }
                        inject::queue(sp.peer, kHeaderChannelGame, p.type, payload.data(), payload.size());
                        types += std::format(" 0x{:X}", p.type);
                    }
                    log::info("summon replay: summon {:08X} now exists (after {} replay(s)); re-sent the owner's {} packet(s) about it from "
                              "the summon:{}",
                              handle, sp.replays, sp.burst.size(), types.empty() ? " none cached" : types);
                }
                it = g_spawns.erase(it);
                continue;
            }
            if (!settled || now - sp.received < kMinAge || sp.replays >= kMaxReplays ||
                (sp.replays && now - sp.last_replay < kRetryInterval)) { ++it; continue; }

            std::string dist = "at an unknown distance";
            const auto peer = g_positions.find(sp.peer);
            if (have_me && peer != g_positions.end() && now - peer->second.at < kPositionFresh) {
                const float dx = peer->second.x - me->second.x, dz = peer->second.z - me->second.z;
                dist = std::format("{:.0f} m away", std::sqrt(dx * dx + dz * dz));
            }
            const Seen& seen = g_seen[sp.peer][static_cast<uint8_t>(handle & 0xFF)];
            inject::queue(sp.peer, kHeaderChannelMod, kTypeSpawn, sp.payload.data(), sp.payload.size());
            sp.replays++;
            sp.last_replay = now;
            if (sp.replays <= 3 || sp.replays % 10 == 0) {
                log::warn("summon replay: summon {:08X} (block {:08X}) announced {} s ago by {:016X} does not exist here; the summoner is {}; "
                          "replaying the announcement (#{}). Owner traffic for it: state item {} (flag {:02X}), status {}",
                          handle, sp.block, std::chrono::duration_cast<std::chrono::seconds>(now - sp.received).count(), sp.peer, dist,
                          sp.replays, ago(seen.owner_state, now), seen.state_flag, ago(seen.status, now));
            }
            ++it;
        }
    }
}

}

void start(const Context& context) {
    g_game_base = reinterpret_cast<uintptr_t>(context.game);
    if (HANDLE t = CreateThread(nullptr, 0, thread, nullptr, 0, nullptr)) CloseHandle(t);
    log::info("summon replay: started (replays a partner's summon announcement {} s after it arrived if the summon is still missing "
              "and our session is settled, every {} s until it exists; dropped on the partner's despawn message)",
              kMinAge.count(), kRetryInterval.count());
}

void note_spawn(uint64_t peer, const uint8_t* payload, size_t len) {
    if (!peer || len < 1) return;
    const size_t count = payload[0];
    if (!count || (len - 1) % count != 0) return;
    const size_t entry = (len - 1) / count;
    if (1 + entry < kHandleOffset + 4) return;
    const auto now = clock::now();
    std::lock_guard lock(g_mutex);
    for (size_t i = 0; i < count; ++i) {
        const uint8_t* e = payload + 1 + i * entry;
        uint32_t handle, block;
        std::memcpy(&handle, e + kHandleOffset - 1, 4);
        std::memcpy(&block, e + kBlockOffset - 1, 4);
        auto& sp = g_spawns[handle];
        sp.peer = peer;
        sp.block = block;
        sp.payload.assign(1, uint8_t{1});
        sp.payload.insert(sp.payload.end(), e, e + entry);
        sp.received = now;
        sp.replays = 0;
        sp.burst.clear();
        log::info("summon replay: noted spawn announcement {:08X} in block {:08X} from {:016X} (entry {}/{} of {} B)", handle, block, peer,
                  i + 1, count, len);
    }
}

void note_game_packet(uint64_t peer, uint32_t type, const uint8_t* payload, size_t len) {
    if (!peer || !len) return;
    const auto now = clock::now();
    std::lock_guard lock(g_mutex);
    if (type == kTypeChrEvent && len >= kChrEventFrame + 4) std::memcpy(&g_frames[peer], payload + kChrEventFrame, 4);
    else if (type == kTypeSenderEvent && len >= 4) std::memcpy(&g_frames[peer], payload, 4);
    for (auto& [handle, sp] : g_spawns) {
        if (sp.peer != peer || now - sp.received > kBurstWindow || sp.burst.size() >= kMaxBurst) continue;

        const uint8_t net[8] = {0xFF, 0xFF, 0xFF, 0xFF, static_cast<uint8_t>(handle & 0xFF), 0x88, 0x03, 0x00};
        if (len < sizeof(net)) continue;
        for (size_t i = 0; i + sizeof(net) <= len; ++i) {
            if (std::memcmp(payload + i, net, sizeof(net)) == 0) {
                sp.burst.push_back({type, std::vector<uint8_t>(payload, payload + len)});
                break;
            }
        }
    }
}

void note_despawn(uint64_t peer, const uint8_t* payload, size_t len) {
    if (len < 4) return;
    uint32_t handle;
    std::memcpy(&handle, payload, 4);
    std::lock_guard lock(g_mutex);
    auto it = g_spawns.find(handle);
    if (it == g_spawns.end()) return;
    log::info("summon replay: {:016X} despawned summon {:08X}; dropping its cached announcement ({} replay(s) made)", peer,
              handle, it->second.replays);
    g_spawns.erase(it);
}

void note_position(uint64_t peer, const uint8_t* payload, size_t len) {
    if (len < kStreamPosOffset + 12) return;
    Position p;
    std::memcpy(&p.x, payload + kStreamPosOffset, 4);
    std::memcpy(&p.y, payload + kStreamPosOffset + 4, 4);
    std::memcpy(&p.z, payload + kStreamPosOffset + 8, 4);
    std::memcpy(&p.block, payload + kStreamPosOffset - 4, 4);
    if (!std::isfinite(p.x) || !std::isfinite(p.z)) return;
    p.at = clock::now();
    std::lock_guard lock(g_mutex);
    g_positions[peer] = p;
}

bool own_stream_position(uint32_t& block, float& x, float& y, float& z) {
    std::lock_guard lock(g_mutex);
    const auto me = g_positions.find(0);
    if (me == g_positions.end() || clock::now() - me->second.at > std::chrono::seconds(3)) return false;
    block = me->second.block;
    x = me->second.x;
    y = me->second.y;
    z = me->second.z;
    return true;
}

void note_owner_state(uint64_t peer, const uint8_t* payload, size_t len) {
    if (!peer || len < 1) return;
    const size_t count = payload[0];
    if (len < 1 + 9 * count) return;
    const auto now = clock::now();
    std::lock_guard lock(g_mutex);
    for (size_t i = 0; i < count; ++i) {
        uint32_t block, selector;
        std::memcpy(&block, payload + 1 + 9 * i, 4);
        std::memcpy(&selector, payload + 5 + 9 * i, 4);
        uint8_t slot;
        if (!summon_slot(block, selector, slot)) continue;
        auto& s = g_seen[peer][slot];
        const uint8_t flag = payload[9 + 9 * i];
        if (s.owner_state == clock::time_point{} || flag != s.state_flag) {
            log::info("summon replay: owner {:016X} state item for slot {:02X}: flag {:02X}{}", peer, slot, flag,
                      s.owner_state == clock::time_point{} ? " (first)" : "");
        }
        s.owner_state = now;
        s.state_flag = flag;
    }
}

void note_chr_status(uint64_t peer, const uint8_t* payload, size_t len) {
    if (!peer || len < 9) return;
    uint32_t block, selector;
    std::memcpy(&block, payload + 1, 4);
    std::memcpy(&selector, payload + 5, 4);
    uint8_t slot;
    if (!summon_slot(block, selector, slot)) return;
    std::lock_guard lock(g_mutex);
    auto& s = g_seen[peer][slot];
    if (s.status == clock::time_point{}) log::info("summon replay: owner {:016X} status (0x46) for slot {:02X} seen (first)", peer, slot);
    s.status = clock::now();
}

}
