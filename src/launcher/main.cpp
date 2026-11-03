#include "common/game_builds.h"
#include "common/sha256.h"
#include "fix/known_builds.h"

#include <windows.h>
#include <shellapi.h>
#include <tlhelp32.h>

#include <cstdio>
#include <filesystem>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr DWORD kWaitForGameMs = 5 * 60 * 1000;
constexpr DWORD kPollMs = 1000;

fs::path exe_dir() {
    wchar_t buffer[MAX_PATH * 2];
    DWORD length = GetModuleFileNameW(nullptr, buffer, static_cast<DWORD>(std::size(buffer)));
    return fs::path(std::wstring_view(buffer, length)).parent_path();
}

std::vector<DWORD> find_processes(std::wstring_view name) {
    std::vector<DWORD> pids;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return pids;
    PROCESSENTRY32W entry{.dwSize = sizeof(entry)};
    for (BOOL ok = Process32FirstW(snapshot, &entry); ok; ok = Process32NextW(snapshot, &entry)) {
        if (_wcsicmp(entry.szExeFile, name.data()) == 0) pids.push_back(entry.th32ProcessID);
    }
    CloseHandle(snapshot);
    return pids;
}

bool has_module(DWORD pid, std::wstring_view module) {
    HANDLE snapshot;
    do {
        snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid);
    } while (snapshot == INVALID_HANDLE_VALUE && GetLastError() == ERROR_BAD_LENGTH);
    if (snapshot == INVALID_HANDLE_VALUE) return false;

    bool found = false;
    MODULEENTRY32W entry{.dwSize = sizeof(entry)};
    for (BOOL ok = Module32FirstW(snapshot, &entry); ok && !found; ok = Module32NextW(snapshot, &entry)) {
        found = _wcsicmp(entry.szModule, module.data()) == 0;
    }
    CloseHandle(snapshot);
    return found;
}

bool inject(DWORD pid, const fs::path& dll) {
    HANDLE process = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION |
                                     PROCESS_VM_WRITE | PROCESS_VM_READ,
                                 FALSE, pid);
    if (!process) {
        std::fwprintf(stderr, L"OpenProcess(%lu) failed: %lu", pid, GetLastError());
        if (GetLastError() == ERROR_ACCESS_DENIED) {
            std::fwprintf(stderr, L" - the game runs elevated; run this launcher as administrator too");
        }
        std::fwprintf(stderr, L"\n");
        return false;
    }

    const std::wstring path = dll.wstring();
    const SIZE_T bytes = (path.size() + 1) * sizeof(wchar_t);
    void* remote = VirtualAllocEx(process, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote) std::fwprintf(stderr, L"VirtualAllocEx failed: %lu\n", GetLastError());
    bool ok = remote && WriteProcessMemory(process, remote, path.c_str(), bytes, nullptr);
    if (remote && !ok) std::fwprintf(stderr, L"WriteProcessMemory failed: %lu\n", GetLastError());

    if (ok) {
        auto load_library = reinterpret_cast<LPTHREAD_START_ROUTINE>(
            GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW"));
        HANDLE thread = CreateRemoteThread(process, nullptr, 0, load_library, remote, 0, nullptr);
        ok = thread != nullptr;
        if (!thread) std::fwprintf(stderr, L"CreateRemoteThread failed: %lu\n", GetLastError());
        if (thread) {
            const DWORD wait = WaitForSingleObject(thread, 15'000);
            DWORD module_low_bits = 0;
            GetExitCodeThread(thread, &module_low_bits);
            CloseHandle(thread);

            ok = module_low_bits != 0 || has_module(pid, L"ersc_fix.dll");
            if (!ok) {
                if (wait == WAIT_TIMEOUT) {
                    std::fwprintf(stderr, L"the game did not finish loading the DLL within 15 s\n");
                } else {
                    std::fwprintf(stderr,
                                  L"the game could not load %ls.\n"
                                  L"Usual causes: Windows Smart App Control or SmartScreen blocking an unsigned DLL\n"
                                  L"(right-click the zip -> Properties -> Unblock before extracting, or allow it in\n"
                                  L"Windows Security), or an antivirus quarantining it. Check that the file still exists.\n",
                                  path.c_str());
                }
            }
        }
    }

    if (remote) VirtualFreeEx(process, remote, 0, MEM_RELEASE);
    CloseHandle(process);
    return ok;
}

int fail(const wchar_t* message) {
    std::fwprintf(stderr, L"error: %s\n\nPress Enter to close.", message);
    std::getwchar();
    return 1;
}

}

int wmain(int argc, wchar_t** argv) {
    const fs::path fix_dir = exe_dir();
    fs::path game_dir = fix_dir.parent_path().parent_path();
    bool attach_only = false;

    for (int i = 1; i < argc; ++i) {
        std::wstring_view arg = argv[i];
        if (arg == L"--attach") {
            attach_only = true;
        } else if (arg == L"--game-dir" && i + 1 < argc) {
            game_dir = argv[++i];
        }
    }

    const fs::path dll = fix_dir / L"ersc_fix.dll";
    const fs::path ersc_launcher = game_dir / L"ersc_launcher.exe";
    std::wprintf(L"ersc_fix_launcher %hs\n", ERSC_FIX_VERSION);
    if (!fs::exists(dll)) return fail(L"ersc_fix.dll not found next to this launcher");

    for (const auto& f : {dll, fix_dir / L"ersc_fix_launcher.exe"}) {
        DeleteFileW((f.wstring() + L":Zone.Identifier").c_str());
    }

    {
        const fs::path exe = game_dir / L"eldenring.exe";
        const std::string version = ersc_fix::game_builds::file_version(exe.c_str());
        if (!ersc_fix::game_builds::find(version)) {

            std::wprintf(L"Elden Ring %hs is not in ersc-fix %hs's verified list (%hs); the DLL will resolve it by "
                         L"signature scan and close the game if that fails.\n",
                         version.empty() ? "<unknown version>" : version.c_str(), ERSC_FIX_VERSION,
                         ersc_fix::game_builds::supported_versions().c_str());
        } else {
            std::wprintf(L"Elden Ring %hs: verified\n", version.c_str());
        }
    }

    {
        const fs::path ersc_dll = game_dir / L"SeamlessCoop" / L"ersc.dll";
        const std::string hash = ersc_fix::common::sha256_file(ersc_dll);
        const ersc_fix::KnownBuild* known = nullptr;
        for (const auto& b : ersc_fix::kKnownErscBuilds) {
            if (b.sha256 == hash) known = &b;
        }
        if (!known) {
            std::fwprintf(stderr,
                          L"ersc.dll sha256 %hs is not a Seamless Co-op build this ersc-fix (%hs) has been checked against.\n"
                          L"Not starting the game through the fix. Use ersc_launcher.exe for stock Seamless Co-op until ersc-fix is updated.\n",
                          hash.empty() ? "<unreadable>" : hash.c_str(), ERSC_FIX_VERSION);
            return fail(L"unknown Seamless Co-op build");
        }
        std::wprintf(L"Seamless Co-op: %hs (verified)\n", std::string(known->label).c_str());
    }

    if (!attach_only) {
        if (!fs::exists(ersc_launcher)) return fail(L"ersc_launcher.exe not found (use --game-dir)");
        std::wprintf(L"starting %ls\n", ersc_launcher.c_str());
        const std::wstring dir = game_dir.wstring();
        SHELLEXECUTEINFOW info{.cbSize = sizeof(info),
                               .lpFile = ersc_launcher.c_str(),
                               .lpDirectory = dir.c_str(),
                               .nShow = SW_SHOWNORMAL};
        if (!ShellExecuteExW(&info)) return fail(L"could not start ersc_launcher.exe");
    }

    std::wprintf(L"waiting for eldenring.exe with ersc.dll loaded...\n");
    const ULONGLONG deadline = GetTickCount64() + kWaitForGameMs;
    while (GetTickCount64() < deadline) {
        for (DWORD pid : find_processes(L"eldenring.exe")) {
            if (!has_module(pid, L"ersc.dll")) continue;
            if (has_module(pid, L"ersc_fix.dll")) {
                std::wprintf(L"ersc_fix.dll already loaded in pid %lu\n", pid);
                return 0;
            }
            Sleep(3000);
            if (!inject(pid, dll)) {

                if (!attach_only) {
                    if (HANDLE p = OpenProcess(PROCESS_TERMINATE, FALSE, pid)) {
                        TerminateProcess(p, 1);
                        CloseHandle(p);
                        std::fwprintf(stderr, L"closed Elden Ring (pid %lu) because the fix DLL did not load; "
                                              L"fix the cause above and launch again, or use ersc_launcher.exe for stock Seamless Coop\n",
                                      pid);
                    }
                }
                return fail(L"injection failed; the game was closed");
            }
            std::wprintf(L"ersc_fix.dll injected into pid %lu\n", pid);
            return 0;
        }
        Sleep(kPollMs);
    }
    return fail(L"timed out waiting for Seamless Coop to start");
}
