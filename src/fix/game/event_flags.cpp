#include "game/event_flags.h"

#include "game/offsets.h"
#include "log.h"

#include <windows.h>

#include <atomic>

namespace ersc_fix::game::event_flags {

namespace {

uintptr_t g_base = 0;
std::atomic<uint64_t> g_faults{0};

using GetFlagFn = bool(__fastcall*)(void* manager, uint32_t flag_id);

int call_getter(GetFlagFn fn, void* manager, uint32_t flag_id, bool* out) {
    __try {
        *out = fn(manager, flag_id);
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

using SetFlagFn = void(__fastcall*)(void* manager, int* flag_id, char value, void* source, char queue_relay);
int call_setter(SetFlagFn fn, void* manager, int flag_id, char value) {
    __try {
        fn(manager, &flag_id, value, nullptr, 0);
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}
}

bool can_set() { return available() && kSetEventFlag != 0; }

bool set(uint32_t flag_id, bool value) {
    if (!can_set()) return false;
    MEMORY_BASIC_INFORMATION mbi;
    uintptr_t slot = g_base + kCsEventFlagManPtr;
    if (!VirtualQuery(reinterpret_cast<void*>(slot), &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) return false;
    void* manager = *reinterpret_cast<void**>(slot);
    if (!manager) return false;
    if (!call_setter(reinterpret_cast<SetFlagFn>(g_base + kSetEventFlag), manager, static_cast<int>(flag_id), value ? 1 : 0)) {
        log::warn("event flags: setter faulted for flag {}", flag_id);
        return false;
    }
    return true;
}

void init(uintptr_t game_base) {
    g_base = game_base;
    if (!available()) {
        log::info("event flags: reader disabled (CSEventFlagMan / GetEventFlag RVAs not set for this game build)");
    }
}

bool available() {
    return g_base && kCsEventFlagManPtr && kGetEventFlag;
}

std::optional<bool> get(uint32_t flag_id) {
    if (!available()) return std::nullopt;
    MEMORY_BASIC_INFORMATION mbi;
    uintptr_t slot = g_base + kCsEventFlagManPtr;
    if (!VirtualQuery(reinterpret_cast<void*>(slot), &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) return std::nullopt;
    void* manager = *reinterpret_cast<void**>(slot);
    if (!manager) return std::nullopt;

    bool value = false;
    if (!call_getter(reinterpret_cast<GetFlagFn>(g_base + kGetEventFlag), manager, flag_id, &value)) {
        if (++g_faults <= 3) log::warn("event flags: getter faulted for flag {} (fault #{})", flag_id, g_faults.load());
        return std::nullopt;
    }
    return value;
}

}
