#pragma once

#include <cstdint>
#include <string_view>

namespace ersc_fix {

struct ErscOffsets {

    uint32_t init_thread = 0;

    uint32_t net_send = 0;

    uint32_t net_send_status = 0;

    uint32_t event_flag_set = 0;

    uint32_t game_packet_tunnel = 0;
};

struct KnownBuild {
    std::string_view sha256;
    std::string_view label;
    ErscOffsets offsets;
};

inline constexpr KnownBuild kKnownErscBuilds[] = {
    {"fcd11a1849fd37851bb449be30f1525cf498f63fd1fa568c4e15030ebe7cb251", "Seamless Co-op v2.0.1 (ersc.dll dated 2026-10-10)",
     {.init_thread = 0x222010,
      .net_send = 0xA7690,
      .net_send_status = 0xA7E90,
      .event_flag_set = 0x214E0,
      .game_packet_tunnel = 0x8B5E0}},

    {"0a093d8490967bf19c62f895fdf1fda21840282d2b2b371b9b4fc24a0cfe146e", "Seamless Co-op v2.0.2 (ersc.dll dated 2026-10-27)",
     {.init_thread = 0x224010,
      .net_send = 0xA8C00,
      .net_send_status = 0xA9400,
      .event_flag_set = 0x21500,
      .game_packet_tunnel = 0x8C6F0}},
};

}
