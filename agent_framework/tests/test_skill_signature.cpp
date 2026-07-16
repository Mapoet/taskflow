#include <agent/skills/skill_supply_chain.hpp>

#include <openssl/evp.h>
#include <openssl/pem.h>

#include <cassert>
#include <iostream>
#include <string>

using namespace agent_framework;

namespace {

std::string bio_string(BIO* bio) {
    char* data = nullptr;
    const auto size = BIO_get_mem_data(bio, &data);
    return std::string(data, static_cast<std::size_t>(size));
}

std::pair<std::string, std::string> keypair() {
    auto* context = EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, nullptr);
    assert(context && EVP_PKEY_keygen_init(context) == 1);
    EVP_PKEY* key = nullptr;
    assert(EVP_PKEY_keygen(context, &key) == 1);
    EVP_PKEY_CTX_free(context);
    auto* private_bio = BIO_new(BIO_s_mem());
    auto* public_bio = BIO_new(BIO_s_mem());
    assert(PEM_write_bio_PrivateKey(private_bio, key, nullptr, nullptr, 0, nullptr, nullptr) == 1);
    assert(PEM_write_bio_PUBKEY(public_bio, key) == 1);
    auto private_key = bio_string(private_bio);
    auto public_key = bio_string(public_bio);
    BIO_free(private_bio);
    BIO_free(public_bio);
    EVP_PKEY_free(key);
    return {private_key, public_key};
}

} // namespace

int main() {
    const auto [private_key, public_key] = keypair();
    SkillSignatureEnvelope envelope;
    envelope.subject_digest = std::string(64, 'a');
    envelope.publisher = "org.example";
    envelope.source_uri = "https://registry.example/skills/demo";
    envelope.sbom_digest = std::string(64, 'b');
    envelope.provenance_digest = std::string(64, 'c');
    auto signed_result = sign_skill_subject(envelope, private_key);
    assert(signed_result.ok && signed_result.envelope.signature.size() == 88);
    assert(signed_result.envelope.to_json().dump().find("PRIVATE KEY") == std::string::npos);

    std::string error;
    auto key_id = skill_public_key_id(public_key, &error);
    assert(key_id && *key_id == signed_result.envelope.key_id);
    SkillTrustStore trust;
    trust.keys.push_back({*key_id, "org.example", public_key,
                          {SkillTrustRole::Package}, {"https://registry.example/skills/"},
                          100, 200});
    assert(verify_skill_signature(signed_result.envelope, trust, SkillTrustRole::Package, 150).ok);

    auto tampered = signed_result.envelope;
    tampered.subject_digest[0] = 'd';
    assert(!verify_skill_signature(tampered, trust, SkillTrustRole::Package, 150).ok);
    assert(!verify_skill_signature(signed_result.envelope, trust, SkillTrustRole::Registry, 150).ok);
    assert(!verify_skill_signature(signed_result.envelope, trust, SkillTrustRole::Package, 99).ok);
    assert(!verify_skill_signature(signed_result.envelope, trust, SkillTrustRole::Package, 201).ok);

    auto wrong_source = signed_result.envelope;
    wrong_source.source_uri = "https://evil.example/demo";
    assert(!verify_skill_signature(wrong_source, trust, SkillTrustRole::Package, 150).ok);

    auto revoked = trust;
    revoked.revoked_publishers.insert("org.example");
    assert(!verify_skill_signature(signed_result.envelope, revoked, SkillTrustRole::Package, 150).ok);
    revoked = trust;
    revoked.revoked_keys.insert(*key_id);
    assert(!verify_skill_signature(signed_result.envelope, revoked, SkillTrustRole::Package, 150).ok);
    revoked = trust;
    revoked.revoked_packages.insert(signed_result.envelope.subject_digest);
    assert(!verify_skill_signature(signed_result.envelope, revoked, SkillTrustRole::Package, 150).ok);

    auto [other_private, other_public] = keypair();
    (void)other_private;
    auto wrong_key = trust;
    wrong_key.keys.front().public_key_pem = other_public;
    assert(!verify_skill_signature(signed_result.envelope, wrong_key, SkillTrustRole::Package, 150).ok);

    auto invalid = signed_result.envelope;
    invalid.signature = "not-base64";
    assert(!verify_skill_signature(invalid, trust, SkillTrustRole::Package, 150).ok);

    std::cout << "skill Ed25519 signature tests passed\n";
}
