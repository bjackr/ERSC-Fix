#pragma once

#include "known_builds.h"

#include <windows.h>

#include <filesystem>
#include <string>
#include <string_view>

namespace ersc_fix::guard {

HMODULE wait_for_module(const wchar_t* name, DWORD timeout_ms);

std::filesystem::path module_path(HMODULE module);

std::string sha256_file(const std::filesystem::path& path);

const KnownBuild* find_known_build(std::string_view sha256);

std::string describe_address(const void* address);

}
