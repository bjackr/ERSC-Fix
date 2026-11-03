#pragma once

#include <cstdint>
#include <filesystem>
#include <format>
#include <string_view>

namespace ersc_fix::log {

void init(const std::filesystem::path& dir);

const std::filesystem::path& directory();

void write(std::string_view level, std::string_view message);

void flush();

struct Stats {
    uint64_t lines = 0;
    uint64_t bytes = 0;
    uint64_t dropped = 0;
};
Stats stats();

template <class... Args>
void info(std::format_string<Args...> fmt, Args&&... args) {
    write("INFO", std::format(fmt, std::forward<Args>(args)...));
}

template <class... Args>
void warn(std::format_string<Args...> fmt, Args&&... args) {
    write("WARN", std::format(fmt, std::forward<Args>(args)...));
}

template <class... Args>
void error(std::format_string<Args...> fmt, Args&&... args) {
    write("ERROR", std::format(fmt, std::forward<Args>(args)...));
}

}
