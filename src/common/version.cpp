#include "eventbook/common/version.hpp"

#include <fmt/format.h>

namespace eventbook {
namespace {

// Whether this translation unit was compiled with AddressSanitizer or
// UndefinedBehaviorSanitizer active. Clang and GCC advertise this differently.
constexpr bool sanitizers_active() {
#if defined(__SANITIZE_ADDRESS__)
    return true;
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(undefined_behavior_sanitizer)
    return true;
#else
    return false;
#endif
#else
    return false;
#endif
}

}  // namespace

std::string to_string(const Version& version) {
    return fmt::format("{}.{}.{}", version.major, version.minor, version.patch);
}

BuildInfo build_info() {
    return BuildInfo{
        .version = kVersion,
        .compiler = EVENTBOOK_COMPILER_ID " " EVENTBOOK_COMPILER_VERSION,
        .build_type = EVENTBOOK_BUILD_TYPE,
        .cxx_standard = __cplusplus,
        .sanitizers_enabled = sanitizers_active(),
    };
}

std::string describe_build() {
    const BuildInfo info = build_info();
    return fmt::format("eventbook {} ({}, {}, __cplusplus={}{})", to_string(info.version),
                       info.build_type, info.compiler, info.cxx_standard,
                       info.sanitizers_enabled ? ", sanitizers on" : "");
}

}  // namespace eventbook
