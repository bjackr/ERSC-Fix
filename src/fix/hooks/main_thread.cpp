#include "hooks/main_thread.h"

#include "log.h"

#include <windows.h>

#include <atomic>
#include <deque>
#include <mutex>

namespace ersc_fix::hooks::main_thread {

namespace {

std::atomic<uint32_t> g_thread_id{0};
std::atomic<HHOOK> g_hook{nullptr};
std::mutex g_mutex;
std::deque<std::function<void()>> g_queue;
std::atomic<long> g_pumps{0};
std::atomic<long> g_ran{0};

std::atomic<long long> g_last_pump_qpc{0};
std::atomic<long long> g_max_gap_us{0};
std::atomic<long> g_gaps_50{0}, g_gaps_100{0};
long long g_qpc_freq = 1;

struct FindWindow {
    DWORD pid;
    HWND hwnd = nullptr;
    DWORD thread = 0;
};

BOOL CALLBACK enum_windows(HWND hwnd, LPARAM lp) {
    auto* f = reinterpret_cast<FindWindow*>(lp);
    DWORD pid = 0;
    const DWORD thread = GetWindowThreadProcessId(hwnd, &pid);
    if (pid != f->pid || !IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER) != nullptr) return TRUE;
    f->hwnd = hwnd;
    f->thread = thread;
    return FALSE;
}

bool run_guarded(std::function<void()>& task) {
    __try {
        task();
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

LRESULT CALLBACK get_message_proc(int code, WPARAM wparam, LPARAM lparam) {
    if (code == HC_ACTION) {
        ++g_pumps;
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        const long long prev = g_last_pump_qpc.exchange(now.QuadPart);
        if (prev) {
            const long long gap_us = (now.QuadPart - prev) * 1000000 / g_qpc_freq;
            if (gap_us > g_max_gap_us.load()) g_max_gap_us.store(gap_us);
            if (gap_us > 100000) ++g_gaps_100;
            else if (gap_us > 50000) ++g_gaps_50;
        }
        for (;;) {
            std::function<void()> task;
            {
                std::lock_guard lock(g_mutex);
                if (g_queue.empty()) break;
                task = std::move(g_queue.front());
                g_queue.pop_front();
            }
            if (run_guarded(task)) ++g_ran;
            else log::warn("main thread: a queued task raised an exception; dropped");
        }
    }
    return CallNextHookEx(g_hook.load(), code, wparam, lparam);
}

DWORD WINAPI install_thread(LPVOID) {

    FindWindow f{GetCurrentProcessId()};
    for (int i = 0; i < 600 && !f.hwnd; ++i) {
        EnumWindows(enum_windows, reinterpret_cast<LPARAM>(&f));
        if (!f.hwnd) Sleep(500);
    }
    if (!f.hwnd) {
        log::warn("main thread: no visible top-level window found in 5 min; game-thread tasks unavailable");
        return 0;
    }
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&get_message_proc), &self);
    HHOOK h = SetWindowsHookExW(WH_GETMESSAGE, get_message_proc, self, f.thread);
    if (!h) {
        log::warn("main thread: SetWindowsHookEx failed ({}); game-thread tasks unavailable", GetLastError());
        return 0;
    }
    g_hook.store(h);
    g_thread_id.store(f.thread);
    log::info("main thread: message hook installed on the game window's thread {} (window {})", f.thread, static_cast<void*>(f.hwnd));

    for (;;) {
        Sleep(60000);
        log::info("main thread: {} message pumps, {} tasks run; longest gap between pumps {:.1f} ms, {} gap(s) over 50 ms, {} over 100 ms", g_pumps.exchange(0),
                  g_ran.load(), g_max_gap_us.exchange(0) / 1000.0, g_gaps_50.exchange(0), g_gaps_100.exchange(0));
    }
}

}

void start(const Context&) {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g_qpc_freq = f.QuadPart ? f.QuadPart : 1;
    if (HANDLE t = CreateThread(nullptr, 0, install_thread, nullptr, 0, nullptr)) CloseHandle(t);
}

bool post(std::function<void()> task) {
    if (!g_hook.load()) return false;
    std::lock_guard lock(g_mutex);
    g_queue.push_back(std::move(task));
    return true;
}

uint32_t thread_id() { return g_thread_id.load(); }

}
