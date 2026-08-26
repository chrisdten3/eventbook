find_package(Catch2 3 CONFIG REQUIRED)
include(Catch)

# A single test binary keeps the feedback loop short. Split it only when link
# time actually becomes a problem, not before.
add_executable(eventbook_tests
  unit/test_decimal.cpp
  unit/test_eligibility.cpp
  unit/test_features.cpp
  unit/test_identifiers.cpp
  unit/test_journal.cpp
  unit/test_market.cpp
  unit/test_order_book.cpp
  unit/test_market_query.cpp
  unit/test_price.cpp
  unit/test_quantity.cpp
  unit/test_replay.cpp
  unit/test_rest_client.cpp
  unit/test_signing.cpp
  unit/test_ws_protocol.cpp
  unit/test_ws_session.cpp
  unit/test_time.cpp
  unit/test_version.cpp
)

target_include_directories(eventbook_tests PRIVATE support)

# Fixtures are located by absolute path so the tests do not depend on whatever
# working directory ctest happens to use.
target_compile_definitions(eventbook_tests
  PRIVATE EVENTBOOK_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")

target_link_libraries(eventbook_tests
  PRIVATE
    eventbook::core
    eventbook::warnings
    Catch2::Catch2WithMain
)

# The signing tests verify our signatures with raw OpenSSL rather than with our
# own verifier, so the check runs against an independent implementation. Only
# the headers are needed here: eventbook_core links OpenSSL PRIVATE, which
# already places libcrypto on this target's link line.
target_include_directories(eventbook_tests PRIVATE ${OPENSSL_INCLUDE_DIR})

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

# The bad-signature test generates a throwaway key pair, same as the unit tests.
target_include_directories(eventbook_live_tests PRIVATE support ${OPENSSL_INCLUDE_DIR})
