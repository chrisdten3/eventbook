find_package(Catch2 3 CONFIG REQUIRED)
include(Catch)

# A single test binary keeps the feedback loop short. Split it only when link
# time actually becomes a problem, not before.
add_executable(eventbook_tests
  unit/test_decimal.cpp
  unit/test_identifiers.cpp
  unit/test_price.cpp
  unit/test_quantity.cpp
  unit/test_rest_client.cpp
  unit/test_time.cpp
  unit/test_version.cpp
)

target_include_directories(eventbook_tests PRIVATE support)

target_link_libraries(eventbook_tests
  PRIVATE
    eventbook::core
    eventbook::warnings
    Catch2::Catch2WithMain
)

# Registers each TEST_CASE with CTest individually, so `ctest -R` can select one.
catch_discover_tests(eventbook_tests)

# Live smoke tests are built (so they cannot rot) but deliberately NOT
# registered with CTest: `ctest` must never touch the network, or an offline
# machine or a venue outage becomes a red build. Run them by hand:
#   ./build/<preset>/bin/eventbook_live_tests
add_executable(eventbook_live_tests
  integration/live_smoke.cpp
)

target_link_libraries(eventbook_live_tests
  PRIVATE
    eventbook::core
    eventbook::warnings
    Catch2::Catch2WithMain
)
