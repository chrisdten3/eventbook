# One executable per pipeline stage. `scout` describes the eligible market
# universe (M1) and `collect` will record it (M2); `replay`, `build_features`,
# and `simulate_execution` arrive with M3, M4, and M6.
add_executable(collect collect.cpp)

target_link_libraries(collect
  PRIVATE
    eventbook::core
    eventbook::warnings
    CLI11::CLI11
    spdlog::spdlog
)

add_executable(scout scout.cpp)

target_link_libraries(scout
  PRIVATE
    eventbook::core
    eventbook::warnings
    CLI11::CLI11
    spdlog::spdlog
)
