#pragma once

#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>

#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace eventbook::testing {

struct KeyPair {
    std::string private_pem;
    std::string public_pem;
};

namespace detail {

using BioPtr = std::unique_ptr<BIO, decltype(&BIO_free)>;
using PkeyPtr = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
using MdCtxPtr = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;

// BIO_get_mem_data is a macro containing a C-style cast. GCC attributes that
// cast to the expansion site rather than to the OpenSSL header that defines it,
// so -Wold-style-cast would fire here on code we did not write.
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wold-style-cast"
#endif
inline std::string bio_contents(BIO* bio) {
    char* data = nullptr;
    const long length = BIO_get_mem_data(bio, &data);
    if (data == nullptr || length <= 0) {
        throw std::runtime_error("empty BIO");
    }
    return std::string(data, static_cast<std::size_t>(length));
}
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

inline std::vector<unsigned char> base64_decode(std::string_view text) {
    if (text.empty()) {
        return {};
    }
    std::vector<unsigned char> decoded(text.size());
    const int written =
        EVP_DecodeBlock(decoded.data(), reinterpret_cast<const unsigned char*>(text.data()),
                        static_cast<int>(text.size()));
    if (written < 0) {
        throw std::runtime_error("base64 decode failed");
    }
    decoded.resize(static_cast<std::size_t>(written));

    // EVP_DecodeBlock always emits a multiple of three bytes, so any '='
    // padding in the input has to be accounted for by hand.
    std::size_t padding = 0;
    if (text.back() == '=') {
        ++padding;
    }
    if (text.size() > 1 && text[text.size() - 2] == '=') {
        ++padding;
    }
    decoded.resize(decoded.size() - padding);
    return decoded;
}

}  // namespace detail

/// Generate a throwaway RSA key pair at run time.
///
/// Generated rather than committed. AGENTS.md forbids private keys in the
/// repository, and a key that exists only for tests is still a private key.
inline KeyPair generate_rsa_key_pair(unsigned int bits = 2048) {
    detail::PkeyPtr key{EVP_RSA_gen(bits), EVP_PKEY_free};
    if (!key) {
        throw std::runtime_error("EVP_RSA_gen failed");
    }

    KeyPair pair;
    {
        detail::BioPtr bio{BIO_new(BIO_s_mem()), BIO_free};
        if (PEM_write_bio_PrivateKey(bio.get(), key.get(), nullptr, nullptr, 0, nullptr, nullptr) !=
            1) {
            throw std::runtime_error("PEM_write_bio_PrivateKey failed");
        }
        pair.private_pem = detail::bio_contents(bio.get());
    }
    {
        detail::BioPtr bio{BIO_new(BIO_s_mem()), BIO_free};
        if (PEM_write_bio_PUBKEY(bio.get(), key.get()) != 1) {
            throw std::runtime_error("PEM_write_bio_PUBKEY failed");
        }
        pair.public_pem = detail::bio_contents(bio.get());
    }
    return pair;
}

/// Generated once per process: RSA key generation is slow enough that doing it
/// per test case would dominate the suite's run time.
inline const KeyPair& shared_rsa_key_pair() {
    static const KeyPair pair = generate_rsa_key_pair();
    return pair;
}

/// An EC key, for asserting that the loader rejects non-RSA material.
inline std::string generate_ec_private_pem() {
    detail::PkeyPtr key{EVP_EC_gen("P-256"), EVP_PKEY_free};
    if (!key) {
        throw std::runtime_error("EVP_EC_gen failed");
    }
    detail::BioPtr bio{BIO_new(BIO_s_mem()), BIO_free};
    if (PEM_write_bio_PrivateKey(bio.get(), key.get(), nullptr, nullptr, 0, nullptr, nullptr) !=
        1) {
        throw std::runtime_error("PEM_write_bio_PrivateKey failed");
    }
    return detail::bio_contents(bio.get());
}

/// Verify a base64 RSA-PSS/SHA-256 signature using OpenSSL directly.
///
/// Deliberately an independent code path. The production signer is checked
/// against raw OpenSSL rather than against itself, and since PSS is randomized
/// there is no stored expected signature to compare with -- verification is the
/// only way to establish that the signer is correct.
inline bool verify_pss_sha256(const std::string& public_pem, std::string_view message,
                              std::string_view signature_base64) {
    detail::BioPtr bio{BIO_new_mem_buf(public_pem.data(), static_cast<int>(public_pem.size())),
                       BIO_free};
    detail::PkeyPtr key{PEM_read_bio_PUBKEY(bio.get(), nullptr, nullptr, nullptr), EVP_PKEY_free};
    if (!key) {
        throw std::runtime_error("PEM_read_bio_PUBKEY failed");
    }

    const auto signature = detail::base64_decode(signature_base64);
    if (signature.empty()) {
        return false;
    }

    detail::MdCtxPtr context{EVP_MD_CTX_new(), EVP_MD_CTX_free};
    EVP_PKEY_CTX* pkey_context = nullptr;
    if (EVP_DigestVerifyInit(context.get(), &pkey_context, EVP_sha256(), nullptr, key.get()) != 1) {
        throw std::runtime_error("EVP_DigestVerifyInit failed");
    }
    if (EVP_PKEY_CTX_set_rsa_padding(pkey_context, RSA_PKCS1_PSS_PADDING) != 1 ||
        EVP_PKEY_CTX_set_rsa_pss_saltlen(pkey_context, RSA_PSS_SALTLEN_DIGEST) != 1 ||
        EVP_PKEY_CTX_set_rsa_mgf1_md(pkey_context, EVP_sha256()) != 1) {
        throw std::runtime_error("PSS parameters rejected");
    }

    return EVP_DigestVerify(context.get(), signature.data(), signature.size(),
                            reinterpret_cast<const unsigned char*>(message.data()),
                            message.size()) == 1;
}

}  // namespace eventbook::testing
