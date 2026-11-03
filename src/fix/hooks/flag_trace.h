#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <map>
#include <vector>

namespace ersc_fix::hooks::flag_trace {

void start(uintptr_t game_base, const std::atomic<uint32_t>* session_state, bool pack_fix);

std::vector<uint32_t> keys_of(const std::vector<uint32_t>& range_bases);

using Snap = std::map<uint32_t, std::vector<uint8_t>>;
Snap snapshot(const std::vector<uint32_t>& keys);

void log_diff(const char* writer, const Snap& before, const std::vector<uint32_t>& keys, bool quiet_when_unchanged = false);

void log_incoming(const char* what, const uint8_t* data, size_t size, const std::vector<uint32_t>& keys);

void log_storage(const char* why, const std::vector<uint32_t>& keys);

}
