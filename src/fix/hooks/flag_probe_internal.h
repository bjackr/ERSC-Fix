#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <vector>

namespace ersc_fix::hooks::flag_probe::detail {

using clock = std::chrono::steady_clock;
constexpr uint32_t kFlagsPerBlock = 10000;

struct Snapshot {
    uint32_t base = 0;
    uint32_t generation = 0;
    std::vector<uint8_t> bits;
    std::map<uint32_t, bool> extra;
    clock::time_point at{};
};

inline bool bit(const std::vector<uint8_t>& bits, uint32_t i) { return i / 8 < bits.size() && (bits[i / 8] >> (i % 8)) & 1; }

extern std::atomic<uint32_t> g_generation;

}
