#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace eventbook {

/// Compile-time version of the eventbook library.
///
/// Every research result must be traceable to the code that produced it
/// (see AGENTS.md, "Research claims and resume rules"). This is the first link
/// in that chain: the version is baked into the binary at compile time and
/// reported by every application, so a results file can always name its build.
struct Version {
    std::uint32_t major{};
    std::uint32_t minor{};
    std::uint32_t patch{};

    [[nodiscard]] friend constexpr bool operator==(const Version&, const Version&) = default;
};

/// The version this translation unit was compiled against.
///
/// The EVENTBOOK_VERSION_* macros are injected by CMake from the `project()`
/// version, so there is exactly one source of truth: the top-level CMakeLists.
inline constexpr Version kVersion{
    EVENTBOOK_VERSION_MAJOR,
    EVENTBOOK_VERSION_MINOR,
    EVENTBOOK_VERSION_PATCH,
};

/// Human-readable "major.minor.patch".
[[nodiscard]] std::string to_string(const Version& version);

/// Identity of the toolchain and configuration that produced the library.
///
/// Returned by value as a snapshot of compile-time state. The `string_view`
/// members point at string literals with static storage duration, so they
/// outlive every caller and the struct is safe to copy and store.
struct BuildInfo {
    Version version{};
    std::string_view compiler;    ///< e.g. "AppleClang 17.0.0.17000013"
    std::string_view build_type;  ///< e.g. "Debug", "RelWithDebInfo"
    long cxx_standard{};          ///< value of __cplusplus, e.g. 202002
    bool sanitizers_enabled{};    ///< true when built with ASan and/or UBSan
};

/// Build identity of the library itself, not of the calling translation unit.
[[nodiscard]] BuildInfo build_info();

/// One-line banner suitable for logs and `--version` output.
[[nodiscard]] std::string describe_build();

}  // namespace eventbook
