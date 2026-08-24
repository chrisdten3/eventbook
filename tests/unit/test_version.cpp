#include "eventbook/common/version.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>

TEST_CASE("Version formats as major.minor.patch", "[version]") {
    const eventbook::Version version{1, 2, 3};
    REQUIRE(eventbook::to_string(version) == "1.2.3");
}

TEST_CASE("Version compares by value", "[version]") {
    REQUIRE(eventbook::Version{0, 1, 0} == eventbook::Version{0, 1, 0});
    REQUIRE_FALSE(eventbook::Version{0, 1, 0} == eventbook::Version{0, 1, 1});
}

TEST_CASE("build_info reports a coherent build identity", "[version]") {
    const eventbook::BuildInfo info = eventbook::build_info();

    // The library was compiled from the same project() version the header saw.
    REQUIRE(info.version == eventbook::kVersion);

    // C++20 or newer. If this fails the standard flags are not reaching the
    // library target, which would silently change overload resolution.
    REQUIRE(info.cxx_standard >= 202002L);

    REQUIRE_FALSE(info.compiler.empty());
    REQUIRE_FALSE(info.build_type.empty());
}

TEST_CASE("describe_build mentions the version", "[version]") {
    const std::string banner = eventbook::describe_build();
    REQUIRE(banner.find(eventbook::to_string(eventbook::kVersion)) != std::string::npos);
}
