cmake_minimum_required(VERSION 3.25)

# The toolchain file must be set before project() so that vcpkg can install the
# manifest dependencies during configuration. CMakePresets.json does this for us.
project(eventbook
  VERSION 0.1.0
  DESCRIPTION "Market-microstructure and execution-research platform for Kalshi binary event contracts"
  LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Emit compile_commands.json for clangd / clang-tidy.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# All binaries land in one predictable place so README commands stay stable.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(EventBookWarnings)

option(EVENTBOOK_BUILD_TESTS "Build the eventbook test suite" ON)

find_package(fmt CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(boost_beast CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(CLI11 CONFIG REQUIRED)

add_subdirectory(src)
add_subdirectory(apps)

if(EVENTBOOK_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
