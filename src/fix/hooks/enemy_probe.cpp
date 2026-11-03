#include "hooks/enemy_probe.h"

#include "config.h"
#include "game/mem.h"
#include "game/offsets.h"
#include "hooks/flag_probe.h"
#include "hooks/inject.h"
#include "hooks/main_thread.h"
#include "hooks/ride_probe.h"
#include "hooks/steam_log.h"
#include "hooks/summon_replay.h"
#include "log.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <format>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace ersc_fix::hooks::enemy_probe {

namespace {

using namespace ersc_fix::game;
using namespace ersc_fix::game::mem;

constexpr int kLinesPerTick = 40;
constexpr DWORD kTickMs = 1000;
constexpr float kJump = 20.0f;
constexpr float kJumpY = 5.0f;
constexpr int kBulk = 6;
constexpr auto kCensusEvery = std::chrono::seconds(60);
constexpr auto kLiveEvery = std::chrono::seconds(30);
bool g_raw = false;

constexpr uint32_t kModulesData = 0x0;
constexpr uint32_t kDataHp = 0x138;
constexpr uint32_t kDataMaxHp = 0x13C;

constexpr uint32_t kDataBlock = 0x7C;
constexpr uint32_t kChrFlag1CA = 0x1CA;

uintptr_t g_game_base = 0;

bool g_revive = false;
constexpr int kReviveStreamMentions = 5;
constexpr int64_t kReviveRespawnWindowMs = 90000;
constexpr auto kReviveMinDisabled = std::chrono::seconds(15);

bool g_sync = true;
bool g_sync_apply = true;
constexpr uint32_t kDisabledSetMagic = 0x32545344;
constexpr auto kFreshWindow = std::chrono::seconds(120);
constexpr auto kPeerSetFresh = std::chrono::seconds(3);

constexpr auto kOwnDeathWindow = std::chrono::seconds(60);
struct PeerSet {
    std::map<uint64_t, std::chrono::steady_clock::time_point> since;
    std::chrono::steady_clock::time_point at;
};
std::mutex g_peer_sets_mutex;
std::map<uint64_t, PeerSet> g_peer_sets;
std::mutex g_last_mutex;
std::map<uintptr_t, struct State> g_last;

struct State {
    int set = -1;
    uint32_t h0 = 0, h1 = 0;
    uint32_t net_block = 0, net_sel = 0;
    int32_t npc = -1;
    uint32_t block = 0;
    uint32_t load = 0;
    uint32_t parked = 0;
    uint32_t fade = 0;
    uint8_t flags1c4 = 0;
    uint32_t sync0 = 0, sync1 = 0;
    int32_t interval = 0, mode = 0;
    bool model = false;
    uint32_t ctrl_dead = 0;
    int32_t hp = -1, max_hp = -1;
    uint8_t entry_state = 0, entry_flags = 0;
    uint8_t f1ca = 0;
    uint32_t data_block = 0;
    std::string raw;
    uint64_t owner = 0;
    bool mine = false;
    float x = 0, y = 0, z = 0;
    bool abs = false;
    float alpha = 0, dist = 0;

    uint64_t key() const { return (static_cast<uint64_t>(net_block) << 32) | net_sel; }
    bool moved_from(const State& o) const {
        return model && o.model && abs == o.abs &&
               (std::fabs(x - o.x) >= kJump || std::fabs(z - o.z) >= kJump || std::fabs(y - o.y) >= kJumpY);
    }
    bool faded_from(const State& o) const { return model && o.model && std::fabs(alpha - o.alpha) >= 0.3f; }

    bool update_changed(const State& o) const {
        return model && o.model && ((interval == -1) != (o.interval == -1) || (mode == 2) != (o.mode == 2));
    }
    bool operator==(const State& o) const {
        return block == o.block && load == o.load && parked == o.parked && fade == o.fade && model == o.model &&
               (sync0 & 0x10) == (o.sync0 & 0x10) && (sync1 & 0x4) == (o.sync1 & 0x4) &&
               (ctrl_dead & 1) == (o.ctrl_dead & 1) && entry_state == o.entry_state && entry_flags == o.entry_flags &&
               f1ca == o.f1ca && data_block == o.data_block && !moved_from(o) && !faded_from(o) && !update_changed(o);
    }
};

bool raw_xyz(uintptr_t chr, uint32_t& block, float& x, float& y, float& z) {
    uintptr_t physics = deref(deref(chr + kChrModules) + kModulesPhysics);
    if (!physics || !read(chr + kChrBlockId, block)) return false;
    if (!read(physics + kPhysicsPosition, x) || !read(physics + kPhysicsPosition + 4, y) || !read(physics + kPhysicsPosition + 8, z)) return false;
    if ((block >> 24) == 0x3C && (block & 0xFF) == 0) {
        x += static_cast<float>((block >> 16) & 0xFF) * 256.0f;
        z += static_cast<float>((block >> 8) & 0xFF) * 256.0f;
    }
    return true;
}

bool same_frame(uint32_t a, uint32_t b) {
    if (a == b) return true;
    return (a >> 24) == 0x3C && (b >> 24) == 0x3C && (a & 0xFF) == 0 && (b & 0xFF) == 0;
}

struct Frame {
    bool valid = false;
    float dx = 0, dy = 0, dz = 0;
};

Frame stream_frame(float my_raw_x, float my_raw_y, float my_raw_z) {
    Frame f;
    uint32_t block;
    float sx, sy, sz;
    if (!summon_replay::own_stream_position(block, sx, sy, sz)) return f;
    if ((block >> 24) == 0x3C && (block & 0xFF) == 0) {
        sx += static_cast<float>((block >> 16) & 0xFF) * 256.0f;
        sz += static_cast<float>((block >> 8) & 0xFF) * 256.0f;
    }
    f.valid = true;
    f.dx = sx - my_raw_x;
    f.dy = sy - my_raw_y;
    f.dz = sz - my_raw_z;
    return f;
}

struct Ownership {
    std::map<uint32_t, uint64_t> owners;
    std::set<uint32_t> owned;
};

void read_ownership(Ownership& o) {
    uintptr_t sess = deref(g_game_base + kSessionManPtr);
    uintptr_t own = sess ? deref(sess + kSessBlockOwnership) : 0;
    if (!own) return;
    walk_tree(own + kOwnOwnedSet, [&](uintptr_t node) {
        uint32_t block = 0;
        if (read(node + kTreeNodeValue, block)) o.owned.insert(block);
    });
    walk_tree(own + kOwnBlockOwnerMap, [&](uintptr_t node) {
        uint32_t block = 0;
        uint64_t owner = 0;
        if (read(node + kTreeNodeValue, block) && read(node + kTreeNodeMapValue, owner)) o.owners[block] = owner;
    });
}

struct Me {
    uint32_t block = 0;
    float x = 0, y = 0, z = 0;
    Frame frame;
};

bool sample(uintptr_t chr, const Ownership& own, const Me& me, State& s) {
    float rx = 0, ry = 0, rz = 0;
    if (!raw_xyz(chr, s.block, rx, ry, rz)) {

        if (!read(chr + kChrBlockId, s.block)) return false;
        rx = ry = rz = 0;
    }
    const float dx = rx - me.x, dz = rz - me.z;
    s.dist = same_frame(s.block, me.block) ? std::sqrt(dx * dx + dz * dz) : -1.0f;
    s.abs = me.frame.valid;
    s.x = s.abs ? rx + me.frame.dx : rx;
    s.y = s.abs ? ry + me.frame.dy : ry;
    s.z = s.abs ? rz + me.frame.dz : rz;
    read(chr + kChrHandle, s.h0);
    read(chr + kChrHandle + 4, s.h1);
    read(chr + kChrNetHandle, s.net_block);
    read(chr + kChrNetHandle + 4, s.net_sel);
    read(chr + kChrNpcId, s.npc);
    read(chr + kChrLoadState, s.load);
    read(chr + kChrLoadStateExtra, s.parked);
    read(chr + kChrActivationState, s.fade);
    read(chr + kChrFlags1C4, s.flags1c4);
    read(chr + kChrFlags1C4 + 4, s.sync0);
    read(chr + kChrFlags1C4 + 8, s.sync1);
    read(chr + kChrUpdateInterval, s.interval);
    read(chr + kChrUpdateMode, s.mode);
    read(chr + kChrFadeAlpha, s.alpha);
    s.model = deref(chr + kChrModelIns) != 0;
    if (uintptr_t ctrl = deref(chr + kChrCtrl)) read(ctrl + kCtrlDeadFlags, s.ctrl_dead);
    if (uintptr_t data = deref(deref(chr + kChrModules) + kModulesData)) {
        int32_t hp = -1, max = -1;
        if (read(data + kDataHp, hp) && read(data + kDataMaxHp, max) && hp >= 0 && max >= hp && max <= 200000) {
            s.hp = hp;
            s.max_hp = max;
        }
        read(data + kDataBlock, s.data_block);
    }
    if (uintptr_t entry = deref(chr + kChrSetEntryPtr)) {
        read(entry + kEntryBlockState, s.entry_state);
        read(entry + kEntryFlags, s.entry_flags);
    }
    read(chr + kChrFlag1CA, s.f1ca);
    auto it = own.owners.find(s.block);
    s.owner = it == own.owners.end() ? 0 : it->second;
    s.mine = own.owned.count(s.block) != 0;
    return true;
}

void scan_set(uintptr_t set, int index, uintptr_t me_chr, const Ownership& own, const Me& me, std::map<uintptr_t, State>& out) {
    uint32_t cap = 0;
    uintptr_t entries = 0;
    if (!set || !read(set + kChrSetCapacity, cap) || !read(set + kChrSetEntries, entries) || cap > 8192 || !entries) return;
    for (uint32_t i = 0; i < cap; ++i) {
        uintptr_t chr = deref(entries + 0x10ull * i);
        if (!chr || chr == me_chr || out.count(chr)) continue;
        State s;
        s.set = index;
        if (sample(chr, own, me, s)) out[chr] = s;
    }
}

bool scan_synced_sets(uintptr_t wcm, uintptr_t me_chr, const Ownership& own, const Me& me, std::map<uintptr_t, State>& out) {
    uintptr_t mgr = deref(wcm + kWcmNetChrSync);
    int32_t count = 0;
    if (!mgr || !read(mgr + kNetSyncSlotCount, count) || count <= 0 || count > 256) return false;
    const uintptr_t players = wcm + kWcmChrSets;
    for (int i = 0; i < count; ++i) {
        uintptr_t slot = deref(mgr + kNetSyncSlots + 8ull * i);
        uintptr_t set = slot ? deref(slot + kNetSyncSlotChrSet) : 0;
        if (!set || set == players) continue;
        scan_set(set, i, me_chr, own, me, out);
    }
    return true;
}

DWORD WINAPI thread(LPVOID) {
    std::map<uintptr_t, State> tracked;
    std::map<uintptr_t, std::chrono::steady_clock::time_point> first_seen;
    std::map<uintptr_t, std::chrono::steady_clock::time_point> disabled_since;
    std::map<int, std::chrono::steady_clock::time_point> rebuilt_at;
    std::set<uintptr_t> applied;
    bool sent_set = false;
    for (;;) {
        Sleep(kTickMs);
        uintptr_t wcm = deref(g_game_base + kWorldChrManPtr);
        uintptr_t me_chr = wcm ? deref(wcm + kWcmMainPlayer) : 0;
        Me me;
        if (!me_chr || !raw_xyz(me_chr, me.block, me.x, me.y, me.z)) {
            tracked.clear();
            continue;
        }
        me.frame = stream_frame(me.x, me.y, me.z);
        Ownership own;
        read_ownership(own);

        std::map<uintptr_t, State> now;
        const bool via_sync = scan_synced_sets(wcm, me_chr, own, me, now);
        if (!via_sync) {
            for (int k = 1; k < 5; ++k) scan_set(wcm + kWcmChrSets + k * kWcmChrSetStride, k, me_chr, own, me, now);
        }
        if (uintptr_t buddy_man = deref(wcm + kWcmSummonBuddyManager)) {
            scan_set(deref(buddy_man + kBuddyManChrSet), -2, me_chr, own, me, now);
        }
        const auto tick_t = std::chrono::steady_clock::now();
        {
            std::lock_guard lock(g_last_mutex);
            g_last = now;
        }
        for (auto& [chr, s] : now) {
            if (!first_seen.count(chr)) first_seen[chr] = tick_t;
            if (s.model && (s.entry_flags & 1)) disabled_since.try_emplace(chr, tick_t);
            else disabled_since.erase(chr);
        }
        for (auto it = first_seen.begin(); it != first_seen.end();) {
            if (!now.count(it->first)) { applied.erase(it->first); disabled_since.erase(it->first); it = first_seen.erase(it); }
            else ++it;
        }

        std::map<int, int> arrivals;
        for (auto& [chr, s] : now) if (!tracked.count(chr)) ++arrivals[s.set];
        std::set<int> bulk_in;
        for (auto& [set, n] : arrivals) {
            if (n < kBulk) continue;
            bulk_in.insert(set);
        }
        for (int set : bulk_in) {

            rebuilt_at[set] = tick_t;
        }

        if (g_sync) {
            std::vector<uint32_t> words;
            words.push_back(kDisabledSetMagic);
            words.push_back(0);
            for (auto& [chr, s] : now) {
                if (!s.model || !(s.entry_flags & 1) || !s.net_sel || s.net_sel == 0xFFFFFFFFu) continue;
                words.push_back(s.net_block);
                words.push_back(s.net_sel);
                auto ds = disabled_since.find(chr);
                const auto age = ds == disabled_since.end() ? std::chrono::milliseconds(0)
                                                            : std::chrono::duration_cast<std::chrono::milliseconds>(tick_t - ds->second);
                words.push_back(static_cast<uint32_t>(std::min<int64_t>(age.count(), 0x7FFFFFFF)));
            }
            words[1] = static_cast<uint32_t>((words.size() - 2) / 3);
            if (words[1] || sent_set) {
                uint64_t peers[16];
                const size_t n = inject::peers(peers, 16);
                int ok = 0;
                for (size_t i = 0; i < n; ++i) {
                    if (steam_log::send_raw(peers[i], 1, kDisabledSetType, words.data(), words.size() * sizeof(uint32_t))) ++ok;
                }
                if (!sent_set || (words[1] == 0)) {
                    log::info("enemy probe: disabled set sync: {} disabled enemies announced to {} of {} peers", words[1], ok, n);
                }
                sent_set = words[1] != 0;
            }
        }

        if (g_sync && g_sync_apply) {
            std::map<uint64_t, PeerSet> peer_sets;
            {
                std::lock_guard lock(g_peer_sets_mutex);
                peer_sets = g_peer_sets;
            }
            for (auto& [chr, s] : now) {
                if (!s.model || (s.entry_flags & 1) || (s.ctrl_dead & 1) || applied.count(chr)) continue;
                auto rb = rebuilt_at.find(s.set);
                if (rb == rebuilt_at.end() || tick_t - rb->second > kFreshWindow) continue;

                if (flag_probe::reload_verdict() != flag_probe::Reload::unilateral) continue;
                auto fs = first_seen.find(chr);
                if (fs == first_seen.end() || fs->second < rb->second) continue;
                uint64_t from = 0;
                for (auto& [peer, ps] : peer_sets) {
                    if (tick_t - ps.at > kPeerSetFresh) continue;
                    auto k = ps.since.find(s.key());

                    if (k != ps.since.end() && k->second < rb->second) {
                        from = peer;
                        break;
                    }
                }
                if (!from) continue;
                if (!kChrSetDisabled) {
                    if (!applied.count(0)) {
                        applied.insert(0);
                        log::warn("enemy probe: disabled set sync cannot act: ChrIns::SetDisabled was not resolved by signature in this game build");
                    }
                    continue;
                }
                uintptr_t entry = deref(chr + kChrSetEntryPtr);
                uint8_t flags = 0;
                if (!entry || !read(entry + kEntryFlags, flags) || (flags & 1)) continue;
                applied.insert(chr);

                const uintptr_t game = g_game_base;
                const State snap = s;
                const uint64_t peer = from;
                const long long since_rebuild = std::chrono::duration_cast<std::chrono::seconds>(tick_t - rb->second).count();
                const bool posted = main_thread::post([chr, entry, game, snap, peer, since_rebuild] {
                    uintptr_t owner = 0;
                    uint8_t f = 0;
                    if (!read(entry, owner) || owner != chr || !read(entry + kEntryFlags, f) || (f & 1)) {
                        log::info("enemy probe: disable of fresh copy {:X} skipped on the game thread: entry {:X} no longer its own (owner {:X}, flags {:02X})",
                                  chr, entry, owner, f);
                        return;
                    }
                    reinterpret_cast<void (*)(uintptr_t, uint8_t)>(game + kChrSetDisabled)(chr, 1);
                    uint8_t after = 0;
                    read(entry + kEntryFlags, after);
                    log::warn("enemy probe: disabled fresh copy {:X} (net {:08X}:{:08X}, npc {}, {:.0f} m, at ({:.0f}, {:.0f}, {:.0f})) via "
                              "ChrIns::SetDisabled on the game thread, to match peer {:016X}, who holds it disabled; our set s{} was rebuilt {} s ago "
                              "(entry flags {:02X} -> {:02X})",
                              chr, snap.net_block, snap.net_sel, snap.npc, snap.dist, snap.x, snap.y, snap.z, peer, snap.set, since_rebuild, f, after);
                });
                if (!posted) {
                    applied.erase(chr);
                    if (!applied.count(1)) {
                        applied.insert(1);
                        log::warn("enemy probe: disabled set sync cannot act yet: game-thread hook not installed");
                    }
                }
            }
        }

        tracked.swap(now);
    }
}

}

void note_disabled_set(uint64_t peer, const uint8_t* payload, size_t len) {
    if (!peer || !payload || len < 8) return;
    uint32_t magic = 0, count = 0;
    std::memcpy(&magic, payload, 4);
    std::memcpy(&count, payload + 4, 4);
    if (magic != kDisabledSetMagic || len < 8 + 12ull * count || count > 4096) return;
    PeerSet ps;
    ps.at = std::chrono::steady_clock::now();
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t block = 0, sel = 0, age_ms = 0;
        std::memcpy(&block, payload + 8 + 12ull * i, 4);
        std::memcpy(&sel, payload + 12 + 12ull * i, 4);
        std::memcpy(&age_ms, payload + 16 + 12ull * i, 4);
        ps.since[(static_cast<uint64_t>(block) << 32) | sel] = ps.at - std::chrono::milliseconds(age_ms);
    }
    std::lock_guard lock(g_peer_sets_mutex);
    auto& cur = g_peer_sets[peer];
    bool changed = cur.since.size() != ps.since.size();
    if (!changed) {
        for (auto& [k, t] : ps.since) if (!cur.since.count(k)) { changed = true; break; }
    }
    if (changed) {
        std::string list;
        int shown = 0;
        for (auto& [k, t] : ps.since) {
            if (shown++ == 8) { list += " .."; break; }
            list += std::format(" {:08X}:{:08X}({}s)", static_cast<uint32_t>(k >> 32), static_cast<uint32_t>(k),
                                std::chrono::duration_cast<std::chrono::seconds>(ps.at - t).count());
        }
        log::info("enemy probe: peer {:016X} now holds {} disabled enemies:{}", peer, ps.since.size(), list);
    }
    cur = std::move(ps);
}

void start(const Context& context) {
    g_game_base = reinterpret_cast<uintptr_t>(context.game);
    g_revive = GetPrivateProfileIntW(L"hooks", L"disabled_copy_revive", 0, config::ini_path().c_str()) != 0;
    g_raw = GetPrivateProfileIntW(L"hooks", L"enemy_probe_raw", 0, config::ini_path().c_str()) != 0;
    g_sync = GetPrivateProfileIntW(L"hooks", L"disabled_set_sync", 1, config::ini_path().c_str()) != 0;
    g_sync_apply = GetPrivateProfileIntW(L"hooks", L"disabled_set_apply", 1, config::ini_path().c_str()) != 0;
    if (HANDLE t = CreateThread(nullptr, 0, thread, nullptr, 0, nullptr)) CloseHandle(t);
    log::info("enemy probe: disabled set sync {} (peers exchange their disabled enemies), apply {} (a game rebuilt after its own death "
              "disables its fresh copies to match, via ChrIns::SetDisabled at +0x{:X} on the game thread); disabled-copy revive {} (v0.108 experiment)",
              g_sync ? "on" : "off", g_sync_apply ? (kChrSetDisabled ? "on" : "on but the routine was NOT resolved: inert") : "off",
              kChrSetDisabled, g_revive ? "on" : "off");
    log::info("enemy probe: started (all non-player chrs in every synced ChrSet, per-block sets included; logged on state change; "
              "modelled chrs listed every 10 s with position, alpha, activation, HP and the network messages naming them; census every 60 s)");
}

bool describe(uintptr_t chr, ChrInfo& out) {
    std::lock_guard lock(g_last_mutex);
    auto it = g_last.find(chr);
    if (it == g_last.end()) return false;
    const State& s = it->second;
    out.npc = s.npc;
    out.net_block = s.net_block;
    out.net_sel = s.net_sel;
    out.block = s.block;
    out.entry_flags = s.entry_flags;
    out.model = s.model;
    out.x = s.x;
    out.y = s.y;
    out.z = s.z;
    return true;
}

bool describe_by_handle(uint64_t handle, ChrInfo& out) {
    const uint32_t sel = static_cast<uint32_t>(handle), block = static_cast<uint32_t>(handle >> 32);
    std::lock_guard lock(g_last_mutex);
    for (auto& [chr, s] : g_last) {
        if (s.h0 != sel || s.h1 != block) continue;
        out.npc = s.npc;
        out.net_block = s.net_block;
        out.net_sel = s.net_sel;
        out.block = s.block;
        out.entry_flags = s.entry_flags;
        out.model = s.model;
        out.x = s.x;
        out.y = s.y;
        out.z = s.z;
        return true;
    }
    return false;
}

bool peer_holds_disabled_model(int32_t npc, uint32_t block, uint64_t* peer_out, uint32_t* sel_out) {
    std::vector<uint64_t> keys;
    {
        std::lock_guard lock(g_last_mutex);
        for (auto& [chr, s] : g_last) if (s.npc == npc && s.block == block && s.net_sel && s.net_sel != 0xFFFFFFFFu) keys.push_back(s.key());
    }
    if (keys.empty()) return false;
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard lock(g_peer_sets_mutex);
    for (auto& [peer, ps] : g_peer_sets) {
        if (now - ps.at > kPeerSetFresh) continue;
        for (uint64_t k : keys) {
            if (ps.since.count(k)) {
                if (peer_out) *peer_out = peer;
                if (sel_out) *sel_out = static_cast<uint32_t>(k);
                return true;
            }
        }
    }
    return false;
}

}
