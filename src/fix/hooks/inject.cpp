#include "hooks/inject.h"

#include "log.h"

#include <array>
#include <atomic>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <vector>

namespace ersc_fix::hooks::inject {

namespace {

constexpr size_t kMaxPending = 128;
constexpr size_t kMaxPayload = 1024;

std::mutex g_mutex;
std::map<uint64_t, SteamNetworkingIdentity> g_peers;
std::array<std::atomic<int>, 8> g_channel_for_header;

struct Pending {
    uint64_t peer;
    uint8_t header_channel;
    uint32_t type;
    std::vector<uint8_t> payload;
};
std::deque<Pending> g_pending;

struct Synthetic {
    SteamNetworkingMessage msg{};
    uint8_t buf[sizeof(ErscHeader) + kMaxPayload];
};

void __fastcall release_synthetic(SteamNetworkingMessage* m) {
    delete reinterpret_cast<Synthetic*>(m);
}
void __fastcall free_synthetic(SteamNetworkingMessage*) {}

struct Init {
    Init() { for (auto& c : g_channel_for_header) c.store(-1); }
} g_init;

}

void note_channel(uint8_t header_channel, int local_channel) {
    if (header_channel >= g_channel_for_header.size()) return;
    auto& slot = g_channel_for_header[header_channel];
    const int known = slot.load(std::memory_order_relaxed);
    if (known == local_channel) return;
    slot.store(local_channel, std::memory_order_relaxed);
    log::info("inject: header channel {} messages are polled on Steam channel {} (was {})", header_channel, local_channel,
              known);
}

void note_peer(const SteamNetworkingIdentity& peer) {
    if (peer.type != kIdentityTypeSteamId || !peer.steam_id) return;
    std::lock_guard lock(g_mutex);
    if (!g_peers.count(peer.steam_id)) g_peers.emplace(peer.steam_id, peer);
}

bool known_peer(uint64_t steam_id) {
    std::lock_guard lock(g_mutex);
    return g_peers.count(steam_id) != 0;
}

size_t peers(uint64_t* out, size_t max) {
    std::lock_guard lock(g_mutex);
    size_t n = 0;
    for (auto& [id, _] : g_peers) {
        if (n >= max) break;
        out[n++] = id;
    }
    return n;
}

void queue(uint64_t peer, uint8_t header_channel, uint32_t type, const void* payload, size_t len) {
    if (len > kMaxPayload) return;
    std::lock_guard lock(g_mutex);
    if (!g_peers.count(peer)) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            log::warn("inject: no identity for peer {:016X} yet; dropping a type 0x{:X} message", peer, type);
        }
        return;
    }
    if (g_pending.size() >= kMaxPending) return;
    Pending p{peer, header_channel, type, std::vector<uint8_t>(static_cast<const uint8_t*>(payload),
                                                               static_cast<const uint8_t*>(payload) + len)};
    g_pending.push_back(std::move(p));
}

int inject(int local_channel, SteamNetworkingMessage** out, int capacity) {
    if (capacity <= 0) return 0;
    std::lock_guard lock(g_mutex);
    int n = 0;
    for (auto it = g_pending.begin(); it != g_pending.end() && n < capacity;) {
        const Pending& p = *it;
        const int steam_channel = p.header_channel < g_channel_for_header.size()
                                      ? g_channel_for_header[p.header_channel].load(std::memory_order_relaxed)
                                      : -1;
        if (steam_channel != local_channel) { ++it; continue; }
        auto peer = g_peers.find(p.peer);
        if (peer == g_peers.end()) { it = g_pending.erase(it); continue; }

        auto* m = new Synthetic;
        ErscHeader h{p.header_channel, 0, 0, p.type, static_cast<uint32_t>(p.payload.size())};
        std::memcpy(m->buf, &h, sizeof(h));
        std::memcpy(m->buf + sizeof(h), p.payload.data(), p.payload.size());
        m->msg.data = m->buf;
        m->msg.size = static_cast<int32_t>(sizeof(h) + p.payload.size());
        m->msg.peer = peer->second;
        m->msg.free_fn = reinterpret_cast<void*>(&free_synthetic);
        m->msg.release_fn = reinterpret_cast<void*>(&release_synthetic);
        m->msg.channel = local_channel;
        out[n++] = &m->msg;
        it = g_pending.erase(it);
    }
    return n;
}

}
