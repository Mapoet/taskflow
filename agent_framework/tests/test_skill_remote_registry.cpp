#include "agent/skill_remote_registry.hpp"

#include <openssl/evp.h>
#include <openssl/pem.h>

#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>

using namespace agent_framework;
namespace fs = std::filesystem;

namespace {
std::string bio_string(BIO* bio) {
    char* data = nullptr;
    const auto size = BIO_get_mem_data(bio, &data);
    return std::string(data, static_cast<std::size_t>(size));
}
std::pair<std::string, std::string> keypair() {
    auto* context = EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, nullptr);
    EVP_PKEY* key = nullptr;
    assert(context && EVP_PKEY_keygen_init(context) == 1 && EVP_PKEY_keygen(context, &key) == 1);
    EVP_PKEY_CTX_free(context);
    auto* private_bio = BIO_new(BIO_s_mem());
    auto* public_bio = BIO_new(BIO_s_mem());
    assert(PEM_write_bio_PrivateKey(private_bio, key, nullptr, nullptr, 0, nullptr, nullptr) == 1);
    assert(PEM_write_bio_PUBKEY(public_bio, key) == 1);
    auto result = std::make_pair(bio_string(private_bio), bio_string(public_bio));
    BIO_free(private_bio); BIO_free(public_bio); EVP_PKEY_free(key);
    return result;
}
void write(const fs::path& path, const std::string& bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    assert(output.good());
}
}

int main() {
    const std::string index_uri = "https://registry.example/v1/index.json";
    const std::string signature_uri = "https://registry.example/v1/index.json.sig";
    SkillRegistryIndex index;
    index.registry_id = "example";
    index.generation = 3;
    index.artifacts.push_back({"org.example.demo", "1.0.0", std::string(64, 'a'), 100,
        {"https://mirror-1.example/demo.tfskill", "https://mirror-2.example/demo.tfskill"},
        "https://registry.example/v1/demo.tfskill.sig"});
    const auto index_bytes = index.to_json().dump();
    auto digest = skill_sha256_bytes(index_bytes);
    assert(digest);
    const auto [private_key, public_key] = keypair();
    SkillSignatureEnvelope envelope;
    envelope.subject_kind = "registry";
    envelope.subject_digest = *digest;
    envelope.publisher = "registry.example";
    envelope.source_uri = index_uri;
    envelope.sbom_digest = std::string(64, '0');
    envelope.provenance_digest = std::string(64, '0');
    auto signature = sign_skill_subject(envelope, private_key);
    assert(signature.ok);
    SkillTrustStore trust;
    trust.keys.push_back({signature.envelope.key_id, "registry.example", public_key,
                          {SkillTrustRole::Registry, SkillTrustRole::Package},
                          {"https://registry.example/"}, 1, 200});
    auto transport = std::make_shared<SkillMemoryRegistryTransport>();
    transport->responses[index_uri] = index_bytes;
    transport->responses[signature_uri] = signature.envelope.to_json().dump();
    SkillRemoteRegistryClient client(trust, transport);
    auto synced = client.sync(index_uri, signature_uri, 100);
    assert(synced.ok && synced.index_digest == *digest && synced.index.generation == 3);
    std::string error;
    auto resolved = client.resolve(synced.index, "org.example.demo", "1.0.0", &error);
    assert(resolved && resolved->digest == std::string(64, 'a'));
    assert(!client.resolve(synced.index, "org.example.demo", "2.0.0", &error));
    assert(transport->requests.size() == 2);

    transport->responses[index_uri][0] ^= 1;
    assert(!client.sync(index_uri, signature_uri, 100).ok);
    transport->responses[index_uri] = index_bytes;
    assert(!client.sync("http://registry.example/index", signature_uri, 100).ok);
    transport->responses[index_uri] = std::string(
        static_cast<std::size_t>(SkillRemoteRegistryClient::kMaxIndexBytes + 1), 'x');
    assert(!client.sync(index_uri, signature_uri, 100).ok);

    auto insecure = index;
    insecure.artifacts.front().mirrors = {"http://mirror.example/demo.tfskill"};
    const auto insecure_bytes = insecure.to_json().dump();
    envelope.subject_digest = *skill_sha256_bytes(insecure_bytes);
    auto insecure_signature = sign_skill_subject(envelope, private_key);
    transport->responses[index_uri] = insecure_bytes;
    transport->responses[signature_uri] = insecure_signature.envelope.to_json().dump();
    assert(!client.sync(index_uri, signature_uri, 100).ok);

    const std::string package_bytes = "pinned tfskill bytes";
    const auto package_digest = *skill_sha256_bytes(package_bytes);
    SkillSignatureEnvelope package_envelope;
    package_envelope.subject_digest = package_digest;
    package_envelope.publisher = "registry.example";
    package_envelope.source_uri = "https://registry.example/packages/demo";
    package_envelope.sbom_digest = std::string(64, 'b');
    package_envelope.provenance_digest = std::string(64, 'c');
    auto package_signature = sign_skill_subject(package_envelope, private_key);
    assert(package_signature.ok);
    SkillRegistryArtifact pinned{"org.example.demo", "1.0.0", package_digest,
        package_bytes.size(), {"https://mirror-1.example/demo.tfskill",
                               "https://mirror-2.example/demo.tfskill"},
        "https://registry.example/v1/demo.tfskill.sig"};
    transport->responses[pinned.signature_uri] = package_signature.envelope.to_json().dump();
    transport->responses[pinned.mirrors[0]] = std::string(package_bytes.size(), 'x');
    transport->responses[pinned.mirrors[1]] = package_bytes;
    const auto root = fs::temp_directory_path() /
        ("taskflow-registry-import-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    auto fetched = client.fetch_pinned(pinned, root / "demo.tfskill", 100);
    assert(fetched.ok && fetched.selected_uri == pinned.mirrors[1]);
    assert(fs::file_size(root / "demo.tfskill") == package_bytes.size());

    write(root / "demo.tfskill.sig", package_signature.envelope.to_json().dump());
    assert(client.verify_offline(root / "demo.tfskill", root / "demo.tfskill.sig",
                                 package_digest, 100).ok);
    assert(!client.verify_offline(root / "demo.tfskill", root / "demo.tfskill.sig",
                                  std::string(64, '0'), 100).ok);

    transport->responses[pinned.mirrors[0]] = package_bytes;
    const auto concurrent_path = root / "concurrent.tfskill";
    auto first_import = std::async(std::launch::async, [&] {
        return client.fetch_pinned(pinned, concurrent_path, 100);
    });
    auto second_import = std::async(std::launch::async, [&] {
        return client.fetch_pinned(pinned, concurrent_path, 100);
    });
    assert(first_import.get().ok && second_import.get().ok);
    assert(fs::file_size(concurrent_path) == package_bytes.size());

    auto unavailable = pinned;
    unavailable.mirrors = {"https://missing-1.example/demo.tfskill",
                           "https://missing-2.example/demo.tfskill"};
    const auto failed_path = root / "failed.tfskill";
    assert(!client.fetch_pinned(unavailable, failed_path, 100).ok);
    assert(!fs::exists(failed_path));
    for(const auto& item : fs::directory_iterator(root))
        assert(item.path().filename().string().find("failed.tfskill.tmp") == std::string::npos);
    fs::remove_all(root);

    std::cout << "signed remote Registry tests passed\n";
}
