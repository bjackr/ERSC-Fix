#include "log.h"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

namespace ersc_fix::log {

namespace {

std::mutex g_mutex;
std::mutex g_file_mutex;
FILE* g_file = nullptr;
std::vector<std::string> g_queue;
HANDLE g_wake = nullptr;
std::atomic<uint64_t> g_lines{0}, g_bytes{0}, g_dropped{0};
constexpr size_t kMaxQueued = 20000;
std::filesystem::path g_dir;

void write_out(std::vector<std::string>& lines) {
    std::lock_guard file_lock(g_file_mutex);
    if (!g_file) return;
    for (auto& l : lines) std::fwrite(l.data(), 1, l.size(), g_file);
    std::fflush(g_file);
}

DWORD WINAPI writer_thread(LPVOID) {
    std::vector<std::string> batch;
    for (;;) {
        WaitForSingleObject(g_wake, 250);
        {
            std::lock_guard lock(g_mutex);
            if (g_queue.empty()) continue;
            batch.swap(g_queue);
        }
        write_out(batch);
        batch.clear();
    }
}

}

void init(const std::filesystem::path& dir) {
    std::lock_guard file_lock(g_file_mutex);
    if (g_file) return;

    g_dir = dir;
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);

    SYSTEMTIME t;
    GetLocalTime(&t);
    auto name = std::format(L"ersc_fix_{:04}{:02}{:02}_{:02}{:02}{:02}.log", t.wYear, t.wMonth, t.wDay,
                            t.wHour, t.wMinute, t.wSecond);
    g_file = _wfsopen((dir / name).c_str(), L"w", _SH_DENYWR);
    g_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (HANDLE th = CreateThread(nullptr, 0, writer_thread, nullptr, 0, nullptr)) CloseHandle(th);
}

const std::filesystem::path& directory() {
    return g_dir;
}

void write(std::string_view level, std::string_view message) {
    SYSTEMTIME t;
    GetLocalTime(&t);
    std::string line = std::format("{:02}:{:02}:{:02}.{:03} [{:5}] {:<5} {}\n", t.wHour, t.wMinute, t.wSecond,
                                   t.wMilliseconds, GetCurrentThreadId(), level, message);
    {
        std::lock_guard lock(g_mutex);
        if (g_queue.size() >= kMaxQueued) {
            g_dropped++;
            return;
        }
        g_bytes += line.size();
        g_lines++;
        g_queue.push_back(std::move(line));
    }

}

void flush() {
    std::vector<std::string> batch;
    {
        std::lock_guard lock(g_mutex);
        batch.swap(g_queue);
    }
    write_out(batch);
}

Stats stats() {
    return {g_lines.load(), g_bytes.load(), g_dropped.load()};
}

}
