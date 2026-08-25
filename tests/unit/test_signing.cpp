#include "eventbook/api/signing.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include "test_rsa_key.hpp"

using eventbook::CredentialErrorKind;
using eventbook::KeyLoadError;
using eventbook::load_signer_from_environment;
using eventbook::local_time_from_epoch_micros;
using eventbook::RequestSigner;
using eventbook::RsaPrivateKey;
using eventbook::signing_payload;
using eventbook::testing::generate_ec_private_pem;
using eventbook::testing::shared_rsa_key_pair;
using eventbook::testing::verify_pss_sha256;

namespace {

constexpr std::string_view kWebSocketPath = "/trade-api/ws/v2";

RsaPrivateKey load_test_key() {
    auto key = RsaPrivateKey::from_pem(shared_rsa_key_pair().private_pem);
    REQUIRE(key.has_value());
    return *std::move(key);
}

RequestSigner make_signer(std::string key_id = "test-key-id") {
    return RequestSigner{std::move(key_id), load_test_key()};
}

}  // namespace

TEST_CASE("the signing payload is timestamp, method, then path with no separators") {
    // Getting this string wrong yields a valid signature over the wrong bytes,
    // which the venue rejects with a generic authentication failure that says
    // nothing about which part was wrong.
    CHECK(signing_payload("1700000000000", "GET", kWebSocketPath) ==
          "1700000000000GET/trade-api/ws/v2");
    CHECK(signing_payload("1", "GET", "/trade-api/v2/markets") == "1GET/trade-api/v2/markets");
}

TEST_CASE("a produced signature verifies against the public key") {
    const auto signer = make_signer();
    const auto at = local_time_from_epoch_micros(1'700'000'000'000'000);

    const auto signature = signer.sign_get(kWebSocketPath, at);
    REQUIRE(signature.has_value());

    const auto expected_payload =
        signing_payload(signature->timestamp_millis, "GET", kWebSocketPath);
    CHECK(verify_pss_sha256(shared_rsa_key_pair().public_pem, expected_payload,
                            signature->signature));
}

TEST_CASE("the signature covers exactly the documented payload and nothing else") {
    const auto signer = make_signer();
    const auto signature = signer.sign_get(kWebSocketPath, local_time_from_epoch_micros(0));
    REQUIRE(signature.has_value());

    const auto& public_pem = shared_rsa_key_pair().public_pem;

    // Right payload verifies.
    CHECK(verify_pss_sha256(public_pem, "0GET/trade-api/ws/v2", signature->signature));

    // Every plausible near-miss does not.
    CHECK_FALSE(verify_pss_sha256(public_pem, "0GET/trade-api/ws/v3", signature->signature));
    CHECK_FALSE(verify_pss_sha256(public_pem, "0POST/trade-api/ws/v2", signature->signature));
    CHECK_FALSE(verify_pss_sha256(public_pem, "1GET/trade-api/ws/v2", signature->signature));
    CHECK_FALSE(verify_pss_sha256(public_pem, "0 GET /trade-api/ws/v2", signature->signature));
    CHECK_FALSE(verify_pss_sha256(public_pem, "GET/trade-api/ws/v2", signature->signature));
}

TEST_CASE("PSS is randomized, so two signatures of one message differ and both verify") {
    // This is why correctness cannot be asserted against a stored expected
    // signature: there is no single right answer, only verifiable ones.
    const auto signer = make_signer();
    const auto at = local_time_from_epoch_micros(1'700'000'000'000'000);

    const auto first = signer.sign_get(kWebSocketPath, at);
    const auto second = signer.sign_get(kWebSocketPath, at);
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());

    CHECK(first->signature != second->signature);

    const auto payload = signing_payload(first->timestamp_millis, "GET", kWebSocketPath);
    CHECK(verify_pss_sha256(shared_rsa_key_pair().public_pem, payload, first->signature));
    CHECK(verify_pss_sha256(shared_rsa_key_pair().public_pem, payload, second->signature));
}

TEST_CASE("a different key does not verify") {
    const auto signer = make_signer();
    const auto signature = signer.sign_get(kWebSocketPath, local_time_from_epoch_micros(0));
    REQUIRE(signature.has_value());

    const auto other = eventbook::testing::generate_rsa_key_pair();
    CHECK_FALSE(verify_pss_sha256(other.public_pem, "0GET/trade-api/ws/v2", signature->signature));
}

TEST_CASE("the timestamp is milliseconds, not the project's microseconds") {
    const auto signer = make_signer();

    const auto signature =
        signer.sign_get(kWebSocketPath, local_time_from_epoch_micros(1'700'000'000'123'456));
    REQUIRE(signature.has_value());
    // 1,700,000,000,123,456 us -> 1,700,000,000,123 ms. Sub-millisecond
    // precision is discarded here on purpose; this is the one place it happens.
    CHECK(signature->timestamp_millis == "1700000000123");
}

TEST_CASE("the key id is carried through untouched") {
    const auto signer = make_signer("abc-123-def");
    CHECK(signer.key_id() == "abc-123-def");

    const auto signature = signer.sign_get(kWebSocketPath, local_time_from_epoch_micros(0));
    REQUIRE(signature.has_value());
    CHECK(signature->key_id == "abc-123-def");
}

TEST_CASE("malformed key material is rejected") {
    SECTION("empty") {
        const auto key = RsaPrivateKey::from_pem("");
        REQUIRE_FALSE(key.has_value());
        CHECK(key.error() == KeyLoadError::EmptyInput);
    }

    SECTION("not PEM at all") {
        const auto key = RsaPrivateKey::from_pem("hunter2");
        REQUIRE_FALSE(key.has_value());
        CHECK(key.error() == KeyLoadError::MalformedPem);
    }

    SECTION("truncated PEM") {
        auto truncated = shared_rsa_key_pair().private_pem.substr(0, 80);
        const auto key = RsaPrivateKey::from_pem(truncated);
        REQUIRE_FALSE(key.has_value());
        CHECK(key.error() == KeyLoadError::MalformedPem);
    }

    SECTION("a valid key of the wrong type") {
        // Kalshi signs with RSA-PSS. An EC key is perfectly good PEM and
        // entirely useless here, so it fails with its own reason.
        const auto key = RsaPrivateKey::from_pem(generate_ec_private_pem());
        REQUIRE_FALSE(key.has_value());
        CHECK(key.error() == KeyLoadError::UnsupportedKeyType);
    }
}

TEST_CASE("a key loads from a file") {
    const auto path = std::filesystem::temp_directory_path() / "eventbook_test_key.pem";
    {
        std::ofstream out{path};
        out << shared_rsa_key_pair().private_pem;
    }

    const auto key = RsaPrivateKey::from_pem_file(path);
    CHECK(key.has_value());

    std::filesystem::remove(path);

    const auto missing = RsaPrivateKey::from_pem_file(path);
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error() == KeyLoadError::FileNotFound);
}

TEST_CASE("environment loading reports precisely what is missing") {
    const std::string key_id_var{eventbook::kKeyIdEnvironmentVariable};
    const std::string key_path_var{eventbook::kKeyPathEnvironmentVariable};

    unsetenv(key_id_var.c_str());
    unsetenv(key_path_var.c_str());

    SECTION("no key id") {
        const auto signer = load_signer_from_environment();
        REQUIRE_FALSE(signer.has_value());
        CHECK(signer.error().kind == CredentialErrorKind::KeyIdNotSet);
    }

    SECTION("key id but no path") {
        setenv(key_id_var.c_str(), "abc", 1);
        const auto signer = load_signer_from_environment();
        REQUIRE_FALSE(signer.has_value());
        CHECK(signer.error().kind == CredentialErrorKind::KeyPathNotSet);
    }

    SECTION("an empty key id counts as unset") {
        setenv(key_id_var.c_str(), "", 1);
        const auto signer = load_signer_from_environment();
        REQUIRE_FALSE(signer.has_value());
        CHECK(signer.error().kind == CredentialErrorKind::KeyIdNotSet);
    }

    SECTION("path pointing at nothing") {
        setenv(key_id_var.c_str(), "abc", 1);
        setenv(key_path_var.c_str(), "/nonexistent/eventbook/key.pem", 1);
        const auto signer = load_signer_from_environment();
        REQUIRE_FALSE(signer.has_value());
        CHECK(signer.error().kind == CredentialErrorKind::KeyUnreadable);
        CHECK(signer.error().key_error == KeyLoadError::FileNotFound);
    }

    SECTION("both set and valid") {
        const auto path = std::filesystem::temp_directory_path() / "eventbook_env_key.pem";
        {
            std::ofstream out{path};
            out << shared_rsa_key_pair().private_pem;
        }
        setenv(key_id_var.c_str(), "env-key-id", 1);
        setenv(key_path_var.c_str(), path.string().c_str(), 1);

        auto signer = load_signer_from_environment();
        REQUIRE(signer.has_value());
        CHECK(signer->key_id() == "env-key-id");

        std::filesystem::remove(path);
    }

    unsetenv(key_id_var.c_str());
    unsetenv(key_path_var.c_str());
}
