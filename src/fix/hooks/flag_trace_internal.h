#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace ersc_fix::hooks::flag_trace::detail {

constexpr uint32_t kInfeMagic = 0x45464E49;
constexpr size_t kInfeHeader = 16;

struct Entry {
    uint32_t slot = 0;
    uint8_t* bytes = nullptr;
};
struct Infe {
    uint16_t version = 0, flags = 0;
    uint32_t count = 0, bpb = 0;
    std::vector<Entry> entries;
    std::string problem;
};

extern uintptr_t g_game_base;
extern const std::atomic<uint32_t>* g_state;
extern bool g_pack_fix;

int read_buffer(const uint8_t* p, size_t n, uint8_t* out);
int write_buffer(uint8_t* p, const uint8_t* src, size_t n);

Infe parse(uint8_t* p, size_t size);

int legacy_slot(uintptr_t mgr, uint32_t key);

std::map<int, uint32_t> slot_map(uintptr_t mgr, const std::vector<uint32_t>& keys);
inline bool get_bit(const uint8_t* b, uint32_t i) { return (b[i >> 3] >> (7 - (i & 7))) & 1; }

}
