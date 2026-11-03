#include "hooks/steam_log.h"

#include "hooks/away_relocate.h"
#include "hooks/enemy_probe.h"
#include "hooks/flag_probe.h"
#include "hooks/mount_probe.h"
#include "hooks/mount_replay.h"
#include "hooks/summon_replay.h"
#include "hooks/inject.h"
#include "hooks/steam_types.h"

#include "game/event_flags.h"
#include "log.h"

#include <windows.h>
#include <intrin.h>
#pragma intrinsic(_ReturnAddress)

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <algorithm>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace ersc_fix::hooks::steam_log {

namespace {

constexpr int kSendReliable = 8;
constexpr int kResultOK = 1;

using SendFn = int(__fastcall*)(void* self, const SteamNetworkingIdentity* to, const void* data, uint32_t size,
                                int flags, int channel);
using RecvFn = int(__fastcall*)(void* self, int local_channel, SteamNetworkingMessage** out, int max);

SendFn g_orig_send = nullptr;
RecvFn g_orig_recv = nullptr;

std::mutex g_mutex;

constexpr uint32_t kTypeFlagSet = 0x11;
constexpr uint32_t kTypeFlagDeferred = 0x12;

std::atomic<bool> g_storm_breaker{true};
constexpr auto kEchoWindow = std::chrono::milliseconds(150);
constexpr auto kEchoDecay = std::chrono::seconds(1);
constexpr auto kSuppressFor = std::chrono::seconds(2);

constexpr int kEchoesToTrip = 3;
struct FlagEchoState {
    std::chrono::steady_clock::time_point last_recv{};
    uint8_t last_recv_value = 0;
    int echoes = 0;
    std::chrono::steady_clock::time_point last_echo{};
    std::chrono::steady_clock::time_point suppressed_until{};
    uint64_t dropped = 0;
    uint64_t trips = 0;
};
std::map<uint32_t, FlagEchoState> g_flag_echo;

void storm_note_recv(uint32_t flag, uint8_t value, std::chrono::steady_clock::time_point now) {
    auto& s = g_flag_echo[flag];
    s.last_recv = now;
    s.last_recv_value = value;
}

bool storm_check_send(uint32_t flag, uint8_t value, std::chrono::steady_clock::time_point now) {
    auto it = g_flag_echo.find(flag);
    if (it == g_flag_echo.end()) return false;
    auto& s = it->second;

    if (now < s.suppressed_until) {
        s.dropped++;
        return true;
    }
    if (s.dropped) {
        auto local = game::event_flags::get(flag);
        log::info("storm breaker: flag {} suppression ended, {} replies dropped{}", flag, s.dropped,
                  local ? std::format("; this game now holds {}", *local ? 1 : 0) : std::string{});
        s.dropped = 0;
    }

    const bool echo = (now - s.last_recv) < kEchoWindow && value != s.last_recv_value;
    if (!echo) return false;
    if (now - s.last_echo > kEchoDecay) s.echoes = 0;
    s.last_echo = now;
    if (++s.echoes < kEchoesToTrip) return false;

    s.echoes = 0;
    s.trips++;
    s.suppressed_until = now + kSuppressFor;
    s.dropped = 1;
    auto local = game::event_flags::get(flag);
    log::warn("storm breaker: flag {} is ping-ponging (we reply {} to their {}); dropping our replies for {} s "
              "[trip #{}]{}",
              flag, value, s.last_recv_value, kSuppressFor.count(), s.trips,
              local ? std::format("; this game holds {}", *local ? 1 : 0) : std::string{});
    return true;
}

uint64_t peer_id(const SteamNetworkingIdentity& id) {
    return id.type == kIdentityTypeSteamId ? id.steam_id : 0;
}

std::atomic<bool> g_ride_replay{true};
constexpr uint32_t kTypeRideState = 0x17;
constexpr uint32_t kTypePlayerStream = 0x1;
constexpr auto kStreamGap = std::chrono::seconds(5);
constexpr auto kFreshlyLoaded = std::chrono::seconds(20);
constexpr int kReplayDelaysSec[] = {4, 10};
void* g_iface = nullptr;

std::map<uint64_t, SteamNetworkingIdentity> g_identities;
std::array<int, 8> g_send_channel = {-1, -1, -1, -1, -1, -1, -1, -1};
std::map<uint32_t, int> g_send_flags;

struct PeerRide {
    SteamNetworkingIdentity identity{};
    std::vector<uint8_t> last_ride_packet;
    int last_ride_flags = 0;
    int last_ride_channel = 0;
    std::chrono::steady_clock::time_point last_stream_recv{};
    std::chrono::steady_clock::time_point stream_started{};
    std::chrono::steady_clock::time_point replay_at[2]{};
    uint64_t replays = 0;
};
std::map<uint64_t, PeerRide> g_ride;

void ride_schedule(PeerRide& p, std::chrono::steady_clock::time_point now) {
    for (size_t i = 0; i < std::size(kReplayDelaysSec); ++i) p.replay_at[i] = now + std::chrono::seconds(kReplayDelaysSec[i]);
}

void ride_note_stream_recv(uint64_t peer, std::chrono::steady_clock::time_point now) {
    auto& p = g_ride[peer];
    const bool restarted = p.last_stream_recv == std::chrono::steady_clock::time_point{} || now - p.last_stream_recv > kStreamGap;
    p.last_stream_recv = now;
    if (!restarted) return;
    p.stream_started = now;
    if (!p.last_ride_packet.empty()) {
        ride_schedule(p, now);
        log::info("ride replay: peer {:016X} (re)started streaming; will resend our last ride state at +{} s and +{} s",
                  peer, kReplayDelaysSec[0], kReplayDelaysSec[1]);
    }
}

void ride_note_send(uint64_t peer, const SteamNetworkingIdentity& to, const void* data, uint32_t size, int flags,
                    int channel, std::chrono::steady_clock::time_point now) {
    auto& p = g_ride[peer];
    p.identity = to;
    p.last_ride_packet.assign(static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + size);
    p.last_ride_flags = flags;
    p.last_ride_channel = channel;

    for (auto& t : p.replay_at) t = {};
    if (p.stream_started != std::chrono::steady_clock::time_point{} && now - p.stream_started < kFreshlyLoaded) {
        ride_schedule(p, now);
        log::info("ride replay: sent ride state to freshly loaded peer {:016X}; will resend at +{} s and +{} s", peer,
                  kReplayDelaysSec[0], kReplayDelaysSec[1]);
    }
}

void ride_tick(std::chrono::steady_clock::time_point now) {
    std::vector<std::pair<uint64_t, PeerRide>> due;
    {
        std::lock_guard lock(g_mutex);
        for (auto& [peer, p] : g_ride) {
            for (auto& t : p.replay_at) {
                if (t != std::chrono::steady_clock::time_point{} && now >= t) {
                    t = {};
                    due.emplace_back(peer, p);
                    break;
                }
            }
        }
    }
    for (auto& [peer, p] : due) {
        if (!g_orig_send || !g_iface || p.last_ride_packet.empty()) continue;
        const int r = g_orig_send(g_iface, &p.identity, p.last_ride_packet.data(),
                                  static_cast<uint32_t>(p.last_ride_packet.size()), p.last_ride_flags, p.last_ride_channel);
        std::lock_guard lock(g_mutex);
        g_ride[peer].replays++;
        log::info("ride replay: resent ride state ({} bytes) to peer {:016X} -> EResult {}", p.last_ride_packet.size(),
                  peer, r);
    }
}

DWORD WINAPI ride_worker(LPVOID) {
    for (;;) {
        Sleep(500);
        if (g_ride_replay) ride_tick(std::chrono::steady_clock::now());
    }
}

bool parse(const void* data, size_t size, ErscHeader& h) {
    if (!data || size < sizeof(ErscHeader)) return false;
    std::memcpy(&h, data, sizeof(h));
    return true;
}

std::atomic<uint32_t> g_hdr_seen{0}, g_hdr_bad{0};
constexpr uint32_t kHdrSample = 300;
void check_header(const ErscHeader& h) {
    const uint32_t seen = g_hdr_seen.fetch_add(1) + 1;
    if (seen > kHdrSample) return;
    const bool ok = h.channel >= 1 && h.channel <= 3 && h.compressed <= 1 && h.pad == 0 && h.type < 0x200 && h.original_size <= 0x200000;
    if (!ok) g_hdr_bad.fetch_add(1);
    if (seen == kHdrSample && g_hdr_bad.load() * 10 >= kHdrSample) {
        const std::string msg = std::format(
            "ersc-fix {}: {} of the first {} Seamless Co-op messages do not match the known message header. The mod's network "
            "format has probably changed.\n\nThe game will close. Use ersc_launcher.exe for stock Seamless Co-op until ersc-fix is updated.",
            ERSC_FIX_VERSION, g_hdr_bad.load(), kHdrSample);
        log::error("{}", msg);
        log::flush();
        MessageBoxA(nullptr, msg.c_str(), "ersc-fix", MB_OK | MB_ICONERROR | MB_TOPMOST);
        TerminateProcess(GetCurrentProcess(), 1);
    }
}

int __fastcall hk_send(void* self, const SteamNetworkingIdentity* to, const void* data, uint32_t size, int flags,
                       int channel) {
    ErscHeader h;
    bool parsed;
    {
        parsed = parse(data, size, h);
        if (parsed) check_header(h);
        if (parsed && g_storm_breaker && h.type == kTypeFlagSet && !h.compressed && size >= sizeof(h) + 5) {
            uint32_t flag;
            std::memcpy(&flag, static_cast<const uint8_t*>(data) + sizeof(h), 4);
            const uint8_t value = static_cast<const uint8_t*>(data)[sizeof(h) + 4];
            std::lock_guard lock(g_mutex);
            if (storm_check_send(flag, value, std::chrono::steady_clock::now())) return kResultOK;
        }
    }

    const int result = g_orig_send(self, to, data, size, flags, channel);
    if (parsed && to) {

        std::lock_guard lock(g_mutex);
        g_identities[peer_id(*to)] = *to;
        if (h.channel < g_send_channel.size()) g_send_channel[h.channel] = channel;
        g_send_flags[h.type] = flags;
    }
    if (parsed && g_ride_replay && h.type == kTypeRideState && h.channel == kChannelGameEvents && to) {
        std::lock_guard lock(g_mutex);
        ride_note_send(peer_id(*to), *to, data, size, flags, channel, std::chrono::steady_clock::now());
    }
    if (parsed) {

        if (h.type == kTypePlayerStream && h.channel == 2 && !h.compressed) {
            summon_replay::note_position(0, static_cast<const uint8_t*>(data) + sizeof(h), size - sizeof(h));
        }
        if (!h.compressed) {

            {
                const auto* p = static_cast<const uint8_t*>(data) + sizeof(h);
                const size_t n = size - sizeof(h);
                if (h.channel == 1 && h.type == 0x19 && n == 1 && p[0] == 1) flag_probe::note_own_reload("mod 0x19");
                else if (h.channel == kChannelGameEvents && h.type == 0x0A && n == 1) flag_probe::note_own_reload("game 0x0A");
                else if (h.channel == kChannelGameEvents && (h.type == 0x42 || h.type == 0x35) && n == 8) {
                    uint32_t a, b;
                    std::memcpy(&a, p, 4);
                    std::memcpy(&b, p + 4, 4);
                    if (h.type == 0x42) flag_probe::note_own_block_request(b, "0x42 block-data request");
                    else if (b == 1) flag_probe::note_own_block_request(a, "0x35 block loaded");
                }
            }
        }
    }
    return result;
}

int __fastcall hk_recv(void* self, int local_channel, SteamNetworkingMessage** out, int max) {
    int n = g_orig_recv(self, local_channel, out, max);
    for (int i = 0; i < n && out; ++i) {
        const SteamNetworkingMessage* m = out[i];
        if (!m) continue;
        ErscHeader h;
        if (parse(m->data, static_cast<size_t>(m->size), h)) {
            check_header(h);
            {
                std::lock_guard lock(g_mutex);
                g_identities[peer_id(m->peer)] = m->peer;
            }
            const auto* payload = static_cast<const uint8_t*>(m->data) + sizeof(h);
            const size_t payload_len = static_cast<size_t>(m->size) - sizeof(h);

            if (h.channel == 1 && h.type == flag_probe::kFlagBitmapType && !h.compressed) {
                flag_probe::note_peer_flags(peer_id(m->peer), payload, payload_len);
                if (m->release_fn) reinterpret_cast<void (*)(SteamNetworkingMessage*)>(m->release_fn)(const_cast<SteamNetworkingMessage*>(m));
                for (int k = i; k + 1 < n; ++k) out[k] = out[k + 1];
                out[--n] = nullptr;
                --i;
                continue;
            }

            if (h.channel == 1 && h.type == enemy_probe::kDisabledSetType && !h.compressed) {
                enemy_probe::note_disabled_set(peer_id(m->peer), payload, payload_len);
                if (m->release_fn) reinterpret_cast<void (*)(SteamNetworkingMessage*)>(m->release_fn)(const_cast<SteamNetworkingMessage*>(m));
                for (int k = i; k + 1 < n; ++k) out[k] = out[k + 1];
                out[--n] = nullptr;
                --i;
                continue;
            }
            if (h.channel == 1 && h.type == 0x0C && !h.compressed && payload_len == 1) {
                std::string why;
                const bool drop = mount_probe::should_drop_horse_reset(peer_id(m->peer), payload[0], why);
                log::info("steam: recv 0x0C ch1 horse type {:02X} from {:016X}: {} ({})", payload[0], peer_id(m->peer),
                          drop ? "DROPPED (redundant reload while the partner rides)" : "passed", why);
                if (drop) {
                    if (m->release_fn) reinterpret_cast<void (*)(SteamNetworkingMessage*)>(m->release_fn)(const_cast<SteamNetworkingMessage*>(m));
                    for (int k = i; k + 1 < n; ++k) out[k] = out[k + 1];
                    out[--n] = nullptr;
                    --i;
                    continue;
                }
            }
            if (h.type == kTypeFlagSet && !h.compressed && payload_len >= 5) {
                uint32_t flag;
                std::memcpy(&flag, payload, 4);
                std::lock_guard lock(g_mutex);
                storm_note_recv(flag, payload[4], std::chrono::steady_clock::now());
            }

            if (!h.compressed) {
                if (h.channel == kChannelGameEvents && h.type == 0x12) mount_replay::note_ride_record(peer_id(m->peer), payload, payload_len);
                else if (h.channel == kChannelGameEvents && h.type == 0x34) mount_replay::note_block_notice(peer_id(m->peer), payload, payload_len);
                else if (h.channel == kChannelGameEvents && h.type == 0x46) mount_replay::note_status(peer_id(m->peer), payload, payload_len);
                else if (h.channel == 2 && h.type == 0x1) mount_replay::note_stream(peer_id(m->peer), payload, payload_len);
            }
            if (g_ride_replay && h.type == kTypePlayerStream) {
                std::lock_guard lock(g_mutex);
                ride_note_stream_recv(peer_id(m->peer), std::chrono::steady_clock::now());
            }
            inject::note_channel(h.channel, local_channel);

            if (!h.compressed) {
                if (h.channel == kChannelGameEvents && h.type == 0x0A && payload_len == 1) flag_probe::note_peer_reload(peer_id(m->peer), "game 0x0A");
                else if (h.channel == 1 && h.type == 0x19 && payload_len == 1 && payload[0] == 1) flag_probe::note_peer_reload(peer_id(m->peer), "mod 0x19");
                else if (h.channel == kChannelGameEvents && (h.type == 0x42 || h.type == 0x35) && payload_len == 8) {
                    uint32_t a, b;
                    std::memcpy(&a, payload, 4);
                    std::memcpy(&b, payload + 4, 4);
                    if (h.type == 0x42) flag_probe::note_peer_block_request(peer_id(m->peer), b, "0x42 block-data request");
                    else if (b == 1) flag_probe::note_peer_block_request(peer_id(m->peer), a, "0x35 block loaded");
                }
            }

            if (h.type == 0x1 && h.channel == 2 && !h.compressed) {
                summon_replay::note_position(peer_id(m->peer), payload, payload_len);
                away_relocate::rewrite(peer_id(m->peer), const_cast<uint8_t*>(payload), payload_len);
            }

            if (!h.compressed) {
                if (h.type == 0x3 && h.channel == 2) summon_replay::note_owner_state(peer_id(m->peer), payload, payload_len);
                else if (h.type == 0x46 && h.channel == kChannelGameEvents) summon_replay::note_chr_status(peer_id(m->peer), payload, payload_len);

                if (h.channel == kChannelGameEvents) summon_replay::note_game_packet(peer_id(m->peer), h.type, payload, payload_len);
            }

            if (h.channel == 1 && !h.compressed) {
                if (h.type == 0x15) summon_replay::note_spawn(peer_id(m->peer), payload, payload_len);
                else if (h.type == 0x16) summon_replay::note_despawn(peer_id(m->peer), payload, payload_len);
            }
        }
        inject::note_peer(m->peer);
    }

    if (out && n >= 0 && n < max) n += inject::inject(local_channel, out + n, max - n);
    return n;
}

}

bool send_raw(uint64_t peer, uint8_t header_channel, uint32_t type, const void* payload, size_t len) {
    SteamNetworkingIdentity to{};
    int channel = -1, flags = 0;
    {
        std::lock_guard lock(g_mutex);
        auto it = g_identities.find(peer);
        if (it == g_identities.end()) return false;
        to = it->second;
        if (header_channel < g_send_channel.size()) channel = g_send_channel[header_channel];
        auto f = g_send_flags.find(type);
        flags = f != g_send_flags.end() ? f->second : 9;
    }
    if (!g_orig_send || !g_iface || channel < 0) return false;
    std::vector<uint8_t> msg(sizeof(ErscHeader) + len);
    ErscHeader h{};
    h.channel = header_channel;
    h.compressed = 0;
    h.pad = 0;
    h.type = type;
    h.original_size = static_cast<uint32_t>(len);
    std::memcpy(msg.data(), &h, sizeof(h));
    std::memcpy(msg.data() + sizeof(h), payload, len);
    const int r = g_orig_send(g_iface, &to, msg.data(), static_cast<uint32_t>(msg.size()), flags, channel);
    if (r != kResultOK) log::warn("steam: send_raw type 0x{:X} ({} B) to {:016X} on channel {} -> EResult {}", type, len, peer, channel, r);
    return r == kResultOK;
}

namespace {

bool swap_slot(void** vtable, size_t index, void* replacement, void** original_out) {
    DWORD old;
    if (!VirtualProtect(&vtable[index], sizeof(void*), PAGE_READWRITE, &old)) return false;
    *original_out = vtable[index];
    vtable[index] = replacement;
    VirtualProtect(&vtable[index], sizeof(void*), old, &old);
    return true;
}

DWORD WINAPI install_thread(LPVOID) {
    using GetUserFn = int32_t(__cdecl*)();
    using FindFn = void*(__cdecl*)(int32_t user, const char* version);

    HMODULE steam = nullptr;
    GetUserFn get_user = nullptr;
    FindFn find = nullptr;
    void* iface = nullptr;
    for (int attempt = 0; attempt < 120 && !iface; ++attempt) {
        if (!steam) steam = GetModuleHandleW(L"steam_api64.dll");
        if (steam && !get_user) {
            get_user = reinterpret_cast<GetUserFn>(GetProcAddress(steam, "SteamAPI_GetHSteamUser"));
            find = reinterpret_cast<FindFn>(GetProcAddress(steam, "SteamInternal_FindOrCreateUserInterface"));
        }
        if (get_user && find) {
            if (int32_t user = get_user()) iface = find(user, "SteamNetworkingMessages002");
        }
        if (!iface) Sleep(1000);
    }
    if (!iface) {
        log::warn("steam_log: ISteamNetworkingMessages not available after 120 s; not installed");
        return 0;
    }

    void** vtable = *reinterpret_cast<void***>(iface);
    void* orig_send = nullptr;
    void* orig_recv = nullptr;
    if (!swap_slot(vtable, 0, reinterpret_cast<void*>(&hk_send), &orig_send) ||
        !swap_slot(vtable, 1, reinterpret_cast<void*>(&hk_recv), &orig_recv)) {
        log::error("steam_log: could not make the ISteamNetworkingMessages vtable writable");
        return 0;
    }
    g_orig_send = reinterpret_cast<SendFn>(orig_send);
    g_orig_recv = reinterpret_cast<RecvFn>(orig_recv);
    g_iface = iface;
    log::info("steam_log: hooked ISteamNetworkingMessages002 at {} (send {}, recv {})", iface, orig_send, orig_recv);
    if (g_ride_replay) {
        if (HANDLE t = CreateThread(nullptr, 0, ride_worker, nullptr, 0, nullptr)) CloseHandle(t);
        log::info("ride replay: enabled (resends our last 0x17 ride state to peers that just loaded)");
    }
    return 0;
}

}

void install_async(bool storm_breaker, bool ride_replay) {
    g_storm_breaker = storm_breaker;
    g_ride_replay = ride_replay;
    if (HANDLE t = CreateThread(nullptr, 0, install_thread, nullptr, 0, nullptr)) CloseHandle(t);
}

}
