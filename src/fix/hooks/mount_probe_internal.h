#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <string>

namespace ersc_fix::hooks::mount_probe::detail {

struct Rider {
    uint32_t h0 = 0, h1 = 0;
    uint32_t req_state = 0;
    uint64_t req_handle = 0;
    uint32_t node_state = 0;
    uint32_t load = 0;
    uint8_t entry = 0;
    float pos[3]{};
    bool operator==(const Rider& o) const {
        return req_state == o.req_state && req_handle == o.req_handle && node_state == o.node_state && load == o.load && entry == o.entry;
    }
    bool riding() const;
    bool riding_by_node() const;
};

struct Horse {
    uint32_t h0 = 0, h1 = 0;
    uint32_t load = 0, block = 0, node_state = 0;
    bool model = false;
    uint8_t entry = 0;
    uint32_t activation = 0;
    float alpha = 0;
    float pos[3]{};
    bool operator==(const Horse& o) const {
        return load == o.load && block == o.block && node_state == o.node_state && model == o.model && activation == o.activation && entry == o.entry &&
               (alpha > 0.0f) == (o.alpha > 0.0f);
    }
    uint64_t handle() const { return static_cast<uint64_t>(h0) | (static_cast<uint64_t>(h1) << 32); }
};

extern uintptr_t g_game_base;

extern std::atomic<int> g_dropped, g_passed;

void sample_position(uintptr_t chr, float* pos);
bool sample_rider(uintptr_t chr, Rider& r);
bool sample_horse(uintptr_t chr, Horse& h);
void collect(uintptr_t wcm, uintptr_t me, std::map<uintptr_t, Rider>& riders, std::map<uintptr_t, Horse>* horses);
std::string describe(const Rider& r);
std::string describe(const Horse& h);

}
