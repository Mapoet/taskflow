#include <agent/skills/skill_package_gate.hpp>

#include <openssl/evp.h>
#include <openssl/pem.h>

#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace fs = std::filesystem;
using namespace agent_framework;

namespace {

void write(const fs::path& path, const std::string& value) {
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    output << value;
    assert(output.good());
}

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

} // namespace

int main() {
    const auto root = fs::temp_directory_path() /
        ("taskflow-skill-trust-gate-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    const auto package = root / "package";
    write(package / "data.txt", "trusted payload");
    write(package / "SKILL.md", R"(---
api-version: agent.taskflow/v1
kind: Skill
name: trusted-package
version: 1.0.0
description: trust gate fixture
resources:
  references:
    - id: data
      path: data.txt
---
body
)");
    SkillProvenanceOptions provenance{"https://registry.example/skills/trusted", "abc123", "test"};
    const auto archive_path = root / "trusted.tfskill";
    auto archive = build_audited_skill_archive(package, archive_path, provenance);
    assert(archive.ok);

    const auto [private_key, public_key] = keypair();
    SkillSignatureEnvelope envelope;
    envelope.subject_digest = archive.metadata.archive_digest;
    envelope.publisher = "org.example";
    envelope.source_uri = provenance.source_uri;
    envelope.sbom_digest = archive.metadata.sbom_digest;
    envelope.provenance_digest = archive.metadata.provenance_digest;
    auto signed_package = sign_skill_subject(envelope, private_key);
    assert(signed_package.ok);
    SkillTrustStore trust;
    trust.keys.push_back({signed_package.envelope.key_id, "org.example", public_key,
                          {SkillTrustRole::Package}, {"https://registry.example/skills/"}, 1, 200});

    SkillPackageGate::TrustOptions options;
    options.trust = trust;
    options.signature = signed_package.envelope;
    options.remote = true;
    options.now = 100;
    auto accepted = SkillPackageGate{}.inspect_archive(archive_path, options);
    assert(accepted.ok && accepted.package && accepted.package->id == "trusted-package");
    assert(accepted.package->signature_identity == signed_package.envelope.key_id);

    SkillPackageGate::TrustOptions unsigned_options;
    assert(!SkillPackageGate{}.inspect_archive(archive_path, unsigned_options).ok);
    unsigned_options.allow_unsigned_local = true;
    assert(SkillPackageGate{}.inspect_archive(archive_path, unsigned_options).ok);
    unsigned_options.remote = true;
    assert(!SkillPackageGate{}.inspect_archive(archive_path, unsigned_options).ok);

    auto tampered_options = options;
    tampered_options.signature->signature[0] =
        tampered_options.signature->signature[0] == 'A' ? 'B' : 'A';
    assert(!SkillPackageGate{}.inspect_archive(archive_path, tampered_options).ok);

    auto registry = std::make_shared<SkillRegistry>(std::vector<fs::path>{});
    SkillLifecycleManager manager(registry, root / "store");
    SkillInstallOptions install_options;
    install_options.trust = trust;
    install_options.signature = tampered_options.signature;
    install_options.remote = true;
    install_options.verification_time = 100;
    assert(!manager.install(archive_path, install_options).ok);
    std::string error;
    assert(SkillPackageStore(root / "store").catalog(&error).empty());
    install_options.signature = signed_package.envelope;
    auto installed = manager.install(archive_path, install_options);
    assert(installed.ok && installed.package);
    assert(installed.package->signature_identity == signed_package.envelope.key_id);
    assert(installed.package->archive_digest == archive.metadata.archive_digest);
    assert(installed.package->publisher == "org.example");
    assert(!installed.package->legacy_unsigned);
    assert(fs::exists(root / "store" / "sha256" / installed.package->package_digest /
                      "archive.tfskill"));
    auto enabled = manager.enable("trusted-package");
    assert(enabled.ok && enabled.lockfile && enabled.lockfile->packages.size() == 1);
    const auto& locked = enabled.lockfile->packages.front();
    assert(locked.archive_digest == archive.metadata.archive_digest);
    assert(locked.key_id == signed_package.envelope.key_id && !locked.legacy_unsigned);

    auto legacy_json = enabled.lockfile->to_json();
    for(const auto* field : {"archiveDigest", "publisher", "keyId", "signatureDigest",
                             "sbomDigest", "provenanceDigest", "registryDigest", "legacyUnsigned"})
        legacy_json["packages"][0].erase(field);
    auto legacy = SkillLockfile::from_json(legacy_json, &error);
    assert(legacy && legacy->packages.front().legacy_unsigned);

    const auto stored_archive = root / "store" / "sha256" /
        installed.package->package_digest / "archive.tfskill";
    std::fstream corrupt(stored_archive, std::ios::binary | std::ios::in | std::ios::out);
    corrupt.seekp(40);
    corrupt.put('X');
    corrupt.close();
    assert(!SkillPackageStore(root / "store").load(installed.package->package_digest, &error));

    fs::remove_all(root);
    std::cout << "skill package trust gate tests passed\n";
}
