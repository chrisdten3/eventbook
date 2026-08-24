#pragma once

#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace eventbook::testing {

/// Read a file from tests/fixtures. The directory is injected by CMake so the
/// tests do not depend on the working directory ctest happens to use.
inline std::string read_fixture(const std::string& name) {
    const std::string path = std::string{EVENTBOOK_FIXTURE_DIR} + "/" + name;
    std::ifstream file{path};
    if (!file) {
        throw std::runtime_error("missing fixture: " + path);
    }
    std::ostringstream contents;
    contents << file.rdbuf();
    return contents.str();
}

}  // namespace eventbook::testing
