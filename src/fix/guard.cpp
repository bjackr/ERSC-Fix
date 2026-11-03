#include "guard.h"

#include "common/sha256.h"

#include <format>
#include <fstream>
#include <vector>

namespace ersc_fix::guard {

HMODULE wait_for_module(const wchar_t* name, DWORD timeout_ms) {
    const ULONGLONG deadline = GetTickCount64() + timeout_ms;
    for (;;) {
        if (HMODULE module = GetModuleHandleW(name)) return module;
        if (GetTickCount64() >= deadline) return nullptr;
        Sleep(250);
    }
}

std::filesystem::path module_path(HMODULE module) {
    wchar_t buffer[MAX_PATH * 2];
    DWORD length = GetModuleFileNameW(module, buffer, static_cast<DWORD>(std::size(buffer)));
    return std::filesystem::path(std::wstring_view(buffer, length));
}

std::string sha256_file(const std::filesystem::path& path) {
    return common::sha256_file(path);
}

const KnownBuild* find_known_build(std::string_view sha256) {
    for (const auto& build : kKnownErscBuilds) {
        if (build.sha256 == sha256) return &build;
    }
    return nullptr;
}

std::string describe_address(const void* address) {
    HMODULE module = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCWSTR>(address), &module)) {
        auto offset = reinterpret_cast<uintptr_t>(address) - reinterpret_cast<uintptr_t>(module);
        return std::format("{}+0x{:X}", module_path(module).filename().string(), offset);
    }
    return std::format("0x{:X}", reinterpret_cast<uintptr_t>(address));
}

}
