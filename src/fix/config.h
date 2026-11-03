#pragma once

#include <filesystem>

namespace ersc_fix::config {

void load(const std::filesystem::path& dir);

bool hook_enabled(const wchar_t* name);

const std::wstring& ini_path();

}
