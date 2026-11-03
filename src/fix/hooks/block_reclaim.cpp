#include "hooks/block_reclaim.h"

#include "game/mem.h"
#include "game/offsets.h"
#include "hooks/inject.h"
#include "log.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <map>
#include <set>
#include <vector>

namespace ersc_fix::hooks::block_reclaim {

namespace {

using namespace ersc_fix::game;
using namespace ersc_fix::game::mem;
using clock = std::chrono::steady_clock;

constexpr uint32_t kTypeBlockLoadNotice = 0x35;
constexpr uint32_t kRoleHost = 3;
constexpr uint32_t kStateInSession = 6;
constexpr int kStableChecks = 2;
constexpr auto kRetryInterval = std::chrono::seconds(30);
constexpr auto kBounceMaxCooldown = std::chrono::minutes(10);
constexpr auto kBounceForget = std::chrono::minutes(10);

uintptr_t g_game_base = 0;

struct Snapshot {
    uint32_t role = 0, state = 0;
    std::set<uint32_t> owned;
    std::map<uint32_t, uint64_t> owners;
    std::map<uint64_t, std::set<uint32_t>> loaded;
};

bool take_snapshot(Snapshot& s) {
    uintptr_t sess = deref(g_game_base + kSessionManPtr);
    if (!sess) return false;
    read(sess + kSessRole, s.role);
    read(sess + kSessState, s.state);
    uintptr_t own = deref(sess + kSessBlockOwnership);
    if (!own) return false;
    walk_tree(own + kOwnOwnedSet, [&](uintptr_t node) {
        uint32_t block = 0;
        if (read(node + kTreeNodeValue, block)) s.owned.insert(block);
    });
    walk_tree(own + kOwnBlockOwnerMap, [&](uintptr_t node) {
        uint32_t block = 0;
        uint64_t owner = 0;
        if (read(node + kTreeNodeValue, block) && read(node + kTreeNodeMapValue, owner)) s.owners[block] = owner;
    });
    walk_list(own + kOwnSessionList, [&](uintptr_t entry) {
        uint64_t id = 0;
        if (!read(entry + kSessEntryId, id)) return;
        auto& blocks = s.loaded[id];
        walk_tree(entry + kSessEntryLoadedHead, [&](uintptr_t node) {
            uint32_t block = 0;
            if (read(node + kSessEntryLoadedKey, block)) blocks.insert(block);
        });
    });
    return true;
}

uint64_t g_my_id = 0;

constexpr auto kRecentlyOwned = std::chrono::seconds(120);

DWORD WINAPI reclaim_thread(LPVOID) {
    std::map<uint32_t, int> streak;
    struct Bounce {
        int attempts = 0;
        clock::time_point last;
    };
    std::map<uint32_t, Bounce> bounce;
    std::map<uint32_t, clock::time_point> owned_seen;
    bool warned_no_id = false;
    for (;;) {
        Sleep(2000);
        Snapshot s;
        if (!take_snapshot(s) || s.role != kRoleHost || s.state != kStateInSession) {
            streak.clear();
            if (s.role != kRoleHost) g_my_id = 0;
            continue;
        }
        for (uint32_t block : s.owned) {
            auto it = s.owners.find(block);
            if (it != s.owners.end() && it->second) { g_my_id = it->second; break; }
        }
        if (!g_my_id) {
            if (!warned_no_id) {
                warned_no_id = true;
                log::warn("block reclaim: host owns no block yet, so our session id is unknown; waiting");
            }
            continue;
        }
        auto mine = s.loaded.find(g_my_id);
        if (mine == s.loaded.end()) { streak.clear(); continue; }

        const auto now0 = clock::now();
        for (uint32_t block : s.owned) owned_seen[block] = now0;
        for (auto it = owned_seen.begin(); it != owned_seen.end();) {
            if (now0 - it->second > kRecentlyOwned) it = owned_seen.erase(it); else ++it;
        }

        std::vector<uint32_t> candidates;
        for (auto& [block, owner] : s.owners) {
            if (owner == 0 || owner == g_my_id) continue;
            if (!mine->second.count(block)) continue;
            if (!owned_seen.count(block)) continue;
            if (!inject::known_peer(owner)) continue;
            candidates.push_back(block);
        }
        for (auto it = streak.begin(); it != streak.end();) {
            if (std::find(candidates.begin(), candidates.end(), it->first) == candidates.end()) it = streak.erase(it);
            else ++it;
        }
        const auto now = clock::now();
        for (uint32_t block : candidates) {
            if (++streak[block] < kStableChecks) continue;

            auto& b = bounce[block];
            if (b.attempts && now - b.last > kBounceForget) b.attempts = 0;
            auto cooldown_for = [](int attempts) {
                std::chrono::seconds cd = kRetryInterval * (1 << std::min(attempts, 5));
                return cd > kBounceMaxCooldown ? std::chrono::duration_cast<std::chrono::seconds>(kBounceMaxCooldown) : cd;
            };
            if (b.attempts && now - b.last < cooldown_for(b.attempts - 1)) continue;
            b.last = now;
            b.attempts++;
            if (b.attempts > 1) {
                log::warn("block reclaim: block {:08X} has left us {} times; next reclaim no sooner than {} s after this one",
                          block, b.attempts, cooldown_for(b.attempts - 1).count());
            }
            const uint64_t owner = s.owners[block];
            uint32_t notice[2] = {block, 0};
            inject::queue(owner, kChannelGameEvents, kTypeBlockLoadNotice, notice, sizeof(notice));
            notice[1] = 1;
            inject::queue(owner, kChannelGameEvents, kTypeBlockLoadNotice, notice, sizeof(notice));
            log::warn("block reclaim: block {:08X} is loaded here but owned by {} (we are {}); queued a synthetic "
                      "unload+load notice from that peer so the game reassigns it to us",
                      block, owner & 0xFFFFFFFFu, g_my_id & 0xFFFFFFFFu);
        }
    }
}

}

void start(const Context& context) {
    g_game_base = reinterpret_cast<uintptr_t>(context.game);
    if (HANDLE t = CreateThread(nullptr, 0, reclaim_thread, nullptr, 0, nullptr)) CloseHandle(t);
    log::info("block reclaim: started (host only; every 2 s; takes back loaded blocks another session owns)");
}

}
