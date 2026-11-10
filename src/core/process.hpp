#pragma once

#include <string>
#include <vector>

namespace apctl::core {

struct ProcessResult {
    int exit_code = -1;
    std::string out;
    std::string err;
};

ProcessResult run_process(const std::vector<std::string>& args);

}  // namespace apctl::core
