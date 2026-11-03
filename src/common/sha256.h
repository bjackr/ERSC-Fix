#pragma once

#include <filesystem>
#include <string>

namespace ersc_fix::common {

std::string sha256_file(const std::filesystem::path& path);

}
