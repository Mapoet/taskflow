#include <cassert>
#include <memory>
#include <string>

#include <openssl/evp.h>
#include <openssl/pem.h>

#include "agent/live/production_signature.hpp"

namespace {
std::string hex(const unsigned char* data, std::size_t size) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(size * 2);
    for(std::size_t i = 0; i < size; ++i) {
        out.push_back(digits[data[i] >> 4]);
        out.push_back(digits[data[i] & 15]);
    }
    return out;
}
}

int main() {
    using namespace agent_framework::live;
    std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> generator(
        EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, nullptr), EVP_PKEY_CTX_free);
    assert(generator && EVP_PKEY_keygen_init(generator.get()) == 1);
    EVP_PKEY* raw_key = nullptr;
    assert(EVP_PKEY_keygen(generator.get(), &raw_key) == 1);
    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key(raw_key, EVP_PKEY_free);

    const std::string digest = "sha256:production-report-digest";
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> signer(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    assert(signer && EVP_DigestSignInit(signer.get(), nullptr, nullptr, nullptr, key.get()) == 1);
    std::size_t signature_size = 0;
    assert(EVP_DigestSign(signer.get(), nullptr, &signature_size,
        reinterpret_cast<const unsigned char*>(digest.data()), digest.size()) == 1);
    std::string signature(signature_size, '\0');
    assert(EVP_DigestSign(signer.get(), reinterpret_cast<unsigned char*>(signature.data()),
        &signature_size, reinterpret_cast<const unsigned char*>(digest.data()), digest.size()) == 1);
    signature.resize(signature_size);

    std::unique_ptr<BIO, decltype(&BIO_free)> bio(BIO_new(BIO_s_mem()), BIO_free);
    assert(bio && PEM_write_bio_PUBKEY(bio.get(), key.get()) == 1);
    char* public_data = nullptr;
    const auto public_size = BIO_get_mem_data(bio.get(), &public_data);
    const std::string public_key(public_data, static_cast<std::size_t>(public_size));

    SignatureEnvelope envelope{"ed25519", "kms://live/signing", digest,
                               hex(reinterpret_cast<const unsigned char*>(signature.data()), signature.size())};
    std::string error;
    assert(verify_ed25519_signature(envelope, public_key, &error));
    envelope.signed_digest += "-tampered";
    assert(!verify_ed25519_signature(envelope, public_key, &error));
    envelope.algorithm = "hmac-sha256";
    assert(!verify_ed25519_signature(envelope, public_key, &error));
    return 0;
}
