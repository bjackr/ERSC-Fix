#pragma once

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

#pragma comment(lib, "version.lib")

namespace ersc_fix::game_builds {

struct AddressSet {
    const char* version;
    const char* label;
    uint32_t world_chr_man, cs_event_flag_man, session_man, get_event_flag;
    uint32_t vt_network, vt_pad, vt_netai, vt_ride;
};

inline constexpr AddressSet kBuilds[] = {
    {"2.7.1.0", "eldenring.exe 2.7.1.0 (full decompile)", 0x3D69FF8, 0x3D6C4B8, 0x3D7E540, 0x5FA250, 0x2A2E1B8, 0x2A2E788,
     0x2A2D858, 0x2A2F118},
    {"2.7.2.0", "eldenring.exe 2.7.2.0 (ported by signature)", 0x3D6D018, 0x3D6F4D8, 0x3D81560, 0x5FA700, 0x2A301C8,
     0x2A30798, 0x2A2F868, 0x2A31128},
};

inline const AddressSet* find(const std::string& version) {
    for (const auto& b : kBuilds) {
        if (version == b.version) return &b;
    }
    return nullptr;
}

inline std::string supported_versions() {
    std::string s;
    for (const auto& b : kBuilds) {
        if (!s.empty()) s += ", ";
        s += b.version;
    }
    return s;
}

inline std::string file_version(const wchar_t* path) {
    DWORD handle = 0;
    const DWORD size = GetFileVersionInfoSizeW(path, &handle);
    if (!size) return {};
    std::vector<uint8_t> buf(size);
    if (!GetFileVersionInfoW(path, 0, size, buf.data())) return {};
    VS_FIXEDFILEINFO* ffi = nullptr;
    UINT len = 0;
    if (!VerQueryValueW(buf.data(), L"\\", reinterpret_cast<void**>(&ffi), &len) || !ffi) return {};
    return std::to_string(HIWORD(ffi->dwFileVersionMS)) + "." + std::to_string(LOWORD(ffi->dwFileVersionMS)) + "." +
           std::to_string(HIWORD(ffi->dwFileVersionLS)) + "." + std::to_string(LOWORD(ffi->dwFileVersionLS));
}

}
