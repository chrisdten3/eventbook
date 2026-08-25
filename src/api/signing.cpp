#include "eventbook/api/signing.hpp"

#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <utility>
#include <vector>

namespace eventbook {
namespace {

struct EvpPkeyDeleter {
    void operator()(EVP_PKEY* key) const noexcept {
        EVP_PKEY_free(key);
    }
};

struct BioDeleter {
    void operator()(BIO* bio) const noexcept {
        BIO_free(bio);
    }
};

struct MdCtxDeleter {
    void operator()(EVP_MD_CTX* context) const noexcept {
        EVP_MD_CTX_free(context);
    }
};

using PkeyHandle = std::unique_ptr<EVP_PKEY, EvpPkeyDeleter>;
using BioHandle = std::unique_ptr<BIO, BioDeleter>;
using MdCtxHandle = std::unique_ptr<EVP_MD_CTX, MdCtxDeleter>;

/// Base64 without line breaks, which is what the header expects.
Result<std::string, SigningError> base64_encode(const std::vector<unsigned char>& bytes) {
    if (bytes.empty()) {
        return Failure{SigningError::EncodingFailed};
    }
    // EVP_EncodeBlock writes 4 characters per 3 input bytes plus a terminator.
    const std::size_t encoded_size = 4 * ((bytes.size() + 2) / 3);
    std::string encoded(encoded_size + 1, '\0');

    const int written = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(encoded.data()),
                                        bytes.data(), static_cast<int>(bytes.size()));
    if (written < 0 || static_cast<std::size_t>(written) != encoded_size) {
        return Failure{SigningError::EncodingFailed};
    }
    encoded.resize(encoded_size);
    return encoded;
}

}  // namespace

struct RsaPrivateKey::Impl {
    PkeyHandle key;
};

std::string_view to_string(KeyLoadError error) {
    switch (error) {
        case KeyLoadError::EmptyInput:
            return "empty key material";
        case KeyLoadError::FileNotFound:
            return "private key file not found";
        case KeyLoadError::FileUnreadable:
            return "private key file could not be read";
        case KeyLoadError::MalformedPem:
            return "private key is not valid PEM";
        case KeyLoadError::UnsupportedKeyType:
            return "private key is not RSA";
    }
    return "unknown key load error";
}

std::string_view to_string(SigningError error) {
    switch (error) {
        case SigningError::ContextAllocationFailed:
            return "could not allocate a signing context";
        case SigningError::ParameterRejected:
            return "OpenSSL rejected the RSA-PSS parameters";
        case SigningError::SignFailed:
            return "signing failed";
        case SigningError::EncodingFailed:
            return "base64 encoding failed";
    }
    return "unknown signing error";
}

std::string_view to_string(CredentialErrorKind kind) {
    switch (kind) {
        case CredentialErrorKind::KeyIdNotSet:
            return "EVENTBOOK_KALSHI_KEY_ID is not set";
        case CredentialErrorKind::KeyPathNotSet:
            return "EVENTBOOK_KALSHI_KEY_PATH is not set";
        case CredentialErrorKind::KeyUnreadable:
            return "the private key could not be loaded";
    }
    return "unknown credential error";
}

RsaPrivateKey::RsaPrivateKey(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

RsaPrivateKey::~RsaPrivateKey() = default;
RsaPrivateKey::RsaPrivateKey(RsaPrivateKey&&) noexcept = default;
RsaPrivateKey& RsaPrivateKey::operator=(RsaPrivateKey&&) noexcept = default;

Result<RsaPrivateKey, KeyLoadError> RsaPrivateKey::from_pem(std::string_view pem) {
    if (pem.empty()) {
        return Failure{KeyLoadError::EmptyInput};
    }

    BioHandle bio{BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()))};
    if (!bio) {
        return Failure{KeyLoadError::MalformedPem};
    }

    // No passphrase callback is supplied, so an encrypted key fails here rather
    // than prompting on a terminal that may not exist.
    PkeyHandle key{PEM_read_bio_PrivateKey(bio.get(), nullptr, nullptr, nullptr)};
    if (!key) {
        return Failure{KeyLoadError::MalformedPem};
    }
    if (EVP_PKEY_base_id(key.get()) != EVP_PKEY_RSA) {
        return Failure{KeyLoadError::UnsupportedKeyType};
    }

    auto impl = std::make_unique<Impl>();
    impl->key = std::move(key);
    return RsaPrivateKey{std::move(impl)};
}

Result<RsaPrivateKey, KeyLoadError> RsaPrivateKey::from_pem_file(
    const std::filesystem::path& path) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || ec) {
        return Failure{KeyLoadError::FileNotFound};
    }

    std::ifstream file{path, std::ios::binary};
    if (!file) {
        return Failure{KeyLoadError::FileUnreadable};
    }
    std::ostringstream contents;
    contents << file.rdbuf();
    if (!file.good() && !file.eof()) {
        return Failure{KeyLoadError::FileUnreadable};
    }
    return from_pem(contents.str());
}

Result<std::string, SigningError> RsaPrivateKey::sign_pss_sha256(std::string_view message) const {
    MdCtxHandle context{EVP_MD_CTX_new()};
    if (!context) {
        return Failure{SigningError::ContextAllocationFailed};
    }

    EVP_PKEY_CTX* pkey_context = nullptr;
    if (EVP_DigestSignInit(context.get(), &pkey_context, EVP_sha256(), nullptr, impl_->key.get()) !=
        1) {
        return Failure{SigningError::ContextAllocationFailed};
    }

    // All three are Kalshi's requirement rather than OpenSSL's defaults. The
    // default padding is PKCS#1 v1.5, which would produce a well-formed
    // signature that the venue rejects with an unhelpful generic error.
    if (EVP_PKEY_CTX_set_rsa_padding(pkey_context, RSA_PKCS1_PSS_PADDING) != 1 ||
        EVP_PKEY_CTX_set_rsa_pss_saltlen(pkey_context, RSA_PSS_SALTLEN_DIGEST) != 1 ||
        EVP_PKEY_CTX_set_rsa_mgf1_md(pkey_context, EVP_sha256()) != 1) {
        return Failure{SigningError::ParameterRejected};
    }

    const auto* bytes = reinterpret_cast<const unsigned char*>(message.data());

    // Called twice: once with a null buffer to learn the length, then to sign.
    std::size_t signature_size = 0;
    if (EVP_DigestSign(context.get(), nullptr, &signature_size, bytes, message.size()) != 1) {
        return Failure{SigningError::SignFailed};
    }

    std::vector<unsigned char> signature(signature_size);
    if (EVP_DigestSign(context.get(), signature.data(), &signature_size, bytes, message.size()) !=
        1) {
        return Failure{SigningError::SignFailed};
    }
    signature.resize(signature_size);

    return base64_encode(signature);
}

std::string signing_payload(std::string_view timestamp_millis, std::string_view method,
                            std::string_view path) {
    std::string payload;
    payload.reserve(timestamp_millis.size() + method.size() + path.size());
    payload.append(timestamp_millis);
    payload.append(method);
    payload.append(path);
    return payload;
}

RequestSigner::RequestSigner(std::string key_id, RsaPrivateKey key)
    : key_id_(std::move(key_id)), key_(std::move(key)) {}

Result<RequestSignature, SigningError> RequestSigner::sign_get(std::string_view path,
                                                               LocalTimestamp at) const {
    // Kalshi wants milliseconds. The project stores microseconds, so this is
    // the one place the extra precision is discarded on purpose.
    const std::string timestamp = std::to_string(epoch_micros(at) / 1000);

    auto signature = key_.sign_pss_sha256(signing_payload(timestamp, "GET", path));
    if (!signature) {
        return Failure{signature.error()};
    }
    return RequestSignature{key_id_, timestamp, *signature};
}

Result<RequestSigner, CredentialError> load_signer_from_environment() {
    const char* key_id = std::getenv(std::string{kKeyIdEnvironmentVariable}.c_str());
    if (key_id == nullptr || *key_id == '\0') {
        return Failure{CredentialError{CredentialErrorKind::KeyIdNotSet, {}}};
    }

    const char* key_path = std::getenv(std::string{kKeyPathEnvironmentVariable}.c_str());
    if (key_path == nullptr || *key_path == '\0') {
        return Failure{CredentialError{CredentialErrorKind::KeyPathNotSet, {}}};
    }

    auto key = RsaPrivateKey::from_pem_file(std::filesystem::path{key_path});
    if (!key) {
        // Deliberately reports only the failure kind. The path is already known
        // to whoever set the variable, and error strings have a way of ending
        // up in logs that get shared.
        return Failure{CredentialError{CredentialErrorKind::KeyUnreadable, key.error()}};
    }

    return RequestSigner{std::string{key_id}, *std::move(key)};
}

}  // namespace eventbook
