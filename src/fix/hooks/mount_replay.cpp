#include "hooks/mount_replay.h"

#include "game/mem.h"
#include "game/offsets.h"
#include "hooks/inject.h"
#include "log.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <deque>
#include <format>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace ersc_fix::hooks::mount_replay {

namespace {

using namespace ersc_fix::game;
using namespace ersc_fix::game::mem;

constexpr DWORD kTickMs = 250;
constexpr uint8_t kChannelGame = 3;
constexpr uint32_t kTypeRideRecord = 0x12, kTypeBlockNotice = 0x34, kTypeChrStatus = 0x46;
constexpr size_t kRideRecordLen = 60, kBlockNoticeLen = 16;

constexpr size_t kRecBlock = 0, kRecPos1 = 4, kRecPos2 = 16, kRecHorseSel = 0x28, kRecRiderSel = 0x30, kRecState = 0x38;
constexpr size_t kNoticeSel = 4, kStatusSel = 5;
constexpr uint32_t kStateMounting = 3, kStateMounted = 5;
constexpr size_t kStreamBlock = 3, kStreamPos = 7, kStreamMinLen = 19;
constexpr size_t kKeepStatuses = 3;

uintptr_t g_game_base = 0;

struct PeerState {
    std::vector<uint8_t> notice;
    std::deque<std::vector<uint8_t>> statuses;
    std::vector<uint8_t> rec_a, rec_b;
    bool mounted = false;
    bool any_record = false;
    uint32_t horse_sel = 0, rider_sel = 0;
    bool have_stream = false;
    uint32_t stream_block = 0xFFFFFFFFu;
    float pos[3]{};
    int redeliveries = 0;
};
std::mutex g_mutex;
std::map<uint64_t, PeerState> g_peers;
std::atomic<int> g_redelivered{0}, g_rode{0}, g_abandoned{0}, g_waiting{0}, g_unmounted{0};

std::string counters() {
    return std::format("mount replay: counters: bursts re-delivered at a spawn {} copies rode a placed horse {} mount requests abandoned {} copies waiting on an "
                       "unplaced horse {} copies unmounted by the game while the owner rides {}",
                       g_redelivered.load(), g_rode.load(), g_abandoned.load(), g_waiting.load(), g_unmounted.load());
}

struct Copy {
    uintptr_t chr = 0;
    uint32_t h0 = 0, net_sel = 0, node = 0, req = 0, load = 0;
    uint8_t entry = 0;
    bool riding() const { return node == kRideNodeRiding || node == kRideNodeMounting; }
};
struct Horse {
    uintptr_t chr = 0;
    uint32_t h0 = 0, load = 0, block = 0, activation = 0;
    float alpha = 0;
    bool model = false, dead = false;
    bool placed() const { return model && load == kLoadStateHealthy && block != 0xFFFFFFFFu; }
};

bool sample_copy(uintptr_t chr, Copy& c) {
    c.chr = chr;
    read(chr + kChrHandle, c.h0);
    read(chr + kChrNetHandle + 4, c.net_sel);
    read(chr + kChrLoadState, c.load);
    if (uintptr_t req = deref(chr + kChrRideRequestObj)) read(req + kRideReqState, c.req);
    if (uintptr_t entry = deref(chr + kChrSetEntryPtr)) read(entry + kEntryBlockState, c.entry);
    uintptr_t ride = deref(deref(chr + kChrModules) + kModulesRide);
    if (!ride) return false;
    if (uintptr_t node = deref(ride + kRideNode)) read(node + kRideNodeState, c.node);
    return true;
}

bool sample_horse(uintptr_t chr, Horse& h) {
    int32_t npc = -1;
    if (!read(chr + kChrNpcId, npc) || npc != kTorrentNpcId) return false;
    h.chr = chr;
    read(chr + kChrHandle, h.h0);
    read(chr + kChrLoadState, h.load);
    read(chr + kChrBlockId, h.block);
    read(chr + kChrActivationState, h.activation);
    read(chr + kChrFadeAlpha, h.alpha);
    h.model = deref(chr + kChrModelIns) != 0;
    if (uintptr_t ctrl = deref(chr + kChrCtrl)) {
        uint32_t f0 = 0, w24 = 0;
        read(ctrl + kCtrlDeadFlags, f0);
        if (uintptr_t st = deref(ctrl + kCtrlStateObj)) read(st + kCtrlStateDeadWord, w24);
        h.dead = (f0 & 1) || (w24 & 1);
    }
    return true;
}

std::string describe(const Copy& c) {
    return std::format("copy {:08X} node {} req {} load {:X} entry {}", c.h0, c.node, c.req, c.load, c.entry);
}

std::string describe(const Horse* h) {
    if (!h) return "no horse object";
    return std::format("horse {:08X} load {:X} blk {:08X} model {} act {} alpha {:.2f}", h->h0, h->load, h->block, h->model ? "yes" : "NULL", h->activation,
                       h->alpha);
}

std::string inject_burst(uint64_t peer, const PeerState& ps) {
    std::string what;
    if (!ps.notice.empty()) {
        inject::queue(peer, kChannelGame, kTypeBlockNotice, ps.notice.data(), ps.notice.size());
        what += "notice ";
    }
    for (auto& s : ps.statuses) inject::queue(peer, kChannelGame, kTypeChrStatus, s.data(), s.size());
    if (!ps.statuses.empty()) what += std::format("{} status ", ps.statuses.size());
    for (const auto* r : {&ps.rec_a, &ps.rec_b}) {
        if (r->empty()) continue;
        std::vector<uint8_t> rec = *r;
        uint32_t block = 0;
        std::memcpy(&block, rec.data() + kRecBlock, 4);
        if (block != 0xFFFFFFFFu && ps.have_stream) {
            std::memcpy(rec.data() + kRecBlock, &ps.stream_block, 4);
            std::memcpy(rec.data() + kRecPos1, ps.pos, 12);
            std::memcpy(rec.data() + kRecPos2, ps.pos, 12);
        }
        inject::queue(peer, kChannelGame, kTypeRideRecord, rec.data(), rec.size());
    }
    what += std::format("{} ride record(s)", (ps.rec_a.empty() ? 0 : 1) + (ps.rec_b.empty() ? 0 : 1));
    return what;
}

void drop(PeerState& ps) {
    ps.mounted = false;
    ps.notice.clear();
    ps.statuses.clear();
    ps.rec_a.clear();
    ps.rec_b.clear();
    ps.redeliveries = 0;
}

struct Seen {
    bool present = false;
    Copy copy;
    bool horse_placed = false;
    bool waiting_reported = false;
};
std::map<uint64_t, Seen> g_seen;

DWORD WINAPI thread(LPVOID) {
    for (;;) {
        Sleep(kTickMs);
        uintptr_t wcm = deref(g_game_base + kWorldChrManPtr);
        uintptr_t me = wcm ? deref(wcm + kWcmMainPlayer) : 0;
        std::map<uint32_t, Copy> copies;
        std::map<uint32_t, Horse> horses;
        if (me) {
            uint32_t cap = 0;
            uintptr_t entries = 0;
            if (read(wcm + kWcmPlayerSetCapacity, cap) && read(wcm + kWcmPlayerSetEntries, entries) && cap <= 64) {
                for (uint32_t k = 0; k < cap; ++k) {
                    uintptr_t chr = deref(entries + 0x10ull * k);
                    Copy c;
                    if (chr && chr != me && sample_copy(chr, c) && (c.h0 >> 8) == 0x16F000 && c.net_sel) copies[c.net_sel] = c;
                }
            }
            if (uintptr_t buddy_man = deref(wcm + kWcmSummonBuddyManager)) {
                uintptr_t set = deref(buddy_man + kBuddyManChrSet);
                if (set && read(set + kChrSetCapacity, cap) && read(set + kChrSetEntries, entries) && cap <= 4096 && entries) {
                    for (uint32_t i = 0; i < cap; ++i) {
                        uintptr_t chr = deref(entries + 0x10ull * i);
                        Horse h;
                        if (chr && sample_horse(chr, h)) horses[h.h0 & 0xFF] = h;
                    }
                }
            }
        }
        std::lock_guard lock(g_mutex);
        for (auto& [peer, ps] : g_peers) {
            Seen& seen = g_seen[peer];
            auto cit = copies.find(ps.rider_sel);
            auto hit = horses.find(ps.horse_sel & 0xFF);
            const Horse* horse = hit == horses.end() ? nullptr : &hit->second;
            if (ps.mounted && horse && horse->dead) {
                log::info("mount replay: peer {:016X}: the horse object here is dead; cached mount dropped", peer);
                drop(ps);
            }
            if (cit == copies.end()) {
                if (seen.present) {
                    log::info("mount replay: peer {:016X}: their copy is gone (last: {})", peer, describe(seen.copy));
                }
                seen = Seen{};
                continue;
            }
            const Copy& c = cit->second;
            if (!seen.present) {
                log::info("mount replay: peer {:016X}: their copy exists ({}; {}; owner's records: {})", peer, describe(c), describe(horse),
                          ps.mounted ? "mounted" : "not mounted");
            } else if (ps.mounted) {
                const Copy& p = seen.copy;
                const bool placed = horse && horse->placed();
                if (!p.riding() && c.riding()) {

                    if (placed) {
                        ++g_rode;
                        log::info("mount replay: {} is riding a placed horse ({}); {}", describe(c), describe(horse), counters());
                    } else {
                        log::warn("mount replay: {} is riding an UNPLACED horse ({}); the watch stays armed for what the block arrival does",
                                  describe(c), describe(horse));
                    }
                } else if (p.riding() && c.riding() && !seen.horse_placed && placed) {
                    ++g_rode;
                    log::info("mount replay: the horse under riding {} is now placed ({}); {}", describe(c), describe(horse), counters());
                } else if (p.riding() && !c.riding()) {
                    ++g_unmounted;
                    log::warn("mount replay: {} was UNMOUNTED by the game while its owner {:016X} is still mounted ({}); the ride watch names the "
                              "writer. {}",
                              describe(c), peer, describe(horse), counters());
                } else if (p.req == kRideReqMount && c.req != kRideReqMount && !c.riding()) {
                    ++g_abandoned;
                    log::warn("mount replay: the mount request of {} was ABANDONED by the game although its owner {:016X} is mounted ({}); the ride "
                              "watch names the writer. {}",
                              describe(c), peer, describe(horse), counters());
                }
            }

            const bool waiting = ps.mounted && c.req == kRideReqMount && !c.riding() && (!horse || !horse->placed());
            if (waiting && !seen.waiting_reported) {
                ++g_waiting;
                log::warn("mount replay: {} holds a mount request on a horse that is not placed in the world ({}); owner {:016X}; the copy stays "
                          "half-loaded until the horse gets its block. {}",
                          describe(c), describe(horse), peer, counters());
            }
            seen.present = true;
            seen.copy = c;
            seen.horse_placed = horse && horse->placed();
            seen.waiting_reported = waiting ? true : (seen.waiting_reported && c.req == kRideReqMount);
        }
    }
}

}

void note_ride_record(uint64_t peer, const uint8_t* payload, size_t len) {
    if (len != kRideRecordLen) return;
    uint32_t state = 0, horse = 0, rider = 0;
    std::memcpy(&state, payload + kRecState, 4);
    std::memcpy(&horse, payload + kRecHorseSel, 4);
    std::memcpy(&rider, payload + kRecRiderSel, 4);
    std::lock_guard lock(g_mutex);
    auto& ps = g_peers[peer];
    ps.any_record = true;
    if (state == kStateMounting || state == kStateMounted) {
        if (state == kStateMounting || !ps.mounted || ps.rec_a.empty()) {

            ps.rec_a.assign(payload, payload + len);
            ps.rec_b.clear();
            if (ps.horse_sel != horse) {
                ps.notice.clear();
                ps.statuses.clear();
            }
            log::info("mount replay: peer {:016X} mounting (horse selector {:08X}, rider selector {:08X}); burst cached: notice {} status {}",
                      peer, horse, rider, ps.notice.empty() ? "no" : "yes", ps.statuses.size());
        } else {
            ps.rec_b.assign(payload, payload + len);
        }
        ps.mounted = true;
        ps.horse_sel = horse;
        ps.rider_sel = rider;
        ps.redeliveries = 0;
    } else {
        if (ps.mounted) {
            log::info("mount replay: peer {:016X} dismounted (record state {}); cached mount dropped", peer, state);
        }
        drop(ps);
    }
}

void note_block_notice(uint64_t peer, const uint8_t* payload, size_t len) {
    if (len != kBlockNoticeLen) return;
    uint32_t sel = 0;
    std::memcpy(&sel, payload + kNoticeSel, 4);
    if ((sel >> 8) != 0x388) return;
    std::lock_guard lock(g_mutex);
    auto& ps = g_peers[peer];

    if (!ps.mounted || ps.horse_sel == sel || ps.horse_sel == 0) {
        ps.notice.assign(payload, payload + len);
        ps.horse_sel = ps.mounted ? ps.horse_sel : sel;
    }
}

void note_status(uint64_t peer, const uint8_t* payload, size_t len) {
    if (len < 9 || payload[0] != 1) return;
    uint32_t sel = 0;
    std::memcpy(&sel, payload + kStatusSel, 4);
    if ((sel >> 8) != 0x388) return;
    std::lock_guard lock(g_mutex);
    auto& ps = g_peers[peer];
    if (ps.mounted && ps.horse_sel != sel) return;
    ps.statuses.emplace_back(payload, payload + len);
    while (ps.statuses.size() > kKeepStatuses) ps.statuses.pop_front();
}

void note_stream(uint64_t peer, const uint8_t* payload, size_t len) {
    if (len < kStreamMinLen) return;
    std::lock_guard lock(g_mutex);
    auto& ps = g_peers[peer];
    std::memcpy(&ps.stream_block, payload + kStreamBlock, 4);
    std::memcpy(ps.pos, payload + kStreamPos, 12);
    ps.have_stream = true;
}

int owner_record(uint64_t peer) {
    std::lock_guard lock(g_mutex);
    auto it = g_peers.find(peer);
    if (it == g_peers.end() || !it->second.any_record) return -1;
    return it->second.mounted && !it->second.rec_a.empty() ? 1 : 0;
}

uint64_t owner_mounted_on(uint32_t horse_handle) {
    std::lock_guard lock(g_mutex);
    for (auto& [peer, ps] : g_peers) {
        if (ps.mounted && !ps.rec_a.empty() && (ps.horse_sel & 0xFF) == (horse_handle & 0xFF)) return peer;
    }
    return 0;
}

uint32_t owner_block(uint64_t peer) {
    std::lock_guard lock(g_mutex);
    auto it = g_peers.find(peer);
    if (it == g_peers.end() || !it->second.mounted) return 0xFFFFFFFFu;
    const PeerState& ps = it->second;
    if (ps.have_stream && ps.stream_block != 0xFFFFFFFFu) return ps.stream_block;
    if (ps.notice.size() == kBlockNoticeLen) {
        uint32_t block = 0;
        std::memcpy(&block, ps.notice.data() + 12, 4);
        return block;
    }
    return 0xFFFFFFFFu;
}

bool owner_position(uint64_t peer, float out[3]) {
    std::lock_guard lock(g_mutex);
    auto it = g_peers.find(peer);
    if (it == g_peers.end() || !it->second.mounted || !it->second.have_stream) return false;
    std::memcpy(out, it->second.pos, sizeof(float) * 3);
    return true;
}

std::string redeliver(uint64_t peer) {
    std::lock_guard lock(g_mutex);
    auto it = g_peers.find(peer);
    if (it == g_peers.end() || !it->second.mounted || it->second.rec_a.empty()) return {};
    PeerState& ps = it->second;
    const std::string what = inject_burst(peer, ps);
    ++ps.redeliveries;
    ++g_redelivered;
    return std::format("{} (re-delivery #{} of this mount)", what, ps.redeliveries);
}

void start(const Context& context) {
    g_game_base = reinterpret_cast<uintptr_t>(context.game);
    if (HANDLE t = CreateThread(nullptr, 0, thread, nullptr, 0, nullptr)) CloseHandle(t);
    log::info("mount replay: started (keeps each partner's mount burst for the creation-time ride-flag repair and re-delivers it at the game's "
              "spawn of their copy; observes the copy's mount request afterwards; no timers)");
}

}
