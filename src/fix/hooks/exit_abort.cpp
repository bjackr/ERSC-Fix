#include "hooks/exit_abort.h"

#include "log.h"

#include <atomic>

namespace ersc_fix::hooks::exit_abort {

namespace {

struct MsvcThread {
    HANDLE handle;
    DWORD id;
};

std::atomic<MsvcThread*> g_thread{nullptr};

}

void arm(const Context& context) {
    auto* thread = reinterpret_cast<MsvcThread*>(reinterpret_cast<char*>(context.ersc) +
                                                  context.build->offsets.init_thread);

    if (!thread->handle || GetThreadId(thread->handle) != thread->id) {
        log::warn("exit_abort: init thread at ersc+0x{:X} doesn't look like a std::thread "
                  "(handle {}, id {}); fix disabled",
                  context.build->offsets.init_thread, thread->handle, thread->id);
        return;
    }
    g_thread = thread;
    log::info("exit_abort: armed (ersc init thread id {})", thread->id);
}

void on_process_exit() {
    MsvcThread* thread = g_thread.exchange(nullptr);
    if (!thread || thread->id == 0) return;

    thread->handle = nullptr;
    thread->id = 0;
    log::info("exit_abort: detached ersc init thread before its destructor ran");
    log::flush();
}

}
