#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

#include "eventbook/common/result.hpp"
#include "eventbook/common/time.hpp"

namespace eventbook {

enum class KeyLoadError {
    EmptyInput,
    FileNotFound,
    FileUnreadable,
    MalformedPem,
    UnsupportedKeyType,
};

[[nodiscard]] std::string_view to_string(KeyLoadError error);

enum class SigningError {
    ContextAllocationFailed,
    ParameterRejected,
    SignFailed,
    EncodingFailed,
};

[[nodiscard]] std::string_view to_string(SigningError error);

/// An RSA private key held in memory for request signing.
///
/// The key material is never exposed. It is read straight into an OpenSSL
/// EVP_PKEY and only ever used to produce signatures, so this class offers no
/// accessor that could put private key bytes into a log line, an exception
/// message, or a stack trace. That is the single most important property here:
/// a leaked signing key is an account compromise, and the usual way one leaks
/// is a well-meaning diagnostic that prints a struct.
///
/// Move-only. Two owners of one key would make its lifetime ambiguous for no
/// benefit, and OpenSSL handles are not trivially copyable anyway.
class RsaPrivateKey {
public:
    [[nodiscard]] static Result<RsaPrivateKey, KeyLoadError> from_pem_file(
        const std::filesystem::path& path);

    /// Load from PEM text already in memory. Used by tests, which generate a
    /// throwaway key at run time rather than committing one -- AGENTS.md
    /// forbids private keys in the repository, and a test key is still a
    /// private key.
    [[nodiscard]] static Result<RsaPrivateKey, KeyLoadError> from_pem(std::string_view pem);

    ~RsaPrivateKey();
    RsaPrivateKey(RsaPrivateKey&&) noexcept;
    RsaPrivateKey& operator=(RsaPrivateKey&&) noexcept;
    RsaPrivateKey(const RsaPrivateKey&) = delete;
    RsaPrivateKey& operator=(const RsaPrivateKey&) = delete;

    /// Sign with RSA-PSS: SHA-256 digest, MGF1 over SHA-256, salt length equal
    /// to the digest length. Returns base64.
    ///
    /// Those parameters are Kalshi's requirement, not OpenSSL's defaults, and
    /// getting any of them wrong produces a signature the venue rejects with a
    /// generic authentication failure.
    ///
    /// PSS is randomized: signing the same message twice yields two different
    /// signatures, both valid. Correctness therefore cannot be checked against
    /// a stored expected value -- it has to be verified with the public key.
    [[nodiscard]] Result<std::string, SigningError> sign_pss_sha256(std::string_view message) const;

private:
    struct Impl;
    explicit RsaPrivateKey(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

/// The exact byte string Kalshi signs: timestamp, then method, then path.
///
/// No separators between the parts, and the query string is excluded. Because
/// the query is not signed, two requests to one path with different parameters
/// produce interchangeable signatures -- the timestamp is what limits replay,
/// which is why it is milliseconds rather than seconds.
[[nodiscard]] std::string signing_payload(std::string_view timestamp_millis,
                                          std::string_view method, std::string_view path);

/// The three header values an authenticated Kalshi request carries.
struct RequestSignature {
    std::string key_id;            ///< KALSHI-ACCESS-KEY
    std::string timestamp_millis;  ///< KALSHI-ACCESS-TIMESTAMP
    std::string signature;         ///< KALSHI-ACCESS-SIGNATURE, base64
};

/// Produces authentication headers for read-only requests.
///
/// There is deliberately only sign_get(). Version one is read-only, and a valid
/// signature is precisely what would make an order request acceptable to the
/// venue. Having no way to sign anything but GET is a stronger guarantee than
/// having no call site that sends a POST: it cannot be undone by adding one
/// line somewhere else, only by editing this interface in review.
class RequestSigner {
public:
    RequestSigner(std::string key_id, RsaPrivateKey key);

    /// `at` is passed in rather than read from the clock so that signing stays
    /// deterministic and testable, in keeping with the project's rule that
    /// nothing reaches for a hidden global clock.
    [[nodiscard]] Result<RequestSignature, SigningError> sign_get(std::string_view path,
                                                                  LocalTimestamp at) const;

    [[nodiscard]] const std::string& key_id() const {
        return key_id_;
    }

private:
    std::string key_id_;
    RsaPrivateKey key_;
};

enum class CredentialErrorKind {
    KeyIdNotSet,
    KeyPathNotSet,
    KeyUnreadable,
};

[[nodiscard]] std::string_view to_string(CredentialErrorKind kind);

struct CredentialError {
    CredentialErrorKind kind{};
    KeyLoadError key_error{};  ///< meaningful only when kind == KeyUnreadable
};

/// Environment variable naming the API key id.
inline constexpr std::string_view kKeyIdEnvironmentVariable = "EVENTBOOK_KALSHI_KEY_ID";

/// Environment variable naming the path to the RSA private key PEM file.
///
/// A path rather than the key itself: an environment variable holding key
/// material shows up in `ps`, in container inspection output, and in crash
/// reports on some systems. A path leaks only a filename.
inline constexpr std::string_view kKeyPathEnvironmentVariable = "EVENTBOOK_KALSHI_KEY_PATH";

[[nodiscard]] Result<RequestSigner, CredentialError> load_signer_from_environment();

}  // namespace eventbook
