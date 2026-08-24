find_package(Catch2 3 CONFIG REQUIRED)
include(Catch)

# A single test binary keeps the feedback loop short. Split it only when link
# time actually becomes a problem, not before.
add_executable(eventbook_tests
  unit/test_version.cpp
)

target_link_libraries(eventbook_tests
  PRIVATE
    eventbook::core
    eventbook::warnings
    Catch2::Catch2WithMain
)

# Registers each TEST_CASE with CTest individually, so `ctest -R` can select one.
catch_discover_tests(eventbook_tests)
