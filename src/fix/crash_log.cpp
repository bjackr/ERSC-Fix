#include "crash_log.h"

#include "guard.h"
#include "log.h"

#include <windows.h>

#include <atomic>

namespace ersc_fix::crash_log {

namespace {

constexpr DWORD kStatusFatalAppExit = 0x40000015;
constexpr DWORD kStatusHeapCorruption = 0xC0000374;
constexpr int kMaxReports = 32;

std::atomic<int> g_reports{0};
thread_local bool t_in_handler = false;

bool is_interesting(DWORD code) {
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION:
        case EXCEPTION_STACK_OVERFLOW:
        case EXCEPTION_ILLEGAL_INSTRUCTION:
        case STATUS_STACK_BUFFER_OVERRUN:
        case kStatusHeapCorruption:
        case kStatusFatalAppExit:
            return true;
        default:
            return false;
    }
}

bool readable(const void* address, size_t size) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(address, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    auto end = static_cast<const char*>(mbi.BaseAddress) + mbi.RegionSize;
    return static_cast<const char*>(address) + size <= end;
}

bool in_module(DWORD64 address) {
    HMODULE module = nullptr;
    return GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                              reinterpret_cast<LPCWSTR>(address), &module) != 0;
}

void log_unwound_stack(CONTEXT context) {
    for (int frame = 0; frame < 32 && context.Rip; ++frame) {
        log::error("  #{:02} {}", frame, guard::describe_address(reinterpret_cast<void*>(context.Rip)));

        DWORD64 image_base = 0;
        auto* function = RtlLookupFunctionEntry(context.Rip, &image_base, nullptr);
        if (!function) {

            if (!readable(reinterpret_cast<void*>(context.Rsp), sizeof(DWORD64))) break;
            context.Rip = *reinterpret_cast<DWORD64*>(context.Rsp);
            context.Rsp += sizeof(DWORD64);
            continue;
        }
        void* handler_data = nullptr;
        DWORD64 establisher_frame = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, context.Rip, function, &context, &handler_data,
                         &establisher_frame, nullptr);
    }
}

void log_stack_scan(DWORD64 rsp) {
    constexpr int kSlots = 1024;
    constexpr int kMaxHits = 48;
    auto* slot = reinterpret_cast<const DWORD64*>(rsp);
    int hits = 0;
    for (int i = 0; i < kSlots && hits < kMaxHits; ++i, ++slot) {
        if (!readable(slot, sizeof(DWORD64))) break;
        if (*slot > 0x10000 && in_module(*slot)) {
            log::error("  rsp+0x{:04X}: {}", i * 8, guard::describe_address(reinterpret_cast<void*>(*slot)));
            ++hits;
        }
    }
}

LONG CALLBACK on_exception(EXCEPTION_POINTERS* info) {
    const auto* record = info->ExceptionRecord;
    if (t_in_handler || !is_interesting(record->ExceptionCode)) return EXCEPTION_CONTINUE_SEARCH;
    if (g_reports.fetch_add(1) >= kMaxReports) return EXCEPTION_CONTINUE_SEARCH;

    t_in_handler = true;
    log::error("exception 0x{:08X} at {} (thread {})", record->ExceptionCode,
               guard::describe_address(record->ExceptionAddress), GetCurrentThreadId());
    if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2) {
        static constexpr const char* kOps[] = {"read", "write", "?", "?", "?", "?", "?", "?", "execute"};
        auto op = record->ExceptionInformation[0];
        log::error("  {} of 0x{:X}", op < std::size(kOps) ? kOps[op] : "?", record->ExceptionInformation[1]);
    }
    log::error(" unwound stack:");
    log_unwound_stack(*info->ContextRecord);
    log::error(" stack scan:");
    log_stack_scan(info->ContextRecord->Rsp);
    log::flush();
    t_in_handler = false;

    return EXCEPTION_CONTINUE_SEARCH;
}

}

void install() {
    AddVectoredExceptionHandler(1, on_exception);
}

}
