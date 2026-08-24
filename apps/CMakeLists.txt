# One executable per pipeline stage. Only `collect` exists today; `replay`,
# `build_features`, and `simulate_execution` arrive with M3, M4, and M6.
add_executable(collect collect.cpp)

target_link_libraries(collect
  PRIVATE
    eventbook::core
    eventbook::warnings
    CLI11::CLI11
    spdlog::spdlog
)
