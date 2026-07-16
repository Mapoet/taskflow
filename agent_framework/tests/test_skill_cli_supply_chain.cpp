#include <agent/skills/skill_supply_chain.hpp>

#include <nlohmann/json.hpp>
#include <openssl/evp.h>
#include <openssl/pem.h>

#include <cassert>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sys/wait.h>

namespace fs = std::filesystem;
using namespace agent_framework;

namespace {
void write(const fs::path& path, const std::string& value) {
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    output << value;
    assert(output.good());
}
std::string read(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
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
int run(const std::string& arguments, const fs::path& output) {
    const std::string command = std::string(AGENT_SKILLCTL_PATH) + " " + arguments +
                                " > " + output.string();
    const int status = std::system(command.c_str());
    return WIFEXITED(status) ? WEXITSTATUS(status) : 255;
}
}

int main() {
    const auto root = fs::temp_directory_path() /
        ("taskflow-skill-cli-supply-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    const auto registry_root = root / "registry-root";
    const auto package = root / "source";
    fs::create_directories(registry_root);
    write(package / "data.txt", "cli payload");
    write(package / "SKILL.md", R"(---
api-version: agent.taskflow/v1
kind: Skill
name: cli-supply
version: 1.0.0
description: cli supply fixture
resources:
  references:
    - id: data
      path: data.txt
---
body
)");
    const auto archive = root / "cli.tfskill";
    const auto signature = root / "cli.tfskill.sig";
    const auto output = root / "output.json";
    const std::string prefix = "--root " + registry_root.string() + " ";
    assert(run(prefix + "package build " + package.string() + " " + archive.string() +
               " --source https://registry.example/skills/cli --revision abc123", output) == 0);
    auto built = nlohmann::json::parse(read(output));
    assert(built.at("apiVersion") == "agent.taskflow/skillctl-output/v1" && built.at("ok") == true);
    const auto digest = built.at("data").at("metadata").at("archiveDigest").get<std::string>();

    const auto [private_key, public_key] = keypair();
    write(root / "private.pem", private_key);
    auto key_id = skill_public_key_id(public_key);
    assert(key_id);
    SkillTrustStore trust;
    trust.keys.push_back({*key_id, "org.example", public_key, {SkillTrustRole::Package},
                          {"https://registry.example/skills/"}, 1, 200});
    write(root / "trust.json", trust.to_json().dump(2));
    assert(run(prefix + "package sign " + archive.string() + " " + signature.string() +
               " --key " + (root / "private.pem").string() +
               " --publisher org.example --source https://registry.example/skills/cli", output) == 0);
    assert(read(output).find("PRIVATE KEY") == std::string::npos);
    assert(run(prefix + "package verify " + archive.string() + " " + signature.string() +
               " --trust " + (root / "trust.json").string() + " --now 100", output) == 0);
    assert(nlohmann::json::parse(read(output)).at("data").at("verified") == true);
    assert(run(prefix + "package inspect " + archive.string(), output) == 0);

    assert(run(prefix + "--store " + (root / "bad-store").string() + " install " +
               archive.string() + " --remote --allow-unsigned-local", output) != 0);
    assert(run(prefix + "--store " + (root / "store").string() + " install " +
               archive.string() + " --signature " + signature.string() + " --trust " +
               (root / "trust.json").string() + " --remote --now 100 --digest " + digest,
               output) == 0);
    auto installed = nlohmann::json::parse(read(output));
    assert(installed.at("data").at("package").at("archiveDigest") == digest);
    assert(installed.at("data").at("package").at("legacyUnsigned") == false);

    fs::remove_all(root);
    std::cout << "skillctl supply-chain tests passed\n";
}
