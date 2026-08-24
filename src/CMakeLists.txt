# eventbook_core holds every piece of domain logic. Applications and tests link
# against it; nothing of substance lives in apps/ or tests/ themselves. That is
# what makes the same book and feature code reachable from both live collection
# and offline replay (AGENTS.md, "One event model, two data sources").
add_library(eventbook_core
  common/version.cpp
)
add_library(eventbook::core ALIAS eventbook_core)

target_include_directories(eventbook_core
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

# Build identity, injected once here from the values CMake already knows.
target_compile_definitions(eventbook_core
  PUBLIC
    EVENTBOOK_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
    EVENTBOOK_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    EVENTBOOK_VERSION_PATCH=${PROJECT_VERSION_PATCH}
  PRIVATE
    EVENTBOOK_COMPILER_ID="${CMAKE_CXX_COMPILER_ID}"
    EVENTBOOK_COMPILER_VERSION="${CMAKE_CXX_COMPILER_VERSION}"
    EVENTBOOK_BUILD_TYPE="$<CONFIG>"
)

target_link_libraries(eventbook_core
  PUBLIC
    fmt::fmt
  PRIVATE
    eventbook::warnings
)
