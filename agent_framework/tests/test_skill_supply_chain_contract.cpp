#include <agent/skills/skill_supply_chain.hpp>

#include <cassert>
#include <iostream>

using namespace agent_framework;

int main() {
    const std::string digest(64, 'a');
    SkillPackageMetadata package{"agent.taskflow/skill-package/v1", "org.example.demo",
                                 "1.2.3", digest, digest, digest, 4};
    std::string error;
    auto parsed_package = SkillPackageMetadata::from_json(package.to_json(), &error);
    assert(parsed_package && parsed_package->package_id == package.package_id);

    auto invalid = package.to_json();
    invalid["archiveDigest"] = "not-a-digest";
    error.clear();
    assert(!SkillPackageMetadata::from_json(invalid, &error));
    assert(error.rfind(kSkillSupplyChainInvalid, 0) == 0);

    SkillSignatureEnvelope signature;
    signature.subject_digest = digest;
    signature.key_id = digest;
    signature.publisher = "example-publisher";
    signature.source_uri = "https://registry.example/skills";
    signature.sbom_digest = digest;
    signature.provenance_digest = digest;
    signature.signature = "AA==";
    assert(SkillSignatureEnvelope::from_json(signature.to_json(), &error));

    SkillTrustStore trust;
    trust.keys.push_back({digest, "example-publisher", "PUBLIC KEY",
                          {SkillTrustRole::Package, SkillTrustRole::Registry},
                          {"https://registry.example/"}, 1, 100});
    trust.revoked_packages.insert(std::string(64, 'b'));
    auto parsed_trust = SkillTrustStore::from_json(trust.to_json(), &error);
    assert(parsed_trust && parsed_trust->keys.front().roles.count(SkillTrustRole::Registry));

    SkillRegistryIndex index;
    index.registry_id = "example";
    index.generation = 7;
    index.artifacts.push_back({"org.example.demo", "1.2.3", digest, 42,
                               {"https://mirror.example/demo.tfskill"},
                               "https://mirror.example/demo.tfskill.sig"});
    auto parsed_index = SkillRegistryIndex::from_json(index.to_json(), &error);
    assert(parsed_index && parsed_index->artifacts.front().digest == digest);

    std::cout << "skill supply-chain contract tests passed\n";
}
