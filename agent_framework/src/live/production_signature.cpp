#include "agent/live/production_signature.hpp"

#include <memory>
#include <optional>

#include <openssl/evp.h>
#include <openssl/pem.h>

namespace agent_framework::live {
namespace {
void fail(std::string* error, std::string message) { if(error) *error = std::move(message); }
std::optional<std::string> hex_decode(std::string_view value) {
    if(value.size() % 2) return std::nullopt;
    auto nibble = [](char c) -> int {
        if(c >= '0' && c <= '9') return c - '0';
        if(c >= 'a' && c <= 'f') return c - 'a' + 10;
        if(c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::string out(value.size() / 2, '\0');
    for(std::size_t i = 0; i < out.size(); ++i) {
        const int high = nibble(value[i * 2]), low = nibble(value[i * 2 + 1]);
        if(high < 0 || low < 0) return std::nullopt;
        out[i] = static_cast<char>((high << 4) | low);
    }
    return out;
}
}

bool verify_ed25519_signature(const SignatureEnvelope& envelope,
                              std::string_view public_key_pem, std::string* error) {
    if(envelope.algorithm != "ed25519" || envelope.key_id.empty() ||
       envelope.signed_digest.empty() || envelope.signature.empty()) {
        fail(error, "ed25519 signature envelope is incomplete");
        return false;
    }
    const auto signature = hex_decode(envelope.signature);
    if(!signature) { fail(error, "ed25519 signature is not hexadecimal"); return false; }
    std::unique_ptr<BIO, decltype(&BIO_free)> bio(
        BIO_new_mem_buf(public_key_pem.data(), static_cast<int>(public_key_pem.size())), BIO_free);
    if(!bio) { fail(error, "unable to allocate public key buffer"); return false; }
    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key(
        PEM_read_bio_PUBKEY(bio.get(), nullptr, nullptr, nullptr), EVP_PKEY_free);
    if(!key || EVP_PKEY_base_id(key.get()) != EVP_PKEY_ED25519) {
        fail(error, "public key is not Ed25519");
        return false;
    }
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    if(!context || EVP_DigestVerifyInit(context.get(), nullptr, nullptr, nullptr, key.get()) != 1) {
        fail(error, "unable to initialize Ed25519 verification");
        return false;
    }
    if(EVP_DigestVerify(context.get(), reinterpret_cast<const unsigned char*>(signature->data()),
                        signature->size(),
                        reinterpret_cast<const unsigned char*>(envelope.signed_digest.data()),
                        envelope.signed_digest.size()) != 1) {
        fail(error, "Ed25519 signature verification failed");
        return false;
    }
    return true;
}

}  // namespace agent_framework::live
