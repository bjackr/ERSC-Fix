#include "hooks/away_relocate.h"

#include "config.h"
#include "game/mem.h"
#include "game/offsets.h"
#include "log.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <format>
#include <map>
#include <mutex>

namespace ersc_fix::hooks::away_relocate {

namespace {

using namespace ersc_fix::game;
using namespace ersc_fix::game::mem;

constexpr uint32_t kRoundtableBlock = 0x0B0A0000;
constexpr size_t kBlockOffset = 3;
constexpr size_t kPosOffset = 7;
constexpr size_t kMinLen = kPosOffset + 12;
constexpr uint32_t kOverworldArea = 0x3C;
constexpr uint32_t kUndergroundArea = 0x0C;
constexpr DWORD kTickMs = 250;
constexpr float kFadeStep = 0.25f / 1.5f;

bool entrance_area(uint32_t area) {
    switch (area) {
        case 0x1E:
        case 0x1F:
        case 0x20:
        case 0x22:
        case 0x23:
        case 0x27:
            return true;
        default:
            return false;
    }
}
bool g_rewrite_underground = false;
bool g_rewrite_dungeons = false;

uint32_t g_rt_block = 0x3C2B2500;
float g_rt_x = -3226.0f, g_rt_y = 92.0f, g_rt_z = -1422.0f;
uintptr_t g_game_base = 0;

std::atomic<uint32_t> g_local_block{0};
std::atomic<int> g_away_peers{0};

struct PeerState {
    bool have_home = false;
    uint32_t home_block = 0;
    float home[3] = {0, 0, 0};
    bool away = false;
    uint64_t rewritten = 0;
    std::chrono::steady_clock::time_point last_seen{};
};
std::mutex g_mutex;
std::map<uint64_t, PeerState> g_peers;
constexpr auto kStreamTimeout = std::chrono::seconds(5);

void leave_away(uint64_t peer, PeerState& st, const char* why) {
    if (!st.away) return;
    log::info("away relocate: peer {:016X} {} after {} rewritten position messages", peer, why, st.rewritten);
    st.away = false;
    st.rewritten = 0;
    g_away_peers.fetch_sub(1, std::memory_order_relaxed);
}

void load_config() {
    const wchar_t* ini = config::ini_path().c_str();
    g_rewrite_underground = GetPrivateProfileIntW(L"away_relocate", L"underground", 0, ini) != 0;
    g_rewrite_dungeons = GetPrivateProfileIntW(L"away_relocate", L"dungeons", 0, ini) != 0;
    wchar_t buf[64];
    if (GetPrivateProfileStringW(L"away_relocate", L"roundtable_block", L"", buf, 64, ini) > 0) g_rt_block = wcstoul(buf, nullptr, 0);
    if (GetPrivateProfileStringW(L"away_relocate", L"roundtable_x", L"", buf, 64, ini) > 0) g_rt_x = wcstof(buf, nullptr);
    if (GetPrivateProfileStringW(L"away_relocate", L"roundtable_y", L"", buf, 64, ini) > 0) g_rt_y = wcstof(buf, nullptr);
    if (GetPrivateProfileStringW(L"away_relocate", L"roundtable_z", L"", buf, 64, ini) > 0) g_rt_z = wcstof(buf, nullptr);
}

DWORD WINAPI thread(LPVOID) {
    std::map<uintptr_t, float> alpha;
    for (;;) {
        Sleep(kTickMs);
        uintptr_t wcm = deref(g_game_base + kWorldChrManPtr);
        if (!wcm) continue;
        uintptr_t me = deref(wcm + kWcmMainPlayer);
        uint32_t block = 0;
        if (me) read(me + kChrBlockId, block);
        g_local_block.store(block, std::memory_order_relaxed);

        {
            std::lock_guard lock(g_mutex);
            const auto now = std::chrono::steady_clock::now();
            for (auto& [peer, st] : g_peers) {
                if (st.away && now - st.last_seen > kStreamTimeout) leave_away(peer, st, "sent no position for 5 s");
            }
        }

        const bool any_away = g_away_peers.load(std::memory_order_relaxed) > 0;
        uint32_t cap = 0;
        uintptr_t entries = 0;
        if (!read(wcm + kWcmPlayerSetCapacity, cap) || !read(wcm + kWcmPlayerSetEntries, entries) || cap > 64) continue;
        for (uint32_t k = 0; k < cap; ++k) {
            uintptr_t chr = deref(entries + 0x10ull * k);
            if (!chr || chr == me) continue;
            float current = 1.0f;
            if (!read(chr + kChrFadeAlpha, current)) continue;
            auto it = alpha.find(chr);
            if (it == alpha.end()) {
                if (!any_away) continue;
                it = alpha.emplace(chr, current).first;
                log::info("away relocate: fading partner copy {:X} out while their position is rewritten", chr);
            }
            float& a = it->second;
            const float target = any_away ? 0.0f : 1.0f;
            if (a == target) {
                if (!any_away) { alpha.erase(it); continue; }
                *reinterpret_cast<volatile float*>(chr + kChrFadeAlpha) = 0.0f;
                continue;
            }
            a = target > a ? std::min(target, a + kFadeStep) : std::max(target, a - kFadeStep);
            *reinterpret_cast<volatile float*>(chr + kChrFadeAlpha) = a;
            *reinterpret_cast<volatile float*>(chr + kChrFadeDelta) = 0.0f;
            if (a == 1.0f) log::info("away relocate: partner copy {:X} faded back in", chr);
        }
    }
}

}

void start(const Context& context) {
    g_game_base = reinterpret_cast<uintptr_t>(context.game);
    load_config();
    if (HANDLE t = CreateThread(nullptr, 0, thread, nullptr, 0, nullptr)) CloseHandle(t);
    log::info("away relocate: started (rewrites the partner's position stream while they are at the Roundtable / in a "
              "dungeon (minor dungeons {}, underground {}); Roundtable shown at block {:08X} ({:.0f}, {:.0f}, {:.0f}); copies fade out meanwhile)",
              g_rewrite_dungeons ? "on" : "off", g_rewrite_underground ? "on" : "off", g_rt_block, g_rt_x, g_rt_y, g_rt_z);
}

bool rewrite(uint64_t peer, uint8_t* payload, size_t len) {
    if (!payload || len < kMinLen || !peer) return false;
    uint32_t block;
    std::memcpy(&block, payload + kBlockOffset, 4);
    std::lock_guard lock(g_mutex);
    auto& st = g_peers[peer];
    st.last_seen = std::chrono::steady_clock::now();
    if ((block >> 24) == kOverworldArea) {
        st.have_home = true;
        st.home_block = block;
        std::memcpy(st.home, payload + kPosOffset, 12);
        leave_away(peer, st, "is back on the overworld");
        return false;
    }
    const uint32_t area = block >> 24;
    const bool roundtable = block == kRoundtableBlock;
    if (!roundtable && !st.have_home) {

        leave_away(peer, st, "entered a map we cannot point at");
        return false;
    }
    if (!roundtable && !(entrance_area(area) && g_rewrite_dungeons) && !(area == kUndergroundArea && g_rewrite_underground)) {
        leave_away(peer, st, "entered a map we leave to the mod");
        return false;
    }

    const uint32_t ours = g_local_block.load(std::memory_order_relaxed);
    if (ours == block || (!roundtable && (ours >> 24) != kOverworldArea)) {
        leave_away(peer, st, "is in the same map as us (or we are off the overworld); passing their stream through");
        return false;
    }
    if (!st.away) {
        st.away = true;
        g_away_peers.fetch_add(1, std::memory_order_relaxed);
        if (roundtable) {
            log::warn("away relocate: peer {:016X} is at the Roundtable; rewriting their position stream to block {:08X} at "
                      "({:.0f}, {:.0f}, {:.0f})", peer, g_rt_block, g_rt_x, g_rt_y, g_rt_z);
        } else {
            log::warn("away relocate: peer {:016X} is in block {:08X} (dungeon/underground); rewriting their position stream "
                      "to the entrance, block {:08X} at ({:.0f}, {:.0f}, {:.0f})",
                      peer, block, st.home_block, st.home[0], st.home[1], st.home[2]);
        }
    }
    const uint32_t out_block = roundtable ? g_rt_block : st.home_block;
    const float pos[3] = {roundtable ? g_rt_x : st.home[0], roundtable ? g_rt_y : st.home[1], roundtable ? g_rt_z : st.home[2]};
    std::memcpy(payload + kBlockOffset, &out_block, 4);
    std::memcpy(payload + kPosOffset, pos, 12);
    st.rewritten++;
    return true;
}

}
