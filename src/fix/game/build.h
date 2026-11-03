#pragma once

#include <string>

namespace ersc_fix::game::build {

struct Info {
    std::string version;
    bool known = false;
    const char* label = "unknown";
};

const Info& detect();
const Info& current();

}
