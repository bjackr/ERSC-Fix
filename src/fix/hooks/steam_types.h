#pragma once

#include <cstddef>
#include <cstdint>

namespace ersc_fix::hooks {

constexpr int kIdentityTypeSteamId = 16;

#pragma pack(push, 1)
struct SteamNetworkingIdentity {
    int32_t type;
    int32_t size;
    uint64_t steam_id;
    uint8_t rest[120];
};
#pragma pack(pop)
static_assert(sizeof(SteamNetworkingIdentity) == 136);

struct SteamNetworkingMessage {
    void* data;
    int32_t size;
    uint32_t conn;
    SteamNetworkingIdentity peer;
    int64_t conn_user_data;
    int64_t usec_received;
    int64_t message_number;
    void* free_fn;
    void* release_fn;
    int32_t channel;
    int32_t flags;
    int64_t user_data;
    uint16_t lane;
    uint16_t pad;
};
static_assert(offsetof(SteamNetworkingMessage, peer) == 16);
static_assert(offsetof(SteamNetworkingMessage, channel) == 192);

#pragma pack(push, 1)
struct ErscHeader {
    uint8_t channel;
    uint8_t compressed;
    uint8_t pad;
    uint32_t type;
    uint32_t original_size;
};
#pragma pack(pop)
static_assert(sizeof(ErscHeader) == 11);

constexpr uint8_t kChannelGameEvents = 3;

}
